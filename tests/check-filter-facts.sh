#!/bin/sh
#
# Every filter is asked the same two questions, and answers them.
#
# The filter operator knows seventeen names. What separates them is not
# written down anywhere the language can be read from: two of them
# refuse to be built from a name alone and want a parameter dictionary
# first, and four of them are in a build only if it found a library.
# Both are differences a program meets -- a name that answers undefined
# is a name it cannot use -- and until this file nothing said which
# names those were, or why.
#
# The family matters more than any member. The last defect in these was
# a lazy end-of-data desynchronisation present in ALL FIVE stream
# decoders, because each was written by copying the one before it; a
# question asked of one member and not the others is exactly how a
# family acquires a defect in every member at once.
#
# So this asks all seventeen, in one interpreter, and holds three things
# to each other:
#
#   the names the source compares against, which is the membership
#   what tests/filter-facts says each one is
#   what the interpreter really answers when asked to build it
#
# Membership is derived rather than listed, so a filter added to the
# operator and to no register fails here rather than passing unexamined.
# The build symbol is derived too, from the conditional the source
# encloses the name in, so a register line claiming the wrong library --
# or a name that quietly stopped being conditional -- is a failure and
# not a comment nobody re-read.
#
#   $1  path to the source tree root
#   $2  path to the xpost binary
set -u
src=${1:?usage: check-filter-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-filter-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$src/src/lib/xpost_op_file.c" "the filter operator"
guard_require_file "$src/tests/filter-facts" "the filter register"

guard_workdir
cr=$(printf '\r')

# ---- membership, and what each name is, read from the source
#
# The filter operator states its family as a table: one row per name,
# carrying the direction the filter works in and the constructor that
# builds it, or NULL where it cannot be built from the name alone. That
# table is the only list, so it is what this reads -- and reading a table
# rather than a chain of comparisons means the three facts come out
# together and can each be held to the register.
#
# The conditional a row sits inside is tracked as the file is scanned, so
# a name is reported with the innermost HAVE_ symbol enclosing it and with
# "always" where there is none. Only HAVE_ conditionals count: the file
# has others, and they are not statements about which library the build
# found.
tr -d "$cr" < "$src/src/lib/xpost_op_file.c" | awk '
    /_filter_specs\[\][ \t]*=/ { intable = 1; next }
    !intable { next }
    /^[ \t]*\};/ { intable = 0; next }
    /^[ \t]*#[ \t]*ifdef[ \t]+HAVE_[A-Z0-9_]+/ {
        for (i = 1; i <= NF; i++)
            if ($i ~ /^HAVE_/) { depth++; sym[depth] = $i }
        next
    }
    /^[ \t]*#[ \t]*if/  { other++; next }
    /^[ \t]*#[ \t]*endif/ {
        if (other > 0) other--
        else if (depth > 0) depth--
        next
    }
    /^[ \t]*\{[ \t]*"[A-Za-z0-9]+"[ \t]*,/ {
        line = $0
        match(line, /"[A-Za-z0-9]+"/)
        nm = substr(line, RSTART + 1, RLENGTH - 2)
        rest = substr(line, RSTART + RLENGTH)
        n = split(rest, fld, ",")
        dir = fld[2];  gsub(/[ \t]/, "", dir)
        cons = fld[3]; gsub(/[ \t}]/, "", cons)
        print nm, (depth > 0) ? sym[depth] : "always", \
              (dir == "1") ? "encode" : "decode", \
              (cons == "NULL") ? "needsdict" : "plain"
    }
' | LC_ALL=C sort > "$work/source-spec"

if [ ! -s "$work/source-spec" ]; then
    echo "FAILURES: no filter rows were read from the table in"
    echo "      src/lib/xpost_op_file.c; the operator was rewritten in a way"
    echo "      this cannot follow, and a check that finds no members proves"
    echo "      nothing about them"
    exit 1
fi
awk '{ print $1, $2 }' "$work/source-spec" | LC_ALL=C sort > "$work/source"
awk '{ print $1 }' "$work/source" | LC_ALL=C sort > "$work/source-names"

# ---- what the register says
# A member line is one whose second field is a kind. A difference line
# carries a disposition there instead, so the two are told apart by what
# they say rather than by where they sit -- and a member line with a
# mistyped kind still reaches the check below that refuses it, which it
# would not if members were selected by naming the kinds.
grep -v '^[[:space:]]*#' "$src/tests/filter-facts" \
    | awk 'NF >= 4 && $1 != "entries" && $2 != "settled" && $2 != "thorn" && $2 != "heading"' \
    > "$work/reg"
