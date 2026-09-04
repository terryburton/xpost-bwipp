#!/bin/sh
# Meson test wrapper: the byte-identity render gate. Render the golden
# page (golden_page.ps) through every deterministic output device and
# require each output's sha256 to match tests/golden/manifest.sha256.
#
# This is the instrument behind every "no behavioral change" refactor:
# a restructuring that claims zero cost must leave these bytes exactly
# as they were.
#
# WHICH DEVICES. Every device the roster names, less two kinds, and the
# roster is what settles it rather than a list here: a list of its own
# was how a device came to be registered in every place the framework
# asks for and compared in none of them, since nothing in the tree said
# it was missing from this one.
#
#   The devices whose page never arrives at the output path, which have
#   no bytes here to compare -- the two that hand their raster to an
#   embedding program and the two that paint nothing. That is
#   DEVICE_FLEET_NOFILE, held by check-device-roster.sh against what each
#   device leaves there.
#
#   The devices that need a library the build may not have, which is
#   DEVICE_FLEET_OPTIONAL. What such a build may be missing is the
#   library that writes the page, so those bytes are that library's
#   version rather than this interpreter's arithmetic, and a manifest of
#   them would report a drift on every machine with a different one.
#   Their pages are held by the raster-formats and band-writer wrappers
#   instead, which read what the bytes decode to.
#
# A device outside neither is rendered, and a device rendered and absent
# from the manifest fails below -- so a device added to the roster
# arrives here on the day it is added and says so.
#
# THREE ROUTES, not one. A paginated device carries state between pages,
# and which state it carries depends on where the document ends, so each
# of the three ways a job can end up on disk is a route of its own and
# each is held here.
#
#   <device>       the single page to a fixed output name
#   <device>-all   the three-page job to a fixed output name: one
#                  document opened once and finalised at the end of the
#                  run, with three pages written into it
#   <device>%d     the three-page job to a name carrying a %d, which
#                  opens and finalises a document per page
#
# A page after the first is where all of this shows. What the writer
# restates at a page end -- the matrix its marks are made under, the
# selections its stream carries, the descriptions its content calls --
# is written once for the first page and has to be written again for
# each page after it, and a device that leaves one out produces a page
# that is well formed and wrong. Held on one route and not the others,
# such a page ships.
#
# Regenerate after an INTENDED rendering change (declare it in the same
# commit) with:
#     tests/run-golden-render.sh <xpost> <page.ps> <pages.ps> <goldendir> --regen
#
#   $1  path to the built xpost binary
#   $2  path to golden_page.ps
#   $3  path to golden_pages.ps, the multi-page job
#   $4  path to the tests/golden directory (the manifest's home)
#   $5  optionally --regen
set -u
xpost=$1
page=$2
pages=$3
golden=$4
regen=${5:-}
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

devices=
skipped=
for dev in $DEVICE_FLEET_ALL; do
    case " $DEVICE_FLEET_NOFILE " in
        *" $dev "*) skipped="$skipped $dev(no file)"; continue ;;
    esac
    case " $DEVICE_FLEET_OPTIONAL " in
        *" $dev "*) skipped="$skipped $dev(library bytes)"; continue ;;
    esac
    devices="$devices $dev"
done
if [ -z "$devices" ]; then
    echo "FAILURES: the roster left no device to render, so this gate would"
    echo "      compare nothing and report the bytes held"
    exit 1
fi
# named, so that what is not held here is read rather than inferred from
# a shorter report
echo "NOTE not held to the byte here:$skipped"

# The two object widths are two personalities, and they do not render
# byte for byte alike: the wide build's integers reach further, so
# arithmetic that leaves the narrow build's range as a real stays exact
# there and rounds a colour differently. Each personality is held to its
# own manifest rather than one being declared the right answer.
probe=$(mktemp)
cat > "$probe" <<'PROBEEOF'
2147483647 1 add type /integertype eq
    { (XPOSTWIDTH=wide) }{ (XPOSTWIDTH=narrow) } ifelse =
