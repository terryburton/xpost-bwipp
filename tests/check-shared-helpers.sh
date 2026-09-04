#!/bin/sh
#
# A shared helper is only shared if everything that should use it does.
#
# Two families of script here each have one file holding what the family
# has in common: tests/verdict.sh for the wrappers that run a test and
# judge what it printed, and tests/guard-paths.sh for the guards that
# read the tree. Both were written after most of their family already
# existed, and both were adopted by sweeping the family once. A sweep is
# a moment, not a rule: it says nothing about the next file, and the next
# file is written by copying a neighbour, which may be the one the sweep
# missed.
#
# The file most likely to be missed is the one that is named like the
# family without being a member of it: it answers every search for the
# family and belongs to none of its rules. tests/run-profile.sh is that
# file here, which is why it is named in the register rather than left
# to be rediscovered.
#
# So the requirement is written down instead of remembered. The
# population is taken from the DIRECTORY, not from a list kept beside it:
# a guard that holds one list against another is blind to whatever is
# absent from both, which is precisely the state a newly added file is
# in. Every tests/run-*.sh must source verdict.sh and every
# tests/check-*.sh must source guard-paths.sh, unless it is named in the
# register below with a reason, and a register entry naming a file that
# is not there is itself a failure -- otherwise an exemption outlives
# what it exempted and quietly covers a later file of the same name.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-shared-helpers.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

reg="$src/tests/shared_helpers.exempt"
guard_require_file "$reg" "the register of exempt scripts"

guard_workdir

# The register is read with carriage returns taken out, for the reason
# given where guard_mirror is defined.
tr -d '\r' < "$reg" | sed 's/#.*//' | awk 'NF { print $1, $2 }' \
    > "$work/exempt"

fails=0
checked=0
exempted=0

_exempt_reason() {  # $1 basename . prints the reason, empty if not exempt
    awk -v n="$1" '$1 == n { $1 = ""; sub(/^ /, ""); print; exit }' "$work/exempt"
}

_hold() {           # $1 glob . $2 helper file . $3 what the family is
    for f in $1; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        checked=$((checked + 1))
        if grep -q "$2" "$f"; then
            # An exemption for a file that went on to adopt the helper is
            # stale in the other direction, and would go on excusing the
            # next file to carry that name.
            if [ -n "$(_exempt_reason "$b")" ]; then
                echo "FAILURES: $b sources $2 and is also excused from it;"
                echo "      take it out of tests/shared_helpers.exempt"
                fails=$((fails + 1))
            fi
            continue
        fi
        reason=$(_exempt_reason "$b")
        if [ -n "$reason" ]; then
            exempted=$((exempted + 1))
            continue
        fi
        echo "FAILURES: $b is $3 and does not source $2, and no reason"
        echo "      is written for it in tests/shared_helpers.exempt"
        fails=$((fails + 1))
    done
}

_hold "$src/tests/run-*.sh"   verdict.sh     "a test wrapper"
_hold "$src/tests/check-*.sh" guard-paths.sh "a guard"

# Sourcing the shared file is not the whole of it. A script that sources
# it and then does the thing itself anyway has put a second implementation
# back, and the second one is the one nobody maintains. So each act that
# now has exactly one implementation is named here by what a script would
# have to write to do it another way.
#
# A script may still make a directory of its own beyond the one the helper
# gives it -- a second tree for a library run, a per-case directory inside
# the first -- and this does not forbid that. What it forbids is taking
# the first one from somewhere else, because the first one is the one
# whose removal the helper arranges.
tr -d '\r' < "$src/tests/one_implementation.exempt" | sed 's/#.*//' \
    | awk 'NF { print $1, $2 }' > "$work/only"

_only() {           # $1 glob . $2 the act . $3 the helper . $4 what the act is
    for f in $1; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        grep -q "$2" "$f" || continue
        grep -q "$3" "$f" && continue
        if [ -n "$(awk -v n="$b" '$1 == n { print "yes"; exit }' "$work/only")" ]; then
            exempted=$((exempted + 1))
            continue
        fi
        echo "FAILURES: $b $4 rather than calling $3, so there are two"
        echo "      implementations of it and the shared one is not the"
        echo "      one this script uses"
        fails=$((fails + 1))
    done
}

_only "$src/tests/check-*.sh" 'mktemp -d'  guard_workdir \
      "makes its scratch directory"
_only "$src/tests/run-*.sh"   'mktemp -d'  verdict_workdir \
      "makes its scratch directory"
_only "$src/tests/check-*.sh" 'awk -v K='  guard_hold_count \
      "reads a register's declared count"
