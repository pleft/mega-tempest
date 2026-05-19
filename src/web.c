#include "web.h"
#include "sprites.h"
#include <main/vdp.h>
#include <main/memmap.h>

#if VIDEO == PAL
#define VIDEO_SIGNAL VDP_PAL_VIDEO
#else
#define VIDEO_SIGNAL 0
#endif

/* ---- VRAM layout (post-MC-T6c) -------------------------------------------
 *   Web tile data: $280..$4BF  (24x24 = 576 tiles, VRAM $5000..$97FF)
 *   Sprite tiles:  $4C0 (8x8), $4D0..$4D3 (16x16)  — MUST stay above $4BF
 *   Plane B tilemap: $4000..$4FFF (web paints cells 8..31, 2..25)
 *   Sprite attribute table: $B800
 */
#define ROT_TILE_BASE_IDX  0x280
#define ROT_TILE_VRAM_ADDR (ROT_TILE_BASE_IDX * 32)
#define PLANE_B_PAINT_COL  8
#define PLANE_B_PAINT_ROW  2

#define WEB_IMG_W   192
#define WEB_IMG_H   192
#define WEB_CELLS_W 24
#define WEB_CELLS_H 24
#define WEB_BUF_BYTES (WEB_CELLS_W * WEB_CELLS_H * 32)   /* 18432 bytes */

/* Sprite-tile VRAM slots (data from src/sprites.{c,h}, baked by
 * tools/extract_mcd_sprites.py from tempest2k-source/src/obj2d.s).
 * Player is 16 pre-rotated claws (one per lane) packed contiguously so we
 * can pick by `PLAYER_TILE_BASE + lane * 4`. */
#define FLIPPER_TILE_FAR    0x4C0       /* 8x8  = 1 tile,  red flipper */
#define FLIPPER_TILE_MID    0x4D0       /* 16x16 = 4 tiles, red flipper */
#define PLAYER_TILE_BASE    0x4E0       /* 16 × 4 tiles = 64 tiles, $4E0..$51F */
#define SHOT_TILE           0x520       /* 8x8 = 1 tile, white shot */

#define PLAYER_TILES_PER_LANE 4         /* 2x2 = 4 tiles per claw rotation */

#define SPR_TABLE_VRAM 0xb800
#define SPR_MAX 32

static u8 g_web_buf[WEB_BUF_BYTES];
static s16 g_lane_rim_x[NUM_LANES];
static s16 g_lane_rim_y[NUM_LANES];

/* Which of the 16 pre-rotated claws best faces inward from each lane's
 * rim point. Filled by web_init() per shape — radial shapes (CIRCLE,
 * OCTAGON, etc.) end up with claw_idx == lane, but non-radial shapes
 * (FAN, where every lane is on a horizontal line) get a per-lane choice. */
static u8 g_player_claw_idx[NUM_LANES];

/* Direction each claw "opens toward", as int8 scaled by 64.
 * Derived from native UP = (0,-1) rotated by -L*π/8 (matches the
 * extractor's rotation). Used by web_init's dot-product search. */
static const s8 CLAW_DIR_X[16] = {
    0, -25, -45, -59, -64, -59, -45, -25,
    0,  25,  45,  59,  64,  59,  45,  25,
};
static const s8 CLAW_DIR_Y[16] = {
  -64, -59, -45, -25,   0,  25,  45,  59,
   64,  59,  45,  25,   0, -25, -45, -59,
};

/* Scale base-radius-60 shape-table coords → render radius 80 (v * 4/3).
 * Uses inline divs.w to avoid pulling __divsi3 from libgcc. */
static inline s16 web_scale(s8 v)
{
  s32 t = (s32) v * 4;
  s16 three = 3;
  asm ("divs.w %1, %0" : "+d"(t) : "d"(three) : "cc");
  return (s16) t;
}

/* ---- 16-lane rim offsets — one table per shape ---------------------------
 * Each lane's (dx, dy) is the offset from web centre to that lane's rim,
 * in pixels at the base radius (60). web_scale() rescales to render radius
 * (80). Lane 0 points "down" (+Y), going clockwise to lane 15. */

