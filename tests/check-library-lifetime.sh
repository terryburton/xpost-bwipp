#!/bin/sh
# Guard: nothing a module keeps for the process outlives the library
# without having said so.
#
# xpost_init and xpost_quit are counted, and the balancing quit takes the
# library down: the font module gives its library back, the log gives its
# file back, and what those owned is freed. A process may then bring the
# library up again -- an embedder serving work in bursts does exactly
# that -- and the second lifetime is meant to start where the first one
# did.
#
# What can stop it is a module-level variable. The library's lifetime is
# counted but nothing stands for it, so state that wants to last a
# lifetime has nowhere to live but a file-scope static, and a static
# lasts the process. Where what it holds was given back at the teardown,
# the next lifetime is handed something that has been freed -- not a
# stale value to be noticed later, but a pointer followed immediately by
# whatever asks it a question.
#
# So each such variable is held to saying which it is. The register at
# tests/library_statics.golden gives every one of them a fate:
#
#   outlives <reason>     What it holds is not the library's to give
#                         back, so a second lifetime finding it there is
#                         right. A string literal is program image; a
#                         latch documented as belonging to the process is
#                         the process's.
#
#   cleared <function>    It is given up at the teardown, by that
#                         function, which the teardown must reach --
#                         either because xpost_quit calls it, or because
#                         it was handed to xpost_at_quit, which is how a
#                         module asks to be called by registering on the
#                         path that acquires what it will give back.
#                         Checked here, not asserted, so a release that
#                         stops being reached fails rather than going
#                         quiet.
#
#   reset <function>      It is given up as the next lifetime starts
#                         rather than as the last one ends, by that
#                         function, which must be reached from an
#                         operator installation or from context
#                         initialisation. Nothing carries across, which
#                         is what this guard is about; what is held
#                         between the last context and the next is a
#                         residue this does not speak for.
#
# The population is derived, not listed: every file-scope static in
# src/lib whose declaration can be made to name something. There are two
# ways to name something here and the population takes both.
#
# A declaration carrying a * names storage by its address, unless it is a
# const object or a pointer that is itself const -- neither of those can
# be made to name anything else, so neither can carry a lifetime across.
#
# A declaration of an Xpost_Object names storage by ENTITY NUMBER, which
# is how a composite says where its storage is: an index into the memory
# table rather than a place in the process. It carries no *, and reading
# the population off the star alone would miss every one of them -- which
# it did, while the tree moved from addresses to entity numbers underneath
# it.
#
# A static holding only scalars or a plain char buffer names nothing and
# is not in the population.
#
# The C is read through guard_c_source, so a mention in a comment or
# inside a string literal answers nothing.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-library-lifetime.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
golden="$src/tests/library_statics.golden"
guard_require_file "$golden" "the register of library-lifetime statics"

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
# A declaration is joined from the line its static begins on to the
# semicolon that ends it, so that an initialiser spanning lines reads as
# one. A static that reaches an opening brace before any semicolon, with
# no assignment before it, is a function definition and is read past.
awk -F'\t' '
{ f = $1; ln = $2; c = $3 }
f != pf { pf = f; depth = 0; acc = "" }
{
    if (acc == "") {
        if (depth == 0 && c ~ /^[ \t]*static[ \t]/) { acc = c; aln = ln }
    } else acc = acc " " c

    if (acc != "") {
        # the semicolon that ends the declaration is the one outside any
        # brace group -- a struct written out in place has semicolons
        # between its members, and stopping at the first of those would
        # read the name of a member for the name of the variable
        e = decl_end(acc)
        if (e > 0) {
            emit(f, aln, substr(acc, 1, e - 1))
            acc = ""
        } else if (fn_body(acc) || length(acc) > 4000) acc = ""
    }

    n = gsub(/{/, "{", c); m = gsub(/}/, "}", c)
    depth += n - m; if (depth < 0) depth = 0
}

# where a declaration ends: the first semicolon at brace depth zero,
# or 0 while the accumulation has not reached one
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

