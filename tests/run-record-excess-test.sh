#!/bin/sh
# Meson test wrapper: what a record costs against what banding saves.
#
# Writing a page down instead of painting it buys the raster the page
# does not have to hold, and that raster is the whole of what it buys:
# the run less one band of it, because a page held in bands still holds a
# band. What the marks cost follows the drawing and has no bound at all,
# so a page can be drawn whose record costs more than the raster it is
# saving, and that page is held in bands anyway and pays both.
#
# This is where that page is drawn and the two sides are put beside each
# other. Nothing here is a verdict on a page: a page put out band by band
# and the same page put out whole are the same bytes, which is the
# requirement the whole mechanism is under and is also why every other
# reading is asked of the interpreter instead.
#
# Two arms, because the boundary and the cost are different questions.
#
#   the boundary  a page a little past the band budget saves one band's
#                 worth of rows and no more, which is the shape a run
#                 meets by accident -- every page a little too large for
#                 the budget is one. A light drawing on it keeps a record
#                 well under the saving and a heavier one carries the
#                 record past it, so both directions are asked, and asked
#                 at each colour count a record is made in. A check that
#                 only ever saw records exceed the saving would pass a
#                 tree where every record did.
#
#   the cost      the same comparison at full size, on a sheet at print
#                 resolution carrying a drawing of small filled
#                 rectangles. Here the peak resident size of the two runs
#                 is read as well, so what the excess comes to is on the
#                 record rather than inferred, and it is held to the one
#                 thing that bounds it: the excess is the record less
#                 what the band raster saves against the whole one, so it
#                 cannot be more than the record itself.
#
# The figures are printed whether or not anything is wrong with them.
# What the excess comes to is the measurement this exists to keep, and a
# run that reported only a verdict would leave a later reader nothing to
# compare against.
#
#   $1  path to the built xpost binary
#   $2  path to record_excess_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

datadir=${XPOST_DATA_DIR:-}
if [ -z "$datadir" ]; then
    echo "FAILURES: no XPOST_DATA_DIR; the run has no boot files to break"
    exit 1
fi
datadir=$(path_anchor "$datadir")
rundata=$datadir

verdict_workdir
fail=0
checks=0

note() {
    echo "FAILURES: $1"
    shift
    for n_line in "$@"; do
        echo "      $n_line"
    done
    checks=$((checks + 1))
    fail=1
}

ok() {
    echo "OK   $*"
    checks=$((checks + 1))
}

# How much is drawn in each arm. The light drawing is far under what the
# small page's banding saves and the heavy one far past it, so neither
# side of the boundary rests on a few bytes either way; the large page's
# grid is the drawing the cost below was measured on.
LIGHT=50
HEAVY=2000
GRID=202500

# What the meter may be out by, in kibibytes. The peak resident size of a
# process carries the machine it was read on: the same reading elsewhere
# in this suite has been seen to move by most of a mebibyte between runs.
# The quantity it is added to here is the record, which on this page is
# nineteen of them.
SLACK=4096

# run SELECTION DIR M BIG [OUTFILE] -- one interpreter in a directory of
# its own.
#
# Judged on what it left and on what it said, which are separate answers:
# a run that wrote its page and then died on the way out has left every
# figure the checks below read.
run() {
    r_sel=$1; r_dir=$2; r_m=$3; r_big=$4; r_out=${5:-/dev/null}
    rm -rf "$r_dir"; mkdir -p "$r_dir" || return 1
    ( cd "$r_dir" && XPOST_DATA_DIR=$rundata "$xpost" -q -d "$r_sel" \
        -o "$r_out" -DM="$r_m" -DBIG="$r_big" "$script" </dev/null ) \
        >"$r_dir/out.txt" 2>&1
    r_st=$?
    verdict_run "$r_st" "$(cat "$r_dir/out.txt" 2>/dev/null)" \
        "the $r_sel run" || return 1
    grep -q '^DONE' "$r_dir/out.txt" || {
        echo "FAILURES: the $r_sel run did not finish its page"
        return 1
    }
    return 0
}

