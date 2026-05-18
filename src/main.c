// Tempest 2000 — Mega CD Mode 1 cart.
// MC-T2: + in-game MOD music via Sub CPU + RF5C164. On boot the spx.smd
// sub module is uploaded once into PRG-RAM (only if a Mega CD is
// attached). PLAYFIELD entry uploads & starts a MOD; B-out stops it.

#include "res.h"
#include "../sub/src/shared.h"
#include <main/gate_arr.def.h>
#include <main/io.h>
#include <main/memmap.h>
#include <main/vdp.h>
#include <memory.h>
#include <system.h>

#define PLANE_A_ADDR    0x2000
#define PLANE_B_ADDR    0x4000
#define SPRITE_TBL_ADDR 0xb800

#define plane_xy(x, y) (to_vdp_addr(VDP_PLANE_POS(x, y, Width64) + PLANE_A_ADDR) | VRAM_W)

// ---- joypad ---------------------------------------------------------------

volatile u8 p1_hold, p1_single, p1_prev;

static void read_inputs(void)
{
  p1_hold = ~read_input_joypad(io_data1);
  p1_single = (p1_hold != p1_prev) ? (p1_hold & ~p1_prev) : 0;
  p1_prev = p1_hold;
}

// ---- VDP ------------------------------------------------------------------

static u16 vdp_regs[18];
static u16 cram[64];

static void update_cram(void)
{
  vdp_ctrl_32 = to_vdp_addr(0) | CRAM_W;
  u16 * p = cram;
  for (u8 i = 0; i < 64; ++i) vdp_data = *p++;
}
static void clear_vram(void)
{
  vdp_ctrl_32 = to_vdp_addr(0) | VRAM_W;
  for (u16 i = 0; i < (0x10000 / 4); ++i) vdp_data_32 = 0;
}
static void clear_vsram(void)
{
  vdp_ctrl_32 = to_vdp_addr(0) | VSRAM_W;
  for (u16 i = 0; i < 40; ++i) vdp_data = 0;
}

volatile bool vblank_done;
__attribute__((interrupt)) void INT2_EXT(void)    { return; }
__attribute__((interrupt)) void INT4_HBLANK(void) { return; }
__attribute__((interrupt)) void INT6_VBLANK(void)
{
  read_inputs();
  vblank_done = true;
}

#if VIDEO == PAL
#define VIDEO_SIGNAL VDP_PAL_VIDEO
#else
#define VIDEO_SIGNAL 0
#endif

static u16 const default_vdp_regs[] = {
  VDP_REG_MODE1 | VDP_HICOLOR_ENABLE,
  VDP_REG_MODE2 | VDP_MD_DISPLAY_MODE | VDP_VBLANK_ENABLE | VIDEO_SIGNAL | VDP_DISPLAY_ENABLE,
  VDP_REG_PLA_ADDR | (PLANE_A_ADDR / 0x400),
  VDP_REG_WIN_ADDR | (0xa00 / 0x400),
  VDP_REG_PLB_ADDR | (PLANE_B_ADDR / 0x2000),
  VDP_REG_SPR_ADDR | (SPRITE_TBL_ADDR / 0x200),
  VDP_REG_SPR_ADDR2,
  VDP_REG_BGCOLOR,
  VDP_REG_HBLANK_COUNT,
  VDP_REG_MODE3 | VDP_EXTINT_ENABLE,
  VDP_REG_MODE4 | VDP_MASK_WIDTH_40CELL,
  VDP_REG_HS_ADDR | (0xbc00 / 0x400),
  VDP_REG_PL_ADDR2,
  VDP_REG_AUTOINC | 2,
  VDP_REG_PL_SIZE | VDP_PL_64x32,
  VDP_REG_WIN_HPOS,
  VDP_REG_WIN_VPOS};

static void print(char const * s, vdp_addr pos)
{
  vdp_ctrl_32 = pos;
  while (*s) vdp_data = *s++;
}

// ---- Mega CD plumbing -----------------------------------------------------

#define GA_REG_MEMMODE_W      ((volatile u16 *) 0xA12002)
#define GA_REG_SUBCTRL_B      ((volatile u8 *)  0xA12001)
#define GA_REG_COMCMD0_W      ((volatile u16 *) 0xA12010)
#define GA_REG_COMCMD1_W      ((volatile u16 *) 0xA12012)
#define GA_REG_COMCMD2_W      ((volatile u16 *) 0xA12014)
#define GA_REG_COMSTAT0_W     ((volatile u16 *) 0xA12020)
#define PRG_RAM_WINDOW        ((volatile u16 *) 0x420000)

static void sub_request_bus(void)     { *GA_REG_SUBCTRL_B = 0x02; while (!((*GA_REG_SUBCTRL_B) & 0x02)) ; }
static void sub_release_and_run(void) { *GA_REG_SUBCTRL_B = 0x01; while (!((*GA_REG_SUBCTRL_B) & 0x01)) ; }

