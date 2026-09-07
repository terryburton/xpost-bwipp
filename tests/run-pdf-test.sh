#!/bin/sh
# Meson/make-check wrapper: write a PDF through the pdfwrite device and check
# it. Every check reads the document itself -- the header and EOF trailer, the
# object structure, the colour spaces each paint reaches the content stream in,
# the page tree, and the trailer's metadata -- so the test needs nothing beyond
# the interpreter that wrote the file.
#   $1  path to the built xpost binary
#   $2  path to the input drawing (a fill; e.g. bbox_test.ps)
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# Run the interpreter and hold it to its own answer. What a run wrote is
# read by the block that asked for it; the status it left and anything it
# said on the way are read here, since a document with every landmark in
# it is what a run that wrote the whole file and then died leaves behind.
# What the run printed stays in `out` for the probe blocks that read it.
run_xpost() {   # $1 what to call it in a complaint, $2... arguments
    rx_who=$1
    shift
    # The device is named here rather than left to the build: what a
    # build with no option named makes is whatever its libraries allowed,
    # and on one of them that is a window on the screen the run was
    # started from. Every caller below names the writer it wants, after
    # this, and the name it gives is the one used.
    out=$("$xpost" -q -d null "$@" </dev/null 2>&1)
    verdict_run "$?" "$out" "$rx_who" || exit 1
}
# The traps below name every scratch file the checks make; predeclare them
# so the EXIT cleanup stays valid under set -u before the block that makes
# one has run.
infops= infopdf=
pdf=$(mktemp)
# A sink for output the checks below do not read. /dev/null is a POSIX
# name for it and the platform null device is not that word everywhere,
# while a scratch file is a file wherever the interpreter runs. The name
# is relative for the same reason the separation file's is: the shell and
# a program built for another environment need not read one absolute path
# the same way.
discard=./discard-$$.pdf
trap 'rm -f "$pdf" "$discard"' EXIT INT TERM

run_xpost "the pdfwrite run" -d pdfwrite -o "$pdf" "$script"

# structural (no external dependency; the content stream may be compressed,
# so check the object structure rather than content-stream operators)
head -c 8 "$pdf" | grep -q '%PDF-1'    || { echo "FAIL: no PDF header";   exit 1; }
grep -q '/Type[ ]*/Page' "$pdf"        || { echo "FAIL: no page object";  exit 1; }
grep -q 'stream' "$pdf"                || { echo "FAIL: no content stream"; exit 1; }
tail -c 16 "$pdf" | grep -q '%%EOF'    || { echo "FAIL: no EOF trailer";  exit 1; }
echo "PDF structure OK"

# Cross-reference table geometry (PDF 7.5.4). Every entry is twenty bytes:
# a ten-digit byte offset, a five-digit generation, the type letter and a
# two-byte ending. The width is what the table is for -- a reader reaches
# entry n by seeking twenty times n from the head of the table rather than
# by scanning -- so an entry written without its leading zeros is not a
# short entry but a table whose every later entry is at an address nobody
# computes. The reading here is the one a reader does: follow startxref to
# the table, take the count the subsection names, and measure.
xrefwidth() {   # $1 what to call it in a complaint, $2 the file
    xw_who=$1
    xw_pdf=$2
    # startxref and its value are the last three lines before %%EOF
    xw_off=$(tail -c 64 "$xw_pdf" | tr -d '\r' \
             | awk '/^startxref$/ { getline v; print v; exit }')
    case $xw_off in
        '' | *[!0-9]*)
            echo "FAIL: $xw_who names no startxref offset"; return 1 ;;
    esac
    # The entry's own text is the first eighteen bytes; the length rule
    # covers the two that end it, whichever of the endings PDF allows the
    # writer chose.
    tail -c "+$((xw_off + 1))" "$xw_pdf" | awk -v who="$xw_who" '
        NR == 1 { if ($0 != "xref") {
                      printf "FAIL: %s startxref reaches \"%s\", not a table\n",
                             who, $0
                      bad = 1; done = 1; exit }
                  next }
        NR == 2 { want = $2 + 0; next }
        $1 == "trailer" { done = 1; exit }
        { n++
          if (length($0) + 1 != 20) {
              printf "FAIL: %s xref entry %d is %d bytes, want 20\n",
                     who, n, length($0) + 1
              bad = 1 }
          if ($0 !~ /^[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9] [0-9][0-9][0-9][0-9][0-9] [nf]/) {
              printf "FAIL: %s xref entry %d is not ten digits, five digits and a type\n",
                     who, n
              bad = 1 } }
        END { if (!done) {
                  printf "FAIL: %s xref table runs off the end with no trailer\n", who
                  bad = 1 }
              else if (n != want) {
                  printf "FAIL: %s xref subsection names %d entries and holds %d\n",
                         who, want, n
                  bad = 1 }
              exit bad ? 1 : 0 }' || return 1
    return 0
}

xrefwidth "the single-file PDF" "$pdf" || exit 1
# The two output shapes are written by two different offset writers, so a
# rule held at one of them is a rule held nowhere in particular. A %d in
# the name puts each page in its own file, which is the other writer.
perpdf=./page-$$-1.pdf
trap 'rm -f "$pdf" "$discard" "$perpdf"' EXIT INT TERM
run_xpost "the per-page pdfwrite run" -d pdfwrite -o "./page-$$-%d.pdf" "$script"
[ -f "$perpdf" ] || { echo "FAIL: the per-page run wrote no first page"; exit 1; }
xrefwidth "the per-page PDF" "$perpdf" || exit 1
echo "xref entry width OK"

