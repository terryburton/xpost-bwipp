#!/bin/sh
# Meson/make-check wrapper for the svgwrite device: render a known scene
# (fill, stroke, glyph outlines) through setpagedevice and require the
# structural landmarks of the produced document -- the svg root sized in
# points with a device-unit viewBox, the filled rectangle's path with its
# exact coordinates, a stroked path carrying the graphics-state attributes,
# at least one glyph outline path with curve commands, and a circle fill
# whose curves are preserved. A second page at
# 144dpi must report the same point size with a doubled viewBox.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
if faceless_build "$xpost"; then
    echo "SKIPPED: this run shows text through a face, and this build carries no face library"
    exit 77
fi
# relative: the OutputFile paths are named inside the PS program the
# interpreter runs, and a native interpreter under a POSIX shell need not
# share the shell's view of an absolute path
tmp=svgwrite-$$
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp"
cat > "$tmp/t.ps" <<PSEOF
<< /OutputDevice /svgwrite /OutputFile ($tmp/a.svg) /PageSize [200 100] >> setpagedevice
0 0 1 setrgbcolor newpath 20 20 moveto 60 0 rlineto 0 40 rlineto -60 0 rlineto closepath fill
1 0 0 setrgbcolor 2 setlinewidth 1 setlinejoin newpath 100 20 moveto 40 30 rlineto 40 -30 rlineto stroke
0 setgray /Courier findfont 18 scalefont setfont 20 80 moveto (Og) show
0 1 0 setrgbcolor newpath 160 70 15 0 360 arc closepath fill
0 setgray 1 setlinewidth newpath 130 30 10 0 180 arc stroke
0 setgray newpath 10.12345 5 moveto 5 0 rlineto 0 2 rlineto -5 0 rlineto closepath fill
gsave 60 5 translate 20 20 scale
4 4 8 [ 4 0 0 -4 0 4 ] { <004080c0 4080c000 80c00040 c0004080> } image
grestore
showpage
<< /HWResolution [144 144] /OutputFile ($tmp/b.svg) >> setpagedevice
0 0 1 setrgbcolor newpath 20 20 moveto 60 0 rlineto 0 40 rlineto -60 0 rlineto closepath fill
showpage
<< /OutputDevice /null >> setpagedevice
quit
PSEOF
out=$("$xpost" -q -d null -o /dev/null "$tmp/t.ps" </dev/null 2>&1)
status=$?

fail() { echo "FAIL: $1"; exit 1; }

# the document is read below; how the run left is read here, since a
# device that wrote every landmark and then died on the way out leaves a
# document with every landmark in it
verdict_run "$status" "$out" "the svg run" || exit 1
a=$tmp/a.svg; b=$tmp/b.svg
[ -s "$a" ] || fail "no output"
grep -q '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" version="1.1" width="200pt" height="100pt" viewBox="0 0 200 100">' "$a" || fail "svg root"
# An image refers to its content through the xlink namespace, which is
# where the version this document declares puts it. The bare href a later
# version allows is passed over by viewers that read the rest of the
# document, and the picture then goes missing with nothing said.
grep -q 'xlink:href="data:image/' "$a" || fail "image reference through xlink"
grep -q ' href="data:image/' "$a" && fail "image referred to by a bare href"
grep -q '<path fill="rgb(0%,0%,100%)" fill-rule="nonzero" d="M20 80L80 80L80 40L20 40Z"/>' "$a" || fail "filled rect path"
grep -q '<path fill="none" stroke="rgb(100%,0%,0%)" stroke-width="2" stroke-linecap="butt" stroke-linejoin="round" stroke-miterlimit="10" d="M100 80L140 50L180 80"/>' "$a" || fail "stroked path"
grep -q '<path fill="rgb(0%,0%,0%)" d="M[0-9.]* [0-9.]* C' "$a" || fail "glyph outline"
grep -q '<path fill="rgb(0%,100%,0%)" fill-rule="nonzero" d="M175 30C' "$a" || fail "curve-preserving circle fill"
grep -q 'stroke-width="1"[^>]*d="M140 70C' "$a" || fail "curve-preserving stroke"
grep -q 'd="M10.1235 95L15.1235 95L15.1235 93L10.1235 93Z"' "$a" || fail "four-decimal coordinates"
grep -q '<image transform="matrix(' "$a" || fail "sampled image element"
# PNG where the build can write one -- a format a reader is required to
# have -- and the bitmap where it cannot
grep -q 'data:image/png;base64,' "$a" || grep -q 'data:image/bmp;base64,' "$a" \
    || fail "image data in an embeddable format"
