#!/bin/sh
# Meson test wrapper: assert that no registered private-namespace member
# has moved home or vanished from the interpreter's PostScript sources,
# and that no graphics-state template slot has been dropped, renamed or
# added without being declared.
#
# tests/dict_homes.golden is the register: a "DICT /name" line must be
# answered by the sources, either by the pair appearing in data/*.ps (a
# definition or a frozen reference -- both disappear when a member is
# relocated) or by a definition of the name at the top level of a
# `DICT begin` block, where the dictionary is opened once and the members
# written without it beside them. "gstate /slot" lines must name the
# slots of the .gstatetemplate literal in data/gstate.ps, all of them. A
# feature that adds either registers it in the same commit. This keeps
# machinery born in its final home -- a later commit cannot relocate it
# without failing here.
#
# Both halves are held in both directions. Forwards, every registered
# name must still be there. Backwards, every member the sources define
# into one of the private dictionaries must be registered, as every slot
# the template declares must be: an unregistered member is one this check
# holds to nothing, free to move home in a later commit with no test
# saying otherwise. The register declares how many members it carries, so
# that a member half gone empty is a failure rather than a vacuous
# agreement between two empty lists.
#
# A registered name is found by looking for the pair in the sources, so
# where the name ends matters. The terminator was once "any character
# that is not a letter, digit, dot or equals", which let `_` and `-` end
# a name: the register could then carry /DATA, which nothing defines,
# and be satisfied by /DATA_DIR, which something else does. Four entries
# were fiction on that account. A name ends where PostScript ends one --
# at whitespace or a delimiter -- so that is what is required here.
#
# Where the pair is found matters as much. Read as text, a comment counts:
# a register carrying .xpostsys /.h, which nothing defines, would be
# answered by the line of data/init.ps that writes
# `.xpostsys /.h { ... } put` while explaining what the helper-call idiom
# looks like. An entry satisfied that way holds nothing to anything, and
# any name a comment or a message happens to spell could be registered
# without a member behind it. So the sources are read as PostScript
# instead: what follows a `%` outside a string is not part of the
# program, and a string is not part of it either -- data/clip.ps prints
# `(//.xpostsys /.doclip get exec)` as an example of a call, which is a
# mention of a member and not a use of one.
#
# The names in .xpostsys are held to one rule further: a member holding a
# procedure is dotted, and a member that is not dotted is a dictionary
# whose name ends in Dict. A helper under a plain word reads as a
# program's own name, and this is what stops one arriving.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-dict-homes.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
golden="$src/tests/dict_homes.golden"
guard_require_file "$golden" "the register of private-namespace homes"
guard_require_file "$src/data/gstate.ps" "the graphics state module"

guard_workdir
fail=0
cr=$(printf '\r')   # tolerate CRLF line endings (Windows checkouts)

# ---- the sources, as PostScript rather than as text ----
# Strings go first, since a `%` inside one is not a comment: a line
# mentioning (%stdout) would otherwise lose its tail and a real
# definition on it go unseen. Each string becomes the single token
# (STR), which leaves the token count and the nesting of what encloses
# it intact -- a string may hold an unmatched brace or bracket, and one
# left in the stream would carry the depth count away with it. Strings
# nest, run across lines, and take a backslash escape, so they are read
# out character by character rather than matched by a pattern.
#
# Each file is opened with a bare `%` on a line of its own, which the
# scan below reads as the end of anything it is following. Nothing else
# in this output can hold one -- every `%` in the sources ends its line
# here -- and a statement does not run from one file into the next, so a
# name left dangling at the end of one must not be answered by the
# operator that happens to open the other.
awk '
    FNR == 1 { sdepth = 0; print "%" }
    {
        line = $0
        gsub(/\r/, "", line)    # tolerate CRLF line endings
        out = ""
        i = 1
        n = length(line)
        while (i <= n) {
            c = substr(line, i, 1)
            if (sdepth > 0) {
                if (c == "\\") { i += 2; continue }
                if (c == "(") { sdepth++; i++; continue }
                if (c == ")") {
                    sdepth--
                    if (sdepth == 0) out = out "(STR)"
                    i++
                    continue
                }
                i++
                continue
            }
            if (c == "%") break
            if (c == "(") { sdepth = 1; i++; continue }
            out = out c
            i++
        }
        print out
    }' "$src"/data/*.ps > "$work/code"
