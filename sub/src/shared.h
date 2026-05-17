// Shared definitions between cart Main CPU and Sub CPU spx.
#ifndef SHARED_H
#define SHARED_H

#define CMD_PLAY_MOD   1   /* COMCMD1:COMCMD2 = MOD size in bytes; MOD bytes at PRG-RAM sub $60000 */
#define CMD_STOP_MOD   2
#define CMD_RENDER_ROT 3   /* Run the ASIC stamp/map rotation engine and hand WR to main */

#endif
