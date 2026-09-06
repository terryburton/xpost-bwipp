#!/bin/sh
# Where an image lands when a writer states its placement, run over the
# writers that state one.
#
# An image is stated on its own grid of samples, so the matrix carrying it
# to the page is the extent divided by the sample count -- a number that is
# almost never round. Whatever a writer keeps of that number is multiplied
# by the sample count when the far edge of the image lands, so a matrix
# written to the precision a coordinate needs puts the last row of a tall
# mask whole units from where it belongs.
#
# The property held here is the one that says so without appealing to any
# consumer: a page written and read back by this interpreter is the page it
# was written from. A mask whose scale is exact would hold that however the
# matrix were written, so the mask below is 646 by 761 over 400 by 500 --
# scales of 0.6191950 and 0.6570302, which nothing short rounds well.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-image-placement.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0

# a mask of fine horizontal stripes: a placement out by part of a row shows
# in every stripe, where a solid mask would only move at its edges
"$xpost" -q -d null -o /dev/null </dev/null >/dev/null 2>&1
cat > "$work/mk.ps" <<'EOF'
/W 646 def /H 761 def
/rb W 7 add 8 idiv def
/bits rb H mul string def
0 1 H 1 sub {
    /r exch def
    r 8 idiv 2 mod 0 eq {
        0 1 rb 1 sub { bits exch r rb mul add 16#AA put } for
    }{
        0 1 rb 1 sub { bits exch r rb mul add 16#CC put } for
    } ifelse
} for
0 setgray
100 100 translate 400 500 scale
<< /ImageType 1 /Width W /Height H /BitsPerComponent 1
   /DataSource bits /ImageMatrix [ W 0 0 H 0 0 ] /Decode [1 0] >>
imagemask
showpage
EOF

"$xpost" -q -d pgm -o "$work/direct.pgm" "$work/mk.ps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: the direct render errored"; exit 1; }

# THE PROPERTY. What a writer states, this interpreter reads back as the
# page it was given. Only dscwrite states a page in a language this can
# read, which is what makes it the one that can be asked.
"$xpost" -q -d dscwrite -o "$work/out.ps" "$work/mk.ps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: the dscwrite run errored"; fail=1; }
"$xpost" -q -d pgm -o "$work/round.pgm" "$work/out.ps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: reading the written page back errored"; fail=1; }
cmp -s "$work/direct.pgm" "$work/round.pgm" \
    || { echo "FAIL: the page a writer states is not the page it was given"; fail=1; }

# THE MECHANISM, so that a regression is caught where it happens rather
# than only where it shows. The scale is 0.61919..., and a matrix entry
# kept to the two places a coordinate needs would be written 0.62.
grep -qE '0\.6191[0-9]+ 0 0 -0\.6570[0-9]+ ' "$work/out.ps" \
    || { echo "FAIL: dscwrite states the placement too coarsely to land it"
         grep -oE '[-0-9.]+ [-0-9.]+ [-0-9.]+ [-0-9.]+ [-0-9.]+ [-0-9.]+ cm' \
             "$work/out.ps" | head -3
         fail=1; }

# and the same of the writer that states one in its own language
"$xpost" -q -d svgwrite -o "$work/out.svg" "$work/mk.ps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: the svgwrite run errored"; fail=1; }
grep -qE 'matrix\(0\.6191[0-9]+,0,0,-0\.6570[0-9]+,' "$work/out.svg" \
    || { echo "FAIL: svgwrite states the placement too coarsely to land it"
         grep -oE 'matrix\([^)]*\)' "$work/out.svg" | head -3
         fail=1; }

# SCOPE. A coordinate is not a matrix entry and keeps the short form: it
# is multiplied by nothing, so the places it does not have cost nothing and
# writing them would grow every page for no one. Without this arm the
# checks above would pass on a writer that simply wrote every number long.
cat > "$work/pl.ps" <<'EOF'
0 setgray 10.1234567 20.7654321 100 50 rectfill showpage
EOF
"$xpost" -q -d svgwrite -o "$work/pl.svg" "$work/pl.ps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: the plain-fill svgwrite run errored"; fail=1; }
grep -q '10\.12' "$work/pl.svg" \
    || { echo "FAIL: a plain coordinate is no longer written in the short form"
         grep -oE '[0-9]+\.[0-9]+' "$work/pl.svg" | sort -u | head -5
         fail=1; }
grep -qE '10\.12[0-9]' "$work/pl.svg" \
    && { echo "FAIL: a plain coordinate took the matrix form and grew the page"; fail=1; }

test $fail -eq 0 && echo "OK: an image lands where the writer said it would"
exit $fail
