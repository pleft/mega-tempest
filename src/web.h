// Web rendering + VDP hardware-sprite enemy tile data.
// Main-CPU pipeline (MC-T5): rasterize lines into a main-RAM tile buffer,
// DMA the buffer to VRAM, paint plane B with that tile range. Sprite tile
// data for enemies is generated procedurally at scene install and DMA'd
// to a separate VRAM region (above the web tile range — see MC-T6c).

#ifndef TEMPEST_WEB_H
#define TEMPEST_WEB_H

#include <types.h>
#include "entity.h"

/* MAX_LANES is the upper bound for storage; the actual number of lanes
 * per web shape comes from its own definition. T2K's triangle (web9)
 * has 18 vertices so MAX_LANES must be at least 18. */
#define MAX_LANES      18
#define NUM_LANES      16          /* legacy alias for code still hard-coded */
#define WEB_CENTER_X  160
#define WEB_CENTER_Y  112

/* Helpers for moving between lanes — handle modular arithmetic + the
 * open/closed distinction (closed shapes wrap, open shapes clamp). */
u8 web_lane_count(void);
u8 web_lane_left(u8 current);
u8 web_lane_right(u8 current);

/* 8 web shapes, all extracted from Tempest 2000's yak.s. Each maps to
 * a specific T2K web by polygon + lane count. Slot order is whatever
 * felt like a sensible "simple → exotic" progression for cycling on
 * the title screen. */
typedef enum {
  WEB_SHAPE_V        = 0,    /* T2K web2  — V/U-bowl, 16 closed lanes  */
  WEB_SHAPE_SQUARE,          /* T2K web11 — square, 16 closed lanes    */
  WEB_SHAPE_PLUS,            /* T2K web10 — cross, 16 closed lanes     */
  WEB_SHAPE_TRIANGLE,        /* T2K web9  — triangle, 18 closed lanes  */
  WEB_SHAPE_PENTAGON,        /* T2K web27 — pentagon, 15 closed lanes  */
  WEB_SHAPE_STAR,            /* T2K web22 — 6-point star, 12 closed    */
  WEB_SHAPE_W,               /* T2K web12 — W, 14 open lanes           */
  WEB_SHAPE_FAN,             /* T2K web1  — flat plane, 11 open lanes  */
  WEB_SHAPE_COUNT,
} WebShape;

extern u8 g_web_shape;
extern const char * const WEB_SHAPE_NAMES[WEB_SHAPE_COUNT];

/* Build rim-point table for the current shape. Call before web_pixel_*.
 * Resets claw animation state — call ONCE per scene install. */
void web_init(void);

/* Per-frame rim projection. Reads g_vp_x / g_vp_y and recomputes
 * g_lane_mid_outer/inner. Doesn't touch claw animation. */
void web_project(void);

/* Project a lane + depth_fp onto screen pixel coords. */
s16 web_pixel_x(u8 lane, fp16 depth_fp);
s16 web_pixel_y(u8 lane, fp16 depth_fp);

/* Rasterize current shape's web into the main-RAM tile buffer (`pal` is the
 * 4-bit palette index used for the lines), then DMA it to VRAM, then paint
 * plane B's tilemap entries to reference the web tiles. */
void web_render_main(u8 pal);
void web_dma_main_to_vram(void);
void web_paint_plane_b(void);
void web_clear_plane_b(void);

/* ASIC-side variant: render the current web at 128x128 scale into the
 * given buffer (must be at least 128*128/2 = 8192 bytes, in standard
 * VDP 8x8 tile format = 16x16 cells x 32 bytes each).
 * The web is rendered with proportional vanish offset so it fits the
 * 128x128 without clipping. */
void web_render_for_asic(u8 * buf, u8 pal);

/* Exposed for the ASIC stamp packer in mcd.c via getter — keep array
 * static internally to match MC-T15 exact linkage. */
#define WEB_BUF_CELLS 26
u8 * web_get_buf(void);

/* Generate + DMA all sprite tile data (player, shots, enemies) to VRAM.
 * Called once per scene install. */
void load_sprite_tiles_to_vram(void);

/* True-3D camera, used by web_init for rim projection and by
 * web_render_main when rasterising. Set per frame before calling
 * web_init + web_render_main.
 *
 *   screen_outer = (world - vp) / Z_NEAR + centre
 *   screen_inner = (world - vp) / Z_FAR  + centre   (Z_FAR = 8 * Z_NEAR)
 *
 * Outer rim shifts by full vp, inner rim shifts by vp/8 — matches T2K
 * Jaguar's `polyo2d` per-vertex projection. */
extern s16 g_vp_x;
extern s16 g_vp_y;

void render_sprites(void);

/* Rolling-claw animation + position slide. Player has 16 pre-rotated
 * claw sprites; on a lane change the claw rolls a full revolution AND
 * slides smoothly from the old rim point to the new one. Call
 * `web_lane_changed(new_lane, dir)` whenever the player switches lane
 * (dir = +1 for CCW visual rotation, -1 for CW). Call
 * `web_claw_tick(lane)` every frame to advance both animations.
 * `web_player_snap_to(lane)` skips the animation and parks the claw at
 * the given lane immediately — used on respawn after death. */
void web_lane_changed(u8 new_lane, s8 dir);
void web_claw_tick(u8 current_lane);
void web_player_snap_to(u8 lane);

#endif
