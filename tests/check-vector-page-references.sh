#!/bin/sh
# A page names no description it does not carry, on every page and in
# every shape the vector writers write.
#
# The three writers all file a description once and place it wherever it
# recurs -- a form the program placed twice, a letter of a page of text
# that the glyph machinery filed for itself. The number that comes back
# from filing is what the placement names, and it is only good against
# the record that minted it. Where each page is a file of its own the
# record is given up at the page end, so a number kept across that
# boundary names a description the next page does not carry: a placement
# of nothing, or of whatever else has since been filed under the number.
#
# Neither outcome is an error the producing run can see. The file is well
# formed and the run reports success; a consumer either drops the mark or
# draws the wrong one, and a page of text comes out as a page of the
# wrong letters. tests/check-pdf-save-resource.sh holds the same
# invariant within one page. This one holds it across the page boundary,
# which is where the record moves.
#
# THE WORKLOAD draws one page three times. The same drawing written
# three times must come out the same three times, so the pages can be
# held to each other as well as to themselves -- and a page after the
# first is where a note kept across the boundary is read.
#
# The text is set in a face a reader cannot be assumed to have, because
# that is the face whose letters are drawn rather than named, and drawn
# letters are what the glyph machinery files. Which face the host
# substitutes does not matter here: what is checked is that the page
# defines what it names, whatever shapes those are.
#
# CompressPages false leaves the PDF content readable, so the names it
# uses can be read against the names its resources define.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-vector-page-references.sh <xpost binary>}
case $xpost in /*) ;; *) xpost=$(pwd)/$xpost ;; esac
. "$(dirname "$0")/guard-paths.sh"
# The page each check is read off shows text, because a page's fonts are
# one of the things named here: a build with no face library cannot make
# that page and says so rather than reading four checks off a page that
# was never written.
. "$(dirname "$0")/verdict.sh"
skip_if_faceless "$xpost" "the page these checks are read off shows text"
guard_workdir
fail=0

cat > "$work/pages.ps" <<'EOF'
<< /CompressPages false >> setdistillerparams
/F << /FormType 1 /BBox [0 0 40 40] /Matrix [1 0 0 1 0 0]
      /PaintProc { pop 0 0 40 40 rectfill } >> def
/page {
    gsave  60 300 translate F execform grestore
    gsave 160 300 translate F execform grestore
    /NotAFaceAReaderHas 10 selectfont
    1 1 12 {
        20 mul 700 exch moveto
        (the quick brown fox jumps over the lazy dog) show
    } for
    showpage
} def
page page page
EOF

run() {     # $1 device  $2 output name
    ( cd "$work" && "$xpost" -q -d "$1" -o "$2" pages.ps </dev/null >/dev/null 2>&1 ) \
        || { echo "FAIL: the $1 run for $2 errored"; fail=1; }
}

# What each format spells a definition and a placement with. A writer that
# stops filing anything is caught by the count check below rather than
# passing for having nothing to resolve.
defined() {     # $1 file  $2 device
    case $2 in
        pdfwrite) grep -aoE '/Fm[0-9]+ [0-9]+ 0 R' "$1" | awk '{print $1}' ;;
        dscwrite) grep -aoE '^/Fm[0-9]+ \{' "$1" | awk '{print $1}' ;;
        svgwrite) grep -aoE 'id="xq[0-9]+"' "$1" \
                  | sed 's|id="xq|/Fm|; s|"$||' ;;
    esac | sed 's|^/||' | sort -u
}

placed() {      # $1 file  $2 device
    case $2 in
        pdfwrite|dscwrite) grep -aoE '/Fm[0-9]+ Do' "$1" | awk '{print $1}' ;;
        svgwrite) grep -aoE 'xlink:href="#xq[0-9]+"' "$1" \
                  | sed 's|xlink:href="#xq|/Fm|; s|"$||' ;;
    esac | sed 's|^/||' | sort -u
}

# every placement resolves in the file the placement was made in
holds() {       # $1 file  $2 device  $3 what to call it
    [ -s "$1" ] || { echo "FAIL: $3 produced no file"; fail=1; return; }
    d=$( defined "$1" "$2" )
    p=$( placed  "$1" "$2" )
    [ -n "$p" ] || { echo "FAIL: $3 places no description, so it tests nothing"
                     fail=1; return; }
    for one in $p; do
        printf '%s\n' $d | grep -qx "$one" \
            || { echo "FAIL: $3 places /$one and the page defines no such description"
                 fail=1; }
    done
}

# THE PAGE-PER-FILE SHAPE, which is where the record moves. Each file is
# one page and carries its own descriptions.
for dev in pdfwrite dscwrite svgwrite; do
    case $dev in pdfwrite) x=pdf ;; dscwrite) x=eps ;; *) x=svg ;; esac
    rm -f "$work"/p-[0-9]*."$x"
    run "$dev" "p-%d.$x"
    n=0
    for f in "$work"/p-[0-9]*."$x"; do
        [ -f "$f" ] || continue
        n=$((n + 1))
        holds "$f" "$dev" "$dev page $(basename "$f")"
    done
    [ "$n" = 3 ] || { echo "FAIL: $dev wrote $n files for a three-page job"; fail=1; }
done

# THE ONE-FILE SHAPE. A description belongs to the document there, so the
# check is over the document; what it holds is that a page of one file
# does not name what no page of it defines.
for dev in pdfwrite dscwrite; do
    case $dev in pdfwrite) x=pdf ;; *) x=eps ;; esac
    rm -f "$work/all.$x"
    run "$dev" "all.$x"
    holds "$work/all.$x" "$dev" "$dev one-file document"
done

# NOT held here: that two pages of the same drawing come out as the same
# bytes. They need not. An outline is filed only once it has come back
# often enough to pay for a description, and a short one -- a bar, a
# comma -- can cross that on the second page, so the page before it
# writes the outline where it stands and the page after it places a
# description. Both draw the same letter, and the two spellings are not
# the same bytes: a description is written in the face's own units and
# placed under a matrix, and an outline written where it stands is
# already at the pen. Comparing the pages as bytes would fail on a
# workload that is behaving.

# --- and the fonts, which a page declares for itself the same way ------
#
# A page names a font the same way it names a description, and the same
# thing has to be true of it: what the content names, the page carries.
# The record the declaration is written from is one record for the whole
# page, and a page is made of more streams than one -- its own, a
# pattern's cell, a filed description -- each of which writes its own
# resources from that record. A read that emptied it would hand every
# font to whichever stream was written first and leave the rest naming
# fonts they do not declare; a reader drops that text and says nothing
# the producing run can see.
#
# So the workload is text on a page that also carries a pattern, which
# is the arrangement that has to hold, and it is CompressPages false so
# the content can be read against the resources.
cat > "$work/fonts.ps" <<'EOF'
<< /CompressPages false >> setdistillerparams
/Cell << /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 40 20]
         /XStep 40 /YStep 20
         /PaintProc { pop /Helvetica findfont 6 scalefont setfont
                      2 6 moveto (tile) show } >> def
/page {
    /Pattern setcolorspace
    Cell matrix makepattern setcolor
    0 0 300 200 rectfill
    0 setgray /Helvetica findfont 14 scalefont setfont
    40 240 moveto (page text) show
    showpage
} def
page page
EOF
rm -f "$work"/f-[0-9]*.pdf
( cd "$work" && "$xpost" -q -d pdfwrite -o "f-%d.pdf" fonts.ps </dev/null >/dev/null 2>&1 )     || { echo "FAIL: the font run errored"; fail=1; }
seen=0
for f in "$work"/f-[0-9]*.pdf; do
    [ -f "$f" ] || continue
    seen=$((seen + 1))
    # the page object, and the fonts its own resources define
    page=$( awk '/\/Type \/Page[^s]/,/endobj/' "$f" )
    named=$( grep -aoE '/F[0-9]+ [0-9.]+ Tf' "$f" | awk '{print $1}' | sed 's|^/||' | sort -u )
    [ -n "$named" ] || { echo "FAIL: $(basename "$f") shows no text, so it tests nothing"
                         fail=1; continue; }
    for one in $named; do
        case $page in
            *"/$one <<"*) : ;;
            *) echo "FAIL: $(basename "$f") shows text in /$one and the page"
               echo "      declares no such font, so a reader drops the text"
               fail=1 ;;
        esac
    done
done
[ "$seen" = 2 ] || { echo "FAIL: the font workload wrote $seen files for a two-page job"; fail=1; }

# --- and where a run of text lands in the stream -----------------------
#
# A run of text is gathered rather than written a glyph at a time, so it
# is held open while the next glyph might continue it. What closes it is
# anything else reaching the content: a colour, a path, a bracket, the
# end of a description being captured. A run left open past one of those
# is written after it -- text painted in the colour of whatever was
# drawn next, or written into the page when the cell being captured
# around it should have carried it. Both are pages a reader draws
# without complaint and a run cannot see.
cat > "$work/order.ps" <<'EOF'
<< /CompressPages false >> setdistillerparams
/Cell << /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 40 20]
         /XStep 40 /YStep 20
         /PaintProc { pop /Helvetica findfont 6 scalefont setfont
                      2 6 moveto (incell) show } >> def
0 setgray /Helvetica findfont 14 scalefont setfont
40 700 moveto (before) show
1 0 0 setrgbcolor 40 600 100 40 rectfill
/Pattern setcolorspace Cell matrix makepattern setcolor
40 400 200 100 rectfill
showpage
EOF
( cd "$work" && "$xpost" -q -d pdfwrite -o order.pdf order.ps </dev/null >/dev/null 2>&1 )     || { echo "FAIL: the ordering run errored"; fail=1; }
o=$work/order.pdf
# Whether this build wrote its text as a run at all. A face the writer
# cannot name is drawn instead, a glyph at a time, and there is then no
# run for the three readings below to find -- which is a property of the
# faces this host resolves and not of the ordering they hold. Read off
# the file rather than assumed, so a build that names its faces and puts
# the run in the wrong place still fails: what is asked is whether ANY
# run was written, and the readings ask where THIS one went.
runs=0
[ -s "$o" ] && runs=$( grep -ac ') Tj' "$o" )
if [ -s "$o" ] && [ "${runs:-0}" -eq 0 ]; then
    echo "NOTE the ordering page carries no text run: the face it asked for"
    echo "     is one this build draws rather than names, so where a run"
    echo "     lands in the stream is not a question this host can be"
    echo "     asked. The three readings that need one are not made."
elif [ -s "$o" ]; then
    # the page's own stream: the run set in black must be written before
    # the colour of the box that follows it
    pos_text=$( grep -aob '(before) Tj' "$o" | sed 1q | cut -d: -f1 )
    pos_red=$(  grep -aob '1 0 0 rg'    "$o" | sed 1q | cut -d: -f1 )
    if [ -z "$pos_text" ]; then
        echo "FAIL: the ordering page shows no text, so it tests nothing"; fail=1
    elif [ -z "$pos_red" ]; then
        echo "FAIL: the ordering page states no colour after its text, so the"
        echo "      order this holds is not being reached"; fail=1
    elif [ "$pos_text" -gt "$pos_red" ]; then
        echo "FAIL: a run of text is written after a colour set for what came"
        echo "      after it, so the text is painted in that colour"
        fail=1
    fi
    # the cell's glyphs belong to the cell, and to nothing else
    cell=$( grep -ac 'incell' "$o" )
    [ "$cell" -ge 1 ] || { echo "FAIL: the pattern cell's text is nowhere in the file"
                           fail=1; }
    # ... and they belong to the CELL, which is the stream that follows
    # the pattern's own dictionary. A run still open when the capture
    # ends is written wherever the content next goes, which is the page
    # -- and the page names a font only the cell declares. Read from the
    # cell's side rather than the page's: the page's stream is written
    # before the page object that names it, so scanning forward from the
    # page finds nothing and a check written that way passes on
    # everything.
    incell=$( awk '/\/PatternType/{p=1} p&&/stream$/{s=1;next} s&&/endstream/{exit} s' "$o" \
              | grep -c 'incell' )
    [ "${incell:-0}" -ge 1 ] || {
        echo "FAIL: the pattern cell's own glyphs are not in the cell. A run"
        echo "      still open when the capture ended is written into the"
        echo "      stream the cell was taken out of instead"
        fail=1; }
fi

# --- and a stencil painted in a pattern colour ------------------------
#
# What a device is handed for a stencil is the bits and the matrix; what
# says which colour they are painted in is the graphics state, and a
# pattern is not a colour in the terms that hand-over has for one. So a
# stencil under a pattern is not handed over: the walk that resolves it
# into runs assembles them into one path and the tiling runs once over
# that, which is a pattern fill of the shape the bits describe. Handed
# over instead, it comes out painted in whatever plain colour lies under
# the pattern -- a silhouette where the program asked for the tiling to
# show through the mask, which is what a masked image emulated on Level
# 2 asks for.
cat > "$work/stencil.ps" <<'EOF'
<< /CompressPages false >> setdistillerparams
/Cell << /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 8 8]
         /XStep 8 /YStep 8
         /PaintProc { pop 1 0 0 setrgbcolor 0 0 4 4 rectfill } >> def
/Pattern setcolorspace Cell matrix makepattern setcolor
gsave 100 500 translate 200 200 scale
8 8 true [ 8 0 0 -8 0 8 ] { <FF81BDA5A5BD81FF> } imagemask
grestore
showpage
EOF
( cd "$work" && "$xpost" -q -d pdfwrite -o stencil.pdf stencil.ps </dev/null >/dev/null 2>&1 ) \
    || { echo "FAIL: the stencil run errored"; fail=1; }
sp=$work/stencil.pdf
if [ -s "$sp" ]; then
    grep -aq '/Pattern cs' "$sp" \
        || { echo "FAIL: a stencil painted in a pattern colour reaches the"
             echo "      content without naming the pattern, so it is painted"
             echo "      in whatever plain colour lies under it"
             fail=1; }
    grep -aq '/ImageMask true' "$sp" \
        && { echo "FAIL: a stencil painted in a pattern colour was handed to"
             echo "      the writer as a stencil, which has no way to say the"
             echo "      pattern and paints a silhouette"
             fail=1; }
fi

# THE CONTROL. The workload has to reach the machinery this is about: a
# page that filed nothing would satisfy every check above by having
# nothing to resolve. Each writer's second page places descriptions, and
# a page-per-file run that stopped filing them fails here.
for x in pdf eps svg; do
    case $x in pdf) dev=pdfwrite ;; eps) dev=dscwrite ;; *) dev=svgwrite ;; esac
    c=$( placed "$work/p-2.$x" "$dev" | wc -l )
    [ "$c" -ge 10 ] \
        || { echo "FAIL: the .$x page places $c descriptions, too few to hold anything"
             fail=1; }
done

[ "$fail" = 0 ] || { echo "FAILURES: see above"; exit 1; }
echo "SUCCESS (three writers, both output shapes, every placement resolved)"
