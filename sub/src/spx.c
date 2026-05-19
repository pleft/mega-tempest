// Mode 1 cart Sub CPU module — bridges the Chilly Willy / Ami-PlayMOD
// upstream (module.c + pcm.c + pcm-io.s) to commands from the main CPU.
//
// Memory map on the sub side:
//   $00000-$00007  bootstrap (8-byte SP/PC, written by main mcd_init)
//   $0006C         Level-3 autovector — INT3 / General Timer fires here.
//                  We point it at $5F82 so the JMP opcode that
//                  pcm_start_timer() writes there reaches timer_int.
//   $0006C $00074  Level-3, Level-4 autovectors (configured below)
//   $05F82         pcm_start_timer installs its `JMP timer_int.l` here.
//   $10000-$1DFFF  spx code (MODULE_ROM_ORIGIN, 56 KB)
//   $1E000-$1FFFF  spx BSS/RAM (MODULE_RAM_ORIGIN, 8 KB)
//   $20000-$3FFFF  mod_sample_buf — scratch buffer InitMOD uses while
//                  uploading samples to PCM RAM (128 KB)
//   $40000-$5FFFF  pattern_buf — MOD pattern data (128 KB, mostly unused)
//   $60000-$7FFFF  MOD file bytes (main writes here before CMD_PLAY_MOD)

#include <stdint.h>
#include "shared.h"
#include "memfile.h"
#include "module.h"
#include "pcm.h"
#include "sfx_data.h"

// Inline the sub-side gate-array comm registers we need. We avoid
// megadev's <sub/gate_arr.h> because it pulls in megadev's types.h,
// which redefines int8_t etc. as plain `char` and clashes with gcc's
// <stdint.h> that module.c (upstream) needs.
#define ga_reg_comcmd0      ((volatile uint16_t *) 0xFF8010)
#define ga_reg_comcmd1      ((volatile uint16_t *) 0xFF8012)
#define ga_reg_comcmd2      ((volatile uint16_t *) 0xFF8014)
#define ga_reg_comstat0     ((volatile uint16_t *) 0xFF8020)
#define ga_reg_comstat1     ((volatile uint16_t *) 0xFF8022)
#define ga_reg_comflags_sub ((volatile uint8_t  *) 0xFF800F)

/* WR-handshake byte (low byte of MEMMODE = $FF8003): bit0 RET, bit1 DMNA. */
#define ga_memmode_lo       ((volatile uint8_t  *) 0xFF8003)

/* ASIC stamp/map rotation engine. */
#define ga_reg_stampsize      ((volatile uint16_t *) 0xFF8058)
#define ga_reg_stampmapbase   ((volatile uint16_t *) 0xFF805A)
#define ga_reg_imgbufvsize    ((volatile uint16_t *) 0xFF805C)
#define ga_reg_imgbufstart    ((volatile uint16_t *) 0xFF805E)
#define ga_reg_imgbufoffset   ((volatile uint16_t *) 0xFF8060)
#define ga_reg_imgbufhdotsize ((volatile uint16_t *) 0xFF8062)
#define ga_reg_imgbufvdotsize ((volatile uint16_t *) 0xFF8064)
#define ga_reg_tracevectbase  ((volatile uint16_t *) 0xFF8066)

/* Sub view of Word RAM is at $80000 (2M mode). */
#define WORD_RAM_SUB ((volatile uint8_t *) 0x80000)

/* Match megadev's reference inline-asm verbatim. Doing this in C with
 * volatile bytes ought to be equivalent but mysteriously isn't on the
 * sub side here — the C version returns "ownership transferred" too
 * early and the first WR write stalls the sub. The btst-based asm
 * spins on the actual hardware-latched DMNA bit and only exits when
 * the gate array confirms the transfer is complete. */
