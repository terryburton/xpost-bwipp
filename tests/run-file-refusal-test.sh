#!/bin/sh
#
# What a refused file operation is called.
#
# The interesting case cannot be built from PostScript: it needs a
# directory the run may search and not write, so that the file inside it
# is readable while removing or renaming it is refused. That is what
# separates a refusal from a missing name and from an unopenable file.
# Those three are one answer away from each other, and reporting a
# refusal as ioerror would name a device fault on a run where the device
# was fine.
#
# The directory is made here, handed to the program by name, and taken
# back afterwards whatever the run did. Its mode is 500: searchable and
# readable, not writable, which is what makes the removal fail with a
# permission errno rather than a missing-name one.
#
# A run as a user who may write anywhere regardless -- root, or a
# platform whose permission bits do not stop the owner -- cannot see the
# refusal at all. Such a run is SKIPPED rather than passed, because a
# test that cannot reach its subject and says nothing is the shape this
# suite exists to avoid.
#
#   $1  path to the xpost binary
#   $2  path to the source tree root
set -u
xpost=${1:?usage: run-file-refusal-test.sh <xpost> <srcroot>}
src=${2:?usage: run-file-refusal-test.sh <xpost> <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
. "$(dirname "$0")/verdict.sh"
guard_require_srcroot "$src"
if [ ! -x "$xpost" ]; then
    echo "FAILURES: the interpreter is not an executable: $xpost"
    exit 1
fi

prog="$src/tests/file_refusal_test.ps"
[ -r "$prog" ] || { echo "FAILURES: not readable: $prog"; exit 1; }

verdict_workdir
# 500 is restored before the removal: a directory with no write bit
# cannot have its contents unlinked, including by the trap.
trap 'chmod 700 "$work/ro" 2>/dev/null; rm -rf "$work" "$libwork"' EXIT

# The combined program goes in a directory of its own: these runners set up a
# scratch tree the suite itself looks at.
libwork=$(mktemp -d) || libwork=$work
. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$prog" "$libwork"

mkdir "$work/ro" || { echo "FAILURES: could not make the directory"; exit 1; }
echo content > "$work/ro/f" || { echo "FAILURES: could not write the file"; exit 1; }
chmod 500 "$work/ro" || { echo "FAILURES: could not take the write bit off"; exit 1; }

# Can this run actually be refused? If the removal succeeds here, the
# platform or the user does not enforce the bit and the subject of this
# test does not exist for them.
if ( cd "$work/ro" 2>/dev/null && rm -f f 2>/dev/null && [ ! -e f ] ); then
    echo "SKIP: this user may remove a file from a directory with no write bit,"
    echo "      so the refusal this holds cannot arise here"
    exit 77
fi

# Both halves: what the run said, and how it ended. A run that printed
# its verdict and then died on the way out has not passed.
out=$(XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null \
      -o /dev/null "-DRODIR=($work/ro)" "$testlib_run" </dev/null 2>&1)
st=$?
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the refusal test" || { printf '%s\n' "$out" | sed 's/^/      /'; exit 1; }
if [ "$st" -ne 0 ]; then
    echo "FAILURES: the refusal test exited with status $st after reporting"
    exit 1
fi
printf '%s\n' "$out" | grep -v '^SUCCESS' | sed 's/^/  /'
echo "SUCCESS (a refused removal and rename are both invalidfileaccess)"
