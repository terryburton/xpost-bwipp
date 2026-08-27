#!/bin/sh
#
# One device roster, spelled in several places, held to agreeing.
#
# A device name is a selection: -d names it on the command line and
# setpagedevice names it from the program, and they are two spellings of
# one thing. The names live in three files -- the option parser's list,
# the names the interpreter accepts, and the .devicemakers dictionary the
# page-device operator looks in -- and nothing made them agree, so five
# devices were selectable with -d and unreachable by name. That is worse
# than merely unreachable: a page-device request naming no device
# defaults to the running one, so on those five every setpagedevice
# raised rangecheck and the page could not even be resized.
#
# The fourth is tests/device-fleet.sh, the roster the test wrappers run.
# It is held to naming every device the interpreter can make, bar the
# platform exclusions declared below, and its cross-product subsets are
# held to naming only members of it.
#
# The fifth is prose: doc/MANUAL's table of devices, and the two
# sentences in it that name which devices hold a run's pages in one file
# and which can take a page a band at a time. A table cannot be derived
# at build time and it can be held to what is, which is what the last
# section does -- a device added to the interpreter and not to the manual
# is one no reader of the manual will hear about.
#
# Sources are read by name rather than by scanning a directory: a built
# tree leaves object files beside them whose debug information matches
# every pattern here, so a directory scan reads green where nothing was
# built and red where something was.
#
#   $1  path to the source tree root
#   $2  the built interpreter, for the one roster that cannot be read
#       off the source (see the last section)
set -u
src=${1:?usage: check-device-roster.sh <srcroot> <xpost>}
xpost=${2:?usage: check-device-roster.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
# Taken before the mirror below moves src, so that the roster read here
# and the devices asked for there come from the one tree.
guard_srcdata "$src"

guard_workdir
# read a tree whose lines end where the scans below expect them to
srcdir=$src
guard_mirror_tree "$src"
src=$mirror

main_c="$src/src/bin/xpost_main.c"
interp_c="$src/src/lib/xpost_interpreter.c"
init_ps="$src/data/init.ps"

guard_require_file "$main_c" "the option parser"
guard_require_file "$interp_c" "the interpreter"
guard_require_file "$init_ps" "the interpreter's PostScript"

fail=0

# The names the command line accepts.
awk '/_xpost_main_devices\[\] *=/ { in_t = 1; next }
     in_t && /NULL/ { in_t = 0 }
     in_t && /^ *"/ { gsub(/[",]/, ""); gsub(/^ +| +$/, ""); if ($0 != "") print }' \
    "$main_c" | sort -u > "$work/cmdline"

