#!/bin/sh
#
# Every colour space family is accounted for, and each one paints.
#
# PLRM 4.8 defines eleven families. What makes this family worth a
# register is not the count but its history: a member here has twice been
# reached by whichever code its author happened to write, and both times
# what let it happen was a list of members nothing held to another list.
# The component count was written twice and the second copy stopped one
# family short. The cross product of patterns against base spaces listed
# six bases and no CIE family. The characterisation test constructed nine
# members of eleven.
#
# ---- what this holds
#
#   the family set, DERIVED from the roster in data/color.ps and held to
#   the register both ways, so a family added to the roster and left out
#   of the register fails, and a line for a family the roster drops fails
#
#   a CONSTRUCTOR for every family, held to the roster the same way: a
#   family nobody can build is a family nothing can ask a question of,
#   which is exactly the state the two pre-extensions were in
#
#   what a colour in each family is made of, and that a region filled in
#   it PUTS INK ON A PAGE -- counted, not inferred from the absence of an
#   error. A space that is set without complaint and marks nothing is
#   what a half-wired conversion looks like, and it satisfies any check
#   that only asks whether an error was raised
#
#   the lookup-table shapes the two CIE pre-extensions accept, since the
#   array nesting is the whole difference between those two families
#
#   an uncoloured pattern over every family as a base, ink counted --
#   the crossing where this family's two live defects were found
#
#   every divergence, each with the probe that finds it
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-colourspace-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-colourspace-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"

# the facts below are asked of text a face answers for: a build without
# a face library cannot answer them, and says so rather than failing
. "$(dirname "$0")/verdict.sh"
if faceless_build "$xpost"; then
    echo "SKIPPED: these facts are asked of text a face answers for, and this build carries no face library"
    exit 77
fi
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/colourspace-facts"
guard_require_file "$register" "the register of colour space facts"
guard_require_file "$src/data/color.ps" "the colour machinery"

fail=0
grep -v '^[[:space:]]*#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"
awk 'NF >= 5 && $2 ~ /^(device|cie|indexed|tint|pattern)$/ { print $1 " " $2 " " $3 " " $4 }' \
    "$work/reg" | sort > "$work/reg.fam"
awk '$1 == "table" { print $2 " " $3 " " $4 }' "$work/reg" | sort > "$work/reg.table"
awk '$1 == "base"  { print $2 " " $3 }'        "$work/reg" | sort > "$work/reg.base"
awk 'NF >= 3 && $2 ~ /^(settled|thorn|heading)$/ { print $1 }' "$work/reg" \
    | sort -u > "$work/reg.diverge"

[ -s "$work/reg.fam" ] || { echo "FAILURES: the register names no colour space family"; exit 1; }

# ---- the roster, read from the source rather than listed here. The same
#      table tests/check-colour-spaces.sh holds against the two pricing
#      tables, so the two guards cannot disagree about who the family is.
awk '
    /^\.xpostsys \/\.spacekinds <</ { inside = 1; next }
    inside && /^>> put/             { inside = 0 }
    inside && /^[ \t]*\/[A-Za-z]/ {
        gsub(/^[ \t]*\//, ""); v = $2; sub(/^\//, "", v); print $1 " " v
    }' "$src/data/color.ps" | sort > "$work/roster"
if [ ! -s "$work/roster" ]; then
    echo "FAILURES: data/color.ps states no .spacekinds roster where this can"
    echo "      read it. The roster is what membership is derived from;"
    echo "      without it there is nothing to hold the register to."
    exit 1
fi
awk '{ print $1 }' "$work/roster" | sort > "$work/roster.names"
awk '{ print $1 }' "$work/reg.fam" | sort > "$work/reg.names"

guard_held=0
guard_hold "$work/reg.names" "$work/roster.names" \
    "named in the register and absent from the roster in data/color.ps.
      Retire the line and the count with it:" \
    "in the roster and named by no line in the register. A family a
      program can select is one whose differences from the rest have to
      be written down; add it to tests/colourspace-facts:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- a constructor per family. Hand-written, because a valid space
#      cannot be synthesised from a family name -- a CIE pre-extension
#      needs a lookup table of the right shape, an Indexed space a
#      palette. Held to the roster, so a family arriving without one is
#      reported rather than skipped.
WP='/WhitePoint [0.9505 1.0 1.089]'
DEFTAB='[ <000000FF000000FF000000FFFF> <FFFFFF808080404040000000FF> ]'
DEFGTAB='[ [ <000000FF000000FF000000FFFF> <FFFFFF808080404040000000FF> ]
           [ <101010202020303030404040FF> <505050606060707070808080FF> ] ]'