# weigh SELECTION DIR M BIG -- the same, with the peak resident size of
# the process read from the timer. Sets peakkib.
weigh() {
    peakkib=''
    w_sel=$1; w_dir=$2
    rm -rf "$w_dir"; mkdir -p "$w_dir" || return 1
    ( cd "$w_dir" && /usr/bin/time -f '%M' -o rss.txt \
        "$xpost" -q -d "$w_sel" -o /dev/null \
        -DM="$3" -DBIG="$4" "$script" </dev/null ) \
        >"$w_dir/out.txt" 2>&1
    w_st=$?
    verdict_run "$w_st" "$(cat "$w_dir/out.txt" 2>/dev/null)" \
        "the $w_sel weighing" || return 1
    grep -q '^DONE' "$w_dir/out.txt" || {
        echo "FAILURES: the $w_sel weighing did not finish its page"
        return 1
    }
    peakkib=$(tail -1 "$w_dir/rss.txt" 2>/dev/null)
    case ${peakkib:-x} in *[!0-9]*) peakkib='' ;; esac
    [ -n "$peakkib" ]
}

# What one run said.
said() {
    awk -v k="$2" '$1 == k { print $2 }' "$1/out.txt" | head -1
}
pageh() {
    awk '$1 == "PAGE" { print $3 }' "$1/out.txt" | head -1
}

# ---- the boundary, at each colour count a record is made in ----
# The raster classes written in PostScript that take a page a band at a
# time -- the fleet's banders less the ones that assemble a page in
# compiled code. Each writes a different arrangement of pixels, which is
# what makes them worth walking here: the boundary this measures is where
# a record stops being cheaper than the page, and that turns on what a
# page costs per pixel.
RECDEVS=
for e_dev in $DEVICE_FLEET_BANDS; do
    case " $DEVICE_FLEET_BUFFER " in *" $e_dev "*) continue ;; esac
    RECDEVS="$RECDEVS $e_dev"
done
for dev in $RECDEVS; do
    lite=$work/$dev-lite
    heav=$work/$dev-heavy
    dirl=$work/$dev-dirlite
    dirh=$work/$dev-dirheavy
    run "$dev:band" "$lite" "$LIGHT" 0 page.pgm || {
        note "the $dev light recording run failed"; continue; }
    run "$dev:band" "$heav" "$HEAVY" 0 page.pgm || {
        note "the $dev heavy recording run failed"; continue; }
    run "$dev:whole" "$dirl" "$LIGHT" 0 page.pgm || {
        note "the $dev light direct run failed"; continue; }
    run "$dev:whole" "$dirh" "$HEAVY" 0 page.pgm || {
        note "the $dev heavy direct run failed"; continue; }

    b_h=$(pageh "$heav")
    b_band=$(said "$heav" BAND)
    b_save=$(said "$heav" SAVING)
    b_lite=$(said "$lite" COST)
    b_heav=$(said "$heav" COST)
    if [ -z "${b_h:-}" ] || [ -z "${b_band:-}" ] || [ -z "${b_save:-}" ] \
        || [ -z "${b_lite:-}" ] || [ -z "${b_heav:-}" ]; then
        note "the $dev runs named no page, band, saving or cost"
        continue
    fi

    # The page has to have banded, or every figure below is the figure
    # for a page that was held whole all along.
    if [ "$b_band" -gt 0 ] && [ "$b_band" -lt "$b_h" ]; then
        ok "$dev: the page is held in bands of $b_band rows of $b_h"
    else
        note "the $dev page was to band and is held in $b_band rows of" \
             "$b_h; nothing here is about banding"
    fi
    if [ "$b_save" -gt 0 ]; then
        ok "$dev: banding the page saves $b_save bytes"
    else
        note "the $dev page bands and is said to save $b_save bytes by it"
    fi

    # The route that never records is the control: it saves nothing
    # because it holds no record, and a reading that came out the same on
    # both routes is a reading of something other than the record.
    if [ "$(said "$dirh" ROUTE)" = false ]; then
        ok "$dev: the direct route reached no record"
    else
        note "the $dev direct route reached a record"
    fi

    # Both directions of the comparison. A record smaller than the saving
    # is buying what it costs; one larger than it is not, and that is the
    # case this test exists to keep on the record.
    if [ "$b_lite" -lt "$b_save" ]; then
        ok "$dev: a light drawing holds $b_lite bytes of record, under" \
           "the $b_save banding it saves"
    else
        note "the $dev light drawing holds $b_lite bytes of record against" \
             "a saving of $b_save; the light side of the boundary is gone"
    fi
    if [ "$b_heav" -gt "$b_save" ]; then
        ok "$dev: a heavy drawing holds $b_heav bytes of record, past the" \
           "$b_save banding it saves"
    else
        note "the $dev heavy drawing holds $b_heav bytes of record and does" \
             "not reach the $b_save banding it saves; the case is not drawn"
    fi

    # ... and what makes it unbounded: the drawing grew and the saving
    # did not. The saving is the page, which is fixed; the record is the
    # drawing, which is not.
    if [ "$(said "$lite" SAVING)" = "$b_save" ]; then
        ok "$dev: the saving is the same $b_save bytes for both drawings"
    else
        note "the $dev saving moved from $(said "$lite" SAVING) to $b_save" \
             "between two drawings on the same page"
    fi
    if [ "$b_heav" -gt "$b_lite" ]; then
        ok "$dev: the record grew from $b_lite to $b_heav bytes with the" \
           "drawing"
    else
        note "the $dev record went from $b_lite to $b_heav bytes for a" \
             "drawing forty times the size"
    fi

    # and the pages, which is the requirement the mechanism is under:
    # where the pixels were formed is not something a page shows
    e_same=1
    for pair in "$lite $dirl light" "$heav $dirh heavy"; do
        set -- $pair
        if [ ! -s "$1/page.pgm" ]; then
            note "the $dev $3 recording run wrote no page"
            e_same=0
            continue
        fi
        cmp -s "$1/page.pgm" "$2/page.pgm" || {
            note "the $dev $3 page differs between the route that recorded" \
                 "it and the route that painted it directly"
            e_same=0
        }
    done
    [ "$e_same" -eq 1 ] && ok "$dev: both pages are the same bytes on both" \
                              "routes"
