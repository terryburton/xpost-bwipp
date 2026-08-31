#!/bin/sh
# Meson wrapper: the pngalpha device must write RGBA where the erased
# page is transparent, marks are opaque, and an explicit white fill
# stays opaque (distinct from the page background); the png device must
# stay plain RGB. Colour types come from the IHDR; pixel semantics are
# checked when python3 is available.
#
# A pixel is read against a colour whose three components differ from
# one another and from the 255 an opaque alpha carries. Black, white and
# transparency are the same bytes under every rearrangement of a pixel's
# components, so a check that asks only for ink at (0,0,0,255) and a
# white fill at (255,255,255,255) is satisfied by a device that wrote
# blue where red goes. The colour below is what says which component
# landed where, and it is asked of both devices.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
skip_if_faceless "$xpost" "this run shows text through a face"

ps=$(mktemp)
outa=$(mktemp)
outrgb=$(mktemp)
trap 'rm -f "$ps" "$outa" "$outrgb"' EXIT INT TERM

cat > "$ps" <<'EOF'
newpath 20 20 moveto 100 20 lineto 100 60 lineto 20 60 lineto closepath fill
1 setgray newpath 120 20 moveto 200 20 lineto 200 60 lineto 120 60 lineto closepath fill
% the colour that tells the components apart: 0.75 0.5 0.25 folds to
% 191 127 63, no two alike and none of them the 255 of an opaque alpha
0.75 0.5 0.25 setrgbcolor
newpath 220 20 moveto 300 20 lineto 300 60 lineto 220 60 lineto closepath fill
% anti-aliased text: the glyph edges are blended against the page, which
% is the device's blending method rather than its rectangle fill
0 setgray /Helvetica findfont 24 scalefont setfont
20 90 moveto (Ag) show
showpage
quit
EOF

# the two images are read below; how each run left is read here, since a
# device whose teardown faults has already written the image the checks
# below open
out=$("$xpost" -q -d pngalpha -o "$outa" "$ps" </dev/null 2>&1)
verdict_run "$?" "$out" "the pngalpha run" || exit 1
out=$("$xpost" -q -d png -o "$outrgb" "$ps" </dev/null 2>&1)
verdict_run "$?" "$out" "the png run" || exit 1

# IHDR colour type: byte 25 of the file (2 = RGB, 6 = RGBA)
ct() { od -An -j25 -N1 -tu1 "$1" | tr -d ' '; }
[ "$(ct "$outa")" = 6 ]   || { echo "FAIL: pngalpha colour type $(ct "$outa"), want 6"; exit 1; }
[ "$(ct "$outrgb")" = 2 ] || { echo "FAIL: png colour type $(ct "$outrgb"), want 2"; exit 1; }
echo "colour types OK (RGBA=6, RGB=2)"

# Every channel is written at its full depth, and the file says so. The
# IHDR bit depth is byte 24; the significant-bits chunk that follows it
# names how many of those bits carry the original values, one byte per
# channel, and it sits at a fixed place because IHDR is a fixed size.
#
# The two must agree. A channel whose significant bits are fewer than
# its depth is one whose samples have been scaled down to fit and must
# be scaled back up before they are written, and nothing here does that
# scaling: the writer hands the library samples that already fill the
# depth. So this is not a fact about the format, it is the condition
# under which handing them over untouched is right, and a change to
# either number has to be made together with a way of restoring the
# bits the other one gives up.
depth() { od -An -j24 -N1 -tu1 "$1" | tr -d ' '; }
sbitname() { od -An -j37 -N4 -c "$1" | tr -d ' '; }
sbits() { od -An -j41 -N"$2" -tu1 "$1" | tr -s ' ' | sed 's/^ //;s/ $//'; }
for f in "$outa:4:pngalpha" "$outrgb:3:png"; do
    img=${f%%:*}; rest=${f#*:}; nch=${rest%%:*}; dev=${rest##*:}
    d=$(depth "$img")
    [ "$d" = 8 ] || { echo "FAIL: $dev bit depth $d, want 8"; exit 1; }
    [ "$(sbitname "$img")" = sBIT ] || {
        echo "FAIL: $dev writes no significant-bits chunk after its header,"
        echo "      so nothing states how many bits of each sample carry a value"
        exit 1; }
    want=8; i=1
    while [ "$i" -lt "$nch" ]; do want="$want 8"; i=$((i+1)); done
    got=$(sbits "$img" "$nch")
    [ "$got" = "$want" ] || {
        echo "FAIL: $dev declares significant bits [$got], want [$want] --"
        echo "      samples narrower than the depth would have to be scaled"
        echo "      back up before they are written, and they are not"
        exit 1; }
done
echo "sample depth OK (8 bits, all of them significant, both devices)"

# Read one image's pixels: the file, and how many bytes a pixel takes in
# it. A four-byte pixel carries the alpha checks as well.
pixels_of() {   # $1 file  $2 bytes a pixel  $3 how the file is named
    python3 - "$1" "$2" "$3" <<'PYEOF'
import struct, sys, zlib
path, bpp, label = sys.argv[1], int(sys.argv[2]), sys.argv[3]
d = open(path,'rb').read()
pos, idat, meta = 8, b'', {}
while pos < len(d):
    ln, typ = struct.unpack('>I4s', d[pos:pos+8])
    if typ == b'IHDR':
        meta['w'], meta['h'] = struct.unpack('>II', d[pos+8:pos+16])
    if typ == b'IDAT':
        idat += d[pos+8:pos+8+ln]
    pos += 12 + ln
raw = zlib.decompress(idat)
W, H = meta['w'], meta['h']
stride = W*bpp
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
def pix(x,y):
    o=(y*W+x)*bpp; return tuple(out[o:o+bpp])

# The three fills, at a point well inside each. The coloured one is the
# only one of them that can tell one component from another; the black
# and the white say that ink and an explicit white reach the file at
# all, which is a different question and one they can answer.
checks = [
    (pix(60,H-40)[:3] == (0,0,0),          "ink is black"),
    (pix(160,H-40)[:3] == (255,255,255),   "an explicit white fill is white"),
    (pix(260,H-40)[:3] == (191,127,63),    "a colour reaches the file component"
                                           " for component, in the order the"
                                           " format declares"),
]

if bpp == 4:
    # A glyph rendered with anti-aliasing has edge pixels only partly
    # covered: their alpha lies strictly between transparent and opaque.
    # A device that painted glyphs without blending would give every
    # pixel one or the other.
    alphas = {out[(y*W+x)*4 + 3] for y in range(H) for x in range(W)}
    partial = {a for a in alphas if 0 < a < 255}
    checks += [
        (pix(2,2)[3] == 0,        "erased page is transparent"),
        (pix(60,H-40)[3] == 255,  "ink is opaque"),
        (pix(160,H-40)[3] == 255, "an explicit white fill is opaque"),
        (pix(260,H-40)[3] == 255, "a coloured fill is opaque"),
        (255 in alphas,           "the text rendered at all"),
        (len(partial) > 0,        "anti-aliased glyph edges are partly covered"),
    ]

bad = [msg for ok,msg in checks if not ok]
for m in bad: print("FAIL: %s: %s" % (label, m))
sys.exit(1 if bad else 0)
PYEOF
}

if command -v python3 >/dev/null 2>&1; then
    fail=0
    pixels_of "$outa" 4 "pngalpha" || fail=1
    pixels_of "$outrgb" 3 "png" || fail=1
    [ "$fail" -eq 0 ] || exit 1
    echo "pixel semantics OK (both devices, components in the declared order)"
else
    echo "python3 not found: pixel semantics not checked"
fi
