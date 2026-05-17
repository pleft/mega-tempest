// Cart-friendly Sub CPU initial program (Mode 1).
// Strips the CD-ROM init / file-load path entirely (would hang without a disc)
// and jumps directly to the spx module the Main CPU has pre-loaded into PRG-RAM
// at 0x10000.

#include <macros.s>
#include <sub/memmap.def.h>
#include <sub/sub.macro.s>

.section .text

.global sp_text_org
.equ sp_text_org, _sp_text_org

GLABEL sp_init
    CLEAR_COMM_REGS
    rts

GLABEL sp_int2
    rts

GLABEL sp_main
    jbra 0x10000

GLABEL sp_user
    rts

GLABEL sp_fatal
    move.w #0xFF, GA_REG_COMSTAT0
0:  nop
    bra 0b