space() {           # <family> -> the space, or empty if none is written
    case $1 in
    DeviceGray)   printf '/DeviceGray' ;;
    DeviceRGB)    printf '/DeviceRGB' ;;
    DeviceCMYK)   printf '/DeviceCMYK' ;;
    CIEBasedA)    printf '[ /CIEBasedA << %s >> ]' "$WP" ;;
    CIEBasedABC)  printf '[ /CIEBasedABC << %s >> ]' "$WP" ;;
    CIEBasedDEF)  printf '[ /CIEBasedDEF << %s /Table [ 2 2 2 %s ] >> ]' "$WP" "$DEFTAB" ;;
    CIEBasedDEFG) printf '[ /CIEBasedDEFG << %s /Table [ 2 2 2 2 %s ] >> ]' "$WP" "$DEFGTAB" ;;
    Indexed)      printf '[ /Indexed /DeviceRGB 1 <FF000000FF00> ]' ;;
    Separation)   printf '[ /Separation /Spot /DeviceGray { } ]' ;;
    DeviceN)      printf '[ /DeviceN [ /A /B ] /DeviceRGB { pop pop 0.0 0.0 0.0 } ]' ;;
    Pattern)      printf '[ /Pattern ]' ;;
    *)            printf '' ;;
    esac
}
# A colour in each family that marks. Hand-written for the same reason
# the constructor is, and for one more: a component of zero is not ink
# everywhere -- four zeros in DeviceCMYK is white, and a probe that
# assumed zero would report the family mute and be believed.
colour() {          # <family> -> operands for setcolor
    case $1 in
    DeviceGray)   printf '0' ;;
    DeviceRGB)    printf '0 0 0' ;;
    DeviceCMYK)   printf '0 0 0 1' ;;
    CIEBasedA)    printf '0' ;;
    CIEBasedABC)  printf '0 0 0' ;;
    CIEBasedDEF)  printf '0.2 0.3 0.4' ;;
    CIEBasedDEFG) printf '0.2 0.3 0.4 0.5' ;;
    Indexed)      printf '0' ;;
    Separation)   printf '0' ;;
    DeviceN)      printf '0.5 0.5' ;;
    Pattern)      printf '<< /PatternType 1 /PaintType 1 /TilingType 1
                            /BBox [0 0 4 4] /XStep 4 /YStep 4
                            /PaintProc { pop 0 setgray 0 0 2 2 rectfill } >>
                          matrix makepattern' ;;
    *)            printf '' ;;
    esac
}
: > "$work/built"
while read -r fam; do
    [ -n "$(space "$fam")" ] && echo "$fam" >> "$work/built"
done < "$work/roster.names"
sort "$work/built" -o "$work/built"
guard_held=0
guard_hold "$work/built" "$work/roster.names" \
    "built by a constructor here and absent from the roster. Retire the
      constructor:" \
    "in the roster and built by no constructor here, so nothing in this
      guard can ask it a single question. Write one in space():"
[ "$guard_held" -eq 0 ] || fail=1

# ---- what a program is told it may use
#
# The set above is what this interpreter builds. What a PROGRAM is told
# it may use is a second statement of the same fact: the /ColorSpaceFamily resource
# category (PLRM Table 3.8), whose instances are a list written by hand
# in data/color.ps.
#
# A hand-written list of what the code can do is the shape that drifts,
# and holding it to another list cannot catch the drift that matters --
# something missing from both reads exactly like something that does not
# exist. The set above is read off the interpreter, so holding the
# declaration to it holds a claim against behaviour rather than against
# a second claim.
# The roster this is held to is itself held, just above, to the spaces
# the interpreter actually builds, so a family here is one that works.
printf '(*) { =only (\\n) print } 32 string /ColorSpaceFamily resourceforall\n' > "$work/decl.ps"
XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/decl.ps" </dev/null 2>/dev/null \
    | tr -d '\r' | awk 'NF == 1 { print }' | LC_ALL=C sort > "$work/decl.set"
if [ ! -s "$work/decl.set" ]; then
    echo "FAILURES: the /ColorSpaceFamily category named no instances at all. A"
    echo "      category that answers nothing cannot be held to anything,"
    echo "      and an empty answer reads exactly like one that is"
    echo "      correctly empty."
    exit 1
