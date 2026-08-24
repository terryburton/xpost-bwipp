#!/bin/sh
# The job-boundary growth guard.
#
# A job server is given a sequence of programs and runs each as a job: a save
# of the initial state of virtual memory -- both banks, PLRM 3.7.7 -- then the
# program, then a restore back to that state at the program's Control-D
# boundary. The restore returns the used figure of both banks to the baseline
# the language and device established, and the arena's size with it, so a
# server's footprint tracks the job it runs and not the largest it ever ran.
# A job that instead leaves something behind after its boundary accumulates
# over the server's life -- a table nothing clears, a name interned and never
# dropped, an arena grown and never handed back -- once for every job for as
# long as the process serves them.
#
# The instrument that finds such a thing is a long-lived server taking many
# jobs. This runs the interpreter's whole test corpus as one: every program a
# Control-D-framed job fed to --jobserver, in one process, with the used
# figure of both banks read at the start of each -- which is the baseline the
# previous job's boundary reverted to. Every reading must equal the first. One
# that has grown is memory the job before it left past its boundary, and a
# server would carry it from that job on. Separate processes could not find
# it, being fresh each time; the shared, long-lived context is the point.
#
# WHAT THE PROBE READS. The used figure is taken before the probe parses the
# string it prints its name into, so the figure is the state at the job's
# start and not the probe's own scratch -- a name a byte longer would
# otherwise read a byte higher, and every workload's name is a different
# length.
#
# THE STREAM MUST REACH ITS END. A workload that does not terminate stalls the
# stream on itself and leaves every job after it unmeasured. The last job is a
# sentinel whose reading proves the stream ran through; a run that does not
# print it failed on whatever job it stalled on.
#
# THE SKIP REGISTER. A workload that cannot be a job in a stream -- one that
# never reaches its Control-D -- is named in tests/vm_growth.golden, with the
# reason, and left out. The population is otherwise the whole directory: a
# workload that arrives unregistered is run, so a new test is held to the
# baseline the day it lands and not the day someone remembers it.
#
#   $1  path to the source tree root
#   $2  path to the built xpost binary

set -u
src=${1:?usage: check-vm-growth.sh <srcroot> <xpost>}
xpost=${2:?usage: check-vm-growth.sh <srcroot> <xpost>}

. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$xpost" "the interpreter"
# named absolutely, so it resolves the same wherever the run is made from
case $xpost in
    /*) ;;
    *)  xpost=$(cd "$(dirname "$xpost")" && pwd)/$(basename "$xpost") ;;
esac
guard_require_file "$xpost" "the interpreter, named absolutely"
skipfile="$src/tests/vm_growth.golden"
guard_require_file "$skipfile" "the register of workloads a job stream cannot carry"

guard_workdir

XPOST_DATA_DIR="$src/data"
export XPOST_DATA_DIR

# A limit on the whole run, for a workload that hangs despite being registered
# to terminate. Set well above what the corpus needs -- it sweeps in under a
# minute on the machine this was written on, an address-sanitized build with
# leak checking multiplies that, a shared runner is slower again -- so a slow
# host is not raced. Honoured with the system's timeout where there is one and
# a background sleeper where there is not (the base system of macOS has none).
if command -v timeout > /dev/null 2>&1; then
    xg_limit() { _lim=$1; shift; timeout "$_lim" "$@"; }
elif command -v gtimeout > /dev/null 2>&1; then
    xg_limit() { _lim=$1; shift; gtimeout "$_lim" "$@"; }
else
    xg_limit() {
        _lim=$1; shift
        "$@" &
        _job=$!
        ( sleep "$_lim"; kill -9 "$_job" ) > /dev/null 2>&1 &
        _watch=$!
        wait "$_job"; _rc=$?
        kill "$_watch" > /dev/null 2>&1
        return $_rc
    }
fi

# the skip register: the first field of each non-comment line is a workload
# left out of the stream; the rest of the line is why.
skips=$(grep -vE '^[[:space:]]*(#|$)' "$skipfile" | awk '{ print $1 }')

# The probe, split around the workload name. vmstatus and globalvmstatus are
# read onto the stack before the name string is parsed, so the two used
# figures are the job's baseline and carry none of the probe's own cost.
probe_head='vmstatus pop exch pop globalvmstatus pop exch pop (XG '
probe_tail=' )print exch 20 string cvs print ( )print 20 string cvs print (\n)print flush'

# Build the stream: each workload a Control-D-framed job behind its probe, a
# suite asking for the shared framework getting it as its own harness would
# (run-ps-test.sh prepends testlib.ps to a %!testlib first line).
: > "$work/stream"
nrun=0
for f in "$src"/tests/*.ps; do
    b=$(basename "$f")
    case " $skips " in *" $b "*) continue ;; esac
    {
        printf '%s%s%s\n' "$probe_head" "$b" "$probe_tail"
        case $(head -n 1 "$f" 2>/dev/null) in
            '%!testlib'*) cat "$src/tests/testlib.ps" "$f" ;;
            *)            cat "$f" ;;
        esac
        printf '\004'
    } >> "$work/stream"
    nrun=$((nrun + 1))
done
# the sentinel whose reading proves the stream ran to the end
printf '%s__END__%s\004' "$probe_head" "$probe_tail" >> "$work/stream"

if [ "$nrun" -lt 50 ]; then
    echo "FAILURES: only $nrun workloads were streamed, fewer than this tree"
    echo "      carries; the reading is broken, not the tree"
    exit 1
fi

# Run the whole corpus as one job server.
xg_limit 600 "$xpost" -q --no-sandbox -d null --jobserver \
    < "$work/stream" > "$work/out" 2>"$work/err"

# A collection that cannot find an entity's table row is following a dangling
# root -- a context field the boundary left naming an entity the revert
# discarded. The corpus run as a job stream is where such a root is walked; a
# program run on its own never crosses a boundary and never sees it. The
# message is the collector's own, and it is held for here because the harness
# would otherwise pass while the interpreter corrupted its own memory: the
# used figures revert whatever the roots point at, and the corruption is
# fatal only in some layouts.
if grep -q 'cannot find table for ent' "$work/err" 2>/dev/null; then
    echo "FAILURES: the collector followed a dangling context root across a job"
    echo "      boundary -- a root left naming an entity the revert discarded:"
    grep 'cannot find table for ent' "$work/err" | sed 's/^/      /' | head -3
    exit 1
fi

# Hold every job's baseline reading to the first, and require the sentinel.
awk '
    /^XG / {
        if (base == "") { base = $3 " " $4; bl = $3 + 0; bg = $4 + 0 }
        else if ($3 " " $4 != base) {
            printf "%s left %d bytes of local and %d of global virtual memory\n", prev, $3 - bl, $4 - bg
            printf "        past its job boundary; a server would carry it from that job on\n"
            bad = 1
        }
        if ($2 == "__END__") ended = 1
        prev = $2
        seen++
    }
    END {
        if (seen == 0)
            print "no job printed a baseline reading, so nothing was measured"
        else if (!ended)
            printf "the stream did not run to its end -- it stalled on the job after %s\n", prev
        if (bad || seen == 0 || !ended) exit 1
    }
' "$work/out" > "$work/problems" 2>&1
rc=$?

if [ "$rc" -ne 0 ] || [ -s "$work/problems" ]; then
    echo "FAILURES: a workload left virtual memory past its job boundary, or the"
    echo "      stream did not run to its end:"
    sed 's/^/      /' "$work/problems"
    exit 1
fi

printf 'SUCCESS (%s workloads run as job-server jobs in one process; each\n' "$nrun"
printf '         returned both banks of virtual memory to the baseline at its\n'
printf '         Control-D boundary)\n'
