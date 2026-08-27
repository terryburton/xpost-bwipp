#!/bin/sh
#
# Every fact a device states is accounted for, and a difference between
# two devices carries a reason.
#
# A device says what it is by the entries on its class: which colour
# models it offers, how many bits of glyph coverage it can take, whether
# a grey reaches it as a pattern of pixels. The machinery above the
# device reads those entries and sends it different work accordingly. So
# an entry is a question the family has been asked, and every device is
# answering it -- including the ones whose author never heard the
# question, which answer by inheriting whatever the class they were
# copied from said.
#
# That is how the differences got there. A mechanism arrives, its author
# wires it to the device in front of them, and the rest of the family is
# never asked: the entry spreads by dict copy to devices it does not fit
# and stops short of devices it does. Nothing about a spelling says
# which of those happened, so nothing catches either.
#
# ---- what this holds
#
# The register beside this file, tests/device-facts, has one line per
# entry, and the two are held to each other in BOTH directions:
#
#   an entry no line classifies fails -- so a new mechanism cannot be
#   added quietly. Its author is stopped here until they say whether
#   every device must answer it, or why it is not a family question.
#
#   a line naming an entry no device carries fails -- so a register
#   cannot outlive what it describes. A reason that has stopped being
#   about anything reads exactly like one that still holds.
#
#   the devices carrying an entry are named, and are held to the ones
#   that do. An entry spreading to another device is the drift this
#   exists to catch, and it fails here whichever direction it spread.
#
# Some entries are questions with an answer per device. Those carry a
# family answer, and a device answering otherwise must name the file it
# states its own answer in and say why -- prose sitting where the
# difference is, saying what would make it false. An answer that has
# come back to the family answer and kept its reason fails too.
#
# ---- why it asks a running interpreter
#
# A class is built, not spelled. The compiled drivers copy a class
# written in PostScript and then say their own thing about the copy, and
# one driver body makes two classes and says different things for each.
# What a file spells and what a class ends up holding are two questions,
# and the one that matters is the second -- so every device is installed
# by name and its dictionary read, exactly as tests/check-device-roster.sh
# reads what a device says about taking its page in bands.
#
# ---- and the recorder
#
# The record paints nothing. It stands in front of a device that does,
# writes down what it was asked to paint, and plays it back. So its
# answers are not its own: they are its target's, and a record answering
# for itself would answer about a device it is standing in front of.
# That is held here directly -- the record is made over each target in
# turn and asked -- which is the one way to see it, since what the
# record carries is copied at the moment it is specialised and no
# reading of any file shows it.
#
#   $1  path to the source tree root
#   $2  the built interpreter
set -u
src=${1:?usage: check-device-facts.sh <srcroot> <xpost>}
xpost=${2:?usage: check-device-facts.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_srcdata "$src"

guard_workdir

srcdir=$src
guard_mirror_tree "$src"
src=$mirror

register="$src/tests/device-facts"
guard_require_file "$register" "the register of device facts"
fleet="$src/tests/device-fleet.sh"
guard_require_file "$fleet" "the device roster"

fail=0

# The page codec, by the name of its type. Everything about it below is
# read off that type and off what each device puts in one, so this is the
# only place the descriptor is named.
CODEC_TYPE=Xpost_Dev_Page_Codec

# ---------------------------------------------------------------------
# What the devices say
#
# Each device is installed by a page-device request and its dictionary
# read. The run starts on the device that paints nothing, so what a
# device says is read after the request that installed it and never off
# whatever device this build was configured with.
#
# A value is written down as what can be compared: a procedure as the
# word, a dictionary as its keys, a string and an array as what they
# are. What matters about a procedure is that the device has one, and
# two devices whose procedures differ differ in every line of them.
cat > "$work/enc.ps" <<'EOF'
/S 64 string def
/.enc {
    dup xcheck { pop (proc) print }{
    dup type /dicttype eq { (set:) print { pop S cvs print (,) print } forall }{
    dup type /arraytype eq { pop (array) print }{
    dup type /packedarraytype eq { pop (array) print }{
    dup type /stringtype eq { pop (str) print }{
    dup type /nulltype eq { pop (null) print }{
    S cvs print } ifelse } ifelse } ifelse } ifelse } ifelse } ifelse
} bind def
/.dump {
    /DN exch def
    DEVICE { exch (K ) print DN S cvs print ( ) print S cvs print ( ) print
             .enc (\n) print } forall
} bind def
EOF

( . "$fleet"
  for v in $DEVICE_FLEET_ALL; do echo "$v"; done ) 2>/dev/null \
    | sort -u > "$work/roster"
( . "$fleet"
  for v in $DEVICE_FLEET_BANDS; do echo "$v"; done ) 2>/dev/null \
    | sort -u > "$work/targets"
if [ ! -s "$work/roster" ] || [ ! -s "$work/targets" ]; then
    echo "FAILURES: tests/device-fleet.sh names no roster or no band targets"
    exit 1
fi

{
    cat "$work/enc.ps"
    echo "["
    grep -vx record "$work/roster" | sed 's|^|/|'
    cat <<'EOF'
]
{ /D exch def
  { << /OutputDevice D /PageSize [ 8 8 ] >> setpagedevice } stopped
  { (UNMADE ) print D 60 string cvs print (\n) print }
  { D .dump } ifelse
} forall
EOF
} > "$work/ask.ps"
{ cat "$work/enc.ps"; echo "/record .dump"; } > "$work/askrec.ps"