_only "$src/tests/check-*.sh" '! -x "\$xpost"' guard_require_interpreter \
      "refuses a path that is not an interpreter"
_only "$src/tests/check-*.sh" 'srcdata='  guard_srcdata \
      "names the directory the interpreter reads its boot files from"

# A register entry naming nothing in the tree.
while read -r name rest; do
    [ -n "$name" ] || continue
    if [ ! -e "$src/tests/$name" ]; then
        echo "FAILURES: tests/shared_helpers.exempt excuses $name, which"
        echo "      is not in tests/ -- the exemption has outlived its file"
        fails=$((fails + 1))
    fi
done < "$work/exempt"

# The same, for the register that excuses a script from the one-place rule.
while read -r name rest; do
    [ -n "$name" ] || continue
    if [ ! -e "$src/tests/$name" ]; then
        echo "FAILURES: tests/one_implementation.exempt excuses $name, which"
        echo "      is not in tests/ -- the exemption has outlived its file"
        fails=$((fails + 1))
    fi
done < "$work/only"

# A register entry with no reason excuses nothing: the reason is the
# whole value of writing the exemption down rather than leaving the file
# out of a sweep.
while read -r name rest; do
    [ -n "$name" ] || continue
    if [ -z "$rest" ]; then
        echo "FAILURES: tests/shared_helpers.exempt names $name with no"
        echo "      reason; an exemption without one is a file nobody looked at"
        fails=$((fails + 1))
    fi
done < "$work/exempt"

# ---- the pair, written by hand
#
# Sourcing the helper file is not the same as using what is in it. The
# thing these guards have most in common is holding a derived set
# against a register in both directions, and guard_hold is that; a guard
# that instead writes comm -23 and comm -13 over the same two operands
# has written the helper again, and the copy is where the drift starts.
# One direction later gets edited and the other does not, and a guard
# checking one direction is blind to whatever is absent from the list it
# started from -- the state a newly added member is in, and the failure
# the whole family exists to prevent.
#
# Only the PAIR is refused. A single comm that subtracts one set from
# another to build a third is an ordinary set operation, not a check
# written twice, and several guards need it.
pairreg="$src/tests/direct_comm.exempt"
guard_require_file "$pairreg" "the register of guards excused from guard_hold"
tr -d '\r' < "$pairreg" | sed 's/#.*//' | awk 'NF { print }' > "$work/pairex"

for f in "$src"/tests/check-*.sh; do
    [ -e "$f" ] || continue
    b=$(basename "$f")
    hand=$(sed -nE 's/.*comm -(23|13)  *("[^"]*")  *("[^"]*").*/\1 \2 \3/p' "$f" \
        | awk '{ k = $2 " " $3; d[k] = d[k] $1 " " }
               END { for (k in d) if (d[k] ~ /23/ && d[k] ~ /13/) print k }')
    excuse=$(awk -v n="$b" '$1 == n { $1 = ""; sub(/^ /, ""); print; exit }' "$work/pairex")

    if [ -z "$hand" ]; then
        # and an excuse for a guard that has since adopted the helper
        # would go on excusing whatever is written there next
        if [ -n "$excuse" ]; then
            echo "FAILURES: $b writes no comm pair and is still excused from"
            echo "      guard_hold; take it out of tests/direct_comm.exempt"
            fails=$((fails + 1))
        fi
        continue
    fi
    if [ -n "$excuse" ]; then
        exempted=$((exempted + 1))
        continue
    fi
    echo "FAILURES: $b holds two sets to each other by hand rather than"
    echo "      through guard_hold, over:"
    printf '%s\n' "$hand" | sed 's/^/          /'
    echo "      Call guard_hold, or write the reason it cannot in"
    echo "      tests/direct_comm.exempt."
    fails=$((fails + 1))
done

# and an excuse for a guard that is not there at all
while read -r name rest; do
    [ -n "$name" ] || continue
    if [ ! -e "$src/tests/$name" ]; then
        echo "FAILURES: tests/direct_comm.exempt excuses $name, which is not"
        echo "      in tests/ -- the exemption has outlived its file"
        fails=$((fails + 1))
    fi
    if [ -z "$rest" ]; then
        echo "FAILURES: tests/direct_comm.exempt names $name with no reason"
        fails=$((fails + 1))
    fi
done < "$work/pairex"

if [ "$checked" -eq 0 ]; then
    echo "FAILURES: no scripts were examined, so this proves nothing"
    exit 1
fi

if [ "$fails" -ne 0 ]; then
    echo "FAILURES: $fails"
    exit 1
fi
echo "SUCCESS ($checked scripts hold to their family's helper, $exempted excused with a reason)"
