#!/bin/sh
# The blocks held outside virtual memory, held to the job boundary.
#
# A device's instance state, a vector writer's accumulated content, a
# font's face and a file's stream are blocks this process holds and
# virtual memory names: an entity carries a handle, and the block itself
# is recorded beside the entity rather than allocated in the arena. Every
# way an entity goes gives its block up as it goes -- a free, a
# collection, the end of the memory file -- but the job-encapsulation
# boundary (PLRM 3.7.7) takes the whole arena back at once, without
# walking an entity, so a job that ends with such a block still named
# leaves it recorded and still allocated unless the boundary sweeps for
# it. One block per handle per job, for as long as a server serves them.
#
# WHY THE COUNT AND NOT THE MEMORY. The block is invisible to every other
# measure. It is not in the arena, so vmstatus and check-vm-growth do not
# move when it is allocated or freed; it stays pointed at by the record,
# so a leak checker calls it reachable. What is left is resident memory,
# and a record is small: forty jobs of them come to about eighty
# kilobytes a job, which is inside what two runs of the same jobs differ
# by on a host under load. MEASURED, a resident-memory reading of exactly
# this population passed three times and failed once on one machine in
# one sitting, and the failure named a leak that was not there.
#
# So the records are counted instead. .vmhandlecount is the number of
# them, the reading is exact, and the assertion is equality: a boundary
# that gives up one record fewer than it took fails here on the next job,
# where the resident-memory reading could not have seen a hundred.
#
# What the count says is that the record went. Whether the block went
# with it is a different question and belongs to a different reader: a
# record cleared without its block being freed leaves the block reached
# from nothing, which is the one state a leak checker reports outright.
# The sanitized lanes are where that half is answered, and they can only
# answer it because the record no longer points at the block -- which is
# to say the two halves are complementary, and neither covers the other.
#
# THE PROBE MUST MAKE SOMETHING. A count that never rises is a count that
# would report a boundary giving up nothing as a boundary with nothing to
# give up. Each job therefore reads the number twice -- at its start,
# which is what the boundary before it left, and at its end, with the
# records it made still named -- and the run is refused unless the second
# stands clear of the first.
#
#   $1  path to the source tree root
#   $2  path to the built interpreter

set -u

src=${1:?usage: check-handle-records.sh <srcroot> <xpost>}
xpost=${2:?usage: check-handle-records.sh <srcroot> <xpost>}

. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$xpost" "the interpreter"
# named absolutely, so it resolves the same wherever the run is made from
case $xpost in
    /*) ;;
    *)  xpost=$(cd "$(dirname "$xpost")" && pwd)/$(basename "$xpost") ;;
esac
guard_require_file "$xpost" "the interpreter, named absolutely"

guard_workdir

XPOST_DATA_DIR="$src/data"
export XPOST_DATA_DIR
# The number is machinery's, reached through the machinery's own
# dictionary, which a run reporting on the interpreter is handed and a
# run of a program is not.
XPOST_CENSUS=1
export XPOST_CENSUS

njobs=20

# What each job makes. A page device is a record in every configuration;
# a face is one only where the build has a face library, and whether this
# one does is put to the build rather than read off a flag: a name that
# says so is a second statement of the same thing, and the two part
# company -- a build carrying no faces still answers findfont with a
# refusal where a name nothing supplies is asked for, so the refusal is
# what to ask about. A job that let the refusal out would be flushed
# before its second reading and the run would report a stream that did
# not finish rather than a build without faces.
#
# Both are left named at the boundary. That is the case the sweep is for:
# a record whose entity nothing reclaimed before the revert took the
# arena away.
count='1183615869 internaldict /.vmhandlecount get exec'
body='<< /PageSize [200 200] >> setpagedevice
/facesok true def
mark { /Courier findfont } stopped { /facesok false def } if cleartomark
facesok { 1 1 200 { /Courier findfont exch scalefont /F exch def } for } if'

: > "$work/stream"
i=0
while [ "$i" -lt "$njobs" ]; do
    {
        printf '(HR start %d ) print %s 20 string cvs print (\\n) print flush\n' \
               "$i" "$count"
        printf '%s\n' "$body"
        printf '(HR end %d ) print %s 20 string cvs print (\\n) print flush\n' \
               "$i" "$count"
        printf '\004'
    } >> "$work/stream"
    i=$((i + 1))
done
# the sentinel whose reading proves the stream ran to the end
printf '(HR start __END__ ) print %s 20 string cvs print (\\n) print flush\004' \
       "$count" >> "$work/stream"

"$xpost" -q --no-sandbox -d raster -o /dev/null --jobserver \
    < "$work/stream" > "$work/out" 2>"$work/err"

LC_ALL=C awk -v n="$njobs" '
    /^HR start / { if ($3 != "__END__") { start[$3] = $4 + 0; nstart++ }
                   else ended = $4 + 0
                   if (base == "") base = $4 + 0
                   if ($4 + 0 != base) {
                       grew++
                       if (grew <= 3) {
                           printf "job %s began with %d records where the first job\n", $3, $4
                           printf "        began with %d: the boundary before it gave up %d fewer\n", base, $4 - base
                           printf "        blocks than it took\n"
                       }
                       bad = 1
                   }
                 }
    /^HR end /   { end[$3] = $4 + 0; nend++
                   if ($3 in start) {
                       made = $4 + 0 - start[$3]
                       if (made > most) most = made
                       if (made < 1) {
                           empty++
                           if (empty <= 3) {
                               printf "job %s made no record at all (%d at its start, %d at its\n", $3, start[$3], $4
                               printf "        end), so its boundary was asked to give up nothing\n"
                           }
                           bad = 1
                       }
                   }
                 }
    END {
        if (grew > 3)
            printf "and %d further jobs began above the first the same way\n", grew - 3
        if (empty > 3)
            printf "and %d further jobs made nothing the same way\n", empty - 3
        if (nstart != n) {
            printf "%d of %d jobs reported a starting count; the run did not\n", nstart, n
            printf "        get through the stream\n"
            bad = 1
        }
        if (nend != n) {
            printf "%d of %d jobs reported a closing count\n", nend, n
            bad = 1
        }
        if (ended == "") {
            print "the stream did not run to its end: the sentinel never read"
            bad = 1
        }
        if (bad) exit 1
        printf "%d\n", most
    }
' "$work/out" > "$work/problems" 2>&1
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "FAILURES: the job boundary did not give up the blocks held outside"
    echo "      virtual memory that the entities it dropped named:"
    sed 's/^/      /' "$work/problems"
    if [ -s "$work/err" ]; then
        echo "      the run said:"
        sed 's/^/      /' "$work/err" | head -5
    fi
    exit 1
fi


# A JOB THAT DROPS WHAT IT BEGAN WITH. The sweep above is about blocks a
# job made. This is the other side of the same boundary: a block the
# BASELINE named, which the job stopped naming and a collection in the
# same job then reached.
#
# Installing a page device drops the one the run started with, so
# nothing names that device's entity any more and a collection is
# entitled to reclaim it. The revert then restores the entity -- byte
# for byte, handle number and all -- and the next job is handed a device
# whose block was freed in the job before. What that job sees is its
# Destroy answering undefined, being flushed, and the run reporting
# success: every job after the first is lost and nothing says so.
#
# The reading is the count again, and the assertion is that every job
# ran: a job flushed before its closing print reports no closing count.
: > "$work/stream2"
i=0
while [ "$i" -lt "$njobs" ]; do
    {
        printf '(HR start %d ) print %s 20 string cvs print (\\n) print flush\n' \
               "$i" "$count"
        printf '<< /PageSize [200 200] >> setpagedevice\n2 vmreclaim\n'
        printf '(HR end %d ) print %s 20 string cvs print (\\n) print flush\n' \
               "$i" "$count"
        printf '\004'
    } >> "$work/stream2"
    i=$((i + 1))
done

"$xpost" -q --no-sandbox -d raster -o /dev/null --jobserver \
    < "$work/stream2" > "$work/out2" 2>"$work/err2"

LC_ALL=C awk -v n="$njobs" '
    /^HR start / { nstart++
                   if (base == "") base = $4 + 0
                   if ($4 + 0 != base) {
                       if (++grew <= 3)
                           printf "job %s began with %d records where the first began with %d: the boundary gave up one the baseline still names\n", $3, $4, base
                       bad = 1
                   }
                 }
    /^HR end /   { nend++ }
    END {
        if (nstart != n) { printf "%d of %d jobs began\n", nstart, n; bad = 1 }
        if (nend != n) {
            printf "%d of %d jobs ran to their end: a job that drops the device it\n", nend, n
            printf "        began with and collects left the next one a handle to nothing\n"
            bad = 1
        }
        if (bad) exit 1
    }
' "$work/out2" > "$work/problems2" 2>&1
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "FAILURES: a block the baseline named did not survive the job that"
    echo "      stopped naming it:"
    sed 's/^/      /' "$work/problems2"
    if [ -s "$work/err2" ]; then
        echo "      the run said:"
        sed 's/^/      /' "$work/err2" | head -5
    fi
    exit 1
fi

printf 'SUCCESS (%d jobs, the most of them making %s blocks held outside\n' \
       "$njobs" "$(cat "$work/problems")"
printf '         virtual memory and leaving them named at the boundary; every\n'
printf '         job began with the record count the first began with, and\n'
printf '         %d jobs that dropped the device they began with and\n' "$njobs"
printf '         collected each left the next job one that still worked)\n'
