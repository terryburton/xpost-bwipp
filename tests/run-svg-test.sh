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
skip_if_faceless "$xpost" "this run shows text through a face"
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
<< /HWResolution [72 72] /OutputFile ($tmp/eo.svg) /PageSize [400 400] >> setpagedevice
newpath 200 200 150 0 360 arc closepath 200 200 75 0 360 arc closepath
eoclip newpath
0 0 1 setrgbcolor newpath 0 0 moveto 400 0 lineto 400 400 lineto 0 400 lineto closepath fill
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
grep -q '<path fill="#0000ff" d="M20 80L80 80L80 40L20 40Z"/>' "$a" || fail "filled rect path"
grep -q '<path fill="none" stroke="#ff0000" stroke-width="2" stroke-linecap="butt" stroke-linejoin="round" stroke-miterlimit="10" d="M100 80L140 50L180 80"/>' "$a" || fail "stroked path"
grep -q '<path fill="#000000" d="M[0-9.]* [0-9.]* C' "$a" || fail "glyph outline"
grep -q '<path fill="#00ff00" d="M175 30C' "$a" || fail "curve-preserving circle fill"
grep -q 'stroke-width="1"[^>]*d="M140 70C' "$a" || fail "curve-preserving stroke"
# A coordinate is written to two decimals, the precision the writer states
# for every number it puts down, and never in exponential form. Marks reach
# this in device space -- a scale the program set is already in the number
# rather than in a transform above it -- so a decimal here is a hundredth
# of a point. A matrix is written to its full precision instead, because it
# multiplies everything drawn under it.
grep -q 'd="M10.12 95L15.12 95L15.12 93L10.12 93Z"' "$a" || fail "two-decimal coordinates"
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

# --- a clip goes out as a clip, under the rule it was taken by ---------
#
# Two rings wound the same way enclose an annulus by the even-odd rule
# and a disc by the nonzero one, so a region is its outline AND the rule
# that outline is read under. A document carrying the outline alone
# leaves the reader to apply its own default -- nonzero -- and the hole
# in the middle of this page closes up.
#
# The shape reaching the document at all is the other half of it. The
# alternative is the region resolved to pixel-band rectangles, which
# arrives as one path per scanline: hundreds of them for a clip two
# curves describe, and a page written a row at a time.
eo=$tmp/eo.svg
[ -s "$eo" ] || fail "no output for the clipped page"
grep -q '<clipPath id="xc1"><path clip-rule="evenodd" d="M[0-9.]* [0-9.]*C' "$eo" \
    || fail "the even-odd clip did not go out as an even-odd clip with its curves"
grep -q '<g clip-path="url(#xc1)">' "$eo" || fail "the fill is not drawn through the clip"
neop=$(grep -c '<path' "$eo")
[ "$neop" -le 4 ] || fail "the clipped fill went out as $neop paths, which is the region resolved to pixel-band rectangles rather than the shape it was taken from"

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
nm=$(grep -o '<mask id="xm' "$c" | wc -l | tr -d ' ')
[ "$nm" = 2 ] || fail "expected 2 masks for 3 placements of 2 distinct stencils, saw $nm"
nr=$(grep -o 'mask="url(#xm' "$c" | wc -l | tr -d ' ')
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

# --- a tiling pattern goes out as the pattern SVG has for it ----------
#
# SVG 1.1 13.3 has the construct the language has (PLRM 4.9): a pattern
# element holding the cell, tiled by the reader over whatever the fill
# covers. Without it the interpreter steps the cells itself and the
# document carries a shape per cell, so what it weighs grows with the
# area filled -- 1,200 of them for the fill below.
#
# The tile is the step, and that is also this construct's limit: a cell
# larger than its step overlaps its neighbours, which one width and one
# height cannot say, so those keep the route they had.
#
# The last half is that the cell is written once however many fills name
# it, and that an uncoloured pattern (PLRM Table 4.9 PaintType 2) in two
# colours is two cells, the colour being painted into the cell.
d=$tmp/d.svg
cat > "$tmp/d.ps" <<'DEOF'
<< /PageSize [400 500] >> setpagedevice
/pat <<
  /PatternType 1 /PaintType 1 /TilingType 1
  /BBox [0 0 10 10] /XStep 10 /YStep 10
  /PaintProc { pop 1 0 0 setrgbcolor 1 1 8 8 rectfill }