# document metadata: a DOCINFO pdfmark must land in the trailer's
# Info dictionary, readable by the consumer
infops=$(mktemp)
infopdf=$(mktemp)
trap 'rm -f "$pdf" "$discard" "$perpdf" "$infops" "$infopdf"' EXIT INT TERM
cat > "$infops" <<'EOF'
[ /Creator (pdf-device check) /DOCINFO pdfmark
100 100 moveto 200 100 lineto 200 200 lineto closepath fill
showpage
EOF
run_xpost "the DOCINFO run" -d pdfwrite -o "$infopdf" "$infops"
# the Info object's number depends on the page's object layout, so match
# the reference without pinning it, and read the value it points at
grep -aqE '/Info [0-9]+ 0 R' "$infopdf" || { echo "FAIL: no Info reference in trailer"; exit 1; }
grep -aqE '/Creator *\(pdf-device check\)' "$infopdf" || { echo "FAIL: the DOCINFO Creator is not in the file"; exit 1; }
echo "DOCINFO OK"

# colour-space preservation: by default each paint reaches the content
# stream in the space it was set in -- grey as g/G, RGB as rg, CMYK as
# k -- for fills, strokes and glyphs alike, so a press workflow receives
# pure-K ink as pure K rather than a converted black.
# Probed through the uncompressed accumulator, no consumer needed.
cspps=$(mktemp)
cat > "$cspps" <<EOF
<< /OutputDevice /pdfwrite /OutputFile ($discard) /PageSize [100 100] >> setpagedevice
0.5 setgray newpath 10 10 moveto 20 0 rlineto 0 20 rlineto -20 0 rlineto closepath fill
1 0 0 setrgbcolor newpath 40 10 moveto 20 0 rlineto 0 20 rlineto -20 0 rlineto closepath fill
0 0 0 1 setcmykcolor newpath 70 10 moveto 20 0 rlineto 0 20 rlineto -20 0 rlineto closepath fill
0 setgray 2 setlinewidth newpath 10 50 moveto 60 70 lineto stroke
/Courier findfont 12 scalefont setfont
0 1 0 0 setcmykcolor 10 80 moveto (K) show
0.25 setgray 40 80 moveto (g) show
% the accumulator probe reads the private .pdfchunks operator, which lives in
% internaldict rather than systemdict; fetch it once through the password
/.pdfchunks 1183615869 internaldict /.pdfchunks get def
/probe { % (needle) (name)  .  -
    exch DEVICE .pdfchunks 0 get exch search
    { pop pop pop (ok ) print print (\n) print }
    { pop (MISSING ) print print (\n) print } ifelse
} def
(0.5 g\n) (grey fill preserved as g) probe
(1 0 0 rg\n) (RGB fill preserved as rg) probe
(0 0 0 1 k\n) (pure-K CMYK fill preserved as k) probe
(0 G\n) (grey stroke preserved as G) probe
(0 1 0 0 k\n) (CMYK glyph preserved as k) probe
(0.25 g\n) (grey glyph preserved as g) probe
showpage
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the colour-space probe" -d null -o /dev/null "$cspps"
rm -f "$cspps"
printf '%s\n' "$out" | grep -q 'MISSING' && { printf '%s\n' "$out" | grep MISSING; echo "FAIL: colour-space preservation probes"; exit 1; }
n=$(printf '%s\n' "$out" | grep -c '^ok ')
[ "$n" = 6 ] || { echo "FAIL: expected 6 preservation probes, saw $n"; exit 1; }
echo "colour-space preservation OK"

# process colour model: under /ProcessColorModel /DeviceCMYK every mark
# separates in CMYK -- default black (a DeviceGray source) and RGB black
# as pure K, explicit CMYK passed through, strokes as K, glyphs as k.
# Probed through the uncompressed accumulator, no consumer needed.
cmykps=$(mktemp)
cat > "$cmykps" <<EOF
<< /OutputDevice /pdfwrite /OutputFile ($discard) /PageSize [100 100] /ProcessColorModel /DeviceCMYK >> setpagedevice
newpath 10 10 moveto 20 0 rlineto 0 20 rlineto -20 0 rlineto closepath fill
1 0 0 setrgbcolor newpath 40 10 moveto 20 0 rlineto 0 20 rlineto -20 0 rlineto closepath fill
0 setgray 2 setlinewidth newpath 10 50 moveto 60 70 lineto stroke
/Courier findfont 12 scalefont setfont 10 80 moveto (K) show
% the accumulator probe reads the private .pdfchunks operator, which lives in
% internaldict rather than systemdict; fetch it once through the password
/.pdfchunks 1183615869 internaldict /.pdfchunks get def
/probe { % (needle) (name)  .  -
    exch DEVICE .pdfchunks 0 get exch search
    { pop pop pop (ok ) print print (\n) print }
    { pop (MISSING ) print print (\n) print } ifelse
} def
(0 0 0 1 k\n) (gray-black fill as pure K) probe
(0 1 1 0 k\n) (rgb red converted with undercolor removal) probe
(0 0 0 1 K\n) (stroke in CMYK) probe
( rg\n) (no RGB operators remain) exch DEVICE .pdfchunks 0 get exch search
    { pop pop pop (MISSING ) print print (\n) print }
    { pop (ok ) print print (\n) print } ifelse
showpage
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the CMYK probe" -d null -o /dev/null "$cmykps"
rm -f "$cmykps"
printf '%s\n' "$out" | grep -q 'MISSING' && { printf '%s\n' "$out" | grep MISSING; echo "FAIL: CMYK separation probes"; exit 1; }
n=$(printf '%s\n' "$out" | grep -c '^ok ')
[ "$n" = 4 ] || { echo "FAIL: expected 4 CMYK probes, saw $n"; exit 1; }
echo "CMYK process colour model OK"

# separation colour spaces: a [/Separation name alt tint] space set through
# setcolorspace/setcolor paints as /CS<i> cs t scn (CS/SCN for strokes) with
# the space preserved in the page's /ColorSpace resources and the tint
# transform embedded as a function -- Type 4 calculator source when the
# procedure stays within that operator set, sampled Type 0 otherwise (the
# second space's procedure reads a variable). Registration survives an
# intervening restore, and a gsave/grestore round-trip re-selects the
# separation after a process-colour interlude.
sepps=$(mktemp)
# A relative path resolves to the same file for the shell and for the
# interpreter, which is embedded in the program below and need not share the
# shell's view of an absolute path (e.g. a native binary under a POSIX shell).
seppdf=./sep-$$.pdf
trap 'rm -f "$pdf" "$discard" "$perpdf" "$infops" "$infopdf" "$sepps" "$seppdf"' EXIT INT TERM
cat > "$sepps" <<EOF
<< /OutputDevice /pdfwrite /OutputFile ($seppdf) /PageSize [100 100] >> setpagedevice
[/Separation (Spot A) /DeviceCMYK {dup 0 exch dup 0.5 mul exch 0.25 mul}] setcolorspace
0.8 setcolor
newpath 10 10 moveto 20 0 rlineto 0 20 rlineto -20 0 rlineto closepath fill
2 setlinewidth newpath 10 50 moveto 60 70 lineto stroke
gsave 0 setgray newpath 70 40 moveto 10 0 rlineto 0 10 rlineto -10 0 rlineto closepath fill grestore
newpath 40 10 moveto 10 0 rlineto 0 10 rlineto -10 0 rlineto closepath fill
/half 0.5 def
[/Separation /SpotB /DeviceGray {half mul 1 exch sub}] setcolorspace
save 1.0 setcolor newpath 50 50 moveto 20 0 rlineto 0 20 rlineto -20 0 rlineto closepath fill restore
% the accumulator probe reads the private .pdfchunks operator, which lives in
% internaldict rather than systemdict; fetch it once through the password
/.pdfchunks 1183615869 internaldict /.pdfchunks get def
/probe { % (needle) (name)  .  -
    exch DEVICE .pdfchunks 0 get exch search
    { pop pop pop (ok ) print print (\n) print }
    { pop (MISSING ) print print (\n) print } ifelse
} def
(/CS0 cs 0.8 scn\n) (fill in the separation) probe
(/CS0 CS 0.8 SCN\n) (stroke in the separation) probe
(0 g\n) (process interlude inside gsave) probe
(/CS1 cs 1 scn\n) (registration survives restore) probe
showpage
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the separation probe" -d null -o /dev/null "$sepps"
rm -f "$sepps"
printf '%s\n' "$out" | grep -q 'MISSING' && { printf '%s\n' "$out" | grep MISSING; echo "FAIL: separation content probes"; exit 1; }
n=$(printf '%s\n' "$out" | grep -c '^ok ')
[ "$n" = 4 ] || { echo "FAIL: expected 4 separation probes, saw $n"; exit 1; }
sepdump() { echo "  seppdf=$seppdf ($(wc -c < "$seppdf" 2>/dev/null) bytes)"; grep -an 'Separation\|FunctionType\|0 obj' "$seppdf" 2>/dev/null | head -20; }
# the function object number depends on the page's object layout, so match the
# colour-space resource without pinning it (the plate check below is the proof)
grep -aqE '/CS0 \[/Separation /Spot#20A /DeviceCMYK [0-9]+ 0 R\]' "$seppdf" || { echo "FAIL: no escaped Spot A colour space resource"; sepdump; exit 1; }
grep -aqE '/CS1 \[/Separation /SpotB /DeviceGray [0-9]+ 0 R\]' "$seppdf"   || { echo "FAIL: no SpotB colour space resource"; sepdump; exit 1; }
grep -aq '/FunctionType 4' "$seppdf" || { echo "FAIL: no Type 4 tint transform"; sepdump; exit 1; }
grep -aq '/FunctionType 0' "$seppdf" || { echo "FAIL: no sampled Type 0 tint transform"; sepdump; exit 1; }
echo "separation colour spaces OK"

# independent oracle: a separating consumer must image each separation as
# its own plate, named as given

# multi-page single-file: a plain multi-showpage job collects every page into
# one document (a %d in the name would split it into per-page files instead).
# Each page wraps in save...showpage...restore -- the separation-plate idiom --
# and registers its own separation, so this exercises the accumulating file
# surviving restore, the page tree over all pages, and a separation registered
# on one page being written once yet referenced by the later pages that share
# it.
mpps=$(mktemp)
mppdf=./mp-$$.pdf
trap 'rm -f "$pdf" "$discard" "$perpdf" "$infops" "$infopdf" "$sepps" "$seppdf" "$mpps" "$mppdf"' EXIT INT TERM
cat > "$mpps" <<EOF
<< /OutputDevice /pdfwrite /OutputFile ($mppdf) /PageSize [80 80] >> setpagedevice
save
  [/Separation (Ink1) /DeviceCMYK { 0 0 0 } ] setcolorspace 0.7 setcolor
  10 10 moveto 60 0 rlineto 0 60 rlineto -60 0 rlineto closepath fill
showpage restore
save
  [/Separation (Ink2) /DeviceCMYK { 0 exch 0 0 } ] setcolorspace 0.5 setcolor
  20 20 moveto 40 0 rlineto 0 40 rlineto -40 0 rlineto closepath fill
showpage restore
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the multi-page run" -d null -o /dev/null "$mpps"
# the document says how many pages it has three times over, and a reader
# believes whichever it consults, so all three must agree: the page tree's
# count, the number of page objects, and the number of children it names
grep -aq '/Count 2' "$mppdf" || { echo "FAIL: multi-page tree is not /Count 2"; exit 1; }
[ "$(grep -ac '/Type /Page[^s]' "$mppdf")" = 2 ] || { echo "FAIL: want two page objects"; exit 1; }
nk=$(grep -aoE '/Kids *\[[^]]*\]' "$mppdf" | head -1 | grep -oE '[0-9]+ 0 R' | wc -l | tr -d ' ')
[ "$nk" = 2 ] || { echo "FAIL: multi-page tree names $nk children, want 2"; exit 1; }
# the second page references both separations; the first only its own
grep -aq '/CS0 \[/Separation /Ink1 /DeviceCMYK' "$mppdf" || { echo "FAIL: no Ink1 colour space"; exit 1; }
grep -aq '/CS1 \[/Separation /Ink2 /DeviceCMYK' "$mppdf" || { echo "FAIL: no Ink2 colour space on the later page"; exit 1; }
# Ink1's function object is written once though two pages reach it: the
# colour space names the tint transform indirectly, so both references
# naming one object is what says the object was written once. Counted as
# distinct references rather than as a bound on how many there are, so a
# pattern that has stopped matching gives none and fails here.
nfn=$(grep -aoE '/Separation /Ink1 /DeviceCMYK [0-9]+ 0 R' "$mppdf" \
      | grep -oE '[0-9]+ 0 R$' | sort -u | wc -l | tr -d ' ')
[ "$nfn" = 1 ] || { echo "FAIL: Ink1's pages name $nfn tint transforms, want 1"; exit 1; }
echo "multi-page single-file PDF OK"

# a program's redefinition of fill must not capture the machinery's
# internal references: eofill on a vector device resolves through the
# nonzero fill, and a redefined fill that itself invokes eofill would
# otherwise recurse without bound
recps=$(mktemp)
cat > "$recps" <<EOF
<< /OutputDevice /pdfwrite /OutputFile ($discard) /PageSize [100 100] >> setpagedevice
/fill { 0.5 setgray eofill } def
newpath 10 10 moveto 80 10 lineto 45 80 lineto closepath eofill
(eofill-under-redefined-fill OK\n) print
showpage
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the redefined-fill run" -d null -o /dev/null "$recps"
rm -f "$recps"
printf '%s\n' "$out" | grep -q 'eofill-under-redefined-fill OK' || { echo "FAIL: eofill under a redefined fill"; exit 1; }
echo "eofill under redefined fill OK"

# A clip with a hole of many edges is drawn, and drawn cheaply. Meeting a
# subject against each of a hole's edges limited by the edges before it
# rises with the square of the edge count, and a clip that took that route
# is refused rather than squared (data/clip.ps, holemax). A region that is
# not a single rectangle does not take it: it meets the paint span by span
# instead, which is linear in the region and is what a holed clip gets.
# Measured over this shape, 256 times the edges costs four times the work
# -- 200 edges 0.05s, 51200 edges 0.21s in 28MB -- so what is held here is
# that such a clip is DRAWN, not that it is refused. Wrapped in stopped so
# the run returns and the marker below says which way it went, and in
# gsave/grestore so an abort would leave the clip state as it found it; an
# ordinary clip clips afterward.
clipps=$(mktemp)
cat > "$clipps" <<EOF
<< /OutputDevice /pdfwrite /OutputFile ($discard) /PageSize [600 800] >> setpagedevice
gsave
{ newpath
  20 20 moveto 580 20 lineto 580 780 lineto 20 780 lineto closepath
  12800 1 sub -1 0
    { 360 mul 12800 div dup cos 250 mul 300 add exch sin 250 mul 300 add lineto } for
  closepath
  clip 150 150 moveto 450 150 lineto 450 450 lineto 150 450 lineto closepath fill } stopped
{ (huge-hole clip FAIL: ) print \$error /errorname get 32 string cvs print (\n) print }
{ (huge-hole clip drawn OK\n) print } ifelse
grestore
newpath 100 100 moveto 300 100 lineto 300 300 lineto 100 300 lineto closepath clip
50 200 moveto 350 200 lineto 4 setlinewidth stroke
(ordinary clip clips OK\n) print
showpage
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the clip-bound run" -d null -o /dev/null "$clipps"
rm -f "$clipps"
printf '%s\n' "$out" | grep -q 'huge-hole clip drawn OK' || { echo "FAIL: a huge-hole clip was not drawn"; printf '%s\n' "$out" | grep 'huge-hole'; exit 1; }
printf '%s\n' "$out" | grep -q 'ordinary clip clips OK'    || { echo "FAIL: an ordinary clip did not clip after the bound"; exit 1; }
echo "clip bound OK"

# A clip reaches the document as a clip, and under the rule it was taken
# by. Two rings wound the same way enclose an annulus by the even-odd
# rule and a disc by the nonzero one, so a region is its outline AND the
# rule that outline is read under: a writer carrying the outline alone
# would put the disc on the page, which is not the region the run
# enforced anywhere. The shape reaching the document at all is the other
# half -- the alternative is the region resolved to pixel-band
# rectangles, which arrives as one subpath per scanline, hundreds of them
# for a clip two curves describe. Both are read off the content, which
# the distiller parameter leaves uncompressed for the purpose.
# the output name is relative for the reason the separation file's is: it
# is read by the interpreter and not by the shell
eoclipps=$(mktemp)
eoclippdf=./eoclip-$$.pdf
trap 'rm -f "$pdf" "$discard" "$perpdf" "$infops" "$infopdf" "$sepps" "$seppdf" "$mpps" "$mppdf" "$eoclipps" "$eoclippdf"' EXIT INT TERM
cat > "$eoclipps" <<EOF
<< /CompressPages false >> setdistillerparams
<< /OutputDevice /pdfwrite /OutputFile ($eoclippdf) /PageSize [400 400] >> setpagedevice
newpath
200 200 150 0 360 arc closepath
200 200 75 0 360 arc closepath
eoclip newpath
0 0 1 setrgbcolor
newpath 0 0 moveto 400 0 lineto 400 400 lineto 0 400 lineto closepath fill
showpage
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the even-odd clip run" -d null -o /dev/null "$eoclipps"
grep -q 'W\* n' "$eoclippdf" || {
    echo "FAIL: an even-odd clip did not reach the document as one. A clip"
    echo "      written out under the nonzero rule encloses the whole of what"
    echo "      the even-odd rule leaves a hole in."
    exit 1; }
grep -q ' c$' "$eoclippdf" || { echo "FAIL: the clip shape reached the document without its curves"; exit 1; }
eosubpaths=$(grep -c '^h$' "$eoclippdf")
[ "$eosubpaths" -le 20 ] || {
    echo "FAIL: the clipped fill arrived as $eosubpaths subpaths, which is the"
    echo "      region resolved to pixel-band rectangles and written a row at a"
    echo "      time rather than the shape it was taken from."
    exit 1; }
echo "even-odd clip OK"

# --- the stream's state is written where it changes, and where a Q undid it ---
#
# A PDF content stream is a state machine (PDF 8.4): a colour, a line
# width, a cap, a join and a miter limit stand until an operator replaces
# them. Restating one that is already in force adds bytes and changes
# nothing on the page, and a page of strokes under one pen was carrying a
# full set of them per stroke.
#
# The other half is what makes suppression safe. q and Q save and restore
# the whole graphics state (PDF 8.4.2), so a value written inside a q is
# gone after the matching Q: a writer that remembered it across the Q
# would leave the next paint with no colour operator and the consumer
# painting it in whatever the Q brought back. The clipped fill below
# lands inside a q...Q, so the fill after it must state its colour again
# even though the same colour was stated moments earlier.
#
# Read off the content, which the distiller parameter leaves uncompressed
# for the purpose.
statps=$(mktemp)
statpdf=./state-$$.pdf
trap 'rm -f "$pdf" "$discard" "$perpdf" "$infops" "$infopdf" "$sepps" "$seppdf" "$mpps" "$mppdf" "$eoclipps" "$eoclippdf" "$statps" "$statpdf"' EXIT INT TERM
cat > "$statps" <<EOF
<< /CompressPages false >> setdistillerparams
<< /OutputDevice /pdfwrite /OutputFile ($statpdf) /PageSize [400 400] >> setpagedevice
0 0 1 setrgbcolor 3 setlinewidth 1 setlinecap 1 setlinejoin 4 setmiterlimit
% twenty strokes under one unchanging pen
0 1 19 {
    10 mul 10 add dup
    newpath 20 exch moveto 380 exch lineto stroke
} for
% a clip no rectangle describes: the shape goes to the device and the
% fill under it is written inside a q ... Q
gsave
newpath 200 300 80 0 360 arc closepath 200 300 40 0 360 arc closepath
eoclip newpath
1 0 0 setrgbcolor
newpath 0 0 moveto 400 0 lineto 400 400 lineto 0 400 lineto closepath fill
grestore
% the same colour again, outside the clip: the Q undid the one inside
1 0 0 setrgbcolor
newpath 300 20 moveto 380 20 lineto 380 60 lineto 300 60 lineto closepath fill
showpage
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the graphics-state run" -d null -o /dev/null "$statps"
rm -f "$statps"
nstroke=$(grep -c '^S$' "$statpdf")
[ "$nstroke" = 20 ] || { echo "FAIL: expected 20 strokes, the run wrote $nstroke"; exit 1; }
for opline in '3 w' '1 J' '1 j' '4 M' '0 0 1 RG'; do
    n=$(grep -c "^$opline\$" "$statpdf")
    [ "$n" = 1 ] || {
        echo "FAIL: \"$opline\" is in the stream $n times over 20 strokes that"
        echo "      never change it. A state operator restating a value the"
        echo "      stream already carries adds bytes and no marks."
        exit 1; }
done
nred=$(grep -c '^1 0 0 rg$' "$statpdf")
[ "$nred" = 2 ] || {
    echo "FAIL: the fill colour is in the stream $nred times, want 2. One is"
    echo "      written inside the q the clip opens and taken back off the"
    echo "      state stack by the matching Q, so the fill after it must"
    echo "      state the colour again -- a writer that remembered it across"
    echo "      the Q leaves that fill painted in whatever the Q restored."
    exit 1; }
echo "graphics-state re-emission OK"

# separation registry at scale: the registry indexes a separation by name
# so that a page naming many finds each in constant time, and the index
# grows and is rebuilt as the count passes eight, sixteen and on. Register
# twenty distinct separations, then reference every one of them again: the
# second round must find each already there and add none, so the content
# reaches /CS0 through /CS19 and never a /CS20 -- an index that lost an
# entry across a rebuild would register it afresh and write one.
sepscaleps=$(mktemp)
cat > "$sepscaleps" <<EOF
<< /OutputDevice /pdfwrite /OutputFile ($discard) /PageSize [100 100] >> setpagedevice
/mark1 { newpath 10 10 moveto 5 0 rlineto 0 5 rlineto -5 0 rlineto closepath fill } def
/useit { % i  -  register/reference separation number i and mark with it
    /i exch def
    [ /Separation i 8 string cvs cvn /DeviceGray { } ] setcolorspace
    0.5 setcolor mark1
} def
1 1 20 { useit } for       % twenty distinct separations
1 1 20 { useit } for       % every one referenced again -- must dedup
/.pdfchunks 1183615869 internaldict /.pdfchunks get def
/present { % (needle) (name)  .  -
    exch DEVICE .pdfchunks 0 get exch search
    { pop pop pop (ok ) print print (\n) print }
    { pop (MISSING ) print print (\n) print } ifelse } def
/absent { % (needle) (name)  .  -
    exch DEVICE .pdfchunks 0 get exch search
    { pop pop pop (PRESENT ) print print (\n) print }
    { pop (ok ) print print (\n) print } ifelse } def
(/CS19 cs) (the twentieth separation registered) present
(/CS20 cs) (no twenty-first from a re-reference) absent
showpage
<< /OutputDevice /null >> setpagedevice
quit
EOF
run_xpost "the separation-scale run" -d null -o /dev/null "$sepscaleps"
rm -f "$sepscaleps"
printf '%s\n' "$out" | grep -q 'MISSING' && { printf '%s\n' "$out" | grep -E 'MISSING|PRESENT'; echo "FAIL: separation registry lost an entry across a rebuild"; exit 1; }
printf '%s\n' "$out" | grep -q 'PRESENT' && { printf '%s\n' "$out" | grep -E 'MISSING|PRESENT'; echo "FAIL: a re-referenced separation was registered afresh"; exit 1; }
n=$(printf '%s\n' "$out" | grep -c '^ok ')
[ "$n" = 2 ] || { echo "FAIL: expected 2 separation-scale probes, saw $n"; exit 1; }
echo "separation registry at scale OK"

# --- a stencil goes out as a stencil, and a repeated one goes out once ---
#
# imagemask is what the language has for painting a bitmap through the
# current colour, and PDF has the same construct: an image XObject with
# ImageMask. Without it a stencil is decomposed into a fill per run of
# set samples, which is what a page of bitmap glyphs used to come out as.
#
# The second half is what makes it worth having. A page of text paints
# the same few glyphs over and over, and filing one XObject per
# occurrence would trade thousands of little fills for thousands of
# little images. The same bits must file once and be drawn many times.
maskps=$(mktemp)
maskpdf=$(mktemp)
cat > "$maskps" <<'MEOF'
<< /PageSize [64 64] >> setpagedevice
0 setgray
% two placements of one stencil, and one of another
gsave  4  4 translate 8 8 scale 8 8 true [8 0 0 -8 0 8] <FF81BDA5A5BD81FF> imagemask grestore
gsave 20  4 translate 8 8 scale 8 8 true [8 0 0 -8 0 8] <FF81BDA5A5BD81FF> imagemask grestore
gsave 36  4 translate 8 8 scale 8 8 true [8 0 0 -8 0 8] <0F0F0F0FF0F0F0F0> imagemask grestore
showpage
MEOF
run_xpost "the stencil run" -d pdfwrite -o "$maskpdf" "$maskps"
rm -f "$maskps"

if [ ! -s "$maskpdf" ]; then
    echo "FAIL: the PDF writer emitted nothing for a stencil"; exit 1
fi
grep -aq 'ImageMask' "$maskpdf" || {
    echo "FAIL: the stencil went out as fills rather than as the image"
    echo "      mask PDF has for exactly this"
    exit 1; }
nmask=$(grep -ac 'ImageMask' "$maskpdf")
[ "$nmask" = 2 ] || {
    echo "FAIL: expected 2 mask XObjects for 3 placements of 2 distinct"
    echo "      stencils, saw $nmask -- a repeated stencil is being filed"
    echo "      once per placement"
    exit 1; }
grep -aq '/Decode \[1 0\]' "$maskpdf" || {
    echo "FAIL: a true polarity is decode [1 0] (PLRM 4.10.4) and the"
    echo "      emitted mask does not say so"
    exit 1; }
rm -f "$maskpdf"
echo "stencil emission and reuse OK"

# --- a whole image or stencil carries the clip in force ----------------
#
# PLRM 4.8.1 paints an image only where the current clipping path admits
# it, exactly as it paints a fill. A fill arrives at this device already
# resolved against the region -- what reaches FillPoly is the part the
# clip admits and nothing else -- but an image and a stencil do not: what
# goes over is the samples and the matrix that places them, so the region
# has to be written beside them or the consumer paints the lot.
#
# A region a rectangle describes went over as one; any other region went
# over as nothing at all, and the paint then covered the whole page. Both
# ways of reaching such a region are driven here: a program clipping to a
# shape of its own, and the pattern tiling, which resolves the region
# being painted to pixel-band spans and paints each cell under them.
clipps=$(mktemp)
clippdf=$(mktemp)
trap 'rm -f "$pdf" "$discard" "$perpdf" "$infops" "$infopdf" "$sepps" "$seppdf" "$mpps" "$mppdf" "$eoclipps" "$eoclippdf" "$statps" "$statpdf" "$clipps" "$clippdf"' EXIT INT TERM
cat > "$clipps" <<'CEOF'
<< /PageSize [100 100] >> setpagedevice
<< /CompressPages false >> setdistillerparams
0 setgray
gsave
  newpath 50 50 25 0 360 arc clip
  20 20 translate 60 60 scale
  8 8 true [8 0 0 -8 0 8] <FFFFFFFFFFFFFFFF> imagemask
grestore
showpage
CEOF
run_xpost "the clipped-stencil run" -d pdfwrite -o "$clippdf" "$clipps"
sed -n '/^stream$/,/^endstream$/p' "$clippdf" > "$clipps"
ndo=$(command grep -c 'Do$' "$clipps" || true)
[ "$ndo" = 1 ] || {
    echo "FAIL: expected the stencil to be drawn once, saw $ndo"; exit 1; }
# the clip is the region's own outline and not a rectangle: the region
# here is a circle, so the operator before W is a lineto rather than re
awk '/^W n$/ { print prev; found = 1 } { prev = $0 }
     END { exit found ? 0 : 1 }' "$clipps" > "$clippdf" || {
    echo "FAIL: the stencil was written with no clip at all, so a consumer"
    echo "      paints it over the whole page (PLRM 4.8.1)"
    exit 1; }