static inline void wait_2m_sub(void) {
  asm volatile (
    "1: btst  #1, 0xFF8003 \n\t"
    "   beq.b 1b            \n\t"
    ::: "cc"
  );
}
static inline void grant_2m_sub(void) {
  asm volatile (
    "1: bset  #0, 0xFF8003 \n\t"
    "   btst  #0, 0xFF8003 \n\t"
    "   beq.b 1b            \n\t"
    ::: "cc"
  );
}

// Tiny libc / libgcc fill-ins so we don't have to link those libraries.

// rand() — used by module.c for the random tremolo/vibrato waveform.
// Multiply uses only 16x16 muls to dodge the recursive __mulsi3 call
// (rand is reached from PlayMOD, which is itself called many times).
static uint16_t g_rand_state_lo = 0x5678;
static uint16_t g_rand_state_hi = 0x1234;
int rand(void)
{
  uint16_t lo = g_rand_state_lo * 0x4E6D + 0x3039;
  g_rand_state_hi = g_rand_state_hi * 0x4E6D + g_rand_state_lo * 0x5DEE + (lo >> 16);
  g_rand_state_lo = lo;
  return (int) ((((uint32_t) g_rand_state_hi) << 16) | g_rand_state_lo) >> 1;
}

// memset — used by module.c for clearing Mod_t structs.
void * memset(void * s, int c, unsigned long n)
{
  unsigned char * p = (unsigned char *) s;
  while (n--) *p++ = (unsigned char) c;
  return s;
}

// memcpy — used by module.c and memfile.c.
void * memcpy(void * dst, const void * src, unsigned long n)
{
  unsigned char * d = (unsigned char *) dst;
  const unsigned char * s = (const unsigned char *) src;
  while (n--) *d++ = *s++;
  return dst;
}

// strcpy — used by module.c (just for the "untitled" fallback title).
char * strcpy(char * dst, const char * src)
{
  char * d = dst;
  while ((*d++ = *src++) != 0) ;
  return dst;
}

// __mulsi3 is provided by mulsi3.s — pure-assembly version using three
// native mulu.w (16x16=32) instructions. The C version we tried first
// recursed infinitely because `(unsigned long)al * bl` compiles back to
// a 32-bit multiply, which... calls __mulsi3. Stack-overflows the sub.

// --- Static buffers module.c expects via extern symbols ---

uint8_t * const mod_sample_buf = (uint8_t *) 0x20000;

// ============================================================================
// Per-channel sample streaming — each of the 4 MOD channels gets a 4 KB
// ring in the top 16 KB of PCM RAM. Any sample marked streamed by InitMOD
// is fed from cart ROM into the playing channel's ring at the chip's
// read rate. Up to 4 different streamed samples can play concurrently.
// ============================================================================

#define MOD_DATA_ADDR_VAL 0x60000        // sub-side base of MOD file bytes
#define STREAM_USABLE_SIZE 0xFFF         // 4095 (last byte = $FF terminator)
#define STREAM_NUM_CH 4

// PCM RAM block where each channel's ring starts.
//   ch 0: $C0 → bytes $C000-$CFFF
//   ch 1: $D0 → bytes $D000-$DFFF
//   ch 2: $E0 → bytes $E000-$EFFF
//   ch 3: $F0 → bytes $F000-$FFFF
static const uint8_t g_channel_ring_block[STREAM_NUM_CH] = {0xC0, 0xD0, 0xE0, 0xF0};

// Per-sample flags + cart-ROM source offset. Set by InitMOD. -1 indices
// (e.g. sample 0) treated as non-streamed by default.
uint8_t  g_sample_is_streamed[31];
uint32_t g_sample_src_offset [31];

// Per-channel streaming state.
typedef struct {
  uint8_t  active;         // 1 if channel currently streams a sample
  int8_t   sample_idx;     // sample index this channel is streaming (-1 if none)
  uint32_t src_len;        // bytes in the streamed sample's source data
  uint32_t src_pos;        // next source byte to write
  uint16_t ring_pos;       // next ring offset to write (0..STREAM_USABLE_SIZE-1)
  uint16_t period;         // current chip period (for FD)
  uint32_t byte_accum_q16; // fractional 16.16 accumulator for write rate
  uint8_t  eos;            // 1 if source exhausted (silenced via $FF at ring start)
} stream_ch_t;
static stream_ch_t g_stream_ch[STREAM_NUM_CH];

