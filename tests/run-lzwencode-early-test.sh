#!/bin/sh
# LZWEncode reads an EarlyChange parameter. It is 0 or 1 (PLRM 3.13.3); the
# encoder's code-table reset is gated on a sum that includes it, so a value
# outside that range -- a negative one, or one large enough to overflow the
# sum -- defeats the reset and lets the code counter run past the fixed
# table the encoder writes into: a heap overflow with the bytes the encoder
# emits. Require an out-of-range EarlyChange to be refused before a filter
# is made, and a valid one to encode and round-trip so the guard has not
# broken ordinary use.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"
# the runs are made from the scratch directory so their files can be named
# relatively: a native interpreter driven by a POSIX shell cannot open a
# file named to it by the shell's absolute path
xpost=$(path_anchor "$xpost")
verdict_workdir

# An out-of-range EarlyChange is refused at the filter, before any encoding.
cat > "$work/bad.ps" <<'PS'
/f (o.lzw) (w) file def
{ f << /EarlyChange -1000000 >> /LZWEncode filter } stopped
    { $error /errorname get /rangecheck eq { (REFUSED-NEG\n) } { (WRONG-ERROR\n) } ifelse }
    { (ACCEPTED-NEG\n) } ifelse print
{ f << /EarlyChange 2 >> /LZWEncode filter } stopped
    { $error /errorname get /rangecheck eq { (REFUSED-2\n) } { (WRONG-ERROR\n) } ifelse }
    { (ACCEPTED-2\n) } ifelse print
flush
PS
out=$(cd "$work" && run_limited 10 "$xpost" -q --no-sandbox -d null bad.ps </dev/null 2>&1)
st=$?
verdict_run "$st" "$out" "the out-of-range EarlyChange run" || exit 1
case $out in
    *ACCEPTED-NEG*) echo "FAIL: LZWEncode accepted a negative EarlyChange"; exit 1 ;;
    *WRONG-ERROR*)  echo "FAIL: LZWEncode gave the wrong error for an out-of-range EarlyChange"; exit 1 ;;
    *REFUSED-NEG*) ;;
    *) echo "FAIL: unexpected output: $out"; exit 1 ;;
esac
case $out in
    *REFUSED-2*) ;;
    *) echo "FAIL: LZWEncode did not refuse EarlyChange 2"; exit 1 ;;
esac
echo "out-of-range EarlyChange refused"

# A valid EarlyChange still encodes and round-trips.
cat > "$work/rt.ps" <<'PS'
/orig 5000 string def
0 1 orig length 1 sub { /i exch def orig i i 256 mod put } for
/ef (rt.lzw) (w) file def
/enc ef << /EarlyChange 1 >> /LZWEncode filter def
enc orig writestring enc closefile ef closefile
/df (rt.lzw) (r) file << /EarlyChange 1 >> /LZWDecode filter def
/back 5000 string def
df back readstring pop /got exch def df closefile
got orig eq { (RT-OK\n) } { (RT-MISMATCH\n) } ifelse print flush
PS
out=$(cd "$work" && run_limited 10 "$xpost" -q --no-sandbox -d null rt.ps </dev/null 2>&1)
st=$?
verdict_run "$st" "$out" "the round-trip run" || exit 1
case $out in
    *RT-OK*) ;;
    *) echo "FAIL: a valid LZWEncode/LZWDecode round-trip did not reproduce its data"; exit 1 ;;
esac
echo "valid EarlyChange round-trips"

echo "run-lzwencode-early-test: ok"
exit 0