static void copy_words(const u8 * src, volatile u16 * dst, u32 size_bytes)
{
  u16 * s = (u16 *) src;
  u32 n = (size_bytes + 1) >> 1;
  while (n--) *dst++ = *s++;
}

// Once at boot: load spx.smd into sub-side $10000 with reset vectors at $0.
static void mcd_init(void)
{
  *GA_REG_MEMMODE_W = 0xff00;
  *GA_REG_SUBCTRL_B = 0x03;
  *GA_REG_SUBCTRL_B = 0x02;
  *GA_REG_SUBCTRL_B = 0x00;
  sub_request_bus();
  *GA_REG_MEMMODE_W = 0x0000;
  ((volatile u32 *) PRG_RAM_WINDOW)[0] = 0x00080000;  // SP
  ((volatile u32 *) PRG_RAM_WINDOW)[1] = 0x00010000;  // PC = spx main
  copy_words(res_spx.data, (volatile u16 *) 0x430000, res_spx.size);
  sub_release_and_run();
}

// Upload MOD bytes into PRG-RAM bank 3 (sub $60000), then resume sub.
static void mcd_upload_mod(DataChunk const * mod)
{
  sub_request_bus();
  *GA_REG_MEMMODE_W = (3 << 6);
  copy_words(mod->data, PRG_RAM_WINDOW, mod->size);
  *GA_REG_MEMMODE_W = 0x0000;
  sub_release_and_run();
}

static void mcd_play_mod(u32 size)
{
  *GA_REG_COMCMD1_W = (u16) (size >> 16);
  *GA_REG_COMCMD2_W = (u16) (size & 0xFFFF);
  *GA_REG_COMCMD0_W = CMD_PLAY_MOD;
}

static void mcd_stop_mod(void)
{
  *GA_REG_COMCMD0_W = CMD_STOP_MOD;
}

/* Main-side WR ownership: bit 0 RETURN_2M (sub gave it back), bit 1 DMNA. */
#define GA_MEMMODE_LO ((volatile u8 *) 0xA12003)
static inline u8 wait_2m_main_to(u32 t) {
  while (!(*GA_MEMMODE_LO & 0x01) && t) t--;
  return t ? 1 : 0;
}
static inline u8 grant_2m_main_to(u32 t) {
  do {
    *GA_MEMMODE_LO |= 0x02;
    if (*GA_MEMMODE_LO & 0x02) return 1;
  } while (t--);
  return 0;
}

/* fill_color: palette index 0..15 to fill the entire plane-B ASIC region with. */
static void mcd_render_rot(u8 fill_color)
{
  u8 byte = (u8) ((fill_color << 4) | (fill_color & 0x0F));
  *GA_REG_COMCMD1_W = byte;
  grant_2m_main_to(0x80000);
  *GA_REG_COMCMD0_W = CMD_RENDER_ROT;
  while (*GA_REG_COMSTAT0_W != CMD_RENDER_ROT) ;
  *GA_REG_COMCMD0_W = 0;
  while (*GA_REG_COMSTAT0_W != 0) ;
  wait_2m_main_to(0x40000);
}

static void mcd_wait_ack(u16 expected)
{
  while (*GA_REG_COMSTAT0_W != expected) ;
  *GA_REG_COMCMD0_W = 0;
  while (*GA_REG_COMSTAT0_W != 0) ;
}

// ---- Mega CD detection (canonical three-check) ----------------------------

static u8 detect_mega_cd(void)
{
  volatile u8  * hw_ver  = (volatile u8 *)  0xA10001;
  volatile u32 * mcd_sig = (volatile u32 *) 0x400100;
  u8 disk_bit_clear = (((*hw_ver) >> 5) & 1) == 0;
  u8 sega_at_400100 = (*mcd_sig == 0x53454741);
  *GA_REG_COMCMD0_W = 0xCAFE;
  u16 rb = *GA_REG_COMCMD0_W;
  u8 gate_writable = (rb == 0xCAFE);
  *GA_REG_COMCMD0_W = 0;
  return disk_bit_clear || sega_at_400100 || gate_writable;
}

// ---- Engine (doc 16) ------------------------------------------------------
//
// Three function pointers per scene:
//   always_vblank — runs every VBlank no matter what (input, audio tick).
//   gated_vblank  — runs every VBlank unless paused.
//   main_thread   — runs on the main loop after gated_vblank fired this frame.
// Scene transition is just a swap of all three pointers in g_engine.
// Pattern matches Jaguar Tempest's main.s ($808900).

typedef void (*Handler)(void);

typedef struct {
  Handler always_vblank;
  Handler gated_vblank;
  Handler main_thread;
  u8      paused;
  u16     frame;          // u16 to avoid 32-bit libgcc divide/mod
} EngineState;

static EngineState g_engine;

static u8  g_mcd_present;
static u8  g_music_playing;
static u8  g_scene_dirty;     // set by install_*; main loop redraws static text

// ---- Fixed-point ----------------------------------------------------------