static const s8 WEB_RIM_CIRCLE[16][2] = {
  {   0,  60 }, {  23,  55 }, {  42,  42 }, {  55,  23 },
  {  60,   0 }, {  55, -23 }, {  42, -42 }, {  23, -55 },
  {   0, -60 }, { -23, -55 }, { -42, -42 }, { -55, -23 },
  { -60,   0 }, { -55,  23 }, { -42,  42 }, { -23,  55 },
};

static const s8 WEB_RIM_SQUARE[16][2] = {
  {   0,  60 }, {  25,  60 }, {  60,  60 }, {  60,  25 },
  {  60,   0 }, {  60, -25 }, {  60, -60 }, {  25, -60 },
  {   0, -60 }, { -25, -60 }, { -60, -60 }, { -60, -25 },
  { -60,   0 }, { -60,  25 }, { -60,  60 }, { -25,  60 },
};

/* PLUS / cross — cardinal arms reach out, diagonals are pulled back. */
static const s8 WEB_RIM_PLUS[16][2] = {
  {   0,  60 }, {   8,  25 }, {  15,  15 }, {  25,   8 },
  {  60,   0 }, {  25,  -8 }, {  15, -15 }, {   8, -25 },
  {   0, -60 }, {  -8, -25 }, { -15, -15 }, { -25,  -8 },
  { -60,   0 }, { -25,   8 }, { -15,  15 }, {  -8,  25 },
};

/* Diamond — like square but rotated 45°. */
static const s8 WEB_RIM_DIAMOND[16][2] = {
  {   0,  60 }, {  15,  45 }, {  30,  30 }, {  45,  15 },
  {  60,   0 }, {  45, -15 }, {  30, -30 }, {  15, -45 },
  {   0, -60 }, { -15, -45 }, { -30, -30 }, { -45, -15 },
  { -60,   0 }, { -45,  15 }, { -30,  30 }, { -15,  45 },
};

/* Triangle — equilateral, pointing up. */
static const s8 WEB_RIM_TRIANGLE[16][2] = {
  {   0,  30 }, {  13,  30 }, {  26,  30 }, {  39,  30 },
  {  52,  30 }, {  39,   7 }, {  26, -15 }, {  13, -37 },
  {   0, -60 }, { -13, -37 }, { -26, -15 }, { -39,   7 },
  { -52,  30 }, { -39,  30 }, { -26,  30 }, { -13,  30 },
};

/* Octagon — even lanes = vertices, odd lanes = midpoints. */
static const s8 WEB_RIM_OCTAGON[16][2] = {
  {   0,  60 }, {  21,  51 }, {  42,  42 }, {  51,  21 },
  {  60,   0 }, {  51, -21 }, {  42, -42 }, {  21, -51 },
  {   0, -60 }, { -21, -51 }, { -42, -42 }, { -51, -21 },
  { -60,   0 }, { -51,  21 }, { -42,  42 }, { -21,  51 },
};

/* Star — 8-pointed: even lanes at full radius 60, odd lanes pulled in. */
static const s8 WEB_RIM_STAR[16][2] = {
  {   0,  60 }, {  11,  28 }, {  42,  42 }, {  28,  11 },
  {  60,   0 }, {  28, -11 }, {  42, -42 }, {  11, -28 },
  {   0, -60 }, { -11, -28 }, { -42, -42 }, { -28, -11 },
  { -60,   0 }, { -28,  11 }, { -42,  42 }, { -11,  28 },
};

/* Fan / Line — all 16 rim points along a horizontal line at +40. */
static const s8 WEB_RIM_FAN[16][2] = {
  { -60,  40 }, { -52,  40 }, { -44,  40 }, { -36,  40 },
  { -28,  40 }, { -20,  40 }, { -12,  40 }, {  -4,  40 },
  {   4,  40 }, {  12,  40 }, {  20,  40 }, {  28,  40 },
  {  36,  40 }, {  44,  40 }, {  52,  40 }, {  60,  40 },
};