>> matrix makepattern def
/upat <<
  /PatternType 1 /PaintType 2 /TilingType 1
  /BBox [0 0 10 10] /XStep 10 /YStep 10
  /PaintProc { pop 0 0 moveto 10 10 lineto 0 10 lineto closepath fill }
>> matrix makepattern def
/Pattern setcolorspace pat setpattern
20 20 moveto 320 20 lineto 320 420 lineto 20 420 lineto closepath fill
% the same pattern again: one element, not two
30 430 moveto 200 430 lineto 200 480 lineto 30 480 lineto closepath fill
[/Pattern /DeviceRGB] setcolorspace
0 0 1 upat setcolor
330 20 moveto 390 20 lineto 390 200 lineto 330 200 lineto closepath fill
0 1 0 upat setcolor
330 220 moveto 390 220 lineto 390 400 lineto 330 400 lineto closepath fill
showpage
DEOF
"$xpost" -q -d svgwrite -o "$d" "$tmp/d.ps" </dev/null >/dev/null 2>&1
[ -s "$d" ] || fail "the SVG writer emitted nothing for a pattern"
grep -q '<pattern id="xp0" patternUnits="userSpaceOnUse"' "$d" \
    || fail "the tiling pattern went out as the cells the interpreter steps rather than as the pattern element SVG has for exactly this"
np=$(grep -o '<pattern id="xp' "$d" | wc -l | tr -d ' ')
[ "$np" = 3 ] || fail "expected 3 pattern elements -- one coloured pattern used twice, and one uncoloured pattern in two colours -- saw $np"
nu=$(grep -o 'fill="url(#xp' "$d" | wc -l | tr -d ' ')
[ "$nu" = 4 ] || fail "expected 4 fills naming a pattern, saw $nu"
grep -q 'fill="url(#xp0)"[^>]*d="M20 480L320 480L320 80L20 80Z"' "$d" \
    || fail "the pattern fill is not one path naming the pattern"
# the cell is clipped to its box, which the tile is not: a step wider
# than the box would otherwise let the tile show what the box excludes
grep -q '<clipPath id="xb0"><rect x="0" y="0" width="10" height="10"/></clipPath>' "$d" \
    || fail "the cell is not clipped to the box PLRM 4.9.2 clips it to"
# and the document does not carry the cells: a shape per cell would be
# 1,200 paths for the first fill alone
ndp=$(grep -c '<path' "$d")
[ "$ndp" -le 12 ] || fail "the pattern fills went out as $ndp paths, which is the cells stepped rather than the pattern named"

# A cell larger than its step overlaps its neighbours, and one width and
# one height cannot say that: those keep the route they had.
e=$tmp/e.svg
cat > "$tmp/e.ps" <<'EEOF'
<< /PageSize [200 200] >> setpagedevice
<< /PatternType 1 /PaintType 1 /TilingType 1
   /BBox [0 0 20 20] /XStep 8 /YStep 8
   /PaintProc { pop 1 0.5 0 setrgbcolor 0 0 20 3 rectfill } >> matrix makepattern
/Pattern setcolorspace setpattern
20 20 moveto 180 20 lineto 180 180 lineto 20 180 lineto closepath fill
showpage
EEOF
"$xpost" -q -d svgwrite -o "$e" "$tmp/e.ps" </dev/null >/dev/null 2>&1
[ -s "$e" ] || fail "the SVG writer emitted nothing for an overlapping pattern"
grep -q '<pattern' "$e" && fail "a cell larger than its step was written as a tile, which states one width and one height and cannot say the cells overlap"

# --- what a page refers to is written in the page's own file ----------
#
# An SVG document holds one page, so this writer emits one whole document
# per page: a %d in the output name gives each page its own file, and a
# name without one is rewritten by every page and holds the last. Either
# way no file carries more than one page, and a reference in it resolves
# against the document being read (SVG 1.1 14.4) -- so an element a page
# names has to be written into that page's own file, and every record
# this writer keeps of what it has already written answers for one page
# and no more.
#
# What that costs when it slips is not a broken document: a mask property
# whose reference resolves nowhere is passed over and the rectangle it
# qualifies is painted whole, so a page of bitmap glyphs after the first
# comes out as solid blocks with nothing reported.
#
# The page below carries one of each kind of element this writer refers
# to by identity -- a stencil's mask, a clip shape, a tiling pattern and
# the pattern's box -- and is drawn twice, so every record is asked for
# something it holds from the page before.

