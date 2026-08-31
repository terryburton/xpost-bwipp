#!/bin/sh
# Meson test wrapper: a restore back past a setpagedevice makes the retired
# device current again, and painting on it must not follow its released
# buffer (device_restore_retired_test.ps), run against the lifetime
# roster of tests/device-fleet.sh.
#
# The devices that keep their raster in a malloc'd buffer are the ones that
# can follow a cleared handle here; the rest are run because the rule is the
# same for all of them and the sequence is an ordinary page-size change.
#
#   $1  path to the built xpost binary
#   $2  path to device_restore_retired_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

ns=$(sandbox_flag "$xpost")

verdict_workdir
devices=$DEVICE_FLEET_LIFETIME
fail=0
ran=0

# This wrapper runs two scripts, and one of them asks its question by
# painting a mark and reading it back. A device that reports no pixel
# back cannot be asked that: it answers the page's ground wherever it
# holds no pixel, which is what the contract has a device answer, and
# what a device holding the page as the marks that made it answers
# everywhere. Such a device says so and stands down.
#
# Which devices those are is read off the runs rather than assumed, and
# held to the list below: a device that has quietly stopped reporting
# its pixels says exactly what one that never could says, and would take
# itself out of the check with nothing saying so. The script that asks
# no readback question has every device silent on both counts, and the
# list is then not held -- so the two scripts share this wrapper without
# sharing an answer neither of them owes.
#
#   record  keeps the marks a page made rather than the pixels they
#           cover, so a read of it answers the ground until those marks
#           are played into a device that paints.
NO_READBACK='record'
readback_quiet=
readback_seen=

# A roster that skipped from end to end leaves the loop having asked
# nothing and every verdict untaken, which reads exactly as a roster
# that answered. The floor is the roster less what a build may not have
# the library for.
floor=0
for dev in $devices; do
    case " $DEVICE_FLEET_OPTIONAL " in *" $dev "*) continue ;; esac
    floor=$((floor + 1))
done

for dev in $devices; do
    # The question here is what the device holds and whether it survives
    # a restore, and it is asked by reading a pixel back. A device that
    # bands is asked for the page whole: a record holds no pixel and
    # answers the ground, which would read as a device that lost its
    # raster rather than as one that never had one.
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
    if printf '%s\n' "$out" | grep -q '^NOREADBACK$'; then
        readback_quiet="$readback_quiet $dev"
        readback_seen=yes
    fi
    if printf '%s\n' "$out" | grep -q '^READBACK$'; then
        readback_seen=yes
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
done

want=$(printf '%s\n' $NO_READBACK | grep . | sort | tr '\n' ' ')
got=$(printf '%s\n' $readback_quiet | grep . | sort | tr '\n' ' ')
if [ -n "$readback_seen" ] && [ "$want" != "$got" ]; then
    for dev in $got; do
        case " $want " in
            *" $dev "*) ;;
            *) echo "FAILURES: $dev reported no pixel back, and nothing here"
               echo "      says it cannot; the raster question stopped being"
               echo "      asked of it" ;;
        esac
    done
    for dev in $want; do
        case " $got " in
            *" $dev "*) ;;
            *) echo "FAILURES: $dev reported its pixels back, and it is named"
               echo "      here as a device that cannot" ;;
        esac
    done
    fail=1
fi

rm -rf "$work"
if [ "$ran" -lt "$floor" ]; then
    echo "FAILURES: $ran of the roster answered and $floor of it is made"
    echo "      without an optional library; the rest said they were not"
    echo "      built in, which is a build to fix rather than a run to pass"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a device retired by setpagedevice did not survive a restore"
    exit 1
fi
echo "SUCCESS ($ran devices)"
exit 0
