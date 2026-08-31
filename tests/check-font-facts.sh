#!/bin/sh
#
# Every text operator is asked what it does with every font type.
#
# This family has no dispatch: the type is re-read at thirteen places in
# data/font.ps and two in data/cid.ps, and between them those places ask
# only three questions -- is this a composite, is this a Type 3, does this
# dictionary carry a face. A site that asks half of what it needs looks
# perfectly reasonable on its own, so reading the source finds nothing.
#
# The register therefore asks behaviourally, and holds the whole cross
# product of operator against font type. A site asking half the question
# is then one cell that disagrees with its neighbours -- which is how
# glyphshow taking a composite font was found, sitting beside kshow
# refusing one, where the specification says the same thing about both.
#
# ---- what this holds
#
#   which font types definefont accepts, probed with a face-bearing
#   dictionary since that is what its default arm actually tests
#
#   every operator against every font type it can be handed, held to the
#   register both ways, so an operator that starts or stops refusing a
#   type is reported rather than discovered
#
#   every divergence, each with the probe that finds it
#
# A composite font here is mapped by bytes and is handed a string of as
# many bytes as its mapping consumes. One byte exhausts the selector and
# reports a range error that belongs to the string, not to the operator --
# which is a probe fault that would otherwise be read as five operators
# refusing composites.
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-font-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-font-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"

# the facts below are asked of text a face answers for: a build without
# a face library cannot answer them, and says so rather than failing
. "$(dirname "$0")/verdict.sh"
skip_if_faceless "$xpost" "these facts are asked of text a face answers for"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/font-facts"
guard_require_file "$register" "the register of font facts"
guard_require_file "$src/data/font.ps" "the font machinery"

fail=0
grep -v '^[[:space:]]*#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"
awk '$1 == "type"  { print $2 " " $3 }'            "$work/reg" | sort -n > "$work/reg.type"
awk '$1 == "route" { print $2 " " $3 " " $4 }'     "$work/reg" | sort   > "$work/reg.route"
awk 'NF >= 3 && $2 ~ /^(settled|thorn|heading)$/ { print $1 }' "$work/reg" \
    | sort -u > "$work/reg.diverge"
awk '$1 == "reads" && NF >= 3 { n++ } END { print n+0 }' "$work/reg" > "$work/reg.reads"

[ -s "$work/reg.route" ] || { echo "FAILURES: the register names no route"; exit 1; }

# the four fonts a probe can build or find here, and the string each
# one's mapping consumes
prelude() {
    cat <<'PS'
/S 120 string def
/Base /Helvetica findfont def
/Comp Base maxlength 6 add dict def
Base { Comp 3 1 roll put } forall
Comp /FontType 0 put Comp /FMapType 2 put
Comp /Encoding [ 0 0 ] put
Comp /FDepVector [ Base 12 scalefont ] put
Comp /FontMatrix [1 0 0 1 0 0] put
/F0 Comp /XpostRegComposite exch definefont def
/T3 << /FontType 3 /FontMatrix [0.001 0 0 0.001 0 0]
       /FontBBox [0 0 1000 1000] /Encoding 256 array
       /BuildGlyph { pop pop 1000 0 0 0 1000 1000 setcachedevice
                     0 0 600 700 rectfill } >> def
T3 /Encoding get dup 97 /a put pop
/F3 T3 /XpostRegType3 exch definefont def
/F1 /Courier findfont def
/F42 /DejaVuSans findfont def
PS
}

font() {            # <code> -> the PostScript naming that font
    case $1 in
    0)  printf 'F0' ;;
    1)  printf 'F1' ;;
    3)  printf 'F3' ;;
    42) printf 'F42' ;;
    *)  printf '' ;;
    esac
}
str() {             # <code> -> the string that font's mapping consumes
    case $1 in
    0)  printf '(\\000a)' ;;   # two bytes: a byte-mapped composite takes two
    *)  printf '(a)' ;;
    esac
}
opbody() {          # <operator> -> the body, reading STR for its string
    case $1 in
    show)        printf '5 10 moveto STR show' ;;
    widthshow)   printf '5 10 moveto 0 0 97 STR widthshow' ;;
    ashow)       printf '5 10 moveto 0 0 STR ashow' ;;
    awidthshow)  printf '5 10 moveto 0 0 97 0 0 STR awidthshow' ;;
    kshow)       printf '5 10 moveto { pop pop } STR kshow' ;;
    glyphshow)   printf '5 10 moveto /a glyphshow' ;;
    xshow)       printf '5 10 moveto STR [10] xshow' ;;
    stringwidth) printf 'STR stringwidth pop pop' ;;
    charpath)    printf 'newpath 5 10 moveto STR true charpath' ;;
    cshow)       printf '5 10 moveto { pop pop pop } STR cshow' ;;
    *)           printf '' ;;
    esac
}

