#!/bin/sh
#
# One family, asked as a family: every device that can hand its page to
# an embedder is on the roster of the test that holds the handoff.
#
# A run started with XPOST_OUTPUT_BUFFEROUT stores its finished page
# through an address the embedder gave, and from then on the page is the
# embedder's: it outlives the context, Destroy leaves it alone, and
# xpost_output_buffer_release() is the only thing that gives it back.
# That is one contract, and it belongs to every device that keeps a page
# in a buffer of its own -- but it was written device by device and
# tested device by device, so two of the four then compiled freed a
# handed-over page at Destroy and nothing said so. The divergence was
# visible in a grep and nothing was grepping, because nothing was asking
# the question of the family.
#
# What this holds is the membership, so that the question cannot be asked
# of a shorter family than there is. The runtime answers are
# tests/output_buffer_release_test.c's, which puts every member to every
# one of them.
#
# Membership is read off the sources rather than listed:
#
#   data/init.ps names, for each device a page-device request can make,
#   the maker that makes it. A device with no maker there is a device
#   nothing can select.
#
#   A library source registers that maker as an operator, which is what
#   ties a device name to the driver behind it. Two names may share one
#   driver -- the alpha device is the same C with a flag -- and both are
#   members, because both hand a page over.
#
#   A driver hands a page over by calling the one helper that stores
#   through the embedder's address. A driver that calls it is in the
#   family; one that does not is not, whatever it is otherwise.
#
# and the roster the test carries is held to what that reading produces,
# both ways: a driver that gains a handoff and is not on the roster
# fails, and a name on the roster whose driver hands nothing over fails
# too.
#
# Sources are read by name rather than by scanning a directory: a built
# tree leaves object files beside them whose debug information matches
# every pattern here, so a directory scan reads green where nothing was
# built and red where something was.
#
# Also held: the release entry point finds the block a page sits in by
# stepping one pointer back from the page, so every driver in the family
# has to keep the block's address there. Each states that as a
# compile-time assertion beside the buffer it declares. A driver that
# hands a page over without one is a driver whose layout holds by luck,
# and the day it stops holding, the embedder frees an address the
# allocator never returned.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-buffer-family.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
# read a tree whose lines end where the scans below expect them to
guard_mirror_tree "$src"
src=$mirror

init_ps="$src/data/init.ps"
libdir="$src/src/lib"
roster_c="$src/tests/output_buffer_release_test.c"
driver_h="$libdir/xpost_dev_driver.h"

guard_require_file "$init_ps" "the interpreter's PostScript"
guard_require_dir "$libdir" "the library source directory"
guard_require_file "$roster_c" "the buffer-family test"
guard_require_file "$driver_h" "the device driver contract"

# The helper a driver hands a page over through, and the shape of the
# assertion that keeps the block in front of the page. Read from the
# contract header rather than spelled here, so a rename moves both the
# rule and this check at once.
handoff=xpost_dev_output_buffer_handoff
if ! grep -q "$handoff" "$driver_h"; then
    echo "FAILURES: $handoff() is not in the device driver contract;"
    echo "      the handoff has been renamed and this guard reads for a name"
    echo "      no driver calls, which would pass over the whole family"
    exit 1
fi

fail=0

