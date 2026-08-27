#!/bin/sh
#
# Every build configuration raises the test time limit off meson's
# default, and the raising is not optional.
#
# A limit that a test does not name is thirty seconds, which is a number
# meson chose knowing nothing about this suite. Two hundred and eighty
# tests lean on it, and a gate runs on a machine doing other things: with
# the cores subscribed twice over, the longest of those has been measured
# at twenty-nine seconds -- and two guards that used to lean on it
# reached the multiplied limit exactly and were killed there. A test
# killed by a clock reports the same red as a test that found a defect,
# and a red that is not a defect teaches a reader to discount red.
#
# So no configuration is left on the bare default: each carries a test
# setup that multiplies it, and the setup is the default one so that a
# caller who types `meson test` with no arguments gets it too. Which
# multiplier belongs to which configuration is a judgement recorded in
# meson.build beside the setups; what this holds is that a configuration
# cannot come to have none, because a deleted branch here costs nothing
# visible -- the suite goes on passing on an idle machine and starts
# failing at random on a busy one, which is the shape that reads as
# flakiness rather than as a regression.
#
# The multiplier is the floor and not the whole answer. A test whose own
# cost has grown to where the multiplied default is no longer a margin
# names a limit of its own in meson.build, and the guards that read the
# whole of a family's sources are where that has been needed.
#
# Read out of meson.build rather than listed here, so a configuration
# added tomorrow is inside the rule the day it is added.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-test-timeouts.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$src/meson.build" "the build description"

guard_workdir
guard_mirror timeouts "$src/meson.build"
meson="$mirror/meson.build"

fail=0

# ---- every setup multiplies, and one of them is the default ----
#
# Counted off the registrations rather than assumed: a setup added
# without a multiplier is a configuration back on the bare default with
# a line in the file suggesting otherwise.
setups=$(grep -c '^  *add_test_setup(' "$meson")
if [ "$setups" -lt 2 ]; then
    echo "FAIL: meson.build registers $setups test setup(s); the sanitized"
    echo "      and plain configurations each need one, or one of them runs"
    echo "      on meson's thirty-second default"
    fail=1
fi

mult=$(grep -c '^  *timeout_multiplier:' "$meson")
if [ "$mult" -ne "$setups" ]; then
    echo "FAIL: meson.build registers $setups test setup(s) and names"
    echo "      $mult timeout multiplier(s); a setup without one leaves its"
    echo "      configuration on the default it was added to raise"
    fail=1
fi

dflt=$(grep -c '^  *is_default: true)*' "$meson")
if [ "$dflt" -ne "$setups" ]; then
    echo "FAIL: $dflt of $setups test setup(s) are the default one; a setup"
    echo "      that is not applies only when named on the command line, and"
    echo "      the gate does not name it"
    fail=1
fi

# ---- the branch that carries them covers both configurations ----
#
# The sanitized and plain cases are the two arms of one condition. An
# arm removed leaves its configuration with no setup at all, which the
# counts above cannot see: they count what is written, not what is
# reachable.
if ! grep -q "^if not san.contains('none')" "$meson"; then
    echo "FAIL: meson.build no longer selects a test setup on whether the"
    echo "      build carries a sanitizer; the shape this guard reads for"
    echo "      has changed, so fix the guard"
    fail=1
elif ! awk "/^if not san.contains\('none'\)/,/^endif/" "$meson" | grep -q '^else$'; then
    echo "FAIL: the test-setup condition in meson.build has no else branch,"
    echo "      so a build without a sanitizer registers no setup and runs"
    echo "      on meson's thirty-second default"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: the test time limits do not hold"
    exit 1
fi

echo "SUCCESS ($setups test setups, each a default one naming a multiplier)"
exit 0
