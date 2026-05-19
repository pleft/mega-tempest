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

/* Lane MIDPOINT positions — entities live in the gaps BETWEEN adjacent
 * radial lines (T2K convention). Sized to MAX_LANES (17, the largest
 * T2K web); the current shape's `g_lane_count` says how many of these
 * slots are actually live. */
static s16 g_lane_mid_outer_x[MAX_LANES];
static s16 g_lane_mid_outer_y[MAX_LANES];
static s16 g_lane_mid_inner_x[MAX_LANES];
static s16 g_lane_mid_inner_y[MAX_LANES];

/* Per-shape active lane count + open/closed flag, populated by web_init. */
static u8 g_lane_count;
static u8 g_shape_closed;

/* Which of the 16 pre-rotated claws best faces inward from each lane's
 * rim point. Filled by web_init() per shape — radial shapes (CIRCLE,
 * OCTAGON, etc.) end up with claw_idx == lane, but non-radial shapes
 * (FAN, where every lane is on a horizontal line) get a per-lane choice. */
static u8 g_player_claw_idx[MAX_LANES];

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

/* ---- Per-shape rim tables ------------------------------------------------
 * Each lane's (dx, dy) is the offset from web centre to that lane's rim,
 * at the base radius (60). web_scale() rescales to render radius. Lane
 * count per shape varies (11..17, matching T2K) — see WEB_LANE_COUNT and
 * WEB_CLOSED below. Closed shapes wrap lane N-1 → 0; open ones clamp. */

/* V / U-bowl — T2K web2 ("v"), 16 closed vertices.
 * Bounding box (1..17, 1..15), centre (9, 8), scale 7 (max abs 8). */
static const s8 WEB_RIM_V[16][2] = {
  { -56, -49 }, { -49, -35 }, { -42, -21 }, { -35,  -7 },
  { -28,   7 }, { -21,  21 }, { -14,  35 }, {  -7,  49 },
  {   7,  49 }, {  14,  35 }, {  21,  21 }, {  28,   7 },
  {  35,  -7 }, {  42, -21 }, {  49, -35 }, {  56, -49 },
};

/* Square — T2K web11, 16 closed vertices.
 * Bounding box (4..12, 4..12), centre (8, 8), scale 15 (max abs 4). */
static const s8 WEB_RIM_SQUARE[16][2] = {
  { -60, -60 }, { -60, -30 }, { -60,   0 }, { -60,  30 },
  { -60,  60 }, { -30,  60 }, {   0,  60 }, {  30,  60 },
  {  60,  60 }, {  60,  30 }, {  60,   0 }, {  60, -30 },
  {  60, -60 }, {  30, -60 }, {   0, -60 }, { -30, -60 },
};

/* Plus / Cross — T2K web10, 16 closed vertices.
 * Bounding box (3..13, 3..13), centre (8, 8), scale 12 (max abs 5). */
static const s8 WEB_RIM_PLUS[16][2] = {
  { -12, -60 }, { -12, -36 }, { -36, -12 }, { -60, -12 },
  { -60,  12 }, { -36,  12 }, { -12,  36 }, { -12,  60 },
  {  12,  60 }, {  12,  36 }, {  36,  12 }, {  60,  12 },
  {  60, -12 }, {  36, -12 }, {  12, -36 }, {  12, -60 },
};

/* Triangle — T2K web9, 18 closed vertices.
 * Bounding box (2..14, 1..13), centre (8, 7), scale 10 (max abs 6). */
static const s8 WEB_RIM_TRIANGLE[18][2] = {
  {   0, -60 }, { -10, -40 }, { -20, -20 }, { -30,   0 },
  { -40,  20 }, { -50,  40 }, { -60,  60 }, { -40,  60 },
  { -20,  60 }, {   0,  60 }, {  20,  60 }, {  40,  60 },
  {  60,  60 }, {  50,  40 }, {  40,  20 }, {  30,   0 },
  {  20, -20 }, {  10, -40 },
};

