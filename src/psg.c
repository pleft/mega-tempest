#include "psg.h"
#include <types.h>

/* PSG SN76489 — single write-only 8-bit port at $C00011.
 *   Latch byte: 1 cc t dddd   cc = channel 0..3, t = 0 tone / 1 vol, dddd = low 4 bits
 *   Data  byte: 0 0 dddddd   high 6 bits of a 10-bit tone period
 * Channel 3 ("tone" register) is the noise control: low 3 bits select
 *   mode (bit 2 white/periodic) + shift rate. Volume is 4-bit attenuation:
 *   0 = loud, 15 = silent. */
#define PSG_PORT ((volatile u8 *) 0xC00011)

static inline void psg_tone(u8 ch, u16 period) {
  *PSG_PORT = (u8) (0x80 | ((ch & 3) << 5) | (period & 0x0F));
  *PSG_PORT = (u8) ((period >> 4) & 0x3F);
}
static inline void psg_vol(u8 ch, u8 atten) {
  *PSG_PORT = (u8) (0x90 | ((ch & 3) << 5) | (atten & 0x0F));
}
static inline void psg_noise(u8 mode) {
  *PSG_PORT = (u8) (0xE0 | (mode & 0x07));
}

void psg_init(void) {
  psg_vol(0, 15); psg_vol(1, 15); psg_vol(2, 15); psg_vol(3, 15);
}

/* ---- SFX runtime ---------------------------------------------------------
 * One tone slot + one noise slot. A slot's lifetime is fully described by
 * its starting period/vol, per-frame deltas, and frame count. */

typedef struct {
  u8  active;
  u8  frames;        /* frames remaining */
  u16 period;        /* current PSG period (tone slot only) */
  s16 period_delta;  /* added per frame */
  u8  vol;           /* current attenuation 0..15 */
  u8  vol_delta;     /* added per frame (positive = fade out) */
} SfxSlot;

static SfxSlot g_tone;
static SfxSlot g_noise;

#define TONE_CH   0
#define NOISE_CH  3

static void tick_tone(void) {
  psg_tone(TONE_CH, g_tone.period);
  psg_vol(TONE_CH, g_tone.vol);
  if (--g_tone.frames == 0) {
    psg_vol(TONE_CH, 15);
    g_tone.active = 0;
    return;
  }
  g_tone.period = (u16) ((s16) g_tone.period + g_tone.period_delta);
  u16 nv = (u16) g_tone.vol + g_tone.vol_delta;
  g_tone.vol = (u8) (nv > 15 ? 15 : nv);
}

static void tick_noise(void) {
  psg_vol(NOISE_CH, g_noise.vol);
  if (--g_noise.frames == 0) {
    psg_vol(NOISE_CH, 15);
    g_noise.active = 0;
    return;
  }
  u16 nv = (u16) g_noise.vol + g_noise.vol_delta;
  g_noise.vol = (u8) (nv > 15 ? 15 : nv);
}

void psg_tick(void) {
  if (g_tone.active)  tick_tone();
  if (g_noise.active) tick_noise();
}

/* ---- SFX triggers --------------------------------------------------------
 * Period N → frequency = 3.58 MHz / (32 × N) ≈ 111720 / N Hz.
 *   N=100 → 1117 Hz   (highish)
 *   N=300 →  372 Hz   (low)
 *   N=600 →  186 Hz   (very low)
 * Lower period = higher pitch. */

void sfx_fire(void) {
  g_tone.period       = 300;        /* ~372 Hz */
  g_tone.period_delta = -30;        /* rising pitch */
  g_tone.vol          = 0;          /* loudest */
  g_tone.vol_delta    = 2;          /* fade out */
  g_tone.frames       = 6;
  g_tone.active       = 1;
}

void sfx_hit(void) {
  psg_noise(0x04);                  /* white noise, fast shift = brightest */
  g_noise.vol       = 0;
  g_noise.vol_delta = 2;
  g_noise.frames    = 6;
  g_noise.active    = 1;
}

void sfx_death(void) {
  /* Descending tone overlapped with a longer noise rumble. */
  g_tone.period       = 120;
  g_tone.period_delta = 25;         /* falling pitch */
  g_tone.vol          = 0;
  g_tone.vol_delta    = 1;
  g_tone.frames       = 14;
  g_tone.active       = 1;

  psg_noise(0x05);                  /* white noise, medium shift */
  g_noise.vol       = 0;
  g_noise.vol_delta  = 1;
  g_noise.frames     = 14;
  g_noise.active     = 1;
}