: > "$work/said"
out=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
       "$xpost" -q -d null -o facts.scratch ask.ps </dev/null 2>&1 )
rc=$?
printf '%s\n' "$out" >> "$work/said"
if [ $rc -ne 0 ] || ! grep -q '^K ' "$work/said"; then
    echo "FAILURES: the interpreter could not be asked what its devices state:"
    printf '%s\n' "$out" | sed 's/^/      /' | head -8
    exit 1
fi

# The record over each target in turn. A target this build cannot make
# takes its record with it and is left out of both sides below.
: > "$work/rec"
recasked=0
while read -r t; do
    rout=$( cd "$work" && XPOST_DATA_DIR="$srcdata" \
            "$xpost" -q -d "$t:band" -o facts.scratch askrec.ps \
            </dev/null 2>&1 )
    printf '%s\n' "$rout" | grep '^K record ' \
        | sed "s/^K record /R $t /" >> "$work/rec"
    if grep -q "^R $t " "$work/rec"; then
        recasked=$((recasked + 1))
        echo "$t" >> "$work/tasked"
    fi
done < "$work/targets"
[ -f "$work/tasked" ] || : > "$work/tasked"

grep '^UNMADE ' "$work/said" | awk '{print $2}' | sort -u > "$work/unmade"
grep -vxF -f "$work/unmade" "$work/roster" 2>/dev/null | grep . \
    > "$work/made" || cp "$work/roster" "$work/made"
[ -s "$work/unmade" ] || cp "$work/roster" "$work/made"

# counts in before counts out: a run that installed nothing agrees with
# any register at all
nasked=$(grep -c . "$work/made" || true)
nkey=$(awk '$1 == "K" { print $3 }' "$work/said" | sort -u | grep -c . || true)
nrkey=$(awk '$1 == "R" { print $3 }' "$work/rec" | sort -u | grep -c . || true)
if [ "$nasked" -lt 8 ] || [ "$nkey" -lt 40 ] || [ "$recasked" -lt 1 ]; then
    echo "FAILURES: $nasked device(s) answered, stating $nkey distinct entries,"
    echo "      with the record made over $recasked target(s). The family is"
    echo "      larger than that; the question is being asked wrong."
    exit 1
fi

# The value a device states, canonicalised: a dictionary is its keys, and
# its keys are a set rather than an order.
canon() {
    awk '{
        v = $4
        if (v ~ /^set:/) {
            sub(/^set:/, "", v); n = split(v, a, ","); m = 0
            for (i = 1; i <= n; i++) if (a[i] != "") b[++m] = a[i]
            for (i = 1; i < m; i++) for (j = i + 1; j <= m; j++)
                if (b[j] < b[i]) { t = b[i]; b[i] = b[j]; b[j] = t }
            v = "set:"; for (i = 1; i <= m; i++) v = v b[i] (i < m ? "," : "")
            delete b
        }
        print $2, $3, v
    }'
}
awk '$1 == "K"' "$work/said" | canon | sort > "$work/state"
awk '$1 == "R"' "$work/rec"  | canon | sort > "$work/rstate"

# Who carries an entry. The record carries one when it carries it over
# every target it could be made over; where it carries one over some of
# them, that is the target's entry showing through, which the mirror
# rule below is what holds.
awk '{ print $2, $1 }' "$work/state" | sort -u > "$work/carry"
awk -v n="$recasked" '{ c[$2]++ } END { for (k in c) if (c[k] == n) print k, "record" }' \
    "$work/rstate" | sort >> "$work/carry"
sort -u "$work/carry" -o "$work/carry"
awk '{ print $1 }' "$work/carry" | sort -u > "$work/keys.seen"

# ---------------------------------------------------------------------
# What the register says
grep -v '^[[:space:]]*#' "$register" > "$work/reg"

awk '
    /^[ \t]/ { if (last != "") prose[last] = prose[last] + 1; next }
    NF == 0 { next }
    $1 == "question" || $1 == "method" || $1 == "state" || $1 == "part" ||
    $1 == "elsewhere" || $1 == "open" || $1 == "trait" || $1 == "tune" {
        print "KIND", $2, $1 > (out "/reg.kind")
        line = ""
        for (i = 3; i <= NF; i++) line = line " " $i
        print $2 line > (out "/reg.carry")
        key = $2; last = "kind:" $2; next
    }
    $1 == "disposition" {
        print key, $2 > (out "/reg.disp"); last = "disp:" key ":" $2; next
    }
    $1 == "codecslots" { print $2 > (out "/reg.codecslots"); last = ""; next }
    $1 == "codecgap" {
        print $2, $3 > (out "/reg.codecgap")
        last = "codecgap:" $2 ":" $3; next
    }
    $1 == "family" { print key, $2 > (out "/reg.family"); last = "family:" key; next }
    $1 == "answer" {
        print key, $2, $3, ($4 == "" ? "-" : $4) > (out "/reg.answer")
        last = "answer:" key ":" $2; next
    }
    { print "check-device-facts: unreadable register line: " $0 > "/dev/stderr"
      bad = 1 }
    END {
        for (k in prose) print k, prose[k] > (out "/reg.prose")
        exit bad ? 1 : 0
    }
