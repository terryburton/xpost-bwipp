#!/bin/sh
# The pdfwrite device carries the overprint graphics-state parameter into the
# PDF. Setting overprint (or the overprint mode) makes a paint reference an
# ExtGState resource that declares /OP, /op and /OPM, so a separation workflow
# reads the intent the program set. A document that never touches overprint
# carries no ExtGState at all, so an ordinary page is unchanged.
#
# The ExtGState lives in the page object's /Resources, written uncompressed,
# and a resource entry is filed only when a `gs` operator was emitted for it,
# so reading the resource is reading whether the content referenced it. The
# content stream itself may be deflate-compressed, so it is not read here.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-pdf-overprint.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0
oppdf="$work/op.pdf"; plainpdf="$work/plain.pdf"; droppdf="$work/drop.pdf"; rawpdf="$work/raw.pdf"
opps="$work/op.ps"; plainps="$work/plain.ps"; dropps="$work/drop.ps"; rawps="$work/raw.ps"

cat > "$opps" <<'EOF'
1 0 0 0 setcmykcolor 100 100 200 200 rectfill
true setoverprint 1 setoverprintmode
0 1 0 0 setcmykcolor 150 150 200 200 rectfill
showpage
EOF
cat > "$plainps" <<'EOF'
1 0 0 0 setcmykcolor 100 100 200 200 rectfill
0 1 0 0 setcmykcolor 150 150 200 200 rectfill
showpage
EOF
# PreserveOverprintSettings false drops the overprint from the output
cat > "$dropps" <<'EOF'
<< /PreserveOverprintSettings false >> setdistillerparams
true setoverprint 0 1 0 0 setcmykcolor 150 150 200 200 rectfill
showpage
EOF
# CompressPages false leaves the content stream readable
cat > "$rawps" <<'EOF'
<< /CompressPages false >> setdistillerparams
true setoverprint 0 1 0 0 setcmykcolor 150 150 200 200 rectfill
showpage
EOF

"$xpost" -q -d pdfwrite -o "$oppdf" "$opps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: the overprint run errored"; exit 1; }
"$xpost" -q -d pdfwrite -o "$plainpdf" "$plainps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: the plain run errored"; exit 1; }
"$xpost" -q -d pdfwrite -o "$droppdf" "$dropps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: the PreserveOverprintSettings run errored"; exit 1; }
"$xpost" -q -d pdfwrite -o "$rawpdf" "$rawps" </dev/null >/dev/null 2>&1 \
    || { echo "FAIL: the CompressPages run errored"; exit 1; }

# the overprint page declares an ExtGState carrying the whole state
grep -q '/ExtGState' "$oppdf" || { echo "FAIL: no /ExtGState in the overprint PDF"; fail=1; }
grep -q '/OP true'   "$oppdf" || { echo "FAIL: /OP true not written"; fail=1; }
grep -q '/op true'   "$oppdf" || { echo "FAIL: /op true not written"; fail=1; }
grep -q '/OPM 1'     "$oppdf" || { echo "FAIL: /OPM 1 not written"; fail=1; }

# an ordinary page carries none of it
grep -q '/ExtGState' "$plainpdf" && { echo "FAIL: a plain page emitted an /ExtGState"; fail=1; }

# PreserveOverprintSettings false: overprint set, but nothing carried
grep -q '/ExtGState' "$droppdf" && { echo "FAIL: overprint survived PreserveOverprintSettings false"; fail=1; }

# CompressPages false: the content stream is not deflate-filtered and its
# operators are readable in the file
grep -q 'FlateDecode' "$rawpdf" && { echo "FAIL: CompressPages false still compressed the content"; fail=1; }
grep -q '0 1 0 0 k'  "$rawpdf" || { echo "FAIL: CompressPages false content not readable"; fail=1; }

if [ "$fail" -eq 0 ]; then echo SUCCESS; else exit 1; fi
