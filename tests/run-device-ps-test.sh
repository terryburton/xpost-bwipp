#!/bin/sh
# Run a PostScript suite on a named output device.
#
#   $1  path to the built xpost binary
#   $2  path to test.ps
#   $3  the device to paint on
#
# tests/run-ps-test.sh names the null device, which paints nothing. A
# suite whose subject is what happens WHILE painting cannot ask its
# question there: the screening path never builds a threshold cell, and
# the branch that captures a form for the form cache is never taken. Both
# are reachable only by painting somewhere that paints, and the device is
# named here rather than by the suite because a program cannot choose the
# device it was started on.
#
# The page goes nowhere: what the suite reports is what it printed while
# painting, not the raster it painted.
set -u
xpost=$1
script=$2
device=${3:?usage: run-device-ps-test.sh <xpost> <script.ps> <device>}
. "$(dirname "$0")/verdict.sh"
verdict_workdir
. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$script" "$work"
run=$testlib_run
out=$("$xpost" -q --no-sandbox -d "$device" -o "$work/page.out" "$run" </dev/null 2>&1)
status=$?
printf '%s\n' "$out"
if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    exit 1
fi
verdict_ok "$out"
