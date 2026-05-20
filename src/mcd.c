#include "mcd.h"
#include "res.h"
#include <main/gate_arr.def.h>
#include <main/memmap.h>
#include <main/vdp.h>

extern u16 vdp_regs[18];   /* defined in main.c */

#define GA_REG_MEMMODE_W      ((volatile u16 *) 0xA12002)
#define GA_REG_SUBCTRL_B      ((volatile u8 *)  0xA12001)
#define GA_REG_COMCMD0_W      ((volatile u16 *) 0xA12010)
#define GA_REG_COMCMD1_W      ((volatile u16 *) 0xA12012)
#define GA_REG_COMCMD2_W      ((volatile u16 *) 0xA12014)
#define GA_REG_COMSTAT0_W     ((volatile u16 *) 0xA12020)
#define PRG_RAM_WINDOW        ((volatile u16 *) 0x420000)

static void sub_request_bus(void)     { *GA_REG_SUBCTRL_B = 0x02; while (!((*GA_REG_SUBCTRL_B) & 0x02)) ; }
static void sub_release_and_run(void) { *GA_REG_SUBCTRL_B = 0x01; while (!((*GA_REG_SUBCTRL_B) & 0x01)) ; }

static void copy_words(const char * src, volatile u16 * dst, u32 size_bytes)
{
  const u16 * s = (const u16 *) src;
  u32 n = (size_bytes + 1) >> 1;
  while (n--) *dst++ = *s++;
}

u8 detect_mega_cd(void)
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

// Once at boot: load spx.smd into sub-side $10000 with reset vectors at $0.
void mcd_init(void)
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
void mcd_upload_mod(DataChunk const * mod)
{
  sub_request_bus();
  *GA_REG_MEMMODE_W = (3 << 6);
  copy_words(mod->data, PRG_RAM_WINDOW, mod->size);
  *GA_REG_MEMMODE_W = 0x0000;
  sub_release_and_run();
}

void mcd_play_mod(u32 size)
{
  *GA_REG_COMCMD1_W = (u16) (size >> 16);
  *GA_REG_COMCMD2_W = (u16) (size & 0xFFFF);
  *GA_REG_COMCMD0_W = CMD_PLAY_MOD;
}

void mcd_stop_mod(void)
{
  *GA_REG_COMCMD0_W = CMD_STOP_MOD;
}

/* Need to do the standard ack handshake — sub's command loop blocks
 * after processing a command until main clears COMCMD0. Without this,
 * sub gets stuck after the first SFX and ignores subsequent commands. */
void mcd_play_sfx(u8 idx)
{
  *GA_REG_COMCMD1_W = (u16) idx;
  *GA_REG_COMCMD0_W = CMD_PLAY_SFX;
  mcd_wait_ack(CMD_PLAY_SFX);
}

/* Word RAM 2M ownership control (Mode 1 main view): the MEMMODE lo byte at
 * $A12003 has bit 0 = RET (sub gave WR back to main, 1=main owns) and bit
 * 1 = DMNA (main asks for WR, write 1 to request). */
#define GA_MEMMODE_LO ((volatile u8 *) 0xA12003)

static u8 wait_2m_main_to(u32 t) {
  while (!(*GA_MEMMODE_LO & 0x01) && t) t--;
  return t ? 1 : 0;
}
static u8 grant_2m_main_to(u32 t) {
  do {
    *GA_MEMMODE_LO |= 0x02;
    if (*GA_MEMMODE_LO & 0x02) return 1;
  } while (t--);
  return 0;
}

/* WR-main layout matches sub's view:
 *   $600000  stamp data (stamp01..04 written starting at +$200)
 *   $604000  byte-pair-swap scratch (8 KB, repacked IMG buffer goes here)
 *   $610000  stamp map (256 entries × 2B for 16x16 cells, REPEAT)
 *   $620000  trace table (sub fills this)
 *   $630000  IMG buffer (engine output, 8 KB for 128x128 4bpp)
 */
#define WR_MAIN          ((volatile u8 *) 0x600000)
#define WR_STAMP_DATA    (WR_MAIN + 0x00200)   /* skip the entry-0 stamp slot */
#define WR_REPACK_SCR    (WR_MAIN + 0x04000)
#define WR_STAMP_MAP     (WR_MAIN + 0x10000)
#define WR_IMG_BUF       (WR_MAIN + 0x30000)

