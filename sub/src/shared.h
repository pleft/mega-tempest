// Shared definitions between cart Main CPU and Sub CPU spx.
#ifndef SHARED_H
#define SHARED_H

#define CMD_PLAY_MOD   1   /* COMCMD1:COMCMD2 = MOD size in bytes; MOD bytes at PRG-RAM sub $60000 */
#define CMD_STOP_MOD   2
#define CMD_RENDER_ROT 3   /* Run the ASIC stamp/map engine, identity transform */
#define CMD_PLAY_SFX   4   /* COMCMD1 low byte = SFX index (0=FIRE, 1=HIT, 2=DEATH) */
#define CMD_RENDER_WARP 5  /* Run the ASIC stamp/map engine, perspective dx-per-line */
#define CMD_RENDER_TILT 6  /* ASIC with x/y shear; COMCMD1=tilt_x, COMCMD2=tilt_y (signed pixels, at top of IMG) */
#define CMD_RENDER_SCALE 7 /* ASIC uniform scale; COMCMD1 = dx (5.11 fixed; 0x0800 = identity, > = zoom-out) */
#define CMD_BRAM_LOAD    8 /* Read "MEGATEMPHI" from Backup RAM into PRG-RAM bank 0 offset $1000. COMSTAT1 = filesize (0xFFFF on fail). */
#define CMD_BRAM_SAVE    9 /* Write PRG-RAM bank 0 offset $1000 (COMCMD1 bytes) to "MEGATEMPHI". COMSTAT1 = 0 ok / 0xFFFF fail. */

/* Shared hi-score buffer location in PRG-RAM bank 0 (sub-side address
 * $1000; main-side via window at $421000 with bank 0 selected). 256
 * bytes — enough for the 88-byte table + headroom. */
#define BRAM_BUF_SUB_OFFSET 0x1000

#endif
