#!/bin/sh
# Meson test wrapper: render a MULTI-PAGE job through every file device and
# require the right multi-page shape.
#
# The job wraps each page in save...showpage...restore -- the separation-plate
# idiom -- which rewinds local virtual memory between pages. State a device
# keeps per page (a page counter, an open output file) must not be rewound with
# it, or later pages collide with the first.
#
# The whole sweep runs twice: once on a job that takes the device it started
# on, and once on a job that changes the page device first. The second is the
# ordinary shape of a real job, and it is a different case: setpagedevice
# retires one device and builds another, so every page is written by a device
# that arrived after the job began, and the first of them is emitted inside a
# save. Anything such a device leaves until its first page -- an output file it
# has not opened yet, say -- is opened inside that save and closed by its
# restore.
#
# Two shapes, by device:
#
#  * Paginated container formats default to ONE file holding every page; a %d
#    in the output name selects a file per page instead. So a plain
#    multi-showpage job to a fixed name yields a single N-page document, and the
#    same job to a %d name yields N one-page files.
#
#  * Everything else cannot hold more than one page in a file, so a %d gives a
#    file per page and a fixed name keeps the last page (every page rewrites the
#    one file). This is also what a single-page consumer -- which reads the file
#    right after showpage -- depends on.
#
# WHICH DEVICES, AND WHICH SHAPE. Both are read rather than listed. The roster
# is tests/device-fleet.sh's, so a device the interpreter gains is swept here
# the day it is added; which of them leave nothing at the output path is
# DEVICE_FLEET_NOFILE, held by check-device-roster.sh against what each device
# leaves there; and which of them hold a run's pages in one document is asked of
# the running interpreter, since a device that does carries the method that
# accumulates a page into an open one and a device that does not carries none.
# A list here was how a device came to be registered everywhere the framework
# asks for and swept in neither shape, with nothing in the tree saying so.
#
# The output name carries one extension for every device. What a device writes
# is the name it was handed, so the extension names nothing about the format and
# a per-device one would be a fourth roster to keep in step.
#
# The %d name is the only page-specific step in either shape, and it is taken
# once for every device, in .transmitpage (data/device.ps), so the numbering a
# compiled device produces is the numbering the PostScript devices produce. The
# open still goes through the ordinary `file` operator for a device written in
# PostScript, and through the one shared opener for a compiled one, so a device
# under the file-access sandbox is bound by the same rules a single-page write
# is either way.
#
# The devices that leave nothing at the output path -- the two that hand their
# raster to the embedder and the two that paint nothing -- are swept here too:
# "no file" is their answer and not an omission, and a job of any length must
# leave that answer unchanged.
#
# WHAT THIS DOES NOT COVER, plainly. Every reading below compares one of this
# run's outputs against another of them: the three %d pages must differ from
# each other, and the fixed-name file must equal the last of them. Not one page
# is held to anything recorded, so nothing here can tell a right page from a
# wrong one -- only a page numbered wrongly from a page numbered rightly. A
# change that corrupts every page in the same way passes here with the same
# report as a tree in good order, and a colour raster whose components were
# emitted in the wrong order did exactly that. What a page's bytes have to be
# is the golden-render manifest's, and what a raster format's pixels have to be
# is tests/run-raster-formats-test.sh's; this is the page counter's and the
# per-page file semantics', which is what it was written for.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

# One run below is started in the directory it writes to, so what this
# was handed has to name the same thing from there
xpost=$(path_anchor "$xpost")

# Reach the interpreter's data directory outside any sandbox root: disable the
# file-access sandbox when this build has one (detected from the usage text),
# so the test is valid at every point in the series.
ns=$(sandbox_flag "$xpost")

verdict_workdir
plain="$work/pages.ps"
# three pages, each a stroke at its own position, so the pages differ in content
printf '%s\n' \
    'save 10 10 moveto 40 40 lineto stroke showpage restore' \
    'save 40 40 moveto 70 70 lineto stroke showpage restore' \
    'save 70 10 moveto 95 35 lineto stroke showpage restore' > "$plain"

# the same job after a page device change. setpagedevice retires the device the
# job started on and builds another, so every page here is written by a device
# that arrived while the job was already running -- and the first of them is
# emitted inside a save. A device that leaves any part of its output to the
# first page is tying that part to the first page's restore.
pagedev="$work/pagesdev.ps"
{ printf '%s\n' '<< /PageSize [100 100] >> setpagedevice'; cat "$plain"; } > "$pagedev"

ext=out

# The devices whose page is a file, and the devices whose page is not. The
# second is the roster's answer; the first is the rest of it.
buffer_devices=$DEVICE_FLEET_NOFILE
file_devices=
for dev in $DEVICE_FLEET_ALL; do
    case " $buffer_devices " in *" $dev "*) continue ;; esac
    file_devices="$file_devices $dev"
