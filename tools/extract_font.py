#!/usr/bin/env python3
"""Build the in-game 8x8 font from inline 6x7 chunky-italic glyph data.

Output: res/tempest_font.md.chr (96 tiles × 32 bytes), DMA'd to VRAM at
tile slot 0x20 (' ') at boot. Forward italic slant applied per row by
SLANT[]; row 7 left blank so adjacent text rows don't merge."""

from pathlib import Path

OUT_BIN = Path(__file__).resolve().parents[1] / "res/tempest_font.md.chr"

PAL = 1            # palette index for lit pixels (cram[1] = white)

# 6x7 glyph designs. '#' = lit, '.' / ' ' = transparent. Seven rows per
# glyph; the eighth (bottom) row is always blank in the tile. Cols 6/7
# of each tile are also blank for the inter-letter gap.
G = {}

def _add(ch, rows):
    assert len(rows) == 7, f"{ch!r} must have 7 rows (got {len(rows)})"
    for r in rows:
        assert len(r) == 6, f"{ch!r} row {r!r} must be 6 cols"
    G[ch] = rows


# Uppercase — chunky 2px-stroke arcade style. Verticals are 2px wide
# wherever possible, bars are 2px tall, giving letters more presence
# on a CRT and matching the bold Tempest aesthetic.
_add('A', [".####.",
           "##..##",
           "##..##",
           "######",
           "######",
           "##..##",
           "##..##"])
_add('B', ["#####.",
           "##..##",
           "##..##",
           "#####.",
           "##..##",
           "##..##",
           "#####."])
_add('C', [".####.",
           "##..##",
           "##....",
           "##....",
           "##....",
           "##..##",
           ".####."])
_add('D', ["#####.",
           "##..##",
           "##..##",
           "##..##",
           "##..##",
           "##..##",
           "#####."])
_add('E', ["######",
           "##....",
           "##....",
           "#####.",
           "##....",
           "##....",
           "######"])
_add('F', ["######",
           "##....",
           "##....",
           "#####.",
           "##....",
           "##....",
           "##...."])
_add('G', [".####.",
           "##..##",
           "##....",
           "##.###",
           "##..##",
           "##..##",
           ".####."])
_add('H', ["##..##",
           "##..##",
           "##..##",
           "######",
           "##..##",
           "##..##",
           "##..##"])
_add('I', [".####.",
           "..##..",
           "..##..",
           "..##..",
           "..##..",
           "..##..",
           ".####."])
_add('J', ["..####",
           "....##",
           "....##",
           "....##",
           "....##",
           "##..##",
           ".####."])
_add('K', ["##..##",
           "##.##.",
           "####..",
           "###...",
           "####..",
           "##.##.",
           "##..##"])
_add('L', ["##....",
           "##....",
           "##....",
           "##....",
           "##....",
           "##....",
           "######"])
_add('M', ["##..##",
           "######",
           "######",
           "##..##",
           "##..##",
           "##..##",
           "##..##"])
_add('N', ["##..##",
           "###.##",
           "######",
           "##.###",
           "##..##",
           "##..##",
           "##..##"])
_add('O', [".####.",
           "##..##",
           "##..##",
           "##..##",
           "##..##",
           "##..##",
           ".####."])
_add('P', ["#####.",
           "##..##",
           "##..##",
           "#####.",
           "##....",
           "##....",
           "##...."])
_add('Q', [".####.",
           "##..##",
           "##..##",
           "##..##",
           "##.###",
           "##..##",
           ".#####"])
_add('R', ["#####.",
           "##..##",
           "##..##",
           "#####.",
           "##.##.",
           "##..##",
           "##..##"])
_add('S', [".#####",
           "##....",
           "##....",
           ".####.",
           "....##",
           "....##",
           "#####."])
_add('T', ["######",
           "..##..",
           "..##..",
           "..##..",
           "..##..",
           "..##..",
           "..##.."])
_add('U', ["##..##",
           "##..##",
           "##..##",
           "##..##",
           "##..##",
           "##..##",
           ".####."])
_add('V', ["##..##",
           "##..##",
           "##..##",
           "##..##",
           "##..##",
           ".####.",
           "..##.."])
_add('W', ["##..##",
           "##..##",
           "##..##",
           "##..##",
           "######",
           "######",
           "##..##"])
_add('X', ["##..##",
           "##..##",
           ".####.",
           "..##..",
           ".####.",
           "##..##",
           "##..##"])
_add('Y', ["##..##",
           "##..##",
           "##..##",
           ".####.",
           "..##..",
           "..##..",
           "..##.."])
_add('Z', ["######",
           "....##",
           "...##.",
           "..##..",
           ".##...",
           "##....",
           "######"])

# Digits — matched to the chunky 2px-stroke uppercase above.
_add('0', [".####.",
           "##..##",
           "##.###",
           "######",
           "###.##",
           "##..##",
           ".####."])
