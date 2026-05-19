// PSG (SN76489) SFX engine — fire / hit / death.
//
// The PSG is on the MD side (VDP chip, port $C00011) — independent of the
// RF5C164 PCM chip the Mega CD MOD player drives, so SFX and music can play
// at the same time. Two slots run in parallel: one tone (PSG1) and one
// noise (PSG4). `psg_tick` is safe to call every frame whether or not a
// SFX is playing — it early-exits cheaply when both slots are idle.

#ifndef TEMPEST_PSG_H
#define TEMPEST_PSG_H

void psg_init(void);     /* mute all 4 channels — call once at boot */
void psg_tick(void);     /* call every VBlank */

void sfx_fire(void);     /* rising chirp on PSG1 */
void sfx_hit(void);      /* short noise burst on PSG4 */
void sfx_death(void);    /* descending pitch + noise */

#endif
