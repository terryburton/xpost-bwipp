#!/bin/sh
# Guard how a private helper is frozen and how it is called.
#
# Two invariants, both about the same thing -- what a name in a helper
# body resolves to once the interpreter is sealed.
#
# A helper is never bound where it is defined -- and never, whichever way
# the definition closes: "} bind put", "end } bind put" after a dictionary,
# and a bare "}" with "bind put" on the line after are one thing to the
# interpreter, so they are one thing here.
#
# A definition nested inside a block is not asked. // resolves as the file
# is READ and bind resolves when the enclosing block RUNS, so a helper
# defined inside something that runs later -- when graphics has loaded, say
# -- can only have bind: the name // would splice does not exist yet. The
# anchor below is what leaves those alone, and that is the reason for it.
#
# A body needing the reference to something the lockdown takes away asks
# for that one reference with //, which splices the object and leaves every
# other name alone. Binding the whole body to keep one name is what this
# forbids: it freezes them all, and makes the body read-only so .bindscope
# cannot come back for the ones bind could not substitute yet.
#
# A helper is never bound where it is defined. bind substitutes an
# operator only where the name already answers with one, and the standard
# operators written in PostScript do not answer with one until .finalize
# promotes them. A body bound at its definition is therefore frozen with
# those names still names, and bind makes what it binds read-only, so the
# lockdown sweep cannot come back for it: the names stay dynamic for the
# whole run, which is a program's redefinition waiting to be picked up
# from inside the machinery. .finalize binds .xpostsys once the promotion
# is done, and that is the only point at which binding substitutes
# anything. init.ps states this beside the convention it belongs to.
#
# The counterpart for the other half of the tree is a runtime one:
# .defopin records any operator body that arrives read-only, and
# .finalize holds that register to empty. Helper bodies go through a
# plain put with nothing to intercept them, so they are held here from
# the source instead.
#
# A helper a test redefines is reached by name and not baked in. A test
# that appends
#     .xpostsys /.h { ... } put
# to a copy of the data tree is redefining the dictionary entry. A call
# baked with // holds the procedure the file defined and never looks at
# the entry again, so the redefinition is inert and the test goes on
# passing while reading nothing. Reaching the helper by name is what
# keeps such a probe live.
#
# An operator the C takes away is reached with // and never by name. The C
# undefines a few operators from systemdict at lockdown so a program
# cannot name them afterwards; a // resolves one while the name still
# answers and puts the operator itself in the body, which is the reference
# that survives, where a run-time lookup would find nothing. Which
# operators those are is read out of the C rather than listed here, so a
# name added there is followed rather than written down twice.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-helper-bind.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
data=$src/data
tests=$src/tests
fail=0

guard_workdir

