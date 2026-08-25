#!/bin/sh
# Meson test wrapper: bring an interpreter up out of an image of virtual
# memory, and say whether the image is worth having.
#
# Two questions, in two halves.
#
# Idempotence. A context booted the long way writes an image; a second
# process reads that image and writes another; the two must match to the
# byte. That is the gate, and nothing weaker will do: an image that is
# subtly wrong is a silent wrong answer rather than a crash, so what has
# to be shown is not that the interpreter still runs but that the memory
# it runs on is the memory the image describes. Two processes, because
# what an image must not carry is anything of the process that wrote it.
# It holds one of this run's images against another of them, so it says
# an image describes the memory it was written from and cannot say that
# memory was right: a boot that built the language wrongly writes the
# same wrong image twice and passes. What a boot has to produce is
# tests/vm_image_test.c's, which compares two fresh boots field by field
# and says in its own prose what that establishes and what it does not.
#
# Refusal. Every way an image can be unusable has to end with the
# language built from the boot files instead. Each way is asked for in
# turn -- every stamp at the head of the image damaged one at a time, the
# operator names permuted, an entity pointed past its arena, the file cut
# short, lengthened, and made unrecognisable -- and each must leave a
# working interpreter that says it built the language.
#
#   $1  path to the vm-image-load test executable
set -u
exe=${1:?usage: run-vm-image-load-test.sh <vm_image_load_test executable>}
. "$(dirname "$0")/verdict.sh"

verdict_workdir
if [ -z "$work" ] || [ ! -d "$work" ]; then
    echo "FAILURES: could not make a scratch directory (is TMPDIR writable?)"
    exit 1
fi

fail=0

# One boot, with the environment saying where to read an image from.
# Answers the output, so a caller can read which way the language came.
# A run with no image named reads the one in the user's cache if there is
# one, which is what the image is for and is not what this asks about:
# each boot here is given the image it is to read, or told there is none.
boot() {                # <image to read, or empty> <image to write, or empty>
    if [ -z "${1:-}" ]; then
        ( unset XPOST_VM_IMAGE
          XPOST_NO_VM_IMAGE=1 XPOST_VM_IMAGE_WRITE=${2:-} "$exe" boot 2>&1 )
    else
        ( unset XPOST_NO_VM_IMAGE
          XPOST_VM_IMAGE=$1 XPOST_VM_IMAGE_WRITE=${2:-} "$exe" boot 2>&1 )
    fi
}

# The long way, and the image it produces.
out=$(boot "" "$work/a.img")
status=$?
verdict_run "$status" "$out" "the boot that writes the image" || exit 1
case $out in
    *"the language was built from the boot files"*) ;;
    *) echo "FAILURES: a boot with no image to read did not build the"
       echo "      language: $out"
       exit 1 ;;
esac
if [ ! -s "$work/a.img" ]; then
    echo "FAILURES: the first boot wrote no image of virtual memory"
    exit 1
fi

# The short way, and the image it produces.
out=$(boot "$work/a.img" "$work/b.img")
status=$?
verdict_run "$status" "$out" "the boot that reads the image" || exit 1
case $out in
    *"the language was read from an image"*) ;;
    *) echo "FAILURES: a boot given an image of its own build did not read"
       echo "      it, so nothing below says anything about reading one: $out"
       exit 1 ;;
esac

if cmp -s "$work/a.img" "$work/b.img"; then
    echo "idempotent: a context out of an image writes back the image it read"
else
    echo "FAILURES: a context brought up out of an image wrote back an image"
    echo "      that differs from the one it read"
    cmp "$work/a.img" "$work/b.img" 2>&1 | sed 's/^/      /'
    fail=1
fi

# Refusal, one damage at a time.
damages=$("$exe" damages 2>&1)
status=$?
verdict_run "$status" "$damages" "asking what an image can be damaged in" \
    || exit 1
count=$(printf '%s\n' "$damages" | sed -n 1p)
case $count in
    ''|*[!0-9]*)
        echo "FAILURES: could not read how many ways an image can be damaged"
        exit 1 ;;
esac
if [ "$count" -lt 20 ]; then
    echo "FAILURES: only $count ways to damage an image were offered, which is"
    echo "      fewer than the fixed ones this knows about"
    exit 1
