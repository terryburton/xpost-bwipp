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