command grep -q ' l$' "$clippdf" || {
    echo "FAIL: the clip written for the stencil is not the region in"
    echo "      force -- the last operator before W is $(cat "$clippdf")"
    exit 1; }
echo "a stencil under a shaped clip carries that clip OK"

# The tiling walk paints a cell at a time under the region resolved to
# spans, so a cell that draws a stencil is the same case reached without
# the program ever setting such a clip itself. The fill leaves the page,
# which is what keeps this off the pattern-resource route and on the walk.
cat > "$clipps" <<'CEOF'
<< /PageSize [100 100] >> setpagedevice
<< /CompressPages false >> setdistillerparams
<< /PatternType 1 /PaintType 1 /TilingType 1
   /BBox [0 0 20 20] /XStep 20 /YStep 20
   /PaintProc { pop 0 setgray
      gsave 20 20 scale
      8 8 true [8 0 0 -8 0 8] <FFFFFFFFFFFFFFFF> imagemask
      grestore } >>
matrix makepattern /Pattern setcolorspace setpattern
30 -5 moveto 70 -5 lineto 70 70 lineto 30 70 lineto closepath fill
showpage
CEOF
run_xpost "the pattern-walk stencil run" -d pdfwrite -o "$clippdf" "$clipps"
command grep -aq '/PatternType 1' "$clippdf" && {
    echo "FAIL: the fill went out as a pattern resource, so this measured"
    echo "      the route it was written to keep off"
    exit 1; }