' out="$work" "$work/reg" || fail=1
for f in reg.kind reg.carry reg.family reg.answer reg.prose reg.disp \
         reg.codecslots reg.codecgap; do
    [ -f "$work/$f" ] || : > "$work/$f"
done
if [ ! -s "$work/reg.kind" ]; then
    echo "FAILURES: tests/device-facts classifies nothing; every entry the"
    echo "      devices state would be a finding and a tree in good order"
    echo "      would read the same as one in disorder"
    exit 1
fi

# A trait is not a key on a class -- it is a fact about how the device is
# built, read from the source rather than from a dictionary -- so it is
# held out of the comparison against what the devices state and checked
# against the tree below instead.
awk '$3 == "trait" { print $2 }' "$work/reg.kind" | sort > "$work/keys.trait"
awk '$3 == "tune"  { print $2 }' "$work/reg.kind" | sort > "$work/keys.tune"
awk '$3 != "trait" && $3 != "tune" { print $2 }' "$work/reg.kind" | sort > "$work/keys.reg"
if [ "$(sort "$work/keys.reg" | uniq -d | grep -c . || true)" -ne 0 ]; then
    echo "FAIL: tests/device-facts classifies an entry twice:"
    sort "$work/keys.reg" | uniq -d | sed 's/^/      /'
    fail=1
fi
sort -u "$work/keys.reg" -o "$work/keys.reg"

# ---- the two directions
#
# An entry is shown with the devices that carry it, which is what says
# whether an unclassified key is one device's private business or a
# question the whole family answers. A key the register names and no
# device states has no carrier, and is shown as the bare name.
guard_format() {
    while read -r k; do
        carried=$(awk -v k="$k" '$1 == k { printf " %s", $2 }' "$work/carry")
        if [ -n "$carried" ]; then
            printf '      %-18s carried by%s\n' "$k" "$carried"
        else
            printf '      %s\n' "$k"
        fi
    done
}

guard_held=0
guard_hold "$work/keys.seen" "$work/keys.reg" \
    "stated by the devices and not classified by tests/device-facts.
      Say there what each is: a question every device must answer, with
      its answers, or why it is not one. An entry nobody classified is a
      mechanism the family was never asked about:" \
    "classified by tests/device-facts and stated by no device. A line
      that has outlived its entry reads exactly like one that still
      holds:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- and who carries each
while read -r k; do
    want=$(awk -v k="$k" '$1 == k { for (i = 2; i <= NF; i++) print $i }' \
           "$work/reg.carry" | sort -u)
    case " $want " in
        *" every "*) want=$(cat "$work/made") ;;
    esac
    want=$(printf '%s\n' "$want" | grep . | sort -u)
    # a device this build could not make states nothing, so it is held to
    # nothing and the register's side is narrowed to match
    if [ -s "$work/unmade" ]; then
        want=$(printf '%s\n' "$want" | grep -vxF -f "$work/unmade" || true)
    fi
    got=$(awk -v k="$k" '$1 == k { print $2 }' "$work/carry" | sort -u)
    if [ "$want" != "$got" ]; then
        echo "FAIL: $k is carried by devices tests/device-facts does not name:"
        echo "      register: $(printf '%s ' $want)"
        echo "      devices:  $(printf '%s ' $got)"
        fail=1
    fi
done < "$work/keys.reg"

# ---------------------------------------------------------------------
# The questions
#
# A question carries a family answer and one answer per carrier. An
# answer other than the family's is a difference, and a difference must
# be owned -- stated in a named file rather than inherited -- and must
# carry a reason. An answer that has come back to the family's may not
# keep one.
nq=0
nreason=0
# An entry held by another guard names which, and one this does not yet
# ask names why not: both are judgements that can go stale, so both are
# held to saying enough to be argued with.
awk '$3 == "elsewhere" || $3 == "open" { print $2, $3 }' "$work/reg.kind" \
    | while read -r k kind; do
    if ! awk -v k="kind:$k" '$1 == k && $2 >= 3 { f = 1 } END { exit f ? 0 : 1 }' \
         "$work/reg.prose"; then
        echo "FAIL: $k is classified $kind in fewer than three lines. Say which"
        echo "      guard holds it, or why it is not a family question and what"
        echo "      asking it would cost."
        fail=1
    fi
done

