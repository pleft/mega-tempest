#!/usr/bin/env python3
"""Extract Tempest 2000's small font (`cfont`) from the mwenge source tree
into a raw 4bpp tile blob suitable for megadev's vdp_dma_transfer.

Adapted from /tools/extract_font.py (the SGDK version) — same parsing of
cfont.dat + the beasty3.cry atlas, but emits a flat binary instead of a
C array so we can drop it straight into res/ alongside the other
`*.md.chr` font assets and load it via the existing `FILE` macro.

Output: res/tempest_font.md.chr — 96 tiles × 32 bytes = 3072 bytes,
covering ASCII 32..127. Lit pixels are palette index 1 (our text white,
cram[1]); we don't carry the original's dim/bright distinction because
all our nearby palette slots are already claimed by the web fill.
"""

import re, sys
from pathlib import Path

ROOT       = Path(__file__).resolve().parents[2]
CFONT_PATH = ROOT / "tempest2k-source/src/dat/cfont.dat"
ATLAS_PATH = ROOT / "tempest2k-source/src/images/beasty3.cry"
OUT_BIN    = Path(__file__).resolve().parents[1] / "res/tempest_font.md.chr"

ATLAS_W      = 320
NUM_CHARS    = 96    # ASCII 32..127
GLYPH_W      = 8
GLYPH_H      = 8
RIGHT_MARGIN = 2     # keep 2 px of horizontal breathing room (Jag uses 11 px
                     # stride for 8 px glyphs; the 3 px gap is what makes the
                     # font readable. We can't widen tiles but we can drop the
                     # rightmost 2 cols, losing 1 px of letter for 2 px of gap.)
LIT_THRESH   = 0x80  # Y-channel intensity above which a pixel is "lit".
                     # 0x40 (SGDK's "dim") catches the anti-alias halo and
                     # blobs all uppercase letters into the same shape.
                     # 0xA0 ("bright") drops too many letter strokes,
                     # leaving holes. 0x80 is the sweet spot.
PAL_LIT      = 1     # cram[1] = white in main.c

for p in (CFONT_PATH, ATLAS_PATH):
    if not p.is_file():
        sys.exit(f"error: {p} not found.\n"
                 "  Expected tempest2k-source/ next to megacd-port/.\n"
                 "  Clone it: git clone https://github.com/mwenge/tempest2k")


def parse_cfont():
    """cfont.dat layout: dc.l pic ; dc.l $00080008 ; dc.l <descriptor> × N.
    First dc.l is an unresolved assembler symbol; only $-prefixed entries
    parse, so we end up with [size_header, descriptors...]. Each descriptor
    is the Jaguar blitter's A1_PIXEL: top u16 = pixel Y, low u16 = pixel X
    into the atlas. """
    text = CFONT_PATH.read_text()
    descs = [int(m.group(1), 16) for m in re.finditer(r'dc\.l\s+\$([0-9a-fA-F]+)', text)]
    if len(descs) < 1 + NUM_CHARS:
        sys.exit(f"only {len(descs)} $-prefixed dc.l entries in cfont.dat")
    assert descs[0] == 0x00080008, f"unexpected glyph size header {descs[0]:#x}"
    return descs[1:1 + NUM_CHARS]


def extract_glyph(atlas, x, y):
    """Read GLYPH_H rows × GLYPH_W cols of Y-channel bytes from the
    CRY16 atlas at (x, y). CRY pixels are 2 bytes: byte 0 = CR, byte 1 = Y."""
    out = []
    for r in range(GLYPH_H):
        row = []
        for c in range(GLYPH_W):
            off = ((y + r) * ATLAS_W + (x + c)) * 2
            row.append(atlas[off + 1])
        out.append(row)
    return out


def glyph_to_tile(intensities):
    """8x8 4bpp tile = 32 bytes, 8 rows × 4 bytes, each byte = two
    high-nibble-first 4bpp pixels. Lit = PAL_LIT, else palette 0
    (transparent)."""
    tile = bytearray(32)
    for r in range(GLYPH_H):
        for c in range(0, GLYPH_W, 2):
            p1 = PAL_LIT if (c     < GLYPH_W - RIGHT_MARGIN and
                             intensities[r][c]     >= LIT_THRESH) else 0
            p2 = PAL_LIT if (c + 1 < GLYPH_W - RIGHT_MARGIN and
                             intensities[r][c + 1] >= LIT_THRESH) else 0
            tile[r * 4 + c // 2] = (p1 << 4) | p2
    return tile


def main():
    atlas = ATLAS_PATH.read_bytes()
    if len(atlas) != ATLAS_W * 200 * 2:
        sys.exit(f"unexpected atlas size {len(atlas)} (expected {ATLAS_W*200*2})")
    descriptors = parse_cfont()

    # cfont uses a "default unused" sentinel descriptor for slots that
    # aren't real glyphs. That sentinel points at a noisy spot of the
    # atlas which decodes as garbage — treat any sentinel or off-atlas
    # descriptor as a blank glyph.
    UNUSED_DESC = 0xb50137
    blob = bytearray()
    blanks = 0
    for desc in descriptors:
        y = (desc >> 16) & 0xFFFF
        x = desc & 0xFFFF
        if desc == UNUSED_DESC or y + GLYPH_H > 200 or x + GLYPH_W > ATLAS_W:
            pix = [[0] * GLYPH_W for _ in range(GLYPH_H)]
            blanks += 1
        else:
            pix = extract_glyph(atlas, x, y)
        blob.extend(glyph_to_tile(pix))

    OUT_BIN.parent.mkdir(parents=True, exist_ok=True)
    OUT_BIN.write_bytes(bytes(blob))
    print(f"wrote {OUT_BIN}  ({len(blob)} bytes, {NUM_CHARS} glyphs, {blanks} blank)")


if __name__ == "__main__":
    main()