typedef s32 fp16;
#define FP_ONE     ((fp16) 0x10000)
#define FP_FROM_INT(i) (((s32)(i)) << 16)
#define FP_INT(fp)     ((s16) ((fp) >> 16))

// ---- Perspective web (md-port stage 15, doc 22) --------------------------
//
// 16 lanes evenly spaced around a circle, centre = (160, 128), radius 80.
// Lane 0 = directly below centre; clockwise. Player walks the rim. Depth
// 0 = centre (vanishing point); depth FP_ONE = rim.

#define NUM_LANES      16
#define WEB_CENTER_X  160
#define WEB_CENTER_Y  128
#define WEB_DOT_STEPS   8     // dots per lane drawn from centre toward rim

static const s16 WEB_RIM_OFFSET[NUM_LANES][2] = {
  {   0,  80 }, {  31,  74 }, {  57,  57 }, {  74,  31 },
  {  80,   0 }, {  74, -31 }, {  57, -57 }, {  31, -74 },
  {   0, -80 }, { -31, -74 }, { -57, -57 }, { -74, -31 },
  { -80,   0 }, { -74,  31 }, { -57,  57 }, { -31,  74 },
};

static s16 g_lane_rim_x[NUM_LANES];
static s16 g_lane_rim_y[NUM_LANES];

static void web_init(void)
{
  for (u8 i = 0; i < NUM_LANES; ++i) {
    g_lane_rim_x[i] = WEB_CENTER_X + WEB_RIM_OFFSET[i][0];
    g_lane_rim_y[i] = WEB_CENTER_Y + WEB_RIM_OFFSET[i][1];
  }
}

static inline s16 web_pixel_x(u8 lane, fp16 depth_fp)
{
  s32 dx = (s32) g_lane_rim_x[lane] - WEB_CENTER_X;
  return (s16) (WEB_CENTER_X + ((dx * depth_fp) >> 16));
}
static inline s16 web_pixel_y(u8 lane, fp16 depth_fp)
{
  s32 dy = (s32) g_lane_rim_y[lane] - WEB_CENTER_Y;
  return (s16) (WEB_CENTER_Y + ((dy * depth_fp) >> 16));
}

// Precomputed (cx, cy) cell of each web dot, so we can repaint a single
// lane's rim cell when the player vacates it (depth index WEB_DOT_STEPS-1
// is the rim).
static u8 g_web_dot_cx[NUM_LANES][WEB_DOT_STEPS];
static u8 g_web_dot_cy[NUM_LANES][WEB_DOT_STEPS];

// ---- Entity pool (doc 10) -------------------------------------------------

#define ENTITY_POOL_SIZE 32

typedef struct Entity Entity;
typedef enum { E_PLAYER = 1, E_SHOT = 2, E_FLIPPER = 3 } EntityType;

struct Entity {
  u8       type;
  u8       alive;
  u8       glyph;          // character used to draw at the entity's cell
  u8       lane;           // 0..NUM_LANES-1 — which radial lane
  fp16     depth_fp;       // 0 = centre (vanishing point), FP_ONE = rim
  fp16     depth_vel_fp;   // per-tick depth_fp delta (negative = inward)
  u8       phase;          // flipper: 0=descending, 1=rim-walking
  u8       step_period;    // flipper: frames between rim-walk hops
  u8       lifetime;       // flipper: countdown to next hop
  u8       _pad;
  s16      prev_cx, prev_cy;  // last drawn cell (for erase), -1 if never drawn
  Entity * prev;
  Entity * next;
};

static Entity   g_entity_pool[ENTITY_POOL_SIZE];
static Entity * g_active_head;
static Entity * g_free_head;
static u16      g_active_count;

static void pool_init(void)
{
  for (u8 i = 0; i < ENTITY_POOL_SIZE; ++i) {
    g_entity_pool[i].alive = 0;
    g_entity_pool[i].prev_cx = -1;
    g_entity_pool[i].prev_cy = -1;
    g_entity_pool[i].prev = 0;
    g_entity_pool[i].next = (i + 1 < ENTITY_POOL_SIZE) ? &g_entity_pool[i + 1] : 0;
  }
  g_free_head    = &g_entity_pool[0];
  g_active_head  = 0;
  g_active_count = 0;
}

static Entity * entity_spawn(void)
{
  if (!g_free_head) return 0;
  Entity * e = g_free_head;
  g_free_head = e->next;

  e->alive = 1;
  e->prev_cx = -1;
  e->prev_cy = -1;
  e->prev = 0;
  e->next = g_active_head;
  if (g_active_head) g_active_head->prev = e;
  g_active_head = e;
  g_active_count++;
  return e;
}

static void entity_kill(Entity * e)
{
  if (!e->alive) return;
  e->alive = 0;
  if (e->prev) e->prev->next = e->next;
  else         g_active_head = e->next;
  if (e->next) e->next->prev = e->prev;
  e->next = g_free_head;
  e->prev = 0;
  g_free_head = e;
  g_active_count--;
}

