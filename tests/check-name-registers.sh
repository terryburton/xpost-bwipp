#!/bin/sh
# Meson test wrapper: hold the name registers the isolation tests walk to
# the names a live interpreter really has.
#
# tests/isolation_test.ps and tests/nographics_test.ps each iterate over
# lists of names and assert something about every one of them. Both
# assertions are about a name NOT being reachable, so an entry naming
# something the interpreter does not have passes for the wrong reason:
# nothing defines it, so nothing can show it. The entry then checks
# nothing, cannot fail, and stays that way for as long as the name is
# wrong -- which is as long as nobody looks, because the run is green.
# Two had gone that way already. /.t3capture went on being listed as a
# hidden font helper for the whole life of the helper called .t3capkeys,
# and /arcbez outlived the PostScript arc-to-bezier conversion it named
# by the whole of that conversion's life in C.
#
# So the registers are held to a live startup, the way
# check-private-refs.sh holds the private namespaces: a name counts as
# present when the interpreter holds it, however it came to be there --
# defined in place, relocated at lockdown, or installed from C. A guard
# built the other way, matching the registers against the sources with a
# second set of patterns, would call a name missing every time a
# definition form was spelt in a way the patterns did not anticipate.
#
# The two registers are held to different things, because they are
# written for different runs.
#
# isolation_test.ps runs with graphics loaded and asserts each name is
# hidden from the program. The name must therefore be one the interpreter
# holds somewhere -- a private namespace, the local scratch dictionary,
# or systemdict -- or there is nothing being hidden.
#
# nographics_test.ps runs without graphics and asserts each name is
# absent. An absence proves the graphics load did not happen only if the
# name would have been there had it happened, so each name must be in
# systemdict when graphics loads and out of it when graphics does not. A
# private helper satisfies neither half: it is invisible to a program
# whether graphics loaded or not. That is what the five names this
# register used to carry were -- measured both ways round, all five
# answered identically in the two runs the test exists to tell apart.
#
# The registers are read from the tests rather than restated here. A
# second copy of what the tests iterate over is a second answer, and the
# one that goes stale is the one nothing runs.
#
#   $1  path to the xpost binary
#   $2  path to the source tree root
set -u
xpost=${1:?usage: check-name-registers.sh <xpost> <srcroot>}
src=${2:?usage: check-name-registers.sh <xpost> <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

iso="$src/tests/isolation_test.ps"
nog="$src/tests/nographics_test.ps"
guard_require_file "$iso" "the isolation test"
guard_require_file "$nog" "the no-graphics test"

guard_workdir
cr=$(printf '\r')   # tolerate CRLF line endings (Windows checkouts)

# ---- the register: what a real interpreter holds ----
# .xpostsys cannot be named after lockdown. It is recovered the way
# tamper_dispatch_test.ps and check-private-refs.sh recover it:
# privatedict anchors bound procedures, and those carry the dictionary
# baked in by their // references. The local scratch dictionary is
# reached through the .gscratch operator, which keeps its systemdict
# name; three of the names these registers carry live only there.
cat > "$work/dump.ps" <<'PSEOF'
/dump { % dict (label)  .  -
  /lbl exch def
  { pop dup type /nametype eq
    { lbl print 60 string cvs print (\n) print }{ pop } ifelse } forall
} bind def
1183615869 internaldict /.namespacenames known
  { /xpostsys 1183615869 internaldict /.namespacenames get exec
    { (xpostsys ) print 60 string cvs print (\n) print } forall } if
.privatedict (privatedict ) dump
1183615869 internaldict (internaldict ) dump
.gscratch (gscratch ) dump
systemdict (systemdict ) dump
PSEOF

# The same interpreter without its graphics: only systemdict is asked
# for, because that is the only thing the no-graphics register is about,
# and the font machinery that reaches internaldict is not loaded there.
cat > "$work/nodump.ps" <<'PSEOF'
/dump { % dict (label)  .  -
  /lbl exch def
  { pop dup type /nametype eq
    { lbl print 60 string cvs print (\n) print }{ pop } ifelse } forall
} bind def
systemdict (systemdict ) dump
PSEOF

XPOST_DATA_DIR="$src/data" XPOST_CENSUS=1 "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/dump.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | grep -E '^(xpostsys|privatedict|internaldict|gscratch|systemdict) .' \
    | sort -u > "$work/all"

XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox --no-graphics -d null \
    -o /dev/null "$work/nodump.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | grep -E '^systemdict .' | sort -u > "$work/nogfx"

for ns in xpostsys privatedict internaldict gscratch systemdict; do
    if ! grep -q "^$ns " "$work/all"; then
        echo "FAILURES: the interpreter reported no $ns members; the register is unusable"
        exit 1
    fi
done
if [ ! -s "$work/nogfx" ]; then
    echo "FAILURES: the no-graphics interpreter reported no systemdict members;"
    echo "      the register is unusable"
    exit 1
fi

awk '{ print $2 }' "$work/all" | sort -u > "$work/held"
awk '$1 == "systemdict" { print $2 }' "$work/all" | sort -u > "$work/gfxsys"
awk '{ print $2 }' "$work/nogfx" | sort -u > "$work/nogfxsys"

# ---- what the tests walk ----
# A register is a named array of literal names at the top level of a
# test: "/NAME [ /a /b ... ] def", however many lines it takes. The name
# is read off the head so a failure can say which register carries the
# entry. A `%` inside a string is not a comment, so those are neutralised
# before comments are stripped.
registers() {       # <file> <tag>
    tr -d "$cr" < "$1" | sed 's|(%[^)]*)|(STR)|g; s|%.*||' | awk -v tag="$2" '
        /^\/[A-Z][A-Z0-9]*[ \t]+\[/ { reg = substr($1, 2); open = 1; buf = "" }
        open {
            buf = buf " " $0
            if (index($0, "]") > 0) {
                n = split(buf, tok, /[ \t]+/)
                for (i = 1; i <= n; i++)
                    if (substr(tok[i], 1, 1) == "/" && tok[i] != "/" reg)
                        print tag, reg, substr(tok[i], 2)
                open = 0
            }
        }'
}

registers "$iso" isolation > "$work/entries"
registers "$nog" nographics >> "$work/entries"

for tag in isolation nographics; do
    if ! grep -q "^$tag " "$work/entries"; then
        echo "FAILURES: found no name register in the $tag test; the registers"
        echo "      were emptied or renamed and this guard no longer reads them"
        exit 1
    fi
done

# ---- the check ----
fail=0

# Every name the isolation register hides must be one the interpreter has.
: > "$work/absent"
while read -r tag reg name; do
    [ "$tag" = isolation ] || continue
    grep -qxF "$name" "$work/held" || printf '%s /%s\n' "$reg" "$name" >> "$work/absent"
done < "$work/entries"
if [ -s "$work/absent" ]; then
    echo "FAIL: listed as hidden from a program, but the interpreter has no"
    echo "      such name, so nothing is being hidden:"
    sed 's|^|      |' "$work/absent"
    echo "      correct the name, or drop the entry if what it named is gone"
    fail=1
fi

# Every name the no-graphics register calls absent must be one that
# loading graphics puts in systemdict and not loading it leaves out.
: > "$work/undiscerning"
while read -r tag reg name; do
    [ "$tag" = nographics ] || continue
    if ! grep -qxF "$name" "$work/gfxsys"; then
        printf '%s /%s (graphics does not put it in systemdict)\n' \
            "$reg" "$name" >> "$work/undiscerning"
    elif grep -qxF "$name" "$work/nogfxsys"; then
        printf '%s /%s (it is in systemdict without graphics too)\n' \
            "$reg" "$name" >> "$work/undiscerning"
    fi
done < "$work/entries"
if [ -s "$work/undiscerning" ]; then
    echo "FAIL: listed as absent without graphics, but its absence says"
    echo "      nothing about whether graphics loaded:"
    sed 's|^|      |' "$work/undiscerning"
    echo "      list a name the graphics load adds to systemdict"
    fail=1
fi

[ "$fail" = 0 ] || exit 1

echo "SUCCESS ($(grep -c '^isolation ' "$work/entries" | tr -d ' ') hidden names and $(grep -c '^nographics ' "$work/entries" | tr -d ' ') graphics-only names resolve)"
exit 0