done

# Which of them hold every page of a run in one document. Asked of a running
# interpreter rather than listed: a device that accumulates a page into an open
# document carries the method that does it, and one that writes each page whole
# carries none. Each is installed by a page-device request and its dictionary
# read, exactly as check-device-roster.sh reads what a device says about taking
# its page in bands.
ask="$work/paginated.ps"
{
    echo "["
    for dev in $file_devices; do echo "/$dev"; done
    cat <<'ASKEOF'
]
{ /D exch def
  { << /OutputDevice D /PageSize [ 8 8 ] >> setpagedevice } stopped
  { (PAGINATED ) print D 60 string cvs print ( unmade\n) print }
  { (PAGINATED ) print D 60 string cvs print ( ) print
    DEVICE /.emitpage known { (yes) }{ (no) } ifelse print (\n) print }
  ifelse
} forall
quit
ASKEOF
} > "$ask"
said=$( cd "$work" && "$xpost" -q $ns -d null -o paginated.scratch \
        paginated.ps </dev/null 2>&1 )
paginated=$(printf '%s\n' "$said" \
            | awk '$1 == "PAGINATED" && $3 == "yes" { printf " %s", $2 }')
if ! printf '%s\n' "$said" | grep -q '^PAGINATED '; then
    echo "FAILURES: the interpreter could not be asked which devices hold a"
    echo "      run's pages in one document:"
    printf '%s\n' "$said" | sed 's/^/      /' | head -8
    rm -rf "$work"
    exit 1
fi
if [ -z "$paginated" ]; then
    echo "FAILURES: no device holds a run's pages in one document, and two of"
    echo "      them do. The question is being asked wrong."
    rm -rf "$work"
    exit 1
fi

fail=0

render() {   # $1=device $2=output-path ; returns 1 on skip/error
    err=$("$xpost" -q $ns -d "$1" -o "$2" "$prog" </dev/null 2>&1)
    status=$?
    case "$err" in
        *"wrong device"*) return 1 ;;
    esac
    verdict_run "$status" "$err" "$1" || exit 1
    if printf '%s' "$err" | grep -q '%%\[ Error'; then
        echo "FAIL $1: $(printf '%s' "$err" | grep '%%\[ Error' | head -1)"
        fail=1
        return 1
    fi
    return 0
}

