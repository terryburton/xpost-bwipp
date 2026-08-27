#!/bin/sh
# Meson test wrapper: run a check that needs a data target which refuses
# every byte written to it -- the "the disk filled up" case, which no
# ordinary file can be made to reproduce on demand. The PostScript side
# looks for such a target itself and reports INCONCLUSIVE when the
# platform has none, which skips the test rather than passing it.
#   $1  path to the built xpost binary
#   $2  path to the test program
#
# The sandbox is lifted because the target is named by absolute path;
# what is under test is the file layer, not the path filter.
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

out=$("$xpost" -q --no-sandbox -d null "$script" </dev/null 2>&1)
status=$?
printf '%s\n' "$out" | tr -d '\000'
if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    exit 1
fi
if printf '%s\n' "$out" | grep -q '^INCONCLUSIVE'; then
    echo "SKIP: this platform offers no data target that refuses data"
    exit 77
fi
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the check"
