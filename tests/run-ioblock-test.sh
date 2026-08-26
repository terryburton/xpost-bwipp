#!/bin/sh
# A read that would wait gives the scheduler something else to run.
#
# The interpreter can say "this context is blocked on input" -- the ioblock
# return, which makes the mainloop put the operator back and choose another
# context. This holds the read to producing that answer, and to producing it
# only where it is right to.
#
# The test is a deadlock rather than a race, so it does not depend on how
# fast anything is. A writer holds its byte back until a file appears, and
# only the forked context creates that file. If the read waits where it
# stands, the child never runs, the file never appears, the byte never
# arrives, and the run hangs until the harness kills it. If the read is
# given up, the child runs, the file appears, the byte arrives, and the
# read -- run again from the top, its operand put back -- gets it.
#
# The second half holds the other half of the bargain: with only one context
# there is nothing to switch to, so the read must still wait in select()
# rather than spin. That is measured as processor time against elapsed time.
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"
verdict_workdir

# The whole of this rests on the poll a read does before it waits, and
# that block is admitted by sys/select.h, which the Windows builds do not
# have: a read there waits in the C library, where nothing can ask it to
# give way. So there is nothing to hold the platform to -- the source says
# as much where the block is written (xpost_file.c).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "skipped: a read waits in the C library here, with no poll to give way at"
        exit 77 ;;
esac

command -v mkfifo >/dev/null 2>&1 || { echo "SKIP: no mkfifo"; exit 77; }

# ---- a blocked read lets a forked context run -------------------------

mkfifo "$work/p" || { echo "SKIP: cannot make a fifo here"; exit 77; }

# the paths go in as the file is written: sed's in-place option takes a
# backup suffix on some platforms and not others, so it is not used here
cat > "$work/block.ps" <<PS
/pipe ($work/p) (r) file def
% the child's whole job is to prove it ran: the writer is waiting for this
% yield first: fork switches to the child at once, so without the yield the
% child would finish before the parent ever reached the read. Yielding puts
% the parent back in charge with the child still owing its one job, which is
% what makes that job depend on the read giving way.
mark { yield ($work/go) (w) file closefile } fork /child exch def
% the byte cannot arrive until the child has run, so a read that waits here
% waits for ever
pipe read
    { (READ-) print 3 string cvs print (\n) print }
    { (READ-EOF\n) print } ifelse
child join
(CHILD-JOINED\n) print
flush
PS

# The writer opens its end at once, read-write rather than write-only: a
# write-only open of a fifo waits for a reader, and on at least one
# platform that wait comes back EINTR when the shell is signalled, which
# leaves the fifo with no writer and the reader waiting for ever. Opened
# read-write it does not wait at all, and still counts as the writer the
# reader is waiting for.
(
    exec 3<>"$work/p"
    i=0
    while [ ! -f "$work/go" ] && [ $i -lt 160 ]; do
        i=$((i + 1))
        sleep 0.05
    done
    # the byte goes only where the child said it ran. Sending it regardless
    # would let a read that never gave way finish anyway, and the test would
    # pass on a tree with none of this wired
    if [ -f "$work/go" ]; then
        printf 'X' >&3
    fi
    exec 3>&-
) &
writer=$!

out=$(run_limited 60 "$xpost" -q --no-sandbox --enable-dps -d null \
        "$work/block.ps" </dev/null 2>&1)
st=$?
wait "$writer" 2>/dev/null

verdict_run "$st" "$out" "the blocked-read run" || exit 1

case $out in
    *READ-88*) ;;
    *READ-EOF*)
        echo "FAIL: the read ended rather than waiting for its byte: $out"
        exit 1 ;;
    *)
        echo "FAIL: the read did not get the byte the writer sent: $out"
        exit 1 ;;
esac
case $out in
    *CHILD-JOINED*) ;;
    *) echo "FAIL: the forked context did not finish: $out"; exit 1 ;;
esac
echo "a blocked read let the forked context run"