// Forward decl from pcm.c.
extern void pcm_cpy(uint16_t doff, void * src, uint16_t len, uint16_t conv);
extern void pcm_set_ctrl(uint8_t val);

// Compute the PCM RAM byte offset of a channel's ring base.
static inline uint16_t channel_ring_offset(uint8_t channel)
{
  return ((uint16_t) g_channel_ring_block[channel]) << 8;
}

// Internal — write `count` source bytes from cart MOD memory into the
// channel's ring at `ring_off`, skipping the per-ring $FFF terminator.
static void stream_write_chunk(uint8_t channel, uint32_t src_pos, uint16_t ring_off, uint16_t count)
{
  int8_t s = g_stream_ch[channel].sample_idx;
  uint8_t * src = (uint8_t *) (MOD_DATA_ADDR_VAL + g_sample_src_offset[(uint8_t) s] + src_pos);
  uint16_t ring_base = channel_ring_offset(channel);
  while (count > 0) {
    if (ring_off >= STREAM_USABLE_SIZE) ring_off = 0;
    uint16_t avail = STREAM_USABLE_SIZE - ring_off;
    if (avail > count) avail = count;
    /* conv=0: streamed sample bytes were pre-converted to sign-magnitude
     * by InitMOD, so we just byte-copy into the wave window here. */
    pcm_cpy(ring_base + ring_off, src, avail, 0);
    src      += avail;
    ring_off += avail;
    count    -= avail;
  }
}

void mod_stream_setup(uint8_t channel, uint8_t sample, uint32_t src_len)
{
  stream_ch_t * c = &g_stream_ch[channel];
  c->active         = 1;
  c->sample_idx     = (int8_t) sample;
  c->src_len        = src_len;
  c->src_pos        = 0;
  c->ring_pos       = 0;
  c->byte_accum_q16 = 0;
  c->eos            = 0;

  /* Write the per-ring $FF terminator at the channel-ring's last byte. */
  uint16_t ring_base = channel_ring_offset(channel);
  static const uint8_t stop_marker = 0xFF;
  pcm_cpy(ring_base + STREAM_USABLE_SIZE, (void *) &stop_marker, 1, 0);

  /* Pre-fill just enough to keep the chip ahead of its drain rate until
   * the next 50 Hz tick. Max drain rate at FD=2048 is ~651 bytes/tick,
   * so 1024 bytes gives ~1.5 ticks of headroom. Filling the full 4095-
   * byte ring synchronously here was overrunning the IRQ budget on rows
   * that retrigger multiple streamed channels at once (caused real-HW
   * + BlastEm slowdowns on large MODs like ELYSIUM). */
  #define STREAM_INITIAL_FILL 1024
  uint32_t initial = src_len;
  if (initial > STREAM_INITIAL_FILL) initial = STREAM_INITIAL_FILL;
  stream_write_chunk(channel, 0, 0, (uint16_t) initial);

  c->src_pos  = initial;
  c->ring_pos = (uint16_t) initial;
  if (c->ring_pos >= STREAM_USABLE_SIZE) c->ring_pos = 0;

  /* Diagnostic: dump source offset + first 4 bytes for the LATEST setup.
   * Only useful when one channel streams; with multiple, the values
   * reflect whichever channel most recently called this. */
  *((volatile uint16_t *) 0xFF8028) = (uint16_t) (g_sample_src_offset[sample] >> 16);
  *((volatile uint16_t *) 0xFF802A) = (uint16_t) (g_sample_src_offset[sample] & 0xFFFF);
  {
    uint8_t * src = (uint8_t *) (MOD_DATA_ADDR_VAL + g_sample_src_offset[sample]);
    *((volatile uint16_t *) 0xFF802C) = (uint16_t) ((src[0] << 8) | src[1]);
    *((volatile uint16_t *) 0xFF802E) = (uint16_t) ((src[2] << 8) | src[3]);
  }
}