# The names the interpreter accepts as a device selection. What builds
# the device is the .devicemakers dictionary below; what this list
# answers is whether a name is a device at all, which is the question
# xpost_create answers to its caller before any run begins.
awk '/device_strings\[\] *=/ { in_t = 1; next }
     in_t && /NULL/ { in_t = 0 }
     in_t && /^ *"/ { sub(/^ *"/, ""); sub(/".*$/, ""); print }' \
    "$interp_c" | sort -u > "$work/maker"

# The names setpagedevice will make.
awk '/\.devicemakers *<</ { in_t = 1; next }
     in_t && />> *put/ { in_t = 0 }
     in_t && /^ *\/[a-z]/ { sub(/^ *\//, ""); sub(/ .*$/, ""); print }' \
    "$init_ps" | sort -u > "$work/pagedevice"

for f in cmdline maker pagedevice; do
    if [ ! -s "$work/$f" ]; then
        echo "FAILURES: no device names found for the $f roster"
        echo "      the shape the guard reads for has changed; fix the guard"
        exit 1
    fi
done

# $1 label for the roster under test, $2 its file, $3 the accepted list it
# is held to, $4 where each lives
report_diff() {
    guard_held=0
    guard_hold "$3" "$2" \
        "not named by the $1 roster ($4):" \
        "named by the $1 roster and not something the interpreter can
      make ($4):"
    [ "$guard_held" -eq 0 ] || fail=1
}

report_diff "command-line" "$work/cmdline" "$work/maker" \
        "src/bin/xpost_main.c against src/lib/xpost_interpreter.c"
report_diff "page-device" "$work/pagedevice" "$work/maker" \
        "data/init.ps .devicemakers against src/lib/xpost_interpreter.c"

# The fourth spelling: the roster the test wrappers run. It used to be a
# list per wrapper, which is how a whole device came to be built,
# selectable and never once exercised; it is one file now, and this is
# what holds it to the names the interpreter accepts.
#
# Excluded from the roster, with reasons rather than by omission:
#   gdi, gl  the Windows window devices: they need a platform that can
#            open a window, so the wrappers reach them by name where the
#            platform provides one rather than through the roster.
#   xcb      the X11 window device: it needs a display, and the wrappers
#            that can conjure a virtual one run it by name.
exclude='gdi gl xcb'

fleet="$src/tests/device-fleet.sh"
guard_require_file "$fleet" "the device roster"

# The roster is three shell assignments, so read them by running the file
# rather than by matching its text: a continued list, a comment or a
# respelling then reads the same here as it does in a wrapper.
( . "$fleet"
  for v in $DEVICE_FLEET_ALL; do echo "all $v"; done
  for v in $DEVICE_FLEET_LIFETIME; do echo "lifetime $v"; done
  for v in $DEVICE_FLEET_MARKING; do echo "marking $v"; done
  for v in $DEVICE_FLEET_OPTIONAL; do echo "optional $v"; done
) > "$work/fleet" 2>/dev/null
for set in all lifetime marking optional; do
    awk -v s="$set" '$1 == s { print $2 }' "$work/fleet" | sort -u \
        > "$work/fleet-$set"
    if [ ! -s "$work/fleet-$set" ]; then
        echo "FAILURES: DEVICE_FLEET_$(echo "$set" | tr a-z A-Z) is empty or unset"
        echo "      in tests/device-fleet.sh"
        exit 1
    fi
done

grep -vx -e gdi -e gl -e xcb "$work/maker" > "$work/headless"
report_diff "device-fleet" "$work/fleet-all" "$work/headless" \
        "tests/device-fleet.sh DEVICE_FLEET_ALL against src/lib/xpost_interpreter.c,\
 less $exclude"

# A subset names members of the roster. One that names something else is
# a device nothing makes, run nowhere and reported as covered.
for set in lifetime marking optional; do
    stray=$(comm -23 "$work/fleet-$set" "$work/fleet-all")
    if [ -n "$stray" ]; then
        echo "FAIL: DEVICE_FLEET_$(echo "$set" | tr a-z A-Z) names devices the roster does not:"
        printf '%s\n' "$stray" | sed 's/^/      /'
        fail=1
    fi
done

# The subsets leave devices out, so something has to run the whole
# roster. That is the smoke wrapper, and it has to reach it through the
# roster rather than through a list of its own.
smoke="$src/tests/run-devices-test.sh"
guard_require_file "$smoke" "the device smoke wrapper"
if ! grep -q 'DEVICE_FLEET_ALL' "$smoke"; then
    echo "FAIL: run-devices-test.sh does not render the whole roster;"
    echo "      the devices the cross-product subsets leave out are then"
    echo "      run nowhere"
    fail=1
fi

# ---------------------------------------------------------------------
# The devices that band, which are written down three times
#
# Selecting one of these selects banding, and the selection is settled
# before any boot file is read -- so the list exists in C as well as in
# the recording class's own roster, and a third time in the test fleet
# that wrappers ask for the page-whole spelling through. Three copies of
# one fact is three chances for it to drift, and this is where they are
# held to each other.
( . "$fleet"; for v in $DEVICE_FLEET_BANDS; do echo "$v"; done ) \
    2>/dev/null | sort -u > "$work/bands-fleet"
if [ ! -s "$work/bands-fleet" ]; then
    echo "FAIL: DEVICE_FLEET_BANDS is empty or unset in tests/device-fleet.sh"
    fail=1
fi

awk '/^#define XPOST_BANDS_BY_DEFAULT\(X\)/ { inmacro = 1 }
     inmacro { print; if ($0 !~ /\\$/) exit }' \
    "$src/src/lib/xpost.h" \
    | grep -o 'X("[a-z0-9]*")' | sed 's/^X("//; s/")$//' | sort -u > "$work/bands-c"
if [ ! -s "$work/bands-c" ]; then
    echo "FAIL: no XPOST_BANDS_BY_DEFAULT list found in src/lib/xpost.h"
    fail=1
fi

sed -n '/\/\.playtargets </,/>>/p' "$src/data/recorddev.ps" \
    | sed -n 's|^  *//*\([a-z0-9]*\) /\..*$|\1|p' | sort -u \
    > "$work/bands-ps"
if [ ! -s "$work/bands-ps" ]; then
    echo "FAIL: no .playtargets roster found in data/recorddev.ps"
    fail=1
fi

for pair in 'bands-c:the list the C selection is compiled from' \
            'bands-ps:the recording class roster in data/recorddev.ps'; do
    other=${pair%%:*}
    what=${pair#*:}
    if [ -s "$work/bands-fleet" ] && [ -s "$work/$other" ] \
       && ! cmp -s "$work/bands-fleet" "$work/$other"; then
        echo "FAIL: DEVICE_FLEET_BANDS and $what name different devices:"
        diff "$work/bands-fleet" "$work/$other" | sed 's/^/      /' | head -8
        fail=1
    fi
done

# ---------------------------------------------------------------------
# And the declaration those three lists are lists of
#
# Three rosters agreeing says which devices are routed through the band
# loop. It says nothing about whether those are the devices whose pages
# can arrive that way, and that is a separate statement each device makes
# for itself: /BandedPage on its class. The two are a pair. A device
# declaring it and absent from .playtargets promises an arrival no budget
# will ever give it, whatever is named to --band-bytes; one named there
# and declaring nothing is played into a device expecting the page whole.
#
# It is asked of a running interpreter rather than read off the source,
# because a class is built and not written: the compiled drivers copy a
# class that declares it and then say their own thing about the copy, and
# one driver body here makes two classes and says a different thing for
# each. What a file spells and what a class ends up holding are therefore
# two questions, and this is the one that matters.
#
# Each device is installed by name and its running dictionary read. A
# device the build left out cannot be installed and is reported as such
# rather than counted either way.
#
# The record is not asked. It is not something a page is routed to but
# the thing that does the routing, and what it says about bands is
# whatever it copied from the class it was specialised from
# (data/recorddev.ps), so its answer is the target's answer read twice.
asked="$work/asked.ps"
{
    echo "["
    grep -vx record "$work/fleet-all" | sed 's|^|/|'
    cat <<'EOF'
]
{ /D exch def
  { << /OutputDevice D /PageSize [ 8 8 ] >> setpagedevice } stopped
  { (BANDS ) print D 60 string cvs print ( unmade\n) print }
  { (BANDS ) print D 60 string cvs print ( ) print
    DEVICE /BandedPage known { (yes) }{ (no) } ifelse print (\n) print }
  ifelse
} forall
EOF
} > "$asked"

# Started on the device that paints nothing, so that what a device says
# is read after a page-device request installed it and never off whatever
# device this build was configured with.
said=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
        "$xpost" -q -d null -o roster.scratch "$asked" \
        </dev/null 2>&1 )
if [ $? -ne 0 ] || [ -z "$said" ]; then
    echo "FAILURES: the interpreter could not be asked what its devices say"
    echo "      about taking a page a band at a time:"
    printf '%s\n' "$said" | sed 's/^/      /' | head -8
    exit 1
fi

printf '%s\n' "$said" | awk '$1 == "BANDS" && $3 == "yes" { print $2 }' \
    | sort -u > "$work/says-yes"
printf '%s\n' "$said" | awk '$1 == "BANDS" && $3 == "unmade" { print $2 }' \
    | sort -u > "$work/unmade"

# A device that could not be made said nothing, so it is held to nothing
# -- and the roster it would have been held to is narrowed to match,
# rather than the device being counted absent from it.
grep -vx record "$work/bands-ps" | { [ -s "$work/unmade" ] &&
    grep -vxF -f "$work/unmade" || cat; } | sort -u > "$work/routed"

if [ ! -s "$work/says-yes" ]; then
    echo "FAILURES: no device the interpreter can make says its page may"
    echo "      arrive a band at a time, and the roster names $(wc -l < "$work/routed" | tr -d ' ')."
    echo "      The question is being asked wrong."
    exit 1
fi

guard_held=0
guard_hold "$work/says-yes" "$work/routed" \
    "declaring BandedPage and not routed through the band loop (their
      class against .playtargets, data/recorddev.ps). A page reaches
      such a device whole whatever --band-bytes names, so the
      declaration describes an arrival that cannot happen. Route it, or
      take the declaration back out where the class makes it and say
      what stands in the way:" \
    "routed through the band loop and not declaring BandedPage
      (.playtargets, data/recorddev.ps, against their class). A device
      that has not said this is handed the whole page it expects
      everywhere else, and is being handed part of one here:"
[ "$guard_held" -eq 0 ] || fail=1

# ---------------------------------------------------------------------
# And which of them leave nothing at the output path
#
# A page is a file for all but four devices: two hand their raster to the
# program embedding the interpreter and two paint nothing, and a run
# naming -o leaves that name untouched. Every wrapper that compares the
# bytes of a page has to know which four, and each of them used to know
# separately -- the byte-identity gate, the multi-page shapes and the
# smoke wrapper carried the same four names three times over, so a device
# added to the roster and to none of them was rendered and never
# compared, with nothing anywhere saying so.
#
# The list is DEVICE_FLEET_NOFILE and this is what holds it. It is a
# reading rather than a declaration: a page is asked for through each
# device and the output path is looked at afterwards, which is the same
# question the wrappers ask and answers it the same way. Both directions
# are held, so a device that stopped writing its page fails here as
# surely as one that started.
( . "$fleet"; for v in $DEVICE_FLEET_NOFILE; do echo "$v"; done ) \
    2>/dev/null | sort -u > "$work/nofile-fleet"
if [ ! -s "$work/nofile-fleet" ]; then
    echo "FAIL: DEVICE_FLEET_NOFILE is empty or unset in tests/device-fleet.sh"
    fail=1
fi
stray=$(comm -23 "$work/nofile-fleet" "$work/fleet-all")
if [ -n "$stray" ]; then
    echo "FAIL: DEVICE_FLEET_NOFILE names devices the roster does not:"
    printf '%s\n' "$stray" | sed 's/^/      /'
    fail=1
fi

# one page, small, through each device in turn
printf '%s\n' 'newpath 2 2 moveto 6 6 lineto stroke showpage' \
    > "$work/onepage.ps"
: > "$work/wrote"
: > "$work/left"
: > "$work/pathasked"
asked=0
while read -r dev; do
    rm -f "$work/path.$dev"
    said=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
            "$xpost" -q -d "$dev" -o "path.$dev" onepage.ps \
            </dev/null 2>&1 )
    case "$said" in
        *"wrong device"*) continue ;;
    esac
    asked=$((asked + 1))
    echo "$dev" >> "$work/pathasked"
    if [ -e "$work/path.$dev" ]; then
        echo "$dev" >> "$work/wrote"
    else
        echo "$dev" >> "$work/left"
    fi