fi

n=0
while [ "$n" -lt "$count" ]; do
    what=$(printf '%s\n' "$damages" | sed -n "$((n + 2))p")
    rm -f "$work/d.img"
    out=$("$exe" damage "$work/a.img" "$work/d.img" "$n" 2>&1)
    verdict_run "$?" "$out" "damaging an image in $what" || { fail=1; n=$((n+1)); continue; }

    out=$(boot "$work/d.img" "")
    status=$?
    if [ "$status" != 0 ]; then
        echo "FAILURES: a boot given an image damaged in $what did not"
        echo "      come up at all"
        printf '%s\n' "$out" | sed 's/^/      /'
        fail=1
    else
        case $out in
            *"the language was built from the boot files"*)
                echo "refused: $what" ;;
            *)
                echo "FAILURES: an image damaged in $what was read rather"
                echo "      than refused"
                fail=1 ;;
        esac
    fi
    n=$((n + 1))
done

# ---- the image a run finds for itself
#
# The two halves above are about an image somebody named. This is about
# the one nobody did: a run told nothing looks in the user's cache, and
# writes there when it finds none. That is what makes the image worth
# having -- the saving goes to whoever runs the interpreter rather than
# to whoever knew to arrange it -- so it is held to the same two things
# as a named image. A first run builds the language and leaves a file; a
# second finds that file and reads it.
#
# HOME and the cache variable are moved into the scratch directory, so
# this reads and writes the test's own cache and never the user's.
cache=$work/home
mkdir -p "$cache" || { echo "FAILURES: could not make a cache directory"; exit 1; }

found() {               # -  .  what the boot said
    ( unset XPOST_VM_IMAGE XPOST_VM_IMAGE_WRITE XPOST_NO_VM_IMAGE
      HOME=$cache XDG_CACHE_HOME=$cache LOCALAPPDATA=$cache \
          "$exe" boot 2>&1 )
}

out=$(found)
status=$?
verdict_run "$status" "$out" "the first boot with an empty cache" || exit 1
case $out in
    *"the language was built from the boot files"*) ;;
    *) echo "FAILURES: a boot with an empty cache did not build the language:"
       echo "      $out"
       fail=1 ;;
esac

left=$(find "$cache" -name '*.vmimg' 2>/dev/null | sed -n 1p)
if [ -z "$left" ]; then
    echo "FAILURES: a boot that built the language wrote no image into the"
    echo "      cache, so no run after it can save anything"
    fail=1
else
    # Nothing part-written is left where another run would read it.
    stray=$(find "$cache" -name '*.tmp' 2>/dev/null | sed -n 1p)
    if [ -n "$stray" ]; then
        echo "FAILURES: a file the write goes through was left behind: $stray"
        fail=1
    fi

    out=$(found)
    status=$?
    verdict_run "$status" "$out" "the second boot with a filled cache" || exit 1
    case $out in
        *"the language was read from an image"*)
            echo "found: a run told nothing about an image writes one and the"
            echo "       next run reads it" ;;
        *) echo "FAILURES: a boot did not read the image the boot before it"
           echo "      left in the cache: $out"
           fail=1 ;;
    esac
fi

# Told there is to be no image, a run neither reads nor writes one.
rm -rf "$cache"
mkdir -p "$cache" || { echo "FAILURES: could not remake the cache directory"; exit 1; }
out=$( unset XPOST_VM_IMAGE XPOST_VM_IMAGE_WRITE
       XPOST_NO_VM_IMAGE=1 HOME=$cache XDG_CACHE_HOME=$cache \
           LOCALAPPDATA=$cache "$exe" boot 2>&1 )
case $out in
    *"the language was built from the boot files"*) ;;
    *) echo "FAILURES: a run refusing images did not build the language: $out"
       fail=1 ;;
esac
left=$(find "$cache" -name '*.vmimg' 2>/dev/null | sed -n 1p)
if [ -n "$left" ]; then
    echo "FAILURES: a run refusing images wrote one anyway: $left"
    fail=1
fi

[ "$fail" = 0 ] || { echo "FAILURES: an image of virtual memory is not safe to read"; exit 1; }
echo "SUCCESS (idempotent, found without being named, and $count ways of" \
     "being unusable each refused)"