# ---- with nothing else to run, the read sleeps ------------------------
#
# The claim is that the waiting itself costs no processor. Measured
# against a control rather than against a constant: the same program is
# run twice, once with its byte there at once and once with it three
# seconds late, and what the wait costs is the difference between them.
# A constant would be wrong wherever the interpreter is slower or
# hungrier to start than it is here -- under a sanitizer it is both --
# and the difference is what the claim was about anyway.

# The processor time is read from time(1), whose format option is not the
# same everywhere: where the one written here is not understood, the half
# that needs it is passed over rather than guessed at.
if ! /usr/bin/time -f '%U %S %e' -o /dev/null true >/dev/null 2>&1; then
    echo "SKIP: time(1) here does not take the format this reads"
    echo "SUCCESS: a blocked read yields when it can (the sleep half skipped)"
    exit 0
fi

# run the lone-context program against a fifo whose byte arrives after $1
# seconds, leaving the processor and elapsed times in $work/cpu
lone_run() {
    _delay=$1
    _fifo=$work/q$_delay
    rm -f "$_fifo"
    mkfifo "$_fifo" || return 1
    cat > "$work/lone$_delay.ps" <<PS
/pipe ($_fifo) (r) file def
pipe read { pop }{ } ifelse
(LONE-DONE\\n) print
flush
PS
    # The fifo is held open here, for the whole run, rather than in the
    # writer: a fifo closed before its reader opens has nowhere to keep
    # what was written and the reader then waits for a writer that has
    # been and gone. Held open, a byte written early simply waits in it.
    exec 3<>"$_fifo"
    _w=''
    if [ "$_delay" -gt 0 ]; then
        ( sleep "$_delay"; printf 'Y' >&3 ) &
        _w=$!
    else
        printf 'Y' >&3
    fi
    run_limited 90 /usr/bin/time -f '%U %S %e' -o "$work/cpu" \
        "$xpost" -q --no-sandbox --enable-dps -d null \
        "$work/lone$_delay.ps" </dev/null > "$work/lone.out" 2>&1
    _st=$?
    [ -n "$_w" ] && wait "$_w" 2>/dev/null
    exec 3>&-
    return $_st
}

lone_run 0
st=$?
out=$(cat "$work/lone.out")
verdict_run "$st" "$out" "the control run" || exit 1
case $out in
    *LONE-DONE*) ;;
    *) echo "FAIL: the control run did not finish: $out"; exit 1 ;;
esac
control=$(awk '{ printf "%.2f", $1 + $2 }' "$work/cpu")

lone_run 3
st=$?
out=$(cat "$work/lone.out")
verdict_run "$st" "$out" "the lone-context run" || exit 1
case $out in
    *LONE-DONE*) ;;
    *) echo "FAIL: the lone run did not finish: $out"; exit 1 ;;
esac
waited=$(awk '{ printf "%.2f", $1 + $2 }' "$work/cpu")
elapsed=$(awk '{ printf "%.2f", $3 }' "$work/cpu")

# it has to have waited at all, or there is nothing to say about the cost
short=$(awk -v e="$elapsed" 'BEGIN { print (e < 2.0) ? 1 : 0 }')
if [ "$short" -eq 1 ]; then
    echo "SKIP: the byte arrived in ${elapsed}s, so nothing waited here"
    echo "SUCCESS: a blocked read yields when it can (the sleep half skipped)"
    exit 0
fi

# spun, the three seconds would show up as three seconds of processor on
# top of what starting up costs; slept, as almost none
cost=$(awk -v a="$waited" -v b="$control" 'BEGIN { printf "%.2f", a - b }')
over=$(awk -v c="$cost" 'BEGIN { print (c > 1.0) ? 1 : 0 }')
if [ "$over" -eq 1 ]; then
    echo "FAIL: waiting ${elapsed}s for a byte cost ${cost}s of processor over"
    echo "      the same run without the wait (${control}s); the read is"
    echo "      spinning rather than sleeping"
    exit 1
fi
echo "a lone context slept through its wait (${cost}s of processor over the control)"


echo "SUCCESS: a blocked read yields when it can and sleeps when it cannot"
exit 0
