#!/bin/sh
# Meson test wrapper: what becomes of the page a job of text leaves behind.
#
# A page mark is the record that a job painted something, and a job that
# painted and never asked for its page still has one to give. A file
# written to be included in another document carries no showpage -- the
# including document supplies it -- and a great many such files paint
# nothing but text.
#
# Text does not reach the page the way a path does. A path is scan
# converted by the painting operators, which record the mark as they go;
# a glyph is rendered and its pixels written straight through the
# device's pixel methods, or handed to a device that draws glyphs as
# outlines, or reduced to the ink box an extent-tracking device wants.
# None of those passes a painting operator, so each has to record what it
# left for itself, and each is a separate way for the record to be
# missed.
#
# The converse matters as much, and is the reason the mark cannot simply
# be made by the show operators. A string with no characters in it paints
# nothing. A string of blanks selects glyphs with no outline and paints
# nothing either. And a glyph the clip keeps nothing of reaches the page
# nowhere, exactly as a path outside the clip does -- a page that would
# be blank must not be transmitted merely because a program asked for
# text on it.
#
# Read from the device rather than from the interpreter's own account: a
# page either arrived or it did not. What a run left is read as the bytes
# of the file it was told to write plus the bytes it wrote to its own
# output, so a device that reports its page rather than rastering one is
# read on the same terms as the rest.
#
# Every marking device, not one of them. Which of the three routes above
# a device takes is the device's own choice, so a rule held on one device
# is a rule held on one route; asking all of them holds it on all three,
# and holds them in agreement.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

ns=$(sandbox_flag "$xpost")

topwork=$(mktemp -d)
trap 'rm -rf "$topwork"' EXIT INT TERM

fail=0

# Run the interpreter and hand back everything it wrote, the newlines it
# ended with included, along with how it ended.
#
# A command substitution strips the newlines its command's output ends
# with, so how much of a run's output survives being captured turns on
# what the run printed last rather than on what it left, and a program
# that ends on a newline is measured a byte short of one that does not.
# The measure below is one byte wide with no page behind it, so that byte
# decides cases. The sentinel carries the newlines through the
# substitution and comes off again here; the status comes out of the
# subshell the same way.
#   sets out to what the run wrote, and st to how it ended
run_out() {         # the interpreter and its arguments
    out=$( "$@" </dev/null 2>&1; _run_st=$?; printf 'X'; exit "$_run_st" )
    st=$?
    out=${out%X}
}

# What a run left, as one number: the bytes of the file it was told to
# write plus the bytes it wrote to its own output.
#
# Both halves are the program's. These runs name an output file and have
# no terminal on their standard input, so the interpreter frames nothing
# for them -- no greeting, no page-boundary announcement, no prompt --
# and the output channel carries what the program printed and nothing
# else. That is what makes the two halves addable: framing that arrived
# on the second half would be counted as a page. run_case holds the runs
# to it rather than subtracting it here, so framing that came back would
# be reported instead of removed.
left() {            # file output
    _left_n=0
    if [ -s "$1" ]; then
        _left_n=$(wc -c < "$1")
    fi
    _left_m=$(printf '%s' "$2" | wc -c)
    echo $((_left_n + _left_m))
}

# Run one program on the current device and hold what it left against
# what it should have left.
run_case() {        # name expect program
    name=$1; expect=$2; prog=$3
    printf '%s\n' "$prog" > "$work/$name.ps"
    rm -f "$work/$name.$dev"
    run_out "$xpost" -q $ns -d "$dev" -o "$work/$name.$dev" "$work/$name.ps"
    verdict_run "$st" "$out" "the $name job on $dev" || { fail=$((fail + 1)); return; }
    # what left counts on: the output channel is the program's alone
    case $out in
        *----showpage----*)
            echo "FAILURES: $dev: $name was framed by a page-boundary" \
                 "announcement on its output, which the measure below would" \
                 "read as a page"
            fail=$((fail + 1))
            return ;;
    esac
    got=$(left "$work/$name.$dev" "$out")

    if [ "$expect" = page ]; then
        if [ "$got" -le "$empty" ]; then
            echo "FAILURES: $dev: $name left no page, and the program painted one"
            fail=$((fail + 1))
        fi
    else
        if [ "$got" -gt "$empty" ]; then
            echo "FAILURES: $dev: $name left a page of $got units, and should have left none"
            fail=$((fail + 1))
        fi
    fi
}

# A glyph from a font the interpreter builds for itself, so that the
# route through a program's own build procedure is asked about on a
# build with no font library and on one with no font installed. The
# glyph is painted by an image mask, which is what a bitmap font's build
# procedure does.
built='8 dict dup begin
  /FontType 3 def
  /FontMatrix [0.001 0 0 0.001 0 0] def
  /FontBBox [0 0 1000 1000] def
  /Encoding 256 array def
  0 1 255 { Encoding exch /.notdef put } for
  Encoding 97 /square put
  /CharProcs 2 dict def
  CharProcs /square {
      1000 0 0 0 1000 1000 setcachedevice
      8 8 true [0.008 0 0 -0.008 0 8] {<ffffffffffffffff>} imagemask
  } put
  CharProcs /.notdef { 0 0 setcharwidth } put
  /BuildChar { exch begin Encoding exch get CharProcs exch get exec end } def
end
/GlyphPageT3 exch definefont pop
/GlyphPageT3 40 selectfont
72 72 moveto (aaa) show'

mark='0 0 moveto 40 0 lineto 40 40 lineto 0 40 lineto closepath fill'
text='/Helvetica 24 selectfont 72 72 moveto (hi) show'

