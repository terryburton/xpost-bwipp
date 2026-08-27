#!/bin/sh
# Guard: a counter whose numbers name something says what it gives up
# when it runs out.
#
# Caches here are asked their question by comparing a key, and part of
# several of those keys is a number some counter handed out. Such a cache
# is exactly as trustworthy as the promise that no number is handed out
# twice, and there are two ways to break that promise. A counter kept in
# virtual memory is wound back by restore while the entries it named are
# not, so the next value it hands out is one the cache has already filed
# something under. And a counter of any width runs out, and when it
# starts again everything it named before is indistinguishable from
# everything it is about to name.
#
# The first is answered by where a counter lives: a file-scope static in
# C is not virtual memory and no restore reaches it. That is why the
# population read here is the C statics rather than anything in the
# interpreted tree. The second cannot be answered by where it lives, only
# by what is done when it happens, and that is what tests/counters.golden
# holds each counter to saying.
#
# What is checked, per disposition, is written out in that file. The
# short of it: a counter that mints must, in the function that advances
# it, both notice that it has run out and give up what the old numbers
# named -- and the giving up must be a function this tree has, not a
# note. A counter that names nothing says which kind of number it is, so
# that a later reader does not have to work it out again, and so that a
# counter which STARTS naming something is a line that has to change.
#
# The population is derived from the source, not listed, and derived by
# the mechanism rather than the spelling: a static that is advanced by
# ++, --, += or -= is a counter whatever it is called. Deriving on names
# would have found the counters someone had already thought about, which
# are precisely the ones that are already right.
#
# The C is read through guard_c_source, so a mention in a comment or
# inside a string literal answers nothing.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-counter-register.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
golden="$src/tests/counters.golden"
guard_require_file "$golden" "the register of counters"

guard_workdir
guard_mirror_tree "$src"
tree=$mirror

