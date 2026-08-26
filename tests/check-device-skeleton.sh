#!/bin/sh
#
# Guard the device-driver skeleton: the compiled-buffer device fleet must
# reach the shared mechanics through xpost_dev_driver.h rather than
# hand-writing them, so the contract stated there stays the only
# statement of how a device folds operands, reaches its private struct,
# and which rectangle FillRect paints. The PostScript device classes are
# held to the same idea from their side: what every class does the same
# way is written once and referred to.
#
# No exemptions. The two files that used to have them were the two that
# broke the contract: the Windows driver, which cannot be compiled here,
# clamped a negative origin without shrinking the extent and treated the
# far edge as exclusive; and the generic rasteriser, whose two compiled
# base-class fills restated the extent arithmetic in floor space beside
# a helper that truncated. Exempting a file from the rule it breaks
# leaves the rule stated and unenforced, which is the failure this guard
# exists to prevent -- so both are covered, the Windows driver textually,
# since that holds whether or not this platform can build it.
#
# Sources are named rather than globbed out of a directory: a built tree
# leaves object files beside them whose debug information matches every
# pattern here, so a scan would read green where nothing was built and
# red where something was.
#
# Usage: check-device-skeleton.sh <source root>

set -eu

src=${1:?usage: check-device-skeleton.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
# read a tree whose lines end where the scans below expect them to
guard_mirror_tree "$src"
src=$mirror

libdir="$src/src/lib"
guard_require_dir "$libdir" "the library source directory"

# The compiled devices that put pixels down, and the rasteriser holding
# the base classes' compiled fills; the Windows driver is checked
# textually alongside. The rules about what a mark paints are these
# files' rules: which pixels a rectangle covers, which a line covers,
# and that a polygon is resolved by the shared scan conversion.
painting="xpost_dev_bgr.c xpost_dev_jpeg.c xpost_dev_png.c xpost_dev_raster.c xpost_dev_xcb.c"

# The compiled device that paints nothing: it writes each marking call
# into a record and plays the record into a device that does paint. It
# is held to everything that is about being a device -- the private
# struct, the method table, the slots a device with state of its own may
# not inherit -- and to a rule of its own below, since the rules about
# which pixels a mark covers are about a device that covers pixels.
recording="xpost_dev_record.c"

fleet="$painting $recording"
marking="$fleet xpost_dev_win32.c xpost_dev_generic.c"
# what the rules about painted pixels are held over
paints="$painting xpost_dev_win32.c xpost_dev_generic.c"

# Every file that defines a device class, read out of the directory
# rather than named. The object files that make a glob unsafe over
# src/lib are not left in data/, and a class file is recognisable by
# what it writes:
#
#   a class dictionary spelled out, /.xpost_NAME <<
#   a class made by copying another, /.xpost_NAME .xpost_OTHER dup
#     length ... dict copy
#   the shared body the generated classes are built over, which defines
#     no class name of its own and is recognisable instead as a
#     dictionary storing the class-to-instance copy
#
# The rules below all take the same shape: they walk this list and hold
# what they find. A list typed here is one a class written tomorrow is
# not on, and every rule then passes over that class saying nothing --
# which is the defect this file exists to catch, in this file.
written=$(cd "$src/data" && grep -lE '^/\.xpost_[A-Za-z0-9_]+[[:blank:]]+<<' *.ps || true)
copied=$(cd "$src/data" && grep -lE '^/\.xpost_[A-Za-z0-9_]+[[:blank:]]+\.xpost_[A-Za-z0-9_]+[[:blank:]]+dup length[[:blank:]].*dict copy' *.ps || true)
shared=$(cd "$src/data" && grep -lE '^[[:blank:]]*/\.copydict[[:blank:]]' *.ps || true)
classes=$(printf '%s\n' $written $copied $shared | grep . | sort -u)

# All three shapes are in the tree today, so a pattern matching nothing
# is a derivation that has come adrift from how the classes are spelled
# -- a guard reading a shorter tree than the one it reports on. That is
# the failure this whole scheme is meant to remove, so it is refused
# outright rather than counted.
if [ -z "$written" ]; then
    echo "check-device-skeleton: no file in data/ spells a class dictionary out;" >&2
    echo "the roster is derived from that spelling. Fix the derivation." >&2
    exit 1
fi
if [ -z "$copied" ]; then
    echo "check-device-skeleton: no file in data/ makes a class by copying" >&2
    echo "another; the roster is derived from that spelling. Fix the derivation." >&2
    exit 1
fi
if [ -z "$shared" ]; then
    echo "check-device-skeleton: no file in data/ stores the class-to-instance" >&2
    echo "copy; the roster is derived from that spelling. Fix the derivation." >&2
    exit 1
fi

fail=0

# 1. Private-struct access goes through xpost_dev_private_get/put: no raw
#    memory accessor in any device source (the helpers in the driver
#    header hold the only calls).
for f in $marking; do
    hits=$(grep -nE '\bxpost_memory_(get|put)\(' "$libdir/$f" || true)
    if [ -n "$hits" ]; then
        echo "check-device-skeleton: raw memory accessor in $f:" >&2
        printf '%s\n' "$hits" >&2
        echo "Reach the private struct through xpost_dev_private_get/put." >&2
        fail=1
    fi
done

# 2. Operand folding goes through xpost_dev_num_to_*: hand-folding is
#    recognisable by its realtype dispatch, which a migrated device no
#    longer needs. The generic rasteriser is exempt from this one rule
#    alone: it inspects operand types for reasons that are not folding.
for f in $fleet xpost_dev_win32.c; do
    hits=$(grep -nE '\brealtype\b' "$libdir/$f" || true)
    if [ -n "$hits" ]; then
        echo "check-device-skeleton: hand-folded numeric operand in $f:" >&2
        printf '%s\n' "$hits" >&2
        echo "Fold operands with the xpost_dev_num_to_* helpers." >&2
        fail=1
    fi
done

# 3a. A device that reaches its own buffer for a pixel reaches it for a
#    rectangle too. The base class fills a rectangle by walking it a pixel
#    at a time and calling PutPix for each, which is the right answer for
#    a device whose page is the base class's own row array and the wrong
#    one for a device that has put its buffer somewhere else: every call
#    goes out through the operator dispatch and comes back in. Every page
#    begins with an erasepage over the whole of it, so a device without
#    its own FillRect spends the page's area in dispatches before a
#    program has drawn anything -- twenty seconds, on a page two thousand
#    square, to reach the state the page starts in.
#
#    A line is not held to this. Its cost is its length, where a
#    rectangle's is the area of the page.
#
#    This is a cost, not a wrong answer, so no rendering shows it and no
#    assertion about what a device paints will catch it. Timing it in the
#    suite would answer differently on a busy machine. What can be said
#    for certain is which methods a device offers, so that is what is
#    asked.
for f in $marking; do
    if grep -q '"PutPix"' "$libdir/$f" && ! grep -q '"FillRect"' "$libdir/$f"; then
        echo "check-device-skeleton: $f reaches its own buffer for a pixel but not" >&2
        echo "      for a rectangle, so it fills one through the base class, a" >&2
        echo "      dispatch per pixel. Give it a FillRect beside its PutPix." >&2
        fail=1
    fi
done

# 3. A file that fills a rectangle paints the contract rectangle: its
#    extent arithmetic must be xpost_dev_rect_normalize, not a private
#    restatement.
#
#    Held over the files that paint a rectangle. A device that writes the
#    call down instead keeps the operands as they arrived and takes no
#    view of which pixels they cover -- that is the whole point of a
#    record, since the device the marks are played into settles it, and
#    it may not be this one.
for f in $paints; do
    if grep -qE '"FillRect"|_fillrect' "$libdir/$f" &&
       ! grep -q 'xpost_dev_rect_normalize' "$libdir/$f"; then
        echo "check-device-skeleton: $f fills a rectangle without xpost_dev_rect_normalize()." >&2
        echo "The painted rectangle is defined once, in xpost_dev_driver.h." >&2
        fail=1
    fi
done

#    And nothing in any of them may restate the two steps that arithmetic
#    is made of -- reflecting a negative extent through its origin, and
#    clamping a coordinate to the device -- since a restatement is how the
#    four behaviours came about. The page's own extent is not this
#    arithmetic and is excepted by name below.
for f in $marking; do
    awk '/^[A-Za-z_].*[^A-Za-z0-9_]xpost_device_raster_bytes[ \t]*\(/ { skip = 1 }
         skip && /^}/                                                  { skip = 0; next }
         { if (!skip) print FNR ": " $0 }' "$libdir/$f" > "$work/rectscan"
    hits=$(grep -E '(w|h|width|height)[ \t]*(\.int_\.val)?[ \t]*<[ \t]*0|\bfloor[ \t]*\((dx|dy|x|y)\b' \
           "$work/rectscan" || true)
    if [ -n "$hits" ]; then
        echo "check-device-skeleton: $f restates the rectangle arithmetic:" >&2
        printf '%s\n' "$hits" >&2
        echo "Reflecting a negative extent and flooring a coordinate belong to" >&2
        echo "xpost_dev_rect_normalize(); call it instead." >&2
        fail=1
    fi