done < "$work/fleet-all"
sort -u "$work/left" -o "$work/left"
sort -u "$work/pathasked" -o "$work/pathasked"
if [ "$asked" -lt 8 ]; then
    echo "FAILURES: only $asked device(s) could be asked for a page, and the"
    echo "      roster names $(wc -l < "$work/fleet-all" | tr -d ' '). The question is being asked wrong."
    exit 1
fi
if [ ! -s "$work/wrote" ]; then
    echo "FAILURES: no device left a file at the output path, so every device"
    echo "      reads as one whose page is not a file. The question is being"
    echo "      asked wrong."
    exit 1
fi
# a device this build could not make was never asked, so the list it is
# held to is narrowed rather than the device counted absent from it
comm -12 "$work/nofile-fleet" "$work/pathasked" > "$work/nofile-want"
guard_held=0
guard_hold "$work/left" "$work/nofile-want" \
    "leaving nothing at the output path and not named by
      DEVICE_FLEET_NOFILE. Every wrapper that compares the bytes of a
      page reads that list to know which devices have none, so a device
      missing from it is one they will each ask for a file and find
      nothing. Name it there with what its page arrives as instead:" \
    "named by DEVICE_FLEET_NOFILE and writing a file after all. A device
      whose page is a file is one the byte comparisons can hold, and
      naming it there is what keeps them off it:"
