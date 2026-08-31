#!/bin/sh
# Meson test wrapper: what a device selection spells, and what it refuses.
#
# A selection is a device and, after a colon, a mode: the device is the
# head of it everywhere in this tree, and how its page is held is the
# tail. For a device whose page may arrive a band at a time there are
# three spellings and they are one axis --
#
#     -d pgm          weighed: banded where banding pays
#     -d pgm:whole    the page held whole, whatever its size
#     -d pgm:band     a band of it held, whatever its size
#
# -- and the whole of what this holds is that each of them means what it
# says, that the tail names the device the third one records for, and
# that a spelling outside the vocabulary is refused rather than read as
# one of them.
#
# The refusal is the half that cannot be seen from a page. A mode nobody
# recognises would be a selection carrying a mode, which is a run having
# asked for something specific, so it would turn the weighing off without
# saying so: `pgm:bnad` would put out exactly the page `pgm:whole` puts
# out and the run comparing the two ways by changing one word would be
# comparing one way with itself. So every spelling that is not in the
# vocabulary is required to be refused, and the refusal is required to
# name what was given and what the device does take -- a refusal that
# said only "no" would leave the misspelling to be found by eye.
#
# What is asked, at the standard page, which is under the band budget
# for every device here:
#
#   the bare name        is weighed, and this page is not banded
#   DEVICE:whole         is not banded
#   DEVICE:band          is banded, at a page the weighing would not band
#   record               is banded, and paints through the colour raster,
#                        which is what ppm:band selects -- the recording
#                        class stays selectable by name and that is what
#                        selecting it means
#
# and the device each recorded page is played into is required to differ
# from device to device, which is what says the tail of the selection is
# naming the device rather than being carried along.
#
# The controls, which this runs on itself at the end and requires to
# fail: each of the three readings is stubbed, both ways where it has two
# -- a run always taken as accepted, a run always taken as refused, a
# route always read as direct, a route always read as recorded, and a
# refusal required to name a word no refusal names. A check that could
# not see those five would be reading nothing.
#
#   $1  path to the built xpost binary
#
# and, for the controls it invokes on itself:
#
#   --sabotage N $1   run a reduced pass with defect N built in, and
#                     answer as it finds it. The caller requires a
#                     failure.
set -u

sab=0
case ${1:-} in
    --sabotage) sab=$2; shift 2 ;;
esac
xpost=${1:?usage: run-band-select-test.sh [--sabotage N] <xpost>}
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
# an absolute path may begin with a drive letter as well as a slash;
# prepending the working directory to one of those makes every
# invocation a path that does not exist
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac

verdict_workdir
fail=0

# What the run says about the device it was given. A record names the
# class it plays into and carries the budget it divides that class's rows
# by; a device painting its own page carries neither. Both are read,
# since one of them answering alone would be an instrument with nothing
# holding it.
cat > "$work/route.ps" <<'PSEOF'
/played DEVICE /.playclass known def
/priced DEVICE /.bandbytes known def
(ROUTE ) print
played priced eq
    { played { (record) }{ (direct) } ifelse }
    { (split) }
ifelse print
( into ) print
played { DEVICE /.playclass get 40 string cvs }{ (-) } ifelse print
(\n) print flush
PSEOF

# ask SELECTION -- run the interpreter with that selection and leave what
# it said in $out and how it ended in $st. Nothing is judged here: half
# of what is asked below is a selection that must not be accepted.
ask() {
    out=$("$xpost" -q --no-sandbox -d "$1" -o /dev/null "$work/route.ps" \
          </dev/null 2>&1)
    st=$?
}

# accepted SELECTION -- whether the interpreter took that selection and
# ran the program with a device made from it. Both halves: a run that
# started and then died has not shown that the selection was taken, and a
# run that exited cleanly having printed a failure has not either.
accepted() {
    ask "$1"
    [ "$sab" -eq 1 ] && return 0
    [ "$sab" -eq 2 ] && return 1
    verdict_run "$st" "" "the $1 run" >/dev/null || return 1
    case $out in *"ROUTE "*) return 0 ;; esac
    return 1
}

