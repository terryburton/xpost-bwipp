#!/bin/sh
#
# What a colour rendering dictionary must be, and which types a program
# can discover.
#
# PLRM 7.1.3 Table 7.1 gives a type 1 dictionary three required entries
# -- ColorRenderingType, WhitePoint and TransformPQR -- and says the type
# code must be 1. This interpreter records such a dictionary and renders
# through the device's own conversion, which COMPLIANCE states. So what a
# program can learn about colour rendering here is exactly two things:
# whether a dictionary is well formed, and which type codes exist. Both
# are claims, and until this guard neither was held to anything.
#
# ---- what this holds
#
#   the type set, DERIVED by offering setcolorrendering a dictionary
#   carrying each code from 0 to 20 and reading the refusal. rangecheck
#   is the type itself being unknown; anything else is the type being
#   recognised and the dictionary being incomplete, which is recognition.
#   Nothing here reads a list to find the set, which is the point: a list
#   compared against another list cannot see a type missing from both.
#
#   the /ColorRenderingType category's instances, held to that set both
#   ways, so a type this interpreter takes and the category does not
#   offer fails, and a type offered that setcolorrendering refuses fails
#
#   the refusals a malformed dictionary earns, each probed: a missing
#   required entry is undefined, a type code of the wrong type is
#   typecheck, an unimplemented code is rangecheck, and a dictionary
#   carrying all three required entries is taken
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-colorrendering-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-colorrendering-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

guard_require_file "$src/data/gstate.ps" "the colour rendering machinery"

cr=$(printf '\r')
fail=0

# what setcolorrendering says about one dictionary
ask() {             # <dictionary text> -> "<errorname>" or "none"
    {
        printf '/S 64 string def\n'
        printf '{ %s setcolorrendering } stopped\n' "$1"
        printf '{ (E ) print $error /errorname get S cvs print (\\n) print clear }\n'
        printf '{ (E none\\n) print } ifelse\n'
    } > "$work/case.ps"
    XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
        "$work/case.ps" </dev/null 2>/dev/null \
        | tr -d "$cr" | awk '$1 == "E" { print $2; exit }'
}

# ---- the type set, derived
: > "$work/got.type"
t=0
while [ "$t" -le 20 ]; do
    ans=$(ask "<< /ColorRenderingType $t >>")
    case $ans in
        rangecheck)      ;;
        undefined|none)  echo "$t" >> "$work/got.type" ;;
        '')  echo "FAIL: type $t drew no answer from the interpreter at all,"
             echo "      so the membership below would be read off silence"
             fail=1 ;;
        *)   echo "FAIL: type $t answered '$ans', which is neither the"
             echo "      rangecheck that says the code is unknown nor the"
             echo "      complaint about entries that says it is known"
             fail=1 ;;
    esac
    t=$((t + 1))
done
sort -n "$work/got.type" -o "$work/got.type"

if [ ! -s "$work/got.type" ]; then
    echo "FAILURES: setcolorrendering recognised no type code at all. A"
    echo "      membership this cannot read is one it must not report on."
    exit 1
fi

# ---- what a program is told it may ask for
#
# The set above is read off the interpreter. The /ColorRenderingType
# category (PLRM Table 3.8) is a list written by hand in data/gstate.ps,
# and it is what a program reads when it asks which types exist. Holding
# the list to the probe holds a claim against behaviour; holding it to
# another list would not, because a type missing from both reads exactly
# like a type that does not exist.
printf '(*) { =only (\\n) print } 32 string /ColorRenderingType resourceforall\n' \
    > "$work/decl.ps"
XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/decl.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | awk 'NF == 1 { print }' | sort -n > "$work/decl.set"

if [ ! -s "$work/decl.set" ]; then
    echo "FAILURES: the /ColorRenderingType category named no instances at"
    echo "      all. A category that answers nothing cannot be held to"
    echo "      anything, and an empty answer reads exactly like one that"
    echo "      is correctly empty."
    exit 1
fi

guard_held=0
guard_hold "$work/got.type" "$work/decl.set" \
    "taken by setcolorrendering and not offered as a /ColorRenderingType
      resource. A program asking which types exist is told less than the
      truth; the list in data/gstate.ps is where to say so:" \
    "offered as a /ColorRenderingType resource and refused by
      setcolorrendering. A program is promised a rendering type this
      interpreter will not take:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the refusals
#
# Each of these is a way a dictionary can fail to be a colour rendering
# dictionary, and each earns the answer the language uses for that fault
# everywhere else. They are probed rather than asserted from the source
# because the source is what they are meant to hold.
wp='/WhitePoint [0.9505 1.0 1.089]'
pqr='/TransformPQR [{} {} {}]'
for probe in \
    "<< >>|undefined|a dictionary carrying no ColorRenderingType" \
    "<< /ColorRenderingType 1 $pqr >>|undefined|a type 1 with no WhitePoint" \
    "<< /ColorRenderingType 1 $wp >>|undefined|a type 1 with no TransformPQR" \
    "<< /ColorRenderingType (1) $wp $pqr >>|typecheck|a type code that is not an integer" \
    "<< /ColorRenderingType 2 $wp $pqr >>|rangecheck|a type code this interpreter does not implement" \
    "<< /ColorRenderingType 1 $wp $pqr >>|none|a type 1 carrying all three required entries" \
; do
    body=${probe%%|*}
    rest=${probe#*|}
    want=${rest%%|*}
    what=${rest#*|}
    got=$(ask "$body")
    if [ "$got" != "$want" ]; then
        echo "FAIL: $what answered '${got:-nothing}' where the language"
        echo "      answers '$want' for that fault elsewhere"
        fail=1
    fi
done

[ "$fail" = 0 ] || { echo "FAILURES: see above"; exit 1; }
ntype=$(grep -c . "$work/got.type" || true)
printf 'SUCCESS (%s rendering type(s) derived and offered, 6 dictionaries judged)\n' "$ntype"