if [ ! -s "$work/code" ]; then
    echo "FAILURES: no PostScript found under $src/data; every member would"
    echo "      be reported missing from a tree this check cannot read"
    exit 1
fi

# ---- the members the sources define, and where they define them ----
#
# A member is a name the PostScript puts into one of the private
# dictionaries, written either as `DICT /name <value> put` or as
# `/name <value> def` at the top level of a `DICT begin ... end` block.
# Which of those a `DICT /name` occurrence is cannot be told from the
# pair alone -- `DICT /name get` reads the same member back -- so the
# scan reads forward from the name to the first operator that governs it
# at top level: `put` or `def` defines, `get`, `known`, `undef`, `load`
# and `where` only reach. An occurrence it can classify as neither is
# reported rather than passed over, since a definition the scan reads
# past is a member this check would never ask about.
#
# Depth is counted over procedures and dictionary literals, and over
# array brackets only outside a procedure. `[` is also the mark that
# `counttomark` and `cleartomark` consume, so within a procedure body it
# need never be closed and counting it there would carry the scan off
# the end of the file; the enclosing braces already hold the scan away
# from operators that are not the statement's.
#
# What this derives is what the sources state. A name the C defines, one
# a running procedure computes, and one copied in by a loop are members
# too and are not written down anywhere here, so the register properly
# names more members than this finds; the forward direction is what
# holds those.
#
# The dictionaries read are named here rather than taken from the
# register, which would stop being read for a dictionary whose entries
# all went at once. A home the register names and this does not is
# refused below instead.
homes=".xpostsys .privatedict .internaldict .gscratch"
awk -v homes="$homes" '
    BEGIN { nh = split(homes, h, " "); for (k = 1; k <= nh; k++) ishome[h[k]] = 1 }

    function opens(t,   s, c) {
        s = t; c = gsub(/[{]/, "", s)
        s = t; c += gsub(/<</, "", s)
        return c
    }
    function closes(t,   s, c) {
        s = t; c = gsub(/[}]/, "", s)
        s = t; c += gsub(/>>/, "", s)
        return c
    }
    function bopens(t,   s) { s = t; return gsub(/[[]/, "", s) }
    function bcloses(t,   s) { s = t; return gsub(/[]]/, "", s) }

    # what the name at position p is given, as far as the shape of the
    # value says: a procedure opens with a brace, a dictionary with `<<`
    # or as `<n> dict`. Anything else -- an array, a string, a number, an
    # operator taken by `load` -- is neither, and named as neither.
    function kind(p,   a, b) {
        a = tok[p + 1]; b = tok[p + 2]
        if (a ~ /^[{]/) return "procedure"
        if (a ~ /^<</) return "dictionary"
        if (b == "dict" && a ~ /^[0-9]+$/) return "dictionary"
        return "other"
    }

    # the operator governing the name at position p, or "?" if the scan
    # cannot reach one
    function governs(p,   j, d, b, t) {
        d = 0; b = 0
        for (j = p + 1; j <= n; j++) {
            t = tok[j]
            if (t == "%") return "?"
            if (d == 0 && b == 0) {
                if (t == "put" || t == "def") return t
                if (t == "get" || t == "known" || t == "undef" ||
                    t == "load" || t == "where") return "ref"
            }
            if (d == 0) { b += bopens(t) - bcloses(t); if (b < 0) b = 0 }
            d += opens(t) - closes(t)
            if (d < 0) return "?"
        }
        return "?"
    }

    { for (i = 1; i <= NF; i++) tok[++n] = $i }

    END {
        for (i = 1; i <= n; i++) {
            if (!(tok[i] in ishome)) continue
            d = tok[i]
            if (tok[i + 1] == "begin") {
                # the block: names defined at its top level are members
                # of d. A begin inside a procedure body runs later and
                # opens some other dictionary, so only those outside one
                # count towards the block being closed.
                bd = 0; dd = 1
                for (j = i + 2; j <= n && dd > 0; j++) {
                    t = tok[j]
                    if (t == "%") break
                    if (bd == 0 && t == "begin") dd++
                    else if (bd == 0 && t == "end") { dd--; if (dd == 0) break }
                    else if (bd == 0 && dd == 1 && t ~ /^\/[^][(){}<>\/%]+$/) {
                        v = governs(j)
                        if (v == "def") {
                            print "MEMBER " d " " t
                            print "SHAPE " d " " t " " kind(j)
                        }
                        else if (v == "?") print "UNREAD " d " " t
                    }
                    bd += opens(t) - closes(t)
                }
                if (dd > 0) print "UNREAD " d " begin"
                continue
            }
            if (tok[i + 1] !~ /^\/[^][(){}<>\/%]+$/) continue
            v = governs(i + 1)
            if (v == "put") {
                print "MEMBER " d " " tok[i + 1]
                print "SHAPE " d " " tok[i + 1] " " kind(i + 1)
            }
            else if (v == "?") print "UNREAD " d " " tok[i + 1]
        }
    }' "$work/code" > "$work/defscan"

if grep -q "^UNREAD " "$work/defscan"; then
    echo "FAILURES: these private-namespace names are written in a shape this"
    echo "      check cannot read as either a definition or a reference, so a"
    echo "      member among them would go unheld:"
    sed -n 's/^UNREAD /      /p' "$work/defscan"
    exit 1
fi
sed -n 's/^MEMBER //p' "$work/defscan" | sort -u > "$work/members"
sed -n 's/^SHAPE //p' "$work/defscan" | sort -u > "$work/shapes"
if [ ! -s "$work/members" ]; then
    echo "FAILURES: no private-namespace member definitions found under"
    echo "      $src/data; the register would be held to nothing"
    exit 1
fi

# ---- the graphics state template, and every slot in it ----
#
# The scan below reads one slot per line, so that is required of the
# literal rather than assumed of it: two slots written on one line would
# be a slot the scan cannot see, and therefore one this check does not
# hold to anything.
sed -n '/\.gstatetemplate[[:space:]]*<</,/^[[:space:]]*>>[[:space:]]*def/p' \
    "$src/data/gstate.ps" | tr -d "$cr" > "$work/template"
guard_require_file "$work/template" "the .gstatetemplate literal in data/gstate.ps"
if ! grep -qE '^[[:space:]]*>>[[:space:]]*def' "$work/template"; then
    echo "FAILURES: the .gstatetemplate literal in data/gstate.ps is not closed"
    echo "      by a '>> def' line; the slot scan read to the end of the file"
    exit 1
fi

sed '1d; $d' "$work/template" | sed 's/%.*//' \
    | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$' > "$work/body"

# One key and one value per line: the key first, and after it exactly one
# top-level item -- a token, a balanced composite, or a `<n> dict`. A line
# carrying a second key would declare a slot the scan reads straight past.
awk '
    {
        if ($1 !~ /^\/[A-Za-z][A-Za-z0-9]*$/) { print "BAD " $0; next }
        items = 0; depth = 0
        for (i = 2; i <= NF; i++) {
            if (depth == 0) items++
            t = $i
            depth += gsub(/[{[<]/, "&", t) - gsub(/[]}>]/, "&", t)
        }
        if (depth != 0) { print "BAD " $0; next }
        if (items == 1 || (items == 2 && $NF == "dict")) {
            print "SLOT " substr($1, 2)
        } else {
            print "BAD " $0
        }
    }' "$work/body" > "$work/scan"

if grep -q '^BAD ' "$work/scan"; then
    echo "FAILURES: every line of .gstatetemplate must declare one slot, named"
    echo "      first, so that the slots can be read off it:"
    sed -n 's/^BAD /      /p' "$work/scan"
    exit 1
fi
sed -n 's/^SLOT //p' "$work/scan" | sort -u > "$work/slots"
if [ ! -s "$work/slots" ]; then
    echo "FAILURES: no slots found in the .gstatetemplate literal"
    exit 1
fi

tr -d "$cr" < "$golden" > "$work/register"
awk '$1 == "gstate" { print substr($2, 2) }' "$work/register" \
    | sort -u > "$work/registered"
awk '$1 !~ /^#/ && $1 != "gstate" && $1 != "entries" && $1 != "" && $2 ~ /^\// \
    { print $1 " " $2 }' "$work/register" | sort -u > "$work/regmembers"

# a home the scan above does not read is one whose members are registered
# and never derived, so the register could omit any of them
awk '{ print $1 }' "$work/regmembers" | sort -u > "$work/reghomes"
strangers=
while read -r h; do
    [ -n "$h" ] || continue
    case " $homes " in
        *" $h "*) ;;
        *) strangers="$strangers $h" ;;
    esac