# The references a file makes that nothing in that file answers.
unresolved() {
    grep -o 'url(#[A-Za-z0-9]*)' "$1" | sed 's/url(#//; s/)//' | sort -u |
    while read -r id; do
        grep -q "id=\"$id\"" "$1" || echo "$id"
    done
}

cat > "$tmp/g.ps" <<'GEOF'
<< /PageSize [200 200] >> setpagedevice
/pat << /PatternType 1 /PaintType 1 /TilingType 1
        /BBox [0 0 10 10] /XStep 10 /YStep 10
        /PaintProc { pop 1 0 0 setrgbcolor 1 1 8 8 rectfill } >> matrix makepattern def
/page {
    0 setgray
    gsave 4 4 translate 8 8 scale
        8 8 true [8 0 0 -8 0 8] <FF81BDA5A5BD81FF> imagemask
    grestore
    gsave
        newpath 100 100 60 0 360 arc closepath 100 100 30 0 360 arc closepath
        eoclip
        0 0 1 setrgbcolor
        newpath 40 40 moveto 160 40 lineto 160 160 lineto 40 160 lineto closepath fill
    grestore
    /Pattern setcolorspace pat setpattern
    newpath 20 170 moveto 90 170 lineto 90 195 lineto 20 195 lineto closepath fill
    showpage
} def
page page
GEOF

# the per-page form: a file per page, each answering for itself
out=$("$xpost" -q -d svgwrite -o "$tmp/g%d.svg" "$tmp/g.ps" </dev/null 2>&1)
verdict_run "$?" "$out" "the two-page svg run" || exit 1
g1=$tmp/g1.svg; g2=$tmp/g2.svg
[ -s "$g1" ] || fail "no file for the first page"
[ -s "$g2" ] || fail "no file for the second page"
for f in "$g1" "$g2"; do
    u=$(unresolved "$f" | tr '\n' ' ')
    [ -z "$u" ] || fail "$f refers to $u, which is written in no file it is read with"
done
grep -q '<mask id="xm' "$g2" || fail "the second page names a mask that only the first page's file defines"
grep -q '<pattern id="xp' "$g2" || fail "the second page names a pattern that only the first page's file defines"
grep -q '<clipPath id="xc' "$g2" || fail "the second page names a clip that only the first page's file defines"
# and the two pages are the same page: a document that holds one page
# cannot be told where in a job that page fell, so an identical page has
# to come out identically wherever it falls -- the identities included
cmp -s "$g1" "$g2" || fail "two identical pages did not produce identical documents"

# the single-file form: no %d, so every page is written to the one name
# and the file holds the last of them -- which still has to answer for
# what it refers to
out=$("$xpost" -q -d svgwrite -o "$tmp/g.svg" "$tmp/g.ps" </dev/null 2>&1)
verdict_run "$?" "$out" "the single-name svg run" || exit 1
[ -s "$tmp/g.svg" ] || fail "no file for the single-name form"
u=$(unresolved "$tmp/g.svg" | tr '\n' ' ')
[ -z "$u" ] || fail "the single-name form refers to $u, which is written in no file it is read with"
cmp -s "$tmp/g.svg" "$g2" || fail "the single-name form did not leave the last page in the file"

# A record of what has been written is kept in local memory, since a
# stencil's bits and a cell's text are; so a restore reaching over the
# end of a page puts back entries naming elements in the file that page
# closed. The save below is taken after the page is drawn and given back
# after it is emitted, which is where that lands.
cat > "$tmp/h.ps" <<'HEOF'
<< /PageSize [200 200] >> setpagedevice
/pat << /PatternType 1 /PaintType 1 /TilingType 1
        /BBox [0 0 10 10] /XStep 10 /YStep 10
        /PaintProc { pop 1 0 0 setrgbcolor 1 1 8 8 rectfill } >> matrix makepattern def
/page {
    0 setgray
    gsave 4 4 translate 8 8 scale
        8 8 true [8 0 0 -8 0 8] <FF81BDA5A5BD81FF> imagemask
    grestore
    /Pattern setcolorspace pat setpattern
    newpath 20 170 moveto 90 170 lineto 90 195 lineto 20 195 lineto closepath fill
} def
page
/sv save def
showpage
sv restore
page
showpage
HEOF
out=$("$xpost" -q -d svgwrite -o "$tmp/h%d.svg" "$tmp/h.ps" </dev/null 2>&1)
verdict_run "$?" "$out" "the restoring svg run" || exit 1
[ -s "$tmp/h2.svg" ] || fail "no file for the second page of the restoring run"
u=$(unresolved "$tmp/h2.svg" | tr '\n' ' ')
[ -z "$u" ] || fail "after a restore over the end of a page, the next page refers to $u, which is written in no file it is read with"

