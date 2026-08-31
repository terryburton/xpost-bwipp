#!/bin/sh
# Meson test wrapper: what a caller is told when the page the run was
# started with is one the device cannot provide.
#
# A page size given on the command line is applied while the graphics
# language loads, before the program is read, so there is no program to
# catch the refusal and no page to write. What is left is the two things
# the caller does have: the exit status and whatever was printed. Both
# are held here.
#
# The status must say the run produced nothing. A start-up that never
# reached the program and a program that ran and drew nothing are not the
# same outcome, and a caller reading the status alone is entitled to tell
# them apart -- a caller that cannot will read a missing output file as a
# job that chose not to write one.
#
# The report must name the device, the page asked for and the error the
# attempt ended in. Those three are what a caller needs in order to ask
# for something else, and none of them is anything it can go and read
# afterwards: the run that knew them is over. PLRM 8.2 gives limitcheck
# for a limit of the implementation, which is what a page with more
# pixels than a device can address reaches, so that is the name the
# devices holding one block of pixels are held to here. A device whose
# raster is virtual memory reaches the memory first and says VMerror,
# which is the limit it really met; either name is a limit reached, and
# the check asks for one of them rather than choosing for the device.
#
# The devices that keep no raster are the control. They provide a page of
# any size, so they must still start on the same geometry: a check that
# turned every large page into a failure would pass here while breaking
# every one of them.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"
# an absolute path may begin with a drive letter as well as a slash;
# prepending the working directory to one of those makes every
# invocation a path that does not exist
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac

verdict_workdir
echo showpage > "$work/job.ps"

# A square page whose sides are ordinary numbers and whose area is not:
# every device that keeps a raster refuses it, whether the bound it
# reaches is the count a pixel's position is held in or the memory the
# raster would take. A side small enough to look like a page is what
# makes this a limit of the area rather than of the dimension.
#
# The area is past every machine's memory rather than merely large, so
# that no build allocates for it and the refusal is the same wherever
# this runs. A page that some machine could hold would be provided on
# that machine and refused on the next, and the test would be reading
# the host rather than the interpreter.
BIG=2000000000x2000000000+0+0
# and a page any of them provides, so that a refusal is read as a refusal
# of this page and not of every page.
SMALL=200x200+0+0

# Which kind each device is, taken from the fleet rather than listed
# here. The three kinds below and the raster classes written in
# PostScript, which fall through to the ordinary case, are the whole
# fleet between them, so a device it gains is one of them on the day it
# arrives instead of being passed over by every case here.

# The devices whose raster is one block of pixels outside the PostScript
# virtual machine, which is where the position of a pixel is the bound.
INDEXED=" $DEVICE_FLEET_BUFFER "
# The devices that keep no raster at all and so have no page they cannot
# provide: what is left once the devices that hold pixels -- in a block
# of their own or in the rows of a class -- and the one that holds marks
# are taken out.
UNBOUNDED=' '
for s_dev in $DEVICE_FLEET_ALL; do
    case " $DEVICE_FLEET_BUFFER $DEVICE_FLEET_BANDS record " in
        *" $s_dev "*) continue ;;
    esac
    UNBOUNDED="$UNBOUNDED$s_dev "
done
# The device that holds a page as the marks that made it. It starts on
# any page, because what it builds at start-up is a record and a record
# is priced by the marks; the limit arrives when the page is put out,
# which it does by playing those marks into a raster, and that raster is
# a raster like any other. So its refusal is a page it could not put
# out rather than a device it could not start, and what is asked of it
# is that it starts, then refuses, and names a limit.
DEFERRED=' record '

fail=0

# run DEVICE GEOMETRY -- leaves the status in $st and the output in $out
run_one() {
    out=$("$xpost" -q --no-sandbox -g "$2" -d "$1" -o "$work/out.$1" \
          "$work/job.ps" </dev/null 2>&1)
    st=$?
}