fi
guard_held=0
guard_hold "$work/roster.names" "$work/decl.set" \
    "builds by this interpreter and not offered as a /ColorSpaceFamily resource. A
      program asking what it may use is told less than the truth; the
      list in data/color.ps is where to say so:" \
    "offered as a /ColorSpaceFamily resource and not builds by this interpreter. A
      program is promised something this interpreter refuses:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- run a program and report what it printed, plus the ink it laid
run() {             # <body> -> "<errorname>" or "ink <n>"
    {
        printf '<< /PageSize [40 40] >> setpagedevice\n/S 200 string def\n'
        printf '{ %s } stopped\n' "$1"
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

# how many components the space is entered holding, which is how many
# setcolor takes
ncomp() {           # <family> -> a number, or an error name
    {
        printf '/S 200 string def\n'
        printf 'mark { %s setcolorspace currentcolor } stopped\n' "$(space "$1")"
        printf '{ cleartomark (E ) print $error /errorname get S cvs print (\\n) print }\n'
        printf '{ counttomark S cvs (N ) print print (\\n) print cleartomark } ifelse\n'
    } > "$work/n.ps"
    ( cd "$work" && XPOST_DATA_DIR="$srcdata" \
      "$xpost" -q --no-sandbox -d null n.ps </dev/null 2>/dev/null ) | awk '
        $1 == "N" { print $2; found = 1; exit }
        $1 == "E" { print $2; found = 1; exit }
        END { if (!found) print "noanswer" }'
}

# ---- what each family is made of, and that it paints
: > "$work/got.fam"
while read -r fam kind comps verdict; do
    sp=$(space "$fam")
    if [ -z "$sp" ]; then
        echo "FAIL: the register names $fam and space() cannot build it"
        fail=1
        continue
    fi
    k=$(awk -v F="$fam" '$1 == F { print $2 }' "$work/roster")
    n=$(ncomp "$fam")
    case $n in
        ''|*[!0-9]*)
            echo "FAIL: $fam does not answer how many components it holds: '$n'"
            echo "      currentcolor in a freshly set space is what states the"
            echo "      count, so a space that cannot be asked cannot be priced."
            fail=1
            echo "$fam ${k:-nokind} $n unmeasured" >> "$work/got.fam"
            continue ;;
    esac
    case $(run "$sp setcolorspace $(colour "$fam") setcolor 5 5 30 30 rectfill") in
        blank) v=mute ;;
        ink\ *) v=paints ;;
        *)      v=$(run "$sp setcolorspace $(colour "$fam") setcolor 5 5 30 30 rectfill") ;;
    esac
    echo "$fam ${k:-nokind} $n $v" >> "$work/got.fam"
done < "$work/reg.fam"
sort "$work/got.fam" -o "$work/got.fam"
if ! cmp -s "$work/reg.fam" "$work/got.fam"; then
    echo "FAIL: what the families are is not what the register records:"
    diff "$work/reg.fam" "$work/got.fam" 2>/dev/null | sed 's/^/      /'
    echo "      A family that stops painting still passes every check that"
    echo "      only asks whether an error was raised."
    fail=1
fi

# ---- the CIE pre-extension table shapes
tablecase() {       # <family> <case> -> the space
    case $1-$2 in
    CIEBasedDEF-well-formed)    printf '[ /CIEBasedDEF << %s /Table [ 2 2 2 %s ] >> ]' "$WP" "$DEFTAB" ;;
    CIEBasedDEF-short-strings)  printf '[ /CIEBasedDEF << %s /Table [ 2 2 2 [ <0000> <FFFF> ] ] >> ]' "$WP" ;;
    CIEBasedDEF-short-array)    printf '[ /CIEBasedDEF << %s /Table [ 9 2 2 %s ] >> ]' "$WP" "$DEFTAB" ;;
    CIEBasedDEFG-well-formed)   printf '[ /CIEBasedDEFG << %s /Table [ 2 2 2 2 %s ] >> ]' "$WP" "$DEFGTAB" ;;
    CIEBasedDEFG-flat-array)    printf '[ /CIEBasedDEFG << %s /Table [ 2 2 2 2 %s ] >> ]' "$WP" "$DEFTAB" ;;
    CIEBasedDEFG-short-strings) printf '[ /CIEBasedDEFG << %s /Table [ 2 2 2 9 %s ] >> ]' "$WP" "$DEFGTAB" ;;
    *)                          printf '' ;;
    esac
}
: > "$work/got.table"
while read -r fam kase verdict; do
    sp=$(tablecase "$fam" "$kase")
    if [ -z "$sp" ]; then
        echo "FAIL: the register has a table line for $fam $kase and no probe builds it"
        fail=1
        continue
    fi
    n=$(awk -v F="$fam" '$1 == F { print $3 }' "$work/reg.fam")
    ans=$(run "$sp setcolorspace $(
        i=0; while [ "$i" -lt "${n:-3}" ]; do printf '0.2 '; i=$((i+1)); done
        ) setcolor 5 5 30 30 rectfill")
    case $ans in
        blank) echo "$fam $kase mute" >> "$work/got.table" ;;
        ink\ *) echo "$fam $kase takes" >> "$work/got.table" ;;
        *)      echo "$fam $kase refuses" >> "$work/got.table" ;;
    esac
