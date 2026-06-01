#!/usr/bin/env python3
"""Extract Tempest 2000 SFX samples → RF5C164-ready byte blobs.

The Jaguar source ships SFX as 8-bit signed PCM (sample 06 wrapped in
IFF 8SVX, samples 04/17 raw). The Mega CD's RF5C164 PCM chip wants:
  - 8-bit SIGN-MAGNITUDE encoding (bit 7 = sign, NOT 2's complement)
  - $FF byte = end-of-sample marker (so no real sample byte can be $FF)

Per pcm.c's Chilly Willy convention:
  signed >= 0: byte = magnitude | 0x80     (positive: bit 7 set)
  signed <  0: byte = -signed (clamped to 127)   (negative: bit 7 clear)

Output goes into sub/src/sfx_data.{c,h} — embedded in the sub module so
the cart doesn't need a runtime upload path.

Sample table from tempest2k-source/src/images_sounds.s:
    file 06  -> sample 5  "Player Shot Normal"   (1956 B IFF, 1856 B PCM)
    file 17  -> sample 16 "Normal Explosion"     (10934 B raw PCM)
    file 04  -> sample 3  "Player Death"         (21658 B raw PCM)
    file 08  -> sample 7  "Crackle"              (17812 B raw PCM) — superzapper

All at ~8363 Hz per the 8SVX header on sample 06; the raw samples don't
state a rate but the source plays them at the same period $01ac/$00d6
in the Jaguar DSP — close enough to treat as 8363 for now.
"""

import struct, sys
from pathlib import Path

# tempest2k-source/ is expected as a sibling of megacd-port/.
ROOT    = Path(__file__).resolve().parents[2]
JAG_SRC = ROOT / "tempest2k-source/src/sounds/samples"
OUT_C   = Path(__file__).resolve().parents[1] / "sub/src/sfx_data.c"
OUT_H   = Path(__file__).resolve().parents[1] / "sub/src/sfx_data.h"

if not JAG_SRC.is_dir():
    sys.exit(f"error: {JAG_SRC} not found.\n"
             "  Expected tempest2k-source/ next to megacd-port/.\n"
             "  Clone it: git clone https://github.com/mwenge/tempest2k")

SFX = [
    # (name, file, wrapped, max_pcm_bytes, gain)
    # `gain` pre-amplifies the signed PCM before sign-magnitude conversion
    # (so the chip sees louder bytes). 1.0 = unchanged, >1.0 with hard
    # clip at ±127. Use sparingly — the SFX bytes are added to MOD output
    # by the chip, so over-loud SFX clips the MOD too.
    ("FIRE",  "06", True,  None,  1.0),    # IFF 8SVX wrapper
    ("HIT",   "17", False, None,  1.0),    # raw PCM
    # DEATH is 21658 bytes (~1.3 s at 8363 Hz). Trim to ~15 KB (~0.9 s)
    # — the impact / burst character is in the first ~0.5 s anyway —
    # to free sub-ROM headroom for the EXCELLENT voice clip below.
    ("DEATH", "04", False, 12000, 1.0),    # raw PCM
    # Crackle is 17812 bytes (~2.1 s at 8363 Hz). Trim to ~7 KB (~0.8 s)
    # so the whole sub module still fits in MODULE_ROM_LENGTH = 0xE000.
    ("ZAP",   "08", False, 7000,  1.0),    # raw PCM — sfx 7 "Crackle" in yak.s
    # "EXCELLENT!" voice — Jag's sfx 21 (yak.s:10691, sayex routine).
    # File 22 = sample 21 per the -1 mapping convention. Played at
    # period 188 (≈ 19 kHz) on the cart — the Jag voice samples are
    # recorded at that rate; the metadata's period 512 (≈ 6927 Hz) is
    # just MOD reference notation, not the native rate.
    # 11000 bytes / 19000 Hz ≈ 0.58 s, enough for the full word.
    # Source is quietly recorded so apply 2.5× gain to cut through MOD.
    ("EXC",   "22", False, 13500, 2.5),
]


