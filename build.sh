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

cp "$ROM" "$HERE/../megacd-port.bin"
echo "built: $HERE/../megacd-port.bin"

if [ "$LAUNCH" = "1" ]; then
  /Users/pleft/Dev/sega/blastem/blastem "$HERE/../megacd-port.bin" &
fi
