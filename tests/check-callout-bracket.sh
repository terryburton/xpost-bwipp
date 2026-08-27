#!/bin/sh
# Meson test wrapper: assert that program code is called out to through
# the one bracket, and that nothing steps around it.
#
# .callout runs a procedure the program wrote with the machinery's own
# dictionaries off the stack, and puts back the graphics state depth, the
# dictionary stack and the named graphicsdict slots afterwards however
# the procedure returned. The mechanism is worth nothing if a later site
# parks something in a slot without naming it, or escapes its dictionary
# scope by hand: both compile, both work, and both fail only when a
# program re-enters or raises -- which is to say, in the field.
#
# So this checks the two things that would let that happen.
#
# First, every graphicsdict slot the machinery reaches by a dotted name
# is registered in tests/graphicsdict_slots.golden as one of two kinds,
# and a slot registered bracketed really is named in a key array handed
# to .callout. Adding a slot therefore means deciding, in a file that
# gets read, whether a callback can be mid-way through its use.
#
# Second, the dictionary-scope escape has one spelling. It used to have
# three -- `end exec begin`, `end exec` followed by a re-begin from a
# fetched dictionary, and `currentdict end N 1 roll ... begin` -- and
# none of them unwound if the procedure raised. The one site left is
# named below because it belongs to another part of the tree; the count
# may not grow.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-callout-bracket.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
golden="$src/tests/graphicsdict_slots.golden"
[ -s "$golden" ] || { echo "FAILURES: no usable register at $golden"; exit 1; }

guard_workdir
cr=$(printf '\r')

# comments are not code, and a % inside a string is not a comment
for f in "$src"/data/*.ps; do
    tr -d "$cr" < "$f" | sed 's|(%[^)]*)|(STR)|g; s|%.*||'
done > "$work/lines"

# ---- the slots the sources reach ----------------------------------
# A slot is named right after the dictionary, with no other key between:
# `graphicsdict /currgstate get /x ... put` writes x in the graphics
# state, not in graphicsdict, and does not count. The dictionary is also
# reached through a local name where a procedure has to embed the object
# in a program it builds, so those names count as the dictionary too.
aliases=$(grep -oE '/[A-Za-z][A-Za-z0-9]* +graphicsdict +def' "$work/lines" \
          | sed -E 's|^/||; s| .*||' | sort -u)
pattern='graphicsdict'
for a in $aliases; do pattern="$pattern|$a"; done

grep -oE "(^|[^A-Za-z0-9./])($pattern)[^/]*/\.[A-Za-z0-9]+" "$work/lines" \
    | grep -oE '/\.[A-Za-z0-9]+$' | sort -u > "$work/named"

# ---- the slots the brackets name ----------------------------------
# A key array is a .xpostsys member whose name ends in keys, holding the
# slots one bracket saves; .callout takes one as its operand.
grep -oE '\.xpostsys +/\.[A-Za-z0-9]*keys +\[[^]]*\]' "$work/lines" \
    | grep -oE '/\.[A-Za-z0-9]+' | sort -u > "$work/keyed.all"
# the key array's own name is not one of its members
grep -oE '\.xpostsys +/\.[A-Za-z0-9]*keys' "$work/lines" \
    | grep -oE '/\.[A-Za-z0-9]+$' | sort -u > "$work/keynames"
comm -23 "$work/keyed.all" "$work/keynames" > "$work/keyed"

# A slot named in a key array is reached whether or not it is spelled
# next to the dictionary anywhere: the bracket itself reads and writes
# it, and a slot written from inside a program a procedure compiles has
# nowhere else to be spelled.
sort -u "$work/named" "$work/keyed" > "$work/reached"

# ---- the register -------------------------------------------------
sed 's|#.*||' "$golden" | tr -d "$cr" | awk 'NF == 2 { print $1, $2 }' > "$work/reg"
awk '$1 == "bracketed" { print $2 }' "$work/reg" | sort -u > "$work/reg.bracketed"
awk '$1 == "lifetime"  { print $2 }' "$work/reg" | sort -u > "$work/reg.lifetime"
sort -u "$work/reg.bracketed" "$work/reg.lifetime" > "$work/reg.all"

if [ ! -s "$work/reached" ] || [ ! -s "$work/keyed" ]; then
    echo "FAILURES: found no graphicsdict slots or no bracket key arrays to check"
    exit 1
fi

fail=0

guard_held=0
guard_hold "$work/reached" "$work/reg.all" \
    "reached through graphicsdict but not registered: add it to
      tests/graphicsdict_slots.golden as bracketed or lifetime:" \
    "registered but no longer reached through graphicsdict: drop it
      from tests/graphicsdict_slots.golden:"
guard_hold "$work/reg.bracketed" "$work/keyed" \
    "registered bracketed but named in no bracket key array: name it in
      the key array of the bracket that parks it, or register it
      lifetime:" \
    "named in a bracket key array but not registered bracketed:
      register it in tests/graphicsdict_slots.golden:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- one spelling for the dictionary-scope escape -----------------
# path.ps hands each path element to one of pathforall's four
# procedures, dropping the enumerator's dictionary scope around the
# call and taking it back after; the call itself goes through the
# boundary, so every spelling of it is counted here and the one site is
# the only one left.
esc='end +(//\.pexec +|//\.xpostsys +/\.pexec +get +)?exec'
escapes=$(grep -cE "$esc" "$work/lines")
if [ "$escapes" -gt 1 ]; then
    echo "FAIL: the dictionary-scope escape is spelled by hand at $escapes sites, not 1:"
    grep -nE "$esc" "$src"/data/*.ps | sed 's|^|      |'
    echo "      run the procedure through .callout instead"
    fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "SUCCESS ($(wc -l < "$work/reg.bracketed" | tr -d ' ') bracketed and $(wc -l < "$work/reg.lifetime" | tr -d ' ') lifetime slots)"
exit 0
