#!/bin/sh
# Guard: every C source says what it is for, in its own words.
#
# A header names the file, says in a line what it holds, states the
# copyright and names the licence. check-file-headers.sh holds the last
# two on every source and the first two on PostScript ones -- a data file
# is asked to name itself with `% <name>` on a line -- but it has never
# asked a C file either question. That is not a small gap: it is the
# whole reason nought of a hundred and twenty-five C files opened with a
# sentence saying what they were, while the data files, whose branch did
# ask, are tidy. A rule nothing checks is a rule that erodes to nothing,
# and the erosion is invisible because the build goes on working.
#
# What is asked for is what a third of the tree already writes: the
# doxygen block the headers carry, `@file <name>` and `@brief <line>`.
# Nothing new is invented, and a file that already conforms is untouched.
# `@file` is required to name the file it is in, so a block copied from a
# neighbour is a failure rather than a lie the reader has to catch.
#
# A MIGRATION, NOT A CLIFF -- and it is now finished. Ninety-three files
# did not carry a block when this was written; the list of them is empty.
# Requiring them all at once would have turned the gate red for everyone
# until the last was written, which is how a rule gets reverted rather
# than kept, so they were named in tests/file-purpose and that list was
# emptied instead. The register stays, because it is what says the rule
# applies to everything with nothing standing outside it. It is held in
# both directions:
#
#   a file not named there must carry a block -- so a new file joins the
#   rule the day it is added, and a converted file cannot quietly lose
#   its block again;
#
#   a file named there must NOT carry one -- so converting a file and
#   forgetting the register is a failure, and the list cannot rot into a
#   permanent excuse for files that have long since been written;
#
#   and every name there must exist, so a deleted file does not leave a
#   line behind that makes the debt look larger than it is.
#
# The list of files is read off the tree on every run rather than kept
# here, because a register compared only against another register is
# blind to exactly what a new file is: absent from both.
#
# Named by the path under src/ and not by the bare file name, because two
# files here share one: src/lib/xpost_main.c and src/bin/xpost_main.c are
# different files of two hundred and a thousand lines. Keyed by name, one
# of them would answer for the other and never be asked at all.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-file-purpose.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_dir "$src/src/lib" "the library source"
register=$src/tests/file-purpose
fail=0

if [ ! -r "$register" ]; then
    echo "FAILURES: $register is unreadable; the check would read nothing" >&2
    exit 1
fi

guard_workdir

# What the tree has, and what the register says is not written yet.
for f in "$src"/src/lib/*.c "$src"/src/lib/*.h \
         "$src"/src/bin/*.c "$src"/src/bin/*.h; do
    [ -f "$f" ] || continue
    printf '%s\n' "${f#"$src"/src/}"
done | LC_ALL=C sort -u > "$work/present"

sed 's/#.*$//' "$register" | awk 'NF == 1 { print $1 }' \
    | LC_ALL=C sort -u > "$work/pending"

n=$(grep -c . "$work/present" || true)
if [ "${n:-0}" -lt 50 ]; then
    echo "FAILURES: read $n C source(s) under $src/src; a tree this size has" >&2
    echo "many more, so this is not reading what it says it reads" >&2
    exit 1
fi

# Does one file carry a block naming itself?
has_block() {                   # <path> <basename>  .  0 where it does
    awk -v want="$2" '
        $0 ~ /@file[ \t]+/ {
            name = $0
            sub(/^.*@file[ \t]+/, "", name)
            sub(/[ \t\r]+$/, "", name)
            if (name == want) seenfile = 1
        }
        $0 ~ /@brief[ \t]+[^ \t]/ { seenbrief = 1 }
        END { exit !(seenfile && seenbrief) }
    ' "$1"
}

written=0
while read -r b; do
    [ -n "$b" ] || continue
    p=$src/src/$b
    if has_block "$p" "$(basename "$b")"; then
        written=$((written + 1))
        if grep -qx "$b" "$work/pending"; then
            echo "  $b: says what it is for, but is still named in" >&2
            echo "      tests/file-purpose as one that does not; drop the line" >&2
            fail=1
        fi
    else
        if ! grep -qx "$b" "$work/pending"; then
            echo "  $b: does not name itself and say what it holds." >&2
            echo "      Open it with a doxygen block:  @file $(basename "$b")" >&2
            echo "      and an @brief line saying what the file is for" >&2
            fail=1
        fi
    fi
done < "$work/present"

# a name in the register that is not in the tree
while read -r b; do
    [ -n "$b" ] || continue
    if ! grep -qx "$b" "$work/present"; then
        echo "  $b: named in tests/file-purpose but is not in the tree" >&2
        fail=1
    fi
done < "$work/pending"

[ "$fail" -eq 0 ] || {
    echo "FAILURES: the sources that say what they are for and the register" >&2
    echo "of the ones that do not have come apart (above)" >&2
    exit 1
}
pending=$(grep -c . "$work/pending" || true)
echo "SUCCESS ($written of $n C sources say what they are for," \
     "$pending yet to be written)"
