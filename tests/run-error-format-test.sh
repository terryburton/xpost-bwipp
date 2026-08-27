#!/bin/sh
# Meson/make-check wrapper: a top-level error in a job reports in the standard
# Adobe form -- the error line
#   %%[ Error: NAME; OffendingCommand: CMD ]%%
# (note the space before the closing bracket) followed by the flush notice
#   %%[ Flushing: rest of job (to end-of-file) will be ignored ]%%
# while a clean quit or a self-caught error reports neither. The two lines are
# held to the byte, being what a job's consumer parses.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"
tmp=${TMPDIR:-/tmp}/errfmt-$$
trap 'rm -f "$tmp".err.ps "$tmp".ok.ps "$tmp".caught.ps "$tmp".cascade.ps "$tmp".badreport.ps "$tmp".vmerror.ps "$tmp".widecycle.ps "$tmp".widecycle.out' EXIT INT TERM

# 1. a top-level undefined error: the error line (with its trailing space)
#    and then the flush notice
printf 'mistypedname\n' > "$tmp".err.ps
out=$("$xpost" -q --no-sandbox -d null "$tmp".err.ps </dev/null 2>&1)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -Fq '%%[ Error: undefined; OffendingCommand: mistypedname ]%%' || exit 1
printf '%s\n' "$out" | grep -Fq '%%[ Flushing: rest of job (to end-of-file) will be ignored ]%%' || exit 1

# 2. a clean job that quits normally: no flush notice
#    A job the interpreter completes is judged as a completed run: it
#    leaves a zero status and says nothing about a failure of its own.
#    Cases 1 and 5 are the jobs whose whole point is that they do not
#    complete, and case 4 below is where their statuses are required.
printf '(ok) = quit\n' > "$tmp".ok.ps
out=$("$xpost" -q --no-sandbox -d null "$tmp".ok.ps </dev/null 2>&1)
verdict_run "$?" "$out" "the clean job" || exit 1
printf '%s\n' "$out" | grep -Fq 'Flushing' && exit 1

# 3. a job that catches its own error and completes: no flush notice
printf '{ oops } stopped pop (done) = quit\n' > "$tmp".caught.ps
out=$("$xpost" -q --no-sandbox -d null "$tmp".caught.ps </dev/null 2>&1)
verdict_run "$?" "$out" "the self-caught job" || exit 1
printf '%s\n' "$out" | grep -Fq 'Flushing' && exit 1
printf '%s\n' "$out" | grep -Fq 'done' || exit 1

# 4. process exit status: an uncaught error is a failed job; a clean
#    job and a job that catches its own error succeed
"$xpost" -q --no-sandbox -d null "$tmp".err.ps </dev/null >/dev/null 2>&1 && exit 1
"$xpost" -q --no-sandbox -d null "$tmp".ok.ps </dev/null >/dev/null 2>&1 || exit 1
"$xpost" -q --no-sandbox -d null "$tmp".caught.ps </dev/null >/dev/null 2>&1 || exit 1

# 5. a runaway error cascade -- an errordict handler that itself raises an
#    error, so recovery never reaches `stop` -- must abort the job cleanly
#    rather than spin until VM exhaustion. The meson/make-check timeout
#    bounds the run, so a failure to abort surfaces as a test timeout.
printf 'errordict /undefinedresult { 1 0 div } put 1 0 div\n' > "$tmp".cascade.ps
out=$("$xpost" -q --no-sandbox -d null "$tmp".cascade.ps </dev/null 2>&1)
rc=$?
printf '%s\n' "$out" | grep -Fq 'runaway error cascade' || exit 1
[ "$rc" -ne 0 ] || exit 1

# 6. an error whose report does not finish. handleerror is the program's
#    to replace and the standard one asks for names, strings and
#    dictionaries a run at an implementation limit may not have to give,
#    so the report is a place an error can be raised. The job still ends
#    with the flush notice, and says the report ahead of it stopped
#    short: a report that tails off with nothing to mark the end reads as
#    the whole of what the interpreter had to say.
printf 'errordict /handleerror { thishandlerisbroken } put\nmistypedname\n' \
    > "$tmp".badreport.ps
out=$("$xpost" -q --no-sandbox -d null "$tmp".badreport.ps </dev/null 2>&1)
rc=$?
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -Fq '%%[ Report incomplete: reporting this error raised another ]%%' || exit 1
printf '%s\n' "$out" | grep -Fq '%%[ Flushing: rest of job (to end-of-file) will be ignored ]%%' || exit 1
[ "$rc" -ne 0 ] || exit 1

# 7. the standard report of a VMerror finishes. That error records no
#    stack snapshots (PLRM 8.2: its default handler, alone among them,
#    does not snapshot the stacks), so it is the one error whose report
#    has nothing to print under the stack headings -- and a report that
#    prints a heading anyway raises inside itself and stops, which the
#    case above cannot tell apart from a handler the program broke on
#    purpose. Asked of the standard handler, so it is the shipped
#    report being held to finishing, and asked by name, so the error is
#    the one under test rather than whichever one the run happens to
#    reach.
printf '(cmd) /VMerror signalerror\n' > "$tmp".vmerror.ps
out=$("$xpost" -q --no-sandbox -d null "$tmp".vmerror.ps </dev/null 2>&1)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -Fq '%%[ Error: VMerror; OffendingCommand: cmd ]%%' || exit 1
printf '%s\n' "$out" | grep -Fq '%%[ Report incomplete' && exit 1
printf '%s\n' "$out" | grep -Fq '%%[ Flushing: rest of job (to end-of-file) will be ignored ]%%' || exit 1

# 8. the report of an error raised with a structure on the operand stack
#    that cannot be printed in full. The report prints every stack entry
#    in re-readable form, so what it costs is whatever walking those
#    entries costs -- and an array that refers to itself twice is
#    re-expanded down both branches at every level of the descent, once
#    per element, so a wide one is a finite walk of astronomical length
#    from a program of four lines. Nothing about it is unusual to reach:
#    the array is on the stack because the error was raised there.
#    Bounding the descent is not enough, since a single array entered
#    costs as much as it is long; what has to be bounded is the printing
#    itself. The run is held to a wall-clock limit rather than left to
#    the harness, because the failure being guarded against is a report
#    that does not end, and a test that hangs reports nothing.
printf '/o 2000 array def 0 1 1999 { o exch 0 put } for\no 0 o put o 1 o put\no 1 add\n' \
    > "$tmp".widecycle.ps
run_limited 30 "$xpost" -q --no-sandbox -d null "$tmp".widecycle.ps \
    </dev/null > "$tmp".widecycle.out 2>&1
rc=$?
case $rc in
    124 | 137)
        echo "FAILURES: the report of an error holding a self-referential array"
        echo "      did not finish inside the limit"
        exit 1 ;;
esac
grep -Fq '%%[ Error: typecheck; OffendingCommand: add ]%%' "$tmp".widecycle.out || exit 1
grep -Fq '%%[ Flushing: rest of job (to end-of-file) will be ignored ]%%' \
    "$tmp".widecycle.out || exit 1
[ "$rc" -ne 0 ] || exit 1
# Finishing is not on its own the property wanted: a walk bounded only by
# where it stops descending finishes too, after writing more than anyone
# will read. The report of this error runs to about a hundred and thirty
# kibibytes; a megabyte is far above that and far below what an unbounded
# one reaches.
sz=$(wc -c < "$tmp".widecycle.out)
if [ "$sz" -gt 1048576 ]; then
    echo "FAILURES: the report of an error holding a self-referential array"
    echo "      ran to $sz bytes"
    exit 1
fi

exit 0
