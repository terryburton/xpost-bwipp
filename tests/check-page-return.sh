#!/bin/sh
# The page return, held to each of the two callers that reach it.
#
# xpost_memory_file_release_range hands committed pages back to the operating
# system, and the interpreter reaches it two ways: vmreclaim within a job,
# through the arena compaction (xpost_free_compact), and the job boundary
# between jobs, returning the growth a job left above the baseline. The two
# are separate triggers for the one reclaim, and a test that watched only one
# would go blind to the other regressing.
#
# Neither can be watched through vmstatus. The reported size falls whether or
# not the pages were returned -- a boundary that renamed the arena to the
# baseline without handing its pages back reports exactly what one that
# returned them does -- so check-vm-growth, which holds those figures to the
# baseline, cannot tell a page return from a rename. What only falls when the
# pages truly go back is the process's resident memory. This reads it from
# /proc/self/statm and holds each caller to returning a job's growth.
#
# It is therefore a Linux test: where that file is absent it skips, and the
# reported-figure invariance check-vm-growth makes stands for the reclaim on
# the platforms whose own page return (DiscardVirtualMemory, a fixed re-map)
# is not read the same way.
set -u
src=${1:?usage: check-page-return.sh <srcroot> <xpost>}
xpost=${2:?usage: check-page-return.sh <srcroot> <xpost>}

. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$xpost" "the interpreter"
# named absolutely, so it resolves the same wherever the run is made from
case $xpost in
    /*) ;;
    *)  xpost=$(cd "$(dirname "$xpost")" && pwd)/$(basename "$xpost") ;;
esac
guard_require_file "$xpost" "the interpreter, named absolutely"

# Resident memory is read from /proc/self/statm; where it is absent there is
# nothing to measure, and the reported-figure invariance stands for the
# reclaim. A skip, not a pass: a run that could not measure must not read as
# one that measured and found the pages returned.
if [ ! -r /proc/self/statm ]; then
    echo "SKIP: /proc/self/statm is not readable, so resident memory cannot be"
    echo "      measured; check-vm-growth holds the reported figures instead"
    exit 77
fi

guard_workdir

XPOST_DATA_DIR="$src/data"
export XPOST_DATA_DIR

# Resident pages: field two of /proc/self/statm, read before the probe parses
# the scratch it prints into, so the figure is the state it names and not the
# probe's own allocation.
rss='(/proc/self/statm)(r)file dup token pop pop token pop'

# Grow about thirty-nine megabytes -- six hundred strings a word-size each --
# so a page return stands well clear of measurement noise. The fall asserted
# below is a fraction of it, so the test turns on whether the pages went back
# at all, not on how nearly all of them did.
grow='/keep 600 array def 0 1 599 { keep exch 65535 string put } for'

# Within a job: vmreclaim returns the arena's garbage. The growth is dropped
# and 2 vmreclaim collects both banks, so what it returns is the storage the
# now-unreachable strings held.
{
    printf '/rss { %s } def\n' "$rss"
    printf 'true setglobal\n%s\n' "$grow"
    printf '(WITHIN peak ) print rss 20 string cvs print (\\n) print flush\n'
    printf '/keep null def 2 vmreclaim\n'
    printf '(WITHIN after ) print rss 20 string cvs print (\\n) print flush\n'
} > "$work/within"
"$xpost" -q --no-sandbox -d null --jobserver < "$work/within" > "$work/within.out" 2>/dev/null

# Between jobs: the boundary returns the growth. The strings are kept live
# across the measurement, so vmreclaim could not have taken them -- only the
# Control-D boundary's revert, discarding the job's whole virtual memory, can
# -- and the next job reads resident memory back at the baseline.
{
    printf '/rss { %s } def\n' "$rss"
    printf 'true setglobal\n%s\n' "$grow"
    printf '(BETWEEN peak ) print rss 20 string cvs print (\\n) print flush\n'
    printf '\004'
    printf '/rss { %s } def\n' "$rss"
    printf '(BETWEEN after ) print rss 20 string cvs print (\\n) print flush\n'
    printf '\004'
} > "$work/between"
"$xpost" -q --no-sandbox -d null --jobserver < "$work/between" > "$work/between.out" 2>/dev/null

# Between jobs, the device buffer: a page device holds a raster outside the
# arena, reached only through the private string in its instance dictionary.
# The boundary must retire a job's device -- run its Destroy -- or the buffer
# is orphaned when the image restore drops that dictionary: a leak the arena
# reclaim above cannot see, and a leak checker calls reachable because the
# malloc stays reached through the arena. Held here by resident memory: a few
# setpagedevice jobs against many must not grow by a buffer per job.
setpd='<< /PageSize [500 500] >> setpagedevice'
for spec in few:4 many:44; do
    tag=${spec%:*}
    n=${spec#*:}
    {
        i=0
        while [ "$i" -lt "$n" ]; do printf '%s\n\004' "$setpd"; i=$((i + 1)); done
        printf '/rss { %s } def (DEVICE %s ) print rss 20 string cvs print (\\n) print flush\004' "$rss" "$tag"
    } > "$work/device.$tag"
    "$xpost" -q --no-sandbox -d raster -o /dev/null --jobserver \
        < "$work/device.$tag" > "$work/device.$tag.out" 2>/dev/null
done

cat "$work/within.out" "$work/between.out" \
    "$work/device.few.out" "$work/device.many.out" > "$work/out"

# Each pair must show resident memory fall by a clear margin. The growth is
# about ten thousand pages; a return of a fifth of it is asked, which the
# real return clears several times over and a shrink that renamed the arena
# without handing its pages back -- leaving the after figure at the peak --
# does not reach.
awk '
    /WITHIN peak/   { wp = $3 }
    /WITHIN after/  { wa = $3 }
    /BETWEEN peak/  { bp = $3 }
    /BETWEEN after/ { ba = $3 }
    /DEVICE few/    { df = $3 }
    /DEVICE many/   { dm = $3 }
    END {
        want = 5000     # pages, ~20MB, well under the ~39MB grown
        grew = 2000     # pages, ~8MB; 40 leaked 500x500x3 buffers is ~7300
        bad = 0
        if (wp == "" || wa == "")
            { print "the within-job probe did not report both figures"; bad = 1 }
        else if (wp - wa < want) {
            printf "vmreclaim did not return the arena within a job: resident fell\n"
            printf "%d pages, and a return of the growth is at least %d\n", wp - wa, want
            bad = 1
        }
        if (bp == "" || ba == "")
            { print "the between-job probe did not report both figures"; bad = 1 }
        else if (bp - ba < want) {
            printf "the job boundary did not return the growth between jobs: resident\n"
            printf "fell %d pages, and a return of the growth is at least %d\n", bp - ba, want
            bad = 1
        }
        if (df == "" || dm == "")
            { print "the device probe did not report both figures"; bad = 1 }
        else if (dm - df > grew) {
            printf "the job boundary did not retire the page device it installed:\n"
            printf "resident grew %d pages over 40 extra setpagedevice jobs\n", dm - df
            bad = 1
        }
        if (bad) exit 1
        printf "within a job vmreclaim returned %d pages; between jobs the boundary\n", wp - wa
        printf "returned %d and held the device buffer flat (%d pages over 40 jobs)\n", bp - ba, dm - df
    }
' "$work/out" > "$work/problems" 2>&1
rc=$?

if [ "$rc" -ne 0 ] || [ ! -s "$work/out" ]; then
    echo "FAILURES: a page-return path did not return resident memory to the system:"
    sed 's/^/      /' "$work/problems"
    exit 1
fi

printf 'SUCCESS (%s)\n' "$(cat "$work/problems")"
