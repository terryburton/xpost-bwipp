#!/bin/sh
#
# Every operator the specification defines is reachable, or is one this
# interpreter says it does not have.
#
# COMPLIANCE used to answer this by listing the operators it implemented,
# and a list of what works cannot report what is missing from it: that one
# named 318 operators where the specification defines 382, and the
# sixty-odd it never mentioned were invisible from inside it. They had all
# been tested. None had been written down.
#
# So the question is asked of the interpreter instead. tests/plrm-operators
# is the specification's side -- every operator PLRM 8.2 defines, with the
# errors it gives each -- and this asks a running interpreter which of them
# it can reach, by name or through one of the procedure sets that carry the
# rest. What it cannot reach must be exactly what the register says is
# deliberately absent.
#
# Both directions matter. An operator that stops being reachable is a
# regression; one that becomes reachable while still listed as absent is a
# register that has outlived its reason.
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-plrm-operators.sh <srcroot> <xpost>}
xpost=${2:?usage: check-plrm-operators.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/plrm-operators"
guard_require_file "$register" "the register of operators the specification defines"

cr=$(printf '\r')
fail=0

grep -v '^#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"
awk -F'\t' '$1 != "absent" { print $2 }' "$work/reg" | LC_ALL=C sort -u > "$work/ops"
awk -F'\t' '$1 == "absent" { print $2 }' "$work/reg" | LC_ALL=C sort -u > "$work/declared-absent"

nops=$(grep -c . "$work/ops" || true)
if [ "$nops" -lt 300 ]; then
    echo "FAILURES: the register names $nops operators; the specification"
    echo "      defines several hundred, so this is not reading it and would"
    echo "      report nothing about what the interpreter has"
    exit 1
fi

# What the interpreter can reach. A name is reachable if the dictionary
# stack finds it, or if one of the procedure sets that carry the rest of
# the language holds it -- the CID and CMap builders, the bitmap-font
# builder, trapping and colour rendering are reached through their sets
# and are not on any dictionary stack.
{
    cat <<'HDR'
/psnames 400 dict def
[ /CIDInit /BitmapFontInit /Trapping /ColorRendering /FontSetInit ] {
    /setname exch def
    setname /ProcSet resourcestatus {
        pop pop
        setname /ProcSet findresource { pop psnames exch true put } forall
    } if
} forall
/known? { dup where { pop pop true }{ psnames exch known } ifelse } def
HDR
    while read -r op; do
        printf '(%s) cvn dup known? { pop } { 64 string cvs print (\\n) print } ifelse\n' "$op"
    done < "$work/ops"
} > "$work/probe.ps"

XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/probe.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | awk 'NF == 1 { print }' | LC_ALL=C sort -u > "$work/unreachable"

# A probe that reached nothing would agree with any register at all
if [ ! -s "$work/probe.ps" ]; then
    echo "FAILURES: no probe was written, so nothing was asked"
    exit 1
fi

guard_held=0
guard_hold "$work/unreachable" "$work/declared-absent" \
    "defined by the specification and not reachable in this interpreter,
      and not named as absent. Either it has gone missing, or it never
      arrived and nobody said so -- say so in tests/plrm-operators and in
      COMPLIANCE:" \
    "named in tests/plrm-operators as deliberately absent and reachable
      after all. The line has outlived its reason; take it out, and take
      the COMPLIANCE entry with it:"
[ "$guard_held" -eq 0 ] || fail=1

[ "$fail" = 0 ] || { echo "FAILURES: see above"; exit 1; }
nabs=$(grep -c . "$work/declared-absent" || true)
printf 'SUCCESS (%s operators of the specification, %s reachable, %s named absent)\n' \
    "$nops" "$((nops - nabs))" "$nabs"
