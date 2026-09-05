#!/bin/sh
# Meson test wrapper: a device that owns its raster is painted the same
# page whichever way its fill is called.
#
# A device holding its own pixels declares a compiled rectangle fill, and
# the machinery reaches it either by putting the operands on the operand
# stack and going through the operator's own call protocol, or by calling
# the function with the operands it already holds. The second is taken
# where the method's declared shape allows it, which is a property of the
# device and not of the page, so a page must not be able to tell which
# ran.
#
# Nothing about a page says which route made it -- that is the whole
# point of the second one -- so no comparison of one page against a
# recorded page could catch a route that painted differently. Both are
# made here instead, from the one program, and compared to each other.
# XPOST_NODIRECT is what asks for the first route; without it the run
# takes whichever the device allows.
#
# Read on more than one device, because each device carries its own
# compiled fill and folds the operands it is handed its own way. Two
# devices agreeing is two readings of the claim; one device agreeing is
# one, and the fills are not shared code.
#
#   $1  path to the built xpost binary
#   $2  path to a program that paints an image
set -u
xpost=${1:?usage: run-fill-route-test.sh <xpost> <program>}
prog=${2:?usage: run-fill-route-test.sh <xpost> <program>}
. "$(dirname "$0")/verdict.sh"

ns=$(sandbox_flag "$xpost")
verdict_workdir
fail=0
seen=0

for dev in png jpeg; do
    d=$work/direct.$dev
    s=$work/stack.$dev

    out=$("$xpost" -q $ns -d "$dev" -o "$d" "$prog" </dev/null 2>&1)
    st=$?
    verdict_run "$st" "$out" "the $dev run" || fail=1

    out=$(XPOST_NODIRECT=1 "$xpost" -q $ns -d "$dev" -o "$s" "$prog" \
          </dev/null 2>&1)
    st=$?
    verdict_run "$st" "$out" "the $dev run by the other route" || fail=1

    if [ ! -s "$d" ] || [ ! -s "$s" ]; then
        echo "FAILURES: the $dev runs did not both produce a page, so the"
        echo "      comparison below would hold two absences to each other"
        fail=1
        continue
    fi
    seen=$((seen + 1))
    if cmp -s "$d" "$s"; then
        echo "OK   $dev paints the same page by either route"
        echo "     ($(wc -c < "$d" | tr -d ' ') bytes)"
    else
        echo "FAILURES: $dev paints a different page depending on how its"
        echo "      fill was called"
        cmp "$d" "$s" 2>&1 | sed 's/^/      /' | head -2
        fail=1
    fi
done

# Both devices have to have answered, or a run where neither produced a
# page would report nothing wrong.
if [ "$seen" -lt 2 ]; then
    echo "FAILURES: only $seen of 2 devices produced a pair to compare"
    fail=1
fi

verdict_exit
