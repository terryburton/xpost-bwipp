#!/bin/sh
#
# Every halftone type is asked what it is, how it is screened, and where
# it differs from the specification -- and answers.
#
# PLRM Table 7.3 names ten types of halftone dictionary and this
# interpreter screens with eight of them. Which eight was written down
# nowhere: a type that answers rangecheck is a type a program cannot
# use, and the two that answer it do so for a reason -- they carry no
# screen at all -- which read exactly like the reason a type nobody had
# got round to would give.
#
# So the set is derived rather than listed. Every type code from 0 to
# 100 is offered a dictionary carrying a superset of the entries the
# types want, and a type is taken to be SCREENED when sethalftone
# answers anything but rangecheck. A complaint about a particular entry
# is still recognition of the type; only rangecheck says the type itself
# is unknown. That distinction is what lets membership be read off the
# interpreter instead of maintained by hand: a type implemented later
# appears here without anyone saying so, and one that stops working
# disappears.
#
# Membership is only the first question. The others are the ones that
# catch a feature given to one member of a family and not the rest:
#
#   which helper presents a type as a simpler one, read from the router
#   whether the cell geometry has one derivation or has grown a second
#   whether a type may sit inside a type 5, asked of the interpreter
#
# and every difference between this interpreter and the specification
# carries a reason and a disposition, held in both directions so that a
# line cannot outlive the defect it describes.
#
#   $1  path to the source tree root
#   $2  path to the xpost binary
set -u
src=${1:?usage: check-halftone-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-halftone-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$src/tests/halftone-facts" "the halftone register"
guard_require_file "$src/data/gstate.ps" "the halftone machinery"

guard_workdir
cr=$(printf '\r')

# ---- what the register says
grep -v '^[[:space:]]*#' "$src/tests/halftone-facts" \
    | awk 'NF >= 6 && $1 ~ /^[0-9]+$/' > "$work/reg"
awk '{ print $1 }' "$work/reg" | LC_ALL=C sort > "$work/reg-types"
awk '$2 == "screens" { print $1 }' "$work/reg" | LC_ALL=C sort > "$work/reg-screens"

# the divergence section: a name, a disposition, and a reason
grep -v '^[[:space:]]*#' "$src/tests/halftone-facts" \
    | awk 'NF >= 3 && $1 !~ /^[0-9]+$/ && $1 != "entries" && $1 != "divergences" \
           && $1 != "entry" && $1 != "entries-reached" \
           && $1 != "optional" && $1 != "optionals"' \
    > "$work/div"
awk '{ print $1 }' "$work/div" | LC_ALL=C sort > "$work/div-names"

if [ ! -s "$work/reg" ]; then
    echo "FAILURES: tests/halftone-facts classifies no type; a register that"
    echo "      holds nothing proves nothing about the family"
    exit 1
fi

# ---- and what the interpreter answers, for every code it could name
#
# The dictionary carries every entry any type wants, so that a type is
# refused for being unknown rather than for missing something. Only
# rangecheck means the type itself is not screened with.
cat > "$work/probe.ps" <<'PSEOF'
/ask {                                  % n  .  -
    /n exch def
    n 8 string cvs print ( ) print
    { << /HalftoneType n
         /Frequency 60 /Angle 0 /SpotFunction { pop pop 0 }
         /Width 2 /Height 2
         /Thresholds <000102030405060708090A0B0C0D0E0F>
         /Xsquare 3 /Ysquare 2
         /RedFrequency 60 /RedAngle 0 /RedSpotFunction { pop pop 0 }
         /GreenFrequency 60 /GreenAngle 0 /GreenSpotFunction { pop pop 0 }
         /BlueFrequency 60 /BlueAngle 0 /BlueSpotFunction { pop pop 0 }
         /GrayFrequency 60 /GrayAngle 0 /GraySpotFunction { pop pop 0 }
         /RedWidth 2 /RedHeight 2 /RedThresholds <00408080>
         /GreenWidth 2 /GreenHeight 2 /GreenThresholds <00408080>
         /BlueWidth 2 /BlueHeight 2 /BlueThresholds <00408080>
         /GrayWidth 2 /GrayHeight 2 /GrayThresholds <00408080>
         /Default << /HalftoneType 1 /Frequency 60 /Angle 0
                     /SpotFunction { pop pop 0 } >> >>
      sethalftone } stopped
    { $error /errorname get /rangecheck eq { (refused) }{ (screens) } ifelse }
    { (screens) } ifelse
    print (\n) print
    clear
} bind def
0 1 100 { ask } for
PSEOF

XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/probe.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | awk 'NF == 2 { print }' > "$work/answers"

nasked=$(grep -c . "$work/answers" || true)
if [ "$nasked" -ne 101 ]; then
    echo "FAILURES: 101 type codes were offered and $nasked answered; a"
    echo "      membership this cannot read is one it must not report on"
    exit 1
fi
awk '$2 == "screens" { print $1 }' "$work/answers" | LC_ALL=C sort > "$work/live-screens"

fail=0

# ---- the screened set, both ways
guard_held=0
guard_hold "$work/live-screens" "$work/reg-screens" \
    "screened with by the interpreter and not recorded as screening in
      tests/halftone-facts. Say there what the type is; a type the
      family gained and nobody wrote down is one nobody examined:" \
    "recorded as screening in tests/halftone-facts and answering
      rangecheck. Either the type stopped working or the line outlived
      it, and a line that has outlived its type reads exactly like one
      that still holds:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- a refused type must really be refused
while read -r t kind rest; do
    [ "$kind" = refused ] || continue
    got=$(awk -v n="$t" '$1 == n { print $2 }' "$work/answers")
    if [ "$got" != refused ]; then
        echo "FAIL: tests/halftone-facts records type $t as refused and the"
        echo "      interpreter screens with it. A type that gained a screen"
        echo "      has gained one worth saying out loud."
        fail=1
    fi
done < "$work/reg"

# ---- every line says what, how and why, in the vocabulary of each column
while read -r t kind lat norm comp rest; do
    case $kind in
        screens|refused) ;;
        *)  echo "FAIL: type $t is '$kind', which is neither screens nor refused"
            fail=1 ;;
    esac
    case $lat in
        spot|sides|parts|none) ;;
        *)  echo "FAIL: type $t has lattice '$lat', which is none of spot,"
            echo "      sides, parts or none"
            fail=1 ;;
    esac
    case $comp in
        may|maynot) ;;
        *)  echo "FAIL: type $t has component '$comp', which is neither may"
            echo "      nor maynot"
            fail=1 ;;
    esac
    if [ -z "$rest" ]; then
        echo "FAIL: the line for type $t gives no reason. A type classified"
        echo "      without one is a type nobody examined."
        fail=1
    fi
