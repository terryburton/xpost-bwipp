# Sourced by the run-*.sh wrappers: reach what a run was handed, and
# judge what it answered with.
#
# And say, for every one of them, that this run is taking a census.
# A suite reaching the machinery through `1183615869 internaldict` only
# finds it because of this. PLRM 8 entitles a program to nothing there --
# undocumented contents, subject to change at any time, and an operator
# some interpreters do not have at all -- so a shipped run is answered
# with a dictionary holding nothing, and the machinery keeps its own
# where no name reaches it. A run that says it is taking a census is
# answered with the machinery's, which is what lets a suite ask what a
# shipped run refuses to be asked. Said here rather than in each wrapper
# because a wrapper added later would otherwise be answered with the
# empty one and report a moved member as a missing one.
#
# It also decides which entry points the lockdown keeps, so a census run
# boots to a different language and reads and writes an image named
# apart from the ordinary one.
XPOST_CENSUS=1
export XPOST_CENSUS
#
# A run answers in one of two ways, and there is an entry point here for
# each. A run that prints its own verdict is judged by verdict_ok. A run
# that reports through its exit status, leaving the wrapper to inspect
# whatever it produced, is judged by verdict_run. Both carry the same
# failure half, so neither wrapper has to spell it.
#
# A run reports its own result, and a wrapper that looks for SUCCESS
# anywhere in the output accepts a run that printed a failure and then
# printed SUCCESS. Both halves of that happen. A test whose assertion
# fails says so and carries on to a verdict it computes from something
# else; a test whose failure branch ends the run reaches its verdict
# anyway if the run does not stop where it was told to. Either way the
# failure is on screen, in a log nobody reads because the test passed.
#
# So the verdict is not a line to be found: it is a line that counts only
# when the same run printed no failure. That is the whole rule, kept here
# so that it is the same rule in every wrapper -- one enforced in some of
# them and not in others is the same hole with a longer way in.
#
# The same two halves make the whole of verdict_run. A wrapper that reads
# only the artifacts a run left behind accepts a run that wrote every one
# of them and then died on the way out, which is where a device's
# teardown lives; and a wrapper that reads only the status accepts a run
# that complained its way to a clean exit. What a run left and what it
# said are separate answers and a pass needs both.
#
# Position is deliberately not the rule, and neither is a line of the
# verdict's own. A wrapper that folds the log channel in gets whatever
# the device said on its way out, before the verdict and after it. A
# program that prints without a newline leaves the verdict on the end of
# what it printed. And a wrapper run at a terminal is greeted before its
# program starts and told of each page as it goes, either of which can
# sit against the verdict -- none of which happens to these wrappers,
# which redirect the standard input away from one, but none of which is
# the test's to arrange either. A rule about position would judge the
# framing around the answer rather than the answer. What the rule does
# need is that the word is the run's own and not the tail of a longer
# one, which is what the leading boundary below is for.
#
# The verdict is SUCCESS everywhere but the one check whose PostScript
# side answers PASS, FAIL or INCONCLUSIVE because it has a third thing
# to say, and says it at the head of a line it goes on to explain in.
# That wrapper names the shape it is looking for; the rule around it is
# the same one either way.

