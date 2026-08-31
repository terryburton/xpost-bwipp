#!/bin/sh
# Meson/make-check wrapper: render a known fill through the bbox device and
# require the exact bounding box it reports. Exercises the -d bbox path end to
# end (device selection, fill accumulation, device->user y-flip), then the
# WhiteIsOpaque device key: by default painted white does not contribute to
# the box (crop semantics); with /WhiteIsOpaque true it does. A final job at
# 144dpi requires the same user-space box, proving Emit unscales the device
# resolution and that a same-device setpagedevice merges with overrides.
#   $1  path to the built xpost binary
#   $2  path to bbox_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
skip_if_faceless "$xpost" "this run shows text through a face"
expect='%%BoundingBox: 10 10 50 60'
out=$("$xpost" -q -d bbox -o /dev/null "$script" </dev/null 2>&1)
status=$?
printf '%s\n' "$out"
verdict_run "$status" "$out" "the bbox job" || exit 1
printf '%s\n' "$out" | grep -qx "$expect" || exit 1

tmp=${TMPDIR:-/tmp}/bbox-wio-$$.ps
trap 'rm -f "$tmp"' EXIT INT TERM
cat > "$tmp" <<'PSEOF'
<< /OutputDevice /bbox /PageSize [200 200] /HWResolution [72 72] >> setpagedevice
0 setgray newpath 10 10 moveto 40 0 rlineto 0 40 rlineto -40 0 rlineto closepath fill
1 setgray newpath 100 100 moveto 50 0 rlineto 0 50 rlineto -50 0 rlineto closepath fill
showpage
<< /WhiteIsOpaque true >> setpagedevice
0 setgray newpath 10 10 moveto 40 0 rlineto 0 40 rlineto -40 0 rlineto closepath fill
1 setgray newpath 100 100 moveto 50 0 rlineto 0 50 rlineto -50 0 rlineto closepath fill
showpage
<< /HWResolution [144 144] /WhiteIsOpaque false >> setpagedevice
0 setgray newpath 10 10 moveto 40 0 rlineto 0 40 rlineto -40 0 rlineto closepath fill
1 setgray newpath 100 100 moveto 50 0 rlineto 0 50 rlineto -50 0 rlineto closepath fill
showpage
quit
PSEOF
out=$("$xpost" -q -d null -o /dev/null "$tmp" </dev/null 2>&1)
status=$?
printf '%s\n' "$out"
verdict_run "$status" "$out" "the WhiteIsOpaque job" || exit 1
printf '%s\n' "$out" | grep -q '%%BoundingBox: 10 10 50 50' || exit 1
printf '%s\n' "$out" | grep -q '%%BoundingBox: 10 10 150 150' || exit 1
test "$(printf '%s\n' "$out" | grep -c '%%BoundingBox: 10 10 50 50')" = 2 || exit 1
# Text reaches the box by a path of its own: an extent-tracking device
# needs no glyph rasterization, so each glyph contributes its ink box
# rather than its pixels. The numbers depend on whichever font resolves,
# so what is required here holds for any of them: the box is not empty,
# it starts no further left than the pen, the ascender of the capital
# rises above the baseline and the descender of the g falls below it.
# An even-odd frame at fractional coordinates: the box must be the
# path's own vertices, identical at 72 and 144 dpi. The frame's interior
# resolves to pixel-band rectangles on the polygon route, whose rows sit
# on the device grid, so a box read off the bands grows with the grid's
# coarseness; the bbox device takes the whole path instead (FillPath)
# and this holds it to that. Quarter-point coordinates are exact in
# binary at both resolutions, so the required strings are exact.
eo=${TMPDIR:-/tmp}/bbox-eo-$$.ps
trap 'rm -f "$tmp" "$eo"' EXIT INT TERM
cat > "$eo" <<'PSEOF'
<< /OutputDevice /bbox /PageSize [200 200] /HWResolution [72 72] >> setpagedevice
0 setgray
newpath 10.25 10.75 moveto 89.25 10.75 lineto 89.25 60.75 lineto 10.25 60.75 lineto closepath
        20.25 20.75 moveto 20.25 50.75 lineto 79.25 50.75 lineto 79.25 20.75 lineto closepath
eofill
showpage
<< /HWResolution [144 144] >> setpagedevice
0 setgray
newpath 10.25 10.75 moveto 89.25 10.75 lineto 89.25 60.75 lineto 10.25 60.75 lineto closepath
        20.25 20.75 moveto 20.25 50.75 lineto 79.25 50.75 lineto 79.25 20.75 lineto closepath