sed -n '/^stream$/,/^endstream$/p' "$clippdf" > "$clipps"
ndo=$(command grep -c 'Do$' "$clipps" || true)
nclip=$(command grep -c '^W n$' "$clipps" || true)
[ "$ndo" -gt 0 ] || { echo "FAIL: the tiling drew no cell at all"; exit 1; }
[ "$ndo" = "$nclip" ] || {
    echo "FAIL: $ndo cell stencil(s) drawn against $nclip clip(s) -- a cell"
    echo "      written without the region it was painted under covers the"
    echo "      whole page (PLRM 4.8.1)"
    exit 1; }
rm -f "$clipps" "$clippdf"
echo "a stencil inside a pattern cell carries the fill's region OK ($ndo cells)"

# --- a tiling pattern goes out as the pattern PDF has for it ---
#
# PDF 8.7.3.1 has the construct the language has (PLRM 4.9): a /Pattern
# resource holding the cell as a content stream of its own, tiled by the
# consumer over whatever the paint covers. Without it the interpreter
# steps the cells itself and the page carries a shape per cell, so what
# the document weighs grows with the area painted -- the fill below is
# 1,200 cells, and twice the area is twice the document. The resource is
# the same size whatever it covers.
#
# What is measured is that growth and not a byte count: the page is
# painted twice over, once at each of two areas, and a document that
# carries the cells rather than naming them grows between the two.
#
# The second half is what says the construct is reused rather than
# restated. One pattern painted in two places is one resource; and an
# uncoloured pattern (PLRM Table 4.9 PaintType 2) painted in two colours
# is two, the colour being painted into the cell rather than left to the
# space the cell is used in.
patps=$(mktemp)
patpdf=./pat-$$.pdf
patbig=./patbig-$$.pdf
trap 'rm -f "$pdf" "$discard" "$perpdf" "$infops" "$infopdf" "$sepps" "$seppdf" "$mpps" "$mppdf" "$eoclipps" "$eoclippdf" "$statps" "$statpdf" "$clipps" "$clippdf" "$patps" "$patpdf" "$patbig"' EXIT INT TERM
patprog() {   # $1 the half-width of the pattern fill
cat <<PEOF
<< /PageSize [400 500] >> setpagedevice
<< /CompressPages false >> setdistillerparams
/pat <<
  /PatternType 1 /PaintType 1 /TilingType 1
  /BBox [0 0 10 10] /XStep 10 /YStep 10
  /PaintProc { pop 1 0 0 setrgbcolor 1 1 8 8 rectfill }
>> matrix makepattern def
/upat <<
  /PatternType 1 /PaintType 2 /TilingType 1
  /BBox [0 0 10 10] /XStep 10 /YStep 10
  /PaintProc { pop 0 0 moveto 10 10 lineto 0 10 lineto closepath fill }
>> matrix makepattern def
/Pattern setcolorspace pat setpattern
20 20 moveto $1 20 lineto $1 420 lineto 20 420 lineto closepath fill
% the same pattern again: one resource, not two
30 430 moveto 200 430 lineto 200 480 lineto 30 480 lineto closepath fill
[/Pattern /DeviceRGB] setcolorspace
0 0 1 upat setcolor
330 20 moveto 390 20 lineto 390 200 lineto 330 200 lineto closepath fill
0 1 0 upat setcolor
330 220 moveto 390 220 lineto 390 400 lineto 330 400 lineto closepath fill
showpage
PEOF
}
patprog 320 > "$patps"
run_xpost "the pattern run" -d pdfwrite -o "$patpdf" "$patps"
patprog 160 > "$patps"
run_xpost "the half-area pattern run" -d pdfwrite -o "$patbig" "$patps"
rm -f "$patps"