done < "$work/reghomes"
if [ -n "$strangers" ]; then
    echo "FAILURES: the register gives members a home this check does not"
    echo "      read --$strangers; name it among the homes above, or its"
    echo "      members are held in one direction only"
    exit 1
fi

# a slot nobody declared is a slot nothing holds: it may be renamed at
# will and no test says otherwise
if [ -s "$work/slots" ]; then
    while read -r slot; do
        [ -n "$slot" ] || continue
        if ! grep -qx "$slot" "$work/registered"; then
            echo "UNDECLARED gstate slot: /$slot"
            echo "      add 'gstate /$slot' to tests/dict_homes.golden in this commit"
            fail=1
        fi
    done < "$work/slots"
fi

# and the same of a member: one the sources define and the register does
# not name is machinery this check holds nowhere
while read -r member; do
    [ -n "$member" ] || continue
    if ! grep -qxF "$member" "$work/regmembers"; then
        echo "UNREGISTERED member: $member"
        echo "      add '$member' to tests/dict_homes.golden in this commit"
        fail=1
    fi
done < "$work/members"

# ---- and under what name a member of .xpostsys may hold it ----
#
# A member of the private helper namespace says what it is by how it is
# named. A procedure is dotted, and so are the static tables the
# procedures read; a name that is not dotted ends in Dict and holds a
# dictionary. The interpreter's flags are neither and live in
# .internaldict, in upper case. So a name alone tells a reader what it
# reaches, and a helper cannot arrive under a plain word -- the shape a
# program's own names have, and the shape every reader takes for a
# dictionary.
#
# The two halves ask two different sources. The name is asked of the
# register, which carries every member, including the ones defined in C
# or computed at run time that no scan reads. The value is asked of the
# scan, which is the only side that knows what a member holds; a member
# it cannot see is held by the name half alone. Nothing is asked of a
# dotted member's value, which is where the tables live as well as the
# procedures.
while read -r home name; do
    [ "$home" = .xpostsys ] || continue
    case "$name" in
        /.?*|*Dict) continue ;;
    esac
    echo "MISNAMED member: $home $name"
    echo "      a member of .xpostsys is dotted when it holds a procedure or"
    echo "      a table, and ends in Dict when it is a dictionary the helpers"
    echo "      read; a plain word is neither"
    fail=1
