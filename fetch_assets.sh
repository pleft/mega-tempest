#!/usr/bin/env bash
# Fetch + regenerate the Tempest 2000 game assets mega-tempest needs to build.
#
# The music, font, sprite shapes, and sound effects are pulled from the
# mwenge/tempest2k source tree and converted on the fly by the
# tools/extract_*.py scripts.
#
# Safe to run repeatedly: the clone is skipped if already present, and every
# output is deterministic (byte-identical on each run).
#
# Requires on PATH: git, python3 (extractors are pure stdlib — no pip deps).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PARENT="$(cd "$HERE/.." && pwd)"

# Upstream T2K source (pinned for reproducible builds). Override T2K_URL /
# T2K_PIN to build against a fork or a newer revision.
T2K_URL="${T2K_URL:-https://github.com/mwenge/tempest2k.git}"
T2K_PIN="${T2K_PIN:-8b50b467ce7dd7eee475aaf5ae86804d95401ae7}"

# The extract_*.py tools resolve tempest2k-source relative to this repo's
# parent, so this path is fixed.
T2K_DIR="$PARENT/tempest2k-source"

if [ -d "$T2K_DIR/.git" ]; then
  head="$(git -C "$T2K_DIR" rev-parse HEAD)"
  if [ "$head" != "$T2K_PIN" ]; then
    echo "==> tempest2k-source present at $T2K_DIR (HEAD ${head:0:9}, pinned ${T2K_PIN:0:9} — leaving as-is)"
  else
    echo "==> tempest2k-source present at $T2K_DIR (pinned revision)"
  fi
else
  echo "==> cloning $T2K_URL"
  git clone --quiet "$T2K_URL" "$T2K_DIR"
  git -C "$T2K_DIR" checkout --quiet "$T2K_PIN"
  echo "    checked out ${T2K_PIN:0:9} -> $T2K_DIR"
fi

echo "==> copying music (5 MODs, byte-identical to the Jaguar T2K soundtrack)"
for m in rave4 tune5 tune7 tune12 tune13; do
  cp "$T2K_DIR/src/sounds/$m.mod" "$HERE/res/$m.mod"
done

echo "==> converting font / sprites / SFX from the T2K source"
python3 "$HERE/tools/extract_font.py"
python3 "$HERE/tools/extract_mcd_sprites.py"
python3 "$HERE/tools/extract_sfx.py"

echo "==> assets ready in res/ and src/"
