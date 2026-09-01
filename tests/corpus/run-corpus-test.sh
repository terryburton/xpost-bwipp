#!/bin/sh
# Meson wrapper for the differential corpus. It runs evaluate.sh over whatever
# corpora have been fetched into place and reports:
#   - SKIP (exit 77) when there is nothing to run: no corpus is present, the
#     comparison tools it needs (Ghostscript, ImageMagick compare) are absent,
#     or a corpus is present without the prelude its programs need and that
#     prelude is one fetch.sh populates rather than one the tree carries. The
#     corpus is thus never a build-time dependency -- populate it with fetch.sh
#     to make this test do its work.
#   - FAIL (exit 1) when xpost crashes or hangs on a corpus program, when the
#     evaluation did not reach every program it named, or when what drew no
#     page is not what the corpus declares draws none. A rendering
#     difference is a lead, not a verdict (see README.md), so it is reported
#     but does not fail the test; a signal death or a timeout is an
#     unambiguous regression and does, and so is a corpus only part of which
#     was evaluated -- a gate that reports success over work it did not do
#     says nothing about the work it did not do. Reaching a program is not
#     the same as rendering one, and the count of programs evaluated cannot
#     tell the two apart: a run in which every program failed reaches all of
#     them. So the pages that were never drawn are held to a declared set as
#     well, and a run that compared nothing has nothing declaring it may.
#     Nor is a page one engine drew the same as a page there was to draw: a
#     program that stops emitting partway through matches on every page it
#     reached and is silent about the rest, so the count of pages each
#     program has is declared too. The three numbers the run closes with --
#     what was declared, compared and absent -- are worked out a second time
#     here from the corpus on disk, and a report that does not meet them
#     fails whichever of the two is wrong.
#   - PASS (exit 0) otherwise, with the per-page differences left in the log for
#     inspection (meson test corpus -v, or meson-logs/testlog.txt).
#   $1  path to the built xpost binary (optional; evaluate.sh finds one itself)
#   $2  one corpus to evaluate (optional; all of them by default). Naming one
#       per test lets them run concurrently rather than end to end.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ "${1:-}" ] && XPOST=$1 && export XPOST
corpus=${2:-}

command -v gs >/dev/null 2>&1 || {
    echo "corpus: Ghostscript not found -- skipping"; exit 77; }
command -v compare >/dev/null 2>&1 || {
    echo "corpus: ImageMagick 'compare' not found -- skipping"; exit 77; }