done

# 4. A file that draws a line walks the contract's line. The window
#    devices each had a walk of their own -- one including both
#    endpoints, one excluding the last -- so a wire drawn on one landed
#    on different pixels than the same wire on the other, and neither
#    matched the base class.
for f in $paints; do
    if grep -q '_drawline' "$libdir/$f" &&
       ! grep -q 'xpost_dev_line_init' "$libdir/$f"; then
        echo "check-device-skeleton: $f draws a line without xpost_dev_line_init()." >&2
        echo "The painted line is defined once, in xpost_dev_driver.h." >&2
        fail=1
    fi
done

# 5. Every marking source includes the contract header it is held to.
for f in $marking; do
    if ! grep -q 'xpost_dev_driver\.h' "$libdir/$f"; then
        echo "check-device-skeleton: $f does not include xpost_dev_driver.h." >&2
        fail=1
    fi
done

# 6. A class dictionary that would not take a method leaves the device
#    incomplete, so the refusal reaches the caller: the value of every
#    xpost_dict_put is either returned or tested, and a test never
#    answers success. Textual, so it holds for the sources this platform
#    cannot compile as well as the ones it can.
for f in $marking; do
    hits=$(awk '
        /xpost_dict_put[ \t]*\(/ {
            if ($0 !~ /=/ && $0 !~ /return/)
                printf "%s:%d: the refusal is discarded\n", FILENAME, FNR
            win = 8; sawif = 0; next
        }
        win > 0 {
            win--
            if ($0 ~ /^[ \t]*if \(ret\)/) { sawif = 1; next }
            if (sawif && $0 ~ /^[ \t]*return 0;/)
                printf "%s:%d: the refusal is answered with success\n", FILENAME, FNR
            sawif = 0
        }
    ' "$libdir/$f")
    if [ -n "$hits" ]; then
        echo "check-device-skeleton: registration refusal ignored in $f:" >&2
        printf '%s\n' "$hits" >&2
        echo "A device that could not register a method must not load." >&2
        fail=1
    fi
done

# 7. The class-to-instance copy is one procedure. A class dictionary
#    stores /.copydict, and Create (and every C driver, which fetches it
#    from the class before specialising the copy) calls it. Each class
#    used to carry its own body, and two of them carried a shorter one
#    that left the output file name off the instance, so a device made
#    from those classes wrote wherever its Emit defaulted to. A class
#    may name .classcopydict; it may not restate it.
copies=0
for f in $classes; do
    p="$src/data/$f"
    [ -f "$p" ] || continue
    if grep -qE '^[ \t]*/\.copydict[ \t]*\{' "$p"; then
        echo "check-device-skeleton: $f writes a class copy of its own:" >&2
        grep -nE '^[ \t]*/\.copydict[ \t]*\{' "$p" >&2
        echo "The copy is .xpostsys /.classcopydict; store that, do not restate it." >&2
        fail=1
    fi
    grep -qE '/\.copydict[ \t]+(//\.classcopydict|//\.xpostsys[ \t]+/\.classcopydict[ \t]+get)' "$p" &&
        copies=$((copies + 1))
done
if [ "$copies" -lt 5 ]; then
    echo "check-device-skeleton: only $copies classes store the shared class copy;" >&2
    echo "expected every class that defines /.copydict to name .classcopydict." >&2
    fail=1
fi
if [ "$(grep -c '\.xpostsys /\.classcopydict {' "$src/data/device.ps")" != 1 ]; then
    echo "check-device-skeleton: the shared class copy is not defined once in device.ps." >&2
    fail=1
fi

# 8. A device is completed once, identically, on every path that
#    creates one. The finishing a fresh device takes -- the page's
#    default matrix, the compiled rasterisers its raster shape can
#    take, the process colour model it was asked for -- is
#    .completedevice, and only it installs them. It was written twice,
#    for the device the interpreter starts with and the device
#    setpagedevice makes, and the two were not the same: one adopted the
#    process colour model and the other did not, so the same device
#    behaved differently according to how it had been selected.
#
#    One site installs a compiled rasteriser on a device that is not a
#    page device and never becomes one: the glyph cache in font.ps, a
#    scratch raster the machinery paints into and reads back. It is
#    named here rather than left to slip through a looser pattern. A
#    form is not one of them -- what a form is painted into is a
#    recorder, which brings its own marking methods and is finished by
#    nothing.
scratch=0
for f in device.ps font.ps init.ps image.ps pgmimage.ps pbmimage.ps \
         ppmimage.ps tiffimage.ps nulldev.ps bboxdev.ps pdfwrite.ps \
         svgwrite.ps dscwrite.ps paint.ps callout.ps gstate.ps; do
    p="$src/data/$f"
    [ -f "$p" ] || continue
    while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        case "$f:$hit" in
            device.ps:*"dev /Fill"*)   continue ;;   # .completedevice itself
            font.ps:*"mdev /Fill"*)    scratch=$((scratch + 1)); continue ;;
        esac
        echo "check-device-skeleton: a device is completed outside .completedevice:" >&2
        echo "  $f:$hit" >&2
        echo "A page device is finished by .completedevice (data/device.ps); the only" >&2
        echo "site that may install a rasteriser directly is the scratch raster," >&2
        echo "font.ps's glyph cache, named mdev." >&2
        fail=1
    done <<EOF