void mod_stream_channel_stop(uint8_t channel)
{
  g_stream_ch[channel].active     = 0;
  g_stream_ch[channel].sample_idx = -1;
}

void mod_stream_set_period(uint8_t channel, uint32_t period)
{
  g_stream_ch[channel].period = (uint16_t) period;
}

static void stream_tick_channel(uint8_t channel)
{
  stream_ch_t * c = &g_stream_ch[channel];
  if (!c->active) return;
  if (c->period < 4) return;

  /* End of source — write $FF at ring start so chip silences here. */
  if (c->src_pos >= c->src_len) {
    if (!c->eos) {
      uint16_t ring_base = channel_ring_offset(channel);
      static const uint8_t stop_byte = 0xFF;
      pcm_cpy(ring_base + 0, (void *) &stop_byte, 1, 0);
      c->eos = 1;
    }
    return;
  }

  /* FD = 223152 / (period+1). bytes_per_tick (q16) = FD * 20832. */
  uint16_t period_plus_one = (uint16_t) (c->period + 1);
  uint16_t fd16;
  asm volatile (
      "move.l #223152, %%d0\n\t"
      "divu.w %1, %%d0\n\t"
      "move.w %%d0, %0\n\t"
      : "=d"(fd16)
      : "d"(period_plus_one)
      : "d0", "cc"
  );
  uint32_t step_q16 = (uint32_t) fd16 * 20832u;
  c->byte_accum_q16 += step_q16;
  uint16_t bytes = (uint16_t) (c->byte_accum_q16 >> 16);
  c->byte_accum_q16 &= 0xFFFFu;

  if (bytes == 0) return;
  if (bytes > 1024) bytes = 1024;

  uint32_t remaining = c->src_len - c->src_pos;
  if (bytes > remaining) bytes = (uint16_t) remaining;

  stream_write_chunk(channel, c->src_pos, c->ring_pos, bytes);
  c->src_pos  += bytes;
  c->ring_pos += bytes;
  while (c->ring_pos >= STREAM_USABLE_SIZE)
    c->ring_pos -= STREAM_USABLE_SIZE;
}

void mod_stream_tick(void)
{
  for (uint8_t i = 0; i < STREAM_NUM_CH; i++)
    stream_tick_channel(i);
}

// Reset all streaming state. Call before InitMOD for a new MOD.
void mod_stream_reset_all(void)
{
  for (uint8_t i = 0; i < STREAM_NUM_CH; i++) {
    g_stream_ch[i].active     = 0;
    g_stream_ch[i].sample_idx = -1;
    g_stream_ch[i].period     = 856;
  }
  for (uint8_t i = 0; i < 31; i++) {
    g_sample_is_streamed[i] = 0;
    g_sample_src_offset[i]  = 0;
  }
}

// Public accessor for InitMOD to set the ring block in Inst[i].SampleHandle.
uint8_t mod_stream_ring_block_for_channel(uint8_t channel)
{
  return g_channel_ring_block[channel];
}

// Hardcoded buffers. Avoid statics with non-zero initialisers — megadev's
// sub bootstrap doesn't run a C runtime, so .data isn't necessarily copied
// from ROM and .bss isn't zeroed. Using literal constants compiles to
// immediate operands in the instruction stream.
uint32_t * mod_alloc_pattern_buf(uint32_t size_bytes)
{
  if (size_bytes > 0x20000) return 0;
  return (uint32_t *) 0x40000;
}

void mod_free_pattern_buf(uint32_t * buf)
{
  (void) buf;
}

// --- MOD bytes location (filled by main before CMD_PLAY_MOD) ---

#define MOD_DATA_ADDR ((const uint8_t *) 0x60000)

// --- Player state ---

static Mod_t     g_mod;
static MemFile_t g_mf;
static uint8_t   g_mod_loaded = 0;

