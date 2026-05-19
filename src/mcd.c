#include "mcd.h"
#include "res.h"
#include <main/gate_arr.def.h>
#include <main/memmap.h>

#define GA_REG_MEMMODE_W      ((volatile u16 *) 0xA12002)
#define GA_REG_SUBCTRL_B      ((volatile u8 *)  0xA12001)
#define GA_REG_COMCMD0_W      ((volatile u16 *) 0xA12010)
#define GA_REG_COMCMD1_W      ((volatile u16 *) 0xA12012)
#define GA_REG_COMCMD2_W      ((volatile u16 *) 0xA12014)
#define GA_REG_COMSTAT0_W     ((volatile u16 *) 0xA12020)
#define PRG_RAM_WINDOW        ((volatile u16 *) 0x420000)

static void sub_request_bus(void)     { *GA_REG_SUBCTRL_B = 0x02; while (!((*GA_REG_SUBCTRL_B) & 0x02)) ; }
static void sub_release_and_run(void) { *GA_REG_SUBCTRL_B = 0x01; while (!((*GA_REG_SUBCTRL_B) & 0x01)) ; }

static void copy_words(const char * src, volatile u16 * dst, u32 size_bytes)
{
  const u16 * s = (const u16 *) src;
  u32 n = (size_bytes + 1) >> 1;
  while (n--) *dst++ = *s++;
}

u8 detect_mega_cd(void)
{
  volatile u8  * hw_ver  = (volatile u8 *)  0xA10001;
  volatile u32 * mcd_sig = (volatile u32 *) 0x400100;
  u8 disk_bit_clear = (((*hw_ver) >> 5) & 1) == 0;
  u8 sega_at_400100 = (*mcd_sig == 0x53454741);
  *GA_REG_COMCMD0_W = 0xCAFE;
  u16 rb = *GA_REG_COMCMD0_W;
  u8 gate_writable = (rb == 0xCAFE);
  *GA_REG_COMCMD0_W = 0;
  return disk_bit_clear || sega_at_400100 || gate_writable;
}

// Once at boot: load spx.smd into sub-side $10000 with reset vectors at $0.
void mcd_init(void)
{
  *GA_REG_MEMMODE_W = 0xff00;
  *GA_REG_SUBCTRL_B = 0x03;
  *GA_REG_SUBCTRL_B = 0x02;
  *GA_REG_SUBCTRL_B = 0x00;
  sub_request_bus();
  *GA_REG_MEMMODE_W = 0x0000;
  ((volatile u32 *) PRG_RAM_WINDOW)[0] = 0x00080000;  // SP
  ((volatile u32 *) PRG_RAM_WINDOW)[1] = 0x00010000;  // PC = spx main
  copy_words(res_spx.data, (volatile u16 *) 0x430000, res_spx.size);
  sub_release_and_run();
}

// Upload MOD bytes into PRG-RAM bank 3 (sub $60000), then resume sub.
void mcd_upload_mod(DataChunk const * mod)
{
  sub_request_bus();
  *GA_REG_MEMMODE_W = (3 << 6);
  copy_words(mod->data, PRG_RAM_WINDOW, mod->size);
  *GA_REG_MEMMODE_W = 0x0000;
  sub_release_and_run();
}

void mcd_play_mod(u32 size)
{
  *GA_REG_COMCMD1_W = (u16) (size >> 16);
  *GA_REG_COMCMD2_W = (u16) (size & 0xFFFF);
  *GA_REG_COMCMD0_W = CMD_PLAY_MOD;
}

void mcd_stop_mod(void)
{
  *GA_REG_COMCMD0_W = CMD_STOP_MOD;
}

void mcd_wait_ack(u16 expected)
{
  while (*GA_REG_COMSTAT0_W != expected) ;
  *GA_REG_COMCMD0_W = 0;
  while (*GA_REG_COMSTAT0_W != 0) ;
}
