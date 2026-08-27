#!/bin/sh
# Meson test wrapper: what the machinery notes about a render in progress
# must outlive the program's own restore.
#
# The page's note, a pattern cell's and a glyph's are each written while a
# procedure of the program's is running and read after it returns.
# Generated PostScript brackets such a procedure in save and restore as a
# matter of course, and restore reverts local virtual memory wholesale
# (PLRM 3.7.2) -- so a note kept in that bank is rewound between the write
# and the read, and the machinery acts on what the program erased. Moving
# any of the three there costs a render, and the three cases below are the
# renders it costs: page numbering that collapses onto one file, a Type 3
# glyph that comes out blank with the pen where it started, and a pattern
# cell filed with marks that belong to the page rather than to the cell.
#
# Each case is judged by what came out. Nothing here reads the notes: what
# the machinery keeps is its own, and a wrapper that reached in would be
# holding the note rather than the render.
#
# Each case carries a control -- a render beside it that must come out
# differently -- so a case that has stopped seeing anything fails on its
# control rather than reporting the render clean.
#
#   $1  path to the built xpost binary
#   $2  path to the PostScript workload
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

verdict_workdir

fail=0
note() { echo "FAIL: $*"; fail=1; }

# ---------------------------------------------------------------- pages
#
# The separation-plate idiom: a save and a restore around each page. The
# count of pages transmitted fills the %d in the output name, so three
# pages kept apart write three files and three pages that rewound the
# count write one.
out=$("$xpost" -q --no-sandbox -DCASE=/pages -d pgm \
      -o "$work/p%d.pgm" "$script" </dev/null 2>&1)
verdict_run "$?" "$out" "the page-numbering run" || exit 1

wrote=0
for f in "$work"/p*.pgm; do
    [ -e "$f" ] && wrote=$((wrote + 1))
done
if [ "$wrote" -ne 3 ]; then
    note "three pages under save and restore wrote $wrote files, not 3;" \
         "the page count did not survive the restore"
else
    cmp -s "$work/p1.pgm" "$work/p2.pgm" \
        && note "the first two pages are the same page -- this run cannot" \
                "tell one page from another"
fi

# ---------------------------------------------------------------- glyph
#
# Two Type 3 glyphs that paint the same box and declare the same width,
# one bracketing its build in save and restore and one not. Page 1 is
# blank, page 2 the unbracketed glyphs, page 3 the bracketed ones.
out=$("$xpost" -q --no-sandbox -DCASE=/glyph -d pgm \
      -o "$work/g%d.pgm" "$script" </dev/null 2>&1)
verdict_run "$?" "$out" "the Type 3 glyph run" || exit 1

pen=$(printf '%s\n' "$out" | sed -n 's/^  control pen: //p')
[ "$pen" = "180" ] \
    || note "the unbracketed glyphs moved the pen $pen units, not 180 --" \
            "this run is not showing text and its verdict means nothing"
pen=$(printf '%s\n' "$out" | sed -n 's/^  bracketed pen: //p')
[ "$pen" = "180" ] \
    || note "a build bracketed in save and restore moved the pen $pen units," \
            "not 180; the advance it declared did not survive the restore"

if [ ! -f "$work/g1.pgm" ] || [ ! -f "$work/g2.pgm" ] || [ ! -f "$work/g3.pgm" ]
then
    note "the glyph run did not write its three pages"
else
    cmp -s "$work/g1.pgm" "$work/g2.pgm" \
        && note "the unbracketed glyphs left the page blank -- this run" \
                "cannot see a glyph's marks and its verdict means nothing"
    cmp -s "$work/g2.pgm" "$work/g3.pgm" \
        || note "a build bracketed in save and restore painted a different" \
                "page from the same build without the bracket"
fi

# ---------------------------------------------------------------- cells
#
# A tiling pattern whose paint procedure brackets its own work and paints
# with a second pattern. The second pattern anchors its marks to the page,
# so they are not the cell's and the cell cannot be filed for the writer
# to repeat: the document must carry no tiling pattern. The control is the
# same pattern painting a colour of its own, which IS the cell's, and
# which the writer does file.
patterns_in() {    # $1 the document
    LC_ALL=C tr -d '\000' < "$1" | LC_ALL=C grep -c '/Type /Pattern' || true
}

out=$("$xpost" -q --no-sandbox -DCASE=/cells -d pdfwrite \
      -o "$work/cells.pdf" "$script" </dev/null 2>&1)
verdict_run "$?" "$out" "the pattern-cell run" || exit 1

out=$("$xpost" -q --no-sandbox -DCASE=/cellcontrol -d pdfwrite \
      -o "$work/control.pdf" "$script" </dev/null 2>&1)
verdict_run "$?" "$out" "the pattern-cell control run" || exit 1

for f in "$work/cells.pdf" "$work/control.pdf"; do
    head -c 8 "$f" 2>/dev/null | LC_ALL=C grep -q '%PDF-1' \
        || note "$(basename "$f") is not a document -- the pattern runs" \
                "wrote nothing to weigh"
done

n=$(patterns_in "$work/control.pdf")
[ "$n" -eq 1 ] \
    || note "the control document carries $n tiling patterns, not 1 --" \
            "this run cannot see a cell being filed and its verdict means" \
            "nothing"
n=$(patterns_in "$work/cells.pdf")
[ "$n" -eq 0 ] \
    || note "a cell painted with a second pattern was filed as a tiling" \
            "pattern ($n in the document); the note that the cell could not" \
            "be carried did not survive the restore"

[ "$fail" -eq 0 ] || exit 1

echo "  page count, glyph advance and pattern cell each survived a restore"
echo "SUCCESS"