# ---- the traits, which are not on a class
#
# Two things about a device are decided by how it was built rather than
# by what it says: whether there is compiled code behind it at all, and
# whether it drives its page out through a codec. Neither is a key on a
# class, so neither can be read from a dictionary the way everything
# above is. They are derived from the tree here and the register is held
# to what is found, in both directions, exactly as the class entries are.
#
# A device is compiled if a driver is named for it, or if a driver
# installs a leaf class named for it -- the second is how one driver body
# comes to make two devices. A class whose name ends IMAGE is not a leaf:
# it is the PostScript class the compiled drivers copy, and matching it
# would call every device that derives from it compiled, which is the
# opposite of the truth.
#
# Which driver, if any, makes a device is asked ONCE and both traits are
# read off the answer. It used to be asked twice: whether a device is
# compiled looked for the leaf class as well as the file, and whether it
# drives a codec looked only at the file name -- so pngalpha, a second
# device out of one driver body, was compiled according to one question
# and had no codec according to the other, while sharing the very
# descriptor the second question is about.
driver_of() {       # <device> -- the source that makes it, or nothing
    _u=$(echo "$1" | tr 'a-z' 'A-Z')
    if [ -f "$src/src/lib/xpost_dev_$1.c" ]; then
        echo "$src/src/lib/xpost_dev_$1.c"
        return
    fi
    grep -l "\"\.xpost_${_u}DEVICE\"" "$src"/src/lib/xpost_dev_*.c 2>/dev/null \
        | head -1
}

: > "$work/trait.derived"
while read -r d; do
    f=$(driver_of "$d")
    [ -n "$f" ] || continue
    echo "compiled $d" >> "$work/trait.derived"
    # the descriptor by its TYPE, so that what the device happens to have
    # called its own instance of it does not decide the answer
    grep -q "$CODEC_TYPE[ \t][ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=" "$f" &&
        echo "pagecodec $d" >> "$work/trait.derived"
done < "$work/made"

while read -r t; do
    want=$(awk -v t="$t" '$1 == t { for (i = 2; i <= NF; i++) print $i }' \
           "$work/reg.carry" | sort -u | tr '\n' ' ')
    got=$(awk -v t="$t" '$1 == t { print $2 }' "$work/trait.derived" \
          | sort -u | tr '\n' ' ')
    if [ "$want" != "$got" ]; then
        echo "FAIL: the trait $t is carried by a different set than the"
        echo "      register names."
        echo "        register: ${want:-(none)}"
        echo "        the tree: ${got:-(none)}"
        echo "      A trait is read from how the devices are built, so the"
        echo "      register cannot be right about it by being edited."
        fail=1
    fi
done < "$work/keys.trait"

# ---- the tuning options, which are not on a class either
#
# A compiled driver may read a knob off its device dictionary when it
# builds or opens its instance -- how hard its codec compresses, whether
# its image interlaces, which row filters its codec may choose between.
# The knob arrives as a key of a setpagedevice request or as a default
# an embedder recorded on the class, so on the plain installs above no
# class carries it and the comparison cannot see it; what it is is a
# word in the request's vocabulary, and a word added to one member of
# the family must force the question for the rest exactly as a class
# entry does.
#
# The population is derived by the mechanism, not the spelling: a read
# of the device dictionary through xpost_dict_get(ctx, devdic,
# xpost_name_cons(ctx, "...")) in a device's driver is a tuning option
# whatever it is called -- less the keys the devices were seen stating
# above, since a read of an entry the class itself carries is the class
# comparison's business and already held there. A device is charged
# with every option its driver body reads, which is what makes two
# devices out of one body carry the same set. Comments are stripped
# first, string contents kept, so a call written about is not a call.
: > "$work/tune.derived"
while read -r d; do
    f=$(driver_of "$d")
    [ -n "$f" ] || continue
    awk '
        FNR == 1 { inblock = 0 }
        {
            line = $0; sub(/\r$/, "", line)
            out = ""; i = 1; n = length(line)
            while (i <= n) {
                c = substr(line, i, 1); t = substr(line, i, 2)
                if (inblock) {
                    if (t == "*/") { inblock = 0; i += 2 } else i++
                    continue
                }
                if (t == "/*") { inblock = 1; i += 2; continue }
                if (t == "//") break
                out = out c; i++
            }
            printf "%s ", out
        }' "$f" \
    | grep -o 'xpost_dict_get(ctx, *devdic, *xpost_name_cons(ctx, *"[A-Za-z0-9_]*")' \
    | sed 's/.*"\([A-Za-z0-9_]*\)")$/\1/' \
    | grep -vxF -f "$work/keys.seen" \
    | while read -r o; do echo "$o $d" >> "$work/tune.derived"; done
done < "$work/made"
sort -u "$work/tune.derived" -o "$work/tune.derived"

awk '{ print $1 }' "$work/tune.derived" | sort -u > "$work/tune.keys"
guard_held=0
guard_hold "$work/tune.keys" "$work/keys.tune" \
    "read off the device dictionary by a device driver and not
      classified by tests/device-facts. A knob one member of the family
      grew is a question the rest have been asked; say what it tunes
      and which devices read it." \
    "classified as a tuning option and read by no driver. The knob is
      gone or renamed; the line excusing it is cover for the next one."
[ "$guard_held" -eq 0 ] || fail=1

while read -r t; do
    want=$(awk -v t="$t" '$1 == t { for (i = 2; i <= NF; i++) print $i }' \
           "$work/reg.carry" | sort -u | tr '\n' ' ')
    got=$(awk -v t="$t" '$1 == t { print $2 }' "$work/tune.derived" \
          | sort -u | tr '\n' ' ')
    if [ "$want" != "$got" ]; then
        echo "FAIL: the tuning option $t is read by a different set of devices"
        echo "      than the register names."
        echo "        register: ${want:-(none)}"
        echo "        the tree: ${got:-(none)}"
        echo "      An option is read from the driver body, so every device"
        echo "      that body makes carries it, asked for or not."
        fail=1
    fi
