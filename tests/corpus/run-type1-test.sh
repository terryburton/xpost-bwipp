#!/bin/sh
# Meson wrapper for the Type 1 corpus: real font programs, run through
# this interpreter and held to what they define and paint.
#
# A Type 1 font program is PostScript, and the part of it that describes
# the glyphs is reached only by running it: the interpreter deciphers
# the encrypted section, scans the binary charstrings out of it and
# hands the dictionaries to definefont. On the way it is asked a
# question no other kind of program asks. The standard array of
# procedures a font carries is built by running PostScript that reaches
# for the interpreter's internal dictionary -- the one use PLRM 8
# sanctions for it -- and what that dictionary answers decides whether
# the font program finishes or stops on the spot.
#
# So this corpus is not held to a picture. What it holds is what a font
# program of this shape must do here:
#
#   the program runs to the end, exits zero and reports no error;
#   it defines one font, of Type 1, carrying charstrings;
#   the glyphs its encoding names take a path apiece, each closing
#   every contour it opens and enclosing an area;
#   and the page they are painted on carries ink.
#
# Ink rather than a hash of the bytes, because the fonts are not this
# tree's: they are copied off the machine the test runs on, from
# whichever typesetting system is installed there, and a hash would hold
# this tree to a file nobody here controls. What does not move is that a
# font paints. "Carries ink" is read against a page this same
# interpreter and device produce for a program that paints nothing, so
# the comparison is to this build's own idea of a blank page.
#
# The register beside the fonts says which of them run their array of
# procedures as they load and which merely carry it, and a corpus
# holding none of the first kind cannot ask the question above -- so
# that is a skip naming what it wanted, not a pass.
#
#   - SKIP (exit 77) when the corpus holds no font, when it holds none
#     that reaches the interpreter as it loads, and when the build
#     carries no face library and so can define no such font at all.
#     The fonts belong to their makers and are not committed, so this is
#     never a build-time dependency -- run tests/corpus/fetch.sh type1
#     to make this test do its work.
#   - FAIL (exit 1) on any of the above, and on a font the corpus holds
#     and the register does not describe.
#   - PASS (exit 0) otherwise.
#
#   $1  path to the built xpost binary
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$here/../verdict.sh"

# A font program is an ordinary program, and this corpus is about what
# an ordinary run answers one. The census a wrapper takes by default is
# answered at internaldict with the machinery's own dictionary, so a
# font program asking the question this corpus was assembled around
# would be answered differently here from how it is answered in every
# run that ships -- which is the one answer a corpus of real programs
# must not be given.
unset XPOST_CENSUS

xpost=${1:?usage: run-type1-test.sh <path to xpost>}
xpost=$(path_anchor "$xpost")
d="$here/type1"

skip_if_faceless "$xpost" "this corpus defines fonts out of font programs"

# The register and the driver are ours and are committed, so their
# absence is a broken tree rather than a corpus waiting to be fetched.
for ours in fonts paint; do
    if [ ! -s "$d/$ours" ]; then
        echo "FAILURES: tests/corpus/type1/$ours is missing or empty, so the"
        echo "      corpus describes nothing and holds nothing to anything"
        exit 1
    fi
done

# What the register says about one font: the fields after its name, with
# a comment tail counting for nothing.
declared() {   # name field
    awk -v b="$1" -v f="$2" '{ sub(/#.*/, "") } $1 == b { print $f; exit }' \
        "$d/fonts"
}