( cd "$tree" && guard_c_source src/lib/*.c ) > "$work/code" 2>/dev/null
if [ ! -s "$work/code" ]; then
    echo "FAILURES: no library source could be read from $src/src/lib"
    exit 1
fi

# ---- the population ----
#
# Every file-scope static whose declaration carries no * and whose type
# is integral. A declaration is joined from the line its static begins on
# to the semicolon that ends it, so an initialiser spanning lines reads
# as one; a static that reaches an opening brace with no assignment
# before it is a function definition and is read past.
awk -F'\t' '
{ f = $1; ln = $2; c = $3 }
f != pf { pf = f; depth = 0; acc = "" }
{
    if (acc == "") {
        if (depth == 0 && c ~ /^[ \t]*static([ \t]|$)/) { acc = c; aln = ln }
    } else acc = acc " " c

    if (acc != "") {
        e = decl_end(acc)
        if (e > 0) { emit(f, aln, substr(acc, 1, e - 1)); acc = "" }
        else if (fn_body(acc) || length(acc) > 4000) acc = ""
    }

    n = gsub(/{/, "{", c); m = gsub(/}/, "}", c)
    depth += n - m; if (depth < 0) depth = 0
}

function decl_end(t,   i, d, c) {
    d = 0
    for (i = 1; i <= length(t); i++) {
        c = substr(t, i, 1)
        if (c == "{") d++
        else if (c == "}") d--
        else if (c == ";" && d <= 0) return i
    }
    return 0
}

function fn_body(t,   b, head) {
    b = index(t, "{")
    if (b == 0) return 0
    head = substr(t, 1, b - 1)
    return (head ~ /\(/ && head !~ /=/)
}

function emit(file, line, head,   outer, decls, i, k, nm, rest) {
    if (head ~ /\*/) return                    # pointers are another register
    outer = head
    while (outer ~ /{[^{}]*}/) gsub(/{[^{}]*}/, " ", outer)
    if (outer ~ /\(/) return                   # a prototype, not a variable
    if (outer ~ /^[ \t]*static[ \t]+const[ \t]/) return
    if (outer !~ /(int|long|short|unsigned|size_t|word)/) return
    rest = outer
    sub(/^[ \t]*static[ \t]+/, "", rest)
    k = split(rest, decls, ",")
    for (i = 1; i <= k; i++) {
        nm = decls[i]
        sub(/=.*/, "", nm)
        if (nm ~ /\[/) continue                # an array is not a counter
        gsub(/[^A-Za-z0-9_]+$/, "", nm)
        if (nm ~ /[^A-Za-z0-9_ \t]/) continue
        sub(/^.*[^A-Za-z0-9_]/, "", nm)
        if (nm == "" || nm ~ /^[0-9]/) continue
        print file "\t" nm "\t" line
    }
}
' "$work/code" | sort -u > "$work/scalars"

# ---- the code, attributed to the function it sits in ----
#
# So that "advances it" and "notices it has run out" can be asked of the
# same function rather than of the file, which would let a wrap test
# somewhere else answer for a counter that has none.
awk -F'\t' '
{ f = $1; c = $3 }
f != pf { pf = f; depth = 0; head = ""; cur = "" }
{
    n = length(c)
    for (i = 1; i <= n; i++) {
        ch = substr(c, i, 1)
        if (depth == 0) {
            if (ch == "{") {
                if (head ~ /\(/ && head !~ /=[^=]*$/) {
                    nm = head
                    sub(/\(.*/, "", nm)
                    gsub(/[ \t]+$/, "", nm)
                    sub(/^.*[^A-Za-z0-9_]/, "", nm)
                    cur = nm
                } else cur = ""
                head = ""
                depth = 1
            } else if (ch == ";") head = ""
            else head = head ch
        } else {
            if (ch == "{") depth++
            else if (ch == "}") { depth--; if (depth == 0) cur = "" }
        }
    }
    # a return type on its own line is still part of the head, and would
    # otherwise run into the name that follows it
    if (depth == 0) head = head " "
    if (cur != "") print f "\t" cur "\t" c
}
' "$work/code" > "$work/bodies"

# a counter is one that is advanced somewhere in its own file
: > "$work/population"
while IFS="$guard_tab" read -r f n l; do
    if awk -F'\t' -v F="$f" -v N="$n" '
        $1 == F {
            t = $3
            if (t ~ ("[^A-Za-z0-9_]" N "[ \t]*(\\+\\+|--|\\+=|-=)")) { found = 1; exit }
            if (t ~ ("(\\+\\+|--)[ \t]*" N "[^A-Za-z0-9_]")) { found = 1; exit }
        }
        END { exit !found }' "$work/code"
    then
        printf '%s\t%s\t%s\n' "$f" "$n" "$l" >> "$work/population"
    fi
done < "$work/scalars"

npop=$(grep -c . < "$work/population" || true)
if [ "$npop" -lt 8 ]; then
    echo "FAILURES: only $npop counters were read out of src/lib, which is fewer"
    echo "      than this tree can have; the reading is broken rather than the tree"
    exit 1
fi

# the functions this tree defines, so a register naming one it does not
# have fails rather than reading as a promise
cut -f2 "$work/bodies" | sort -u > "$work/functions"

# ---- the register ----
#
# A line beginning with whitespace continues the line before it, so a
# reason may be written as prose across several lines.
sed 's/\r$//' "$golden" \
  | awk '/^#/ { next }
         /^[ \t]*$/ { if (acc != "") { print acc; acc = "" } next }
         /^[ \t]/ { acc = acc " " $0; next }
         { if (acc != "") print acc; acc = $0 }
         END { if (acc != "") print acc }' \
  | sed 's/[[:blank:]][[:blank:]]*/ /g; s/^ //; s/ $//' > "$work/rows"

grep '^obligation ' "$work/rows" > "$work/obligations" || true
grep -v '^obligation ' "$work/rows" | grep . > "$work/recorded" || true

fail=0
: > "$work/problems"

# every counter the tree has is registered, and says something checkable
: > "$work/registered-keys"
while IFS="$guard_tab" read -r file name line; do
    row=$(awk -v f="$file" -v n="$name" \
              '$1 == f && $2 == n { print; exit }' "$work/recorded")
    if [ -z "$row" ]; then
        printf '%s:%s  %s  is advanced in its own file and the register does not\n' \
            "$file" "$line" "$name" >> "$work/problems"
        printf '        say whether its numbers name anything\n' >> "$work/problems"
        continue
    fi
    printf '%s %s\n' "$file" "$name" >> "$work/registered-keys"
    disp=$(printf '%s\n' "$row" | cut -d' ' -f3)
    detail=$(printf '%s\n' "$row" | cut -d' ' -f4-)

    case $disp in
    mints|mints-signalled|refuses)
        # the function that advances it must notice that it has run out
        adv=$(awk -F'\t' -v F="$file" -v N="$name" '
            $1 == F && ($3 ~ ("[^A-Za-z0-9_]" N "[ \t]*(\\+\\+|--|\\+=|-=)") \
                     || $3 ~ ("(\\+\\+|--)[ \t]*" N "[^A-Za-z0-9_]")) { print $2 }' \
            "$work/bodies" | sort -u)
        [ -n "$adv" ] || adv='(none)'
        noticed=""
        for fn in $adv; do
            if awk -F'\t' -v F="$file" -v FN="$fn" -v N="$name" '
                $1 == F && $2 == FN &&
                $3 ~ ("[^A-Za-z0-9_]" N "[ \t]*(<=|<|>=|>|==|!=)") { found = 1 }
                END { exit !found }' "$work/bodies"
            then noticed="$noticed $fn"; fi
        done
        if [ -z "$noticed" ]; then
            printf '%s  %s  mints cache keys and nothing where it is advanced (%s)\n' \
                "$file" "$name" "$(echo $adv)" >> "$work/problems"
            printf '        tests whether it has run out, so the numbers start again\n' \
                >> "$work/problems"
            printf '        silently and the cache cannot tell the reuse from the use\n' \
                >> "$work/problems"
        fi
        ;;
    esac

    case $disp in
    mints)
        fn=$(printf '%s\n' "$detail" | cut -d' ' -f1)
        if [ -z "$fn" ]; then
            printf '%s  %s  is registered as minting and names nothing that gives\n' \
                "$file" "$name" >> "$work/problems"
            printf '        up what the old numbers named\n' >> "$work/problems"
        elif ! grep -qx "$fn" "$work/functions"; then
            printf '%s  %s  gives up what it named through %s(), which this tree\n' \
                "$file" "$name" "$fn" >> "$work/problems"
            printf '        does not define\n' >> "$work/problems"
        else
            called=""
            for a in $noticed; do
                if awk -F'\t' -v F="$file" -v FN="$a" -v G="$fn" '
                    $1 == F && $2 == FN &&
                    $3 ~ ("[^A-Za-z0-9_]?" G "[ \t]*\\(") { found = 1 }
                    END { exit !found }' "$work/bodies"
                then called=1; fi
            done
            if [ -z "$called" ]; then
                printf '%s  %s  notices that it has run out and does not call %s()\n' \
                    "$file" "$name" "$fn" >> "$work/problems"
                printf '        there, so the numbers restart over entries still held\n' \
                    >> "$work/problems"
            fi
        fi
        ;;
    mints-signalled|refuses|decides|counts)
        if [ -z "$detail" ]; then
            printf '%s  %s  is registered as %s with no reason written down\n' \
                "$file" "$name" "$disp" >> "$work/problems"
        fi
        ;;
    *)
        printf '%s  %s  has the unknown disposition %s\n' \
            "$file" "$name" "$disp" >> "$work/problems"
        ;;
    esac
