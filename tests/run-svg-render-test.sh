#!/bin/sh
# Meson test wrapper: what the SVG writer emits is a drawing, so render it
# and see whether it draws the same page.
#
# The other two vector writers each have a test that reads their output
# back -- the DSC one runs it as the program it is, the PDF one hands it
# to a consumer -- and this one had none. A drawing can be well formed,
# carry every element the page asked for, and paint nothing: an element
# referred to by an attribute the document's own version does not define
# is skipped in silence, which is a blank page that no check on the text
# of it can see.
#
# What is held is the ink. A renderer's edges are its own -- antialiasing
# and fill rules differ, and the same page rasterised twice by different
# programs differs along every boundary -- so the pixels are not compared
# one for one. The quantity of ink is not a matter of edges: MEASURED,
# this writer's output lands within a quarter of a percent of what the
# raster device draws for the same program, where a drawing whose
# elements are skipped holds none at all and one that paints outside the
# box a form declared holds half as much again.
#
#   $1 xpost   $2 source root
set -u
xp=${1:?usage: run-svg-render-test.sh <xpost> <source root>}
src=${2:?usage: run-svg-render-test.sh <xpost> <source root>}
. "$(dirname "$0")/verdict.sh"

# A renderer is not something a build machine is required to have. Which
# one answers is not this test's business -- what each draws differs at
# the edges and the ink is what is read -- so the first one present wins.
render=
for c in rsvg-convert inkscape google-chrome chromium; do
    command -v "$c" >/dev/null 2>&1 && { render=$c; break; }
done
[ -n "$render" ] || {
    echo "svg-render: no SVG renderer (rsvg-convert, inkscape or chrome) -- skipping"
    exit 77; }
command -v convert >/dev/null 2>&1 || {
    echo "svg-render: no ImageMagick 'convert' -- skipping"; exit 77; }

verdict_workdir
fail=0

# The workloads. Each is a page whose ink the raster device settles, and
# between them they reach what this writer emits that nothing else here
# renders back: a form written once and used again, a form held to the
# box it declares, and the ordinary paints.
cat > "$work/paints.ps" <<'EOF'
0.2 0.4 0.6 setrgbcolor 40 40 200 120 rectfill
0 setgray 4 setlinewidth 40 200 moveto 240 200 lineto 240 320 lineto stroke
0.5 setgray newpath 300 40 moveto 420 40 lineto 360 160 lineto closepath fill
showpage
EOF
cat > "$work/forms.ps" <<'EOF'
/F << /FormType 1 /BBox [0 0 40 40] /Matrix [1 0 0 1 0 0]
      /PaintProc { pop 0 0 40 40 rectfill } >> def
0 1 5 { /r exch def 0 1 5 {  /c exch def
    gsave c 50 mul 40.5 add r 50 mul 40.5 add translate F execform grestore
} for } for
showpage
EOF
cat > "$work/outside.ps" <<'EOF'
/F << /FormType 1 /BBox [0 0 40 40] /Matrix [1 0 0 1 0 0]
      /PaintProc { pop 0 0 40 40 rectfill 60 60 30 30 rectfill } >> def
gsave 100.5 400.5 translate F execform grestore
gsave 250.5 400.5 translate F execform grestore
showpage
EOF

ink() {  # $1 an image the renderer or the device made
    convert "$1" -threshold 50% -format '%[fx:int(w*h*(1-mean))]' info: 2>/dev/null
}

for w in paints forms outside; do
    ps=$work/$w.ps
    # Both runs are judged before anything reads what they left. A run
    # that fell over and a run that drew nothing are different answers,
    # and reading the artifact first reports the second when it was the
    # first.
    _out=$(XPOST_NO_VM_IMAGE=1 "$xp" -q -d pgm -o "$work/$w.pgm" "$ps" \
        </dev/null 2>&1); _st=$?
    verdict_run "$_st" "$_out" "the raster device on $w" || { fail=1; continue; }
    _out=$(XPOST_NO_VM_IMAGE=1 "$xp" -q -d svgwrite -o "$work/$w.svg" "$ps" \
        </dev/null 2>&1); _st=$?
    verdict_run "$_st" "$_out" "the SVG writer on $w" || { fail=1; continue; }

    W=$(sed -n '2p' "$work/$w.pgm" | awk '{print $1}')
    H=$(sed -n '2p' "$work/$w.pgm" | awk '{print $2}')
    case $render in
    rsvg-convert) rsvg-convert -w "$W" -h "$H" -o "$work/$w.png" "$work/$w.svg" \
                      >/dev/null 2>&1 ;;
    inkscape)     inkscape --export-type=png --export-filename="$work/$w.png" \
                      -w "$W" -h "$H" "$work/$w.svg" >/dev/null 2>&1 ;;
    *)            "$render" --headless --disable-gpu --no-sandbox --hide-scrollbars \
                      --screenshot="$work/$w.png" --window-size="$W,$H" \
                      "file://$work/$w.svg" >/dev/null 2>&1 ;;
    esac
    [ -s "$work/$w.png" ] || {
        echo "FAIL: $render produced no image for $w"; fail=1; continue; }
    convert "$work/$w.png" -background white -alpha remove -alpha off \
        -colorspace Gray "$work/$w.rendered.pgm" 2>/dev/null

    want=$(ink "$work/$w.pgm")
    got=$(ink "$work/$w.rendered.pgm")
    case "${want:-0}" in ''|*[!0-9]*) want=0 ;; esac
    case "${got:-0}"  in ''|*[!0-9]*) got=0  ;; esac
    if [ "$want" -le 0 ]; then
        echo "FAIL: $w puts no ink on the page the raster device drew, so there"
        echo "      is nothing here to hold the drawing to"
        fail=1
        continue
    fi
    # two percent, against a quarter of a percent measured and the two
    # failures this exists for at fifty-seven and a hundred
    lo=$(( want * 98 / 100 ))
    hi=$(( want * 102 / 100 ))
    if [ "$got" -lt "$lo" ] || [ "$got" -gt "$hi" ]; then
        echo "FAIL: the drawing for $w holds $got ink where the page the raster"
        echo "      device drew holds $want. A drawing that holds none is one"
        echo "      whose elements were skipped; one that holds more is painting"
        echo "      what the page does not."
        fail=1
    fi
done

verdict_exit "3 page(s) rendered by $render and held to the ink the raster device drew"
