#!/bin/sh
#
# Everything the specification entitles a program to is still there.
#
# This is the counterweight to the reachability registers. Those count what a
# program must not reach, and every one of them improves by taking something
# away; none of them notices when something a program is entitled to goes with
# it. A tree hardened until it refused its own users would pass all of them
# and this one alone would say so.
#
# Run WITHOUT a census, because a shipped run is the one a program meets.
#
#   $1  path to the source tree root
#   $2  path to the built xpost binary
set -u
src=${1:?usage: check-plrm-entitlements.sh <srcroot> <xpost>}
xpost=${2:?usage: check-plrm-entitlements.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
probe="$src/tests/plrm_entitlements_test.ps"
guard_require_file "$probe" "the entitlements probe"

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
    echo "everything the specification entitles a program to is reachable: SUCCESS"
    exit 0
fi

echo "FAILURES: the interpreter has been closed further than the"
echo "      specification allows -- a program is refused something PLRM"
echo "      gives it:"
grep -a '^FAIL: ' "$work/out" | sed 's/^/      /' | head -10
exit 1
