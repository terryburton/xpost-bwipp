#!/bin/sh
# Render the same PostScript through two of this interpreter's own trees
# and report what moved. Nothing else renders anything here: both sides
# are xpost, so a difference is this tree's doing, and there is nothing to
# adjudicate against a second implementation's reading of the language.
#
# That is the question a change asks and cannot answer out of its own
# diff: a refactor claims to move no bytes, a fix claims to move exactly
# some, and both claims are about output nobody has in front of them. The
# golden manifest answers it for one page through seven devices; this
# answers it for whatever programs it is given, and for the corpus in
# particular, which is the widest set actually rendered.
#
#   render-differ.sh [--gate] <sideA> <sideB> [program ...]
#
# A side is a tree, a data directory or a revision:
#
#   /path/to/worktree   a source tree: its data/, and its own built
#                       interpreter at build/src/bin/xpost if it has one
#   /path/to/data       a data directory (one holding init.ps)
#   dceb31d3^           a revision, whose data/ is extracted into the
#                       run's own directory and read from there
#
# The programs default to every corpus program fetched under tests/corpus,
# which is where the interesting input is; naming programs overrides that.
# The input is one file that both sides read -- rendering each side's own
# copy of the program would compare two inputs as well as two trees.
#
# The interpreter is $XPOST, or the tree's build, or a side's own build
# where that side is a tree that has one. One interpreter over two data
# directories is the fast path and covers every change under data/ without
# a rebuild; a change under src/ needs a build per side, and two revisions
# that differ there are refused rather than reported on, because one
# interpreter reading both trees' PostScript says nothing about C that
# neither run executed. The two interpreters must also agree on object
# width, for the same reason: a narrow build against a wide one differs by
# the build.
#
#   --gate            a difference is a failure rather than a finding.
#                     Without it a difference is the answer and the run is
#                     a success; the summary says which it was either way.
#
#   RENDER_JOBS       renders at once (default: one per core)
#   RENDER_DEVICES    devices to render through
#   RENDER_TIMEOUT    seconds one render may take (default 240)
#   RENDER_KEEP       keep what was rendered, and say where
#   SKIP_NONDET       hold out the programs that differ from themselves
#   XPOST             the interpreter, where a side does not carry one
#
# Every render is independent, so they all run at once; a serial run of
# this leaves fifteen sixteenths of a machine idle for the seven minutes
# it then takes.
#
# Every render gets a directory of its own, and it is its working
# directory rather than somewhere to write to, so a program that writes
# beside itself writes there. Renders sharing a directory is not a
# tidiness point: it is one program's output read as another's, and the
# reading succeeds.
#
# What a run says it did is held to what it was asked to do. The programs
# are named into a plan before anything renders, and the plan is what
# reports, so a unit that produced nothing is named as one rather than
# passed over. The counts in and out are printed together, because a run
# that quietly did four fifths of the work reads exactly like one that did
# all of it.
#
# Each render is a process group of its own, and a timeout takes the
# group. Killing the process alone leaves whatever it started running: an
# interpreter at full tilt outlives the run that started it, by hours, and
# the run it outlives has already reported success. A render is out of
# this run's group for the same reason, so interrupting the run does not
# reach one; what reaches it is its own watchdog, which is out of the
# group too, so nothing survives its limit either way.
#
# Where none of this can start, that is a skip (77) and not a pass. A
# comparison that could not run is not a comparison that found nothing.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
. "$here/verdict.sh"
. "$here/device-fleet.sh"

LIMIT=${RENDER_TIMEOUT:-240}
JOBS=${RENDER_JOBS:-$(nproc 2>/dev/null || echo 4)}

# The devices this renders through unless told otherwise: every one whose
# page arrives at the output path, less those whose bytes are a library's
# rather than this interpreter's, and less the recorder, whose page is a
# record of marks rather than a rendering to compare. That is the same
# reading the byte-identity gate makes, and taking it from the fleet
# rather than repeating its answer is what keeps the two from drifting --
# this had kept a copy of the gate's list from before the fleet was named,
# and a device added since would have been rendered by neither.
differ_devices=
for r_dev in $DEVICE_FLEET_ALL; do
    case " $DEVICE_FLEET_NOFILE $DEVICE_FLEET_OPTIONAL record " in
        *" $r_dev "*) continue ;;
    esac
    differ_devices="$differ_devices $r_dev"
