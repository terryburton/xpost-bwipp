#!/bin/sh
#
# Every device takes its marks in bulk, or says why it takes them one at
# a time.
#
# A method a device states as a compiled operator can be called from the
# machinery that resolves marks: the span conversion fills through it
# (xpost_dev_generic.c), the image blit writes rows through it
# (data/paint.ps, blitok), and the glyph walk binds it once for a string
# (xpost_op_font.c). A method stated as a PostScript procedure can be
# called by nothing but the interpreter, so the machinery hands it the
# marks one at a time instead -- a call per span, per sample, per inked
# pixel.
#
# That difference does not announce itself. Both spellings paint the same
# page, the register of what a device states says nothing about which was
# used, and the cost only shows as a device being slow in a way nothing
# attributes to the device. It was found by measuring the device that
# paints nothing: an image cost it eighty-seven seconds to discard, where
# a raster painting every pixel took half a second, and a stencil cost a
# real raster device thirty-six seconds where the same pixels as an image
# took a third of one. A device that draws nothing cannot be slower than
# one that draws, and where it is, the pipeline is handing marks over
# singly for everyone.
#
# ---- what this holds
#
# The register beside this file, tests/device-fastpaths, has one line per
# device and slot that is NOT a compiled operator, and the two are held to
# each other in both directions:
#
#   a slot that is a procedure and that no line excuses fails -- so a
#   device cannot quietly arrive taking its marks singly.
#
#   a line excusing a slot that is compiled, or a device that is gone,
#   fails -- so the register cannot keep an excuse that stopped being
#   true.
#
# An excuse is a reason, not a note. A vector writer's FillPoly IS its
# output language and cannot be anything else; a device that discards has
# nothing to call. Those are reasons. "Not done yet" is not one, and the
# register says so where it is the truth.
#
# ---- one axis of several
#
# This asks whether a mark can be handed over cheaply. It is not the only
# way a device avoids work, and a device is optimised when it carries
# every mechanism its output shape admits rather than the same set as its
# neighbour. The others, each with its own applicability:
#
#   PaintImage, PaintImageMasked, PaintStencil, FillPattern
#                    take the object whole rather than a mark per sample,
#                    per run or per tile
#   DiscardsMarks    do not resolve the marks at all
#   GlyphExtents     take a glyph's ink box rather than its pixels
#   .recordimage, .recordglyph   record the object for replay
#   .emptyrow, .groundrow        do not emit a row that is all ground
#   .bandrows, .bandtop          do not hold a whole page
#
# The whole-object hooks are held below. The rest are classified by
# tests/device-facts, which says what each entry is but does not demand
# one, so a device that could carry one and does not is invisible there.
# Adding them here is the way to make it visible.
#
#   $1  path to the source tree root
#   $2  path to the built interpreter

set -u

src=${1:?usage: check-device-fastpaths.sh <srcroot> <xpost>}
xpost=${2:?usage: check-device-fastpaths.sh <srcroot> <xpost>}

. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$xpost" "the interpreter"
case $xpost in
    /*) ;;
    *)  xpost=$(cd "$(dirname "$xpost")" && pwd)/$(basename "$xpost") ;;
esac
guard_require_interpreter "$xpost"
guard_workdir
guard_srcdata "$src"

# The machinery's own names, which a run reporting on the interpreter is
# handed and a run of a program is not.
XPOST_CENSUS=1
export XPOST_CENSUS

register=$src/tests/device-fastpaths
register_facts=$src/tests/device-facts
fleet=$src/tests/device-fleet.sh
[ -f "$register" ] || { echo "FAILURES: $register is missing"; exit 1; }
[ -f "$fleet" ] || { echo "FAILURES: $fleet is missing"; exit 1; }

# The slots whose spelling decides whether marks are taken in bulk. Each
# is a method the resolving machinery would call directly if it could.
SLOTS="PutPix GetPix BlendPix FillRect FillPoly"

( . "$fleet"; for v in $DEVICE_FLEET_ALL; do echo "$v"; done ) 2>/dev/null \
    | sort -u > "$work/roster"
[ -s "$work/roster" ] || { echo "FAILURES: tests/device-fleet.sh names no roster"; exit 1; }

{
    echo "["
    grep -vx record "$work/roster" | sed 's|^|/|'
    echo "]"
    cat <<'EOF'
{ /D exch def
  { << /OutputDevice D /PageSize [ 8 8 ] >> setpagedevice } stopped
  { (UNMADE ) print D 60 string cvs print (\n) print }
  {
    [ /PutPix /GetPix /BlendPix /FillRect /FillPoly ]
    { /S exch def
      (F ) print D 60 string cvs print ( ) print S 60 string cvs print ( ) print
      DEVICE S 2 copy known {
          get type /operatortype eq { (compiled) }{ (procedure) } ifelse
      }{ pop pop (absent) } ifelse print (\n) print
    } forall
    [ /PaintImage /PaintImageMasked /PaintStencil /FillPattern ]
    { /S exch def
      (W ) print D 60 string cvs print ( ) print S 60 string cvs print ( ) print
      DEVICE S known { (present) }{ (absent) } ifelse print (\n) print
    } forall
  } ifelse
} forall
EOF
} > "$work/ask.ps"

out=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
       "$xpost" -q -d null -o fp.scratch ask.ps </dev/null 2>"$work/ask.err" )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "FAILURES: the interpreter could not be asked what its devices call:"
    sed 's/^/      /' "$work/ask.err" | head -5
    exit 1
fi
printf '%s\n' "$out" | grep '^F ' | awk '{print $2, $3, $4}' | sort > "$work/said"
printf '%s\n' "$out" | grep '^W ' | awk '{print $2, $3, $4}' | sort > "$work/wsaid"
[ -s "$work/said" ] || { echo "FAILURES: no device stated any method"; exit 1; }

# what the register excuses
grep -vE '^[[:space:]]*#|^[[:space:]]*$' "$register" \
    | grep -E '^slow ' | awk '{print $2, $3}' | sort -u > "$work/excused"

# a slot that is not compiled and not excused
awk '$3 != "compiled" { print $1, $2 }' "$work/said" | sort -u > "$work/slow"
guard_held=0
guard_hold "$work/slow" "$work/excused" \
  "a device takes its marks one at a time and tests/device-fastpaths does not say why. Add a 'slow <device> <slot>' line with the reason it cannot be compiled, or give the device a compiled method:" \
  "tests/device-fastpaths excuses a slot that is not slow, or a device that is gone. An excuse that stopped being true reads like one that was never examined:"
[ "$guard_held" -eq 0 ] || exit 1

# ---- the whole-object hooks
#
# A device whose output language has an image or a stencil in it is handed
# one whole, before the image matrix is concatenated, rather than a mark
# per sample or per run: data/paint.ps offers PaintImage,
# PaintImageMasked and PaintStencil there. Which devices those are is not
# named here -- a device carrying VectorGlyphs writes a document rather
# than a raster, tests/device-facts says which they are and holds it, and
# that is the same set -- so a fourth writer is held to this the day it
# arrives.
WHOLE="PaintImage PaintImageMasked PaintStencil FillPattern"
awk '$2 == "VectorGlyphs" && $1 == "open" {
        for (i = 3; i <= NF; i++) print $i }' "$register_facts" \
    | LC_ALL=C sort -u > "$work/writers"
if [ -s "$work/writers" ]; then
    : > "$work/wwant"
    while read -r d; do
        for h in $WHOLE; do echo "$d $h"; done
    done < "$work/writers" | LC_ALL=C sort -u > "$work/wwant"
    awk '$3 != "absent" { print $1, $2 }' "$work/wsaid" \
        | LC_ALL=C sort -u > "$work/whave"
    grep -E '^whole ' "$register" | awk '{print $2, $3}' \
        | LC_ALL=C sort -u > "$work/wexcused"
    LC_ALL=C sort -u "$work/whave" "$work/wexcused" > "$work/wcovered"
    # An excuse for a hook the device carries is one that stopped being
    # true, and a set union would hide it: the device is covered either
    # way, so the excuse has to be held to the absence it names.
    LC_ALL=C comm -12 "$work/wexcused" "$work/whave" > "$work/wboth"
    if [ -s "$work/wboth" ]; then
        echo "FAIL: tests/device-fastpaths excuses a whole-object hook the"
        echo "      device carries. An excuse that stopped being true reads"
        echo "      like one that was examined:"
        sed 's/^/      /' "$work/wboth"
        guard_held=1
    fi
    guard_hold "$work/wwant" "$work/wcovered" \
      "a device that writes a document does not take an image or a stencil whole, so it takes a mark per sample or per run. Give it the method, or a 'whole <device> <hook>' line in tests/device-fastpaths with the reason its language has no such construct:" \
      "tests/device-fastpaths excuses a whole-object hook for a device that has it, or that does not write a document:"
    [ "$guard_held" -eq 0 ] || exit 1
fi

n=$(wc -l < "$work/said" | tr -d ' ')
e=$(wc -l < "$work/excused" | tr -d ' ')
w=$(wc -l < "$work/wwant" 2>/dev/null | tr -d ' '); w=${w:-0}
wx=$(wc -l < "$work/wexcused" 2>/dev/null | tr -d ' '); wx=${wx:-0}
printf 'SUCCESS (%s device/slot pairs stated; %s of them take their marks\n' "$n" "$e"
printf '         one at a time, each with a reason the register carries.\n'
printf '         %s whole-object hooks asked of the devices that write a\n' "$w"
printf '         document; %s of those excused)\n' "$wx"
