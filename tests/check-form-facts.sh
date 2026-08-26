#!/bin/sh
#
# Every form type is accounted for, and every entry of a form dictionary
# carries what this interpreter does with it.
#
# PLRM 4.7 defines one form type, so this family has one member. A
# register for a family of one looks like effort spent on nothing until
# the second member arrives, which is exactly when nobody thinks to ask
# whether the first was ever held to anything. What the register buys
# now is the entries: six of them, two of which this interpreter accepts
# and does not read, and neither of those two was written down anywhere
# before this.
#
# ---- what this holds
#
#   the accepted type set, DERIVED by offering every code from 0 to 20
#   to execform, and held to the register in both directions
#
#   which entries are required, PROBED by leaving each one out. An entry
#   that quietly becomes optional, or quietly starts being demanded,
#   fails here rather than being found by a program that relied on it
#
#   which entries are read, probed by changing the value and looking for
#   a change in the page -- an entry recorded as ignored that starts
#   being consulted fails, and so does one recorded as read that stops
#
#   every divergence, each with the probe that finds it, so a reason
#   cannot outlive the difference it explains
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-form-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-form-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/form-facts"
guard_require_file "$register" "the register of form facts"
guard_require_file "$src/data/init.ps" "the form machinery"

fail=0

grep -v '^[[:space:]]*#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"
awk '$1 == "type" { print $2 " " $3 }' "$work/reg" | sort -n > "$work/reg.type"
awk '$1 == "entry" { print $2 " " $3 " " $4 }' "$work/reg" | sort > "$work/reg.entry"
awk '$1 == "cache" { print $2 " " $3 }' "$work/reg" | sort > "$work/reg.cache"
awk 'NF >= 3 && $2 ~ /^(settled|thorn|heading)$/ { print $1 }' "$work/reg" \
    | sort -u > "$work/reg.diverge"

if [ ! -s "$work/reg.type" ]; then
    echo "FAILURES: tests/form-facts names no form type"
    exit 1
fi

# Run a program and answer with the error name, or with the ink it put
# down. A form that paints puts a known area on the page, so a form that
# stops painting is caught by the ink and not only by the absence of an
# error.
run() {             # <program body> -> "<errorname>" or "ink <n>"
    {
        printf '<< /PageSize [16 16] >> setpagedevice\n/S 64 string def\n'
        printf '{ %s } stopped\n' "$1"
        printf '{ (E ) print $error /errorname get S cvs print (\\n) print clear }\n'
        printf '{ (E none\\n) print } ifelse\nshowpage\n'
    } > "$work/case.ps"
    rm -f "$work/case.pgm"
    _e=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
          "$xpost" -q --no-sandbox -d pgm -o case.pgm case.ps </dev/null 2>&1 \
          | awk '$1 == "E" { print $2; exit }' )
    if [ "${_e:-}" != none ]; then printf '%s' "${_e:-noanswer}"; return; fi
    guard_pnm_ink "$work/case.pgm"
}

# a form that paints a filled box, with one entry replaced or dropped
FORMBODY='/PaintProc { pop 0 setgray 0 0 4 4 rectfill }'
form() {            # [entry to omit] -> the dictionary text
    _omit=${1:-}
    _d="<<"
    [ "$_omit" = FormType ]  || _d="$_d /FormType 1"
    [ "$_omit" = BBox ]      || _d="$_d /BBox [0 0 4 4]"
    [ "$_omit" = Matrix ]    || _d="$_d /Matrix [1 0 0 1 0 0]"
    [ "$_omit" = PaintProc ] || _d="$_d $FORMBODY"
    [ "$_omit" = UniqueID ]  || _d="$_d /UniqueID 42"
    [ "$_omit" = Implementation ] || _d="$_d"
    printf '%s >>' "$_d"
}

# ---- the type set, derived
: > "$work/got.type"
t=0
while [ "$t" -le 20 ]; do
    ans=$(run "<< /FormType $t /BBox [0 0 4 4] /Matrix [1 0 0 1 0 0] $FORMBODY >> execform")
    case $ans in
        ink\ *) echo "$t accepted" >> "$work/got.type" ;;
        rangecheck) ;;
        *)  echo "FAIL: form type $t answered '$ans', which is neither painting nor"
            echo "      the refusal a type outside the set is given"
            fail=1 ;;
    esac
    t=$((t + 1))
