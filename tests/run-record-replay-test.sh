#!/bin/sh
# Meson test wrapper: a page recorded and played back is the same page.
#
# The recording device writes down the marks a page makes instead of
# making them, and paints the page at Emit by playing what it wrote into
# a device that does paint. So the claim is a byte one, and it is made
# against a directly painted page rather than against another run of the
# recorder: the same program, on the same page, through the device that
# paints it in one go and through the device that writes it down first.
#
# Three ways this passes without having established anything, and what
# is done about each:
#
#   The replay never runs. A recorder that emitted nothing would leave
#   the two files different, but a recorder that emitted a blank page
#   over a program that painted nothing would not -- so the page is one
#   that paints, and the run is required to say what it recorded.
#
#   The record is empty. The counts come from playing the record into a
#   device that counts, so a recorder that recorded nothing counts
#   nothing and the run says so.
#
#   Only one kind of mark is exercised. The page reaches all five, and
#   the run prints the count of each; the count line is held to naming
#   five non-zero counts here as well as in the program, so a program
#   edit that stopped reaching one cannot pass by having stopped
#   checking too.
#
#   $1  path to the built xpost binary
#   $2  path to record_replay_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
skip_if_faceless "$xpost" "this run shows text through a face"

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

# $1 device, $2 output path; sets out to what the run said
render() {
    out=$("$xpost" -q $ns -d "$1" -o "$2" "$script" </dev/null 2>&1)
    st=$?
    verdict_run "$st" "$out" "the $1 run" || return 1
    if [ ! -s "$2" ]; then
        echo "FAILURES: the $1 run produced no page"
        return 1
    fi
    return 0
}

# The painter by itself, asked for as the mode that holds the page whole:
# selecting a device by name selects the record in front of it, and the
# comparison here is between a recorder and a painter.
render ppm:whole "$work/direct.ppm" || fail=1
direct=$out
render record "$work/played.ppm" || fail=1
played=$out

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

# What the recorder recorded. The counts are the five marking kinds and
# then the subpath separators the polygons carried, which is the part of
# a polygon a flat record can silently lose.
counts=$(printf '%s\n' "$played" | sed -n 's/^MARKS //p' | head -1)
if [ -z "$counts" ]; then
    echo "FAILURES: the record run did not report what it recorded, so"
    echo "      nothing here establishes that anything was recorded at all"
    fail=1
else
    n=0
    zero=
    for c in $counts; do
        n=$((n + 1))
        [ "$c" -gt 0 ] || zero="$zero $n"
    done
    if [ "$n" -ne 6 ]; then
        echo "FAILURES: the record run reported $n counts and this expects 6"
        echo "      (five marking kinds and the subpath separators)"
        fail=1
    elif [ -n "$zero" ]; then
        echo "FAILURES: the page reached nothing at position(s)$zero of"
        echo "      putpix blendpix drawline fillrect fillpoly separators"
        fail=1
    else
        echo "OK   recorded: $counts"
    fi
fi

# The device that paints has no record to ask about, and must not be
# answering this branch: a page that reported its marks through both
# devices would be reporting something other than the recorder's.
if printf '%s\n' "$direct" | grep -q '^MARKS '; then
    echo "FAILURES: the directly painted run reported a record, so the"
    echo "      comparison is not between a recorder and a painter"
    fail=1
fi

if cmp -s "$work/direct.ppm" "$work/played.ppm"; then
    echo "OK   the recorded page and the painted page are the same bytes"
else
    echo "FAILURES: a page played back from its record is not the page that"
    echo "      was painted directly"
    cmp "$work/direct.ppm" "$work/played.ppm" 2>&1 | sed 's/^/      /' | head -3
    echo "      $(wc -c < "$work/direct.ppm") bytes painted,"
    echo "      $(wc -c < "$work/played.ppm") bytes played back"
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
