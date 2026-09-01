#!/bin/sh
#
# Every pattern type is accounted for, and each one paints.
#
# PLRM 4.9.1 defines exactly two: a tiling pattern and a shading
# pattern. What makes the family worth a register is that the type is
# read in three places and two of them ask the same question, while the
# third asks a different one and answers it in a way that contradicts
# them -- so a pattern could be minted by one operator and refused by
# the next.
#
# ---- what this holds
#
#   the type set, DERIVED by offering every code from -3 to 20 to
#   makepattern and painting with what comes back. The dictionary
#   offered carries EVERY entry either branch could want, so a type is
#   never reported refused merely because a key its branch wanted was
#   missing -- only the type code varies
#
#   accepted means PAINTS, counted in ink. A type that is instantiated
#   without complaint and then paints as though it were a different type
#   is the defect this register was written after, and it satisfies any
#   check that only asks whether an error was raised
#
#   what a value that is not a pattern type gets, and that the three
#   answers stay distinct: absent, out of range, and of the wrong type
#   are three different faults and PLRM names three different errors
#
#   which entries each type requires, PROBED by leaving each one out
#
#   the values PLRM states a rule for, where one outside it would
#   otherwise be painted or silently dropped
#
#   every divergence, each with the probe that finds it
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-pattern-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-pattern-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/pattern-facts"
guard_require_file "$register" "the register of pattern facts"
guard_require_file "$src/data/pattern.ps" "the pattern machinery"

fail=0
grep -v '^[[:space:]]*#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"
awk '$1 ~ /^-?[0-9]+$/ && $2 == "accepted" { print $1 }' "$work/reg" | sort -n > "$work/reg.type"
awk '$1 == "badtype" { print $2 " " $3 }' "$work/reg" | sort > "$work/reg.bad"
awk '$1 == "entry"   { print $2 " " $3 " " $4 " " $5 }' "$work/reg" | sort > "$work/reg.entry"
awk '$1 == "value"   { print $2 " " $3 " " $4 }' "$work/reg" | sort > "$work/reg.value"
awk '$1 == "instantiated" { print $2 " " $3 }' "$work/reg" | sort > "$work/reg.inst"
awk 'NF >= 3 && $2 ~ /^(settled|thorn|heading)$/ { print $1 }' "$work/reg" \
    | sort -u > "$work/reg.diverge"

[ -s "$work/reg.type" ] || { echo "FAILURES: the register names no pattern type"; exit 1; }

# every entry either branch of makepattern could want, so only the type
# code under test varies. A shading that paints the whole region, and a
# paint procedure that inks a cell.
FULL='/PaintType 1 /TilingType 1 /BBox [0 0 4 4] /XStep 4 /YStep 4
      /PaintProc { pop 0 setgray 0 0 4 4 rectfill }
      /Shading << /ShadingType 2 /ColorSpace /DeviceGray /Coords [0 0 40 40]
                  /Function << /FunctionType 2 /Domain [0 1]
                               /C0 [0] /C1 [0] /N 1 >> >>'

# make a pattern of the stated type and paint with it
paint() {           # <PatternType literal> -> "<errorname>" or "ink <n>"
    {
        printf '<< /PageSize [40 40] >> setpagedevice\n/S 200 string def\n'
        printf '{ [ /Pattern ] setcolorspace\n'
        printf '  << /PatternType %s %s >> matrix makepattern setcolor\n' "$1" "$FULL"
        printf '  5 5 30 30 rectfill } stopped\n'
        printf '{ (E ) print $error /errorname get S cvs print (\\n) print clear }\n'
        printf '{ (E none\\n) print } ifelse\nshowpage\n'
    } > "$work/case.ps"
    rm -f "$work/case.pgm"
    _e=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
          "$xpost" -q --no-sandbox -d pgm -o case.pgm case.ps </dev/null 2>/dev/null \
          | awk '$1 == "E" { print $2; exit }' )
    [ "${_e:-}" = none ] || { printf '%s' "${_e:-noanswer}"; return; }
    guard_pnm_ink "$work/case.pgm"
}