done
sort -n "$work/got.type" -o "$work/got.type"

guard_held=0
guard_hold "$work/reg.type" "$work/got.type" \
    "in the register and no longer accepted by execform. Retire the line
      and the count with it:" \
    "accepted by execform and not in the register. A type that can be
      asked for is one whose differences from the rest have to be
      written down; add it to tests/form-facts:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- what a program is told it may use
#
# The set above is what this interpreter paints. What a PROGRAM is told
# it may use is a second statement of the same fact: the /FormType resource
# category (PLRM Table 3.8), whose instances are a list written by hand
# in data/init.ps.
#
# A hand-written list of what the code can do is the shape that drifts,
# and holding it to another list cannot catch the drift that matters --
# something missing from both reads exactly like something that does not
# exist. The set above is read off the interpreter, so holding the
# declaration to it holds a claim against behaviour rather than against
# a second claim.
printf '(*) { =only (\\n) print } 32 string /FormType resourceforall\n' > "$work/decl.ps"
XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/decl.ps" </dev/null 2>/dev/null \
    | tr -d '\r' | awk 'NF == 1 { print }' | sort -n > "$work/decl.set"
if [ ! -s "$work/decl.set" ]; then
    echo "FAILURES: the /FormType category named no instances at all. A"
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
    "paints by this interpreter and not offered as a /FormType resource. A
      program asking what it may use is told less than the truth; the
      list in data/init.ps is where to say so:" \
    "offered as a /FormType resource and not paints by this interpreter. A
      program is promised something this interpreter refuses:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the entries: required or not, read or not
nread=0
while read -r name need use; do
    [ -n "$use" ] || continue
    ans=$(run "$(form "$name") execform")
    case $need in
        required)
            case $ans in
                ink\ *)
                    echo "FAIL: $name is recorded as required and the form painted"
                    echo "      without it ($ans)"
                    fail=1 ;;
            esac ;;
        optional)
            case $ans in
                ink\ *) ;;
                *)  echo "FAIL: $name is recorded as optional and leaving it out"
                    echo "      answered $ans"
                    fail=1 ;;
            esac ;;
        *)  echo "FAIL: $name has no readable required column"
            fail=1 ;;
    esac
    # an entry recorded as ignored must not change the page when it changes
    if [ "$use" = ignored ]; then
        with=$(run "$(form) execform")
        without=$(run "$(form "$name") execform")
        if [ "$with" != "$without" ]; then
            echo "FAIL: $name is recorded as ignored and the page changes when it"
            echo "      is left out: with '$with', without '$without'"
            fail=1
        fi
    else
        nread=$((nread + 1))
    fi
done < "$work/reg.entry"


# How many times a form's paint procedure runs for two placements, on a
# stated device. Once means the drawing was held and replayed; twice
# means the procedure ran again. The counter lives in an array because a
# form's procedure runs inside a dictionary of the form machinery's, so
# a name defined there would not come back.
runs() {            # <device> <setup before the placements> -> a count, or an error name
    {
        printf '/S 64 string def\n/N [ 0 ] def\n'
        printf '<< /PageSize [32 32] >> setpagedevice\n'
        printf '%s\n' "$2"
        printf '{ FD dup execform dup execform pop } stopped\n'
        printf '{ (E ) print $error /errorname get S cvs print (\\n) print clear }\n'
        printf '{ (E none\\n) print (N ) print N 0 get S cvs print (\\n) print } ifelse\n'
        printf 'showpage\n'
    } > "$work/runs.ps"
    _out=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
            "$xpost" -q --no-sandbox -d "$1" -o runs.out runs.ps </dev/null 2>&1 )
    _e=$( printf '%s\n' "$_out" | awk '$1 == "E" { print $2; exit }' )
    if [ "${_e:-}" != none ]; then printf '%s' "${_e:-noanswer}"; return; fi
    printf '%s\n' "$_out" | awk '$1 == "N" { print $2; exit }'
}

# the form every routing probe places, defined as FD by the setup
FORMDEF='/FD << /FormType 1 /BBox [0 0 8 8] /Matrix [1 0 0 1 0 0]
   /PaintProc { pop N 0 N 0 get 1 add put 0 setgray 0 0 8 8 rectfill } >> def'

