#!/bin/sh
# Meson test wrapper: a recorded page of images played into a device
# that keeps no rows.
#
# tests/run-record-image-test.sh holds the image entry itself, through
# the devices that keep their page as rows the interpreter can see. This
# is the same claim for the devices that do not -- the compiled writers,
# whose raster is a buffer of their own. Such a device has nowhere an
# image can be written into, so the picture is painted through the
# rectangle fill it declares: one call per run of columns, the runs
# being the ones the row writer would have covered.
#
# Four claims, and no two of them imply each other:
#
#   The bytes. A page of images played out of a record into one of these
#   devices is the page that device paints directly, byte for byte, at
#   several band heights including a band of one row -- which is where
#   every row of the page is a boundary between bands, and where a
#   replay that got its rows wrong at the ends of an image shows it.
#
#   The pixels. Byte equality above compares a device against itself, so
#   both sides could round the same way and both be wrong. What holds
#   the two routes to each other is the same page through a device that
#   keeps rows and through one that keeps a buffer, decoded and compared
#   pixel for pixel: the sampling is one writer's and a second set of
#   rounding decisions would show here and nowhere else.
#
#   What the bands were asked to paint. A mark or an image played into a
#   run of rows it does not reach paints nothing, so a replay handed the
#   whole page for every band puts out exactly the page a replay handed
#   each band's own rows puts out. No comparison of pages can see the
#   difference; what can is what the record was asked to play, which the
#   record counts. Two counts, because one of them alone passes a replay
#   that gets the rows wrong: a page held whole plays each image once and
#   a page in bands plays it once per band it reaches, which says the
#   bands were separate replays but nothing about what each was handed;
#   and the sample rows those replays put through the image writer, which
#   is what says each was handed its own part of the picture. A replay
#   painting the whole picture into every band plays it exactly as often
#   as a right one does, and writes the picture's rows every time.
#
#   The bound. These devices keep their raster outside the memory the
#   interpreter reports on, so what is weighed is the process: the peak
#   resident size of a run at a short page and a tall one. The
#   whole-page route is required to grow by a row of pixels for every
#   row of page, since a measurement that saw nothing would pass
#   everything, and the banded route by a small fraction of that. The
#   reading is the process's and not the page's, so it carries whatever
#   else the machine was doing; the section itself says what is done
#   about that.
#
#   $1  path to the built xpost binary
#   $2  path to record_span_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# The runs below are started in the directory the pages are written to,
# so what they were handed has to name the same thing from there.
xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0
ran=0

# One run: the device selection, the output name, and the definitions.
# Sets out to what the run said.
run() {  # $1 device; $2 output; $3... -Dname=value
    r_dev=$1
    r_out=$2
    shift 2
    out=$( cd "$work" && "$xpost" -q $ns -d "$r_dev" -o "$r_out" "$@" \
           "$script" </dev/null 2>&1 )
    r_st=$?
    verdict_run "$r_st" "$out" "the $r_dev run" || return 1
    verdict_ok "$out" "the $r_dev run" || return 1
    return 0
}

field() { printf '%s\n' "$1" | tr -s '-' '\n' | sed -n "s/^$2 //p" | head -1; }

# Whether a device is in this build at all: one that is not answers
# nothing rather than a page.
have() {  # $1 device
    "$xpost" -q $ns -d "$1" -o /dev/null /dev/null </dev/null >/dev/null 2>&1
}

