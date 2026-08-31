#!/bin/sh
# Meson test wrapper: every reach into userdict is registered with the
# reason it is the program's business -- from compiled code, and from
# the boot files.
#
# userdict is the program's workspace. Compiled code has exactly one way
# to reach it -- fetching slot 2 of the dictionary stack from the bottom
# -- and that mechanism is what is derived here, so the register cannot
# be satisfied by renaming anything. The population may not grow
# quietly: a driver that starts reading a knob out of userdict, or
# defining machinery into it, fails here until a line in
# tests/userdict_reach.golden owns the decision, and a reach that
# disappears fails too, so the register cannot outlive what it
# describes.
#
# Comments are stripped before the scan, so a call written about is not
# a call. The scan is by line, held to a whitespace-joined scan of the
# same text, so a reach wrapped across lines is reported rather than
# missed: the spelling is one line, which is also what lets the
# enclosing function be named.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-userdict-reach.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
golden="$src/tests/userdict_reach.golden"
[ -s "$golden" ] || { echo "FAILURES: no usable register at $golden"; exit 1; }
psgolden="$src/tests/userdict_parked.golden"
[ -s "$psgolden" ] || { echo "FAILURES: no usable register at $psgolden"; exit 1; }

guard_workdir

# the fixed-slot fetch that answers userdict; ds is reached through
# whichever context variable the site holds
reach='xpost_stack_bottomup_fetch[(][A-Za-z_][A-Za-z0-9_]*->lo, *[A-Za-z_][A-Za-z0-9_]*->ds, *2[)]'

strip_comments() {
    awk '
        {
            line = $0; sub(/\r$/, "", line)
            out = ""; i = 1; n = length(line)
            while (i <= n) {
                c = substr(line, i, 1); t = substr(line, i, 2)
                if (inblock) {
                    if (t == "*/") { inblock = 0; i += 2 } else i++
                    continue
                }
                if (t == "/*") { inblock = 1; i += 2; continue }
                if (t == "//") break
                out = out c; i++
            }
            print out
        }' "$1"
}

: > "$work/derived"
nline=0
njoin=0
for f in "$src"/src/lib/*.c "$src"/src/lib/*.h "$src"/src/bin/*.c; do
    [ -f "$f" ] || continue
    strip_comments "$f" > "$work/stripped"
    # by line, remembering the function the site sits in
    grep -c . /dev/null >/dev/null # keep set -u quiet on empty loops
    awk -v file="$(basename "$f")" -v reach="$reach" '
        /^[A-Za-z_][A-Za-z0-9_ *]*[A-Za-z0-9_*]\(/ {
            fn = $0; sub(/\(.*/, "", fn); sub(/.*[ *]/, "", fn)
        }
        $0 ~ reach { print file, (fn == "" ? "?" : fn) }
    ' "$work/stripped" >> "$work/derived"
    n=$(grep -cE "$reach" "$work/stripped" || true)
    nline=$((nline + n))
    j=$(tr '\n' ' ' < "$work/stripped" | grep -oE "$reach" | grep -c . || true)
    njoin=$((njoin + j))
done

if [ "$nline" -ne "$njoin" ]; then
    echo "FAILURES: a reach into userdict is wrapped across lines and the"
    echo "      line scan cannot name its function. The spelling is one"
    echo "      line ($nline named, $njoin found joined)."
    exit 1
fi

sort -u "$work/derived" > "$work/derived.set"
grep -v '^[[:space:]]*#' "$golden" | grep . | sort -u > "$work/golden.set"

# a register whose every line is commentary registers nothing, and the
# interpreter cannot run at all without the create-time reach, so an
# empty side here is the scan or the register broken, never the tree
if [ ! -s "$work/derived.set" ] || [ ! -s "$work/golden.set" ]; then
    echo "FAILURES: the scan derived nothing or the register names nothing;"
    echo "      an empty side would hold everything to nothing and pass"
    exit 1
fi

guard_held=0
guard_hold "$work/derived.set" "$work/golden.set" \
    "compiled code reaches userdict at sites the register does not
      own. userdict is the program's workspace: neither a configuration
      channel nor a place to define machinery. Register the reach with
      its reason, or take it out:" \
    "the register owns a reach into userdict that no code makes. The
      site is gone or renamed; the line excusing it is cover for the
      next one:"

if [ "$guard_held" -ne 0 ]; then
    echo "FAILURES: the reaches into userdict and their register disagree"
    exit 1
fi
# ---- and the boot files' own touches of userdict --------------------
#
# The same question of the PostScript side, by the same kind of
# mechanism: the token userdict, read with comments stripped and string
# contents blanked, paired with the token that follows it -- which for a
# put is the name being parked. A boot file that starts keeping
# machinery state in userdict grows a row here and fails until the
# register owns the decision.
: > "$work/ps.derived"
for f in "$src"/data/*.ps; do
    sed 's|(%[^)]*)|(STR)|g; s|%.*||' "$f" \
        | awk -v file="$(basename "$f")" \
            '{ for (i=1;i<=NF;i++) if ($i=="userdict")
                   print file, (i<NF ? $(i+1) : "-") }' >> "$work/ps.derived"
done
sort -u "$work/ps.derived" > "$work/ps.derived.set"
grep -v '^[[:space:]]*#' "$psgolden" | grep . | sort -u > "$work/ps.golden.set"

if [ ! -s "$work/ps.derived.set" ] || [ ! -s "$work/ps.golden.set" ]; then
    echo "FAILURES: the boot-file scan derived nothing or the register names"
    echo "      nothing; an empty side would hold everything to nothing"
    exit 1
fi

guard_hold "$work/ps.derived.set" "$work/ps.golden.set" \
    "a boot file touches userdict at sites the register does not own.
      userdict is the program's workspace: machinery state parked there
      is a program's to shadow or corrupt. Register the touch with its
      reason, or move the state where its consumers live:" \
    "the register owns a touch of userdict no boot file makes. The site
      is gone or renamed; the line excusing it is cover for the next
      one:"

if [ "$guard_held" -ne 0 ]; then
    echo "FAILURES: the reaches into userdict and their registers disagree"
    exit 1
fi
echo "SUCCESS (every reach into userdict is registered, $nline compiled sites)"
exit 0
