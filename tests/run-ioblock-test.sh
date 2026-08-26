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

command -v mkfifo >/dev/null 2>&1 || { echo "SKIP: no mkfifo"; exit 77; }

# ---- a blocked read lets a forked context run -------------------------

mkfifo "$work/p" || { echo "SKIP: cannot make a fifo here"; exit 77; }

cat > "$work/block.ps" <<'PS'
/pipe (PIPE) (r) file def
% the child's whole job is to prove it ran: the writer is waiting for this
% yield first: fork switches to the child at once, so without the yield the
% child would finish before the parent ever reached the read. Yielding puts
% the parent back in charge with the child still owing its one job, which is
% what makes that job depend on the read giving way.
mark { yield (GO) (w) file closefile } fork /child exch def
% the byte cannot arrive until the child has run, so a read that waits here
% waits for ever
pipe read
    { (READ-) print 3 string cvs print (\n) print }
    { (READ-EOF\n) print } ifelse
child join
(CHILD-JOINED\n) print
flush
PS
sed -i "s|PIPE|$work/p|; s|GO|$work/go|" "$work/block.ps"

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

out=$(run_limited 25 "$xpost" -q --no-sandbox --enable-dps -d null \
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

# The processor time is read from time(1), whose format option is not the
# same everywhere: where the one written here is not understood, the half
# that needs it is passed over rather than guessed at.
if ! /usr/bin/time -f '%U %S' -o /dev/null true >/dev/null 2>&1; then
    echo "SKIP: time(1) here does not take the format this reads"
    echo "SUCCESS: a blocked read yields when it can (the sleep half skipped)"
    exit 0
fi

mkfifo "$work/q" || { echo "SKIP: cannot make a second fifo"; exit 77; }

cat > "$work/lone.ps" <<'PS'
/pipe (PIPE) (r) file def
pipe read { pop }{ } ifelse
(LONE-DONE\n) print
flush
PS
sed -i "s|PIPE|$work/q|" "$work/lone.ps"

(
    exec 3<>"$work/q"
    sleep 2
    printf 'Y' >&3
    exec 3>&-
) &
writer=$!

# the processor time this run spends is the whole point: a read that spun
# would spend the two seconds it waits, one that sleeps spends almost none
run_limited 25 /usr/bin/time -f '%U %S' -o "$work/cpu" \
    "$xpost" -q --no-sandbox --enable-dps -d null \
    "$work/lone.ps" </dev/null > "$work/lone.out" 2>&1
st=$?
out=$(cat "$work/lone.out")
wait "$writer" 2>/dev/null

verdict_run "$st" "$out" "the lone-context run" || exit 1
case $out in
    *LONE-DONE*) ;;
    *) echo "FAIL: the lone run did not finish: $out"; exit 1 ;;
esac

spent=$(awk '{ printf "%.2f", $1 + $2 }' "$work/cpu")

# a two-second wait spun would cost about two seconds of processor; slept it
# costs a small fraction of one. The bar is set well clear of both.
over=$(awk -v s="$spent" 'BEGIN { print (s > 1.0) ? 1 : 0 }')
if [ "$over" -eq 1 ]; then
    echo "FAIL: a lone context spent ${spent}s of processor waiting 2s for a"
    echo "      byte; the read is spinning rather than sleeping"
    exit 1
fi
echo "a lone context slept through its wait (${spent}s of processor)"

echo "SUCCESS: a blocked read yields when it can and sleeps when it cannot"
exit 0
