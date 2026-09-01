#!/bin/sh
#
# The rule a threshold array is read by, at the values where it decides.
#
# PLRM 7.4.5 states it in two sentences and a note, and each of the three
# is separately checkable:
#
#   "If the requested gray level is less than the threshold value, paint
#     the device pixel black; otherwise, paint it white" -- so the
#     comparison is strict, and a grey that lands exactly on its
#     threshold leaves the pixel white
#   "Gray levels in the range 0.0 to 1.0 correspond to threshold values
#     from 0 to the maximum available" -- so a grey either side of a
#     threshold falls the other way
#   "A threshold value of 0 is treated as if it were 1; therefore, a gray
#     level of 0.0 paints all pixels black, regardless of the values in
#     the threshold array" -- so black is black through any array, and a
#     zero in one is not a place the darkest grey there is leaves white
#
# The third is the one that can be absent while the screen looks right:
# an array with no zero in it never asks the question, and an array with
# one asks it only at black. A page of ordinary tints through such an
# array differs by a scatter of pixels, which reads as two screens
# disagreeing dot for dot -- which is a thing that happens and is not
# this.
#
# The numbers below are not this interpreter's output recorded as a
# baseline; a baseline taken that way locks in whatever it did, a fault
# included, and can no longer report one. Each is what the sentence
# above it requires, worked out from the cell and the grey the case
# paints. Every case fills the same sixteen by sixteen page, so a count
# is comparable with any other count here, and the four-by-four cell
# tiles it sixteen times over: a cell of one threshold puts that
# threshold under all 256 pixels, and a cell half zeroes and half full
# puts each under 128.
#
# The greys are named as a byte over 255 because that is the scale the
# thresholds are on, but the grey does not travel to the comparison on
# that scale: it arrives on one of 0 to 256, and the two part company by
# one step in the upper part of the range. So the boundary cases here
# are taken from the lower part, where a grey of k over 255 is compared
# as k and the sentence can be read literally against the page; a case
# at the top would be asking about the scale rather than about the rule,
# and the scale is not what this is for.
#
#   $1  path to the source tree root
#   $2  path to the xpost binary
set -u
src=${1:?usage: check-halftone-threshold.sh <srcroot> <xpost>}
xpost=${2:?usage: check-halftone-threshold.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$src/data/halftone.ps" "the halftone machinery"

guard_workdir

# a case is a name, the count expected, the threshold array, and the grey
run_case() {
    _name=$1; _want=$2; _thr=$3; _grey=$4
    {
        printf '<< /PageSize [16 16] >> setpagedevice\n'
        printf '<< /HalftoneType 3 /Width 4 /Height 4 /Thresholds <%s> >> sethalftone\n' "$_thr"
        printf '%s setgray\n0 0 16 16 rectfill\nshowpage\n' "$_grey"
    } > "$work/case.ps"
    XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d pbm \
        -o "$work/case.out" "$work/case.ps" </dev/null >/dev/null 2>&1
    if [ ! -s "$work/case.out" ]; then
        echo "FAIL: $_name produced no page at all, so nothing about the"
        echo "      threshold rule can be read from it"
        fail=1
        return
    fi
    _got=$(guard_pnm_pixels "$work/case.out" | awk '
        { for (k = 0; k < 8; k++) if (int($1 / 2^k) % 2) ink++ }
        END { print ink+0 }')
    if [ "$_got" != "$_want" ]; then
        echo "FAIL: $_name inked $_got of 256 places and the rule it stands"
        echo "      on requires $_want."
        fail=1
    fi
}

fail=0

# every threshold 64, and greys on it and either side of it
#
# The first two are the control for every other case here: if a grey a
# clear step below its threshold stops inking the page and one a clear
# step above stops leaving it, the two scales have parted company and
# the boundary case below says nothing about the boundary.
flat=40404040404040404040404040404040
run_case "a grey below its threshold"        256 "$flat" "63 255 div"
run_case "a grey above its threshold"          0 "$flat" "65 255 div"
run_case "a grey exactly on its threshold"     0 "$flat" "64 255 div"

# the ends, where the darkest threshold an array can carry meets the
# greys that must come out white and black whatever it says
full=FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
run_case "white through the darkest threshold"   0 "$full" "1"
run_case "black through the darkest threshold" 256 "$full" "0"

# and one more boundary, half way up, so that a comparison shifted by a
# step is caught somewhere other than at 64
mid=80808080808080808080808080808080
run_case "a grey one step below halfway"       256 "$mid" "127 255 div"
run_case "a grey exactly halfway"                0 "$mid" "128 255 div"

# and a threshold of zero, which is the one that is not what it says
#
# Black must be black through it. Read literally, a zero threshold is
# reached by every grey including black, and the page would come back
# white where the array zeroed; the note is what stops that, and this
# is the case that asks whether it was applied.
zero=00000000000000000000000000000000
run_case "black through an array of zeroes"    256 "$zero" "0"

# It is raised to one and no further. A grey of one on the same scale
# lands exactly on the raised threshold, so the strict comparison leaves
# it white -- which is what says the zero became a one rather than
# something the arithmetic merely rounds past.
run_case "the darkest grey above a raised zero"  0 "$zero" "1 255 div"

# and the raise reaches the zeroes of an array that is not all zeroes,
# without reaching anything else. Half this cell is zero and half is
# 255, so black inks all of it and a middle grey inks the half whose
# threshold it is still below.
half=0000000000000000FFFFFFFFFFFFFFFF
run_case "black through an array half zeroes"  256 "$half" "0"
run_case "a middle grey through the same"      128 "$half" "0.5"

# The array is the program's object and the screen is built from a cell
# of its own, so what the program handed over reads back as it handed it
# over: a raise that edited the array in place would answer 1 here.
cat > "$work/keep.ps" <<PS
<< /HalftoneType 3 /Width 4 /Height 4 /Thresholds <$zero> >> sethalftone
0 setgray 0 0 16 16 rectfill
currenthalftone /Thresholds get 0 get ==
PS
kept=$(XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d pbm \
       -o "$work/keep.out" "$work/keep.ps" </dev/null 2>&1 | tr -d '\r')
if [ "$kept" != 0 ]; then
    echo "FAIL: after painting through an array of zeroes the array reads"
    echo "      back as '$kept' where the program left 0 in it"
    fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "SUCCESS (12 cases: the comparison is strict, the ends are solid, and a zero threshold counts as one)"
