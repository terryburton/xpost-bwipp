#!/bin/sh
# Guard: a procedure in a data file that has been written up states its
# stack effect, and states it once.
#
# doc/CONTRIBUTING.md asks every procedure in the interpreter's own
# PostScript for a header: the stack effect on the definition line, and
# prose above it saying which dictionary is current when it runs, which
# VM bank its definitions land in, whether it is executed once as the
# file loads or left for a later caller, who calls it, and what a change
# would trip. Only the first of those is a shape a line scan can see, so
# only the first is held here. The prose is the part that carries the
# orientation and it is left to review, which is a limit worth stating
# rather than hiding: green here means the effects are present and
# singular, not that a reader landing in the file can find their way.
#
# The tree is part-way through that work, so this cannot ask the rule of
# every file at once. tests/proc-spec says of each data file whether its
# procedures have been written up, and only an annotated file is held.
# The register is what makes the check enforce forwards: a file added to
# it can never quietly lose an effect again, and the pending list is a
# work list rather than a permanent excuse.
#
# Both halves of the register are checked, because an exemption register
# rots in two directions. A data file no line classifies is a file the
# rule silently does not reach, and a line naming a file that is gone
# reads as cover for whatever lands under that name next. A pending file
# is scanned too, and one that has come to satisfy the rule without
# being enrolled is reported: that is the state a file is in for exactly
# as long as nobody notices the work is done, and it is the state the
# next edit undoes for free.
#
# What a line scan cannot see, and what review keeps:
#
#   Whether an effect is honest about the procedure beneath it. Nothing
#   here executes anything, so a wrong effect passes.
#
#   Whether a comment that is not a header wants a blank line above it
#   too. Only a block heading a definition is checked, because that is
#   the one whose ownership is in doubt: a remark inside a body belongs
#   to the statement it introduces either way.
#
#   Whether an effect written above the definition belongs on the
#   definition line instead. The rule is that it goes on the definition
#   line and only a polymorphic procedure's several forms go above, but
#   "would not fit" is a judgement about the reader, not a column count,
#   so an effect in the header is accepted wherever it is written once.
#
#   A definition written in a shape this does not recognise -- a
#   procedure put into a dictionary named earlier on the line, for one.
#   The scan under-matches deliberately. A guard that invents
#   definitions fails on code that is right, and a guard that fails on
#   code that is right stops being read, which costs more than the
#   definitions it would have caught.
#
# Usage: check-proc-spec.sh <source tree root>