/* ---- ASIC stamp/map render — MC-T3 tracer bullet ------------------------
 * Lays down a solid-colour 32x32 stamp at stamp index 1, a 16x16 stamp map
 * referencing that stamp everywhere, a trace-vector table for an identity
 * scan (no rotate, no scale), fires the engine, polls until done, then
 * returns Word RAM to the main CPU for DMA to VRAM.
 *
 * Word RAM layout (sub view, 2M mode):
 *   $80000 + 0x00000  stamp data (stamp #1 at byte +512)
 *   $80000 + 0x10000  stamp map  (16x16 entries, each 16-bit)
 *   $80000 + 0x20000  trace vector table (128 entries × 8 bytes)
 *   $80000 + 0x30000  image-buffer output (16x16 cells = 8 KB)
 */
#define ROT_STAMP_DATA_OFF  0x00000
#define ROT_STAMP_MAP_OFF   0x10000
#define ROT_TRACE_TBL_OFF   0x20000
#define ROT_IMG_BUF_OFF     0x30000
#define ROT_IMG_W           128
#define ROT_IMG_H           128
/* The IMGBUFVSIZE register sets the cell-stride of the output buffer.
 * Empirically with VSIZE=(H/8)-1 we get half the cells we expect, so the
 * register's actual meaning is "buffer-wide cells minus one" — we need
 * to set it to match the WIDTH not the height. */
#define ROT_IMG_VSIZE_CELLS 16   /* 128 / 8 */

/* ---- Sub-CPU pixel renderer ---------------------------------------------
 *
 * IMG buffer is 128x128 px = 16x16 cells, laid out in VDP-tile format
 * (8 rows × 4 bytes per row, 4bpp packed left-to-right within each byte).
 * Cells are stored row-major matching how main's plane-B paint walks them.
 *
 * Pixel (px, py) in IMG buffer →
 *   cell (cx, cy)         = (px / 8, py / 8)
 *   cell-local (lx, ly)   = (px & 7, py & 7)
 *   cell offset           = (cy * 16 + cx) * 32
 *   byte within cell      = ly * 4 + (lx >> 1)
 *   nibble                = (lx & 1) ? low : high
 */

#define IMG_W 128
#define IMG_H 128

/* MC-T4b workaround: framebuffer staging in Sub PRG-RAM.
 *
 * Word RAM writes from Sub side have non-obvious coherency / alignment
 * quirks (see feedback_megacd_mode1_wr_write_coherency.md) — RMW writes
 * at byte / word / long granularity all produced stippled diagonals.
 *
 * Build the framebuffer in PRG-RAM (writes there are guaranteed
 * reliable), then bulk-copy to WR via a tight long-write loop right
 * before grant_2m_sub. This sidesteps all the per-pixel WR write
 * issues — the bulk-copy writes longs sequentially with no intervening
 * reads or other writes, which the hardware/emulator should handle
 * cleanly. */
/* Staging buffer at sub PRG-RAM $1000 — well outside mod_sample_buf
 * ($20000-$3FFFF), pattern_buf ($40000-$5FFFF), and MOD file bytes
 * ($60000-$7FFFF). The area $400-$FFFF is otherwise free. */
#define STAGE_BYTES (16 * 16 * 32)             /* 8192 bytes */
#define G_IMG_STAGE ((uint8_t *) 0x1000)

static void img_clear(uint8_t pal_pair)
{
  uint32_t * p = (uint32_t *) G_IMG_STAGE;
  uint32_t w = ((uint32_t) pal_pair << 24) | ((uint32_t) pal_pair << 16)
             | ((uint32_t) pal_pair <<  8) |  (uint32_t) pal_pair;
  for (uint16_t i = 0; i < STAGE_BYTES / 4; ++i) p[i] = w;
}