done < "$work/keys.tune"

# ---- and the roster each driver states, held to its reads
#
# A driver states its knobs in an Xpost_Dev_Option table beside the
# reads (the -p switch refuses everything outside the stated rosters,
# and the class takes an embedder's defaults up from them), so a knob
# read but not stated is one the command line refuses while the device
# obeys it, and one stated but not read is a control that selects
# nothing. Both directions are held here, per driver body, by the same
# comment-stripped scan the reads come from.
while read -r d; do
    f=$(driver_of "$d")
    [ -n "$f" ] || continue
    b=$(basename "$f")
    grep -q "Xpost_Dev_Option" "$f" || continue
    echo "$b" >> "$work/tune.rosterfiles.raw"
    awk '
        FNR == 1 { inblock = 0; intab = 0 }
        {
            line = $0; sub(/\r$/, "", line)
            out = ""; i = 1; n = length(line)
            while (i <= n) {
                c = substr(line, i, 1); t = substr(line, i, 2)
                if (inblock) {
                    if (t == "*/") { inblock = 0; i += 2 } else i++
                    continue
                }
                if (t == "/*") { inblock = 1; i += 2; continue }
                if (t == "//") break
                out = out c; i++
            }
            if (out ~ /Xpost_Dev_Option options\[\]/) intab = 1
            if (intab && out ~ /^ *};/) intab = 0
            if (intab && match(out, /\{ *"[A-Za-z0-9_]+"/)) {
                key = substr(out, RSTART, RLENGTH)
                sub(/^\{ *"/, "", key); sub(/"$/, "", key)
                print key
            }
        }' "$f" | sed "s/^/$b /" >> "$work/tune.stated"
done < "$work/made"
[ -f "$work/tune.stated" ] || : > "$work/tune.stated"
[ -f "$work/tune.rosterfiles.raw" ] || : > "$work/tune.rosterfiles.raw"
sort -u "$work/tune.stated" -o "$work/tune.stated"
sort -u "$work/tune.rosterfiles.raw" > "$work/tune.rosterfiles"

# the reads, keyed the same way: by the driver file that makes them
: > "$work/tune.readsbyfile"
while read -r pair; do
    o=${pair% *}; d=${pair#* }
    f=$(driver_of "$d"); [ -n "$f" ] || continue
    echo "$(basename "$f") $o" >> "$work/tune.readsbyfile"
done < "$work/tune.derived"
sort -u "$work/tune.readsbyfile" -o "$work/tune.readsbyfile"

guard_held=0
guard_hold "$work/tune.readsbyfile" "$work/tune.stated" \
    "read by a driver and absent from the roster it states beside the
      read. The -p switch refuses what the roster does not name, so
      this knob is one the command line refuses while the device obeys
      it. State it:" \
    "stated in a driver's roster and read by no driver body. The knob
      is gone or renamed; a control that selects nothing reads exactly
      like one that selects something."
[ "$guard_held" -eq 0 ] || fail=1

# ---- the slots of the page codec
#
# Carrying a codec is one bit, and the register held only that bit. A
# descriptor is a list of promises, though: a device carrying one fills
# every slot of it, and a slot left empty tells the shared machinery not
# to call it for this device. That is a difference between two members of
# a family, and it was being made in a trailing comment.
#
# Both sides are derived. The slots come from the type in the driver
# header, in the order they are declared; the fills come from each
# device's own initialiser, read positionally against that order. So a
# slot added to the descriptor, or a device that stops filling one, shows
# up here whether or not anybody remembered.
slots=$(sed 's|/\*.*\*/||' "$src"/src/lib/*.h | awk -v TY="$CODEC_TYPE" '
    /^typedef[ \t]+struct/ { n = 0; next }
    /^}[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*;/ {
        name = $0; sub(/^}[ \t]*/, "", name); sub(/[ \t]*;.*$/, "", name)
        if (name == TY) for (i = 1; i <= n; i++) print m[i]
        n = 0; next
    }
    /\([ \t]*\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\)/ {
        match($0, /\([ \t]*\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\)/)
        t = substr($0, RSTART, RLENGTH); gsub(/[()* \t]/, "", t); m[++n] = t
    }')
nslots=$(printf '%s\n' "$slots" | grep -c . || true)
if [ "$nslots" -lt 2 ]; then
    echo "FAILURES: $CODEC_TYPE reads as $nslots slot(s), so the descriptor is"
    echo "      not being found and no device could be held to filling it"
    exit 1
fi
regslots=$(awk '{ print $1 }' "$work/reg.codecslots" 2>/dev/null | head -1)
case ${regslots:-} in
    ''|*[!0-9]*)
        echo "FAILURES: the register states no 'codecslots <n>'. The number of"
        echo "      promises the descriptor makes is what a device carrying one"
        echo "      is signing up to, and prose describing it has gone stale"
        echo "      before now by nobody counting."
        fail=1 ;;
    *)  if [ "$regslots" -ne "$nslots" ]; then
            echo "FAILURES: the register says the page codec is $regslots calls"
            echo "      and $CODEC_TYPE declares $nslots. Say what the ones that"
            echo "      arrived are for."
            fail=1
        fi ;;
