#!/bin/sh
# Run a PostScript suite on a device that halftones for itself.
#
#   $1  path to the built xpost binary
#   $2  path to test.ps
#
# tests/run-ps-test.sh names the null device, which paints nothing and so
# never builds a threshold cell. A suite whose subject is the screening
# path has to be painted somewhere that screens, and the device is named
# here rather than by the suite because a program cannot choose the
# device it was started on -- which is the whole reason the screening
# path is reachable only this way.
#
# The page goes nowhere: what the suite reports is what it printed while
# painting, not the raster it painted.
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
verdict_workdir
. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$script" "$work"
run=$testlib_run
out=$("$xpost" -q --no-sandbox -d pbm -o "$work/page.pbm" "$run" </dev/null 2>&1)
status=$?
printf '%s\n' "$out"
if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    exit 1
fi
verdict_ok "$out"