done < "$work/regmembers"

while read -r home name what; do
    [ "$home" = .xpostsys ] || continue
    case "$name" in
        /.?*) continue ;;
    esac
    if [ "$what" = procedure ]; then
        echo "UNDOTTED procedure: $home $name"
        echo "      a member of .xpostsys holding a procedure is dotted; give"
        echo "      it its dot in this commit and move its register entry with it"
        fail=1
    elif [ "$what" != dictionary ]; then
        echo "UNDOTTED member: $home $name holds neither a procedure nor a"
        echo "      dictionary, and only a dictionary is named without a dot"
        fail=1
    fi
done < "$work/shapes"

# ---- the register, line by line ----
lineno=0
members=0
declared=
counts=0
while read -r home name extra; do
    lineno=$((lineno + 1))
    home=${home%"$cr"}; name=${name%"$cr"}; extra=${extra%"$cr"}
    case "$home" in
        ''|'#'*) continue ;;
    esac
    # a line that is not a "DICT /name" pair is not a registration, and a
    # missing name would make the search below match everything
    if [ -z "$name" ] || [ -n "$extra" ]; then
        echo "MALFORMED register line $lineno: $home $name $extra"
        fail=1
        continue
    fi
    if [ "$home" = entries ]; then
        counts=$((counts + 1))
        case "$name" in
            *[!0-9]*|'') echo "MALFORMED register line $lineno: entries takes a count: $name"
                         fail=1 ;;
            *)           declared=$name ;;
        esac
        continue
    fi
    case "$name" in
        /?*) ;;
        *)  echo "MALFORMED register line $lineno: the name must be literal: $home $name"
            fail=1
            continue ;;
    esac
    case "$home" in
        gstate)
            slot=${name#/}
            if ! grep -qx "$slot" "$work/slots"; then
                echo "MISSING gstate slot: /$slot (dropped or renamed in .gstatetemplate)"
                fail=1
            fi
            ;;
        *)
            members=$((members + 1))
            # A member defined at the top level of a `DICT begin` block is
            # written without its dictionary beside it and reached by the
            # bare name, so there is no pair to find; the scan above
            # attributed it to a home, which is the stronger witness of
            # the two and settles the entry.
            grep -qxF "$home $name" "$work/members" && continue
            # Otherwise the pair. The name ends where PostScript ends one:
            # at whitespace or a delimiter. Regex metacharacters in the
            # name -- the leading dot most of them carry -- are matched as
            # themselves.
            pat=$(printf '%s' "$name" | sed 's/[].[^$*\\+?(){}|/]/\\&/g')
            if ! grep -qE "(\\$home|${home#.}) $pat([][(){}<>/%[:space:]]|\$)" \
                 "$work/code"; then
                echo "MISSING member: $home $name (relocated or removed from data/*.ps)"
                fail=1
            fi
            ;;
    esac