eofill
showpage
quit
PSEOF
out=$("$xpost" -q -d null -o /dev/null "$eo" </dev/null 2>&1)
status=$?
printf '%s\n' "$out"
verdict_run "$status" "$out" "the even-odd exactness job" || exit 1
test "$(printf '%s\n' "$out" | grep -c '%%HiResBoundingBox: 10.25 10.75 89.25 60.75')" = 2 \
    || { echo "FAIL: the even-odd box is not the path's own, at both resolutions"; exit 1; }
echo "even-odd exact bounding box OK"

txt=${TMPDIR:-/tmp}/bbox-text-$$.ps
trap 'rm -f "$tmp" "$eo" "$txt"' EXIT INT TERM
cat > "$txt" <<'PSEOF'
/Helvetica findfont 24 scalefont setfont
20 40 moveto (Ag) show
showpage
quit
PSEOF
out=$("$xpost" -q -d bbox -o /dev/null "$txt" </dev/null 2>&1)
status=$?
printf '%s\n' "$out"
verdict_run "$status" "$out" "the text job" || exit 1
box=$(printf '%s\n' "$out" | grep -m1 '^%%BoundingBox:')
[ -n "$box" ] || { echo "FAIL: shown text produced no bounding box"; exit 1; }
set -- $box
llx=$2; lly=$3; urx=$4; ury=$5
[ "$urx" -gt "$llx" ] && [ "$ury" -gt "$lly" ] \
    || { echo "FAIL: the text box is empty: $box"; exit 1; }
[ "$llx" -ge 19 ] || { echo "FAIL: the text box starts left of the pen: $box"; exit 1; }
[ "$ury" -gt 40 ] || { echo "FAIL: nothing rises above the baseline: $box"; exit 1; }
[ "$lly" -lt 40 ] || { echo "FAIL: nothing falls below the baseline: $box"; exit 1; }
echo "text bounding box OK ($box)"

# A box belongs to the page it was accumulated for. showpage clears the
# page, and a cleared page holds no marks, so the next page's box is the
# next page's marks -- not the marks of everything the job has drawn.
# The pages below are disjoint and the second is the smaller: a box that
# carries reports the first page again, and a box that is thrown away
# too eagerly reports nothing.
tmp2=${TMPDIR:-/tmp}/bbox-perpage-$$.ps
trap 'rm -f "$tmp2"' EXIT
cat > "$tmp2" <<'PSEOF'
0 setgray
newpath 200 200 moveto 100 0 rlineto 0 100 rlineto -100 0 rlineto closepath fill
showpage
newpath 20 20 moveto 30 0 rlineto 0 30 rlineto -30 0 rlineto closepath fill
showpage
quit
PSEOF
out=$("$xpost" -q -d bbox -o /dev/null "$tmp2" </dev/null 2>&1)
status=$?
printf '%s
' "$out"
verdict_run "$status" "$out" "the per-page box job" || exit 1
printf '%s
' "$out" | grep -qx '%%BoundingBox: 200 200 300 300' \
    || { echo "FAIL: the first page's box is not its own marks"; exit 1; }
printf '%s
' "$out" | grep -qx '%%BoundingBox: 20 20 50 50' \
    || { echo "FAIL: the second page's box is not its own marks; a box that"
         echo "      carries across showpage reports the whole job instead"
         exit 1; }
echo "per-page bounding box OK"

# A device's writable state sits beside the device, not above it. It has
# to be writable -- it is what the page does to the device -- so a
# program can put whatever it likes under whatever key it likes. What it
# must not buy with that is the device's own machinery: if a method body
# reached its names down a dictionary stack with the state on top, a
# procedure stored under a method's name would be found first, and the
# program would be choosing the code the device runs on every mark.
tmp3=$(mktemp); trap 'rm -f "$tmp3"' EXIT
cat > "$tmp3" <<'PSEOF'
/ranmine false def
100 100 50 50 rectfill
DEVICE /.state get /.maxmin { pop pop /ranmine true store } put
newpath 200 200 moveto 300 300 lineto 300 200 lineto closepath fill
(planted: ) print ranmine ==
showpage
quit
PSEOF
out=$("$xpost" -q -d bbox -o /dev/null "$tmp3" </dev/null 2>&1)
status=$?
printf '%s\n' "$out"
verdict_run "$status" "$out" "the shadowed-method job" || exit 1
printf '%s\n' "$out" | grep -qx 'planted: false' \
    || { echo "FAIL: the device ran a procedure the program stored in its"
         echo "      state -- state named above the device on the dictionary"
         echo "      stack shadows every method the device has"
         exit 1; }