$(grep -nE '/(FillPoly|FillRect)([ \t]+//\.internaldict|$)' "$p" || true)
EOF
done
if [ "$scratch" -ne 2 ]; then
    echo "check-device-skeleton: $scratch scratch-raster completions, expected 2." >&2
    echo "A new one is another place a device gets finished; give it" >&2
    echo ".completedevice or add it here with its reason." >&2
    fail=1
fi
if [ "$(grep -c '\.privatedict /\.completedevice {' "$src/data/device.ps")" != 1 ]; then
    echo "check-device-skeleton: the device completion is not defined once in device.ps." >&2
    fail=1
fi
# summed with awk rather than bc: bc is not present in every environment
# this runs in, and a guard that cannot run is a guard that is not checking
# counted over the concatenation rather than per file: grep -c prefixes
# each count with the path, and a windows path carries a drive-letter
# colon, so splitting on the colon takes the path for the count and the
# check reads zero on the platform it most needs to run on
callers=$(cat "$src/data/device.ps" "$src/data/init.ps" \
          | grep -c '/\.completedevice get exec')
if [ "$callers" -lt 2 ]; then
    echo "check-device-skeleton: only $callers path completes a device;" >&2
    echo "both the startup device and setpagedevice's must call .completedevice." >&2
    fail=1