# run one case and report ok or the error name
outcome() {         # <fontcode> <operator> -> "ok" or "<errorname>"
    _f=$(font "$1")
    _b=$(opbody "$2")
    {
        printf '<< /PageSize [80 40] >> setpagedevice\n'
        prelude
        printf '/STR %s def\n' "$(str "$1")"
        printf 'gsave mark { %s 12 scalefont setfont %s } stopped\n' "$_f" "$_b"
        printf '{ cleartomark (E ) print $error /errorname get S cvs print (\\n) print }\n'
        printf '{ cleartomark (E ok\\n) print } ifelse grestore\nshowpage\n'
    } > "$work/f.ps"
    ( cd "$work" && XPOST_DATA_DIR="$srcdata" \
      "$xpost" -q --no-sandbox -d pgm -o f.pgm f.ps </dev/null 2>/dev/null ) \
      | awk '$1 == "E" { print $2; exit } END { }'
}

# ---- which types definefont accepts
: > "$work/got.type"
while read -r code verdict; do
    {
        printf '<< /PageSize [80 40] >> setpagedevice\n/S 120 string def\n'
        printf '/Base /Helvetica findfont def\n'
        # The copy leaves out the font data. Which face the host resolved
        # for the name above is not this probe's subject and differs
        # between hosts -- where it resolves to a TrueType face the copy
        # carries the data a Type 42 is read from, and the dictionary
        # stamped 42 below is then a real one and is accepted, where on a
        # host resolving anything else the same probe is refused. Dropping
        # the key makes the dictionary claim a type it has no data for on
        # every host, which is what the register's line for 42 is about.
        printf '/D Base maxlength 8 add dict def\n'
        printf 'Base { 1 index /sfnts eq { pop pop }{ D 3 1 roll put } ifelse } forall\n'
        printf 'D /FontType %s put\n' "$code"
        printf 'D /FMapType 2 put D /Encoding [ 0 0 ] put\n'
        printf 'D /FDepVector [ Base 12 scalefont ] put\n'
        printf 'mark { D /XpostRegT%s exch definefont pop } stopped\n' "$code"
        printf '{ cleartomark (E ) print $error /errorname get S cvs print (\\n) print }\n'
        printf '{ cleartomark (E ok\\n) print } ifelse\nshowpage\n'
    } > "$work/t.ps"
    ans=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
           "$xpost" -q --no-sandbox -d pgm -o t.pgm t.ps </dev/null 2>/dev/null \
           | awk '$1 == "E" { print $2; exit }' )
    case ${ans:-noanswer} in
        ok) echo "$code accepted" >> "$work/got.type" ;;
        *)  echo "$code refused"  >> "$work/got.type" ;;
    esac
done < "$work/reg.type"
sort -n "$work/got.type" -o "$work/got.type"
if ! cmp -s "$work/reg.type" "$work/got.type"; then
    echo "FAIL: the font types definefont accepts are not the ones the"
    echo "      register records:"
    diff "$work/reg.type" "$work/got.type" 2>/dev/null | sed 's/^/      /'
    fail=1
fi

# ---- the cross product
: > "$work/got.route"
while read -r op code verdict; do
    if [ -z "$(font "$code")" ]; then
        echo "FAIL: the register has a route line for font type $code and no"
        echo "      probe can build or find such a font"
        fail=1
        continue
    fi
    if [ -z "$(opbody "$op")" ]; then
        echo "FAIL: the register has a route line for operator '$op' and no"
        echo "      probe exercises it"
        fail=1
        continue
    fi
    echo "$op $code $(outcome "$code" "$op")" >> "$work/got.route"
