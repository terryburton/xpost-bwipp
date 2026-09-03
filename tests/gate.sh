#!/bin/sh
# Run the tests a change can be answered by, at the widths it can be
# wrong at.
#
# The cost profiles in meson.build select on what a test costs. This
# selects on what a change touches: tests/gate-map says which part of
# the tree each area of the suite answers for, this reads it, and what
# runs is the union of the areas the change reaches. A change to doc/
# runs the guard over the distribution lists. A change to one device
# runs that device's tests, the byte-comparison harnesses and the
# banding campaign. A change to the object or the memory it lives in
# runs the lot, at both widths, because that is the layer a width is a
# property of.
#
# ---- what it will not do
#
# It will not decide that a change is safe. Everything here is a
# selection, and a selection is only as good as the table behind it. Two
# things keep the table from quietly shrinking the gate:
#
#   A path no rule classifies falls through to a catch-all and selects
#   the whole suite at both widths. The failure mode of an incomplete
#   table is a slow gate, never a small one.
#
#   Every test must be named by some area, which tests/check-gate-map.sh
#   holds the table to. A test in no area would be one this never runs,
#   and nothing else in the tree would say so.
#
# and one thing keeps the run from quietly shrinking too: what meson
# actually ran is read back out of its record and compared against the
# selection, name by name. A run that reported on fewer tests than were
# asked for agrees with whatever the rest would have said, which is the
# same silence the profile wrapper beside this exists to refuse.
#
# ---- the two widths
#
# The tree builds at two object widths and the narrow one is primary.
# Neither is dropped here. A change reaching the layer that decides the
# width runs its whole selection at both; every other change still runs
# the wide build, over the tests that read the width directly. Those are
# a tripwire and are said to be one: they cost a few seconds and they
# are not a wide run of the change's own tests.
#
# What is a wide run of everything is --batch, which is the gate a
# branch is held to before it goes anywhere. Per-change gating is for
# the loop in between, where the alternative is not a more thorough run
# but a developer who has stopped running anything.
#
#   tests/gate.sh [option...] [path...]
#
#   --narrow DIR   the narrow-width build (default $XPOST_NARROW_BUILD,
#                  else build)
#   --wide DIR     the wide-width build (default $XPOST_WIDE_BUILD, else
#                  blarge)
#   --since REF    take the changed paths from what REF does not have
#   --area NAME    name an area outright, whatever the paths say
#   --batch        every test, both widths: the merge-batch gate
#   --list         say what would run and stop
#   -j N           tests at once, at most 16
#
# With no paths and no --since, the change is what the working tree has
# that the last commit does not; failing that, what this branch has that
# master does not.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$(CDPATH= cd -- "$here/.." && pwd)
map="$here/gate-map"
. "$here/meson-tests.sh"
# The reading of a listing line, shared with the other readers of one.
listing="$here/listing.awk"

narrow=${XPOST_NARROW_BUILD:-build}
wide=${XPOST_WIDE_BUILD:-blarge}
since=''
batch=no
listonly=no
jobs=16
areas_named=''
paths_named=''

while [ $# -gt 0 ]; do
    case $1 in
        --narrow) narrow=${2:?--narrow needs a directory}; shift 2 ;;
        --wide)   wide=${2:?--wide needs a directory}; shift 2 ;;
        --since)  since=${2:?--since needs a revision}; shift 2 ;;
        --area)   areas_named="$areas_named ${2:?--area needs a name}"; shift 2 ;;
        --batch)  batch=yes; shift ;;
        --list)   listonly=yes; shift ;;
        -j)       jobs=${2:?-j needs a number}; shift 2 ;;
        -j*)      jobs=${1#-j}; shift ;;
        --)       shift; while [ $# -gt 0 ]; do paths_named="$paths_named $1"; shift; done ;;
        -*)       echo "FAILURES: no such option: $1"; exit 1 ;;
        *)        paths_named="$paths_named $1"; shift ;;
    esac
done

