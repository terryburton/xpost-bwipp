#!/bin/sh
#
# Nothing new becomes callable by a program.
#
# The guard beside this one asks whether the machinery's objects are sealed,
# and they are. This asks the question sealing does not answer: an operator is
# not a writable object, so a fully sealed object graph still hands over any
# part of itself if an operator stands that installs a replacement. What is
# counted here is the entry points themselves, reached from the roots a
# program holds -- systemdict, userdict and globaldict, and internaldict,
# whose password the specification publishes (PLRM 8) and which is therefore
# a root a program holds rather than a place where anything is out of reach.
#
# What is held is a count and a digest, never a list, for the reason set out
# in the register: this script's output goes into public build logs, and a
# list of what can still be called is a map for whoever would like to call it.
#
# Each count is a ratchet -- it may fall, it may not rise -- and the pair is
# rewritten in the commit that moves it.
#
#   $1  path to the source tree root
#   $2  path to the built xpost binary
set -u
src=${1:?usage: check-machinery-reachable.sh <srcroot> <xpost>}
xpost=${2:?usage: check-machinery-reachable.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
golden="$src/tests/machinery_reachable.golden"
guard_require_file "$golden" "the register of what a program can call"
walk="$src/tests/machinery_reachable_test.ps"
guard_require_file "$walk" "the walk"

guard_workdir

# This guard is the one that must NOT take a census. Every other suite here
# asks for one, so that it can reach the machinery at all; this one asks what
# a shipped run answers, and a census run answers something else entirely.
# Unset rather than assumed unset: the shared guard preamble exports it, and
# a control that depends on nobody having set the variable is not a control.
unset XPOST_CENSUS

run() {  # extra-args outfile
    XPOST_DATA_DIR="$src/data" XPOST_NO_VM_IMAGE=1 \
        "$xpost" -q $1 -d null -o /dev/null "$walk" </dev/null > "$2" 2>&1
    if ! grep -q '^WALKED$' "$2"; then
        echo "FAILURES: the walk did not run to its end, so its silence means"
        echo "      nothing. It reported:"
        sed 's/^/      /' "$2" | head -12
        exit 1
    fi
}

run ""             "$work/sandboxed"
run "--no-sandbox" "$work/open"

# And again from the image of virtual memory, because that is how a run starts
# once one has been written. An image carrying a language whose entry points
# answer differently from the one the boot files build is a difference nothing
# else here would report.
XPOST_DATA_DIR="$src/data" \
    "$xpost" -q -d null -o /dev/null "$walk" </dev/null > "$work/image" 2>&1
if ! grep -q '^WALKED$' "$work/image"; then
    echo "FAIL: the walk could not be taken from the image, so this asks"
    echo "      nothing of the way a run starts once an image exists"
    exit 1
fi
# The comparison is of what the two runs ANSWER, not of the order they
# happened to enumerate a dictionary in: the name block below is a set, and
# a language built from the boot files hashes it into a different order from
# one loaded out of an image. Only the reported lines are compared.
for f in sandboxed image; do
    grep -aE '^(MACHINERY-CALLABLE-[A-Z]+|WALKED)' "$work/$f" > "$work/$f.answers"
done
if ! diff -q "$work/sandboxed.answers" "$work/image.answers" >/dev/null 2>&1; then
    echo "FAIL: the walk answers differently from the image than from the"
    echo "      boot files, so what a program can call depends on which way"
    echo "      the run started:"
    diff "$work/sandboxed.answers" "$work/image.answers" | sed 's/^/      /' | head -12
    exit 1
fi

check() {  # key marker outfile what
    key=$1 marker=$2 out=$3 what=$4
    have_n=$(awk -v m="$marker" '$1==m{print $2}' "$out")
    have_d=$(awk -v m="$marker" '$1==m{print $3}' "$out")
    want_n=$(awk -v k="$key" '$1==k{print $2}' "$golden")
    want_d=$(awk -v k="$key" '$1==k{print $3}' "$golden")
    case ${have_n:-x}${have_d:-x} in
        *[!0-9]*|'') echo "FAILURES: the walk reported no '$marker' pair"; exit 1 ;;
    esac
    case ${want_n:-x}${want_d:-x} in
        *[!0-9]*|'') echo "FAILURES: $golden holds no '$key' line"; exit 1 ;;
    esac
    if [ "$have_n" -gt "$want_n" ]; then
        echo "FAILURES: $((have_n - want_n)) more machinery entry point(s) $what."
        echo "      A program can call one, and calling it reaches what writing"
        echo "      the object would have reached. Remove the entry point, or"
        echo "      refuse it in a shipped run the way the census operators do."
        echo "      If the growth is right, say why in the commit and write"
        echo "      '$key $have_n $have_d' into $(basename "$golden")."
        exit 1
    fi
    if [ "$have_n" -lt "$want_n" ] || [ "$have_d" != "$want_d" ]; then
        echo "FAILURES: the '$key' set moved: the register records"
        echo "      '$want_n $want_d' and the walk answers '$have_n $have_d'."
        echo "      Write '$key $have_n $have_d' into $(basename "$golden") in"
        echo "      the commit that moved it, so the ratchet keeps its meaning."
        exit 1
    fi
}

