#include "web.h"
#include "sprites.h"
#include <main/vdp.h>
#include <main/memmap.h>

#if VIDEO == PAL
#define VIDEO_SIGNAL VDP_PAL_VIDEO
#else
#define VIDEO_SIGNAL 0
#endif

/* ---- VRAM layout (post-MC-T10) -------------------------------------------
 *   Web tile data: $280..$523  (26x26 = 676 tiles, VRAM $5000..$A47F)
 *   Plane B tilemap: $4000..$4FFF (web paints cells 7..32, 1..26)
 *   Sprite tiles must live above the web range and below the sprite
 *   attribute table at VRAM $B800 (= tile $5C0):
 *     $540        flipper FAR (8x8)
 *     $550..$553  flipper MID (16x16)
 *     $560..$59F  player claws — 16 rotations × 4 tiles
 *     $5A0        shot (8x8)
 *   Sprite attribute table: $B800
 */
#define ROT_TILE_BASE_IDX  0x280
#define ROT_TILE_VRAM_ADDR (ROT_TILE_BASE_IDX * 32)
#define PLANE_B_PAINT_COL  7
#define PLANE_B_PAINT_ROW  1

#define WEB_IMG_W   208
#define WEB_IMG_H   208
#define WEB_CELLS_W 26
#define WEB_CELLS_H 26
#define WEB_BUF_BYTES (WEB_CELLS_W * WEB_CELLS_H * 32)   /* 21632 bytes */

/* Perspective: the inner "vanishing" shape sits offset from the geometric
 * centre, so the web reads as a tilted tube (vanishing point above the
 * outer rim's centre — you're looking slightly down into the playfield).
 * Positive = downward; for a Tempest-style "look down the tube" we use
 * negative = inner shape lifted upward. */
#define WEB_VANISH_OFFSET_Y  (-25)

/* Sprite-tile VRAM slots (data from src/sprites.{c,h}, baked by
 * tools/extract_mcd_sprites.py from tempest2k-source/src/obj2d.s).
 * Player is 16 pre-rotated claws (one per lane) packed contiguously so we
 * can pick by `PLAYER_TILE_BASE + lane * 4`. */
#define FLIPPER_TILE_FAR    0x540       /* 8x8  = 1 tile,  red flipper */
#define FLIPPER_TILE_MID    0x550       /* 16x16 = 4 tiles, red flipper */
#define PLAYER_TILE_BASE    0x560       /* 16 × 4 tiles = 64 tiles, $560..$59F */
#define SHOT_TILE           0x5A0       /* 8x8 = 1 tile, white shot */

#define PLAYER_TILES_PER_LANE 4         /* 2x2 = 4 tiles per claw rotation */

#define SPR_TABLE_VRAM 0xb800
#define SPR_MAX 32

static u8 g_web_buf[WEB_BUF_BYTES];
static s16 g_lane_rim_x[NUM_LANES];
static s16 g_lane_rim_y[NUM_LANES];

/* Inner-rim (vanishing) points — at 1/4 the rim distance from centre, so
 * the web has a "tunnel" 3D feel rather than all lines converging at a
 * single point. Filled alongside rim points in web_init. */
static s16 g_lane_inner_x[NUM_LANES];
static s16 g_lane_inner_y[NUM_LANES];

/* Which of the 16 pre-rotated claws best faces inward from each lane's
 * rim point. Filled by web_init() per shape — radial shapes (CIRCLE,
 * OCTAGON, etc.) end up with claw_idx == lane, but non-radial shapes
 * (FAN, where every lane is on a horizontal line) get a per-lane choice. */
static u8 g_player_claw_idx[NUM_LANES];

/* Rolling-claw animation state. `g_claw_render_idx` is what render_sprites
 * actually shows; `g_claw_spin_steps` counts down a fresh revolution
 * triggered by each lane change. */
static u8 g_claw_render_idx;
static u8 g_claw_spin_steps;
static s8 g_claw_spin_dir;
#define CLAW_SPIN_STEPS_PER_LANE  16     /* one full revolution per move */
#define CLAW_SPIN_STEPS_PER_FRAME 4      /* finishes in 4 frames = LANE_HOLD_REPEAT */

