#!/bin/sh
# Meson test wrapper: a record's lifetime is a page's, not a job's.
#
# The recording device writes down the marks a page makes and paints the
# page at Emit by playing them into a raster. Both of those things are
# per page, and neither of them being per page shows on any page:
#
#   A record not given up at the page boundary holds every page a job has
#   drawn so far. Emitting page n then replays all n pages' marks, once
#   per band, and still paints page n correctly -- because a page opens
#   by painting over its predecessors, so the marks that ought to have
#   been dropped are the ones that get covered again.
#
#   A raster built per emission leaves every page's rows behind. Raster
#   memory is taken from the half of virtual memory nothing here gives
#   back, so a job of n pages ends holding n pages' worth of rows and
#   paints the same pixels doing it. This is the argument the band loop
#   is built on, one level up: .moveband exists so the loop does not
#   leave a raster per band behind, and leaving one per page behind is
#   the same failure at the page's scale.
#
#   An image is the one recorded entry whose replay costs the picture.
#   Playing it again over rows it was already played into leaves exactly
#   the page it left the first time, an image write being idempotent, so
#   a replay that plays it once per mark it steps over on the way to it
#   is invisible to every comparison of pages.
#
# So this run reads three counts out of the interpreter and holds them to
# properties, and reads the pages too, since a count held to a property
# over a device that paints the wrong page says nothing.
#
#   The bytes. Every page of a multi-page job, put out through the
#   recording device, must be the page the device that paints puts out.
#
#   What a record holds. The number of marks in the record at the end of
#   each page must be the same number on every page. A job whose record
#   is the job's holds a multiple of it.
#
#   What the job takes. Global virtual memory, read after each page has
#   been put out, must not grow from page to page -- and the device that
#   paints, which holds one whole page and no record at all, is the
#   control that says a flat reading is a reading of something.
#
#   How many times an image is played. A page of marks then one image,
#   put out in bands, must play the image no more times than there are
#   bands, and must play it the same number of times whether few marks or
#   many stand in front of it. The two together are what separate "once
#   per band it reaches" from "once per mark stepped over": the bound
#   alone would pass a replay that played it once per page, and the
#   invariance alone would pass one that played it once per band it does
#   not reach as well.
#
# The meter is checked before anything is measured with it. The run takes
# a known quantity of global memory and reports what the reading moved
# by; a reading that could not see memory being taken would report every
# route flat and pass everything put in front of it.
#
#   $1  path to the built xpost binary
#   $2  path to page_lifetime_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
skip_if_faceless "$xpost" "this run shows text through a face"

# The runs below are started in the directory the pages are written to,
# so what they were handed has to name the same thing from there.
xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

render() {  # $1 device; sets out
    out=$( cd "$work" && "$xpost" -q $ns -d "$1" -o unused.ppm "$script" \
           </dev/null 2>&1 )
    st=$?
    verdict_run "$st" "$out" "the $1 run" || return 1
    return 0
}

render record || fail=1
recorded=$out
# The device holding the whole page, asked for as the mode that says so:
# selecting a device by name selects the record in front of it, and what
# is compared here is a recorded page against a painted one.
render ppm:whole || fail=1
direct=$out

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

# The showpage banner of the default page semantics ends without a
# newline, so a reported line can arrive with it on the front.
reclines=$(printf '%s\n' "$recorded" | tr -s '-' '\n')
dirlines=$(printf '%s\n' "$direct" | tr -s '-' '\n')
recfield() { printf '%s\n' "$reclines" | sed -n "s/^$1 //p"; }
dirfield() { printf '%s\n' "$dirlines" | sed -n "s/^$1 //p"; }

# ---- the meter, before anything is read with it ----
meter=$(recfield METER | head -1)
asked=$(printf '%s\n' "$meter" | awk '{ print $1 }')
saw=$(printf '%s\n' "$meter" | awk '{ print $2 }')
if [ -z "${asked:-}" ] || [ -z "${saw:-}" ]; then
    echo "FAILURES: the run did not say what the memory reading moved by"
    echo "      when memory was taken, so nothing it reports about memory"
    echo "      can be read"
    exit 1
fi
if [ "$saw" -lt "$asked" ]; then
    note "taking $asked bytes moved the reading by $saw; the meter cannot" \
         "see memory being taken, so every route would read flat and this" \
         "run would pass whatever it was given"
else
    echo "OK   taking $asked bytes moves the reading by $saw"
fi

# The two routes through the emission the job is run over: the page put
# out whole, which is what the budget the class carries does with a page
# this size, and the page put out in bands. A raster kept from one page
# to the next has to be put back to what Create left it as on both, and
# they put it back by different means.
phases='whole band'

nband=$(recfield BANDS | head -1)
if [ -z "${nband:-}" ] || [ "$nband" -lt 2 ]; then
    note "the page was put out in ${nband:-0} band(s); a page one band" \
         "covers has no boundary between bands on it"
fi

# ---- the bytes ----
# Every page both routes wrote, compared page for page. The pages are
# named from the page counter, so the two runs write the same numbers.
nfile=0
for f in "$work"/page-*-rec.ppm; do
    [ -e "$f" ] || continue
    d=$(printf '%s\n' "$f" | sed 's/-rec\.ppm$/-dir.ppm/')
    nfile=$((nfile + 1))
    if [ ! -s "$f" ] || [ ! -s "$d" ]; then
        note "$(basename "$f") or its counterpart is empty"
    elif ! cmp -s "$f" "$d"; then
        note "$(basename "$f") is not the page the device that paints put out"
        cmp "$f" "$d" 2>&1 | sed 's/^/      /' | head -2
    fi
