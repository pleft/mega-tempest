#!/usr/bin/env python3
"""Extract every instrument sample from a Protracker MOD file into individual
WAV files. Helps identify which sample is which by ear.

Usage:
    python3 tools/extract_mod_samples.py res/tune13.mod
    python3 tools/extract_mod_samples.py res/*.mod

WAVs land in <input>.samples/<NN>_<name>.wav next to the MOD file.

Sample rate is fixed at 8363 Hz (the Amiga PAL C-2 reference rate used by
Protracker), which is also the rate our RF5C164 driver plays back at."""

import struct
import sys
import wave
from pathlib import Path


SAMPLE_RATE = 8363   # Amiga C-2 reference, matches our chip period 428


def extract(mod_path: Path) -> int:
    data = mod_path.read_bytes()
    if len(data) < 1084:
        print(f"{mod_path}: too small to be a MOD")
        return 0

    # Parse instrument records — 31 instruments, 30 bytes each, starting at 20.
    instruments = []
    for i in range(31):
        base = 20 + i * 30
        name = data[base:base + 22].decode("ascii", errors="replace").rstrip("\x00 ").strip()
        sample_words = struct.unpack(">H", data[base + 22:base + 24])[0]
        sample_len = sample_words * 2
        instruments.append((name, sample_len))

    # Song length + pattern positions table.
    song_len = data[950]
    positions = data[952:952 + 128]
    num_patterns = max(positions[:song_len]) + 1 if song_len else 0

    # Sample data starts after pattern data.
    sample_data_offset = 1084 + num_patterns * 1024

    out_dir = mod_path.with_suffix(mod_path.suffix + ".samples")
    out_dir.mkdir(exist_ok=True)

    cursor = sample_data_offset
    written = 0
    print(f"\n=== {mod_path} (samples dir: {out_dir.name}) ===")
    for i, (name, length) in enumerate(instruments, start=1):
        if length == 0:
            continue
        sample = data[cursor:cursor + length]
        cursor += length

        # MOD samples are signed 8-bit. Convert to signed 16-bit for WAV.
        widened = bytearray()
        for b in sample:
            s8 = b - 256 if b >= 128 else b
            s16 = s8 << 8
            widened += struct.pack("<h", s16)

        # File name: NN_<safe-name>.wav
        safe_name = "".join(c if c.isalnum() or c in " _-" else "_" for c in name).strip()
        if not safe_name:
            safe_name = "unnamed"
        out_path = out_dir / f"{i:02d}_{safe_name}.wav"

        with wave.open(str(out_path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(bytes(widened))

        dur_s = length / SAMPLE_RATE
        print(f"  {i:>2}: {out_path.name:<40} {length:>6}b  {dur_s:>5.2f}s")
        written += 1

    return written


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    total = 0
    for arg in sys.argv[1:]:
        p = Path(arg)
        if not p.exists():
            print(f"skip {p}: not found")
            continue
        total += extract(p)
    print(f"\nDone. {total} sample WAVs written.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