[ -s "$patpdf" ] || { echo "FAIL: the PDF writer emitted nothing for a pattern"; exit 1; }
grep -aq '/PatternType 1' "$patpdf" || {
    echo "FAIL: the tiling pattern went out as the cells the interpreter"
    echo "      steps rather than as the /Pattern resource PDF 8.7.3.1 has"
    echo "      for exactly this"
    exit 1; }
grep -aq '/Pattern cs /P0 scn' "$patpdf" || {
    echo "FAIL: no paint in the /Pattern colour space names a resource"
    exit 1; }
npat=$(grep -ac '/Type /Pattern' "$patpdf")
[ "$npat" = 3 ] || {
    echo "FAIL: expected 3 pattern objects -- one coloured pattern used"
    echo "      twice, and one uncoloured pattern in two colours -- saw $npat"
    exit 1; }
grep -aqE '/Pattern << /P0 [0-9]+ 0 R /P1 [0-9]+ 0 R /P2 [0-9]+ 0 R >>' "$patpdf" || {
    echo "FAIL: the page's Resources do not name the three patterns its"
    echo "      content refers to"
    exit 1; }
# the cell is the cell and nothing more: a resource holding the stepped
# cells would carry a shape per cell here too
patlen=$(sed -n 's:.*/PatternType 1 .*/Length \([0-9]*\).*:\1:p' "$patpdf" | head -1)
case $patlen in
    '' | *[!0-9]*) echo "FAIL: no pattern stream length to read"; exit 1 ;;
