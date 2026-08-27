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
# The boundary has more than the arena to give back, and what it does not
# give back is invisible to every other measure: a block held outside the
# arena is reached through the entity that carries its handle, and the
# revert takes the whole arena away at once without walking an entity. Both
# such blocks are held here by the same reading of resident memory -- the
# raster a page device holds, and the record a handle itself is -- because
# neither the arena figures nor a leak checker can see them: the arena is
# back at its baseline either way, and the block stays reachable from the
# table that records it.
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
# is not read the same way. It skips too where the build has no backing that
# can hand pages back at all, which is a configuration and not a platform.
set -u
src=${1:?usage: check-page-return.sh <srcroot> <xpost> <builddir>}
xpost=${2:?usage: check-page-return.sh <srcroot> <xpost> <builddir>}
build=${3:?usage: check-page-return.sh <srcroot> <xpost> <builddir>}

. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$xpost" "the interpreter"
# named absolutely, so it resolves the same wherever the run is made from
case $xpost in
    /*) ;;
    *)  xpost=$(cd "$(dirname "$xpost")" && pwd)/$(basename "$xpost") ;;
esac
guard_require_file "$xpost" "the interpreter, named absolutely"
guard_require_file "$build/config.h" "the generated configuration header"

guard_workdir

# Resident memory is read from /proc/self/statm, and it is the interpreter
# that reads it. Whether the shell can is a different question with a
# different answer, and the two part company on exactly the platform this
# was silently wrong on: the Windows build runs under a shell whose
# emulation carries /proc while the native binary it starts does not, so a
# shell-side test passed there and the figures came back missing or flat --
# read as the page return having regressed. Ask the process that has to do
# the reading. A skip, not a pass: a run that could not measure must not
# read as one that measured and found the pages returned.
#
# Asked first, so a platform that cannot measure says that rather than
# answering about its backing: Windows returns pages through its
# reservation and would be misdescribed by the skip below.
{
    printf '{ (/proc/self/statm) (r) file closefile (YES) print } stopped\n'
    printf '{ (NO) print } if flush\n'
} > "$work/statm"
if [ "$("$xpost" -q --no-sandbox -d null --jobserver < "$work/statm" 2>/dev/null)" \
     != YES ]; then
    echo "SKIP: the interpreter cannot read /proc/self/statm, so resident memory"
    echo "      cannot be measured; check-vm-growth holds the reported figures"
    exit 77
fi

# What the two callers reach hands pages back only where the arena owns the
# pages -- a mapping, or the reservation Windows takes. Configured onto the
# host allocator (-Dmmap=false) the arena borrows its storage, there is no
# call to make, the routine is compiled away to nothing, and resident memory
# cannot fall however correct the reclaim is. That is a build being asked a
# question it does not have rather than a regression, so it is a skip -- and
# it is read off config.h, which is what the configuration decided, rather
# than off the option, which is only what was asked for.
if ! grep -q '^#define HAVE_MMAP' "$build/config.h"; then
    echo "SKIP: this build backs virtual memory with the host allocator, which"
    echo "      owns no pages to hand back; there is no page return to hold"
    exit 77
fi

# An address-sanitized build does not hand a freed block back to the system
# either: the sanitizer holds it in a quarantine so that a read of it is
# still reported, and its quarantine is larger than everything this streams.
# Resident memory therefore cannot fall however correctly the boundary
# retires the device, which is the same shape of skip as the two above --
# a build being asked a question it does not have.
#
# Measured rather than supposed. The forty-job probe grows 8410 pages under
# the default quarantine and is flat (-2 pages) with the quarantine set to a
# megabyte, on the same build and the same binary: nothing is retained that
# the interpreter retained.
#
# Read off the binary, which is what the build is, rather than off the
# option, which is only what was asked for.
if ldd "$xpost" 2>/dev/null | grep -q 'libasan' ||
   nm -D "$xpost" 2>/dev/null | grep -q '__asan_init'; then
    echo "SKIP: this build is address-sanitized, and the sanitizer holds every"
    echo "      freed block in a quarantine rather than returning it, so"
    echo "      resident memory cannot fall for the boundary to be held to"
    exit 77
fi

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

# Between jobs, the handle record: a font dictionary names the face it was
# built over through a handle, which is a record kept outside the arena and
# given up where the entity carrying it goes -- at a free, at a collection,
# at the end of the memory file. The revert is none of those: it puts the
# whole arena back without walking an entity, so a job that ends with a font
# dictionary still reachable leaves the record behind unless the boundary
# sweeps for it. One record is small, so the probe makes many per job and
# counts on the difference between four jobs and forty-four: the sweep holds
# it flat, and without it the forty extra jobs cost about eleven hundred
# pages.
holds='1 1 2000 { /Courier findfont exch scalefont /F exch def } for'
for spec in few:4 many:44; do
    tag=${spec%:*}
    n=${spec#*:}
    {
        i=0
        while [ "$i" -lt "$n" ]; do printf '%s\n\004' "$holds"; i=$((i + 1)); done
        printf '/rss { %s } def (HANDLE %s ) print rss 20 string cvs print (\\n) print flush\004' "$rss" "$tag"
        # Beside the resident figure, what virtual memory itself says it is
        # holding. The two answer different questions and a report of one
        # without the other cannot be acted on: resident memory that grew
        # while the arena did not is memory the interpreter gave up and the
        # host did not take back, and an arena that grew is a job carrying
        # something across the boundary. A host where this case fails will
        # say which of the two it is.
        printf '/rss { %s } def (HANDLEVM %s ) print vmstatus pop exch pop 20 string cvs print ( ) print globalvmstatus pop exch pop 20 string cvs print (\\n) print flush\004' "$rss" "$tag"
    } > "$work/handle.$tag"
    "$xpost" -q --no-sandbox -d null --jobserver \
        < "$work/handle.$tag" > "$work/handle.$tag.out" 2>/dev/null
done

cat "$work/within.out" "$work/between.out" \
    "$work/device.few.out" "$work/device.many.out" \
    "$work/handle.few.out" "$work/handle.many.out" > "$work/out"

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
    # anchored, because the pair of lines the handle case prints share a
    # prefix and an unanchored match would read the second into the first
    /^HANDLE few/    { hf = $3 }
    /^HANDLE many/   { hm = $3 }
    /^HANDLEVM few/  { vlf = $3; vgf = $4 }
    /^HANDLEVM many/ { vml = $3; vmg = $4 }
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
        held = 700      # pages; 40 jobs x 2000 records leak about 1100,
                        # and the sweep holds the same probe inside +-270
        if (hf == "" || hm == "")
            { print "the handle probe did not report both figures"; bad = 1 }
        else if (hm - hf > held) {
            printf "the job boundary did not give up the handle records of the\n"
            printf "entities it dropped: resident grew %d pages over 40 extra jobs\n", hm - hf
            printf "      virtual memory over the same forty: local %d bytes, global %d\n", vml - vlf, vmg - vgf
            printf "      (a resident figure that grew while these did not is memory\n"
            printf "      the interpreter gave up and the host did not take back)\n"
            bad = 1
        }
        if (bad) exit 1
        printf "within a job vmreclaim returned %d pages; between jobs the boundary\n", wp - wa
        printf "returned %d, held the device buffer flat (%d pages over 40 jobs)\n", bp - ba, dm - df
        printf "and the handle records flat (%d pages over 40 jobs, virtual memory\n", hm - hf
        printf "over the same forty: local %d bytes, global %d)\n", vml - vlf, vmg - vgf
    }
' "$work/out" > "$work/problems" 2>&1
rc=$?

if [ "$rc" -ne 0 ] || [ ! -s "$work/out" ]; then
    echo "FAILURES: a page-return path did not return resident memory to the system:"
    sed 's/^/      /' "$work/problems"
    exit 1
fi

printf 'SUCCESS (%s)\n' "$(cat "$work/problems")"