# Anchor a path so that it names the same thing from anywhere.
#
# A wrapper that starts its runs in a directory of its own has to do this
# to everything it was handed, because what it was handed is relative to
# where the wrapper itself was started. What counts as already anchored
# is not a leading slash alone: a host whose names carry the volume they
# are on writes them as C:/dir, which is relative to nothing, and putting
# the current directory in front of one produces a name for nowhere.
#
# Stated once because it is the same question in every wrapper, and one
# wrapper answering it differently is one platform's worth of runs
# looking for their arguments in a place that does not exist.
path_anchor() {
    case $1 in
        /* | ?:[/\\]*) printf '%s\n' "$1" ;;
        *)             printf '%s/%s\n' "$PWD" "$1" ;;
    esac
}

# Run a program under a wall-clock limit, on the hosts that carry a
# command for it and on the ones that do not. The base system of macOS
# has no timeout(1), and a wrapper naming it outright has every run exit
# 127 there -- which reads as the program failing rather than as the
# limit being unavailable. So the limit is kept where the command is
# missing rather than dropped: what it guards is a run that hangs, and
# the shell does the same job with a sleeper beside the run, whichever
# finishes first deciding. The sleeper's output goes nowhere, so a
# command substitution around a call does not wait on it as well.
#   $1  the limit in seconds; the rest is the command and its arguments
if command -v timeout > /dev/null 2>&1; then
    run_limited() { _rl=$1; shift; timeout "$_rl" "$@"; }
elif command -v gtimeout > /dev/null 2>&1; then
    run_limited() { _rl=$1; shift; gtimeout "$_rl" "$@"; }
else
    run_limited() {
        _rl=$1; shift
        # 0<&0 keeps the standard input the caller redirected. A background
        # command in a non-interactive shell is handed /dev/null unless it
        # names a source of its own, and a redirection written on the call
        # to this function is the function's, not the background command's.
        # Every caller here feeds /dev/null, so nothing turns on it today --
        # which is why it is written down rather than left out: a caller
        # that piped a program in would read end-of-file instead, and a run
        # that read nothing reports as a run that found nothing wrong. The
        # arm this is in is the one no host with timeout(1) reaches, so the
        # difference would show on one platform and not the others.
        "$@" 0<&0 &
        _rl_job=$!
        ( sleep "$_rl"; kill -9 "$_rl_job" ) > /dev/null 2>&1 &
        _rl_watch=$!
        wait "$_rl_job"; _rl_rc=$?
        kill "$_rl_watch" > /dev/null 2>&1
        return $_rl_rc
    }
fi

# Whether the peak resident size of a run of the program under test can
# be read on this machine.
#
# The timer answering is not the question. A machine can carry a timer
# that reports a figure for everything it starts and follows only the
# processes it built itself: what it then says about a program built
# another way is a constant, the same for a page of ten rows as for a
# page of four thousand. A weighing made from that reads every route as
# holding nothing, which is the answer a route that held the whole page
# would want.
#
# So the timer is put to the program itself, over two pages whose rasters
# differ by tens of mebibytes, and is believed only where its two
# readings do. A machine it cannot be believed on is told so by the
# wrappers, which weigh nothing there rather than weighing a constant.
_peak_rss_of() {    # $1 program; $2 directory; $3 case; sets _pr_kib
    _pr_kib=''
    ( cd "$2" && /usr/bin/time -f '%M' -o "$3.rss" \
        "$1" -q -d null "$3.ps" </dev/null >"$3.out" 2>/dev/null )
    grep -q PROBEDONE "$2/$3.out" 2>/dev/null || return 1
    [ -f "$2/$3.rss" ] || return 1
    _pr_kib=$(tail -1 "$2/$3.rss")
    case ${_pr_kib:-x} in *[!0-9]*) return 1 ;; esac
    return 0
}

peak_rss_reads() {  # $1 the program under test
    peak_rss_why='the peak resident size of a run cannot be read on this machine'
    # A runtime that keeps what a run gives back, and maps memory of its
    # own beside it, is what the reading follows once one is there: a
    # route that finishes a band and takes the next reads as a route that
    # kept both, and a weighing of one route against another comes out
    # the wrong way round rather than merely large. The runtimes that do
    # it -- the address, leak, memory and thread ones -- answer when
    # asked for their flags. The instrumentation that does not, which
    # leaves the heap where it was, is not asked, and a build carrying
    # only that one still weighs.
    for _pr_v in ASAN_OPTIONS LSAN_OPTIONS MSAN_OPTIONS TSAN_OPTIONS; do
        if env "$_pr_v=help=1" "$1" -q -d null /dev/null </dev/null 2>&1 |
           grep -q 'Available flags for'; then
            peak_rss_why='this build carries a sanitizer runtime, which keeps what a run gives back and maps memory of its own, so what a run holds is not what the reading would follow'
            return 1
        fi
    done
    /usr/bin/time -f '%M' true >/dev/null 2>&1 || return 1
    _pr_dir=$(mktemp -d) || return 1
    # What the two runs differ by is thirty mebibytes the interpreter is
    # holding when it reports, and nothing else: no device is asked for
    # a page, so what a class does or does not keep a raster of decides
    # nothing here.
    printf '%%!PS\n(PROBEDONE) print\n' > "$_pr_dir/small.ps"
    printf '%%!PS\n/a 500 array def\n0 1 499 { a exch 60000 string put } for\n(PROBEDONE) print\n' \
        > "$_pr_dir/big.ps"
    _pr_ok=1
    _peak_rss_of "$1" "$_pr_dir" small || _pr_ok=0
    _pr_small=${_pr_kib:-0}
    _peak_rss_of "$1" "$_pr_dir" big || _pr_ok=0
    _pr_big=${_pr_kib:-0}
    rm -rf "$_pr_dir"
    [ "$_pr_ok" -eq 1 ] || return 1
    # A timer reading this program's own size answers most of those
    # thirty mebibytes; one reading something else answers the few
    # hundred kibibytes a machine moves about by, in either direction.
    # Eight mebibytes tells the two apart with room to spare in both.
    [ $((_pr_big - _pr_small)) -ge 8000 ]
}

# Not everything a run is handed is a path. A -Dname=/token argument
# names something inside the interpreter, and one host's shell runtime
# rewrites arguments that look like paths on their way into a program it
# did not itself build: /token arrives volume-qualified, and the run
# executes it and stops on a name it has never heard of. Arguments
# carrying a token are excluded from that rewriting here, once, for every
# wrapper; paths are left to be converted as they must be. The variable
# means nothing to hosts that do no such rewriting.
MSYS2_ARG_CONV_EXCL="${MSYS2_ARG_CONV_EXCL:+$MSYS2_ARG_CONV_EXCL;}-D"
export MSYS2_ARG_CONV_EXCL

# What a run prints to report a failure. Every spelling the suite uses
# starts with one of these: FAIL, FAILURE, FAILURES, MISMATCH.
#
# The line boundary is spelt as two whole branches rather than as (^|...)
# inside one. A caret anchors only at the start of an expression; what it
# means anywhere else is left undefined, and a matcher that reads it as
# neither an anchor nor a literal matches no line at all. What that costs
# here is not a wrong answer but no answer: a rule that never fires, and
# a suite that comes back clean because nothing was asked.
VERDICT_FAILURE_RE='^(FAIL|MISMATCH)|[^A-Za-z](FAIL|MISMATCH)'

# The failure half of the rule, which both entry points below share.
# Prints what the run said about a failure and answers yes when it said
# anything at all.
_verdict_complained() {
    printf '%s\n' "$1" | grep -qE "$VERDICT_FAILURE_RE" || return 1
    echo "FAILURES: $2 printed a failure:"
    printf '%s\n' "$1" | grep -E "$VERDICT_FAILURE_RE" | sed 's/^/      /'
    return 0
}

# Judge a run that reports through its exit status, the wrapper having
# read whatever the run produced for itself.
#   $1  the status the run left
#   $2  the run's output, as captured (the empty string where a wrapper
#       kept none: the status half still holds)
#   $3  what to call the run in a complaint (optional)
# Prints what was wrong and returns non-zero unless the run left a zero
# status and printed no failure.
# Whether the program under test was built without a face library. Such
# a build seeds NOFACES into systemdict, and the probe asks the
# interpreter itself: a wrapper watching findfont refuse cannot tell
# that build apart from a host with no fonts installed, and the two
# deserve different answers -- the first is a configuration this suite
# may be meaningless in, the second is a host the run-time skip already
# names.
# The flag that lets a run reach what a shipped run seals, or nothing on a
# build that has no such flag. Asked of the interpreter rather than assumed,
# because a build without it must still be testable -- and because forty-one
# guards were each carrying their own copy of the asking.
sandbox_flag() {    # $1 the program under test; echoes the flag or nothing
    if "$1" -h 2>/dev/null | grep -q -- '--no-sandbox'; then
        echo '--no-sandbox'
    fi
}

# Report a failure and remember that one happened. The caller's own `fail`
# is what is set, which is the contract every copy of this already had: a
# guard says `note "what went wrong" "and the detail"` and tests `fail` at
# the end; the detail lines are indented under the first, which is what a
# reader of a failing run sees.
#
# Fourteen guards carried this verbatim. Five others define a `note` of
# their own and still do: two report under a different word entirely and
# one counts the check as well, so they are different helpers that happened
# to share a name. A definition in a guard shadows this one, which is what
# makes that divergence a choice rather than an accident.
note() {
    echo "FAILURES: $1"
    shift
    for n_line in "$@"; do
        echo "      $n_line"
    done
    fail=1
}

# A build with no face library cannot show text, so a guard that reads text
# skips rather than fails. What the guard needed a face FOR is asked for
# here rather than fixed: the tree carries two wordings and both are right
# about their own guard. What is not the guard's business -- that this is a
# skip, that it leaves under status 77, and how the absence is described --
# is settled once.
skip_if_faceless() {    # $1 the program under test; $2 what needed a face
    if faceless_build "$1"; then
        echo "SKIPPED: $2, and this build carries no face library"
        exit 77
    fi
}

faceless_build() {    # $1 the program under test
    _fb_ps=${TMPDIR:-/tmp}/xpost-faceprobe.$$.ps
    printf 'systemdict /NOFACES known { (NOFACES) = } if\n' > "$_fb_ps" \
        || return 1
    _fb_out=$("$1" -q --no-sandbox -d null "$_fb_ps" </dev/null 2>/dev/null)
    rm -f "$_fb_ps"
    [ "${_fb_out:-}" = NOFACES ]
}

# A run that cannot ask its question in this build says so itself, on a
# line beginning SKIPPED: that names the reason. The wrapper turns that
# into the harness's skip status -- loudly, with the reason on screen --
# rather than into a pass or a failure. A run that complained and then
# skipped is a failure: the complaint is the verdict, and a skip after
# it would bury a red run. Exits rather than returns on both of those,
# so no wrapper can fall past a skip into a byte comparison of output
# the run never produced; a run that did not skip returns 1 and the
# wrapper judges it as ever.
verdict_skipped() {    # $1 output; $2 what to call the run
    printf '%s\n' "$1" | grep -q '^SKIPPED:' || return 1
    if _verdict_complained "$1" "${2:-the run}"; then
        exit 1
    fi
    printf '%s\n' "$1" | grep '^SKIPPED:'
    exit 77
}

verdict_run() {
    _verdict_st=$1
    _verdict_out=$2
    _verdict_who=${3:-the run}
    _verdict_bad=0

    _verdict_complained "$_verdict_out" "$_verdict_who" && _verdict_bad=1
    if [ "$_verdict_st" -ne 0 ]; then
        echo "FAILURES: $_verdict_who exited with status $_verdict_st"
        _verdict_bad=1
    fi
    return "$_verdict_bad"
}

# Judge one run's output.
#   $1  the output, as captured
#   $2  what to call the run in a complaint (optional)
#   $3  what the run's success line looks like (optional); the default
#       is SUCCESS ending a line, bounded so that it is the run's own
#       word and not the tail of a longer one
# Prints what was wrong and returns non-zero unless the run reported
# success exactly once and reported no failure.
verdict_ok() {
    _verdict_out=$1
    _verdict_who=${2:-the run}
    _verdict_re=${3:-'^SUCCESS[[:space:]]*$|[^A-Za-z]SUCCESS[[:space:]]*$'}

    _verdict_complained "$_verdict_out" "$_verdict_who" && return 1

    # a trailing carriage return is a line ending, not content: a run on a
    # platform whose text output carries one still reported success
    _verdict_n=$(printf '%s\n' "$_verdict_out" \
                 | grep -cE "$_verdict_re") || _verdict_n=0
    if [ "$_verdict_n" -eq 0 ]; then
        echo "FAILURES: $_verdict_who did not report success"
        return 1
    fi
    # A run reports once. Twice means two runs' output was judged as one,
    # or that a program which should have stopped at its first verdict
    # went on to reach a second.
    if [ "$_verdict_n" -gt 1 ]; then
        echo "FAILURES: $_verdict_who reported success $_verdict_n times;"
        echo "      one run reports one verdict"
        return 1
    fi
    return 0
}

# A scratch directory for a run, removed however the wrapper ends.
#
# Removing it is arranged here rather than left to each wrapper, because
# a trap on EXIT alone does not run when the shell is killed by a signal
# and the test runner enforces its time limits by sending one. So the
# signals it sends are caught here as well, and a wrapper that runs long
# or a suite stopped at the keyboard leaves nothing behind. Removing the
# directory on the way out instead would be stepped past by any exit
# taken before it.
verdict_workdir() {
    work=$(mktemp -d 2>/dev/null) || work=
    if [ -z "$work" ] || [ ! -d "$work" ] || [ ! -w "$work" ]; then
        echo "FAILURES: could not make a scratch directory (is TMPDIR writable?)"
        exit 1
    fi
    trap 'rm -rf "$work"' EXIT INT TERM
}