done < "$work/population"

# a register line for a counter this tree no longer has
awk '{ print $1 " " $2 }' "$work/recorded" | sort -u > "$work/recorded-keys"
sort -u "$work/registered-keys" > "$work/live-keys"
comm -23 "$work/recorded-keys" "$work/live-keys" > "$work/stale"

# ---- the obligations ----
while read -r kw kind a b c d; do
    [ "$kw" = obligation ] || continue
    case $kind in
    colocated)
        # a and b are names in file $a, and every access of either must
        # name the dictionary $b, so one restore reaches both
        for nm in "$c" "$d"; do
            [ -n "$nm" ] || continue
            bad=$(awk -v N="$nm" -v D="$b" -v SHOW="$a" '
                index($0, N) == 0 { next }
                # the exact name: what follows it must not continue it
                { p = index($0, N)
                  nxt = substr($0, p + length(N), 1)
                  if (nxt ~ /[A-Za-z0-9_]/) next
                  if (index($0, D) == 0) print SHOW ":" FNR ": " $0 }' \
                "$tree/$a" 2>/dev/null | head -3)
            if [ -n "$bad" ]; then
                printf '%s  %s is reached without %s, so a restore can take one\n' \
                    "$a" "$nm" "$b" >> "$work/problems"
                printf '        of the pair and leave the other:\n' >> "$work/problems"
                printf '%s\n' "$bad" | sed 's/^/          /' >> "$work/problems"
            fi
            if ! grep -q "$b" "$tree/$a" 2>/dev/null; then
                printf '%s  names no %s at all, so this obligation is answering\n' \
                    "$a" "$b" >> "$work/problems"
                printf '        for something that has moved\n' >> "$work/problems"
            fi
        done
        ;;
    detects)
        # the counter that restarts below the last value it handed out
        # says so to a reader elsewhere, and this is that reader: the
        # remembered value and the giving up must sit together, or the
        # signal is sent to nobody
        near=$(awk -v R="$b" -v D="$c" '
            index($0, R) { r = FNR }
            index($0, D) && r && FNR - r <= 2 { found = 1 }
            END { print found + 0 }' "$tree/$a" 2>/dev/null)
        if [ "${near:-0}" = 0 ]; then
            printf '%s  reads %s and does not give the cache up through %s\n' \
                "$a" "$b" "$c" >> "$work/problems"
            printf '        beside it, so a counter that has restarted is noticed\n' \
                >> "$work/problems"
            printf '        by nothing and the old entries answer the new numbers\n' \
                >> "$work/problems"
        fi
        ;;
    releases)
        # within $b's body in $a, an entry's key field $c is compared,
        # which is the cache being walked for what is about to go
        if ! grep -qx "$b" "$work/functions"; then
            printf '%s  does not define %s(), so the release this obligation\n' \
                "$a" "$b" >> "$work/problems"
            printf '        rests on is not there\n' >> "$work/problems"
        elif ! awk -F'\t' -v F="$a" -v FN="$b" -v K="$c" '
                $1 == F && $2 == FN && $3 ~ (K "[ \t]*==") { found = 1 }
                END { exit !found }' "$work/bodies"
        then
            printf '%s  %s() releases what a cache keyed on %s names and does\n' \
                "$a" "$b" "$c" >> "$work/problems"
            printf '        not give up the entries naming it, so a reissued\n' \
                >> "$work/problems"
            printf '        address is answered by the entries of the last one\n' \
                >> "$work/problems"
        fi
        ;;
    *)
        printf 'the register carries the unknown obligation %s\n' "$kind" \
            >> "$work/problems"
        ;;
    esac
done < "$work/obligations"

nobl=$(grep -c . < "$work/obligations" || true)
if [ "$nobl" -lt 1 ]; then
    echo "FAILURES: the register records no obligations at all, and the two the"
    echo "      counters rest on have nowhere else to be written down"
    exit 1
fi

if [ -s "$work/problems" ]; then
    echo "FAILURES: a counter whose numbers can name something is not held to"
    echo "      saying what happens when it runs out:"
    sed 's/^/      /' "$work/problems"
    fail=1
fi
if [ -s "$work/stale" ]; then
    echo "FAILURES: the register names a counter this tree does not advance, so"
    echo "      it is answering for something that is gone:"
    sed 's/^/      /' "$work/stale"
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1

nmint=$(awk '$3 == "mints" || $3 == "mints-signalled"' "$work/recorded" | grep -c . || true)
ndec=$(awk '$3 == "decides"' "$work/recorded" | grep -c . || true)
ncount=$(awk '$3 == "counts"' "$work/recorded" | grep -c . || true)
printf 'SUCCESS (%s counters: %s mint cache keys, %s decides what a cache holds,' \
    "$npop" "$nmint" "$ndec"
printf ' %s name nothing; %s obligations)\n' "$ncount" "$nobl"
