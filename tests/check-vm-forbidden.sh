#!/bin/sh
#
# Nothing new becomes reachable and writable in global virtual memory.
#
# A restore does not reach global virtual memory (PLRM 3.7.2), so a write into
# such an object stands for the rest of the job, and the machinery's namespaces
# are reachable through the bodies bind froze the dictionaries into. An object
# with all three properties is one a program can rewrite and the machinery will
# then run.
#
# What is held is a count and a digest, never a list. A list of the objects
# that still have the property is a map for whoever would like to use them, and
# both this register and this script's output are published. So a failure here
# says which way the set moved and by how much, and does not name anything: the
# person acting on it runs tests/vm_forbidden_test.ps locally, where the sweep
# names them.
#
# The count is a ratchet -- it may fall, it may not rise -- and the pair is
# rewritten in the commit that moves it. That is the point of holding it in the
# build: a claim about what a program can reach, made by whoever checked the
# corner they thought of, does not survive their attention moving on.
#
#   $1  path to the source tree root
#   $2  path to the built xpost binary
set -u
src=${1:?usage: check-vm-forbidden.sh <srcroot> <xpost>}
xpost=${2:?usage: check-vm-forbidden.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
golden="$src/tests/vm_forbidden.golden"
guard_require_file "$golden" "the register of what a program can reach and write"
guard_require_file "$src/tests/vm_forbidden_test.ps" "the sweep"

guard_workdir

# XPOST_CENSUS asks for the walk-coverage census, which is not taken
# otherwise: it costs peak resident memory at startup and only a reading
# wants it. A run without it answers nothing reached, which is refused
# below rather than read as nothing missed.
# Asked of a run that was given an output file, because that is what an
# invocation looks like and it is not the same question: the name a run
# writes to arrives as a host setting, is copied onto the device and
# reached from the template graphics state, and none of that exists in a
# run that named no output. Asked without it, this reported nothing while
# two objects a program could write stood there.
XPOST_DATA_DIR="$src/data" XPOST_NO_VM_IMAGE=1 XPOST_CENSUS=1 \
    "$xpost" -q --no-sandbox -d null -o /dev/null "$src/tests/vm_forbidden_test.ps" \
    </dev/null > "$work/out" 2>&1

# And asked again of a run that started from the image of virtual memory,
# because that is how a run starts once one has been written and the
# numbers above would otherwise describe a boot nobody does twice. The
# two must agree: an image carrying a language whose objects answer
# differently from the one the boot files build is a difference nothing
# else here would report.
XPOST_DATA_DIR="$src/data" XPOST_CENSUS=1 \
    "$xpost" -q --no-sandbox -d null -o /dev/null "$src/tests/vm_forbidden_test.ps" \
    </dev/null > "$work/out.image" 2>&1
if grep -q '^SWEPT$' "$work/out.image"; then
    if ! diff -q "$work/out" "$work/out.image" >/dev/null 2>&1; then
        echo "FAIL: the census answers differently from the image than from"
        echo "      the boot files, so what a run reaches depends on which"
        echo "      way it started:"
        diff "$work/out" "$work/out.image" | sed 's/^/      /' | head -12
        exit 1
    fi
else
    echo "FAIL: the census could not be taken from the image, so this asks"
    echo "      nothing of the way a run starts once an image exists"
    exit 1
fi

if ! grep -q '^SWEPT$' "$work/out"; then
    echo "FAILURES: the sweep did not run to its end, so its silence means"
    echo "      nothing. It reported:"
    sed 's/^/      /' "$work/out" | head -12
    exit 1
fi

have_n=$(awk '/^REACHABLE-WRITABLE-GLOBAL /{print $2}' "$work/out")
have_d=$(awk '/^REACHABLE-WRITABLE-GLOBAL /{print $3}' "$work/out")
want_n=$(awk '/^[0-9]+ [0-9]+$/{print $1; exit}' "$golden")
want_d=$(awk '/^[0-9]+ [0-9]+$/{print $2; exit}' "$golden")

case ${have_n:-x}${have_d:-x} in
    *[!0-9]*|'') echo "FAILURES: the sweep did not report a count and a digest"; exit 1 ;;
