#!/bin/sh
# Meson test wrapper: render one trivial page through EVERY built output
# device and require each to succeed. The rest of the suite runs under
# -d null, which never loads the graphics/device stack, so a device that
# fails to initialise or emit a page is invisible to it.
#
# This is where "every" is spelt: the roster in tests/device-fleet.sh,
# which check-device-roster.sh holds to the interpreter's maker table.
# The cross-product wrappers run representative subsets of it, so a
# device that leaves one of those is still rendered here.
#
# Two device classes:
#   file  - writes a raster/vector page to the -o path; must emit bytes.
#   buf   - leaves nothing at that path: the two whose raster is a buffer
#           the library hands back rather than a file, which the CLI
#           cannot capture, and the two that paint nothing at all. Each
#           is required only to render to completion with no error, which
#           still exercises the full graphics + device init and fillrect
#           path -- what a device regression breaks.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/device-fleet.sh"
. "$(dirname "$0")/verdict.sh"

# Reach the interpreter's data directory, which lives outside any sandbox
# root. When this build has a file-access sandbox, disable it for the test;
# earlier builds have no such option and need nothing. Detect it from the
# usage text rather than assuming, so the one test is valid at every point
# in the series.
ns=$(sandbox_flag "$xpost")

verdict_workdir
prog="$work/page.ps"

# The page is not quite trivial, and the part that is not earns its place.
# A stroked line alone was what this rendered, and it asks too little: the
# constructs a device may have to decompose rather than emit -- a tiling
# pattern above all -- were never put to one here, and a tiling pattern
# after setpagedevice raised a typecheck on all three vector writers while
# every raster device painted it. The page size is set for the same
# reason: it is what made the writers' height fractional, which is what
# the failure turned on.
cat > "$prog" <<'PAGEEOF'
<< /PageSize [100 100] >> setpagedevice
newpath 10 10 moveto 90 90 lineto stroke
gsave
  << /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 8 8]
     /XStep 8 /YStep 8 /PaintProc { pop 0 0 4 4 rectfill } >>
  matrix makepattern /Pattern setcolorspace setcolor
  10 60 30 30 rectfill
grestore
showpage
PAGEEOF

# the devices that leave nothing at the -o path, and everything else in
# the roster. Which are which is DEVICE_FLEET_NOFILE's answer, held by
# check-device-roster.sh against what each device actually leaves there,
# so a device added to the roster is one this wrapper requires bytes of
# unless the roster says its page is not a file.
file_devices=
for dev in $DEVICE_FLEET_ALL; do
    case " $DEVICE_FLEET_NOFILE " in
        *" $dev "*) continue ;;
    esac
    file_devices="$file_devices $dev"
done
buf_devices=$DEVICE_FLEET_NOFILE

fail=0
ran=0

# This wrapper's whole claim is that every device in the roster renders,
# and a device that is not built in skips. A roster that skipped from
# end to end renders nothing, leaves nothing to weigh and reports the
# claim kept. The floor is the roster less what a build may not have the
# library for.
floor=0
for dev in $DEVICE_FLEET_ALL; do
    case " $DEVICE_FLEET_OPTIONAL " in *" $dev "*) continue ;; esac
    floor=$((floor + 1))
done

run_dev() {   # $1=device
    dev=$1
    out="$work/out.$dev"
    rm -f "$out"
    # a device this build did not compile in prints "wrong device"; skip it.
    err=$("$xpost" -q $ns -d "$dev" -o "$out" "$prog" </dev/null 2>&1)
    st=$?
    case "$err" in
        *"wrong device"*) echo "SKIP $dev (not built in)"; return 2 ;;
    esac
    # what the run left is read by the caller; what it said and how it
    # left are read here. A device that painted every pixel and then
    # faulted in its teardown wrote a whole file to be found.
    verdict_run "$st" "$err" "$dev" || return 1
    if printf '%s' "$err" | grep -q '%%\[ Error'; then
        echo "FAIL $dev: $(printf '%s' "$err" | grep '%%\[ Error' | head -1)"
        return 1
    fi
    return 0
}

for dev in $file_devices; do
    run_dev "$dev"; rc=$?
    [ "$rc" -eq 2 ] && continue
    ran=$((ran + 1))
    if [ "$rc" -ne 0 ]; then fail=1; continue; fi
    if [ -f "$out" ]; then sz=$(wc -c < "$out"); else sz=0; fi
    if [ "${sz:-0}" -le 0 ]; then
        echo "FAIL $dev: produced no output"; fail=1
    else
        echo "OK   $dev ($sz bytes)"
    fi
done

for dev in $buf_devices; do
    run_dev "$dev"; rc=$?
    [ "$rc" -eq 2 ] && continue
    ran=$((ran + 1))
    if [ "$rc" -ne 0 ]; then fail=1; continue; fi
    echo "OK   $dev (rendered, leaves no file)"
done

rm -rf "$work"
if [ "$ran" -lt "$floor" ]; then
    echo "FAILURES: $ran of the roster rendered and $floor of it is made"
    echo "      without an optional library; the rest said they were not"
    echo "      built in, which is a build to fix rather than a run to pass"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: at least one device did not render"
    exit 1
fi
echo "SUCCESS ($ran devices)"
exit 0
