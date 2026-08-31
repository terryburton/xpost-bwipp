#!/bin/sh
# Meson test wrapper: what the DSC writer emits is a PostScript program,
# so run it back and see whether it draws the same page.
#
# A vector writer can be wrong in two ways that byte-comparison against a
# manifest cannot see. It can emit a document that no longer says how big
# its page is, so the marks land correctly on the wrong paper; and it can
# emit one that does not run at all. Both are invisible to a hash, which
# only says the bytes are the ones recorded -- including when what was
# recorded was already wrong.
#
# So this asks the question the hash cannot: emit, run the emission, and
# compare where the ink ended up.
#
# WHAT IS COMPARED, and what turned out not to be enough. The size of the
# page comes first: a document that does not carry its own page size
# renders onto whatever medium the interpreter defaults to, and the marks
# go to the same coordinates on it -- so the BOUNDING BOX OF THE MARKS IS
# UNCHANGED and says nothing at all. That was this test's first shape and
# it passed against the very bug it was written for. The raster it renders
# to carries the page size in its own header, and that is what moves.
#
# The box is compared as well, because it catches the other half: marks
# displaced or dropped on a page of the right size. Pixels are not, for
# the usual reason -- the round trip re-renders from geometry that has
# been through decimal text, so a coordinate lands a fraction differently
# and antialiasing turns that into differing pixels; the tolerance needed
# to swallow that would swallow a regression too.
#
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: run-dsc-roundtrip-test.sh <xpost binary>}
. "$(dirname "$0")/verdict.sh"

verdict_workdir
[ -n "$work" ] && [ -d "$work" ] || {
    echo "FAILURES: could not make a scratch directory"; exit 1; }

ns=$(sandbox_flag "$xpost")

fail=0

# One page, on a medium that is not the default, carrying the constructs a
# writer has to decide what to do with. The page size is the point: a
# writer that forgets it renders this onto whatever it defaults to.
cat > "$work/src.ps" <<'PAGEEOF'
<< /PageSize [200 120] >> setpagedevice
0 setgray
10 10 moveto 60 0 rlineto 0 30 rlineto -60 0 rlineto closepath fill
gsave
  << /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 8 8]
     /XStep 8 /YStep 8 /PaintProc { pop 0 0 4 4 rectfill } >>
  matrix makepattern /Pattern setcolorspace setcolor
  100 60 60 40 rectfill
grestore
showpage
PAGEEOF

# Every run below reports through its exit status and leaves an artifact
# behind, so each is judged by verdict_run before anything reads what it
# left. A run that failed and a run that drew nothing are different
# answers, and only the judgement tells them apart -- reading the artifact
# first would report the second when it was the first.
#
# Each helper leaves its reading in a variable rather than printing it,
# because verdict_run prints its own complaint and a command substitution
# would swallow that into the reading.

box() {                 # <file> <who>  .  leaves the bbox line in $boxline
    boxline=''
    _out=$(XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d bbox -o /dev/null "$1" \
        </dev/null 2>&1)
    _st=$?
    verdict_run "$_st" "$_out" "$2" || return 1
    boxline=$(printf '%s\n' "$_out" | sed -n 's/^%%BoundingBox: //p' | head -1)
    return 0
}

# the page the emission renders onto, read off the raster's own header
size() {                # <file> <who>  .  leaves "W H" in $sizeline
    sizeline=''
    _out=$(XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d ppm -o "$work/r.ppm" "$1" \
        </dev/null 2>&1)
    _st=$?
    verdict_run "$_st" "$_out" "$2" || return 1
    [ -s "$work/r.ppm" ] || return 1
    sizeline=$(head -c 64 "$work/r.ppm" | tr '\n' ' ' | awk '{ print $2, $3 }')
    return 0
}

# <file> <out> <who>  .  emit through the writer under test
emit() {
    _out=$(XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d dscwrite -o "$2" \
        "$1" </dev/null 2>&1)
    _st=$?
    verdict_run "$_st" "$_out" "$3"
}

box "$work/src.ps" "the bounding-box run over the source page" || exit 1
want=$boxline
if [ -z "$want" ]; then
    echo "FAILURES: the source page reported no bounding box, so there is"
    echo "      nothing for the round trip to be compared against"
    exit 1
fi

emit "$work/src.ps" "$work/out.dsc" "the DSC writer over the source page" || exit 1
[ -s "$work/out.dsc" ] || { echo "FAILURES: the DSC writer emitted nothing"; exit 1; }

size "$work/src.ps" "the raster run over the source page" || exit 1
wantsize=$sizeline
if [ -z "$wantsize" ]; then
    echo "FAILURES: the source page rendered to no raster, so its size is"
    echo "      not known and the comparison below means nothing"
    exit 1
fi

if size "$work/out.dsc" "the raster run over the emitted document"; then
    gotsize=$sizeline
else
    gotsize=''
    fail=1
fi
if [ "$gotsize" != "$wantsize" ]; then
    echo "FAILURES: the emitted document renders onto a different page."
    echo "      the page:   $wantsize"
    echo "      run back:   $gotsize"
    echo "      A document that does not carry its own page size renders"
    echo "      onto the default medium; the marks keep their coordinates,"
    echo "      so only the page says so."
    fail=1
fi

if box "$work/out.dsc" "the bounding-box run over the emitted document"; then
    got=$boxline
else
    got=''
    fail=1