fi

# 9. A device's methods are registered from its method table, not one
#    at a time. Written out by hand, each registration carried its own
#    arity, its own operand types and its own put, and five of six
#    devices answered success from a failed PutPix registration -- the
#    device loaded with no PutPix and failed at its first paint. The
#    table states the slot and the kind; xpost_dev_class_install derives
#    the arity from the declared colour space, stops at the first
#    refusal, and checks what it produced.
for f in $fleet xpost_dev_win32.c; do
    if ! grep -q 'Xpost_Dev_Method methods\[\]' "$libdir/$f"; then
        echo "check-device-skeleton: $f has no method table." >&2
        echo "Register a device's suite through xpost_dev_class_install()." >&2
        fail=1
    fi
    if ! grep -q 'xpost_dev_class_install' "$libdir/$f"; then
        echo "check-device-skeleton: $f does not install its class through the contract." >&2
        fail=1
    fi
    # a method slot put into the class dictionary outside the table is a
    # registration the completeness check never sees
    hits=$(grep -nE 'xpost_dict_put\(ctx, classdic, xpost_name_cons\(ctx, "(Create|PutPix|GetPix|DrawLine|DrawRect|FillRect|FillPoly|BlendPix|Emit|Flush|Destroy|Erase)"\)' \
           "$libdir/$f" || true)
    if [ -n "$hits" ]; then
        echo "check-device-skeleton: $f installs a method slot outside its table:" >&2
        printf '%s\n' "$hits" >&2
        fail=1
    fi
done

# 10. The contract's list of slots that read the base class's raster is
#     the classes' list. The completeness check refuses a device that
#     keeps its own buffer and leaves one of them inherited; if a class
#     grows another such method and the header does not hear about it,
#     the check goes on passing while the hole reopens.
#
#     Only the names the pipeline looks up count. A dot-prefixed name is
#     a parameter of the generated raster suite -- .writepage reads the
#     row array too, but nothing reaches it except Emit, which is on the
#     list, so a device that overrides that never runs it.
#
#     A slot is read whether the class states it as a body of its own or
#     fills it with a compiled operator, since such an operator reads the
#     row array out of the instance dictionary it is handed. Both are
#     collected, or a slot filled from C reads as no slot at all: that is
#     what BlendPix was, and a device that keeps its own raster inherited
#     a blend with nothing to blend into.
sed -n 's/^#define XPOST_DEV_RASTER_SLOTS { \(.*\) }$/\1/p' \
    "$libdir/xpost_dev_driver.h" | tr -d '" ' | tr ',' '\n' \
    | grep -v '^$' | sort > "$work/hdr"
classfiles="$src/data/image.ps $src/data/pgmimage.ps $src/data/ppmimage.ps
            $src/data/pbmimage.ps $src/data/tiffimage.ps"
