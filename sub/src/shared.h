// Shared definitions between cart Main CPU and Sub CPU spx.
#ifndef SHARED_H
#define SHARED_H

#define CMD_PLAY_MOD   1   /* COMCMD1:COMCMD2 = MOD size in bytes; MOD bytes at PRG-RAM sub $60000 */
#define CMD_STOP_MOD   2
#define CMD_RENDER_ROT 3   /* Run the ASIC stamp/map engine, identity transform */
#define CMD_PLAY_SFX   4   /* COMCMD1 low byte = SFX index (0=FIRE, 1=HIT, 2=DEATH) */
#define CMD_RENDER_WARP 5  /* Run the ASIC stamp/map engine, perspective dx-per-line */
#define CMD_RENDER_TILT 6  /* ASIC with x/y shear; COMCMD1=tilt_x, COMCMD2=tilt_y (signed pixels, at top of IMG) */

#endif
