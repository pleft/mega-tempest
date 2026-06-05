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
# Cart-side blob — the bytes the cart .incbin's into ROM and uploads
# to PRG-RAM at boot (see mcd_upload_sfx_bank). Per-SFX metadata
# header lives next to it on the cart side.
OUT_BIN = Path(__file__).resolve().parents[1] / "res/sfx_data.bin"
OUT_H   = Path(__file__).resolve().parents[1] / "src/sfx_data.h"

if not JAG_SRC.is_dir():
    sys.exit(f"error: {JAG_SRC} not found.\n"
             "  Expected tempest2k-source/ next to megacd-port/.\n"
             "  Clone it: git clone https://github.com/mwenge/tempest2k")

SFX = [
    # (name, file, wrapped, max_pcm_bytes, gain, period)
    #
    # `gain` pre-amplifies signed PCM (hard clip at ±127). Use sparingly:
    # the chip adds SFX to MOD output, so over-loud SFX clips the MOD too.
    #
    # `period` is the RF5C164 sample-rate divisor: 428 ≈ 8363 Hz (music
    # baseline); 188 ≈ 19 kHz (Jag voice samples — recorded at that rate).
    #
    # Cart-side SFX bank lives in PRG-RAM (see mcd_upload_sfx_bank) so
    # the budget is the slack region size, not MODULE_ROM. Bytes here
    # can be near-Jag-original sizes now.
    ("FIRE",  "06", True,  None,  1.0, 428),   # Player Shot Normal — IFF 8SVX wrap
    ("HIT",   "17", False, None,  1.0, 428),   # Normal Explosion — raw PCM
    ("DEATH", "04", False, None,  1.0, 428),   # Player Death — raw PCM (full Jag size now)
    ("ZAP",   "08", False, None,  1.0, 428),   # Crackle (sfx 7) — superzapper
    ("EXC",   "22", False, None,  2.5, 188),   # Excellent! voice (sfx 21) @ 19 kHz
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
    blob_bytes = bytearray()
    entries = []    # (name, offset, length, period)
    for name, fn, wrapped, max_bytes, gain, period in SFX:
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
        sfx = to_sign_magnitude(pcm) + b'\xff' * 32
        offset = len(blob_bytes)
        blob_bytes += sfx
        entries.append((name, offset, len(sfx), period))
        g_str = f" ×{gain}" if gain != 1.0 else ""
        print(f"  {name:6s} ← samples/{fn} ({len(pcm)} PCM bytes{g_str} → {len(sfx)} w/$FF tail)  "
              f"@ blob+{offset:#06x}, period {period}")

    OUT_BIN.parent.mkdir(parents=True, exist_ok=True)
    OUT_BIN.write_bytes(bytes(blob_bytes))
    print(f"\nwrote {OUT_BIN}  ({len(blob_bytes)} bytes total)")

    # Cart-side header.
    lines_h = [
        "// Generated by tools/extract_sfx.py — do not edit.",
        "//",
        "// Tempest 2000 SFX bank, pre-converted to RF5C164 sign-magnitude.",
        "// The raw bytes live in res/sfx_data.bin and are .incbin'd into the",
        "// cart ROM by res.s as res_sfx_data; at boot, mcd_upload_sfx_bank",
        "// copies them to PRG-RAM at SFX_PRG_BASE and ships SFX_BANK[] to",
        "// the sub via CMD_INIT_SFX_BANK so the sub can dereference them.",
        "",
        "#ifndef SFX_DATA_H",
        "#define SFX_DATA_H",
        "",
        "#include <types.h>",
        "",
        f"#define SFX_COUNT       {len(entries)}",
        f"#define SFX_BLOB_BYTES  {len(blob_bytes)}",
        "",
        "/* PRG-RAM addresses the cart writes to / the sub reads from.",
        " * SFX_PRG_BASE is where the byte blob starts (sub dereferences",
        " * SFX_PRG_BASE + SFX_BANK[idx].offset for sample data).",
        " * SFX_META_PRG is where the SfxEntry[] table lives (sub reads",
        " * `((const SfxEntry *) SFX_META_PRG)[idx]`). Both are in PRG-RAM",
        " * bank 0; the cart accesses them via the window at $420000 + off. */",
        "#define SFX_PRG_BASE    0x01100u",
        "#define SFX_META_PRG    0x0F000u",
        "",
        "/* Per-SFX metadata. Offsets are relative to SFX_PRG_BASE (so the",
        " * sub computes data_ptr = SFX_PRG_BASE + offset). period feeds",
        " * pcm_set_period (428 ≈ 8363 Hz, 188 ≈ 19 kHz). */",
        "typedef struct {",
        "  u16 offset;",
        "  u16 length;",
        "  u16 period;",
        "} SfxEntry;",
        "",
        "extern const SfxEntry SFX_BANK[SFX_COUNT];",
        "",
    ]
    for i, (name, _o, _l, _p) in enumerate(entries):
        lines_h.append(f"#define SFX_IDX_{name:<6}  {i}")
    lines_h.append("")
    lines_h.append("#endif")
    OUT_H.write_text("\n".join(lines_h) + "\n")
    print(f"wrote {OUT_H}")

    # Cart-side companion .c — defines the SFX_BANK array.
    out_c = OUT_H.with_suffix(".c")
    lines_c = [
        "/* Generated by tools/extract_sfx.py — do not edit. */",
        '#include "sfx_data.h"',
        "",
        "const SfxEntry SFX_BANK[SFX_COUNT] = {",
    ]
    for name, offset, length, period in entries:
        lines_c.append(f"  {{ {offset:>6}, {length:>6}, {period:>4} }},  /* {name} */")
    lines_c.append("};")
    lines_c.append("")
    out_c.write_text("\n".join(lines_c))
    print(f"wrote {out_c}")


if __name__ == "__main__":
    main()