// ---- Render helpers -------------------------------------------------------

static void plane_putc(u16 cx, u16 cy, char c)
{
  vdp_ctrl_32 = plane_xy(cx, cy);
  vdp_data = (u8) c;
}

static void clear_play_area(void)
{
  for (u8 y = 4; y < 27; ++y) {
    vdp_ctrl_32 = plane_xy(0, y);
    for (u8 x = 0; x < 40; ++x) vdp_data = ' ';
  }
}

// Forward decls — the scenes call each other across file order.
static void install_title(void);
static void title_main_thread(void);
static void play_main_thread(void);

// ---- Scene: TRANSFORM DEMO (MC-T3 tracer) --------------------------------
//
// Runs the Mega CD ASIC stamp/map rotation engine once, DMAs its output to
// VRAM, displays it as a 16x16-tile block on plane B. The visual goal is
// the simplest possible: a 128x128 solid-colour square. If the square
// shows up, every leg of the pipeline (WR handshake, ASIC config, ASIC
// fire/poll, WR → VRAM DMA, plane B tilemap) is working.

#define ROT_TILE_BASE_IDX  0x300            /* tile slot for rendered image */
#define ROT_TILE_VRAM_ADDR (ROT_TILE_BASE_IDX * 32)
#define ROT_WORD_RAM_IMG   ((u8 *) 0x630000)   /* Mode 1 main view of WR + 0x30000 */
#define ROT_DMA_WORDS      (16 * 16 * 32 / 2)  /* 16x16 cells × 32 bytes /2 = 4096 */

static void rot_dma_image_to_vram(void)
{
  /* Explicitly re-set autoinc=2 right before DMA, in case anything
   * else has touched the VDP register. Sequential word DMA writes need
   * a 2-byte stride per word. */
  vdp_ctrl = VDP_REG_AUTOINC | 2;

  /* DMA enable, do the transfer, DMA back to "armed off". */
  u16 const mode2_dma_on  = VDP_REG_MODE2 | VDP_MD_DISPLAY_MODE | VDP_VBLANK_ENABLE
                          | VIDEO_SIGNAL | VDP_DISPLAY_ENABLE | VDP_DMA_ENABLE;
  u16 const mode2_dma_off = mode2_dma_on & ~VDP_DMA_ENABLE;
  vdp_ctrl = mode2_dma_on;
  vdp_dma_transfer(ROT_WORD_RAM_IMG,
                   to_vdp_addr(ROT_TILE_VRAM_ADDR) | VRAM_W,
                   (u16) ROT_DMA_WORDS);
  vdp_ctrl = mode2_dma_off;
}

static void rot_paint_plane_b(void)
{
  /* Diagnostic mode: show 16 columns × 16 rows in plane-B order, tile
   * index = row*16 + col. Whatever shape we see (16x8 / 8x16 / 16x16)
   * tells us how the engine laid out the buffer. */
  for (u8 row = 0; row < 16; ++row) {
    u16 plane_b_addr = 0x4000 + ((6 + row) * 64 + 12) * 2;
    vdp_ctrl_32 = to_vdp_addr(plane_b_addr) | VRAM_W;
    for (u8 col = 0; col < 16; ++col)
      vdp_data = (u16) (ROT_TILE_BASE_IDX + row * 16 + col);
  }
}

static void rot_clear_plane_b(void)
{
  for (u8 row = 0; row < 16; ++row) {
    u16 plane_b_addr = 0x4000 + ((6 + row) * 64 + 12) * 2;
    vdp_ctrl_32 = to_vdp_addr(plane_b_addr) | VRAM_W;
    for (u8 col = 0; col < 16; ++col) vdp_data = 0;
  }
}

// ---- Main-CPU web renderer (MC-T5) ---------------------------------------
//
// Bypasses the flaky sub-WR-DMA pipeline by drawing the 16-lane web directly
// into a main-RAM tile-data buffer and DMAing main RAM → VRAM. Plane B paint
// is unchanged. Main-RAM access is well-understood; DMA from main RAM is
// the standard MD pattern. If this produces clean diagonals, the bug really
// is specific to the WR pipeline we used previously.

#define WEB_IMG_W 128
#define WEB_IMG_H 128
#define WEB_BUF_BYTES (16 * 16 * 32)     // 8192 bytes = 16x16 tiles × 32

static u8 g_web_buf[WEB_BUF_BYTES];

static const s8 WEB_RIM_60[16][2] = {
  {   0,  60 }, {  23,  55 }, {  42,  42 }, {  55,  23 },
  {  60,   0 }, {  55, -23 }, {  42, -42 }, {  23, -55 },
  {   0, -60 }, { -23, -55 }, { -42, -42 }, { -55, -23 },
  { -60,   0 }, { -55,  23 }, { -42,  42 }, { -23,  55 },
};

