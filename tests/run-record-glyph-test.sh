#!/bin/sh
# Meson test wrapper: a page of text recorded and played back is the page
# that was painted, and the record of it is priced by the page's glyphs
# rather than by its ink.
#
# This is run-record-image-test.sh's claim for the second thing a record
# of the five marking kinds is the wrong shape for. Text reaches a device
# one pixel at a time -- a fully covered pixel through PutPix and a
# partly covered edge pixel through BlendPix with its coverage -- which
# costs a raster nothing and would cost a record tens of bytes for every
# inked pixel on the page. A record holds a coverage mask and a placement
# of it instead, one mask however many placements name it.
#
# The whole difficulty is that the failure is invisible. A record holding
# a mark per inked pixel replays to exactly the same bytes; the page
# comes out right and costs twenty times what it should. This run
# demonstrates that rather than asserting it: the first thing it does
# after the ordinary run is take the device's glyph entry away and check
# that the page is still identical while every reading below has changed.
#
# So the page is compared, and then four things the page cannot show are
# read off the record itself:
#
#   The page has ink to speak of. A page of a few pixels would satisfy
#   every bound below by having nothing in it, so the ink is counted --
#   by playing the record into a device that counts the calls -- and
#   required to be many pixels per mark. That is also what says the
#   saving is real: a mark per glyph is worth having exactly because a
#   glyph is a great many pixels.
#
#   Text arrived as a mark per glyph. The record's own mark count is
#   held to the glyphs the page says it showed, with room for the page
#   clear. A mark per pixel is a hundred times past that rather than a
#   few marks past it.
#
#   One mask serves many placements. The record's mask count is held to a
#   fraction of its marks. Without this a record could hold a mask per
#   placement -- a mark per glyph, and the coverage copied for each of
#   them -- which is most of the cost back again.
#
#   What the record cost follows its masks and its marks. Stated as a
#   ceiling per mark plus the masks, so that the reading is about the
#   record's shape rather than about this page's size; and the whole of
#   it is required to be smaller than the raster it saves holding, which
#   is the claim the mechanism exists to make.
#
#   $1  path to the built xpost binary
#   $2  path to record_glyph_test.ps
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

# The margins the readings are held to. Each is far from what the page
# gives and far from what the defect gives, so that a build whose font
# renders a little heavier or lighter moves neither verdict.
minink=20000      # inked pixels, below which the page says nothing
perglyph=8        # inked pixels a mark must be worth, at least
slack=8           # marks a page clear and its like may add
share=10          # placements a mask must serve, at least
permark=160       # bytes a mark may cost, at most

# $1 device, $2 output path, $3.. extra arguments; sets out to what the
# run said
render() {
    r_dev=$1; r_out=$2
    shift 2
    out=$("$xpost" -q $ns -d "$r_dev" -o "$r_out" "$@" "$script" </dev/null 2>&1)
    st=$?
    verdict_run "$st" "$out" "the $r_dev run" || return 1
    if [ ! -s "$r_out" ]; then
        echo "FAILURES: the $r_dev run produced no page"
        return 1
    fi
    return 0
}

# $1 what the run said, $2 field name; prints that field's first line
# The trailing space the page separates its figures with is taken
# off, so that a reading is a number and prints as one.
field() { printf '%s\n' "$1" | sed -n "s/^$2 //p" | head -1 \
          | sed 's/[[:blank:]]*$//'; }

# The painter by itself, asked for as the mode that holds the page whole:
# selecting a device by name selects the record in front of it, and the
# comparison here is between a recorder and a painter.
render pgm:whole "$work/direct.pgm" || fail=1
direct=$out
render pgm:band "$work/played.pgm" || fail=1
played=$out
render pgm:band "$work/noentry.pgm" -DSAB=1 || fail=1
noentry=$out

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

if cmp -s "$work/direct.pgm" "$work/played.pgm"; then
    echo "OK   the recorded page and the painted page are the same bytes"
else
    echo "FAILURES: a page of text played back from its record is not the"
    echo "      page that was painted directly"
    cmp "$work/direct.pgm" "$work/played.pgm" 2>&1 | sed 's/^/      /' | head -3
    fail=1
fi

# The device that paints has no record to ask about, and must not be
# answering the branch that reports one.
if printf '%s\n' "$direct" | grep -q '^RECORD '; then
    echo "FAILURES: the directly painted run reported a record, so the"
    echo "      comparison is not between a recorder and a painter"
    fail=1
fi

# ... and the demonstration that the comparison above cannot see the
# thing this test is for. With the device's glyph entry taken away the
# page is identical and the record is the ink.
if cmp -s "$work/direct.pgm" "$work/noentry.pgm"; then
    echo "OK   the page is the same with the glyph entry taken away, so no" \
         "comparison of pages could ever see this"
else
    echo "FAILURES: taking the glyph entry away changed the page. The two"
    echo "      routes resolve the same coverage and must paint the same"
    echo "      pixels; either they no longer do, or the defect this run"
    echo "      builds is not the defect it means to build"
    fail=1
fi

