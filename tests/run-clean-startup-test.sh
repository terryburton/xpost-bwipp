#!/bin/sh
# Meson test wrapper: the interpreter must start and run a trivial job
# without emitting any diagnostic of its own. A self-check that reports
# on every ordinary run is worse than no self-check -- it trains the
# reader to ignore the channel real errors arrive on -- so a clean run
# must be silent on stderr.
#
# It must be silent on stdout too, and for a different reason. That
# channel is the job's: what the program prints, and the page stream a
# job writing one puts there. Anything of the interpreter's arriving on
# it is interleaved with the job's own output and corrupts it, and a
# caller reading the run for its answer has to know what to strip before
# it can read anything -- which is a thing to get wrong today and a
# thing to get wrong again whenever the interpreter's own output
# changes. So both channels are read, and both must be empty.
#
# A job that shows a page is asked as well as one that does not. Showing
# a page is where the interpreter has something of its own to say at a
# page boundary, and it says it to a person at a terminal; there is
# nobody at a terminal here. It is asked only of the runs that loaded
# graphics, showpage being one of the operators a run without them does
# not have.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

ns=$(sandbox_flag "$xpost")

verdict_workdir
printf '1 2 add pop\n' > "$work/t.ps"
printf '%%!PS\nshowpage\n' > "$work/page.ps"

fail=0
for mode in "" "--no-graphics"; do
    jobs='t page'
    [ -z "$mode" ] || jobs=t
    for job in $jobs; do
        # the two channels are taken separately so that each is judged on
        # its own: a run folding them together cannot say which one said
        # something, and the answer owed differs between them only in the
        # reason, not in the amount
        err=$("$xpost" -q $ns $mode -d null "$work/$job.ps" </dev/null 2>&1 >/dev/null)
        status=$?
        out=$("$xpost" -q $ns $mode -d null "$work/$job.ps" </dev/null 2>/dev/null)
        who="a quiet run of the $job job${mode:+ ($mode)}"
        verdict_run "$status" "$err" "$who" || exit 1
        if [ -n "$err" ]; then
            echo "FAIL: $who wrote to stderr:"
            printf '%s\n' "$err" | head -5
            fail=1
        fi
        if [ -n "$out" ]; then
            echo "FAIL: $who wrote to stdout, which is the job's channel:"
            printf '%s\n' "$out" | head -5
            fail=1
        fi
    done
done

rm -rf "$work"
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: startup is not silent"
    exit 1
fi
echo SUCCESS
exit 0
