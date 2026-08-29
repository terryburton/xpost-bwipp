#!/bin/sh
# Meson test wrapper: the raster devices' pixel formats.
#
# A raster format is an arrangement of components, so what a format test
# has to say is which component landed where. Two families carry one:
#
#  * The devices that keep their pixels in a buffer of their own and hand
#    it to whoever embedded the interpreter. The buffer's arrangement is
#    the name after the colon in the device string -- raster:argb and the
#    rest -- and falls back to rgb when no name is given. Each of the
#    four is painted and read back here, and so is the selection that
#    names none. The four are the whole of what such a selection may
#    carry: a word outside them is refused before the run begins, which
#    tests/run-band-select-test.sh holds along with every other device's
#    modes.
#
#  * The devices that write their raster to a file. Their format is the
#    file's: how many bytes a pixel takes and in which order its
#    components stand. This is asked of the bytes that come out, against
#    the colour that was painted, so a page whose every pixel is wrong in
#    the same way is a failure here -- which a comparison of one output
#    against another cannot be.
#
# What this does NOT cover:
#
#  * The byte order inside the buffer the raster: names are named after.
#    That order is the embedder's view of the buffer; from a program
#    GetPix answers red, green and blue whatever the format holds them
#    as, so a format whose store and load agree on a wrong arrangement
#    reads correct here. Nothing in this suite reads that buffer as an
#    embedding program does.
#
#  * The compiled image writers -- png, pngalpha and jpeg -- whose bytes
#    depend on the version of the library that encodes them. Their pixel
#    semantics are tests/run-pngalpha-test.sh's.
#
#  * Whether the marks are in the right places. That is the golden-render
#    manifest's, over a page of every construct; what is asked here is
#    only that the components of a painted colour reach the page in the
#    order the format declares.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
# an absolute path may begin with a drive letter as well as a slash;
# prepending the working directory to one of those makes every
# invocation a path that does not exist
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
if faceless_build "$xpost"; then
    echo "SKIPPED: this run shows text through a face, and this build carries no face library"
    exit 77
fi

verdict_workdir

# A path written into a program the interpreter runs is read by the
# interpreter, so it has to be spelt the way the platform spells it. A
# shell that keeps a file-system of its own rewrites a path it hands over
# as an argument, and cannot rewrite one inside a file it wrote: the
# directory this shell calls /tmp/x is nowhere at all to a program built
# against the platform, and a program naming it opens nothing. So the
# programs below name the working directory the platform's way, through
# the shell's own translator where there is one.
cygpath=$(command -v cygpath 2>/dev/null) || cygpath=
if [ -n "$cygpath" ]; then
    hwork=$("$cygpath" -m "$work")
else
    hwork=$work
fi

fail=0

# The colour every check below is measured against. Its three components
# differ from one another, so no rearrangement of them is invisible: a
# pure primary would read the same under two of the six orders.
R=255
G=102
B=0

# ---------------------------------------------------------------- buffers

cat > "$work/paint.ps" <<'PSEOF'
1 0.4 0 setrgbcolor
newpath 10 10 moveto 60 10 lineto 60 40 lineto closepath fill
0 0 1 setrgbcolor
newpath 20 20 moveto 30 0 rlineto 0 30 rlineto closepath fill
% Text as well as fills: a glyph's edge pixels are blended into the
% device's own buffer, which is a different method from the one that
% fills reach and one these devices have to carry themselves.
0 setgray /Helvetica findfont 14 scalefont setfont
10 60 moveto (Ag) show

% These devices keep their pixels in a buffer of their own and write no
% file, so nothing outside the interpreter can see what the marks above
% did. The buffer is therefore read here, before the page goes: a
% painted pixel and one nothing reached. An interpreter that made no
% device, or one whose buffer was sized and never written, prints
% nothing and exits cleanly, and that is what these lines are against.
/device? { DEVICE } def
/dev device? def
/report {       % x y (label)  .  -
    /L exch def
    % the marks above are placed in user space and the buffer is indexed
    % in the device's, so the point is carried across by the same matrix
    % that carried the marks
    transform round cvi exch round cvi exch
    count /d0 exch def
    dev dup /GetPix get exec
    count d0 sub 2 add /pn exch def
    (READBACK ) print L print ( ) print pn 4 string cvs print ( ) print
    pn 3 eq {
        /pb exch def /pg exch def /pr exch def
        pr 4 string cvs print ( ) print
        pg 4 string cvs print ( ) print
        pb 4 string cvs print
    }{
        pn { pop } repeat
    } ifelse
    (\n) print
} bind def
50 15 (painted) report
5 55 (clear) report
showpage
quit
PSEOF