quit
PROBEEOF
width=$("$xpost" -q --no-sandbox -d null "$probe" </dev/null 2>/dev/null \
        | grep -o 'XPOSTWIDTH=[a-z]*' | head -1 | cut -d= -f2)
rm -f "$probe"
case $width in
    wide)   manifest="$golden/manifest-large.sha256" ;;
    narrow) manifest="$golden/manifest.sha256" ;;
    *)      echo "FAILURES: could not tell which object width this build has"
            exit 1 ;;
esac

if command -v sha256sum >/dev/null 2>&1; then
    sum() { sha256sum "$1" | cut -d' ' -f1; }
else
    sum() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0
out_manifest="$work/manifest.sha256"

for dev in $devices; do
    out="$work/golden.$dev"
    err=$("$xpost" -q $ns -d "$dev" -o "$out" "$page" </dev/null 2>&1)
    status=$?
    verdict_run "$status" "$err" "$dev" || exit 1
    if printf '%s' "$err" | grep -q '%%\[ Error'; then
        echo "FAIL $dev: $(printf '%s' "$err" | grep '%%\[ Error' | head -1)"
        fail=1
        continue
    fi
    if [ ! -s "$out" ]; then
        echo "FAIL $dev: produced no output"
        fail=1
        continue
    fi
    printf '%s  %s\n' "$(sum "$out")" "$dev" >> "$out_manifest"
done

# ---- the same devices through the one-document multi-page route ----
#
# The same three pages the per-page route below writes as three files,
# written here into the one document a run opens once. What differs
# between the two is where a document starts and ends, which is exactly
# what a writer's per-page state is measured against.
for dev in $devices; do
    out="$work/golden.$dev-all"
    err=$("$xpost" -q $ns -d "$dev" -o "$out" "$pages" </dev/null 2>&1)
    status=$?
    verdict_run "$status" "$err" "$dev-all" || exit 1
    if printf '%s' "$err" | grep -q '%%\[ Error'; then
        echo "FAIL $dev-all: $(printf '%s' "$err" | grep '%%\[ Error' | head -1)"
        fail=1
        continue
    fi
    if [ ! -s "$out" ]; then
        echo "FAIL $dev-all: produced no output"
        fail=1
        continue
    fi
    printf '%s  %s\n' "$(sum "$out")" "$dev-all" >> "$out_manifest"
done

# ---- the same devices through the per-page route ----
#
# The pages are joined in order and hashed as one, so a device answers
# for its whole run in one line and a page that went missing, arrived
# twice, or came out in the wrong order changes the hash rather than
# being averaged away. The count is reported beside a failure, because
# "the bytes differ" and "there are two files where there were three"
# are different faults and the hash alone cannot tell them apart.
mkdir -p "$work/pp"
for dev in $devices; do
    mkdir -p "$work/pp/$dev"
    err=$("$xpost" -q $ns -d "$dev" -o "$work/pp/$dev/p%d" "$pages" </dev/null 2>&1)
    status=$?
    verdict_run "$status" "$err" "$dev%d" || exit 1
    if printf '%s' "$err" | grep -q '%%\[ Error'; then
        echo "FAIL $dev%d: $(printf '%s' "$err" | grep '%%\[ Error' | head -1)"
        fail=1
        continue
    fi
    order=$(ls "$work/pp/$dev" 2>/dev/null | sed 's/^p//' | sort -n)
    npages=$(printf '%s\n' "$order" | grep -c '[0-9]')
    if [ "$npages" -lt 2 ]; then
        echo "FAIL $dev%d: a %d output name asks for a file per page and a"
        echo "     three-page job left $npages of them, so this route is not"
        echo "     being exercised at all"
        fail=1
        continue
    fi
    joined="$work/golden.$dev%d"
    : > "$joined"
    for p in $order; do cat "$work/pp/$dev/p$p" >> "$joined"; done
    printf '%s  %s\n' "$(sum "$joined")" "$dev%d" >> "$out_manifest"
done

if [ "$fail" -ne 0 ]; then
    rm -rf "$work"
    echo "FAILURES: a device did not render the golden page"
    exit 1
