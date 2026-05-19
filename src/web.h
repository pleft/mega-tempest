// Web rendering + VDP hardware-sprite enemy tile data.
// Main-CPU pipeline (MC-T5): rasterize lines into a main-RAM tile buffer,
// DMA the buffer to VRAM, paint plane B with that tile range. Sprite tile
// data for enemies is generated procedurally at scene install and DMA'd
// to a separate VRAM region (above the web tile range — see MC-T6c).

#ifndef TEMPEST_WEB_H
#define TEMPEST_WEB_H

#include <types.h>
#include "entity.h"

#define NUM_LANES      16
#define WEB_CENTER_X  160
#define WEB_CENTER_Y  112

typedef enum {
  WEB_SHAPE_CIRCLE = 0,
  WEB_SHAPE_SQUARE,
  WEB_SHAPE_PLUS,
  WEB_SHAPE_DIAMOND,
  WEB_SHAPE_TRIANGLE,
  WEB_SHAPE_OCTAGON,
  WEB_SHAPE_STAR,
  WEB_SHAPE_FAN,
  WEB_SHAPE_COUNT,
} WebShape;

extern u8 g_web_shape;
extern const char * const WEB_SHAPE_NAMES[WEB_SHAPE_COUNT];

/* Build rim-point table for the current shape. Call before web_pixel_*. */
void web_init(void);

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

/* Generate + DMA enemy sprite tile data to VRAM (once per scene install). */
void load_enemy_sprites_to_vram(void);

/* Walk the entity active-list and write the VDP sprite attribute table.
 * Called once per frame; hides sprite 0 when there are no flippers. */
void render_enemy_sprites(void);

#endif
