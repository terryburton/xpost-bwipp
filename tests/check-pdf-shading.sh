#!/bin/sh
# The pdfwrite device carries an analytic shading into the PDF as a shading
# dictionary rather than decomposing it into flat-colour fills. PDF holds
# function-based, axial and radial shadings natively (PDF 8.7.4.5), so the
# consumer's own arithmetic draws the gradient: no bands, and a fraction of
# the size.
#
# The mesh types are not offered, their geometry being a stream this writer
# does not rewrite, and they must still decompose -- which is what makes the
# scope of the feature testable rather than assumed.
#
# CompressPages false leaves the content stream readable, so the `sh`
# operator that paints the shading can be read alongside the resource it
# names.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-pdf-shading.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0

mk() {  # $1 name, $2 body
    cat > "$work/$1.ps" <<EOF
<< /CompressPages false >> setdistillerparams
$2
showpage
EOF
    "$xpost" -q -d pdfwrite -o "$work/$1.pdf" "$work/$1.ps" </dev/null >/dev/null 2>&1 \
        || { echo "FAIL: the $1 run errored"; fail=1; }
}

# axial, exponential function
mk axial '0 0 300 300 rectclip
<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 300 0] /Extend [true true]
   /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> shfill'

# radial, exponential function
mk radial '0 0 300 300 rectclip
<< /ShadingType 3 /ColorSpace /DeviceGray /Coords [150 150 10 150 150 140]
   /Function << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> >> shfill'

# function-based, sampled function (a stream in the output)
mk funcbased '0 0 200 200 rectclip
<< /ShadingType 1 /ColorSpace /DeviceRGB /Domain [0 1 0 1] /Matrix [200 0 0 200 0 0]
   /Function << /FunctionType 0 /Domain [0 1 0 1] /Range [0 1 0 1 0 1]
                /Size [2 2] /BitsPerSample 8
                /DataSource <FF0000 00FF00 0000FF FFFF00> >> >> shfill'

# a mesh type stays decomposed -- the negative control for scope
mk mesh '0 0 200 200 rectclip
<< /ShadingType 4 /ColorSpace /DeviceRGB /BitsPerCoordinate 8 /BitsPerComponent 8
   /BitsPerFlag 8 /Decode [0 255 0 255 0 1 0 1 0 1]
   /DataSource <00 0A0A FF0000  00 F00A 00FF00  00 7FF0 0000FF> >> shfill'

# a page with no shading names no shading resource -- the control that proves
# the checks below are reading this feature and not something always present
mk plain '0 0 1 setrgbcolor 10 10 100 100 rectfill'

for t in axial:2 radial:3 funcbased:1; do
    n=${t%%:*}; ty=${t##*:}
    grep -q "/ShadingType $ty" "$work/$n.pdf" \
        || { echo "FAIL: no /ShadingType $ty in the $n PDF"; fail=1; }
    grep -q '/Shading' "$work/$n.pdf" \
        || { echo "FAIL: no /Shading resource in the $n PDF"; fail=1; }
    grep -q ' sh$\| sh ' "$work/$n.pdf" \
        || { echo "FAIL: no sh operator in the $n content stream"; fail=1; }
done

grep -q '/Coords' "$work/axial.pdf"  || { echo "FAIL: axial carries no /Coords"; fail=1; }
grep -q '/Extend \[true true' "$work/axial.pdf" \
    || { echo "FAIL: axial did not carry /Extend"; fail=1; }
grep -q '/Coords' "$work/radial.pdf" || { echo "FAIL: radial carries no /Coords"; fail=1; }
grep -q '/FunctionType 2' "$work/axial.pdf" \
    || { echo "FAIL: axial carries no exponential function"; fail=1; }
grep -q '/FunctionType 0' "$work/funcbased.pdf" \
    || { echo "FAIL: function-based carries no sampled function"; fail=1; }
grep -q '/Matrix' "$work/funcbased.pdf" \
    || { echo "FAIL: function-based carries no /Matrix"; fail=1; }

# scope: the mesh type is still decomposed, and says so by naming no shading
grep -q '/ShadingType 4' "$work/mesh.pdf" \
    && { echo "FAIL: a mesh shading was written as a shading dictionary"; fail=1; }
# ...and it must still have painted something
test "$(wc -c < "$work/mesh.pdf")" -gt 700 \
    || { echo "FAIL: the mesh page painted nothing"; fail=1; }

grep -q '/ShadingType' "$work/plain.pdf" \
    && { echo "FAIL: a page with no shading named a shading"; fail=1; }

# the whole point: a preserved shading is far smaller than its decomposition
as=$(wc -c < "$work/axial.pdf")
test "$as" -lt 3000 || { echo "FAIL: axial PDF is $as bytes, not a preserved shading"; fail=1; }

test $fail -eq 0 && echo "OK: pdfwrite preserves analytic shadings"
exit $fail
