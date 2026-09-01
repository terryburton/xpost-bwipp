#!/bin/sh
#
# What a function dictionary says about its own outputs, held to the
# outputs it produces.
#
# PLRM 3.10 gives a function three separate sayings about its numbers,
# and each of them is a way a function can look implemented and be
# ignored, because ignoring one still paints a plausible page:
#
#   Encode places the input in the sample table -- a table is addressed
#     over the interval Encode names, and only by default over the whole
#     of itself
#   Decode reads a sample out -- a sample spans the interval Decode
#     names, and only by default the function's Range
#   Range clips the result, whatever the type computed it -- an output
#     outside its interval becomes the nearer boundary, and a function
#     naming no Range is not clipped at all
#
# A function that honours none of the three still runs, still paints a
# gradient, and still puts ink in roughly the right places. So each is
# asked for here with a case whose answer cannot be produced any other
# way: a mapping that collapses the whole domain onto one value, which a
# reader that ignored the entry would paint as a gradient rather than as
# a flat page, and a mapping that reverses it, which is the control row
# read backwards and nothing else.
#
# The expectations are not this interpreter's output recorded as a
# baseline. Every one is what the formulas above come to for the table
# the case carries -- black where the mapping selects a sample of zero,
# white where it selects one of all ones, the control row reversed where
# the mapping reverses, and for the two greys that are neither, the byte
# this device paints for that value, asked of the device rather than
# written down.
#
# The gradient control is what makes the flat pages mean anything. A
# reader that answered one colour to everything would satisfy every flat
# case here and none of the rising ones.
#
#   $1  path to the source tree root
#   $2  path to the xpost binary
set -u
src=${1:?usage: check-function-mapping.sh <srcroot> <xpost>}
xpost=${2:?usage: check-function-mapping.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_require_file "$src/data/shade.ps" "the function machinery"

guard_workdir

fail=0

# The page is sixteen device pixels across and four down, and every case
# paints across it with the axis of the shading laid along it, so one row
# of the raster is the function read left to right.
run() {             # <shading dictionary> -> "<16 values>" or "E <errorname>"
    {
        printf '<< /PageSize [16 4] >> setpagedevice\n'
        printf '{ %s shfill } stopped\n' "$1"
        printf '{ (E ) print $error /errorname get 32 string cvs print (\\n) print clear }\n'
        printf '{ } ifelse\nshowpage\n'
    } > "$work/case.ps"
    rm -f "$work/case.pgm"
    _e=$( XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d pgm \
          -o "$work/case.pgm" "$work/case.ps" </dev/null 2>&1 \
          | awk '$1 == "E" { print $2; exit }' )
    if [ -n "${_e:-}" ]; then printf 'E %s' "$_e"; return; fi
    [ -f "$work/case.pgm" ] || { printf 'E nopage'; return; }
    guard_pnm_pixels "$work/case.pgm" | head -16 | tr '\n' ' ' | sed 's/ $//'
}

axial() {           # <function dictionary> -> the row it paints
    run "<< /ShadingType 2 /ColorSpace /DeviceGray /Coords [0 0 16 0]
            /Function $1 >>"
}

# The function-based shading is the one that asks a function for two
# inputs, so it is the only way to reach the two-input reader from a
# program. Its domain is taken through the Matrix onto the page.
cellular() {        # <function dictionary> -> the row it paints
    run "<< /ShadingType 1 /ColorSpace /DeviceGray /Domain [0 1 0 1]
            /Matrix [16 0 0 4 0 0] /Function $1 >>"
}

# What this device paints for a grey, asked of the device. Two cases
# below come to a value that is neither black nor white, and what a
# component becomes in a raster byte is the device's business rather
# than this guard's.
grey() {            # <PostScript number> -> the byte it paints
    {
        printf '<< /PageSize [16 4] >> setpagedevice\n'
        printf '%s setgray 0 0 16 4 rectfill showpage\n' "$1"
    } > "$work/grey.ps"
    rm -f "$work/grey.pgm"
    XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d pgm \
        -o "$work/grey.pgm" "$work/grey.ps" </dev/null >/dev/null 2>&1
    guard_pnm_pixels "$work/grey.pgm" | head -1
}

# A case that was refused rather than painted has no row to judge, and
# saying so is not the same as saying the row was wrong.
refused() {         # <name> <row> -> true when the case raised an error
    case $2 in
        E\ *) echo "FAIL: $1 was refused: ${2#E }. The mapping it declares is"
              echo "      one the specification allows, so a refusal is not an"
              echo "      answer to it."
              fail=1
              return 0 ;;
    esac
    return 1
}

flat() {            # <name> <byte> <row>
    _n=$1; _v=$2; shift 2
    refused "$_n" "$*" && return
    for _p in "$@"; do
        [ "$_p" = "$_v" ] && continue
        echo "FAIL: $_n painted"
        echo "        $*"
        echo "      where the mapping it declares sends the whole domain to"
        echo "      one value, so every pixel is $_v. A page that varies is"
        echo "      one painted without the entry."
        fail=1
        return
    done
}

rising() {          # <name> <row>
    _n=$1; shift
    refused "$_n" "$*" && return
    _prev=-1
    for _p in "$@"; do
        [ "$_p" -gt "$_prev" ] || {
            echo "FAIL: $_n painted"
            echo "        $*"
            echo "      which does not rise across the page. Without a case"
            echo "      that varies, the flat cases here are satisfied by a"
            echo "      reader that answers one colour to everything."
            fail=1
            return
        }
        _prev=$_p
    done
}