# a function definition rather than a variable: it reaches an opening
# brace, and what stands before that brace is a parameter list rather
# than an initialiser
function fn_body(t,   b, head) {
    b = index(t, "{")
    if (b == 0) return 0
    head = substr(t, 1, b - 1)
    return (head ~ /\(/ && head !~ /=/)
}

function emit(file, line, head,   outer, decls, i, k, nm, rest) {
    if (head !~ /\*/ && head !~ /(^|[^A-Za-z0-9_])Xpost_Object([^A-Za-z0-9_]|$)/)
        return
    # what is written outside any brace group: the object being declared,
    # rather than the members of a struct it is one of
    outer = head
    while (outer ~ /{[^{}]*}/) gsub(/{[^{}]*}/, " ", outer)
    if (outer ~ /\*[ \t]*const[ \t]/) return          # the pointer is const
    if (outer !~ /\*/ && outer ~ /^[ \t]*static[ \t]+const[ \t]/) return  # a const object
    if (outer ~ /\(/ && outer !~ /\(\*/) return       # a prototype, not a variable

    # a pointer to a function: the name is inside the parentheses the
    # star is in, and the parameter list after it would otherwise be read
    # for part of the declarator and the whole line dropped
    if (outer ~ /\([ \t]*\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\)[ \t]*\(/) {
        nm = outer
        sub(/^.*\([ \t]*\*[ \t]*/, "", nm)
        sub(/[ \t]*\).*$/, "", nm)
        if (nm != "" && nm !~ /[^A-Za-z0-9_]/)
            print file "\t" nm "\t" line
        return
    }

    # the name of each declarator: strip the initialiser, split on the
    # commas that separate them, and take the identifier each ends with
    rest = outer
    sub(/^[ \t]*static[ \t]+/, "", rest)
    k = split(rest, decls, ",")
    for (i = 1; i <= k; i++) {
        nm = decls[i]
        sub(/=.*/, "", nm)
        sub(/\[.*/, "", nm)
        gsub(/[^A-Za-z0-9_]+$/, "", nm)
        if (nm ~ /[^A-Za-z0-9_ \t*]/) continue
        sub(/^.*[^A-Za-z0-9_]/, "", nm)
        if (nm == "" || nm ~ /^[0-9]/) continue
        print file "\t" nm "\t" line
    }
}
' "$work/code" | sort -u > "$work/population"

npop=$(grep -c . < "$work/population" || true)
if [ "$npop" -lt 10 ]; then
    echo "FAILURES: only $npop library statics were read, which is fewer than"
    echo "      this tree can have; the reading is broken rather than the tree"
    exit 1
fi

# ---- what xpost_quit reaches ----
#
# The functions xpost_quit calls, and the functions those call: a
# clearing function named in the register has to be one of them, which is
# what makes the register's promise a fact rather than a note.
reached() {         # <function> -> the names it calls, one per line
    awk -F'\t' -v FN="$1" '
    { L[++n] = $2; T[n] = $3 }
    END {
        for (i = 1; i <= n && !bs; i++) {
            if (T[i] !~ /^[A-Za-z_]/) continue
            if ((" " T[i]) !~ ("[^A-Za-z0-9_]" FN "[ \t]*\\(")) continue
            S = ""; depth = 0; seen = 0
            for (j = i; j <= n; j++) {
                m = length(T[j])
                for (k = 1; k <= m; k++) {
                    c = substr(T[j], k, 1)
                    S = S c
                    if (c == "{") { if (!seen) { seen = 1; bs = length(S) + 1 } depth++ }
                    else if (c == "}") { depth--; if (seen && depth == 0) { be = length(S) - 1; break } }
                    else if (c == ";" && !seen) { S = ""; seen = 0; break }
                }
                if (be) break
                S = S " "
            }
            if (!be) { bs = 0 }
        }
        if (!bs) exit
        for (p = bs; p <= be; p++) {
            if (substr(S, p, 1) != "(") continue
            q = p - 1
            while (q >= bs && substr(S, q, 1) ~ /[ \t]/) q--
            e = q
            while (q >= bs && substr(S, q, 1) ~ /[A-Za-z0-9_]/) q--
            if (e > q) print substr(S, q + 1, e - q)
        }
    }' "$work/code"
}

reached xpost_quit | sort -u > "$work/reached"
if [ ! -s "$work/reached" ]; then
    echo "FAILURES: xpost_quit could not be read, so what it reaches is unknown"
    exit 1
fi
# What is handed to xpost_at_quit is run by the teardown as surely as
# what it calls: a module registers on the path that takes the thing it
# will have to give back, rather than being written into a list here.
awk -F'\t' '{
        t = $3
        while (match(t, /xpost_at_quit[ \t]*\(/)) {
            t = substr(t, RSTART + RLENGTH)
            a = t
            sub(/[,)].*/, "", a)
            gsub(/[^A-Za-z0-9_]/, "", a)
            if (a != "") print a
        }
    }' "$work/code" | sort -u > "$work/registered"

# what the teardown calls directly, plus what it was asked to call, and
# then one level further: a module teardown gives up through helpers of
# its own. xpost_quit itself counts, for a module small enough to clear
# inline.
{ echo xpost_quit; cat "$work/reached" "$work/registered"; } \
    | sort -u > "$work/teardown1"
while read -r fn; do
    reached "$fn"
done < "$work/teardown1" | sort -u > "$work/teardown2"
cat "$work/teardown1" "$work/teardown2" | sort -u > "$work/teardown"

# and what a lifetime starting reaches, for the state given up there
awk -F'\t' '$3 ~ /^(int|void)?[ \t]*xpost_(oper_init_[a-z0-9_]+|context_init|interpreter_init)[ \t]*\(/ {
        t = $3; sub(/[ \t]*\(.*/, "", t); sub(/^.*[^A-Za-z0-9_]/, "", t); print t
    }' "$work/code" | sort -u > "$work/starts"
# A lifetime starting reaches the functions its roots call AND the roots
# themselves -- an init that sets a static directly is as much a place the
# state is given up as one it calls to do it. The teardown half unions its
# roots the same way; without it here, a fate naming an init outright is
# refused for not being reached by the thing it is.
: > "$work/setup1"
while read -r fn; do
    reached "$fn"
done < "$work/starts" | sort -u > "$work/setup1"
cat "$work/starts" "$work/setup1" | sort -u > "$work/setup"

# ---- the register ----
grep -vE '^[[:space:]]*(#|$)' "$golden" | tr -d '\r' \
  | awk 'NF >= 3 { print $1 "\t" $2 "\t" $3 "\t" $4 }' | sort -u > "$work/recorded"

fail=0
: > "$work/unregistered"
: > "$work/unreached"
while IFS="$guard_tab" read -r file name line; do
    row=$(awk -F'\t' -v f="$file" -v n="$name" \
              '$1 == f && $2 == n { print; exit }' "$work/recorded")
    if [ -z "$row" ]; then
        printf '%s:%s  %s\n' "$file" "$line" "$name" >> "$work/unregistered"
        continue
    fi
    fate=$(printf '%s\n' "$row" | cut -f3)
    case $fate in
        cleared) where=$work/teardown; what='xpost_quit does not reach' ;;
        reset)   where=$work/setup;    what='no lifetime starting reaches' ;;
        outlives) continue ;;
        *) printf '%s:%s  %s  has the unknown fate %s\n' \
               "$file" "$line" "$name" "$fate" >> "$work/unreached"; continue ;;
    esac
    fn=$(printf '%s\n' "$row" | cut -f4)
    if [ -z "$fn" ]; then
        printf '%s:%s  %s  is %s by nothing this can name\n' \
            "$file" "$line" "$name" "$fate" >> "$work/unreached"
    elif ! grep -qx "$fn" "$where"; then
        printf '%s:%s  %s  %s by %s(), which %s\n' \
            "$file" "$line" "$name" "$fate" "$fn" "$what" >> "$work/unreached"
    fi
