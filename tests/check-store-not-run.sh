#!/bin/sh
# Nothing the machinery runs may come out of the writable dictionaries a
# program can reach: the job store and the render scratch.
#
# The store is the machinery's own writable state, and the sweep passes over
# it for that reason -- it is the one hole in the rule that nothing writable
# is left where the machinery reads or runs it. That hole is safe only while
# the store holds DATA. The moment a procedure is fetched from a store
# dictionary and executed, a key planted there is a procedure the machinery
# will run, and the exemption stops being about state.
#
# MEASURED when this was written: the store holds fourteen dictionaries, all
# writable, containing no procedure at all, and no site in data/ fetches and
# executes anything out of one. That is a property of the sources rather than
# a rule the interpreter enforces, which is what this holds.
#
# The population is derived: the members are the names data/ puts into
# either dictionary, so one added later is covered without this being
# revisited. .gscratch is here for the same reason the store is -- its
# accessor stays in systemdict by decision, so a program reaches it, and
# MEASURED it holds five members and no procedure at all.
set -u
src=${1:?usage: check-store-not-run.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
guard_mirror data "$src"/data/*.ps
data=$mirror

# the members, as data/ names them
grep -ohE '\.(jobstore|gscratch) +/\.?[A-Za-z0-9_=.-]+' "$data"/*.ps \
    | sed 's|.*/||' | sort -u > "$work/members"
nm=$(grep -c . < "$work/members" || true)
if [ "$nm" -lt 5 ]; then
    echo "FAILURES: only $nm members were found in data/, which this"
    echo "      tree has more of; the reading is broken rather than the tree"
    exit 1
fi

: > "$work/bad"

# the containers themselves, not only their members: `.gscratch /k get exec`
# runs out of the dictionary a program can write just as surely as
# `.pagestate /k get exec` runs out of one it holds. A sabotage of the
# first form went straight through a guard that asked only about the
# second.
for f in "$data"/*.ps; do
    tr '\n' ' ' < "$f" \
        | grep -oE "(//)?\.(jobstore|gscratch) +/[A-Za-z0-9_.=-]+ +get +exec" \
        | sed "s|^|$(basename "$f")	|" >> "$work/bad" || true
done
while read -r m; do
    [ -n "$m" ] || continue
    # <member> /key get exec, on one line or split across two
    tr '\n' ' ' < /dev/null >/dev/null
    for f in "$data"/*.ps; do
        tr '\n' ' ' < "$f" \
            | grep -oE "(//)?\\$m +/[A-Za-z0-9_.=-]+ +get +exec" \
            | sed "s|^|$(basename "$f")	|" >> "$work/bad" || true
    done
done < "$work/members"

if [ -s "$work/bad" ]; then
    echo "FAILURES: the machinery runs something fetched out of a dictionary"
    echo "      a program can write,"
    echo "      so a key planted there is a procedure it will run:"
    sed 's/^/      /' "$work/bad"
    echo "      Keep what is run in the sealed namespace and let the store"
    echo "      hold only what is read."
    exit 1
fi

printf 'nothing is run out of the writable dictionaries: SUCCESS (%s members)\n' "$nm"
exit 0
