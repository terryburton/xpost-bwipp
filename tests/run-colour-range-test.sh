#!/bin/sh
# Meson test wrapper: run the out-of-range value check
# (colour_range_test.ps) against the marking roster of
# tests/device-fleet.sh.
#
# Three tiers. The formatter tier and the component tier are the same on
# every device and run everywhere. The coverage tier asks what BlendPix
# painted, so it runs only on a device whose GetPix reports back what a
# marking method wrote; a device that keeps its raster somewhere this
# test cannot see announces itself by failing the readback probe and
# takes the other two alone. The count below holds that tier to running
# on the devices that can witness it, so a device that stops reporting
# its pixels cannot quietly reduce what is asserted.
#
#   $1  path to the built xpost binary
#   $2  path to colour_range_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

# devices whose GetPix reports back what BlendPix wrote
readback_min=5
readback=0

ns=$(sandbox_flag "$xpost")

verdict_workdir
devices=$DEVICE_FLEET_MARKING
fail=0

for dev in $devices; do
    # The device holding the whole page, asked for as the mode that says
    # so. The coverage tier reads back what BlendPix left, and selecting
    # a device by name selects the record in front of it, which keeps
    # the marks a page made rather than the pixels they cover and
    # answers a read with the ground. The record answers for itself as a
    # member of the roster below.
    out=$("$xpost" -q $ns -d "$(fleet_whole "$dev")" -o "$work/out.$dev" \
          "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP $dev (not built in)"; continue ;;
    esac
    if [ "$st" -ne 0 ]; then
        echo "FAIL $dev: the interpreter exited with status $st"
        fail=1
        continue
    fi
    if printf '%s\n' "$out" | grep -q '^READBACK$'; then
        readback=$((readback + 1))
    fi
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
    if verdict_ok "$out" "$dev"; then
        echo "OK   $dev"
    else
        fail=1
    fi
done

rm -rf "$work"
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a value outside its range was not folded to the nearest one"
    exit 1
fi
if [ "$readback" -lt "$readback_min" ]; then
    echo "FAILURES: the coverage tier ran on $readback devices, fewer than $readback_min"
    echo "      a device that no longer reports its pixels back silently"
    echo "      stops being asserted about; restore its GetPix"
    exit 1
fi
echo "SUCCESS ($readback devices witnessed the coverage tier)"
exit 0
