#!/bin/sh
# Meson test wrapper: the band budget, set from the invocation.
#
# How deep a band is is a byte budget divided by what one row of the
# raster costs, and the same number decides whether a page is held as its
# marks at all: a page the budget covers arrives in one band, which is
# the page, so it is painted directly and nothing is written down. So the
# budget is the one control over both, and what this holds is that it
# reaches both from the command line.
#
# What is asked:
#
#   the budget is reported     MaxBandBytes says what the run named, and
#                              four million where it named nothing
#   a small budget bands       an ordinary page, at the bare device name,
#                              with no boot file touched. This is the
#                              point of the whole thing: without it the
#                              only banded page is one too large to hold,
#                              and a run comparing the routes at an
#                              ordinary page compares a page with itself
#   the two readings agree     the rows reported are the budget divided
#                              by what a row costs, and nought where that
#                              buys the page. A run reporting a budget it
#                              did not band to would pass either reading
#                              alone
#   the page does not change   the bytes a banded page puts out are the
#                              bytes the same page held whole puts out,
#                              at every budget down to one row a band
#   the refusals               a budget that will not buy one row of the
#                              page is refused when the device is made,
#                              and one that is not a whole number of
#                              bytes in range is refused before the run
#                              begins. Each names what was given
#   the marks are bounded      a record is held to the budget as well as
#                              to what banding saves, so a page whose
#                              bands save nothing -- one reached with
#                              :band under the budget -- still stops
#                              growing. --spill=never is the way to ask
#                              for the other thing
#
# The controls, which this runs on itself at the end and requires to
# fail: the budget reported as a constant, the rows reported as a
# constant, the route always read as recorded, a refusal that accepts
# everything, and a record that never says it spilled. A check that could
# not see those five would be reading nothing.
#
#   $1  path to the built xpost binary
#   $2  path to tests/band_budget_test.ps
#
# and, for the controls it invokes on itself:
#
#   --sabotage N $1 $2   run a reduced pass with defect N built in, and
#                        answer as it finds it. The caller requires a
#                        failure.
set -u

sab=0
case ${1:-} in
    --sabotage) sab=$2; shift 2 ;;
esac
xpost=${1:?usage: run-band-budget-test.sh [--sabotage N] <xpost> <test.ps>}
script=${2:?usage: run-band-budget-test.sh [--sabotage N] <xpost> <test.ps>}
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
skip_if_faceless "$xpost" "this run shows text through a face"
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")
data=${XPOST_DATA_DIR:-$(cd "$(dirname "$0")/../data" && pwd)}

verdict_workdir
fail=0

# run PAGE MARKS DEVICE ARG... -- run the interpreter over the test
# program, leaving what it printed in $out, how it ended in $st and its
# page in $page. Nothing is judged here: half of what is asked below is
# an invocation that must not be accepted.
#
# The device is a parameter of its own rather than one of the arguments
# that follow, so that every run made here names one where it is
# written. A run naming none takes the device the build was configured
# with, which is whichever the libraries found allowed: on a machine
# where the window system was found that is a window on whoever's screen
# the run was started from, once per invocation.
run() {
    page=$work/$1.pgm
    r_marks=$2
    r_dev=$3
    shift 3
    out=$("$xpost" -q --no-sandbox -DMARKS="$r_marks" -o "$page" -d "$r_dev" \
          "$@" "$script" </dev/null 2>&1)
    st=$?
}

# field NAME -- what the run reported under that name, from the line the
# program prints its readings on.
field() {
    printf '%s\n' "$out" | sed -n "s/.*[ ]*$1 \([^ ]*\).*/\1/p" | head -1
}

# reading TAG PAGE MARKS DEVICE ARG... -- run, require the run to have
# got as far as reporting, and leave the readings in $f_budget, $f_rows,
# $f_route, $f_carries, $f_rowbytes, $f_spill and $f_record.
reading() {
    r_tag=$1
    shift
    run "$@"
    verdict_run "$st" "$out" "the $r_tag run" || { fail=1; return 1; }
    f_budget=$(field BUDGET); f_rows=$(field ROWS)
    f_route=$(field ROUTE);   f_carries=$(field CARRIES)
    f_rowbytes=$(field ROWBYTES)
    f_spill=$(printf '%s\n' "$out" | sed -n 's/^SPILL [^ ]* \([^ ]*\).*/\1/p' \
              | head -1)
    f_record=$(field RECORD)
    [ "$sab" -eq 1 ] && f_budget=4000000
    [ "$sab" -eq 2 ] && f_rows=0
    [ "$sab" -eq 3 ] && f_route=record
    [ "$sab" -eq 5 ] && f_spill=memory
    if [ -z "$f_budget" ] || [ -z "$f_rows" ] || [ -z "$f_route" ]; then
        note "the $r_tag run reported no readings to check" \
             "$(printf '%s\n' "$out" | tail -3)"
        return 1
    fi
    return 0
}