# --- a whole image or stencil carries the clip in force ----------------
#
# PLRM 4.8.1 paints an image only where the current clipping path admits
# it, exactly as it paints a fill. A fill arrives at this device already
# resolved against the region -- what reaches FillPoly is the part the
# clip admits and nothing else -- but an image and a stencil do not: what
# goes over is the samples and the matrix that places them, so the region
# has to travel with them or the reader paints the lot.
#
# The region goes over as a clipPath the paint's group references, which
# is the element this format has for it, and it is written for a region
# that cuts something: one covering the whole page constrains no paint,
# and a page of bitmap glyphs under no clip would otherwise carry a group
# and a reference per glyph for nothing.
f=$tmp/f.svg
cat > "$tmp/f.ps" <<'FEOF'
<< /PageSize [100 100] >> setpagedevice
0 setgray
gsave
  newpath 50 50 25 0 360 arc clip
  20 20 translate 60 60 scale
  8 8 true [8 0 0 -8 0 8] <FFFFFFFFFFFFFFFF> imagemask
grestore
showpage
FEOF
"$xpost" -q -d svgwrite -o "$f" "$tmp/f.ps" </dev/null >/dev/null 2>&1
[ -s "$f" ] || fail "the SVG writer emitted nothing for a clipped stencil"
grep -q '<clipPath id="xc' "$f"     || fail "the stencil was written with no clip at all, so a reader paints it over the whole page (PLRM 4.8.1)"
grep -q '<g clip-path="url(#xc[0-9]*)">' "$f"     || fail "the clip was written but nothing references it, so the stencil is still painted unclipped"

# and the same stencil under no clip carries none: the region is the
# whole page and constrains nothing
g=$tmp/g.svg
cat > "$tmp/g.ps" <<'GEOF'
<< /PageSize [100 100] >> setpagedevice
0 setgray
gsave 20 20 translate 60 60 scale
  8 8 true [8 0 0 -8 0 8] <FFFFFFFFFFFFFFFF> imagemask
grestore
showpage
GEOF
"$xpost" -q -d svgwrite -o "$g" "$tmp/g.ps" </dev/null >/dev/null 2>&1
grep -q '<clipPath' "$g" && fail "a stencil under the whole page was written with a clip that cuts nothing"

# The tiling walk paints a cell at a time under the region resolved to
# spans, so a cell that draws a stencil is the same case reached without
# the program ever setting such a clip itself. A cell larger than its
# step is what keeps this off the pattern element and on the walk.
h=$tmp/h.svg
cat > "$tmp/h.ps" <<'HEOF'
<< /PageSize [100 100] >> setpagedevice
<< /PatternType 1 /PaintType 1 /TilingType 1
   /BBox [0 0 20 20] /XStep 10 /YStep 10
   /PaintProc { pop 0 setgray
      gsave 20 20 scale
      8 8 true [8 0 0 -8 0 8] <FFFFFFFFFFFFFFFF> imagemask
      grestore } >>
matrix makepattern /Pattern setcolorspace setpattern
30 30 moveto 70 30 lineto 70 70 lineto 30 70 lineto closepath fill
showpage
HEOF
"$xpost" -q -d svgwrite -o "$h" "$tmp/h.ps" </dev/null >/dev/null 2>&1
[ -s "$h" ] || fail "the SVG writer emitted nothing for a pattern of stencils"
grep -q '<pattern' "$h" && fail "the fill went out as a pattern element, so this measured the route it was written to keep off"
ncell=$(grep -c 'mask="url(#xm' "$h")
ngrp=$(grep -c '<g clip-path="url(#xc' "$h")
[ "$ncell" -gt 0 ] || fail "the tiling drew no cell at all"
[ "$ncell" = "$ngrp" ] || fail "$ncell cell stencil(s) drawn against $ngrp clip group(s) -- a cell written without the region it was painted under covers the whole page (PLRM 4.8.1)"

echo "SUCCESS"
exit 0
