#!/bin/sh
# Meson/make-check wrapper: hold the PDF the pdfwrite device writes to an
# independent consumer's reading of it.
#
# The self-contained checks in run-pdf-test.sh read the file the writer
# wrote, which is the one reading guaranteed to agree with the writer: a
# convention held consistently and wrongly at both ends passes every one
# of them. What no amount of self-reading reaches is whether a program
# that did not write the file can find its way through it, and that is
# the only property a PDF exists to have. So this wrapper hands the file
# to consumers from another lineage and holds the writer to what they
# make of it.
#
# Two consumers, for two kinds of question. qpdf answers the structural
# one -- the cross-reference table, the object and stream syntax, the
# trailer -- by parsing the document as a reader does and reporting every
# place it had to guess. poppler answers the imaging one, by rendering
# the page: what a consumer paints from our PDF is compared with what the
# interpreter paints from the drawing the PDF came from, pixel for pixel,
# so a mark that reached the file in the wrong place or not at all shows
# up as a difference rather than as a well-formed lie. Both sides of that
# imaging comparison come from the same drawing, though, so it says the
# file carries what the interpreter drew and cannot say the interpreter
# drew the right thing: a fault in the drawing stands in both rasters.
# What a page has to look like is the golden-render manifest's.
#
# Only what pdfwrite writes is read here. The other writing devices put
# out other languages -- dscwrite PostScript, svgwrite SVG -- and neither
# of those carries a cross-reference table for the structural reader to
# walk or a PDF page for the imaging one to paint, so neither consumer
# has a reading to give of them.
#
# Neither consumer is a build dependency. A tree without them skips.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

have_qpdf=no
have_poppler=no
command -v qpdf >/dev/null 2>&1 && have_qpdf=yes
# The rasters are compared without antialiasing: with it, two renderers
# disagree over every edge pixel's coverage and the comparison measures
# their filters rather than our geometry. A poppler too old to turn it
# off cannot make the imaging comparison, so it is treated as absent.
if command -v pdftoppm >/dev/null 2>&1 && command -v pdfinfo >/dev/null 2>&1 \
   && pdftoppm -h 2>&1 | grep -q -- '-aaVector'; then
    have_poppler=yes
fi
if [ "$have_qpdf" = no ] && [ "$have_poppler" = no ]; then
    echo "SKIP: neither qpdf nor poppler is available to read the PDF back"
    exit 77
fi

verdict_workdir
if [ -z "$work" ] || [ ! -d "$work" ]; then
    echo "SKIP: could not make a scratch directory (is TMPDIR writable?)"
    exit 77
fi

fail=0

# Run the interpreter and hold it to its own answer: what it left behind
# is read by the block that asked for it, and the status it left and
# anything it said on the way are read here.
run_xpost() {   # $1 what to call it in a complaint, $2... arguments
    rx_who=$1
    shift
    # The device is named here rather than left to the build: what a
    # build with no option named makes is whatever its libraries allowed,
    # and on one of them that is a window on the screen the run was
    # started from. Every caller below names the device it wants, after
    # this, and the name it gives is the one used.
    rx_out=$("$xpost" -q -d null "$@" </dev/null 2>&1)
    verdict_run "$?" "$rx_out" "$rx_who" || return 1
    return 0
}

# ---------------------------------------------------------------- rasters

# A PGM's pixels, without its header. Both producers write P5 with a
# 255 maximum and no comment, so the header is whatever precedes the
# w*h bytes of pixel data -- a length rather than a grammar, which is
# what makes it the same arithmetic for either producer.
pixels() {      # $1 pgm  $2 width  $3 height  -> writes the bytes to stdout
    px_sz=$(wc -c < "$1" | tr -d ' ')
    px_hdr=$((px_sz - $2 * $3))
    if [ "$px_hdr" -le 0 ] || [ "$px_hdr" -gt 32 ]; then
        echo "FAIL: $1 is $px_sz bytes, not a ${2}x${3} greymap" >&2
        return 1
    fi
    head -c 16 "$1" | grep -aq '^P5' || {
        echo "FAIL: $1 is not a binary greymap" >&2; return 1; }
    tail -c "+$((px_hdr + 1))" "$1"
    return 0
}

