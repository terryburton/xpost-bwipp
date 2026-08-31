#!/bin/sh
# Meson test wrapper: run a PostScript program under a leak checker and
# require the process to end holding nothing it allocated on its behalf.
#
# Some of what the interpreter holds for a program lives outside virtual
# memory: the stream and coding state behind a file or a filter, the font
# program a face reads its glyphs out of. What becomes of those has no
# expression in PostScript at all. The program's own assertions say the
# bytes came through; only the checker can say the memory went away.
#
# Where there is no checker the question cannot be asked and the test
# skips, rather than passing without having tested anything.
#
#   $1  path to the built xpost binary
#   $2  path to the PostScript program
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# the run happens in a scratch directory, so every path must survive the
# change of directory
# an absolute path may begin with a drive letter as well as a slash;
# prepending the working directory to one of those makes every
# invocation a path that does not exist
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac
case $script in /* | ?:/* | ?:\\*) ;; *) script=$PWD/$script ;; esac

if ! command -v valgrind >/dev/null 2>&1; then
    echo "SKIP: no leak checker on this platform"
    exit 77
fi

verdict_workdir

# One allocation the workload asks for is meant to be refused: a coding
# whose buffer is four thousand million bytes, so that a filter fails
# after its object, its struct and its claim on the target already exist.
# A host that hands out address space it does not have would grant the
# request, so the allowance is capped below it -- far above what the
# checker and the interpreter need together, and nothing else in the
# workload comes near it. Where the shell cannot lower the allowance the
# request is granted, the filter is built, and the workload closes it
# instead.
log=$work/valgrind.log
out=$(
    cd "$work" || exit 1
    ulimit -v 3145728 2>/dev/null
    valgrind --leak-check=full --show-leak-kinds=definite,indirect \
             --error-exitcode=9 --log-file="$log" \
             "$xpost" -q --no-sandbox -d null "$script" </dev/null 2>&1
)
status=$?
printf '%s\n' "$out"

if [ "$status" -ne 0 ]; then
    echo "FAILURES: the interpreter exited with status $status"
    [ -f "$log" ] && cat "$log"
    exit 1
fi
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the suite" || exit 1

# what the checker found: anything lost outright, or lost through
# something that was, is memory the run held for the program and never
# gave up
lost=$(sed -n 's/^==[0-9]*==  *\(definitely\|indirectly\) lost: \([0-9,]*\) bytes.*/\2/p' \
       "$log" | tr -d ',')
if [ -z "$lost" ]; then
    echo "FAILURES: the checker reported no leak summary"
    cat "$log"
    exit 1
fi
for n in $lost; do
    if [ "$n" -ne 0 ]; then
        echo "FAILURES: the run ended holding memory it allocated for the program"
        cat "$log"
        exit 1
    fi
done

echo "SUCCESS"
exit 0
