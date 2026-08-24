#!/bin/sh
# Guard: every source file opens with a conforming header.
#
# A header names the file, says in a line what it holds, states the
# copyright, and names the licence by SPDX identifier -- the full text
# living once in COPYING. Written down in doc/CONTRIBUTING.md, the rule
# erodes the moment a new file is added without one: nothing in a build
# notices a missing header, and a tree half of whose files carry a
# licence and half of which do not is the state a release cannot ship
# from. So the shape is a build-time check and not a convention.
#
# A C source is held to a product line naming the interpreter, at least
# one copyright line, and the SPDX identifier; and to carrying none of
# the ~24-line BSD boilerplate the identifier replaced, since a file
# still carrying it is one the sweep did not reach. A PostScript data
# file is held to the %!PS magic line first, then a comment naming the
# file, a copyright and the identifier. readstring.ps is a bare test
# fixture and is excepted by name.
#
# Usage: check-file-headers.sh <source tree root>

set -eu

src=${1:?usage: check-file-headers.sh <source tree root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_dir "$src/src/lib" "the library source"
guard_require_dir "$src/data" "the PostScript data files"

fail=0
checked=0

# the first lines of a file, where a header lives and a stray later
# match cannot reach
head_of() { head -n 30 "$1"; }

for f in "$src"/src/lib/*.c "$src"/src/lib/*.h \
         "$src"/src/bin/*.c "$src"/src/bin/*.h; do
    [ -f "$f" ] || continue
    checked=$((checked + 1))
    b=$(basename "$f")
    h=$(head_of "$f")
    printf '%s\n' "$h" | grep -q 'Xpost.*PostScript.*\(interpreter\|parser\|viewer\)' \
        || { echo "  $b: header names no Xpost product"; fail=1; }
    printf '%s\n' "$h" | grep -q 'Copyright (c)' \
        || { echo "  $b: header states no copyright"; fail=1; }
    printf '%s\n' "$h" | grep -q 'SPDX-License-Identifier: BSD-3-Clause' \
        || { echo "  $b: header states no SPDX licence"; fail=1; }
    if printf '%s\n' "$h" | grep -q 'Level-2'; then
        echo "  $b: header still says Level-2"; fail=1
    fi
    if grep -q 'POSSIBILITY OF SUCH DAMAGE' "$f"; then
        echo "  $b: still carries the inline BSD text"; fail=1
    fi
done

for f in "$src"/data/*.ps; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    [ "$b" = readstring.ps ] && continue
    checked=$((checked + 1))
    h=$(head_of "$f")
    [ "$(head -n 1 "$f")" = '%!PS' ] \
        || { echo "  $b: first line is not %!PS"; fail=1; }
    printf '%s\n' "$h" | grep -q "^% ${b}\$" \
        || { echo "  $b: header does not name the file"; fail=1; }
    printf '%s\n' "$h" | grep -q '% Copyright (c)' \
        || { echo "  $b: header states no copyright"; fail=1; }
    printf '%s\n' "$h" | grep -q '% SPDX-License-Identifier: BSD-3-Clause' \
        || { echo "  $b: header states no SPDX licence"; fail=1; }
done

if [ "$checked" -eq 0 ]; then
    echo "FAILURES: found no source files to check; the check is unusable"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "FAILURES: some sources do not carry a conforming header (above)"
    exit 1
fi
echo "SUCCESS ($checked sources carry a conforming header)"
exit 0
