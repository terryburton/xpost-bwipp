#!/bin/sh
#
# Every painting operator is exercised on every device, or says why not.
#
# tests/device_features_test.ps runs one workload per device, and what it
# runs is a list of painting operators written out by hand. A device
# roster does not work that way -- tests/device-fleet.sh is held to the
# interpreter's maker table by tests/check-device-roster.sh, so a device
# added to the tree and left out of the roster fails -- and an operator
# list that nobody holds to anything is a list that stops being true
# without saying so. The bug shape is the one cross-product testing is
# for: an operator arrives, is wired to the device in front of its
# author, and is never asked of the rest of the family.
#
# ---- what this holds
#
# The operators are read off the files that define them rather than
# named here:
#
#   data/paint.ps   the path painters, the images and erasepage
#   data/font.ps    the show family
#   data/init.ps    the rectangle painters
#   data/shade.ps   shfill
#
# and each has to appear in tests/device_features_test.ps as a case of
# its own, or in tests/painting-operators.exempt with the reason it is
# not a painting operator. Held both ways: an operator neither exercised
# nor excused fails, and an excuse for an operator that is gone fails,
# so the exemptions cannot outlive what they were written about.
#
#   $1  path to the source tree root

set -u

src=${1:?usage: check-painting-operators.sh <srcroot>}

. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_workdir

features=$src/tests/device_features_test.ps
exempt=$src/tests/painting-operators.exempt
for f in "$features" "$exempt"; do
    [ -f "$f" ] || { echo "FAILURES: $f is missing"; exit 1; }
done

# What the files define. An operator is declared at the left margin as
# its name, its operand types and its body, which is the one shape
# .defop takes in these files.
{
    sed -n 's/^\/\([a-z][a-zA-Z0-9]*\) \[.*/\1/p' "$src/data/paint.ps"
    sed -n 's/^\/\([a-z][a-zA-Z0-9]*show[a-zA-Z0-9]*\) \[.*/\1/p' "$src/data/font.ps"
    sed -n 's/^\/\(show\) \[.*/\1/p' "$src/data/font.ps"
    sed -n 's/^\/\(rectfill\|rectstroke\) \[.*/\1/p' "$src/data/init.ps"
    sed -n 's/^\/\(shfill\) \[.*/\1/p' "$src/data/shade.ps"
    sed -n 's/^\/\(ufill\|ueofill\|ustroke\) \[.*/\1/p' "$src/data/path.ps" 2>/dev/null
} | LC_ALL=C sort -u > "$work/defined"
[ -s "$work/defined" ] || { echo "FAILURES: no painting operator was found"; exit 1; }

# What the cross-product runs. A case names the operator it exercises in
# the body it runs, so the body is what is read: a name mentioned in the
# report text alone would let a case drift off the operator it claims.
LC_ALL=C sort -u "$work/defined" | while read -r op; do
    if grep -qE "(^|[^a-zA-Z0-9])$op([^a-zA-Z0-9]|$)" "$features"; then
        echo "$op"
    fi
done > "$work/exercised"

grep -vE '^[[:space:]]*#|^[[:space:]]*$' "$exempt" \
    | awk '{print $1}' | LC_ALL=C sort -u > "$work/excused"

LC_ALL=C sort -u "$work/exercised" "$work/excused" > "$work/covered"

guard_held=0
guard_hold "$work/defined" "$work/covered" \
  "a painting operator is defined and the cross-product neither exercises nor excuses it. Give it a case in tests/device_features_test.ps, or a line in tests/painting-operators.exempt saying why it does not paint:" \
  "tests/painting-operators.exempt excuses an operator that no file defines. An exemption that outlived what it was written about reads like one that was examined:"
[ "$guard_held" -eq 0 ] || exit 1

n=$(wc -l < "$work/defined" | tr -d ' ')
e=$(wc -l < "$work/excused" | tr -d ' ')
printf 'SUCCESS (%s painting operators defined; %s exercised on every\n' "$n" "$((n - e))"
printf '         device, %s excused with a reason)\n' "$e"
