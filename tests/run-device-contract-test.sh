#!/bin/sh
# Meson test wrapper: run the device-method contract check
# (device_contract_test.ps) against the marking roster of
# tests/device-fleet.sh, one device per marking implementation.
# The test feeds each device method its boundary inputs (degenerate,
# inverted, fractional, out-of-range) and requires no errors and an
# emitted page; on a device that reports its own pixels back it also
# asserts what the marking methods painted. Window devices need a
# display: xcb runs under a virtual one (xvfb-run) when the host
# provides it, gdi is not run.
#
# The behaviour tier skips itself where it cannot see the raster, so a
# run in which it skipped everywhere would still pass. The devices that
# cannot witness it are named below rather than counted: a count says
# how many went quiet and a name says which, and a device that starts
# answering is held to the tier it was excused from.
#
#   $1  path to the built xpost binary
#   $2  path to device_contract_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

# The members of the marking roster that report no pixels back, with the
# reason each cannot: null paints nothing and bbox records a page's
# extent rather than its pixels, so neither has a pixel to report; the
# vector writers keep a document rather than a raster and answer a read
# with a fixed value; and the recording device keeps the marks a page
# made rather than the pixels they cover, so until it plays them into a
# device that paints there is no pixel to read -- what it answers is the
# ground, which is what the contract has a device answer wherever it
# holds no pixel. Every other member reports what a marking method wrote
# and is asserted about.
NO_READBACK='null bbox pdfwrite svgwrite record'
readback=

# Of those, the ones that answer a read with the page's ground rather
# than refusing it: the recording device, which holds the marks and not
# the pixels. That is a behaviour of its own and is asserted rather than
# excused -- a device that had begun answering the ink, or nothing,
# would otherwise read exactly like one legitimately quiet. The test
# reports GROUNDBACK where it made those assertions and the runs are
# held to this list.
GROUND_READBACK='record'
groundback=

if "$xpost" -h 2>/dev/null | grep -q -- '--no-sandbox'; then
    ns='--no-sandbox'
else
    ns=''
fi

verdict_workdir
devices=$DEVICE_FLEET_MARKING
fail=0

for dev in $devices; do
    # The device holding the whole page, asked for as the mode that says
    # so. The behaviour tier asserts what a marking method painted by
    # reading the pixel back, and selecting a device by name selects the
    # record in front of it, which holds no pixel to read. What a record
    # does answer is asserted of it below.
    out=$("$xpost" -q $ns -d "$(fleet_whole "$dev")" -o "$work/out.$dev" \
          "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP $dev (not built in)"; continue ;;
    esac
    if [ "$st" -ne 0 ]; then
        echo "FAIL $dev: the interpreter exited with status $st"
        fail=1
        continue
    fi
    if printf '%s\n' "$out" | grep -q '^READBACK$'; then
        readback="$readback $dev"
    fi
    if printf '%s\n' "$out" | tr -s '-' '\n' | grep -q '^GROUNDBACK$'; then
        groundback="$groundback $dev"
    fi
    if verdict_ok "$out" "$dev"; then
        echo "OK   $dev"
    else
        fail=1
    fi
done

# the xcb window device, on a private virtual display; its FillRect,
# PutPix and DrawLine methods see the same boundary inputs as the
# headless devices above
if command -v xvfb-run >/dev/null 2>&1; then
    out=$(xvfb-run -a "$xpost" -q $ns -d xcb "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP xcb (not built in)" ;;
        *)
            # This device's teardown runs after the program has printed
            # its verdict -- it holds a display connection, a window, a
            # pixmap and a graphics context -- so what the run said and
            # how it ended are two answers and a pass needs both.
            if [ "$st" -ne 0 ]; then
                echo "FAIL xcb: the interpreter exited with status $st"
                printf '%s\n' "$out" | sed 's/^/      /'
                fail=1
            fi
            if verdict_ok "$out" "xcb"; then
                echo "OK   xcb"
            else
                # the whole run, not just the failure lines: this device
                # talks to a display server, and what it says on the way
                # to the failure is the diagnosis
                printf '%s\n' "$out" | sed 's/^/      /'
                fail=1
            fi
            ;;
    esac
else
    echo "SKIP xcb (no xvfb-run)"
fi

# The same contract on the route a selection takes without asking for
# one. The roster above asked for each device with the mode that holds
# the page whole, because that is where a pixel can be read back; a
# selection naming the device alone is weighed against the band budget
# and may reach the device through a record. That is what an ordinary
# run gets, and without this the route most runs take would be the one
# never asked.
#
# Which route it is depends on the page, so both sides of the boundary
# are asked and each is held to the contract of what it reaches.
#
#   On a page over the budget the selection reaches a record, which is
#   held to the record's contract rather than the painter's: it keeps
#   the marks a page made and not the pixels they cover, so it answers
#   a read with the ground, reported as GROUNDBACK.
#
#   On the page a run takes when it names none there is no record, and
#   the same selection answers a read the way the device that paints it
#   does -- which is the answer the roster above asserted about.
#
# The tall page is over the budget for every device on the roster: the
# costliest row on it is four bytes a pixel at 612, and eight thousand
# of those is several times the budget. A budget raised past it would
# not quietly stop this half from asking -- the run would answer as the
# painter and be reported here as a failure.
BAND_OVER=612x8000+0+0

for dev in $DEVICE_FLEET_BANDS; do
    for page in over default; do
        case $page in
            over) geom="-g $BAND_OVER"; want=ground ;;
            *)    geom=; want=pixels ;;
        esac
        # the page and the route are two words in one label, so a
        # failure names the run that produced it
        who="$dev on the $page page"
        out=$("$xpost" -q $ns $geom -d "$dev" -o "$work/band.$dev.$page" \
              "$script" </dev/null 2>&1)
        st=$?
        case "$out" in
            *"wrong device"*) echo "SKIP $who (not built in)"; continue ;;
        esac
        if [ "$st" -ne 0 ]; then
            echo "FAIL $who: the interpreter exited with status $st"
            fail=1
            continue
        fi
        if printf '%s\n' "$out" | tr -s '-' '\n' | grep -q '^GROUNDBACK$'; then
            got=ground
        else
            got=pixels
        fi
        if [ "$got" != "$want" ]; then
            echo "FAILURES: -d $dev on the $page page answered a read with"
            echo "      the $got where the route it takes there answers"
            echo "      the $want; what a selection reaches without asking"
            echo "      is asserted rather than assumed"
            fail=1
            continue
        fi
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
        if verdict_ok "$out" "$who"; then
            echo "OK   $who ($got)"
        else
            fail=1
        fi
    done
