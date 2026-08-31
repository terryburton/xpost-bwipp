#!/bin/sh
# Meson test wrapper: every raster class that assembles its own page puts
# it out a band at a time, and the page it assembles is the page it
# paints whole.
#
# WHICH DEVICES. The roster is derived, not listed. It is the devices
# that say their page may arrive a band at a time -- DEVICE_FLEET_BANDS,
# held by check-device-roster.sh against the C table the selection is
# rewritten from and against the recording class's own roster -- less the
# ones whose page is assembled in compiled code rather than by the class.
# Which those are is asked of a running interpreter: a device whose Emit
# is an operator writes its page from C, so the class's row writer this
# run counts calls into is never reached, and there is nothing here to
# read. Those are the PNG writers and the JPEG one, and their bands are
# held by tests/run-band-writer-test.sh, which weighs the process instead
# and decodes what the bytes come to.
#
# A list of two names is what stood here, and for as long as it did the
# grayscale and colour classes put out no banded page in this wrapper at
# all -- each was covered by another test for its own reasons, and
# nothing in the tree said which formats this one had left out.
#
# What the four devices bring to it, one at a time:
#
#   The bilevel class packs its rows to bits, and every grey it stores
#   passes through the halftone cell under the pixel. A screen phased on
#   the band rather than on the page dithers differently from a page
#   painted whole -- and does so invisibly at any band deep enough to
#   hold a cell and the marks either side of it. So the pages here are
#   put out in bands of one row as well as in deep ones: at one row a
#   band, every row of the page is a boundary between bands and a screen
#   phased on anything but the page shows it.
#
#   The TIFF class writes a header naming where the strip begins and how
#   long it runs before a row of it exists. The page stays one strip
#   whatever it arrives in, so nothing in the file says where the bands
#   fell; what is asked here is that the promise made ahead of the rows
#   is the one the rows keep, which the byte comparison answers exactly.
#
#   The grayscale and colour classes carry the writers those two are
#   derived from, and a row of each is a different shape -- one byte a
#   pixel against three planes of one. What a row costs is what the band
#   budget is divided by, so the two are the two ends of the arithmetic
#   the bound below is read against.
#
# Three claims, and no two of them imply each other:
#
#   The bytes. Each page put out in bands is the page put out whole,
#   byte for byte, at several band heights including one row.
#
#   The bound. A loop that quietly held the whole page anyway paints the
#   same bytes, so no comparison of pages can see it. What can is the
#   growth of the global half of virtual memory across the page, which
#   the run reports for both routes at two page heights: the whole-page
#   route is required to grow with the page, since a measurement that
#   saw nothing would pass everything, and the banded route by a small
#   fraction of that.
#
#   The rows reached the writer as the bands finished. A device that
#   collected the bands and wrote the page at the end would pass both of
#   the above -- the file would be right and the raster would be a band.
#   So the run counts the calls into the class's row writer and the rows
#   in them: a page put out in n bands reaches the writer in n pieces
#   carrying one page's worth of rows between them, and a page put out
#   whole reaches it in one.
#
#   $1  path to the built xpost binary
#   $2  path to band_format_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

# The runs below are started in the directory the pages are written to,
# so what they were handed has to name the same thing from there.
xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