sweep() {   # $1=job label ; renders $prog through every device and checks the shape
    job=$1
    ran=0
    # The floor a sweep is held to: the roster less the members that need a
    # library the build may not have, which are the only ones entitled to
    # answer "wrong device".
    want=0
    for dev in $file_devices; do
        case " $DEVICE_FLEET_OPTIONAL " in *" $dev "*) continue ;; esac
        want=$((want + 1))
    done
    for dev in $file_devices; do
        case " $paginated " in
            *" $dev "*) pag=1 ;;
            *) pag=0 ;;
        esac
        rm -f "$work"/page_* "$work"/fixed.* 2>/dev/null

        # every device: a %d gives one file per page, all three distinct
        if ! render "$dev" "$work/page_%d.$ext"; then
            [ "$fail" -eq 0 ] && echo "SKIP $dev (not built in)"
            continue
        fi
        ran=$((ran + 1))
        p1="$work/page_1.$ext"; p2="$work/page_2.$ext"; p3="$work/page_3.$ext"
        n=$(ls "$work"/page_*."$ext" 2>/dev/null | wc -l)
        if [ "$n" -ne 3 ]; then
            echo "FAIL $dev ($job): %d gave $n file(s), want 3"; fail=1; continue
        fi
        for p in "$p1" "$p2" "$p3"; do
            [ -s "$p" ] || { echo "FAIL $dev ($job): $(basename "$p") missing or empty"; fail=1; }
        done
        [ "$fail" -ne 0 ] && continue
        if cmp -s "$p1" "$p2" || cmp -s "$p2" "$p3" || cmp -s "$p1" "$p3"; then
            echo "FAIL $dev ($job): %d page files are not all distinct (counter rewound?)"; fail=1; continue
        fi

        # fixed name (no %d)
        if ! render "$dev" "$work/fixed.$ext"; then
            echo "FAIL $dev ($job): fixed-name render failed"; fail=1; continue
        fi
        if [ "$pag" = 1 ]; then
            # one file holding all three pages, which is a claim about its
            # size before it is a claim about its structure: a document
            # carrying three pages is larger than any one of them.
            fsz=$(LC_ALL=C wc -c < "$work/fixed.$ext" | tr -d ' ')
            for p in "$p1" "$p2" "$p3"; do
                psz=$(LC_ALL=C wc -c < "$p" | tr -d ' ')
                if [ "$fsz" -le "$psz" ]; then
                    echo "FAIL $dev ($job): the one file is $fsz bytes and the page"
                    echo "     $(basename "$p") alone is $psz; three pages did not land in it"
                    fail=1
                fi
            done
            [ "$fail" -ne 0 ] && continue
            # ... and how the format says how many pages it holds, which is
            # the format's own and has to be read in the format's own terms.
            # A paginated device with no reading here fails rather than
            # passing on the size alone: a document whose page count says two
            # and whose bytes hold three is a document every reader of it
            # gets wrong, and that is exactly what no size can see.
            case "$dev" in
            pdfwrite)
                # the document says how many pages it has three times over, and a
                # reader believes whichever it consults, so all three must agree:
                # the page tree's count, the number of page objects, and the
                # number of children the tree names
                c=$(grep -aoE '/Count [0-9]+' "$work/fixed.$ext" | awk '{print $2}')
                [ "$c" = 3 ] || { echo "FAIL $dev ($job): page tree /Count $c, want 3"; fail=1; continue; }
                np=$(grep -ac '/Type /Page[^s]' "$work/fixed.$ext")
                [ "$np" = 3 ] || { echo "FAIL $dev ($job): $np page objects, want 3"; fail=1; continue; }
                nk=$(grep -aoE '/Kids *\[[^]]*\]' "$work/fixed.$ext" | head -1 \
                     | grep -oE '[0-9]+ 0 R' | wc -l | tr -d ' ')
                [ "$nk" = 3 ] || { echo "FAIL $dev ($job): page tree names $nk children, want 3"; fail=1; continue; }
                ;;
            dscwrite)
                np=$(grep -ac '^%%Page:' "$work/fixed.$ext")
                [ "$np" = 3 ] || { echo "FAIL $dev ($job): $np %%Page sections, want 3"; fail=1; continue; }
                grep -aq '^%%Pages: 3' "$work/fixed.$ext" || { echo "FAIL $dev ($job): no %%Pages: 3 trailer"; fail=1; continue; }
                ;;
            *)
                echo "FAIL $dev ($job): this device holds a run's pages in one"
                echo "     document and nothing here reads how many that document"
                echo "     says it holds. Say it in this device's format, beside"
                echo "     the two above."
                fail=1
                continue
                ;;
            esac
            echo "OK   $dev ($job: one file, three pages; %d gives three files)"
        else
            # last page stands in the one file
            cmp -s "$work/fixed.$ext" "$p3" || { echo "FAIL $dev ($job): fixed-name output is not the last page"; fail=1; continue; }
            echo "OK   $dev ($job: one page per file via %d; fixed name = last page)"
        fi
    done

    # A sweep that skipped from end to end renders nothing, compares nothing
    # and leaves every verdict untaken, which is what a sweep that passed
    # leaves too. Everything but the optional members is built from this
    # tree, so "not built in" from one of those is a build to fix.
    if [ "$ran" -lt "$want" ]; then
        echo "FAIL ($job): $ran devices rendered and at least $want are built"
        echo "     from this tree; the rest said they were not built in"
        fail=1
    fi
}

# The devices that own no file: a job of any length leaves nothing at the
# output path, with a %d in it or without. Two of them hand their raster to
# the embedder and two paint nothing at all, so this is their answer and not
# a page that went missing.
buffersweep() {   # $1=job label
    job=$1
    for dev in $buffer_devices; do
        for name in "$work/buf_%d.out" "$work/buf.out"; do
            rm -f "$work"/buf*.out
            err=$("$xpost" -q $ns -d "$dev" -o "$name" "$prog" </dev/null 2>&1)
            status=$?
            case "$err" in
                *"wrong device"*) echo "SKIP $dev (not built in)"; continue ;;
            esac
            verdict_run "$status" "$err" "$dev" || exit 1
            left=$(ls "$work"/buf*.out 2>/dev/null | wc -l | tr -d ' ')
            if [ "$left" -ne 0 ]; then
                echo "FAIL $dev ($job): wrote $left file(s) for a device the roster"
                echo "     names as leaving nothing at the output path"
                fail=1
            fi
        done
        rm -f "$work"/buf*.out
        [ "$fail" -eq 0 ] && echo "OK   $dev ($job: writes no file, whatever the name)"
    done
}

prog=$plain;   sweep pages;                       buffersweep pages
prog=$pagedev; sweep 'pages after setpagedevice'; buffersweep 'pages after setpagedevice'

rm -rf "$work"
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: multi-page output regressed"
    exit 1
fi
echo "SUCCESS"
exit 0
