
.section .rodata
#include "macros.s"

FILE "basic_font.md.chr", res_basic_font

GLABEL res_megadev_pal
.incbin "megadev.md.pal"

// Sub CPU module (~9 KB), uploaded into PRG-RAM at boot if a Mega CD is
// attached. Drives MOD playback on the RF5C164.
FILE "spx.smd", res_spx

// In-game music. For MC-T2 we just ship one track and play it on
// PLAYFIELD entry; later we can sequence per-level music.
FILE "rave4.mod", res_rave4_mod

// ASIC test stamps (32x32, 16 colors) — backported from /asic-test/
// for MC-T16 revival. Sega character closeups from megadev transforms.
FILE "stamp01.md.chr", res_stamp01
FILE "stamp02.md.chr", res_stamp02
FILE "stamp03.md.chr", res_stamp03
FILE "stamp04.md.chr", res_stamp04
FILE "stampmap.bin",   res_stamp_map

GLABEL res_stamps_pal
.incbin "stamps.md.pal"