# What a component reads as where the page was painted, and where it was
# not: the erased page is white, and the fill is the colour asked for.
paint() {   # $1 the device string
    p_out=$("$xpost" -q --no-sandbox -d "$1" -o /dev/null "$work/paint.ps" \
            </dev/null 2>&1)
    verdict_run "$?" "$p_out" "$1" || fail=1
    printf '%s\n' "$p_out" > "$work/paint.out"
    for line in "painted 3 $R $G $B" "clear 3 255 255 255"; do
        if ! grep -q "^READBACK $line *\$" "$work/paint.out"; then
            echo "FAIL $1: the buffer reads"
            grep '^READBACK' "$work/paint.out" | sed 's/^/      /'
            echo "      where READBACK $line was painted"
            fail=1
            return
        fi
    done
}

for sub in rgb argb bgr bgra; do
    paint "raster:$sub"
done

# and the device with no format named at all, which takes the first of
# the four
paint raster

# the other device that keeps its pixels in a buffer of its own
paint bgr

# ------------------------------------------------------------------ files

# A page of one colour, small enough that every pixel of it is read.
W=8
H=8
cat > "$work/flat.ps" <<PSEOF
<< /PageSize [$W $H] >> setpagedevice
1 0.4 0 setrgbcolor
0 0 $W $H rectfill
showpage
quit
PSEOF
for g in 0 1; do
    cat > "$work/grey$g.ps" <<PSEOF
<< /PageSize [$W $H] >> setpagedevice
$g setgray
0 0 $W $H rectfill
showpage
quit
PSEOF
done

emit() {    # $1 device  $2 program  $3 output
    e_out=$("$xpost" -q --no-sandbox -d "$1" -o "$3" "$work/$2" \
            </dev/null 2>&1)
    verdict_run "$?" "$e_out" "$1" || { fail=1; return 1; }
    [ -s "$3" ] && return 0
    echo "FAIL $1: wrote no page"
    fail=1
    return 1
}

# The header a format opens with, byte for byte. It carries the format's
# own magic and the page's extent, so a device that wrote the pixels of
# one format under the banner of another is caught before the pixels are
# read at all.
header_is() {   # $1 device  $2 file  $3 the header
    printf '%s' "$3" > "$work/want.hdr"
    hn=$(wc -c < "$work/want.hdr" | tr -d ' ')
    head -c "$hn" "$2" > "$work/got.hdr"
    cmp -s "$work/want.hdr" "$work/got.hdr" && return 0
    echo "FAIL $1: the header is not the $hn bytes the format opens with"
    od -An -c -v "$work/got.hdr" | sed 's/^/      got /'
    od -An -c -v "$work/want.hdr" | sed 's/^/      want /'
    fail=1
    return 1
}

# The pixel data, as decimal bytes one to a line, from $3 bytes in.
data_of() {     # $1 file  $2 output  $3 header bytes
    tail -c "+$(($3 + 1))" "$1" | od -An -v -tu1 | tr -s ' ' '\n' \
        | grep '[0-9]' > "$2"
}