void mcd_asic_load_stamps(void)
{
  wait_2m_main_to(0x80000);
  /* Stamp slot 0 is unused (engine treats as transparent). Slots 1..4. */
  copy_words(res_stamp01.data, (volatile u16 *) (WR_MAIN + 0x00200), res_stamp01.size);
  copy_words(res_stamp02.data, (volatile u16 *) (WR_MAIN + 0x00400), res_stamp02.size);
  copy_words(res_stamp03.data, (volatile u16 *) (WR_MAIN + 0x00600), res_stamp03.size);
  copy_words(res_stamp04.data, (volatile u16 *) (WR_MAIN + 0x00800), res_stamp04.size);
  copy_words(res_stamp_map.data, (volatile u16 *) WR_STAMP_MAP, res_stamp_map.size);
}

/* Build a Tempest-styled 32x32 "web cell" stamp directly in WR.
 * Layout: black inside, blue gradient band along outer edges (palette
 * 5..8 darkest→brightest), yellow (4) corner/centre marks. Designed so
 * tiling produces lane-like patterns matching the game's web palette.
 *
 * Stored in stamp slot 1 (WR + $200) in the NON-STANDARD byte-pair
 * layout [b2,b3,b0,b1] per row, because the ASIC engine reads stamps in
 * that same format and the IMG output also uses that format. Storing
 * the source in matched format means the round-trip stamp→IMG→repack
 * produces the original pixel order. */
static void write_tempest_stamp(volatile u8 * dst32x32_512)
{
  /* Generate per-pixel palette indices for a 32x32 stamp. */
  static u8 px[32][32];
  for (u8 y = 0; y < 32; ++y) {
    for (u8 x = 0; x < 32; ++x) {
      u8 c;
      u8 dx = (x < 16) ? x : (31 - x);
      u8 dy = (y < 16) ? y : (31 - y);
      u8 d  = dx < dy ? dx : dy;     /* distance to nearest edge */
      if      (d == 0) c = 4;        /* outermost rim — yellow */
      else if (d == 1) c = 8;        /* brightest blue */
      else if (d == 2) c = 7;        /* medium blue */
      else if (d == 3) c = 6;        /* dark blue */
      else if (d <= 5) c = 5;        /* darkest navy */
      else             c = 0;        /* interior — black */
      if ((x == 15 || x == 16) && (y == 15 || y == 16)) c = 4;
      px[y][x] = c;
    }
  }

  /* Walk 4x4 grid of VDP tiles, col-major (tile 0 = top-left, tile 1 =
   * below it, tile 4 = next column over). Bytes written in STANDARD VDP
   * order [b0,b1,b2,b3] = left-to-right pixel pairs. Sega's stamp format
   * empirically appears to be standard; only the IMG buffer output has
   * the byte-pair swap quirk that we fix on display. */
  for (u8 vc = 0; vc < 4; ++vc) {
    for (u8 vr = 0; vr < 4; ++vr) {
      volatile u8 * tile = dst32x32_512 + (vc * 4 + vr) * 32;
      for (u8 r = 0; r < 8; ++r) {
        u8 y = vr * 8 + r;
        u8 x = vc * 8;
        tile[r*4 + 0] = (u8) ((px[y][x  ] << 4) | px[y][x+1]);
        tile[r*4 + 1] = (u8) ((px[y][x+2] << 4) | px[y][x+3]);
        tile[r*4 + 2] = (u8) ((px[y][x+4] << 4) | px[y][x+5]);
        tile[r*4 + 3] = (u8) ((px[y][x+6] << 4) | px[y][x+7]);
      }
    }
  }
}

