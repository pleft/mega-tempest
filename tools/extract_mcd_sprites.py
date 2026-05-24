#!/usr/bin/env python3
"""Extract Tempest entity polygons → megacd-port sprite tile data.

Sister of ../tools/extract_obj2d.py (which targets md-port / SGDK). This
one writes raw byte arrays straight into src/sprites.c suitable for
megadev's vdp_dma_transfer, and lets each sprite force a single palette
colour for ALL its faces (megacd-port has a 4-colour palette — yellow web,
red enemies, white text/shots — so the Jaguar's face-by-face shading
doesn't translate, and we paint each entity in one chosen colour).

Run from the megacd-port directory:
    python3 tools/extract_mcd_sprites.py
"""

import math, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OBJ2D = ROOT / "tempest2k-source/src/obj2d.s"
OUT_C = Path(__file__).resolve().parents[1] / "src/sprites.c"
OUT_H = Path(__file__).resolve().parents[1] / "src/sprites.h"

# (c_name, sprite_label, verts_label_or_None, rotation_rad, size_tiles_per_side, palette_idx, scale)
# palette_idx maps to megacd-port main.c cram[]: 1=white, 2=red, 4=yellow.
# scale shrinks the polygon around its centroid before rasterising, so the
# same polygon can yield "far / mid / near" tile variants for depth-based
# sprite scaling (3 tiers, all 8x8 = 1x1 sprite, just less filled when far).
PAL_WHITE  = 1
PAL_RED    = 2
PAL_PINK   = 3
PAL_YELLOW = 4
PAL_CYAN   = 9     # set in main.c cram[9] = 0x0EE0 — pulsar

# Flipper tile pack: 3 size tiers × 4 rotation frames (0°, 90°, 180°, 270°).
# All output as 1x1 sprites; tier 0 = small dot (far), tier 2 = full 8x8 (near).
FLIPPER_FRAMES = []
# Angles deliberately span 0..3π/4 (no 180°) — the flipper polygon is
# 180°-symmetric, so 0° and 180° produce identical downsampled tiles.
# 0°, 45°, 90°, 135° all yield visually distinct frames.
for tier, scale in [(0, 0.45), (1, 0.70), (2, 1.0)]:
    for f, ang in enumerate([0.0, math.pi/4, math.pi/2, 3*math.pi/4]):
        FLIPPER_FRAMES.append((f"flipper_t{tier}_f{f}", "s_flipper", "fverts", ang, 1, PAL_RED, scale))

# Tanker tile pack: 3 size tiers × 1 frame = 3 tiles. The diamond
# polygon (s_fliptank) is visually distinct from the flipper X; no
# rotation needed (4-fold symmetric anyway).
TANKER_FRAMES = [
    (f"tanker_t{tier}", "s_fliptank", "ftankverts", 0.0, 1, PAL_PINK, scale)
    for tier, scale in [(0, 0.45), (1, 0.70), (2, 1.0)]
]

# Pulsar tile pack: 3 size tiers × 3 unique animation frames = 9 tiles.
# Frames sampled at spuls1/3/5 (the original Jag pulse animation has 6
# frames, but the in-between ones are visually similar — picking every
# other one gives the same readability for half the VRAM cost).
PULSAR_FRAMES = []
for tier, scale in [(0, 0.45), (1, 0.70), (2, 1.0)]:
    for f, label in enumerate(["spuls1", "spuls3", "spuls5"]):
        verts_label = {"spuls1": "spv1", "spuls3": "spv3", "spuls5": "spv5"}[label]
        PULSAR_FRAMES.append(
            (f"pulsar_t{tier}_f{f}", label, verts_label, 0.0, 1, PAL_CYAN, scale))

SPRITES = FLIPPER_FRAMES + TANKER_FRAMES + PULSAR_FRAMES + [
    ("shot",      "pshot",     "pshotverts", 0.0, 1, PAL_WHITE, 1.0),
] + [
    # 16 lane-specific claw rotations. `-lane * π/8` in y-down screen coords
    # rotates the claw visually CW as the player walks CW around the rim,
    # so the claw's open end always faces the centre (where enemies appear).
    (f"player_l{i:02d}", "sclaw4", None, -i * math.pi / 8, 2, PAL_YELLOW, 1.0)
    for i in range(16)
]


