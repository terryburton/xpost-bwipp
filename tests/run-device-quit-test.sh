#!/bin/sh
# Meson test wrapper: run the quit check (quit_run_test.ps) against the
# lifetime roster of tests/device-fleet.sh.
#
# quit ends the run, and what a run does after it has ended is the
# device's teardown. A device whose Destroy is a PostScript procedure is
# torn down by re-entering the interpreter, so it is the one that can
# carry the re-entry on into the program the quit abandoned; a device
# whose Destroy is an operator is called directly and never could. Both
# kinds are run, because which kind a device is is not a property the
# test can see and not one a device is held to.
#
#   $1  path to the built xpost binary
#   $2  path to quit_run_test.ps
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

for dev in $devices; do
    out=$("$xpost" -q $ns -d "$dev" -o "$work/out.$dev" "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP $dev (not built in)"; continue ;;
    esac
    if [ "$st" -ne 0 ]; then
        echo "FAIL $dev: the interpreter exited with status $st"
        fail=1
        continue
    fi
    ran=$((ran + 1))
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
            ran=$((ran + 1))
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
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

rm -rf "$work"
# a run that skipped every device would have asked nothing
if [ "$ran" -lt 8 ]; then
    echo "FAILURES: only $ran devices were tried; that is not this build"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a run did not stop at its quit"
    exit 1
fi
echo "SUCCESS ($ran devices)"
exit 0