# Render the PDF through the consumer onto the same pixel grid the
# interpreter drew on. The grid is named rather than derived from a
# resolution so that the two rasters are comparable whatever the page's
# declared size; that size is checked separately, where a wrong one is
# the finding rather than a nuisance.
consume() {     # $1 pdf  $2 width  $3 height  $4 output pgm
    rm -f "$work/c.pgm"
    pdftoppm -gray -aa no -aaVector no \
             -scale-to-x "$2" -scale-to-y "$3" -singlefile "$1" "$work/c" \
             >/dev/null 2>&1
    [ -s "$work/c.pgm" ] || { echo "FAIL: the consumer rendered nothing from $1"; return 1; }
    mv "$work/c.pgm" "$4"
    return 0
}

# How many pixels two rasters of the same grid disagree about.
pixdiff() {     # $1 pgm  $2 pgm  $3 width  $4 height  -> prints a count
    pixels "$1" "$3" "$4" > "$work/pa" || return 1
    pixels "$2" "$3" "$4" > "$work/pb" || return 1
    cmp -l "$work/pa" "$work/pb" 2>/dev/null | wc -l | tr -d ' '
    return 0
}

# The box the ink occupies, in pixels of the raster's own grid, as
# "x0 y0 x1 y1"; "blank" where nothing marked. Both rasters run
# top-down, so two boxes are directly comparable.
inkbox() {      # $1 pgm  $2 width  $3 height  -> prints the box
    pixels "$1" "$2" "$3" | od -An -v -tu1 | awk -v w="$2" '
        { for (i = 1; i <= NF; i++) {
              if ($i + 0 < 128) {
                  x = n % w; y = int(n / w)
                  if (!seen) { x0 = x; x1 = x; y0 = y; y1 = y; seen = 1 }
                  else { if (x < x0) x0 = x; if (x > x1) x1 = x
                         if (y < y0) y0 = y; if (y > y1) y1 = y } }
              n++ } }
        END { if (seen) print x0, y0, x1, y1; else print "blank" }'
}

# One pixel of a raster, by its place on the grid. What a hole is made of
# is the pixels in the middle of a figure, and a count over the whole page
# answers about the boundary as readily as about the middle.
pixelat() {     # $1 pgm  $2 width  $3 height  $4 x  $5 y  -> the byte
    pixels "$1" "$2" "$3" | od -An -v -tu1 \
      | awk -v idx="$(( $5 * $2 + $4 ))" '
            { for (i = 1; i <= NF; i++) { if (n == idx) { print $i; exit }; n++ } }'
}

# How many pixels of a raster are dark.
inkcount() {    # $1 pgm  $2 width  $3 height  -> prints a count
    pixels "$1" "$2" "$3" | od -An -v -tu1 \
      | awk '{ for (i = 1; i <= NF; i++) if ($i + 0 < 128) n++ } END { print n + 0 }'
}

# Every edge of two boxes within a tolerance.
boxwithin() {   # $1 box  $2 box  $3 tolerance  -> 0 when they agree
    printf '%s\n%s\n' "$1" "$2" | awk -v tol="$3" '
        NR == 1 { for (i = 1; i <= 4; i++) a[i] = $i; na = NF }
        NR == 2 { for (i = 1; i <= 4; i++) b[i] = $i; nb = NF }
        END { if (na != 4 || nb != 4) exit 1
              for (i = 1; i <= 4; i++) {
                  d = a[i] - b[i]; if (d < 0) d = -d
                  if (d > tol) exit 1 }
              exit 0 }'
}

# ------------------------------------------------------ the drawings

# One fill, axis-aligned on integer coordinates, so the two renderers
# have nothing to disagree about at the edges.
cat > "$work/fill.ps" <<'EOF'
<< /PageSize [100 100] >> setpagedevice
0 setgray
10 10 moveto 40 10 lineto 40 50 lineto 10 50 lineto closepath fill
showpage
EOF

# Glyphs, which reach the PDF as outlines rather than as a font
# reference, so what the consumer paints is our own curves.
cat > "$work/text.ps" <<'EOF'
<< /PageSize [200 60] >> setpagedevice
/Helvetica findfont 20 scalefont setfont
0 setgray
10 20 moveto (Vector Glyphs) show
showpage
EOF