done
if [ "$nfile" -lt 3 ]; then
    note "$nfile page(s) were written; a job of one page says nothing about" \
         "what the page after it costs"
else
    echo "OK   $nfile pages recorded and replayed are the pages painted"
fi

# ---- what a record holds ----
for phase in $phases; do
    cost=$(recfield COST | awk -v p="$phase" '$1 == p { print $2 " " $3 }')
    npages=$(printf '%s\n' "$cost" | grep -c .)
    if [ "$npages" -lt 3 ]; then
        note "the $phase job reported the record's size on $npages page(s);" \
             "a job of one or two pages says little about what the page" \
             "after costs"
        continue
    fi
    first=$(printf '%s\n' "$cost" | head -1 | awk '{ print $2 }')
    if [ -z "${first:-}" ] || [ "$first" -lt 50 ]; then
        note "the $phase job's first page holds ${first:-0} mark(s); a page" \
             "that draws almost nothing cannot show a record accumulating"
        continue
    fi
    bad=$(printf '%s\n' "$cost" | awk -v w="$first" '$2 != w { print $1 " " $2 }')
    if [ -n "$bad" ]; then
        note "the $phase job's record holds $first mark(s) after page one" \
             "and a different number after these:"
        printf '%s\n' "$bad" | sed 's/^/      page /'
        note "a record whose lifetime is the job's holds every page drawn so" \
             "far, and replays all of them once per band to paint the last"
    else
        echo "OK   the $phase job's record holds $first marks after every one" \
             "of $npages pages"
    fi
done

# ---- what the job takes ----
# Read at the first page of a job and its last, so what is compared is
# how each route grows over that job rather than what either costs once.
# It is compared within a job and not across the two, because the second
# begins by making a page device and retiring the first one, and a
# retired device's rows are not given back either -- which is a bound of
# its own and not this one.
for phase in $phases; do
    recmem=$(recfield GVM | awk -v p="$phase" '$1 == p { print $3 }')
    dirmem=$(dirfield GVM | awk -v p="$phase" '$1 == p { print $3 }')
    if [ -z "$recmem" ] || [ -z "$dirmem" ]; then
        note "one of the two routes did not report what the $phase job took"
        continue
    fi
    reclo=$(printf '%s\n' "$recmem" | head -1)
    rechi=$(printf '%s\n' "$recmem" | tail -1)
    dirlo=$(printf '%s\n' "$dirmem" | head -1)
    dirhi=$(printf '%s\n' "$dirmem" | tail -1)
    echo "OK   held: $phase record $reclo -> $rechi bytes"
    echo "OK   held: $phase direct $dirlo -> $dirhi bytes"
    if [ "$dirhi" -ne "$dirlo" ]; then
        note "over the $phase job the device that paints grew by" \
             "$((dirhi - dirlo)) bytes; it holds one page and no record, so" \
             "it is the control that says what flat looks like, and it is" \
             "not flat"
    fi
    if [ "$rechi" -ne "$reclo" ]; then
        note "over the $phase job the recording device grew by" \
             "$((rechi - reclo)) bytes; the raster a page is played into" \
             "comes out of memory nothing gives back, so a raster per page" \
             "leaves every page's rows behind and the job ends holding the" \
             "pages it painted"
    else
        echo "OK   neither route grew over the $phase job"
    fi
done

# ---- how many times an image is played ----
plays() { printf '%s\n' "$reclines" | sed -n "s/^PLAYS $1 //p" | head -1; }
few=$(plays few)
many=$(plays many)
fewn=$(printf '%s\n' "$few" | awk '{ print $1 }')
manyn=$(printf '%s\n' "$many" | awk '{ print $1 }')
fewm=$(printf '%s\n' "$few" | awk '{ print $2 }')
manym=$(printf '%s\n' "$many" | awk '{ print $2 }')
if [ -z "${fewn:-}" ] || [ -z "${manyn:-}" ]; then
    note "the run did not say how many times it played the image"
elif [ "$((manym - fewm))" -lt 40 ]; then
    note "the two image pages hold $fewm and $manym marks; they differ by" \
         "too few for the count of plays to be able to follow them, so the" \
         "invariance below would hold for a replay that follows the marks"
else
    if [ "$fewn" -lt 1 ]; then
        note "the image was played $fewn times, so it never reached the page"
    elif [ "$fewn" -gt "$nband" ]; then
        note "the image was played $fewn times over $nband bands; an image" \
             "is played once for each band that reaches it, so a count past" \
             "the bands there are is the replay finding the same entry again"
    else
        echo "OK   the image is played $fewn times over $nband bands"
    fi
    if [ "$fewn" -ne "$manyn" ]; then
        note "the image was played $fewn times behind $fewm marks and" \
             "$manyn times behind $manym; what it costs is following the" \
             "marks the replay steps over rather than the bands it reaches"
    else
        echo "OK   the image is played $fewn times behind $fewm marks and" \
             "behind $manym"
    fi
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
