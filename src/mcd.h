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
void mcd_wait_ack(u16 expected);

#endif
