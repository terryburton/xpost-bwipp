#!/bin/sh
#
# Every operator the specification gives stackunderflow raises exactly
# that when it is called with an empty operand stack.
#
# tests/plrm-operators has carried the specification's error lists since
# it was written, and nothing has ever read them: the guard beside it
# uses the operator names alone, to ask which of them this interpreter
# can reach. A column no guard reads is a column that drifts, and that
# one had -- rows carrying errors the specification does not give and
# missing ones it does, and free text run onto the end of the list with
# nothing to separate it. So the lists this reads are derived afresh from
# the specification and kept in tests/plrm-errors, which says how.
#
# Of the errors an operator can raise, stackunderflow is the one that can
# be provoked knowing nothing about the operator but its name. Every
# other class needs an operand of a stated wrong shape, which is
# per-operator knowledge; this needs an empty stack, which is the same
# preparation for all of them. That is why it is the class this drives,
# and driving one class properly is worth more than driving two badly.
#
# Both directions are held. An operator that stops raising it has lost
# something a program relies on to tell a mistake from a result. An
# operator excluded from the question that starts raising it is an
# exclusion that has outlived its reason, and the exclusions are the part
# of a guard like this that rots: each names why it is one, and the day
# the reason stops being true this says so.
#
# One interpreter run, not one per operator: the probe is a few hundred
# lines of PostScript and the cost of the guard is the cost of starting
# the interpreter once.
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-plrm-errors.sh <srcroot> <xpost>}
xpost=${2:?usage: check-plrm-errors.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/plrm-errors"
guard_require_file "$register" "the register of the errors the specification gives"

cr=$(printf '\r')
guard_held=0

grep -v '^[[:space:]]*#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"

# What the specification gives stackunderflow, and what is held out of
# the question with a reason beside it.
awk -F'\t' '$1 == "errors" && $3 ~ /(^|,)stackunderflow(,|$)/ { print $2 }' \
    "$work/reg" | LC_ALL=C sort -u > "$work/want.all"
awk -F'\t' '$1 == "procset" { print $2 }' "$work/reg" \
    | LC_ALL=C sort -u > "$work/excluded"
LC_ALL=C comm -23 "$work/want.all" "$work/excluded" > "$work/want"

nwant=$(grep -c . "$work/want" || true)
if [ "$nwant" -lt 150 ]; then
    echo "FAILURES: the register names $nwant operators as taking"
    echo "          stackunderflow; the specification gives it to several"
    echo "          hundred, so this is not reading the register and would"
    echo "          report nothing about the interpreter"
    exit 1
fi

# Every operator excluded must be one the specification gives
# stackunderflow: an exclusion for an operator the question was never put
# to holds nothing.
#
# One direction, and only one: an operator the register gives
# stackunderflow and does not exclude is the ordinary case, not a
# finding, so this is not the two-way hold guard_hold performs and is
# not written as one. Asked with join rather than the opposite comm over
# the same two sets, because that pair IS guard_hold written again
# (tests/direct_comm.exempt) and the copy is where one direction drifts
# from the other.
LC_ALL=C join -v 1 "$work/excluded" "$work/want.all" > "$work/stray"
if [ -s "$work/stray" ]; then
    echo "FAILURES: held out of a question that was never asked of them --"
    echo "      the register excludes these and does not give them"
    echo "      stackunderflow in the first place:"
    sed 's/^/      /' "$work/stray"
    guard_held=1
fi

# ---- ask the interpreter ----------------------------------------------
#
# Each operator is called with the operand stack emptied first, inside
# stopped, and the answer is whatever $error names. The dictionary stack
# is put back to the depth the probe started at afterwards: an operator
# that opened a dictionary before it failed would otherwise leave every
# operator after it running in a dictionary of its own making.
{
    cat <<'HDR'
userdict /probe {
    userdict /nm 3 -1 roll put
    userdict /d0 countdictstack put
    clear
    { userdict /nm get cvx exec } stopped
    { $error /errorname get }{ /noerror } ifelse
    userdict /ans 3 -1 roll put
    countdictstack userdict /d0 get sub
    dup 0 gt { { end } repeat }{ pop } ifelse
    clear
    userdict /nm get 64 string cvs print (\t) print
    userdict /ans get 64 string cvs print (\n) print
} put
HDR
    cat "$work/want" "$work/excluded" | while read -r op; do
        printf '(%s) cvn userdict /probe get exec\n' "$op"
    done
    echo flush
} > "$work/probe.ps"

if [ ! -s "$work/probe.ps" ]; then
    echo "FAILURES: no probe was written, so nothing was asked"
    exit 1
fi

XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/probe.ps" </dev/null 2>/dev/null | tr -d "$cr" > "$work/answers"

awk -F'\t' 'NF == 2 && $2 == "stackunderflow" { print $1 }' "$work/answers" \
    | LC_ALL=C sort -u > "$work/raised"

if [ ! -s "$work/raised" ]; then
    echo "FAILURES: no operator answered stackunderflow at all. The probe"
    echo "          did not run, or the interpreter stopped part way, so"
    echo "          nothing below says anything about the interpreter"
    exit 1
fi

# ---- direction one: what must raise it --------------------------------
LC_ALL=C comm -23 "$work/want" "$work/raised" > "$work/missing"
if [ -s "$work/missing" ]; then
    echo "FAILURES: given stackunderflow by the specification and answering"
    echo "      something else to a call with an empty operand stack. An"
    echo "      operator that stops raising it has lost what a program"
    echo "      tells a mistake from a result by:"
    while read -r op; do
        got=$(awk -F'\t' -v O="$op" '$1 == O { print $2 }' "$work/answers")
        printf '      %s answered %s\n' "$op" "${got:-nothing}"
    done < "$work/missing"
    guard_held=1
fi

# ---- direction two: what must not ------------------------------------
LC_ALL=C comm -12 "$work/excluded" "$work/raised" > "$work/unexcluded"
if [ -s "$work/unexcluded" ]; then
    echo "FAILURES: held out of the question and answering it correctly"
    echo "      after all. The reason beside each of these in"
    echo "      tests/plrm-errors has stopped being true; take the line"
    echo "      out and let the operator be held like the rest:"
    sed 's/^/      /' "$work/unexcluded"
    guard_held=1
fi

# ---- the ratchet ------------------------------------------------------
nraised=$(grep -c . "$work/raised" || true)
nrec=$(awk -F'\t' '$1 == "answers" && $2 ~ /^[0-9]+$/ && !f { print $2; f = 1 }' \
    "$work/reg")
case ${nrec:-} in
    ''|*[!0-9]*)
        echo "FAILURES: tests/plrm-errors has no 'answers <n>' line, so the"
        echo "          count nothing else bounds is bounded by nothing"
        guard_held=1 ;;
    *)  if [ "$nraised" -lt "$nrec" ]; then
            echo "FAILURES: $nraised operators answer stackunderflow where the"
            echo "          register records $nrec. The count is a ratchet: it"
            echo "          may rise and may not fall"
            guard_held=1
        elif [ "$nraised" -gt "$nrec" ]; then
            echo "FAILURES: $nraised operators answer stackunderflow where the"
            echo "          register records $nrec. Raise the answers line to"
            echo "          $nraised in this commit, so the ratchet holds where"
            echo "          the interpreter now is"
            guard_held=1
        fi ;;
esac

[ "$guard_held" -eq 0 ] || { echo "FAILURES: see above"; exit 1; }
nexc=$(grep -c . "$work/excluded" || true)
printf 'SUCCESS (%s operators the specification gives stackunderflow, %s raise it, %s held out with a reason)\n' \
    "$((nwant + nexc))" "$nraised" "$nexc"