# White glyphs over a black field: marks in the current colour cut
# holes, marks in an assumed black leave the field solid.
cat > "$work/white.ps" <<'EOF'
<< /PageSize [200 60] >> setpagedevice
0 setgray
10 10 moveto 180 0 rlineto 0 40 rlineto -180 0 rlineto closepath fill
1 setgray
/Helvetica findfont 30 scalefont setfont
16 20 moveto (WHITE) show
showpage
EOF

# Two pages in one document. The page tree is the one structure a
# writer can get wrong in a way that reads back consistently -- a count
# and a list of children that agree with each other and with nothing
# else -- so what settles it is how many pages a reader finds.
cat > "$work/two.ps" <<'EOF'
<< /PageSize [80 80] >> setpagedevice
0 setgray
10 10 moveto 60 0 rlineto 0 60 rlineto -60 0 rlineto closepath fill
showpage
0.5 setgray
20 20 moveto 40 0 rlineto 0 40 rlineto -40 0 rlineto closepath fill
showpage
EOF

# A bent polyline at a width and a join the graphics state chose. The
# divergence a lost join or a defaulted width produces is under a pixel
# at screen resolution, so the page is imaged four times over.
cat > "$work/stroke.ps" <<'EOF'
<< /PageSize [50 40] /HWResolution [288 288] >> setpagedevice
0 setgray
0.75 setlinewidth 1 setlinejoin
10 10 moveto 15 13.5 lineto 10 17 lineto
25 10 moveto 32 17 lineto 39 10 lineto
stroke
showpage
EOF

# A subpath walked against the one around it, which the nonzero fill
# rule reads as a hole (PLRM 4.5.2). Nothing but the direction the inner
# square is walked in says so, and a writer that dropped it would put a
# solid square on the page while the file stayed well formed. A consumer
# is the only reader that can tell the two apart.
cat > "$work/hole.ps" <<'EOF'
<< /PageSize [100 100] >> setpagedevice
0 setgray
10 10 moveto 90 10 lineto 90 90 lineto 10 90 lineto closepath
30 30 moveto 30 70 lineto 70 70 lineto 70 30 lineto closepath
fill
showpage
EOF

# ------------------------------------------------------ the documents

# The documents both consumers read. Two output shapes -- the
# accumulating single file, and the per-page files a %d in the name
# selects -- are written by different code, so a reader's verdict on one
# is no verdict on the other.
run_xpost "the pdfwrite run" -d pdfwrite -o "$work/fill.pdf" "$work/fill.ps" || fail=1
run_xpost "the per-page pdfwrite run" \
          -d pdfwrite -o "$work/per%d.pdf" "$work/fill.ps" || fail=1
run_xpost "the two-page pdfwrite run" \
          -d pdfwrite -o "$work/two.pdf" "$work/two.ps" || fail=1

# ------------------------------------------------------ structural

if [ "$have_qpdf" = yes ]; then
    for f in "$work/fill.pdf" "$work/per1.pdf" "$work/two.pdf"; do
        [ -s "$f" ] || { echo "FAIL: $f was not written"; fail=1; continue; }
        if qpdf --check "$f" > "$work/qpdf.txt" 2>&1; then
            :
        else
            echo "FAIL: the reader would not read $(basename "$f") cleanly:"
            sed 's/^/      /' "$work/qpdf.txt"
            fail=1
        fi
    done
    [ "$fail" = 0 ] && echo "reader structural check OK"
else
    echo "note: qpdf absent, the structural reading is not made"
fi

# ------------------------------------------------------ imaging