done

rm -rf "$work"
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a device rejected a boundary input"
    exit 1
fi

# The devices that witnessed the behaviour tier, against the roster less
# the ones named above as unable to. A device that has quietly stopped
# reporting its pixels reads exactly like one that never could, so the
# reading is taken from the runs and held to what this file says it
# should be.
quiet=
for dev in $devices; do
    case " $readback " in *" $dev "*) continue ;; esac
    quiet="$quiet $dev"
done
want=$(printf '%s\n' $NO_READBACK | grep . | sort | tr '\n' ' ')
got=$(printf '%s\n' $quiet | grep . | sort | tr '\n' ' ')
if [ "$want" != "$got" ]; then
    for dev in $got; do
        case " $want " in
            *" $dev "*) ;;
            *) echo "FAILURES: $dev reported no pixels back, and nothing here"
               echo "      says it cannot; it has stopped being asserted about,"
               echo "      so restore its GetPix" ;;
        esac
    done
    for dev in $want; do
        case " $got " in
            *" $dev "*) ;;
            *) echo "FAILURES: $dev reported its pixels back, and it is named"
               echo "      here as a device that cannot; it can be held to the"
               echo "      behaviour tier now" ;;
        esac
    done
    exit 1
fi

# And the ones that answered with the ground, held the same way round: a
# device excused the tier above for holding no pixel has to have said so
# by answering, and one that stopped answering has stopped being
# asserted about.
gwant=$(printf '%s\n' $GROUND_READBACK | grep . | sort | tr '\n' ' ')
ggot=$(printf '%s\n' $groundback | grep . | sort | tr '\n' ' ')
if [ "$gwant" != "$ggot" ]; then
    echo "FAILURES: the devices answering a read with the page's ground are"
    echo "      [$ggot], and the ones named here as doing so are [$gwant]"
    exit 1
fi

echo "SUCCESS ($(printf '%s' "$readback" | wc -w) devices witnessed the behaviour"
echo "      tier, $(printf '%s' "$groundback" | wc -w) the ground it is excused for,"
echo "      and $(printf '%s' "$DEVICE_FLEET_BANDS" | wc -w) the route a selection takes without asking)"
exit 0