[ "$guard_held" -eq 0 ] || fail=1

# ---------------------------------------------------------------------
# The devices that hold their pixels outside virtual memory
#
# The list is DEVICE_FLEET_BUFFER and this is what holds it. Like the one
# above it is a reading and not a declaration: each device is made and
# asked the two questions that tell its kind -- whether its class's Emit
# is an operator, so the page is assembled in compiled code, and whether
# it carries a band sink, so it holds pixels at all -- and the answers
# are compared with the list both ways round.
#
# Asking the running device rather than reading the source is what makes
# this worth having: a class that gains its compiled Emit through a copy
# of another class says so here without anyone noticing it was copied.
( . "$fleet"; for v in $DEVICE_FLEET_BUFFER; do echo "$v"; done ) \
    2>/dev/null | sort -u > "$work/buffer-fleet"
if [ ! -s "$work/buffer-fleet" ]; then
    echo "FAIL: DEVICE_FLEET_BUFFER is empty or unset in tests/device-fleet.sh"
    fail=1
fi
stray=$(comm -23 "$work/buffer-fleet" "$work/fleet-all")
if [ -n "$stray" ]; then
    echo "FAIL: DEVICE_FLEET_BUFFER names devices the roster does not:"
    printf '%s\n' "$stray" | sed 's/^/      /'
    fail=1
