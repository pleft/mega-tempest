// libgcc-style 32x32 -> 32 unsigned multiply for the m68000.
// The CPU only has 16x16 = 32 (mulu.w), so the C compiler emits calls to
// __mulsi3 for any 32-bit multiplication.
//
// Pattern:
//   a*b mod 2^32 = al*bl + ((ah*bl + al*bh) << 16) mod 2^32
//
// Caller pushes b then a (cdecl). Result in d0.

    .text
    .align 2

    .global __mulsi3
__mulsi3:
    // m68k-elf ABI: d2-d7 are callee-saved. We use d2-d4 internally so
    // they must be saved/restored around the work. Without this, the
    // caller's d2-d7 values get clobbered and any computation downstream
    // that depended on them silently produces garbage.
    movem.l  d2-d4, -(sp)
    move.l   16(sp), d0         // d0 = a   (args now at +12 after push of 3 regs)
    move.l   20(sp), d1         // d1 = b

    move.l   d0, d2             // d2.w = al (low 16 of a)
    mulu.w   d1, d2             // d2 = al * bl  (16x16 = 32)

    move.l   d0, d3
    swap     d3                 // d3.w = ah
    mulu.w   d1, d3             // d3 = ah * bl

    swap     d1                 // d1.w = bh
    move.l   d0, d4
    mulu.w   d1, d4             // d4 = al * bh

    add.l    d4, d3             // d3 = ah*bl + al*bh (low 32; carry dropped)
    swap     d3                 // d3 = (ah*bl + al*bh) rotated 16
    clr.w    d3                 // d3 = (ah*bl + al*bh & 0xFFFF) << 16

    add.l    d3, d2             // d2 = al*bl + ((cross & 0xFFFF) << 16)
    move.l   d2, d0
    movem.l  (sp)+, d2-d4
    rts