esac
case ${want_n:-x}${want_d:-x} in
    *[!0-9]*|'') echo "FAILURES: $golden holds no 'count digest' line"; exit 1 ;;
esac

if [ "$have_n" -gt "$want_n" ]; then
    echo "FAILURES: $((have_n - want_n)) more object(s) in global virtual memory"
    echo "      are now reachable and writable. A program can rewrite one and the"
    echo "      machinery will run what it wrote, and a restore will not put it"
    echo "      back. Run tests/vm_forbidden_test.ps locally to see which."
    exit 1
fi
if [ "$have_n" -lt "$want_n" ]; then
    echo "FAILURES: $((want_n - have_n)) fewer than the register records. Some were"
    echo "      fixed and the register was not moved with them -- write"
    echo "      '$have_n $have_d' into $(basename "$golden") in the commit that fixed"
    echo "      them, so the ratchet keeps its meaning."
    exit 1
fi
if [ "$have_d" != "$want_d" ]; then
    echo "FAILURES: the set changed without changing size: the register records"
    echo "      digest $want_d and the sweep answers $have_d. Something acquired"
    echo "      the property and something else lost it. Run"
    echo "      tests/vm_forbidden_test.ps locally to see what."
    exit 1
fi

have_x=$(awk '/^DECLARED-WRITABLE-STORAGE /{print $2}' "$work/out")
want_x=$(awk '/^declared [0-9]+$/{print $2; exit}' "$golden")
case ${have_x:-x}${want_x:-x} in
    *[!0-9]*|'') echo "FAILURES: the declared-storage count is missing"; exit 1 ;;
esac
if [ "$have_x" -gt "$want_x" ]; then
    echo "FAILURES: $((have_x - want_x)) more object(s) are declared as storage the"
    echo "      language lets programs write, so the sweep now passes over them."
    echo "      That declaration is the one hole in the rule; it may shrink and it"
    echo "      may not grow. If the growth is right, say why in the commit and"
    echo "      write 'declared $have_x' into $(basename "$golden")."
    exit 1
fi
if [ "$have_x" -lt "$want_x" ]; then
    echo "FAILURES: $((want_x - have_x)) fewer declared than the register records --"
    echo "      write 'declared $have_x' into $(basename "$golden") in the same commit."
    exit 1
fi

have_l=$(awk '/^REACHABLE-WRITABLE-LOCAL /{print $2}' "$work/out")
have_ld=$(awk '/^REACHABLE-WRITABLE-LOCAL /{print $3}' "$work/out")
want_l=$(awk '/^local [0-9]+ [0-9]+$/{print $2; exit}' "$golden")
want_ld=$(awk '/^local [0-9]+ [0-9]+$/{print $3; exit}' "$golden")
case ${have_l:-x}${have_ld:-x}${want_l:-x}${want_ld:-x} in
    *[!0-9]*|'') echo "FAILURES: the local-memory count and digest are missing"; exit 1 ;;
esac
if [ "$have_l" -gt "$want_l" ]; then
    echo "FAILURES: $((have_l - want_l)) more machinery object(s) in LOCAL virtual"
    echo "      memory are reachable and writable. A program can rewrite one and the"
    echo "      machinery will run what it wrote for the rest of the job -- which,"
    echo "      one job to a request, is the whole request. A restore and the job"
    echo "      boundary do put it back. Run tests/vm_forbidden_test.ps locally."
    exit 1
fi
if [ "$have_l" -lt "$want_l" ] || [ "$have_ld" != "$want_ld" ]; then
    echo "FAILURES: the local-memory set moved: the register records"
    echo "      '$want_l $want_ld' and the sweep answers '$have_l $have_ld'. Write"
    echo "      'local $have_l $have_ld' into $(basename "$golden") in the commit that"
    echo "      moved it, so the ratchet keeps its meaning."
    exit 1
fi

have_f=$(awk '/^FROZEN-WRITABLE-CONSTANTS /{print $2}' "$work/out")
have_fd=$(awk '/^FROZEN-WRITABLE-CONSTANTS /{print $3}' "$work/out")
want_f=$(awk '/^frozen [0-9]+ [0-9]+$/{print $2; exit}' "$golden")
want_fd=$(awk '/^frozen [0-9]+ [0-9]+$/{print $3; exit}' "$golden")
case ${have_f:-x}${have_fd:-x}${want_f:-x}${want_fd:-x} in
    *[!0-9]*|'') echo "FAILURES: the frozen-constant count is missing"; exit 1 ;;