done < "$work/reg"

# ---- the normaliser column, read from the router rather than listed
#
# A type reaches its screen either directly or through a helper that
# presents it as a simpler type. Which types go to which helper is read
# out of .ht5component: the type codes tested in a branch are collected,
# and the helper the branch goes on to call is what they are attributed
# to. That way a type given a helper of its own has to be written down
# here, and a helper that stops serving a type shows up too.
tr -d "$cr" < "$src/data/gstate.ps" | awk '
    /^\.xpostsys \/\.ht5component \{/ { inrouter = 1; next }
    inrouter && /^\} put/            { inrouter = 0 }
    !inrouter { next }
    {
        line = $0
        # The call has two spellings: baked into the body with //, and
        # fetched by name. Reading only one of them finds no helpers.
        piece = ""
        if (match(line, /\/\/\.ht[A-Za-z0-9]+[ \t]+exec/)) {
            piece = substr(line, RSTART + 3, RLENGTH - 3)
            sub(/[ \t]+exec$/, "", piece)
        } else if (match(line, /\/\.ht[A-Za-z0-9]+[ \t]+get[ \t]+exec/)) {
            piece = substr(line, RSTART + 2, RLENGTH - 2)
            sub(/[ \t]+get[ \t]+exec$/, "", piece)
        }
        if (piece != "") {
            for (i = 1; i <= np; i++) print substr(piece, 3), pend[i]
            np = 0
            next
        }
        rest = line
        while (match(rest, /[0-9]+ eq/)) {
            tok = substr(rest, RSTART, RLENGTH)
            rest = substr(rest, RSTART + RLENGTH)
            sub(/ eq/, "", tok)
            pend[++np] = tok
        }
        if (line ~ /\}/ && np > 0 && line !~ /[0-9]+ eq/) np = 0
    }