same() {            # <name> <expected row> <got row>
    [ "$2" = "$3" ] && return
    echo "FAIL: $1 painted"
    echo "        $3"
    echo "      where the mapping it declares comes to"
    echo "        $2"
    fail=1
}

reverse() {         # <row> -> the row backwards
    printf '%s\n' "$1" | tr ' ' '\n' | sed '1!G;h;$!d' | tr '\n' ' ' | sed 's/ $//'
}

# ---- a sample table, addressed by Encode and read out by Decode
#
# Two samples at the ends of what a byte can hold, so a table position
# is a grey and the two mappings that collapse the domain land on black
# and on white exactly.
tbl='/FunctionType 0 /Domain [0 1] /Range [0 1] /Size [2] /BitsPerSample 8 /DataSource <00FF>'

control=$(axial "<< $tbl >>")
rising "a sample table with neither Encode nor Decode" $control

flat "a table addressed at its first sample only (Encode [0 0])" 0 \
     $(axial "<< $tbl /Encode [0 0] >>")
flat "a table addressed at its last sample only (Encode [1 1])" 255 \
     $(axial "<< $tbl /Encode [1 1] >>")
same "a table addressed backwards (Encode [1 0])" \
     "$(reverse "$control")" "$(axial "<< $tbl /Encode [1 0] >>")"

flat "samples read out over [0 0] (Decode [0 0])" 0 \
     $(axial "<< $tbl /Decode [0 0] >>")
flat "samples read out over [1 1] (Decode [1 1])" 255 \
     $(axial "<< $tbl /Decode [1 1] >>")
same "samples read out backwards (Decode [1 0])" \
     "$(reverse "$control")" "$(axial "<< $tbl /Decode [1 0] >>")"

# PLRM 3.10.1 allows a sample table of one entry in a dimension, every
# input in that dimension mapping to the single value it holds. There is
# no second sample to interpolate towards, and a reader that reached for
# one would run off the end of a table that is exactly as long as it
# says it is.
flat "a sample table of one entry" "$(grey '128 255 div')" \
     $(axial "<< /FunctionType 0 /Domain [0 1] /Range [0 1] /Size [1]
                 /BitsPerSample 8 /DataSource <80> >>")

# ---- Range, which clips whatever the type computed
#
# An exponential interpolation running from black to white, which is a
# gradient of its own making rather than a table read: the clip is a
# property of the dictionary and not of the branch that computed it.
exp='/FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1'

rising "an exponential interpolation declaring no Range" \
       $(axial "<< $exp >>")
flat "an exponential interpolation with Range [0 0]" 0 \
     $(axial "<< $exp /Range [0 0] >>")
flat "an exponential interpolation with Range [1 1]" 255 \
     $(axial "<< $exp /Range [1 1] >>")

# A stitching function's own Range, whose subfunctions have already
# clipped themselves to theirs: the outer dictionary is a function like
# any other and says what its outputs are.
flat "a stitching function with Range [1 1]" 255 \
     $(axial "<< /FunctionType 3 /Domain [0 1] /Range [1 1] /Bounds [0.5]
                 /Encode [0 1 0 1]
                 /Functions [ << $exp >> << $exp >> ] >>")

# Decode puts every sample outside the Range, so the two entries are in
# the order the specification gives them: read out first, clipped after.
# A reader that clipped the sample before decoding it, or that took
# Decode for Range, paints white here.
flat "samples decoded outside the Range they are then clipped to" \
     "$(grey 0.5)" \
     $(axial "<< /FunctionType 0 /Domain [0 1] /Size [2] /BitsPerSample 8
                 /DataSource <00FF> /Decode [1 1] /Range [0 0.5] >>")

# ---- the same three sayings on the two-input reader
#
# A four-entry table over the unit square, whose corners differ, so the
# control varies across the page in its own right.
tbl2='/FunctionType 0 /Domain [0 1 0 1] /Range [0 1] /Size [2 2]
      /BitsPerSample 8 /DataSource <0040 80FF>'

varied=$(cellular "<< $tbl2 >>")
case $varied in
    E\ *) echo "FAIL: a function-based shading with a plain table answered $varied"
          fail=1 ;;
    *)  _first=${varied%% *}
        _rest=${varied#* }
        _flat=yes
        for _p in $_rest; do [ "$_p" = "$_first" ] || _flat=no; done
        [ "$_flat" = no ] || {
            echo "FAIL: a function-based shading with a plain table painted one"
            echo "      colour across a table whose corners differ, so the flat"
            echo "      cases below prove nothing."
            fail=1; }
        ;;
esac

flat "a two-input table read out over [1 1] (Decode [1 1])" 255 \
     $(cellular "<< $tbl2 /Decode [1 1] >>")
flat "a two-input table decoded outside the Range it is clipped to" \
     "$(grey 0.5)" \
     $(cellular "<< /FunctionType 0 /Domain [0 1 0 1] /Size [2 2]
                    /BitsPerSample 8 /DataSource <0040 80FF>
                    /Decode [1 1] /Range [0 0.5] >>")

[ "$fail" = 0 ] || exit 1
echo "SUCCESS (Encode, Decode and Range each held to a page no reader that ignored it can paint)"
exit 0