def strip_8svx(blob):
    """Find the BODY chunk and return just its data bytes."""
    body = blob.find(b'BODY')
    assert body >= 0, "no BODY chunk in 8SVX file"
    size = struct.unpack('>I', blob[body+4:body+8])[0]
    return blob[body+8:body+8+size]


def to_sign_magnitude(signed_pcm):
    """2's-complement → Chilly Willy sign-magnitude (matches pcm.c conv=1).
    Also coerces any resulting $FF byte to $FE so the chip's end-of-sample
    sentinel isn't triggered mid-sample."""
    out = bytearray(len(signed_pcm))
    for i, b in enumerate(signed_pcm):
        s = b if b < 128 else b - 256
        if s < 0:
            mag = -s
            if mag > 127: mag = 127
            v = mag                  # bit 7 clear = negative in this convention
        else:
            if s > 126: s = 126
            v = s | 0x80             # bit 7 set = positive
        if v == 0xFF: v = 0xFE
        out[i] = v
    return bytes(out)


def apply_gain(signed_pcm, factor):
    if factor == 1.0:
        return signed_pcm
    out = bytearray(len(signed_pcm))
    for i, b in enumerate(signed_pcm):
        s = b if b < 128 else b - 256
        v = int(s * factor)
        if v >  127: v =  127
        if v < -128: v = -128
        out[i] = v & 0xFF
    return bytes(out)


def main():
    blobs = []
    for name, fn, wrapped, max_bytes, gain in SFX:
        raw = (JAG_SRC / fn).read_bytes()
        pcm = strip_8svx(raw) if wrapped else raw
        if max_bytes is not None and len(pcm) > max_bytes:
            pcm = pcm[:max_bytes]
        if gain != 1.0:
            pcm = apply_gain(pcm, gain)
        # Append 32 $FF bytes — matching pcm.c's loop_markers[] convention.
        # The chip advances past a single $FF before fully halting playback,
        # which lets it skid into stale data behind the sample and produce
        # an "echo". 32 markers keep it firmly in terminator mode.
        conv = to_sign_magnitude(pcm) + b'\xff' * 32
        blobs.append((name, conv))
        g_str = f" ×{gain}" if gain != 1.0 else ""
        print(f"  {name:6s} ← samples/{fn} ({len(pcm)} PCM bytes{g_str} → {len(conv)} including 32-byte $FF tail)")

    # Header
    lines_h = [
        "// Generated by tools/extract_sfx.py — do not edit.",
        "// Tempest 2000 SFX samples, pre-converted to RF5C164 sign-magnitude",
        "// with $FF end-of-sample terminator appended.",
        "",
        "#ifndef SFX_DATA_H",
        "#define SFX_DATA_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    for name, blob in blobs:
        lines_h.append(f"extern const uint8_t SFX_{name}[{len(blob)}];")
    lines_h.append("")
    for i, (name, _) in enumerate(blobs):
        lines_h.append(f"#define SFX_IDX_{name}  {i}")
    lines_h.append("")
    lines_h.append("#endif")
    OUT_H.write_text("\n".join(lines_h) + "\n")

    # Data
    lines_c = [
        "/* Generated by tools/extract_sfx.py — do not edit. */",
        '#include "sfx_data.h"',
        "",
    ]
    for name, blob in blobs:
        lines_c.append(f"const uint8_t SFX_{name}[{len(blob)}] = {{")
        for r in range(0, len(blob), 16):
            row = ", ".join(f"0x{b:02X}" for b in blob[r:r+16])
            lines_c.append(f"  {row},")
        lines_c.append("};")
        lines_c.append("")
    OUT_C.write_text("\n".join(lines_c))

    print(f"wrote {OUT_H}")
    print(f"wrote {OUT_C}")


if __name__ == "__main__":
    main()
