#!/bin/sh
#
# Every image type is accounted for, and a sample width is either read
# correctly or refused.
#
# PLRM 4.10.5 defines three image types. What makes the family worth a
# register is the reading: an image's samples are a bit stream, as a
# sampled function's samples and a mesh shading's vertices are, and those
# two are read by one reader with an allowed-width table that refuses
# anything outside it. The image path had a third reader with no table at
# all, so widths it could not read were read as though they were twelve.
#
# ---- what this holds
#
#   the type set, DERIVED by offering every code from -3 to 20 and
#   painting with what is accepted
#
#   every width, PROBED with the samples packed the way that width
#   requires -- data of the wrong shape runs out, and running out raises
#   what being refused raises, so a probe reusing one string would report
#   every width as unsupported. A width that is taken is checked against
#   the RAMP it should produce, not merely against not raising: reading
#   sixteen bits as twelve does not fail, it paints the wrong page
#
#   which entries a type 1 dictionary requires
#
#   that a required entry comes from the image dictionary and from
#   nowhere else. The dictionary is made current to be read, which puts
#   it above whatever the program already had on the dictionary stack; an
#   entry read as a bare name is then satisfied by any enclosing
#   dictionary defining that name, and the image paints from a value that
#   was never part of it
#
#   every divergence, each with the probe that finds it
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-image-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-image-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/image-facts"
guard_require_file "$register" "the register of image facts"
guard_require_file "$src/data/paint.ps" "the image machinery"

fail=0
grep -v '^[[:space:]]*#' "$register" | grep -v '^[[:space:]]*$' > "$work/reg"
awk '$1 ~ /^-?[0-9]+$/ && $2 == "accepted" { print $1 }' "$work/reg" | sort -n > "$work/reg.type"
awk '$1 == "width"    { print $2 " " $3 }'            "$work/reg" | sort -n > "$work/reg.width"
awk '$1 == "entry"    { print $2 " " $3 }'            "$work/reg" | sort   > "$work/reg.entry"
awk '$1 == "isolated" { print $2 " " $3 " " $4 }'     "$work/reg" | sort   > "$work/reg.iso"
awk 'NF >= 3 && $2 ~ /^(settled|thorn|heading)$/ { print $1 }' "$work/reg" \
    | sort -u > "$work/reg.diverge"

[ -s "$work/reg.type" ] || { echo "FAILURES: the register names no image type"; exit 1; }

# eight samples spanning the width's whole range, MSB first, the row
# padded to a byte as PLRM 4.10.2 requires
samples() {         # <bits> -> hex
    awk -v B="$1" 'BEGIN {
        n = 8; max = 2 ^ B - 1; bits = ""
        for (k = 0; k < n; k++) {
            v = int(k * max / (n - 1) + 0.5)
            for (i = B - 1; i >= 0; i--) bits = bits sprintf("%d", int(v / 2 ^ i) % 2)
        }
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

# the eight block centres of a 64x8 page, or an error name
centres() {         # <body> -> "<errorname>" or "v v v v v v v v"
    {
        printf '<< /PageSize [64 8] >> setpagedevice\n/S 200 string def\n'
        printf '/DeviceGray setcolorspace\n64 8 scale\n'
        printf '{ %s } stopped\n' "$1"
        printf '{ (E ) print $error /errorname get S cvs print (\\n) print clear }\n'
        printf '{ (E none\\n) print } ifelse\nshowpage\n'
    } > "$work/c.ps"
    rm -f "$work/c.pgm"
    _e=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
          "$xpost" -q --no-sandbox -d pgm -o c.pgm c.ps </dev/null 2>/dev/null \
          | awk '$1 == "E" { print $2; exit }' )
    [ "${_e:-}" = none ] || { printf '%s' "${_e:-noanswer}"; return; }
    guard_pnm_pixels "$work/c.pgm" |
        awk 'NR % 8 == 5 && k < 8 { s = s (k++ ? " " : "") $1 } END { print s }'
}

img() {             # <ImageType> <bits> <hex> -> a dictionary
    printf '<< /ImageType %s /Width 8 /Height 1 /BitsPerComponent %s
               /Decode [0 1] /ImageMatrix [8 0 0 -1 0 1] /DataSource <%s> >>' \
           "$1" "$2" "$3"
}

