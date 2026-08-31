#!/bin/sh
# Meson test wrapper: a page placing one form many times, put to both of
# the routes a page can take, and the form's description counted rather
# than the page's pixels.
#
# A form is a self-contained description painted at several locations,
# and the language allows an interpreter to keep the graphical output of
# one and substitute it for a later execution instead of running the
# description again (PLRM 4.7). This interpreter keeps it where the
# device holds the page as rows of its own. A device that writes down
# what it is asked to paint holds none, so on that route the description
# is run once per placement.
#
# The difficulty is the same one every check in this area has: the two
# routes paint the same page. A description re-executed puts down what a
# copy of its output puts down, so no comparison of pages can see which
# happened, and a run that compared only pages would report a clean
# result whichever way round it was. So the description counts its own
# executions and this run reads that count; the pages are compared for a
# different claim, which is that the route makes no difference to them.
#
# Four readings, and a control for each of the two that could go quiet:
#
#   The copy works, on the route that makes one. Read as how the count
#   of executions moves when the count of placements moves: a page
#   placing the form nine times and a page placing it twenty-five times
#   must execute the description the same few times. A single number
#   would not say this -- it is the standing still that is the claim.
#
#   Its control is the same page on the same route with the form
#   dictionary read-only, which is what the copy is keyed in and so
#   withdraws it, nothing else about the run changing. The count must
#   then follow the placements. Without this, a reading that always
#   answered two would satisfy the check above by never moving.
#
#   The route makes no difference to the page, at whole-numbered
#   placement: the recorded page and the painted page, byte for byte.
#
#   The route makes no difference to the page, at placement that falls
#   between pixels: the recorded page against a painted page with the
#   copy withdrawn. It is compared against that rather than against the
#   ordinary painted page because of the next reading.
#
#   Its control is a pair that must NOT match: the same page painted
#   with the copy in use and with it withdrawn, where the placements
#   fall at two different phases against the pixel grid. A copy is taken
#   at one phase and replayed at both, quantized to the grid, so the two
#   pages differ -- which is what says the byte comparison above can see
#   a difference at all. Two comparisons that must match and one that
#   must not is the whole instrument.
#
# And one quantity stated rather than required: how many times the
# record route executed the description. It is the open item here -- the
# copy is not made on that route, so a form is described again at every
# placement -- and it is printed so that the day it changes, this run
# says so rather than passing quietly either way.
#
#   $1  path to the built xpost binary
#   $2  path to form_band_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

# How many executions a page whose form is copied may come to, whatever
# it places. Two is what taking the copy costs -- the description is run
# over each of two grounds and the pixels they agree on are the form's
# own marks -- and the bound is loose enough that a route reaching it by
# another number of passes still reads as a copy rather than as one
# execution per placement.
cachedmax=4

# $1 name for the page, $2 device, $3.. extra arguments; sets out to
# what the run said and leaves the page at $work/$1.pgm
render() {
    r_name=$1; r_dev=$2
    shift 2
    out=$("$xpost" -q $ns -d "$r_dev" "$@" -o "$work/$r_name.pgm" "$script" \
          </dev/null 2>&1)
    st=$?
    verdict_run "$st" "$out" "the $r_name run" || return 1
    if [ ! -s "$work/$r_name.pgm" ]; then
        echo "FAILURES: the $r_name run produced no page"
        return 1
    fi
    return 0
}

# $1 what the run said, $2 field name; prints that field's first line
field() { printf '%s\n' "$1" | sed -n "s/^$2 //p" | head -1 \
          | sed 's/[[:blank:]]*$//'; }

# Selecting a device by name selects the record in front of it where the
# page is over the budget, so both modes are asked for by name: the
# comparison here is between a route that holds the page and a route
# that writes down what it was asked to paint.
render whole9   pgm:whole -DN=9              || fail=1
w9=$out
render whole25  pgm:whole -DN=25             || fail=1
w25=$out
render band9    pgm:band  -DN=9              || fail=1
b9=$out
render band25   pgm:band  -DN=25             || fail=1
b25=$out
render direct9  pgm:whole -DN=9  -DRO=1      || fail=1
d9=$out
render direct25 pgm:whole -DN=25 -DRO=1      || fail=1
d25=$out
render fcopy    pgm:whole -DN=25 -DFRAC=1         || fail=1
fc=$out
render fdirect  pgm:whole -DN=25 -DFRAC=1 -DRO=1  || fail=1
fd=$out
render fband    pgm:band  -DN=25 -DFRAC=1         || fail=1
fb=$out

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

