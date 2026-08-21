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
out=$(timeout 10 "$xpost" -q --no-sandbox -d null "$work/hidden.ps" </dev/null 2>&1)
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
out=$(timeout 10 "$xpost" -q --no-sandbox --enable-dps -d null "$work/enabled.ps" </dev/null 2>&1)
st=$?
verdict_run "$st" "$out" "the --enable-dps run" || exit 1
for want in FORK-INSTALLED SELFJOIN-INVALIDCONTEXT CC-YIELDS-CONTEXT DETACH-OK FORKJOIN-5; do
    case $out in
        *"$want"*) ;;
        *) echo "FAIL: --enable-dps run did not report $want; got: $out"; exit 1 ;;
    esac
done
echo "context operators install and validate the context identifier under --enable-dps"

echo "run-dps-test: ok"
exit 0