esac

: > "$work/codecgap.derived"
for d in $(awk '$1 == "pagecodec" { print $2 }' "$work/trait.derived" | sort -u)
do
    f=$(driver_of "$d")
    [ -n "$f" ] || continue
    init=$(sed 's|/\*.*\*/||' "$f" | awk -v TY="$CODEC_TYPE" '
        !open {
            if ($0 ~ TY "[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*=" && index($0, "{")) {
                open = 1; text = substr($0, index($0, "{") + 1)
            }
            next
        }
        index($0, "}") { text = text " " substr($0, 1, index($0, "}") - 1); exit }
        { text = text " " $0 }
        END { print text }')
    i=0
    for s in $slots; do
        i=$((i + 1))
        v=$(printf '%s' "$init" | awk -F, -v k="$i" '{ s = $k; gsub(/[ \t&]/, "", s); print s }')
        [ "$v" = NULL ] && echo "$d $s" >> "$work/codecgap.derived"
    done
done
sort -u "$work/codecgap.derived" -o "$work/codecgap.derived"
sort -u "$work/reg.codecgap" 2>/dev/null > "$work/codecgap.reg" || : > "$work/codecgap.reg"

guard_held=0
guard_hold "$work/codecgap.reg" "$work/codecgap.derived" \
    "recorded as leaving a codec slot empty and now filling it. Retire
      the line, so that a device said to want nothing from the shared
      machinery is one that still wants nothing:" \
    "leaving a codec slot empty with nothing said about it. A device
      carrying a descriptor promises every slot of it; write in
      tests/device-facts what this one means by refusing:"
[ "$guard_held" -eq 0 ] || fail=1

# a gap says nothing unless it says why
while read -r gd gs; do
    [ -n "$gs" ] || continue
    n=$(awk -v k="codecgap:$gd:$gs" '$1 == k { print $2 }' "$work/reg.prose")
    if [ -z "$n" ]; then
        echo "FAIL: the register records $gd leaving $gs empty and gives no"
        echo "      reason. What the device means by refusing a call the rest"
        echo "      of the family takes is the whole of the entry."
        fail=1
    fi
done < "$work/codecgap.reg"

# ---- what a difference is, and where it is going
#
# A reason says why a difference exists. It does not say whether anyone
# means to keep it, and those are separate questions: a family can be
# told exactly why two of its members differ and still not know whether
# that is the design or a debt. So every entry that is a difference
# rather than a value carries one of three words.
#
#   settled   meant, or forced by something outside this tree, and not
#             going away. Nothing further is owed.
#   thorn     lived with for now. Owes what would remove it and what
#             removing it costs, because a debt nobody priced is a debt
#             nobody pays.
#   heading   the direction of travel. Owes the state being moved to and
#             what closes the gap, so the entry can be read as a
#             description of where this is going rather than of where it
#             has stopped.
#
# state is exempt: two devices holding different values there are not
# differing about anything. method is not asked yet, and the entry for
# that is below.
awk '$3 == "question" || $3 == "elsewhere" || $3 == "open" ||
     $3 == "part" || $3 == "trait" || $3 == "tune" { print $2 }' \
    "$work/reg.kind" | sort > "$work/needdisp"
while read -r k; do
    n=$(awk -v k="$k" '$1 == k { c++ } END { print c + 0 }' "$work/reg.disp")
    if [ "$n" -ne 1 ]; then
        echo "FAIL: $k states $n dispositions and must state exactly one."
        echo "      Say settled, thorn or heading: whether this difference is"
        echo "      meant, tolerated, or being removed. A reason says why it is"
        echo "      so; a disposition says whether anyone means it to stay."
        fail=1
        continue
    fi
    d=$(awk -v k="$k" '$1 == k { print $2 }' "$work/reg.disp")
    case "$d" in
        settled) ;;
        thorn|heading)
            if ! awk -v k="disp:$k:$d" '$1 == k && $2 >= 1 { f = 1 }
                                        END { exit f ? 0 : 1 }' \
                 "$work/reg.prose"; then
                echo "FAIL: $k is a $d and says nothing under it. A $d owes what"
                echo "      would close it and what closing it costs; without"
                echo "      that it is a settled difference wearing another word."
                fail=1
            fi ;;
        *)  echo "FAIL: $k has disposition '$d', which is not one of settled,"
            echo "      thorn or heading."
            fail=1 ;;
    esac
done < "$work/needdisp"

# The thorns are reported whether or not anything failed. A debt that is
# only visible when a guard is red is a debt that accumulates quietly.
if [ -s "$work/reg.disp" ]; then
    awk '$2 == "thorn" { print "      " $1 }' "$work/reg.disp" | sort > "$work/thorns"
    if [ -s "$work/thorns" ]; then
        echo "NOTE: the device register carries $(wc -l < "$work/thorns" | tr -d ' ') thorn(s):"
        cat "$work/thorns"
    fi
fi