# ---- the routing: when the drawing is held and when it is painted again
: > "$work/got.cache"
while read -r cond want rest; do
    [ -n "$want" ] || continue
    case $cond in
        raster-device)     dev=pgm;      setup="$FORMDEF" ;;
        clip-holds-box)    dev=pgm;      setup="$FORMDEF
0 0 32 32 rectclip" ;;
        clip-cuts-box)     dev=pgm;      setup="$FORMDEF
0 0 4 4 rectclip" ;;
        screening-device)  dev=pbm;      setup="$FORMDEF" ;;
        read-only-form)    dev=pgm;      setup="$FORMDEF
/FD FD readonly def" ;;
        vector-device)     dev=pdfwrite; setup="$FORMDEF" ;;
        *)  echo "FAIL: the register names a routing condition this check has no"
            echo "      probe for: $cond"
            fail=1
            continue ;;
    esac
    n=$(runs "$dev" "$setup")
    case "$n:$want" in
        1:held|2:afresh) ;;
        1:afresh)
            echo "FAIL: $cond is recorded as painting afresh and the drawing was"
            echo "      held: the paint procedure ran once for two placements"
            fail=1 ;;
        2:held)
            echo "FAIL: $cond is recorded as holding the drawing and the paint"
            echo "      procedure ran twice for two placements"
            fail=1 ;;
        *)  echo "FAIL: $cond answered '$n', which is neither one placement's"
            echo "      worth of painting nor two"
            fail=1 ;;
    esac
    echo "$cond $n" >> "$work/got.cache"
done < "$work/reg.cache"

count() {           # <keyword> <how many were derived>
    guard_hold_count "$work/reg" "$1" "$2" || fail=1
}
count types "$(grep -c . "$work/reg.type")"
count entries "$(grep -c . "$work/reg.entry")"
count divergences "$(grep -c . "$work/reg.diverge")"
count caches "$(grep -c . "$work/reg.cache")"

# ---- the divergences, each with its probe
: > "$work/got.diverge"

# a required entry absent raises undefined rather than a refusal naming
# the operator
ans=$(run "<< /FormType 1 /Matrix [1 0 0 1 0 0] $FORMBODY >> execform")
[ "$ans" = undefined ] && echo missing-required >> "$work/got.diverge"

# the identity a cache files by is not the program's UniqueID: two forms
# carrying one UniqueID and different drawings paint their own drawings
a=$(run "<< /FormType 1 /BBox [0 0 4 4] /Matrix [1 0 0 1 0 0] /UniqueID 7
          /PaintProc { pop 0 setgray 0 0 4 4 rectfill } >> execform
          << /FormType 1 /BBox [0 0 8 8] /Matrix [1 0 0 1 0 0] /UniqueID 7
          /PaintProc { pop 0 setgray 0 0 8 8 rectfill } >> execform")
b=$(run "<< /FormType 1 /BBox [0 0 8 8] /Matrix [1 0 0 1 0 0]
          /PaintProc { pop 0 setgray 0 0 8 8 rectfill } >> execform")
[ "$a" = "$b" ] && echo identity-not-the-programs >> "$work/got.diverge"

# nothing is written into Implementation
imp=$(run "/fd << /FormType 1 /BBox [0 0 4 4] /Matrix [1 0 0 1 0 0] $FORMBODY >> def
           fd execform
           fd /Implementation known { /yes }{ /no } ifelse
           /no eq not { /execform cvx /rangecheck signalerror } if")
case $imp in ink\ *) echo implementation-unwritten >> "$work/got.diverge" ;; esac

# a form on a path-writing device painted once per placement rather than
# emitted once and referred to
v=$(awk '$1 == "vector-device" { print $2 }' "$work/got.cache")
[ "$v" = 2 ] && echo form-not-shared >> "$work/got.diverge"

sort -u "$work/got.diverge" -o "$work/got.diverge"
guard_held=0
guard_hold_divergence form-facts "$work/reg.diverge" "$work/got.diverge"
[ "$guard_held" -eq 0 ] || fail=1

[ "$fail" = 0 ] || exit 1
printf 'SUCCESS (%s form type(s), %s entry(ies) of which %s read, %s routing condition(s) counted by how often the paint procedure ran, %s divergence(s) each found by its own probe)\n' \
    "$(grep -c . "$work/reg.type")" "$(grep -c . "$work/reg.entry")" \
    "$nread" "$(grep -c . "$work/reg.cache")" "$(grep -c . "$work/reg.diverge")"
exit 0
