#!/bin/sh
# Meson test wrapper: run the open-file-limit check with the process's
# open-file allowance lowered, so the limit is reached in a moment rather
# than after a thousand opens (and on hosts whose allowance is a million,
# at all).
#
# Where the shell cannot lower the allowance the check cannot be made and
# the test skips, rather than passing without having tested anything.
#
#   $1  path to the built xpost binary
#   $2  path to the PostScript program
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# the run happens in a scratch directory, so both paths must survive the
# change of directory
# an absolute path may begin with a drive letter as well as a slash;
# prepending the working directory to one of those makes every
# invocation a path that does not exist
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac
case $script in /* | ?:/* | ?:\\*) ;; *) script=$PWD/$script ;; esac

allowance=64
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
verdict_ok "$out" "the suite"