printf '%s\n' "$out" | grep -qx '%%BoundingBox: 100 100 300 300' \
    || { echo "FAIL: the box does not cover both marks, so the device's own"
         echo "      extent method did not run for the second one"
         exit 1; }
echo "state does not shadow the device's methods OK"

# The device tests ask for their devices unsealed, so that they can reach
# in and state what is there. That is an instrumentation setting and it
# is read once, as the language is locked down. A run that starts from
# the built image has locked down already: the setting arrives too late
# to be read, and the seal is not a thing an environment can undo. Both
# halves are checked here, because a probe that could not see an unsealed
# device would report this as held whether it were held or not.
tmp4=$(mktemp); trap 'rm -f "$tmp2" "$tmp3" "$tmp4"' EXIT
cat > "$tmp4" <<'PSEOF'
{ DEVICE /zz 1 put } stopped { (sealed) = }{ (unsealed) = } ifelse
quit
PSEOF
# The image has to exist before this can ask anything of it, and it is
# minted here rather than found: a cache the run happens to share with
# whatever ran before it would answer about that run's image.
verdict_workdir
trap 'rm -f "$tmp2" "$tmp3" "$tmp4"; rm -rf "$work"' EXIT INT TERM
sealc=$work/seal; mkdir -p "$sealc"
env -u XPOST_NO_VM_IMAGE -u XPOST_DATA_DIR -u XPOST_UNSEALED_DEVICES \
    HOME="$sealc" XDG_CACHE_HOME="$sealc" LOCALAPPDATA="$sealc" \
    "$xpost" -q -d bbox -o /dev/null "$tmp4" </dev/null >/dev/null 2>&1
find "$sealc" -name '*.vmimg' 2>/dev/null | grep -q . \
    || { echo "FAIL: no image was written, so what follows would ask"
         echo "      nothing of one"; rm -rf "$sealc"; exit 1; }
out=$(env -u XPOST_NO_VM_IMAGE -u XPOST_DATA_DIR \
        HOME="$sealc" XDG_CACHE_HOME="$sealc" LOCALAPPDATA="$sealc" \
        XPOST_UNSEALED_DEVICES=1 \
        "$xpost" -q -d bbox -o /dev/null "$tmp4" </dev/null 2>&1)
printf '%s\n' "$out" | grep -qx 'sealed' \
    || { echo "FAIL: a run starting from the image left its device unsealed"
         echo "      because the environment asked; the seal is not the"
         echo "      environment's to lift"
         exit 1; }
out=$(XPOST_NO_VM_IMAGE=1 XPOST_UNSEALED_DEVICES=1 "$xpost" -q -d bbox -o /dev/null "$tmp4" </dev/null 2>&1)
printf '%s\n' "$out" | grep -qx 'unsealed' \
    || { echo "FAIL: the setting did nothing even where it is read, so the"
         echo "      check above could not have seen an unsealed device"
         exit 1; }
echo "the seal is not the environment's to lift OK"

# An image is written by the first run that boots the long way, and every
# later run on that machine starts from it. A run given the device
# instrumentation boots to a language whose classes were deliberately
# left open, so if it wrote one, every later run would start with the
# devices open and nothing in that run to say why. It boots the long way
# and leaves the cache alone. HOME and the cache variables move into the
# scratch directory, so this reads and writes its own cache and never the
# user's.
imgc=$work/img; mkdir -p "$imgc"
env -u XPOST_NO_VM_IMAGE -u XPOST_DATA_DIR \
    HOME="$imgc" XDG_CACHE_HOME="$imgc" LOCALAPPDATA="$imgc" \
    XPOST_UNSEALED_DEVICES=1 "$xpost" -q -d bbox -o /dev/null "$tmp4" \
    </dev/null >/dev/null 2>&1
if find "$imgc" -name '*.vmimg' 2>/dev/null | grep -q .; then
    echo "FAIL: a run given the device instrumentation wrote an image of"
    echo "      virtual memory; every later run on the machine would boot"
    echo "      from it with the devices left open"
    exit 1
fi
out=$(env -u XPOST_NO_VM_IMAGE -u XPOST_DATA_DIR -u XPOST_UNSEALED_DEVICES \
      HOME="$imgc" XDG_CACHE_HOME="$imgc" LOCALAPPDATA="$imgc" \
      "$xpost" -q -d bbox -o /dev/null "$tmp4" </dev/null 2>&1)
printf '%s\n' "$out" | grep -qx 'sealed' \
    || { echo "FAIL: a plain run after an instrumented one found its"
         echo "      device open, so the instrumented run left something"
         echo "      behind for it to boot from"
         exit 1; }
echo "an instrumented run leaves no image behind OK"

exit 0