# run a body and report only whether and how it failed
outcome() {         # <body> -> "<errorname>" or "none"
    {
        printf '<< /PageSize [40 40] >> setpagedevice\n/S 200 string def\n'
        printf '{ %s } stopped\n' "$1"
        printf '{ (E ) print $error /errorname get S cvs print (\\n) print clear }\n'
        printf '{ (E none\\n) print } ifelse\nshowpage\n'
    } > "$work/o.ps"
    ( cd "$work" && XPOST_DATA_DIR="$srcdata" \
      "$xpost" -q --no-sandbox -d pgm -o o.pgm o.ps </dev/null 2>/dev/null ) \
      | awk '$1 == "E" { print $2; exit } END { }'
}

# ---- the type set, derived
: > "$work/got.type"
t=-3
while [ "$t" -le 20 ]; do
    ans=$(paint "$t")
    case $ans in
        ink\ 0)     echo "FAIL: pattern type $t is instantiated and paints nothing"
                    fail=1 ;;
        ink\ *)     echo "$t" >> "$work/got.type" ;;
        rangecheck|typecheck|undefined) ;;
        *)          echo "FAIL: pattern type $t answered '$ans', which is neither"
                    echo "      painting nor one of the three refusals"
                    fail=1 ;;
    esac
    t=$((t + 1))
done
sort -n "$work/got.type" -o "$work/got.type"

guard_held=0
guard_hold "$work/reg.type" "$work/got.type" \
    "in the register and no longer painting. Retire the line and the
      count with it:" \
    "instantiated and painting, and named by no line in the register. A
      type a program can ask for is one whose differences from the rest
      have to be written down; add it to tests/pattern-facts:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- what a program is told it may use
#
# The set above is what this interpreter paints. What a PROGRAM is told
# it may use is a second statement of the same fact: the /PatternType resource
# category (PLRM Table 3.8), whose instances are a list written by hand
# in data/pattern.ps.
#
# A hand-written list of what the code can do is the shape that drifts,
# and holding it to another list cannot catch the drift that matters --
# something missing from both reads exactly like something that does not
# exist. The set above is read off the interpreter, so holding the
# declaration to it holds a claim against behaviour rather than against
# a second claim.
printf '(*) { =only (\\n) print } 32 string /PatternType resourceforall\n' > "$work/decl.ps"
XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/decl.ps" </dev/null 2>/dev/null \
    | tr -d '\r' | awk 'NF == 1 { print }' | sort -n > "$work/decl.set"
if [ ! -s "$work/decl.set" ]; then
    echo "FAILURES: the /PatternType category named no instances at all. A"
    echo "      category that answers nothing cannot be held to anything,"
    echo "      and an empty answer reads exactly like one that is"
    echo "      correctly empty."
    exit 1
fi
guard_held=0
guard_hold "$work/got.type" "$work/decl.set" \
    "paints by this interpreter and not offered as a /PatternType resource. A
      program asking what it may use is told less than the truth; the
      list in data/pattern.ps is where to say so:" \
    "offered as a /PatternType resource and not paints by this interpreter. A
      program is promised something this interpreter refuses:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- what a value that is not a pattern type gets
badval() {          # <case> -> the literal, or empty
    case $1 in
    absent)   printf '' ;;
    zero)     printf '0' ;;
    negative) printf '%s' '-1' ;;
    three)    printf '3' ;;
    twenty)   printf '20' ;;
    real)     printf '1.0' ;;
    name)     printf '/Tiling' ;;
    string)   printf '(1)' ;;
    *)        printf 'NOSUCHCASE' ;;
    esac
}
: > "$work/got.bad"
while read -r kase want; do
    lit=$(badval "$kase")
    if [ "$lit" = NOSUCHCASE ]; then
        echo "FAIL: the register has a badtype line for '$kase' and no probe builds it"
        fail=1
        continue
    fi
    if [ "$kase" = absent ]; then
        ans=$(outcome "<< $FULL >> matrix makepattern pop")
    else
        ans=$(paint "$lit")
    fi
    case $ans in
        ink\ *) echo "$kase accepted" >> "$work/got.bad" ;;
        none)   echo "$kase accepted" >> "$work/got.bad" ;;
        *)      echo "$kase $ans" >> "$work/got.bad" ;;
    esac