# route_of SELECTION -- the route the run took, in $route, and the class
# its page was played into, in $into. Answers non-zero for a run that did
# not get that far or could not say.
route_of() {
    accepted "$1" || {
        note "-d $1 did not reach the program, so it says nothing about" \
             "the route that selection takes:" \
             "$(printf '%s\n' "$out" | tail -2 | tr '\n' ' ')"
        return 1
    }
    route=$(printf '%s\n' "$out" | sed -n 's/^ROUTE \([a-z][a-z]*\) into .*/\1/p')
    into=$(printf '%s\n' "$out" | sed -n 's/^ROUTE [a-z][a-z]* into \(.*\)$/\1/p')
    [ "$sab" -eq 3 ] && route=direct
    [ "$sab" -eq 4 ] && route=record
    if [ -z "${route:-}" ] || [ -z "${into:-}" ]; then
        note "-d $1 said nothing about the route it took"
        return 1
    fi
    if [ "$route" = split ]; then
        note "-d $1 is half a record: it names a class to play into or a" \
             "budget to divide, but not both"
        return 1
    fi
    return 0
}

# took SELECTION ROUTE WHY -- the selection reaches that route
took() {
    route_of "$1" || return 1
    if [ "$route" != "$2" ]; then
        note "-d $1 $3" \
             "wanted $2, and the run reports $route (into $into)"
        return 1
    fi
    return 0
}

# refused SELECTION WORD... -- the selection is refused, and the refusal
# names each of the words given. A refusal that named nothing would leave
# a misspelling to be found by eye, which is the whole reason the
# vocabulary is held to at all.
refused() {
    r_sel=$1
    shift
    if accepted "$r_sel"; then
        note "-d $r_sel was accepted; a selection outside the vocabulary" \
             "reads as a run having asked for something specific, and what" \
             "it would get is whichever route the spelling fell into"
        return 1
    fi
    r_said=$out
    for r_word in "$@"; do
        case $r_said in
            *"$r_word"*) ;;
            *)  note "-d $r_sel was refused without the refusal naming" \
                     "\"$r_word\"; what a caller needs to fix a selection" \
                     "is what it gave and what the device takes:" \
                     "$(printf '%s\n' "$r_said" | grep -i 'takes no\|does not' \
                        | head -1)"
                return 1 ;;
        esac
    done
    return 0
}

# A build without a device's library has no such selection to make.
# Which those are is read off the interpreter rather than assumed: handed
# a device name it does not have, it lists the ones it does.
have=$("$xpost" -q --no-sandbox -d '' /dev/null </dev/null 2>&1)
ASK=
for dev in $DEVICE_FLEET_BANDS; do
    if printf '%s\n' "$have" | grep -qx "[[:space:]]*$dev"; then
        ASK="$ASK $dev"
    else
        echo "SKIP $dev (not in this build)"
    fi
done
if [ -z "$ASK" ]; then
    echo "FAILURES: no device that bands by default is in this build, so"
    echo "      nothing was asked"
    exit 1
fi

# The reduced pass a control runs: one device is enough to show that a
# stubbed reading is seen.
if [ "$sab" -ne 0 ]; then
    for dev in $ASK; do ASK=$dev; break; done
fi

# ---------------------------------------------------------------------
# The vocabulary
# ---------------------------------------------------------------------
asked=0
plays=''
for dev in $ASK; do
    took "$dev" direct \
         "is the standard page, which the budget covers, so the weighing" \
        || continue
    took "$dev:whole" direct \
         "asks for the page whole, so at any page" || continue
    took "$dev:band" record \
         "asks for a band of the page, so at any page" || continue
    plays="$plays $into"
    asked=$((asked + 1))
    echo "OK   -d $dev is weighed, -d $dev:whole holds the page whole, and" \
         "-d $dev:band records into $into"
done

if [ "$asked" -eq 0 ]; then
    echo "FAILURES: no device answered, so nothing was held to anything"
    exit 1
fi

# The tail of the selection names the device, so no two of them can name
# the same one. A record that ignored the tail would answer with the
# roster's default for every device here and pass every line above.
ndistinct=$(printf '%s\n' $plays | sort -u | grep -c .)
if [ "$ndistinct" -ne "$asked" ]; then
    note "$asked device(s) selected with the band mode play into" \
         "$ndistinct class(es):$plays; the device named before the mode is" \
         "not what settles which device paints the recorded page"
fi

# The recording class is still a device a run may select, and what
# selecting it means is the colour raster recorded at any page -- which
# is ppm:band. Held here so that the compatibility spelling cannot
# quietly come to mean something else. doc/MANUAL says the same.
case " $ASK " in
    *" ppm "*)
        if took record record "names the recording class itself, so"; then
            r_into=$into
            if took ppm:band record "asks for a recorded colour page, so"; then
                if [ "$r_into" != "$into" ]; then
                    note "-d record plays into $r_into and -d ppm:band into" \
                         "$into; the spelling kept for the runs that use it" \
                         "no longer means what it is documented to mean"
                else
                    echo "OK   -d record is -d ppm:band: recorded into $into"
                fi
            fi
        fi ;;
    *) echo "SKIP -d record (the colour raster it defaults to is not in" \
            "this build)" ;;
