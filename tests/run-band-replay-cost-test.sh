#!/bin/sh
# Meson test wrapper: what the band loop charges for a page whose cost
# is its marks.
#
# The band loop does not paint a page: it writes the marks down and
# plays them back, once for each run of rows the page is put out in. So
# a page put out this way is charged for its marks twice over, and what
# playing one costs is a cost the same page painted directly never
# pays. Everything else about the loop is settled elsewhere -- the bytes
# by run-band-campaign-test.sh, the bounds by the counting the other
# wrappers do. What is left, and what has no other test, is the price.
#
# It has no other test because it does not show. A page put out either
# way carries the same bytes, so nothing comparing pages can see it, and
# no count of marks can either: a replay that took a thousand times
# longer would play exactly the same marks. What can see it is the
# clock, and only against a page of the one shape that shows it.
#
# THE SHAPE. A page drawing in a loop is charged for the loop. Rectangle
# fills, path fills and strokes all spend more above the device than
# they spend at it, so the two runs come within a few per cent of each
# other and stay there however many marks are added -- adding marks adds
# program in the same proportion. A page built that way passed while the
# replay was twenty times slower than the direct paint, which is what
# this wrapper exists to stop happening again. Small text is the other
# shape: a glyph is painted against the ground pixel by pixel and the
# glyphs are cached, so a line of it is a few operators of program and
# thousands of marks. The page is in band_replay_cost_test.ps and says
# the same thing at more length.
#
# THE READING. The page reports the interpreter time it took, taken
# after showpage so that the replay is inside it, and the wrapper
# compares the two readings as a ratio. A ratio is what can be compared:
# an absolute time is a statement about the machine the suite is running
# on, and a bound on one would be a bound nobody can choose.
#
# The clock read counts execution rather than elapsed time, which takes
# out the waiting but not the contention: a suite running a dozen tests
# at once costs each of them cache and memory bandwidth, and that is
# charged as execution. Measured under twelve-fold load, the two
# readings each tripled and their ratio moved by a seventh -- not
# evenly, because the two runs are not in contention for the same
# things. So the page is measured in rounds, one direct run and one
# banded run adjacent in each, and the verdict is the best round of
# them. A round that was interfered with is not evidence about the code,
# and the check is about the code; what a passing round says is that the
# machine managed to make the measurement once, which is all that is
# being asked of the machine.
#
# Three things have to hold, and no two of them imply each other:
#
#   The ratio. The banded run may cost the stated multiple of the direct
#   one and no more.
#
#   The bytes. Both runs must put out the same page. Without this a
#   replay that painted nothing would be the fastest of all and the
#   ratio would welcome it.
#
#   The floor. Both readings must be well clear of the clock's own
#   resolution. Without this a page that stopped early would be timed at
#   nothing against nothing, and a ratio computed from two readings of
#   zero would be whatever the arithmetic did with it.
#
# and each has a control of its own: the run ends by breaking its own
# instruments on purpose and requiring itself to fail. A sabotage that
# passed would mean the check it targets sees nothing.
#
#   $1  path to the built xpost binary
#   $2  path to band_replay_cost_test.ps
#
# and, for the run's own controls, which it invokes on itself:
#
#   --sabotage N $1 $2   run the comparison with defect N built in, and
#                        answer as it finds it. The caller requires a
#                        failure.
set -u

sab=0
case ${1:-} in
    --sabotage) sab=$2; shift 2 ;;
esac
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

verdict_workdir
fail=0

ns=$(sandbox_flag "$xpost")

# What the banded run may cost against the direct one.
#
# The two are not the same work and the bound is not 1: the banded run
# writes every mark down as well as playing it, and it paints into a
# raster it moves down the page rather than one standing still. What the
# bound holds is that playing a mark back is of the same order as making
# it, which is what it is when a band is played in a loop and is not
# when each mark is played by going round the interpreter.
#
# Chosen from both sides rather than from one. A band played in a loop
# comes to two, a band played a mark at a time through the interpreter
# to two and six tenths, and each reading is stable to within a couple
# of per cent across runs. The bound sits half way between them, which
# is the only place that neither flakes on the behaviour it is meant to
# admit nor sleeps through the one it is here to catch. It is not a
# target: what the loop can be brought to is a question for whoever
# brings it, and a bound moved down after it has been is a bound that
# still says what it says.
bound_num=23
bound_den=10

render() { # $1 device  $2 output file; sets cost
    r_out=$( cd "$work" && "$xpost" -q $ns -d "$1" -o "$2" "$script" \
             </dev/null 2>&1 )
    r_st=$?
    verdict_run "$r_st" "$r_out" "the $1 run" || return 1
    cost=$( printf '%s\n' "$r_out" | tr -s '-' '\n' | sed -n 's/^COST //p' )
    case ${cost:-} in
        ''|*[!0-9]*)
            note "the $1 run did not report what it cost the interpreter"
            return 1 ;;
    esac
    return 0
}