# ---- obj2d.s parsing (lifted from tools/extract_obj2d.py) -----------------

def _parse_num(s):
    s = s.strip()
    if not s: return None
    return int(s[1:], 16) if s.startswith('$') else int(s)

def find_label_block(text, label):
    m = re.search(rf'^{re.escape(label)}:\s*(.*?)(?=^[a-zA-Z_][a-zA-Z0-9_]*:|\Z)',
                  text, re.MULTILINE | re.DOTALL)
    if not m: sys.exit(f"label {label!r} not found in obj2d.s")
    return m.group(1)

def parse_sprite(block):
    """Tokenise dc.l/dc.w, parse faces (8 tokens each: color + 3×(idx,intensity) + 0)
    and any inline vertex table after the faces."""
    tokens = []
    face_count = None
    for raw in block.splitlines():
        clean = raw.split(';', 1)[0]
        m = re.match(r'\s*dc\.l\s+(\S+)', clean)
        if m and face_count is None:
            face_count = _parse_num(m.group(1)); continue
        m = re.match(r'\s*dc\.w\s+(.+)', clean)
        if m:
            for tok in m.group(1).split(','):
                v = _parse_num(tok)
                if v is not None: tokens.append(v)
    if face_count is None: sys.exit("missing dc.l face count")
    FT = 8
    faces, i = [], 0
    for _ in range(face_count):
        if i + FT > len(tokens): sys.exit("ran out of tokens parsing faces")
        if tokens[i + 7] != 0: sys.exit(f"face terminator missing at i={i}")
        faces.append({'verts': [(tokens[i+1+2*k], tokens[i+2+2*k]) for k in range(3)]})
        i += FT
    rem = tokens[i:]
    if len(rem) % 2: rem = rem[:-1]
    return faces, [(rem[k], rem[k+1]) for k in range(0, len(rem), 2)]

def parse_verts(text, label):
    block = find_label_block(text, label)
    nums = []
    for line in block.splitlines():
        clean = line.split(';', 1)[0]
        m = re.match(r'\s*dc\.w\s+(.+)', clean)
        if m:
            for tok in m.group(1).split(','):
                v = _parse_num(tok)
                if v is not None: nums.append(v)
    if len(nums) % 2: nums = nums[:-1]
    return [(nums[i], nums[i+1]) for i in range(0, len(nums), 2)]


# ---- Polygon → 19×19 pixel buffer -----------------------------------------

def rasterize_triangle(buf, w, h, p0, p1, p2, color):
    pts = sorted([p0, p1, p2], key=lambda p: p[1])
    (x0, y0), (x1, y1), (x2, y2) = pts
    def edge(y0, y1, x0, x1):
        if y1 == y0: return []
        return [(x0 + (x1 - x0) * (y - y0) / (y1 - y0)) for y in range(y0, y1)]
    long_edge = edge(y0, y2, x0, x2)
    short_edge = edge(y0, y1, x0, x1) + edge(y1, y2, x1, x2)
    for i, y in enumerate(range(y0, y2)):
        if 0 <= y < h:
            xa, xb = long_edge[i], short_edge[i]
            if xa > xb: xa, xb = xb, xa
            for x in range(int(round(xa)), int(round(xb)) + 1):
                if 0 <= x < w: buf[y * w + x] = color

def render_polygon(faces, verts, color, w=19, h=19):
    buf = [0] * (w * h)
    for f in faces:
        if len(f['verts']) < 3: continue
        idxs = [v[0] for v in f['verts']]
        for tri in range(len(idxs) - 2):
            p0 = verts[idxs[0]]
            p1 = verts[idxs[tri + 1]]
            p2 = verts[idxs[tri + 2]]
            rasterize_triangle(buf, w, h, p0, p1, p2, color)
    return buf

def rotate_verts(verts, angle):
    if angle == 0.0 or not verts: return verts
    xs, ys = [v[0] for v in verts], [v[1] for v in verts]
    cx, cy = (min(xs)+max(xs))/2.0, (min(ys)+max(ys))/2.0
    c, s = math.cos(angle), math.sin(angle)
    return [(int(round(cx + (x-cx)*c - (y-cy)*s)),
             int(round(cy + (x-cx)*s + (y-cy)*c))) for x, y in verts]