# want TAG NAME GOT WANTED
want() {
    [ "$3" = "$4" ] && return 0
    note "the $1 run reported $2 $3 where it should have reported $4"
    return 1
}

echo "== the budget is what the invocation named, and is reported =="

# A page at the default extent, which is under the default budget: the
# ordinary page the whole complaint is about.
if reading default default 200 pgm; then
    want default BUDGET "$f_budget" 4000000
    want default ROWS "$f_rows" 0
    want default ROUTE "$f_route" direct
    rowbytes=$f_rowbytes
    echo "OK   a page under the default budget is painted directly, at" \
         "$rowbytes bytes a row"
else
    rowbytes=612
fi

if reading named named 200 pgm --band-bytes=100000; then
    want named BUDGET "$f_budget" 100000
    want named CARRIES "$f_carries" 100000
    echo "OK   the budget the invocation named is the budget reported"
fi

echo "== a small budget bands an ordinary page =="

# Every band depth from a sixteenth of the page down to a single row, at
# the bare device name -- the spelling that is weighed -- so that what is
# shown is the weighing changing its mind and not a mode overruling it.
banded_budgets=""
for rows in 163 40 7 1; do
    budget=$((rowbytes * rows))
    banded_budgets="$banded_budgets $budget"
    if reading "$rows-row" "b$rows" 200 pgm --band-bytes="$budget"; then
        want "$rows-row" ROUTE "$f_route" record
        want "$rows-row" ROWS "$f_rows" "$rows"
        want "$rows-row" BUDGET "$f_budget" "$budget"
        # and the two readings are one arithmetic: what was reported as
        # bought is what the budget buys at the row price reported
        [ "$f_rows" = "$((f_budget / f_rowbytes))" ] ||
            note "a $rows-row band was reported at a budget of $f_budget" \
                 "and a row of $f_rowbytes, which do not divide to it"
    fi
done
[ "$fail" -eq 0 ] &&
    echo "OK   an ordinary page bands at 163, 40, 7 and 1 rows, no file patched"

echo "== the page does not change with the budget =="

# The page held whole is the comparison, and it is reached by the mode
# that says so rather than by a budget, so the two sides of the
# comparison are not both made by the thing under test.
run whole 200 pgm:whole
verdict_run "$st" "$out" "the whole-page run" || fail=1
if [ -s "$work/whole.pgm" ]; then
    for budget in $banded_budgets; do
        run "cmp$budget" 200 pgm --band-bytes="$budget"
        if ! verdict_run "$st" "$out" "the $budget-byte run"; then
            fail=1
        elif ! cmp -s "$work/whole.pgm" "$work/cmp$budget.pgm"; then
            note "the page banded at a budget of $budget bytes differs" \
                 "from the same page held whole"
        fi
    done
    [ "$fail" -eq 0 ] &&
        echo "OK   every banded budget puts out the whole page's bytes"
else
    note "the whole-page run wrote no page to compare against"
fi

echo "== what is refused =="

# refused TAG WORD PATTERN -- the invocation must not be accepted, and
# what it says must name what it was given.
refused() {
    run "r$1" 20 pgm --band-bytes="$2"
    [ "$sab" -eq 4 ] && st=1 && out="no such budget: $2 1 to 2147483647"
    if [ "$st" -eq 0 ]; then
        note "--band-bytes=$2 was accepted" "$(printf '%s\n' "$out" | tail -2)"
        return
    fi
    case $out in
        *"$2"*) ;;
        *) note "--band-bytes=$2 was refused without naming what it was given" \
                "$(printf '%s\n' "$out" | tail -2)"; return ;;
    esac
    case $out in
        *"$3"*) ;;
        *) note "the refusal of --band-bytes=$2 does not say what the range is" \
                "wanted: $3" "$(printf '%s\n' "$out" | tail -2)"; return ;;
    esac
    echo "OK   --band-bytes=$2 is refused, naming what was given"
}

# below one row of the page in hand: refused where the device is made,
# naming the budget and what a row costs, rather than taken as one row
refused subrow "$((rowbytes - 1))" "$rowbytes"
# and outside what a budget may be at all: refused before the run begins
refused zero 0 2147483647
refused negative -5 2147483647
refused word lots 2147483647
refused trailing 100x 2147483647
refused huge 99999999999 2147483647

echo "== a budget belongs to the run and not to the language =="

# The language a run is given may have been read whole out of an image of
# the virtual memory another run built it in, and that other run's
# invocation is not this one's. So the image is written by a run at one
# budget and read by runs at two others, one of which names none: each
# has to get its own, and the one that names none has to get the default
# rather than whatever the run that wrote the file was given.
img=$work/vm.img
# The suite builds the language rather than reading a cached image; this
# block is the one place that wants the reading, and says so.
unset XPOST_NO_VM_IMAGE
XPOST_VM_IMAGE_WRITE=$img XPOST_VM_IMAGE=$img \
    "$xpost" -q --no-sandbox -DMARKS=5 -d pgm --band-bytes=6120 \
    -o "$work/img0.pgm" "$script" </dev/null >/dev/null 2>&1