awk '$3 == "question" { print $2 }' "$work/reg.kind" | sort > "$work/questions"
while read -r k; do
    [ -n "$k" ] || continue
    nq=$((nq + 1))
    fam=$(awk -v k="$k" '$1 == k { print $2 }' "$work/reg.family")
    if [ -z "$fam" ]; then
        echo "FAIL: $k is a question and states no family answer"
        fail=1
        continue
    fi
    if ! awk -v k="kind:$k" '$1 == k && $2 >= 3 { f = 1 } END { exit f ? 0 : 1 }' \
         "$work/reg.prose"; then
        echo "FAIL: $k is a question and says in fewer than three lines what it"
        echo "      asks. A question needs what the entry is for, what the"
        echo "      family answer means and what answering otherwise buys."
        fail=1
    fi
    carriers=$(awk -v k="$k" '$1 == k { print $2 }' "$work/carry" | sort -u)
    atfamily=0
    for d in $carriers; do
        [ "$d" = record ] && continue
        got=$(awk -v k="$k" -v d="$d" '$1 == d && $2 == k { print $3 }' "$work/state")
        rec=$(awk -v k="$k" -v d="$d" '$1 == k && $2 == d { print $3 }' "$work/reg.answer")
        file=$(awk -v k="$k" -v d="$d" '$1 == k && $2 == d { print $4 }' "$work/reg.answer")
        if [ -z "$rec" ]; then
            echo "FAIL: $d carries $k and tests/device-facts records no answer"
            echo "      for it. Adding a question costs the whole family an"
            echo "      answer each; that is what makes it a family question."
            fail=1
            continue
        fi
        if [ "$rec" != "$got" ]; then
            echo "FAIL: $d answers $k with '$got' and tests/device-facts"
            echo "      records '$rec'"
            fail=1
            continue
        fi
        hasprose=$(awk -v k="answer:$k:$d" '$1 == k { print $2 }' "$work/reg.prose")
        [ -n "$hasprose" ] || hasprose=0
        if [ "$fam" != "per-device" ] && [ "$got" = "$fam" ]; then
            atfamily=$((atfamily + 1))
            if [ "$hasprose" -gt 0 ] || [ "$file" != "-" ]; then
                echo "FAIL: $d answers $k as the family does and keeps a reason"
                echo "      for differing. The reason has stopped being about"
                echo "      anything; take it out with the difference."
                fail=1
            fi
            continue
        fi
        # a difference, or a question with no family answer to inherit
        if [ "$hasprose" -lt 3 ]; then
            echo "FAIL: $d answers $k with '$got' and says why in fewer than"
            echo "      three lines. Say what the difference is, why this"
            echo "      device makes it, and what would make it false."
            fail=1
        else
            nreason=$((nreason + 1))
        fi
        if [ "$file" = "-" ]; then
            echo "FAIL: $d answers $k with '$got' and names no file it states"
            echo "      that in. An answer nobody stated is one the device was"
            echo "      copied into, which is how a claim made about one device"
            echo "      comes to be made on behalf of another."
            fail=1
        elif [ ! -f "$srcdir/$file" ]; then
            echo "FAIL: $d answers $k in $file, which is not there"
            fail=1
        elif ! grep -q "/$(printf '%s' "$k" | sed 's/\./\\./g')\([^A-Za-z0-9_.]\|\$\)" \
                  "$srcdir/$file"; then
            echo "FAIL: $d answers $k with '$got' and $file does not state $k"
            fail=1
        fi
    done
    if [ "$fam" != "per-device" ] && [ "$atfamily" -eq 0 ]; then
        echo "FAIL: no device answers $k as tests/device-facts says the family"
        echo "      does ($fam). A family answer nobody gives is not one."
        fail=1
    fi
    # the record answers its target's answer, over every target
    if awk -v k="$k" '$1 == k && $2 == "record" { f = 1 } END { exit f ? 0 : 1 }' \
       "$work/carry"; then
        while read -r t; do
            rv=$(awk -v k="$k" -v t="$t" '$1 == t && $2 == k { print $3 }' "$work/rstate")
            tv=$(awk -v k="$k" -v t="$t" '$1 == t && $2 == k { print $3 }' "$work/state")
            if [ "$rv" != "$tv" ]; then
                echo "FAIL: a record made for $t answers $k with '$rv' and $t"
                echo "      itself answers '$tv'. A record answers for the"
                echo "      device it stands in front of, or it is played into"
                echo "      a device the marks were not made for."
                fail=1
            fi
        done < "$work/tasked"
    fi
done < "$work/questions"
if [ "$nq" -eq 0 ]; then
    echo "FAILURES: tests/device-facts asks no questions; fix the register"
    exit 1
fi

