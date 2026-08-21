#!/bin/sh
# sethalftone reads a halftone cell's samples from its Thresholds source and
# then drains the rest of that source. A source that goes on decoding far
# more than the cell can hold -- a decompression bomb behind a filter -- was
# drained to its end, so a one-sample cell fed a stream that expanded to
# gigabytes cost minutes of CPU. The drain is now capped. Hand it a source
# far larger than a one-sample cell needs and require it to stop near the
# cap rather than read to the source's end.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"
# the run is made from the scratch directory so the Thresholds source can be
# named relatively: a native interpreter driven by a POSIX shell cannot open
# a file named to it by the shell's absolute path
xpost=$(path_anchor "$xpost")
verdict_workdir

# Ten megabytes of zeros -- far more than a one-sample cell's one byte. No
# compression, so the source is exactly its size and the position it leaves
# is a direct reading of how far the drain went.
head -c 10485760 /dev/zero > "$work/big.dat" 2>/dev/null \
    || { echo "run-halftone-drain-test: skipped (could not make the source)"; exit 77; }

cat > "$work/ht.ps" <<'PS'
/f (big.dat) (r) file def
<< /HalftoneType 6 /Width 1 /Height 1 /Thresholds f >> sethalftone
(POS=)print f fileposition 20 string cvs print (\n)print flush
PS

out=$(cd "$work" && run_limited 20 "$xpost" -q --no-sandbox -d null ht.ps </dev/null 2>&1)
st=$?
verdict_run "$st" "$out" "the halftone drain run" || exit 1

pos=$(printf '%s\n' "$out" | sed -n 's/^POS=\([0-9][0-9]*\).*/\1/p')
if [ -z "$pos" ]; then
    echo "FAIL: the run did not report the Thresholds file position"
    echo "      output was: $out"
    exit 1
fi

# The cap is sixteen four-kilobyte reads. A bound well above that and well
# below the source's ten megabytes separates a capped drain from one that
# read on toward the end of the source.
if [ "$pos" -ge 131072 ]; then
    echo "FAIL: sethalftone drained $pos bytes of the Thresholds source -- the"
    echo "      drain read past 128 KB toward the source's end, so it is not capped"
    exit 1
fi

echo "run-halftone-drain-test: ok (drained $pos bytes, within the cap)"
exit 0