fi

cat > "$work/kind.ps" <<'EOF'
/getdevice { .privatedict /.graphicsdict get /currgstate get /device get } def
/d getdevice def
(KIND ) print
d /Emit get type /operatortype eq { (compiled) }{ (interpreted) } ifelse print
( ) print
d /.bandsink known { (pixels) }{ (nopixels) } ifelse print
(\n) print flush quit
EOF
: > "$work/buffer-saw"
: > "$work/kindasked"
while read -r dev; do
    said=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
            "$xpost" -q -d "$dev" -o /dev/null kind.ps </dev/null 2>&1 )
    case "$said" in
        *"wrong device"*) continue ;;
    esac
    kind=$(printf '%s\n' "$said" | sed -n 's/^KIND //p' | head -1)
    [ -n "${kind:-}" ] || continue
    echo "$dev" >> "$work/kindasked"
    [ "$kind" = "compiled pixels" ] && echo "$dev" >> "$work/buffer-saw"
done < "$work/fleet-all"
sort -u -o "$work/buffer-saw" "$work/buffer-saw"
sort -u -o "$work/kindasked" "$work/kindasked"
if [ ! -s "$work/kindasked" ]; then
    echo "FAIL: no device answered what kind it is, so this comparison was"
    echo "      never made and the list it holds stands unchecked."
    exit 1
fi
comm -12 "$work/buffer-fleet" "$work/kindasked" > "$work/buffer-want"
guard_held=0
guard_hold "$work/buffer-saw" "$work/buffer-want" \
    "assembling their page in compiled code and holding pixels of their
      own, and not named by DEVICE_FLEET_BUFFER. The wrappers that
      compare a band against a whole page, and the one that asks which
      page a device refuses at start-up, read that list to know which
      devices to ask. A device missing from it is asked by none of
      them:" \
    "named by DEVICE_FLEET_BUFFER and no longer answering that way.
      Either the class stopped assembling its page in compiled code or
      it stopped holding pixels; whichever it is, the questions those
      wrappers ask of it no longer fit:"
[ "$guard_held" -eq 0 ] || fail=1