done < "$work/reg.route"
sort "$work/got.route" -o "$work/got.route"
if ! cmp -s "$work/reg.route" "$work/got.route"; then
    echo "FAIL: what each operator does with each font type is not what the"
    echo "      register records:"
    diff "$work/reg.route" "$work/got.route" 2>/dev/null | sed 's/^/      /'
    echo "      Every one of these sites reads the type for itself, so two"
    echo "      operators the specification says the same thing about are"
    echo "      held together by nothing but this table."
    fail=1
fi

count() {           # <keyword> <how many were derived>
    guard_hold_count "$work/reg" "$1" "$2" || fail=1
}
count types       "$(grep -c . "$work/reg.type")"
count routes      "$(grep -c . "$work/reg.route")"
count divergences "$(grep -c . "$work/reg.diverge")"

# ---- and the type is read nowhere the register does not account for
sites=$(grep -hnE "[^A-Za-z]FontType" "$src/data/font.ps" "$src/data/cid.ps" \
        | grep -cE "get|known" || true)
want=$(cat "$work/reg.reads")
if [ "${sites:-0}" -ne "${want:-0}" ]; then
    echo "FAIL: the font type is read at $sites place(s) in data and the register"
    echo "      accounts for $want. The route question has one home; a new"
    echo "      reader either belongs in it or owes a line in tests/font-facts"
    echo "      saying what it asks instead:"
    grep -nE "[^A-Za-z]FontType" "$src/data/font.ps" "$src/data/cid.ps" \
      | grep -E "get|known" | sed 's|.*/data/|      data/|'
    fail=1
fi

# ---- the divergences, each found by its own probe
: > "$work/got.diverge"
# glyphshow takes a composite where kshow refuses one: the pair is probed
# together, so the line retires only when the two agree
[ "$(outcome 0 glyphshow)" = ok ] && [ "$(outcome 0 kshow)" = invalidfont ] \
    && echo glyphshow-takes-a-composite >> "$work/got.diverge"
# the two operators the specification says the same thing about now say the
# same thing; if either drifts, the route table above reports the cell and
# this pair reports the difference returning

# charpath through a procedure glyph appends only the advance
{
    printf '<< /PageSize [80 40] >> setpagedevice\n'
    prelude
    printf 'F3 20 scalefont setfont newpath 5 10 moveto (a) true charpath\n'
    printf 'mark { pathbbox } stopped\n'
    printf '{ cleartomark (E nopath\\n) print }\n'
    printf '{ pop exch pop sub abs 0.01 lt\n'
    printf '  { (E degenerate\\n) }{ (E outline\\n) } ifelse print cleartomark } ifelse\n'
    printf 'showpage\n'
} > "$work/cp.ps"
cp=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
      "$xpost" -q --no-sandbox -d pgm -o cp.pgm cp.ps </dev/null 2>/dev/null \
      | awk '$1 == "E" { print $2; exit }' )
[ "${cp:-}" = degenerate ] && echo charpath-of-a-procedure-glyph >> "$work/got.diverge"
# definefont takes a type nothing implements
[ "$(awk '$1 == 14 && $2 == "accepted"' "$work/got.type" | wc -l)" -gt 0 ] \
    && echo unknown-type-accepted >> "$work/got.diverge"
sort -u "$work/got.diverge" -o "$work/got.diverge"

# ---- the CIDFontType-to-FontType correspondence, held and probed
#
# The register carries PLRM Table 5.11 and .cidtypemap carries the same
# table in the interpreter, so the two are held to each other; a row added
# to one without the other fails. Then each row is probed through BOTH
# operators the specification requires the stamp of, because one of them
# doing it looks exactly like both from any single test.
sed -n 's|^\.xpostsys /\.cidtypemap[[:space:]]*<<\(.*\)>>.*|\1|p' \
    "$src/data/font.ps" \
    | tr -s ' ' '\n' | grep . | paste - - | LC_ALL=C sort -n > "$work/cid.src"
awk '$1 == "cidtype" && NF >= 3 { print $2 "\t" $3 }' "$work/reg" \
    | LC_ALL=C sort -n > "$work/cid.reg"

ncid=$(grep -c . "$work/cid.reg" || true)
if [ "$ncid" -lt 1 ] || [ "$(grep -c . "$work/cid.src")" -lt 1 ]; then
    echo "FAILURES: the CIDFontType table is empty on one side (register"
    echo "      $ncid, source $(grep -c . "$work/cid.src")). Two empty sets"
    echo "      agree about nothing, so this cannot report on them"
    exit 1