case $jobs in
    ''|*[!0-9]*) echo "FAILURES: -j wants a number, not $jobs"; exit 1 ;;
esac
[ "$jobs" -lt 1 ] && jobs=1
# Sixteen at once is the standing cap for this tree, and a gate is run
# on a machine that is doing other things.
[ "$jobs" -gt 16 ] && jobs=16

if [ ! -f "$map" ]; then
    echo "FAILURES: no gate map at $map"
    exit 1
fi
if [ ! -r "$listing" ]; then
    echo "FAILURES: no listing reader at $listing"
    exit 1
fi
for b in "$narrow" "$wide"; do
    if [ ! -f "$b/meson-info/meson-info.json" ]; then
        echo "FAILURES: $b is not a meson build directory"
        echo "      the gate needs one build of each object width; name them"
        echo "      with --narrow and --wide, or in XPOST_NARROW_BUILD and"
        echo "      XPOST_WIDE_BUILD"
        exit 1
    fi
done

# The two builds must actually be the two widths. A gate run against the
# same width twice reports a wide result that no wide build produced,
# which is the one mistake here that looks exactly like success.
width_of() {
    if grep -q '^#define WANT_LARGE_OBJECT' "$1/config.h" 2>/dev/null; then
        echo wide
    else
        echo narrow
    fi
}
if [ "$(width_of "$narrow")" != narrow ] || [ "$(width_of "$wide")" != wide ]; then
    echo "FAILURES: $narrow must be a narrow-object build and $wide a"
    echo "      wide-object build; they read as $(width_of "$narrow") and $(width_of "$wide")."
    echo "      Configure the wide one with -Dlarge-object=true"
    exit 1
fi

# The two builds treat warnings the same way or the gate is not one gate.
# Where one promotes warnings to errors and the other does not, the lax
# leg compiles what the strict leg would have refused, and a diagnostic
# only one width can raise is answered by whichever width was asked --
# so the gate reports two green legs, one of which never enforced the
# rule it was there to enforce.
werror_of() {
    grep -o '"name": "werror", "value": [a-z]*' \
        "$1/meson-info/intro-buildoptions.json" 2>/dev/null |
        sed 's/.*: //'
}
if [ "$(werror_of "$narrow")" != "$(werror_of "$wide")" ]; then
    echo "FAILURES: $narrow and $wide disagree on werror; they read as"
    echo "      '$(werror_of "$narrow")' and '$(werror_of "$wide")'."
    echo "      Set both the same with: meson configure DIR -Dwerror=BOOL"
    exit 1
fi

work=$(mktemp -d 2>/dev/null) || work=
if [ -z "$work" ] || [ ! -d "$work" ]; then
    echo "FAILURES: could not make a scratch directory (is TMPDIR writable?)"
    exit 1
fi
trap 'rm -rf "$work"' EXIT INT TERM

# A tab, as the character itself. The table below is tab-separated and
# awk is told so between the files it is handed, and an assignment among
# awk's file operands is taken as a string literal by some awks and left
# as the two characters it was spelt with by others. Where it is left, no
# rule line splits and every selection comes back empty. The trailing
# period holds the character through the substitution.
gate_tab=$(printf '\t.'); gate_tab=${gate_tab%.}

# ---- the table, split by kind, paths keeping their order
awk '
    /^[[:space:]]*#/ { next }
    NF < 3 { next }
    { area = $1; kind = $2; rest = $3
      # testif selects exactly as test does; the two differ only to the
      # checker, which holds a test rule to naming a test that exists and
      # cannot hold a testif rule to it
      if (kind == "testif") kind = "test"
      if (kind == "path" || kind == "test" || kind == "width" ||
          kind == "scope")
          print area "\t" rest > (out "/map." kind)
    }
' out="$work" "$map"
for k in path test width scope; do
    [ -f "$work/map.$k" ] || : > "$work/map.$k"
done
if [ ! -s "$work/map.path" ] || [ ! -s "$work/map.test" ]; then
    echo "FAILURES: $map names no paths or no tests"
    exit 1