done < "$work/reg.table"
sort "$work/got.table" -o "$work/got.table"
if ! cmp -s "$work/reg.table" "$work/got.table"; then
    echo "FAIL: the table shapes accepted are not the ones the register records:"
    diff "$work/reg.table" "$work/got.table" 2>/dev/null | sed 's/^/      /'
    echo "      The array nesting is the whole difference between the two"
    echo "      pre-extension families, so a reader that stops telling them"
    echo "      apart reads one family's table as the other's."
    fail=1
fi

# ---- an uncoloured pattern over every family as a base
: > "$work/got.base"
while read -r fam verdict; do
    sp=$(space "$fam")
    if [ -z "$sp" ]; then
        echo "FAIL: the register has a base line for $fam and space() cannot build it"
        fail=1
        continue
    fi
    n=$(awk -v F="$fam" '$1 == F { print $3 }' "$work/reg.fam")
    comps=$(i=0; while [ "$i" -lt "${n:-1}" ]; do printf '0.5 '; i=$((i+1)); done)
    ans=$(run "[ /Pattern $sp ] setcolorspace
        << /PatternType 1 /PaintType 2 /TilingType 1 /BBox [0 0 4 4]
           /XStep 4 /YStep 4 /PaintProc { pop 0 0 2 2 rectfill } >>
        matrix makepattern $comps $((${n:-1} + 1)) -1 roll setcolor
        5 5 30 30 rectfill")
    case $ans in
        blank) echo "$fam mute" >> "$work/got.base" ;;
        ink\ *) echo "$fam paints" >> "$work/got.base" ;;
        *)      echo "$fam $ans" >> "$work/got.base" ;;
    esac
done < "$work/reg.base"
sort "$work/got.base" -o "$work/got.base"
if ! cmp -s "$work/reg.base" "$work/got.base"; then
    echo "FAIL: the bases an uncoloured pattern paints over are not the ones"
    echo "      the register records:"
    diff "$work/reg.base" "$work/got.base" 2>/dev/null | sed 's/^/      /'
    echo "      Two live defects were found in this product, one at a time,"
    echo "      and each time the list that should have caught it was a list"
    echo "      of bases somebody wrote out."
    fail=1
fi

count() {           # <keyword> <how many were derived>
    guard_hold_count "$work/reg" "$1" "$2" || fail=1
}
count families    "$(grep -c . "$work/reg.fam")"
count tables      "$(grep -c . "$work/reg.table")"
count bases       "$(grep -c . "$work/reg.base")"
count divergences "$(grep -c . "$work/reg.diverge")"

# ---- the divergences, each found by its own probe
: > "$work/got.diverge"
[ "$(run "[ /CIEBasedA << >> ] setcolorspace")" = undefined ] \
    && echo missing-required >> "$work/got.diverge"
# is the array currentcolorspace answers the one the program handed in?
{
    printf '[ /DeviceRGB ] dup setcolorspace currentcolorspace eq\n'
    printf '{ (SAME\\n) print }{ (REBUILT\\n) print } ifelse\n'
} > "$work/id.ps"
identity=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
            "$xpost" -q --no-sandbox -d null id.ps </dev/null 2>/dev/null \
            | awk '$1 == "SAME" || $1 == "REBUILT" { print $1; exit }' )
[ "${identity:-}" = REBUILT ] && echo space-array-rebuilt >> "$work/got.diverge"
case $(run "[ /DeviceN [ /None ] /DeviceGray { } ] setcolorspace
            0.5 setcolor 5 5 30 30 rectfill") in
    blank) ;;
    ink\ *) echo special-names-in-devicen >> "$work/got.diverge" ;;