' | LC_ALL=C sort -u > "$work/router"

# a parse that matches nothing reads exactly like a router with no
# helpers in it, which is the state this must never pass in silence
nhelp=$(awk '{ print $1 }' "$work/router" | LC_ALL=C sort -u | grep -c . || true)
nattr=$(grep -c . "$work/router" || true)
if [ "$nhelp" -lt 2 ] || [ "$nattr" -lt 4 ]; then
    echo "FAILURES: the router in data/gstate.ps yielded $nhelp helpers over"
    echo "      $nattr types, and it has always had at least two helpers over"
    echo "      at least four. The router was rewritten in a way this cannot"
    echo "      follow, and a check that finds no members proves nothing"
    exit 1
fi
awk '{ print $2, $1 }' "$work/router" | LC_ALL=C sort > "$work/router-pairs"
awk '$4 != "none" && $4 != "component" { print $1, $4 }' "$work/reg" \
    | LC_ALL=C sort > "$work/reg-pairs"

guard_held=0
guard_hold "$work/router-pairs" "$work/reg-pairs" \
    "sent to a helper by the router in data/gstate.ps and not recorded
      against that helper in tests/halftone-facts. A type given a
      presentation of its own is a type the rest of the family has to be
      asked about:" \
    "recorded against a helper in tests/halftone-facts and not sent to
      one by the router. Either the type stopped needing it or the line
      outlived it:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the lattice column: one derivation, not one per parameterisation
#
# Every cell shape in this file resolves to the same two quantities, the
# determinant of the repeat vectors and the tile side that determinant
# reduces to. The column above says which parameterisation a type states
# them in; what is checked here is that stating them differently has not
# turned into deriving them differently. Two helpers with the same body
# is how that starts, and it is how it started once already.
tr -d "$cr" < "$src/data/gstate.ps" | awk '
    /^\.xpostsys \/[^ ]+ \{[ \t]*(%.*)?$/ { name = $2; body = ""; inb = 1; next }
    inb && /^\} (bind )?put[ \t]*$/ {
        gsub(/[ \t]+/, " ", body); sub(/^ /, "", body)
        if (length(body) >= 30) print body "\t" name
        inb = 0; next
    }
    inb {
        line = $0
        sub(/^[ \t]*%.*$/, "", line)
        sub(/[ \t]{2,}%.*$/, "", line)
        body = body " " line
    }
' | LC_ALL=C sort > "$work/bodies"

nbodies=$(grep -c . "$work/bodies" || true)
if [ "$nbodies" -lt 20 ]; then
    echo "FAILURES: only $nbodies helper bodies were read from data/gstate.ps,"
    echo "      and it has always held many more. A comparison that reads"
    echo "      almost nothing agrees with almost anything"
    exit 1
fi
dups=$(awk -F'\t' '{ if ($1 == prev) print prevname " and " $2; prev = $1; prevname = $2 }' \
    "$work/bodies")
if [ -n "$dups" ]; then
    echo "FAIL: two helpers in data/gstate.ps have the same body:"
    printf '%s\n' "$dups" | sed 's/^/      /'
    echo "      One of them is a second copy of a quantity the family"
    echo "      already had a name for. Delete it and call the first."
    fail=1
fi

