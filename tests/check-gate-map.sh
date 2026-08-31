#!/bin/sh
# Guard: the table that decides which tests a change is gated against
# describes the tree it is applied to, and leaves no test out of reach.
#
# tests/gate-map is what lets a gate be smaller than the suite. Its
# failure modes are not alike, and only one of them is loud:
#
#   A path no rule classifies is not a failure. It falls through to the
#   catch-all, selects the whole suite at both widths, and costs a
#   developer time. Nothing is checked about it here for that reason:
#   the table is allowed to be incomplete, because incomplete means slow
#   and never means small.
#
#   A test no area names IS a failure, and a silent one. It is a test
#   that no proportionate selection ever runs, and every other check in
#   this tree would go on passing without it: the suite still holds it,
#   `full` still runs it, and only a gate narrower than the suite stops
#   asking it anything. This is what says so. It is the reason the table
#   can be trusted to subtract.
#
#   A rule that matches nothing is a failure too, in both directions. A
#   test rule naming a test that has been renamed stops selecting it and
#   says nothing; a path rule shadowed by an earlier one -- the ordering
#   here is first-match-wins -- stops classifying the files it names and
#   also says nothing. Both read as a table that is still working.
#
# A rule is checked against what the build actually carries rather than
# against a list written here. The tests come from the build directory's
# own record of them, which meson writes at configure time, so a test
# registered today is inside this rule today. The files come from the
# tree.
#
#   $1  path to the source tree root
#   $2  path to a build directory, for the tests it defines
set -u
src=${1:?usage: check-gate-map.sh <srcroot> <buildroot>}
build=${2:?usage: check-gate-map.sh <srcroot> <buildroot>}
. "$(dirname "$0")/guard-paths.sh"
. "$(dirname "$0")/meson-tests.sh"
guard_require_srcroot "$src"
guard_require_file "$src/tests/gate-map" "the gate map"
guard_require_file "$src/tests/gate.sh" "the gate"
guard_require_dir "$build" "the build directory"
guard_require_file "$build/meson-info/intro-tests.json" \
    "the build's record of the tests it defines"

guard_workdir
guard_mirror gate "$src/tests/gate-map"
map="$mirror/gate-map"

fail=0

# ---- what the build defines
#
# Read out of the record meson writes at configure time rather than by
# running meson, because this runs as one of the tests meson is running.
# A test object states its name between the command it runs and the
# directory it runs in, which is the one place in the file that shape
# occurs: an environment variable's name is a key and carries no space
# before its value.
#
# How that record is laid out is not fixed. Some meson versions write it
# as one line and others indent it over thousands, and a host whose text
# files end their lines in CRLF leaves a return on each. The whole file
# is therefore flattened to one line of single-spaced text before the
# shape above is looked for, so that the same rule reads every layout
# rather than one of them reading as a build defining no tests.
meson_test_names "$build/meson-info/intro-tests.json" "$work/tests" || exit 1
ntests=$(grep -c . "$work/tests" || true)

# ---- what the tree holds
#
# Tracked files where there is something to ask, and a walk of the
# directories the table names otherwise. Either way the build
# directories are outside it: this tree carries several, each holding a
# copy of every generated header, and a path rule that matched one of
# those would read as live while classifying nothing anyone edits.
#
# Which of the two is used is decided by what the first one answers,
# not by where it is being asked. A tree unpacked for a release check
# sits under a build directory, which is under the working copy, so a
# question about the context -- is there a repository here -- is
# answered yes for a tree the repository lists nothing of: the files
# there are ignored, being generated, and asking git about them yields
# an empty list rather than an error. Taking the answer instead means
# the walk is reached wherever the listing comes back short, which is
# the case the walk is for.
( cd "$src" && git ls-files ) > "$work/files" 2>/dev/null
if [ "$(grep -c . "$work/files" || true)" -lt 100 ]; then
    ( cd "$src" && find data doc examples m4 src tests tools -type f -print \
        2>/dev/null ) > "$work/files"
    ( cd "$src" && find . -maxdepth 1 -type f -print 2>/dev/null |
        sed 's|^\./||' ) >> "$work/files"
