#!/bin/sh
#
# What VMThreshold does: a run that names a small count collects more
# often than one that names a large one, and is left holding less.
#
# PLRM C.3.5 gives the parameter as "the frequency of automatic garbage
# collection, which is triggered whenever this many bytes have been
# allocated since the previous collection". A count the interpreter
# records and never reads would satisfy every question a program can ask
# about the parameter itself -- it reads back, it reverts with restore --
# while changing nothing about the run, so what is held here is the
# effect rather than the setting.
#
# Two interpreters rather than two passes in one. The count of bytes in
# use rises while the memory file is growing and stops rising once a
# collection has left a free list behind it, so a second pass in the
# same process satisfies itself out of that list and reports almost no
# growth whatever count it was given. Measured that way the answer would
# follow the order the passes ran in; measured in fresh processes it
# follows the count.
#
#   $1  path to the xpost binary
#   $2  path to the source tree root
set -u
xpost=${1:?usage: run-vm-threshold-test.sh <xpost> <srcroot>}
src=${2:?usage: run-vm-threshold-test.sh <xpost> <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
. "$(dirname "$0")/verdict.sh"
guard_require_srcroot "$src"
if [ ! -x "$xpost" ]; then
    echo "FAILURES: the interpreter is not an executable: $xpost"
    exit 1
fi

prog="$src/tests/vm_threshold_test.ps"
if [ ! -r "$prog" ]; then
    echo "FAILURES: the program this runs is not readable: $prog"
    exit 1
fi

# One run under one count. Sets `growth` to the bytes the work grew
# virtual memory by, and answers non-zero if the run did not reach a
# verdict of its own -- a figure printed by a run that then reported a
# failure is a figure taken from a program that did not do what it was
# asked, which is the whole of what verdict.sh is for.
grew() {                        # <count>; sets growth
    _out=$(XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null \
        -o /dev/null "-DTHRESHOLD=$1" "$prog" </dev/null 2>&1)
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$_out" "the suite"
    verdict_ok "$_out" "the run under a count of $1" || return 1
    growth=$(printf '%s\n' "$_out" | sed -n 's/^GREW //p')
    return 0
}

# The counts are far apart on purpose: one large enough that no
# collection falls inside the work, one small enough that many do.
fail=0
growth=
grew 64000000 || fail=1
loose=$growth
grew 1000 || fail=1
tight=$growth
[ "$fail" -eq 0 ] || exit 1
case ${loose:-} in
    ''|*[!0-9-]*)
        echo "FAILURES: the run under the large count reported no growth figure;"
        echo "      the program did not reach the end it prints one from"
        fail=1 ;;
esac
case ${tight:-} in
    ''|*[!0-9-]*)
        echo "FAILURES: the run under the small count reported no growth figure;"
        echo "      the program did not reach the end it prints one from"
        fail=1 ;;
esac
[ "$fail" -eq 0 ] || exit 1

echo "  grew under a large count (64000000): $loose"
echo "  grew under a small count (1000):     $tight"

# A run that never collects has to grow by something, or the work below
# is not making the garbage this is about and neither figure means
# anything.
if [ "$loose" -le 0 ]; then
    echo "FAILURES: the run that should never collect grew by $loose bytes,"
    echo "      so the work makes no garbage and the comparison below would"
    echo "      hold between two numbers that measure nothing"
    fail=1
fi

if [ "$tight" -ge "$loose" ]; then
    echo "FAILURES: the smaller count did not leave less held: $tight against"
    echo "      $loose. A count that paces collection would collect more often"
    echo "      under the smaller one and be left holding less"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "SUCCESS (a smaller count left $tight held against $loose)"
exit $fail