check_device() {  # $1 device
    dev=$1
    # The subject is the raster class's own band loop, which the run
    # drives by hand through the device's .moveband. Selecting a device
    # by name selects the record in front of it, and a record carries no
    # such method: the device is asked for as the mode that holds the
    # page whole, and the run divides that page itself.
    out=$( cd "$work" && "$xpost" -q $ns -d "$(fleet_whole "$dev")" \
           -o unused.out "-DDEVNAME=/$dev" "$script" </dev/null 2>&1 )
    st=$?
    verdict_run "$st" "$out" "the $dev run" || { fail=1; return; }

    # The run puts its pages out through the device rather than through
    # showpage, so nothing here arrives on the end of that operator's
    # banner and the lines are read as they were printed. That matters
    # because the names below carry the page and band heights in them.
    lines=$out
    field() { printf '%s\n' "$lines" | sed -n "s/^$1 //p"; }

    width=$(field PAGE | head -1)
    said=$(field DEVICE | head -1)
    rowbytes=$(field ROWBYTES | head -1)
    if [ -z "${width:-}" ] || [ "${said:-}" != "$dev" ] ||
       [ -z "${rowbytes:-}" ] || [ "$rowbytes" -le 0 ]; then
        note "the $dev run did not say what page it painted or what a row" \
             "of it costs, so nothing here can be read out of what it wrote"
        return
    fi

    # ---- the bytes ----
    nband=0
    onerow=0
    printf '%s\n' "$lines" | sed -n 's/^BAND //p' > "$work/bands-$dev"
    while read -r h b name; do
        [ -n "${name:-}" ] || continue
        nband=$((nband + 1))
        [ "$b" -eq 1 ] && onerow=1
        nbands=$(( (h + b - 1) / b ))
        if [ "$nbands" -lt 2 ]; then
            note "the $dev page of $h rows was put out in $nbands band(s); a" \
                 "page one band covers has no boundary between bands on it" \
                 "and every arrangement of marks passes on one"
        fi
        w=$work/whole-$h-$dev.out
        if [ ! -s "$work/$name" ] || [ ! -s "$w" ]; then
            note "the $dev page of $h rows in bands of $b produced nothing"
            continue
        fi
        if cmp -s "$work/$name" "$w"; then
            echo "OK   $dev: the page of $h rows in $nbands bands of $b is" \
                 "the page held whole"
        else
            note "the $dev page of $h rows put out in $nbands bands of $b is" \
                 "not the page put out whole"
            cmp "$work/$name" "$w" 2>&1 | sed 's/^/      /' | head -2
        fi
    done < "$work/bands-$dev"
    if [ "$nband" -lt 2 ]; then
        note "the $dev run put out $nband banded page(s); one says nothing" \
             "about whether the band height matters"
    fi
    if [ "$onerow" -ne 1 ]; then
        note "no $dev page was put out in bands of one row, so no boundary" \
             "between bands fell inside a mark's end or inside the screen's" \
             "own cell"
    fi

    # ---- the rows reached the writer as the bands finished ----
    printf '%s\n' "$lines" | sed -n 's/^CALLS //p' > "$work/calls-$dev"
    ncall=0
    while read -r kind h want got rows; do
        [ -n "${rows:-}" ] || continue
        ncall=$((ncall + 1))
        if [ "$got" -ne "$want" ]; then
            note "the $dev $kind page of $h rows was put out in $want" \
                 "band(s) and reached the row writer in $got call(s); the" \
                 "rows are not going out as the bands finish"
        fi
        if [ "$rows" -ne "$h" ]; then
            note "the $dev $kind page of $h rows handed the row writer" \
                 "$rows row(s); a page is written once and whole"
        fi
    done < "$work/calls-$dev"
    if [ "$ncall" -lt 3 ]; then
        note "the $dev run reported $ncall page(s) reaching the row writer"
    else
        echo "OK   $dev: every page reached the row writer once per band," \
             "carrying the page's rows between them"
    fi

    # ---- the bound ----
    # Read at the shortest page and the tallest, so what is compared is
    # how each route grows with the page rather than what either costs
    # once. The band height is the same at both, since a band that grew
    # with the page would be no bound at all.
    mem() { field MEM | awk -v k="$1" -v h="$2" '$1 == k && $2 == h { print $3 }'; }
    heights=$(field MEM | awk '$1 == "band" { print $2 }' | sort -n | uniq)
    lo=$(printf '%s\n' $heights | head -1)
    hi=$(printf '%s\n' $heights | tail -1)
    bandlo=$(mem band "$lo" | tail -1); bandhi=$(mem band "$hi" | tail -1)
    wholelo=$(mem whole "$lo" | tail -1); wholehi=$(mem whole "$hi" | tail -1)
    if [ -z "${bandlo:-}" ] || [ -z "${bandhi:-}" ] ||
       [ -z "${wholelo:-}" ] || [ -z "${wholehi:-}" ]; then
        note "the $dev run did not say what both routes took at both" \
             "heights, so there is nothing to compare"
        return
    fi
    if [ "$hi" -le "$lo" ]; then
        note "the $dev pages measured are not of different heights"
        return
    fi
    rows=$((hi - lo))
    # what each route took for each further row of page, in bytes; scaled
    # so that a slope below one byte a row is still a number here
    bandslope=$((1000 * (bandhi - bandlo) / rows))
    wholeslope=$((1000 * (wholehi - wholelo) / rows))
    echo "OK   $dev held: band $bandlo -> $bandhi bytes over $lo -> $hi rows"
    echo "OK   $dev held: whole $wholelo -> $wholehi bytes over $lo -> $hi rows"
    # what a row of this device's raster costs is the device's own answer,
    # read off the run rather than tabulated here: it is what the band
    # budget is divided by, so a table beside it would be a second
    # statement of the arithmetic under test
    if [ "$wholeslope" -lt $((800 * rowbytes)) ]; then
        note "holding the whole $dev page grew by $wholeslope/1000 bytes a" \
             "row where a row of it is $rowbytes bytes; the measurement is" \
             "not seeing the raster, so it cannot say the band bounds it"
    else
        echo "OK   $dev: holding the whole page grows by $wholeslope/1000" \
             "bytes a row"
    fi
    # ... and the banded route must not. What still grows with the page
    # is one slot per row of the array of rows -- the references, not the
    # rows -- which is a small fraction of a row of pixels.
    if [ $((bandslope * 10)) -ge "$wholeslope" ]; then
        note "the $dev band route grew by $bandslope/1000 bytes a row" \
             "against the whole page's $wholeslope/1000; what it holds is" \
             "following the page's height, so the band is not bounding it"
    else
        echo "OK   $dev: the band route grows by $bandslope/1000 bytes a" \
             "row, under a tenth of it"
    fi
    # and the plainest statement of the same thing: the tallest page in
    # bands holds less than the shortest page held whole
    if [ "$bandhi" -ge "$wholelo" ]; then
        note "the tallest $dev page in bands held $bandhi bytes and the" \
             "shortest page held whole took $wholelo; a band that is not" \
             "smaller than a page bounds nothing"
    else
        echo "OK   $dev: the tallest page in bands holds less than the" \
             "shortest page held whole"
    fi
}

