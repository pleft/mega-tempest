// Wrap a raw memory buffer in the upstream CDFileHandle_t interface so
// module.c::InitMOD can read MOD bytes from PRG-RAM (Mode 1 cart) instead
// of streaming them from a CD-ROM file handle. Avoids any patches to
// module.c — we just hand it a synthetic file handle.

#ifndef _MEMFILE_H
#define _MEMFILE_H

#include "cdfh.h"

typedef struct MemFile {
  CDFileHandle_t base;
  const uint8_t * data;
} MemFile_t;

// Initialise an existing MemFile_t over (data, length). The resulting
// `&mf.base` can be passed anywhere a CDFileHandle_t* is expected.
void memfile_init(MemFile_t * mf, const uint8_t * data, int32_t length);

#endif
