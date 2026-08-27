#!/bin/sh
#
# The confinement an ordinary command line arranges.
#
# Every other PostScript test here runs with --no-sandbox, so the
# configuration every real run has -- the sandbox the command line raises
# by default -- was exercised by nothing that speaks PostScript. This
# runs the shipped default and holds it to what the option's own help
# text promises: the program reaches its working area and nothing else.
#
# The run happens from a scratch directory of its own, because the
# permitted set is built from where the run stands: the current
# directory, the temporary directory, and the directories of the input
# and output files. A run from the source tree would be permitted the
# source tree, and the file it is refused has to be somewhere none of
# those reach. So both the working directory and the refused file are
# made here, as siblings, and the program is copied in -- its own
# directory is read-permitted too, which would otherwise be the way in.
#
#   $1  path to the xpost binary
#   $2  path to the source tree root
set -u
xpost=${1:?usage: run-cli-sandbox-test.sh <xpost> <srcroot>}
src=${2:?usage: run-cli-sandbox-test.sh <xpost> <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
. "$(dirname "$0")/verdict.sh"
guard_require_srcroot "$src"
if [ ! -x "$xpost" ]; then
    echo "FAILURES: the interpreter is not an executable: $xpost"
    exit 1
fi
# the run below happens from a directory of its own, so a binary named
# relative to where this started would not be there to find
case $xpost in
    /*) ;;
    *) xpost=$(pwd)/$xpost ;;
esac

prog="$src/tests/cli_sandbox_test.ps"
[ -r "$prog" ] || { echo "FAILURES: not readable: $prog"; exit 1; }

verdict_workdir
trap 'rm -rf "$work" "$libwork"' EXIT INT TERM

# The combined program goes in a directory of its own: these runners set up a
# scratch tree the suite itself looks at.
libwork=$(mktemp -d) || libwork=$work
. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$prog" "$libwork"

mkdir "$work/run" || { echo "FAILURES: could not make the working directory"; exit 1; }
cp "$testlib_run" "$work/run/prog.ps" || { echo "FAILURES: could not place the program"; exit 1; }
mkdir "$work/elsewhere" || { echo "FAILURES: could not make the other directory"; exit 1; }
# Where the output goes, kept apart from where the run stands. In most
# invocations they are the same place, and then the working-directory
# permit covers everything the output permit would have -- so a test that
# let them coincide could not tell the two apart.
mkdir "$work/dest" || { echo "FAILURES: could not make the output directory"; exit 1; }
echo beside > "$work/dest/beside.txt" ||
    { echo "FAILURES: could not write the file beside the output"; exit 1; }
# A temporary directory of the run's own, with a file already in it. The
# default does not permit the temporary directory, and the program checks
# that; naming one here rather than letting the run inherit the system's
# means the file it is refused is one this test made, so a refusal cannot
# be a file that was never there.
mkdir "$work/tmp" || { echo "FAILURES: could not make the temporary directory"; exit 1; }
echo scratch > "$work/tmp/f" ||
    { echo "FAILURES: could not write the file in the temporary directory"; exit 1; }
echo secret > "$work/elsewhere/f" ||
    { echo "FAILURES: could not write the file to be refused"; exit 1; }

# The refused file must exist and be readable to this user, or the
# refusal below would be indistinguishable from a missing name.
[ -r "$work/elsewhere/f" ] ||
    { echo "FAILURES: the file to be refused is not readable outside the run"; exit 1; }

# No --no-sandbox: this is the shipped configuration. The output goes to
# a directory of its own, so that what naming an output file grants can
# be told apart from what standing in a directory grants.
out=$(cd "$work/run" && TMPDIR="$work/tmp" XPOST_DATA_DIR="$src/data" "$xpost" \
      -q -d ppm -o "$work/dest/out.ppm" "-DOUTSIDE=($work/elsewhere/f)"\
      "-DOUTSIB=($work/dest/beside.txt)" "-DOUTNEW=($work/dest/made.txt)" \
      "-DSCRATCH=($work/tmp/f)" "-DTMPNEW=($work/tmp/made.txt)" \
      prog.ps </dev/null 2>&1)
st=$?
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the command-line sandbox test" ||
    { printf '%s\n' "$out" | sed 's/^/      /'; exit 1; }
if [ "$st" -ne 0 ]; then
    echo "FAILURES: the sandbox test exited with status $st after reporting"
    exit 1
fi

# What the program was told and what the filesystem did are two claims.
if [ ! -s "$work/elsewhere/f" ]; then
    echo "FAILURES: the file outside the permitted set was emptied"
    exit 1
fi
if [ ! -s "$work/tmp/f" ]; then
    echo "FAILURES: the file in the temporary directory was emptied"
    exit 1
fi
if [ -e "$work/tmp/made.txt" ]; then
    echo "FAILURES: a file was made in the temporary directory"
    exit 1
fi
if [ ! -s "$work/dest/beside.txt" ]; then
    echo "FAILURES: a file beside the output was emptied"
    exit 1
fi
if [ -e "$work/dest/made.txt" ]; then
    echo "FAILURES: a file was made beside the output"
    exit 1
fi
if [ ! -s "$work/dest/out.ppm" ]; then
    echo "FAILURES: the output itself was not written"
    exit 1
fi

printf '%s\n' "$out" | grep -v '^SUCCESS' | sed 's/^/  /'
echo "SUCCESS (the default command line confines a program to its working area)"
