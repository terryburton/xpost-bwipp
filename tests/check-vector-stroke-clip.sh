#!/bin/sh
# A stroke under a clip must reach a vector device as the stroke it is.
#
# The line a stroke is drawn along is not a polygon, so it cannot be cut to
# the clip the way a fill's outline is: the stroke of a cut line is not the
# cut stroke, and where the line encloses the clip window the cut leaves the
# window itself -- which then gets drawn, putting the clip's own boundary on
# the page as a line the program never asked for.
#
# The device is handed the clip instead, and draws the whole line under it.
#
# Judged on what the output says, not on comparing two rasterisers: a stroke
# resolved to pixels and a stroke drawn by a consumer differ by the width of
# an anti-aliased edge even with no clip in force, so pixel equality between
# the two devices is not a thing this can be held to.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-vector-stroke-clip.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0

run() {  # $1 label  $2 device  $3 ext  $4 body
    # CompressPages false leaves the content stream readable, so what the
    # consumer will act on can be read straight out of the file.
    cat > "$work/$1.ps" <<EOF
<< /CompressPages false >> setdistillerparams
$4
showpage
EOF
    "$xpost" -q -d "$2" -o "$work/$1.$3" "$work/$1.ps" </dev/null >/dev/null 2>&1 \
        || { echo "FAIL: $1 ($2) errored"; fail=1; return 1; }
    return 0
}

# 1. A stroke wholly outside the clip marks nothing. Judged on the page a
#    consumer draws, which is absolute -- no tolerance to argue about, and
#    it is the case that fails by drawing the clip's own outline. The path
#    may still be written: written under a clip that excludes it, it marks
#    nothing, which is the point.
run outside pdfwrite pdf 'newpath 50 50 100 100 rectclip
0 setgray newpath 0 0 300 300 rectstroke' && {
    if command -v pdftoppm >/dev/null 2>&1 && command -v convert >/dev/null 2>&1; then
        pdftoppm -f 1 -l 1 -r 72 -png "$work/outside.pdf" "$work/outside_v" >/dev/null 2>&1
        # minima of the thresholded page is 1 exactly when no pixel is dark.
        # Counting them instead would be read through w*h*(1-mean), which
        # carries a few pixels of floating-point noise at this page size.
        m=$(convert "$work/outside_v-1.png" -colorspace gray -threshold 50% \
              -format '%[fx:minima]' info: 2>/dev/null)
        case $m in
            1|1.0|1.00000) : ;;
            '') echo "FAIL: outside produced nothing to measure"; fail=1 ;;
            *)  echo "FAIL: a stroke wholly outside the clip marked the page"; fail=1 ;;
        esac
    fi
}

# 2. The clip the device is given is the program's clip, and the line drawn
#    is the program's line. Read from the content stream, so what is checked
#    is what a consumer will act on.
run crossing pdfwrite pdf 'newpath 50 50 100 100 rectclip
0 setgray 4 setlinewidth newpath 0 100 moveto 300 100 lineto stroke' && {
    grep -aq 'W n' "$work/crossing.pdf" \
        || { echo "FAIL: no clip operator written for a clipped stroke"; fail=1; }
    grep -aq '^S$' "$work/crossing.pdf" \
        || { echo "FAIL: no stroke operator written"; fail=1; }
}

# 3. A clip that is not a rectangle is the shape a device must be given
#    rather than have reduced for it.
run round pdfwrite pdf 'newpath 100 100 60 0 360 arc clip
0 setgray 3 setlinewidth newpath 0 0 moveto 300 300 lineto stroke' && {
    grep -aq 'W n' "$work/round.pdf" \
        || { echo "FAIL: no clip operator written for a non-rectangular clip"; fail=1; }
}

# 4. THE CONTROL. With no clip in force nothing may be written -- without
#    this, an emitter that wrote a clip unconditionally would pass every
#    check above while meaning nothing.
run noclip pdfwrite pdf '0 setgray 3 setlinewidth
newpath 20 20 moveto 200 200 lineto stroke' && {
    grep -aq 'W n' "$work/noclip.pdf" \
        && { echo "FAIL: a clip was written where the program set none"; fail=1; }
}

# 5. A stroke inside the clip is drawn whole: the clip cuts nothing off it,
#    so the line the consumer gets still reaches its own endpoints.
run inside svgwrite svg 'newpath 0 0 300 300 rectclip
0 setgray 3 setlinewidth newpath 20 20 moveto 200 200 lineto stroke' && {
    grep -q '<path' "$work/inside.svg" \
        || { echo "FAIL: a stroke wholly inside the clip drew nothing"; fail=1; }
}

# 6. Fills under the same clip have always been right and must stay so.
run fillctl svgwrite svg 'newpath 50 50 100 100 rectclip
0 setgray newpath 0 0 300 300 rectfill' && {
    grep -q '<path' "$work/fillctl.svg" \
        || { echo "FAIL: a clipped fill drew nothing"; fail=1; }
}

test $fail -eq 0 && echo "OK: strokes reach a vector device under their clip"
exit $fail