check nopassword MACHINERY-CALLABLE-NOPASSWORD "$work/sandboxed" \
    "are reachable with no password at all"
check password   MACHINERY-CALLABLE-PASSWORD   "$work/sandboxed" \
    "stand behind the password the specification publishes"
check nosandbox  MACHINERY-CALLABLE-NOPASSWORD "$work/open" \
    "are reachable with no password in a run started --no-sandbox"

# And the names systemdict holds that PLRM 8.2 does not define. Counting
# machinery by the leading dot cannot see a machinery name spelled without
# one: graphicsdict and DEVICE were both spelled without, and each answered
# with live machinery.
#
# Held as a LIST rather than a count. A count is a property of the build and
# not of the language: the device loaders depend on what was compiled in,
# NOFACES on whether a face library was found, WIN32 on the platform. A
# register holding a number therefore reports a different one on every
# configuration -- MEASURED, red on the faceless lane for NOFACES and on
# macOS for the device loaders it does not build. So an absence never fails
# here and an arrival always does: a name in neither PLRM 8.2 nor
# tests/nonplrm-names is surface nobody declared.
awk '/^SYSTEMDICT-NAMES-BEGIN$/{f=1;next} /^SYSTEMDICT-NAMES-END$/{f=0} f' \
    "$work/sandboxed" | sed 's|^/||' | sort -u > "$work/sdnames"
awk -F'\t' '!/^#/ && NF>=2 && $1!="absent" {print $2}' "$src/tests/plrm-operators" \
    | sort -u > "$work/plrm"
grep -vE '^#|^[[:space:]]*$' "$src/tests/nonplrm-names" | sort -u > "$work/declared"
comm -23 "$work/sdnames" "$work/plrm" > "$work/nonplrm"
comm -23 "$work/nonplrm" "$work/declared" > "$work/undeclared"
if [ -s "$work/undeclared" ]; then
    echo "FAILURES: systemdict holds $(wc -l < "$work/undeclared" | tr -d ' ') name(s)"
    echo "      that PLRM 8.2 does not define and tests/nonplrm-names does not"
    echo "      declare. A name there is one a program reaches in a single token,"
    echo "      whatever it is spelled with. If it belongs to the language, add it"
    echo "      to that file with what a program may do with it; if it is"
    echo "      machinery, sweep it at the lockdown:"
    sed 's/^/      /' "$work/undeclared" | head -12
    exit 1
fi

n=$(awk '$1=="nopassword"{print $2}' "$golden")
p=$(awk '$1=="password"{print $2}' "$golden")
printf 'machinery entry points callable: %s with no password, %s behind the published password: SUCCESS\n' "$n" "$p"
exit 0