esac
# ---- the None colorant, asked of every painting operator
#
# PLRM 4.8.5: painting in a Separation named None "has no effect on the
# current page". Asked of every operator that can mark, because that is
# the only way the statement can be true -- a colour honoured by the
# operators somebody remembered is a colour that marks through the rest,
# and which ones were remembered is invisible from any single test.
#
# Two operators are expected to mark and are right to. The operand form of
# image carries grey samples and a shading carries its own ColorSpace, so
# neither paints in the current space at all, and a None separation being
# current says nothing about either. Both are held to MARKING for the same
# reason the others are held to silence: if one of them ever went quiet
# under a colour that has nothing to do with it, that would be the defect.
#
# Every case carries an inking control in an ordinary colour. A case whose
# control is blank proves nothing about the colour, and would otherwise
# read as a success.
nonefam='[ /Separation /None /DeviceGray { 1 exch sub } ] setcolorspace 1 setcolor'
glyph='/Courier findfont 40 scalefont setfont 5 8 moveto'
none_case() {
    case $1 in
      fill)       echo 'newpath 5 5 moveto 35 5 lineto 35 35 lineto closepath fill' ;;
      eofill)     echo 'newpath 5 5 moveto 35 5 lineto 35 35 lineto closepath eofill' ;;
      stroke)     echo 'newpath 5 20 moveto 35 20 lineto 10 setlinewidth stroke' ;;
      rectfill)   echo '5 5 30 30 rectfill' ;;
      rectstroke) echo '10 setlinewidth 5 5 30 30 rectstroke' ;;
      show)       echo "$glyph (M) show" ;;
      ashow)      echo "$glyph 0 0 (M) ashow" ;;
      widthshow)  echo "$glyph 0 0 8#115 (M) widthshow" ;;
      xshow)      echo "$glyph (M) [20] xshow" ;;
      glyphshow)  echo "$glyph /M glyphshow" ;;
      imagemask)  echo 'gsave 5 5 translate 30 30 scale 4 4 true [4 0 0 -4 0 4] <FFFFFFFF> imagemask grestore' ;;
      image)      echo 'gsave 5 5 translate 30 30 scale 4 4 8 [4 0 0 -4 0 4] <00000000000000000000000000000000> image grestore' ;;
      shfill)     echo 'gsave newpath 5 5 30 30 rectclip << /ShadingType 2 /ColorSpace /DeviceGray /Coords [5 5 35 35] /Function << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [0] /N 1 >> >> shfill grestore' ;;
    esac
}
nsilent=0
for op in fill eofill stroke rectfill rectstroke \
          show ashow widthshow xshow glyphshow imagemask \
          image shfill; do
    body=$(none_case "$op")
    case $op in
        image|shfill) want=marks ;;
        *)            want=silent ;;
    esac
    got=$(run "$nonefam
               $body")
    ctl=$(run "0 setgray
               $body")
    case $ctl in
        blank)
            echo "FAIL: the $op control laid no ink in an ordinary colour, so"
            echo "      this cannot report on what the None colorant does to it"
            fail=1
            continue ;;
        ink\ *) ;;
        *)  echo "FAIL: the $op control answered '$ctl' rather than ink"
            fail=1
            continue ;;
    esac
    case $got in
        blank) verdict=silent ;;
        ink\ *) verdict=marks ;;
        *)      verdict=$got ;;
    esac
    if [ "$verdict" != "$want" ]; then
        echo "FAIL: under a Separation named None, $op $verdict where it must"
        echo "      be $want. PLRM 4.8.5 gives that colorant no visible"
        echo "      output; image in its operand form and shfill are the two"
        echo "      that do not paint in the current space and must still mark."
        fail=1
    else
        nsilent=$((nsilent + 1))
    fi
done

sort -u "$work/got.diverge" -o "$work/got.diverge"

guard_held=0
guard_hold_divergence colourspace-facts "$work/reg.diverge" "$work/got.diverge"
[ "$guard_held" -eq 0 ] || fail=1

thorns=$(awk 'NF >= 3 && $2 == "thorn" { print "      " $1 }' "$work/reg")
if [ -n "$thorns" ]; then
    echo "THORNS still carried by the colour space family:"
    printf '%s\n' "$thorns"
fi

[ "$fail" = 0 ] || exit 1
printf 'SUCCESS (%s families derived from the roster and each one painting, %s table shape(s), %s base(s) under an uncoloured pattern, %s divergence(s) each found by its own probe)\n' \
    "$(grep -c . "$work/reg.fam")" "$(grep -c . "$work/reg.table")" \
    "$(grep -c . "$work/reg.base")" "$(grep -c . "$work/reg.diverge")"
exit 0
