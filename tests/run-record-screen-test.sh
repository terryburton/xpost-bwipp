#!/bin/sh
# Meson test wrapper: a record carries the screen its marks were made
# under, and a page played back in bands is the page painted whole.
#
# The claim is a byte one and it is made against a directly painted
# page: the same program, on the same page, through the bilevel device
# that paints it in one go and through the device that writes it down
# first and plays it back a run of rows at a time.
#
# Four ways a run like this passes without having established anything,
# and what is done about each:
#
#   The page has nothing on it a screen decides. Black and white come
#   out of every cell alike, so a page of them would replay right
#   whether or not the screen was carried. The negative control is the
#   same page painted under one screen throughout: it has to differ from
#   the page painted under three, or the screen changes this test exists
#   for are not reaching the page.
#
#   The bands are not bands. A budget larger than the page is one band
#   and compares the whole page against itself, so the heights swept
#   here are small against the page and the run says how many rows a
#   band held.
#
#   The screen is written down per mark. That would be right and
#   ruinous: a page of a million marks would hold a million cells. The
#   count is held to the number of screens the page sets.
#
#   The count comes from a device that holds no record. A device that is
#   not recording answers nothing, and the run reads the absence rather
#   than reading a missing answer as a zero.
#
#   Only the last page is compared. A job of two pages put out under one
#   name leaves that name holding the last of them, and the last page
#   here sets no screen -- so it is painted under the screen the replay
#   would hold anyway and would match however the screens were carried.
#   Both pages are written and both are compared.
#
#   $1  path to the built xpost binary
#   $2  path to record_screen_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

# $1 device, $2 output stem, rest: program arguments. Every page is
# written, under the stem with the page number in it.
render() {
    dev=$1
    stem=$2
    shift 2
    rm -f "$work/$stem"*.pbm
    out=$("$xpost" -q $ns -d "$dev" -o "$work/$stem%d.pbm" "$@" "$script" \
          </dev/null 2>&1)
    st=$?
    verdict_run "$st" "$out" "the $dev run" || return 1
    return 0
}

# Both pages of $1 against both pages of $2; sets diffpage to the first
# that differs. A page missing on either side counts as a difference,
# so a run that stopped after page one cannot pass by having less to
# compare.
samepages() {
    diffpage=''
    for n in 1 2; do
        if [ ! -s "$work/$1$n.pbm" ] || [ ! -s "$work/$2$n.pbm" ]; then
            diffpage=$n
            return 1
        fi
        if ! cmp -s "$work/$1$n.pbm" "$work/$2$n.pbm"; then
            diffpage=$n
            return 1
        fi
    done
    return 0
}

# ---------------------------------------------------------------------
# The page, painted and played
#
# A page of greys under three screens, then a page that sets none.
render pbm direct || fail=1
if [ ! -s "$work/direct1.pbm" ] || [ ! -s "$work/direct2.pbm" ]; then
    echo "FAILURES: the bilevel device put out fewer than the two pages" \
         "this program paints"
    fail=1
fi

# The negative control: the same page under one screen. If this matches
# the page above, the screen changes are not reaching the raster and
# every comparison below would hold with the screen thrown away.
render pbm one -DONESCREEN=1 || fail=1
if [ "$fail" -eq 0 ] && samepages direct one; then
    echo "FAILURES: the pages painted under three screens are the pages" \
         "painted under one, so nothing here is held to carrying a screen"
    fail=1
fi

render pbm:band whole || fail=1
if [ "$fail" -eq 0 ] && ! samepages direct whole; then
    echo "FAILURES: page $diffpage played back whole is not the page painted"
    fail=1
fi

# ---------------------------------------------------------------------
# The same page, a band at a time
#
# Heights that divide the page and heights that do not, down to one row:
# a screen is met by every band, so every band has to pass through the
# same screens in the same order as a replay of the whole page.
if [ "$fail" -eq 0 ]; then
    bad=''
    for rows in 1 2 3 7 16 64 149; do
        if ! render pbm:band band "-DBAND=$rows"; then
            bad="$bad $rows"
            continue
        fi
        samepages direct band || bad="$bad $rows"
    done
    if [ -n "$bad" ]; then
        echo "FAILURES: a page of 300 rows played back in bands of$bad" \
             "row(s) is not the page painted whole"
        fail=1
    else
        echo "OK   a page of greys under three screens, played back in" \
             "bands of 1 2 3 7 16 64 149 rows, is the page the bilevel" \
             "device paints in one go -- and so is the page after it," \
             "which sets no screen of its own"
    fi
fi

# ---------------------------------------------------------------------
# What the record holds
#
# One for the screen the page opens under and one for each change after
# it. The page is cleared before it sets a screen of its own, so the
# screen in force at the clear is written down where the page begins --
# which is the same entry a page that sets none at all gets, and is what
# lets the second page here be painted under the screen the first one
# ended with.
count_screens() {
    out=$("$xpost" -q $ns -d "$1" -o "$work/count.out" -DCOUNT=1 \
          "$script" </dev/null 2>&1)
    st=$?
    verdict_run "$st" "$out" "the $1 census" || return 1
    case $out in
        *NORECORD*)
            echo "FAILURES: the $1 run holds no record, so the count of" \
                 "screens below came from a device that keeps none"
            return 1 ;;
    esac
    screens=$(printf '%s\n' "$out" | sed -n 's/^SCREENS \([0-9][0-9]*\).*/\1/p' \
              | head -1)
    if [ -z "${screens:-}" ]; then
        echo "FAILURES: the $1 run did not say how many screens its" \
             "record holds"
        return 1
    fi
    return 0
}

if count_screens pbm:band; then
    if [ "$screens" -ne 4 ]; then
        echo "FAILURES: the record of a page opening under one screen and" \
             "setting three more holds $screens, where four is one for the" \
             "one it opened under and one for each change. One per mark" \
             "would hold a cell for every mark on the page"
        fail=1
    else
        echo "OK   the record of a page setting three screens holds four:" \
             "the one it opened under and one per change, not one per mark"
    fi
else
    fail=1
fi

# A page under one screen holds one, so the count above follows the page
# rather than being a constant.
out=$("$xpost" -q $ns -d pbm:band -o "$work/count1.out" -DCOUNT=1 \
      -DONESCREEN=1 "$script" </dev/null 2>&1)
st=$?
if verdict_run "$st" "$out" "the one-screen census"; then
    s1=$(printf '%s\n' "$out" | sed -n 's/^SCREENS \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ "${s1:-x}" != 2 ]; then
        echo "FAILURES: the record of a page setting one screen holds" \
             "${s1:-no} screen(s), where two is the one it opened under" \
             "and the one it set, so the count does not follow the page"
        fail=1
    else
        echo "OK   the record of a page setting one screen holds two: the" \
             "one it opened under and the one it set"
    fi
else
    fail=1
fi

# A target that does not screen is never told of one and holds none, so
# what a screen costs a page that cannot use it is nothing.
out=$("$xpost" -q $ns -d pgm:band -o "$work/countg.out" -DCOUNT=1 \
      "$script" </dev/null 2>&1)
st=$?
if verdict_run "$st" "$out" "the greyscale census"; then
    sg=$(printf '%s\n' "$out" | sed -n 's/^SCREENS \([0-9][0-9]*\).*/\1/p' | head -1)
    if [ "${sg:-x}" != 0 ]; then
        echo "FAILURES: a record played into a device that does not screen" \
             "holds ${sg:-no} screen(s), which is state it can never use"
        fail=1
    else
        echo "OK   a record played into a device that does not screen" \
             "holds none"
    fi
else
    fail=1
fi

verdict_exit
