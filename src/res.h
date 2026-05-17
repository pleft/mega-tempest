#ifndef RES_H
#define RES_H

#include "types.h"

typedef struct
{
  u32  size;
  char data[];
} DataChunk;

extern DataChunk const res_basic_font;
extern u16 const res_megadev_pal[16];

extern DataChunk const res_spx;
extern DataChunk const res_rave4_mod;

#endif
