#!/bin/sh
#
# What a program cannot reach, a save and a restore do not hand back.
#
# The lockdown runs at boot, before any save a program can take, so a restore
# has no earlier state to put back. That is the reasoning. This is the test of
# it, because the reasoning has been wrong here before: the record of the
# private dictionary is context state rather than virtual memory precisely so
# that a restore cannot displace it, and nothing but a test says which of the
# two any given thing is.
#
# Run WITHOUT a census: the question is what a shipped run answers, and the
# shared guard preamble asks for one.
#
#   $1  path to the source tree root
#   $2  path to the built xpost binary
set -u
src=${1:?usage: check-reach-survives-restore.sh <srcroot> <xpost>}
xpost=${2:?usage: check-reach-survives-restore.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
probe="$src/tests/reach_survives_restore_test.ps"
guard_require_file "$probe" "the save-and-restore probe"

unset XPOST_CENSUS
guard_workdir

XPOST_DATA_DIR="$src/data" XPOST_NO_VM_IMAGE=1 \
    "$xpost" -q -d null -o /dev/null "$probe" </dev/null > "$work/out" 2>&1

if ! grep -aq '^FAILED ' "$work/out"; then
    echo "FAILURES: the probe did not run to its end, so its silence means"
    echo "      nothing. Its last lines were:"
    tail -6 "$work/out" | sed 's/^/      /'
    exit 1
fi
if grep -aq '^SUCCESS$' "$work/out"; then
    echo "the machinery stays out of reach across save and restore: SUCCESS"
    exit 0
fi

echo "FAILURES: a save and a restore handed back something the machinery"
echo "      keeps out of a program's reach:"
grep -a '^FAIL: ' "$work/out" | sed 's/^/      /' | head -8
exit 1