# ---- the type set, derived. A type 3 or 4 dictionary wraps a type 1, so
#      each is offered the shape its own table asks for and only the code
#      under test varies within that shape.
typedict() {        # <code> -> a dictionary of that code
    _d=$(img 1 8 "$(samples 8)")
    case $1 in
    3) printf '<< /ImageType 3 /InterleaveType 3 /DataDict %s
                  /MaskDict << /ImageType 1 /Width 8 /Height 1
                               /BitsPerComponent 1 /Decode [0 1]
                               /ImageMatrix [8 0 0 -1 0 1]
                               /DataSource <FF> >> >>' "$_d" ;;
    4) printf '<< /ImageType 4 /Width 8 /Height 1 /BitsPerComponent 8
                  /Decode [0 1] /ImageMatrix [8 0 0 -1 0 1]
                  /DataSource <%s> /MaskColor [200 210] >>' "$(samples 8)" ;;
    *) printf '<< /ImageType %s /Width 8 /Height 1 /BitsPerComponent 8
                  /Decode [0 1] /ImageMatrix [8 0 0 -1 0 1]
                  /DataSource <%s>
                  /InterleaveType 3 /MaskColor [200 210]
                  /DataDict %s
                  /MaskDict << /ImageType 1 /Width 8 /Height 1
                               /BitsPerComponent 1 /Decode [0 1]
                               /ImageMatrix [8 0 0 -1 0 1]
                               /DataSource <FF> >> >>' "$1" "$(samples 8)" "$_d" ;;
    esac
}
: > "$work/got.type"
t=-3
while [ "$t" -le 20 ]; do
    ans=$(centres "$(typedict $t) image")
    case $ans in
        rangecheck|typecheck|undefined) ;;
        *\ *) echo "$t" >> "$work/got.type" ;;
        *)  echo "FAIL: image type $t answered '$ans', which is neither painting"
            echo "      nor one of the three refusals"
            fail=1 ;;
    esac
    t=$((t + 1))
done
sort -n "$work/got.type" -o "$work/got.type"

guard_held=0
guard_hold "$work/reg.type" "$work/got.type" \
    "in the register and no longer painting. Retire the line and the
      count with it:" \
    "painting and named by no line in the register. A type a program can
      ask for is one whose differences from the rest have to be written
      down; add it to tests/image-facts:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- what a program is told it may use
#
# The set above is what this interpreter paints. What a PROGRAM is told
# it may use is a second statement of the same fact: the /ImageType resource
# category (PLRM Table 3.8), whose instances are a list written by hand
# in data/paint.ps.
#
# A hand-written list of what the code can do is the shape that drifts,
# and holding it to another list cannot catch the drift that matters --
# a type missing from both reads exactly like one that does not exist.
# The set above is read off the interpreter, so holding the declaration
# to it holds a claim against behaviour rather than against a second
# claim.
printf '(*) { =only (\\n) print } 16 string /ImageType resourceforall\n' > "$work/decl.ps"
XPOST_DATA_DIR="$srcdata" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/decl.ps" </dev/null 2>/dev/null \
    | tr -d '\r' | awk 'NF == 1 { print }' | sort -n > "$work/decl.set"
if [ ! -s "$work/decl.set" ]; then
    echo "FAILURES: the /ImageType category named no instances at all. A"
    echo "      category that answers nothing cannot be held to anything,"
    echo "      and an empty answer reads exactly like one that is"
    echo "      correctly empty."
    exit 1
fi
guard_held=0
guard_hold "$work/got.type" "$work/decl.set" \
    "paints by this interpreter and not offered as a /ImageType resource. A
      program asking what it may use is told less than the truth; the
      list in data/paint.ps is where to say so:" \
    "offered as a /ImageType resource and not paints by this interpreter. A
      program is promised something this interpreter refuses:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the widths. A width that is taken must produce the ramp its
#      samples describe; the eight values are the range's ends and the
#      six steps between, so any misreading moves at least one of them.
# the bytes those samples must normalise to at this width: the same
# eight values the packer wrote, scaled to the byte range. At one bit
# the ramp is two levels and at two bits four, so the vector to expect
# is the width's own -- comparing every width to the eight-bit ramp
# would report the narrow widths as misread when they are exact.
ramp() {            # <bits> -> "v v v v v v v v"
    awk -v B="$1" 'BEGIN {
        n = 8; max = 2 ^ B - 1; s = ""
        for (k = 0; k < n; k++) {
            v = int(k * max / (n - 1) + 0.5)
            s = s (k ? " " : "") int(v * 255 / max + 0.5)
        }
        print s
    }'
}
: > "$work/got.width"
while read -r bits verdict; do
    [ "$bits" -gt 0 ] 2>/dev/null && want_ramp=$(ramp "$bits") || want_ramp="-"
    ans=$(centres "$(img 1 "$bits" "$(samples "$bits")") image")
    case $ans in
        rangecheck|typecheck|undefined) echo "$bits refuses" >> "$work/got.width" ;;
        "$want_ramp")                   echo "$bits takes"   >> "$work/got.width" ;;
        *\ *) echo "$bits misreads" >> "$work/got.width"
              echo "NOTE: width $bits paints [$ans] where its samples describe"
              echo "      [$want_ramp]" ;;
        *)    echo "$bits $ans" >> "$work/got.width" ;;
    esac
done < "$work/reg.width"
sort -n "$work/got.width" -o "$work/got.width"
if ! cmp -s "$work/reg.width" "$work/got.width"; then
    echo "FAIL: the widths a sample may be are not the widths the register"
    echo "      records:"
    diff "$work/reg.width" "$work/got.width" 2>/dev/null | sed 's/^/      /'
    echo "      A width that is accepted and read as another width does not"
    echo "      fail; it paints the wrong page."
    fail=1
fi

# ---- which entries a type 1 dictionary requires
: > "$work/got.entry"
while read -r ent req; do
    ans=$(centres "$(img 1 8 "$(samples 8)") dup /$ent undef image")
    case $ans in
        *\ *) echo "$ent optional" >> "$work/got.entry" ;;
        *)    echo "$ent required" >> "$work/got.entry" ;;
    esac
