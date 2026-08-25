#!/bin/sh
# Guard the helpers that name an operator in a refusal.
#
# PLRM 3.11.1 has $error /command hold what was executing, and a program
# reads it to learn which operator it called went wrong. A helper reached
# by several operators cannot name one of them without reporting that name
# to all the others -- which is how undefineresource came to report
# findresource, colorimage to report image, and customcolorimage to report
# setcustomcolor. Those three take the caller's name now, and
# tests/operator_identity_test.ps holds them to it by provoking each
# refusal from each operator that reaches it.
#
# This is the other half: the population that still names an operator
# outright. Most of those are correct, because every route to the helper
# really is that one operator, and a list of them would rot quietly if
# nothing read it. So the list is derived from the sources on every run and
# held to tests/operator-names, which says which are deliberate and why. A
# helper that gains a refusal, or changes the name it uses, is looked at
# rather than landing unread.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-operator-names.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
data=$src/data
register=$src/tests/operator-names
fail=0

if [ ! -s "$register" ] || [ ! -r "$register" ]; then
    echo "FAILURES: the register $register is empty or unreadable" >&2
    exit 1
fi

guard_workdir

# What the sources say. A definition is a helper at the top level of its
# file; a refusal names an operator as `/name cvx /error signalerror`. A %
# inside a (...) string is not a comment, and a brace inside one is text:
# both would drift the depth that decides what is top level.
awk '
function code(line,   out, i, c, par) {
    out = ""; par = 0
    for (i = 1; i <= length(line); i++) {
        c = substr(line, i, 1)
        if (c == "(") par++
        else if (c == ")" && par > 0) par--
        else if (c == "%" && par == 0) break
        out = out ((par > 0 && (c == "{" || c == "}")) ? " " : c)
    }
    return out
}
FNR == 1 { depth = 0; cur = "" }
{
    t = code($0); here = depth
    depth += gsub(/\{/, "{", t) - gsub(/\}/, "}", t)
    if (here == 0) {
        cur = ""
        if (match($0, /^(\.xpostsys[ \t]+|\.privatedict[ \t]+)?\/[A-Za-z.][A-Za-z0-9_.]*[ \t]+\{/)) {
            cur = substr($0, RSTART, RLENGTH)
            sub(/^\.xpostsys[ \t]+/, "", cur); sub(/^\.privatedict[ \t]+/, "", cur)
            sub(/^\//, "", cur); sub(/[ \t]+\{$/, "", cur)
        }
    }
    if (cur == "") next
    rest = t
    while (match(rest, /\/[A-Za-z][A-Za-z0-9_]*[ \t]+cvx[ \t]+\/[a-z]+[ \t]+signalerror/)) {
        piece = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        sub(/^\//, "", piece); sub(/[ \t]+cvx.*$/, "", piece)
        print cur "\t" piece
    }
}
' "$data"/*.ps | LC_ALL=C sort -u > "$work/found"

nfound=$(grep -c . "$work/found" || true)
if [ "${nfound:-0}" -lt 10 ]; then
    echo "FAILURES: read $nfound helper(s) naming an operator; a tree this size" >&2
    echo "has ten or more, so the scan is not reading what it says it reads" >&2
    exit 1
fi

sed 's/#.*$//' "$register" | awk 'NF == 2 { print $1 "\t" $2 }' \
    | LC_ALL=C sort -u > "$work/declared"

if ! diff -u "$work/declared" "$work/found" > "$work/diff" 2>&1; then
    echo "FAILURES: the helpers that name an operator and the register disagree." >&2
    echo "A helper reached by more than one operator must take the caller's name;" >&2
    echo "one reached by only that operator may name it, and says so in the" >&2
    echo "register. Lines marked - are declared and not found, + found and not" >&2
    echo "declared:" >&2
    sed -n '3,$p' "$work/diff" | sed 's/^/      /' >&2
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS ($nfound helper(s) name an operator, each one declared)"
