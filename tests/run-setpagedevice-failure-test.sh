#!/bin/sh
# Meson test wrapper: a setpagedevice whose maker fails must leave the
# incumbent device painting (setpagedevice_failure_test.ps), run against
# the lifetime roster of tests/device-fleet.sh.
#
# The devices that keep their raster in a malloc'd buffer are the ones with
# something to lose: retiring them frees that buffer, so a graphics state
# still naming a retired device reads freed memory on the next mark. The
# devices that hold their raster as PostScript objects are run too -- they
# cost nothing to check and the rule is the same for all of them.
#
#   $1  path to the built xpost binary
#   $2  path to setpagedevice_failure_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

if "$xpost" -h 2>/dev/null | grep -q -- '--no-sandbox'; then
    ns='--no-sandbox'
else
    ns=''
fi

verdict_workdir
devices=$DEVICE_FLEET_LIFETIME
fail=0
ran=0

# A roster that skipped from end to end leaves the loop having asked
# nothing and every verdict untaken, which reads exactly as a roster
# that answered. The floor is the roster less what a build may not have
# the library for.
floor=0
for dev in $devices; do
    case " $DEVICE_FLEET_OPTIONAL " in *" $dev "*) continue ;; esac
    floor=$((floor + 1))
done

for dev in $devices; do
    out=$("$xpost" -q $ns -d "$dev" -o "$work/out.$dev" "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP $dev (not built in)"; continue ;;
    esac
    ran=$((ran + 1))
    if [ "$st" -ne 0 ]; then
        echo "FAIL $dev: the interpreter exited with status $st"
        printf '%s\n' "$out" | tail -3
        fail=1
        continue
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
if [ "$ran" -lt "$floor" ]; then
    echo "FAILURES: $ran of the roster's devices answered, and $floor of them"
    echo "      are made without an optional library; the rest said they were"
    echo "      not built in, which is a build to fix rather than a run to pass"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a caught setpagedevice failure did not leave the device painting"
    exit 1
fi
echo "SUCCESS ($ran devices)"
exit 0