done < "$work/reg.bad"
sort "$work/got.bad" -o "$work/got.bad"
if ! cmp -s "$work/reg.bad" "$work/got.bad"; then
    echo "FAIL: what a value that is not a pattern type gets is not what the"
    echo "      register records:"
    diff "$work/reg.bad" "$work/got.bad" 2>/dev/null | sed 's/^/      /'
    echo "      Absent, out of range and of the wrong type are three faults,"
    echo "      and PLRM names three errors for them."
    fail=1
fi

# ---- which entries each type requires
without() {         # <type> <entry> -> a dictionary missing that entry
    printf '<< /PatternType %s %s >> dup /%s undef' "$1" "$FULL" "$2"
}
: > "$work/got.entry"
while read -r ty ent req err; do
    ans=$(outcome "$(without "$ty" "$ent") matrix makepattern
                   [ /Pattern ] setcolorspace setcolor 5 5 30 30 rectfill")
    case $ans in
        none) echo "$ty $ent optional none" >> "$work/got.entry" ;;
        *)    echo "$ty $ent required $ans" >> "$work/got.entry" ;;
    esac
done < "$work/reg.entry"
sort "$work/got.entry" -o "$work/got.entry"
if ! cmp -s "$work/reg.entry" "$work/got.entry"; then
    echo "FAIL: the entries a pattern requires are not the ones the register"
    echo "      records:"
    diff "$work/reg.entry" "$work/got.entry" 2>/dev/null | sed 's/^/      /'
    fail=1
fi

# ---- the values PLRM states a rule for
valcase() {         # <entry> <case> -> a pattern dictionary
    _v=
    case $2 in
        zero)     _v=0 ;;
        negative) _v=-4 ;;
        one)      _v=1 ;;
        two)      _v=2 ;;
        three)    _v=3 ;;
        four)     _v=4 ;;
        *)        printf 'NOSUCHCASE'; return ;;
    esac
    printf '<< /PatternType 1 %s >> dup /%s %s put' "$FULL" "$1" "$_v"
}
: > "$work/got.value"
while read -r ent kase want; do
    d=$(valcase "$ent" "$kase")
    if [ "$d" = NOSUCHCASE ]; then
        echo "FAIL: the register has a value line for $ent $kase and no probe builds it"
        fail=1
        continue
    fi
    # PaintType 2 is an uncoloured pattern: PLRM 4.9.1 has it take its
    # colour from the space beneath, so it needs a base space and a
    # component. Painting it the coloured way would report the family
    # refusing a value it accepts.
    if [ "$ent$kase" = PaintTypetwo ]; then
        # An uncoloured cell is painted in the colour set beneath it and may
        # not set one of its own (PLRM 4.9.2), so this case cannot be asked
        # with the shared paint procedure, which sets one. Asked with that
        # one it would report the family refusing a value it accepts.
        ans=$(outcome "[ /Pattern /DeviceGray ] setcolorspace
                       << /PatternType 1 /PaintType 2 /TilingType 1
                          /BBox [0 0 4 4] /XStep 4 /YStep 4
                          /PaintProc { pop 0 0 4 4 rectfill } >>
                       matrix makepattern 0 exch setcolor
                       5 5 30 30 rectfill")
    else
        ans=$(outcome "[ /Pattern ] setcolorspace $d matrix makepattern setcolor
                       5 5 30 30 rectfill")
    fi
    case $ans in
        none) echo "$ent $kase takes" >> "$work/got.value" ;;
        *)    echo "$ent $kase refuses" >> "$work/got.value" ;;
    esac
done < "$work/reg.value"
sort "$work/got.value" -o "$work/got.value"
if ! cmp -s "$work/reg.value" "$work/got.value"; then
    echo "FAIL: the values an entry may hold are not the ones the register"
    echo "      records:"
    diff "$work/reg.value" "$work/got.value" 2>/dev/null | sed 's/^/      /'
    echo "      A rule PLRM states and nothing enforces is a pattern that"
    echo "      paints wrongly or silently paints nothing."
    fail=1