static void web_setpx(s16 x, s16 y, u8 pal)
{
  if ((u16) x >= WEB_IMG_W || (u16) y >= WEB_IMG_H) return;
  u16 cell_off = (u16) ((y >> 3) * 16 + (x >> 3)) * 32;
  u16 byte_off = (u16) ((y & 7) * 4 + ((x & 7) >> 1));
  u8 * dst = g_web_buf + cell_off + byte_off;
  if (x & 1)
    *dst = (u8) ((*dst & 0xF0) | (pal & 0x0F));
  else
    *dst = (u8) ((*dst & 0x0F) | ((pal & 0x0F) << 4));
}

static void web_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 pal)
{
  s16 dx =  (s16) ((x1 > x0) ? (x1 - x0) : (x0 - x1));
  s16 dy = -(s16) ((y1 > y0) ? (y1 - y0) : (y0 - y1));
  s16 sx = (x0 < x1) ? 1 : -1;
  s16 sy = (y0 < y1) ? 1 : -1;
  s16 err = dx + dy;
  while (1) {
    /* 2x2 brush. */
    web_setpx(x0,     y0,     pal);
    web_setpx((s16)(x0 + 1), y0,     pal);
    web_setpx(x0,     (s16)(y0 + 1), pal);
    web_setpx((s16)(x0 + 1), (s16)(y0 + 1), pal);
    if (x0 == x1 && y0 == y1) break;
    s16 e2 = (s16) (err << 1);
    if (e2 >= dy) { err += dy; x0 = (s16) (x0 + sx); }
    if (e2 <= dx) { err += dx; y0 = (s16) (y0 + sy); }
  }
}

static void web_render_main(u8 pal)
{
  for (u16 i = 0; i < WEB_BUF_BYTES; ++i) g_web_buf[i] = 0;
  for (u8 lane = 0; lane < 16; ++lane) {
    s16 rx = (s16) (64 + WEB_RIM_60[lane][0]);
    s16 ry = (s16) (64 + WEB_RIM_60[lane][1]);
    web_line(64, 64, rx, ry, pal);
  }
}

static void web_dma_main_to_vram(void)
{
  u16 const mode2_dma_on  = VDP_REG_MODE2 | VDP_MD_DISPLAY_MODE | VDP_VBLANK_ENABLE
                          | VIDEO_SIGNAL | VDP_DISPLAY_ENABLE | VDP_DMA_ENABLE;
  u16 const mode2_dma_off = mode2_dma_on & ~VDP_DMA_ENABLE;
  vdp_ctrl = VDP_REG_AUTOINC | 2;
  vdp_ctrl = mode2_dma_on;
  vdp_dma_transfer((char *) g_web_buf,
                   to_vdp_addr(ROT_TILE_VRAM_ADDR) | VRAM_W,
                   (u16) (WEB_BUF_BYTES / 2));
  vdp_ctrl = mode2_dma_off;
}

static void xform_always_vblank(void) { return; }
static void xform_gated_vblank (void) { return; }
static void xform_main_thread  (void);

static void install_xform_demo(void)
{
  g_engine.always_vblank = xform_always_vblank;
  g_engine.gated_vblank  = xform_gated_vblank;
  g_engine.main_thread   = xform_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;

  if (g_mcd_present && g_music_playing) {
    mcd_stop_mod();
    mcd_wait_ack(CMD_STOP_MOD);
    g_music_playing = 0;
  }
}

static void xform_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    print("MC-T3 ASIC TRACER",  plane_xy(11, 3));
    print("B = TITLE",          plane_xy(2, 27));
    g_scene_dirty = 0;
    if (!g_mcd_present) {
      print("NO MEGA CD DETECTED — DEMO REQUIRES IT", plane_xy(2, 14));
    }
  }

  if (g_mcd_present) {
    /* XFORM scene now cycles colour through palette indices 1..15 (then
     * back) — a quick demo that per-frame sub-rendered repaints are real
     * and run at 60 fps. */
    static u16 anim = 0;
    anim++;
    u8 phase = (u8) (anim >> 3);              /* slow it down a bit */
    u8 c = phase & 0x0F;
    if (c == 0) c = 1;
    mcd_render_rot(c);
    rot_dma_image_to_vram();
    rot_paint_plane_b();
  }

  if (p1_single & PAD_B) {
    rot_clear_plane_b();
    install_title();
  }
}

// ---- Scene: TITLE ---------------------------------------------------------

static void title_always_vblank(void) { return; }
static void title_gated_vblank (void) { return; }
static void title_main_thread  (void);

static void install_title(void)
{
  g_engine.always_vblank = title_always_vblank;
  g_engine.gated_vblank  = title_gated_vblank;
  g_engine.main_thread   = title_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;

  if (g_mcd_present && g_music_playing) {
    mcd_stop_mod();
    mcd_wait_ack(CMD_STOP_MOD);
    g_music_playing = 0;
  }
  rot_clear_plane_b();         // wipe gameplay/xform ASIC bg before TITLE
}