fi

if [ "$regen" = "--regen" ]; then
    mkdir -p "$golden"
    {
        echo "# platform $(uname -s 2>/dev/null)-$(uname -m 2>/dev/null)"
        cat "$out_manifest"
    } > "$manifest"
    echo "regenerated $manifest:"
    cat "$manifest"
    rm -rf "$work"
    exit 0
fi

if [ ! -s "$manifest" ]; then
    rm -rf "$work"
    echo "FAILURES: no usable manifest at $manifest (run with --regen to create)"
    exit 1
fi

# the manifest must cover every device rendered, or a truncated file would
# silently reduce the gate to whatever lines survived
for dev in $devices; do
    if ! grep -q " $dev\$" "$manifest"; then
        echo "FAIL: $dev is rendered but absent from the manifest"
        fail=1
    fi
    if ! grep -q " $dev-all\$" "$manifest"; then
        echo "FAIL: $dev is rendered through the one-document multi-page"
        echo "      route and absent from the manifest, so that route holds"
        echo "      no bytes"
        fail=1
    fi
    if ! grep -q " $dev%d\$" "$manifest"; then
        echo "FAIL: $dev is rendered through the per-page route and absent"
        echo "      from the manifest, so that route holds no bytes"
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    rm -rf "$work"
    echo "FAILURES: the manifest does not cover the rendered devices"
    exit 1
fi

# Which devices are held to the byte.
#
# The raster devices are: every byte of a raster is this interpreter's
# own arithmetic, and it comes out the same wherever that arithmetic
# runs -- measured, not assumed.
#
# The vector writers are held to the byte only where the manifest was
# made. What they write is text, and two parts of it are the platform's
# rather than this interpreter's: a number is rendered to digits by the
# C library, and a compressed stream is packed by whatever zlib is
# linked. Both produce output that is correct and not identical. Holding
# them to the byte elsewhere reports a drift on every run and teaches a
# reader to discount the report; leaving them out entirely gives up a
# gate that does catch drift where it is meaningful. So they are
# compared where the comparison means something and named where it does
# not.
platform=$(uname -s 2>/dev/null)-$(uname -m 2>/dev/null)
reference=$(sed -n 's/^# platform //p' "$manifest" 2>/dev/null)
if [ -n "$reference" ] && [ "$platform" != "$reference" ]; then
    byte_exact_only_raster=1
else
    byte_exact_only_raster=0
fi

# compare per device so a mismatch names its device
while read -r want dev; do
    case $dev in
        pdfwrite|svgwrite|dscwrite|pdfwrite-all|svgwrite-all|dscwrite-all|\
        pdfwrite%d|svgwrite%d|dscwrite%d)
            if [ "$byte_exact_only_raster" = 1 ]; then
                echo "NOTE $dev: not held to the byte on $platform;"
                echo "     the manifest was made on $reference, and what this"
                echo "     writer emits is partly the platform's own"
                continue
            fi ;;
    esac
    got=$(grep " $dev\$" "$out_manifest" | cut -d' ' -f1)
    if [ "$got" != "$want" ]; then
        echo "FAIL $dev: rendered bytes differ from the golden manifest"
        # A hash says only that two byte strings differ. Show where the
        # rendered one parts from what a reader of this report can see,
        # so a drift in how a number is written is told apart from a
        # drift in what was drawn.
        rendered="$work/golden.$dev"
        if [ -r "$rendered" ]; then
            echo "     bytes: $(LC_ALL=C wc -c < "$rendered" 2>/dev/null), and it opens:"
            LC_ALL=C dd if="$rendered" bs=1 count=160 2>/dev/null \
                | LC_ALL=C tr -c '[:print:]' '.' | sed 's/^/       /'
            echo ""
        fi
        fail=1
    else
        echo "OK   $dev"
    fi
done <<EOF
$(grep -v '^#' "$manifest")
EOF

rm -rf "$work"
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: rendered output drifted from the golden bytes"
    exit 1
fi
echo "SUCCESS"
exit 0