done
DEVICES=${RENDER_DEVICES:-$differ_devices}

if command -v sha256sum >/dev/null 2>&1; then
    sum() { sha256sum "$1" | cut -d' ' -f1; }
else
    sum() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

# ---- one render ------------------------------------------------------
#
# In $1, which is its working directory and nobody else's. Leaves what the
# device wrote, the interpreter's output in log, its status in status, and
# timedout if it was still running when its time ran out.
render_side() {         # <dir> <interpreter> <datadir> <device> <program>
    _d=$1; _bin=$2; _data=$3; _dev=$4; _prog=$5
    mkdir -p "$_d" || return 1
    (
        cd "$_d" || exit 1
        XPOST_DATA_DIR=$_data
        export XPOST_DATA_DIR
        # The interpreter leads a session of its own, so its pid is its
        # process group and the group holds everything it starts. It
        # writes that number itself: taken from $! it would be the group
        # only while setsid does not fork, and a group kill aimed by a
        # number that is not a group reaches whichever group has it.
        # shellcheck disable=SC2086
        setsid sh -c 'echo $$ > pgid; exec "$@"' render \
               "$_bin" -q $NS -d "$_dev" -o "out_%d.$_dev" "$_prog" \
               </dev/null >log 2>&1 &
        _leader=$!
        setsid sh -c '
            sleep "$1" || exit 0
            kill -0 "$2" 2>/dev/null || exit 0
            : > timedout
            _pg=$(cat pgid 2>/dev/null)
            case $_pg in
                ""|*[!0-9]*) kill -TERM "$2" 2>/dev/null; exit 0 ;;
            esac
            kill -TERM "-$_pg" 2>/dev/null
            sleep 3
            kill -KILL "-$_pg" 2>/dev/null
        ' watchdog "$LIMIT" "$_leader" >/dev/null 2>&1 &
        _watch=$!
        wait "$_leader"
        _st=$?
        # the watchdog sleeps in a group of its own, so taking its group
        # away takes the sleep with it instead of leaving it to expire.
        # The pid stands behind the group only so that a wait cannot be
        # left holding a watchdog nothing reached.
        kill -TERM "-$_watch" 2>/dev/null || kill -TERM "$_watch" 2>/dev/null
        wait "$_watch" 2>/dev/null
        echo "$_st" > status
    )
}

# Everything a render left, by name and by content. Reading the directory
# rather than the name the device was handed is what makes this
# indifferent to how many files a device writes and what it calls them,
# so a page appearing or going missing is a difference like any other.
fingerprint() {         # <dir>
    (
        cd "$1" 2>/dev/null || exit 0
        for _f in *; do
            [ -f "$_f" ] || continue
            case $_f in log|pgid|status|timedout|fp) continue ;; esac
            printf '%s %s %s\n' "$(sum "$_f")" "$(wc -c < "$_f" | tr -d ' ')" "$_f"
        done | LC_ALL=C sort -k3
    )
}

# What moved between two of those, by name: a file only one side wrote,
# and a file both wrote differently, with the two sizes. A difference
# reported as a count says something moved and leaves the reader to
# render it again to find out what.
what_moved() {          # <fp A> <fp B>
    awk '
        NR == FNR { a[$3] = $1; asz[$3] = $2; next }
        {
            seen[$3] = 1
            if (!($3 in a)) { out[++k] = $3 " only on B (" $2 " bytes)" }
            else if (a[$3] != $1) { out[++k] = $3 " " asz[$3] " -> " $2 " bytes" }
        }
        END {
            for (f in a) if (!(f in seen)) out[++k] = f " only on A (" asz[f] " bytes)"
            s = ""
            for (i = 1; i <= k && i <= 4; i++) s = s (i > 1 ? "; " : "") out[i]
            if (k > 4) s = s "; and " (k - 4) " more"
            print s
        }' "$1" "$2"
}

