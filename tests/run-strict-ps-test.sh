#!/bin/sh
# Meson test wrapper: run a PostScript test and require that its output is
# exactly "SUCCESS" -- every assertion must hold AND nothing else may print.
# Guards machinery that must stay silent: a stray diagnostic from a device
# method interleaves with a page stream written to standard output and
# corrupts it.
#
# The comparison is against what the run wrote, whole and unfiltered. A run
# nobody is watching -- no terminal on the standard input, and here not even
# an open one -- is one the interpreter says nothing of its own to: no
# greeting, no page-boundary announcement, no prompt. So the output channel
# carries the program's answer alone, and anything of the interpreter's
# arriving on it is a failure of the thing this asserts rather than
# something to filter out before asserting it.
#   $1  path to the built xpost binary
#   $2  path to the test script
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
# capture stdout only: the silence requirement is about the page-stream
# channel; the log channel (stderr) is judged by other tests
out=$("$xpost" -q --no-sandbox -d null "$script" </dev/null 2>/dev/null)
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
verdict_ok "$out" "the run" || exit 1
# and nothing but: the silence is the other half of what is asserted here
test "$out" = "SUCCESS"