fi
sort -u -o "$work/files" "$work/files"
nfiles=$(grep -c . "$work/files" || true)
if [ "$nfiles" -lt 100 ]; then
    echo "FAILURES: found $nfiles file(s) under $src, which cannot be right."
    echo "      Every path rule below would then be checked against nothing."
    exit 1
fi

# ---- the table, read once
#
# A line is an area, a kind and a value. Anything else in the file is a
# line this cannot act on, and a table with a line nobody reads is a
# table saying something nobody hears.
awk '
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*$/ { next }
    { if (NF != 3 || ($2 != "path" && $2 != "test" && $2 != "testif" &&
                     $2 != "width" && $2 != "scope")) {
          print FNR "\t" $0; bad++ }
    }
    END { exit 0 }
' "$map" > "$work/malformed"
if [ -s "$work/malformed" ]; then
    echo "FAIL: gate-map holds line(s) that are not <area> <path|test|testif|width|scope> <value>:"
    sed 's/^/      line /' "$work/malformed"
    fail=1
fi

awk '/^[[:space:]]*#/ || NF != 3 { next }
     $2 == "path"  { print $1 "\t" $3 > (o "/rule.path") }
     $2 == "test"  { print $1 "\t" $3 > (o "/rule.test")
                     print $1 "\t" $3 > (o "/rule.test.strict") }
     $2 == "testif" { print $1 "\t" $3 > (o "/rule.test") }
     $2 == "width" { print $1 "\t" $3 > (o "/rule.width") }
     $2 == "scope" { print $1 "\t" $3 > (o "/rule.scope") }
' o="$work" "$map"
for k in path test test.strict width scope; do [ -f "$work/rule.$k" ] || : > "$work/rule.$k"; done

cut -f1 "$work/rule.path" "$work/rule.test" "$work/rule.width" \
       "$work/rule.scope" | sort -u > "$work/areas"

cat > "$work/glob.awk" <<'AWK'
function globre(g,   i, c, r, star) {
    r = "^"; star = 0
    for (i = 1; i <= length(g); i++) {
        c = substr(g, i, 1)
        if (c == "*") { star++; continue }
        if (star) { r = r (star > 1 ? ".*" : "[^/]*"); star = 0 }
        if (c ~ /[.+(){}\[\]^$|\\?]/) r = r "\\" c
        else r = r c
    }
    if (star) r = r (star > 1 ? ".*" : "[^/]*")
    return r "$"
}
AWK

# ---- one width policy per area
while read -r a; do
    n=$(awk -F'\t' -v a="$a" '$1 == a' "$work/rule.width" | grep -c . || true)
    if [ "$n" -ne 1 ]; then
        echo "FAIL: area $a declares $n width policies; it needs exactly one"
        echo "      of 'full' and 'witness'"
        fail=1
        continue
    fi
    p=$(awk -F'\t' -v a="$a" '$1 == a { print $2 }' "$work/rule.width")
    case $p in
        full|witness) ;;
        *) echo "FAIL: area $a declares the width policy '$p'; it must be"
           echo "      'full' -- the wide build runs the whole selection --"
           echo "      or 'witness' -- it runs the tests that read the width"
           fail=1 ;;
    esac
done < "$work/areas"

# ---- an area a path can reach must select something
#
# Either by naming tests, or by saying that the whole suite answers for
# it. An area that does neither is one a change can land in and be gated
# on nothing at all.
while read -r a; do
    if awk -F'\t' -v a="$a" '$1 == a { f = 1 } END { exit !f }' "$work/rule.path" &&
       ! awk -F'\t' -v a="$a" '$1 == a { f = 1 } END { exit !f }' "$work/rule.test" &&
       ! awk -F'\t' -v a="$a" '$1 == a && $2 == "all" { f = 1 } END { exit !f }' "$work/rule.scope"
    then
        echo "FAIL: area $a classifies paths, names no test and does not"
        echo "      carry scope all, so a change reaching it would be gated"
        echo "      on nothing at all"
        fail=1
    fi
done < "$work/areas"

# A scope is `all` or it is not written. Anything else is a word this
# does not act on sitting where a reader would take it for a policy.
awk -F'\t' '$2 != "all" { print }' "$work/rule.scope" > "$work/badscope"
if [ -s "$work/badscope" ]; then
    echo "FAIL: gate-map declares a scope that is not 'all':"
    sed 's/^/      /' "$work/badscope"
    fail=1
