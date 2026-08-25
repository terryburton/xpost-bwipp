#!/bin/sh
# Guard the configuration an image of virtual memory carries.
#
# An image is read as the context is created, and it carries the language
# it was written with. An option that changes what the boot files build --
# rather than what a run does with what they built -- must therefore be
# written into the image and compared on the way back, or a run wanting
# one language reads an image of another and is never told.
#
# Two halves, and the second is the one that matters later:
#
#   Every XPOST_VM_IMAGE_CONFIG_ bit the header declares is set by the
#   command line, and every bit the command line sets is declared. A bit
#   nothing sets protects nothing; a bit nothing declares does not compile,
#   but the pair is read together here so neither can drift alone.
#
#   Every command-line option is classified in tests/image-config as
#   changing the language or not, and each one that does names its bit.
#   That is the half a new option trips: an option added without being
#   classified fails here rather than quietly leaving two languages
#   indistinguishable.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-image-config.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
register=$src/tests/image-config
main=$src/src/bin/xpost_main.c
header=$src/src/lib/xpost.h
fail=0

for f in "$register" "$main" "$header"; do
    if [ ! -s "$f" ] || [ ! -r "$f" ]; then
        echo "FAILURES: $f is empty or unreadable; the check would read nothing" >&2
        exit 1
    fi
done

guard_workdir

# ---- the bits the header declares, and the bits the command line sets ----
grep -oE '#define[[:blank:]]+XPOST_VM_IMAGE_CONFIG_[A-Z_]+' "$header" \
    | awk '{ print $2 }' | LC_ALL=C sort -u > "$work/declared"
grep -oE 'XPOST_VM_IMAGE_CONFIG_[A-Z_]+' "$main" \
    | LC_ALL=C sort -u > "$work/set"

ndecl=$(grep -c . "$work/declared" || true)
if [ "${ndecl:-0}" -eq 0 ]; then
    echo "FAILURES: the header declares no XPOST_VM_IMAGE_CONFIG_ bit;" >&2
    echo "the configuration stamp has changed shape and this reads nothing" >&2
    exit 1
fi

if ! diff -u "$work/declared" "$work/set" > "$work/bits" 2>&1; then
    echo "FAILURES: the bits declared and the bits the command line sets" >&2
    echo "disagree. A bit nothing sets protects nothing. - declared and not" >&2
    echo "set, + set and not declared:" >&2
    sed -n '3,$p' "$work/bits" | sed 's/^/      /' >&2
    fail=1
fi

# ---- every option classified, and every language option carrying a bit ----
grep -oE '"--[a-z-]+"' "$main" | tr -d '"' | LC_ALL=C sort -u > "$work/options"
sed 's/#.*$//' "$register" | awk 'NF == 3 && $1 ~ /^--/ { print $1 }' \
    | LC_ALL=C sort -u > "$work/classified"

nopt=$(grep -c . "$work/options" || true)
if [ "${nopt:-0}" -lt 5 ]; then
    echo "FAILURES: read $nopt command-line option(s) out of the source;" >&2
    echo "the option parser has changed shape and this reads nothing" >&2
    exit 1
fi

if ! diff -u "$work/classified" "$work/options" > "$work/opts" 2>&1; then
    echo "FAILURES: the options the command line takes and the ones classified" >&2
    echo "in tests/image-config disagree. Say of each whether it changes the" >&2
    echo "language a context comes up with; one that does needs a bit, or two" >&2
    echo "languages tell each other apart by nothing. - classified and gone," >&2
    echo "+ taken and unclassified:" >&2
    sed -n '3,$p' "$work/opts" | sed 's/^/      /' >&2
    fail=1
fi

# each option the register calls a language option names a declared bit
sed 's/#.*$//' "$register" | awk 'NF == 3 && $2 == "yes" { print $1, $3 }' \
    > "$work/language"
nlang=$(grep -c . "$work/language" || true)
if [ "${nlang:-0}" -eq 0 ]; then
    echo "FAILURES: the register calls no option a language option, yet the" >&2
    echo "header declares $ndecl bit(s); one of the two is not being read" >&2
    fail=1
fi
while read -r opt bit; do
    [ -n "${opt:-}" ] || continue
    if ! grep -qx "$bit" "$work/declared"; then
        echo "check-image-config: $opt names $bit, which the header does not declare" >&2
        fail=1
    fi
done < "$work/language"

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS ($nopt option(s) classified, $nlang change the language," \
     "$ndecl bit(s) declared and set)"