if [ "$have_poppler" = yes ]; then
    # -- the fill, and the box it round-trips as ------------------------
    run_xpost "the fill raster run" \
              -d pgm -o "$work/fill.pgm" "$work/fill.ps" || fail=1
    size=$(pdfinfo "$work/fill.pdf" 2>/dev/null | sed -n 's/^Page size: *//p')
    case $size in
        '100 x 100 pts'*) ;;
        *) echo "FAIL: the reader sees the page as '$size', want 100 x 100 pts"
           fail=1 ;;
    esac
    if consume "$work/fill.pdf" 100 100 "$work/fill.consumed.pgm"; then
        d=$(pixdiff "$work/fill.pgm" "$work/fill.consumed.pgm" 100 100) || fail=1
        echo "fill: $d of 10000 pixels differ"
        # An axis-aligned fill on integer coordinates lands on the same
        # pixels under any correct scan conversion, so the budget is
        # there for the renderers to disagree about an edge, not for a
        # mark to move: it is a thousandth of the page.
        [ "${d:-99999}" -le 10 ] || {
            echo "FAIL: the consumer's fill differs from the interpreter's"; fail=1; }

        # The bounding box the drawing declares, realised as ink in the
        # PDF: the box measured from what a consumer paints must be the
        # box measured from what the interpreter paints.
        ba=$(inkbox "$work/fill.pgm" 100 100)
        bb=$(inkbox "$work/fill.consumed.pgm" 100 100)
        echo "fill ink box: interpreter [$ba] consumer [$bb]"
        [ "$ba" != blank ] || { echo "FAIL: the fill left no ink"; fail=1; }
        [ "$ba" = "$bb" ] || {
            echo "FAIL: the fill's box does not round-trip through the PDF"; fail=1; }
    else
        fail=1
    fi

    # -- the same page, written by the per-page writer -------------------
    #
    # A %d in the name makes every page a document of its own, and that
    # document is not the same bytes as the accumulating one: one begins
    # and ends around a single page, the other around a run of them. What
    # they paint has to be the same regardless, and the page here is the
    # same page, so the reference is what the interpreter itself rendered
    # above. Holding the per-page document to the accumulating one's bytes
    # would prove nothing, because they are meant to differ; holding it to
    # nothing at all is what left this route unwatched before.
    if consume "$work/per1.pdf" 100 100 "$work/per1.consumed.pgm"; then
        d=$(pixdiff "$work/fill.pgm" "$work/per1.consumed.pgm" 100 100) || fail=1
        echo "per-page fill: $d of 10000 pixels differ"
        [ "${d:-99999}" -le 10 ] || {
            echo "FAIL: the per-page document paints a page the interpreter did not"
            fail=1; }
        pa=$(inkbox "$work/fill.pgm" 100 100)
        pb=$(inkbox "$work/per1.consumed.pgm" 100 100)
        echo "per-page ink box: interpreter [$pa] consumer [$pb]"
        [ "$pa" != blank ] || { echo "FAIL: the per-page fill left no ink"; fail=1; }
        [ "$pa" = "$pb" ] || {
            echo "FAIL: the per-page document's box does not match the interpreter's"
            fail=1; }
    else
        fail=1
    fi

    # -- the page tree, as a reader walks it ----------------------------
    pages=$(pdfinfo "$work/two.pdf" 2>/dev/null | sed -n 's/^Pages: *//p')
    echo "two-page document: the reader finds ${pages:-no} pages"
    [ "${pages:-0}" = 2 ] || {
        echo "FAIL: the reader walks the page tree to ${pages:-nothing}, want 2"; fail=1; }

    # -- glyph outlines at the pen position -----------------------------
    run_xpost "the text pdfwrite run" \
              -d pdfwrite -o "$work/text.pdf" "$work/text.ps" || fail=1
    run_xpost "the text raster run" \
              -d pgm -o "$work/text.pgm" "$work/text.ps" || fail=1
    if consume "$work/text.pdf" 200 60 "$work/text.consumed.pgm"; then
        ba=$(inkbox "$work/text.pgm" 200 60)
        bb=$(inkbox "$work/text.consumed.pgm" 200 60)
        echo "text ink box: interpreter [$ba] consumer [$bb]"
        [ "$ba" != blank ] || { echo "FAIL: the text left no ink"; fail=1; }
        [ "$bb" != blank ] || { echo "FAIL: the text left no ink in the PDF"; fail=1; }
        # Glyphs are curves, and two scan conversions place a curve's
        # last covered pixel differently; the tolerance is for that one
        # pixel and no more. What it must not absorb is a glyph placed
        # at the wrong pen position or a run cut short, either of which
        # moves an edge by a glyph's width.
        boxwithin "$ba" "$bb" 3 || {
            echo "FAIL: the glyphs do not mark where the drawing puts them"; fail=1; }
    else
        fail=1
    fi

    # -- glyphs mark in the current colour ------------------------------
    run_xpost "the white-text pdfwrite run" \
              -d pdfwrite -o "$work/white.pdf" "$work/white.ps" || fail=1
    if consume "$work/white.pdf" 200 60 "$work/white.consumed.pgm"; then
        dark=$(inkcount "$work/white.consumed.pgm" 200 60)
        # The field alone is 180x40 = 7200 dark pixels. White glyphs at
        # 30 points carve out several hundred of them; glyphs painted in
        # an assumed black carve out none, and a lost field leaves far
        # too few.
        echo "white-on-black: $dark of 7200 field pixels remain dark"
        [ "${dark:-0}" -ge 5000 ] && [ "${dark:-0}" -le 6900 ] || {
            echo "FAIL: the glyphs did not mark in the current colour"; fail=1; }
    else
        fail=1
    fi

    # -- strokes, imaged at four times screen resolution ----------------
    run_xpost "the stroke pdfwrite run" \
              -d pdfwrite -o "$work/stroke.pdf" "$work/stroke.ps" || fail=1
    run_xpost "the stroke raster run" \
              -d pgm -o "$work/stroke.pgm" "$work/stroke.ps" || fail=1
    if consume "$work/stroke.pdf" 200 160 "$work/stroke.consumed.pgm"; then
        d=$(pixdiff "$work/stroke.pgm" "$work/stroke.consumed.pgm" 200 160) || fail=1
        echo "stroke: $d of 32000 pixels differ"
        n=$(inkcount "$work/stroke.pgm" 200 160)
        [ "${n:-0}" -gt 100 ] || { echo "FAIL: the stroke left no marks"; fail=1; }
        # A defaulted width or a lost join moves hundreds of pixels at
        # this resolution; the budget is a thousandth of the page, which
        # is the rounding of coordinates written to two decimals.
        [ "${d:-99999}" -le 32 ] || {
            echo "FAIL: the stroked path diverges from the drawing"; fail=1; }
    else
        fail=1
    fi
    # -- the hole a counter-wound subpath cuts --------------------------
    #
    # Judged by the hole rather than by a pixel budget. The two scan
    # conversions place an interior boundary a pixel apart, which puts a
    # band of a couple of hundred pixels around a 40-point square either
    # way, so the count of differing pixels is reported and not held to:
    # what a lost direction does is fill the hole in, and that is the
    # figure's own middle and a quarter of its area.
    run_xpost "the wound-hole pdfwrite run" \
              -d pdfwrite -o "$work/hole.pdf" "$work/hole.ps" || fail=1
    run_xpost "the wound-hole raster run" \
              -d pgm -o "$work/hole.pgm" "$work/hole.ps" || fail=1
    if consume "$work/hole.pdf" 100 100 "$work/hole.consumed.pgm"; then
        d=$(pixdiff "$work/hole.pgm" "$work/hole.consumed.pgm" 100 100) || fail=1
        echo "wound hole: $d of 10000 pixels differ at the boundaries"
        mid=$(pixelat "$work/hole.consumed.pgm" 100 100 50 50)
        [ "${mid:-0}" -ge 128 ] || {
            echo "FAIL: the middle of the ring is dark in the PDF, so the"
            echo "      inner subpath wound with the outer one instead of"
            echo "      against it and the hole filled in"
            fail=1; }
        # The ring is an 80-point square less the 40-point square the
        # inner subpath takes out of it: 6400 pixels less 1600. The two
        # renderings may differ around each boundary; they cannot differ
        # by the hole.
        na=$(inkcount "$work/hole.pgm" 100 100)
        nb=$(inkcount "$work/hole.consumed.pgm" 100 100)
        echo "wound hole: interpreter $na dark pixels, consumer $nb, ring is 4800"
        [ "$((nb - na))" -lt 800 ] && [ "$((na - nb))" -lt 800 ] || {
            echo "FAIL: the consumer paints $nb dark pixels against the"
            echo "      interpreter's $na; the hole is 1600 of them"
            fail=1; }
    else
        fail=1
    fi

    [ "$fail" = 0 ] && echo "consumer imaging OK"
else
    echo "note: poppler absent, the imaging comparison is not made"
fi

[ "$fail" = 0 ] || { echo "FAILURES: the PDF does not read back as it was drawn"; exit 1; }
exit 0
