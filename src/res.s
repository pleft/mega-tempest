
.section .rodata
#include "macros.s"

// Font tiles uploaded to VRAM starting at glyph 0x20 (' '). Tempest font
// extracted from the Jaguar's cfont.dat / beasty3.cry — see
// tools/extract_font.py to regenerate.
FILE "tempest_font.md.chr", res_font

GLABEL res_megadev_pal
.incbin "megadev.md.pal"

// Sub CPU module (~9 KB), uploaded into PRG-RAM at boot if a Mega CD is
// attached. Drives MOD playback on the RF5C164.
FILE "spx.smd", res_spx

// Gameplay music — cycled per wave (wave_num & 3) → rave4 / tune7 /
// tune5 / tune12. Matches the Jaguar's webtunes[] set (yak.s:19152)
// at a tighter cadence (every wave, not every 32 waves).
FILE "rave4.mod",  res_rave4_mod
FILE "tune7.mod",  res_tune7_mod
FILE "tune5.mod",  res_tune5_mod
FILE "tune12.mod", res_tune12_mod

// Title-screen theme — matches the Jaguar T2K "theme tune"
// (modtable[0] = tune13.mod per tempest2k-source/src/yak.s:1018).
FILE "tune13.mod", res_tune13_mod

// ASIC test stamps (32x32, 16 colors) — backported from /asic-test/
// for MC-T16 revival. Sega character closeups from megadev transforms.
FILE "stamp01.md.chr", res_stamp01
FILE "stamp02.md.chr", res_stamp02
FILE "stamp03.md.chr", res_stamp03
FILE "stamp04.md.chr", res_stamp04
FILE "stampmap.bin",   res_stamp_map

GLABEL res_stamps_pal
.incbin "stamps.md.pal"