# ---- the devices a page-device request can make, and their makers ----
# An entry runs to the brace that closes it, over as many lines as it
# takes, and the maker it names may be on any of them. Read a line at a
# time and the entries whose maker is not on their opening line are
# skipped in silence -- which is a guard reporting about half the fleet
# while reading as though it covered all of it.
awk '/\.devicemakers *<</ { in_t = 1; next }
     in_t && />> *put/    { in_t = 0 }
     in_t && /^ *\/[a-z]/ { name = $1; sub(/^\//, "", name); ent = ""; depth = 0 }
     in_t && name != "" {
         ent = (ent == "") ? $0 : ent " " $0
         code = $0
         sub(/%.*$/, "", code)
         depth += gsub(/\{/, "{", code) - gsub(/\}/, "}", code)
         if (depth > 0) next
         # the maker is the last maker name the entry carries -- spelled
         # as a literal name, the entry reaching it in the private
         # dictionary rather than executing it bare
         maker = ""
         m = split(ent, w, /[ \t]+/)
         for (i = 1; i <= m; i++)
             if (w[i] ~ /^\/?new[A-Za-z0-9_]+device$/) {
                 maker = w[i]; sub(/^\//, "", maker)
             }
         if (maker != "") print name, maker
         name = ""
     }' "$init_ps" | sort -u > "$work/makers"

if [ ! -s "$work/makers" ]; then
    echo "FAILURES: no device makers could be read from data/init.ps"
    echo "      the shape the guard reads for has changed; fix the guard"
    exit 1
fi

# ---- the library source that registers each maker ----
#
# Named rather than globbed, for the reason given above. Every driver in
# the tree is here; one added and left out is one whose handoff this
# guard would never see, which is what the count below catches.
drivers="xpost_dev_bgr.c xpost_dev_jpeg.c xpost_dev_png.c
         xpost_dev_raster.c xpost_dev_xcb.c xpost_dev_win32.c"
for f in $drivers; do
    guard_require_file "$libdir/$f" "a device driver"
done

# maker -> driver, from the operator registration
: > "$work/registered"
for f in $drivers; do
    sed -n 's/.*xpost_operator_cons([^,]*, *"\(new[A-Za-z0-9_]*device\)".*/\1/p' \
        "$libdir/$f" | sort -u | sed "s|\$| $f|" >> "$work/registered"
done
if [ ! -s "$work/registered" ]; then
    echo "FAILURES: no device maker is registered by any driver;"
    echo "      the registration no longer looks as this guard expects"
    exit 1
fi

# A driver may hand the page over itself, or reach it through something
# shared that hands it over on the driver's behalf.
#
# WHICH shared things is DERIVED, not named. A guard that knows one name
# says a driver stopped handing its page over on the day the call moves
# into a second shared place -- which is not a device changing behaviour
# but a refactor moving a line, and a guard that cannot tell those apart
# forbids the refactor. So every function in the shared source whose own
# body makes the call is found, and a driver reaching any of them counts.
#
# It is still not taken on trust: the name has to be reached FROM the
# driver AND the body behind it has to make the call, which is what
# reading the bodies establishes. A shared writer that quietly stopped
# handing pages over takes no driver with it, because it would not be
# in this set.
via_src=xpost_dev_generic.c
guard_require_file "$libdir/$via_src" "the shared page writer"
awk -v H="$handoff" '
    /^[A-Za-z_][A-Za-z0-9_ \t\*]*\(/ && !/;[ \t]*$/ {
        line = $0
        if (match(line, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
            nm = substr(line, RSTART, RLENGTH)
            sub(/[ \t]*\($/, "", nm); sub(/\(/, "", nm)
            cur = nm
        }
    }
    index($0, H "(") || index($0, H " (") { if (cur != "") print cur }
' "$libdir/$via_src" | sort -u > "$work/vias"

if [ ! -s "$work/vias" ]; then
    echo "FAILURES: no function in $via_src calls $handoff(), so nothing"
    echo "      shared hands a page to an embedder. Either the handoff has"
    echo "      moved out of the shared source or this guard has stopped"
    echo "      being able to read it, and neither may be passed over"
    exit 1
fi

# the drivers that hand a page to an embedder, themselves or through it
: > "$work/handers"
for f in $drivers; do
    if grep -q "$handoff *(" "$libdir/$f"; then
        echo "$f" >> "$work/handers"
    else
        while read -r via; do
            if grep -q "$via *(" "$libdir/$f"; then
                echo "$f" >> "$work/handers"
                break
            fi
        done < "$work/vias"
    fi
done
if [ ! -s "$work/handers" ]; then
    echo "FAILURES: no driver calls $handoff();"
    echo "      either the handoff has moved or the family is empty, and"
    echo "      neither is something this guard may pass over"
    exit 1
fi

# ---- the family: a device whose driver hands its page over ----
: > "$work/derived"
while read -r name maker; do
    [ -n "$maker" ] || continue
    driver=$(awk -v M="$maker" '$1 == M { print $2 }' "$work/registered")
    [ -n "$driver" ] || continue
    for d in $driver; do
        if grep -qx "$d" "$work/handers"; then
            echo "$name" >> "$work/derived"
        fi
    done
done < "$work/makers"
sort -u "$work/derived" -o "$work/derived"

if [ ! -s "$work/derived" ]; then
    echo "FAILURES: no device was derived into the buffer family, and the"
    echo "      drivers that hand a page over are:"
    sed 's/^/      /' "$work/handers"
    echo "      the maker-to-driver reading has broken; fix the guard"
    exit 1
fi

# ---- the roster the test carries ----
#
# The members table, read for the device name each entry opens with. The
# entries a build compiles are decided inside the test by a value on each
# row, not by fencing rows out of the table, so every member is here to
# be read whatever this build has.
awk '/^static const Member members\[\] *=/ { in_t = 1; next }
     in_t && /^};/                        { in_t = 0 }
     in_t && /^ *\{ *"/ {
         line = $0
         sub(/^[^"]*"/, "", line)
         sub(/".*$/, "", line)
         if (line != "") print line
     }' "$roster_c" | sort -u > "$work/roster"

if [ ! -s "$work/roster" ]; then
    echo "FAILURES: no members could be read from tests/output_buffer_release_test.c"
    echo "      the table no longer looks as this guard expects; fix the guard"
    exit 1
fi

guard_held=0
guard_hold "$work/derived" "$work/roster" \
    "these devices hand their page to an embedder and the family test does
      not ask them. Add each to the members table in
      tests/output_buffer_release_test.c, with the pixel its unmarked page
      holds:" \
    "the family test asks these devices and no driver hands their page to
      an embedder. Either the device stopped handing a page over, in which
      case take it off the table, or its maker is no longer readable from
      data/init.ps:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the block in front of the page ----
for f in $(cat "$work/handers"); do
    if ! grep -q 'XPOST_DEV_ASSERT_BLOCK_PRECEDES_RASTER' "$libdir/$f"; then
        echo "FAILURES: $f hands a page to an embedder and does not assert"
        echo "      that the block it sits in is named immediately in front"
        echo "      of it. xpost_output_buffer_release() reads the block from"
        echo "      there, so the layout is the contract and not an accident;"
        echo "      declare the assertion beside the buffer, as the other"
        echo "      drivers do."
        fail=1
    fi
done

# ---- the programs in this tree that take a page ----
#
# The release is the only thing that gives a page back, so a program that
# takes one and does not call it leaks a page per run. That is the rule,
# and the exception is named with its reason rather than left to be
# noticed: the viewer takes one page for a window it then lives in and
# exits, so what it holds at the end the process gives back anyway.
#
# Held both ways. A program that starts taking a page and does not give
# it back fails, and so does one named here that has started giving it
# back -- the reason beside its name has stopped being true, and the rule
# it was excused from is one it now keeps.
leaks_one_page='xpost_view.c'

bindir="$src/src/bin"
guard_require_dir "$bindir" "the programs directory"
for p in "$bindir"/*.c; do
    [ -f "$p" ] || continue
    grep -q 'XPOST_OUTPUT_BUFFEROUT' "$p" || continue
    base=$(basename "$p")
    if grep -q 'xpost_output_buffer_release' "$p"; then
        case " $leaks_one_page " in
            *" $base "*)
                echo "FAILURES: src/bin/$base gives a page back and is recorded"
                echo "      here as one that does not. Take it off the"
                echo "      exception list in this guard, so the list says what"
                echo "      is true."
                fail=1 ;;
        esac
    else
        case " $leaks_one_page " in
            *" $base "*) ;;
            *)  echo "FAILURES: src/bin/$base starts a run that hands it a page"
                echo "      and never calls xpost_output_buffer_release(),"
                echo "      which is the only call that gives one back. That is"
                echo "      a page leaked per run."
                fail=1 ;;
        esac
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "SUCCESS ($(grep -c . "$work/derived") devices hand a page over, all on the family roster)"
exit 0
