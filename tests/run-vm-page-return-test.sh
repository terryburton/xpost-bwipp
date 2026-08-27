#!/bin/sh
# Whether a program asking for a collection gets the storage back.
#
# The executable holds the mechanism on its own; what needs two processes
# is whether the operator a program actually calls reaches it. One
# program, run twice, differing only in the `1 vmreclaim` at its end, and
# each process reports what it is left holding.
#
# It cannot be two figures from one process. What a program drops is
# freed twice: once as it runs, which is what a collection it asks for
# can hand back, and again by the rewind that ends the job, which hands
# nothing back. So a figure read after the job has ended says nothing on
# its own, and the comparison has to be between two runs that differ in
# the one thing being asked about.
#
# WHAT COUNTS AS AN ANSWER. The asking run must be lower by a real
# amount. "Real" is a fixed number of bytes rather than a proportion: a
# proportion of a figure that includes the executable, the library and
# the boot would be satisfied by noise, where the storage at stake is a
# known quantity -- the program builds and drops sixteen megabytes, and
# the whole pages inside what a collection recovers from that come to
# about four. A megabyte is asked for, which is far above the spread
# between repeats and far below what the change delivers.
#
#   $1  path to the vm_page_return_test executable
exe=${1:?usage: run-vm-page-return-test.sh <vm_page_return_test executable>}

. "$(dirname "$0")/verdict.sh"

# the mechanism first: a pair that disagreed would have nothing to say if
# the return did not work at all
out=$("$exe" 2>&1); st=$?
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the return itself" || exit 1

# What the mechanism says about this host is what the pair is asked in the
# light of. An arena the memory file borrowed from the host allocator is not
# the process's to give back, and the pass gathers the storage above the
# cursor there as it does anywhere -- so the pair would run, agree, and be
# read as a return that did not happen. Reported as a skip rather than a
# pass: nothing was handed back, and saying so is the whole of what can be
# said on such a host.
case $out in
    *"keeps the storage the arena gathered"*)
        echo "SKIP: the arena here is the host allocator's, so nothing is"
        echo "      handed back for the pair to differ over"
        exit 0 ;;
esac

# Each half reports its figure and its status, so both are judged: a run
# that printed a figure and then complained on its way out has not
# answered, and a figure read off it would be read off a broken run.
run_half() {
    out=$("$exe" "$1" 2>&1); st=$?
    verdict_run "$st" "$out" "the $1 run" || return 1
    printf '%s\n' "$out" | sed -n 's/^rss=//p'
}

noask=$(run_half noask) || exit 1
ask=$(run_half ask) || exit 1

if [ -z "$noask" ] || [ -z "$ask" ]; then
    echo "FAILURES: a run reported no resident figure"
    exit 1
fi

# A host that does not report a resident set says zero, and there is
# nothing here to compare. Reported as a skip and not as a pass: a pass
# would say the storage went.
if [ "$noask" -eq 0 ] || [ "$ask" -eq 0 ]; then
    echo "SKIP: this host does not report a resident set"
    exit 0
fi

want=1048576
saved=$((noask - ask))
if [ "$saved" -lt "$want" ]; then
    echo "FAIL: a program that asks for a collection is left holding"
    echo "      $ask bytes against $noask without the request, which is"
    echo "      $saved less and not the $want a collection over what it"
    echo "      dropped has to hand back"
    echo "FAILURES: asking for a collection does not return the storage"
    exit 1
fi

echo "SUCCESS ($saved bytes fewer held when the program asks: $ask against $noask)"