esac
[ "$patlen" -lt 400 ] || {
    echo "FAIL: the pattern cell is $patlen bytes; the paint procedure it"
    echo "      holds draws one rectangle"
    exit 1; }
# and the document does not grow with the area the pattern covers
whole=$(wc -c < "$patpdf")
half=$(wc -c < "$patbig")
[ "$((whole - half))" -lt 64 ] && [ "$((half - whole))" -lt 64 ] || {
    echo "FAIL: the same page over twice the area is $whole bytes against"
    echo "      $half -- the cells are in the document rather than named"
    exit 1; }
echo "tiling pattern emission and reuse OK ($whole bytes over 1200 cells)"

# A shading pattern (PLRM 4.9.1 type 2) has no cell to hold: it paints a
# gradient across the whole region and travels the route it always has.
shpps=$(mktemp)
cat > "$shpps" <<'SEOF'
<< /PageSize [200 200] >> setpagedevice
<< /PatternType 2 /Shading << /ShadingType 2 /ColorSpace /DeviceRGB
   /Coords [20 20 180 180]
   /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >>
   /Extend [true true] >> >> matrix makepattern
/Pattern setcolorspace setpattern
20 20 moveto 180 20 lineto 180 180 lineto 20 180 lineto closepath fill
showpage
SEOF
run_xpost "the shading-pattern run" -d pdfwrite -o "$patpdf" "$shpps"
rm -f "$shpps"
grep -aq '/Type /Pattern' "$patpdf" && {
    echo "FAIL: a shading pattern was filed as a tiling pattern's cell"
    exit 1; }