# ---- the component column, asked of the interpreter
#
# PLRM 7.4.6 lets a type 5 component be a dictionary of any type except
# 2, 4 or 5. Each type the interpreter screens with is offered as one,
# and what it answers is held to what the register says the language
# allows.
cat > "$work/comp.ps" <<'PSEOF'
/mk {                                   % n  ->  dict
    dup 1 eq { pop << /HalftoneType 1 /Frequency 60 /Angle 0
                      /SpotFunction { pop pop 0 } >> }{
    dup 3 eq { pop << /HalftoneType 3 /Width 2 /Height 2
                      /Thresholds <00408080> >> }{
    dup 6 eq { pop << /HalftoneType 6 /Width 2 /Height 2
                      /Thresholds (00408080) /ASCIIHexDecode filter >> }{
    dup 10 eq { pop << /HalftoneType 10 /Xsquare 2 /Ysquare 1
                       /Thresholds <0040808000> >> }{
    dup 16 eq { pop << /HalftoneType 16 /Width 2 /Height 2
                       /Thresholds (0100804000C800FF) /ASCIIHexDecode filter >> }{
    dup 2 eq { pop << /HalftoneType 2
                      /RedFrequency 60 /RedAngle 0 /RedSpotFunction { pop pop 0 }
                      /GreenFrequency 60 /GreenAngle 0 /GreenSpotFunction { pop pop 0 }
                      /BlueFrequency 60 /BlueAngle 0 /BlueSpotFunction { pop pop 0 }
                      /GrayFrequency 60 /GrayAngle 0 /GraySpotFunction { pop pop 0 } >> }{
    dup 4 eq { pop << /HalftoneType 4
                      /RedWidth 2 /RedHeight 2 /RedThresholds <00408080>
                      /GreenWidth 2 /GreenHeight 2 /GreenThresholds <00408080>
                      /BlueWidth 2 /BlueHeight 2 /BlueThresholds <00408080>
                      /GrayWidth 2 /GrayHeight 2 /GrayThresholds <00408080> >> }{
    pop << /HalftoneType 5 /Default << /HalftoneType 1 /Frequency 60 /Angle 0
                                       /SpotFunction { pop pop 0 } >> >> }
    ifelse } ifelse } ifelse } ifelse } ifelse } ifelse } ifelse
} bind def
/ask {                                  % n  .  -
    /n exch def
    n 8 string cvs print ( ) print
    { << /HalftoneType 5 /Cyan n mk
         /Default << /HalftoneType 1 /Frequency 60 /Angle 0
                     /SpotFunction { pop pop 0 } >> >>
      sethalftone } stopped
    { (refused) }{ (accepted) } ifelse print (\n) print
    clear
} bind def
PSEOF
awk '{ printf "%s ask\n", $1 }' "$work/live-screens" >> "$work/comp.ps"

XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/comp.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | awk 'NF == 2 { print }' > "$work/comp-answers"

ncomp=$(grep -c . "$work/comp-answers" || true)
nscr=$(grep -c . "$work/live-screens" || true)
if [ "$ncomp" -ne "$nscr" ]; then
    echo "FAILURES: $nscr screened types were offered as a type 5 component"
    echo "      and $ncomp answered. A member that does not answer is one"
    echo "      this passes over in silence, which is what it exists to stop"
    exit 1
fi

: > "$work/comp-mismatch"
while read -r t answer; do
    said=$(awk -v n="$t" '$1 == n { print $5; exit }' "$work/reg")
    [ -n "$said" ] || continue
    case "$said:$answer" in
        may:accepted|maynot:refused) ;;
        *) echo "$t" >> "$work/comp-mismatch" ;;
    esac
done < "$work/comp-answers"

# a difference the guard can see must be named, and the name says which
# types differ, so that a change in which of them do forces the line to
# be rewritten rather than quietly kept
if [ -s "$work/comp-mismatch" ]; then
    want=component-$(LC_ALL=C sort -n "$work/comp-mismatch" | tr '\n' '-' | sed 's/-$//')
    if ! grep -q "^$want " "$work/div"; then
        echo "FAIL: the interpreter and PLRM disagree about which types may"
        echo "      be a type 5 component, and tests/halftone-facts does not"
        echo "      say so. Add a divergence line named"
        echo "        $want"
        echo "      with a disposition and the reason. These types answered"
        echo "      the opposite of what the register says the language allows:"
        sed 's/^/        type /' "$work/comp-mismatch"
        fail=1
    fi
    if grep -q '^component-' "$work/div" \
        && ! grep -q "^$want " "$work/div"; then
        :   # already reported above
    fi