# ---------------------------------------------------------------------
# One answer another answer settles
#
# A device that shows a grey as a pattern of pixels ranks each pixel
# against a threshold the screen in force picks from its position, and
# the screen is the program's to set. So such a device cannot say that
# the row it shows over a row it holds no pixel of is one row wherever
# it stands: what stands for a colour at one row is what the cell says
# there, and the cell is not the class's to know. ScreenPaint therefore
# obliges .groundvaries true.
#
# Only that way round. A device could vary its ground for a reason of
# its own and store the grey it is handed, so the two entries stay
# separate and this holds the one implication between them rather than
# folding them.
#
# Asked of the record as well, which takes both off the class it plays
# into by two different routes in src/lib/xpost_dev_record.c -- the
# screen through a branch of its own and the ground through the list of
# facts beside it -- so a record standing in front of a screening device
# and answering that its ground is one row is the drift this catches.
nscreen=0
for side in state rstate; do
    if [ "$side" = state ]; then
        who=$(awk '$2 == "ScreenPaint" { print $1 }' "$work/state" | sort -u)
    else
        who=$(awk '$2 == "ScreenPaint" { print $1 }' "$work/rstate" | sort -u)
    fi
    for d in $who; do
        nscreen=$((nscreen + 1))
        gv=$(awk -v d="$d" -v k=".groundvaries" '$1 == d && $2 == k { print $3 }' \
             "$work/$side")
        [ -n "$gv" ] || gv='(nothing)'
        [ "$gv" = true ] && continue
        if [ "$side" = state ]; then
            echo "FAIL: $d shows a grey as a pattern of pixels and answers"
            echo "      .groundvaries '$gv'. The cell it ranks against is the"
            echo "      screen the program set, so no one row stands for its"
            echo "      ground down the page."
        else
            echo "FAIL: a record made for $d carries ScreenPaint and answers"
            echo "      .groundvaries '$gv'. It writes down marks a screen will"
            echo "      be applied to and puts its page out as though one row"
            echo "      stood for its ground everywhere."
        fi
        fail=1
    done
done
if [ "$nscreen" -eq 0 ]; then
    echo "FAILURES: no device states ScreenPaint, so the rule that a"
    echo "      screening device varies its ground was held over nothing"
    exit 1
fi

# ---------------------------------------------------------------------
# The recorder carries its target's entries and its own, and nothing else
#
# For every entry: the record either carries it over every target, which
# makes it the record's own, or over exactly the targets that carry it,
# which makes it the target's showing through. Over some other set it is
# neither, and a page played into that target is played into a device
# holding an entry the marks were not recorded under.
nmirror=0
while read -r k; do
    over=$(awk -v k="$k" '$2 == k { print $1 }' "$work/rstate" | sort -u)
    n=$(printf '%s\n' "$over" | grep -c . || true)
    [ "$n" -eq 0 ] && continue
    [ "$n" -eq "$recasked" ] && continue
    theirs=$(awk -v k="$k" '$2 == k { print $1 }' "$work/state" | sort -u \
             | grep -xF -f "$work/tasked" 2>/dev/null | sort -u)
    if [ "$over" != "$theirs" ]; then
        echo "FAIL: a record carries $k for [$(printf '%s ' $over)] and the"
        echo "      targets stating it are [$(printf '%s ' $theirs)]"
        fail=1
    else
        nmirror=$((nmirror + 1))
    fi
done < "$work/keys.seen"

# ---- and no page-device parameter may name one of those methods.
#
# A device's painting procedures live in the same dictionary as the
# page-device parameters, so a request key carried onto the device could
# name one of them and replace it -- which is what a request naming
# /Flush or /Destroy used to do. setpagedevice now carries only the keys
# in .pagedeviceparams (data/device.ps) and drops the rest, which closes
# that only while the two lists stay disjoint: a parameter added under a
# name a device already uses for a method reopens it, and nothing about
# either list on its own would say so.
#
# The method names come from the register above rather than from a
# second hand list, so a method renamed is a method this still knows.
awk '/^method/ { print $2 }' "$src/tests/device-facts" | sort -u > "$work/methodnames"
sed -n '/\.pagedeviceparams <</,/^>> put/p' "$src/data/device.ps" \
    | tr ' ' '\n' | sed -n 's|^/\([A-Za-z_][A-Za-z0-9_]*\)$|\1|p' | sort -u > "$work/paramnames"
# The list is what keeps a request from reaching those methods, so a list
# a program could add to is a list that could be made to let one through.
# It is declared read-only where it is built; nothing writes it.
if ! sed -n '/\.pagedeviceparams <</,/>> readonly put/p' "$src/data/device.ps" \
     | grep -q '>> readonly put'; then
    echo "FAIL: the page-device parameter list is not declared read-only,"
    echo "      so a program reaching it could widen it to name a device method"
    fail=1
fi

if [ ! -s "$work/paramnames" ]; then
    echo "FAIL: no page-device parameters were read from data/device.ps, so"
    echo "      the parameters and the device methods were never compared"
    fail=1
elif [ ! -s "$work/methodnames" ]; then
    echo "FAIL: no device methods were read from the register, so the"
    echo "      parameters and the device methods were never compared"
    fail=1
else
    both=$(grep -xF -f "$work/paramnames" "$work/methodnames" || true)
    if [ -n "$both" ]; then
        echo "FAIL: these are both a device method and a page-device parameter,"
        echo "      so a request naming one reaches the method:"
        printf '        %s\n' $both
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: the device facts and their register disagree"
    exit 1
fi

skipped=''
[ -s "$work/unmade" ] &&
    skipped=", $(grep -c . "$work/unmade") not built into this interpreter"
nall=$(grep -c . "$work/keys.seen" || true)
echo "SUCCESS ($nall entries stated by $nasked device(s) and every one classified;\
 $nq question(s) answered device by device with $nreason reason(s) for differing;\
 the recorder held to its target over $recasked target(s), $nmirror entry(ies)\
 of theirs showing through$skipped)"
exit 0