fi

# ---- what is true of the dictionary makepattern hands back
: > "$work/got.inst"
while read -r prop want; do
    case $prop in
    read-only)
        ans=$(outcome "<< /PatternType 1 $FULL >> matrix makepattern
                       /XStep 99 put")
        case $ans in
            invalidaccess) echo "read-only holds" >> "$work/got.inst" ;;
            *)             echo "read-only absent" >> "$work/got.inst" ;;
        esac ;;
    implementation)
        ans=$(outcome "<< /PatternType 1 $FULL >> matrix makepattern
                       /Implementation known not { /undefined signalerror } if")
        case $ans in
            none) echo "implementation holds" >> "$work/got.inst" ;;
            *)    echo "implementation absent" >> "$work/got.inst" ;;
        esac ;;
    *)  echo "FAIL: the register has an instantiated line for '$prop' and no probe builds it"
        fail=1 ;;
    esac
done < "$work/reg.inst"
sort "$work/got.inst" -o "$work/got.inst"
if ! cmp -s "$work/reg.inst" "$work/got.inst"; then
    echo "FAIL: what is true of an instantiated pattern is not what the"
    echo "      register records:"
    diff "$work/reg.inst" "$work/got.inst" 2>/dev/null | sed 's/^/      /'
    echo "      Every later site relies on the instantiated dictionary having"
    echo "      been checked, so a program able to rewrite it reaches"
    echo "      machinery chosen for a type it no longer has."
    fail=1
fi

count() {           # <keyword> <how many were derived>
    guard_hold_count "$work/reg" "$1" "$2" || fail=1
}
count types       "$(grep -c . "$work/reg.type")"
count badtypes    "$(grep -c . "$work/reg.bad")"
count entries     "$(grep -c . "$work/reg.entry")"
count values      "$(grep -c . "$work/reg.value")"
count instantiateds "$(grep -c . "$work/reg.inst")"
count divergences "$(grep -c . "$work/reg.diverge")"

# ---- the divergences, each found by its own probe
: > "$work/got.diverge"
# XUID accepted and never consulted: a pattern with one and a pattern
# without behave the same, and nothing here reads the key
if [ "$(outcome "[ /Pattern ] setcolorspace
                 << /PatternType 1 /XUID [1 2 3] $FULL >> matrix makepattern setcolor
                 5 5 30 30 rectfill")" = none ] \
   && ! grep -q "XUID" "$src/data/pattern.ps" "$src/data/color.ps"; then
    echo xuid-unread >> "$work/got.diverge"
fi
# the same absent entry, named differently by the two operators that
# read it -- probed as a pair, so the line retires only when they agree
mk=$(outcome "<< $FULL >> matrix makepattern pop")
sp=$(outcome "<< $FULL >> setpattern")
[ "$mk" = undefined ] && [ "$sp" = typecheck ] \
    && echo absent-entry-differs-by-operator >> "$work/got.diverge"
sort -u "$work/got.diverge" -o "$work/got.diverge"

guard_held=0
guard_hold_divergence pattern-facts "$work/reg.diverge" "$work/got.diverge"
[ "$guard_held" -eq 0 ] || fail=1

thorns=$(awk 'NF >= 3 && $2 == "thorn" { print "      " $1 }' "$work/reg")
if [ -n "$thorns" ]; then
    echo "THORNS still carried by the pattern family:"
    printf '%s\n' "$thorns"
fi

[ "$fail" = 0 ] || exit 1
printf 'SUCCESS (%s pattern type(s) held to what paints, %s refusal(s) kept distinct, %s entry requirement(s), %s value rule(s), %s divergence(s) each found by its own probe)\n' \
    "$(grep -c . "$work/reg.type")" "$(grep -c . "$work/reg.bad")" \
    "$(grep -c . "$work/reg.entry")" "$(grep -c . "$work/reg.value")" \
    "$(grep -c . "$work/reg.diverge")"
exit 0
