#!/bin/sh
# A raster image whose device-space row origin is driven past INT_MAX by a
# large CTM scale must not spin its device-row loop across the whole int
# range. The loop bound is cast from a double; without a clamp an origin
# past INT_MAX wraps the cast to INT_MIN and the loop iterates about two
# billion times per sample row, uninterruptibly. Render such an image under
# a short deadline and require it to finish -- with the bound held inside
# the page it renders at once, whatever the scale.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

verdict_workdir

# An enormous negative scale double-flips Y into a huge positive device
# origin; a forty-row image then drives the row loop forty times.
cat > "$work/dos.ps" <<'PS'
-1e10 -1e10 scale
1 40 8 [1 0 0 40 0 0] { <ff> } image
showpage
PS

# Post-fix this is milliseconds; the unclamped spin is about a second per
# sample row -- roughly forty seconds for forty rows -- so a short deadline
# divides the two cleanly. timeout returns 124 when it has to step in.
out=$(run_limited 15 "$xpost" -q -d pgm -o "$work/o.pgm" "$work/dos.ps" </dev/null 2>&1)
st=$?
if [ "$st" -eq 124 ]; then
    echo "FAIL: the image render did not finish -- the device-row loop was not clamped"
    exit 1
fi
verdict_run "$st" "$out" "the image render" || exit 1
echo "check-image-devclip: ok"
exit 0
