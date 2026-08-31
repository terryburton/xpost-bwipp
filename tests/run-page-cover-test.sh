#!/bin/sh
# Meson test wrapper: what a fill meant to cover a whole surface covers.
#
# A marking method's extent operands name an inclusive pixel span, so a
# caller that owns a surface's size hands the far corner, one less than
# the size (the driver contract, src/lib/xpost_dev_driver.h).
# page_cover_test.ps asks that of the operands the page clear hands the
# device, once per marking implementation.
#
# The operands are where the question has to be asked, because a raster
# device answers it the same however it is put: it clips the surplus
# column and row away and its raster is identical either way. A vector
# writer has no pixels to clip against -- it converts the inclusive span
# to a half-open rectangle and writes it out -- so the surplus reaches
# the document, and the documents are read here as well. svgwrite writes
# its own rectangle; dscwrite is data/pdfwrite.ps with the page writers
# replaced and inherits that device's FillRect whole, so it witnesses
# pdfwrite's rectangle in a stream that is not compressed.
#
#   $1  path to the built xpost binary
#   $2  path to page_cover_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

# devices whose page clear reaches a FillRect at all; the rest answer
# through an Erase method of their own and are asked nothing
covered_min=10
covered=0

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

for dev in $DEVICE_FLEET_MARKING; do
    out=$("$xpost" -q $ns -d "$dev" -o "$work/out.$dev" "$script" </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "SKIP $dev (not built in)"; continue ;;
    esac
    if [ "$st" -ne 0 ]; then
        echo "FAIL $dev: the interpreter exited with status $st"
        fail=1
        continue
    fi
    # the showpage banner ends without a newline, so the marker can share
    # a line with it; what the rule needs is that the word is the run's
    # own and not the tail of a longer one
    if printf '%s\n' "$out" | grep -qE '^COVERED$|[^A-Za-z]COVERED$'; then
        covered=$((covered + 1))
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

# --- what the vector writers wrote -----------------------------------
#
# A page of odd, unequal dimensions: an extent that happens to agree
# with a size on one page disagrees on every other, and neither number
# can stand in for the other by symmetry.
pw=201
ph=101
cat > "$work/erase.ps" <<PSEOF
<< /PageSize [$pw $ph] >> setpagedevice
erasepage
showpage
quit
PSEOF

# every rectangle in an erased document is a page clear, so all of them
# must be the page; the complaint names the ones that are not
out=$("$xpost" -q $ns -d svgwrite -o "$work/a.svg" "$work/erase.ps" </dev/null 2>&1)
if verdict_run "$?" "$out" "the svgwrite run"; then
    if [ ! -s "$work/a.svg" ]; then
        echo "FAIL svgwrite: no document"
        fail=1
    else
        rects=$(grep -ao 'width="[0-9.]*" height="[0-9.]*"' "$work/a.svg")
        stray=$(printf '%s\n' "$rects" \
                | grep -v "^width=\"$pw\" height=\"$ph\"$" | grep .)
        if [ -z "$rects" ]; then
            echo "FAIL svgwrite: the erased page carries no rectangle"
            fail=1
        elif [ -n "$stray" ]; then
            echo "FAIL svgwrite: a page clear is not the page ($pw by $ph):"
            printf '%s\n' "$stray" | sed 's/^/      /'
            fail=1
        else
            echo "OK   svgwrite document"
        fi
    fi
else
    fail=1
fi

out=$("$xpost" -q $ns -d dscwrite -o "$work/a.ps" "$work/erase.ps" </dev/null 2>&1)
if verdict_run "$?" "$out" "the dscwrite run"; then
    if [ ! -s "$work/a.ps" ]; then
        echo "FAIL dscwrite: no document"
        fail=1
    else
        rects=$(grep -ao '^[0-9.]* [0-9.]* [0-9.]* [0-9.]* re$' "$work/a.ps")
        stray=$(printf '%s\n' "$rects" | grep -v "^0 0 $pw $ph re$" | grep .)
        if [ -z "$rects" ]; then
            echo "FAIL dscwrite: the erased page carries no rectangle"
            fail=1
        elif [ -n "$stray" ]; then
            echo "FAIL dscwrite: a page clear is not the page ($pw by $ph):"
            printf '%s\n' "$stray" | sed 's/^/      /'
            fail=1
        else
            echo "OK   dscwrite document"
        fi
    fi
else
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page clear did not cover the page"
    exit 1
fi
if [ "$covered" -lt "$covered_min" ]; then
    echo "FAILURES: the operand check ran on $covered devices, fewer than $covered_min"
    echo "      a device whose page clear stops reaching FillRect silently"
    echo "      stops being asserted about"
    exit 1
fi
echo "SUCCESS ($covered devices answered for their page clear)"
exit 0
