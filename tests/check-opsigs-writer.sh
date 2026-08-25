#!/bin/sh
# Meson test wrapper: assert that an operator's operand statement is
# recorded in one place, at the definition it describes.
#
# The statements live in a dictionary the dispatcher reads. Four operators
# once could not reach it from where they were written -- one had to land
# in a dictionary other than the current one, two were defined where
# allocation was local and the dictionary is global, and one was written
# as two procedures with the second discarded. Their statements were
# recorded by hand beside the mechanism instead, which put the statement a
# long way from the body it describes and left nothing to notice when the
# two disagreed.
#
# .defopin takes the dictionary and copies a local statement, so there is
# no longer a definition it cannot express. This holds that: the statement
# dictionary has exactly one writer, and a definition that cannot use it
# has to be made expressible rather than worked around.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-opsigs-writer.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
cr=$(printf '\r')

# every mention of the statement dictionary, comments stripped
for f in "$src"/data/*.ps; do
    tr -d "$cr" < "$f" | sed 's|(%[^)]*)|(STR)|g; s|%.*||' \
        | grep -n 'opsigs' | sed "s|^|$(basename "$f"):|"
done > "$work/all"

if [ ! -s "$work/all" ]; then
    echo "FAILURES: found no mention of the statement dictionary; the check is unusable"
    exit 1
fi

# a writer is a put into it; the mechanism's own two lines are the allowed ones
grep 'put' "$work/all" | grep -v 'opsigs 200 dict put' > "$work/writers"

# the one legitimate writer records the statement inside .defopin
site='opsigs([[:blank:]]+get)?[[:blank:]]+d\.name[[:blank:]]+d\.sig[[:blank:]]+put'
allowed=$(grep -cE "$site" "$work/writers" || true)
others=$(grep -vcE "$site" "$work/writers" || true)

if [ "$allowed" -ne 1 ]; then
    echo "FAILURES: expected exactly one recording site inside the mechanism, found $allowed"
    sed 's/^/      /' "$work/writers"
    exit 1
fi

if [ "$others" -ne 0 ]; then
    echo "FAIL: an operand statement is recorded outside the mechanism:"
    grep -vE "$site" "$work/writers" | sed 's/^/      /'
    echo "      state it at the definition through .defop or .defopin instead"
    exit 1
fi

echo "SUCCESS (operand statements have one writer)"
exit 0