// ---- Scene: PLAYFIELD (web + player) -------------------------------------

static Entity * g_player;
static u8  g_player_lane;
static u8  g_left_tick;          // hold-to-repeat cooldowns
static u8  g_right_tick;
static u8  g_fire_cooldown;      // frames until next shot allowed
static u16 g_spawn_timer;        // frames until next flipper spawn
static u8  g_flipper_count;
static u16 g_score;
static u32 g_rng = 0xCAFEF00Du;
#define LANE_HOLD_INITIAL 14     // frames before first repeat after first press
#define LANE_HOLD_REPEAT   4     // frames between subsequent repeats
#define FIRE_COOLDOWN      6     // frames between shots
#define SHOT_INWARD_STEP   (FP_ONE >> 5)   // 32 ticks rim->centre
#define FLIPPER_OUT_STEP   (FP_ONE >> 7)   // 128 ticks centre->rim (~2 sec)
#define FLIPPER_RIM_HOP    12              // frames between rim-walk hops
#define FLIPPER_SPAWN_PERIOD 90            // ~1.5 sec at 60 Hz
#define FLIPPER_MAX_ACTIVE   4
#define HIT_DEPTH_TOL      (FP_ONE >> 4)   // collision threshold

static u16 lcg(void)
{
  g_rng = g_rng * 1103515245u + 12345u;
  return (u16) (g_rng >> 16);
}

static void spawn_shot(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_SHOT;
  e->glyph        = 'o';
  e->lane         = lane;
  e->depth_fp     = FP_ONE;
  e->depth_vel_fp = -SHOT_INWARD_STEP;
  e->phase        = 0;
}

static void spawn_flipper(u8 lane)
{
  Entity * e = entity_spawn();
  if (!e) return;
  e->type         = E_FLIPPER;
  e->glyph        = 'V';
  e->lane         = lane;
  e->depth_fp     = 0;
  e->depth_vel_fp = +FLIPPER_OUT_STEP;
  e->phase        = 0;
  e->step_period  = FLIPPER_RIM_HOP;
  e->lifetime     = FLIPPER_RIM_HOP;
  g_flipper_count++;
}

static void kill_flipper(Entity * e)
{
  g_flipper_count--;
  entity_kill(e);
}

static void draw_web_once(void)
{
  for (u8 lane = 0; lane < NUM_LANES; ++lane) {
    for (u8 d = 1; d <= WEB_DOT_STEPS; ++d) {
      // depth_fp = (d * FP_ONE) / 8 — shift since 8 is power-of-2 so
      // we don't pull __udivsi3 into the link.
      fp16 depth_fp = ((fp16) d) << (16 - 3);
      s16 px = web_pixel_x(lane, depth_fp);
      s16 py = web_pixel_y(lane, depth_fp);
      u8 cx = (u8) (px >> 3);
      u8 cy = (u8) (py >> 3);
      g_web_dot_cx[lane][d - 1] = cx;
      g_web_dot_cy[lane][d - 1] = cy;
      plane_putc(cx, cy, '.');
    }
  }
}

static void play_always_vblank(void) { return; }
static void play_gated_vblank (void);
static void play_main_thread  (void);

static void install_playfield(void)
{
  g_engine.always_vblank = play_always_vblank;
  g_engine.gated_vblank  = play_gated_vblank;
  g_engine.main_thread   = play_main_thread;
  g_engine.paused        = 0;
  g_scene_dirty          = 1;

  pool_init();
  web_init();
  g_player_lane = 0;
  g_left_tick = g_right_tick = 0;
  g_fire_cooldown = 0;
  g_spawn_timer  = FLIPPER_SPAWN_PERIOD;
  g_flipper_count = 0;
  g_score = 0;

  // Kick off music for the round (Mega CD only — gracefully silent on plain MD).
  if (g_mcd_present) {
    if (g_music_playing) {
      mcd_stop_mod();
      mcd_wait_ack(CMD_STOP_MOD);
      g_music_playing = 0;
    }
    mcd_upload_mod(&res_rave4_mod);
    mcd_play_mod(res_rave4_mod.size);
    mcd_wait_ack(CMD_PLAY_MOD);
    g_music_playing = 1;

    // Main-CPU rendered web on plane B — bypasses the WR-DMA pipeline.
    web_render_main(4);              /* yellow web with current palette */
    web_dma_main_to_vram();
    rot_paint_plane_b();
  }

  g_player = entity_spawn();
  if (g_player) {
    g_player->type         = E_PLAYER;
    g_player->glyph        = '^';
    g_player->lane         = g_player_lane;
    g_player->depth_fp     = FP_ONE;
    g_player->depth_vel_fp = 0;
  }
}