esac
if [ "$have_f" -gt "$want_f" ]; then
    echo "FAILURES: $((have_f - want_f)) more constant(s) frozen into a machinery"
    echo "      body are reachable and writable. A program that reads a body reaches"
    echo "      one, and what such a table decides is how the machinery reads what"
    echo "      the program gave it. Seal it where it is defined, or say in the"
    echo "      commit why it may be written, and write 'frozen $have_f $have_fd'"
    echo "      into $(basename "$golden")."
    exit 1
fi
if [ "$have_f" -lt "$want_f" ] || [ "$have_fd" != "$want_fd" ]; then
    echo "FAILURES: the frozen-constant set moved: the register records"
    echo "      '$want_f $want_fd' and the sweep answers '$have_f $have_fd'. Write"
    echo "      'frozen $have_f $have_fd' into $(basename "$golden") in the commit"
    echo "      that moved it."
    exit 1
fi

have_s=$(awk '/^STORE-SHIELDED-STATE /{print $2}' "$work/out")
have_sd=$(awk '/^STORE-SHIELDED-STATE /{print $3}' "$work/out")
want_s=$(awk '/^shielded [0-9]+ [0-9]+$/{print $2; exit}' "$golden")
want_sd=$(awk '/^shielded [0-9]+ [0-9]+$/{print $3; exit}' "$golden")
case ${have_s:-x}${have_sd:-x}${want_s:-x}${want_sd:-x} in
    *[!0-9]*|'') echo "FAILURES: the store-shielded count is missing"; exit 1 ;;
esac
if [ "$have_s" -gt "$want_s" ]; then
    echo "FAILURES: $((have_s - want_s)) more object(s) a machinery body reaches"
    echo "      are held in the job store, where the constant rule passes over"
    echo "      them. That is the one place the rule gives way, and it may shrink"
    echo "      and may not grow. If the growth is right, say why in the commit"
    echo "      and write 'shielded $have_s $have_sd' into $(basename "$golden")."
    exit 1
fi
if [ "$have_s" -lt "$want_s" ] || [ "$have_sd" != "$want_sd" ]; then
    echo "FAILURES: the store-shielded set moved: the register records"
    echo "      '$want_s $want_sd' and the sweep answers '$have_s $have_sd'. Write"
    echo "      'shielded $have_s $have_sd' into $(basename "$golden") in the"
    echo "      commit that moved it."
    exit 1
fi

have_r=$(awk '/^WALK-BLIND-REACHED /{print $2}' "$work/out")
case ${have_r:-0} in
    ''|*[!0-9]*|0) echo "FAILURES: the walk-coverage census was not taken, so"
                   echo "      its answer says nothing about what the sweep misses"
                   exit 1 ;;
esac
have_rb=$(awk '/^READABLE-BODIES /{print $2}' "$work/out")
have_rbd=$(awk '/^READABLE-BODIES /{print $3}' "$work/out")
want_rb=$(awk '/^bodies [0-9]+ [0-9]+$/{print $2; exit}' "$golden")
want_rbd=$(awk '/^bodies [0-9]+ [0-9]+$/{print $3; exit}' "$golden")
if [ -z "$want_rb" ]; then
    echo "FAIL: $golden states no 'bodies' pair, and the run reports"
    echo "      $have_rb. The register is the only thing holding this."
    exit 1
elif [ "$have_rb" -gt "$want_rb" ]; then
    echo "FAIL: $have_rb machinery bodies can still be read, where the"
    echo "      register allows $want_rb. A body a program can read is a"
    echo "      namespace it can reach, bind having frozen one in."
    exit 1
elif [ "$have_rb" -lt "$want_rb" ] || [ "$have_rbd" != "$want_rbd" ]; then
    echo "NOTE: readable bodies moved to $have_rb $have_rbd; say in the"
    echo "      commit why it stands, and write 'bodies $have_rb $have_rbd'"
    echo "      into $golden."
    exit 1
