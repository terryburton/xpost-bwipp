#!/bin/sh
#
# Every function type is accounted for, and the widths a sample may be
# are the specification's.
#
# PLRM 3.10.1 names three function types. What makes this family worth a
# register is not the count but the reading: a sampled function's
# samples are a continuous bit stream with no padding at a byte
# boundary, exactly as a mesh shading's vertices are, and both are read
# by one reader. A width that reader stops taking is a program's data
# that will not load, in two families at once.
#
# ---- what this holds
#
#   the type set, DERIVED by offering every code from 0 to 20 inside a
#   shading and seeing which paint, held to the register both ways
#
#   every width PLRM allows a sample to be, PROBED with the samples
#   packed the way that width requires -- data of the wrong shape runs
#   out, and running out raises what being refused raises, so a probe
#   that reused one string would report every width as unsupported
#
#   every divergence, each with the probe that finds it
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-function-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-function-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/function-facts"
guard_require_file "$register" "the register of function facts"
guard_require_file "$src/data/shade.ps" "the function machinery"

fail=0
grep -v '^[[:space:]]*#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"
awk '$1 ~ /^[0-9]+$/ { print $1 " " $2 }' "$work/reg" | sort -n > "$work/reg.type"
awk '$1 == "width" { print $2 " " $3 " " $4 }' "$work/reg" | sort > "$work/reg.width"
awk 'NF >= 3 && $2 ~ /^(settled|thorn|heading)$/ { print $1 }' "$work/reg" \
    | sort -u > "$work/reg.diverge"

[ -s "$work/reg.type" ] || { echo "FAILURES: the register names no function type"; exit 1; }

# a shading carrying the function, and whether it puts ink on a page
paint() {           # <function dictionary text> -> "<errorname>" or "ink <n>"
    {
        printf '<< /PageSize [16 16] >> setpagedevice 2 2 scale\n/S 64 string def\n'
        printf '{ << /ShadingType 2 /ColorSpace /DeviceGray /Coords [0 0 8 8]\n'
        printf '     /Function %s >> shfill } stopped\n' "$1"
        printf '{ (E ) print $error /errorname get S cvs print (\\n) print clear }\n'
        printf '{ (E none\\n) print } ifelse\nshowpage\n'
    } > "$work/case.ps"
    rm -f "$work/case.pgm"
    _e=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
          "$xpost" -q --no-sandbox -d pgm -o case.pgm case.ps </dev/null 2>&1 \
          | awk '$1 == "E" { print $2; exit }' )
    [ "${_e:-}" = none ] || { printf '%s' "${_e:-noanswer}"; return; }
    guard_pnm_ink "$work/case.pgm"
}

# two samples at a stated width, packed as a bit stream: the ends of the
# range, which every width represents exactly
samples() {         # <bits> -> hex
    awk -v B="$1" '
        function putbits(v, n,   i) {
            for (i = n - 1; i >= 0; i--) bits = bits sprintf("%d", int(v / 2 ^ i) % 2)
        }
        BEGIN {
            bits = ""
            putbits(0, B); putbits(2 ^ B - 1, B)
            while (length(bits) % 8) bits = bits "0"
            out = ""
            for (i = 1; i <= length(bits); i += 8) {
                byte = 0
                for (j = 0; j < 8; j++) byte = byte * 2 + substr(bits, i + j, 1)
                out = out sprintf("%02X", byte)
            }
            print out
        }'
}

fn() {              # <type> -> a dictionary of that type
    case $1 in
        0) printf '<< /FunctionType 0 /Domain [0 1] /Range [0 1] /Size [2] /BitsPerSample 8 /DataSource <00FF> >>' ;;
        2) printf '<< /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >>' ;;
        3) printf '<< /FunctionType 3 /Domain [0 1] /Functions [ << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> << /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [0] /N 1 >> ] /Bounds [0.5] /Encode [0 1 0 1] >>' ;;
        *) printf '<< /FunctionType %s /Domain [0 1] /Range [0 1] /C0 [0] /C1 [1] /N 1 /Size [2] /BitsPerSample 8 /DataSource <00FF> /Functions [ << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> ] /Bounds [] /Encode [0 1] >>' "$1" ;;
    esac
}

# ---- the type set, derived
: > "$work/got.type"
t=0
while [ "$t" -le 20 ]; do
    ans=$(paint "$(fn $t)")
    case $ans in
        ink\ *) echo "$t computes" >> "$work/got.type" ;;
        rangecheck|undefined|typecheck) ;;
        *)  echo "FAIL: function type $t answered '$ans', which is neither computing"
            echo "      a colour nor being refused"
            fail=1 ;;
    esac
    t=$((t + 1))
