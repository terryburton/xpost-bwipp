#!/bin/sh
# What the PDF writer holds outside virtual memory must reach a steady
# state, page after page.
#
# Everything a page files -- its images, its cells, its form descriptions,
# the graphics states its content selects -- is held beside the content
# in memory the interpreter's own accounting cannot see. That is
# deliberate: a restore must not reach it, and a page's marks are not
# undone by one. The cost is that globalvmstatus does not weigh it, so
# tests/global_vm_invariant_test.ps, which is what refuses an operation
# that spends memory it goes on naming, is blind here. A page that went
# on holding what it filed would grow a long-lived context by that much
# for every page it served, and nothing measuring virtual memory would
# say a word.
#
# So the figure is asked for directly. The page below files one of each
# kind and is repeated; what is checked is not a total, which is a
# property of the host and of the allocator, but the SHAPE: the figure
# stops moving. Storage a page gave back is storage the next page takes
# again, so the steady state is a plateau and not a fall.
#
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-pdf-accumulator-cost.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0

# one page filing an image, a tiling cell and a form, inside a save
# bracket so the filing takes the route a restore would otherwise undo
cat > "$work/page.ps" <<'EOF'
/F << /FormType 1 /BBox [0 0 40 40] /Matrix [1 0 0 1 0 0]
      /PaintProc { pop 0 0 40 40 rectfill } >> def
/onepage {
    save
    gsave 100 100 translate 60 60 scale
    4 4 8 [4 0 0 -4 0 4]
      { <ff000000ff000000ff00ff00ff00ff00ff00ff00ff00ff00> } false 3 colorimage
    grestore
    gsave 200 200 translate F execform grestore
    << /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 10 10]
       /XStep 10 /YStep 10 /PaintProc { pop 0 0 5 5 rectfill } >>
      matrix makepattern setpattern
    300 300 100 100 rectfill
    true setoverprint
    100 400 100 50 rectfill
    restore
    showpage
} def
/cost { DEVICE 1183615869 internaldict /.pdfcost get exec } def
1 1 12 {
    pop
    onepage
    cost 20 string cvs print (\n) print
} for
flush
EOF

# The figure is asked of the device itself, and a run reaches its own
# device only when it was started to report on the interpreter.
XPOST_CENSUS=1 "$xpost" -q -d pdfwrite -o "$work/out%d.pdf" "$work/page.ps" </dev/null \
    > "$work/costs.txt" 2>"$work/err.txt" \
    || { echo "FAIL: the run errored"; sed -n 1,4p "$work/err.txt"; exit 1; }

n=$(grep -c '^[0-9][0-9]*$' "$work/costs.txt" || true)
[ "${n:-0}" -ge 12 ] || {
    echo "FAIL: expected a figure a page, got ${n:-0}"
    sed -n 1,4p "$work/costs.txt"; exit 1; }

# The first pages pay one-off costs -- a buffer taken at its first
# growth, a table's first block -- so the steady state is read from the
# later ones. Every one of them must be the figure the one before was.
last=""
i=0
for v in $(grep '^[0-9][0-9]*$' "$work/costs.txt" | tail -6); do
    i=$((i + 1))
    if [ -n "$last" ] && [ "$v" != "$last" ]; then
        echo "FAIL: still moving at the ${i}th of the last six: $last then $v"
        echo "      the whole series: $(tr '\n' ' ' < "$work/costs.txt")"
        fail=1
    fi
    last=$v
done

test $fail -eq 0 && echo "OK: what the writer holds outside VM reaches a steady state"
exit $fail