# Every run has to have said what it placed and what it executed, before
# any of those numbers is read against another.
for pair in "whole9 $w9" "whole25 $w25" "band9 $b9" "band25 $b25" \
            "direct9 $d9" "direct25 $d25" "fband $fb"; do
    name=${pair%% *}
    said=${pair#* }
    if [ -z "$(field "$said" PAINTS)" ] || [ -z "$(field "$said" PLACEMENTS)" ]; then
        echo "FAILURES: the $name run did not say what it placed and executed"
        fail=1
    fi
done
[ "$fail" -eq 0 ] || exit 1

w9p=$(field "$w9" PAINTS)
w25p=$(field "$w25" PAINTS)
d9p=$(field "$d9" PAINTS)
d25p=$(field "$d25" PAINTS)
b9p=$(field "$b9" PAINTS)
b25p=$(field "$b25" PAINTS)
n9=$(field "$w9" PLACEMENTS)
n25=$(field "$w25" PLACEMENTS)

# The two pages have to differ in what they place, or the reading below
# is taken over a quantity that never moved.
if [ "$n9" -ge "$n25" ]; then
    echo "FAILURES: the two pages place the form $n9 and $n25 times;"
    echo "      the copy is read as what holds still while that grows"
    fail=1
fi

# The copy works: executions stand still while placements grow.
if [ "$w9p" -eq "$w25p" ] && [ "$w9p" -le "$cachedmax" ]; then
    echo "OK   a copied form is described $w9p times at $n9 placements and"
    echo "     $w25p at $n25: what it costs is the form, not the placements"
else
    echo "FAILURES: a form on the route that copies it was described $w9p"
    echo "      times at $n9 placements and $w25p times at $n25;"
    echo "      a copy that is being used costs the same either way,"
    echo "      and no more than $cachedmax"
    fail=1
fi

# Its control: withdraw the copy and the same reading must follow the
# placements. A reading that cannot move would pass the check above by
# standing still for the wrong reason.
if [ "$d9p" -eq "$n9" ] && [ "$d25p" -eq "$n25" ]; then
    echo "OK   with the copy withdrawn the same page describes the form"
    echo "     $d9p and $d25p times: the reading moves when the copy goes"
else
    echo "FAILURES: with the copy withdrawn the form was described $d9p"
    echo "      times at $n9 placements and $d25p at $n25; the control"
    echo "      for the reading above is that it follows the placements"
    fail=1
fi

# The route makes no difference to the page.
if cmp -s "$work/whole25.pgm" "$work/band25.pgm"; then
    echo "OK   a recorded page of forms is the painted page, byte for byte"
else
    echo "FAILURES: a page of forms played back from its record is not the"
    echo "      page that was painted"
    cmp "$work/whole25.pgm" "$work/band25.pgm" 2>&1 | sed 's/^/      /' | head -3
    fail=1
fi

# ... and the holding has to be in use there, or the comparison below is
# made where nothing is held and would pass whatever the holding did.
fcp=$(field "$fc" PAINTS)
if [ -n "$fcp" ] && [ "$fcp" -lt "$n25" ]; then
    echo "OK   at placements falling between pixels the form is described"
    echo "     $fcp times for $n25 placements, so the comparison below is"
    echo "     made where a drawing is being held and placed at a point"
    echo "     between pixels it was not made at"
else
    echo "FAILURES: at placements falling between pixels the form was"
    echo "      described $fcp times for $n25 placements, so nothing was"
    echo "      held and the comparison below sees nothing"
    fail=1
fi

# The same where the placements fall between pixels, and against the
# page painted with nothing held rather than against one of the two: two
# routes can agree and both be wrong, which is what a page of forms did
# when what was held was pixels quantized to the grid.
for pair in "fcopy the page held whole" "fband the page held in bands"; do
    name=${pair%% *}
    what=${pair#* }
    if cmp -s "$work/fdirect.pgm" "$work/$name.pgm"; then
        echo "OK   $what is the page painted afresh, byte for byte, where"
        echo "     the placements fall between pixels"
    else
        echo "FAILURES: $what differs from the page painted afresh where the"
        echo "      placements fall between pixels"
        cmp "$work/fdirect.pgm" "$work/$name.pgm" 2>&1 | sed 's/^/      /' | head -3
        fail=1
    fi
done

# The same reading on the route that writes the page down. It is a
# requirement and not a note: what a drawing is held by is a recorder,
# which a record device can be stood in front of as readily as a raster,
# so the count must stand still on this route too.
if [ "$b9p" -eq "$b25p" ] && [ "$b9p" -le "$cachedmax" ]; then
    echo "OK   the record route describes the form $b9p times at $n9"
    echo "     placements and $b25p at $n25: the same reading as the route"
    echo "     that holds its page"
else
    echo "FAILURES: on the record route the form was described $b9p times"
    echo "      at $n9 placements and $b25p at $n25; a drawing held is"
    echo "      held on both routes"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo SUCCESS