awk '{ print $1 }' "$work/reg" | LC_ALL=C sort > "$work/reg-names"
grep -v '^[[:space:]]*#' "$src/tests/filter-facts" \
    | awk 'NF >= 3 && ($2 == "settled" || $2 == "thorn" || $2 == "heading") { print $1 }' \
    | LC_ALL=C sort -u > "$work/reg-diverge"

fail=0

# ---- the membership, both ways
guard_held=0
guard_hold "$work/source-names" "$work/reg-names" \
    "compared against by the filter operator and not classified in
      tests/filter-facts. Say there whether it can be built from a name
      alone and which build carries it. A filter nobody classified is
      one the family was never asked about:" \
    "classified in tests/filter-facts and not a name the filter operator
      compares against. A line that has outlived its filter reads
      exactly like one that still holds:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the build symbol, held to what encloses the name
while read -r name where; do
    said=$(awk -v n="$name" '$1 == n { print $3; exit }' "$work/reg")
    [ -n "$said" ] || continue          # already reported as unclassified
    if [ "$said" != "$where" ]; then
        echo "FAIL: tests/filter-facts says $name is $said and the source"
        echo "      builds it under $where."
        echo "      The register is meant to say which builds carry the"
        echo "      filter; one that names the wrong condition sends a"
        echo "      reader to a library that has nothing to do with it."
        fail=1
    fi
done < "$work/source"

# ---- whether it can be built from a name alone, held to the table
#
# The register's kind column and the table's constructor column state the
# same fact -- a filter with no constructor is one that cannot be built
# from its name -- so they are held to each other. Before the table this
# could only be probed, and a probe sees what the interpreter did rather
# than what it meant to do: a filter accidentally answering as though it
# needed parameters looked exactly like one that does.
while read -r name where dir kind; do
    said=$(awk -v n="$name" '$1 == n { print $2; exit }' "$work/reg")
    [ -n "$said" ] || continue          # already reported as unclassified
    if [ "$said" != "$kind" ]; then
        echo "FAIL: tests/filter-facts calls $name $said and the table in"
        echo "      the filter operator makes it $kind. A filter that cannot"
        echo "      be built from its name alone carries no constructor"
        echo "      there, and the register is where the reason is written."
        fail=1
    fi
    case $dir in
        encode|decode) ;;
        *)  echo "FAIL: the table gives $name the direction '$dir', which is"
            echo "      neither encode nor decode, so the access check it"
            echo "      selects cannot be trusted"
            fail=1 ;;
    esac
done < "$work/source-spec"

# ---- and what the interpreter answers when asked
#
# Each name is offered a file of the direction it wants and nothing
# else. An encoding filter is given a writable file, since one over a
# readable file is refused before the name is even looked at, and its
# output goes to a scratch file rather than to standard output, which
# this reads.
#
# The scratch files are named RELATIVELY and the interpreter is run from
# the directory holding them. A path written into a PostScript string
# literal is one that nothing on the way can rewrite: on a host whose
# shell and interpreter disagree about what a path looks like, an
# absolute name reaches the interpreter unconverted and opens nothing.
# Spelled that way, all seventeen answered undefinedfilename and this
# guard reported a language fault where there was only a portability
# one. A relative name is resolved by the interpreter against its own
# directory, which is the same place either way -- the same route
# tests/filter_family_test.ps takes for its scratch files. Only the
# program and the data directory travel as arguments, and both are made
# absolute first, a drive letter counting as absolute alongside a slash.
printf 'x' > "$work/in.tmp"
cat > "$work/probe.ps" <<'PSEOF'
/isenc { 80 string cvs dup length 6 sub 6 getinterval (Encode) eq } def
/ask {                                  % /Name  .  -
    /nm exch def
    nm 40 string cvs print ( ) print
    nm isenc {
        { (out.tmp) (w) file nm filter } stopped
    }{
        { (in.tmp) (r) file nm filter } stopped
    } ifelse
    { $error /errorname get 40 string cvs }{ pop (ok) } ifelse
    print (\n) print
    clear
} def
PSEOF
awk '{ printf "/%s ask\n", $1 }' "$work/reg-names" >> "$work/probe.ps"