one_device() {
    dev=$1

    run_one "$dev" "$SMALL"
    case $out in
        *"wrong device"*) echo "SKIP $dev (not built in)"; return 2 ;;
    esac
    # The control first: a device that cannot start at all would meet
    # every demand below by failing, and the check would read that as the
    # refusal working. Judged by the shared rule, which is the rule for a
    # run that was meant to come back clean.
    verdict_run "$st" "$out" "$dev on an ordinary page" || return 1

    run_one "$dev" "$BIG"
    printf '%s\n' "$out" | sed "s/^/$dev: /"

    # A device that keeps no raster provides this page like any other, so
    # it too is a run that was meant to come back clean.
    case " $UNBOUNDED " in
        *" $dev "*)
            verdict_run "$st" "$out" "$dev, which keeps no raster to fill," \
                || return 1
            return 0 ;;
    esac

    # A device that meets the limit when it puts the page out started on
    # it, so there is no start-up refusal to read: what it must do is
    # come back dirty and name the limit it met.
    case " $DEFERRED " in
        *" $dev "*)
            rc=0
            if [ "$st" -eq 0 ]; then
                echo "FAILURES: $dev put out a page of $BIG and exited 0"
                rc=1
            fi
            case $out in
                *limitcheck* | *VMerror*) ;;
                *) echo "FAILURES: $dev could not put out the page it was"
                   echo "      asked for and must name the limit it met"
                   rc=1 ;;
            esac
            return $rc ;;
    esac

    # What is left is a device that cannot provide the page, and what is
    # asked of it is the opposite of the shared rule: it must come back
    # dirty, and say why. Held here rather than in verdict.sh, which
    # judges runs that were meant to succeed.

    rc=0
    if [ "$st" -eq 0 ]; then
        echo "FAILURES: $dev produced no page and exited 0"
        rc=1
    fi

    line=$(printf '%s\n' "$out" | grep 'unable to load graphics')
    if [ -z "$line" ]; then
        echo "FAILURES: $dev refused the page and said nothing about it"
        return 1
    fi
    case $line in
        *" $dev "*) ;;
        *) echo "FAILURES: $dev is not named in its own refusal: $line"; rc=1 ;;
    esac
    case $line in
        *2000000000*2000000000*) ;;
        *) echo "FAILURES: $dev does not say what page was asked for: $line"
           rc=1 ;;
    esac
    case " $INDEXED " in
        *" $dev "*)
            # Which of the two bounds this page reaches is the platform's
            # to decide, so both are accepted and neither is assumed. A
            # position within the raster is held in the width the platform
            # expresses a size in: where that width is narrow the area is
            # past what a position counts and the answer is limitcheck,
            # and where it is wide the area is expressible and the memory
            # is what runs out first, which is VMerror. What is held here
            # is that a limit was named, not which one the machine has.
            case $line in
                *limitcheck*|*VMerror*) ;;
                *) echo "FAILURES: $dev reaches a limit on the page it was" \
                        "asked for and must name it: $line"
                   rc=1 ;;
            esac ;;
        *)
            case $line in
                *limitcheck* | *VMerror*) ;;
                *) echo "FAILURES: $dev names no limit it reached: $line"
                   rc=1 ;;
            esac ;;
    esac
    return $rc
}

# Every device the interpreter can make without a display: the ones with
# a raster answer the refusal, the ones without answer the control, and
# neither set is named anywhere but here.
#
# A device the build left the library out for cannot be asked at all.
# Which those are is read off the interpreter rather than assumed: handed
# a device name it does not have, it lists the ones it does.
have=$("$xpost" -q --no-sandbox -d '' /dev/null </dev/null 2>&1)
CANNOT_ANSWER=
for dev in $DEVICE_FLEET_OPTIONAL; do
    printf '%s\n' "$have" | grep -qx "[[:space:]]*$dev" ||
        CANNOT_ANSWER="$CANNOT_ANSWER $dev"
done
fleet_each one_device $DEVICE_FLEET_ALL || fail=1
fleet_hold_unasked "$CANNOT_ANSWER" || fail=1

# A roster that answered for nothing reports as quietly as one that
# answered for everything.
if [ "$fleet_asked" -eq 0 ]; then
    echo "FAILURES: no device answered, so nothing was held to anything"
    exit 1
fi
echo "startup-page: held on $fleet_asked device(s)"

verdict_exit
