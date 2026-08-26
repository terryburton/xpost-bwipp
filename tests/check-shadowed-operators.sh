#!/bin/sh
# Guard the names implemented twice, once in C and once in PostScript.
#
# Only one of the pair is in force. Which one is settled while the
# language is read, by the order the dictionaries are filled and
# searched, and neither the interpreter nor the program is told which
# way it went. The failure is quiet: an operator that answers, with the
# wrong implementation behind it.
#
# So each such name is declared in tests/shadowed_operators.golden, with
# the implementation that is meant to win. This checks that the set of
# doubly-implemented names is exactly the set declared -- a new one
# appearing is a choice nobody made on purpose, and has to be made here
# before it can be relied on anywhere else.
#
# Which implementation actually won is a run-time question, checked by
# startup_surface_test.ps, which carries the same choice as two arrays.
# Those arrays are held to the register here, so the two cannot drift.
#
# Usage: check-shadowed-operators.sh <srcdir> <datadir> <golden>

set -eu

srcdir=${1:?usage: check-shadowed-operators.sh <srcdir> <datadir> <golden>}
datadir=${2:?usage: check-shadowed-operators.sh <srcdir> <datadir> <golden>}
golden=${3:?usage: check-shadowed-operators.sh <srcdir> <datadir> <golden>}

. "$(dirname "$0")/guard-paths.sh"
guard_require_dir "$srcdir" "the library source directory"
guard_require_dir "$datadir" "the PostScript data directory"
guard_require_file "$golden" "the register of doubly-implemented names"

guard_workdir

# every scan below is line-anchored, so read the inputs with their line
# endings taken out
guard_mirror ps "$datadir"/*.ps
datadir=$mirror
guard_mirror reg "$golden" "$(dirname "$golden")/startup_surface_test.ps"
golden="$mirror/$(basename "$golden")"

# every name a C operator is installed under
grep -ho 'xpost_operator_cons(ctx, "[^"]*"' "$srcdir"/*.c 2>/dev/null \
    | sed 's/.*"\(.*\)"/\1/' | LC_ALL=C sort -u > "$work/c"

# every name a PostScript file defines. A definition opens a procedure or
# an array on the line naming it; a `where` guard testing for a name does
# not, and is not a second implementation.
#
# Leading whitespace is part of the line and not part of the rule. The
# scan was once anchored to the first column, which meant a definition
# indented inside a conditional -- callout.ps defines breakhere that way,
# in the fallback block that runs when the file is loaded standalone --
# was a second implementation the check could not see.
grep -hoE '^[[:space:]]*/[A-Za-z=][A-Za-z0-9]* *[[{]' "$datadir"/*.ps 2>/dev/null \
    | sed 's|^[[:space:]]*/||; s| *[[{]$||' | LC_ALL=C sort -u > "$work/ps"

LC_ALL=C comm -12 "$work/c" "$work/ps" > "$work/both"

grep -v '^[[:space:]]*#' "$golden" | grep -v '^[[:space:]]*$' \
    | awk '{print $1}' | LC_ALL=C sort -u > "$work/declared"

if [ ! -s "$work/c" ] || [ ! -s "$work/ps" ]; then
    echo "FAILURES: found no operators to compare in $srcdir or $datadir"
    exit 1
fi

fail=0

guard_held=0
guard_hold "$work/both" "$work/declared" \
    "implemented in both C and PostScript, but not declared: say which
      one wins in $(basename "$golden"):" \
    "declared as implemented twice, but no longer are: remove them from
      $(basename "$golden"):"
[ "$guard_held" -eq 0 ] || fail=1

# every declaration names one of the two implementations
grep -v '^[[:space:]]*#' "$golden" | grep -v '^[[:space:]]*$' \
    | awk '$2 != "postscript" && $2 != "c" { print }' > "$work/bad"
if [ -s "$work/bad" ]; then
    echo "FAIL: a declaration names neither implementation:"
    sed 's/^/      /' "$work/bad"
    fail=1
fi

# The run-time half carries the same choice as two arrays, because a
# PostScript test cannot read this file. Two registers of one fact drift:
# hold them equal here, so declaring a name and forgetting the other half
# is the failure it should be.
surface="$(dirname "$golden")/startup_surface_test.ps"
guard_require_file "$surface" "the startup surface test"
for side in postscript c; do
    awk -v want="$side" '$1 !~ /^#/ && $2 == want { print $1 }' "$golden" \
        | LC_ALL=C sort -u > "$work/golden.$side"
    case $side in
        postscript) arr=WON.PS ;;
        c)          arr=WON.C ;;
    esac
    awk -v arr="$arr" '
        BEGIN { open = "^/" arr "[[:space:]]*\\[" }
        !inb && $0 ~ open { inb = 1; sub(open, "") }
        inb {
            sub(/%.*/, "")
            buf = buf " " $0
            if (index($0, "]") > 0) exit
        }
        END {
            n = split(buf, w, /[^A-Za-z0-9=\/]+/)
            for (i = 1; i <= n; i++)
                if (substr(w[i], 1, 1) == "/") print substr(w[i], 2)
        }' "$surface" | LC_ALL=C sort -u > "$work/surface.$side"
    if ! cmp -s "$work/golden.$side" "$work/surface.$side"; then
        echo "FAIL: $arr in startup_surface_test.ps does not match the '$side'"
        echo "      declarations in $(basename "$golden"):"
        LC_ALL=C comm -3 "$work/golden.$side" "$work/surface.$side" \
            | sed 's/^\t/      only in the test: /; s/^\([^ ]\)/      only in the register: \1/'
        fail=1
    fi
done

[ "$fail" = 0 ] || exit 1
echo "SUCCESS ($(wc -l < "$work/both" | tr -d ' ') names implemented twice, each declared)"
