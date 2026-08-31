#!/bin/sh
# Meson test wrapper: a band nothing painted into is not painted, and
# the page is the same bytes for it.
#
# The band loop plays the marks that reach a band into a raster the
# height of one, puts those rows out and moves on. Where the marks
# reaching a band come to nothing but the colour the page was cleared
# to, there is nothing there to paint: the ground is what a device
# holding no pixel over a row answers and what an emitted page carries
# there, so such a band can be passed over and the page is unchanged.
#
# Two things have to hold for that, and neither implies the other:
#
#   The bytes. A page with large empty regions must be the page a device
#   holding the whole of it paints, at more than one band height, and a
#   page painted everywhere must be unaffected. Both are checked against
#   the same page put out whole through the same device and against a
#   page painted by a device that never had a record at all.
#
#   The skipping. A band painted the ground and a band left alone carry
#   the same bytes -- that is the whole reason leaving it alone is
#   allowed -- so no comparison of pages can see whether anything was
#   skipped. What can see it is how many times the loop put a band out,
#   which the run counts by wrapping the raster class's Emit. The count
#   has to come to the bands there are, less the ones the record says
#   hold nothing but the ground, plus the one call at the end that says
#   the page is finished. A loop ignoring what the record said would
#   count one per band and fail that.
#
# The page painted everywhere is the control, and it is what keeps the
# count honest: nothing there may be skipped, so a run that reported a
# saving where there is none is caught by the page that has none.
#
#   $1  path to the built xpost binary
#   $2  path to band_sparse_test.ps
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
# a run reporting bands through both devices would not be comparing a
# band loop against a device holding the whole page.
if printf '%s\n' "$direct" | grep -qE '^CASE '; then
    note "the directly painted run reported what a band loop put out, so" \
         "the comparison is not between a band loop and a device holding" \
         "the whole page"
fi

# What the run wrote is what the program wrote: these runs name an output
# file and have no terminal on their standard input, so the interpreter
# frames nothing around it and each reported line starts a line.
lines=$banded
field() { printf '%s\n' "$lines" | sed -n "s/^$1 //p"; }

width=$(field PAGE | head -1 | awk '{ print $1 }')
if [ -z "${width:-}" ]; then
    echo "FAILURES: the banded run did not say what page it painted, so"
    echo "      nothing here can be read out of what it wrote"
    exit 1
fi

case_field() { # kind label field-number
    field CASE | awk -v k="$1" -v l="$2" -v n="$3" \
        '$1 == k && $2 == l { print $(n + 2) }'
}
roster() { # kind label
    field GROUND | awk -v k="$1" -v l="$2" '$1 == k && $2 == l { print $3 }'
}

# ---- the bytes ----
for kind in sparse full; do
    for label in 16 5 w; do
        b=$work/$kind-$label-rec.ppm
        w=$work/$kind-w-rec.ppm
        d=$work/$kind-w-dir.ppm
        miss=
        for f in "$b" "$w" "$d"; do
            [ -s "$f" ] || miss="$miss $(basename "$f")"
        done
        if [ -n "$miss" ]; then
            note "the $kind page produced nothing at$miss"
            continue
        fi
        want=$((3 * width))
        got=$(wc -c < "$b")
        if [ "$got" -le "$want" ]; then
            note "the $kind page in $label-row bands is $got bytes, which does" \
                 "not hold a page of ${width}px rows and a header"
        fi
        if cmp -s "$b" "$w" && cmp -s "$b" "$d"; then
            echo "OK   the $kind page in $label-row bands is the page held whole"
        else
            note "the $kind page put out in $label-row bands is not the page" \
                 "put out whole"
            cmp "$b" "$w" 2>&1 | sed 's/^/      /' | head -2
            cmp "$b" "$d" 2>&1 | sed 's/^/      /' | head -2
        fi
    done
done

# ---- what was skipped, and what it cost where there was nothing to
# ---- skip
for kind in sparse full; do
    for label in 16 5; do
        rows=$(case_field "$kind" "$label" 1)
        nband=$(case_field "$kind" "$label" 2)
        emits=$(case_field "$kind" "$label" 3)
        ground=$(case_field "$kind" "$label" 4)
        pat=$(roster "$kind" "$label")
        if [ -z "${rows:-}" ] || [ -z "${nband:-}" ] ||
           [ -z "${emits:-}" ] || [ -z "${ground:-}" ] || [ -z "${pat:-}" ]; then
            note "the run did not say what it did with the $kind page in" \
                 "$label-row bands"
            continue
        fi
        if [ "$nband" -lt 2 ]; then
            note "the $kind page was put out in $nband band(s); a page one" \
                 "band covers has no band nothing painted into and no" \
                 "boundary between bands on it"
            continue
        fi
        if [ "${#pat}" -ne "$nband" ]; then
            note "the $kind page in $label-row bands has $nband band(s) and" \
                 "the run said what $((${#pat})) of them come to"
            continue
        fi
        # What the loop did has to be what the record said: one call per
        # band that was painted, and one at the end that says the page is
        # finished. This is the whole gate on the skipping -- a loop that
        # painted every band regardless would count one per band here.
        expect=$((nband - ground + 1))
        if [ "$emits" -ne "$expect" ]; then
            note "the $kind page in $label-row bands has $nband band(s) of" \
                 "which $ground hold nothing but the ground, so the loop" \
                 "should have put out $expect; it put out $emits"
            continue
        fi
        if [ "$kind" = full ]; then
            # the control: ink in every band, so nothing may be skipped
            # and the count must come to every band and the last call
            if [ "$ground" -ne 0 ]; then
                note "the page painted everywhere had $ground of its $nband" \
                     "band(s) called ground; a band with ink in it is not the" \
                     "ground and skipping it would lose the ink"
            else
                echo "OK   the page painted everywhere skipped none of its" \
                     "$nband bands of $rows rows"
            fi
            continue
        fi
        if [ "$ground" -lt 1 ] || [ "$ground" -ge "$nband" ]; then
            note "the page with empty regions had $ground of its $nband" \
                 "band(s) called ground; a page all of one or all of the" \
                 "other says nothing about a loop that has to tell them apart"
            continue
        fi
        # ... and the mixture is a stated one rather than whatever fell
        # out: nothing is painted into the first band or the last, which
        # are where the file is opened and where it is finished.
        case $pat in
            y*) ;;
            *) note "the first band of the page with empty regions was" \
                    "painted, so the run does not reach a file opened by a" \
                    "band that is not the first" ;;
        esac
        case $pat in
            *y) ;;
            *) note "the last band of the page with empty regions was" \
                    "painted, so the run does not reach the rows left to the" \
                    "call that finds nothing held" ;;
        esac
        case $pat in
            *n*) ;;
            *) note "no band of the page with empty regions was painted" ;;
        esac
        echo "OK   the page with empty regions painted $((nband - ground)) of" \
             "its $nband bands of $rows rows: $pat"
    done
done

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
