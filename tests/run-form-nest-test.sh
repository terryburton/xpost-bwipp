#!/bin/sh
# Meson test wrapper: a page of forms inside forms, put to both of the
# routes a page can take and compared against the same page painted with
# nothing held.
#
# A form is a self-contained description painted at several locations,
# and the language allows an interpreter to keep the graphical output of
# one and substitute it for a later execution rather than running the
# description again (PLRM 4.7). What this run holds the interpreter to
# is that the substitution changes nothing about the page.
#
# THREE PAGES, AND THEY MUST ALL BE THE SAME BYTES.
#
#   painted   the page with the form dictionaries read-only, which is
#             what a description is recorded in and so withdraws the
#             holding: every use runs its description at its own
#             position. It is the page the other two are held to.
#   whole     the same page on the device that holds its raster, with
#             the descriptions held.
#   banded    the same page on the device that writes down what it is
#             asked to paint and plays it back a band at a time, with
#             the descriptions held.
#
# Two routes agreeing is not the claim and would not be one: two routes
# can agree and both be wrong, which is what a page of forms did before
# this -- the copy quantized every placement to the pixel grid on both
# of them. So the painted page is a third reading and the one the others
# are measured against, and the comparison is reported in pixels rather
# than as a verdict.
#
# WHAT MUST NOT MATCH. The same page with one placement moved a fraction
# of a pixel. It says the comparison above can see a placement that has
# moved by less than a pixel -- without it, three pages that were all
# equally wrong would pass.
#
# AND THE COUNT, which is the whole reason any of this exists: how many
# times a form's description was executed. A page comparison cannot see
# it, since a description re-executed paints what a description held
# paints. Three forms used fifty-six times between them must be
# described three times, on both routes.
#
#   $1  path to the built xpost binary
#   $2  path to form_nest_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

# The figure: three forms, used fifty-six times between them. Stated
# here as well as painted, so that a figure that quietly stopped using
# one of its forms would be caught rather than making the count below
# easier to satisfy.
uses=56
forms=3

# What a band of the page may cost, in bytes. Small enough that the page
# arrives in several bands and the figure crosses their edges: the page
# is four hundred pixels wide, so this is fifty rows to a band and eight
# bands to the page.
budget=20000

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

# $1 $2 two pages; prints "<pixels> <largest difference>". Both pages
# are one byte per pixel, so a byte that differs is a pixel that
# differs and the difference between the bytes is the difference
# between the greys. A pair of pages of different lengths is not
# compared by this and is reported by the caller.
pixdiff() {
    cmp -l "$1" "$2" 2>/dev/null | awk '
        function oct(s,   i, v) {
            v = 0
            for (i = 1; i <= length(s); i++) v = v * 8 + substr(s, i, 1)
            return v
        }
        { n++; d = oct($2) - oct($3); if (d < 0) d = -d; if (d > m) m = d }
        END { printf "%d %d\n", n + 0, m + 0 }'
}

# $1 $2 pages, $3 what the pair is; complains unless they are the same
# bytes, and says how far apart they are either way
same() {
    if [ "$(wc -c < "$1")" != "$(wc -c < "$2")" ]; then
        echo "FAILURES: $3 are pages of different sizes"
        fail=1
        return
    fi
    set -- "$1" "$2" "$3" $(pixdiff "$1" "$2")
    if [ "$4" -eq 0 ]; then
        echo "OK   $3 are the same page: $4 pixels differ, largest"
        echo "     difference $5"
    else
        echo "FAILURES: $3 differ in $4 pixels, largest difference $5"
        fail=1
    fi
}

render painted          pgm:whole -DRO=1                  || fail=1
p_whole=$out
render whole            pgm:whole                         || fail=1
w=$out
render banded           pgm:band  -DBB=$budget            || fail=1
b=$out
render moved            pgm:whole -DNUDGE=1               || fail=1
m=$out
render painted-moved    pgm:whole -DRO=1 -DNUDGE=1        || fail=1
pm=$out
render scaled           pgm:whole -DSCALE=1.07            || fail=1
s=$out
render painted-scaled   pgm:whole -DSCALE=1.07 -DRO=1     || fail=1
ps=$out
render spill            pgm:whole -DSPILL=1              || fail=1
sl=$out
render painted-spill    pgm:whole -DSPILL=1 -DRO=1       || fail=1
psl=$out

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

