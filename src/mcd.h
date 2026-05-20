// Mega CD plumbing — sub bus, MOD upload/play/stop, detection.
// Mode 1 cart talking to Sub CPU + RF5C164 via the gate-array comm regs.

#ifndef TEMPEST_MCD_H
#define TEMPEST_MCD_H

#include <types.h>
#include "res.h"                  /* DataChunk */
#include "../sub/src/shared.h"    /* CMD_PLAY_MOD / CMD_STOP_MOD */

u8   detect_mega_cd(void);
void mcd_init(void);                                /* once at boot, only if MCD present */
void mcd_upload_mod(DataChunk const * mod);
void mcd_play_mod(u32 size);
void mcd_stop_mod(void);
void mcd_play_sfx(u8 idx);                          /* idx = 0 FIRE / 1 HIT / 2 DEATH */
/* Fire the ASIC engine and render its output to VRAM at tile_base, painting
 * plane B at (plane_x, plane_y) with a 16x16 cell region. If `warp` is
 * non-zero, the sub uses a Tempest-style perspective trace; otherwise
 * identity. Caller must have called mcd_asic_load_stamps() at least once. */
void mcd_render_asic(u16 tile_base, u8 plane_x, u8 plane_y, u8 warp);
/* Copy the demo Sega-character stamps + stamp map to WR. */
void mcd_asic_load_stamps(void);

/* Generate a Tempest-styled 32x32 stamp (yellow lane outline + blue
 * lane-gradient fill) directly into WR. Fills the stamp map with that
 * single stamp repeating. Uses the game's CRAM indices so the output
 * looks right in the playfield palette. */
void mcd_asic_load_tempest_test_stamp(void);
void mcd_wait_ack(u16 expected);

#endif