done < "$work/population"

# a register line for something that is no longer there
cut -f1,2 "$work/recorded" | sort -u > "$work/recorded-keys"
cut -f1,2 "$work/population" | sort -u > "$work/population-keys"
comm -23 "$work/recorded-keys" "$work/population-keys" > "$work/stale"

if [ -s "$work/unregistered" ]; then
    echo "FAILURES: a library static can be made to name something and the"
    echo "      register does not say what becomes of it when the library"
    echo "      goes down:"
    sed 's/^/      /' "$work/unregistered"
    echo "      Add it to tests/library_statics.golden in this commit, as"
    echo "      'outlives <reason>' or 'cleared <function>'."
    fail=1
fi
if [ -s "$work/unreached" ]; then
    echo "FAILURES: a library static is registered as cleared at the teardown"
    echo "      and the teardown does not reach what clears it:"
    sed 's/^/      /' "$work/unreached"
    fail=1
fi
if [ -s "$work/stale" ]; then
    echo "FAILURES: the register names a static this tree does not have, so it"
    echo "      is answering for something that is gone:"
    sed 's/^/      /' "$work/stale"
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1

ncleared=$(awk -F'\t' '$3 == "cleared"' "$work/recorded" | grep -c . || true)
nreset=$(awk -F'\t' '$3 == "reset"' "$work/recorded" | grep -c . || true)
noutlives=$(awk -F'\t' '$3 == "outlives"' "$work/recorded" | grep -c . || true)
echo "SUCCESS ($npop library statics: $ncleared cleared at the teardown," \
     "$nreset reset as the next lifetime starts, $noutlives outlive it)"