# How many rounds the page gets. A run carrying a defect is asking
# whether a check fires and not how fast anything is, so it takes one:
# the controls at the end run this script three times over and there is
# no reason for them to measure anything three times each.
rounds=3
[ "$sab" -eq 0 ] || rounds=1

whole=0
banded=0
round=0
while [ "$round" -lt "$rounds" ]; do
    round=$(( round + 1 ))
    # The two routes, named as this build spells them. A device named on
    # its own paints the page whole; the same device named through the
    # record has the page written down and played back to it in bands,
    # which is the route being priced. Both spellings have to be exact:
    # the mode selector a selection may carry is the selected device's to
    # read, and a device that reads none of them takes any selection at
    # all and paints the page the one way it paints it -- so a run asking
    # for the banded route by a spelling that device does not know is a
    # run measuring the direct route against itself, which nothing in the
    # readings would show.
    render "ppm" whole.ppm || fail=1
    r_whole=${cost:-0}
    render "ppm:band" banded.ppm || fail=1
    r_banded=${cost:-0}
    [ "$fail" -eq 0 ] || break

    # ---- the sabotage the controls at the end ask for ----
    #
    # Each defect is introduced here, after the round's runs and before
    # anything reads them, so that what a control exercises is the check
    # itself and not a broken way of reaching it.
    case $sab in
        1)  # the ratio gate: a banded reading the bound cannot admit
            r_banded=$(( r_whole * 100 )) ;;
        2)  # the bytes: a banded page that is not the page
            printf 'not the page\n' > "$work/banded.ppm" ;;
        3)  # the floor: readings too small to be readings
            r_whole=0; r_banded=0 ;;
    esac

    # The round kept is the one whose banded run came off lightest
    # against its own direct run. Kept as the pair rather than as the
    # quotient, so that what is reported is what was read.
    if [ "$whole" -eq 0 ] ||
       [ $(( r_banded * whole )) -lt $(( banded * r_whole )) ]; then
        whole=$r_whole
        banded=$r_banded
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

# ---- the floor ----
#
# A hundred milliseconds is some thousands of times the millisecond the
# clock reports in and a few times the longest tick a system that
# charges execution by the scheduling tick can credit at once, so a
# reading above it is a reading of the page rather than of the clock.
floor=100
if [ "$whole" -lt "$floor" ] || [ "$banded" -lt "$floor" ]; then
    note "the page cost ${whole}ms held whole and ${banded}ms in bands," \
         "and a reading under ${floor}ms is not far enough above what the" \
         "clock itself resolves to be compared with another"
else
    echo "OK   both runs are readings of the page: ${whole}ms held whole," \
         "${banded}ms in bands, over $rounds round(s)"
fi

# ---- the bytes ----
if [ ! -s "$work/whole.ppm" ] || [ ! -s "$work/banded.ppm" ]; then
    note "a run put out no page, so there is nothing to compare and the" \
         "cost of putting one out is not what was measured"
elif cmp -s "$work/whole.ppm" "$work/banded.ppm"; then
    echo "OK   both runs put out the same page"
else
    note "the banded run did not put out the page the direct run did, so" \
         "the two readings are not readings of the same work"
fi

# ---- the ratio ----
#
# Held in whole numbers: the shells this suite runs under have no
# arithmetic but integer arithmetic, and a bound of twenty-three tenths
# is a comparison of ten times one reading against twenty-three times
# the other.
if [ "$whole" -gt 0 ] &&
   [ $(( banded * bound_den )) -gt $(( whole * bound_num )) ]; then
    note "the page cost ${whole}ms held whole and ${banded}ms in bands," \
         "which is more than the ${bound_num}/${bound_den} the band loop" \
         "may cost: a band is being played a mark at a time through the" \
         "interpreter rather than in a loop"
elif [ "$whole" -gt 0 ]; then
    echo "OK   the band loop cost ${banded}ms against the ${whole}ms the" \
         "same page cost held whole, within the" \
         "${bound_num}/${bound_den} bound"
fi

# ---- the controls ----
#
# Reached only by the plain run, which invokes itself once per defect
# and requires each to fail. A defect that passed would say the check it
# targets is not looking at anything.
if [ "$sab" -eq 0 ] && [ "$fail" -eq 0 ]; then
    for n in 1 2 3; do
        if sh "$self" --sabotage "$n" "$xpost" "$script" >"$work/sab$n.log" 2>&1
        then
            note "the run with defect $n built in passed, so the check it" \
                 "breaks sees nothing"
            sed 's/^/      /' "$work/sab$n.log"
        else
            echo "OK   the run with defect $n built in failed, as it must"
        fi
    done
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
