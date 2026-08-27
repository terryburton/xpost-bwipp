#!/bin/sh
#
# Every runner asks whether the suite skipped before it asks whether it passed.
#
# A suite that cannot ask its question in the build it was started in says
# so and exits: the one that paints text through a face says it under a
# build carrying no face library. Whether a suite can do that is a property
# of the suite. Which runner starts it is not, and the two are matched by
# what the build description happens to say -- so a suite that learns to
# skip is a suite whose runner may never have been asked to expect one.
#
# A runner that asks only whether the run passed reads the skip as a
# failure, and reports the build as broken where the suite deliberately
# declined to answer. That is not hypothetical: a suite gained a skip for
# the faceless build, its runner was the one that names a device, and the
# faceless build went red on a suite that had correctly declined.
#
# So the rule is held over the whole population rather than over the pairs:
# a runner that reaches verdict_ok reaches verdict_skipped first. The check
# costs nothing where a suite never skips -- the verdict returns and the
# run goes on to the success test -- which is what makes it safe to ask of
# every runner and therefore possible to hold every runner to.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-skip-verdict.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_dir "$src/tests" "the test directory"

guard_workdir
guard_held=0
n=0
missing=0

for f in "$src"/tests/run-*.sh; do
    [ -f "$f" ] || continue
    grep -q 'verdict_ok' "$f" || continue
    n=$((n + 1))
    if ! grep -q 'verdict_skipped' "$f"; then
        if [ "$missing" -eq 0 ]; then
            echo "FAILURES: a runner asks whether the suite passed without first"
            echo "      asking whether it skipped. A suite that declines to answer"
            echo "      in this build is then reported as a failure:"
        fi
        echo "      $(basename "$f")"
        missing=$((missing + 1))
        guard_held=1
    fi
done

if [ "$n" -lt 20 ]; then
    echo "FAILURES: only $n runners were found under tests/, fewer than this"
    echo "      tree carries -- the derivation has stopped matching the"
    echo "      runners and would report full coverage of almost none"
    exit 1
fi

[ "$guard_held" -eq 0 ] || exit 1
printf 'skip verdict: %s runners, each asks for a skip before a pass: SUCCESS\n' "$n"
exit 0
