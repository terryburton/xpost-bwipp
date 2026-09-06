#!/bin/sh
# A form placed inside a form, where the inner one holds glyphs.
#
# Coverage is the one mark a drawing cannot be moved by a fraction of a
# pixel. A glyph is held as the pixels it rasterised to, and where those
# pixels take their coverage from is the fraction of a pixel its origin
# fell at; every other mark is held as itself and resolved into pixels as
# it is played. So a drawing holding a glyph may only be played where the
# placement is a whole number of pixels from the one it was captured at.
#
# What this holds is that a form knows it is carrying a glyph when the
# glyph belongs to a form it PLACES rather than to itself. A form whose
# own marks are a rectangle and a placement holds no mask of its own, and
# a count that reads only what a drawing holds directly says so -- and
# the outer form is then carried to a fractional placement with the inner
# one's glyphs inside it.
#
# The page below is drawn twice: through the nested forms, and with the
# same marks inline at the same places. They must be the same bytes. The
# placements differ from one another by fractions of a pixel, because a
# placement a whole number of pixels away is one the drawing may be
# played at and would not show this.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-form-glyph-nest.sh <xpost binary>}
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0

cat > "$work/form.ps" <<'EOF'
/Inner <<
  /FormType 1 /BBox [0 0 120 40] /Matrix [1 0 0 1 0 0]
  /PaintProc { pop /Helvetica-Bold 12 selectfont 0 setgray
               4 12 moveto (Nested) show }
>> def
/Outer <<
  /FormType 1 /BBox [0 0 200 80] /Matrix [1 0 0 1 0 0]
  /PaintProc { pop gsave 10 20 translate Inner execform grestore
               0 setgray 2 2 40 6 rectfill }
>> def
gsave 100.5 600.25 translate Outer execform grestore
gsave 100.7 500.4  translate Outer execform grestore
gsave 160.35 400.8 translate Outer execform grestore
gsave 100.5 300.25 translate Outer execform grestore
showpage
EOF

cat > "$work/inline.ps" <<'EOF'
/paint { gsave
  10 20 translate
  /Helvetica-Bold 12 selectfont 0 setgray 4 12 moveto (Nested) show
  grestore
  0 setgray 2 2 40 6 rectfill } def
gsave 100.5 600.25 translate paint grestore
gsave 100.7 500.4  translate paint grestore
gsave 160.35 400.8 translate paint grestore
gsave 100.5 300.25 translate paint grestore
showpage
EOF

# Both routes on a grey device and on a bilevel one: the bilevel page
# says whether a pixel was inked at all, and the grey one says what
# coverage it was given, which is the half a misplaced mask gets wrong.
for dev in pgm pbm; do
    "$xpost" -q -d "$dev" -o "$work/form.$dev" "$work/form.ps" </dev/null >/dev/null 2>&1 \
        || { echo "FAIL: the $dev run through forms errored"; fail=1; continue; }
    "$xpost" -q -d "$dev" -o "$work/inline.$dev" "$work/inline.ps" </dev/null >/dev/null 2>&1 \
        || { echo "FAIL: the $dev run inline errored"; fail=1; continue; }
    if ! cmp -s "$work/form.$dev" "$work/inline.$dev"; then
        n=$(cmp -l "$work/form.$dev" "$work/inline.$dev" 2>/dev/null | wc -l)
        echo "FAIL: on $dev the nested form differs from the same marks inline in $n bytes"
        fail=1
    fi
done

# SCOPE, and the control that says the comparison can fail at all: the
# same nesting with no glyph in it is exact whatever the placement, so a
# difference above is the glyph and not the nesting.
cat > "$work/norm.ps" <<'EOF'
/Inner2 <<
  /FormType 1 /BBox [0 0 120 40] /Matrix [1 0 0 1 0 0]
  /PaintProc { pop 0 setgray 4 4 60 12 rectfill }
>> def
/Outer2 <<
  /FormType 1 /BBox [0 0 200 80] /Matrix [1 0 0 1 0 0]
  /PaintProc { pop gsave 10 20 translate Inner2 execform grestore
               0 setgray 2 2 40 6 rectfill }
>> def
gsave 100.5 600.25 translate Outer2 execform grestore
gsave 100.7 500.4  translate Outer2 execform grestore
gsave 160.35 400.8 translate Outer2 execform grestore
showpage
EOF
cat > "$work/norminline.ps" <<'EOF'
/paint2 { gsave 10 20 translate 0 setgray 4 4 60 12 rectfill grestore
          0 setgray 2 2 40 6 rectfill } def
gsave 100.5 600.25 translate paint2 grestore
gsave 100.7 500.4  translate paint2 grestore
gsave 160.35 400.8 translate paint2 grestore
showpage
EOF
"$xpost" -q -d pgm -o "$work/norm.pgm" "$work/norm.ps" </dev/null >/dev/null 2>&1
"$xpost" -q -d pgm -o "$work/norminline.pgm" "$work/norminline.ps" </dev/null >/dev/null 2>&1
cmp -s "$work/norm.pgm" "$work/norminline.pgm" \
    || { echo "FAIL: nesting without a glyph already differs, so the check above says nothing about glyphs"; fail=1; }

test $fail -eq 0 && echo "OK: a form carrying another form's glyphs lands where it says"
exit $fail