else
    if grep -q '^component-' "$work/div"; then
        echo "FAIL: tests/halftone-facts names a component divergence and the"
        echo "      interpreter no longer has one. A line that has outlived"
        echo "      its defect reads exactly like one that still holds --"
        echo "      delete it in the commit that closed it."
        fail=1
    fi
fi

# ---- the transfer divergence, held to whether the tree reads the entry
if grep -rq "TransferFunction" "$src/data" "$src/src/lib" 2>/dev/null; then
    if grep -q '^transfer ' "$work/div"; then
        echo "FAIL: tests/halftone-facts says TransferFunction is read"
        echo "      nowhere and the tree now names it. Say what the types do"
        echo "      with it in the register, and retire the divergence."
        fail=1
    fi
else
    if ! grep -q '^transfer ' "$work/div"; then
        echo "FAIL: nothing in the tree reads TransferFunction, which PLRM"
        echo "      makes optional in five of these types and required for"
        echo "      some type 5 components. A gap the register does not name"
        echo "      is one nobody decided about -- add a 'transfer' line."
        fail=1
    fi
fi

# ---- the side-extent divergence, held to what a zero side answers
cat > "$work/extent.ps" <<'PSEOF'
{ << /HalftoneType 3 /Width 0 /Height 2 /Thresholds <> >> sethalftone } stopped
{ (refused) }{ (accepted) } ifelse print (\n) print
PSEOF
extent=$(XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/extent.ps" </dev/null 2>/dev/null | tr -d "$cr" | awk 'NF == 1 { print; exit }')
case $extent in
    accepted)
        if ! grep -q '^side-extent ' "$work/div"; then
            echo "FAIL: a cell of no extent is accepted and the register does"
            echo "      not say so. Add a 'side-extent' line."
            fail=1
        fi ;;
    refused)
        if grep -q '^side-extent ' "$work/div"; then
            echo "FAIL: tests/halftone-facts says a cell of no extent is"
            echo "      accepted and it is now refused. Retire the line in"
            echo "      the commit that closed it."
            fail=1
        fi ;;
    *)  echo "FAIL: the zero-side probe answered '$extent', which is neither"
        echo "      accepted nor refused, so this cannot report on it"
        fail=1 ;;
esac

# ---- what the extent rule reaches, offered to the interpreter
#
# The rule matches an entry by the end of its name, so it reaches names
# nobody chose. Each name it reaches is offered at nought inside an
# otherwise valid type 1 dictionary -- the rule reads every entry
# whatever the type, so one shape asks about any name -- and the verdict
# is held to what the register says the name is. This is the check that
# was missing when a rule meant to refuse an empty cell began refusing
# an answer the operator writes.
grep -v '^[[:space:]]*#' "$src/tests/halftone-facts" | awk '$1 == "entry" && NF >= 3' \
    > "$work/reach"
nreach=$(grep -c . "$work/reach" || true)

cat > "$work/reach.ps" <<'PSEOF'
/probe {                                % /Name  .  -
    /nm exch def
    nm 40 string cvs print ( ) print
    << /HalftoneType 1 /Frequency 60 /Angle 45 /SpotFunction { pop pop 0 } >>
    dup nm 0 put
    { sethalftone } stopped
    { $error /errorname get /rangecheck eq { (refused) }{ (other) } ifelse }
    { (accepted) } ifelse
    print (\n) print
    clear
} bind def
PSEOF
awk '{ printf "/%s probe\n", $2 }' "$work/reach" >> "$work/reach.ps"

XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/reach.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | awk 'NF == 2 { print }' > "$work/reach-answers"

nans=$(grep -c . "$work/reach-answers" || true)
if [ "$nreach" -lt 10 ] || [ "$nans" -ne "$nreach" ]; then
    echo "FAILURES: the register names $nreach entries the extent rule reaches"
    echo "      and $nans answered. A reach this cannot read is one it must"
    echo "      not report on, and it has always been more than ten"
    exit 1