def scale_verts(verts, scale):
    if scale == 1.0 or not verts: return verts
    xs, ys = [v[0] for v in verts], [v[1] for v in verts]
    cx, cy = (min(xs)+max(xs))/2.0, (min(ys)+max(ys))/2.0
    return [(int(round(cx + (x-cx)*scale)),
             int(round(cy + (y-cy)*scale))) for x, y in verts]


# ---- Downsample 19×19 → N×N, then slice into 8×8 4bpp tiles ---------------

def downsample(src, sw, sh, dw, dh):
    from collections import Counter
    dst = [0] * (dw * dh)
    for dy in range(dh):
        ys, ye = int(dy*sh/dh), int((dy+1)*sh/dh)
        for dx in range(dw):
            xs, xe = int(dx*sw/dw), int((dx+1)*sw/dw)
            samples = [src[y*sw+x] for y in range(ys, max(ye, ys+1))
                                   for x in range(xs, max(xe, xs+1))
                                   if src[y*sw+x] != 0]
            if samples: dst[dy*dw+dx] = Counter(samples).most_common(1)[0][0]
    return dst

def pixels_to_tile(pixels, w, tx, ty):
    """8×8 4bpp MD tile: 4 bytes per row, high nibble = even-x pixel."""
    out = bytearray(32)
    for r in range(8):
        py = ty*8 + r
        for c in range(0, 8, 2):
            px = tx*8 + c
            p1 = pixels[py*w + px] & 0x0F
            p2 = pixels[py*w + px + 1] & 0x0F
            out[r*4 + c//2] = (p1 << 4) | p2
    return out


# ---- Per-sprite pipeline + C emission -------------------------------------

def extract(text, label, verts_label, angle, size_tiles, color, scale=1.0):
    block = find_label_block(text, label)
    faces, inline_verts = parse_sprite(block)
    verts = inline_verts if inline_verts else parse_verts(text, verts_label)
    verts = rotate_verts(verts, angle)
    verts = scale_verts(verts, scale)
    big = render_polygon(faces, verts, color)
    px = size_tiles * 8
    pix = downsample(big, 19, 19, px, px)
    # Column-major order (VDP sprite multi-tile layout).
    tiles = [pixels_to_tile(pix, px, tx, ty)
             for tx in range(size_tiles) for ty in range(size_tiles)]
    return b''.join(tiles)


def main():
    text = OBJ2D.read_text()
    blobs = []
    for name, label, vlabel, ang, sz, col, sc in SPRITES:
        data = extract(text, label, vlabel, ang, sz, col, sc)
        blobs.append((name, sz, col, data))
        print(f"  {name:14s} ← {label:12s} size {sz}x{sz}  pal {col}  scale {sc:.2f}  ({len(data)} bytes)")

    OUT_H.write_text(f"""// Generated by tools/extract_mcd_sprites.py — do not edit.
// Polygon-rasterised entity sprites from tempest2k-source/src/obj2d.s,
// each 8x8 or 16x16 in MD 4bpp tile format, column-major.

#ifndef TEMPEST_SPRITES_H
#define TEMPEST_SPRITES_H

#include <types.h>

""" + "\n".join(
    f"extern const u8 SPR_{n.upper()}[{len(d)}];  /* {sz}x{sz}, pal {c} */"
    for n, sz, c, d in blobs) + """

#endif
""")

    lines = [
        '/* Generated by tools/extract_mcd_sprites.py — do not edit. */',
        '#include "sprites.h"',
        '',
    ]
    for n, sz, col, data in blobs:
        lines.append(f"const u8 SPR_{n.upper()}[{len(data)}] = {{")
        for r in range(0, len(data), 16):
            row = ", ".join(f"0x{b:02X}" for b in data[r:r+16])
            lines.append(f"  {row},")
        lines.append("};")
        lines.append("")
    OUT_C.write_text("\n".join(lines))
    print(f"wrote {OUT_H}")
    print(f"wrote {OUT_C}")


if __name__ == "__main__":
    main()