done

# ---- and the cost, at full size ----
# The peak resident size of a process is what answers here: the raster of
# a device keeping a buffer of its own is not interpreter memory, so the
# interpreter's own count of what it took cannot see the page at all.
if peak_rss_reads "$xpost"; then
    if weigh pgm:whole "$work/cost-whole" "$GRID" 1 \
        && c_whole=$peakkib \
        && weigh pgm:band "$work/cost-band" "$GRID" 1 \
        && c_band=$peakkib; then
        c_save=$(said "$work/cost-band" SAVING)
        c_cost=$(said "$work/cost-band" COST)
        c_rows=$(said "$work/cost-band" BAND)
        c_h=$(pageh "$work/cost-band")
        c_kib=$(( c_cost / 1024 ))
        c_excess=$(( c_band - c_whole ))
        echo "COST $(pageh "$work/cost-band") rows, banded in $c_rows:" \
             "record $c_cost bytes, saving $c_save bytes;" \
             "peak resident whole $c_whole KiB, banded $c_band KiB," \
             "excess $c_excess KiB"

        if [ "$c_rows" -gt 0 ] && [ "$c_rows" -lt "$c_h" ]; then
            ok "cost: the page is held in bands of $c_rows rows of $c_h"
        else
            note "the cost page was to band and is held in $c_rows rows of" \
                 "$c_h, so its excess is not banding's"
        fi
        if [ "$c_cost" -gt "$c_save" ]; then
            ok "cost: the record comes to $c_cost bytes against the" \
               "$c_save banding the page saves"
        else
            note "the cost page holds $c_cost bytes of record against a" \
                 "saving of $c_save; the adverse case is not drawn"
        fi
        # What bounds the excess. Banding holds one band where holding
        # the page whole holds the page, and it holds the marks -- but
        # only until they come to more than the raster it is saving,
        # past which they go into a file and stop being resident. So the
        # two routes come to the same peak, and this is the reading the
        # whole mechanism exists to produce: a page whose marks are worth
        # four times what banding it saves costs no more banded than
        # whole.
        if [ "$c_excess" -le "$SLACK" ]; then
            ok "cost: banding the page peaks $c_excess KiB over holding it" \
               "whole, inside the $SLACK KiB the meter may be out by," \
               "with $c_kib KiB of marks held"
        else
            note "banding the cost page peaks $c_excess KiB over holding it" \
                 "whole, past the $SLACK KiB the meter may be out by; the" \
                 "$c_kib KiB of marks are being paid for twice"
        fi

        # ... and that it stays there as the drawing grows. The excess
        # used to be the record, which follows the drawing, so doubling
        # the drawing doubled it. What is asked here is that doubling the
        # drawing moves the peak by nothing: the marks are bounded now,
        # and a bound that grew with the drawing would not be one.
        if weigh pgm:band "$work/cost-band2" $(( GRID * 2 )) 1 \
            && c_band2=$peakkib; then
            c_grow=$(( c_band2 - c_band ))
            [ "$c_grow" -lt 0 ] && c_grow=$(( -c_grow ))
            echo "COST doubled: $(said "$work/cost-band2" COST) bytes of" \
                 "marks, peak resident banded $c_band2 KiB against" \
                 "$c_band KiB for half the drawing"
            if [ "$c_grow" -le "$SLACK" ]; then
                ok "cost: doubling the drawing moves the banded peak by" \
                   "$c_grow KiB, inside the $SLACK KiB the meter may be" \
                   "out by"
            else
                note "doubling the drawing moved the banded peak by" \
                     "$c_grow KiB, past the $SLACK KiB the meter may be" \
                     "out by; what a banded page costs is following the" \
                     "drawing again"
            fi
        else
            note "the cost page could not be weighed at twice the drawing"
        fi
    else
        note "the cost page could not be weighed on both routes"
    fi