static void play_gated_vblank(void)
{
  // L/R lane hop with hold-to-repeat cooldown.
  if (p1_hold & PAD_LEFT) {
    if (g_left_tick == 0) {
      g_player_lane = (u8) ((g_player_lane + NUM_LANES - 1) % NUM_LANES);
      g_left_tick = (p1_single & PAD_LEFT) ? LANE_HOLD_INITIAL : LANE_HOLD_REPEAT;
    } else {
      g_left_tick--;
    }
  } else {
    g_left_tick = 0;
  }
  if (p1_hold & PAD_RIGHT) {
    if (g_right_tick == 0) {
      g_player_lane = (u8) ((g_player_lane + 1) % NUM_LANES);
      g_right_tick = (p1_single & PAD_RIGHT) ? LANE_HOLD_INITIAL : LANE_HOLD_REPEAT;
    } else {
      g_right_tick--;
    }
  } else {
    g_right_tick = 0;
  }

  if (g_player) g_player->lane = g_player_lane;

  // Fire on A (with cooldown).
  if (g_fire_cooldown) g_fire_cooldown--;
  if ((p1_single & PAD_A) && g_fire_cooldown == 0) {
    spawn_shot(g_player_lane);
    g_fire_cooldown = FIRE_COOLDOWN;
  }

  // Spawn a flipper every spawn_period frames, capped.
  if (g_spawn_timer) g_spawn_timer--;
  if (g_spawn_timer == 0 && g_flipper_count < FLIPPER_MAX_ACTIVE) {
    spawn_flipper((u8) (lcg() & 0x0F));        // 0..15
    g_spawn_timer = FLIPPER_SPAWN_PERIOD;
  }

  // Tick shots and flippers.
  Entity * e = g_active_head;
  while (e) {
    Entity * next = e->next;
    if (e->type == E_SHOT) {
      e->depth_fp += e->depth_vel_fp;
      if (e->depth_fp <= 0) entity_kill(e);
    } else if (e->type == E_FLIPPER) {
      if (e->phase == 0) {
        e->depth_fp += e->depth_vel_fp;
        if (e->depth_fp >= FP_ONE) {
          e->depth_fp     = FP_ONE;
          e->depth_vel_fp = 0;
          e->phase        = 1;
          e->lifetime     = e->step_period;
        }
      } else {
        // Rim-walking: hop one lane toward the player every step_period.
        if (e->lifetime) e->lifetime--;
        if (e->lifetime == 0) {
          if      (e->lane < g_player_lane) e->lane++;
          else if (e->lane > g_player_lane) e->lane--;
          e->lifetime = e->step_period;
        }
      }
    }
    e = next;
  }

  // Shot↔flipper collisions: same lane + close depth_fp.
  Entity * s = g_active_head;
  while (s) {
    Entity * s_next = s->next;
    if (s->type == E_SHOT) {
      Entity * f = g_active_head;
      while (f) {
        Entity * f_next = f->next;
        if (f->type == E_FLIPPER && f->lane == s->lane) {
          fp16 d = s->depth_fp - f->depth_fp;
          if (d < 0) d = -d;
          if (d <= HIT_DEPTH_TOL) {
            entity_kill(s);
            kill_flipper(f);
            g_score++;
            break;
          }
        }
        f = f_next;
      }
    }
    s = s_next;
  }

  // Flipper-at-rim on player's lane → respawn player at lane 0.
  if (g_player) {
    Entity * f = g_active_head;
    while (f) {
      Entity * f_next = f->next;
      if (f->type == E_FLIPPER && f->phase == 1 && f->lane == g_player_lane) {
        g_player_lane = 0;
        g_player->lane = 0;
        kill_flipper(f);
        break;
      }
      f = f_next;
    }
  }
}

// ---- Scene bodies ---------------------------------------------------------

static void title_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    print("TEMPEST 2000",       plane_xy(14, 6));
    print("MEGA CD PORT",       plane_xy(14, 8));
    print("MCD:",               plane_xy(2, 12));
    print(g_mcd_present ? "PRESENT" : "ABSENT ", plane_xy(7, 12));
    print("START = PLAY",       plane_xy(13, 16));
    print("    C = ASIC DEMO",  plane_xy(13, 17));
    g_scene_dirty = 0;
  }
  if (p1_single & PAD_START) install_playfield();
  if (p1_single & PAD_C)     install_xform_demo();
}

