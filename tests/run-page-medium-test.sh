#!/bin/sh
# Meson test wrapper: what a device makes of a page that is not a whole
# number of its pixels (page_medium_test.ps).
#
# PLRM 6.2 puts the far corner of the page at user space (width, height)
# and gives an ImagingBBox of null the largest box possible for that
# page. Where the page is pixels and the corner is not on one, those two
# cannot both hold, and PLRM 6.2.7 describes both ways out: a medium
# smaller than the page, which clips it, and one at least as large in
# both dimensions, which does not.
#
# Which of those a page gets is asked of the whole roster rather than of
# one device. It is a family question in the strict sense -- what a
# device does with a page it cannot hold exactly is a property of how it
# holds a page at all, and there are two kinds: a device whose page is
# pixels rounds up to whole ones, a device whose page is written as
# points takes it exactly. The script reads which kind it is off the
# running device and holds it to that kind's answer, so a device joining
# either family is held without this wrapper naming it.
#
#   $1  path to the built xpost binary
#   $2  path to page_medium_test.ps
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

fail=0

# The roster less what a build may not have the library for: a roster
# that skipped from end to end would report the same success as one that
# answered everywhere.
floor=0
for dev in $DEVICE_FLEET_ALL; do
    case " $DEVICE_FLEET_OPTIONAL " in *" $dev "*) continue ;; esac
    floor=$((floor + 1))
done

one_device() {
    dev=$1
    out=$("$xpost" -q $ns -d "$dev" -o "$work/out.$dev" "$script" \
          </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "$dev: not built in; not asked"; return 2 ;;
    esac
    printf '%s\n' "$out" | sed "s/^/$dev: /"
    verdict_run "$st" "$out" "the page-medium job on $dev" || return 1
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
    verdict_ok "$out" "the page-medium check on $dev" || return 1
    return 0
}

fleet_each one_device $DEVICE_FLEET_ALL || fail=1

if [ "$fleet_asked" -lt "$floor" ]; then
    echo "FAILURES: $fleet_asked of the roster answered and $floor of it is"
    echo "      made without an optional library"
    exit 1
fi

[ "$fail" -eq 0 ] || exit 1
echo "page-medium: held on $fleet_asked device(s)"
echo SUCCESS