fi

# ---- what the build carries
#
# Out of the record meson writes at configure time, read through the one
# reader that knows its shape (tests/meson-tests.sh), which refuses a
# short answer rather than handing back an empty selection.
#
# Regenerated first, because that record is written at configure time and
# not at test time. A commit that adds a test changes meson.build, and
# meson rebuilds its record when something asks the build for anything --
# which, left alone, is the test run itself, after this has already read
# the record the previous configure left. The selection would then be
# drawn from a list the new test is not on, the run would pass, and the
# one test the commit added would be the one test the gate did not run.
for regen in "$narrow" "$wide"; do
    [ -n "$regen" ] || continue
    [ -f "$regen/build.ninja" ] || continue
    if ! ninja -C "$regen" build.ninja >/dev/null 2>&1; then
        echo "FAILURES: $regen will not regenerate its build files, so what"
        echo "      tests it carries cannot be read"
        exit 1
    fi
done
meson_test_names "$narrow/meson-info/intro-tests.json" "$work/tests.all" \
    || exit 1

# ---- what changed
: > "$work/changed"
if [ -n "$paths_named" ]; then
    for p in $paths_named; do printf '%s\n' "$p"; done > "$work/changed"
    origin="named on the command line"
elif [ -n "$areas_named" ] && [ "$batch" = no ]; then
    # An area named outright is the whole of the request. Reading the
    # working tree as well would add whatever else is uncommitted, which
    # is the opposite of asking about one area.
    origin='the areas named'
elif [ -n "$since" ]; then
    git -C "$src" diff --name-only "$since" > "$work/changed" 2>"$work/err" || {
        echo "FAILURES: could not read what changed since $since"
        sed 's/^/      /' "$work/err"
        exit 1
    }
    origin="what $since does not have"
else
    git -C "$src" status --porcelain 2>/dev/null |
        sed 's/^...//; s/.* -> //' > "$work/changed"
    origin='the working tree'
    if [ ! -s "$work/changed" ]; then
        base=$(git -C "$src" merge-base master HEAD 2>/dev/null || echo '')
        if [ -n "$base" ]; then
            git -C "$src" diff --name-only "$base" > "$work/changed" 2>/dev/null
            origin='what master does not have'
        fi
    fi
fi
sort -u -o "$work/changed" "$work/changed"

if [ "$batch" = no ] && [ ! -s "$work/changed" ] && [ -z "$areas_named" ]; then
    echo "FAILURES: nothing changed and no area was named, so there is"
    echo "      nothing to gate. Name paths or an area, or --batch."
    exit 1
fi

# ---- a path finds its area: first rule that matches, and the rules are
# in the order the table wrote them
#
# A glob's `*` stops at a path separator and `**` crosses them, so a
# rule for one directory cannot reach into another by accident. That is
# the difference between doc/GATING.md belonging to the guards and
# belonging to the prose.
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

# Every awk here reads its program from two files: the converter above
# and the pass itself. An inline program alongside -f is read as another
# input FILE, which awk fails to open and carries on from, so the pass
# yields nothing and the selection it was computing comes out empty.
cat > "$work/classify.awk" <<'AWK'
FILENAME == rules { n++; ra[n] = $1; rg[n] = globre($2); next }
{
    for (i = 1; i <= n; i++)
        if ($0 ~ rg[i]) { print ra[i] "\t" $0; next }
    print "everything\t" $0
}
AWK
awk -f "$work/glob.awk" -f "$work/classify.awk" \
    rules="$work/map.path" FS="$gate_tab" "$work/map.path" "$work/changed" \
    > "$work/classified"

cut -f1 "$work/classified" | sort -u > "$work/areas"
for a in $areas_named; do printf '%s\n' "$a" >> "$work/areas"; done
if [ "$batch" = yes ]; then printf 'everything\n' >> "$work/areas"; fi
# Every gate runs these whatever it was pointed at.
printf 'always\n' >> "$work/areas"
sort -u -o "$work/areas" "$work/areas"