# What one side's run amounted to: side_state (ok, nothing, crash,
# timeout), side_why and side_complaint.
#
# The status is the interpreter's own, read back from the process that was
# waited on rather than from a wrapper standing in front of it, and it is
# put to the one rule this suite judges runs by. A run that wrote every
# file it meant to and then died on the way out did not agree with
# anything.
judge_side() {          # <dir> <label>
    side_state=ok
    side_why=
    _st=$(cat "$1/status" 2>/dev/null) || _st=
    case ${_st:-} in ''|*[!0-9]*) _st=127 ;; esac
    _diag=$(grep -m1 '%%\[ Error' "$1/log" 2>/dev/null | sed 's/^[[:space:]]*//')
    side_complaint=$(verdict_run "$_st" "$_diag" "$2" | tr '\n' ' ') || :
    if [ -f "$1/timedout" ]; then
        side_state=timeout
        side_why="ran past ${LIMIT}s"
        return
    fi
    if [ "$_st" -ge 128 ]; then
        side_state=crash
        side_why="died on signal $((_st - 128))"
        return
    fi
    if [ ! -s "$1/fp" ]; then
        side_state=nothing
        side_why="wrote no file${_diag:+ -- $_diag}"
        return
    fi
    side_why="$(wc -l < "$1/fp" | tr -d ' ') file(s)${_diag:+, $_diag}"
}

# ---- the worker ------------------------------------------------------
#
# One unit -- one program through one device, both sides -- re-entered
# through this script so that a plain xargs runs as many at once as it is
# told to. It is handed the directory it works in and reads the rest from
# there, so it invents no path and cannot arrive where another unit is.
one_unit() {            # <unit directory>
    _u=$1
    read -r _base < "$_u/base"
    read -r _prog < "$_u/prog"
    read -r _dev < "$_u/dev"

    render_side "$_u/A" "$BIN_A" "$DATA_A" "$_dev" "$_prog"
    render_side "$_u/B" "$BIN_B" "$DATA_B" "$_dev" "$_prog"
    fingerprint "$_u/A" > "$_u/A/fp"
    fingerprint "$_u/B" > "$_u/B/fp"

    judge_side "$_u/A" "side A ($_base through $_dev)"
    _sa=$side_state; _wa=$side_why; _ca=$side_complaint
    judge_side "$_u/B" "side B ($_base through $_dev)"
    _sb=$side_state; _wb=$side_why; _cb=$side_complaint

    # A report is one line, so that a report half written is a report the
    # run reads as absent rather than as whatever its first half said.
    case "$_sa/$_sb" in
        *timeout*)  _state=timeout; _note="A $_wa; B $_wb. $_ca$_cb" ;;
        *crash*)    _state=crash;   _note="A $_wa; B $_wb. $_ca$_cb" ;;
        nothing/nothing)
                    _state=nothing; _note="neither side rendered: $_wa" ;;
        nothing/*|*/nothing)
                    _state=differ
                    _note="one side rendered and the other did not: A $_wa; B $_wb" ;;
        *)          if cmp -s "$_u/A/fp" "$_u/B/fp"; then
                        _state=same; _note=$_wa
                    else
                        _state=differ
                        _note=$(what_moved "$_u/A/fp" "$_u/B/fp")
                    fi ;;
    esac
    printf '%s\t%s\n' "$_state" "$_note" > "$_u/out"
    [ "$KEEP" != 0 ] || rm -rf "$_u/A" "$_u/B"
}

if [ "${1:-}" = "--one" ]; then
    shift
    unit=${1:?--one needs a unit directory}
    # everything this unit is one of: the two sides, their interpreters
    # and the flags, written once by the run that planned it
    . "$unit/../../pass"
    one_unit "$unit"
    exit 0
fi

# ---- the run ---------------------------------------------------------

skip() { echo "render-differ: $*"; exit 77; }
die()  { echo "render-differ: $*" >&2; exit 2; }

gate=0
KEEP=${RENDER_KEEP:-0}
while [ $# -gt 0 ]; do
    case $1 in
        --gate) gate=1; shift ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^#\{1,\} \{0,1\}//'
                   exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option: $1" ;;
        *) break ;;
    esac
