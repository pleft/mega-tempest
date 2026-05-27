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
extern DataChunk const res_tune7_mod;
extern DataChunk const res_tune5_mod;
extern DataChunk const res_tune12_mod;
extern DataChunk const res_tune13_mod;

/* MC-T16 ASIC test resources. */
extern DataChunk const res_stamp01;
extern DataChunk const res_stamp02;
extern DataChunk const res_stamp03;
extern DataChunk const res_stamp04;
extern DataChunk const res_stamp_map;
extern u16 const res_stamps_pal[16];

#endif
