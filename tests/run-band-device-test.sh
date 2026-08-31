#!/bin/sh
# Meson test wrapper: every device a record can be played into gets its
# page in bands, and the page it gets is the page it would have painted.
#
# A record holds marks and no pixels, and paints its page by playing them
# into a device that does hold pixels. Which device that is, is the one
# the run selected: -d pgm:band asks for a page held a band at a time and
# played into the grayscale raster, -d tiff:band into the TIFF one, and
# -d record, which names the class and no device, for the colour raster
# the roster defaults to. That choice is what this covers. Before
# it there was one, so a page in bands was a page in one class, and every
# other device's banding was driven by hand from a test.
#
# What is asked of each of them:
#
#   The bytes. Three pages have to agree: the page put out in bands, the
#   page the same device puts out whole, and the page a device that never
#   had a record paints as it goes. Two page heights and three band
#   heights, one of them a single row, so that a boundary between bands
#   falls on every row of the page and every mark whose reach ends part
#   way down a row is asked about.
#
#   The colour space. Two of them, because a mark carries one colour
#   value per component of the space it was made in and is played by
#   handing those values to a method whose operands the target's space
#   decides. A record that recorded in the wrong space would paint a
#   colour nobody named -- so the space each record recorded in is
#   reported and held to the space the device painting its page declares,
#   and the run is held to covering more than one of them.
#
#   The marks each band was given, wherever they can be counted. This is
#   the half no page can show. A
#   mark played into a run of rows it does not reach paints nothing, so a
#   replay handed the whole page for every band puts out exactly the page
#   a replay handed each band's own rows puts out; the pixels are equal
#   either way and the cost is the marks times the bands. What tells them
#   apart is the count the record keeps of the marks it has played, read
#   in the class's row writer, which a device puts a run of finished rows
#   through once per band: no band may have been played every mark the
#   page has, the bands must differ from one another, and their total
#   must stay near the marks rather than near the marks times the bands.
#
#   That count is available where the class assembles the page. A device
#   whose page is written from compiled code never reaches the class's
#   row writer, so its bands are compared as pages and counted by nobody;
#   the run says which kind each device is and this reports it rather
#   than leaving a device looking counted.
#
#   $1  path to the built xpost binary
#   $2  path to band_device_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

# Each run is started in a directory of its own, so a page names the same
# file whichever device wrote it and the two are compared across the
# directories rather than through a name carrying the device.
xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

# $1 the device selection, $2 the directory it runs in. Answers 2 where
# the device is not built into this interpreter, which is what an
# optional member may legitimately say.
render() {
    mkdir -p "$work/$2" || return 1
    r_out=$( cd "$work/$2" && "$xpost" -q $ns -d "$1" "$script" \
             </dev/null 2>&1 )
    r_st=$?
    case "$r_out" in
        *"wrong device"*) return 2 ;;
    esac
    verdict_run "$r_st" "$r_out" "the $1 run" || return 1
    printf '%s\n' "$r_out" > "$work/$2.log"
    return 0
}

# $1 directory, $2 field name; prints the field's lines
field() { sed -n "s/^$2 //p" "$work/$1.log"; }

# The devices a record can be played into: DEVICE_FLEET_BANDS, which is
# the recording class's own roster of play targets and is held to it by
# check-device-roster.sh. It is read rather than listed here, because a
# list here is one a device added to the class's roster is not on -- and
# then it is played into by nothing, with the class saying it can be and
# no test saying it is not.
#
# Each is rendered twice: once with the record between the page and it,
# and once by itself. Selecting one of these by name selects banding, so
# the run that wants the device by itself asks for the mode that holds
# the page whole. The comparison is between the two routes, and naming
# the device alone would now name the same route twice.
#
# The bilevel device is among them, and is reached because the roster is
# derived rather than chosen: it is the one that shows a grey as a
# pattern of pixels, and a record standing in front of it has to say so.
# What a record writes down is what the machinery above it was willing
# to send, and the machinery sends a screening device whole pixels
# rather than a glyph's coverage-weighted edges. A recorder that did not
# carry the screen would be sent blends, write them down, and play them
# into a device whose pixel cannot hold one -- so the page the bilevel
# device puts out in bands would not be the page it puts out whole. The
# page below raises the coverage the device is asked for above what the
# bilevel one takes, which is what puts that decision on the route
# rather than leaving it unreached.
formats=
unbuilt=
for f in $DEVICE_FLEET_BANDS; do
    render "$f:band" "rec-$f"; rc=$?
    if [ "$rc" -eq 2 ]; then
        unbuilt="$unbuilt $f"
        echo "SKIP $f (not built in)"
        continue
    fi
    [ "$rc" -eq 0 ] || fail=1
    render "$f:whole" "dir-$f" || fail=1
    formats="$formats $f"