# ---------------------------------------------------------------------
# And the copies of all of it written in prose
#
# doc/MANUAL is where a reader learns which devices there are, which of
# them hold a run's pages in one file and which can take a page a band at
# a time. Those are three more spellings of the rosters above, and prose
# cannot be derived at build time -- but it can be held, which is what
# this does. A device added to the interpreter and not to the manual is
# a device nobody reading the manual knows about, and until now nothing
# said so.
#
# The manual is read from the tree rather than from the mirror above,
# which does not carry doc/, so its line endings are taken off here.
guard_require_file "$srcdir/doc/MANUAL" "the manual"
tr -d '\r' < "$srcdir/doc/MANUAL" > "$work/manual"

# a small number said as a word, which is how the manual says one
numword() {
    case $1 in
        1) echo one ;;   2) echo two ;;   3) echo three ;;
        4) echo four ;;  5) echo five ;;  6) echo six ;;
        7) echo seven ;; 8) echo eight ;; 9) echo nine ;;
        10) echo ten ;;  11) echo eleven ;; 12) echo twelve ;;
        *) echo "$1" ;;
    esac
}
# and the same word with its first letter raised, for a sentence opening
# on it
numword_cap() {
    w=$(numword "$1")
    printf '%s%s\n' "$(printf '%s' "$w" | cut -c1 | tr a-z A-Z)" \
        "$(printf '%s' "$w" | cut -c2-)"
}

# ---- the table of devices
#
# The first column of the section that lists them. One row names two
# devices, so the column is split rather than read as a word.
awk '/^== The devices ==/ { on = 1; next }
     on && /^== / { exit }
     on && /^  [a-z]/ {
         line = $0
         sub(/^  /, "", line)
         sub(/[ \t][ \t].*$/, "", line)
         gsub(/,/, " ", line)
         n = split(line, a, / +/)
         for (i = 1; i <= n; i++) if (a[i] != "") print a[i]
     }' "$work/manual" | sort -u > "$work/manual-devices"
if [ ! -s "$work/manual-devices" ]; then
    echo "FAILURES: doc/MANUAL's table of devices could not be read; the"
    echo "      shape this guard reads for has changed. Fix the guard."
    exit 1
fi
guard_held=0
guard_hold "$work/manual-devices" "$work/maker" \
    "named by doc/MANUAL's table and not something the interpreter can
      make:" \
    "made by the interpreter and not named by doc/MANUAL's table. A
      device absent from that table is one a reader of the manual has no
      way of hearing about:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the sentence about which devices band
#
# Read out of the whole file as one stream, since the list is wrapped
# across lines and where it wraps is the typesetting rather than the
# claim.
band_said=$(tr '\n' ' ' < "$work/manual" | tr -s ' ' \
    | sed -n 's/.*[^A-Za-z]\([A-Za-z][a-z]*\) devices can take their page a band at a time instead -- \([^-]*\) --.*/\1|\2/p')
if [ -z "$band_said" ]; then
    echo "FAILURES: doc/MANUAL's sentence naming the devices that band could"
    echo "      not be read; the shape this guard reads for has changed."
    echo "      Fix the guard."
    exit 1
fi
printf '%s\n' "${band_said#*|}" | tr ',' ' ' | sed 's/ and / /g' \
    | tr ' ' '\n' | grep . | sort -u > "$work/manual-bands"
if ! cmp -s "$work/manual-bands" "$work/bands-fleet"; then
    echo "FAIL: doc/MANUAL names these devices as taking their page a band"
    echo "      at a time and the roster names those:"
    echo "      manual: $(tr '\n' ' ' < "$work/manual-bands")"
    echo "      roster: $(tr '\n' ' ' < "$work/bands-fleet")"
    fail=1
fi
want=$(numword_cap "$(wc -l < "$work/bands-fleet" | tr -d ' ')")
if [ "${band_said%%|*}" != "$want" ]; then
    echo "FAIL: doc/MANUAL opens that sentence with"
    echo "      \"${band_said%%|*}\" and the roster names $want of them"
    fail=1
fi