/* Pentagon — T2K web27, 15 closed vertices.
 * Bounding box (2..14, 2..14), centre (8, 8), scale 10 (max abs 6). */
static const s8 WEB_RIM_PENTAGON[15][2] = {
  {   0, -60 }, { -20, -40 }, { -40, -20 }, { -60,   0 },
  { -50,  20 }, { -40,  40 }, { -30,  60 }, { -10,  60 },
  {  10,  60 }, {  30,  60 }, {  40,  40 }, {  50,  20 },
  {  60,   0 }, {  40, -20 }, {  20, -40 },
};

/* Star — T2K web22 ("tiny star"), 12 closed vertices.
 * Bounding box (3..13, 3..13), centre (8, 8), scale 12 (max abs 5). */
static const s8 WEB_RIM_STAR[12][2] = {
  { -12, -36 }, { -36, -12 }, { -60,   0 }, { -36,  12 },
  { -12,  36 }, {   0,  60 }, {  12,  36 }, {  36,  12 },
  {  60,   0 }, {  36, -12 }, {  12, -36 }, {   0, -60 },
};

/* W — T2K web12, 15 open vertices = 14 lanes.
 * Inverted-W outline (3 upper peaks, 2 lower valleys).
 * Bounding box (-3..19, 8..16), centre (8, 12), scale 5 (max abs 11). */
static const s8 WEB_RIM_W[15][2] = {
  { -55, -20 }, { -45, -10 }, { -35,   0 }, { -25,  10 },
  { -15,  20 }, { -10,  10 }, {  -5,   0 }, {   0, -10 },
  {   5,   0 }, {  10,  10 }, {  15,  20 }, {  25,  10 },
  {  35,   0 }, {  45, -10 }, {  55, -20 },
};

/* Fan / Flat plane — T2K web1, 12 open vertices = 11 lanes.
 * Horizontal line, player walks L↔R along it without wrap. */
static const s8 WEB_RIM_FAN[12][2] = {
  { -55,  40 }, { -45,  40 }, { -35,  40 }, { -25,  40 },
  { -15,  40 }, {  -5,  40 }, {   5,  40 }, {  15,  40 },
  {  25,  40 }, {  35,  40 }, {  45,  40 }, {  55,  40 },
};

const char * const WEB_SHAPE_NAMES[WEB_SHAPE_COUNT] = {
  "V       ", "SQUARE  ", "PLUS    ", "TRIANGLE",
  "PENTAGON", "STAR    ", "W       ", "FAN     ",
};

static const s8 (* const WEB_RIMS[WEB_SHAPE_COUNT])[2] = {
  WEB_RIM_V,        WEB_RIM_SQUARE,   WEB_RIM_PLUS, WEB_RIM_TRIANGLE,
  WEB_RIM_PENTAGON, WEB_RIM_STAR,     WEB_RIM_W,    WEB_RIM_FAN,
};

/* Vertex count + closed flag per shape. Each WEB_RIMS[i] must have
 * exactly WEB_LANE_COUNT[i] entries; for closed shapes that's also the
 * lane count, for open shapes the lane count is one less. */
static const u8 WEB_LANE_COUNT[WEB_SHAPE_COUNT] = {
  16,   /* V        — web2  */
  16,   /* SQUARE   — web11 */
  16,   /* PLUS     — web10 */
  18,   /* TRIANGLE — web9  */
  15,   /* PENTAGON — web27 */
  12,   /* STAR     — web22 */
  15,   /* W        — web12 (15 verts → 14 lanes when open) */
  12,   /* FAN      — web1  (12 verts → 11 lanes when open) */
};
static const u8 WEB_CLOSED[WEB_SHAPE_COUNT] = {
  1, 1, 1, 1, 1, 1,
  0, 0,   /* W and FAN are open — player walks the line, no wrap. */
};