set -u
src=${1:?usage: check-proc-spec.sh <source tree root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_dir "$src/data" "the PostScript data files"
guard_require_file "$src/tests/proc-spec" "the register of written-up data files"

guard_workdir
guard_mirror reg "$src/tests/proc-spec"
reg="$mirror/proc-spec"
guard_mirror data "$src"/data/*.ps
dat="$mirror"

guard_held=0

# ---- the register against the tree, in both directions
awk '$1 == "file" && NF >= 3 { print $2 }' "$reg" > "$work/reg-files"
( cd "$dat" && ls *.ps 2>/dev/null ) > "$work/tree-files"
if [ ! -s "$work/tree-files" ]; then
    echo "FAILURES: found no PostScript data files to check; the check is unusable"
    exit 1
fi
guard_hold "$work/tree-files" "$work/reg-files" \
    "a data file that tests/proc-spec classifies nowhere, so the rule does
      not reach it. Say whether its procedures have been written up:" \
    "named by tests/proc-spec and not among the data files. A line for a
      file that is gone excuses whatever arrives under that name next:"

# A state that is neither of the two says nothing about the file, and a
# typo in one reads as a file nobody has to write up.
odd=$(awk '$1 == "file" && NF >= 3 && $3 != "annotated" && $3 != "pending" {
               printf "      %s: %s\n", $2, $3 }' "$reg")
if [ -n "$odd" ]; then
    echo "FAIL: tests/proc-spec gives a state that is neither annotated nor pending:"
    printf '%s\n' "$odd"
    guard_held=1
fi

awk '$1 == "file" && NF >= 3 && $3 == "annotated" { print $2 }' "$reg" > "$work/annotated"
awk '$1 == "file" && NF >= 3 && $3 == "pending"   { print $2 }' "$reg" > "$work/pending"

# The counts, so that a line deleted rather than edited is caught: what
# is gone leaves nothing to disagree with.
guard_hold_count "$reg" annotated "$(grep -c . "$work/annotated" || :)" || :
guard_hold_count "$reg" pending   "$(grep -c . "$work/pending"   || :)" || :

# ---- what a definition says about its effect
cat > "$work/scan.awk" <<'AWKEOF'
# The per-cent that opens a comment on a line of code, found past any
# string it might fall inside and past the brace that opens the body, so
# a per-cent inside ( ) is not read as one and neither is a comment
# sitting before the definition begins.
function code_pct(l,   i, c, depth, brace) {
    depth = 0; brace = 0
    for (i = 1; i <= length(l); i++) {
        c = substr(l, i, 1)
        if (c == "\\") { i++; continue }
        if (c == "(") { depth++; continue }
        if (c == ")") { if (depth > 0) depth--; continue }
        if (depth > 0) continue
        if (c == "{" && !brace) brace = i
        if (c == "%") return (brace && i > brace) ? i : 0
    }
    return 0
}

# An effect in the form the tree writes: what goes in, two spaces, the
# dot standing where a PLRM signature puts the operator's name, two
# spaces, what comes out. The two spaces on each side are what tells it
# from a sentence that happens to end in a full stop.
function iseffect(t) { return (t ~ /[^ ]  \.  [^ ]/) }

# The form it replaced, with the procedure's own name where the dot now
# goes. Matched against the name actually being defined rather than
# against a pattern for names, so a mention of some other procedure in
# the prose above is not read as a stale effect. Compared by index
# because a name here carries dots, which a regular expression would
# read as anything at all.
function isnameform(t, nm) { return (index(t, "  " nm "  ") > 0) }

function isarrow(t) { return (t ~ /->/) }

# The name a definition line defines: the literal after the slash,
# whether the line opens with it or names the dictionary it goes into
# first.
function defname(l,   m) {
    m = l
    sub(/^[ \t]*/, "", m)
    if (m !~ /^\//) sub(/^[^ \t]+[ \t]+/, "", m)
    sub(/^\//, "", m)
    sub(/[ \t{[].*$/, "", m)
    return m
}

# A name, then the brace that opens the body it is being bound to --
# with an operand-type array between them where the definition declares
# one. Anything else is left alone; see the note in the guard about
# under-matching on purpose.
#
# The array may nest, and it may run past the end of its line: an
# operator that takes several shapes of operand declares one list per
# shape, and the longest of those do not fit on one line. Read a line at
# a time and such a definition is not seen at all -- neither its effect
# nor its header is ever asked about -- which is a check passing over the
# operators that need it most, the polymorphic ones.
function isdefstart(l) {
    if (l ~ /^[ \t]*%/) return 0
    if (l ~ /^[ \t]*\/[^ \t\/{]+[ \t]+[[{]/) return 1
    if (l ~ /^[.A-Za-z][A-Za-z0-9_.]*[ \t]+\/[^ \t\/{]+[ \t]+[[{]/) return 1
    return 0
}

# Whether what follows the name is an operand-type array and then the
# brace that opens the body. The brace has to come AFTER the array
# closes: an array definition carries braces of its own inside it --
# `/rawrows [ ncomp { width string } repeat ] def` -- and taking the
# first brace found would read every one of those as a procedure.
function bodybrace(l,   i,n,c,q,d) {
    n=length(l); i=1
    while (i<=n && substr(l,i,1) ~ /[ \t]/) i++
    # the dictionary a definition is put into, where one is named
    if (substr(l,i,1) != "/") { while (i<=n && substr(l,i,1) !~ /[ \t]/) i++
                                while (i<=n && substr(l,i,1) ~ /[ \t]/) i++ }
    if (substr(l,i,1) != "/") return 0
    while (i<=n && substr(l,i,1) !~ /[ \t]/) i++
    while (i<=n && substr(l,i,1) ~ /[ \t]/) i++
    if (substr(l,i,1) == "[") {
        d=0; q=0
        while (i<=n) {
            c=substr(l,i,1)
            if (q) { if (c=="\\"){i+=2;continue} if(c=="("){q++} else if(c==")"){q--} i++; continue }
            if (c=="%") return 0
            if (c=="(") { q=1; i++; continue }
            if (c=="[") d++
            else if (c=="]") { d--; if (d==0) { i++; break } }
            i++
        }
        while (i<=n && substr(l,i,1) ~ /[ \t]/) i++
    }
    return (substr(l,i,1) == "{")
}

# The bracket depth a line leaves behind, strings and comments apart.
function brdepth(l,   i,n,c,q,d) {
    n=length(l); i=1; q=0; d=0
    while (i<=n) {
        c=substr(l,i,1)
        if (q) { if (c=="\\"){i+=2;continue} if(c=="("){q++} else if(c==")"){q--} i++; continue }
        if (c=="%") break
        if (c=="(") { q=1; i++; continue }
        if (c=="[") d++
        else if (c=="]") d--
        i++
    }
    return d
}

{ L[FNR] = $0 }

END {
    for (i = 1; i <= FNR; i++) {
        if (!isdefstart(L[i])) continue
        nm = defname(L[i])

        # Follow the operand-type array to the line the brace is on: that
        # is the line the effect belongs to, and the header stands above
        # the line the definition starts on.
        d = brdepth(L[i]); e = i; joined = L[i]
        while (d > 0 && e < FNR) { e++; d += brdepth(L[e]); joined = joined " " L[e] }
        # the brace may still be on a later line: the array closes at the
        # end of one line and the body opens at the start of the next
        while (!bodybrace(joined) && e < FNR \
               && (L[e+1] ~ /^[ \t]*\{/ || L[e+1] ~ /^[ \t]*$/)) {
            if (L[e+1] ~ /^[ \t]*$/) break
            e++; joined = joined " " L[e]
        }
        if (!bodybrace(joined)) continue
        head = i                      # where the header must abut
        i = e                         # where the effect must sit

        p = code_pct(L[i])
        inl = p ? iseffect(substr(L[i], p)) : 0

        # The comment block standing directly above, with no blank line
        # between: a block a blank line has floated off belongs to
        # nothing, and what it says is not this definition's header.
        hdr = 0; harrow = 0; hname = 0
        for (j = head - 1; j >= 1 && L[j] ~ /^[ \t]*%/; j--) {
            if (iseffect(L[j])) hdr++
            if (isarrow(L[j])) harrow++
            if (isnameform(L[j], nm)) hname++
        }

        # A header has to be set off from whatever stands above it. One
        # blank line separates two definitions, and a header belongs to
        # the definition below it -- so a block opening straight off the
        # end of the previous body reads as that body's trailing remark
        # for as long as it takes to notice the name, and the two
        # definitions are run together besides.
        #
        # A line opening a body or a literal is not something to be set
        # off from: the first entry of a table, and the first statement
        # inside a procedure, are where they belong already. All three
        # openers count -- a procedure's brace, an array's bracket and a
        # dictionary's double angle. Its own inline comment is stepped
        # over before the shape is read.
        above = (j >= 1) ? L[j] : ""
        sub(/[ \t]*%.*$/, "", above)
        sub(/[ \t]+$/, "", above)
        runon = (j >= 1 && j < head - 1 && above !~ /^[ \t]*$/ \
                 && above !~ /[{[]$/ && above !~ /<<$/)

        if (runon)                                     v = "runon"
        else if (inl && hdr)                           v = "twice"
        else if (inl && hname)                         v = "stale"
        else if (inl || hdr)                           v = "ok"
        else if (hname || (p && isnameform(substr(L[i], p), nm)))
                                                       v = "nameform"
        else if (harrow || (p && isarrow(substr(L[i], p))))
                                                       v = "arrow"
        else                                           v = "none"
        printf "%d\t%s\t%s\n", i, v, nm
    }
}
AWKEOF

say() {   # <verdict> -> what to tell the reader about it
    case $1 in
    none)     echo "states no stack effect" ;;
    nameform) echo "states its effect with the procedure's name where the dot goes" ;;
    arrow)    echo "states its effect with an arrow rather than the dot" ;;
    twice)    echo "states its effect twice, on the definition line and above it" ;;
    runon)    echo "carries a header run together with the code above it; one blank line sets a header off" ;;
    stale)    echo "states its effect on the definition line and again above it, in the old form with the name where the dot goes" ;;
    *)        echo "$1" ;;
    esac
}

defs=0
held=0
while read -r b; do
    [ -n "$b" ] || continue
    # A register line naming a file that is not there is already
    # reported above; reading it here would only add awk's own words to
    # a failure that has been said properly once.
    [ -f "$dat/$b" ] || continue
    awk -f "$work/scan.awk" "$dat/$b" > "$work/scan.out"
    defs=$((defs + $(grep -c . "$work/scan.out" || :)))
    while IFS="$guard_tab" read -r ln v nm; do
        [ "$v" = "ok" ] && continue
        [ "$held" -eq 0 ] && echo "FAIL: an annotated data file has a procedure whose header does not hold:"
        held=1
        echo "      $b:$ln: $nm $(say "$v")"
    done < "$work/scan.out"
done < "$work/annotated"
[ "$held" -eq 0 ] || guard_held=1

# A pending file that now satisfies the rule. Reported rather than
# accepted: the register is a work list, and a line that has stopped
# being true is the part of it nobody reads.
ready=
while read -r b; do
    [ -n "$b" ] || continue
    [ -f "$dat/$b" ] || continue
    awk -f "$work/scan.awk" "$dat/$b" > "$work/scan.out"
    defs=$((defs + $(grep -c . "$work/scan.out" || :)))
    [ -s "$work/scan.out" ] || continue
    if ! grep -q -v "$guard_tab""ok""$guard_tab" "$work/scan.out"; then
        ready="$ready      $b
"
    fi
done < "$work/pending"
if [ -n "$ready" ]; then
    echo "FAIL: tests/proc-spec still calls these pending and every procedure in"
    echo "      them states its effect. Move the line to annotated, and the"
    echo "      counts with it, so the file cannot lose one again:"
    printf '%s' "$ready"
    guard_held=1
fi

# A scan that matched no definition at all agrees with everything, and
# reads exactly like a tree with nothing wrong in it.
if [ "$defs" -eq 0 ]; then
    echo "FAILURES: the scan recognised no procedure definition in any data"
    echo "          file, so it is answering about nothing"
    exit 1
fi

if [ "$guard_held" -ne 0 ]; then
    echo "FAILURES: see above; doc/CONTRIBUTING.md has the rule and tests/proc-spec the register"
    exit 1
fi
echo "SUCCESS ($(grep -c . "$work/annotated" || :) data files written up, $(grep -c . "$work/pending" || :) pending, $defs definitions read)"
exit 0