fi
if ! diff_out=$(diff "$work/cid.reg" "$work/cid.src" 2>&1); then
    echo "FAIL: tests/font-facts and .cidtypemap disagree about which"
    echo "      FontType a CIDFontType carries. PLRM Table 5.11 decides"
    echo "      (< register, > interpreter):"
    printf '%s\n' "$diff_out" | sed 's/^/        /'
    fail=1
fi

# the declared count, for the reason every other count here is declared:
# nothing else would stop the section being emptied
ncidsaid=$(awk '/^cidtypes /{ print $2; found = 1 } END { if (!found) print "" }' \
    "$register")
case $ncidsaid in
    ''|*[!0-9]*)
        echo "FAILURES: tests/font-facts has no 'cidtypes <n>' line"
        fail=1 ;;
    *)  if [ "$ncidsaid" -ne "$ncid" ]; then
            echo "FAILURES: tests/font-facts records $ncidsaid CIDFontTypes and holds $ncid"
            fail=1
        fi ;;
esac

{
    printf '/S 40 string def\n'
    printf '/ask {\n'
    printf '  /which exch def /ct exch def\n'
    printf '  /d << /CIDFontType ct /CIDFontName /XpostRegCID\n'
    printf '        /CIDSystemInfo << /Registry (X) /Ordering (X) /Supplement 0 >>\n'
    printf '        /FontMatrix [0.001 0 0 0.001 0 0]\n'
    printf '        /FontBBox [0 0 1000 1000] >> def\n'
    printf '  { which (definefont) eq\n'
    printf '      { /XpostRegCID d definefont pop }\n'
    printf '      { /XpostRegCID d /CIDFont defineresource pop } ifelse\n'
    printf '  } stopped pop\n'
    printf '  (E ) print ct S cvs print ( ) print which print ( ) print\n'
    printf '  d /FontType known { d /FontType get S cvs print }{ (none) print } ifelse\n'
    printf '  (\\n) print flush clear\n'
    printf '} bind def\n'
    while read -r ct ft; do
        printf '%s (definefont) ask\n' "$ct"
        printf '%s (defineresource) ask\n' "$ct"
    done < "$work/cid.reg"
} > "$work/cidstamp.ps"

( cd "$work" && XPOST_DATA_DIR="$srcdata" \
  "$xpost" -q --no-sandbox -d null cidstamp.ps </dev/null 2>/dev/null ) \
  | awk '$1 == "E" && NF == 4 { print $2 "\t" $3 "\t" $4 }' > "$work/cid.got"

: > "$work/cid.want"
while read -r ct ft; do
    printf '%s\tdefinefont\t%s\n' "$ct" "$ft" >> "$work/cid.want"
    printf '%s\tdefineresource\t%s\n' "$ct" "$ft" >> "$work/cid.want"
done < "$work/cid.reg"

if ! diff_out=$(diff "$work/cid.want" "$work/cid.got" 2>&1); then
    echo "FAIL: a CIDFont was not stamped with the FontType its CIDFontType"
    echo "      calls for. PLRM 5.9 requires the entry of definefont AND of"
    echo "      defineresource (< wanted, > got):"
    printf '%s\n' "$diff_out" | sed 's/^/        /'
    fail=1
fi

guard_held=0
guard_hold_divergence font-facts "$work/reg.diverge" "$work/got.diverge"
[ "$guard_held" -eq 0 ] || fail=1

thorns=$(awk 'NF >= 3 && $2 == "thorn" { print "      " $1 }' "$work/reg")
if [ -n "$thorns" ]; then
    echo "THORNS still carried by the font family:"
    printf '%s\n' "$thorns"
fi

[ "$fail" = 0 ] || exit 1
printf 'SUCCESS (%s font type(s) held to what definefont accepts, %s operator-by-type route(s), %s CIDFontType stamp(s) held through both operators, %s divergence(s) each found by its own probe)\n' \
    "$(grep -c . "$work/reg.type")" "$(grep -c . "$work/reg.route")" \
    "$ncid" "$(grep -c . "$work/reg.diverge")"
exit 0
