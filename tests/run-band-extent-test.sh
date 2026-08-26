#!/bin/sh
# Meson test wrapper: which pages a record starts on, and which it
# refuses before the program is read.
#
# A page size given on the command line is applied while the graphics
# language loads, so a page the device cannot provide stops the run
# before the program. Whether the program printed is therefore the whole
# of how a caller tells a page that was taken from a page that was
# refused, and it is what this reads.
#
# A record holds marks and no pixels, so no page it is given weighs
# anything until the marks are played into the device that paints. What
# settles its extent is the array of rows that device holds the page as,
# which is the one term still growing with the page once banding bounds
# the pixels (doc/xpost_design.dox): the memory that array comes to -- a
# quantity, where a composite's length is the range of a field. A build
# whose field is wide has a wider range and no more memory.
#
# Both ends of that are held, on every build. A page whose rows are past
# any memory is refused, in either dimension and in both: the array
# follows the height and a row follows the width, so a bound reading only
# one of them leaves the other unbounded. And a page of 65,535 -- the
# largest the ordinary build can name -- is taken, so the bound cannot be
# tightened onto a page the interpreter provides.
#
# The question is put to every device a record can be played into, and to
# the record a run selects by name alone. The extent is the record's to
# refuse rather than the target's, so a target answering differently from
# the rest is one reaching a bound of its own first.
#
# The job makes no mark, so no page is put out and nothing here waits on
# a raster. Which page the device started on is the question, and a
# device that started has answered it.
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
echo '(reached the program) print flush' > "$work/job.ps"

# A side whose array of rows is past every machine's memory: two thousand
# million object slots is tens of gigabytes before a pixel exists, so no
# build allocates for it and the refusal is the same wherever this runs.
PAST=2000000000
# The largest a composite counts in the ordinary build, which is the
# largest page that build can name. The wide build names larger ones and
# still takes this one.
NAMED=65535
# and the shape of page a job asks for.
ORDINARY=612x792

# The devices a record can be played into (data/recorddev.ps), each
# selected with the mode that holds a band of the page whatever its size,
# and the recording class itself, which a run may still select by name
# and which then plays into the device its roster defaults to.
BANDED='record pgm:band ppm:band pbm:band tiff:band png:band
        jpeg:band'

fail=0

# run SELECTION GEOMETRY -- leaves the status in $st and the output in $out
run_one() {
    # the selection names the output file, and a colon is a character a
    # path may not carry everywhere this runs
    o=$(printf '%s' "$1" | tr ':' '-')
    out=$("$xpost" -q --no-sandbox -g "$2+0+0" -d "$1" -o "$work/out.$o" \
          "$work/job.ps" </dev/null 2>&1)
    st=$?
}

# reached -- whether the run just made got as far as the program
reached() {
    case $out in *"reached the program"*) return 0 ;; esac
    return 1
}

# taken SELECTION GEOMETRY -- the device started on this page
taken() {
    run_one "$1" "$2"
    reached && return 0
    echo "FAILURES: $1 refused a page of $2, which it provides:"
    printf '%s\n' "$out" | sed 's/^/      /'
    return 1
}

# refused SELECTION GEOMETRY -- the device did not start on this page,
# came back dirty, and named itself, the page and the limit it reached
refused() {
    run_one "$1" "$2"
    rc=0
    if reached; then
        echo "FAILURES: $1 started on a page of $2, whose rows no memory"
        echo "      holds, and ran the program on it"
        rc=1
    fi
    if [ "$st" -eq 0 ]; then
        echo "FAILURES: $1 exited 0 having been asked for a page of $2"
        rc=1
    fi
    line=$(printf '%s\n' "$out" | grep 'unable to load graphics')
    if [ -z "$line" ]; then
        echo "FAILURES: $1 said nothing about the page of $2 it could not"
        echo "      provide"
        return 1
    fi
    # the device a run asked for is the record, whichever device it named
    # for the record to play into
    case $line in
        *" record "*) ;;
        *) echo "FAILURES: $1 is not named in its own refusal: $line"; rc=1 ;;
    esac
    case $line in
        *"$PAST"*) ;;
        *) echo "FAILURES: $1 does not say what page was asked for: $line"
           rc=1 ;;
    esac
    # PLRM 8.2 gives limitcheck for a limit of the implementation and
    # VMerror for virtual memory exhausted; either is a limit reached,
    # and which one is the device's to say.
    case $line in
        *limitcheck* | *VMerror*) ;;
        *) echo "FAILURES: $1 names no limit it reached on $2: $line"; rc=1 ;;
    esac
    return $rc
}

one_device() {
    dev=$1

    # The control first: a device that cannot start at all would meet
    # every demand below by failing, and the check would read that as the
    # refusal working.
    run_one "$dev" "$ORDINARY"
    case $out in
        *"wrong device"*) echo "SKIP $dev (not built in)"; return 2 ;;
    esac
    verdict_run "$st" "$out" "$dev on an ordinary page" || return 1
    reached || {
        echo "FAILURES: $dev did not reach the program on an ordinary page"
        return 1
    }

    taken "$dev" "${NAMED}x792"      || return 1
    taken "$dev" "792x${NAMED}"      || return 1
    taken "$dev" "${NAMED}x${NAMED}" || return 1

    refused "$dev" "${PAST}x792"     || return 1
    refused "$dev" "792x${PAST}"     || return 1
    refused "$dev" "${PAST}x${PAST}" || return 1
    return 0
}

# A record plays into the device the selection names, and a build without
# that device's library has no such selection to make. Which those are is
# read off the interpreter rather than assumed: handed a device name it
# does not have, it lists the ones it does.
have=$("$xpost" -q --no-sandbox -d '' /dev/null </dev/null 2>&1)
ASK=
for dev in $BANDED; do
    target=${dev#record}
    target=${target#:}
    if [ -n "$target" ] &&
       ! printf '%s\n' "$have" | grep -qx "[[:space:]]*$target"; then
        echo "SKIP $dev (the $target device is not in this build)"
        continue
    fi
    ASK="$ASK $dev"
done

fleet_each one_device $ASK || fail=1

# A roster that answered for nothing reports as quietly as one that
# answered for everything.
if [ "$fleet_asked" -eq 0 ]; then
    echo "FAILURES: no device answered, so nothing was held to anything"
    exit 1
fi
echo "band-extent: held on $fleet_asked selection(s)"

[ "$fail" -eq 0 ] || exit 1
echo SUCCESS