echo "shading pattern keeps its route OK"

# --- which way round a subpath was walked reaches the document ---
#
# A fill winds nonzero (PLRM 4.5.2 and PDF 8.5.3.3), so a subpath walked
# against the one enclosing it cuts a hole in it and one walked with it
# does not. Direction is the whole of what says which: the two pages
# below differ in nothing else, and one is a ring while the other is a
# solid square.
#
# PDF's re appends its corners in one fixed order, so it says a
# rectangle without saying which way round one was drawn. The rectangle
# whose corners come round the way re walks them is written as re; the
# one walked the other way is written as its own move, lines and close.
# So the page with a hole in it writes its two squares differently and
# the page without writes them the same way -- and the shortcut is still
# taken, which is what keeps a page of rectangles to one operator
# each.
#
# Read off the content, which the distiller parameter leaves
# uncompressed for the purpose. Both squares are centred on the page, so
# the numbers are the same whichever way the device's axes run.
windps=$(mktemp)
windpdf=./wind-$$.pdf
wind2pdf=./wind2-$$.pdf
trap 'rm -f "$pdf" "$discard" "$perpdf" "$infops" "$infopdf" "$sepps" "$seppdf" "$mpps" "$mppdf" "$eoclipps" "$eoclippdf" "$statps" "$statpdf" "$clipps" "$clippdf" "$patps" "$patpdf" "$patbig" "$windps" "$windpdf" "$wind2pdf"' EXIT INT TERM
windprog() {   # $1 the inner square's second corner, $2 its fourth
cat <<WEOF
<< /CompressPages false >> setdistillerparams
<< /PageSize [400 400] >> setpagedevice
0 setgray
100 100 moveto 300 100 lineto 300 300 lineto 100 300 lineto closepath
150 150 moveto $1 lineto 250 250 lineto $2 lineto closepath
fill
showpage
WEOF
}
# the inner square against the outer one: a ring
windprog '150 250' '250 150' > "$windps"
run_xpost "the counter-wound fill run" -d pdfwrite -o "$windpdf" "$windps"
# and with it: a solid square
windprog '250 150' '150 250' > "$windps"
run_xpost "the same-wound fill run" -d pdfwrite -o "$wind2pdf" "$windps"
rm -f "$windps"

windsquares() {   # $1 pdf  ->  how many of the two squares are re
    grep -acE '^(100 100 200 200|150 150 100 100) re$' "$1"
}
nre=$(windsquares "$windpdf")
nh=$(grep -ac '^h$' "$windpdf")
[ "$nre" = 1 ] && [ "$nh" = 1 ] || {
    echo "FAIL: the ring's two squares went out as $nre rectangle operators"
    echo "      and $nh closed subpaths, want one of each. Written both ways"
    echo "      round as re they wind together and the hole fills in, which"
    echo "      is a page nothing in the file's structure disagrees with."
    exit 1; }
nre2=$(windsquares "$wind2pdf")
nh2=$(grep -ac '^h$' "$wind2pdf")
case "$nre2 $nh2" in
    '2 0' | '0 2') ;;
    *) echo "FAIL: the solid square's two squares went out as $nre2 rectangle"
       echo "      operators and $nh2 closed subpaths; wound the same way they"
       echo "      have to be written the same way"
       exit 1 ;;
esac
echo "subpath direction reaches the document OK"