static inline void img_setpx(int16_t x, int16_t y, uint8_t pal)
{
  if ((uint16_t) x >= IMG_W || (uint16_t) y >= IMG_H) return;
  uint16_t cell_off = (uint16_t) ((y >> 3) * 16 + (x >> 3)) * 32;
  uint16_t byte_off = (uint16_t) ((y & 7) * 4 + ((x & 7) >> 1));
  uint8_t * dst = G_IMG_STAGE + cell_off + byte_off;
  if (x & 1)
    *dst = (uint8_t) ((*dst & 0xF0) | (pal & 0x0F));
  else
    *dst = (uint8_t) ((*dst & 0x0F) | ((pal & 0x0F) << 4));
}

/* Explicit-asm bulk copy from PRG-RAM staging to WR IMG buffer using
 * WORD writes (not long). 8192 bytes = 4096 words. Word writes have
 * proven reliable in our solid-fill tests; long writes appear to lose
 * the second word externally on the Mega CD sub-bus. */
static void img_flush_to_wr(void)
{
  uint8_t * src       = G_IMG_STAGE;
  uint8_t * dst       = (uint8_t *) (WORD_RAM_SUB + ROT_IMG_BUF_OFF);
  asm volatile (
    "movea.l %[s], %%a0          \n\t"
    "movea.l %[d], %%a1          \n\t"
    "move.w  #4095, %%d0         \n\t"
    "1:                          \n\t"
    "move.w  (%%a0)+, (%%a1)+   \n\t"
    "dbra    %%d0, 1b            \n\t"
    :
    : [s] "r"(src), [d] "r"(dst)
    : "d0", "a0", "a1", "cc", "memory"
  );
}

/* Bresenham line drawn with a full 2x2 brush at each step — at 45° the
 * 2x2 blocks tile diagonally with no gaps; at axis-aligned slopes the
 * blocks overlap into a 2-px-thick solid line. */
static void img_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t pal)
{
  int16_t dx =  (int16_t) ((x1 > x0) ? (x1 - x0) : (x0 - x1));
  int16_t dy = -(int16_t) ((y1 > y0) ? (y1 - y0) : (y0 - y1));
  int16_t sx = (x0 < x1) ? 1 : -1;
  int16_t sy = (y0 < y1) ? 1 : -1;
  int16_t err = dx + dy;
  while (1) {
    /* 2x2 brush. */
    img_setpx(x0,     y0,     pal);
    img_setpx(x0 + 1, y0,     pal);
    img_setpx(x0,     y0 + 1, pal);
    img_setpx(x0 + 1, y0 + 1, pal);
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = (int16_t) (err << 1);
    if (e2 >= dy) { err += dy; x0 = (int16_t) (x0 + sx); }
    if (e2 <= dx) { err += dx; y0 = (int16_t) (y0 + sy); }
  }
}

/* 16-lane Tempest web rim offsets, radius 60 px so the web fills most
 * of the 128-px IMG buffer (rim spans (4,4)..(124,124) around the
 * buffer centre (64, 64)). */
static const int8_t WEB_RIM[16][2] = {
  {   0,  60 }, {  23,  55 }, {  42,  42 }, {  55,  23 },
  {  60,   0 }, {  55, -23 }, {  42, -42 }, {  23, -55 },
  {   0, -60 }, { -23, -55 }, { -42, -42 }, { -55, -23 },
  { -60,   0 }, { -55,  23 }, { -42,  42 }, { -23,  55 },
};

static void render_rot(void)
{
  asm volatile("move.w #0x2700, %sr");
  wait_2m_sub();

  uint8_t pal = (uint8_t) (*ga_reg_comcmd1 & 0x0F);
  if (pal == 0) pal = 4;

  img_clear(0x00);

  /* Draw the 16-lane Tempest web into the PRG-RAM staging framebuffer. */
  for (uint8_t lane = 0; lane < 16; ++lane) {
    int16_t rim_x = (int16_t) (64 + WEB_RIM[lane][0]);
    int16_t rim_y = (int16_t) (64 + WEB_RIM[lane][1]);
    img_line(64, 64, rim_x, rim_y, pal);
  }

  /* Bulk-copy the assembled framebuffer to WR via a tight long-write loop. */
  img_flush_to_wr();

  grant_2m_sub();
  asm volatile("move.w #0x2000, %sr");
}