# Every pixel of a one-colour page must read as that colour, in the
# order the format puts its components in, and the data must be exactly
# as long as that many pixels of that many bytes.
data_is() {     # $1 device  $2 data file  $3 pixels  $4... the components
    d_dev=$1
    d_file=$2
    d_px=$3
    shift 3
    printf '%s\n' "$@" > "$work/want.px"
    # The reading is answered rather than left to be inferred from
    # silence: an awk that died before it compared anything says nothing
    # either, and nothing is what a page in good order says too.
    d_msg=$(awk -v px="$d_px" -v want="$work/want.px" '
        BEGIN { nc = 0; while ((getline w < want) > 0) c[nc++] = w }
        { got[n++] = $1 }
        END {
            if (n != px * nc) {
                printf "the data is %d bytes where %d pixels of %d components are %d\n",
                       n, px, nc, px * nc
                exit
            }
            for (i = 0; i < px; i++)
                for (j = 0; j < nc; j++)
                    if (got[i * nc + j] + 0 != c[j] + 0) {
                        printf "pixel %d reads", i
                        for (k = 0; k < nc; k++) printf " %d", got[i * nc + k]
                        printf " where"
                        for (k = 0; k < nc; k++) printf " %d", c[k]
                        printf " was painted\n"
                        exit
                    }
            print "every pixel carries the colour"
        }' "$d_file")
    [ "$d_msg" = "every pixel carries the colour" ] && return 0
    echo "FAIL $d_dev: ${d_msg:-the reading said nothing}"
    fail=1
    return 1
}

px=$((W * H))

# The portable pixmap: a header, then three bytes a pixel, red green blue
# in that order (this is the emitter the whole colour raster reaches).
if emit ppm flat.ps "$work/out.ppm"; then
    if header_is ppm "$work/out.ppm" "P6
$W $H
255
"; then
        data_of "$work/out.ppm" "$work/ppm.px" 11
        data_is ppm "$work/ppm.px" "$px" "$R" "$G" "$B" \
            && echo "OK   ppm (three bytes a pixel, red green blue)"
    fi
fi

# The portable greymap: the same header for one byte a pixel. Black and
# white say which end of the byte is which; the colour page says the
# whole of a colour was reduced to that one byte rather than one
# component of it being kept.
g_fail=0
if emit pgm grey0.ps "$work/black.pgm"; then
    header_is pgm "$work/black.pgm" "P5
$W $H
255
" || g_fail=1
    data_of "$work/black.pgm" "$work/black.px" 11
    data_is pgm "$work/black.px" "$px" 0 || g_fail=1
else
    g_fail=1
fi
if emit pgm grey1.ps "$work/white.pgm"; then
    data_of "$work/white.pgm" "$work/white.px" 11
    data_is pgm "$work/white.px" "$px" 255 || g_fail=1
else
    g_fail=1
fi
if emit pgm flat.ps "$work/out.pgm"; then
    data_of "$work/out.pgm" "$work/pgm.px" 11
    # the one level a colour comes to is between the ends and the same
    # everywhere; which level it is, is the colour conversion's business
    # and not the format's
    lv=$(sort -u "$work/pgm.px" | tr '\n' ' ' | sed 's/ *$//')
    n=$(wc -l < "$work/pgm.px" | tr -d ' ')
    case $lv in
        0|255|*\ *)
            echo "FAIL pgm: a flat colour page reads as levels [$lv]"
            fail=1; g_fail=1 ;;
    esac
    [ "$n" -eq "$px" ] || {
        echo "FAIL pgm: $n bytes for $px pixels of one byte each"
        fail=1; g_fail=1; }
else
    g_fail=1
fi
[ "$g_fail" -eq 0 ] \
    && echo "OK   pgm (one byte a pixel, 0 black through 255 white)"

# The portable bitmap: one bit a pixel, rows padded out to a byte, and a
# set bit is black. A grey between the ends is dithered through the
# halftone screen, so the ends are what can be stated.
rb=$(( (W + 7) / 8 ))
b_fail=0
if emit pbm grey0.ps "$work/black.pbm"; then
    header_is pbm "$work/black.pbm" "P4
$W $H
" || b_fail=1
    data_of "$work/black.pbm" "$work/blackb.px" 7
    data_is pbm "$work/blackb.px" "$((rb * H))" 255 || b_fail=1
else
    b_fail=1
fi
if emit pbm grey1.ps "$work/white.pbm"; then
    data_of "$work/white.pbm" "$work/whiteb.px" 7
    data_is pbm "$work/whiteb.px" "$((rb * H))" 0 || b_fail=1
else
    b_fail=1
fi
[ "$b_fail" -eq 0 ] && echo "OK   pbm (one bit a pixel, a set bit black)"

# The baseline TIFF: a little-endian header, then one strip of the same
# three bytes a pixel the pixmap writes, compressed with the coder the
# directory names. The strip begins where the header ends, so it is read
# from there and decoded through the interpreter's own filter into plain
# bytes the reading above can be put to.
if emit tiff flat.ps "$work/out.tiff"; then
    if header_is tiff "$work/out.tiff" "II*"; then
        cat > "$work/untiff.ps" <<PSEOF
/in ($hwork/out.tiff) (r) file def
in 8 setfileposition
/d in /LZWDecode filter def
/s $((px * 3)) string def
d s readstring pop
/out ($hwork/tiff.raw) (w) file def
out exch writestring
out closefile
quit
PSEOF
        u_out=$("$xpost" -q --no-sandbox -d null "$work/untiff.ps" \
                </dev/null 2>&1)
        if verdict_run "$?" "$u_out" "decoding the tiff strip"; then
            data_of "$work/tiff.raw" "$work/tiff.px" 0
            data_is tiff "$work/tiff.px" "$px" "$R" "$G" "$B" \
                && echo "OK   tiff (one strip of three bytes a pixel, red green blue)"
        else
            fail=1
        fi
    fi
fi

[ "$fail" = 0 ] || { echo "FAILURES: the formats above"; exit 1; }
echo "SUCCESS"
