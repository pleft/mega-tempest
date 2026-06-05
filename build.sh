#!/usr/bin/env bash
# Self-bootstrapping build for the Tempest 2000 Mega CD Mode 1 cart.
#
# Run from a fresh checkout on any machine:
#   ./build.sh
#
# It will, in order:
#   1. check for the three tools it can't install for you (git, docker, python3)
#   2. fetch + convert the Tempest 2000 game assets (./fetch_assets.sh)
#   3. clone the megadev framework next to this repo (if missing)
#   4. build the megadev m68k toolchain docker image (one-time, if missing)
#   5. build the Sub CPU module, then the cart, trim + patch it, and copy the
#      result to ../mega-tempest-${VERSION}.bin
#
# Network access is needed on the first run (git clone + docker image build).
set -euo pipefail

VERSION="v0.3-beta-wip"   # bump on each tagged release (matches the git tag, sans `v`)

HERE="$(cd "$(dirname "$0")" && pwd)"

# --- 0. Preflight: prerequisites we cannot install for you -------------------
missing=0
need() {  # cmd  install-hint
  if ! command -v "$1" >/dev/null 2>&1; then
    printf '  missing: %-8s %s\n' "$1" "$2"
    missing=1
  fi
}
need git     "https://git-scm.com/downloads  (or: brew install git / apt install git)"
need docker  "https://docs.docker.com/get-docker/  (Docker Desktop on macOS/Windows, docker.io on Linux)"
need python3 "https://www.python.org/downloads/  (or: brew install python / apt install python3)"
if [ "$missing" = 1 ]; then
  echo >&2
  echo "error: install the tool(s) above, then re-run ./build.sh" >&2
  exit 1
fi
if ! docker info >/dev/null 2>&1; then
  echo "error: docker is installed but its daemon isn't reachable." >&2
  echo "       start Docker Desktop, or 'sudo systemctl start docker', then re-run." >&2
  exit 1
fi

# --- 1. Fetch + convert the (non-redistributed) Tempest 2000 assets ----------
"$HERE/fetch_assets.sh"

# --- 2. megadev framework (toolchain + library) -----------------------------
MEGADEV_URL="${MEGADEV_URL:-https://github.com/drojaazu/megadev.git}"
MEGADEV_PIN="${MEGADEV_PIN:-7a7246c14b845ad2f1bd3c7d73afb04cf67d83ef}"
MEGADEV="${MEGADEV_DIR:-$(cd "$HERE/.." && pwd)/megadev}"
if [ ! -d "$MEGADEV/.git" ]; then
  echo "==> cloning megadev framework -> $MEGADEV"
  git clone --quiet "$MEGADEV_URL" "$MEGADEV"
  git -C "$MEGADEV" checkout --quiet "$MEGADEV_PIN"
fi
MEGADEV="$(cd "$MEGADEV" && pwd)"

# --- 3. megadev m68k toolchain docker image (one-time) ----------------------
if ! docker image inspect megadev-build >/dev/null 2>&1; then
  echo "==> building 'megadev-build' docker image (one-time, ~1-2 min)"
  docker build --platform linux/amd64 --target base -t megadev-build "$MEGADEV/new_project"
fi

SUB="$HERE/sub"
MAKE_ARGS=("$@")

run_megadev_make() {
  local dir="$1"; shift
  docker run --rm --platform linux/amd64 \
    -v "$MEGADEV":/opt/megadev \
    -v "$dir":/work \
    -w /work \
    -e M68K_PREFIX=m68k-linux-gnu- \
    -e MEGADEV_PATH=/opt/megadev \
    megadev-build make "$@"
}

# --- 4. Sub side first — produces disc/spx.smd which the cart .incbin's ------
echo "==> building Sub CPU module (spx.smd)"
rm -rf "$SUB/build" "$SUB/disc"
mkdir -p "$SUB/build" "$SUB/disc"
run_megadev_make "$SUB"
cp "$SUB/disc/spx.smd" "$HERE/res/spx.smd"

# --- 5. Cart side -----------------------------------------------------------
echo "==> building cart"
rm -rf "$HERE/build" && mkdir -p "$HERE/build"
rm -f "$HERE/tempest.cart"
run_megadev_make "$HERE" "${MAKE_ARGS[@]:-}"

ROM="$HERE/tempest.cart"
[ -f "$ROM" ] || { echo "build: $ROM not produced"; exit 1; }

# Trim the 16 MB zero-padding that md_cart.ld forces objcopy to emit.
# The LD script anchors .data at 0xFF0000, so the binary is padded out
# to that offset. .data isn't loaded on main side anyway (megadev's main
# bootloader doesn't copy .data → RAM), so it can be dropped safely.
# Truncate to _rom_end (last byte of .rodata), rounded up to 256.
ROM_END_HEX=$(awk '$3 == "_rom_end" && $2 == "T" { print $1 }' "$HERE/build/tempest.cart.sym")
if [ -n "$ROM_END_HEX" ]; then
  ROM_END_DEC=$((0x$ROM_END_HEX))
  ROUND=256
  CART_SIZE=$(( (ROM_END_DEC + ROUND - 1) / ROUND * ROUND ))
  python3 -c "
import os
with open('$ROM', 'rb') as f: d = f.read($CART_SIZE)
with open('$ROM', 'wb') as f: f.write(d)
print(f'trimmed cart: {len(d)} bytes (was {os.path.getsize(\"$ROM\")+0:,} after trim)')
"
fi

python3 -c "
p='$ROM'
d=bytearray(open(p,'rb').read())
if d[0x191] != ord('C'):
    d[0x191] = ord('C')
    open(p,'wb').write(d)
    print('patched device flag at \$191 -> C')
else:
    print('device flag already set')
"

OUT="$HERE/../mega-tempest-${VERSION}.bin"
cp "$ROM" "$OUT"
echo "built: $OUT"