done
[ $# -ge 2 ] || die "usage: render-differ.sh [--gate] <sideA> <sideB> [program ...]"

command -v setsid >/dev/null 2>&1 ||
    skip "setsid is not here, so a render cannot be given a process group of its own and a timeout could not take away what it started -- skipping"

work=$(mktemp -d "${TMPDIR:-/tmp}/render-differ.XXXXXX" 2>/dev/null) || work=
[ -n "$work" ] && [ -d "$work" ] && [ -w "$work" ] ||
    die "could not make a working directory"
trap 'if [ "$KEEP" = 0 ]; then rm -rf "$work"; \
      else echo "render-differ: renders kept in $work"; fi' EXIT
# A signal reaches the trap below, which exits, which reaches the one on
# EXIT. PIPE is one of them because reading a long report through head or
# less is the ordinary way to read one, and the reader closing the pipe
# kills a run that has not tidied up: without it every such reading
# leaves a directory behind, which is how a temporary directory becomes a
# thing somebody has to go and clear out.
trap 'exit 1' INT TERM HUP PIPE

# The launcher, put to a case with a known answer before it is trusted
# with a render. Three things have to hold, and each of them is a way a
# timeout goes wrong rather than a tidiness point:
#
#   the status has to be the program's, or every render is judged by a
#   wrapper's exit rather than by the interpreter's
#
#   the pid waited on has to be the program's, or a run is waited on by
#   number and the number is something else's
#
#   the program has to lead a group of its own, or -pid names a group
#   that was already there: the harness's, and everything else in it
#
# The third is the one that looks fine without being asked. Something on
# the path called setsid that only execs passes the first two -- the
# status is right and the pid is right -- and leaves every render in the
# group that started it, where a timeout reaches for the whole run.
setsid sh -c 'echo $$ > "$1/pgid"; ps -o pgid= -p $$ > "$1/pgrp"; exit 42' \
       probe "$work" >/dev/null 2>&1 &
probe=$!
wait "$probe"
probe_st=$?
probe_pg=$(cat "$work/pgid" 2>/dev/null) || probe_pg=
probe_grp=$(tr -d ' \t' < "$work/pgrp" 2>/dev/null) || probe_grp=
if [ "$probe_st" != 42 ] || [ "$probe_pg" != "$probe" ] ||
   [ "$probe_grp" != "$probe" ]; then
    skip "setsid here does not report its program's status (gave $probe_st for 42), or does not leave it the pid waited on (${probe_pg:-none} for $probe), or does not put it at the head of a group of its own (group ${probe_grp:-none} for pid $probe) -- a timeout could not be aimed at the render alone, skipping"
fi
rm -f "$work/pgid" "$work/pgrp"

# ---- the two sides ---------------------------------------------------
#
# Each is a tree, a data directory or a revision. A path that exists is
# read as a path, and only what is not a path is offered to git, so a
# branch sharing a name with a directory is not silently the other one.
side_rev=
resolve_side() {        # <label> <spec>; sets side_data, side_bin, side_desc
    _label=$1; _spec=$2
    side_bin=
    side_rev=
    if [ -d "$_spec" ] && [ -f "$_spec/data/init.ps" ]; then
        side_data=$(CDPATH= cd -- "$_spec/data" && pwd)
        side_desc="tree $_spec"
        # A tree carries its own interpreter where it has built one, which
        # is what lets two revisions be compared. An interpreter named
        # outright is not that: it says which one to render with, and a
        # build the tree happens to hold does not answer for it. Naming one
        # and rendering with another is how a comparison comes to be about
        # a binary nobody asked for, and it cannot be seen in the result.
        if [ -z "${XPOST:-}" ] && [ -x "$_spec/build/src/bin/xpost" ]; then
            side_bin=$(CDPATH= cd -- "$_spec/build/src/bin" && pwd)/xpost
        fi
        return 0
    fi
    if [ -d "$_spec" ] && [ -f "$_spec/init.ps" ]; then
        side_data=$(CDPATH= cd -- "$_spec" && pwd)
        side_desc="data $_spec"
        return 0
    fi
    if [ -e "$_spec" ]; then
        die "$_label: $_spec is neither a source tree nor a data directory"
    fi
    _rev=$(git -C "$root" rev-parse --verify --quiet "$_spec^{commit}") ||
        die "$_label: $_spec is not a path here and not a revision of $root"
    mkdir -p "$work/side$_label" || die "$_label: could not make it a directory"
    git -C "$root" archive "$_rev" data | tar -x -C "$work/side$_label" ||
        die "$_label: could not read data/ out of $_spec"
    [ -f "$work/side$_label/data/init.ps" ] ||
        die "$_label: $_spec holds no data/init.ps"
    side_data="$work/side$_label/data"
    side_desc="revision $_spec ($(git -C "$root" log -1 --format='%h %s' "$_rev"))"
    side_rev=$_rev
    return 0
}

resolve_side A "$1"; DATA_A=$side_data; BIN_A=$side_bin; DESC_A=$side_desc
rev_a=$side_rev
resolve_side B "$2"; DATA_B=$side_data; BIN_B=$side_bin; DESC_B=$side_desc
rev_b=$side_rev
shift 2

# Two revisions whose C is not the same cannot be told apart by their
# PostScript. Rendering both through one interpreter reports on the data
# alone, and calls that the difference between the revisions.
if [ -n "$rev_a" ] && [ -n "$rev_b" ] && [ -z "$BIN_A$BIN_B" ] &&
   ! git -C "$root" diff --quiet "$rev_a" "$rev_b" -- src 2>/dev/null; then
    die "these revisions differ under src/, so one interpreter over both
      trees' data would report on the data and leave the C unrun. Build
      each revision and name its tree instead of its revision."
fi

default_bin=${XPOST:-$root/build/src/bin/xpost}
[ -n "$BIN_A" ] || BIN_A=$default_bin
[ -n "$BIN_B" ] || BIN_B=$default_bin
for b in "$BIN_A" "$BIN_B"; do
    [ -x "$b" ] || skip "no interpreter at $b -- build one, or name it in XPOST; skipping"
done
# A render's working directory is its own, so it is not where anything
# was named from: a path that was relative to the run reaches nothing
# once a render has moved.
abspath() {
    case $1 in
        /*) echo "$1" ;;
        *)  echo "$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")" ;;
    esac
}
BIN_A=$(abspath "$BIN_A")
BIN_B=$(abspath "$BIN_B")

NS=$(sandbox_flag "$BIN_A")

# Object width, read off each interpreter the way the golden render reads
# it. The two personalities do not render alike, so a narrow build against
# a wide one differs by the build and says nothing about the trees.
cat > "$work/width.ps" <<'PROBEEOF'
2147483647 1 add type /integertype eq
    { (XPOSTWIDTH=wide) }{ (XPOSTWIDTH=narrow) } ifelse =
quit
PROBEEOF
width_of() {            # <interpreter> <datadir>
    # shellcheck disable=SC2086
    XPOST_DATA_DIR=$2 "$1" -q $NS -d null "$work/width.ps" </dev/null 2>/dev/null |
        grep -o 'XPOSTWIDTH=[a-z]*' | head -1 | cut -d= -f2
}
width_a=$(width_of "$BIN_A" "$DATA_A")
width_b=$(width_of "$BIN_B" "$DATA_B")
[ -n "$width_a" ] && [ "$width_a" = "$width_b" ] ||
    die "the two sides' interpreters are ${width_a:-unreadable} and ${width_b:-unreadable} object widths; a comparison across widths reports the build"

# ---- what to render --------------------------------------------------
#
# The programs are named into a plan first, and the plan is what reports,
# so what a run leaves undone is named rather than dropped.
mkdir -p "$work/u" "$work/src" || die "could not make a directory for the units"
cat > "$work/pass" <<EOF
BIN_A='$BIN_A'
DATA_A='$DATA_A'
BIN_B='$BIN_B'
DATA_B='$DATA_B'
NS='$NS'
LIMIT='$LIMIT'
KEEP='$KEEP'
EOF

# A corpus may carry a prelude of definitions its programs assume, which
# is prepended once so that the file both sides read is the same file.
prepared() {            # <corpus> <base> <path>
    if [ "$1" != - ] && [ -f "$here/corpus/$1/prelude" ]; then
        cat "$here/corpus/$1/prelude" "$3" > "$work/src/$1-$2.ps"
        echo "$work/src/$1-$2.ps"
    else
        echo "$3"
    fi
}

held=
nondet=
n=0
programs=0

# A corpus says which of its programs differ from themselves between two
# runs of one tree. That is the corpus's list and this reads it rather
# than keeping one of its own, which would be a second answer to the same
# question. It applies however a program was reached, by the sweep below
# or by being named, because what it says is a property of the program.
add_program() {         # <corpus> <base> <path>
    if [ "$1" != - ] && [ -f "$here/corpus/$1/nondeterministic" ] &&
       grep -qxF "$2" "$here/corpus/$1/nondeterministic"; then
        if [ "${SKIP_NONDET:-0}" != 0 ]; then
            held="$held $1/$2(nondeterministic)"
            return 0
        fi
        nondet="$nondet $1/$2"
    fi
    programs=$((programs + 1))
    _p=$(prepared "$1" "$2" "$3")
    for dev in $DEVICES; do
        n=$((n + 1))
        u=$(printf '%s/u/%04d' "$work" "$n")
        mkdir -p "$u" || die "could not make $u"
        echo "$2" > "$u/base"
        echo "$_p" > "$u/prog"
        echo "$dev" > "$u/dev"
        printf '%s\t%s\t%s\t%s\n' "$u" "$1" "$2" "$dev" >> "$work/plan"
    done
}

basename_of() { basename "$1" | sed 's/\.[Pp][Ss]$//;s/\.[Ee][Pp][Ss]$//'; }

: > "$work/plan"
if [ $# -gt 0 ]; then
    for p in "$@"; do
        [ -f "$p" ] || die "no such program: $p"
        p=$(CDPATH= cd -- "$(dirname -- "$p")" && pwd)/$(basename -- "$p")
        c=$(basename -- "$(dirname -- "$p")")
        [ -d "$here/corpus/$c" ] || c=-
        add_program "$c" "$(basename_of "$p")" "$p"
    done
else
    for d in "$here"/corpus/*/; do
        c=$(basename "$d")
        for p in "$d"*.ps "$d"*.eps; do
            [ -f "$p" ] || continue
            b=$(basename_of "$p")
            # a corpus also says which of its programs it holds out of a
            # sweep, and why -- too slow for a per-file timeout, or past
            # a limit of the build. Sweeping a corpus takes that as read;
            # naming one of them does not, since naming it is asking for
            # it and the reason it is held may be the caller's to lift.
            if [ -f "$d/heldout" ] && grep -qxF "$b" "$d/heldout"; then
                held="$held $c/$b(held out)"
                continue
            fi
            add_program "$c" "$b" "$p"
        done
    done
fi
[ "$n" -gt 0 ] ||
    skip "nothing to render: no program was named and no corpus is fetched under $here/corpus (see corpus/fetch.sh) -- skipping"

ndev=$(echo "$DEVICES" | wc -w | tr -d ' ')
echo "render-differ: A = $DESC_A"
echo "render-differ: B = $DESC_B"
if [ "$BIN_A" = "$BIN_B" ]; then
    echo "render-differ: $BIN_A ($width_a objects)"
else
    echo "render-differ: $BIN_A and $BIN_B ($width_a objects)"
fi
echo "render-differ: $programs programs x $ndev devices = $n units, $JOBS at once, ${LIMIT}s each"
echo "render-differ: devices: $DEVICES"
[ -z "$held" ] || echo "render-differ: held out:$held"
[ -z "$nondet" ] || echo "render-differ: nondeterministic:$nondet"

cut -f1 "$work/plan" | xargs -P "$JOBS" -n1 "$0" --one >"$work/workerlog" 2>&1

# ---- what came back --------------------------------------------------

same=0; differ=0; nothing=0; bad=0; missing=0; nd_differ=0
failed=
prev=
psame=0; pdiff=0; pnone=0; pbad=0; pmiss=0; pdetail=

is_nondet() { case " $nondet " in *" $1/$2 "*) return 0 ;; esac; return 1; }

detail() {              # a line under the program it belongs to
    if [ -z "$pdetail" ]; then pdetail=$1; else pdetail="$pdetail
$1"; fi
}

flush() {
    [ -n "$prev" ] || return 0
    _t=$((psame + pdiff + pnone + pbad + pmiss))
    _s="$psame same"
    [ "$pdiff" = 0 ] || _s="$_s, $pdiff differ"
    [ "$pnone" = 0 ] || _s="$_s, $pnone no output"
    [ "$pbad" = 0 ]  || _s="$_s, $pbad crashed or timed out"
    [ "$pmiss" = 0 ] || _s="$_s, $pmiss NOT EVALUATED"
    if is_nondet "${prev%%/*}" "${prev#*/}"; then
        _s="$_s -- nondeterministic, its output is not this tree's alone"
    fi
    printf '  %-26s %2d devices: %s\n' "$prev" "$_t" "$_s"
    [ -z "$pdetail" ] || printf '%s\n' "$pdetail"
    psame=0; pdiff=0; pnone=0; pbad=0; pmiss=0; pdetail=
}

echo "=== per program"
while IFS='	' read -r u c b dev; do
    [ "$prev" = "$c/$b" ] || { flush; prev="$c/$b"; }
    if [ ! -s "$u/out" ]; then
        missing=$((missing + 1)); pmiss=$((pmiss + 1))
        failed="$failed $c/$b:$dev"
        detail "$(printf '      NOT EVALUATED  %-9s the unit left no report' "$dev")"
        continue
    fi
    IFS='	' read -r state note < "$u/out"
    case $state in
        same)    same=$((same + 1)); psame=$((psame + 1)) ;;
        differ)  differ=$((differ + 1)); pdiff=$((pdiff + 1))
                 is_nondet "$c" "$b" && nd_differ=$((nd_differ + 1))
                 detail "$(printf '      differ         %-9s %s' "$dev" "$note")" ;;
        nothing) nothing=$((nothing + 1)); pnone=$((pnone + 1))
                 detail "$(printf '      no output      %-9s %s' "$dev" "$note")" ;;
        *)       bad=$((bad + 1)); pbad=$((pbad + 1))
                 failed="$failed $c/$b:$dev"
                 detail "$(printf '      %-14s %-9s %s' "$state" "$dev" "$note")" ;;
    esac