u8 g_web_shape = WEB_SHAPE_V;

// ---- Lane projection ------------------------------------------------------

void web_init(void)
{
  const s8 (*rim)[2] = WEB_RIMS[g_web_shape];
  g_lane_count   = WEB_LANE_COUNT[g_web_shape];
  g_shape_closed = WEB_CLOSED[g_web_shape];
  u8 const N     = g_lane_count;

  /* Compute the line endpoints (locally — only the lane midpoints
   * derived from them are stored globally). MAX_LANES is the cap. */
  s16 line_outer_x[MAX_LANES], line_outer_y[MAX_LANES];
  s16 line_inner_x[MAX_LANES], line_inner_y[MAX_LANES];
  for (u8 i = 0; i < N; ++i) {
    s16 sx = web_scale(rim[i][0]);
    s16 sy = web_scale(rim[i][1]);
    line_outer_x[i] = (s16) (WEB_CENTER_X + sx);
    line_outer_y[i] = (s16) (WEB_CENTER_Y + sy);
    line_inner_x[i] = (s16) (WEB_CENTER_X + (sx >> 3));
    line_inner_y[i] = (s16) (WEB_CENTER_Y + WEB_VANISH_OFFSET_Y + (sy >> 3));
  }

  /* Lane midpoints = average of adjacent line endpoints. For closed
   * shapes lane k sits between line k and line (k+1) mod N. For open
   * shapes (FAN) only lanes 0..N-2 exist, between consecutive lines. */
  u8 lane_segments = (u8) (g_shape_closed ? N : N - 1);
  for (u8 i = 0; i < lane_segments; ++i) {
    u8 j = (u8) (i + 1);
    if (j >= N) j = 0;
    g_lane_mid_outer_x[i] = (s16) ((line_outer_x[i] + line_outer_x[j]) >> 1);
    g_lane_mid_outer_y[i] = (s16) ((line_outer_y[i] + line_outer_y[j]) >> 1);
    g_lane_mid_inner_x[i] = (s16) ((line_inner_x[i] + line_inner_x[j]) >> 1);
    g_lane_mid_inner_y[i] = (s16) ((line_inner_y[i] + line_inner_y[j]) >> 1);
  }
  /* For open shapes, g_lane_count is still N (vertex count) but only
   * N-1 lanes are real. Player_lane will be clamped to 0..N-2. */
  if (!g_shape_closed) g_lane_count = lane_segments;

  /* Pick the claw rotation that best points from each lane midpoint
   * toward the web's centre via dot-product over the 16 claw orientations. */
  for (u8 i = 0; i < g_lane_count; ++i) {
    s16 tx = (s16) (WEB_CENTER_X - g_lane_mid_outer_x[i]);
    s16 ty = (s16) (WEB_CENTER_Y - g_lane_mid_outer_y[i]);
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

// ---- Lane arithmetic helpers --------------------------------------------

u8 web_lane_count(void) { return g_lane_count; }

u8 web_lane_left(u8 current)
{
  if (current > 0) return (u8) (current - 1);
  /* current == 0: wrap to last lane if closed, else stay. */
  return g_shape_closed ? (u8) (g_lane_count - 1) : 0;
}

u8 web_lane_right(u8 current)
{
  u8 next = (u8) (current + 1);
  if (next < g_lane_count) return next;
  /* past last lane: wrap to 0 if closed, else stay at last. */
  return g_shape_closed ? 0 : (u8) (g_lane_count - 1);
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

/* Interpolate between the lane's inner midpoint (depth=0) and its outer
 * midpoint (depth=FP_ONE). Entities live BETWEEN adjacent radial lines,
 * matching T2K's lane geometry — the lines are visual boundaries, not
 * paths. */
s16 web_pixel_x(u8 lane, fp16 depth_fp)
{
  s32 dx = (s32) g_lane_mid_outer_x[lane] - g_lane_mid_inner_x[lane];
  return (s16) (g_lane_mid_inner_x[lane] + ((dx * depth_fp) >> 16));
}
s16 web_pixel_y(u8 lane, fp16 depth_fp)
{
  s32 dy = (s32) g_lane_mid_outer_y[lane] - g_lane_mid_inner_y[lane];
  return (s16) (g_lane_mid_inner_y[lane] + ((dy * depth_fp) >> 16));
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

/* 32/16 → 16 signed divide via 68000 divs.w — keeps us off __divsi3. */
static inline s16 div_s32_s16(s32 num, s16 den)
{
  s32 t = num;
  asm ("divs.w %1, %0" : "+d"(t) : "d"(den) : "cc");
  return (s16) t;
}

/* Compute x where a line from (px0,py0) to (px1,py1) crosses horizontal
 * row y. Returns false if y is outside [min(py0,py1), max(py0,py1)). */
static u8 edge_x_at_y(s16 px0, s16 py0, s16 px1, s16 py1, s16 y, s16 * out_x)
{
  s16 ymin = py0 < py1 ? py0 : py1;
  s16 ymax = py0 < py1 ? py1 : py0;
  if (y < ymin || y >= ymax) return 0;
  s32 num = (s32) (px1 - px0) * (s32) (y - py0);
  s16 dy = (s16) (py1 - py0);
  *out_x = (s16) (px0 + div_s32_s16(num, dy));
  return 1;
}

/* Scanline-fill a convex quadrilateral with the given palette index.
 * Vertices listed in order (CW or CCW), all 4 edges considered per row.
 * Used to paint each lane segment between adjacent radial lines. */
static void web_fill_quad(s16 x0, s16 y0, s16 x1, s16 y1,
                          s16 x2, s16 y2, s16 x3, s16 y3, u8 pal)
{
  s16 ymin = y0;
  if (y1 < ymin) ymin = y1;
  if (y2 < ymin) ymin = y2;
  if (y3 < ymin) ymin = y3;
  s16 ymax = y0;
  if (y1 > ymax) ymax = y1;
  if (y2 > ymax) ymax = y2;
  if (y3 > ymax) ymax = y3;
  if (ymin < 0) ymin = 0;
  if (ymax >= WEB_IMG_H) ymax = WEB_IMG_H - 1;

  for (s16 y = ymin; y <= ymax; ++y) {
    s16 xs[4];
    u8 n = 0;
    if (edge_x_at_y(x0, y0, x1, y1, y, &xs[n])) n++;
    if (edge_x_at_y(x1, y1, x2, y2, y, &xs[n])) n++;
    if (edge_x_at_y(x2, y2, x3, y3, y, &xs[n])) n++;
    if (edge_x_at_y(x3, y3, x0, y0, y, &xs[n])) n++;
    if (n < 2) continue;
    s16 lx = xs[0], rx = xs[0];
    for (u8 i = 1; i < n; ++i) {
      if (xs[i] < lx) lx = xs[i];
      if (xs[i] > rx) rx = xs[i];
    }
    if (lx < 0) lx = 0;
    if (rx >= WEB_IMG_W) rx = WEB_IMG_W - 1;
    for (s16 x = lx; x <= rx; ++x) web_setpx(x, y, pal);
  }
}

/* Lane fill = 4-band gradient from deep (near vanishing point, inner) to
 * bright (near rim, outer). Palette indices 5..8 are set up in main.c
 * to step from dark navy → medium → brighter → brightest blue/purple. */
#define WEB_FILL_BANDS  4
#define WEB_FILL_PAL_0  5      /* darkest, at depth 0..1/4   (innermost) */
#define WEB_FILL_PAL_1  6
#define WEB_FILL_PAL_2  7
#define WEB_FILL_PAL_3  8      /* brightest, at depth 3/4..1 (outer rim) */

void web_render_main(u8 pal)
{
  const s8 (*rim)[2] = WEB_RIMS[g_web_shape];
  u8 const V = WEB_LANE_COUNT[g_web_shape];        /* vertex count = N */
  u8 const S = g_shape_closed ? V : (u8) (V - 1);  /* number of lane segments */
  for (u16 i = 0; i < WEB_BUF_BYTES; ++i) g_web_buf[i] = 0;
  s16 const cx = WEB_IMG_W / 2;
  s16 const cy = WEB_IMG_H / 2;

  /* Precompute line-endpoint positions for all V boundary lines, both
   * the inner (vanishing-end) and outer (rim) corners. The inner end is
   * at 1/8 scale and lifted by WEB_VANISH_OFFSET_Y for the perspective. */
  s16 ox[MAX_LANES], oy[MAX_LANES], ix[MAX_LANES], iy[MAX_LANES];
  for (u8 k = 0; k < V; ++k) {
    s16 sx = web_scale(rim[k][0]);
    s16 sy = web_scale(rim[k][1]);
    ox[k] = (s16) (cx + sx);
    oy[k] = (s16) (cy + sy);
    ix[k] = (s16) (cx + (sx >> 3));
    iy[k] = (s16) (cy + (sy >> 3) + WEB_VANISH_OFFSET_Y);
  }

  /* 1. FILL each lane segment with a 4-band depth gradient. */
  static const u8 BAND_PAL[WEB_FILL_BANDS] = {
    WEB_FILL_PAL_0, WEB_FILL_PAL_1, WEB_FILL_PAL_2, WEB_FILL_PAL_3,
  };
  for (u8 k = 0; k < S; ++k) {
    u8 j = (u8) (k + 1);
    if (j >= V) j = 0;
    s16 dxk = (s16) (ox[k] - ix[k]);
    s16 dyk = (s16) (oy[k] - iy[k]);
    s16 dxj = (s16) (ox[j] - ix[j]);
    s16 dyj = (s16) (oy[j] - iy[j]);
    for (u8 b = 0; b < WEB_FILL_BANDS; ++b) {
      s16 lo = (s16) (b << 2);
      s16 hi = (s16) ((b + 1) << 2);
      s16 x_k_lo = (s16) (ix[k] + ((dxk * lo) >> 4));
      s16 y_k_lo = (s16) (iy[k] + ((dyk * lo) >> 4));
      s16 x_k_hi = (s16) (ix[k] + ((dxk * hi) >> 4));
      s16 y_k_hi = (s16) (iy[k] + ((dyk * hi) >> 4));
      s16 x_j_lo = (s16) (ix[j] + ((dxj * lo) >> 4));
      s16 y_j_lo = (s16) (iy[j] + ((dyj * lo) >> 4));
      s16 x_j_hi = (s16) (ix[j] + ((dxj * hi) >> 4));
      s16 y_j_hi = (s16) (iy[j] + ((dyj * hi) >> 4));
      web_fill_quad(x_k_lo, y_k_lo,
                    x_k_hi, y_k_hi,
                    x_j_hi, y_j_hi,
                    x_j_lo, y_j_lo, BAND_PAL[b]);
    }
  }

  /* 2. Radial lines (one per vertex). */
  for (u8 k = 0; k < V; ++k)
    web_line(ix[k], iy[k], ox[k], oy[k], pal);

  /* 3. Outer rim polygon — only S segments (no wrap on open shapes). */
  for (u8 k = 0; k < S; ++k) {
    u8 j = (u8) (k + 1);
    if (j >= V) j = 0;
    web_line(ox[k], oy[k], ox[j], oy[j], pal);
  }

  /* 4. Inner rim polygon — same N-1 / N. */
  for (u8 k = 0; k < S; ++k) {
    u8 j = (u8) (k + 1);
    if (j >= V) j = 0;
    web_line(ix[k], iy[k], ix[j], iy[j], pal);
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
