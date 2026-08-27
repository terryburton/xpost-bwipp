#!/bin/sh
# Meson test wrapper: run a script through the interpreter with graphics
# loading skipped (--no-graphics), exercising the no-graphics start procedure
# and lockdown path. Passes iff the script reports SUCCESS.
#   $1  path to the built xpost binary
#   $2  path to the test .ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
# --no-graphics selects the no-graphics start procedure; lets the
# script exercise the interpreter's own file operations
out=$("$xpost" --no-graphics --no-sandbox -q -d null "$script" </dev/null 2>&1)
status=$?
if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    exit 1
fi
printf '%s\n' "$out"
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the script"