_add('1', "..##.. .###.. #.##.. ..##.. ..##.. ..##.. ######".split())
_add('2', ".####. ##..## ....## ...##. ..##.. .##... ######".split())
_add('3', "#####. ....## ....## .####. ....## ....## #####.".split())
_add('4', "...##. ..###. .##.#. ##..#. ###### ....#. ....#.".split())
_add('5', "###### ##.... #####. ....## ....## ##..## .####.".split())
_add('6', ".####. ##..## ##.... #####. ##..## ##..## .####.".split())
_add('7', "###### ....## ...##. ..##.. ..##.. ..##.. ..##..".split())
_add('8', ".####. ##..## ##..## .####. ##..## ##..## .####.".split())
_add('9', ".####. ##..## ##..## .##### ....## ##..## .####.".split())

# Punctuation / common symbols we actually use in the game.
_add(' ', ["......"] * 7)
_add('.', ["......",
           "......",
           "......",
           "......",
           "......",
           "..##..",
           "..##.."])
_add(',', ["......",
           "......",
           "......",
           "......",
           "..##..",
           "..##..",
           ".##..."])
_add(':', ["......",
           "..##..",
           "..##..",
           "......",
           "..##..",
           "..##..",
           "......"])
_add(';', ["......",
           "..##..",
           "..##..",
           "......",
           "..##..",
           "..##..",
           ".##..."])
_add('!', ["..##..",
           "..##..",
           "..##..",
           "..##..",
           "..##..",
           "......",
           "..##.."])
_add('?', [".####.",
           "#....#",
           ".....#",
           "...##.",
           "..##..",
           "......",
           "..##.."])
_add('-', ["......",
           "......",
           "......",
           "######",
           "......",
           "......",
           "......"])
_add('+', ["......",
           "..##..",
           "..##..",
           "######",
           "..##..",
           "..##..",
           "......"])
_add('=', ["......",
           "......",
           "######",
           "......",
           "######",
           "......",
           "......"])
_add('/', [".....#",
           ".....#",
           "....#.",
           "...#..",
           "..#...",
           ".#....",
           "#....."])
_add('(', "...##. ..##.. .##... .##... .##... ..##.. ...##.".split())
_add(')', "..##.. ...##. ....## ....## ....## ...##. ..##..".split())
_add('<', "....#. ...#.. ..#... .#.... ..#... ...#.. ....#.".split())
_add('>', ".#.... ..#... ...#.. ....#. ...#.. ..#... .#....".split())
_add('*', "...... .##.## ..###. ###### ..###. .##.## ......".split())
_add('#', "...... .#..#. ###### .#..#. ###### .#..#. ......".split())
_add('%', "##.... ##...# ....#. ...#.. ..#... #...## ....##".split())
_add('&', "..##.. .#..#. .#..#. ..##.. .#.#.# .#..#. ..##.#".split())
_add('@', ".####. #....# #.####  #.#..# #.####  #..... .####.".split())
_add("'", "..##.. ..##.. ...#.. ...... ...... ...... ......".split())
_add('"', ".#..#. .#..#. .#..#. ...... ...... ...... ......".split())
_add('[', ".####. .#.... .#.... .#.... .#.... .#.... .####.".split())
_add(']', ".####. ....#. ....#. ....#. ....#. ....#. .####.".split())
_add('_', "...... ...... ...... ...... ...... ...... ######".split())


# Per-row right-shift for the forward italic slant. The top rows shift
# right more than the bottom, so letters lean forward (toward the next
# letter). Total extent = 6 cols of content + max shift = 8 cols, which
# exactly fills the tile — adjacent letters touch, which is fine because
# the slant itself gives visual separation between glyphs.
SLANT = [2, 2, 1, 1, 1, 0, 0]


def tile_for_glyph(rows):
    """7 rows of "######" → 32-byte 4bpp tile (8 rows × 4 bytes), with a
    forward italic slant applied (see SLANT above) and the bottom row
    forced transparent so vertically-adjacent text rows don't merge."""
    tile = bytearray(32)
    for r in range(7):
        src = rows[r]
        shift = SLANT[r]
        # Build the 8-col output row by prepending blanks for the slant.
        row = "." * shift + src                       # up to 6+2 = 8 cols
        row = (row + "........")[:8]                  # right-pad if shorter
        for c in range(0, 8, 2):
            p1 = PAL if row[c    ] == '#' else 0
            p2 = PAL if row[c + 1] == '#' else 0
            tile[r * 4 + c // 2] = (p1 << 4) | p2
    # Row 7 stays blank (intentional inter-row gap).
    return tile


def main():
    blank = bytearray(32)
    out   = bytearray()
    designed = 0
    for code in range(0x20, 0x80):
        ch = chr(code)
        if ch in G:
            out += tile_for_glyph(G[ch])
            designed += 1
        else:
            out += blank
    OUT_BIN.parent.mkdir(parents=True, exist_ok=True)
    OUT_BIN.write_bytes(bytes(out))
    print(f"wrote {OUT_BIN}  ({len(out)} bytes, {designed} designed glyphs, "
          f"{96 - designed} blank)")


if __name__ == "__main__":
    main()
