#!/bin/sh
# An axial shading reaches the SVG as a gradient the consumer draws.
#
# SVG interpolates between a gradient's stops, so what is written is the
# colour ramp and not the bands a decomposition leaves. The geometry is not
# sampled at all: the axis goes over as it stands, under the mapping from
# the space the program painted in.
#
# The other shading types have no SVG form -- a function-based shading none
# at all, a radial one a focal radius not every consumer reads -- so they
# must still decompose, which is what makes the scope testable.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-svg-shading.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0

run() {  # $1 label  $2 body
    cat > "$work/$1.ps" <<EOF
$2
showpage
EOF
    "$xpost" -q -d svgwrite -o "$work/$1.svg" "$work/$1.ps" </dev/null >/dev/null 2>&1 \
        || { echo "FAIL: $1 errored"; fail=1; return 1; }
    return 0
}

# an axial shading is one gradient, not a page of flat cells
run axial '0 0 300 300 rectclip
<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 300 0]
   /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> shfill' && {
    grep -q '<linearGradient' "$work/axial.svg" \
        || { echo "FAIL: no gradient written for an axial shading"; fail=1; }
    n=$(grep -c '<path' "$work/axial.svg")
    test "$n" -le 2 || { echo "FAIL: the axial shading left $n paths, so it decomposed"; fail=1; }
    # the ramp is described, not drawn: several stops, and the ends carry colour
    s=$(grep -c '<stop' "$work/axial.svg")
    test "$s" -ge 8 || { echo "FAIL: only $s stops describe the ramp"; fail=1; }
    grep -q 'stop-color="rgb(100%,0%,0%)"' "$work/axial.svg" \
        || { echo "FAIL: the ramp does not start at the colour C0 names"; fail=1; }
    # the mapping from the painting space travels with it, and is well formed
    grep -qE 'gradientTransform="matrix\([-0-9. ]*\)"' "$work/axial.svg" \
        || { echo "FAIL: no well-formed gradientTransform"; fail=1; }
}

# an unextended end is left unpainted, which a stop of no opacity says
grep -q 'stop-opacity="0"' "$work/axial.svg" 2>/dev/null \
    || { echo "FAIL: an unextended end carries no transparent guard"; fail=1; }

# ...and an extended one carries none: the control that shows the guard is
# read off Extend and not written unconditionally
run extended '0 0 300 300 rectclip
<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 300 0] /Extend [true true]
   /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> shfill' && {
    grep -q 'stop-opacity="0"' "$work/extended.svg" \
        && { echo "FAIL: an extended shading was guarded as though it were not"; fail=1; }
}

# a shading pattern is a shading too, and reaches the same gradient
run pattern '<< /PatternType 2
   /Shading << /ShadingType 2 /ColorSpace /DeviceGray /Coords [0 0 200 0]
               /Function << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> >>
>> matrix makepattern /Pattern setcolorspace setcolor
0 0 200 200 rectfill' && {
    grep -q '<linearGradient' "$work/pattern.svg" \
        || { echo "FAIL: a shading pattern did not reach a gradient"; fail=1; }
}

# SCOPE. A radial shading has no gradient here and must still decompose --
# without this the checks above would pass on a writer that claimed every
# type and wrote the wrong thing for most of them.
run radial '0 0 300 300 rectclip
<< /ShadingType 3 /ColorSpace /DeviceGray /Coords [150 150 10 150 150 140]
   /Function << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> >> shfill' && {
    grep -q '<linearGradient' "$work/radial.svg" \
        && { echo "FAIL: a radial shading was written as a linear gradient"; fail=1; }
    test "$(grep -c '<path' "$work/radial.svg")" -gt 2 \
        || { echo "FAIL: the radial shading painted almost nothing"; fail=1; }
}

# a page with no shading names no gradient
run plain '0 0 1 setrgbcolor 10 10 100 100 rectfill' && {
    grep -q 'linearGradient' "$work/plain.svg" \
        && { echo "FAIL: a page with no shading named a gradient"; fail=1; }
}

test $fail -eq 0 && echo "OK: svgwrite writes an axial shading as a gradient"
exit $fail
