#!/bin/sh
# An external interrupt request (SIGINT) raises the PostScript
# interrupt error at the next evaluation step: errordict's handler
# executes stop, the job unwinds, and the interpreter exits instead of
# spinning. POSIX-only: Windows delivers console breaks differently.
#   $1  path to the built xpost binary
#   $2  path to interrupt_test.ps
set -u
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) echo "skipped: no POSIX SIGINT delivery"; exit 77;;
esac
xpost=$1
prog=$2
. "$(dirname "$0")/verdict.sh"
out=$(mktemp)
pid=

# The program under test loops until something interrupts it, so an
# interpreter this script started and did not end goes on running by
# itself -- reparented, spinning a processor, for as long as the machine
# is up. That is what happens when the run this test belongs to is ended
# from outside, which the test cannot detect but can provide for: the
# interpreter goes when the script does, however the script goes.
cleanup() {
    [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    rm -f "$out"
}
trap cleanup EXIT HUP INT TERM

# The interpreter is left running: whether it started is answered by the
# loop below, which waits for it to say so and reports if it never does.
# A status read here would be the status of backgrounding it, which is
# always zero.
"$xpost" -q -d null "$prog" </dev/null >"$out" 2>&1 &
pid=$!

# wait for the program to announce it is inside the loop.
#
# The bound is here to stop a hang, not to time the interpreter: the wait
# ends as soon as the program says it has started or the process is gone,
# so a bound far above what a run needs costs a quick machine nothing and
# keeps a slow one -- an interpreter under a memory checker starts tens of
# times slower -- from being read as a program that never ran.
i=0
while [ $i -lt 1200 ]; do
    grep -q START "$out" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || break
    i=$((i+1)); sleep 0.1
done
grep -q START "$out" || { echo "FAIL: program never reached the loop"; cat "$out"; exit 1; }

kill -INT "$pid"

# the interpreter must exit by itself. The bound is generous for the same
# reason as the one above: what is being held is that the interpreter
# leaves at all, not how quickly.
i=0
while [ $i -lt 1200 ]; do
    kill -0 "$pid" 2>/dev/null || break
    i=$((i+1)); sleep 0.1
done
if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid"
    echo "FAIL: still running after SIGINT"; cat "$out"; exit 1
fi
# the status the interpreter left, which is where it is knowable: a
# status read at the point of backgrounding it is the status of
# backgrounding, and is always zero. The interpreter unwound the job and
# left of its own accord, so it left the way a finished job leaves.
wait "$pid" 2>/dev/null
status=$?
verdict_run "$status" "$(cat "$out")" "the interrupted job" || { cat "$out"; exit 1; }

# stop unwound the job: nothing after the loop may have run
if grep -q AFTER "$out"; then
    echo "FAIL: execution continued past the interrupted loop"; cat "$out"; exit 1
fi
echo "SUCCESS"