const char * const WEB_SHAPE_NAMES[WEB_SHAPE_COUNT] = {
  "CIRCLE  ", "SQUARE  ", "PLUS    ", "DIAMOND ",
  "TRIANGLE", "OCTAGON ", "STAR    ", "FAN     ",
};

static const s8 (* const WEB_RIMS[WEB_SHAPE_COUNT])[2] = {
  WEB_RIM_CIRCLE, WEB_RIM_SQUARE,  WEB_RIM_PLUS,    WEB_RIM_DIAMOND,
  WEB_RIM_TRIANGLE, WEB_RIM_OCTAGON, WEB_RIM_STAR,  WEB_RIM_FAN,
};

u8 g_web_shape = WEB_SHAPE_CIRCLE;

// ---- Lane projection ------------------------------------------------------

void web_init(void)
{
  const s8 (*rim)[2] = WEB_RIMS[g_web_shape];
  for (u8 i = 0; i < NUM_LANES; ++i) {
    g_lane_rim_x[i] = (s16) (WEB_CENTER_X + web_scale(rim[i][0]));
    g_lane_rim_y[i] = (s16) (WEB_CENTER_Y + web_scale(rim[i][1]));
  }

  /* Pick the claw rotation that best points from each rim toward the centre.
   * dot-product against all 16 directions, take the max. (Values stay well
   * within s32 — rim deltas ≤ 80, dir components ≤ 64, sum ≤ ~10240.) */
  for (u8 i = 0; i < NUM_LANES; ++i) {
    s16 tx = (s16) (WEB_CENTER_X - g_lane_rim_x[i]);
    s16 ty = (s16) (WEB_CENTER_Y - g_lane_rim_y[i]);
    u8 best = 0;
    s32 best_dot = -0x7FFFFFFFL;
    for (u8 k = 0; k < 16; ++k) {
      s32 dot = (s32) tx * CLAW_DIR_X[k] + (s32) ty * CLAW_DIR_Y[k];
      if (dot > best_dot) { best_dot = dot; best = k; }
    }
    g_player_claw_idx[i] = best;
  }
}

s16 web_pixel_x(u8 lane, fp16 depth_fp)
{
  s32 dx = (s32) g_lane_rim_x[lane] - WEB_CENTER_X;
  return (s16) (WEB_CENTER_X + ((dx * depth_fp) >> 16));
}
s16 web_pixel_y(u8 lane, fp16 depth_fp)
{
  s32 dy = (s32) g_lane_rim_y[lane] - WEB_CENTER_Y;
  return (s16) (WEB_CENTER_Y + ((dy * depth_fp) >> 16));
}

// ---- Web rasterisation (main-RAM buffer) ----------------------------------

static void web_setpx(s16 x, s16 y, u8 pal)
{
  if ((u16) x >= WEB_IMG_W || (u16) y >= WEB_IMG_H) return;
  u16 cell_off = (u16) ((y >> 3) * WEB_CELLS_W + (x >> 3)) * 32;
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
    web_setpx(x0, y0, pal);
    if (x0 == x1 && y0 == y1) break;
    s16 e2 = (s16) (err << 1);
    if (e2 >= dy) { err += dy; x0 = (s16) (x0 + sx); }
    if (e2 <= dx) { err += dx; y0 = (s16) (y0 + sy); }
  }
}

void web_render_main(u8 pal)
{
  const s8 (*rim)[2] = WEB_RIMS[g_web_shape];
  for (u16 i = 0; i < WEB_BUF_BYTES; ++i) g_web_buf[i] = 0;
  s16 const cx = WEB_IMG_W / 2;
  s16 const cy = WEB_IMG_H / 2;

  /* 1. Radial lines from centre to each scaled rim point. */
  for (u8 lane = 0; lane < 16; ++lane) {
    s16 rx = (s16) (cx + web_scale(rim[lane][0]));
    s16 ry = (s16) (cy + web_scale(rim[lane][1]));
    web_line(cx, cy, rx, ry, pal);
  }

  /* 2. Rim polygon — connect adjacent rim points. */
  for (u8 lane = 0; lane < 16; ++lane) {
    u8 next = (u8) ((lane + 1) & 0x0F);
    s16 ax = (s16) (cx + web_scale(rim[lane][0]));
    s16 ay = (s16) (cy + web_scale(rim[lane][1]));
    s16 bx = (s16) (cx + web_scale(rim[next][0]));
    s16 by = (s16) (cy + web_scale(rim[next][1]));
    web_line(ax, ay, bx, by, pal);
  }
}