fi
while read -r nm answer; do
    said=$(awk -v n="$nm" '$2 == n { print $3; exit }' "$work/reach")
    case "$said:$answer" in
        extent:refused|answer:accepted) ;;
        extent:*)
            echo "FAIL: $nm is an extent and nought was $answer. A size of"
            echo "      nought describes no area and the rule exists to say so."
            fail=1 ;;
        answer:*)
            echo "FAIL: $nm is a value the OPERATOR writes and nought was"
            echo "      $answer. What a program leaves in the slot states"
            echo "      nothing, so reading it as an extent refuses the"
            echo "      dictionary the specification asks for."
            fail=1 ;;
        *)  echo "FAIL: $nm is '$said', which is neither extent nor answer"
            fail=1 ;;
    esac
done < "$work/reach-answers"

# The two kinds have to be told apart for any of the above to mean
# anything -- but only where nothing above already said something more
# useful, since a rule that refuses everything fails both tests and the
# specific one is the one worth reading.
if [ "$fail" = 0 ] \
   && { [ "$(awk '$2 == "refused"' "$work/reach-answers" | grep -c .)" -lt 1 ] \
     || [ "$(awk '$2 == "accepted"' "$work/reach-answers" | grep -c .)" -lt 1 ]; }; then
    echo "FAILURES: every entry the rule reaches answered the same way, so"
    echo "      this comparison cannot tell the two kinds apart"
    exit 1
fi

said=$(awk '/^entries-reached /{ print $2; found = 1 } END { if (!found) print "" }' \
    "$src/tests/halftone-facts")
case $said in
    ''|*[!0-9]*)
        echo "FAILURES: tests/halftone-facts has no 'entries-reached <n>' line"
        fail=1 ;;
    *)  if [ "$said" -ne "$nreach" ]; then
            echo "FAILURES: tests/halftone-facts records $said reached entries and holds $nreach"
            fail=1
        fi ;;
esac

# ---- the optional entries PLRM types, held to the interpreter's table
#
# .htoptypes is where the rule lives, so it is what the register is held
# to: a name in one and not the other fails, and so does a disagreement
# about which types the entry may hold. Nothing here READS either value,
# which is exactly why the typing needs holding -- a check nothing
# depends on is one that can be dropped without any test noticing.
awk '/\.xpostsys \/\.htoptypes </, /^>> put/' "$src/data/gstate.ps" \
    | sed -n 's|^[[:space:]]*/\([A-Za-z0-9]*\)[[:space:]]*\[\([^]]*\)\].*|\1 \2|p' \
    | while read -r nm types; do
          printf '%s %s\n' "$nm" \
              "$(printf '%s\n' "$types" | tr -d '/' | tr -s ' ' '\n' \
                 | grep . | LC_ALL=C sort | paste -sd, -)"
      done | LC_ALL=C sort > "$work/opt-src"

grep -v '^[[:space:]]*#' "$src/tests/halftone-facts" \
    | awk '$1 == "optional" && NF >= 4 { print $2, $3 }' \
    | while read -r nm types; do
          printf '%s %s\n' "$nm" \
              "$(printf '%s\n' "$types" | tr ',' '\n' | grep . \
                 | LC_ALL=C sort | paste -sd, -)"
      done | LC_ALL=C sort > "$work/opt-reg"

nopt=$(grep -c . "$work/opt-reg" || true)
if [ "$nopt" -lt 1 ] || [ "$(grep -c . "$work/opt-src")" -lt 1 ]; then
    echo "FAILURES: the optional-entry table is empty on one side"
    echo "      (register $nopt, source $(grep -c . "$work/opt-src")). Two"
    echo "      empty sets agree about nothing, so this cannot report"
    exit 1
fi

awk '{ print $1 }' "$work/opt-reg" > "$work/opt-reg-names"
awk '{ print $1 }' "$work/opt-src" > "$work/opt-src-names"
guard_held=0
guard_hold "$work/opt-reg-names" "$work/opt-src-names" \
    "named as an optional entry in tests/halftone-facts and absent from
      .htoptypes in data/gstate.ps. Either the entry stopped being typed
      or the line is stale:" \
    "typed by .htoptypes and named by no 'optional' line in
      tests/halftone-facts. Say what the entry is and which types PLRM
      gives it:"
