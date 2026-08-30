#!/bin/sh
# Meson test wrapper: what a program can make the machinery run.
#
# The suite plants a procedure of the program's own everywhere a program
# can reach and write, drives the machinery, and reports the places that
# ran it. This holds that count to the register, which names each pair
# that still diverts and what would close it.
#
# The count is a ratchet: it may fall and it may not rise. A pair that
# stops diverting is a hole closed, and the register is rewritten in the
# commit that closes it.
#   $1  path to the source tree root
#   $2  path to the built xpost binary
set -u
src=${1:?usage: check-machinery-diverted.sh <srcroot> <xpost>}
xpost=${2:?usage: check-machinery-diverted.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
golden="$src/tests/machinery_diverted.golden"
guard_require_file "$golden" "the register of what diverts the machinery"
guard_require_file "$src/tests/machinery_diverted_test.ps" "the suite"

out=$(XPOST_DATA_DIR="$src/data" XPOST_CENSUS=1 "$xpost" -q -d pgm -o /dev/null \
        "$src/tests/machinery_diverted_test.ps" </dev/null 2>/dev/null)
status=$?
if [ "$status" -ne 0 ]; then
    echo "FAILURES: the suite exited with status $status"
    printf '%s\n' "$out" | sed 's/^/      /' | head -12
    exit 1
fi

have=$(printf '%s\n' "$out" | awk '/^machinery-diverted places: /{print $3}')
want=$(awk '/^diverted [0-9]+$/{print $2; exit}' "$golden")
took=$(printf '%s\n' "$out" | awk '/places that took the planting: /{print $NF}')

# A run that could plant nowhere would report no diversion just as
# quietly as one that found none.
case ${have:-} in
    ''|*[!0-9]*) echo "FAILURES: the suite reported no count"; exit 1 ;;
esac
# A run that could plant nowhere would report no diversion just as
# quietly as one that found none, so what it managed to plant is read
# rather than what it was refused: the second scales with the entry
# points driven and says nothing about whether the question was asked.
if [ "${took:-0}" -lt 1 ]; then
    echo "FAILURES: no place took the planting, so the run found nothing"
    echo "      it could have diverted the machinery from"
    exit 1
fi

if [ "$have" -gt "$want" ]; then
    echo "FAIL: $have places divert the machinery into running a"
    echo "      procedure a program supplied, where the register allows"
    echo "      $want. The ones it allows are named in $golden, each"
    echo "      with what would close it; this is not one of those."
    printf '%s\n' "$out" | grep '^DIVERTED: ' | sed 's/^/      /'
    exit 1
fi
if [ "$have" -lt "$want" ]; then
    echo "NOTE: $have places divert where the register allows $want."
    echo "      Say in the commit which pair closed and why, and write"
    echo "      'diverted $have' into $golden."
    exit 1
fi
echo "SUCCESS ($have place(s) divert the machinery, each named in the register;"
echo "     ${took:-0} took the planting)"
exit 0