void web_dma_main_to_vram(void)
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

void web_paint_plane_b(void)
{
  for (u8 row = 0; row < WEB_CELLS_H; ++row) {
    u16 plane_b_addr = 0x4000 + ((PLANE_B_PAINT_ROW + row) * 64 + PLANE_B_PAINT_COL) * 2;
    vdp_ctrl_32 = to_vdp_addr(plane_b_addr) | VRAM_W;
    for (u8 col = 0; col < WEB_CELLS_W; ++col)
      vdp_data = (u16) (ROT_TILE_BASE_IDX + row * WEB_CELLS_W + col);
  }
}

void web_clear_plane_b(void)
{
  for (u8 row = 0; row < WEB_CELLS_H; ++row) {
    u16 plane_b_addr = 0x4000 + ((PLANE_B_PAINT_ROW + row) * 64 + PLANE_B_PAINT_COL) * 2;
    vdp_ctrl_32 = to_vdp_addr(plane_b_addr) | VRAM_W;
    for (u8 col = 0; col < WEB_CELLS_W; ++col) vdp_data = 0;
  }
}

// ---- Sprite tile DMA ------------------------------------------------------
//
// Sprite tile data is pre-baked in src/sprites.{c,h} from the Jaguar T2K
// polygon meshes by tools/extract_mcd_sprites.py. Here we just DMA each
// blob to its assigned VRAM slot.

/* Sprite size byte encoding: bits 3-2 = V size-1, bits 1-0 = H size-1.
 *   1x1 (8x8):   0x00, 1 tile,  16 words
 *   2x2 (16x16): 0x05, 4 tiles, 64 words
 * `half` is half the sprite extent in screen pixels, for centring. */
#define SPR_SIZE_1x1  0x00
#define SPR_SIZE_2x2  0x05
typedef struct { u8 size_byte; u16 tile_base; u8 half; } SpriteSizeDef;
static const SpriteSizeDef FLIPPER_SIZES[2] = {
  { SPR_SIZE_1x1, FLIPPER_TILE_FAR, 4 },     /*  8x8 — far / centre */
  { SPR_SIZE_2x2, FLIPPER_TILE_MID, 8 },     /* 16x16 — near / rim  */
};
static const SpriteSizeDef SHOT_SIZE = { SPR_SIZE_1x1, SHOT_TILE, 4 };
/* PLAYER picks tile_base = PLAYER_TILE_BASE + lane*4 at render time. */

static const u8 * const PLAYER_TILES[NUM_LANES] = {
  SPR_PLAYER_L00, SPR_PLAYER_L01, SPR_PLAYER_L02, SPR_PLAYER_L03,
  SPR_PLAYER_L04, SPR_PLAYER_L05, SPR_PLAYER_L06, SPR_PLAYER_L07,
  SPR_PLAYER_L08, SPR_PLAYER_L09, SPR_PLAYER_L10, SPR_PLAYER_L11,
  SPR_PLAYER_L12, SPR_PLAYER_L13, SPR_PLAYER_L14, SPR_PLAYER_L15,
};

static void dma_tile_blob(const u8 * src, u16 tile_base, u16 byte_count)
{
  u16 const mode2_dma_on  = VDP_REG_MODE2 | VDP_MD_DISPLAY_MODE | VDP_VBLANK_ENABLE
                          | VIDEO_SIGNAL | VDP_DISPLAY_ENABLE | VDP_DMA_ENABLE;
  u16 const mode2_dma_off = mode2_dma_on & ~VDP_DMA_ENABLE;
  vdp_ctrl = VDP_REG_AUTOINC | 2;
  vdp_ctrl = mode2_dma_on;
  vdp_dma_transfer((char const *) src,
                   to_vdp_addr(tile_base * 32) | VRAM_W,
                   (u16) (byte_count / 2));
  vdp_ctrl = mode2_dma_off;
}