# every attribute value is a single quoted run: a doubled quote would end the
# value early and make the document malformed (image-rendering once did)
grep -q '=""' "$a" && fail "empty/doubled attribute quote"
grep -q 'image-rendering="pixelated"' "$a" || fail "image-rendering attribute"
# no attribute may carry a stray quote inside its value
grep -qE '"[a-zA-Z-]+="[^"]*"[^ />]' "$a" && fail "malformed attribute run"
grep -q '</svg>' "$a" || fail "closing tag"
grep -q 'width="200pt" height="100pt" viewBox="0 0 400 200"' "$b" || fail "144dpi page in points"
grep -q 'd="M40 160L160 160L160 80L40 80Z"' "$b" || fail "144dpi coordinates"

# --- a stencil goes out as a mask, and a repeated one goes out once ----
#
# SVG has no stencil of its own, so the bits go in as a mask -- an image
# whose light samples are the ones that let paint through -- and the
# paint is a rectangle of the current colour drawn through it. Without
# it a stencil is decomposed into a fill per run of set samples, which
# is what a page of bitmap glyphs came out as.
#
# The reuse is the half that makes it worth having: a page of text
# paints the same few glyphs over and over, and a mask written per
# occurrence would trade thousands of little fills for thousands of
# little images.
c=$tmp/c.svg
cat > "$tmp/c.ps" <<'CEOF'
<< /PageSize [64 64] >> setpagedevice
0 setgray
gsave  4  4 translate 8 8 scale 8 8 true [8 0 0 -8 0 8] <FF81BDA5A5BD81FF> imagemask grestore
gsave 20  4 translate 8 8 scale 8 8 true [8 0 0 -8 0 8] <FF81BDA5A5BD81FF> imagemask grestore
gsave 36  4 translate 8 8 scale 8 8 true [8 0 0 -8 0 8] <0F0F0F0FF0F0F0F0> imagemask grestore
showpage
CEOF
"$xpost" -q -d svgwrite -o "$c" "$tmp/c.ps" </dev/null >/dev/null 2>&1
[ -s "$c" ] || fail "the SVG writer emitted nothing for a stencil"
grep -q '<mask id="xm' "$c" || fail "the stencil went out as fills rather than as a mask"
nm=$(grep -o '<mask id="xm' "$c" | wc -l)
[ "$nm" = 2 ] || fail "expected 2 masks for 3 placements of 2 distinct stencils, saw $nm"
nr=$(grep -o 'mask="url(#xm' "$c" | wc -l)
[ "$nr" = 3 ] || fail "expected 3 placements drawn through a mask, saw $nr"
grep -q 'mask="url(#xm0)"' "$c" || fail "the repeated stencil does not refer to the first mask"
# The mask holds a path. A viewer is required to read PNG and JPEG
# (SVG 1.1 4.6) and nothing else, so a mask carrying any other raster
# would go blank in a viewer that reads the rest of the document
# perfectly well -- and a stencil used to come out as paths, which
# every viewer reads.
echo "$(sed -n 's/.*<mask id="xm0"\([^!]*\)<\/mask>.*/\1/p' "$c")" | grep -q '<path ' \
    || fail "the mask does not hold a path"
sed -n 's/.*<mask id="xm0"\([^!]*\)<\/mask>.*/\1/p' "$c" | grep -q '<image' \
    && fail "the mask holds a raster, which a viewer is not required to read"

echo SUCCESS
exit 0