# ---- the bytes, and what the bands were asked to paint ----
for dev in png jpeg; do
    if ! have "$dev"; then
        echo "SKIP the $dev device is not in this build"
        continue
    fi
    run "$dev" "direct.out" || { note "$dev could not paint the page"; continue; }
    run "$dev:band" "whole.out" -DBAND=0 || {
        note "$dev could not play a recorded page held whole"
        continue
    }
    wholeplays=$(field "$out" PLAYS)
    wholerows=$(field "$out" IMGROWS)
    if [ -z "${wholerows:-}" ] || [ "$wholerows" -lt 1 ]; then
        note "the record put ${wholerows:-no} sample row(s) through the" \
             "image writer holding the $dev page whole, so there is" \
             "nothing for a band's share of a picture to be read against"
        wholerows=0
    fi
    if [ -z "${wholeplays:-}" ] || [ "$wholeplays" -lt 7 ]; then
        note "the record played ${wholeplays:-no} image(s) into the $dev" \
             "page held whole, and the page paints seven; an image reached" \
             "the page by some other route or is not on it"
    fi
    if ! cmp -s "$work/direct.out" "$work/whole.out"; then
        note "the $dev page played out of a record held whole is not the" \
             "page that device painted"
        cmp "$work/direct.out" "$work/whole.out" 2>&1 | sed 's/^/      /' | head -2
    else
        echo "OK   $dev: the page held whole is the page painted directly"
    fi

    onerow=0
    for band in 1 3 7 64; do
        run "$dev:band" "band-$band.out" -DBAND=$band || {
            note "$dev could not play a recorded page in bands of $band"
            continue
        }
        ran=$((ran + 1))
        [ "$band" = 1 ] && onerow=1
        plays=$(field "$out" PLAYS)
        played=$(field "$out" PLAYED)
        imgrows=$(field "$out" IMGROWS)
        if cmp -s "$work/direct.out" "$work/band-$band.out"; then
            echo "OK   $dev: the page in bands of $band rows is the page" \
                 "painted directly"
        else
            note "the $dev page played out of a record in bands of $band" \
                 "rows is not the page that device painted"
            cmp "$work/direct.out" "$work/band-$band.out" 2>&1 |
                sed 's/^/      /' | head -2
        fi
        # ... and that the bands were bands. An image is played once per
        # band it reaches, so a shallower band plays it more times; a
        # replay quietly handed the whole page for every band would paint
        # this same page and could only be seen here.
        if [ -z "${plays:-}" ] || [ -z "${played:-}" ]; then
            note "the $dev run in bands of $band did not say what the" \
                 "record was played, so nothing here says the bands were" \
                 "bands"
        elif [ "$plays" -le "$wholeplays" ]; then
            note "the $dev record played $plays image(s) in bands of $band" \
                 "rows and $wholeplays holding the page whole; an image" \
                 "crossing a band boundary is played once per band it" \
                 "reaches, so the rows asked for are the page's and not" \
                 "the band's"
        else
            echo "OK   $dev: $plays image plays and $played mark plays in" \
                 "bands of $band rows, against $wholeplays held whole"
        fi
        # ... and that each of those plays was handed its band's share of
        # the picture. The rows a run of the page's rows takes are the
        # samples reaching it, and the chooser keeps two rows either side
        # of them for the blend and one for the rounding -- so over a
        # page a replay costs the picture's rows once, plus at most six
        # for each band it was played into. A replay handed the whole
        # picture every time costs the picture's rows per band, which is
        # past that as soon as a picture is more than a few rows tall.
        limit=$((wholerows + 6 * plays))
        if [ -z "${imgrows:-}" ]; then
            note "the $dev run in bands of $band did not say how many" \
                 "sample rows went through the image writer, so nothing" \
                 "here says a band was handed its own part of a picture"
        elif [ "$imgrows" -gt "$limit" ]; then
            note "the $dev record put $imgrows sample row(s) through the" \
                 "image writer in bands of $band rows, past the $limit a" \
                 "page of $wholerows row(s) played into $plays band(s)" \
                 "accounts for; a band is being handed more of a picture" \
                 "than reaches it"
        else
            echo "OK   $dev: $imgrows sample row(s) through the image" \
                 "writer in bands of $band rows, within the $limit a" \
                 "band's own share accounts for"
        fi
    done
    if [ "$onerow" -ne 1 ]; then
        note "no $dev page was put out in bands of one row, so no boundary" \
             "between bands fell inside an image"
    fi
    rm -f "$work"/*.out
done

if [ "$ran" -eq 0 ]; then
    # A device that is absent from the build is a skip. A device that is
    # PRESENT and could not complete the run is a failure, and the notes
    # above say what it was -- so the skip is only reachable where nothing
    # has been noted. Exiting 77 here regardless is how this suite came to
    # report a page it could not paint as a capability the build lacks:
    # neither red nor green, so nobody looked.
    if [ "$fail" -ne 0 ]; then
        echo "FAILURES: no device completed the run, and every device this" \
             "suite asks for is in the build -- the notes above are the" \
             "reason, not an absent capability"
        exit 1
    fi
    echo "SKIP no device keeping its raster in a buffer of its own is in" \
         "this build"
    exit 77
fi

# ---- the pixels ----
# The same page through a device that keeps rows and through one that
# keeps a buffer, decoded and compared. Both are played out of a record,
# so what is compared is the two tails of the one writer rather than a
# record against a painter.
if have png; then
    run ppm:band "rows.ppm" -DBAND=7 || note "the rows route wrote no page"
    run png:band "spans.png" -DBAND=7 || note "the span route wrote no page"
    if [ -s "$work/rows.ppm" ] && [ -s "$work/spans.png" ]; then
        if run null "/dev/null" -DCMP=1; then
            echo "OK   $(field "$out" PIXELS) pixel bytes compared, none" \
                 "differing"
        else
            note "the page painted through a device's rows and the page" \
                 "painted through its own fill are not the same pixels"
        fi
    else
        note "one of the two routes wrote nothing to compare"
    fi
    rm -f "$work/rows.ppm" "$work/spans.png"
fi

# ---- the bound ----
#
# What is read here is the process and not the page, so it carries the
# machine. Peak resident size is what the machine let a run hold, and a
# run sharing the machine with others reads a little away from what it
# holds alone: measured over thirty rounds of each of the four readings
# with sixteen tests running beside them, every reading moved by up to
# 560 KiB in both directions, two to five per cent of itself. The
# whole-page route grows by a raster and reads straight through that.
# The band route grows by almost nothing, so most of what its two
# readings differ by is that movement -- and the tenth of the whole
# page's growth it is allowed has to stand well clear of it, or the
# verdict is the machine's rather than the code's.
#
# Two things put it clear. The tall page is tall enough that a tenth of
# what the whole-page route grows by is a couple of mebibytes: over the
# seven thousand rows between the two heights of a page a thousand wide
# the whole-page route grows by 21 to 27 MiB, by device, and the band
# route by under a fifth of one, which leaves it two mebibytes of room
# against readings that move by a quarter of that. And the four
# readings are taken in rounds, the round kept being the one whose band
# route came off best against its own whole-page run. A round the
# machine interfered with is not evidence about the code, and what a
# kept round says is that the machine managed the measurement once,
# which is all the machine is being asked for. Neither hides a route
# that follows the page: such a route reads the whole page's own growth
# in every round, which is ten times what any of this moves by.
#
# Each reading is a run and is judged like one. A run that died on the
# way took little, and little is exactly the reading the band route is
# here to produce.
if peak_rss_reads "$xpost"; then
    lo=1000; hi=8000; rounds=3
    peak() {  # $1 selection; $2 height; $3 band; sets peakkib
        peakkib=''
        p_out=$( cd "$work" && /usr/bin/time -f '%M' -o peak.rss \
                 "$xpost" -q $ns -d "$1" -o /dev/null \
                 -DPW=1000 -DPH="$2" -DBAND="$3" "$script" \
                 </dev/null 2>&1 )
        p_st=$?
        if ! verdict_run "$p_st" "$p_out" "the $1 run at $2 rows" ||
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$p_out" "the suite"
           ! verdict_ok "$p_out" "the $1 run at $2 rows"; then
            note "the $1 run at $2 rows did not put out its page, so what" \
                 "it took is not what putting one out takes"
            return 1
        fi
        peakkib=$(tail -1 "$work/peak.rss")
        case ${peakkib:-} in
            ''|*[!0-9]*)
                note "the $1 run at $2 rows did not report what it took"
                return 1 ;;
        esac
        return 0
    }
    for dev in png jpeg; do
        have "$dev" || continue
        kept=no
        round=0
        while [ "$round" -lt "$rounds" ]; do
            round=$((round + 1))
            peak "$dev:band" "$lo" 0  || break
            r_wlo=$peakkib
            peak "$dev:band" "$hi" 0  || break
            r_whi=$peakkib
            peak "$dev:band" "$lo" 64 || break
            r_blo=$peakkib
            peak "$dev:band" "$hi" 64 || break
            r_bhi=$peakkib
            # the room this round leaves the check below, which is that
            # check's own comparison written as a difference: what the
            # whole page grew by, less ten times what the band route
            # grew by. The rows between the two page heights are the
            # same for both routes and divide out of it.
            r_spare=$(( (r_whi - r_wlo) - 10 * (r_bhi - r_blo) ))
            if [ "$kept" = no ] || [ "$r_spare" -gt "$spare" ]; then
                kept=yes
                spare=$r_spare
                wlo=$r_wlo; whi=$r_whi; blo=$r_blo; bhi=$r_bhi
            fi
        done
        [ "$kept" = yes ] || continue
        rows=$((hi - lo))
        # what each route took for each further row of page, in bytes;
        # the timer reports kibibytes, and the scaling keeps a slope
        # below one byte a row a number here
        wslope=$((1000 * (whi - wlo) * 1024 / rows))
        bslope=$((1000 * (bhi - blo) * 1024 / rows))
        echo "OK   held: $dev recorded whole ${wlo} -> ${whi} KiB over" \
             "$lo -> $hi rows, best of $rounds"
        echo "OK   held: $dev recorded in bands ${blo} -> ${bhi} KiB over" \
             "$lo -> $hi rows, best of $rounds"
        # The measurement has to be able to see a page at all. A row of
        # this raster is three or four bytes a pixel over a page a
        # thousand wide, so most of that must show up here or the
        # instrument is not reading the raster.
        if [ "$wslope" -lt $((700 * 3 * 1000)) ]; then
            note "playing a record into a whole $dev page grew by" \
                 "$wslope/1000 bytes a row where a row of it is at least" \
                 "$((3 * 1000)) bytes; the measurement is not seeing the" \
                 "raster, so it cannot say the band bounds it"
        else
            echo "OK   playing a record into the whole $dev page grows by" \
                 "$wslope/1000 bytes a row"
        fi
        if [ $((bslope * 10)) -ge "$wslope" ]; then
            note "the $dev band route grew by $bslope/1000 bytes a" \
                 "row against the whole page's $wslope/1000; what it holds" \
                 "is following the page's height, so the band is not" \
                 "bounding it"
        else
            echo "OK   the $dev band route grows by $bslope/1000" \
                 "bytes a row, under a tenth of it"
        fi
    done
else
    echo "SKIP $peak_rss_why, so what the bands hold is not weighed here"
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