done
sort -n "$work/got.type" -o "$work/got.type"

guard_held=0
guard_hold "$work/reg.type" "$work/got.type" \
    "in the register and no longer computed. Retire the line and the
      count with it:" \
    "computed and not in the register. A type a program can ask for is
      one whose differences from the rest have to be written down; add
      it to tests/function-facts:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- what a program is told it may use
#
# The set above is what this interpreter evaluates. What a PROGRAM is told
# it may use is a second statement of the same fact: the /FunctionType resource
# category (PLRM Table 3.8), whose instances are a list written by hand
# in data/shade.ps.
#
# A hand-written list of what the code can do is the shape that drifts,
# and holding it to another list cannot catch the drift that matters --
# something missing from both reads exactly like something that does not
# exist. The set above is read off the interpreter, so holding the
# declaration to it holds a claim against behaviour rather than against
# a second claim.
printf '(*) { =only (\\n) print } 32 string /FunctionType resourceforall\n' > "$work/decl.ps"
XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/decl.ps" </dev/null 2>/dev/null \
    | tr -d '\r' | awk 'NF == 1 { print }' | sort -n > "$work/decl.set"
if [ ! -s "$work/decl.set" ]; then
    echo "FAILURES: the /FunctionType category named no instances at all. A"
    echo "      category that answers nothing cannot be held to anything,"
    echo "      and an empty answer reads exactly like one that is"
    echo "      correctly empty."
    exit 1
fi
# the derived set carries what each type is beside its number; the
# declaration is numbers alone, so compare the numbers
awk '{ print $1 }' "$work/got.type" | sort -n > "$work/got.set"
guard_held=0
guard_hold "$work/got.set" "$work/decl.set" \
    "evaluates by this interpreter and not offered as a /FunctionType resource. A
      program asking what it may use is told less than the truth; the
      list in data/shade.ps is where to say so:" \
    "offered as a /FunctionType resource and not evaluates by this interpreter. A
      program is promised something this interpreter refuses:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the sample widths
: > "$work/got.width"
while read -r entry bits verdict; do
    [ "$entry" = BitsPerSample ] || { echo "FAIL: the register has a width line for $entry"; fail=1; continue; }
    ds=$(samples "$bits")
    ans=$(paint "<< /FunctionType 0 /Domain [0 1] /Range [0 1] /Size [2]
                    /BitsPerSample $bits /DataSource <$ds> >>")
    case $ans in
        ink\ *)     echo "$entry $bits takes" >> "$work/got.width" ;;
        rangecheck) echo "$entry $bits refuses" >> "$work/got.width" ;;
        *)          echo "$entry $bits $ans" >> "$work/got.width" ;;
    esac
done < "$work/reg.width"
sort "$work/got.width" -o "$work/got.width"
if ! cmp -s "$work/reg.width" "$work/got.width"; then
    echo "FAIL: the widths a sample may be are not the widths the register records:"
    diff "$work/reg.width" "$work/got.width" 2>/dev/null | sed 's/^/      /'
    echo "      A width is a promise to every program with samples in it."
    fail=1
fi

count() {           # <keyword> <how many were derived>
    guard_hold_count "$work/reg" "$1" "$2" || fail=1
}
count types "$(grep -c . "$work/reg.type")"
count widths "$(grep -c . "$work/reg.width")"
count divergences "$(grep -c . "$work/reg.diverge")"

# ---- the divergences
: > "$work/got.diverge"
ans=$(paint "<< /FunctionType 0 /Domain [0 1] /Range [0 1] /Size [2] >>")
[ "$ans" = undefined ] && echo missing-required >> "$work/got.diverge"
ans=$(paint "$(fn 4)")
[ "$ans" = rangecheck ] && echo type-4-is-not-postscript >> "$work/got.diverge"
sort -u "$work/got.diverge" -o "$work/got.diverge"

guard_held=0
guard_hold_divergence function-facts "$work/reg.diverge" "$work/got.diverge"
[ "$guard_held" -eq 0 ] || fail=1

[ "$fail" = 0 ] || exit 1
printf 'SUCCESS (%s function type(s) held to what computes, %s sample width(s) probed, %s divergence(s) each found by its own probe)\n' \
    "$(grep -c . "$work/reg.type")" "$(grep -c . "$work/reg.width")" \
    "$(grep -c . "$work/reg.diverge")"
exit 0