void load_sprite_tiles_to_vram(void)
{
  dma_tile_blob(SPR_FLIPPER_S, FLIPPER_TILE_FAR, sizeof SPR_FLIPPER_S);
  dma_tile_blob(SPR_FLIPPER_M, FLIPPER_TILE_MID, sizeof SPR_FLIPPER_M);
  dma_tile_blob(SPR_SHOT,      SHOT_TILE,        sizeof SPR_SHOT);
  /* All 16 player claws packed contiguously starting at PLAYER_TILE_BASE. */
  for (u8 i = 0; i < NUM_LANES; ++i) {
    dma_tile_blob(PLAYER_TILES[i],
                  (u16) (PLAYER_TILE_BASE + i * PLAYER_TILES_PER_LANE),
                  sizeof SPR_PLAYER_L00);
  }
}

static inline void emit_sprite(u16 * buf, u8 idx, const SpriteSizeDef * sz,
                               s16 px, s16 py)
{
  buf[idx * 4 + 0] = (u16) (py + 128 - sz->half);                     /* Y */
  buf[idx * 4 + 1] = (u16) (((u16) sz->size_byte << 8) | (idx + 1));  /* size + link */
  buf[idx * 4 + 2] = sz->tile_base;                                   /* tile */
  buf[idx * 4 + 3] = (u16) (px + 128 - sz->half);                     /* X */
}

void render_sprites(void)
{
  static u16 spr_buf[SPR_MAX * 4];
  u8 n = 0;

  /* Pass 1: PLAYER first → sprite 0 = highest priority (drawn on top).
   * Claw rotation is picked per-shape in web_init (g_player_claw_idx)
   * so the open end always faces the centre, even on FAN where lanes
   * don't sit on a circle. */
  for (Entity * e = g_active_head; e; e = e->next) {
    if (e->type != E_PLAYER || n >= SPR_MAX) continue;
    s16 px = web_pixel_x(e->lane, e->depth_fp);
    s16 py = web_pixel_y(e->lane, e->depth_fp);
    u8 claw = g_player_claw_idx[e->lane];
    SpriteSizeDef sz = { SPR_SIZE_2x2,
                         (u16) (PLAYER_TILE_BASE + claw * PLAYER_TILES_PER_LANE),
                         8 };
    emit_sprite(spr_buf, n++, &sz, px, py);
  }

  /* Pass 2: SHOTS. */
  for (Entity * e = g_active_head; e; e = e->next) {
    if (e->type != E_SHOT || n >= SPR_MAX) continue;
    s16 px = web_pixel_x(e->lane, e->depth_fp);
    s16 py = web_pixel_y(e->lane, e->depth_fp);
    emit_sprite(spr_buf, n++, &SHOT_SIZE, px, py);
  }

  /* Pass 3: FLIPPERS, depth-scaled. */
  for (Entity * e = g_active_head; e; e = e->next) {
    if (e->type != E_FLIPPER || n >= SPR_MAX) continue;
    s16 px = web_pixel_x(e->lane, e->depth_fp);
    s16 py = web_pixel_y(e->lane, e->depth_fp);
    u8 size_idx = (e->depth_fp < 0x8000) ? 0 : 1;
    emit_sprite(spr_buf, n++, &FLIPPER_SIZES[size_idx], px, py);
  }

  if (n == 0) {
    /* Sprite 0 hidden off-screen, chain ends immediately. */
    spr_buf[0] = spr_buf[1] = spr_buf[2] = spr_buf[3] = 0;
    n = 1;
  } else {
    spr_buf[(n - 1) * 4 + 1] &= 0xFF00;     /* terminate chain */
  }
  vdp_ctrl_32 = to_vdp_addr(SPR_TABLE_VRAM) | VRAM_W;
  u16 const words = (u16) (n * 4);
  for (u16 i = 0; i < words; ++i) vdp_data = spr_buf[i];
}
