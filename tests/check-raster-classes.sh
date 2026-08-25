#!/bin/sh
# Guard the raster-class derivation: a class built by the generator is a
# parameter file over it, a class made by copying one of those overrides
# emission, and the shared method suite is defined exactly once, in the
# prototype. A standalone method definition reappearing in a class file
# means the classes are diverging into twins again.
#
# Which files those are is read out of data/ rather than named here. The
# prototype is the file defining the generator; a generated class is a
# file that calls it; a derived one is a file that copies a class a
# generated file defines; and the family as a whole is every file
# stating one of the parameter slots a raster class answers with. A
# member of the family that is neither generated nor derived is a twin,
# which is the one way back into the state this guard exists to keep the
# tree out of.
#
# A list typed here is one a class written tomorrow is not on, and every
# rule below would then pass over that class in silence.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-raster-classes.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
data=$src/data
fail=0

methods='Create Destroy PutPix GetPix DrawLine DrawRect FillRect FillPoly Emit .copydict .intersect .maxmin'

# The prototype: the file the generator is defined in.
proto=$(cd "$data" && grep -lE '^\.xpostsys[[:blank:]]+/\.makerasterclass[[:blank:]]*\{' *.ps || true)
if [ "$(printf '%s\n' $proto | grep -c .)" != 1 ]; then
    echo "check-raster-classes: the raster class generator is defined in"
    echo "$(printf '%s\n' $proto | grep -c .) files, expected exactly 1."
    exit 1
fi

# The files that call the generator, and the class each one defines.
# Three spellings reach the generator: fetched by name, baked with //, and
# -- at the top level of a file that has opened the namespace -- the bare
# name. The prose names it too, so what is read is the code alone, and the
# file that defines the generator is not one of its callers.
gencall='(/\.makerasterclass[[:blank:]]+get[[:blank:]]+exec|//\.makerasterclass[[:blank:]]+exec|(^|[[:blank:]])\.makerasterclass([[:blank:]]|$))'
gencode() { sed 's/%.*$//' "$1"; }
generated=$(cd "$data" && for f in *.ps; do
    [ "$f" = "$proto" ] && continue
    sed 's/%.*$//' "$f" | grep -qE "$gencall" && echo "$f"
done || true)
if [ -z "$generated" ]; then
    echo "check-raster-classes: no file in data/ calls .makerasterclass;"
    echo "the roster is derived from that call. Fix the derivation."
    exit 1
fi
gennames=$(cd "$data" && sed -n 's|^/\(\.xpost_[A-Za-z0-9_]*\)[[:blank:]]\{1,\}<<.*|\1|p' $generated | sort -u)
if [ -z "$gennames" ]; then
    echo "check-raster-classes: the generated files define no class dictionary;"
    echo "the roster is derived from that spelling. Fix the derivation."
    exit 1
fi

# the generated classes are parameter files: one generator call each,
# and no method definition of their own
for f in $generated; do
    n=$(gencode "$data/$f" | grep -cE "$gencall" || true)
    if [ "$n" != 1 ]; then
        echo "check-raster-classes: $f calls .makerasterclass $n times, expected exactly 1"
        fail=1
    fi
    for m in $methods; do
        if grep -qE "/$m[[:space:]]*\{" "$data/$f"; then
            echo "check-raster-classes: $f defines /$m; it belongs in the prototype ($proto)"
            fail=1
        fi
    done
done

# The files that make a class by copying a generated one. A copy of a
# class the generator did not build belongs to some other family and is
# not read here: what this holds is the raster suite, not every dict
# copy in the tree.
derived=
for f in $(cd "$data" && ls *.ps); do
    while read -r class base; do
        [ -n "${base:-}" ] || continue
        case "
$gennames
" in
            *"
$base
"*) derived="$derived $f" ;;
        esac
    done <<EOF
$(sed -n 's|^/\(\.xpost_[A-Za-z0-9_]*\)[[:blank:]]\{1,\}\(\.xpost_[A-Za-z0-9_]*\)[[:blank:]]\{1,\}dup length[[:blank:]].*dict copy.*|\1 \2|p' "$data/$f")
EOF
done
derived=$(printf '%s\n' $derived | grep . | sort -u)
if [ -z "$derived" ]; then
    echo "check-raster-classes: no file in data/ derives a class from a generated"
    echo "one; the roster is derived from that spelling. Fix the derivation."
    exit 1
fi

# the derived classes override emission (and, for the bilevel class, the
# screening pixel store), never the shared geometry methods or the Emit
# dispatch
for f in $derived; do
    for m in Create Destroy GetPix DrawLine DrawRect FillRect FillPoly Emit .copydict .intersect .maxmin; do
        if grep -qE "/$m[[:space:]]*\{" "$data/$f"; then
            echo "check-raster-classes: $f overrides /$m; derived classes override emission only"
            fail=1
        fi
    done
done

# Every file answering with a raster parameter slot reached the suite by
# one of the two routes. A file stating what a row of its raster costs,
# how a component is stored in one, or what its emitted page is headed
# with, and neither calling the generator nor copying a class it built,
# is a second copy of the suite: it will answer the same questions out
# of its own body, and the two bodies will then drift apart with nothing
# reading both.
family=$(cd "$data" && grep -lE '^[[:blank:]]*(/|\.xpost_[A-Za-z0-9_]+[[:blank:]]+/)(\.rowput|\.rowget|\.rowcost|\.pnmmagic|\.writeband|\.writepage|\.writehead)[[:blank:]]' *.ps || true)
reached=" $(printf '%s ' $generated $derived $proto)"
for f in $family; do
    case "$reached" in
        *" $f "*) continue ;;
    esac
    echo "check-raster-classes: $f answers with a raster parameter slot and"
    echo "reaches the suite neither through .makerasterclass nor by copying a"
    echo "class it built, so it carries a suite of its own."
    fail=1
done

# the suite is defined once, in the prototype. The class copy is not in
# it: every device class shares one copy (.classcopydict, data/device.ps),
# so the prototype names it like the rest of them do and
# check-device-skeleton.sh holds that.
for m in $(echo "$methods" | sed 's/\.copydict//'); do
    n=$(grep -cE "/$m[[:space:]]*\{" "$data/$proto" || true)
    if [ "$n" != 1 ]; then
        echo "check-raster-classes: /$m defined $n times in data/$proto, expected exactly 1"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "check-raster-classes: the raster classes are no longer a single generated suite."
    exit 1
fi
echo "check-raster-classes: ok (one suite in $proto, $(printf '%s\n' $generated | grep -c .) parameter files, $(printf '%s\n' $derived | grep -c .) derived by dict copy)"
exit 0