case $xpost in
    /*|[A-Za-z]:*) absxpost=$xpost ;;
    *)             absxpost=$(pwd)/$xpost ;;
esac
case $src in
    /*|[A-Za-z]:*) abssrc=$src ;;
    *)             abssrc=$(pwd)/$src ;;
esac
( cd "$work" && XPOST_DATA_DIR="$abssrc/data" "$absxpost" \
    -q --no-sandbox -d null -o /dev/null probe.ps </dev/null 2>/dev/null ) \
    | tr -d "$cr" | awk 'NF == 2 { print }' | LC_ALL=C sort > "$work/answers"

nasked=$(grep -c . "$work/answers" || true)
nmembers=$(grep -c . "$work/reg-names" || true)
if [ "$nasked" -ne "$nmembers" ]; then
    echo "FAILURES: $nmembers filters were asked and $nasked answered."
    echo "      A member that does not answer is one this check passes over"
    echo "      in silence, which is the state it exists to prevent."
    exit 1
fi

# ---- what a program is told it may filter with
#
# The names above are read from the table the filter operator compares
# against, which is where a filter either exists or does not. What a
# PROGRAM is told it may filter with is a second statement of the same
# fact: the /Filter resource category (PLRM Table 3.8), whose instances
# are a list written by hand in data/init.ps.
#
# Holding one list to another cannot catch the drift that matters -- a
# filter missing from both reads exactly like one that does not exist --
# so the declaration is held to the operator's own table instead.
printf '(*) { =only (\\n) print } 32 string /Filter resourceforall\n' > "$work/decl.ps"
( cd "$work" && XPOST_DATA_DIR="$abssrc/data" "$absxpost" \
    -q --no-sandbox -d null -o /dev/null decl.ps </dev/null 2>/dev/null ) \
    | tr -d "$cr" | awk 'NF == 1 { print }' | LC_ALL=C sort > "$work/decl.set"
if [ ! -s "$work/decl.set" ]; then
    echo "FAILURES: the /Filter category named no instances at all. A"
    echo "      category that answers nothing cannot be held to anything."
    exit 1
fi
guard_held=0
guard_hold "$work/source-names" "$work/decl.set" \
    "compared against by the filter operator and not offered as a /Filter
      resource. A program asking what it may filter with is told less
      than the truth; the list in data/init.ps is where to say so:" \
    "offered as a /Filter resource and not a name the filter operator
      compares against. A program is promised a filter this interpreter
      does not have:"
[ "$guard_held" -eq 0 ] || fail=1

while read -r name answer; do
    said=$(awk -v n="$name" '$1 == n { print $2; exit }' "$work/reg")
    where=$(awk -v n="$name" '$1 == n { print $3; exit }' "$work/reg")
    [ -n "$said" ] || continue
    case "$said:$answer" in
        plain:ok|needsdict:typecheck)
            # a filter that needs parameters answers with the type error
            # that says the operands were the wrong shape. It used to
            # answer undefined, which is what a name the operator has
            # never heard of answers, so a program that named a real
            # filter and forgot its parameters was told it does not exist
            ;;
        plain:undefined)
            # a conditional filter absent from this build answers the
            # same way, and that is the one reading this cannot tell
            # apart, so it is accepted and counted
            if [ "$where" = always ]; then
                echo "FAIL: tests/filter-facts says $name builds from a name"
                echo "      alone and the interpreter answers undefined. It is"
                echo "      in no build condition, so there is nothing that"
                echo "      could have left it out."
                fail=1
            else
                absent="$absent $name"
            fi ;;
        needsdict:ok)
            echo "FAIL: tests/filter-facts says $name needs a parameter"
            echo "      dictionary and the interpreter built it from the name"
            echo "      alone. Either it grew defaults for the parameters it"
            echo "      had none for -- say so there -- or it is building"
            echo "      something it has not been told the shape of."
            fail=1 ;;
        needsdict:undefined)
            echo "FAIL: tests/filter-facts says $name needs a parameter"
            echo "      dictionary and the interpreter answers undefined, which"
            echo "      is what it answers for a name it does not know. A"
            echo "      program that named a real filter and left out its"
            echo "      parameters is then told the filter does not exist."
            fail=1 ;;
        *)
            echo "FAIL: $name answered $answer, which is none of the three:"
            echo "      ok would mean it built, typecheck that its operands"
            echo "      were the wrong shape, undefined that the operator does"
            echo "      not know the name."
            fail=1 ;;
    esac
done < "$work/answers"

# ---- the count, so retiring a filter is two edits
entries=$(awk '/^entries /{ print $2; found = 1 } END { if (!found) print "" }' \
    "$src/tests/filter-facts")
case $entries in
    ''|*[!0-9]*)
        echo "FAILURES: tests/filter-facts has no 'entries <n>' line"
        fail=1 ;;
    *)  if [ "$entries" -ne "$nmembers" ]; then
            echo "FAILURES: tests/filter-facts records $entries filters and"
            echo "      holds $nmembers"
            fail=1
        fi ;;
esac

# ---- and every line says why
while read -r name kind where rest; do
    if [ -z "$rest" ]; then
        echo "FAIL: the line for $name gives no reason. A member classified"
        echo "      without one is a member nobody examined."
        fail=1
    fi
    case $kind in
        plain|needsdict) ;;
        *)  echo "FAIL: $name is '$kind', which is neither plain nor needsdict"
            fail=1 ;;
    esac
done < "$work/reg"

# ---- the differences, each found by its own probe
#
# Until this, the family had a membership half here and a behavioural
# half in tests/filter_family_test.ps, and the one deviation either had
# found was asserted there and written down nowhere a reader of the
# register would meet it.
# Every probe below runs from the scratch directory, and every file a probe
# names is named relatively for that reason. The scratch directory is the
# shell's, and on a host where the shell and the interpreter do not spell a
# path the same way -- a POSIX shell driving a native Windows binary -- an
# absolute name from the one is a name the other cannot open. A probe whose
# file will not open answers undefinedfilename, which is an answer about the
# name and not about the filter, and the guard reads it as a difference
# between two sources.
run() {             # <body> -> the error name, or "none"
    {
        printf '/S 80 string def\n'
        printf 'mark { %s } stopped\n' "$1"
        printf '{ cleartomark (E ) print $error /errorname get S cvs print (\\n) print }\n'
        printf '{ cleartomark (E none\\n) print } ifelse\n'
    } > "$work/d.ps"
    ( cd "$work" && XPOST_DATA_DIR="$abssrc/data" \
      "$absxpost" -q --no-sandbox -d null -o /dev/null d.ps </dev/null 2>/dev/null ) \
      | awk '$1 == "E" { print $2; exit }'
}

: > "$work/got-diverge"

# A decoder that leaves what follows its end-of-data for the next reader
# gives the sentinel back; one that eats it does not. Encoded through the
# member itself, so the bytes before the marker are what that member
# would really have written.
{
    printf '/TA (fx-a) def /TB (fx-b) def\n'
    printf '/ROW 216 string def\n'
    printf '0 1 215 { /i exch def ROW i i 7 mul 31 add 255 and put } for\n'
    printf '/SENT (SENTINEL) def\n'
    printf 'TA (w) file dup /CCITTFaxEncode filter dup ROW writestring closefile closefile\n'
    printf '/enc TA (r) file 4096 string readstring pop def\n'
    printf 'TB (w) file dup enc writestring dup SENT writestring closefile\n'
    printf '/f TB (r) file def\n'
    printf 'mark { f /CCITTFaxDecode filter 8192 string readstring pop pop } stopped\n'
    printf '{ cleartomark } { cleartomark } ifelse\n'
    printf '/left null def\n'
    printf 'mark { f 64 string readstring pop /left exch def } stopped\n'
    printf 'cleartomark\n'
    printf 'left null eq { (E eaten\\n) }\n'
    printf '{ left SENT eq { (E survives\\n) }{ (E eaten\\n) } ifelse } ifelse print\n'
    printf 'f closefile\n'
} > "$work/eod.ps"
eod=$( cd "$work" && XPOST_DATA_DIR="$abssrc/data" \
       "$absxpost" -q --no-sandbox -d null -o /dev/null eod.ps </dev/null 2>/dev/null \
       | awk '$1 == "E" { print $2; exit }' )
[ "${eod:-}" = eaten ] && echo ccittfax-eats-the-sentinel >> "$work/got-diverge"

# A filter that exists but wants parameters, asked for by name alone: the
# answer should not be the one an unknown name gets. Both members the
# register classifies needsdict are asked, so the line retires only when
# neither answers as though it did not exist.
# A decoder is asked for over a string and an encoder over a file open for
# writing, so each is handed a data source or target of the direction it
# works in and the answer is about the missing parameters and nothing else.
nd=$(awk '$2 == "needsdict" { print $1 }' "$work/reg")
allundef=yes
any=no
for f in $nd; do
    case $f in
        *Encode) probe="(fx-$f) (w) file /$f filter closefile" ;;
        *)       probe="(abc) /$f filter pop" ;;
    esac
    # a filter absent from this build answers undefined for a reason that
    # is not this one, so it is not evidence either way
    case $(awk -v n="$f" '$1 == n { print $3; exit }' "$work/reg") in
        always) ;;
        *)      continue ;;
    esac
    any=yes
    case $(run "$probe") in
        undefined) ;;
        *)         allundef=no ;;
    esac
done
[ "$any" = yes ] && [ "$allundef" = yes ] \
    && echo parameterised-filter-reported-undefined >> "$work/got-diverge"

# One unknown name, every kind of data source: the answer must not depend
# on what the name was handed, because the name is what the program got
# wrong. Three sources, not two -- a string, a file open for writing and a
# file open for reading -- because the direction used to be inferred from
# the name's ending, so which access was demanded of the source depended on
# a suffix an unknown name may or may not have. The name is asked with and
# without that suffix for the same reason.
allsame=yes
first=
for probe in \
    "(abc) /XpostNoSuchFilter filter pop" \
    "(fx-c) (w) file /XpostNoSuchFilter filter closefile" \
    "(fx-c) (w) file dup (x) writestring closefile
     (fx-c) (r) file /XpostNoSuchFilter filter closefile" \
    "(abc) /XpostNoSuchEncode filter pop" \
    "(fx-c) (w) file /XpostNoSuchEncode filter closefile"
do
    got=$(run "$probe")
    if [ -z "$got" ]; then
        echo "FAIL: an unknown filter name left no error name to read, so"
        echo "      this cannot report on whether the answer is uniform"
        fail=1
        allsame=no
        break
    fi
    if [ -z "$first" ]; then first=$got
    elif [ "$got" != "$first" ]; then allsame=no
    fi
done
[ "$allsame" = no ] \
    && echo unknown-name-error-depends-on-the-source >> "$work/got-diverge"

LC_ALL=C sort -u "$work/got-diverge" -o "$work/got-diverge"

guard_held=0
guard_hold_divergence filter-facts "$work/reg-diverge" "$work/got-diverge"
[ "$guard_held" -eq 0 ] || fail=1

ndiv=$(awk '/^divergences /{ print $2; found = 1 } END { if (!found) print "" }' \
    "$src/tests/filter-facts")
case $ndiv in
    ''|*[!0-9]*)
        echo "FAILURES: tests/filter-facts has no 'divergences <n>' line. Without"
        echo "      it the list can be emptied and both directions agree over"
        echo "      two empty sets."
        fail=1 ;;
    *)  held=$(grep -c . "$work/reg-diverge" || true)
        if [ "$ndiv" -ne "$held" ]; then
            echo "FAILURES: tests/filter-facts records $ndiv differences and holds $held"
            fail=1
        fi ;;
esac

# ---- and every difference carries exactly one disposition
grep -v '^[[:space:]]*#' "$src/tests/filter-facts" \
  | awk 'NF >= 3 && ($2 == "settled" || $2 == "thorn" || $2 == "heading")' \
  | while read -r what disp rest; do
        if [ -z "$rest" ]; then
            echo "FAIL: the difference $what carries no reason. A difference"
            echo "      recorded without one cannot be re-examined."
            exit 1
        fi
        case $disp in
            settled) ;;
            thorn|heading)
                case $rest in
                    *removes*|*target*|*closes*) ;;
                    *)  echo "FAIL: $what is a $disp and does not say what would"
                        echo "      remove it. A thorn that owes nothing is a"
                        echo "      difference nobody has costed."
                        exit 1 ;;
                esac ;;
        esac
    done || fail=1

thorns=$(grep -v '^[[:space:]]*#' "$src/tests/filter-facts" \
         | awk 'NF >= 3 && $2 == "thorn" { print "      " $1 }')
if [ -n "$thorns" ]; then
    echo "THORNS still carried by the filter family:"
    printf '%s\n' "$thorns"
fi

[ "$fail" = 0 ] || exit 1
absent=${absent:-}
if [ -n "$absent" ]; then
    printf 'SUCCESS (%s filters, each classified and each asked; not in this build:%s)\n' \
        "$nmembers" "$absent"
else
    printf 'SUCCESS (%s filters, each classified and each asked, all present in this build)\n' \
        "$nmembers"
fi
