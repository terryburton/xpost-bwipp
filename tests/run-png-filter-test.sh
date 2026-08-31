#!/bin/sh
# Meson test wrapper: which row filters the png devices may choose
# between is a closed vocabulary, and the page is the same page under
# every word in it.
#
# Choosing a filter is a walk of every row under each candidate, so the
# choice is paid per row written; png_filter is how a caller who knows
# its pages narrows the menu. What must hold is fourfold, and each
# check below carries one of them:
#
#   - saying nothing and saying /adaptive are the same run, byte for
#     byte, because the default is the library's own whole menu and a
#     word for the default may not become a third behaviour;
#   - every word in the vocabulary yields a well-formed file holding
#     exactly the pixels the default holds -- a filter arranges bytes
#     for the compressor and may not touch the image;
#   - on a page of flat vertical runs -- the shape the option exists
#     for -- refusing the codec every filter but "none" costs bytes
#     against either menu that includes a vertical predictor. That is
#     the direction the option trades in: fewer candidate walks, larger
#     file, the caller's choice to make;
#   - a word outside the vocabulary is refused by name, with the
#     roster, and no file is written. A misspelling that fell back to
#     the default silently would choose the dearest filters and report
#     nothing, which is the defect class the device modes were closed
#     against.
#
# The knob is a page-device key: each run below asks for its word with
# a setpagedevice request prepended to the page, which is the channel a
# program configures a device by, and the run saying nothing carries no
# request at all.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac
. "$(dirname "$0")/verdict.sh"

verdict_workdir

# the shape the option exists for: flat vertical bars over white
cat > "$work/page.ps" <<'PSEOF'
0 setgray
20 40 580 { /x exch def
  newpath x 100 moveto x 4 add 100 lineto x 4 add 600 lineto x 600 lineto
  closepath fill } for
showpage
PSEOF

render() {   # <outfile> [filter-word]
    _o=$1
    if [ $# -gt 1 ]; then
        { printf '<< /png_filter (%s) >> setpagedevice\n' "$2"
          cat "$work/page.ps"; } > "$work/drive.ps"
    else
        cat "$work/page.ps" > "$work/drive.ps"
    fi
    out=$("$xpost" -q -d png -o "$_o" "$work/drive.ps" </dev/null 2>&1)
    verdict_run "$?" "$out" "the png run" || exit 1
    [ -s "$_o" ] || { echo "FAIL: $_o was not written"; exit 1; }
    # the eight bytes every png opens with
    sig=$(od -An -N8 -tu1 "$_o" | tr -s ' ' | sed 's/^ //;s/ $//')
    [ "$sig" = "137 80 78 71 13 10 26 10" ] || {
        echo "FAIL: $_o does not open as a png ($sig)"; exit 1; }
}

render "$work/default.png"
render "$work/adaptive.png" adaptive
render "$work/nsu.png"      none-sub-up
render "$work/none.png"     none

# naming the default is the default
cmp -s "$work/default.png" "$work/adaptive.png" || {
    echo "FAIL: png_filter=adaptive differs from saying nothing; the word"
    echo "      for the default has become a third behaviour"
    exit 1; }
echo "adaptive is the default, byte for byte"

# a filter arranges bytes for the compressor and may not touch the image
if command -v python3 >/dev/null 2>&1; then
    python3 - "$work" <<'PYEOF' || exit 1
import struct, sys, zlib
work = sys.argv[1]
def pixels(path):
    d = open(path,'rb').read()
    pos, idat, wh = 8, b'', None
    while pos < len(d):
        ln, typ = struct.unpack('>I4s', d[pos:pos+8])
        if typ == b'IHDR': wh = struct.unpack('>II', d[pos+8:pos+16])
        if typ == b'IDAT': idat += d[pos+8:pos+8+ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    W, H = wh; bpp = 3; stride = W*bpp
    out, prev, i = bytearray(), bytearray(stride), 0
    for y in range(H):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i+stride]); i += stride
        for x in range(stride):
            a = line[x-bpp] if x>=bpp else 0
            b = prev[x]
            c = prev[x-bpp] if x>=bpp else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+(a+b)//2)&255
            elif f==4:
                pp=a+b-c; pa,pb,pc=abs(pp-a),abs(pp-b),abs(pp-c)
                line[x]=(line[x]+(a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)))&255
        out += line; prev = line
    return bytes(out)
ref = pixels(work + "/default.png")
for v in ("nsu", "none"):
    got = pixels(work + "/" + v + ".png")
    if got != ref:
        print("FAIL: png_filter changed the pixels (%s)" % v)
        sys.exit(1)
print("every vocabulary word decodes to the same pixels")
PYEOF
else
    echo "python3 not found; pixel identity not checked here (the byte"
    echo "identity of the default above still holds the image)"
fi

# the direction the option trades in, on the page it exists for
sz() { wc -c < "$1" | tr -d ' '; }
d=$(sz "$work/default.png"); u=$(sz "$work/nsu.png"); n=$(sz "$work/none.png")
[ "$n" -gt "$d" ] && [ "$n" -gt "$u" ] || {
    echo "FAIL: filter none ($n bytes) is not the largest of the three"
    echo "      (adaptive $d, none-sub-up $u) on a page of flat vertical"
    echo "      runs; the size cost the option trades against has gone,"
    echo "      so the vocabulary no longer says what it says"
    exit 1; }
echo "sizes trade the declared way (none $n > adaptive $d, none-sub-up $u)"

# A word outside the vocabulary is refused by name, with the roster.
# The refusal surfaces through the request: setpagedevice re-creates
# the device, the driver refuses the word as the instance is made, and
# the request errors with rangecheck -- so the run fails, the roster is
# on its standard error, and no page is written.
rm -f "$work/refused.png"
{ printf '<< /png_filter (fastest) >> setpagedevice\n'
  cat "$work/page.ps"; } > "$work/refuse.ps"
out=$("$xpost" -q -d png -o "$work/refused.png" \
      "$work/refuse.ps" </dev/null 2>&1) && {
    echo "FAIL: a run asking for an unknown filter word finished cleanly"
    exit 1; }
case $out in
    *'takes no filter "fastest"'*) ;;
    *) echo "FAIL: an unknown filter word was not refused by name; the run said:"
       printf '%s\n' "$out" | sed 's/^/      /'
       exit 1 ;;
esac
case $out in
    *'adaptive, none-sub-up, none'*) ;;
    *) echo "FAIL: the refusal does not name the vocabulary"; exit 1 ;;
esac
[ ! -s "$work/refused.png" ] || {
    echo "FAIL: a refused run wrote a page anyway"; exit 1; }
echo "an unknown word is refused with the roster, and no page is written"

echo "SUCCESS"
exit 0
