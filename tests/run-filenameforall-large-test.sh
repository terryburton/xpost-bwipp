#!/bin/sh
# Meson test wrapper: fill a scratch directory with more names than the
# cursor field of an object counts, and run the enumeration in it.
#
# The number of names an enumeration walks is bounded by the file system,
# not by the object carrying the enumeration's state, so the directory has
# to be made rather than described: 65535 names beginning with f, which is
# the largest enumeration a sixteen-bit cursor reaches, and one beginning
# with g, which puts a template of (*) one name past it.
#
# A file system that will not take that many names in one directory cannot
# make the check, so the run skips with the reason rather than passing
# without having enumerated anything.
#
#   $1  path to the built xpost binary
#   $2  path to the PostScript program
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# the run happens in the scratch directory, so both paths must survive the
# change of directory
# an absolute path may begin with a drive letter as well as a slash;
# prepending the working directory to one of those makes every
# invocation a path that does not exist
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac
case $script in /* | ?:/* | ?:\\*) ;; *) script=$PWD/$script ;; esac

near=65535

verdict_workdir
if [ -z "$work" ] || [ ! -d "$work" ] || [ ! -w "$work" ]; then
    echo "SKIP: could not make a scratch directory (is TMPDIR writable?)"
    exit 77
fi
libwork=$(mktemp -d 2>/dev/null) || libwork=$work
trap 'rm -rf "$work" "$libwork"' EXIT INT TERM

# The combined program goes OUTSIDE $work. The run happens in $work and this
# suite counts every name it finds there, so a file of ours in that directory
# is one more name than the count it is checking.
. "$(dirname "$0")/testlib-prepend.sh"
testlib_prepend "$script" "$libwork"

# `true` rather than `:`: a redirection that fails on a special built-in
# ends a non-interactive shell outright, which would report the file
# system's refusal as a failure of the interpreter
i=0
while [ "$i" -lt "$near" ]; do
    true > "$work/f$i" 2>/dev/null || break
    i=$((i + 1))
done
true > "$work/g0" 2>/dev/null

if [ "$i" -ne "$near" ] || [ ! -f "$work/g0" ]; then
    echo "SKIP: the file system took only $i of $near names in one directory"
    exit 77
fi

out=$(
    cd "$work" || exit 1
    "$xpost" -q --no-sandbox -d null "$testlib_run" </dev/null 2>&1
)
status=$?
printf '%s\n' "$out"

if [ "$status" -ne 0 ]; then
    if [ "$status" -gt 128 ]; then
        echo "FAILURES: the interpreter was killed by signal $((status - 128))"
    else
        echo "FAILURES: the interpreter exited with status $status"
    fi
    exit 1
fi
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the suite"