have=0
if [ -n "$corpus" ]; then set -- "$here/$corpus/"; else set -- "$here"/*/; fi
for d in "$@"; do
    for p in "$d"*.ps "$d"*.eps; do
        [ -f "$p" ] && { have=1; break 2; }
    done
done
[ "$have" = 1 ] || {
    echo "corpus: ${corpus:-no corpus} not present -- run tests/corpus/fetch.sh, then re-run. Skipping."
    exit 77; }

out=$("$here/evaluate.sh" $corpus 2>&1)
st=$?
printf '%s\n' "$out"
# The evaluator's own status, before anything is read out of what it
# printed. Every verdict below is a pattern looked for in that text, and
# an evaluator that fell over prints little or nothing: no pattern is
# found, and the run that did none of the work tells the same story as
# the run that did all of it.
if [ "$st" -ne 0 ]; then
    echo "corpus: the evaluator exited with status $st -- see above"
    exit 1
fi
# The evaluator skips the lot when a tool it needs is not there, saying so
# and exiting zero. Taken as a pass, that has a run which did nothing tell
# the same story as a run which did everything.
printf '%s\n' "$out" | grep -q 'skipping all' && {
    echo "corpus: the evaluator found nothing to run with -- skipping"; exit 77; }
printf '%s\n' "$out" | grep -Eq 'XPOST (CRASHED|TIMED OUT)' && {
    echo "corpus: xpost crashed or hung on a program -- see above"; exit 1; }
printf '%s\n' "$out" | grep -q 'NOT EVALUATED' && {
    echo "corpus: part of the corpus was never evaluated -- see above"; exit 1; }
printf '%s\n' "$out" | grep -q 'NO-PAGE SET DIFFERS' && {
    echo "corpus: what drew no page is not what the corpus declares -- see above"; exit 1; }
printf '%s\n' "$out" | grep -q 'DISPLACED SET DIFFERS' && {
    echo "corpus: what displaced ink is not what the corpus declares -- see above"; exit 1; }
printf '%s\n' "$out" | grep -q 'REGISTER NAMES NOTHING' && {
    echo "corpus: a register names a program the corpus does not hold -- see above"; exit 1; }
printf '%s\n' "$out" | grep -q 'REGISTER GIVES NO REASON' && {
    echo "corpus: a program is declared nondeterministic with no reason given -- see above"; exit 1; }
printf '%s\n' "$out" | grep -q 'REGISTER MALFORMED' && {
    echo "corpus: a page count declares no number -- see above"; exit 1; }
printf '%s\n' "$out" | grep -q 'PAGES NOT DECLARED' && {
    echo "corpus: a program has no declared page count -- see above"; exit 1; }
printf '%s\n' "$out" | grep -q 'PAGE COUNT DIFFERS' && {
    echo "corpus: a program drew other than the pages declared for it -- see above"; exit 1; }
# What the evaluator says it did, read as numbers rather than as the
# presence of a sentence. A corpus every program of which is held out
# prints its held-out lines and no summary at all; a corpus that reached
# its programs and drew nothing prints a summary whose page count is
# zero. Both are runs that compared nothing, and a gate reading only for
# the sentence passes them both.
#
# A corpus whose prelude is populated rather than committed, and has not
# been populated, is passed over by the evaluator: with nothing to
# prepend there is nothing to compare. That is a corpus not fetched, so
# it reports as a skip here rather than as either.
if ! printf '%s\n' "$out" | grep -q 'programs evaluated'; then
    printf '%s\n' "$out" | grep -q 'prelude absent -- skipped' && {
        echo "corpus: ${corpus:-a corpus} has no prelude to run with -- skipping"; exit 77; }
    echo "corpus: the evaluator reported on no corpus at all -- see above"
    exit 1
fi

# The corpus as it stands on disk, worked out here rather than taken
# from the report: the programs its directory holds less the ones its
# registers hold out, the pages its "pages" file declares for those, and
# the pages of those its "nopage" file declares are never drawn. Three
# numbers this script can arrive at without running anything.
#   $1 corpus -> "programs pages absent"
corpus_expects() {
    cdir="$here/$1"
    progs=0; pgs=0; gone=0
    for cp in "$cdir"/*.ps "$cdir"/*.eps; do
        [ -f "$cp" ] || continue
        cb=$(basename "$cp" | sed 's/\.[Pp][Ss]$//;s/\.[Ee][Pp][Ss]$//')
        [ -f "$cdir/heldout" ] && grep -qxF "$cb" "$cdir/heldout" && continue
        [ "${SKIP_NONDET:-0}" != 0 ] && [ -f "$cdir/nondeterministic" ] &&
            grep -qxF "$cb" "$cdir/nondeterministic" && continue
        progs=$((progs + 1))
        cn=$(awk -v b="$cb" '$1 == b { print $2; exit }' "$cdir/pages" 2>/dev/null)
        case ${cn:-} in ''|*[!0-9]*) cn=0 ;; esac
        pgs=$((pgs + cn))
        [ -f "$cdir/nopage" ] || continue
        # a program declared to draw no page at all stands for every
        # page declared for it; one declared to lose a page stands for
        # that page
        if [ "$(awk -v b="$cb" '{ sub(/#.*/, "") } $1 == b && NF == 1 { c++ }
                                END { print c + 0 }' "$cdir/nopage")" != 0 ]; then
            gone=$((gone + cn))
        else
            gone=$((gone + $(awk -v b="$cb" '{ sub(/#.*/, "") }
                                             $1 == b && NF == 2 { c++ }
                                             END { print c + 0 }' "$cdir/nopage")))
        fi
    done
    echo "$progs $pgs $gone"
}

# and the report held to them. The evaluator's summary is the
# evaluator's account of itself: it agrees with the work the run did,
# and says nothing about the work the run was given. A run that named
# half the programs reports honestly on that half; a run that lost the
# tail of a program compares the pages it reached and reports every one
# of them as a match. So the numbers are worked out a second time, here,
# from the corpus rather than from the run, and a summary that does not
# meet them fails -- whichever of the two is wrong.
if [ -n "$corpus" ]; then set -- "$here/$corpus/"; else set -- "$here"/*/; fi
for d in "$@"; do
    name=$(basename "$d")
    seen=0
    for p in "$d"*.ps "$d"*.eps; do [ -f "$p" ] && { seen=1; break; }; done
    [ "$seen" = 1 ] || continue
    # a corpus waiting for a prelude it does not carry is passed over by
    # the evaluator, and there is nothing of it to hold to anything
    [ -f "$d"prelude.fetched ] && [ ! -s "$d"prelude ] && continue
    line=$(printf '%s\n' "$out" | grep "^$name: " | tail -n 1)
    if [ -z "$line" ]; then
        echo "corpus: the evaluator reported nothing for $name -- see above"
        exit 1
    fi
    said=$(printf '%s\n' "$line" | sed -n \
        's/^[^:]*: \([0-9][0-9]*\) programs evaluated.*, \([0-9][0-9]*\) pages declared, \([0-9][0-9]*\) compared, \([0-9][0-9]*\) absent$/\1 \2 \3 \4/p')
    if [ -z "$said" ]; then
        echo "corpus: $name reported no count of what it did -- see above"
        exit 1
    fi
    read -r sprogs sdecl scmp sabs <<SAID
$said
SAID
    read -r wprogs wpages wabsent <<WANT
$(corpus_expects "$name")
WANT
    if [ "$wprogs" = 0 ] || [ "$wpages" = 0 ]; then
        echo "corpus: $name holds no program with a page declared for it, so a run of it measures nothing"
        exit 1
    fi
    if [ "$sprogs" != "$wprogs" ] || [ "$sdecl" != "$wpages" ] ||
       [ "$sabs" != "$wabsent" ]; then
        echo "corpus: $name says it evaluated $sprogs programs over $sdecl declared pages with $sabs absent, and the corpus on disk comes to $wprogs, $wpages and $wabsent -- see above"
        exit 1
    fi
    if [ "$scmp" != $((sdecl - sabs)) ] || [ "$scmp" = 0 ]; then
        echo "corpus: $name compared $scmp pages of $sdecl declared, $sabs of which are declared to be drawn by nothing -- see above"
        exit 1
    fi
done
exit 0
