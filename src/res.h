#ifndef RES_H
#define RES_H

#include "types.h"

typedef struct
{
  u32  size;
  char data[];
} DataChunk;

extern DataChunk const res_font;
extern u16 const res_megadev_pal[16];

extern DataChunk const res_spx;
extern DataChunk const res_rave4_mod;
extern DataChunk const res_tune7_mod;
extern DataChunk const res_tune5_mod;
extern DataChunk const res_tune12_mod;
extern DataChunk const res_tune13_mod;

/* SFX byte blob — see tools/extract_sfx.py and src/sfx_data.h.
 * Uploaded to PRG-RAM at boot by mcd_upload_sfx_bank. */
extern DataChunk const res_sfx_data;

#endif
