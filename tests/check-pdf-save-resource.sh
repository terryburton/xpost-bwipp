#!/bin/sh
# A resource the writer files -- an image, a form, a tiling pattern -- is
# named by the content stream and written as an object of its own. The two
# have to survive together: content that names an object the document does
# not carry is a file a consumer refuses, and it refuses it silently as far
# as the producing run is concerned, which exits reporting success.
#
# What puts them at risk is save and restore. The content is accumulated
# outside VM and keeps everything written into it; the record of what that
# content refers to is an ordinary object in memory. A program that opens a
# save after the page begins and restores it before the page ends must not
# be able to take the record away and leave the reference.
#
# Encapsulating a figure in save/restore is the most ordinary thing a
# PostScript program does, so each case below is a resource first filed
# inside such a bracket.
#
# CompressPages false leaves the content readable, so the names it uses can
# be read against the names the resources define.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-pdf-save-resource.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0

mk() {  # $1 name, $2 body
    cat > "$work/$1.ps" <<EOF
<< /CompressPages false >> setdistillerparams
$2
showpage
EOF
    "$xpost" -q -d pdfwrite -o "$work/$1.pdf" "$work/$1.ps" </dev/null >/dev/null 2>&1 \
        || { echo "FAIL: the $1 run errored"; fail=1; }
}

# every XObject and Pattern the content names is defined by the resources
check() {  # $1 name
    f=$work/$1.pdf
    [ -s "$f" ] || { echo "FAIL: $1 produced no file"; fail=1; return; }
    # Read with an extended regular expression: `\|` inside a basic one is
    # a GNU extension, and the sed in the base system of macOS takes it as
    # a literal bar. The list would come out empty there and every case
    # would report that it named no resource and tested nothing.
    used=$(grep -aoE '/(Im|Fm|P|GS|Sh)[0-9]*[[:space:]]*(Do|scn|gs|sh)' "$f" \
           | awk '{ sub(/^\//, ""); sub(/[[:space:]]*(Do|scn|gs|sh)$/, ""); print }' \
           | sort -u)
    [ -n "$used" ] || { echo "FAIL: $1 names no resource, so it tests nothing"; fail=1; return; }
    for n in $used; do
        # a resource is defined either as an indirect reference to an
        # object of its own or, for the small ones, written out where it
        # is named
        grep -q "/$n [0-9][0-9]* 0 R" "$f" || grep -q "/$n <<" "$f" \
            || { echo "FAIL: $1 draws /$n and the document defines no such object"; fail=1; }
    done
}

# an image first drawn inside the bracket
mk image 'save
gsave 100 100 translate 60 60 scale
4 4 8 [4 0 0 -4 0 4] { <ff0000 00ff00 0000ff ffffff
                        00ff00 0000ff ffffff ff0000
                        0000ff ffffff ff0000 00ff00
                        ffffff ff0000 00ff00 0000ff> } false 3 colorimage
grestore
restore'

# a form first placed inside the bracket
mk form '/F << /FormType 1 /BBox [0 0 40 40] /Matrix [1 0 0 1 0 0]
              /PaintProc { pop 0 0 40 40 rectfill } >> def
save
gsave 100 100 translate F execform grestore
restore'

# a tiling pattern first painted inside the bracket
mk pattern 'save
<< /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 10 10]
   /XStep 10 /YStep 10
   /PaintProc { pop 0 0 5 5 rectfill } >> matrix makepattern
setpattern
100 100 200 200 rectfill
restore'

# an ExtGState first selected inside the bracket. MEASURED: this is the
# form the corpus breaks on far more often than the XObjects -- a plain
# page with no overprint names no ExtGState and declares none, so what
# fails here is the record being taken away and not a declaration that
# was never made.
mk extgstate 'save
true setoverprint
100 100 200 200 rectfill
restore'

# the bracket around the page end, so the record is gone before the page
# that refers to it is written out
mk atshowpage '/F << /FormType 1 /BBox [0 0 40 40] /Matrix [1 0 0 1 0 0]
                    /PaintProc { pop 0 0 40 40 rectfill } >> def
save
gsave 100 100 translate F execform grestore'
cat >> "$work/atshowpage.ps" <<EOF
restore
EOF

# a shading, and a stitching function over two subfunctions, so that the
# objects a shading brings with it are filed as well as the shading
mk shading 'save
0 0 300 300 rectclip
<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 300 0] /Extend [true true]
   /Function << /FunctionType 3 /Domain [0 1] /Bounds [0.5] /Encode [0 1 0 1]
                /Functions [ << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 1 0] /N 1 >>
                             << /FunctionType 2 /Domain [0 1] /C0 [0 1 0] /C1 [0 0 1] /N 1 >> ] >> >> shfill
restore'

for c in image form pattern extgstate atshowpage shading; do check $c; done

test $fail -eq 0 && echo "OK: a resource filed inside save/restore is still there at page end"
exit $fail
