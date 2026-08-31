#!/bin/sh
# Meson test wrapper: a page the recording device could not hold whole is
# refused, and not put out short of what it could not hold.
#
# The record's refusal ladder ends in a page that is not emitted: a mark
# it could not hold makes it short of one, and a record short of a mark
# refuses every replay of itself and gives nothing back. What the ladder
# needs is that every way of losing a mark reaches it. A mark can be lost
# in the device in front of the record -- handed to it, not written down,
# answered with an error -- and the record cannot learn that from a call
# it never received. Left so, the page is put out with the rest of its
# marks and without that one.
#
# So this drives the loss and reads the page, rather than reading the
# error. Reading the error would establish nothing at all: the device
# answers the same error either way, and the whole fault is in what
# happens afterwards.
#
# Four runs of one program (tests/record_short_test.ps), each painting
# the same marker rectangle and differing in what the device is handed
# beside it:
#
#   whole   a picture the device writes down
#   bare    nothing
#   short   a picture with its decode table withheld, which the device
#           refuses
#   glyph   a glyph placement naming no mask the record holds, which the
#           device refuses
#
# What each run has to answer:
#
#   whole and bare put out a page, and the two pages differ. That is what
#   makes the picture's absence visible: if they matched, every later
#   comparison here would be between two pages nobody could tell apart
#   and the run would pass having shown nothing.
#
#   short and glyph put out no page at all. A page from either is the
#   fault -- and where the page that came out is the bare one, it is the
#   fault in the shape it takes in the wild: the page a program asked for
#   with the picture in it, emitted without the picture and reported as
#   though nothing had happened.
#
#   The record holds no picture in the short run. A run whose offer was
#   quietly accepted after all would refuse its page for some other
#   reason and pass this without the refusal being about anything.
#
#   $1  path to the built xpost binary
#   $2  path to record_short_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

# $1 the case, $2 the page path; sets out to what the run said
render() {
    out=$("$xpost" -q $ns -d record -D"case=/$1" -o "$2" "$script" \
          </dev/null 2>&1)
    verdict_run "$?" "$out" "the $1 run" || return 1
    return 0
}

# What a run said about the offer, about what the record then held, and
# about the page.
offer() { printf '%s\n' "$1" | sed -n 's/^OFFER //p' | head -1; }
held()  { printf '%s\n' "$1" | sed -n 's/^HELD //p' | head -1; }
page()  { printf '%s\n' "$1" | sed -n 's/^PAGE //p' | head -1; }

# $1 what to call the run, $2 its output, $3 the offer it must report,
# $4 what must have become of its page
answered() {
    _a=$(offer "$2")
    _p=$(page "$2")
    if [ "$_a" != "$3" ]; then
        echo "FAILURES: the $1 run answered the offer with '$_a' and this"
        echo "      expects '$3', so the run is not the one described"
        return 1
    fi
    if [ "$_p" != "$4" ]; then
        echo "FAILURES: the $1 run said its page was '$_p' and this expects"
        echo "      '$4'"
        return 1
    fi
    return 0
}

render whole "$work/whole.ppm" || fail=1
whole=$out
render bare "$work/bare.ppm" || fail=1
bare=$out
render short "$work/short.ppm" || fail=1
short=$out
render glyph "$work/glyph.ppm" || fail=1
glyph=$out

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a run did not complete, so nothing here was measured"
    exit 1
fi

answered whole "$whole" "RECORDED" "OUT" || fail=1
answered bare "$bare" "NONE" "OUT" || fail=1
answered short "$short" "REFUSED typecheck" "REFUSED VMerror" || fail=1
answered glyph "$glyph" "REFUSED rangecheck" "REFUSED VMerror" || fail=1

# The picture the whole run wrote down reached the record, and the one
# the short run offered did not. Both halves are read off the record's
# own count of the pictures it holds, second of the two figures.
wimg=$(held "$whole" | awk '{print $2}')
simg=$(held "$short" | awk '{print $2}')
if [ "${wimg:-0}" -lt 1 ]; then
    echo "FAILURES: the whole run's record holds no picture, so the page it"
    echo "      put out is not the page with the picture on it and the"
    echo "      comparison below is between two pages of the same thing"
    fail=1
fi
if [ "${simg:-1}" -ne 0 ]; then
    echo "FAILURES: the short run's record holds $simg picture(s), so its"
    echo "      offer was taken up after all and the page it refused was"
    echo "      refused for some other reason"
    fail=1
fi

# The two pages that do come out, which have to differ: the picture is
# what the short run loses, and a picture nothing can see would make
# every reading here vacuous.
for f in whole bare; do
    if [ ! -s "$work/$f.ppm" ]; then
        echo "FAILURES: the $f run put out no page, and this reads its page"
        fail=1
    fi
done
if [ "$fail" -eq 0 ] && cmp -s "$work/whole.ppm" "$work/bare.ppm"; then
    echo "FAILURES: the page with the picture and the page without it are the"
    echo "      same bytes, so nothing here can see a page put out short"
    fail=1
fi

# And the two that must not. A page from either is the fault this test
# exists for; the bare page is what it looks like in the wild.
for f in short glyph; do
    [ -s "$work/$f.ppm" ] || continue
    if cmp -s "$work/$f.ppm" "$work/bare.ppm"; then
        echo "FAILURES: the $f run put out the page it would have put out"
        echo "      having been handed nothing at all: a device-layer"
        echo "      failure lost what it was handed and the record went on"
        echo "      describing a page without it, so the page came out"
        echo "      short and said nothing about it"
    else
        echo "FAILURES: the $f run put out a page after a failure that lost"
        echo "      what the device was handed, so the page it put out is"
        echo "      missing something and nothing refused it"
    fi
    fail=1
done

[ "$fail" -eq 0 ] || exit 1
echo "OK   a picture the device wrote down is on the page, and the same" \
     "page is refused where the device could not"
echo "SUCCESS"
exit 0
