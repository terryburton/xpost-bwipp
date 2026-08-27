#!/bin/sh
# Meson test wrapper: a page the device cannot build must leave the
# interpreter able to build the next one (oversize_page_test.ps), run
# against the marking roster of tests/device-fleet.sh.
#
# The question is about the page a device builds, so it goes to the
# roster that marks. The devices whose page is the interpreter's own
# virtual memory answer the whole of it; the rest answer the part that
# does not depend on the raster being virtual memory, and the test picks
# which by the key the device carries rather than by its name.
#
#   $1  path to the built xpost binary
#   $2  path to oversize_page_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

if "$xpost" -h 2>/dev/null | grep -q -- '--no-sandbox'; then
    ns='--no-sandbox'
else
    ns=''
fi

verdict_workdir
devices=$DEVICE_FLEET_MARKING
fail=0
ran=0
accounted=0

# A roster that skipped from end to end leaves the loop having asked
# nothing and every verdict untaken, which reads exactly as a roster that
# answered. The floor is the roster less what a build may not have the
# library for.
floor=0
for dev in $devices; do
    case " $DEVICE_FLEET_OPTIONAL " in *" $dev "*) continue ;; esac
    floor=$((floor + 1))
done

for dev in $devices; do
    # The device holding the whole page, asked for as the mode that says
    # so. Selecting a device by name selects the record in front of it,
    # which holds a band: a band is one band whatever the page is, so
    # there is no ceiling for the refusal to find and none of the roster
    # would be left keeping its page where the cost of one can be
    # counted.
    out=$("$xpost" -q $ns -d "$(fleet_whole "$dev")" -o "$work/out.$dev" \
          "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP $dev (not built in)"; continue ;;
    esac
    ran=$((ran + 1))
    if [ "$st" -ne 0 ]; then
        echo "FAIL $dev: the interpreter exited with status $st"
        printf '%s\n' "$out" | tail -3
        fail=1
        continue
    fi
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
    if verdict_ok "$out" "$dev"; then
        echo "OK   $dev"
    else
        fail=1
    fi

    # Whether this device reached the accounting. What a refusal cost
    # against what the ordinary page cost is the whole point of the run,
    # and it is measured only where the page is virtual memory to
    # measure; a run that reached it nowhere asserts that nothing errored
    # and stops there.
    case $(printf '%s\n' "$out" | sed -n 's/^RASTER //p' | head -1) in
        yes) accounted=$((accounted + 1)) ;;
        no)  ;;
        *)   echo "FAIL $dev: said nothing about where it keeps its page"
             fail=1 ;;
    esac
done

rm -rf "$work"
# The devices that kept their page where this run could account for it.
# Three classes hold a page as rows of the interpreter's own virtual
# memory and each must reach the comparison; move a page out of virtual
# memory and the measurement that names this test would stop being made
# without anything saying so.
ACCOUNTED_FLOOR=3
if [ "$accounted" -lt "$ACCOUNTED_FLOOR" ]; then
    echo "FAILURES: $accounted of the roster kept its page where the cost of"
    echo "      a refusal could be counted, and $ACCOUNTED_FLOOR of it does;"
    echo "      the comparison this test is for was made $accounted time(s)"
    exit 1
fi
if [ "$ran" -lt "$floor" ]; then
    echo "FAILURES: $ran of the roster's devices answered, and $floor of them"
    echo "      are made without an optional library; the rest said they were"
    echo "      not built in, which is a build to fix rather than a run to pass"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page the device could not build did not leave the"
    echo "      interpreter able to build the next one"
    exit 1
fi
echo "SUCCESS ($ran devices)"
exit 0
