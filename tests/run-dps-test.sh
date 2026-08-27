#!/bin/sh
# The Display PostScript context operators (fork/join/yield/detach/
# currentcontext, PLRM 2nd ed 7.1) are not standard base PostScript and the
# cooperative scheduler that would drive them is not yet run, so they are
# installed only when a run opts in with --enable-dps. This exercises the
# switch in both positions:
#   - without the flag the names are undefined, so nothing pretends to be a
#     working operator;
#   - with the flag the operators are installed and validate their context
#     identifier: joining the current context (or any invalid identifier)
#     raises invalidcontext, which the interpreter did not previously carry,
#     while a currentcontext still yields a context and a valid detach is
#     accepted.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"
verdict_workdir

# Without the flag, the operators are not installed.
cat > "$work/hidden.ps" <<'PS'
/fork where { pop (FORK-DEFINED\n) }{ (FORK-HIDDEN\n) } ifelse print
/join where { pop (JOIN-DEFINED\n) }{ (JOIN-HIDDEN\n) } ifelse print
/currentcontext where { pop (CC-DEFINED\n) }{ (CC-HIDDEN\n) } ifelse print
flush
PS
out=$(run_limited 10 "$xpost" -q --no-sandbox -d null "$work/hidden.ps" </dev/null 2>&1)
st=$?
verdict_run "$st" "$out" "the default (no --enable-dps) run" || exit 1
for want in FORK-HIDDEN JOIN-HIDDEN CC-HIDDEN; do
    case $out in
        *"$want"*) ;;
        *) echo "FAIL: default run did not report $want; got: $out"; exit 1 ;;
    esac
done
case $out in
    *-DEFINED*) echo "FAIL: a context operator is installed without --enable-dps: $out"; exit 1 ;;
esac
echo "context operators are hidden by default"

# With the flag, they are installed and validate the context identifier.
cat > "$work/enabled.ps" <<'PS'
/fork where { pop (FORK-INSTALLED\n) }{ (FORK-MISSING\n) } ifelse print
% joining the current context is invalidcontext (PLRM 2nd ed 7.1)
{ currentcontext join } stopped
    { $error /errorname get /invalidcontext eq
        { (SELFJOIN-INVALIDCONTEXT\n) }{ (SELFJOIN-WRONGERR\n) } ifelse }
    { (SELFJOIN-NOERROR\n) } ifelse print
% currentcontext still yields a context object
currentcontext type /contexttype eq
    { (CC-YIELDS-CONTEXT\n) }{ (CC-WRONGTYPE\n) } ifelse print
% detaching a valid, freshly forked context is accepted (not invalidcontext)
mark {} fork
{ detach } stopped { (DETACH-ERRORED\n) }{ (DETACH-OK\n) } ifelse print
% fork runs the child cooperatively and join returns the result it left
mark 2 3 { add } fork join
counttomark 1 eq { 5 eq }{ false } ifelse
    { (FORKJOIN-5\n) }{ (FORKJOIN-WRONG\n) } ifelse print
cleartomark
flush
PS
out=$(run_limited 10 "$xpost" -q --no-sandbox --enable-dps -d null "$work/enabled.ps" </dev/null 2>&1)
st=$?
verdict_run "$st" "$out" "the --enable-dps run" || exit 1
for want in FORK-INSTALLED SELFJOIN-INVALIDCONTEXT CC-YIELDS-CONTEXT DETACH-OK FORKJOIN-5; do
    case $out in
        *"$want"*) ;;
        *) echo "FAIL: --enable-dps run did not report $want; got: $out"; exit 1 ;;
    esac
done
echo "context operators install and validate the context identifier under --enable-dps"

# A job's execution contexts do not outlive the job.
#
# The job boundary winds both banks back to a fixed image (PLRM 3.7.7). A
# context the job forked holds stacks and object roots that are entities of
# that virtual memory, so one left standing over the revert would be handed
# the next job's run and would execute an execution stack that is no longer
# there. The second job of the stream below is what says whether it was:
# with the contexts of the first job ended at the boundary the second job
# runs and reports; with one still runnable the scheduler gives the run to
# it instead and the second job never happens.
job1=$(printf '(JOB1\n) print flush\nmark { 1 1 1000000 { pop yield } for } fork pop\n(JOB1-FORKED\n) print flush\n')
job2=$(printf '(JOB2\n) print flush\n/a 100 dict def 0 1 50 { a exch 200 string put } for\n(JOB2-DONE\n) print flush\n')
{ printf '%s\n\004' "$job1"; printf '%s\n\004' "$job2"; } > "$work/stream.ps"
out=$(run_limited 20 "$xpost" -q --no-sandbox --enable-dps --jobserver -d null \
        < "$work/stream.ps" 2>&1)
st=$?
verdict_run "$st" "$out" "the job stream with a context left running" || exit 1
for want in JOB1 JOB1-FORKED JOB2 JOB2-DONE; do
    case $out in
        *"$want"*) ;;
        *) echo "FAIL: the job stream did not report $want; got: $out"; exit 1 ;;
    esac
done
echo "a job's contexts do not outlive the job"

echo "run-dps-test: ok"
exit 0
