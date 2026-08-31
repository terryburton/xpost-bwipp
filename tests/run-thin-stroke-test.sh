#!/bin/sh
# Meson test wrapper: run the thin-stroke coverage check
# (thin_stroke_test.ps) against the devices that mark pixels.
#
# The check asks a device what it painted, so the null device the plain
# wrapper uses has nothing to answer with. It runs instead against the
# marking roster of tests/device-fleet.sh, and a device on that roster
# which cannot report its own pixels back says so and is counted rather
# than passing quietly: a roster that stopped reporting would otherwise
# reduce what is asserted with nothing saying it had.
#
#   $1  path to the built xpost binary
#   $2  path to thin_stroke_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

ns=$(sandbox_flag "$xpost")

verdict_workdir

fail=0
asked=0
skipped=0

for dev in $DEVICE_FLEET_MARKING; do
    out=$("$xpost" -q $ns -d "$dev" -o "$work/out.$dev" "$script" </dev/null 2>&1)
    st=$?
    printf '%s\n' "$out" | sed "s/^/$dev: /"
    verdict_run "$st" "$out" "the thin-stroke job on $dev" || { fail=1; continue; }
    case "$out" in
        *SKIP:*) skipped=$((skipped + 1)); continue ;;
    esac
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
    verdict_ok "$out" "the thin-stroke check on $dev" || fail=1
    asked=$((asked + 1))
done

echo "thin-stroke: asked $asked device(s), $skipped could not report their pixels"

# The whole roster declining to answer is the shape this test fails
# silently in: every device skipping reports no failure and holds nothing.
if [ "$asked" -eq 0 ]; then
    echo "FAILURES: no device on the marking roster reported its own pixels,"
    echo "      so the coverage rule was not held against anything"
    exit 1
fi

verdict_exit