# the methods in the classes whose body reads ImgData
awk '
    /^[ \t]*\/[A-Za-z.][A-Za-z0-9._]*[ \t]*\{/ { m = $1; sub(/^\//, "", m) }
    m != "" && /ImgData/ { print m; m = "" }
' $classfiles | grep -v '^\.' | sort -u > "$work/cls"

# the compiled operators that read the row array, under the names
# PostScript reaches them by: which C function each registered name
# names, and whether that function's body reads ImgData. The rasteriser
# is read twice, once for each question.
awk '
    NR == FNR {
        if ($0 ~ /^}/) { if (fn != "" && saw) reads[fn] = 1; fn = ""; saw = 0 }
        else if ($0 ~ /^[A-Za-z_]/ && $0 ~ /\(/ && $0 !~ /;[ \t]*$/ &&
                 match($0, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
            fn = substr($0, RSTART, RLENGTH)
            sub(/[ \t]*\(.*$/, "", fn)
            saw = 0
        }
        if (fn != "" && $0 ~ /nameImgData/) saw = 1
        next
    }
    match($0, /xpost_operator_cons\(ctx, "\.[A-Za-z0-9_]+",[ \t]*\(Xpost_Op_Func\)[A-Za-z0-9_]+/) {
        s = substr($0, RSTART, RLENGTH)
        ps = s; sub(/^[^"]*"/, "", ps); sub(/".*$/, "", ps)
        f = s; sub(/^.*\(Xpost_Op_Func\)/, "", f)
        if (reads[f]) print ps
    }
' "$libdir/xpost_dev_generic.c" "$libdir/xpost_dev_generic.c" \
    | sort -u > "$work/rowops"
if [ ! -s "$work/rowops" ]; then
    echo "FAILURES: no compiled operator reads the row array; fix the guard" >&2
    exit 1
fi
# the class slots those operators are stored in
sed -n 's|^[[:blank:]]*/\([A-Za-z][A-Za-z0-9_]*\)[[:blank:]]*//\.internaldict[[:blank:]]*/\(\.[A-Za-z0-9_]*\)[[:blank:]]*get.*|\2 \1|p' \
    $classfiles | sort -u > "$work/clsops"
if [ ! -s "$work/clsops" ]; then
    echo "FAILURES: no class slot is filled from the rasteriser; fix the guard" >&2
    exit 1
fi
while read -r op slot; do
    grep -qx "$op" "$work/rowops" && echo "$slot"
done < "$work/clsops" >> "$work/cls"
sort -u -o "$work/cls" "$work/cls"

if [ ! -s "$work/hdr" ] || [ ! -s "$work/cls" ]; then
    echo "FAILURES: the raster-slot lists could not be read; fix the guard" >&2
    exit 1
fi
guard_held=0
guard_hold "$work/hdr" "$work/cls" \
    "named by XPOST_DEV_RASTER_SLOTS as reading the row array, and read
      by no class method:" \
    "read from the row array by a class method and not named by
      XPOST_DEV_RASTER_SLOTS. A device with its own buffer would inherit
      the method and answer undefined:"
[ "$guard_held" -eq 0 ] || fail=1

# 11. A device that keeps its raster in a buffer of its own names every
#     one of those slots in its own method table.
#
#     xpost_dev_class_install says the same thing and refuses a device
#     that does not, but only where the device can be built and loaded.
#     A driver this platform cannot compile is never held to it at all,
#     and that is where the hole lasted: both window devices inherited
#     the blend, and each declared one bit of text alpha, so the text
#     path never reached the slot and nothing else asked.
#
#     Which devices are held is read from the install call rather than
#     listed: the argument after the component count says whether the
#     raster is the device's own. A file whose call this cannot read
#     fails here rather than being passed over.
compiled=0
for f in $fleet xpost_dev_win32.c; do
    # the component count ahead of it may be written as a number or as
    # the name the device gives it; what is read here is the flag
    own=$(sed -n 's/.*xpost_dev_class_install(ctx, classdic, [A-Za-z0-9_][A-Za-z0-9_]*, \([01]\),.*/\1/p' \
          "$libdir/$f" | sort -u | tr -d '\n')
    case "$own" in
        0) continue ;;
        1) ;;
        *) echo "check-device-skeleton: cannot read from $f whether its raster is" >&2
           echo "its own; xpost_dev_class_install is the one call that says so." >&2
           fail=1
           continue ;;
    esac
    compiled=$((compiled + 1))
    while read -r slot; do
        if ! grep -qE "\{[ \t]*\"$slot\"[ \t]*," "$libdir/$f"; then
            echo "check-device-skeleton: $f keeps its own raster and its method" >&2
            echo "table has no $slot, so it inherits one that reads the base" >&2
            echo "class's row array and answers undefined when it is reached." >&2
            fail=1
        fi
    done < "$work/hdr"
done
if [ "$compiled" -eq 0 ]; then
    echo "FAILURES: no device was held to the raster slots; fix the guard" >&2
    exit 1
fi

# 12. What the completion installs a compiled rasteriser under is asked
#     once, and asked before the install.
#
#     Neither compiled fill serves every device. The rectangle fills
#     write the raster as the array of row strings the base classes keep
#     under /ImgData, so a device with a buffer of its own has nothing
#     for them to write; the polygon fill paints in the colour spaces it
#     knows a component count for. A device that does not match keeps
#     what it brought, and a device given the compiled one anyway
#     answers at its first mark rather than at load.
#
#     The condition belongs to the fill, not to a colour space, and
#     writing it out beside each fill is how the two came to disagree:
#     the colour path stated it and the grey path did not, so a device
#     declaring DeviceGray with a buffer of its own had a working
#     rectangle fill replaced by one that answered undefined. So the
#     count is the rule -- one statement of each condition, ahead of
#     every install -- rather than a check that each install has its
#     own, which is what the two-statement form would pass.
#
#     This is a shape a rendering cannot show: every device here that
#     keeps its own buffer declares DeviceRGB, so the grey path is
#     unreached by the fleet and no page comes out different.
#     tests/device_completion_test.ps asks what the completion does; this
#     asks that there is one place where it is decided.
awk '
    /\.privatedict \/\.completedevice \{/          { on = 1 }
    !on                                            { next }
    /\/ImgData known/               { img++;  imgl = FNR }
    /\/operatortype ne/             { own++;  ownl = FNR }
    /\/\.fillrect[a-z]* get put/    { rect++; if (!rectl) rectl = FNR }
    /\/Device[A-Za-z]* eq/          { if (!csl) csl = FNR }
    /\/\.fillpoly get put/          { poly++; polyl = FNR }
    /^\} bind put/                  { on = 0 }
    END { print img+0, imgl+0, own+0, ownl+0, rect+0, rectl+0, \
                csl+0, poly+0, polyl+0 }
' "$src/data/device.ps" > "$work/completion"
read -r c_img c_imgl c_own c_ownl c_rect c_rectl c_csl c_poly c_polyl \
     < "$work/completion"
if [ "$c_rect" -lt 2 ] || [ "$c_poly" -ne 1 ]; then
    echo "check-device-skeleton: .completedevice installs $c_rect compiled" >&2
    echo "rectangle fills and $c_poly polygon fill; expected both rectangle" >&2
    echo "fills and the one polygon fill. A fill that stopped being installed" >&2
    echo "is a device rasterising a page through the interpreter." >&2
    fail=1
elif [ "$c_img" -ne 1 ] || [ "$c_own" -ne 1 ]; then
    echo "check-device-skeleton: the compiled rectangle fills are installed" >&2
    echo "under the raster condition stated $c_img times and the condition on" >&2
    echo "the fill the device brought stated $c_own times; expected one of" >&2
    echo "each. Both belong to the fill rather than to a colour space, so" >&2
    echo "both are asked once and the colour space chooses only which fill" >&2
    echo "is installed." >&2
    fail=1
elif [ "$c_rectl" -lt "$c_imgl" ] || [ "$c_rectl" -lt "$c_ownl" ]; then
    echo "check-device-skeleton: a compiled rectangle fill is installed at" >&2
    echo "data/device.ps:$c_rectl, ahead of the conditions at line $c_imgl and" >&2
    echo "line $c_ownl, so it lands on a device those conditions would have" >&2
    echo "spared: one holding its raster in a buffer of its own, or one that" >&2
    echo "brought a compiled rectangle fill already." >&2
    fail=1
elif [ "$c_csl" -eq 0 ] || [ "$c_csl" -gt "$c_polyl" ]; then
    echo "check-device-skeleton: the compiled polygon fill is installed at" >&2
    echo "data/device.ps:$c_polyl with no colour-space test ahead of it, so a" >&2
    echo "device declaring a space that fill paints no colour in loses the" >&2
    echo "polygon fill it brought." >&2
    fail=1
fi

# 13. A device fills a polygon through the shared scan conversion, never
#     through a polygon primitive of its window system.
#
#     The completion above installs the compiled polygon fill on every
#     device it finishes, so a FillPoly in a driver's own table is a
#     method nothing calls. That is the small half of it. The large half
#     is that the primitive could not stand in for the fill if it were
#     called: the polygon arrives as one point list with a null between
#     subpaths, which such a primitive has no form for; the rule
#     PostScript fills a path under is the nonzero winding number (PLRM
#     8.2), which is not the rule a server-side polygon is drawn with; and
#     which pixels a boundary covers is the driver contract's answer,
#     stated once, not the window system's. The rectangle fills and the
#     line walk are held to that last point by the rules above; this is
#     the same rule for the third shape, and it is stated separately
#     because the way to break it is to bring a method rather than to
#     restate a formula.
for f in $paints; do
    hits=$(grep -nE '\{[ \t]*"FillPoly"[ \t]*,' "$libdir/$f" || true)
    if [ -n "$hits" ]; then
        echo "check-device-skeleton: $f brings a polygon fill of its own:" >&2
        printf '%s\n' "$hits" >&2
        echo "The completion installs the compiled polygon fill over it, so it is" >&2
        echo "never called; and a window system's polygon primitive fills under" >&2
        echo "its own rule, over its own pixels, with no form for the subpath" >&2
        echo "separators the polygon carries. Let the shared scan conversion" >&2
        echo "resolve the polygon and paint the spans through this device's" >&2
        echo "FillRect." >&2
        fail=1
    fi
done

# 14. A device that records its marks declares exactly the five marking
#     methods a record holds, and no other optional method.
#
#     A record holds five kinds of mark and the machinery can ask for
#     more kinds of call than that. What makes up the difference is what
#     the device declines to declare: FillPath would bring it a whole
#     path and the clip shape beside it, ClipPath the clip alone,
#     DrawRect an outlined rectangle, Erase an instruction to reset its
#     page. Declining all of them has each resolved, above the device,
#     into the five the record does hold (doc/xpost_design.dox).
#
#     So this is the rule the whole design rests on, and the way it
#     breaks is by someone adding a method for a good local reason: the
#     marks that method would have taken then go unrecorded, and every
#     page using it replays short of them -- silently, the replay being
#     missing a mark rather than failing. The other direction, that a
#     device brings every method it must, is xpost_dev_class_install's
#     and rule 11's; this is the same shape of rule pointing the other
#     way.
#
#     Both halves of the device are read: the method table it installs
#     from C, and the class dictionary it specialises. A slot arriving by
#     either route is a slot the pipeline finds.
recslots="Create PutPix GetPix BlendPix DrawLine FillRect FillPoly Emit Destroy"
recforbidden="FillPath ClipPath DrawRect Erase StrokePath"
for f in $recording; do
    sed -n 's/.*{[[:blank:]]*"\([A-Za-z]*\)"[[:blank:]]*,[[:blank:]]*"[A-Za-z]*"[[:blank:]]*,.*/\1/p' \
        "$libdir/$f" | sort -u > "$work/rectable"
    if [ ! -s "$work/rectable" ]; then
        echo "FAILURES: no method table could be read from $f; fix the guard" >&2
        exit 1
    fi
    printf '%s\n' $recslots | sort -u > "$work/recwant"
    guard_held=0
    guard_hold "$work/recwant" "$work/rectable" \
        "held by a record and not declared by $f. A method it declines
      is resolved above it into the five the record holds:" \
        "declared by $f and not held by a record. A method it brings is
      a call the record has no entry for:"
    [ "$guard_held" -eq 0 ] || fail=1
done
#     The class dictionary the table specialises is read the same way,
#     as code: a slot it declares is a slot the instance carries whether
#     or not the driver knows about it. Comments are dropped before the
#     scan, since the methods being declined are named in the prose that
#     says why -- a scan that read the prose would fire on the file that
#     is right and go quiet on nothing.
recclass="$src/data/recorddev.ps"
guard_require_file "$recclass" "the recording device's class"
awk '{ sub(/\r$/, ""); sub(/%.*$/, ""); print FNR ": " $0 }' "$recclass" \
    > "$work/recclass"
for slot in $recforbidden; do
    hits=$(grep -nE "/$slot([^A-Za-z0-9_]|\$)" "$work/recclass" || true)
    if [ -n "$hits" ]; then
        echo "check-device-skeleton: the recording device's class declares $slot:" >&2
        printf '%s\n' "$hits" | sed 's|^[0-9]*:|      recorddev.ps:|' >&2
        echo "Declaring it puts the calls it would take beyond the record's" >&2
        echo "reach, and every page using them replays short of a mark." >&2
        fail=1
    fi
done

# 15. A device that specialises a class saying its page may arrive in
#     bands says for itself whether its own may, and one that says yes
#     brings the method that moves its raster from band to band.
#
#     Whether a page may arrive a band at a time is /BandedPage, and the
#     safe answer is silence -- a device that has not thought about it
#     gets the whole page it expects (doc/xpost_design.dox). The compiled
#     devices here are all dict copies of a class that says yes, and a
#     copy carries what it was copied from, so silence is exactly what
#     they cannot have: an untouched copy says yes on behalf of a driver
#     that never considered the question. The PostScript classes have
#     the same problem one layer up and answer it the same way, by
#     taking the declaration back out (pbmimage.ps, tiffimage.ps).
#
#     A file may install more than one class -- the png driver makes the
#     plain device and the alpha one out of one body -- and then it says
#     one thing per class rather than one per file, and the two may
#     differ. How many classes it installs is how many class names of its
#     own it defines in the private dictionary, and a file defining none
#     installs one, reached through the maker it registers instead. So
#     what is counted is statements against classes: a file with one
#     class saying both things, or with two saying one, has left a class
#     answering for a decision nobody made about it.
#
#     And a device that says yes is one a band loop will call .moveband
#     on. That method moves the run of rows a raster stands for, which
#     for these devices is a run within a buffer of their own and not
#     within the base class's array of rows: the inherited one reaches
#     for a row array the instance does not carry and answers undefined.
#     It is not on the raster-slot list above, which is about the slots
#     the class bodies read that array through, and the completeness
#     check therefore does not ask for it -- so it is asked for here, of
#     the devices whose declaration makes it reachable.
banded=0
for f in $fleet xpost_dev_win32.c; do
    # The classes this file installs in the private dictionary: the
    # names it puts there, read joined because the puts wrap lines, and
    # a ternary carries both names of a body that makes two classes.
    tr '\n' ' ' < "$libdir/$f" \
        | grep -oE 'xpost_dict_put[(]ctx, ctx->privatedict,[^;]*' \
        | grep -oE '"\.xpost_[A-Za-z0-9_]*"' | tr -d '"' \
        | LC_ALL=C sort -u > "$work/skel.installs"
    # The class this device specialises, and whether that class says its
    # page may arrive in bands. What the file loads less what it
    # installs: a maker loads the finished class its own loader put
    # there, and a file cannot specialise from a class it installs
    # itself.
    sed -n 's/.*xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, "\(\.xpost_[A-Za-z0-9_]*\)")).*/\1/p' \
           "$libdir/$f" | LC_ALL=C sort -u > "$work/skel.loads"
    base=$(LC_ALL=C comm -23 "$work/skel.loads" "$work/skel.installs")
    inherits=0
    for b in $base; do
        for c in $classes; do
            p="$src/data/$c"
            [ -f "$p" ] || continue
            grep -qE "^/$b[ \t]+<<" "$p" || continue
            grep -qE '^[ \t]*/BandedPage[ \t]+true' "$p" && inherits=1
        done
    done
    [ "$inherits" -eq 1 ] || continue

    # how many classes this file installs, and one where it installs
    # none, its single class reached through the maker it registers
    nclass=$(grep -c . "$work/skel.installs" || true)
    [ "$nclass" -gt 0 ] || nclass=1

    says=$(grep -cE 'xpost_dict_put\(ctx, classdic, xpost_name_cons\(ctx, "BandedPage"\)' \
           "$libdir/$f" || true)
    takes=$(grep -cE 'xpost_dict_undef\(ctx, classdic, xpost_name_cons\(ctx, "BandedPage"\)\)' \
            "$libdir/$f" || true)
    if [ $((says + takes)) -ne "$nclass" ]; then
        echo "check-device-skeleton: $f installs $nclass class(es) copied from one" >&2
        echo "that says its page may arrive a band at a time, and says whether" >&2
        echo "its own may $((says + takes)) time(s) -- $says declaring it and $takes taking" >&2
        echo "it back out. A class left unspoken for inherits the yes. Say it" >&2
        echo "again, or take it back out with the reason, once for each." >&2
        fail=1
        continue
    fi
    banded=$((banded + 1))
    if [ "$says" -gt 0 ] &&
       ! grep -qE '\{[ \t]*"\.moveband"[ \t]*,' "$libdir/$f"; then
        echo "check-device-skeleton: $f says its page may arrive a band at a" >&2
        echo "time and its method table has no .moveband, so a band loop moves" >&2
        echo "its raster through the base class's, which reaches for a row" >&2
        echo "array this device does not carry and answers undefined." >&2
        fail=1
    fi
done
if [ "$banded" -eq 0 ]; then
    echo "FAILURES: no compiled device was held to what it says about taking" >&2
    echo "      its page in bands; fix the guard" >&2
    exit 1
fi

# 16. A device that specialises a class stating what a row of its raster
#     costs states that for itself.
#
#     /.rowcost is the pair -- the elements and the bytes one row takes
#     -- that a class answers about its own raster. The band loop divides
#     a byte budget by the bytes of it to reach a band height
#     (data/recorddev.ps), and a raster class prices the whole raster
#     from it before building any of it (data/image.ps). So a wrong
#     answer is not a wrong page: the bands come out byte for byte the
#     page they always were, and what moves is how much is held at once,
#     which is the whole of what the budget states.
#
#     It is the same shape as /BandedPage above and breaks the same way.
#     The devices held here are copies of a class that prices a row of
#     its own raster, and a copy carries what it was copied from, so a
#     device whose raster is a different shape answers for the shape it
#     was copied from unless it says otherwise: a four-byte pixel
#     answering for a three-byte planar row holds a third more than the
#     budget bought, over pages that come out byte for byte the same.
#
#     Either answer counts, since a device may have no row of its own to
#     price: one whose pixel is settled per instance rather than by the
#     class, and one whose pixels are the window system's. What may not
#     count is silence.
#
#     The two PostScript classes made by copying another are held to it
#     too. Nothing about the defect is particular to a driver written in
#     C -- a dict copy carries the same statement the same way -- and the
#     rule is asked of a file that derives one class from another
#     wherever that is written.
priced=0
for f in $fleet xpost_dev_win32.c; do
    base=$(sed -n 's/.*xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, "\(\.xpost_[A-Za-z0-9_]*\)")).*/\1/p' \
           "$libdir/$f" | sort -u)
    inherits=0
    for b in $base; do
        for c in $classes; do
            p="$src/data/$c"
            [ -f "$p" ] || continue
            grep -qE "^/$b[ \t]+<<" "$p" || continue
            grep -qE '^[ \t]*/\.rowcost[ \t]*\{' "$p" && inherits=1
        done
    done
    [ "$inherits" -eq 1 ] || continue

    says=$(grep -c 'xpost_dev_class_rowcost(' "$libdir/$f" || true)
    takes=$(grep -c 'xpost_dev_class_no_rowcost(' "$libdir/$f" || true)
    if [ "$says" -eq 0 ] && [ "$takes" -eq 0 ]; then
        echo "check-device-skeleton: $f copies a class that prices a row of" >&2
        echo "its raster and says nothing itself, so it answers for a raster" >&2
        echo "that is not the one it holds. Price its own row with" >&2
        echo "xpost_dev_class_rowcost(), or take the statement back out with" >&2
        echo "xpost_dev_class_no_rowcost() and the reason." >&2
        fail=1
        continue
    fi
    if [ "$says" -gt 0 ] && [ "$takes" -gt 0 ]; then
        echo "check-device-skeleton: $f both prices a row and takes the" >&2
        echo "price back out." >&2
        fail=1
        continue
    fi
    priced=$((priced + 1))
done
if [ "$priced" -eq 0 ]; then
    echo "FAILURES: no compiled device was held to what a row of its raster" >&2
    echo "      costs; fix the guard" >&2
    exit 1
fi

#     And the classes one PostScript file derives from another. The
#     derivation is read out of the file rather than listed, so a class
#     added by copying joins this the day it is written.
psderived=0
for c in $classes; do
    p="$src/data/$c"
    [ -f "$p" ] || continue
    while read -r derived b; do
        [ -n "${b:-}" ] || continue
        prices=0
        for o in $classes; do
            q="$src/data/$o"
            [ -f "$q" ] || continue
            grep -qE "^/$b[ \t]+<<" "$q" || continue
            grep -qE '^[ \t]*/\.rowcost[ \t]*\{' "$q" && prices=1
        done
        [ "$prices" -eq 1 ] || continue
        if ! grep -qE "^$derived[ \t]+/\.rowcost[ \t]*(\{|undef)" "$p"; then
            echo "check-device-skeleton: $c derives $derived from $b, which" >&2
            echo "prices a row of its raster, and states no price of its own." >&2
            echo "A dict copy carries what it was copied from, so the derived" >&2
            echo "class answers on behalf of the class it came from; state the" >&2
            echo "price again, or undef it with the reason." >&2
            fail=1
            continue
        fi
        psderived=$((psderived + 1))
    done <<EOF
$(sed -n 's|^/\(\.xpost_[A-Za-z0-9_]*\)[[:blank:]]\{1,\}\(\.xpost_[A-Za-z0-9_]*\)[[:blank:]]\{1,\}dup length[[:blank:]].*dict copy.*|\1 \2|p' "$p")
EOF
done
if [ "$psderived" -eq 0 ]; then
    echo "FAILURES: no PostScript class derived from another was held to what" >&2
    echo "      a row of its raster costs; fix the guard" >&2
    exit 1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "check-device-skeleton: ok (fleet behind the driver contract, $copies classes behind one copy, $callers paths behind one completion, $compiled devices behind the raster slots, $banded saying for themselves whether their page may arrive in bands, $priced devices and $psderived derived classes pricing a row of their own raster)"