# ---- every helper definition, and how it closes ----
# A definition is ".xpostsys /name {" at the top level of a file, or a
# bare "/name {" inside a region that opened the namespace. Its body runs
# to the brace that closes it; the token before the put or def on that
# line is what this reads. A % inside a (...) string is not a comment:
# PostScript names its special files (%stdout), (%stderr), (%d).
awk '
function code(line,   out, i, c, par) {
    out = ""; par = 0
    for (i = 1; i <= length(line); i++) {
        c = substr(line, i, 1)
        if (c == "(") par++
        else if (c == ")" && par > 0) par--
        else if (c == "%" && par == 0) break
        out = out c
    }
    return out
}
FNR == 1 { inx = 0; depth = 0 }
{
    t = code($0)
    if ($0 ~ /^\.xpostsys begin/) inx = 1
    if ($0 ~ /^end/ && inx) inx = 0
    if (open) {
        depth += gsub(/\{/, "{", t) - gsub(/\}/, "}", t)
        if (depth <= 0) {
            # The install may finish on this line or on the next ones: "}
            # bind put", "end } bind put" and a bare "}" followed by "bind
            # put" are one thing to the interpreter, so they are one thing
            # here. Read on to whichever line carries the put or def.
            if (t ~ /(put|def)[ \t]*$/) {
                if (t ~ /(^|[^A-Za-z.])bind([^A-Za-z]|$)/)
                    printf "%s\t%d\t%s\tbind\n", FILENAME, start, name
                else if (t ~ /(^|[^A-Za-z.])executeonly([^A-Za-z]|$)/)
                    printf "%s\t%d\t%s\tclosed\n", FILENAME, start, name
                open = 0
            } else {
                tail = 1
            }
        }
        if (tail) {
            if (t ~ /(^|[^A-Za-z.])bind([^A-Za-z]|$)/) sawbind = 1
            if (t ~ /(^|[^A-Za-z.])executeonly([^A-Za-z]|$)/) sawclose = 1
            if (t ~ /(put|def)[ \t]*$/) {
                if (sawbind)
                    printf "%s\t%d\t%s\tbind\n", FILENAME, start, name
                else if (sawclose)
                    printf "%s\t%d\t%s\tclosed\n", FILENAME, start, name
                tail = 0; sawbind = 0; sawclose = 0; open = 0
            }
        }
        next
    }
    if (match($0, /^\.xpostsys[ \t]+\/[A-Za-z.][A-Za-z0-9_.]*[ \t]+\{/) ||
        (inx && match($0, /^\/[A-Za-z.][A-Za-z0-9_.]*[ \t]+\{/))) {
        name = substr($0, RSTART, RLENGTH)
        sub(/^\.xpostsys[ \t]+/, "", name); sub(/^\//, "", name)
        sub(/[ \t]+\{$/, "", name)
        start = FNR; open = 1; tail = 0; sawbind = 0; sawclose = 0
        depth = gsub(/\{/, "{", t) - gsub(/\}/, "}", t)
        if (depth <= 0) open = 0
    }
}
' "$data"/*.ps > "$work/bound" 2>/dev/null || true

# What the C removes from systemdict at lockdown. Each must be reached
# with // so the reference outlives the name.
sed -n '/_undef_sandbox_ops/,/^}/p' "$src/src/lib/xpost_op_file.c" 2>/dev/null \
    | grep -oE '"\.[A-Za-z][A-Za-z0-9_]*"' | tr -d '"' \
    | LC_ALL=C sort -u > "$work/removed" || true
nrem=$(grep -c . "$work/removed" || true)
if [ "${nrem:-0}" -eq 0 ]; then
    echo "check-helper-bind: read no operator names out of _undef_sandbox_ops;" >&2
    echo "the C has changed shape and the check below rests on nothing" >&2
    fail=1
fi

# The comment describing the rule names the operator too, so what is read
# here is the code with its comments taken off.
awk '
function code(line,   out, i, c, par) {
    out = ""; par = 0
    for (i = 1; i <= length(line); i++) {
        c = substr(line, i, 1)
        if (c == "(") par++
        else if (c == ")" && par > 0) par--
        else if (c == "%" && par == 0) break
        out = out c
    }
    return out
}
{ print code($0) }' "$data"/*.ps > "$work/code"

while read -r op; do
    [ -n "$op" ] || continue
    esc=$(printf '%s' "$op" | sed 's/\./\\./g')
    if grep -qE "(^|[^/])$esc([[:blank:]]|\{|$)" "$work/code" 2>/dev/null; then
        echo "check-helper-bind: $op is reached by name; the C undefines it from" >&2
        echo "systemdict at lockdown, so the name is gone by the time the body" >&2
        echo "runs -- reach it with // while the name still answers" >&2
        fail=1
    fi
done < "$work/removed"

awk -F'\t' '$4 == "bind"'   "$work/bound" > "$work/bound.bind"
awk -F'\t' '$4 == "closed"' "$work/bound" > "$work/bound.closed"

nbound=$(grep -c . "$work/bound.bind" || true)
if [ "${nbound:-0}" -ne 0 ]; then
    echo "check-helper-bind: a helper is bound where it is defined; .finalize" >&2
    echo "binds .xpostsys once the standard operators are promoted, and a body" >&2
    echo "bound before that keeps the names it calls for the whole run:" >&2
    while IFS='	' read -r f ln n k; do
        echo "  ${f##*/}:$ln  $n" >&2
    done < "$work/bound.bind"
    fail=1
fi

# The same fault by the other road, and the quieter one. bind rewrites a
# body by READING it, and a body it may not read it declines in silence --
# no error, nothing in the result. A helper closed with executeonly at its
# own definition is one .finalize's sweep walks past while reporting
# success, and every operator name it calls stays a name a program can
# shadow. Such a body asks to be closed with //.closeafterbind instead,
# and .finalize closes it once the sweep has frozen its references.
nclosed=$(grep -c . "$work/bound.closed" || true)
if [ "${nclosed:-0}" -ne 0 ]; then
    echo "check-helper-bind: a helper is closed where it is defined. bind" >&2
    echo "cannot read a closed body and says nothing when it declines one, so" >&2
    echo "the sweep walks past it and its operator names stay live. Ask for" >&2
    echo "the closing with //.closeafterbind and .finalize will do it after:" >&2
    while IFS='	' read -r f ln n k; do
        echo "  ${f##*/}:$ln  $n" >&2
    done < "$work/bound.closed"
    fail=1
fi

# A scan that found no definitions at all would pass in silence, which is
# the state this exists to refuse.
ndefs=$(awk '
    /^\.xpostsys[ \t]+\/[A-Za-z.][A-Za-z0-9_.]*[ \t]+\{/ { n++ }
    END { print n + 0 }' "$data"/*.ps)
if [ "$ndefs" -lt 100 ]; then
    echo "check-helper-bind: read $ndefs helper definitions; a tree this size" >&2
    echo "holds a hundred or more, so the scan is not reading what it says" >&2
    fail=1
fi

# ---- a helper a test redefines must be reached by name ----
# What the tests inject, taken from the tests rather than from a list
# kept beside them: a list would be the same fact written twice and the
# copies could disagree with nothing to notice.
grep -hoE '\.xpostsys[[:blank:]]+/[A-Za-z.][A-Za-z0-9_.]*[[:blank:]]+\{' "$tests"/*.sh 2>/dev/null \
    | sed 's|^\.xpostsys[[:blank:]]*/||; s|[[:blank:]]*{$||' \
    | LC_ALL=C sort -u > "$work/overridden" || true

ninj=$(grep -c . "$work/overridden" || true)
if [ "${ninj:-0}" -eq 0 ]; then
    echo "check-helper-bind: no test was found redefining a helper; the tests" >&2
    echo "have changed shape and the second half of this check reads nothing" >&2
    fail=1
fi

while read -r n; do
    [ -n "$n" ] || continue
    # .h is the worked example in init.ps's convention note, not a helper
    [ "$n" = ".h" ] && continue
    if grep -qE "//$(printf '%s' "$n" | sed 's/\./\\./g')([[:blank:]]|$)" "$data"/*.ps 2>/dev/null; then
        echo "check-helper-bind: $n is redefined by a test but reached with //;" >&2
        echo "the baked reference holds what the data tree defined, so the" >&2
        echo "redefinition is inert and the test passes while reading nothing" >&2
        fail=1
    fi
done < "$work/overridden"

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS ($ndefs helper definitions, none bound at its own definition;" \
     "$nrem operator(s) the C takes away, each reached with //;" \
     "$ninj redefined by a test, each reached by name)"