else
    echo "SKIP $peak_rss_why, so what banding the cost page is out by" \
         "is not weighed"
fi

# ---- and the comparison broken on purpose ----
# The boot files are copied and the copy is broken, so the interpreter
# under test is the one that ships. What is broken is the statement of
# what banding saves, which is the side of the comparison this file reads
# from the interpreter; each break is required to put one of the two
# directions above the wrong way round. A break that leaves both readings
# where they were says the arm was reading something else.
#
# Only the boundary arm is broken, and only on one device. A break is
# about which reading a check is taking, and that is the same question at
# every colour count and at every page size.
sab() {  # $1 what; $2 tag; $3 line to append to device.ps; $4 direction
    s_what=$1; s_tag=$2; s_line=$3; s_dir=$4
    s_data=$work/data-$s_tag
    rm -rf "$s_data"
    if ! cp -R "$datadir" "$s_data"; then
        note "could not copy the boot files to break"
        return
    fi
    { echo 'currentglobal true setglobal'
      echo "$s_line"
      echo 'setglobal'; } >>"$s_data/device.ps"

    rundata=$s_data
    s_ok=0
    if run pgm:band "$work/sab-$s_tag" \
        "$([ "$s_dir" = light ] && echo "$LIGHT" || echo "$HEAVY")" 0 \
        >/dev/null 2>&1
    then
        s_save=$(said "$work/sab-$s_tag" SAVING)
        s_cost=$(said "$work/sab-$s_tag" COST)
        case $s_dir in
            light) [ -n "${s_save:-}" ] && [ -n "${s_cost:-}" ] \
                   && [ "$s_cost" -lt "$s_save" ] && s_ok=1 ;;
            heavy) [ -n "${s_save:-}" ] && [ -n "${s_cost:-}" ] \
                   && [ "$s_cost" -gt "$s_save" ] && s_ok=1 ;;
        esac
    else
        # a tree broken past running at all leaves nothing to read, which
        # the arm above would fail on too
        s_ok=0
    fi
    rundata=$datadir

    if [ "$s_ok" -eq 0 ]; then
        ok "$s_what is caught"
    else
        note "$s_what and the $s_dir side of the boundary held anyway;" \
             "it is not reading what it says it is"
    fi
}

sab "a saving so large that no record is ever past it" never \
    '.xpostsys /.bandsaving { pop 2000000000 } bind put' heavy
sab "a saving so small that every record is past it" always \
    '.xpostsys /.bandsaving { pop 1 } bind put' light

# The count of what was asked, so that a run which asked nothing fails
# rather than reporting a clean tree. Three devices at eight checks
# apiece, and four more where the meter can be read.
if [ "$checks" -lt 26 ]; then
    note "the wrapper made $checks checks; a run this size makes" \
         "twenty-six or more, so it was not asking what it says it asks"
fi

verdict_exit
