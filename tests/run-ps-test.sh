#!/bin/sh
# Meson test wrapper: run the PLRM-example conformance suite (tests/interpreter_test.ps)
# in the freshly built interpreter and pass iff it reports SUCCESS -- i.e.
# the suite's internal failcount reached zero.
#   $1  path to the built xpost binary
#   $2  path to test.ps
#   $3  optional: a name=value definition to hand the suite, as any
#       caller would hand one in. A suite that needs to know something
#       about the run it is part of -- where it was started from, say --
#       is told by whoever started it, since what the interpreter itself
#       settled about the run is kept where a program cannot read it.
#
# Two things a test written for this harness has to get right, both of
# which fail by reporting success rather than by reporting anything:
#
#   A verdict reached inside save/restore cannot be recorded there.
#   restore reverts local VM, and the failcount lives in userdict, so a
#   failure detected between a save and its restore is erased before it
#   can be printed -- the run reports SUCCESS with the failing assertion
#   already forgotten. Leave the verdict on the operand stack instead,
#   where a boolean survives the restore, and judge after restoring.
#
#   A test that reaches into an internal dictionary must fail when it
#   cannot find what it is looking for, not skip. A lookup guarded by
#   `known` that falls back to an empty result turns a moved member into
#   a differently-shaped test that still passes, or -- worse -- into a
#   run whose exemptions have quietly vanished and whose failures are
#   therefore inventions. Both have happened here.
set -u
xpost=$1
script=$2
define=${3:-}
# Optional extra interpreter flags a suite needs, empty for almost all of
# them. op_context_test.ps asks for --enable-dps here because the Display
# PostScript context operators it exercises are installed only on that
# opt-in; passing it as an argument keeps the shared runner unchanged for
# every suite that does not.
extra=${4:-}
. "$(dirname "$0")/verdict.sh"
# these conformance tests exercise the interpreter's own file operations, so
# run with the CLI file-access sandbox lifted
# capture the interpreter's exit status as well as its output: a run that
# reports SUCCESS and then dies during teardown -- a crash, an assertion,
# a sanitizer abort -- must not be recorded as a pass
# The shared framework (tests/testlib.ps) is prepended to a suite whose
# first line asks for it, and to no other. It has to be asked for: some
# suites here assert that a program's own dictionary starts empty or count
# the dictionary stack, and a framework defined into userdict for one of
# those would be the thing it reports -- and six read their own source
# through currentfile, which prepending would shift. So the opt-in is a
# marker in the file, where a reader of the file can see it, rather than a
# list kept over here.
# The marker is honoured in tests/testlib-prepend.sh, which every runner
# here goes through -- this one is not the only script that runs a suite.
verdict_workdir
. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$script" "$work"
run=$testlib_run
# A definition is passed only when one was asked for: every other suite
# here asserts that a program's own dictionary starts empty, and a
# definition made for one of them would be the thing that filled it.
if [ -n "$define" ]; then
    out=$("$xpost" -q --no-sandbox $extra -d null "-D$define" "$run" </dev/null 2>&1)
else
    out=$("$xpost" -q --no-sandbox $extra -d null "$run" </dev/null 2>&1)
fi
status=$?
printf '%s\n' "$out"
if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    exit 1
fi
# a suite that cannot ask its question in this build -- one whose text
# a face answers, under a build carrying no face library -- says so and
# is a skip, not a pass and not a failure
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the suite"
