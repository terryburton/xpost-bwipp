#!/bin/sh
# Guard: a PostScript data file separates what it holds with a single
# blank line, never a stack of them.
#
# doc/CONTRIBUTING.md sets the rule: two blank lines never separate two
# procedures and no blank line runs them together -- one blank line stands
# between them. The stacked-blank half of that is what erodes first, a
# second and third blank line drifting in as a file is edited until the
# spacing says nothing, so it is the half a build-time check holds. The
# run-them-together half needs a procedure boundary the reader can see but
# a line scan cannot, and is left to review.
#
# Usage: check-ps-spacing.sh <source tree root>

set -eu

src=${1:?usage: check-ps-spacing.sh <source tree root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_dir "$src/data" "the PostScript data files"

fail=0
checked=0

for f in "$src"/data/*.ps; do
    [ -f "$f" ] || continue
    checked=$((checked + 1))
    b=$(basename "$f")
    # the line where a second consecutive blank line falls, one per run
    runs=$(awk '
        /^[[:space:]]*$/ { blank = blank + 1; if (blank == 2) print NR; next }
        { blank = 0 }
    ' "$f")
    if [ -n "$runs" ]; then
        for ln in $runs; do
            echo "  $b:$ln: a second consecutive blank line"
        done
        fail=1
    fi
    # an inline comment -- a % that starts a comment with code before it on
    # the line -- is set off by two spaces, never one. The % is found past
    # any string it falls inside, so a per-cent inside ( ) is not mistaken
    # for one, and a %% or %! line is left to its own conventions.
    inl=$(awk '
        {
            depth = 0; pct = 0; i = 1; n = length($0)
            while (i <= n) {
                c = substr($0, i, 1)
                if (c == "\\") { i += 2; continue }
                if (c == "(") depth++
                else if (c == ")") { if (depth > 0) depth-- }
                else if (c == "%" && depth == 0) { pct = i; i = n }
                i++
            }
            if (pct > 1 && substr($0, pct, 2) != "%%") {
                before = substr($0, 1, pct - 1)
                if (before ~ /[^ \t]/ \
                 && substr($0, pct - 1, 1) == " " \
                 && (pct - 2 < 1 || substr($0, pct - 2, 1) !~ /[ \t]/))
                    print NR
            }
        }
    ' "$f")
    if [ -n "$inl" ]; then
        for ln in $inl; do
            echo "  $b:$ln: an inline comment set off by one space, not two"
        done
        fail=1
    fi
done

if [ "$checked" -eq 0 ]; then
    echo "FAILURES: found no PostScript data files to check; the check is unusable"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: some data files stack blank lines (above); one blank line between what a file holds, two spaces before an inline comment"
    exit 1
fi
echo "SUCCESS ($checked data files hold the blank-line and inline-comment spacing)"
exit 0
