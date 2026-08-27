#!/bin/sh
#
# The colour space families agree about which families there are.
#
# A colour space belongs to a family, and three tables in data/color.ps
# say something about those families: .spacekinds says what kind of thing
# each one is, .spacecomps gives the component count of the device
# spaces, and .ciecomps gives it for the CIE-based ones. A family in one
# table and missing from another is a family the machinery half knows.
#
# That is not a hypothetical failure mode here. The count of components a
# colour carries was written twice, and the second copy knew the indexed
# and tint families and not the CIE ones, so an uncoloured pattern over a
# CIE base asked the device table for a name that is not in it and
# raised. The copies are gone, but what let them diverge was that nothing
# held the family lists to each other.
#
# So the three are held here, in BOTH directions:
#
#   a family of kind device must be priced by .spacecomps, and every
#   family .spacecomps prices must be of kind device
#
#   a family of kind cie must be priced by .ciecomps, and every family
#   .ciecomps prices must be of kind cie
#
#   a family whose colour is reached by running something -- indexed,
#   tint -- and the pattern family must be priced by neither, since
#   their count comes from their own parameters rather than from a table
#
#   every kind named is one of the five, so a table entry cannot invent
#   a sixth kind that nothing dispatches on
#
#   every kind that is a special colour space -- indexed, tint, pattern,
#   the three built over another space -- states in .spaceunder what may
#   not sit beneath it, and only such a kind states one. A special space
#   with no line there is one the composition rules of PLRM 4.8.4, 4.8.5
#   and 4.9.2 cannot reach
#
#   every family of kind cie states in .ciedecodes which decode entries
#   it carries, and only such a family states one. A CIE family with no
#   line there is one whose decode entries are taken whatever type they
#   are
#
# The kinds table is the roster. Adding a family means adding it there,
# and this then asks whether the rest of the family's statements were
# made -- which is the thing that was missing when four of the eleven
# families existed only as equality tests scattered through the code.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-colour-spaces.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$src/data/color.ps" "the colour machinery"

guard_workdir
guard_mirror colourspaces "$src/data/color.ps"
color="$mirror/color.ps"

fail=0

# Read a << /Name value ... >> table that follows a named put, taking the
# entries between the braces. The name is matched at the start of a line
# so that a mention of the table elsewhere cannot be read as its
# definition.
table() {                               # $1 table name -> "name value" lines
    awk -v want="$1" '
        $0 ~ ("^\\.xpostsys /" want " <<") { inside = 1; next }
        inside && /^>> put/               { inside = 0 }
        inside && /^[ \t]*\/[A-Za-z]/ {
            gsub(/^[ \t]*\//, "")
            v = $2; sub(/^\//, "", v)
            print $1, v
        }
    ' "$color"
}

table .spacekinds > "$work/kinds"
table .spacecomps > "$work/comps"
table .ciecomps   > "$work/cie"
table .spaceunder > "$work/under"
table .ciedecodes > "$work/decodes"

for f in kinds comps cie under decodes; do
    if [ ! -s "$work/$f" ]; then
        echo "FAILURES: data/color.ps states no $f table, or it is not"
        echo "      written where this can read it. The tables are the"
        echo "      roster; without one there is nothing to hold."
        exit 1
    fi
done

# ---- every kind is one of the five that something dispatches on
while read -r name kind; do
    case $kind in
        device|cie|indexed|tint|pattern) ;;
        *)  echo "FAIL: the family $name is of kind '$kind', which is not one"
            echo "      of device, cie, indexed, tint or pattern. A kind"
            echo "      nothing dispatches on is a family nothing handles."
            fail=1 ;;
    esac
done < "$work/kinds"

# ---- the two priced kinds are priced, and only they are
awk '$2 == "device" { print $1 }' "$work/kinds" | sort > "$work/want.comps"
awk '$2 == "cie"    { print $1 }' "$work/kinds" | sort > "$work/want.cie"
awk '{ print $1 }' "$work/comps" | sort > "$work/have.comps"
awk '{ print $1 }' "$work/cie"   | sort > "$work/have.cie"

guard_held=0
guard_hold "$work/want.comps" "$work/have.comps" \
    "of kind device and priced by no .spacecomps entry. Every such family
      states its own component count, and one that does not is asked for
      a count that is not there:" \
    "priced by .spacecomps and not of kind device in the roster. A price
      for a family the roster does not place is a price nothing will ask
      for, or a family whose kind changed without its price following:"
guard_hold "$work/want.cie" "$work/have.cie" \
    "of kind cie and priced by no .ciecomps entry:" \
    "priced by .ciecomps and not of kind cie in the roster:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- and the kinds whose count comes from their own parameters are
#      priced by neither, since a price there would be read instead
awk '$2 == "indexed" || $2 == "tint" || $2 == "pattern" { print $1 }' \
    "$work/kinds" | sort > "$work/unpriced"
cat "$work/have.comps" "$work/have.cie" | sort -u > "$work/priced"
both=$(comm -12 "$work/unpriced" "$work/priced")
if [ -n "$both" ]; then
    echo "FAIL: priced by a table although the count comes from the space:"
    printf '%s\n' "$both" | sed 's/^/      /'
    echo "      An indexed space carries one index, a tint space its tints"
    echo "      and a pattern space its pattern, each read from what the"
    echo "      program supplied. A table entry would be consulted first"
    echo "      and would answer for every such space alike."
    fail=1
fi

# ---- the special kinds say what may not sit beneath them, and only they
#
# An Indexed space is built over a base, a tint space over an
# alternative, a Pattern over the space its uncoloured cells paint with.
# Each of the three admits a different set (PLRM 4.8.4, 4.8.5, 4.9.2),
# and a kind absent from .spaceunder is one whose sub-space is validated
# against nothing at all -- which is the state an accepted Pattern over
# a Pattern was found in.
awk '$2 == "indexed" || $2 == "tint" || $2 == "pattern" { print $2 }' \
    "$work/kinds" | sort -u > "$work/want.under"
awk '{ print $1 }' "$work/under" | sort > "$work/have.under"

guard_held=0
guard_hold "$work/want.under" "$work/have.under" \
    "a special colour space in the roster and given no .spaceunder line.
      What may not be built under it is then stated nowhere, and every
      space is admitted there:" \
    "given a .spaceunder line and not a special colour space in the
      roster. A device or CIE-based space is built over nothing, so a
      rule for what may sit beneath it is a rule nothing will consult:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- and every CIE family says which decode entries it carries
#
# The decode entries are procedures (PLRM Tables 4.5 to 4.8), and a
# value of another type is taken in silence -- executing a number leaves
# that number, so the decoded component becomes a constant. A family
# absent from .ciedecodes is a family that check cannot reach.
awk '{ print $1 }' "$work/decodes" | sort > "$work/have.decodes"

guard_held=0
guard_hold "$work/want.cie" "$work/have.decodes" \
    "of kind cie and given no .ciedecodes line. Its decode entries are
      then whatever the program put there:" \
    "given a .ciedecodes line and not of kind cie in the roster:"
[ "$guard_held" -eq 0 ] || fail=1

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: the colour space families do not agree"
    exit 1
fi
printf 'SUCCESS (%s families: %s priced as device, %s as CIE, %s from their own parameters)\n' \
    "$(wc -l < "$work/kinds" | tr -d ' ')" \
    "$(wc -l < "$work/want.comps" | tr -d ' ')" \
    "$(wc -l < "$work/want.cie" | tr -d ' ')" \
    "$(wc -l < "$work/unpriced" | tr -d ' ')"
