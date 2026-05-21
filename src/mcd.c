#include "mcd.h"
#include "res.h"
#include "web.h"             /* g_web_buf + web_render_main */
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

  /* DEBUG diagnostic stamp: one yellow vertical line at column 4 of
   * EVERY tile. If ASIC's IMG output preserves this geometry, we should
   * see vertical yellow lines at x=4, 12, 20, 28 in the 32x32 stamp.
   * After 16x16 cell map, IMG buffer = 128x128 should show vertical
   * yellow stripes at x = 4, 12, 20, ..., 124 in the IMG output. */
  static u16 stamp_buf[256];
  static u8 px[32][32];
  /* Background = gray (palette 15). */
  for (u8 y = 0; y < 32; ++y)
    for (u8 x = 0; x < 32; ++x)
      px[y][x] = 15;

  /* DIAGNOSTIC: ONE 3-pixel-tall horizontal line at y=4..6, full width yellow.
   * Big enough that any zigzag is unambiguous (not optical illusion of
   * single-pixel features). */
  for (u8 x = 0; x < 32; ++x) {
    px[4][x] = 4;   /* yellow */
    px[5][x] = 4;
    px[6][x] = 4;
  }
  /* Same at y=12..14 (white), y=20..22 (red), y=28..30 (blue). */
  for (u8 x = 0; x < 32; ++x) {
    px[12][x] = 1; px[13][x] = 1; px[14][x] = 1;
    px[20][x] = 2; px[21][x] = 2; px[22][x] = 2;
    px[28][x] = 7; px[29][x] = 7; px[30][x] = 7;
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

/* Pre-render the current web shape into 16 ASIC stamps + 4x4 stamp map.
 *
 * Reuses the software rasterizer (web_render_main writes to g_web_buf,
 * 26x26 cells = 208x208 px). We extract the centre 16x16 cells (cells 5..20)
 * and pack them as 16 stamps of 4x4 tiles each, col-major within each stamp.
 *
 * Stamp layout in WR:
 *   slot 1 at $200 = grid (0,0), slot 2 at $400 = grid (0,1), ... 16 at $2000.
 *   Stamp index in map = data_offset / $80 → values 4, 8, 12, ..., 64.
 *
 * Stamp map: cells (col 0..3, row 0..3) reference stamps 1..16 in col-major
 * order; all other cells = 0 (transparent; with REPEAT they wrap but at
 * identity sampling the IMG buffer never sees beyond cell 3). */
/* Helper: read 4bpp pixel at (x, y) from g_web_buf (208x208, 26x26 tiles). */
static u8 web_buf_getpx(u8 const * buf, s16 x, s16 y)
{
  if ((u16) x >= 208 || (u16) y >= 208) return 0;
  u16 cell_off = (u16) ((y >> 3) * WEB_BUF_CELLS + (x >> 3)) * 32;
  u16 byte_off = (u16) ((y & 7) * 4 + ((x & 7) >> 1));
  u8 b = buf[cell_off + byte_off];
  return (x & 1) ? (u8)(b & 0x0F) : (u8)(b >> 4);
}

static void web_buf_setpx(u8 * buf, s16 x, s16 y, u8 pal)
{
  if ((u16) x >= 208 || (u16) y >= 208) return;
  u16 cell_off = (u16) ((y >> 3) * WEB_BUF_CELLS + (x >> 3)) * 32;
  u16 byte_off = (u16) ((y & 7) * 4 + ((x & 7) >> 1));
  u8 * dst = buf + cell_off + byte_off;
  if (x & 1) *dst = (u8) ((*dst & 0xF0) | (pal & 0x0F));
  else       *dst = (u8) ((*dst & 0x0F) | ((pal & 0x0F) << 4));
}

/* Thicken every line-colored pixel by setting its right + below neighbor
 * to the same palette index. Combats the ASIC pipeline's per-column
 * tile-row jitter so 1-pixel Bresenham lines survive intact. */
static void web_buf_dilate(u8 * buf, u8 line_pal)
{
  /* Horizontal dilation: scan right-to-left so we don't double-process. */
  for (s16 y = 0; y < 208; ++y) {
    for (s16 x = 207; x >= 1; --x) {
      if (web_buf_getpx(buf, x - 1, y) == line_pal &&
          web_buf_getpx(buf, x, y) != line_pal)
        web_buf_setpx(buf, x, y, line_pal);
    }
  }
  /* Vertical dilation: scan bottom-up. */
  for (s16 y = 207; y >= 1; --y) {
    for (s16 x = 0; x < 208; ++x) {
      if (web_buf_getpx(buf, x, y - 1) == line_pal &&
          web_buf_getpx(buf, x, y) != line_pal)
        web_buf_setpx(buf, x, y, line_pal);
    }
  }
}

void mcd_asic_load_web_stamps(u8 line_pal)
{
  /* (1) Render web into main-RAM buffer (does NOT DMA — that step is left
   *     to the regular software-web pipeline if both layers want to coexist). */
  web_render_main(line_pal);
  /* No workarounds; once the ASIC pipeline is fixed properly, dilation
   * won't be necessary. */

  /* (2) Wait for WR ownership, then extract + pack. */
  wait_2m_main_to(0x80000);

  enum { EXTRACT_OFFSET = 5 };           /* center of 26x26, want 16 → start at 5 */
  for (u8 mc = 0; mc < 4; ++mc) {
    for (u8 mr = 0; mr < 4; ++mr) {
      u8 stamp_idx = (u8) (mc * 4 + mr + 1);              /* 1..16 */
      volatile u8 * stamp = (volatile u8 *) (WR_MAIN + stamp_idx * 0x200);
      for (u8 t = 0; t < 16; ++t) {
        u8 in_col = t >> 2;
        u8 in_row = t & 3;
        u8 src_gc = (u8) (EXTRACT_OFFSET + mc * 4 + in_col);
        u8 src_gr = (u8) (EXTRACT_OFFSET + mr * 4 + in_row);
        u16 src_off = (u16) ((u16) src_gr * WEB_BUF_CELLS + src_gc) * 32;
        volatile u8 * dst_tile = stamp + (u16) t * 32;
        u8 const * src_tile = web_get_buf() + src_off;
        for (u8 b = 0; b < 32; ++b) dst_tile[b] = src_tile[b];
      }
    }
  }

  /* (3) Stamp map: 4x4 top-left cells reference stamps, rest = 0. */
  static u16 map_buf[256];
  for (u16 i = 0; i < 256; ++i) map_buf[i] = 0;
  for (u8 mc = 0; mc < 4; ++mc) {
    for (u8 mr = 0; mr < 4; ++mr) {
      u8 stamp_idx = (u8) (mc * 4 + mr + 1);
      /* Map index = data_offset / $80 = (stamp_idx * $200) / $80 = stamp_idx*4 */
      map_buf[mr * 16 + mc] = (u16) (stamp_idx * 4);
    }
  }
  copy_words((const char *) map_buf, (volatile u16 *) WR_STAMP_MAP, 512);
}

/* Repack IMG buffer from ASIC's native format to standard VDP tiles.
 *
 * ASIC IMG buffer layout (empirically derived from diagnostic):
 *   32 vertical stripes, 4 pixels wide × 128 tall, 256 bytes per stripe.
 *   ODD-numbered stripes are shifted DOWN by 1 pixel Y compared to
 *   even-numbered stripes (signature alternating zigzag).
 *
 * To get correct VDP tile at display (col, row):
 *   Left 4 pixels from stripe (2*col) at source Y = row*8..row*8+7
 *   Right 4 pixels from stripe (2*col+1) at source Y = row*8+1..row*8+8
 *     (shifted UP by 1 to compensate for the odd-stripe DOWN shift)
 *
 * Bytes per source row in a 4-wide stripe: 2 bytes (= 4 pixels at 4bpp).
 * Stripe is stored as 16 cells of 8 rows each (16 bytes per cell). */
static void repack_img_buf(void)
{
  u8 const * src = (u8 const *) WR_IMG_BUF;
  volatile u8 * dst = (volatile u8 *) WR_REPACK_SCR;
  for (u8 dc = 0; dc < 16; ++dc) {
    u8 const * sA_base = src + (u16) (dc * 2)     * 256;   /* even stripe */
    u8 const * sB_base = src + (u16) (dc * 2 + 1) * 256;   /* odd stripe (shifted) */
    for (u8 dr = 0; dr < 16; ++dr) {
      volatile u8 * dt = dst + (u16) (dc * 16 + dr) * 32;
      for (u8 r = 0; r < 8; ++r) {
        u16 yA = (u16) (dr * 8 + r);
        u16 yB = (u16) (dr * 8 + r + 1);
        if (yB > 127) yB = 127;
        u16 offA = (u16) ((yA >> 3) * 16 + (yA & 7) * 2);
        u16 offB = (u16) ((yB >> 3) * 16 + (yB & 7) * 2);
        dt[r*4 + 0] = sA_base[offA + 0];
        dt[r*4 + 1] = sA_base[offA + 1];
        dt[r*4 + 2] = sB_base[offB + 0];
        dt[r*4 + 3] = sB_base[offB + 1];
      }
    }
  }
}

/* Full ASIC render pipeline. plane_vram_addr selects which plane gets
 * painted (0x2000 = plane A, 0x4000 = plane B). */
void mcd_render_asic(u16 plane_vram_addr, u16 tile_base, u8 plane_x, u8 plane_y, u8 warp)
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

  /* Paint chosen plane col-major: tile = col * 16 + row. 16x16 cell area. */
  for (u8 row = 0; row < 16; ++row) {
    vdp_ctrl_32 = to_vdp_addr(plane_vram_addr + ((plane_y + row) * 64 + plane_x) * 2) | VRAM_W;
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