# An area named that the table does not define selects nothing and would
# do it silently.
while read -r a; do
    if ! cut -f1 "$work/map.test" "$work/map.scope" | grep -qx "$a"; then
        echo "FAILURES: $map defines no area called $a"
        exit 1
    fi
done < "$work/areas"

# ---- a test a changed file is registered as
#
# A change to a test's own source runs that test, and which test that is
# comes from meson.build: the registration names the file, or names an
# executable built from it. Where neither reading finds anything -- a
# golden file a guard opens for itself, a register a check reads -- the
# file's own name is tried against the test names, and a file that
# answers to neither widens the gate rather than narrowing it.
awk '
function nch(s, c,   i, n) { n = 0
    for (i = length(s); i > 0; i--) if (substr(s, i, 1) == c) n++
    return n }
/^[[:space:]]*#/ { next }
{
    line = $0
    if (match(line, /^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*executable\(/)) {
        v = line; sub(/^[[:space:]]*/, "", v); sub(/[[:space:]]*=.*/, "", v)
        exevar = v; exebuf = ""
    }
    if (exevar != "") {
        exebuf = exebuf " " line
        if (nch(exebuf, "(") - nch(exebuf, ")") <= 0) {
            s = exebuf
            while (match(s, /'"'"'(tests|data)\/[^'"'"']*'"'"'/)) {
                f = substr(s, RSTART + 1, RLENGTH - 2)
                exefile[exevar] = exefile[exevar] " " f
                s = substr(s, RSTART + RLENGTH)
            }
            exevar = ""
        }
    }
    if (!collecting) { if (line !~ /^[[:space:]]*test\(/) next
                       collecting = 1; buf = "" }
    buf = buf " " line
    if (nch(buf, "(") - nch(buf, ")") > 0) next
    collecting = 0
    if (!match(buf, /test\([[:space:]]*'"'"'[^'"'"']*'"'"'/)) next
    nm = substr(buf, RSTART, RLENGTH); sub(/^test\([[:space:]]*'"'"'/, "", nm)
    sub(/'"'"'$/, "", nm)
    s = buf
    while (match(s, /'"'"'(tests|data)\/[^'"'"']*'"'"'/)) {
        print substr(s, RSTART + 1, RLENGTH - 2) "\t" nm
        s = substr(s, RSTART + RLENGTH)
    }
    s = buf
    while (match(s, /[A-Za-z_][A-Za-z0-9_]*_exe/)) {
        v = substr(s, RSTART, RLENGTH)
        if (v in exefile) { n = split(exefile[v], ff, " ")
            for (i = 1; i <= n; i++) if (ff[i] != "") print ff[i] "\t" nm }
        s = substr(s, RSTART + RLENGTH)
    }
}
' "$src/meson.build" | sort -u > "$work/registered"

: > "$work/derived"
: > "$work/unclaimed"
while read -r p; do
    case $p in tests/*|data/*) ;; *) continue ;; esac
    got=$(awk -F'\t' -v p="$p" '$1 == p { print $2 }' "$work/registered")
    if [ -z "$got" ]; then
        # the file's own name, with the suffixes a test source carries
        # taken off and underscores read as the hyphens a test name uses
        stem=$(basename "$p")
        stem=${stem%.sh}; stem=${stem%.ps}; stem=${stem%.c}
        stem=${stem%.golden}; stem=${stem%.register}; stem=${stem%.expected}
        stem=${stem%_test}; stem=${stem#check-}; stem=${stem#run-}
        stem=$(printf '%s\n' "$stem" | tr '_' '-')
        got=$(grep -x -- "$stem" "$work/tests.all" 2>/dev/null || true)
    fi
    if [ -n "$got" ]; then
        printf '%s\n' "$got" >> "$work/derived"
    elif awk -F'\t' -v p="$p" '$2 == p && $1 == "suite" { f = 1 }
                               END { exit !f }' "$work/classified"; then
        # Only where the table left the file to its registration in the
        # first place. Everywhere else the file has an area of its own
        # and the derivation is an addition to it, not the whole of it:
        # data/paint.ps answers to no test by name and is still the
        # graphics area, and widening there would undo the table.
        printf '%s\n' "$p" >> "$work/unclaimed"
    fi
done < "$work/changed"
sort -u -o "$work/derived" "$work/derived"

# A file the table left to its registration, and that no registration
# and no name answers to, is one this cannot place -- a golden a guard
# opens for itself, a register a check reads. The gate widens to
# everything rather than reporting on a selection that may not hold it.
if [ -s "$work/unclaimed" ]; then
    printf 'everything\n' >> "$work/areas"
    sort -u -o "$work/areas" "$work/areas"
fi

# ---- the areas' tests
cat > "$work/pick.awk" <<'AWK'
FILENAME == picked { want[$0] = 1; next }
FILENAME == rules { if ($1 in want) { n++; rg[n] = globre($2) } ; next }
{ for (i = 1; i <= n; i++) if ($0 ~ rg[i]) { print; next } }
AWK
awk -f "$work/glob.awk" -f "$work/pick.awk" \
    picked="$work/areas" rules="$work/map.test" \
    "$work/areas" FS="$gate_tab" "$work/map.test" "$work/tests.all" \
    > "$work/sel.narrow.raw"
cat "$work/derived" >> "$work/sel.narrow.raw"

# An area saying the whole suite answers for it takes the selection to
# everything. It is said apart from the test rules so that an area can
# both name its own tests and be one of these.
while read -r a; do
    if awk -F'\t' -v a="$a" '$1 == a && $2 == "all" { f = 1 }
                             END { exit !f }' "$work/map.scope"; then
        cat "$work/tests.all" >> "$work/sel.narrow.raw"
    fi
done < "$work/areas"

sort -u "$work/sel.narrow.raw" | comm -12 - "$work/tests.all" > "$work/sel.narrow"

# ---- the wide leg
#
# The whole selection where the change reaches the layer the width is a
# property of; the tests that read the width otherwise. Never nothing.
widepolicy=witness
while read -r a; do
    if awk -F'\t' -v a="$a" '$1 == a && $2 == "full" { found = 1 }
                             END { exit !found }' "$work/map.width"; then
        widepolicy=full
    fi
done < "$work/areas"

if [ "$widepolicy" = full ]; then
    cp "$work/sel.narrow" "$work/sel.wide"
else
    printf 'width\n' > "$work/areas.width"
    awk -f "$work/glob.awk" -f "$work/pick.awk" \
        picked="$work/areas.width" rules="$work/map.test" \
        "$work/areas.width" FS="$gate_tab" "$work/map.test" \
        "$work/tests.all" | sort -u > "$work/sel.wide"
fi

if [ ! -s "$work/sel.narrow" ] || [ ! -s "$work/sel.wide" ]; then
    echo "FAILURES: the selection is empty, which is a gate that asks"
    echo "      nothing. Areas: $(tr '\n' ' ' < "$work/areas")"
    exit 1
fi

# ---- say what this is
nall=$(wc -l < "$work/tests.all" | tr -d ' ')
nnarrow=$(wc -l < "$work/sel.narrow" | tr -d ' ')
nwide=$(wc -l < "$work/sel.wide" | tr -d ' ')
echo "gate: $(grep -c . "$work/changed") path(s) from $origin"
echo "gate: areas $(tr '\n' ' ' < "$work/areas")"
if [ -s "$work/unclaimed" ]; then
    echo "gate: no test answers for these, so the gate is the whole suite:"
    sed 's/^/      /' "$work/unclaimed"
fi
echo "gate: narrow $narrow -- $nnarrow of $nall tests"
if [ "$widepolicy" = full ]; then
    echo "gate: wide   $wide -- $nwide of $nall tests, the whole selection:"
    echo "      the change reaches the layer the object width is a property of"
else
    echo "gate: wide   $wide -- $nwide of $nall tests, the ones that read the"
    echo "      width. A tripwire, not a wide run of the change's own tests;"
    echo "      tests/gate.sh --batch is that"
fi

if [ "$listonly" = yes ]; then
    echo "--- narrow"
    sed 's/^/  /' "$work/sel.narrow"
    echo "--- wide"
    sed 's/^/  /' "$work/sel.wide"
    exit 0
fi

# ---- run a leg, and read back what it really did
#
# meson counts a skip apart from a pass and then reports no failures
# either way, and a run that fell over partway leaves a record shorter
# than the selection. Both read as green from the summary line, so the
# record is cleared first, read after, and held to the names asked for.
#
# A record names a test as its suites, its project and the name itself; a
# selection names it as the build lists it. The name is read off the
# record's layout by tests/listing.awk, so the two are the same thing
# before they are compared.
status=0
cat > "$work/ran.awk" <<'AWK'
{
    name = ""; res = ""
    if (match($0, /^\{"name": "[^"]*"/))
        name = substr($0, RSTART + 10, RLENGTH - 11)
    if (match($0, /, "result": "[A-Z]+"/)) {
        res = substr($0, RSTART, RLENGTH)
        sub(/^, "result": "/, "", res); sub(/"$/, "", res)
    }
    if (name != "" && res != "")
        print listing_name(name) "\t" res
}
AWK
run_leg() {
    leg=$1; build=$2; sel=$3
    logdir="$build/meson-logs"
    for old in "$logdir"/testlog*.json; do
        [ -f "$old" ] && rm -f "$old"
    done
    # shellcheck disable=SC2046
    meson test -C "$build" --num-processes "$jobs" $(cat "$sel")
    legstatus=$?

    record=''
    for left in "$logdir"/testlog*.json; do
        [ -f "$left" ] || continue
        if [ -n "$record" ]; then
            echo "FAILURES: the $leg leg left more than one record in $logdir"
            return 1
        fi
        record=$left
    done
    if [ -z "$record" ]; then
        echo "FAILURES: the $leg leg ran nothing -- meson wrote no record"
        return 1
    fi
    awk -f "$listing" -f "$work/ran.awk" "$record" | sort > "$work/ran.$leg"

    cut -f1 "$work/ran.$leg" | sort -u > "$work/ranames.$leg"
    if ! cmp -s "$sel" "$work/ranames.$leg"; then
        echo "FAILURES: the $leg leg selected $(wc -l < "$sel" | tr -d ' ') test(s) and its record"
        echo "      holds $(wc -l < "$work/ranames.$leg" | tr -d ' '). A run reporting on a fraction of"
        echo "      what it selected agrees with whatever the rest would say."
        comm -23 "$sel" "$work/ranames.$leg" | head -5 | sed 's/^/      unrun: /'
        comm -13 "$sel" "$work/ranames.$leg" | head -5 | sed 's/^/      extra: /'
        return 1
    fi
    nskip=$(awk -F'\t' '$2 == "SKIP"' "$work/ran.$leg" | grep -c . || true)
    if [ "$nskip" -ne 0 ]; then
        echo "gate: $leg -- $nskip of $(wc -l < "$sel" | tr -d ' ') tests did not run:"
        awk -F'\t' '$2 == "SKIP" { print "      " $1 }' "$work/ran.$leg"
    fi
    return $legstatus
}

echo "gate: --- narrow leg"
run_leg narrow "$narrow" "$work/sel.narrow" || status=1
echo "gate: --- wide leg"
run_leg wide "$wide" "$work/sel.wide" || status=1

if [ "$status" -eq 0 ]; then
    if [ "$widepolicy" = full ]; then
        echo "gate: green -- $nnarrow narrow, $nwide wide"
    else
        echo "gate: green -- $nnarrow narrow, $nwide wide. This does not"
        echo "      speak for the wide build over the rest of the selection;"
        echo "      tests/gate.sh --batch does, and is what a branch is held to"
    fi
else
    echo "FAILURES: the gate is red"
fi
exit $status