fi
if [ -z "$got" ]; then
    echo "FAILURES: what the DSC writer emitted draws nothing when it runs"
    fail=1
elif [ "$got" != "$want" ]; then
    echo "FAILURES: the emitted document draws its marks somewhere else."
    echo "      the page:     $want"
    echo "      run back:     $got"
    fail=1
fi

# and it says the medium in its comments, for a consumer that reads them
# rather than running the program
grep -q '^%%DocumentMedia:' "$work/out.dsc" || {
    echo "FAILURES: the emitted document declares no %%DocumentMedia, so a"
    echo "      consumer that reads the comments cannot tell the paper size"
    fail=1; }

# --- a sampled image goes out as one, and comes back the same ---------
#
# image and colorimage are LanguageLevel 1 operators, so this device emits
# them rather than decomposing a picture into per-sample fills. Two things
# have to hold and only one of them is about size.
#
# The samples are streamed with currentfile and readhexstring, which is
# how LanguageLevel 1 carries them: it has no filters at all, and a hex
# string literal long enough for a real picture is one the scanner
# refuses -- a composite object holds at most 65535 elements. An image
# emitted that way writes cleanly and then cannot be read back, so the
# picture here is deliberately past that bound.
cat > "$work/img.ps" <<'IMGEOF'
<< /PageSize [140 140] >> setpagedevice
gsave 10 10 translate 120 120 scale /DeviceRGB setcolorspace
/hb 360 string def
120 120 8 [120 0 0 -120 0 120] { currentfile hb readhexstring pop } false 3 colorimage
IMGEOF
awk 'BEGIN{ srand(7); for (r = 0; r < 120; r++) {
        line = ""
        for (c = 0; c < 360; c++) line = line sprintf("%02X", int(rand()*256))
        print line } }' >> "$work/img.ps"
printf 'grestore showpage\n' >> "$work/img.ps"

if ! emit "$work/img.ps" "$work/img.dsc" "the DSC writer over a sampled image"; then
    fail=1
elif [ ! -s "$work/img.dsc" ]; then
    echo "FAILURES: the DSC writer emitted nothing for a sampled image"
    fail=1
else
    grep -q 'colorimage' "$work/img.dsc" || {
        echo "FAILURES: the image went out as fills rather than as an image"
        echo "      operator, which LanguageLevel 1 has and which is the"
        echo "      whole reason not to decompose one"
        fail=1; }
    grep -q 'readhexstring' "$work/img.dsc" || {
        echo "FAILURES: the samples are not streamed. A hex literal cannot"
        echo "      hold a picture of this size: 65535 is the most a"
        echo "      composite object takes, and the scanner refuses more"
        fail=1; }
    # and it reads back -- which is what a literal would have failed
    box "$work/img.dsc" "the read-back run over the emitted image" || fail=1
fi

# --- a stencil goes out as a stencil ----------------------------------
#
# imagemask is a LanguageLevel 1 operator, so a stencil has a way of being
# written down and does not have to be decomposed into the runs a device
# without one is painted through. That matters most where stencils are
# most common: a Type 3 bitmap font paints one per glyph, and a page of
# them came out as thousands of little rectangles.
#
# Unlike the sampled image above, this one IS held to the pixel. A stencil
# is bits and a colour, with no decode, no interpolation and no rounding
# through decimal text -- so the emission either lands on the same pixels
# or it is wrong.
cat > "$work/mask.ps" <<'MASKEOF'
<< /PageSize [80 80] >> setpagedevice
0 setgray
gsave 10 10 translate 60 60 scale
8 8 true [8 0 0 -8 0 8] <FF81BDA5A5BD81FF> imagemask
grestore
showpage
MASKEOF

if ! emit "$work/mask.ps" "$work/mask.dsc" "the DSC writer over a stencil"; then
    fail=1
elif [ ! -s "$work/mask.dsc" ]; then
    echo "FAILURES: the DSC writer emitted nothing for a stencil"
    fail=1
else
    grep -q 'imagemask' "$work/mask.dsc" || {
        echo "FAILURES: the stencil went out as fills rather than as the"
        echo "      imagemask LanguageLevel 1 has for exactly this"
        fail=1; }
    _out=$(XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d ppm -o "$work/direct.ppm" \
           "$work/mask.ps" </dev/null 2>&1)
    _st=$?
    verdict_run "$_st" "$_out" "the raster run over the stencil source" || fail=1
    _out=$(XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d ppm -o "$work/backm.ppm" \
           "$work/mask.dsc" </dev/null 2>&1)
    _st=$?
    verdict_run "$_st" "$_out" "the raster run over the emitted stencil" || fail=1
    if [ -s "$work/direct.ppm" ] && [ -s "$work/backm.ppm" ]; then
        cmp -s "$work/direct.ppm" "$work/backm.ppm" || {
            echo "FAILURES: the emitted stencil does not paint the pixels the"
            echo "      stencil painted. A stencil is bits and a colour, so"
            echo "      there is nothing here for a rounding to explain."
            fail=1; }
    else
        echo "FAILURES: one of the stencil renders produced no raster, so the"
        echo "      comparison below would compare nothing"
        fail=1
    fi
fi

[ "$fail" = 0 ] || exit 1
echo "SUCCESS (the emitted document runs back to the same marks: $want," \
     "a sampled image goes out as one, and a stencil as a stencil)"