done < "$work/plan"
flush

reported=$((same + differ + nothing + bad))
echo "=== summary"
echo "  planned        $n units ($programs programs x $ndev devices)"
echo "  reported       $reported"
echo "  same           $same"
if [ "$nd_differ" = 0 ]; then
    echo "  differ         $differ"
else
    echo "  differ         $differ, $nd_differ of them a program that differs from itself"
fi
echo "  no output      $nothing"
echo "  crashed/timed out  $bad"
echo "  NOT EVALUATED  $missing"

status=0
if [ "$missing" -ne 0 ]; then
    echo "render-differ: $missing of $n units left no report, so this run says"
    echo "      nothing about them:$failed"
    # A unit reports by leaving a file, so a unit that left none said
    # whatever it said on the way out and nowhere else. That is the one
    # place to find why, and naming the unit without it leaves the reader
    # to reproduce a failure the run already has in hand.
    if [ -s "$work/workerlog" ]; then
        echo "      what the workers said on the way out:"
        tail -5 "$work/workerlog" | sed 's/^/      /'
    fi
    status=1
fi
if [ "$bad" -ne 0 ]; then
    echo "render-differ: a render crashed or ran out of time:$failed"
    status=1
fi
if [ "$reported" -ne $((n - missing)) ]; then
    echo "render-differ: $reported reports over $((n - missing)) units that left"
    echo "      one; the counts do not meet"
    status=1
fi
if [ "$gate" = 1 ]; then
    real=$((differ - nd_differ))
    if [ "$real" -ne 0 ] || [ "$nothing" -ne 0 ]; then
        echo "render-differ: the two sides were required to render alike, and"
        echo "      $real unit(s) differ, $nothing rendered nothing at all"
        status=1
    fi
fi
[ "$status" = 0 ] &&
    echo "render-differ: $reported of $n units reported, $differ differ"
exit $status
