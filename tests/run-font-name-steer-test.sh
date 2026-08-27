#!/bin/sh
#
# A font name does not name a file.
#
# The font machinery opens three files without consulting the
# file-access sandbox, and publishes what it reads into the running
# program as /sfnts and /CharStrings. That is correct -- a system font
# is outside every permitted tree and reading it is the point -- and it
# is safe only for as long as the program cannot choose which file gets
# opened. The path comes from the font-matching library, which answers
# with the file of the font it matched rather than a file named in the
# request, so a request carrying the matcher's own pattern syntax gets
# the matched font anyway.
#
# That is a property of a library this tree does not own. It has been
# measured on the version installed here; a different version, or a
# change in how xpost calls it, could make the requested file win
# without anything else looking different. This holds it.
#
# The staged file is deliberately NOT a font and NOT in a font
# directory: it is bytes with a marker at the front, and the assertion
# is that the marker never reaches the program.
#
# A machine with no usable fonts SKIPS rather than passes. A test that
# cannot reach its subject and says nothing is the shape this suite
# exists to avoid.
#
#   $1  path to the xpost binary
#   $2  path to the source tree root
set -u
xpost=${1:?usage: run-font-name-steer-test.sh <xpost> <srcroot>}
src=${2:?usage: run-font-name-steer-test.sh <xpost> <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
. "$(dirname "$0")/verdict.sh"
guard_require_srcroot "$src"
if [ ! -x "$xpost" ]; then
    echo "FAILURES: the interpreter is not an executable: $xpost"
    exit 1
fi

prog="$src/tests/font_name_steer_test.ps"
[ -r "$prog" ] || { echo "FAILURES: not readable: $prog"; exit 1; }

verdict_workdir
trap 'rm -rf "$work" "$libwork"' EXIT INT TERM

# The combined program goes in a directory of its own: these runners set up a
# scratch tree the suite itself looks at.
libwork=$(mktemp -d) || libwork=$work
. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$prog" "$libwork"

marker=XPOSTSTEERMARKER
staged="$work/not-a-font.dat"
{ printf '%s' "$marker"; printf 'and then some bytes that are not a font at all\n'; } > "$staged" ||
    { echo "FAILURES: could not stage the file"; exit 1; }
[ -s "$staged" ] || { echo "FAILURES: the staged file is empty"; exit 1; }

# Can this machine match a font at all? If findfont cannot produce one,
# the assertions below would all pass by having nothing to look at.
probe=$("$xpost" -q -d null -o /dev/null --no-sandbox \
        "$src/tests/font_name_steer_probe.ps" </dev/null 2>&1)
case $probe in
    *FONTOK*) ;;
    *) echo "SKIP: no font could be matched here, so the publication this"
       echo "      holds cannot be reached"
       exit 77 ;;
esac

# --no-sandbox so that a refusal from the file layer cannot be mistaken
# for the matcher declining to name the staged file: this test is about
# what the matcher answers, and the sandbox is held elsewhere.
out=$(XPOST_DATA_DIR="$src/data" "$xpost" -q -d null -o /dev/null --no-sandbox \
      "-DMARKER=($marker)" \
      "-DSTAGED=($staged)" \
      "-DPATTERN=(Foo:file=$staged)" \
      "-DFAMPAT=(Helvetica:file=$staged)" \
      "$testlib_run" </dev/null 2>&1)
st=$?
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the font-name steering test" ||
    { printf '%s\n' "$out" | sed 's/^/      /'; exit 1; }
if [ "$st" -ne 0 ]; then
    echo "FAILURES: the steering test exited with status $st after reporting"
    exit 1
fi

printf '%s\n' "$out" | grep -v '^SUCCESS' | sed 's/^/  /'
echo "SUCCESS (a font name reaches the matcher's font, not a file it names)"