held=0
runs=0
for p in "$d"/*.pfa "$d"/*.pfb; do
    [ -f "$p" ] || continue
    held=$((held + 1))
done

if [ "$held" = 0 ]; then
    echo "corpus: the Type 1 corpus holds no font program -- run"
    echo "        tests/corpus/fetch.sh type1, which copies what this"
    echo "        machine already has, then re-run. Skipping."
    exit 77
fi

fail=0
for p in "$d"/*.pfa "$d"/*.pfb; do
    [ -f "$p" ] || continue
    b=$(basename "$p")
    case $(declared "$b" 2) in
        runs)  runs=$((runs + 1)) ;;
        inert) ;;
        *)
            echo "FAIL: the corpus holds $b and the register says nothing"
            echo "      about it, so what it was fetched to exercise is"
            echo "      unrecorded"
            fail=1 ;;
    esac
done

if [ "$runs" = 0 ]; then
    echo "corpus: the Type 1 corpus holds $held font program(s) and none of"
    echo "        them reaches the interpreter as it loads, which is what"
    echo "        this test is for -- see tests/corpus/type1/fonts."
    echo "        Skipping."
    exit 77
fi

verdict_workdir

# A budget separates a font that is slow from one that will never
# finish, spent where there is a command to spend it.
if command -v timeout >/dev/null 2>&1; then
    budget='timeout 300'
else
    budget=''
fi

# A page with nothing on it, made by the interpreter under test on the
# device under test. Every page below is ink or is this.
printf '%%!PS\nshowpage\n' > "$work/blank.ps"
out=$("$xpost" -q -d ppm -o "$work/blank_%d.ppm" "$work/blank.ps" </dev/null 2>&1)
st=$?
verdict_run "$st" "$out" "the blank page the ink check reads against" || exit 1
if [ ! -s "$work/blank_1.ppm" ]; then
    echo "FAILURES: the blank page the ink check reads against was not"
    echo "      produced, so every page below would read as ink"
    exit 1
fi

# The driver reads its font as `program` beside itself and writes what
# it unwraps there too, so each font is run in a directory of its own
# holding nothing else.
cp "$d/paint" "$work/paint" || { echo "FAILURES: cannot stage the driver"; exit 1; }

ran=0
inked=0
glyphs=0
for p in "$d"/*.pfa "$d"/*.pfb; do
    [ -f "$p" ] || continue
    b=$(basename "$p")
    rm -f "$work/program" "$work/program.pfa" "$work"/pg_*.ppm
    cp "$p" "$work/program" || { echo "FAIL: cannot stage $b"; fail=1; continue; }

    # shellcheck disable=SC2086
    out=$(cd "$work" && $budget "$xpost" -q -d ppm -o pg_%d.ppm paint \
          </dev/null 2>&1)
    st=$?
    if [ "$st" = 124 ]; then
        echo "FAIL: $b did not finish inside its budget"
        fail=1
        continue
    fi
    verdict_run "$st" "$out" "$b" || { fail=1; continue; }
    # A PostScript error is reported and the run still ends cleanly, so
    # the status alone would pass a font program that stopped on its
    # first operator and defined nothing.
    if printf '%s\n' "$out" | grep -q 'Error:'; then
        echo "FAIL: $b reported a PostScript error:"
        printf '%s\n' "$out" | grep 'Error:' | sed 's/^/      /'
        fail=1
        continue
    fi

    # The driver's own account of the font, which is also what says it
    # reached the end: a run that stopped half way prints no such line.
    line=$(printf '%s\n' "$out" | grep '^type1: ')
    if [ -z "$line" ]; then
        echo "FAIL: $b produced no account of itself, so the driver did not"
        echo "      reach the end of it"
        fail=1
        continue
    fi
    ran=$((ran + 1))
    n=$(printf '%s\n' "$line" | sed -n 's/.* contours=\([0-9]*\).*/\1/p')
    case ${n:-x} in *[!0-9]*) n=0 ;; esac
    glyphs=$((glyphs + n))
    echo "  $line"

    if [ ! -s "$work/pg_1.ppm" ]; then
        echo "FAIL: $b drew no page"
        fail=1
    elif cmp -s "$work/pg_1.ppm" "$work/blank_1.ppm"; then
        echo "FAIL: $b painted the blank page, and a font paints something"
        fail=1
    else
        inked=$((inked + 1))
    fi
done

# The other direction on the register: a line naming a font the corpus
# does not hold is not a fault -- the fonts are whatever this machine
# had -- but a run that reached no font, or found ink on no page, has
# measured nothing whatever it exits with.
if [ "$ran" = 0 ] || [ "$inked" = 0 ] || [ "$glyphs" = 0 ]; then
    echo "FAILURES: the run reached $ran font program(s), found ink on"
    echo "      $inked page(s) and took $glyphs glyph(s) as paths, so it"
    echo "      measured nothing"
    exit 1
fi

echo "type1: $held font programs held, $ran run, $runs of them reaching the"
echo "       interpreter as they load, $inked pages with ink, $glyphs glyphs"
echo "       each closing every contour it opened"
[ "$fail" -eq 0 ] || exit 1
echo SUCCESS
exit 0