done < "$work/reg.entry"
sort "$work/got.entry" -o "$work/got.entry"
if ! cmp -s "$work/reg.entry" "$work/got.entry"; then
    echo "FAIL: the entries an image requires are not the ones the register"
    echo "      records:"
    diff "$work/reg.entry" "$work/got.entry" 2>/dev/null | sed 's/^/      /'
    fail=1
fi

# ---- a required entry comes from the image dictionary and nowhere else
: > "$work/got.iso"
while read -r op ent verdict; do
    case $op in
    image)
        body="/$ent $(case $ent in
                        Width|Height) echo 8 ;;
                        BitsPerComponent) echo 8 ;;
                        ImageMatrix) echo '[8 0 0 -1 0 1]' ;;
                        Decode) echo '[0 1]' ;;
                        *) echo 1 ;;
                      esac) def
              $(img 1 8 "$(samples 8)") dup /$ent undef image" ;;
    imagemask)
        body="/$ent $(case $ent in
                        Width) echo 8 ;;
                        Height) echo 1 ;;
                        ImageType) echo 1 ;;
                        ImageMatrix) echo '[8 0 0 -1 0 1]' ;;
                        Decode) echo '[0 1]' ;;
                        *) echo 1 ;;
                      esac) def
              << /ImageType 1 /Width 8 /Height 1 /Decode [0 1]
                 /ImageMatrix [8 0 0 -1 0 1] /DataSource <AA> >>
              dup /$ent undef imagemask" ;;
    *)  echo "FAIL: the register has an isolated line for operator '$op'"
        fail=1
        continue ;;
    esac
    ans=$(centres "$body")
    case $ans in
        *\ *) echo "$op $ent leaks"    >> "$work/got.iso" ;;
        *)    echo "$op $ent isolated" >> "$work/got.iso" ;;
    esac
done < "$work/reg.iso"
sort "$work/got.iso" -o "$work/got.iso"
if ! cmp -s "$work/reg.iso" "$work/got.iso"; then
    echo "FAIL: whether a required entry can be satisfied from outside the"
    echo "      image dictionary is not what the register records:"
    diff "$work/reg.iso" "$work/got.iso" 2>/dev/null | sed 's/^/      /'
    echo "      An entry read as a bare name is taken from whatever the"
    echo "      program had on the dictionary stack, so the image paints"
    echo "      from a value that was never part of it."
    fail=1
fi

count() {           # <keyword> <how many were derived>
    guard_hold_count "$work/reg" "$1" "$2" || fail=1
}
count types       "$(grep -c . "$work/reg.type")"
count widths      "$(grep -c . "$work/reg.width")"
count entries     "$(grep -c . "$work/reg.entry")"
count isolateds   "$(grep -c . "$work/reg.iso")"
count divergences "$(grep -c . "$work/reg.diverge")"

# ---- the divergences, each found by its own probe
: > "$work/got.diverge"
[ "$(centres "$(img 1 8 "$(samples 8)") dup /Width undef image")" = undefined ] \
    && echo missing-required >> "$work/got.diverge"
# a stencil Decode of the right length carrying values that are neither
# of the two the specification allows: accepted, and its first element
# stands for the whole
case $(centres "<< /ImageType 1 /Width 8 /Height 1 /BitsPerComponent 1
                   /Decode [0.5 1] /ImageMatrix [8 0 0 -1 0 1]
                   /DataSource <AA> >> imagemask") in
    *\ *) echo stencil-decode-not-held-to-its-two-values >> "$work/got.diverge" ;;
esac
# and more than one data source, which this refuses where both oracles do not
case $(centres "<< /ImageType 1 /Width 8 /Height 1 /BitsPerComponent 1
                   /MultipleDataSources true
                   /Decode [0 1] /ImageMatrix [8 0 0 -1 0 1]
                   /DataSource [ <AA> ] >> imagemask") in
    typecheck) echo stencil-strictness-differs-from-both >> "$work/got.diverge" ;;
esac
sort -u "$work/got.diverge" -o "$work/got.diverge"

guard_held=0
guard_hold_divergence image-facts "$work/reg.diverge" "$work/got.diverge"
[ "$guard_held" -eq 0 ] || fail=1

thorns=$(awk 'NF >= 3 && $2 == "thorn" { print "      " $1 }' "$work/reg")
if [ -n "$thorns" ]; then
    echo "THORNS still carried by the image family:"
    printf '%s\n' "$thorns"
fi

[ "$fail" = 0 ] || exit 1
printf 'SUCCESS (%s image type(s) held to what paints, %s width(s) each checked against its own ramp, %s entry requirement(s), %s entry isolation(s), %s divergence(s) each found by its own probe)\n' \
    "$(grep -c . "$work/reg.type")" "$(grep -c . "$work/reg.width")" \
    "$(grep -c . "$work/reg.entry")" "$(grep -c . "$work/reg.iso")" \
    "$(grep -c . "$work/reg.diverge")"
exit 0
