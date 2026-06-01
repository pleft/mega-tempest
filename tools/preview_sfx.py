#!/usr/bin/env python3
"""Convert every SFX sample in tempest2k-source/src/sounds/samples/ to a
WAV so they can be auditioned before being baked into the cart. Mirrors
the format the RF5C164 ultimately plays — 8-bit signed source, 8363 Hz —
but written as 16-bit signed PCM (left-shifted) for compatibility.

Auto-detects IFF 8SVX wrappers ("FORM…8SVX"); the rest are treated as
raw signed 8-bit PCM.

Output: tools/sfx_previews/NN.wav next to the source files.

Usage:
    python3 tools/preview_sfx.py
"""

import struct, sys, wave
from pathlib import Path


DEFAULT_RATE = 8287   # Amiga C-2 period 428 ≈ 8287 Hz — most samples
AMIGA_CLK    = 3546895   # PAL Amiga clock — rate = AMIGA_CLK / period

ROOT      = Path(__file__).resolve().parents[2]
JAG_SRC   = ROOT / "tempest2k-source/src/sounds/samples"
META_BIN  = ROOT / "tempest2k-source/src/incbin/sound_samples_table.bin"
OUT_DIR   = Path(__file__).resolve().parents[0] / "sfx_previews"


def load_periods() -> dict:
    """Parse sound_samples_table.bin → {file_num: period}. Each entry is
    40 bytes: 20-byte name + 16-bit id (= file_num) + 16-bit period + rest."""
    if not META_BIN.is_file():
        return {}
    blob = META_BIN.read_bytes()
    out = {}
    for i in range(len(blob) // 40):
        e = blob[i*40:(i+1)*40]
        sid    = int.from_bytes(e[20:22], "big")
        period = int.from_bytes(e[22:24], "big")
        # Sanity — Amiga periods are roughly 100..1024
        if 100 <= period <= 1024 and 1 <= sid <= 32:
            out[sid] = period
    return out


def strip_8svx(blob: bytes) -> bytes:
    body = blob.find(b"BODY")
    if body < 0:
        return blob                                    # not really 8SVX
    size = struct.unpack(">I", blob[body + 4 : body + 8])[0]
    return blob[body + 8 : body + 8 + size]


def is_iff(blob: bytes) -> bool:
    return blob[:4] == b"FORM" and blob[8:12] == b"8SVX"


def to_wav(signed_pcm: bytes, out_path: Path, rate: int) -> None:
    frames = bytearray(len(signed_pcm) * 2)
    for i, b in enumerate(signed_pcm):
        s = b if b < 128 else b - 256
        struct.pack_into("<h", frames, i * 2, s << 8)
    with wave.open(str(out_path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def main() -> int:
    if not JAG_SRC.is_dir():
        sys.exit(f"error: {JAG_SRC} not found")
    OUT_DIR.mkdir(exist_ok=True)
    periods = load_periods()
    files = sorted(p for p in JAG_SRC.iterdir() if p.name.isdigit())
    print(f"converting {len(files)} samples → {OUT_DIR}")
    for src in files:
        raw = src.read_bytes()
        pcm = strip_8svx(raw) if is_iff(raw) else raw
        sid = int(src.name)
        period = periods.get(sid)
        rate = (AMIGA_CLK // period) if period else DEFAULT_RATE
        dur = len(pcm) / rate
        out = OUT_DIR / f"{src.name}.wav"
        to_wav(pcm, out, rate)
        rate_tag = f"@ {rate} Hz" + (f" (period {period})" if period else " (default)")
        iff_tag  = " IFF" if is_iff(raw) else ""
        print(f"  {src.name}: {len(pcm):>6} bytes ({dur:>4.2f} s) {rate_tag}{iff_tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
