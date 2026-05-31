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
#define GA_REG_COMSTAT1_W     ((volatile u16 *) 0xA12022)
#define PRG_RAM_WINDOW        ((volatile u16 *) 0x420000)

static void sub_request_bus(void)     { *GA_REG_SUBCTRL_B = 0x02; while (!((*GA_REG_SUBCTRL_B) & 0x02)) ; }
static void sub_release_and_run(void) { *GA_REG_SUBCTRL_B = 0x01; while (!((*GA_REG_SUBCTRL_B) & 0x01)) ; }

static void copy_words(const char * src, volatile u16 * dst, u32 size_bytes)
{
  const u16 * s = (const u16 *) src;
  u32 n = (size_bytes + 1) >> 1;
  while (n--) *dst++ = *s++;
}

/* Forward declaration — definition lower in the file. */
static u16 mcd_wait_ack_timeout(u16 expected);

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

/* Backup RAM hi-score I/O.
 *
 * Shared buffer is at PRG-RAM bank 0 offset $1000 (sub-side address),
 * accessed by main via the PRG-RAM window at $421000 with bank 0
 * selected. mcd_hiscore_load asks the sub to BRMREAD "MEGATEMPHI"
 * into that slot, then copies it to `buf` for the cart's hi-score
 * module to consume. mcd_hiscore_save does the inverse.
 *
 * Return value of load: 0 = ok, non-zero = file missing / BRAM unformatted.
 * Return value of save: 0 = ok, non-zero = BRAM unformatted or write failed. */
#define BRAM_BUF_SUB_OFFSET 0x1000
#define BRAM_BUF_MAIN_ADDR  ((volatile u16 *) (0x420000 + BRAM_BUF_SUB_OFFSET))

u16 mcd_hiscore_load(u8 * buf, u16 len)
{
  *GA_REG_COMCMD0_W = CMD_BRAM_LOAD;
  if (mcd_wait_ack_timeout(CMD_BRAM_LOAD) != 0) {
    /* Sub didn't ack — BIOS BRMINIT may have hung. Force-clear COMCMD0
     * so the sub can recover next command. */
    *GA_REG_COMCMD0_W = 0;
    return 0xFFFF;
  }
  u16 result = *GA_REG_COMSTAT1_W;
  if (result == 0xFFFF) return 0xFFFF;
  /* result holds the file size in bytes. Bound to caller's buffer. */
  if (len > result) len = result;
  sub_request_bus();
  *GA_REG_MEMMODE_W = (0 << 6);                     /* bank 0 */
  volatile u16 * src = BRAM_BUF_MAIN_ADDR;
  u16 words = (u16) ((len + 1) >> 1);
  u16 * d   = (u16 *) buf;
  for (u16 i = 0; i < words; i++) d[i] = src[i];
  *GA_REG_MEMMODE_W = 0x0000;
  sub_release_and_run();
  return 0;
}