fi

have_lc=$(awk '/^LOCAL-CODE-TABLES /{print $2}' "$work/out")
have_lcd=$(awk '/^LOCAL-CODE-TABLES /{print $3}' "$work/out")
want_lc=$(awk '/^localcode [0-9]+ [0-9]+$/{print $2; exit}' "$golden")
want_lcd=$(awk '/^localcode [0-9]+ [0-9]+$/{print $3; exit}' "$golden")
case ${have_lc:-x}${have_lcd:-x}${want_lc:-x}${want_lcd:-x} in
    *[!0-9]*|'') echo "FAILURES: the local-code-table count is missing"; exit 1 ;;
esac
if [ "$have_lc" -gt "$want_lc" ]; then
    echo "FAILURES: $((have_lc - want_lc)) more table(s) in local virtual memory"
    echo "      can be written by a program and run out of by the machinery."
    echo "      A program that replaces a method in one has the machinery run"
    echo "      it for the rest of the job. Split the table -- methods where"
    echo "      they cannot be written, state beside them -- or say in the"
    echo "      commit why it stands, and write 'localcode $have_lc $have_lcd'"
    echo "      into $(basename "$golden")."
    exit 1
fi
if [ "$have_lc" -lt "$want_lc" ] || [ "$have_lcd" != "$want_lcd" ]; then
    echo "FAILURES: the local-code-table set moved: the register records"
    echo "      '$want_lc $want_lcd' and the sweep answers '$have_lc $have_lcd'."
    echo "      Write 'localcode $have_lc $have_lcd' into $(basename "$golden")"
    echo "      in the commit that moved it."
    exit 1
fi

have_c=$(awk '/^BODY-CLOSED-CONSTANTS /{print $2}' "$work/out")
have_cd=$(awk '/^BODY-CLOSED-CONSTANTS /{print $3}' "$work/out")
want_c=$(awk '/^closed [0-9]+ [0-9]+$/{print $2; exit}' "$golden")
want_cd=$(awk '/^closed [0-9]+ [0-9]+$/{print $3; exit}' "$golden")
case ${have_c:-x}${have_cd:-x}${want_c:-x}${want_cd:-x} in
    *[!0-9]*|'') echo "FAILURES: the body-closed count is missing"; exit 1 ;;
esac
if [ "$have_c" != "$want_c" ] || [ "$have_cd" != "$want_cd" ]; then
    echo "FAILURES: the set execute-only has closed moved: the register"
    echo "      records '$want_c $want_cd' and the sweep answers"
    echo "      '$have_c $have_cd'. A body made execute-only moves objects"
    echo "      here out of the frozen count, one for one -- so check that"
    echo "      frozen fell by as much, and write 'closed $have_c $have_cd'"
    echo "      into $(basename "$golden") in the commit that moved it."
    exit 1
fi

have_b=$(awk '/^WALK-BLIND-CONTAINERS /{print $2}' "$work/out")
want_b=$(awk '/^blind [0-9]+$/{print $2; exit}' "$golden")
case ${have_b:-x}${want_b:-x} in
    *[!0-9]*|'') echo "FAILURES: the walk-blind count is missing"; exit 1 ;;
esac
if [ "$have_b" -gt "$want_b" ]; then
    echo "FAILURES: $((have_b - want_b)) more container(s) the interpreter can"
    echo "      reach are outside the walk's roots, so nothing here is asked of"
    echo "      them. Add the root that reaches them to the list the lockdown"
    echo "      builds, or say in the commit why it stands, and write"
    echo "      'blind $have_b' into $(basename "$golden")."
    exit 1
fi
if [ "$have_b" -lt "$want_b" ]; then
    echo "FAILURES: the walk now reaches more than the register records:"
    echo "      it says $want_b and the sweep answers $have_b. Write"
    echo "      'blind $have_b' into $(basename "$golden") in the commit"
    echo "      that moved it."
    exit 1
fi

printf 'reachable writable: global %s, local %s; declared %s; frozen constants %s; store-shielded %s; walk-blind %s: SUCCESS\n' \
    "$have_n" "$have_l" "$have_x" "$have_f" "$have_s" "$have_b"
exit 0