static void play_main_thread(void)
{
  if (g_scene_dirty) {
    clear_play_area();
    print("L/R=WALK  A=FIRE",   plane_xy(2,  3));
    print("B = TITLE",          plane_xy(2, 27));
    print("LANE:",              plane_xy(28, 3));
    print("SCORE:",             plane_xy(28, 27));
    /* Web now rendered as crisp pixel lines on plane B by the Sub CPU;
     * text-dot web on plane A is retired. */
    g_scene_dirty = 0;
  }

  // Score readout — 4 digits, no 32-bit divide needed.
  {
    u16 s = g_score;
    plane_putc(35, 27, (char) ('0' + (s % 10))); s /= 10;
    plane_putc(34, 27, (char) ('0' + (s % 10))); s /= 10;
    plane_putc(33, 27, (char) ('0' + (s % 10))); s /= 10;
    plane_putc(32, 27, (char) ('0' + (s % 10)));
  }

  // Render every live entity at its (lane, depth_fp) computed pixel pos.
  // On move, clear the previous cell with a space — the web lives on plane
  // B now, so plane A just needs to be transparent under entities.
  for (Entity * e = g_active_head; e; e = e->next) {
    s16 px = web_pixel_x(e->lane, e->depth_fp);
    s16 py = web_pixel_y(e->lane, e->depth_fp);
    s16 cx = px >> 3;
    s16 cy = py >> 3;
    if (e->prev_cx >= 0 && (e->prev_cx != cx || e->prev_cy != cy))
      plane_putc((u16) e->prev_cx, (u16) e->prev_cy, ' ');
    plane_putc((u16) cx, (u16) cy, (char) e->glyph);
    e->prev_cx = cx;
    e->prev_cy = cy;
  }

  // Lane index display, two digits.
  plane_putc(34, 3, (char) ('0' + (g_player_lane / 10)));
  plane_putc(35, 3, (char) ('0' + (g_player_lane % 10)));

  if (p1_single & PAD_B) install_title();
}

void main(void)
{
  disable_interrupts();
  vblank_done = false;
  p1_hold = p1_single = p1_prev = 0;
  g_engine.frame = 0;
  memcpy16((u16 *) default_vdp_regs, vdp_regs, 18);

  u16 mode2_display_off = VDP_REG_MODE2 | VDP_MD_DISPLAY_MODE | VDP_VBLANK_ENABLE | VIDEO_SIGNAL;
  u16 mode2_dma_enable  = mode2_display_off | VDP_DMA_ENABLE;
  for (u8 i = 0; i < 18; ++i)
    vdp_ctrl = (i == 1) ? mode2_display_off : vdp_regs[i];
  clear_vram();
  clear_vsram();

  // Palette — 15 distinct colours for tile-byte mapping diagnostic
  cram[0]  = 0x0000;          // 0 black
  cram[1]  = 0x00E0;          // 1 green
  cram[2]  = 0x000E;          // 2 red
  cram[3]  = 0x0E00;          // 3 blue
  cram[4]  = 0x00EE;          // 4 yellow
  cram[5]  = 0x0E0E;          // 5 magenta
  cram[6]  = 0x0EE0;          // 6 cyan
  cram[7]  = 0x0EEE;          // 7 white
  cram[8]  = 0x0088;          // 8 dim red
  cram[9]  = 0x0808;          // 9 dim magenta
  cram[10] = 0x0880;          // A dim cyan
  cram[11] = 0x0086;          // B orange
  cram[12] = 0x0600;          // C dim blue
  cram[13] = 0x0060;          // D dim green
  cram[14] = 0x0006;          // E dim red 2
  cram[15] = 0x0AAA;          // F gray
  update_cram();

  init_joypads();

  // Upload font into VRAM tile area starting at glyph 0x20 (' ').
  vdp_ctrl = mode2_dma_enable;
  vdp_dma_transfer(res_basic_font.data, to_vdp_addr(tile_offset(0x20)),
                   (u16) (res_basic_font.size / 2));
  vdp_ctrl = mode2_display_off;

  g_mcd_present = detect_mega_cd();
  if (g_mcd_present) mcd_init();
  g_music_playing = 0;

  // Persistent UI chrome at the bottom; scenes paint above it.
  print("FRAME:",               plane_xy(2,  25));
  print("SCENE:",               plane_xy(20, 25));

  install_title();

  vdp_ctrl = vdp_regs[1];        // display on
  enable_interrupts();

  while (1) {
    while (!vblank_done) asm("nop");
    vblank_done = false;
    g_engine.frame++;

    // Doc 16: always_vblank fires every frame; gated_vblank only when
    // not paused; main_thread runs immediately after to complete the
    // frame's work.
    if (g_engine.always_vblank) g_engine.always_vblank();
    if (!g_engine.paused && g_engine.gated_vblank) g_engine.gated_vblank();
    if (g_engine.main_thread)   g_engine.main_thread();

    // Frame counter — confirms the VBlank loop runs at 50/60 Hz.
    u16 f = g_engine.frame;
    char digits[6] = {0};
    digits[4] = '0' + (f % 10); f /= 10;
    digits[3] = '0' + (f % 10); f /= 10;
    digits[2] = '0' + (f % 10); f /= 10;
    digits[1] = '0' + (f % 10); f /= 10;
    digits[0] = '0' + (f % 10);
    print(digits, plane_xy(9, 25));

    // Scene-name indicator — proves the handler swap actually happened.
    char const * scene_name = "?    ";
    if      (g_engine.main_thread == title_main_thread) scene_name = "TITLE";
    else if (g_engine.main_thread == play_main_thread)  scene_name = "PLAY ";
    else if (g_engine.main_thread == xform_main_thread) scene_name = "XFORM";
    print(scene_name, plane_xy(27, 25));
  }
}