/* Player-slide animation. When a lane changes the player visually slides
 * from the old lane's rim position to the new one's over SLIDE_FRAMES
 * frames, so the claw glides instead of jumping. */
static u8 g_slide_from_lane;
static u8 g_slide_to_lane;
static u8 g_slide_progress;              /* 0..SLIDE_FRAMES (at target) */
#define SLIDE_FRAMES 4                   /* matches LANE_HOLD_REPEAT */

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

/* Scale base-radius-60 shape-table coords → render radius 100 (v * 5/3).
 * Uses inline divs.w to avoid pulling __divsi3 from libgcc. */
static inline s16 web_scale(s8 v)
{
  s32 t = (s32) v * 5;
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
    s16 sx = web_scale(rim[i][0]);
    s16 sy = web_scale(rim[i][1]);
    g_lane_rim_x[i]   = (s16) (WEB_CENTER_X + sx);
    g_lane_rim_y[i]   = (s16) (WEB_CENTER_Y + sy);
    /* Inner point at 1/4 the rim distance (signed shift right by 2) — gives
     * the "tunnel" 3D shape from each lane's rim down toward a small inner
     * copy of the same outline. */
    g_lane_inner_x[i] = (s16) (WEB_CENTER_X + (sx >> 3));
    g_lane_inner_y[i] = (s16) (WEB_CENTER_Y + WEB_VANISH_OFFSET_Y + (sy >> 3));
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

  /* Reset animation state to a settled-at-lane-0 baseline. */
  g_claw_render_idx  = g_player_claw_idx[0];
  g_claw_spin_steps  = 0;
  g_claw_spin_dir    = 1;
  g_slide_from_lane  = 0;
  g_slide_to_lane    = 0;
  g_slide_progress   = SLIDE_FRAMES;
}

// ---- Rolling-claw animation + position slide -----------------------------

void web_lane_changed(u8 new_lane, s8 dir)
{
  /* Position slide: previous TARGET becomes new SOURCE so the player
   * visually glides between rim points instead of jumping. */
  g_slide_from_lane = g_slide_to_lane;
  g_slide_to_lane   = new_lane;
  g_slide_progress  = 0;

  /* Claw spin: start a fresh revolution. Reset (not accumulate) — the
   * steady state under hold-to-repeat is one revolution per
   * LANE_HOLD_REPEAT which already feels like a continuous roll. */
  g_claw_spin_steps = CLAW_SPIN_STEPS_PER_LANE;
  g_claw_spin_dir   = (dir >= 0) ? 1 : -1;
}

void web_claw_tick(u8 current_lane)
{
  /* Advance the slide first so render_sprites sees a fresh interpolated
   * position alongside the up-to-date claw_render_idx. */
  if (g_slide_progress < SLIDE_FRAMES) g_slide_progress++;

  if (g_claw_spin_steps > 0) {
    u8 steps = (g_claw_spin_steps >= CLAW_SPIN_STEPS_PER_FRAME)
                  ? CLAW_SPIN_STEPS_PER_FRAME
                  : g_claw_spin_steps;
    for (u8 i = 0; i < steps; ++i)
      g_claw_render_idx = (u8) ((g_claw_render_idx + g_claw_spin_dir + 16) & 0xF);
    g_claw_spin_steps = (u8) (g_claw_spin_steps - steps);
    if (g_claw_spin_steps == 0)
      g_claw_render_idx = g_player_claw_idx[current_lane];   /* settle inward */
  } else {
    g_claw_render_idx = g_player_claw_idx[current_lane];
  }
}

void web_player_snap_to(u8 lane)
{
  /* No animation — used on respawn so visual matches logic immediately. */
  g_slide_from_lane = lane;
  g_slide_to_lane   = lane;
  g_slide_progress  = SLIDE_FRAMES;
  g_claw_render_idx = g_player_claw_idx[lane];
  g_claw_spin_steps = 0;
}

/* Interpolated player position — linear lerp between source and target
 * lane rim points based on g_slide_progress. SLIDE_FRAMES is a power of 2
 * (4) so the division is a shift. */