// ---- SFX playback (RF5C164 channel 4; MOD claims 0-3) -------------------
//
// Phase 1: FIRE only. The sample blob is embedded in sfx_data.{c,h} and
// already in RF5C164 sign-magnitude with a trailing $FF end-marker.
// First play of each SFX uploads its blob to a fixed PCM RAM bank (then
// the upload flag stays set for the rest of the session). Subsequent
// plays just retrigger the channel.
//
// PCM RAM offset $B000 (bank 11) — sits high in the static-sample
// region claimed by InitMOD ($0000-$BFFF). If rave4.mod's static samples
// reach that bank we'll hear them clobbered; in practice they don't.

#define SFX_CHANNEL    4
#define SFX_BANK_FIRE  0xB0          /* PCM offset $B000 */

#define PCM_PAN_REG    (*((volatile uint8_t *) 0xFF0003))

/* No more lazy-upload flag — megadev sub doesn't zero .bss, so an
 * uninit static flag was randomly non-zero on boot, skipping the
 * upload and leaving loop pointers / channel state pointing at MOD's
 * voice samples (we heard "Play" looping forever). Just upload every
 * play — 257 bytes via pcm_cpy is microseconds. */

static void sfx_play(uint8_t idx)
{
  if (idx != SFX_IDX_FIRE) return;          /* phase 1: FIRE only */

  /* Upload the real T2K "Player Shot Normal" sample. SFX_FIRE was baked
   * by tools/extract_mcd_sprites.py into sign-magnitude with a trailing
   * $FF byte already in place. */
  uint16_t base = (uint16_t) (SFX_BANK_FIRE << 8);
  pcm_cpy(base, (void *) SFX_FIRE, (uint16_t) sizeof SFX_FIRE, 0);
  uint16_t loop_off = (uint16_t) (base + sizeof SFX_FIRE - 1);

  /* Mask interrupts during channel config so mod_tick (50 Hz timer) can't
   * stomp PCM_CTRL between our register writes and send them to a wrong
   * channel. Without this masking, mid-config interruption would silently
   * misroute everything to whichever channel mod_tick was last servicing. */
  asm volatile("move.w #0x2700, %sr");

  pcm_set_ctrl((uint8_t) (0xC0 | SFX_CHANNEL));           /* chip on + select ch */
  pcm_set_off(SFX_CHANNEL);
  pcm_set_start(SFX_BANK_FIRE, 0);
  /* Loop-start = the $FF terminator byte. After end-of-sample the chip
   * wraps there and immediately re-reads $FF → effectively one-shot. */
  pcm_set_loop(loop_off);
  pcm_set_env(0xFF);                                       /* max volume */
  /* PAN register layout: high nibble = L, low nibble = R. 0x88 = balanced
   * mid (both ~50%). pcm_set_pan goes through pcm_lcf which mangles the
   * value oddly for raw "max both"; writing the register directly. */
  *((volatile uint8_t *) 0xFF0003) = 0xFF;                /* L=F, R=F (centre, loud) */
  pcm_delay();
  pcm_set_period(428);                                     /* Amiga C-2 ≈ 8363 Hz */
  pcm_set_on(SFX_CHANNEL);

  asm volatile("move.w #0x2000, %sr");
}

static void stop_and_release(void)
{
  if (g_mod_loaded) {
    if (g_mod.IsPlaying) StopMOD(&g_mod);
    ExitMOD(&g_mod);
    g_mod_loaded = 0;
  }
}

