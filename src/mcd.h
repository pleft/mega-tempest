// Mega CD plumbing — sub bus, MOD upload/play/stop, detection.
// Mode 1 cart talking to Sub CPU + RF5C164 via the gate-array comm regs.

#ifndef TEMPEST_MCD_H
#define TEMPEST_MCD_H

#include <types.h>
#include "res.h"                  /* DataChunk */
#include "../sub/src/shared.h"    /* CMD_PLAY_MOD / CMD_STOP_MOD */

u8   detect_mega_cd(void);
void mcd_init(void);                                /* once at boot, only if MCD present */
void mcd_upload_sfx_bank(void);                     /* once at boot, after mcd_init */
void mcd_upload_mod(DataChunk const * mod);
void mcd_play_mod(u32 size);
void mcd_stop_mod(void);
void mcd_play_sfx(u8 idx);                          /* idx = 0 FIRE / 1 HIT / 2 DEATH */
/* Fire the ASIC engine and render its output to VRAM at tile_base, painting
 * plane at `plane_vram_addr` at (plane_x, plane_y) with a 26x26 cell region.
 * plane_vram_addr is 0x2000 (plane A) or 0x4000 (plane B).
 * `warp` non-zero = Tempest perspective trace, else identity.
 * Caller must have loaded stamps via mcd_asic_load_*. */
void mcd_render_asic(u16 plane_vram_addr, u16 tile_base, u8 plane_x, u8 plane_y, u8 warp);

/* Dynamic-perspective render: tilt_x = signed pixels of horizontal shear
 * at the TOP of IMG, tilt_y = signed pixels of vertical shear at the
 * LEFT. (0, 0) = identity. Same DMA + paint as mcd_render_asic. */
void mcd_render_asic_tilt(u16 plane_vram_addr, u16 tile_base,
                          u8 plane_x, u8 plane_y, s16 tilt_x, s16 tilt_y);

/* Uniform-scale zoom — `dx` in 5.11 fixed (0x0800 = identity scale,
 * larger = zoom out / web shrinks toward source centre). Caller must
 * have loaded stamps + painted plane B for the ASIC IMG layout. */
void mcd_render_asic_scale(u16 plane_vram_addr, u16 tile_base,
                           u8 plane_x, u8 plane_y, s16 dx);
/* Generate a Tempest-styled 32x32 stamp (yellow lane outline + blue
 * lane-gradient fill) directly into WR. Fills the stamp map with that
 * single stamp repeating. Uses the game's CRAM indices so the output
 * looks right in the playfield palette. */
void mcd_asic_load_tempest_test_stamp(void);

/* Rasterize the current web shape into 16 ASIC stamps (4x4 = 128x128 px),
 * arrange them in the stamp map's top-left 4x4 cells, ready for ASIC
 * perspective render. line_pal is the palette index for the lane lines. */
void mcd_asic_load_web_stamps(u8 line_pal);

/* Pack g_web_buf (already filled by the caller — text, sprite, etc.)
 * into the 5×5 ASIC stamp area + map. Same backend as
 * mcd_asic_load_web_stamps but skipping the web render. */
void mcd_asic_pack_buf_to_stamps(void);
void mcd_asic_load_map_diagnostic(void);

/* Pre-render N web variants (one per lane) into Word RAM, each rendered
 * with that lane's camera position. Per-frame, mcd_dma_variant DMAs the
 * variant matching the player's current lane to plane B tile range. */
void mcd_prebake_web_variants(u8 pal);
void mcd_dma_variant_to_vram(u8 variant_idx);

/* DIAGNOSTIC: bypass the ASIC. Render the current web to g_web_buf,
 * copy the centre 128x128 (cells 5..20) directly into WR_IMG_BUF in
 * COL-MAJOR tile order, then DMA + col-major paint (same back-end as
 * mcd_render_asic). If this shows a connected web but the ASIC path
 * doesn't, the bug is in the ASIC engine / stamp path. If this ALSO
 * shows two halves with a gap, the bug is in g_web_buf or the
 * extraction maths. */
void mcd_render_web_direct(u8 line_pal, u16 plane_vram_addr, u16 tile_base,
                           u8 plane_x, u8 plane_y);

void mcd_wait_ack(u16 expected);

/* Backup RAM hi-score I/O. Both return 0 = success, non-zero = failure
 * (file missing on load, or BRAM unformatted / write rejected on save).
 * `len` must be ≤ 256 bytes (size of the PRG-RAM shared slot). */
u16 mcd_hiscore_load(u8 * buf, u16 len);
u16 mcd_hiscore_save(const u8 * buf, u16 len);

#endif