static void web_player_render_pos(fp16 depth_fp, s16 * out_x, s16 * out_y)
{
  s16 fx = web_pixel_x(g_slide_from_lane, depth_fp);
  s16 fy = web_pixel_y(g_slide_from_lane, depth_fp);
  s16 tx = web_pixel_x(g_slide_to_lane,   depth_fp);
  s16 ty = web_pixel_y(g_slide_to_lane,   depth_fp);
  s16 p = (s16) g_slide_progress;
  *out_x = (s16) (fx + (s16) (((tx - fx) * p) >> 2));   /* >> 2 = / SLIDE_FRAMES */
  *out_y = (s16) (fy + (s16) (((ty - fy) * p) >> 2));
}

/* Interpolate between the inner-rim point (depth=0) and the outer-rim
 * point (depth=FP_ONE). Shots disappear at the inner rim now, not at the
 * web's geometric centre. Flippers spawn at the inner rim and travel out. */
s16 web_pixel_x(u8 lane, fp16 depth_fp)
{
  s32 dx = (s32) g_lane_rim_x[lane] - g_lane_inner_x[lane];
  return (s16) (g_lane_inner_x[lane] + ((dx * depth_fp) >> 16));
}
s16 web_pixel_y(u8 lane, fp16 depth_fp)
{
  s32 dy = (s32) g_lane_rim_y[lane] - g_lane_inner_y[lane];
  return (s16) (g_lane_inner_y[lane] + ((dy * depth_fp) >> 16));
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

  /* 1. Radial lines from INNER rim (offset upward for perspective) to
   * OUTER rim. The inner is at 1/8 the scaled rim distance, with the
   * whole inner shape lifted by WEB_VANISH_OFFSET_Y so the web reads as
   * a tilted tube rather than concentric rings. */
  for (u8 lane = 0; lane < 16; ++lane) {
    s16 ox = web_scale(rim[lane][0]);
    s16 oy = web_scale(rim[lane][1]);
    s16 ix = ox >> 3;
    s16 iy = (s16) ((oy >> 3) + WEB_VANISH_OFFSET_Y);
    web_line((s16) (cx + ix), (s16) (cy + iy),
             (s16) (cx + ox), (s16) (cy + oy), pal);
  }

  /* 2. Outer rim polygon — connect adjacent outer points. */
  for (u8 lane = 0; lane < 16; ++lane) {
    u8 next = (u8) ((lane + 1) & 0x0F);
    s16 ax = (s16) (cx + web_scale(rim[lane][0]));
    s16 ay = (s16) (cy + web_scale(rim[lane][1]));
    s16 bx = (s16) (cx + web_scale(rim[next][0]));
    s16 by = (s16) (cy + web_scale(rim[next][1]));
    web_line(ax, ay, bx, by, pal);
  }

  /* 3. Inner rim polygon — same shape at 1/4 scale. */
  for (u8 lane = 0; lane < 16; ++lane) {
    u8 next = (u8) ((lane + 1) & 0x0F);
    s16 ax = (s16) (cx + (web_scale(rim[lane][0]) >> 3));
    s16 ay = (s16) (cy + (web_scale(rim[lane][1]) >> 3) + WEB_VANISH_OFFSET_Y);
    s16 bx = (s16) (cx + (web_scale(rim[next][0]) >> 3));
    s16 by = (s16) (cy + (web_scale(rim[next][1]) >> 3) + WEB_VANISH_OFFSET_Y);
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
   * Position interpolates between source and target lane (web_player_render_pos)
   * and the claw rotation rolls through a full revolution per lane change
   * (g_claw_render_idx, animated by web_claw_tick). */
  for (Entity * e = g_active_head; e; e = e->next) {
    if (e->type != E_PLAYER || n >= SPR_MAX) continue;
    s16 px, py;
    web_player_render_pos(e->depth_fp, &px, &py);
    SpriteSizeDef sz = { SPR_SIZE_2x2,
                         (u16) (PLAYER_TILE_BASE + g_claw_render_idx * PLAYER_TILES_PER_LANE),
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
