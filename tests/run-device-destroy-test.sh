#!/bin/sh
# Meson test wrapper: run the device-teardown discipline check
# (device_destroy_test.ps) against the lifetime roster of
# tests/device-fleet.sh, one device per release implementation. The
# test Destroys
# the live device twice and job-end teardown makes a third call: each
# must be a no-op after the first, per the device contract. The window
# devices have the most to release twice: xcb a display connection, a
# window, a pixmap and a graphics context, and the Windows pair a window,
# a device context and either a bitmap or a rendering context. xcb runs
# under a virtual display where the host provides one, and the Windows
# pair run where the platform can open a window.
#
#   $1  path to the built xpost binary
#   $2  path to device_destroy_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

ns=$(sandbox_flag "$xpost")

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
        fail=1
        continue
    fi
    if verdict_ok "$out" "$dev"; then
        echo "OK   $dev"
    else
        fail=1
    fi
done

if command -v xvfb-run >/dev/null 2>&1; then
    out=$(xvfb-run -a "$xpost" -q $ns -d xcb "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP xcb (not built in)" ;;
        *)
            # This device's teardown runs after the program has printed
            # its verdict -- it holds a display connection, a window, a
            # pixmap and a graphics context -- so what the run said and
            # how it ended are two answers and a pass needs both.
            if [ "$st" -ne 0 ]; then
                echo "FAIL xcb: the interpreter exited with status $st"
                printf '%s\n' "$out" | sed 's/^/      /'
                fail=1
            fi
            if verdict_ok "$out" "xcb"; then
                echo "OK   xcb"
            else
                fail=1
            fi
            ;;
    esac
else
    echo "SKIP xcb (no xvfb-run)"
fi

# The Windows window devices, on the desktop the platform provides. A
# device that is not built in says so, and a host with no desktop to open
# a window on says that; both are declared skips rather than silence.
for dev in gdi gl; do
    out=$("$xpost" -q $ns -d "$dev" "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP $dev (not built in)"; continue ;;
        *"RegisterClass() failed"*|*"CreateWindowEx() failed"*)
            echo "SKIP $dev (no desktop to open a window on)"; continue ;;
    esac
    if [ "$st" -ne 0 ]; then
        echo "FAIL $dev: the interpreter exited with status $st"
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
    echo "FAILURES: $ran of the roster answered and $floor of it is made"
    echo "      without an optional library; the rest said they were not"
    echo "      built in, which is a build to fix rather than a run to pass"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a device did not survive repeated Destroy"
    exit 1
fi
echo "SUCCESS ($ran devices)"
exit 0