[ "$guard_held" -eq 0 ] || fail=1

if ! diff_out=$(diff "$work/opt-reg" "$work/opt-src" 2>&1); then
    echo "FAIL: the register and .htoptypes disagree about which types an"
    echo "      optional entry may hold. PLRM's tables decide, and the"
    echo "      register carries the citation:"
    printf '%s\n' "$diff_out" | sed 's/^/        /'
    fail=1
fi

# and each one PROBED: a value of a type the entry may hold is accepted,
# and one of a type it may not is a typecheck. Both directions, because a
# check that refuses everything passes a test that only offers it a bad
# value.
litfor() {
    case $1 in
        booleantype) echo 'true' ;;
        integertype) echo '42' ;;
        realtype)    echo '1.5' ;;
        numbertype)  echo '42' ;;
        nametype)    echo '/probename' ;;
        stringtype)  echo '(probe)' ;;
        arraytype)   echo '[ 1 2 ]' ;;
        dicttype)    echo '<< >>' ;;
        proctype)    echo '{ }' ;;
        *)           echo '' ;;
    esac
}

cat > "$work/opt.ps" <<'PSEOF'
/probe {                                % /Name value (label)  .  -
    /lbl exch def /val exch def /nm exch def
    nm 40 string cvs print ( ) print lbl print ( ) print
    << /HalftoneType 1 /Frequency 60 /Angle 45 /SpotFunction { pop pop 0 } >>
    dup nm val put
    { sethalftone } stopped
    { $error /errorname get 40 string cvs print }{ (ok) print } ifelse
    (\n) print
    clear
} bind def
PSEOF

: > "$work/opt-want"
while read -r nm types; do
    for t in $(printf '%s\n' "$types" | tr ',' ' '); do
        lit=$(litfor "$t")
        if [ -z "$lit" ]; then
            echo "FAIL: this cannot build a $t value, so it cannot probe"
            echo "      $nm. Teach litfor the type or this reports on nothing."
            fail=1
            continue
        fi
        printf '/%s %s (takes-%s) probe\n' "$nm" "$lit" "$t" >> "$work/opt.ps"
        printf '%s takes-%s ok\n' "$nm" "$t" >> "$work/opt-want"
    done
    # a type the entry may NOT hold, chosen from the candidates rather
    # than written down, so an entry that gains a type does not leave a
    # stale wrong-value case behind
    for t in booleantype integertype nametype stringtype arraytype; do
        case ",$types," in *",$t,"*) continue ;; esac
        printf '/%s %s (refuses-%s) probe\n' "$nm" "$(litfor "$t")" "$t" \
            >> "$work/opt.ps"
        printf '%s refuses-%s typecheck\n' "$nm" "$t" >> "$work/opt-want"
        break
    done
done < "$work/opt-reg"

XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/opt.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | awk 'NF == 3 { print }' > "$work/opt-got"

if ! diff_out=$(diff "$work/opt-want" "$work/opt-got" 2>&1); then
    echo "FAIL: an optional entry did not answer for its type as the"
    echo "      register says it should (wanted < , got >):"
    printf '%s\n' "$diff_out" | sed 's/^/        /'
    fail=1
fi

# ---- the settled difference over accurate screening, held to whether a
# second algorithm has arrived
#
# The register settles this on the ground that a true AccurateScreens
# leaves ordinary halftoning in force and that the screen actually
# achieved is reported honestly. Both halves are probed: if the two
# settings ever produce different achieved frequencies then a second
# algorithm exists and the line must be retired, and if the achieved
# frequency stops being written the justification has gone.
cat > "$work/acc.ps" <<'PSEOF'
/ask {                                  % bool  .  -
    /flag exch def
    << /HalftoneType 1 /Frequency 60 /Angle 30 /SpotFunction { pop pop 0 }
       /ActualFrequency 0 /ActualAngle 0 /AccurateScreens flag >>
    dup sethalftone
    dup /ActualFrequency get 20 string cvs print ( ) print
    /ActualAngle get 20 string cvs print (\n) print
} bind def
true ask
false ask
PSEOF
XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/acc.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | awk 'NF == 2 { print }' > "$work/acc-got"
accn=$(grep -c . "$work/acc-got" || true)
if [ "$accn" -ne 2 ]; then
    echo "FAILURES: the accurate-screening probe answered $accn lines, not 2,"
    echo "      so it cannot report on the difference the register settles"
    exit 1