done
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

# A device that could not be asked is one this build has no library for,
# and nothing else. A roster that skipped from end to end compares
# nothing and reports the same SUCCESS as one that held.
floor=0
for f in $DEVICE_FLEET_BANDS; do
    case " $DEVICE_FLEET_OPTIONAL " in *" $f "*) continue ;; esac
    floor=$((floor + 1))
done
nran=0
for f in $formats; do nran=$((nran + 1)); done
if [ "$nran" -lt "$floor" ]; then
    echo "FAILURES: $nran of the play targets were rendered and at least"
    echo "      $floor of them are built from this tree; the rest said they"
    echo "      were not built in, which is a build to fix"
    exit 1
fi
for f in $unbuilt; do
    case " $DEVICE_FLEET_OPTIONAL " in
        *" $f "*) ;;
        *) echo "FAILURES: $f could not be asked and needs no optional library"
           exit 1 ;;
    esac
done

nspace=$(for f in $formats; do field "rec-$f" NCOMP; done | sort -u | wc -l)
if [ "$nspace" -lt 2 ]; then
    note "the records this ran cover $nspace colour space(s); one space" \
         "says nothing about a record taking the space of the device its" \
         "page is played into"
fi

for f in $formats; do
    rec=$(field "rec-$f" NCOMP)
    dir=$(field "dir-$f" NCOMP)
    play=$(field "rec-$f" PLAY)
    direct=$(field "dir-$f" PLAY)
    if [ -z "${rec:-}" ] || [ -z "${dir:-}" ]; then
        note "the $f runs did not say what colour a mark carries"
        continue
    fi
    if [ "$play" != "yes" ] || [ "$direct" != "no" ]; then
        note "-d $f:band gave a device with no record to play, or" \
             "-d $f:whole gave one with a record; the comparison is not" \
             "between a recorded page and a painted one"
        continue
    fi
    if [ "$rec" != "$dir" ]; then
        note "a record played into $f holds $rec colour value(s) a mark" \
             "where $f takes $dir; every mark it plays puts a value in the" \
             "place of a different one"
        continue
    fi
    echo "OK   a record played into $f holds the $rec colour value(s) $f takes"

    # Whether the marks a band was played can be counted here at all: the
    # count is taken in the class's row writer, and a device whose page is
    # written from compiled code never reaches it.
    counted=$(field "rec-$f" ASSEMBLY)
    if [ "${counted:-}" != class ] && [ "${counted:-}" != compiled ]; then
        note "the $f run did not say whether its page is assembled by the" \
             "class, so nothing here knows what its band counts mean"
        continue
    fi
    [ "$counted" = compiled ] &&
        echo "NOTE $f assembles its page in compiled code, so the marks each" \
             "band was played are counted by nobody here; its pages are" \
             "compared like any other's"

    # The page put out whole through the record, against the page the
    # device paints with no record at all.
    field "rec-$f" WHOLE | while read -r h name marks; do
        [ -n "${h:-}" ] || continue
        w=$work/rec-$f/$name
        d=$work/dir-$f/$(printf '%s\n' "$name" | sed 's/-rec\.out$/-dir.out/')
        if [ ! -s "$w" ] || [ ! -s "$d" ]; then
            echo "FAILURES: the $f page of $h rows produced nothing"
            continue
        fi
        if [ "${marks:-0}" -lt 20 ]; then
            echo "FAILURES: the $f page of $h rows came to $marks mark(s);"
            echo "      a page with that little on it says nothing about"
            echo "      which of them a band was given"
            continue
        fi
        cmp -s "$w" "$d" ||
            echo "FAILURES: the $f page of $h rows recorded and played back"
    done > "$work/wholeout"
    nwhole=$(field "rec-$f" WHOLE | grep -c . || true)
    if [ -s "$work/wholeout" ]; then
        cat "$work/wholeout"
        fail=1
    elif [ "${nwhole:-0}" -lt 1 ]; then
        echo "FAILURES: $f put out no whole page at all; the comparisons"
        echo "      above were made over an empty list and held nothing"
        fail=1
    else
        echo "OK   $f: $nwhole recorded page(s) put out whole are the page painted"
    fi

    # ... and the page put out a band at a time, against both of those.
    field "rec-$f" BAND | while read -r h rows name nb nrows sum max min; do
        [ -n "${h:-}" ] || continue
        b=$work/rec-$f/$name
        w=$work/rec-$f/whole-$h-rec.out
        d=$work/dir-$f/whole-$h-dir.out
        marks=$(sed -n "s/^WHOLE $h .* //p" "$work/rec-$f.log")
        if [ ! -s "$b" ]; then
            echo "FAILURES: the $f page of $h rows in bands of $rows"
            echo "      produced nothing"
            continue
        fi
        if ! cmp -s "$b" "$w" || ! cmp -s "$b" "$d"; then
            echo "FAILURES: the $f page of $h rows put out in bands of $rows"
            echo "      is not the page put out whole"
            continue
        fi
        # ... and everything below is read out of the class's row writer,
        # which a page assembled in compiled code does not go through
        [ "$counted" = compiled ] && continue
        # The page was divided, and divided into runs that between them
        # are the page: every row of it reached the writer, once.
        if [ "$nb" -lt 2 ]; then
            echo "FAILURES: the $f page of $h rows reached the writer in $nb"
            echo "      run(s) at $rows rows a band; a page one run covers"
            echo "      has no boundary between bands on it"
            continue
        fi
        if [ "$nrows" -ne "$h" ]; then
            echo "FAILURES: the $f page of $h rows handed the writer $nrows"
            echo "      row(s) at $rows rows a band"
            continue
        fi
        # ... and the marks each run was played. A replay handed the
        # whole page for every band paints this same page and pays the
        # marks times the bands for it, so these are the numbers that
        # tell the two apart.
        if [ "$max" -ge "$marks" ]; then
            echo "FAILURES: a band of the $f page of $h rows was played all"
            echo "      $marks of its marks at $rows rows a band, so the"
            echo "      rows a band asked for are the page's and not the"
            echo "      band's"
            continue
        fi
        if [ "$max" -le "$min" ]; then
            echo "FAILURES: every band of the $f page of $h rows was played"
            echo "      the same $max mark(s) at $rows rows a band"
            continue
        fi
        # ... and by a margin, against what a replay ignoring its row
        # range would pay, which is every mark once per band. Stated as a
        # fraction of that rather than as a multiple of the page's marks,
        # because how many bands a mark meets is how tall the mark is: a
        # page whose marks reach its full height costs nearly as much
        # either way, so a multiple of the marks measures the page's
        # shape and not the replay's bound.
        if [ "$sum" -ge $((marks * nb)) ] ||
           [ $((sum * 3)) -gt $((marks * nb * 2)) ]; then
            echo "FAILURES: the $f page of $h rows played $sum mark(s) over"
            echo "      $nb bands of $rows rows, against $marks in the page;"
            echo "      what a band replay costs is following the bands"
            echo "      rather than the drawing"
            continue
        fi
    done > "$work/bandout"
    nband=$(field "rec-$f" BAND | grep -c . || true)
    if [ -s "$work/bandout" ]; then
        cat "$work/bandout"
        fail=1
    elif [ "${nband:-0}" -lt 3 ]; then
        # Every check on a banded page is inside the loop above, so a run
        # that emitted no BAND line runs none of them and reads exactly
        # like a clean one. The run bands each page at three heights, so
        # three is what there is to find.
        echo "FAILURES: $f put out ${nband:-0} banded page(s) and the run bands"
        echo "      each page three ways; the checks above were made over an"
        echo "      empty list and held nothing"
        fail=1
    elif [ "$counted" = class ]; then
        echo "OK   $f: $nband banded pages are the page put out whole, each" \
             "played the marks its own rows meet"
    else
        echo "OK   $f: $nband banded pages are the page put out whole"
    fi
done

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS ($nran device(s) a record was played into)"
exit 0
