#!/bin/sh
# Meson test wrapper: assert that every reference the language makes into
# a private namespace names something that resolves.
#
# The interpreter's machinery does not reach its helpers by ordinary name
# lookup -- the namespaces are off the dictionary stack. It reaches them
# two ways, and this checks both, because checking only the first would
# leave the majority form unexamined while reporting a pass.
#
# A helper is either fetched from the dictionary and run, which is a bare
# `get`; or, inside the namespace's own `begin` block, frozen in at scan
# time as //name. Neither failure is loud. A `get` for a name that is not
# there raises undefined only when that branch executes, and a //name that
# resolves to nothing does not even stop the module loading -- verified by
# planting one, whose file went on to load and print. The branch that
# needed it may be a device this build never selects or an error path no
# test reaches, so either typo can ship.
#
# The register this checks against is not a golden file: it is the running
# interpreter. The three namespaces are dumped from a live startup and the
# fetches are read out of the sources, so a name counts as existing when
# the interpreter really holds it -- however it came to be there, whether
# defined in place, relocated at lockdown, or installed from C. A guard
# built the other way, matching definitions with a second set of patterns,
# would report a dangling reference every time a definition form was
# spelled in a way the patterns did not anticipate.
#
# A name probed with `known` against its own namespace is optional by
# intent and is held to nothing. That is decided by the token following
# the name, not by the line it sits on: `known` on a line usually applies
# to some other dictionary, and dropping such lines wholesale would take
# six real bare fetches out of the check with it.
#
# check-dict-homes.sh asserts that a registered member has not moved home;
# this asserts that a reference still lands. The two are complements.
#
#   $1  path to the xpost binary
#   $2  path to the source tree root
set -u
xpost=${1:?usage: check-private-refs.sh <xpost> <srcroot>}
src=${2:?usage: check-private-refs.sh <xpost> <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
cr=$(printf '\r')   # tolerate CRLF line endings (Windows checkouts)

# ---- the register: what the namespaces hold in a real interpreter ----
# .xpostsys cannot be named after lockdown. It is recovered the way
# tamper_dispatch_test.ps recovers it: privatedict anchors bound
# procedures, and those carry the dictionary baked in by their //
# references.
cat > "$work/dump.ps" <<'PSEOF'
/found null def
/probe { 2 dict begin /d exch def /o exch cvlit def
  d 0 gt found null eq and {
    o type /dicttype eq { { o /.strcat known { /found o store } if } stopped pop }
    { o type /arraytype eq { o rcheck {
        0 1 o length 1 sub { o exch get d 1 sub probe } for } if } if } ifelse
  } if end } def
.privatedict { exch pop dup type /arraytype eq { 6 probe }{ pop } ifelse } forall
/dump { % dict (label)  .  -
  /lbl exch def
  { pop dup type /nametype eq
    { lbl print 60 string cvs print (\n) print }{ pop } ifelse } forall
} bind def
found null ne { found (xpostsys ) dump } if
.privatedict (privatedict ) dump
1183615869 internaldict (internaldict ) dump
systemdict (systemdict ) dump
PSEOF

XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/dump.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | grep -E '^(xpostsys|privatedict|internaldict|systemdict) .' \
    | sort -u > "$work/all"
grep -vE '^systemdict ' "$work/all" > "$work/have"

for ns in xpostsys privatedict internaldict; do
    if ! grep -q "^$ns " "$work/have"; then
        echo "FAILURES: the interpreter reported no $ns members; the register is unusable"
        exit 1
    fi
done

# ---- what the sources say about the namespaces ----
# Files that are never loaded cannot contribute a live reference.
# A `%` inside a string is not a comment, so those are neutralised before
# comments are stripped -- otherwise a line mentioning (%stdout) loses its
# tail and a real fetch on it goes unchecked.
for f in "$src"/data/*.ps; do
    case $(basename "$f") in
        test.ps) continue ;;
    esac
    tr -d "$cr" < "$f" | sed 's|(%[^)]*)|(STR)|g; s|%.*||'
done > "$work/lines"

# Each occurrence is "<namespace> /<name> <next-token>". The next token
# decides which side it counts for: `get` fetches the member, anything
# else (a put, or nothing because the put is on the following line)
# establishes it. Scratch slots the machinery writes at run time are
# therefore members too, though a startup dump cannot see them.
{
    grep -oE '(//)?\.xpostsys +/[A-Za-z0-9._=-]+( +[A-Za-z]+)?' "$work/lines" \
        | sed 's|^//||; s|^\.xpostsys *|xpostsys |'
    grep -oE '(//)?\.internaldict +/[A-Za-z0-9._=-]+( +[A-Za-z]+)?' "$work/lines" \
        | sed 's|^//||; s|^\.internaldict *|internaldict |'
    grep -oE '1183615869 +internaldict +/[A-Za-z0-9._=-]+( +[A-Za-z]+)?' "$work/lines" \
        | sed 's|^1183615869 *internaldict *|internaldict |'
    grep -oE '\.privatedict +/[A-Za-z0-9._=-]+( +[A-Za-z]+)?' "$work/lines" \
        | sed 's|^\.privatedict *|privatedict |'
} | sed 's| /| |' > "$work/occ"

awk '$3 == "get"   { print $1, $2 }' "$work/occ" | sort -u > "$work/want"
awk '$3 == "known" { print $1, $2 }' "$work/occ" | sort -u > "$work/opt"
awk '$3 != "get" && $3 != "known" { print $1, $2 }' "$work/occ" | sort -u > "$work/put"

if [ ! -s "$work/want" ]; then
    echo "FAILURES: found no private-namespace fetches to check in $src/data"
    exit 1
fi

# a member is present if the interpreter holds it, or the sources put it there.
# A name probed with `known` against its own namespace is optional by intent,
# so it neither needs to exist nor counts as established.
sort -u "$work/have" "$work/put" > "$work/present"
mv "$work/present" "$work/have"

# ---- the other half: names frozen in by immediate evaluation ----
# Inside a namespace's own `begin` block a helper is reached as //name
# rather than by fetching it from the dictionary, so no amount of looking
# for `get` finds these -- and they are the majority form in the colour,
# shading, pattern and CID code. A //name that resolves to nothing does
# NOT stop the load: the module carries on and the failure waits for the
# branch that needed it, which is the same latency the fetch check exists
# for.
#
# The name is resolved at scan time against whatever the dictionary stack
# then holds, so the register here is wider: the private namespaces, the
# operators, and the names the file itself defines at top level. A `//`
# inside a URL is not a name -- the character before it settles that.
sed 's|^systemdict ||' "$work/all" | awk '{print $NF}' | sort -u > "$work/known"
: > "$work/baked"
for f in "$src"/data/*.ps; do
    body=$(tr -d "$cr" < "$f" | sed 's|(%[^)]*)|(STR)|g; s|%.*||')
    # names this file defines at top level are on its own dictionary stack
    printf '%s\n' "$body" | grep -oE '^/[A-Za-z0-9._=-]+' | sed 's|^/||' >> "$work/known"
    printf '%s\n' "$body" | grep -oE '(^|[^:])//[A-Za-z0-9._=-]+' \
        | sed 's|.*//||' >> "$work/baked"
done
sort -u -o "$work/known" "$work/known"
# the two namespace anchors are userdict names that exist only while the
# language loads; the finalizer undefines them, so no dump can show them
printf '.internaldict\n.xpostsys\n' >> "$work/known"
sort -u -o "$work/known" "$work/known"
sort -u -o "$work/baked" "$work/baked"
comm -23 "$work/baked" "$work/known" > "$work/badbaked"

# ---- the check ----
if ! comm -23 "$work/want" "$work/have" > "$work/dangling"; then
    echo "FAILURES: could not compare the reference and register sets"
    exit 1
fi

fail=0

if [ -s "$work/dangling" ]; then
    echo "FAIL: fetched from a private namespace, but not present in it:"
    while read -r ns name; do
        printf '      %s /%s\n' "$ns" "$name"
        grep -n "$name get" "$src"/data/*.ps 2>/dev/null | head -2 | sed 's|^|          |'
    done < "$work/dangling"
    echo "      define it, or guard the fetch with known if it is optional"
    fail=1
fi

if [ -s "$work/badbaked" ]; then
    echo "FAIL: frozen in by immediate evaluation, but resolves to nothing:"
    while read -r name; do
        printf '      //%s\n' "$name"
        grep -n "//$name" "$src"/data/*.ps 2>/dev/null | head -2 | sed 's|^|          |'
    done < "$work/badbaked"
    echo "      the name must be defined before the line that freezes it in"
    fail=1
fi

[ "$fail" = 0 ] || exit 1

echo "SUCCESS ($(wc -l < "$work/want" | tr -d ' ') fetches and $(wc -l < "$work/baked" | tr -d ' ') frozen names resolve)"
exit 0