# Every run has to have said what it did before any of it is read.
for pair in "painted $p_whole" "whole $w" "banded $b" "moved $m"; do
    name=${pair%% *}
    said=${pair#* }
    if [ -z "$(field "$said" PAINTS)" ] || [ -z "$(field "$said" PLACEMENTS)" ]
    then
        echo "FAILURES: the $name run did not say what it described"
        fail=1
    fi
done
[ "$fail" -eq 0 ] || exit 1

pp=$(field "$p_whole" PAINTS)
pu=$(field "$p_whole" PLACEMENTS)
wp=$(field "$w" PAINTS)
bp=$(field "$b" PAINTS)
mp=$(field "$m" PAINTS)
sp=$(field "$s" PAINTS)
bmarks=$(field "$b" MARKS)
bdraw=$(field "$b" DRAWINGS)
brows=$(field "$b" BANDROWS)

# The figure is the figure it says it is: nothing below means anything
# if the page stopped using its forms.
if [ "$pu" -ne "$uses" ] || [ "$pp" -ne "$uses" ]; then
    echo "FAILURES: the page used a form $pu times and described one $pp"
    echo "      times with nothing held; the figure is $uses uses of"
    echo "      $forms forms, and every one of them describes afresh"
    fail=1
else
    echo "OK   with nothing held, $uses uses of $forms forms describe them"
    echo "     $pp times: one description per use, at every depth"
fi

# THE COUNT. Each distinct form described once, however many times the
# page uses it and however deep it sits -- on both routes.
for pair in "whole $wp" "banded $bp"; do
    name=${pair%% *}
    got=${pair#* }
    if [ "$got" -eq "$forms" ]; then
        echo "OK   held, the $name route describes $forms forms $got times"
        echo "     for $uses uses: what it costs is the forms, not the uses"
    else
        echo "FAILURES: held, the $name route described a form $got times"
        echo "      for $uses uses of $forms forms; a description held is"
        echo "      run once for each form and place it stands at"
        fail=1
    fi
done

# The banded run has to have banded, or the figure never crossed a band
# edge and the comparison above was made against a page held whole by
# another name.
if [ -n "$brows" ] && [ "$brows" -gt 0 ] && [ "$brows" -lt 400 ]; then
    echo "OK   the banded page arrived in bands of $brows rows, so the"
    echo "     figure crossed their edges"
else
    echo "FAILURES: the banded page arrived in bands of $brows rows of a"
    echo "      four hundred row page, so nothing crossed a band edge"
    fail=1
fi

# What the banded route wrote down, which is what a placement being a
# reference comes to: the page holds one drawing and a mark per use of
# it, not the drawing once per use.
if [ -n "$bmarks" ] && [ -n "$bdraw" ]; then
    echo "NOTE the banded page wrote down $bmarks marks and $bdraw drawing,"
    echo "     the drawings inside it being held by the drawings that"
    echo "     place them"
fi

# THE THREE PAGES.
same "$work/painted.pgm" "$work/whole.pgm" \
     "the page painted afresh and the page held whole"
same "$work/painted.pgm" "$work/banded.pgm" \
     "the page painted afresh and the page held in bands"
same "$work/whole.pgm" "$work/banded.pgm" \
     "the page held whole and the page held in bands"

# WHAT MUST NOT MATCH: one placement moved a fraction of a pixel.
set -- $(pixdiff "$work/whole.pgm" "$work/moved.pgm")
if [ "$1" -eq 0 ]; then
    echo "FAILURES: moving a placement a fraction of a pixel left the page"
    echo "      byte for byte the same, so the comparisons above rest on a"
    echo "      comparison that has never been shown to see anything"
    fail=1
else
    echo "OK   a placement moved a fraction of a pixel moves $1 pixels of"
    echo "     the page, largest difference $2: the comparison sees one"
fi

# ... and the moved page is itself right, which says the fraction was
# not simply lost.
same "$work/painted-moved.pgm" "$work/moved.pgm" \
     "the moved page painted afresh and the moved page held"

# The same page under a scale that puts every use at its own place
# between pixels, which is where a drawing carried to the wrong place
# would show.
if [ "$sp" -eq "$forms" ]; then
    echo "OK   under a scale of 1.07 the same $forms descriptions serve"
    echo "     $uses uses, none of them repeating a place on the pixel"
else
    echo "FAILURES: under a scale of 1.07 the page described a form $sp"
    echo "      times for $uses uses of $forms forms"
    fail=1
fi
same "$work/painted-scaled.pgm" "$work/scaled.pgm" \
     "the scaled page painted afresh and the scaled page held"

# A form painting outside the box it declares is not held at all: a
# description held carries the marks it made and nothing cuts them where
# they land, so what the box would have cut would be carried to every
# place the form is put. Such a form is described afresh at every use,
# which is what a page holding no description pays, and its page must
# still be the page painted afresh.
same "$work/painted-spill.pgm" "$work/spill.pgm" \
     "the page of a form reaching past its box, painted afresh and held"
spp=$(field "$sl" PAINTS)
if [ -n "$spp" ] && [ "$spp" -ge "$uses" ]; then
    echo "NOTE a form reaching past the box it declares is not held: the"
    echo "     page describes forms $spp times, against $wp where every"
    echo "     form keeps inside its box and $pp with nothing held"
else
    echo "FAILURES: a form reaching past the box it declares was described"
    echo "      $spp times, fewer than the $uses uses of it: a form whose"
    echo "      marks leave its box is not one to hold"
    fail=1
fi

verdict_exit
