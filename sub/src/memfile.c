#include <stdint.h>
#include <string.h>
#include "memfile.h"

static int32_t mf_Seek(CDFileHandle_t * h, int32_t offset, int32_t whence)
{
  // 0 = SEEK_SET, 1 = SEEK_CUR, 2 = SEEK_END (POSIX).
  int32_t newpos;
  switch (whence) {
    case 0:  newpos = offset; break;
    case 1:  newpos = h->pos + offset; break;
    case 2:  newpos = h->length + offset; break;
    default: return -1;
  }
  if (newpos < 0) newpos = 0;
  if (newpos > h->length) newpos = h->length;
  h->pos = newpos;
  return 0;
}

static int32_t mf_Tell(CDFileHandle_t * h)
{
  return h->pos;
}

static int32_t mf_Read(CDFileHandle_t * h, void * ptr, int32_t size)
{
  MemFile_t * mf = (MemFile_t *) h;
  int32_t remaining = h->length - h->pos;
  if (size > remaining) size = remaining;
  if (size <= 0) return 0;
  memcpy(ptr, mf->data + h->pos, size);
  h->pos += size;
  return size;
}

static uint8_t mf_Get(CDFileHandle_t * h)
{
  MemFile_t * mf = (MemFile_t *) h;
  if (h->pos >= h->length) return 0;
  return mf->data[h->pos++];
}

static uint8_t mf_Eof(CDFileHandle_t * h)
{
  return h->pos >= h->length;
}

void memfile_init(MemFile_t * mf, const uint8_t * data, int32_t length)
{
  mf->base.Seek   = mf_Seek;
  mf->base.Tell   = mf_Tell;
  mf->base.Read   = mf_Read;
  mf->base.Get    = mf_Get;
  mf->base.Eof    = mf_Eof;
  mf->base.offset = 0;
  mf->base.length = length;
  mf->base.block  = 0;
  mf->base.pos    = 0;
  mf->data        = data;
}
