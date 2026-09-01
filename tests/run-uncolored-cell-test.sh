#!/bin/sh
# The cell an uncoloured tiling pattern files carries the colour it was
# given, so the same pattern asked for in two colours files two cells.
# A device that reuses a filed cell is the one place that invariant is
# visible, which is why this runs through svgwrite rather than a raster:
# on a raster the two fills are simply painted, and a cell shared between
# them would look the same as two cells that agree.
#
# The refusal is exercised here too, and through the same device, because
# the cell capture is a second path to a pattern's paint procedure and a
# restriction held on one path only would be worse than none.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

work=${TMPDIR:-/tmp}/xpost-uncolored-cell-$$
mkdir -p "$work" || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

cat > "$work/clean.ps" <<'EOF'
/P << /PatternType 1 /PaintType 2 /TilingType 1 /BBox [0 0 10 10]
      /XStep 10 /YStep 10 /PaintProc { pop 0 0 5 5 rectfill } >> def
[/Pattern /DeviceGray] setcolorspace
P matrix makepattern 0.75 exch setcolor  0 0 40 40 rectfill
P matrix makepattern 0.10 exch setcolor 50 0 40 40 rectfill
showpage
EOF

cat > "$work/sets-a-colour.ps" <<'EOF'
/P << /PatternType 1 /PaintType 2 /TilingType 1 /BBox [0 0 10 10]
      /XStep 10 /YStep 10 /PaintProc { pop 0.25 setgray 0 0 5 5 rectfill } >> def
[/Pattern /DeviceGray] setcolorspace
P matrix makepattern 0.75 exch setcolor 0 0 40 40 rectfill
showpage
EOF

fail=0

out=$(XPOST_DATA_DIR=${XPOST_DATA_DIR:-} "$xpost" -q -d svgwrite -g 100x60+0+0 \
        -o "$work/clean.svg" "$work/clean.ps" </dev/null 2>&1)
verdict_run $? "$out" "the clean cell" || fail=1
# the filed cells themselves, not every reference to one: the document
# also carries references that are not cells
#
# This holds the invariant; it does not find a procedure that breaks it. A
# cell that sets no colour files one cell per colour whether or not the
# refusal below is in force, so this assertion passes on its own strength
# and answers only for the invariant staying true. The refusal is what
# answers for a procedure that sets a colour. Neither covers the other, and
# taking either away leaves what the remaining one does not reach.
cells=$(grep -coE '<pattern[[:space:]]' "$work/clean.svg" 2>/dev/null || echo 0)
if [ "${cells:-0}" -ne 2 ]; then
    echo "FAILURES: the same pattern in two colours filed $cells cell(s), not 2;"
    echo "      a cell carries the colour it was given, so two colours are two cells"
    fail=1
fi

out=$(XPOST_DATA_DIR=${XPOST_DATA_DIR:-} "$xpost" -q -d svgwrite -g 100x60+0+0 \
        -o "$work/sets.svg" "$work/sets-a-colour.ps" </dev/null 2>&1)
if ! printf '%s\n' "$out" | grep -q "undefined"; then
    echo "FAILURES: a paint procedure setting a colour was not refused on the"
    echo "      path that files a cell; it answered: $out"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "SUCCESS (two colours file two cells; a cell setting a colour is refused)"
verdict_exit