void mcd_asic_load_tempest_test_stamp(void)
{
  wait_2m_main_to(0x80000);

  /* Build a Tempest-styled stamp programmatically into main RAM, then
   * copy_words into WR slot 1 ($200). Stamp-map entry value = stamp
   * data offset / 0x80 — for 32x32 stamps this means the stamp at
   * offset N*0x200 needs map entries of value N*4. Stamp at $200 → 4. */
  static u16 stamp_buf[256];

  /* Generate per-pixel palette indices for a 32x32 stamp. */
  static u8 px[32][32];
  for (u8 y = 0; y < 32; ++y) {
    for (u8 x = 0; x < 32; ++x) {
      u8 c;
      u8 dx = (x < 16) ? x : (31 - x);
      u8 dy = (y < 16) ? y : (31 - y);
      u8 d  = dx < dy ? dx : dy;     /* distance to nearest edge */
      if      (d == 0) c = 4;        /* outermost rim — yellow */
      else if (d == 1) c = 8;        /* brightest blue */
      else if (d == 2) c = 7;        /* medium blue */
      else if (d == 3) c = 6;        /* dark blue */
      else if (d <= 5) c = 5;        /* darkest navy */
      else             c = 0;        /* interior — black */
      if ((x == 15 || x == 16) && (y == 15 || y == 16)) c = 4;
      px[y][x] = c;
    }
  }

  /* Pack into 16 VDP tiles col-major. Standard byte order [b0,b1,b2,b3]
   * (left-to-right pixel pairs). */
  for (u8 vc = 0; vc < 4; ++vc) {
    for (u8 vr = 0; vr < 4; ++vr) {
      u16 * tile_w = stamp_buf + (vc * 4 + vr) * 16;
      for (u8 r = 0; r < 8; ++r) {
        u8 y = vr * 8 + r;
        u8 x = vc * 8;
        tile_w[r*2 + 0] = (u16) (((px[y][x  ] << 12) | (px[y][x+1] << 8)
                                | (px[y][x+2] <<  4) |  px[y][x+3]));
        tile_w[r*2 + 1] = (u16) (((px[y][x+4] << 12) | (px[y][x+5] << 8)
                                | (px[y][x+6] <<  4) |  px[y][x+7]));
      }
    }
  }

  copy_words((const char *) stamp_buf, (volatile u16 *) (WR_MAIN + 0x00200), 512);

  /* Stamp map: all entries reference stamp at offset $200 (index 4). */
  static u16 map_buf[256];
  for (u16 i = 0; i < 256; ++i) map_buf[i] = 0x0004;
  copy_words((const char *) map_buf, (volatile u16 *) WR_STAMP_MAP, 512);
}

/* Repack the 128x128 IMG buffer (256 tiles, 8 KB) from non-standard
 * [b2,b3,b0,b1] byte-pair layout to standard VDP tile format. Source:
 * IMG buffer in WR. Dest: scratch in WR. Both stay in Word RAM since
 * the VDP-DMA-from-WR path is proven. */
static void repack_img_buf(void)
{
  u8 const * src = (u8 const *) WR_IMG_BUF;
  volatile u8 * dst = (volatile u8 *) WR_REPACK_SCR;
  for (u16 t = 0; t < 256; ++t) {
    u8 const * st = src + t * 32;
    volatile u8 * dt = dst + t * 32;
    for (u8 r = 0; r < 8; ++r) {
      dt[r*4 + 0] = st[r*4 + 2];
      dt[r*4 + 1] = st[r*4 + 3];
      dt[r*4 + 2] = st[r*4 + 0];
      dt[r*4 + 3] = st[r*4 + 1];
    }
  }
}

/* Full ASIC render pipeline: fire sub → repack → DMA to VRAM →
 * paint plane B col-major. tile_base is the VRAM tile index where
 * the 256-tile (128x128 px / 8x8) IMG content gets DMA'd. plane_x/y
 * is the upper-left plane-B cell position to paint (16x16 cells).
 * warp=0 fires identity; warp=1 fires Tempest-style perspective. */
void mcd_render_asic(u16 tile_base, u8 plane_x, u8 plane_y, u8 warp)
{
  u16 cmd = warp ? CMD_RENDER_WARP : CMD_RENDER_ROT;

  /* Hand WR to sub. */
  grant_2m_main_to(0x80000);
  *GA_REG_COMCMD0_W = cmd;
  while (*GA_REG_COMSTAT0_W != cmd) ;
  *GA_REG_COMCMD0_W = 0;
  while (*GA_REG_COMSTAT0_W != 0) ;
  wait_2m_main_to(0x80000);

  /* Repack IMG buffer in place inside WR. */
  repack_img_buf();

  /* DMA repacked 8 KB to VRAM tile_base+ (8192/2 = 4096 words). */
  u16 mode2_reg = vdp_regs[1];
  vdp_ctrl = mode2_reg | VDP_DMA_ENABLE;
  vdp_dma_transfer((char const *) WR_REPACK_SCR,
                   to_vdp_addr(tile_base * 32) | VRAM_W, 4096);
  vdp_ctrl = mode2_reg;

  /* Paint plane B col-major: tile = col * 16 + row. 16x16 cell area. */
  for (u8 row = 0; row < 16; ++row) {
    vdp_ctrl_32 = to_vdp_addr(0x4000 + ((plane_y + row) * 64 + plane_x) * 2) | VRAM_W;
    for (u8 col = 0; col < 16; ++col) {
      vdp_data = (u16) (tile_base + col * 16 + row);
    }
  }
}

void mcd_wait_ack(u16 expected)
{
  while (*GA_REG_COMSTAT0_W != expected) ;
  *GA_REG_COMCMD0_W = 0;
  while (*GA_REG_COMSTAT0_W != 0) ;
}