# $1 what a run said, $2 what to call it; sets ink marks masks maskbytes
# bytes raster shown, or answers false
readings() {
    d_what=$2
    d_rec=$(field "$1" RECORD)
    d_ras=$(field "$1" RASTER)
    d_ink=$(field "$1" INK)
    d_shown=$(field "$1" SHOWN)
    if [ -z "$d_rec" ] || [ -z "$d_ras" ] || [ -z "$d_ink" ] ||
       [ -z "$d_shown" ]; then
        echo "FAILURES: the $d_what run did not report what it recorded, so"
        echo "      nothing here establishes anything about it"
        return 1
    fi
    # shellcheck disable=SC2086
    set -- $d_rec
    if [ "$#" -ne 4 ]; then
        echo "FAILURES: the $d_what run reported $# figures and this expects"
        echo "      4 (marks, masks, mask bytes, bytes)"
        return 1
    fi
    marks=$1; masks=$2; maskbytes=$3; bytes=$4
    ink=$d_ink; raster=$d_ras; shown=$d_shown
    return 0
}

# $1 what a run said, $2 what to call it; prints a complaint per reading
# that is wrong and answers whether all of them were right
weigh() {
    w_fail=0
    readings "$1" "$2" || return 1
    if [ "$ink" -lt "$minink" ]; then
        echo "FAILURES: the $2 page comes to $ink inked pixel(s), under the"
        echo "      $minink this reads anything against; a page with nothing"
        echo "      on it satisfies every bound below by having nothing"
        w_fail=1
    fi
    if [ "$ink" -lt $((marks * perglyph)) ]; then
        echo "FAILURES: the $2 page comes to $ink inked pixel(s) over $marks"
        echo "      mark(s), under the $perglyph pixels a mark that would"
        echo "      make holding a mark per glyph worth anything"
        w_fail=1
    fi
    if [ "$marks" -gt $((shown + slack)) ]; then
        echo "FAILURES: the $2 record holds $marks mark(s) for the $shown"
        echo "      glyph(s) the page shows; text reached it as a mark per"
        echo "      inked pixel rather than as a mark per glyph"
        w_fail=1
    fi
    if [ "$masks" -lt 1 ]; then
        echo "FAILURES: the $2 record holds no coverage mask although the"
        echo "      page is nothing but text"
        w_fail=1
    elif [ $((masks * share)) -gt "$marks" ]; then
        echo "FAILURES: the $2 record holds $masks mask(s) for $marks mark(s),"
        echo "      under the $share placements a mask has to serve; the"
        echo "      coverage is being copied per placement rather than shared"
        w_fail=1
    fi
    if [ "$bytes" -gt $((maskbytes * 2 + marks * permark)) ]; then
        echo "FAILURES: the $2 record cost $bytes bytes holding $marks mark(s)"
        echo "      and $maskbytes bytes of mask, past the $permark bytes a"
        echo "      mark this allows; what it costs is not following what it"
        echo "      holds"
        w_fail=1
    fi
    if [ "$bytes" -ge "$raster" ]; then
        echo "FAILURES: the $2 record cost $bytes bytes and the raster it"
        echo "      saves holding costs $raster, so it saves nothing"
        w_fail=1
    fi
    return $w_fail
}

if weigh "$played" "record" > "$work/played.out" 2>&1; then
    readings "$played" record >/dev/null 2>&1
    echo "OK   $marks marks and $masks masks of $maskbytes bytes for $shown" \
         "glyphs and $ink inked pixels: $bytes bytes against a raster of" \
         "$raster"
else
    cat "$work/played.out"
    fail=1
fi

# ---------------------------------------------------------------------
# The run's own controls
#
# Every reading above answers by finding nothing wrong. A reading that
# had stopped looking would answer the same way, so each is run against a
# defect built to trip it and required to trip.
#
#   1  the device's glyph entry taken away, so text is written down a
#      mark per inked pixel. It is the defect the whole entry exists to
#      prevent, and the page it produces is identical.
#   2  the ink counter answering zero, which is the instrument the two
#      readings about the page's ink are taken with.
#   3  the mask count answering the mark count, which is the sharing
#      gone: a mask per placement rather than one for all of them.
# ---------------------------------------------------------------------
for s in 1 2 3; do
    case $s in
        1) why="text written down a mark per inked pixel" ;;
        2) why="the ink counter answering zero" ;;
        3) why="a coverage mask copied per placement" ;;
    esac
    if [ "$s" -eq 1 ]; then
        sabout=$noentry
    else
        render pgm:band "$work/sab$s.pgm" -DSAB=$s || {
            echo "FAILURES: the run with $why could not be rendered"
            fail=1
            continue
        }
        sabout=$out
    fi
    if weigh "$sabout" "defect $s" > "$work/sab$s.out" 2>&1; then
        echo "FAILURES: this run passed with $why; the reading that is"
        echo "      supposed to catch it is not catching anything"
        fail=1
    else
        echo "OK   the run fails on $why:" \
             "$(grep -m1 '^FAILURES:' "$work/sab$s.out" | cut -c11- | cut -c1-52)"
    fi
done

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
