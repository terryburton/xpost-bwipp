#!/bin/sh
# Meson test wrapper: the -p device-parameter switch is the request
# channel's command-line spelling, and its refusals are loud.
#
# -p key=value records a codec tuning key's default for the run through
# the same channel an embedder's options take: onto the device classes,
# so every device made carries it and a program's own setpagedevice
# request still overrides it. One semantic model, three spellings --
# so what is asserted first is byte identity: the page rendered under a
# -p default is the page rendered under the same word asked for by the
# program.
#
# And the switch refuses everything outside the rosters, before
# anything is rendered: a key no device of this build reads is refused
# naming the keys that exist, and a value outside its key's range or
# vocabulary is refused with the key's own terms. A misspelling that
# fell back to a default silently would choose the dearest filters, or
# somebody else's quality, and report nothing -- the defect class the
# device modes and the filter vocabulary were closed against.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac
. "$(dirname "$0")/verdict.sh"

verdict_workdir

# flat vertical bars: the page the filter vocabulary trades on
cat > "$work/page.ps" <<'PSEOF'
0 setgray
20 40 580 { /x exch def
  newpath x 100 moveto x 4 add 100 lineto x 4 add 600 lineto x 600 lineto
  closepath fill } for
showpage
PSEOF

render() {   # <outfile> <device> [xpost args...]
    r_o=$1; r_d=$2; shift 2
    out=$("$xpost" -q -d "$r_d" "$@" -o "$r_o" "$work/page.ps" </dev/null 2>&1)
    verdict_run "$?" "$out" "the $r_d run" || exit 1
    [ -s "$r_o" ] || { echo "FAIL: $r_o was not written"; exit 1; }
}

# ---- one model, three spellings ----
render "$work/default.png"  png
render "$work/reqnone.png"  png
{ printf '<< /png_filter (none) >> setpagedevice\n'; cat "$work/page.ps"; } \
    > "$work/reqnone.ps"
out=$("$xpost" -q -d png -o "$work/reqnone.png" "$work/reqnone.ps" </dev/null 2>&1)
verdict_run "$?" "$out" "the request-channel run" || exit 1
render "$work/pnone.png"    png -p png_filter=none

cmp -s "$work/pnone.png" "$work/reqnone.png" || {
    echo "FAIL: -p png_filter=none and the same word asked for by the"
    echo "      program produce different bytes; the switch is not the"
    echo "      request channel's spelling"
    exit 1; }
cmp -s "$work/pnone.png" "$work/default.png" && {
    echo "FAIL: -p png_filter=none produced the default bytes; the switch"
    echo "      changed nothing"
    exit 1; }
echo "-p is the request channel's spelling, byte for byte"

# ---- the default trades the declared way on the jpeg scale ----
render "$work/default.jpg" jpeg
render "$work/q10.jpg"     jpeg -p jpeg_quality=10
dsz=$(wc -c < "$work/default.jpg" | tr -d ' ')
qsz=$(wc -c < "$work/q10.jpg" | tr -d ' ')
[ "$qsz" -lt "$dsz" ] || {
    echo "FAIL: -p jpeg_quality=10 ($qsz bytes) is not smaller than the"
    echo "      default ($dsz bytes); the default did not reach the codec"
    exit 1; }
echo "-p jpeg_quality trades the declared way ($qsz < $dsz)"

# ---- a program's own request still overrides the default ----
{ printf '<< /png_filter (adaptive) >> setpagedevice\n'; cat "$work/page.ps"; } \
    > "$work/override.ps"
out=$("$xpost" -q -d png -p png_filter=none -o "$work/override.png" \
      "$work/override.ps" </dev/null 2>&1)
verdict_run "$?" "$out" "the override run" || exit 1
cmp -s "$work/override.png" "$work/default.png" || {
    echo "FAIL: a program's own request did not override the -p default"
    exit 1; }
echo "a program's request overrides the -p default"

# ---- the refusals, loud and before anything is rendered ----
refuse() {  # <label> <expected-text> [xpost args...]
    rf_label=$1; rf_want=$2; shift 2
    rm -f "$work/refused.out"
    rf_out=$("$xpost" -q -d png "$@" -o "$work/refused.out" \
             "$work/page.ps" </dev/null 2>&1)
    rf_st=$?
    [ "$rf_st" -ne 0 ] || {
        echo "FAIL: $rf_label finished cleanly; a word outside the roster"
        echo "      has been accepted silently"
        exit 1; }
    case $rf_out in
        *"$rf_want"*) ;;
        *) echo "FAIL: $rf_label was not refused in its own terms; the run said:"
           printf '%s\n' "$rf_out" | sed 's/^/      /'
           exit 1 ;;
    esac
    [ ! -s "$work/refused.out" ] || {
        echo "FAIL: $rf_label wrote a page anyway"; exit 1; }
    echo "$rf_label is refused: no page, naming its terms"
}

refuse "an unknown key"   'takes no key "zebra"'                    -p zebra=1
refuse "an unknown key"   'png_compression_level'                   -p zebra=1
refuse "a word outside the vocabulary" \
       'takes no word "fastest"; the words it takes are: adaptive'  -p png_filter=fastest
refuse "a value outside the range" \
       'takes no value "101"; the value is an integer from 0 to 100' -p jpeg_quality=101
refuse "a value that is not a number" \
       'takes no value "abc"'                                       -p jpeg_quality=abc
refuse "an operand with no value"     'key=value'                   -p png_filter

echo "SUCCESS"
exit 0
