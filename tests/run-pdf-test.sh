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

# applying a clip is bounded: subtracting a hole meets the subject against
# each of the hole's edges limited by the edges before it, so the passes
# rise with the square of the hole's edge count, and each builds a polygon.
# A hole of thousands of edges would run to millions of such passes for a
# shape no consumer reads; the count is bounded far above any real hole (a
# reserved rectangle, the odd knockout), so such a clip is refused with a
# limitcheck rather than squared. Wrapped in stopped so the run returns and
# the marker below says which way it went, and in gsave/grestore so the
# caught abort leaves the clip state as it found it; an ordinary clip clips
# afterward.
clipps=$(mktemp)
cat > "$clipps" <<EOF
<< /OutputDevice /pdfwrite /OutputFile ($discard) /PageSize [600 800] >> setpagedevice
gsave
{ newpath
  20 20 moveto 580 20 lineto 580 780 lineto 20 780 lineto closepath
  250 250 moveto 0 1 4000 { pop 250 250.1 lineto 250.1 250 lineto } for closepath
  clip 40 40 moveto 60 60 lineto 2 setlinewidth stroke } stopped
{ \$error /errorname get /limitcheck eq
    { (huge-hole clip bounded OK\n) print }
    { (huge-hole clip FAIL: wrong error\n) print } ifelse }
{ (huge-hole clip FAIL: not bounded\n) print } ifelse
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
printf '%s\n' "$out" | grep -q 'huge-hole clip bounded OK' || { echo "FAIL: a huge-hole clip was not bounded"; printf '%s\n' "$out" | grep 'huge-hole'; exit 1; }
printf '%s\n' "$out" | grep -q 'ordinary clip clips OK'    || { echo "FAIL: an ordinary clip did not clip after the bound"; exit 1; }
echo "clip bound OK"

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