fi

# ---- the areas that are not parts of the tree
#
# `width` is what the wide build runs and `always` is what every gate
# runs. Neither is a region of the source, and a path rule under either
# would put a change into an area that answers a different question.
for a in width always; do
    if ! grep -qx "$a" "$work/areas"; then
        echo "FAIL: gate-map defines no $a area; the gate needs one"
        fail=1
    elif awk -F'\t' -v a="$a" '$1 == a { f = 1 } END { exit !f }' "$work/rule.path"; then
        echo "FAIL: area $a classifies paths, and it is not a part of the"
        echo "      tree: it is what the gate runs, not what a change touches"
        fail=1
    fi
done

# The personality probe asks all three faces of the width outright and
# is the one test that cannot be left out of what the wide build runs.
if ! awk -F'\t' '$1 == "width" && $2 == "build-personality" { f = 1 }
                 END { exit !f }' "$work/rule.test"; then
    echo "FAIL: the width area does not name build-personality, which is"
    echo "      the test that asks the composite bound, the integer horizon"
    echo "      and the real's significand outright. Without it the wide"
    echo "      build's tripwire has nothing in it that asks the question"
    fail=1
fi

# ---- no test rule is stale
#
# Every awk below reads its program from two files: the glob converter
# above and the pass itself. An inline program alongside -f is read as
# another input FILE, which awk then fails to open and carries on from,
# so the pass produces nothing and its check reads as a clean one.
cat > "$work/stale-test.awk" <<'AWK'
FILENAME == names { t[++nt] = $0; next }
{ for (i = 1; i <= nt; i++) if (t[i] ~ globre($2)) next
  print $1 "\t" $2 }
AWK
awk -f "$work/glob.awk" -f "$work/stale-test.awk" \
    names="$work/tests" "$work/tests" FS="$guard_tab" "$work/rule.test.strict" \
    > "$work/stale.test"
if [ -s "$work/stale.test" ]; then
    echo "FAIL: $(grep -c . "$work/stale.test") test rule(s) in gate-map name no test the build"
    echo "      defines. A renamed test leaves its rule behind, and the rule"
    echo "      then selects nothing while reading as though it did. A test"
    echo "      a configuration may legitimately not define is written"
    echo "      testif rather than test:"
    sed 's/^/      /' "$work/stale.test"
    fail=1
fi

# ---- no path rule is stale or shadowed
#
# Ordered, first match wins, so a rule is live only if some file reaches
# it: one that names files an earlier rule already took has stopped
# classifying anything, which is the mistake that comes of moving a
# block of the table rather than of editing a rule.
cat > "$work/stale-path.awk" <<'AWK'
FILENAME == rules { n++; ra[n] = $1; rg[n] = globre($2); rr[n] = $2; next }
{ for (i = 1; i <= n; i++) if ($0 ~ rg[i]) { hit[i]++; break } }
END { for (i = 1; i <= n; i++) if (!hit[i]) print ra[i] "\t" rr[i] }
AWK
awk -f "$work/glob.awk" -f "$work/stale-path.awk" \
    rules="$work/rule.path" FS="$guard_tab" "$work/rule.path" "$work/files" \
    > "$work/stale.path"
if [ -s "$work/stale.path" ]; then
    echo "FAIL: $(grep -c . "$work/stale.path") path rule(s) in gate-map win no file. Either the"
    echo "      files they name are gone, or a rule earlier in the table"
    echo "      already claims them -- the order is first match wins, so a"
    echo "      block moved above another silently takes its files:"
    sed 's/^/      /' "$work/stale.path"
    fail=1
fi

# ---- every file reaches a rule
cat > "$work/unclassified.awk" <<'AWK'
FILENAME == rules { n++; rg[n] = globre($2); next }
{ for (i = 1; i <= n; i++) if ($0 ~ rg[i]) next
  print }
AWK
awk -f "$work/glob.awk" -f "$work/unclassified.awk" \
    rules="$work/rule.path" FS="$guard_tab" "$work/rule.path" "$work/files" \
    > "$work/unclassified"
if [ -s "$work/unclassified" ]; then
    echo "FAIL: $(grep -c . "$work/unclassified") file(s) reach no rule at all, so the gate has"
    echo "      nothing to widen them to. The table ends in a catch-all for"
    echo "      exactly this; check it is still last and still matches:"
    sed 's/^/      /' "$work/unclassified" | head -10
    fail=1
