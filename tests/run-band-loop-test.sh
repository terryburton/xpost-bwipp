#!/bin/sh
# Meson test wrapper: a page put out a band at a time is the page put
# out whole, and what it holds while it does it is the band.
#
# The band loop makes a raster the height of a band, plays the marks
# that reach that band into it, puts those rows out and moves on. Two
# things have to hold for that to be worth anything, and neither implies
# the other:
#
#   The bytes. A page assembled from bands must be the page a device
#   holding the whole of it paints. That is checked three ways over: the
#   banded page against the same page put out whole through the same
#   device, and both against a page painted by a device that never had a
#   record at all.
#
#   The bound. A loop that quietly held the whole page anyway -- because
#   the rows were never given back, or because the emission accumulated
#   them -- paints exactly the same bytes, so no comparison of pages can
#   see it. What can see it is how much memory the transmission took,
#   which the run reports for both routes at three page heights. The
#   whole-page route is required to grow with the page, since a
#   measurement that saw nothing would pass everything, and the banded
#   route is required to grow by a small fraction of that: what still
#   grows with the page there is one slot per row of the array of rows,
#   which is the row references and not the rows.
#
# Two more things a run of this reports, and why each is here:
#
#   Which classes say their page may arrive in bands. The answer defaults
#   to no, and the two classes that derive from the two that say yes do
#   so by dict copy -- so the way this rule breaks is by inheritance,
#   silently, and the roster is checked rather than assumed. The derived
#   two say yes here because each has earned it and not because it was
#   copied; what holds their pages to it is tests/run-band-format-test.sh.
#
#   A grayscale page put out band by band. The colour class is the one
#   the record plays into, so nothing above reaches the grayscale one;
#   the run drives it directly, offering the same marks to each run of
#   rows, and its page must be the page one raster holding all the rows
#   was painted with. Most of that page is raster nothing painted, so it
#   also holds the moved raster to starting each run as a fresh raster
#   would rather than carrying the run before's ink into it.
#
#   $1  path to the built xpost binary
#   $2  path to band_loop_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
skip_if_faceless "$xpost" "this run shows text through a face"

# The runs below are started in the directory the pages are written to,
# so what they were handed has to name the same thing from there.
xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

render() {  # $1 device; sets out
    out=$( cd "$work" && "$xpost" -q $ns -d "$1" -o page.ppm "$script" \
           </dev/null 2>&1 )
    st=$?
    verdict_run "$st" "$out" "the $1 run" || return 1
    return 0
}

render record || fail=1
banded=$out
# The device holding the whole page, asked for as the mode that says so:
# selecting a device by name selects the record in front of it, and the
# comparison here is between a band loop and a device holding the page.
render ppm:whole || fail=1
direct=$out

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

# The device that paints has no band loop and must not be reporting one:
# a run that reported memory through both devices would be reporting
# something other than the loop's.
if printf '%s\n' "$direct" | tr -s '-' '\n' | grep -qE '^MEM '; then
    note "the directly painted run reported what a band loop took, so the" \
         "comparison is not between a band loop and a device holding the" \
         "whole page"
fi

# The showpage banner of the default page semantics ends without a
# newline, so a reported line can arrive with it on the front.
lines=$(printf '%s\n' "$banded" | tr -s '-' '\n')
field() { printf '%s\n' "$lines" | sed -n "s/^$1 //p"; }

width=$(field PAGE | head -1)
rowbytes=$(field ROWBYTES | head -1)
bandbytes=$(field BANDBYTES | head -1)
if [ -z "${width:-}" ] || [ -z "${rowbytes:-}" ] || [ -z "${bandbytes:-}" ]; then
    echo "FAILURES: the banded run did not say what page it painted, so"
    echo "      nothing here can be read out of what it wrote"
    exit 1
fi
bandrows=$((bandbytes / rowbytes))
if [ "$bandrows" -lt 2 ]; then
    note "a band holds $bandrows row(s), so the page was not divided by one"
fi

# ---- the bytes ----
heights=$(field MEM | awk '$1 == "band" { print $2 }')
if [ -z "$heights" ]; then
    echo "FAILURES: the banded run put out no page in bands"
    exit 1
