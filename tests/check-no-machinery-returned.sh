#!/bin/sh
#
# No operator hands back a machinery object when a program calls it.
#
# The reachability register counts the entry points a program can name; it
# does not call them, so it cannot see what one answers with. That gap has
# already cost this tree once: .privatedict was a single counted name whose
# call returned thirty-nine members naming every device maker and every
# device class. The count said "one entry point". The call handed over a map.
#
# So the walk calls them, with nothing on the operand stack, and looks at what
# is left. An operator that needs operands raises stackunderflow and is
# answered; one that does not is executed and its result examined.
#
# Run WITHOUT a census, because the question is what a shipped run answers.
# The shared guard preamble asks for one, so it is unset here -- a control
# that depends on nobody else having set the variable is not a control.
#
#   $1  path to the source tree root
#   $2  path to the built xpost binary
set -u
src=${1:?usage: check-no-machinery-returned.sh <srcroot> <xpost>}
xpost=${2:?usage: check-no-machinery-returned.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
walk="$src/tests/no_machinery_returned_test.ps"
guard_require_file "$walk" "the walk that calls every operator"

unset XPOST_CENSUS
guard_workdir

XPOST_DATA_DIR="$src/data" XPOST_NO_VM_IMAGE=1 \
    "$xpost" -q -d null -o /dev/null "$walk" </dev/null > "$work/out" 2>&1

# Some of the operators called print as they run -- that is what they are for
# -- so the report is found rather than read off the end.
line=$(grep -a '^CHECKED ' "$work/out" | tail -1)
if [ -z "$line" ]; then
    echo "FAILURES: the walk did not run to its end, so its silence means"
    echo "      nothing. Its last lines were:"
    tail -6 "$work/out" | sed 's/^/      /'
    exit 1
fi

checked=$(printf '%s\n' "$line" | awk '{print $2}')
returned=$(printf '%s\n' "$line" | awk '{print $4}')
failed=$(printf '%s\n' "$line" | awk '{print $6}')
case ${checked:-x}${returned:-x}${failed:-x} in
    *[!0-9]*|'') echo "FAILURES: the walk reported no counts"; exit 1 ;;
esac

# A walk that called nothing, or that called things and was handed nothing
# back, would report no failures for the wrong reason. Both are refused
# rather than read as a pass: the same shape of silence as a census that was
# never taken.
if [ "$checked" -lt 300 ]; then
    echo "FAILURES: only $checked operator(s) were called, where systemdict"
    echo "      holds far more. The walk stopped early and its answer is"
    echo "      about the part it reached."
    exit 1
fi
if [ "$returned" -lt 1 ]; then
    echo "FAILURES: no operator answered with anything at all, so nothing was"
    echo "      examined and the absence of a finding means nothing."
    exit 1
fi

if [ "$failed" -gt 0 ]; then
    echo "FAILURES: $failed operator(s) answered with a machinery dictionary --"
    echo "      one holding a dotted name, or the graphics state's currgstate."
    echo "      A program calls the operator and has the object. Which operators:"
    grep -a '^FAIL: ' "$work/out" | sed 's/^/      /' | head -8
    exit 1
fi

printf 'called %s operators, %s answered, none with machinery: SUCCESS\n' \
    "$checked" "$returned"
exit 0