fi

# ---- every test is named by an area
#
# An area whose test rule is a bare `*` names every test by
# construction, so it is no evidence that any particular test is
# reachable and is discounted here. That is why an area saying the whole
# suite answers for it says so with `scope all` and not with a rule that
# would satisfy this without classifying anything.
awk -F'\t' '{ c[$1]++; if ($2 == "*") w[$1] = 1 }
            END { for (a in c) if (!w[a]) print a }' "$work/rule.test" \
    | sort > "$work/areas.specific"
cat > "$work/unreachable.awk" <<'AWK'
FILENAME == picked { keep[$0] = 1; next }
FILENAME == rules { if ($1 in keep) { n++; rg[n] = globre($2) } ; next }
{ for (i = 1; i <= n; i++) if ($0 ~ rg[i]) next
  print }
AWK
awk -f "$work/glob.awk" -f "$work/unreachable.awk" \
    picked="$work/areas.specific" rules="$work/rule.test" \
    "$work/areas.specific" FS="$guard_tab" "$work/rule.test" \
    "$work/tests" > "$work/unreachable"
if [ -s "$work/unreachable" ]; then
    echo "FAIL: $(grep -c . "$work/unreachable") of $ntests test(s) are named by no area, so no gate"
    echo "      narrower than the whole suite ever runs them. Name each in"
    echo "      the area it answers for:"
    sed 's/^/      /' "$work/unreachable"
    fail=1
fi

# ---- what the wide build runs has a narrow home too
#
# The `width` area is not a region of the tree, so nothing a change
# touches lands in it and no per-change gate selects it in the narrow
# build. A test named there and nowhere else is therefore one the narrow
# build never runs unless the whole suite does -- which is the same
# unreachability as before, moved one step: the test would be reachable
# on the primary width only through a gate that was not selecting.
cat > "$work/widehome.awk" <<'AWK'
FILENAME == rules { if ($1 == "width") { nw++; wg[nw] = globre($2) }
                    else { no++; og[no] = globre($2) } ; next }
{
    for (i = 1; i <= nw; i++) if ($0 ~ wg[i]) {
        for (j = 1; j <= no; j++) if ($0 ~ og[j]) next
        print
        next
    }
}
AWK
awk -f "$work/glob.awk" -f "$work/widehome.awk" \
    rules="$work/rule.test" FS="$guard_tab" "$work/rule.test" \
    "$work/tests" > "$work/widthonly"
if [ -s "$work/widthonly" ]; then
    echo "FAIL: $(grep -c . "$work/widthonly") test(s) are named by the width area and by no"
    echo "      other, so the narrow build -- which is the primary one --"
    echo "      never runs them under any per-change gate. Name each in the"
    echo "      area it answers for as well:"
    sed 's/^/      /' "$work/widthonly"
    fail=1
fi

# ---- the page that tells a developer what to run counts the same suite
#
# doc/GATING.md opens by saying how large the suite is, and says it again
# for the areas that select all of it. A number written in prose is held
# by nothing and drifts by exactly as much as the suite grows: MEASURED,
# it said 316 where the tree carried 458, and every per-area count under
# it was stale by the same neglect. The total is the one figure this
# guard already knows for certain, so it is the one it holds.
gating=$src/../doc/GATING.md
[ -f "$gating" ] || gating=$src/doc/GATING.md
if [ -f "$gating" ]; then
    for said in $(sed -n 's/.*[Tt]he suite is \([0-9]\{1,\}\) tests.*/\1/p;
                          s/.*all \([0-9]\{1,\}\), both widths.*/\1/p' "$gating"); do
        if [ "$said" -ne "$ntests" ]; then
            echo "FAIL: doc/GATING.md counts the suite as $said tests and it holds $ntests."
            echo "      The page says which run answers which question, and a"
            echo "      developer choosing a run from a stale figure is choosing"
            echo "      from the tree as it was."
            fail=1
        fi
    done
fi

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: the gate map does not describe this tree"
    exit 1
fi

echo "SUCCESS ($ntests tests, all named by an area;" \
     "$(grep -c . "$work/rule.path") path rules over $nfiles files, none stale)"
exit 0