# ---- and the sentence about which devices hold a run's pages in one file
#
# The same fact tests/run-multipage-test.sh chooses a page shape by, and
# asked the same way: a device that accumulates a page into an open
# document carries the method that does it.
{
    echo "["
    sed 's|^|/|' "$work/fleet-all"
    cat <<'EOF'
]
{ /D exch def
  { << /OutputDevice D /PageSize [ 8 8 ] >> setpagedevice } stopped
  { pop }
  { DEVICE /.emitpage known
    { (PAGES ) print D 60 string cvs print (\n) print } if }
  ifelse
} forall
EOF
} > "$work/pages.ps"
said=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
        "$xpost" -q -d null -o pages.scratch pages.ps \
        </dev/null 2>&1 )
printf '%s\n' "$said" | awk '$1 == "PAGES" { print $2 }' | sort -u \
    > "$work/paginated"
if [ ! -s "$work/paginated" ]; then
    echo "FAILURES: no device holds a run's pages in one document, and the"
    echo "      manual says two do:"
    printf '%s\n' "$said" | sed 's/^/      /' | head -8
    exit 1
fi
pag_said=$(tr '\n' ' ' < "$work/manual" | tr -s ' ' \
    | sed -n 's/.*The \([a-z]*\) paginated container formats, \([^;]*\), hold every page.*/\1|\2/p')
if [ -z "$pag_said" ]; then
    echo "FAILURES: doc/MANUAL's sentence naming the paginated formats could"
    echo "      not be read; the shape this guard reads for has changed."
    echo "      Fix the guard."
    exit 1
fi
printf '%s\n' "${pag_said#*|}" | tr ',' ' ' | sed 's/ and / /g' \
    | tr ' ' '\n' | grep . | sort -u > "$work/manual-paginated"
if ! cmp -s "$work/manual-paginated" "$work/paginated"; then
    echo "FAIL: doc/MANUAL names these devices as holding a run's pages in"
    echo "      one file and the interpreter's devices say those:"
    echo "      manual: $(tr '\n' ' ' < "$work/manual-paginated")"
    echo "      devices: $(tr '\n' ' ' < "$work/paginated")"
    fail=1
fi
want=$(numword "$(wc -l < "$work/paginated" | tr -d ' ')")
if [ "${pag_said%%|*}" != "$want" ]; then
    echo "FAIL: doc/MANUAL calls them \"the ${pag_said%%|*} paginated container"
    echo "      formats\" and the devices say there are $want"
    fail=1
fi

# The roster is sealed, and so is each maker in it. An entry is a
# procedure a page-device request runs to build the device it names, so a
# roster or a maker a program could write into is one that decides what
# runs as the device. A procedure's access is the reference's own rather
# than the object's, so a maker is sealed by putting the sealed one back
# before the roster itself is sealed; a `readonly` whose answer is
# discarded seals nothing.
if ! grep -q '\.devicemakers get exch 2 copy get readonly put' "$init_ps"; then
    echo "FAIL: the device makers are not sealed into the roster, so a"
    echo "      program reaching one could write the body a page-device"
    echo "      request runs to build its device"
    fail=1
fi
if ! grep -q '\.devicemakers get readonly pop' "$init_ps"; then
    echo "FAIL: the device roster is not declared read-only, so a program"
    echo "      reaching it could name a procedure of its own as a device"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: the device rosters disagree"
    exit 1
fi

skipped=''
[ -s "$work/unmade" ] &&
    skipped=", $(wc -l < "$work/unmade" | tr -d ' ') not built into this interpreter"
echo "SUCCESS ($(wc -l < "$work/maker" | tr -d ' ') devices, one roster in four files and in the manual;\
 $(wc -l < "$work/says-yes" | tr -d ' ') declaring a banded page and routed for one;\
 $(wc -l < "$work/paginated" | tr -d ' ') holding a run's pages in one file;\
 $(wc -l < "$work/wrote" | tr -d ' ') leaving a page at the output path and\
 $(wc -l < "$work/left" | tr -d ' ') leaving none;\
 $(wc -l < "$work/buffer-saw" | tr -d ' ') assembling a page of their own pixels\
 in compiled code$skipped)"
exit 0
