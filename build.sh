#!/usr/bin/env bash
# Build wrapper for the Tempest 2000 Mega CD Mode 1 cart.
# Builds the Sub CPU module first (spx.smd) then the cart, patches the
# 'C' device-support flag at offset $191, optionally launches BlastEm.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MEGADEV="$(cd "$HERE/../megadev" && pwd)"
SUB="$HERE/sub"

LAUNCH=0
MAKE_ARGS=()
for a in "$@"; do
  if [ "$a" = "run" ]; then LAUNCH=1; else MAKE_ARGS+=("$a"); fi
done

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

# Sub side first — produces disc/spx.smd which the cart .incbin's.
rm -rf "$SUB/build" "$SUB/disc"
mkdir -p "$SUB/build" "$SUB/disc"
run_megadev_make "$SUB"
cp "$SUB/disc/spx.smd" "$HERE/res/spx.smd"

# Cart side.
rm -rf "$HERE/build" && mkdir -p "$HERE/build"
rm -f "$HERE/tempest.cart"
run_megadev_make "$HERE" "${MAKE_ARGS[@]:-}"

ROM="$HERE/tempest.cart"
[ -f "$ROM" ] || { echo "build: $ROM not produced"; exit 1; }
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

cp "$ROM" "$HERE/../megacd-port.bin"
echo "built: $HERE/../megacd-port.bin"

if [ "$LAUNCH" = "1" ]; then
  /Users/pleft/Dev/sega/blastem/blastem "$HERE/../megacd-port.bin" &
fi
