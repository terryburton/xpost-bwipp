#!/bin/sh
# Meson test wrapper: run a collection-heavy job with the collector's own
# diagnostics enabled and require them to report nothing.
#
# The collector carries an independent reachability verifier and a
# cross-bank scan (xpost_garbage_diag.c), both gated on environment
# variables and so never reached by an ordinary run. They encode two
# invariants worth holding the collector to: every entity reachable from
# the roots is marked, and no global container references a local entity
# the sweep is about to reclaim (PLRM 3.7.2). A diagnostic nothing runs
# is a diagnostic nothing trusts, so run them here.
#
#   $1  path to the built xpost binary
#   $2  path to the PostScript workload
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

verdict_workdir

. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$script" "$work"

out=$(XPOST_GC_VERIFY=1 XPOST_GC_XBANK_CHECK=1 XPOST_GC_CENSUS=1 \
      "$xpost" -q --no-sandbox -d null "$testlib_run" </dev/null 2>&1)
status=$?
printf '%s\n' "$out"

if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    exit 1
fi

# the workload must have run to its end: a job that died early would
# report no gaps simply by never collecting
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the workload" || exit 1

# the census runs alongside the verifier, so its line is the evidence that
# the diagnostics were reached at all rather than compiled out or skipped
if ! printf '%s\n' "$out" | grep -q '^CENSUS:'; then
    echo "FAILURES: the collector diagnostics did not run"
    exit 1
fi

if printf '%s\n' "$out" | grep -qE '^(VERIFY GAP|VERIFY:|XBANK:)'; then
    echo "FAILURES: the collector diagnostics reported the above"
    exit 1
fi

echo "DIAGNOSTICS CLEAN"
