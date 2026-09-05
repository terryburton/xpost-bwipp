#!/bin/sh
# Guard the DSC writer's output vocabulary, in both directions.
#
# What that device emits is a program in a language it defines as it
# goes: a prolog of short operator names, and a page written in them.
# Two things can go wrong and neither is visible to a byte comparison.
# The page can name an operator the prolog never defined, which stops the
# reader where it is named; and the prolog can define an operator no
# workload ever reaches, which ships unexercised.
#
# So: every bare token the output uses is defined by the prolog, defined
# by the output itself, or named in tests/dsc-vocabulary with the reason
# it is there. And every operator the prolog defines is reached by the
# workloads below.
#
#   $1  path to the source tree root      $2  path to the interpreter
set -u
src=${1:?usage: check-dsc-vocabulary.sh <source root> <xpost>}
xp=${2:?usage: check-dsc-vocabulary.sh <source root> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
[ -x "$xp" ] || { echo "check-dsc-vocabulary: no interpreter at $xp"; exit 1; }
reg=$src/tests/dsc-vocabulary
[ -f "$reg" ] || { echo "check-dsc-vocabulary: no register at $reg"; exit 1; }

# The workload draws text, because a drawn letter filed as a description
# and called wherever it falls is one of the routes into the vocabulary
# below: a build with no face library cannot make that page and says so
# rather than reading a register off a page that was never written.
. "$(dirname "$0")/verdict.sh"
skip_if_faceless "$xp" "the workload these operators are read off draws text"

guard_workdir
fail=0

# The workloads. Between them they must reach every operator the prolog
# defines; an operator no workload reaches is reported below by name.
cat > "$work/w.ps" <<'EOF'
/F << /FormType 1 /BBox [0 0 40 40] /Matrix [1 0 0 1 0 0]
      /PaintProc { pop 0 0 40 40 rectfill } >> def
0.25 0.5 0.75 setrgbcolor  10 10 moveto 100 10 lineto 100 60 lineto closepath fill
0 setgray 3 setlinewidth 1 setlinecap 1 setlinejoin 4 setmiterlimit
  120 10 moveto 200 60 lineto 240 10 lineto stroke
0.1 0.2 0.3 0.4 setcmykcolor 260 10 60 40 rectfill
0.5 setgray 340 10 40 40 rectfill
0.9 0.1 0.4 setrgbcolor 2 setlinewidth 10 100 moveto 120 100 lineto stroke
0.2 0.4 0.6 0.1 setcmykcolor 10 130 moveto 120 130 lineto stroke
gsave newpath 400 10 40 40 rectclip 390 0 60 60 rectfill grestore
gsave  60.5 300.5 translate F execform grestore
gsave 160.5 300.5 translate F execform grestore
gsave 100 500 moveto 200 500 260 560 300 500 curveto 2 setlinewidth stroke grestore
% A face the reader cannot be assumed to have, so its letters are drawn
% rather than named -- and a letter that comes back often enough is filed
% as a description and called wherever it falls, which is a second route
% into the vocabulary below and the one that reached it with no
% definition written. Repeated because filing is what repetition buys.
/NotAFaceAReaderHas 10 selectfont
1 1 12 { 40 mul 700 exch moveto (the quick brown fox jumps over the lazy dog) show } for
showpage
EOF

"$xp" -d dscwrite -o "$work/out.dsc" "$work/w.ps" </dev/null >/dev/null 2>&1 || {
    echo "check-dsc-vocabulary: the writer did not produce a page"; exit 1; }

# what the prolog defines, and what the page uses
grep -oE '^/[A-Za-z][A-Za-z0-9]* \{' "$work/out.dsc" \
    | sed 's|^/||; s| {$||' | sort -u > "$work/defined"
# an operator the prolog gives a second name to, rather than a body
grep -oE '/[A-Za-z][A-Za-z0-9]* /[A-Za-z][A-Za-z0-9]* load def' "$work/out.dsc" \
    | sed 's|^/||; s| .*||' | sort -u >> "$work/defined"
# names the output defines for itself as it goes (a description's procedure)
grep -oE '^/Fm[0-9]+ \{' "$work/out.dsc" | sed 's|^/||; s| {$||' | sort -u >> "$work/defined"
sort -u "$work/defined" -o "$work/defined"
# The page only. The prolog is where the operators are defined, and it
# is written in the reader's own language by definition; what is being
# held here is what the page says, in the language the prolog made.
awk '/^%%BeginProlog/,/^%%EndProlog/{next} !/^%/' "$work/out.dsc" \
    | tr -cs 'A-Za-z0-9_' '\n' \
    | grep -E '^[A-Za-z][A-Za-z0-9]*$' | sort -u > "$work/used"
grep -vE '^#|^$' "$reg" | awk '{print $1}' | sort -u > "$work/allowed"

# direction one: nothing is emitted that the reader cannot run
sort -u "$work/defined" "$work/allowed" > "$work/known"
if unknown=$(comm -23 "$work/used" "$work/known") && [ -n "$unknown" ]; then
    echo "FAIL: the page uses operators its own prolog does not define and"
    echo "      tests/dsc-vocabulary does not name. A reader of this output"
    echo "      stops where one of them is named:"
    printf '      %s\n' $unknown
    fail=1
fi

# direction two: nothing is defined that no workload reaches
if unused=$(comm -13 "$work/used" "$work/defined") && [ -n "$unused" ]; then
    echo "FAIL: the prolog defines operators no workload here reaches, so"
    echo "      what they emit is never run back and never compared. Add a"
    echo "      workload to this guard that reaches each, or take the"
    echo "      definition out:"
    printf '      %s\n' $unused
    fail=1
fi

[ "$fail" = 0 ] || { echo "FAILURES: see above; tests/dsc-vocabulary is the register"; exit 1; }
echo "SUCCESS ($(wc -l < "$work/defined") defined, $(wc -l < "$work/used") used, $(grep -cvE '^#|^$' "$reg") named in the register)"