u16 mcd_hiscore_save(const u8 * buf, u16 len)
{
  if (len == 0 || len > 256) return 0xFFFF;
  sub_request_bus();
  *GA_REG_MEMMODE_W = (0 << 6);                     /* bank 0 */
  volatile u16 * dst = BRAM_BUF_MAIN_ADDR;
  u16 words = (u16) ((len + 1) >> 1);
  const u16 * s = (const u16 *) buf;
  for (u16 i = 0; i < words; i++) dst[i] = s[i];
  *GA_REG_MEMMODE_W = 0x0000;
  sub_release_and_run();
  *GA_REG_COMCMD1_W = len;
  *GA_REG_COMCMD0_W = CMD_BRAM_SAVE;
  if (mcd_wait_ack_timeout(CMD_BRAM_SAVE) != 0) {
    *GA_REG_COMCMD0_W = 0;
    return 0xFFFF;
  }
  return *GA_REG_COMSTAT1_W;
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

/* DIAGNOSTIC: writes 4 solid-colour stamps and a map that uses
 * mr*16+mc indexing to place stamp 1 (white) at "row 0", stamp 2 (red)
 * at "row 1", stamp 4 (yellow) at "row 2", stamp 5 (navy) at "row 3".
 *
 * If ASIC really uses 16-wide row-major map: image shows W/R/Y/N
 * in 4 horizontal bands.
 * If 8-wide col-major: only W and Y visible in vertical strips
 * (since R and N indices go beyond col=3). */
void mcd_asic_load_map_diagnostic(void)
{
  wait_2m_main_to(0x80000);

  /* Solid-colour stamps at slots 1, 2, 4, 5. */
  static u8 const STAMP_SLOTS[4] = {1, 2, 4, 5};
  static u8 const STAMP_PALS[4]  = {1, 2, 4, 5};
  for (u8 i = 0; i < 4; ++i) {
    u8 pal = STAMP_PALS[i];
    u8 byte_fill = (u8) ((pal << 4) | pal);
    volatile u8 * stamp = (volatile u8 *) (WR_MAIN + STAMP_SLOTS[i] * 0x200);
    for (u16 b = 0; b < 512; ++b) stamp[b] = byte_fill;
  }

  /* Map: row 0 = stamp 1, row 1 = stamp 2, row 2 = stamp 4, row 3 = stamp 5.
   * Use SAME indexing as web: map_buf[mr*16+mc] for mc=0..3 mr=0..3. */
  static u16 map_buf[256];
  for (u16 i = 0; i < 256; ++i) map_buf[i] = 0;
  for (u8 mc = 0; mc < 4; ++mc) {
    map_buf[0 * 16 + mc] = (u16) (STAMP_SLOTS[0] * 4);  /* mr=0 → stamp 1 */
    map_buf[1 * 16 + mc] = (u16) (STAMP_SLOTS[1] * 4);  /* mr=1 → stamp 2 */
    map_buf[2 * 16 + mc] = (u16) (STAMP_SLOTS[2] * 4);  /* mr=2 → stamp 4 */
    map_buf[3 * 16 + mc] = (u16) (STAMP_SLOTS[3] * 4);  /* mr=3 → stamp 5 */
  }
  copy_words((const char *) map_buf, (volatile u16 *) WR_STAMP_MAP, 512);
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

  /* LINE TEST: gray background + yellow vertical line at x=4, white
   * horizontal line at y=4, red diagonal from (0,0) to (31,31), blue
   * anti-diagonal from (31,0) to (0,31). Any zigzag = bug remains. */
  for (u8 y = 0; y < 32; ++y)
    for (u8 x = 0; x < 32; ++x)
      px[y][x] = 15;
  for (u8 i = 0; i < 32; ++i) {
    px[i][4]      = 4;       /* yellow vertical line at x=4 */
    px[4][i]      = 1;       /* white horizontal line at y=4 */
    px[i][i]      = 2;       /* red main diagonal */
    px[i][31 - i] = 7;       /* blue anti-diagonal */
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

/* Pack g_web_buf cells (5x5 stamps × 4x4 cells each = 20x20 cells) into
 * ASIC stamps + map. Caller has already filled g_web_buf with whatever
 * source content they want (web, text, etc.). Same logic as the bottom
 * of mcd_asic_load_web_stamps — extracted so other callers can reuse. */
void mcd_asic_pack_buf_to_stamps(void)
{
  wait_2m_main_to(0x80000);

  for (u8 mc = 0; mc < 5; ++mc) {
    for (u8 mr = 0; mr < 5; ++mr) {
      u8 stamp_idx = (u8) (mc * 5 + mr + 1);          /* 1..25 */
      volatile u8 * stamp = (volatile u8 *) (WR_MAIN + stamp_idx * 0x200);
      for (u8 t = 0; t < 16; ++t) {
        u8 in_col = t >> 2;
        u8 in_row = t & 3;
        u8 src_gc = (u8) (mc * 4 + in_col);
        u8 src_gr = (u8) (mr * 4 + in_row);
        u16 src_off = (u16) ((u16) src_gr * WEB_BUF_CELLS + src_gc) * 32;
        volatile u8 * dst_tile = stamp + (u16) t * 32;
        u8 const * src_tile = web_get_buf() + src_off;
        for (u8 b = 0; b < 32; ++b) dst_tile[b] = src_tile[b];
      }
    }
  }

  /* Stamp map: 5x5 grid at (col=0..4, row=0..4) — 8-wide row-major. */
  static u16 map_buf[256];
  for (u16 i = 0; i < 256; ++i) map_buf[i] = 0;
  for (u8 mc = 0; mc < 5; ++mc) {
    for (u8 mr = 0; mr < 5; ++mr) {
      u8 stamp_idx = (u8) (mc * 5 + mr + 1);
      map_buf[mr * 8 + mc] = (u16) (stamp_idx * 4);
    }
  }
  copy_words((const char *) map_buf, (volatile u16 *) WR_STAMP_MAP, 512);
}

void mcd_asic_load_web_stamps(u8 line_pal)
{
  /* Render 208x208 software web into g_web_buf (26x26 cells) at scale
   * 5/4 → web rim radius 75 → web spans buffer pixels (29..179). */
  web_render_main(line_pal);
  mcd_asic_pack_buf_to_stamps();
}

/* DIAGNOSTIC: write the centre 16x16 cells of g_web_buf DIRECTLY into the
 * IMG buffer in COL-MAJOR tile order (skip the ASIC engine entirely),
 * then DMA + col-major paint via the same backend as mcd_render_asic.
 *
 * IMG tile (col, row) at WR_IMG_BUF + (col*16 + row)*32, matching the
 * paint formula `tile_base + col*16 + row`. Each tile takes 32 bytes
 * from g_web_buf cell (5+col, 5+row).
 *
 * This pinpoints whether the split-halves artefact lives in the ASIC
 * engine (stamp→IMG) or in our software web/extraction. */
void mcd_render_web_direct(u8 line_pal, u16 plane_vram_addr, u16 tile_base,
                           u8 plane_x, u8 plane_y)
{
  web_render_main(line_pal);

  wait_2m_main_to(0x80000);

  /* (1) Pack the centre 16x16 cells into IMG buffer, ROW-MAJOR tile order
   *     to match the ASIC's empirical layout. */
  enum { EXTRACT_OFFSET = 5 };
  for (u8 row = 0; row < 16; ++row) {
    for (u8 col = 0; col < 16; ++col) {
      u8 src_gc = (u8) (EXTRACT_OFFSET + col);
      u8 src_gr = (u8) (EXTRACT_OFFSET + row);
      u16 src_off = (u16) ((u16) src_gr * WEB_BUF_CELLS + src_gc) * 32;
      u16 dst_off = (u16) (((u16) row * 16 + col) * 32);
      u8 const * src = web_get_buf() + src_off;
      volatile u8 * dst = WR_IMG_BUF + dst_off;
      for (u8 b = 0; b < 32; ++b) dst[b] = src[b];
    }
  }

  /* (2) DMA + paint — identical to mcd_render_asic's tail, including the
   *     BIOS DMA workaround. */
  u16 mode2_reg = vdp_regs[1];
  vdp_ctrl = mode2_reg | VDP_DMA_ENABLE;
  vdp_dma_transfer((char const *) (WR_IMG_BUF + 2),
                   to_vdp_addr(tile_base * 32) | VRAM_W, 4096);
  vdp_ctrl = mode2_reg;
  vdp_ctrl_32 = to_vdp_addr(tile_base * 32) | VRAM_W;
  vdp_data_32 = *((volatile u32 const *) WR_IMG_BUF);

  for (u8 row = 0; row < 16; ++row) {
    vdp_ctrl_32 = to_vdp_addr(plane_vram_addr + ((plane_y + row) * 64 + plane_x) * 2) | VRAM_W;
    for (u8 col = 0; col < 16; ++col) {
      vdp_data = (u16) (tile_base + row * 16 + col);
    }
  }
}

/* RAW passthrough — no transformation. Use this to see the unmodified
 * IMG buffer layout for further empirical analysis. */
static void repack_img_buf(void)
{
  u8 const * src = (u8 const *) WR_IMG_BUF;
  volatile u8 * dst = (volatile u8 *) WR_REPACK_SCR;
  for (u16 i = 0; i < 8192; ++i) dst[i] = src[i];
}

/* Common ASIC render tail: kick sub, wait for completion, DMA + paint.
 * `cmd` is the CMD_RENDER_* code already loaded with whatever extra params
 * (COMCMD1/2) were set by the caller. */
static void asic_render_kick_and_paint(u16 cmd, u16 plane_vram_addr,
                                       u16 tile_base, u8 plane_x, u8 plane_y)
{
  /* Hand WR to sub. */
  grant_2m_main_to(0x80000);
  *GA_REG_COMCMD0_W = cmd;
  while (*GA_REG_COMSTAT0_W != cmd) ;
  *GA_REG_COMCMD0_W = 0;
  while (*GA_REG_COMSTAT0_W != 0) ;
  wait_2m_main_to(0x80000);

  /* BIOS DMA workaround for Mega CD WR→VRAM hardware bug.
   * 160x160 IMG = 12800 bytes = 6400 words. */
  u16 mode2_reg = vdp_regs[1];
  vdp_ctrl = mode2_reg | VDP_DMA_ENABLE;
  vdp_dma_transfer((char const *) (WR_IMG_BUF + 2),
                   to_vdp_addr(tile_base * 32) | VRAM_W, 6400);
  vdp_ctrl = mode2_reg;
  vdp_ctrl_32 = to_vdp_addr(tile_base * 32) | VRAM_W;
  vdp_data_32 = *((volatile u32 const *) WR_IMG_BUF);

  /* Paint plane COL-MAJOR for 20x20 cells: tile = col*20 + row.
   * Priority=1 so the web stays in front of the plane-A starfield
   * (matches the variant pipeline's web_paint_plane_b). */
  for (u8 row = 0; row < 20; ++row) {
    vdp_ctrl_32 = to_vdp_addr(plane_vram_addr + ((plane_y + row) * 64 + plane_x) * 2) | VRAM_W;
    for (u8 col = 0; col < 20; ++col) {
      vdp_data = (u16) (0x8000 | (tile_base + col * 20 + row));
    }
  }
}

void mcd_render_asic(u16 plane_vram_addr, u16 tile_base, u8 plane_x, u8 plane_y, u8 warp)
{
  u16 cmd = warp ? CMD_RENDER_WARP : CMD_RENDER_ROT;
  asic_render_kick_and_paint(cmd, plane_vram_addr, tile_base, plane_x, plane_y);
}

void mcd_render_asic_tilt(u16 plane_vram_addr, u16 tile_base,
                          u8 plane_x, u8 plane_y, s16 tilt_x, s16 tilt_y)
{
  *GA_REG_COMCMD1_W = (u16) tilt_x;
  *GA_REG_COMCMD2_W = (u16) tilt_y;
  asic_render_kick_and_paint(CMD_RENDER_TILT, plane_vram_addr, tile_base, plane_x, plane_y);
}

/* Uniform-scale zoom via sub-side render_scale. dx = 0x0800 is identity;
 * dx > 0x0800 zooms out (web shrinks toward centre); dx < 0x0800 zooms
 * in. Caller has already loaded the stamps + painted plane B in
 * col-major layout. */
void mcd_render_asic_scale(u16 plane_vram_addr, u16 tile_base,
                           u8 plane_x, u8 plane_y, s16 dx)
{
  *GA_REG_COMCMD1_W = (u16) dx;
  asic_render_kick_and_paint(CMD_RENDER_SCALE, plane_vram_addr, tile_base, plane_x, plane_y);
}

void mcd_wait_ack(u16 expected)
{
  while (*GA_REG_COMSTAT0_W != expected) ;
  *GA_REG_COMCMD0_W = 0;
  while (*GA_REG_COMSTAT0_W != 0) ;
}

/* Same as mcd_wait_ack but bounded to ~1 M poll iterations (~0.5 s on
 * a 7.6 MHz 68000). Returns 0 on success, non-zero on timeout — used by
 * BRAM I/O so a misbehaving BIOS dispatcher can't hang the boot. */
static u16 mcd_wait_ack_timeout(u16 expected)
{
  u32 t = 1000000ul;
  while (*GA_REG_COMSTAT0_W != expected) { if (--t == 0) return 1; }
  *GA_REG_COMCMD0_W = 0;
  t = 1000000ul;
  while (*GA_REG_COMSTAT0_W != 0) { if (--t == 0) return 2; }
  return 0;
}

/* ---- Pre-baked 3D variants ----------------------------------------------
 * Each variant is a 12800-byte software-rendered web with a different
 * camera position (vp_x/vp_y derived from one lane's outer rim). Stored
 * in WR at offset variant_idx * WEB_BUF_BYTES.
 *
 * At install: pre-render N variants. Main holds WR ownership from here
 * on (sub doesn't need WR — music plays from PRG-RAM, SFX from PCM-RAM).
 *
 * Per frame: DMA the variant matching the current player lane to plane B
 * tiles at $5000 (= tile $280). BIOS WR→VRAM DMA workaround applied
 * (DMA from src+2, manually rewrite first long word). */

/* Variant K's WR-offset. The natural offset K*12800 would put variant 10 at
 * 0x1F400..0x225FF, straddling WR offset 0x20000; VDP DMA from that source
 * range delivers data to the wrong VRAM destination (cause of the lane-10
 * glitch). Skip the colliding slot by remapping K=10..N-1 to (K+1)*12800.
 * Slot at K*12800=0x1F400 is reserved/unused. */
static u32 variant_wr_offset(u8 k)
{
  if (k < 10) return (u32) k * 12800;
  return (u32) (k + 1) * 12800;
}

void mcd_prebake_web_variants(u8 pal)
{
  wait_2m_main_to(0x80000);

  u8 const n = web_lane_count();
  u8 const * src_buf = web_get_buf();

  /* Snapshot lane outer-rim positions at vp=0 so the variant cameras
   * are computed from STATIC world-space positions, not from positions
   * that shift with each variant's own vp. */
  g_vp_x = 0;
  g_vp_y = 0;
  web_project();
  s16 lane_off_x[18], lane_off_y[18];   /* MAX_LANES = 18 in web.h */
  for (u8 k = 0; k < n; ++k) {
    lane_off_x[k] = (s16) (web_pixel_x(k, 0x10000) - 160);  /* world offset */
    lane_off_y[k] = (s16) (web_pixel_y(k, 0x10000) - 112);
  }

  /* Pulse cram[1] (LOADING text colour) one step per variant. 8-step
   * cycle; with ~15 variants per shape the loop runs through ~2 cycles
   * over the bake. Safe to do here because no VDP transfer is active
   * between variants — the CPU-only render + RAM→WR copy don't touch
   * the VDP control port. */
  static const u16 LOADING_PULSE[8] = {
    0x0EEE, 0x0CCC, 0x0888, 0x0444, 0x0222, 0x0444, 0x0888, 0x0CCC,
  };
  for (u8 k = 0; k < n; ++k) {
    vdp_ctrl_32 = to_vdp_addr(1 * 2) | CRAM_W;
    vdp_data    = LOADING_PULSE[k & 0x7];

    /* Damped /4: camera leans toward the player's lane without full follow. */
    g_vp_x = (s16) (lane_off_x[k] >> 2);
    g_vp_y = (s16) (lane_off_y[k] >> 2);
    web_project();
    web_render_main(pal);
    /* Copy g_web_buf to WR slot k. Use word writes (Mode 1 byte coherency).
     * Skip the slot at K=10's natural position because it would straddle
     * WR offset 0x20000 — VDP DMA from a source range crossing that
     * boundary delivers data to the wrong VRAM destination (the cause of
     * the lane-10 glitch). variant_wr_offset() handles the offset
     * remapping; we mirror it on the read side in mcd_dma_variant_to_vram. */
    volatile u16 * dst = (volatile u16 *) (WR_MAIN + variant_wr_offset(k));
    u16 const * src = (u16 const *) src_buf;
    for (u16 i = 0; i < 12800 / 2; ++i) dst[i] = src[i];
  }
}

/* DMA variant K from WR to VRAM tile range $280 (= byte $5000).
 * Uses BIOS workaround for the WR→VRAM hardware shift bug.
 * Caller has already updated g_vp_x/g_vp_y to match this variant. */
void mcd_dma_variant_to_vram(u8 variant_idx)
{
  volatile u8 * src = WR_MAIN + variant_wr_offset(variant_idx);

  u16 mode2_reg = vdp_regs[1];
  vdp_ctrl = VDP_REG_AUTOINC | 2;
  vdp_ctrl = mode2_reg | VDP_DMA_ENABLE;
  vdp_dma_transfer((char const *) (src + 2),
                   to_vdp_addr(0x280 * 32) | VRAM_W, 12800 / 2);
  vdp_ctrl = mode2_reg;
  /* Rewrite first long word manually (BIOS dmaTransferToVramWithRewrite). */
  vdp_ctrl_32 = to_vdp_addr(0x280 * 32) | VRAM_W;
  vdp_data_32 = *((volatile u32 const *) src);
}