fi
nheight=0
for h in $heights; do
    nheight=$((nheight + 1))
    nbands=$(( (h + bandrows - 1) / bandrows ))
    if [ "$nbands" -lt 2 ]; then
        note "the page of $h rows was put out in $nbands band(s); a page one" \
             "band covers has no boundary between bands on it and every" \
             "arrangement of marks passes on one"
    fi
    b=$work/band-$h-rec.ppm
    w=$work/whole-$h-rec.ppm
    d=$work/whole-$h-dir.ppm
    for f in "$b" "$w" "$d"; do
        if [ ! -s "$f" ]; then
            note "the page of $h rows produced nothing at $(basename "$f")"
            continue 2
        fi
    done
    want=$((3 * width * h))
    got=$(wc -c < "$b")
    if [ "$got" -le "$want" ]; then
        note "the banded page of $h rows is $got bytes, which does not hold" \
             "a ${width}x$h page of three bytes a pixel and a header"
    fi
    if cmp -s "$b" "$w" && cmp -s "$b" "$d"; then
        echo "OK   the page of $h rows in $nbands bands is the page held whole"
    else
        note "the page of $h rows put out in $nbands bands is not the page" \
             "put out whole"
        cmp "$b" "$w" 2>&1 | sed 's/^/      /' | head -2
        cmp "$b" "$d" 2>&1 | sed 's/^/      /' | head -2
    fi
done
if [ "$nheight" -lt 2 ]; then
    note "$nheight page height(s) were put out in bands; one height says" \
         "nothing about whether what is held follows the page"
fi

# The same page in a band of one row, where every row of it is a boundary
# between bands. A band deep enough to hold both sides of a mark's end
# never asks whether the reach written down for that mark covers the row
# the end falls part way down; at one row a band it is asked of every
# mark on the page.
fine=$(printf '%s\n' "$heights" | head -1)
if [ ! -s "$work/fine-$fine-rec.ppm" ]; then
    note "the page of $fine rows was not put out in bands of one row, so" \
         "no boundary between bands falls inside a mark's end"
elif cmp -s "$work/fine-$fine-rec.ppm" "$work/whole-$fine-dir.ppm"; then
    echo "OK   the page of $fine rows in bands of one row is the page painted" \
         "whole"
else
    note "the page of $fine rows put out in bands of one row is not the page" \
         "painted whole"
    cmp "$work/fine-$fine-rec.ppm" "$work/whole-$fine-dir.ppm" 2>&1 \
        | sed 's/^/      /' | head -2
fi

# ---- the bound ----
# Read at the shortest page and the tallest, so what is compared is how
# each route grows with the page rather than what either costs once.
mem() { field MEM | awk -v k="$1" -v h="$2" '$1 == k && $2 == h { print $3 }'; }
lo=$(printf '%s\n' $heights | head -1)
hi=$(printf '%s\n' $heights | tail -1)
bandlo=$(mem band "$lo"); bandhi=$(mem band "$hi")
wholelo=$(mem whole "$lo"); wholehi=$(mem whole "$hi")
if [ -z "${bandlo:-}" ] || [ -z "${bandhi:-}" ] ||
   [ -z "${wholelo:-}" ] || [ -z "${wholehi:-}" ]; then
    note "the run did not report what both routes took at both heights, so" \
         "there is nothing to compare"
elif [ "$hi" -le "$lo" ]; then
    note "the pages measured are not of different heights"