if [ -s "$img" ]; then
    XPOST_VM_IMAGE=$img
    export XPOST_VM_IMAGE
    if reading "imaged 1224" i1 5 pgm --band-bytes=1224; then
        want "imaged 1224" BUDGET "$f_budget" 1224
        want "imaged 1224" ROWS "$f_rows" 2
    fi
    if reading "imaged default" i2 5 pgm; then
        want "imaged default" BUDGET "$f_budget" 4000000
        want "imaged default" ROUTE "$f_route" direct
    fi
    unset XPOST_VM_IMAGE
    [ "$fail" -eq 0 ] &&
        echo "OK   a language read out of an image still bands to this run's" \
             "budget"
else
    echo "OK   (skipped: this build wrote no virtual memory image)"
fi

echo "== a record is bounded by the budget as well as by the saving =="

# A page reached with :band under the budget arrives in one band, which
# is the page: it saves nothing, so what banding saves says nothing about
# it, and only the budget stands between its record and the drawing. The
# page is small and the budget covers it, so the saving really is nought
# and the bound really is the only one being read.
small="-g 100x100+0+0 --band-bytes=20000"
if reading unbounded heavy 600 pgm:band $small; then
    want unbounded ROWS "$f_rows" 0
    want unbounded ROUTE "$f_route" record
    want unbounded SPILL "$f_spill" file
    echo "OK   a page saving nothing stops growing at the budget" \
         "(record $f_record bytes)"
fi
if reading light light 20 pgm:band $small; then
    want light SPILL "$f_spill" memory
    echo "OK   ... and a page under the budget is not put in a file"
fi
if reading never never 600 pgm:band $small --spill=never; then
    want never SPILL "$f_spill" memory
    echo "OK   ... and never is still never"
fi

# A page of text is the page the bound reaches differently. A glyph does
# not arrive as a mark: the record holds one coverage mask per distinct
# glyph and a placement apiece, so what a page of text mostly holds is a
# table the marks point into -- and a record that has put what it holds
# in a file has to go on answering from there. Both ways of getting there
# are asked, since either may be the one a page arrives by, and both are
# held to the page the same run puts out holding its marks in memory:
# where a page's marks were kept is not something a page shows.
run textmem 400 pgm:band --spill=never -DTEXT=1
verdict_run "$st" "$out" "the in-memory text run" || fail=1
if [ -s "$work/textmem.pgm" ]; then
    for how in "--spill=always" "--band-bytes=20000"; do
        run textspill 400 pgm:band $how -DTEXT=1
        if ! verdict_run "$st" "$out" "the text run at $how"; then
            fail=1
        elif ! cmp -s "$work/textmem.pgm" "$work/textspill.pgm"; then
            note "the page of text whose marks went to a file at $how" \
                 "differs from the same page holding them in memory"
        else
            echo "OK   a page of text is the same page once its marks are in" \
                 "a file ($how)"
        fi
    done
else
    note "the in-memory text run wrote no page to compare against"
fi

# ---- the controls
#
# Each breaks one reading and requires this to notice. Two are broken in
# the interpreter, by a copy of the boot files with the reading
# overridden, and three in the wrapper, where the reading is taken.

if [ "$sab" -eq 0 ]; then
    echo "== the controls =="

    control() {  # $1 what; $2 tag; rest: arguments to the reduced pass
        c_what=$1
        shift
        if "$self" --sabotage "$1" "$xpost" "$script" >"$work/sab.out" 2>&1
        then
            note "$c_what and the check passed anyway"
            sed -n '1,6p' "$work/sab.out" | sed 's/^/      /'
        else
            echo "OK   $c_what is caught"
        fi
    }

    control "a budget reported as the default whatever was asked" 1
    control "a band reported as nought whatever was bought" 2
    control "a route always read as recorded" 3
    control "a refusal that accepts every budget" 4
    control "a record that never says it spilled" 5

    # ... and the interpreter's own reading of the budget, stubbed to a
    # constant in a copy of the boot files: a run banding to a number
    # nobody named must not read as a run that banded to the one it was
    # given.
    sabdir=$work/sabdata
    if cp -r "$data" "$sabdir" 2>/dev/null; then
        { echo 'currentglobal true setglobal'
          echo '.xpostsys /.bandbudget { 4000000 } bind put'
          echo 'setglobal'; } >> "$sabdir/device.ps"
        out=$(XPOST_DATA_DIR=$sabdir "$xpost" -q --no-sandbox -DMARKS=20 \
              -o "$work/sabb.pgm" -d pgm --band-bytes=6120 "$script" \
              </dev/null 2>&1)
        case $out in
            *"BUDGET 6120"*)
                note "a run whose budget reading is stubbed to the default" \
                     "still reported the budget it was given" ;;
            *) echo "OK   a stubbed budget reading is caught" ;;
        esac
    else
        echo "OK   (skipped: the boot files could not be copied to stub)"
    fi
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