asked=0

# One device's whole share of the question. It is run for each device at
# once, so everything it writes goes under a directory of its own: the
# case files are named for the case, and two devices running the same
# case would otherwise be writing one path from two runs.
one_device() {
    dev=$1
    work="$topwork/$dev"
    mkdir -p "$work" || return 1
    fail=0

    # Whether this device can be asked at all, established from the
    # device rather than assumed. Two jobs are run whose answers are not
    # in doubt: one paints and transmits, one paints nothing. A device
    # whose output tells those two apart can be asked about the cases
    # that follow; a device that answers the same either way -- one that
    # keeps its raster for whoever embedded the interpreter -- cannot,
    # and says so instead of being held to a reading its output does not
    # carry.
    printf '%%!PS\n%s\nshowpage\n' "$mark" > "$work/cal-page.ps"
    printf '%%!PS\n%% this job paints nothing\n' > "$work/cal-none.ps"
    run_out "$xpost" -q $ns -d "$dev" -o "$work/cal-page.$dev" "$work/cal-page.ps"
    verdict_run "$st" "$out" "the calibration page on $dev" || return 1
    sz_page=$(left "$work/cal-page.$dev" "$out")
    run_out "$xpost" -q $ns -d "$dev" -o "$work/cal-none.$dev" "$work/cal-none.ps"
    verdict_run "$st" "$out" "the calibration blank on $dev" || return 1
    sz_none=$(left "$work/cal-none.$dev" "$out")

    if [ "$sz_page" -le "$sz_none" ]; then
        echo "$dev: its output does not tell a transmitted page from none; not asked"
        return 2
    fi
    # anything above what the no-page job left is a page having arrived
    empty=$sz_none

    # Whether text reaches this device at all, and which of the glyph
    # routes it takes. Both are read off a job that shows a string and
    # transmits its page: an interpreter with no font to draw with leaves
    # such a job as blank as one that painted nothing, and the glyph
    # cases below have nothing to hold. The device names its own route by
    # the key it carries, so the reading follows the device rather than a
    # list of names kept beside it.
    printf '%%!PS\nDEVICE /VectorGlyphs known { (VECTORGLYPHS) print } if\n%s\nshowpage\n' \
        "$text" > "$work/cal-text.ps"
    run_out "$xpost" -q $ns -d "$dev" -o "$work/cal-text.$dev" "$work/cal-text.ps"
    verdict_run "$st" "$out" "the calibration text on $dev" || return 1
    sz_text=$(left "$work/cal-text.$dev" "$out")
    vector=no
    case "$out" in *VECTORGLYPHS*) vector=yes ;; esac

    # A glyph a program's own build procedure paints reaches every device
    # whether or not the interpreter can draw an installed font, so that
    # one is asked before the reading above is consulted.
    run_case built page "%!PS
$built"

    if [ "$sz_text" -le "$empty" ]; then
        echo "$dev: no installed font reaches it; its glyph routes not asked"
        [ "$fail" -eq 0 ] || return 1
        echo "$dev: the page a job of text leaves behind is what it should be"
        return 0
    fi

    # Painted by show and never asked for: the ordinary shape of an
    # included file whose whole content is a line of text.
    run_case shown page "%!PS
$text"

    # Painted by glyphshow, which selects the glyph itself rather than
    # through the font's encoding and reaches the font by another route.
    run_case glyphshown page "%!PS
/Helvetica 24 selectfont 72 72 moveto /h glyphshow"

    # A string with no characters in it: show was called and nothing was
    # painted, so there is no page to end.
    run_case nostring nothing "%!PS
/Helvetica 24 selectfont 72 72 moveto () show"

    # A string of blanks: the glyphs are selected and rendered, and they
    # have no outline between them, so again nothing is painted.
    run_case blankglyphs nothing "%!PS
/Helvetica 24 selectfont 72 72 moveto (   ) show"

    # The clip keeping nothing of the text: the glyphs meet the region
    # the same set of pixels a fill of their outline would (PLRM 7.5.1),
    # and that set is empty, so the page is as it was found.
    #
    # Asked of the devices that render a glyph. A device that draws
    # glyphs as outlines records the text and the clip that hides it, so
    # what it leaves is not empty however little of it would be seen --
    # the question this case asks is about ink on a page, and a stream of
    # drawing operators is not one.
    if [ "$vector" = no ]; then
        run_case clipped nothing "%!PS
newpath 0 0 2 2 rectclip
/Helvetica 24 selectfont 72 72 moveto (hi) show"
    fi

    [ "$fail" -eq 0 ] || return 1
    echo "$dev: the page a job of text leaves behind is what it should be"
    return 0
}

# The devices that cannot be read for a transmitted page of text, and
# why. null paints nothing; raster and bgr keep their raster for whoever
# embedded the interpreter instead of writing a file, so there is nothing
# on disk to tell a page from none. bbox is not among them: it records
# the extent a glyph covers, and that extent is what it is read for.
CANNOT_ANSWER='null raster bgr'

fleet_each one_device $DEVICE_FLEET_MARKING || fail=1
fleet_hold_unasked "$CANNOT_ANSWER" || fail=1
asked=$fleet_asked

# A roster that answered for nothing reports as quietly as one that
# answered for everything.
if [ "$asked" -eq 0 ]; then
    echo "FAILURES: no device answered, so the rule was held against nothing"
    exit 1
fi
echo "glyph-page: held on $asked device(s)"

verdict_exit