# The devices whose page the class assembles, out of the ones that band.
# Asked of a running interpreter: a device whose Emit is an operator
# assembles its page in compiled code, so the class's row writer the run
# counts calls into is never reached and there is nothing here to read.
ask="$work/emit.ps"
{
    echo "["
    for dev in $DEVICE_FLEET_BANDS; do echo "/$dev"; done
    cat <<'ASKEOF'
]
{ /D exch def
  { << /OutputDevice D /PageSize [ 8 8 ] >> setpagedevice } stopped
  { (EMIT ) print D 60 string cvs print ( unmade\n) print }
  { (EMIT ) print D 60 string cvs print ( ) print
    DEVICE /Emit get type 60 string cvs print (\n) print }
  ifelse
} forall
quit
ASKEOF
} > "$ask"
said=$( cd "$work" && "$xpost" -q $ns -d null -o emit.scratch emit.ps \
        </dev/null 2>&1 )
if ! printf '%s\n' "$said" | grep -q '^EMIT '; then
    echo "FAILURES: the interpreter could not be asked which of its band"
    echo "      devices assemble their own page:"
    printf '%s\n' "$said" | sed 's/^/      /' | head -8
    exit 1
fi
devices=$(printf '%s\n' "$said" \
          | awk '$1 == "EMIT" && $3 == "arraytype" { print $2 }')
elsewhere=$(printf '%s\n' "$said" \
            | awk '$1 == "EMIT" && $3 != "arraytype" { printf " %s", $2 }')
if [ -z "$devices" ]; then
    echo "FAILURES: no device that bands assembles its page through its"
    echo "      class, and the raster classes do. The question is being"
    echo "      asked wrong."
    exit 1
fi
# named, so that what this wrapper does not reach is read rather than
# inferred from a shorter report
[ -n "$elsewhere" ] &&
    echo "NOTE assembled in compiled code, held by band-writer instead:$elsewhere"

ndev=0
for dev in $devices; do
    check_device "$dev"
    ndev=$((ndev + 1))
done

verdict_exit "$ndev raster class(es) assembling the page they band"
