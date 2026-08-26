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

ns=''
"$xpost" -h 2>/dev/null | grep -q -- '--no-sandbox' && ns='--no-sandbox'

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

box() {                 # <file>  .  the bbox line, or empty
    XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d bbox -o /dev/null "$1" \
        </dev/null 2>/dev/null | sed -n 's/^%%BoundingBox: //p' | head -1
}

want=$(box "$work/src.ps")
if [ -z "$want" ]; then
    echo "FAILURES: the source page reported no bounding box, so there is"
    echo "      nothing for the round trip to be compared against"
    exit 1
fi

XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d dscwrite -o "$work/out.dsc" \
    "$work/src.ps" </dev/null 2>"$work/err" || {
    echo "FAILURES: the DSC writer did not emit the page:"
    sed 's/^/      /' "$work/err"
    exit 1; }
[ -s "$work/out.dsc" ] || { echo "FAILURES: the DSC writer emitted nothing"; exit 1; }

# the page the emission renders onto, read off the raster's own header
size() {                # <file>  .  "W H", or empty
    XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d ppm -o "$work/r.ppm" "$1" \
        </dev/null 2>/dev/null || return 0
    [ -s "$work/r.ppm" ] || return 0
    head -c 64 "$work/r.ppm" | tr '\n' ' ' | awk '{ print $2, $3 }'
}
wantsize=$(size "$work/src.ps")
gotsize=$(size "$work/out.dsc")
if [ -z "$wantsize" ]; then
    echo "FAILURES: the source page rendered to no raster, so its size is"
    echo "      not known and the comparison below means nothing"
    exit 1
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

got=$(box "$work/out.dsc")
if [ -z "$got" ]; then
    echo "FAILURES: what the DSC writer emitted does not run, or draws"
    echo "      nothing when it does"
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

XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d dscwrite -o "$work/img.dsc" \
    "$work/img.ps" </dev/null 2>/dev/null
if [ ! -s "$work/img.dsc" ]; then
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
    if ! XPOST_NO_VM_IMAGE=1 "$xpost" -q $ns -d bbox -o /dev/null \
            "$work/img.dsc" </dev/null >/dev/null 2>&1; then
        echo "FAILURES: the emitted image does not read back"
        fail=1
    fi
fi

[ "$fail" = 0 ] || exit 1
echo "SUCCESS (the emitted document runs back to the same marks: $want," \
     "and a sampled image goes out as one)"
