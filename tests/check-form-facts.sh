#!/bin/sh
#
# Every form type is accounted for, and every entry of a form dictionary
# carries what this interpreter does with it.
#
# PLRM 4.7 defines one form type, so this family has one member. A
# register for a family of one looks like effort spent on nothing until
# the second member arrives, which is exactly when nobody thinks to ask
# whether the first was ever held to anything. What the register buys
# now is the entries: six of them, one of which this interpreter accepts
# and does not read, and that one was written down nowhere before this.
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
. "$(dirname "$0")/device-fleet.sh"
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
    [ "$_omit" = XUID ]      || _d="$_d /XUID [1 2 3]"
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

# ---- what execform leaves behind
#
# PLRM 4.7 Table 4.4 has execform insert an Implementation entry, and
# PLRM 8.2 has it make the dictionary read-only -- to the operand
# dictionary itself rather than to a copy, and succeeding whether or not
# the program sealed it first. Both are asked of a dictionary the program
# sealed beforehand, which is the case that has to work for the register's
# read-only-form row to mean what it says: a form nothing could stamp
# could not be filed, and would be painted afresh at every placement.
stamped=$(run "/fd << /FormType 1 /BBox [0 0 4 4] /Matrix [1 0 0 1 0 0]
                      $FORMBODY >> def
               /fd fd readonly def
               fd execform
               fd /Implementation known not
                   { /execform cvx /undefined signalerror } if
               fd wcheck { /execform cvx /invalidaccess signalerror } if")
case $stamped in
    ink\ *) ;;
    undefined)
        echo "FAIL: execform left no Implementation entry in the form"
        echo "      dictionary. PLRM 4.7 Table 4.4 has it insert one, and it"
        echo "      is what says the dictionary has been an operand before"
        fail=1 ;;
    invalidaccess)
        echo "FAIL: execform left the form dictionary writable. PLRM 8.2 has"
        echo "      it make the dictionary read-only"
        fail=1 ;;
    *)  echo "FAIL: stamping a form dictionary the program had sealed answered"
        echo "      '$stamped'. PLRM 8.2 has the alterations succeed even"
        echo "      where the dictionary is already read-only"
        fail=1 ;;
esac

# ---- and what it will not take from a program
#
# The Implementation entry is in the program's own dictionary, and until
# execform has seen the dictionary the program can write it. A form
# arriving with a value naming a serial issued to another form must not be
# served that form's drawing -- which is the one failure a serial exists to
# make impossible, and the only one that puts a wrong shape on a page while
# raising nothing.
#
# Read as a page rather than as an error: the second form paints an eight
# by eight square, the first a four by four, so being served the first
# one's drawing is four times less ink. The honest run is the positive
# control -- it is what says the two forms paint differently at all, so a
# forged run answering the same thing is the forgery failing rather than
# the probe seeing nothing.
forge='/fa << /FormType 1 /BBox [0 0 4 4] /Matrix [1 0 0 1 0 0]
              /PaintProc { pop 0 setgray 0 0 4 4 rectfill } >> def
       fa execform
       /fb << /FormType 1 /BBox [0 0 8 8] /Matrix [1 0 0 1 0 0]
              /PaintProc { pop 0 setgray 0 0 8 8 rectfill } >> def'
honest=$(run "$forge
              fb execform")
stolen=$(run "$forge
              fa execform")
forged=$(run "$forge
              fb /Implementation fa /Implementation get put
              fb execform")
if [ "$honest" = "$stolen" ]; then
    echo "FAIL: the two forms this probe uses paint the same page ($honest),"
    echo "      so a form served the other's drawing would answer what a"
    echo "      form painting its own does and the reading below sees"
    echo "      nothing"
    fail=1
elif [ "$forged" != "$honest" ]; then
    echo "FAIL: a form carrying an Implementation entry the program wrote,"
    echo "      naming a serial issued to another form, painted '$forged'"
    echo "      where painting its own drawing is '$honest' and being served"
    echo "      the other form's is '$stolen'"
    fail=1
fi


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
        raster-device)     dev=$DEVICE_FLEET_RASTER; setup="$FORMDEF" ;;
        clip-holds-box)    dev=pgm;      setup="$FORMDEF
0 0 32 32 rectclip" ;;
        clip-cuts-box)     dev=pgm;      setup="$FORMDEF
0 0 4 4 rectclip" ;;
        screening-device)  dev=$DEVICE_FLEET_SCREENING; setup="$FORMDEF" ;;
        read-only-form)    dev=pgm;      setup="$FORMDEF
/FD FD readonly def" ;;
        vector-device)     dev=$DEVICE_FLEET_VECTOR; setup="$FORMDEF" ;;
        *)  echo "FAIL: the register names a routing condition this check has no"
            echo "      probe for: $cond"
            fail=1
            continue ;;
    esac
    # A condition naming a class of device is asked of every device in
    # it. One of them answering differently is the whole of what this
    # register is for, and a probe that ran the first of them would have
    # recorded the class from a sample of one.
    n=
    for d in $dev; do
        _n=$(runs "$d" "$setup")
        case "$n" in
            '')    n=$_n; nfirst=$d ;;
            "$_n") ;;
            *)     echo "FAIL: $cond is one question and the devices it names do not"
                   echo "      answer it alike: $nfirst answered '$n' and $d answered"
                   echo "      '$_n'. Split the row, or make them agree."
                   fail=1
                   n=$_n ;;
        esac
    done
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

# the identity a cache files by is not the program's XUID: two forms
# carrying one XUID and different drawings paint their own drawings
a=$(run "<< /FormType 1 /BBox [0 0 4 4] /Matrix [1 0 0 1 0 0] /XUID [7]
          /PaintProc { pop 0 setgray 0 0 4 4 rectfill } >> execform
          << /FormType 1 /BBox [0 0 8 8] /Matrix [1 0 0 1 0 0] /XUID [7]
          /PaintProc { pop 0 setgray 0 0 8 8 rectfill } >> execform")
b=$(run "<< /FormType 1 /BBox [0 0 8 8] /Matrix [1 0 0 1 0 0]
          /PaintProc { pop 0 setgray 0 0 8 8 rectfill } >> execform")
[ "$a" = "$b" ] && echo identity-not-the-programs >> "$work/got.diverge"

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