__attribute__((section(".init"))) void main()
{
  register uint16_t command;

  /* Use FL as a single-step state machine (direct assignment, not OR'd
   * bits, so each transition is visible). Values:
   *   $10 alive in spx main
   *   $20 silenced PCM chip
   *   $30 vector $6C installed
   *   $40 ready, in command loop
   *   $50 received CMD_PLAY_MOD, entering InitMOD
   *   $60 InitMOD failed
   *   $70 InitMOD ok, StartMOD called (playing)                            */
  *ga_reg_comflags_sub = 0x10;

  asm volatile("move.w #0x2700, %sr");

  *((volatile uint8_t *) 0xFF0011) = 0xFF;
  /* Initialise the RF5C164 unconditionally at boot so SFX can play even
   * if MOD music never starts. InitMOD also calls this, which is harmless
   * (both paths just zero the channel regs and turn the chip on). */
  pcm_reset();
  *ga_reg_comflags_sub = 0x20;

  *((volatile uint32_t *) 0x006C) = 0x00005F82;

  /* Install a safe RTE for every unset autovector (INT1, INT2, INT4..7) so
   * that if any of them fires we don't jump into garbage. Mode 1 cart has
   * no BIOS to manage these for us. 0x4E73 = `rte` opcode. */
  *((volatile uint16_t *) 0x000400) = 0x4E73;
  *((volatile uint32_t *) 0x000064) = 0x00000400;       /* INT1 */
  *((volatile uint32_t *) 0x000068) = 0x00000400;       /* INT2 hblank */
  *((volatile uint32_t *) 0x000070) = 0x00000400;       /* INT4 CDC */
  *((volatile uint32_t *) 0x000074) = 0x00000400;       /* INT5 CDD */
  *((volatile uint32_t *) 0x000078) = 0x00000400;       /* INT6 subcode */
  *((volatile uint32_t *) 0x00007C) = 0x00000400;       /* INT7 */
  *ga_reg_comflags_sub = 0x30;

  asm volatile("move.w #0x2000, %sr");
  *ga_reg_comflags_sub = 0x40;

  do {
    do { command = *ga_reg_comcmd0; } while (command == 0);
    if (command != *ga_reg_comcmd0) continue;

    switch (command) {
      case CMD_PLAY_MOD: {
        uint32_t mod_size = ((uint32_t) (*ga_reg_comcmd1) << 16)
                          | (uint32_t) (*ga_reg_comcmd2);

        stop_and_release();
        memfile_init(&g_mf, MOD_DATA_ADDR, mod_size);
        /* Megadev's sub bootstrap doesn't run a C runtime that copies
         * .data from ROM to RAM, so initialised statics inside module.c
         * (specifically `gVolume = 12`) read as 0 → silent output even
         * though the player runs. Call VolumeMOD explicitly to set it. */
        VolumeMOD(12);
        *ga_reg_comflags_sub = 0x50;     /* entering InitMOD */
        /* filter=1 enables the upstream's .25/.5/.25 low-pass during the
         * sample-halving step. Without it (filter=0), halved samples are
         * just decimated → aliased high frequencies → harsh / squeaky.
         * filter=1 trades crispness for cleanliness on halved samples. */
        uint8_t err = InitMOD(&g_mf.base, &g_mod, 1);
        *ga_reg_comstat1 = (uint16_t) err;
        if (err == 0) {
          g_mod_loaded = 1;
          *ga_reg_comflags_sub = 0x70;   /* playing */
          StartMOD(&g_mod, 1);
        } else {
          *ga_reg_comflags_sub = 0x60;   /* InitMOD failed */
        }
        break;
      }

      case CMD_STOP_MOD:
        stop_and_release();
        *ga_reg_comflags_sub &= ~0x80;
        break;

      case CMD_RENDER_ROT:
        render_rot();
        break;

      case CMD_PLAY_SFX:
        sfx_play((uint8_t) (*ga_reg_comcmd1 & 0xFF));
        break;
    }

    /* Standard megadev command ack handshake. */
    *ga_reg_comstat0 = *ga_reg_comcmd0;
    do {
      asm("nop");
      command = *ga_reg_comcmd0;
    } while (command != 0);
    *ga_reg_comstat0 = 0;
  } while (1);
}
