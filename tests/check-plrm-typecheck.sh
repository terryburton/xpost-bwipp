#!/bin/sh
#
# Every operator whose operands the specification gives a definite type
# answers typecheck when it is handed objects of the wrong type.
#
# tests/check-plrm-errors.sh drives stackunderflow, which needs no
# knowledge of an operator beyond its name: an empty operand stack is the
# same preparation for all of them. typecheck needs more -- an operand of
# a type the operator excludes -- and that is per-operator knowledge. The
# specification carries it in the syntax line that opens each entry: the
# line names one operand per position, and a null is of a type every
# named operand kind excludes. So tests/plrm-typecheck carries the COUNT
# of operands an entry names and needs no mapping from an operand's name
# to a type -- the names are mostly semantic (x, angle, filename), and
# what is read off them is how many there are, not what each one is.
#
# The operands are nulls, one per operand the syntax line names. A null is
# of a type every named operand kind excludes, so typecheck is the answer;
# and the count is the count the line gives, which is the half that has to
# be right. Handed too few, an operator raises stackunderflow, and a gate
# that took that for a wrong-type refusal would be testing the arity check
# and reporting it as the type check. The register carries the count, and
# the one operator whose count cannot be read from the text is recorded as
# such rather than driven.
#
# Both directions are held, per operator rather than in aggregate. An
# operator that stops refusing a wrong type has lost what tells a mistake
# from a result; one held out of the question that starts answering
# correctly is an exclusion whose reason has expired. An operator answering
# a different error is held to THAT error, not to typecheck, because for
# three of them another refusal is the right one and the register says
# which and why.
#
# One interpreter run for all of them.
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-plrm-typecheck.sh <srcroot> <xpost>}
xpost=${2:?usage: check-plrm-typecheck.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/plrm-typecheck"
guard_require_file "$register" "the register of what a wrong-typed call answers"

cr=$(printf '\r')
tab=$(printf '\t')
guard_held=0

grep -v '^[[:space:]]*#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"

awk -F'\t' '$1 == "drive" && $3 ~ /^[0-9]+$/ { print $2 "\t" $3 "\t" $4 }' \
    "$work/reg" | LC_ALL=C sort > "$work/want"
awk -F'\t' '$1 == "procset" { print $2 }' "$work/reg" | LC_ALL=C sort -u > "$work/procset"

ndrive=$(grep -c . "$work/want" || true)
if [ "$ndrive" -lt 100 ]; then
    echo "FAILURES: the register drives $ndrive operators; the specification"
    echo "          defines several hundred and most take operands of a"
    echo "          stated type, so this is not reading the register and"
    echo "          would report nothing about the interpreter"
    exit 1
fi

# Every operator has exactly one line, so one that stops being defined --
# or arrives -- is a line that no longer matches rather than a silence.
awk -F'\t' '$1 == "drive" || $1 == "procset" || $1 == "wrapped" || $1 == "skip" \
            { print $2 }' "$work/reg" | LC_ALL=C sort > "$work/named"
if [ "$(LC_ALL=C sort -u "$work/named" | wc -l)" -ne "$(wc -l < "$work/named")" ]; then
    echo "FAILURES: an operator has more than one line in the register, so"
    echo "      what is asked of it depends on which line is read first:"
    LC_ALL=C uniq -d "$work/named" | sed 's/^/      /'
    guard_held=1
fi

# ---- ask the interpreter ---------------------------------------------
#
# Each operator is handed its own count of nulls, inside stopped, with the
# operand stack emptied first and the dictionary stack put back to the
# depth it started at: one that opened a dictionary before it refused
# would otherwise leave every operator after it running inside it.
{
    cat <<'HDR'
userdict /probe {
    userdict /nops 3 -1 roll put
    userdict /nm 3 -1 roll put
    userdict /d0 countdictstack put
    clear
    { userdict /nops get { null } repeat
      userdict /nm get cvx exec } stopped
    { $error /errorname get }{ /noerror } ifelse
    userdict /ans 3 -1 roll put
    countdictstack userdict /d0 get sub
    dup 0 gt { { end } repeat }{ pop } ifelse
    clear
    userdict /nm get 64 string cvs print (\t) print
    userdict /ans get 64 string cvs print (\n) print
} put
HDR
    while IFS="$tab" read -r op n want; do
        printf '(%s) cvn %s userdict /probe get exec\n' "$op" "$n"
    done < "$work/want"
    while read -r op; do
        printf '(%s) cvn 0 userdict /probe get exec\n' "$op"
    done < "$work/procset"
    echo 'flush'
} > "$work/probe.ps"

# The file sandbox is left on, which no other guard here needs to say
# because no other guard calls every operator in the language. These
# operands are nulls and the file operators refuse them on their type
# before they reach a name, so nothing here should touch a file at all --
# and leaving the confinement in place is what makes that a property of
# the run rather than of the reasoning about it. The answers are the same
# either way.
XPOST_DATA_DIR="$srcdata" "$xpost" -q -d null -o /dev/null \
    "$work/probe.ps" </dev/null 2>/dev/null | tr -d "$cr" > "$work/answers"

nans=$(awk -F'\t' 'NF == 2' "$work/answers" | wc -l)
nasked=$((ndrive + $(grep -c . "$work/procset" || true)))
if [ "$nans" -ne "$nasked" ]; then
    echo "FAILURES: $nasked operators were asked and $nans answered. The run"
    echo "          stopped part way, so what follows says nothing about the"
    echo "          operators after the one it stopped at"
    exit 1
fi

# ---- direction one: what each must answer ----------------------------
: > "$work/wrong"
while IFS="$tab" read -r op n want; do
    got=$(awk -F'\t' -v O="$op" '$1 == O { print $2 }' "$work/answers")
    [ "$got" = "$want" ] || printf '%s wanted %s and answered %s\n' \
        "$op" "$want" "${got:-nothing}" >> "$work/wrong"
done < "$work/want"
if [ -s "$work/wrong" ]; then
    echo "FAILURES: handed operands of a type its syntax line excludes, and"
    echo "      answering something the register does not record. An"
    echo "      operator that stops refusing a wrong type has lost what a"
    echo "      program tells a mistake from a result by:"
    sed 's/^/      /' "$work/wrong"
    guard_held=1
fi

# ---- direction two: the exclusions -----------------------------------
: > "$work/stale"
while read -r op; do
    got=$(awk -F'\t' -v O="$op" '$1 == O { print $2 }' "$work/answers")
    [ "$got" = "undefined" ] || printf '%s answered %s\n' "$op" "${got:-nothing}" \
        >> "$work/stale"
done < "$work/procset"
if [ -s "$work/stale" ]; then
    echo "FAILURES: held out of the question because a bare call does not"
    echo "      reach them, and answering something other than undefined."
    echo "      The reason beside each in tests/plrm-typecheck has stopped"
    echo "      being true:"
    sed 's/^/      /' "$work/stale"
    guard_held=1
fi

# ---- the ratchet ------------------------------------------------------
ncov=$(awk -F'\t' '$2 == "typecheck" { n++ } END { print n + 0 }' "$work/answers")
nrec=$(awk -F'\t' '$1 == "covered" && $2 ~ /^[0-9]+$/ && !f { print $2; f = 1 }' "$work/reg")
case ${nrec:-} in
    ''|*[!0-9]*)
        echo "FAILURES: tests/plrm-typecheck has no 'covered <n>' line, so the"
        echo "          count nothing else bounds is bounded by nothing"
        guard_held=1 ;;
    *)  if [ "$ncov" -lt "$nrec" ]; then
            echo "FAILURES: $ncov operators refuse a wrong type where the register"
            echo "          records $nrec. The count is a ratchet: it may rise and"
            echo "          may not fall"
            guard_held=1
        elif [ "$ncov" -gt "$nrec" ]; then
            echo "FAILURES: $ncov operators refuse a wrong type where the register"
            echo "          records $nrec. Raise the covered line to $ncov in this"
            echo "          commit, so the ratchet holds where the interpreter is"
            guard_held=1
        fi ;;
esac

[ "$guard_held" -eq 0 ] || { echo "FAILURES: see above"; exit 1; }
nskip=$(awk -F'\t' '$1 == "skip" || $1 == "wrapped" { n++ } END { print n + 0 }' "$work/reg")
printf 'SUCCESS (%s operators handed a wrong type, %s answer typecheck, %s answer another error the specification gives them, %s out of reach with a reason)\n' \
    "$ndrive" "$ncov" "$((ndrive - ncov))" "$((nskip + $(grep -c . "$work/procset" || true)))"
