#!/bin/sh
# Meson test wrapper: a page of images recorded and played back is the
# page that was painted, and the record of it is smaller than the page.
#
# This is run-record-replay-test.sh's claim for the one kind of mark a
# record cannot hold as a marking call. An image reaches a device that
# keeps its raster as rows by being written into those rows and a device
# without them one rectangle per sample, and a recorder has no such rows
# by design -- so a record built from the marking calls alone would hold
# a picture at tens of bytes a sample against the one to three bytes a
# pixel of the page it exists to avoid holding. The record holds one
# entry per image instead, and both halves of that have to be shown: the
# page comes back the same, and it comes back from something smaller
# than the page.
#
# Four ways this passes without having established anything, and what is
# done about each:
#
#   The images are not on the page. Then both runs paint the same blank
#   space and the byte comparison is between two pages of nothing. So
#   the run reports how many images the record holds and this requires
#   the number the page paints.
#
#   The images are on the page and the record expanded them. A record
#   holding a rectangle per sample replays to exactly the same bytes and
#   is the one outcome the entry exists to avoid, so the byte comparison
#   cannot see it at all. The run reports how many marks the record
#   holds and this requires a count a page of images and a page clear
#   can account for.
#
#   The record is not smaller than the page. That is the whole claim, so
#   the run reports what the record cost and what the page's raster
#   would have, and this requires the first to be the smaller.
#
#   The replay never runs. A recorder that emitted nothing would leave
#   the two files different, and the page paints, so it cannot pass by
#   emitting nothing.
#
#   $1  path to the built xpost binary
#   $2  path to record_image_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

if "$xpost" -h 2>/dev/null | grep -q -- '--no-sandbox'; then
    ns='--no-sandbox'
else
    ns=''
fi

verdict_workdir
fail=0

# how many images the page paints, and the most marks a record of it can
# hold: one entry per image, the page clear, and room for the odd mark a
# clip or a background rectangle leaves. A record holding a rectangle
# per sample is orders past this rather than a few marks past it.
images=9
maxmarks=200

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

# What the record holds and what it cost, beside what the page's raster
# costs. Both lines are the recording run's; the painting run has no
# record to ask about.
cost=$(printf '%s\n' "$played" | sed -n 's/^RECORD //p' | head -1)
raster=$(printf '%s\n' "$played" | sed -n 's/^RASTER //p' | head -1)
if [ -z "$cost" ] || [ -z "$raster" ]; then
    echo "FAILURES: the record run did not report what it recorded, so"
    echo "      nothing here establishes that anything was recorded at all"
    fail=1
else
    set -- $cost
    if [ "$#" -ne 3 ]; then
        echo "FAILURES: the record run reported $# figures and this expects 3"
        echo "      (marks, images, bytes)"
        fail=1
    else
        marks=$1
        nimg=$2
        bytes=$3
        if [ "$nimg" -lt "$images" ]; then
            echo "FAILURES: the record holds $nimg image(s) and the page paints"
            echo "      $images, so an image reached the page by some other"
            echo "      route or is not on it"
            fail=1
        fi
        if [ "$marks" -gt "$maxmarks" ]; then
            echo "FAILURES: the record holds $marks marks, past the $maxmarks a"
            echo "      page of $images images and a page clear accounts for:"
            echo "      an image was written down a rectangle per sample"
            fail=1
        fi
        if [ "$bytes" -ge "$raster" ]; then
            echo "FAILURES: the record cost $bytes bytes and the raster it saves"
            echo "      holding costs $raster, so it saves nothing"
            fail=1
        fi
        [ "$fail" -eq 0 ] && echo "OK   $marks marks, $nimg images," \
            "$bytes bytes against a raster of $raster"
    fi
fi

# The device that paints has no record to ask about, and must not be
# answering this branch: a page that reported its record through both
# devices would be reporting something other than the recorder's.
if printf '%s\n' "$direct" | grep -q '^RECORD '; then
    echo "FAILURES: the directly painted run reported a record, so the"
    echo "      comparison is not between a recorder and a painter"
    fail=1
fi

if cmp -s "$work/direct.ppm" "$work/played.ppm"; then
    echo "OK   the recorded page and the painted page are the same bytes"
else
    echo "FAILURES: a page of images played back from its record is not the"
    echo "      page that was painted directly"
    cmp "$work/direct.ppm" "$work/played.ppm" 2>&1 | sed 's/^/      /' | head -3
    echo "      $(wc -c < "$work/direct.ppm") bytes painted,"
    echo "      $(wc -c < "$work/played.ppm") bytes played back"
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