else
    rows=$((hi - lo))
    # what each route took for each further row of page, in bytes; scaled
    # so that a slope below one byte a row is still a number here
    bandslope=$((1000 * (bandhi - bandlo) / rows))
    wholeslope=$((1000 * (wholehi - wholelo) / rows))
    echo "OK   held: band $bandlo -> $bandhi bytes over $lo -> $hi rows"
    echo "OK   held: whole $wholelo -> $wholehi bytes over $lo -> $hi rows"
    # The measurement has to be able to see a page at all. A whole-page
    # raster is three bytes a pixel, so its growth per row is the page's
    # width in pixels times three, and most of that must show up here or
    # the instrument is not reading the raster.
    if [ "$wholeslope" -lt $((800 * 3 * width)) ]; then
        note "holding the whole page grew by $wholeslope/1000 bytes a row" \
             "where a row of it is $((3 * width)) bytes; the measurement is" \
             "not seeing the raster, so it cannot say the band bounds it"
    else
        echo "OK   holding the whole page grows by $wholeslope/1000 bytes a row"
    fi
    # ... and the banded route must not. What still grows with the page
    # is one slot per row of the array of rows -- the references, not the
    # rows -- which is a small fraction of a row of pixels.
    if [ $((bandslope * 10)) -ge "$wholeslope" ]; then
        note "the band route grew by $bandslope/1000 bytes a row against the" \
             "whole page's $wholeslope/1000; what it holds is following the" \
             "page's height, so the band is not bounding it"
    else
        echo "OK   the band route grows by $bandslope/1000 bytes a row," \
             "under a tenth of it"
    fi
    # and the plainest statement of the same thing: the tallest page in
    # bands holds less than the shortest page held whole
    if [ "$bandhi" -ge "$wholelo" ]; then
        note "the tallest page in bands held $bandhi bytes and the shortest" \
             "page held whole took $wholelo; a band that is not smaller than" \
             "a page bounds nothing"
    else
        echo "OK   the tallest page in bands holds less than the shortest" \
             "page held whole"
    fi
fi

# ---- what a class says about taking its page in bands ----
decl() { field DECL | awk -v c="$1" '$1 == c { print $2 }'; }
for c in .xpost_PGMIMAGE:yes .xpost_PPMIMAGE:yes \
         .xpost_PBMIMAGE:yes .xpost_TIFFIMAGE:yes; do
    cls=${c%%:*}; want=${c#*:}
    got=$(decl "$cls")
    if [ -z "${got:-}" ]; then
        note "the run did not say whether $cls takes its page in bands"
    elif [ "$got" != "$want" ]; then
        note "$cls says $got to taking its page in bands and this expects" \
             "$want; a class that has not thought about it must say nothing," \
             "and a dict copy of one that has carries the answer"
    else
        echo "OK   $cls takes its page in bands: $got"
    fi
done

# ---- a run of rows given up holds no row ----
held=$(field HELD)
if [ -z "$held" ]; then
    note "the run reported no move from one run of rows to the next"
else
    nheld=0
    printf '%s\n' "$held" | while read -r top before after; do
        [ -n "${top:-}" ] || continue
        if [ "$before" -ne 0 ]; then
            echo "FAILURES: after moving to row $top the row above it still"
            echo "      holds $before pixels; the rows given up are still held"
            echo "      and the raster is the page rather than a band"
        fi
        if [ "$after" -le 0 ]; then
            echo "FAILURES: after moving to row $top that row holds no pixel"
        fi
    done > "$work/heldout"
    if [ -s "$work/heldout" ]; then
        cat "$work/heldout"
        fail=1
    else
        nheld=$(printf '%s\n' "$held" | wc -l)
        echo "OK   $nheld moves gave up the rows they held and took new ones"
    fi
fi

# ---- the grayscale page, put out band by band against one held whole ----
grey=$(field GREY | head -1)
if [ -z "${grey:-}" ]; then
    note "the run did not put out a grayscale page a band at a time"
else
    gn=$(printf '%s\n' "$grey" | awk '{ print $3 }')
    if [ "${gn:-0}" -lt 2 ]; then
        note "the grayscale page was put out in ${gn:-0} band(s)"
    fi
    if [ ! -s "$work/pgm-band.pgm" ] || [ ! -s "$work/pgm-whole.pgm" ]; then
        note "the grayscale run produced no page"
    elif cmp -s "$work/pgm-band.pgm" "$work/pgm-whole.pgm"; then
        echo "OK   a grayscale page put out in $gn bands is one put out whole"
    else
        note "a grayscale page put out in $gn bands is not the page put out" \
             "whole"
        cmp "$work/pgm-band.pgm" "$work/pgm-whole.pgm" 2>&1 \
            | sed 's/^/      /' | head -2
    fi
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