fi
acc_true=$(sed -n 1p "$work/acc-got")
acc_false=$(sed -n 2p "$work/acc-got")
if [ "$acc_true" = "$acc_false" ]; then
    if ! grep -q '^one-screening-algorithm ' "$work/div"; then
        echo "FAIL: asking for accurate screening changes nothing about the"
        echo "      screen achieved, and the register does not say so. Add a"
        echo "      'one-screening-algorithm' line with the reason."
        fail=1
    fi
else
    if grep -q '^one-screening-algorithm ' "$work/div"; then
        echo "FAIL: tests/halftone-facts settles the accurate-screening"
        echo "      difference on there being one algorithm, and the two"
        echo "      settings now achieve different screens ($acc_true vs"
        echo "      $acc_false). Retire the line in the commit that changed it."
        fail=1
    fi
fi
# the justification rests on the achieved screen being reported at all
case $acc_true in
    "0 0"|"")
        echo "FAIL: the register settles accurate screening on sethalftone"
        echo "      reporting the screen it achieved, and ActualFrequency and"
        echo "      ActualAngle came back as '$acc_true'. Without that report"
        echo "      the difference is silent and is not settled."
        fail=1 ;;
esac

# ---- every divergence says what will become of it
while read -r what disp rest; do
    case $disp in
        settled|thorn|heading) ;;
        *)  echo "FAIL: divergence $what is '$disp', which is none of"
            echo "      settled, thorn or heading"
            fail=1 ;;
    esac
    if [ -z "$rest" ]; then
        echo "FAIL: the divergence $what gives no reason. A difference"
        echo "      recorded without one is one nobody examined."
        fail=1
    fi
done < "$work/div"

# ---- the counts, so retiring a type or a difference is two edits
entries=$(awk '/^entries /{ print $2; found = 1 } END { if (!found) print "" }' \
    "$src/tests/halftone-facts")
nreg=$(grep -c . "$work/reg")
case $entries in
    ''|*[!0-9]*)
        echo "FAILURES: tests/halftone-facts has no 'entries <n>' line"
        fail=1 ;;
    *)  if [ "$entries" -ne "$nreg" ]; then
            echo "FAILURES: tests/halftone-facts records $entries types and holds $nreg"
            fail=1
        fi ;;
esac
# the declared optionals count, for the reason the divergences count is
# declared: nothing else would stop the section being emptied, and two
# empty sets agree
noptsaid=$(awk '/^optionals /{ print $2; found = 1 } END { if (!found) print "" }' \
    "$src/tests/halftone-facts")
case $noptsaid in
    ''|*[!0-9]*)
        echo "FAILURES: tests/halftone-facts has no 'optionals <n>' line"
        fail=1 ;;
    *)  if [ "$noptsaid" -ne "$nopt" ]; then
            echo "FAILURES: tests/halftone-facts records $noptsaid optional entries and holds $nopt"
            fail=1
        fi ;;
esac
ndivsaid=$(awk '/^divergences /{ print $2; found = 1 } END { if (!found) print "" }' \
    "$src/tests/halftone-facts")
ndiv=$(grep -c . "$work/div" || true)
case $ndivsaid in
    ''|*[!0-9]*)
        echo "FAILURES: tests/halftone-facts has no 'divergences <n>' line"
        fail=1 ;;
    *)  if [ "$ndivsaid" -ne "$ndiv" ]; then
            echo "FAILURES: tests/halftone-facts records $ndivsaid divergences and holds $ndiv"
            fail=1
        fi ;;
esac

[ "$fail" = 0 ] || exit 1
printf 'SUCCESS (%s types classified, %s screened, %s normalised, %s optional entr(y|ies) typed, %s differences named)\n' \
    "$nreg" "$nscr" "$(grep -c . "$work/reg-pairs")" "$nopt" "$ndiv"
