#!/bin/sh
#  A shading a device keeps whole is checked for the structure the painters
#  check as they walk it, so a program is told the same thing about the same
#  shading whether it is painted or written out.
#   $1  path to the built xpost binary
set -u
XPOST_ARG=${1:?usage: check-shading-device-parity.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_interpreter "$XPOST_ARG"
XPOST=$xpost
guard_workdir
tmp=$work
fail=0

emit() { cat > "$tmp/$1.ps"; }

emit vpr_short <<'EOF'
<< /ShadingType 5 /ColorSpace /DeviceRGB /VerticesPerRow 0
   /DataSource [ 0 0  1 0 0   10 0  0 1 0 ] >> shfill showpage
EOF
emit vpr_unfillable <<'EOF'
<< /ShadingType 5 /ColorSpace /DeviceRGB /VerticesPerRow 1000000000
   /DataSource [ 0 0  1 0 0   10 0  0 1 0 ] >> shfill showpage
EOF
emit patch_first_flag <<'EOF'
<< /ShadingType 7 /ColorSpace /DeviceRGB /DataSource
   [ 3  0 0 10 0 20 0 30 0 30 10 30 20 30 30 20 30 10 30 0 30 0 20 0 10
     1 0 0  0 1 0  0 0 1  1 1 0 ] >> shfill showpage
EOF
emit samples_short <<'EOF'
<< /ShadingType 2 /ColorSpace /DeviceGray /Coords [0 0 200 0] /Function
   << /FunctionType 0 /Domain [0 1] /Range [0 1] /Size [100000]
      /BitsPerSample 8 /DataSource (ab) >> >> shfill showpage
EOF
emit size_negative <<'EOF'
<< /ShadingType 2 /ColorSpace /DeviceGray /Coords [0 0 200 0] /Function
   << /FunctionType 0 /Domain [0 1] /Range [0 1] /Size [-5]
      /BitsPerSample 2000000000 /DataSource (abcd) >> >> shfill showpage
EOF
emit good_lattice <<'EOF'
<< /ShadingType 5 /ColorSpace /DeviceRGB /VerticesPerRow 2 /DataSource
   [ 0 0  1 0 0   200 0  0 1 0   0 200  0 0 1   200 200  1 1 0 ] >> shfill showpage
EOF
emit good_sampled <<'EOF'
<< /ShadingType 2 /ColorSpace /DeviceGray /Coords [0 0 200 0] /Function
   << /FunctionType 0 /Domain [0 1] /Range [0 1] /Size [4]
      /BitsPerSample 8 /DataSource (\000\100\200\377) >> >> shfill showpage
EOF

errof() {
    "$XPOST" -q -d "$1" -o "$tmp/out" "$2" < /dev/null 2>&1 \
        | sed -n 's/.*Error: \([a-z]*\).*/\1/p' | head -1
}

for c in vpr_short vpr_unfillable patch_first_flag samples_short size_negative \
         good_lattice good_sampled; do
    r=$(errof png "$tmp/$c.ps")
    w=$(errof pdfwrite "$tmp/$c.ps")
    if [ "$c" = good_lattice ] && [ -n "$r$w" ]; then
        echo "$c: a well-formed shading raised '${r:-none}'/'${w:-none}'; the probe is not measuring what it thinks"
        fail=1
    fi
    if [ "$r" != "$w" ]; then
        echo "$c: painted answers '${r:-none}', written answers '${w:-none}'"
        fail=1
    fi
done

# the good ones must still reach the device whole, not be decomposed
for c in good_lattice good_sampled; do
    "$XPOST" -q -d pdfwrite -o "$tmp/$c.pdf" "$tmp/$c.ps" < /dev/null > /dev/null 2>&1
    if ! grep -a -q '/ShadingType' "$tmp/$c.pdf"; then
        echo "$c: the device was given no shading to keep"
        fail=1
    fi
done

[ "$fail" -eq 0 ] || { echo "shading-device-parity: a shading is judged differently by the two routes."; exit 1; }
echo "SUCCESS (7 shadings judged the same painted and written)"