esac

# ---------------------------------------------------------------------
# What is not in the vocabulary
# ---------------------------------------------------------------------
# The word every refusal below is also required to name under the fifth
# control, which no refusal names: what that control puts to this half is
# whether a message that leaves a word out is seen to have left it out,
# and the only way to ask it is to require a word that is not there.
sabword=''
[ "$sab" -eq 5 ] && sabword=notaword

for dev in $ASK; do
    # a misspelling of a mode that exists, which is the case the roster
    # in the refusal is for
    refused "$dev:bnad" bnad whole band $sabword || continue
    refused "$dev:bogus" bogus whole band $sabword || continue
    echo "OK   -d $dev takes no mode but whole and band, and says so"
done

# The recording class takes no mode: what a record plays into is the
# device the run selected, so the device belongs at the head of the
# selection. A run that spells it the other way round is told where the
# head is rather than merely refused.
for dev in $ASK; do
    refused "record:$dev" "$dev:band" $sabword || continue
    echo "OK   -d record:$dev is refused, and names -d $dev:band"
done
refused "record:bogus" record bogus $sabword ||
    note "-d record:bogus is not refused as a mode the recording class" \
         "does not take"

# The raster device's mode is not how much of a page is held but the
# arrangement its page is lent back in, and the four names are the whole
# of it. A misspelling accepted would hand an embedder a page in one
# arrangement to be read in another, which is a page of the right marks
# in the wrong colours and nothing said about it.
if printf '%s\n' "$have" | grep -qx "[[:space:]]*raster"; then
    if refused raster:rbg raster rbg rgb argb bgr bgra $sabword; then
        echo "OK   -d raster takes no mode but its four arrangements," \
             "and says so"
    fi
else
    echo "SKIP -d raster (not in this build)"
fi

# A device that cannot take its page a band at a time takes neither word:
# passing them through would leave a run believing it had asked for a
# banded page and given a whole one under a word that said otherwise.
for dev in bgr raster null; do
    printf '%s\n' "$have" | grep -qx "[[:space:]]*$dev" || continue
    refused "$dev:band" "$dev" band $sabword || continue
    refused "$dev:whole" "$dev" whole $sabword || continue
    echo "OK   -d $dev takes neither whole nor band"
done

# And the rule that has no exceptions: whatever a device takes, a word
# outside it is refused. The devices with words to take are held to
# their own above; this is asked of the whole roster at once, because
# the device it would find is the one nothing above thinks to ask about
# -- a device declaring nothing would take every misspelling silently,
# and a selection read as a run having asked for something specific is
# the one reading that no page and no error can show. Each refusal is
# required to name the device and the word, which is what a caller has
# to have to spell it again.
held=0
for dev in $DEVICE_FLEET_ALL; do
    printf '%s\n' "$have" | grep -qx "[[:space:]]*$dev" || continue
    if refused "$dev:notamode" "$dev" notamode $sabword; then
        held=$((held + 1))
    fi
    [ "$sab" -ne 0 ] && break
done
if [ "$held" -eq 0 ]; then
    note "no device was held to refusing a word outside everything it takes"
else
    echo "OK   $held device(s) refuse a mode that is in no vocabulary," \
         "naming the device and the word"
fi

# and the name that is not a device at all, which is refused before any
# mode is looked at. No word is required of that refusal here: the option
# parser answers it by printing the devices this build has, which is the
# roster and not a sentence about the name it was given.
refused notadevice ||
    note "a selection naming no device this build has was not refused"

if [ "$sab" -ne 0 ]; then
    [ "$fail" -eq 0 ] || exit 1
    echo "the sabotage went unseen"
    exit 0
fi

[ "$fail" -eq 0 ] || exit 1
echo "band-select: held on $asked device(s)"

# ---------------------------------------------------------------------
# The controls
#
# Five defects, one per reading that could go quiet: a run always taken
# as accepted, a run always taken as refused, a route always read as
# direct, a route always read as recorded, and a refusal held to naming a
# word that is in no refusal.
# ---------------------------------------------------------------------
for n in 1 2 3 4 5; do
    s_out=$("$self" --sabotage "$n" "$xpost" 2>&1)
    s_st=$?
    if [ "$s_st" -eq 0 ]; then
        echo "FAILURES: sabotage $n was not noticed, so the check that"
        echo "      should have caught it sees nothing:"
        printf '%s\n' "$s_out" | sed 's/^/      /'
        fail=1
    fi
done

verdict_exit