done < "$golden"

# ---- and how many members the register says it carries ----
#
# Without this the member half could be emptied and still agree with
# everything asked of it: nothing registered is nothing to look for, and
# a derivation that found nothing would have nothing to complain about.
# The count is against the lines rather than the distinct pairs, so a
# member written twice does not pay for one left out.
if [ "$counts" -ne 1 ]; then
    echo "FAILURES: tests/dict_homes.golden must carry exactly one"
    echo "      'entries <n>' line saying how many members it registers;"
    echo "      it carries $counts"
    exit 1
fi
if [ -n "$declared" ] && [ "$declared" -ne "$members" ]; then
    echo "FAILURES: tests/dict_homes.golden declares 'entries $declared' and"
    echo "      carries $members member registrations"
    fail=1
fi
if [ "$(awk 'END { print NR }' "$work/regmembers")" -ne "$members" ]; then
    echo "FAILURES: tests/dict_homes.golden registers a member twice; the"
    echo "      count above then holds fewer members than it says:"
    awk '$1 !~ /^#/ && $1 != "gstate" && $1 != "entries" && $1 != "" && $2 ~ /^\// \
        { print "      " $1 " " $2 }' "$work/register" | sort | uniq -d
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "dict-homes: the register in tests/dict_homes.golden no longer holds."
    exit 1
fi
found=$(awk 'END { print NR }' "$work/members")
slots=$(awk 'END { print NR }' "$work/slots")
echo "SUCCESS ($members members registered, $found read off the sources, $slots gstate slots)"
exit 0
