#!/bin/sh
# Meson test wrapper: hold the interpreter to a small open-file allowance
# while it opens far more files than that, each ended a different way.
#
# A descriptor that is not released is invisible on a host that allows a
# million of them: the job simply runs. Lowering the allowance turns the
# same leak into a refusal to open, at the point it happens. It is how
# the file a run wraps around its program was found to be leaking one per
# failed job.
#
# The workload also asks for a collection in every round, since a file is
# held by the interpreter outside its stacks and a collection is where
# that goes wrong.
#
#   $1  path to the built xpost binary
#   $2  path to the PostScript workload
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
# an absolute path may begin with a drive letter as well as a slash;
# prepending the working directory to one of those makes every
# invocation a path that does not exist
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac
case $script in /* | ?:/* | ?:\\*) ;; *) script=$PWD/$script ;; esac

allowance=96
if ! ( ulimit -n "$allowance" ) 2>/dev/null; then
    echo "SKIP: this shell cannot lower the open-file allowance"
    exit 77
fi

verdict_workdir

. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$script" "$work"

out=$(
    cd "$work" || exit 1
    ulimit -n "$allowance"
    "$xpost" -q --no-sandbox -d null "$testlib_run" </dev/null 2>&1
)
status=$?
printf '%s\n' "$out"

if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    exit 1
fi
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the workload"
