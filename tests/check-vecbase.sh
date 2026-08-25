#!/bin/sh
# Guard the vector-writer base: the PDF, SVG and DSC writers format
# numbers through the single .num2str helper -- exactly ONE two-decimal
# formatter definition exists in the PostScript sources -- the
# page-to-points unscale exists once (.devres-to-points) and its users
# reach it by reference, and the DSC device stays a dict-copy derivation
# of the PDF device. A second formatter or a private unscale snippet
# means the writers are re-deriving the base again.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-vecbase.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
data=$src/data
fail=0

# exactly one two-decimal formatter core, and it is the shared helper
hits=$(grep -l '100 mul round' "$data"/*.ps || true)
if [ "$hits" != "$data/device.ps" ]; then
    echo "check-vecbase: expected the only '100 mul round' formatter core in data/device.ps, found:"
    printf '%s\n' "${hits:-none}"
    fail=1
fi
n=$(grep -c '100 mul round' "$data/device.ps" || true)
if [ "$n" != 1 ]; then
    echo "check-vecbase: expected exactly one '100 mul round' in data/device.ps, found $n"
    fail=1
fi

# every vector writer formats through the shared helper
for f in pdfwrite.ps svgwrite.ps dscwrite.ps; do
    if ! grep -qE '(//\.num2str[ \t]+exec|/\.num2str[ \t]+get[ \t]+exec)' "$data/$f"; then
        echo "check-vecbase: $f does not reach the shared .num2str formatter"
        fail=1
    fi
done

# exactly one defaultmatrix unscale, and its users reach it by reference
hits=$(grep -l '/defaultmatrix 2 copy known' "$data"/*.ps || true)
if [ "$hits" != "$data/device.ps" ]; then
    echo "check-vecbase: expected the only defaultmatrix unscale in data/device.ps, found:"
    printf '%s\n' "${hits:-none}"
    fail=1
fi
for f in svgwrite.ps dscwrite.ps bboxdev.ps; do
    if ! grep -q '/\.devres-to-points get exec' "$data/$f"; then
        echo "check-vecbase: $f does not reach the shared .devres-to-points unscale"
        fail=1
    fi
done

# the DSC device is the PDF device wearing another wrapper
if ! grep -q '\.xpost_PDFWRITE dup length .* dict copy' "$data/dscwrite.ps"; then
    echo "check-vecbase: dscwrite.ps no longer derives by dict copy from .xpost_PDFWRITE"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "check-vecbase: the vector-writer base is no longer shared."
    exit 1
fi
echo "check-vecbase: ok (one formatter, one unscale, dict-copy derivation)"
exit 0
