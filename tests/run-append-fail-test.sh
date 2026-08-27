#!/bin/sh
# Meson test wrapper: run a vector device's append-completeness check under
# an address-space limit that forces the content accumulator's growth to
# fail mid-job. The PostScript side reports PASS, FAIL or INCONCLUSIVE; a
# crash or an unexpected exit is a failure in its own right. Platforms
# whose shell cannot impose the limit (or whose allocator ignores it)
# leave the check inconclusive and the test skips.
#   $1  path to the built xpost binary
#   $2  path to the append-fail test program
#   $3  device name (pdfwrite, svgwrite)
set -u
xpost=$1
script=$2
device=$3
. "$(dirname "$0")/verdict.sh"

# 200 MB of address space: enough for the interpreter, the calibration
# fills and the pre-grown VM; not enough for the accumulator to double
# past 64 MB.
limitkb=200000
if ! ( ulimit -v "$limitkb" ) 2>/dev/null; then
    echo "SKIP: ulimit -v unsupported here"
    exit 77
fi

# The limit above is calibrated against the collector's own settings: it
# has to leave room for the interpreter and the calibration fills while
# denying the accumulator its next doubling. Collecting on a different
# schedule moves what the job allocates and when, so the limit no longer
# lands where this check needs it.
if [ -n "${XPOST_GC_THRESHOLD:-}" ]; then
    echo "SKIP: the address-space limit is calibrated for the default collector"
    exit 77
fi

# a build that cannot start at all under the limit (a sanitizer runtime
# reserves far more address space) cannot host this check
if ! ( ulimit -v "$limitkb"; "$xpost" -V ) >/dev/null 2>&1; then
    echo "SKIP: this build does not start under the limit"
    exit 77
fi

# A sink for output nothing reads. /dev/null is a POSIX name for it and
# the platform null device is not that word everywhere, while a scratch
# file is a file wherever the interpreter runs; the name is relative so
# that the shell and a program built for another environment need not
# read one absolute path the same way.
out=./append-fail-$$.out
trap 'rm -f "$out"' EXIT INT TERM
result=$(
    ulimit -v "$limitkb"
    "$xpost" -q -d "$device" -o "$out" "$script" </dev/null 2>/dev/null
)
status=$?
printf '%s\n' "$result"
if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    exit 1
fi
if printf '%s\n' "$result" | grep -q '^INCONCLUSIVE'; then
    echo "SKIP: the limit never bit on this platform"
    exit 77
fi
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$result" "the suite"
verdict_ok "$result" "the check" '^PASS'
