#!/bin/sh
# Render each corpus through xpost and through a reference engine and
# report the per-page difference. A corpus whose directory is absent or
# empty is skipped, as is one whose programs need a prelude that is
# populated rather than committed and has not been, so this is never a
# build dependency. The reference is an oracle and not an authority:
# read a difference as a lead, not a verdict (see README.md).
#
#   evaluate.sh                 evaluate every corpus present
#   evaluate.sh ghostscript     evaluate one
#   XPOST=/path/to/xpost evaluate.sh    use a specific build
#
# The corpora are meant to be evaluated at once: the build registers one
# test per corpus so that they overlap. So nothing written here may sit
# at a path a second run would arrive at as well. A run makes a working
# directory of its own under .work, each corpus takes a directory under
# that, and each program a numbered directory under that -- three levels
# because all three names repeat. Two corpora both number their programs
# from one, and a corpus evaluated twice at once is two runs of the same
# name; sharing any of it means one program's renders standing where
# another's are read, a list truncated while it is being walked, and a
# report that is neither run's.
#
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
XPOST=${XPOST:-"$root/build/src/bin/xpost"}
GS=${GS:-gs}
jobs=${CORPUS_JOBS:-$(nproc 2>/dev/null || echo 4)}

for tool in "$XPOST" "$GS"; do
    command -v "$tool" >/dev/null 2>&1 || [ -x "$tool" ] || {
        echo "evaluate: missing $tool -- skipping all" >&2; exit 0; }
done
command -v compare >/dev/null 2>&1 || {
    echo "evaluate: ImageMagick 'compare' not found -- skipping all" >&2; exit 0; }

# device and metric for one page, by corpus and file name. The Adobe
# halftone and pattern-screen pages are bilevel; everything else is
# colour. A Blue Book program is named for its number and its title, so
# the four bilevel ones are matched on the number and the hyphen that
# closes it: without the hyphen the pattern would have to end at the
# digit and match nothing, and with a bare star it would take a third
# digit as well.
device_for() {   # corpus base -> "ppm" | "pbm"
    case "$1/$2" in
        adobe/ht_*|adobe/bb_1[2-5]-*) echo pbm;;
        *) echo ppm;;
    esac
}

# Ink one engine put where the other put none.
#
# Two correct renderings of a page of text differ by thousands of pixels,
# because one engine anti-aliases its glyphs and the other does not: the
# edge of every stem is a differing pixel, and a count of differing
# pixels then says how much anti-aliasing there was rather than whether
# the page is right. Measured on a single line of forty-eight point
# Times, the two renderings trim to the same width at the same column and
# still differ by fifteen hundred pixels; the same two engines on a
# filled path differ by none. So a page that has moved a line of text and
# a page that has not report the same four-figure number, and the corpus
# cannot tell them apart.
#
# What no two renderings of the same page produce is ink with nothing
# near it. That is what this counts: a pixel one engine marked solidly
# with no mark of any strength from the other within two pixels of it,
# in both directions. Two pixels is the reach of an anti-aliased edge
# and of the half-pixel either engine may place a stem to either side
# of; anything that moved leaves its whole ink outside that reach twice
# over, once where it went and once where it is no longer. It is
# asymmetric on purpose -- solid ink against a mark of any strength --
# because the thin stems one engine renders at a fraction of full
# coverage are the same stems, and a rule reading both sides at half
# intensity counts them as absent.
#
#   $1 the reference page  $2 this interpreter's page
displaced() {
    one() {   # solid  anymark  -> pixels of the first with none of the second near
        convert \( "$1" -colorspace Gray -threshold 50% -negate \) \
                \( "$2" -colorspace Gray -threshold 98% -negate \
                   -morphology Dilate Square:2 -negate \) \
                -compose Multiply -composite \
                -format '%[fx:mean*w*h]' info: 2>/dev/null
    }
    printf '%s %s\n' "$(one "$2" "$1")" "$(one "$1" "$2")" \
    | awk '{ printf "%.0f\n", $1 + $2 }'
}

# The displacement a page is reported for. Below it a difference is the
# two rasterisers disagreeing about the edge of a glyph, which they do a
# pixel at a time and in single figures over a page; at it or above, the
# ink of something the size of a character of body text has moved. A
# character of ten point text at seventy-two dots to the inch marks
# about thirty pixels, and a character that moved is counted twice --
# where it went and where it no longer is -- so thirty pixels of
# displacement is half a character and less than anything this corpus
# draws.
DISPLACED_FLOOR=30

# One program, rendered by both engines in a directory of its own so that
# any number of these may run at once.
#   $1 corpus  $2 base name  $3 path to the program  $4 work directory
#   $5 the pages the corpus declares it draws, or "-" where it declares none
evaluate_one() {
    corpus=$1
    b=$2
    p=$3
    work=$4
    want=$5
    [ "$want" = - ] && want=0
    mkdir -p "$work" || return
    # Beside the report, what this program produced no page for and how
    # many pages it did get compared. The reader below holds both
    # against what the corpus declares, and it reads these rather than
    # the report, so that the wording of a line is not also a protocol.
    : > "$work.miss"
    : > "$work.disp"
    echo 0 > "$work.cmp"
    (
        dev=$(device_for "$corpus" "$b")
        gsdev=${dev}raw
        rm -f "$work"/g_*.* "$work"/x_*.*
        # an optional compatibility prelude, prepended to both engines so
        # the input stays identical; used where a corpus assumes operators
        # outside the language the reference provides as extensions
        src="$p"
        if [ -f "$here/$corpus/prelude" ]; then
            cat "$here/$corpus/prelude" "$p" > "$work/src.ps"
            src="$work/src.ps"
        fi
        # The two engines have nothing to say to each other: different
        # programs over different output names in a directory that is
        # this program's alone, and nothing is read until both have
        # stopped. So the reference is started in the background and
        # this interpreter runs beside it, and the program costs the
        # slower of the two rather than the sum -- which on the longest
        # program of the suite is the whole of the difference between
        # them, and that program is what the suite's wall clock is.
        #
        # Both exit statuses have to survive that, because the verdicts
        # below are read off them and a status not captured at the
        # moment it is available is gone. So exactly one engine is
        # backgrounded and the other is waited for in the foreground:
        # the foreground status is taken from $? on the line after the
        # command, before anything else can overwrite it, and the
        # background status from "wait" on that one process id, which
        # answers for the process named and not for whichever finished
        # first. Backgrounding both and waiting for the pair would lose
        # them both -- a bare "wait" reports on nothing.
        "$GS" -q -sDEVICE=$gsdev -sPAPERSIZE=letter -r72 -dNOSAFER \
              -dBATCH -dNOPAUSE -o "$work/g_%d.$dev" "$src" \
              </dev/null >/dev/null 2>&1 &
        gspid=$!
        # The budget separates a program that is slow from one that will
        # never finish, and it is spent on a machine this evaluator is
        # itself loading: every corpus renders its programs several at a
        # time and several corpora run at once, so a program can be
        # sharing a core with a handful of its own kind before the rest
        # of the suite is counted. The longest program here takes about
        # a minute alone, seventy-five seconds with the whole suite
        # beside it, and a hundred and sixty on a machine already busy
        # with other work -- and it has still been killed at four
        # minutes on a busier one than any of those. A gate that fails
        # for the machine's reasons rather than the renderer's teaches
        # its reader to discount it, so the budget is four times the
        # point at which that happened.
        timeout 960 "$XPOST" -d $dev -o "$work/x_%d.$dev" "$src" \
                </dev/null >"$work/xlog" 2>&1
        xstatus=$?
        # and the reference collected before any of that is read. It
        # comes first because everything below this line either reads
        # what the two engines wrote or leaves, and the directory is
        # taken away on the way out: a verdict reached while the
        # reference was still running would be reached over half its
        # output, and an exit would leave it drawing into a directory
        # about to be removed.
        wait "$gspid"
        gstatus=$?
        xerr=$(grep -m1 -oE 'Error: [a-zA-Z.]+' "$work/xlog" | sed 's/Error: //')
        # a signal death or a timeout is a hard regression, distinct from a
        # controlled PostScript error (which just yields no page, below)
        if [ "$xstatus" -ge 128 ]; then
            echo "  $b  XPOST CRASHED (signal $((xstatus - 128)))"; exit 0
        fi
        if [ "$xstatus" = 124 ]; then
            echo "  $b  XPOST TIMED OUT"; exit 0
        fi
        ng=$(ls "$work"/g_*.$dev 2>/dev/null | wc -l)
        nx=$(ls "$work"/x_*.$dev 2>/dev/null | wc -l)
        # A program either engine drew nothing for is compared not at
        # all, so a run of them answers none of what it was asked. Record
        # the absence under the program's name for the reader to hold
        # against what the corpus declares; which engine came up empty
        # is the reason for it, and reasons live in that file.
        if [ "$nx" = 0 ] || [ "$ng" = 0 ]; then
            printf '%s\n' "$b" >> "$work.miss"
            # what the reference exited with, where it drew nothing. An
            # engine that died has written no page to say so on, and
            # its status is the only account of it there is; a program
            # the reference merely declines to draw exits cleanly and
            # says nothing here, which is the difference worth seeing.
            gwhy=
            if [ "$gstatus" -ge 128 ]; then
                gwhy=" (reference died on signal $((gstatus - 128)))"
            elif [ "$gstatus" != 0 ]; then
                gwhy=" (reference exited $gstatus)"
            fi
            if [ "$nx" = 0 ] && [ "$ng" = 0 ]; then
                echo "  $b  no page from either engine$gwhy"
            elif [ "$nx" = 0 ]; then
                echo "  $b  XPOST FAILED${xerr:+: $xerr}"
            else
                echo "  $b  reference produced no page ($nx from xpost)$gwhy"
            fi
            exit 0
        fi
        i=1
        compared=0
        # The pages either engine drew, and the pages the corpus says
        # the program has. A bound taken from the reference never
        # reaches a page xpost drew and the reference did not, and a
        # page lost by the side the bound comes from shortens the walk
        # instead of appearing in it -- an absence that removes its own
        # evidence. A page neither engine drew removes it from both
        # sides at once, which no bound either of them supplies can
        # see, so the declared count is a bound as well.
        np=$ng
        [ "$nx" -gt "$np" ] && np=$nx
        [ "$want" -gt "$np" ] && np=$want
        while [ "$i" -le "$np" ]; do
            gp="$work/g_$i.$dev"; xp="$work/x_$i.$dev"
            # a program that stops partway leaves the pages after it
            # unwritten, and each of those is an absence of its own:
            # keyed by its page, so the corpus declares it by the page
            if [ ! -f "$xp" ] && [ ! -f "$gp" ]; then
                echo "  $b p$i  no page from either engine"
                printf '%s p%s\n' "$b" "$i" >> "$work.miss"
                i=$((i+1)); continue
            fi
            [ -f "$xp" ] || { echo "  $b p$i  no xpost page"
                              printf '%s p%s\n' "$b" "$i" >> "$work.miss"
                              i=$((i+1)); continue; }
            [ -f "$gp" ] || { echo "  $b p$i  no reference page"
                              printf '%s p%s\n' "$b" "$i" >> "$work.miss"
                              i=$((i+1)); continue; }
            # The two engines' pages, held to being pages of the same
            # thing before anything is read off them. A program that
            # selects its medium through a product dictionary gets a
            # different sheet from each engine, and the two renderings
            # then answer different questions: the comparison below
            # reads the overlap and reports a perfect match for two
            # pages that have nothing to do with each other, which is
            # the one difference a count of differing pixels cannot
            # see. Such a page is compared -- both engines drew it --
            # and its whole ink is displaced, so it is reported and
            # declared like any other displacement.
            gwh=$(identify -format '%wx%h' "$gp" 2>/dev/null)
            xwh=$(identify -format '%wx%h' "$xp" 2>/dev/null)
            if [ "$gwh" != "$xwh" ]; then
                printf "  %-16s p%-2s  MEDIA %s and %s\n" "$b" "$i" \
                       "${gwh:-?}" "${xwh:-?}"
                printf '%s p%s\n' "$b" "$i" >> "$work.disp"
                compared=$((compared + 1))
                i=$((i+1)); continue
            fi
            d=$(displaced "$gp" "$xp")
            case ${d:-} in ''|*[!0-9]*) d=0 ;; esac
            if [ "$dev" = pbm ]; then
                convert "$gp" -resize 12.5% "$work/a.png" 2>/dev/null
                convert "$xp" -resize 12.5% "$work/b.png" 2>/dev/null
                m=$(compare -metric RMSE "$work/a.png" "$work/b.png" null: 2>&1 \
                    | grep -oE '\([0-9.]+\)' | tr -d '()')
                printf "  %-16s p%-2s  tintRMSE %-11s displaced %s\n" \
                       "$b" "$i" "${m:-?}" "$d"
            else
                m=$(compare -metric AE -fuzz 5% "$gp" "$xp" null: 2>&1 | grep -oE '^[0-9]+')
                printf "  %-16s p%-2s  AE %-11s displaced %s\n" \
                       "$b" "$i" "${m:-?}" "$d"
            fi
            [ "$d" -ge "$DISPLACED_FLOOR" ] && printf '%s p%s\n' "$b" "$i" >> "$work.disp"
            compared=$((compared + 1))
            i=$((i+1))
        done
        printf '%s\n' "$compared" > "$work.cmp"
    ) > "$work.out"
    rm -rf "$work"
}

evaluate_corpus() {
    corpus=$1
    dir="$here/$corpus"
    set -- "$dir"/*.ps "$dir"/*.eps
    have=0
    for p in "$@"; do [ -f "$p" ] && have=1; done
    if [ "$have" = 0 ]; then
        echo "$corpus: absent -- skipped (fetch.sh $corpus)"
        return
    fi
    # A corpus whose programs assume a prelude cannot run without one:
    # with nothing prepended, every program of it fails and the run
    # compares no page at all. Where the prelude is committed that is a
    # broken tree, and the programs failing is the report of it. Where
    # the prelude is populated alongside the programs -- generated and
    # large, and so kept out of the tree as they are -- its absence
    # means only that the corpus is half fetched, which is a skip. The
    # two cases look alike from the missing file, so the corpus says
    # which it is: a committed "prelude.fetched" beside the prelude
    # declares that the prelude is populated rather than committed, and
    # says how to obtain it. A corpus without that file is one whose
    # prelude is part of the tree, and its absence is not skipped over.
    if [ -f "$dir/prelude.fetched" ] && [ ! -s "$dir/prelude" ]; then
        echo "$corpus: prelude absent -- skipped ($corpus/prelude is not committed)"
        grep -v '^[[:space:]]*#' "$dir/prelude.fetched" \
            | grep -v '^[[:space:]]*$' | sed 's/^/  /'
        return
    fi
    echo "=== $corpus"
    cwork="$work/$corpus"
    mkdir -p "$cwork" || return

    # Name the programs to render, in order, and hold out the ones the
    # corpus lists.
    n=0
    held=0
    uncounted=0
    nondet=
    : > "$cwork/list"
    : > "$cwork/all"
    for p in "$@"; do
        [ -f "$p" ] || continue
        b=$(basename "$p" | sed 's/\.[Pp][Ss]$//;s/\.[Ee][Pp][Ss]$//')
        printf '%s\n' "$b" >> "$cwork/all"
        # a corpus may list basenames (one per line) in a "heldout" file:
        # the programs it holds out of the run, each recorded there with
        # the reason it is held. The reason is the entry's whole value --
        # a name in this list is a program nothing measures again until
        # someone reads why it is there -- so the file carries it and
        # this only reads the names
        if [ -f "$dir/heldout" ] && grep -qxF "$b" "$dir/heldout"; then
            echo "  $b  held out (see $corpus/heldout)"
            held=$((held + 1))
            continue
        fi
        # a corpus may also list basenames in a "nondeterministic" file:
        # programs whose output is not a function of this tree, because
        # the program draws from something outside it -- the clock, the
        # execution it has had -- so two runs of the same build may
        # differ and a difference against anything says nothing about
        # the renderer. The entry excuses the program from being read
        # that way; it does not predict that any two runs will in fact
        # differ, which is a property of the machine rather than of the
        # tree and is not a thing this evaluator can measure. What
        # stands behind the entry is the reason written beside it, held
        # to below. They are evaluated and labelled by default, since
        # the rest of what they exercise is still worth running;
        # SKIP_NONDET=1 holds them out for a comparison that needs every
        # difference to mean something.
        if [ -f "$dir/nondeterministic" ] && grep -qxF "$b" "$dir/nondeterministic"; then
            if [ "${SKIP_NONDET:-0}" != 0 ]; then
                echo "  $b  held out (see $corpus/nondeterministic)"
                held=$((held + 1))
                continue
            fi
            nondet="$nondet $b"
        fi
        # and how many pages the corpus says it draws, from a "pages"
        # file of basenames and counts. The run compares the pages both
        # engines drew and can see no further than the further of them,
        # so a page neither drew is one the run does not reach and does
        # not report: a program that stops emitting halfway through
        # matches on every page it did draw. The count is the only
        # thing that says how many there were. A program without one is
        # a program whose pages nothing bounds, so it is rendered and
        # reported but the corpus fails for it.
        want=$(awk -v b="$b" '$1 == b { print $2; exit }' "$dir/pages" 2>/dev/null)
        case ${want:-} in
            ''|*[!0-9]*)
                echo "  $b  no page count in $corpus/pages"
                uncounted=$((uncounted + 1))
                want=- ;;
        esac
        n=$((n + 1))
        printf '%s\n%s\n%s\n%s\n%s\n' "$corpus" "$b" "$p" "$cwork/$n" "$want" \
            >> "$cwork/list"
    done
    [ "$n" = 0 ] && return

    # Render them concurrently -- each engine run is a separate process over
    # its own directory -- then report in the order they were named, so the
    # output does not depend on which finished first. The list is what said
    # which programs those were, so walking it again is what reports them,
    # and a program whose report is not there is named as one rather than
    # passed over: a run that evaluates a fraction of a corpus and says
    # nothing about the rest agrees with whatever the rest would have said.
    xargs -P "$jobs" -n5 "$0" --one < "$cwork/list" >/dev/null 2>&1
    seen=0
    pages=0
    declared=0
    absent=0
    miscount=0
    : > "$cwork/missing"
    : > "$cwork/displaced"
    : > "$cwork/ran"
    while read -r c && read -r b && read -r p && read -r d && read -r want; do
        if [ -s "$d.out" ]; then
            cat "$d.out"
            [ -f "$d.miss" ] && cat "$d.miss" >> "$cwork/missing"
            [ -f "$d.disp" ] && cat "$d.disp" >> "$cwork/displaced"
            got=0
            [ -s "$d.cmp" ] && got=$(cat "$d.cmp")
            pages=$((pages + got))
            printf '%s\n' "$b" >> "$cwork/ran"
            # Every page the corpus declares, either compared or gone:
            # a program that drew nothing is gone whole, one that
            # stopped partway is gone by the page, and the two together
            # have to come to the count. They fall short when a page
            # went missing from both engines at once, which is the one
            # absence neither engine's own output can show; they exceed
            # it when the program drew a page the count does not know
            # about, which is either a defect fixed and not written
            # down or a page arriving twice.
            if [ "$want" != - ]; then
                # this program's absences and no other's: the file
                # holds either the one key that is the whole program,
                # standing for all of its pages, or a key for each page
                # that went missing
                gone=0
                if [ -s "$d.miss" ]; then
                    if grep -qxF "$b" "$d.miss"; then
                        gone=$want
                    else
                        gone=$(wc -l < "$d.miss")
                    fi
                fi
                declared=$((declared + want))
                absent=$((absent + gone))
                if [ $((got + gone)) != "$want" ]; then
                    echo "  $b  $got pages compared and $gone missing, of the $want $corpus/pages declares"
                    miscount=$((miscount + 1))
                fi
            fi
            # a program whose output is not a function of this tree
            # differs from anything, so say so beside its numbers rather
            # than leaving them to be read as the renderer's doing
            case " $nondet " in
                *" $b "*) echo "  $b  nondeterministic: its output is not this tree's alone (see $corpus/nondeterministic)";;
            esac
            seen=$((seen + 1))
        else
            echo "  $b  NOT EVALUATED (no report)"
        fi
    done < "$cwork/list"

    # What produced no page, against what the corpus says produces none.
    # A program neither engine drew is compared not at all, and so is a
    # page of one that stopped before reaching it; either way the run
    # answers nothing about it, so the corpus has to have said so first,
    # in a "nopage" file that names it and records why. An entry is a
    # basename for a whole program, or a basename and " pN" for one page
    # of it -- the keys the evaluator wrote above.
    #
    # The comparison runs both ways, because both directions are news. An
    # absence nobody declared is a run comparing less than it was asked
    # to and reporting as though it had: the count of programs evaluated
    # is honest and says nothing about whether any of them drew, so a
    # corpus in which everything failed reads exactly like one in which
    # everything worked. A declared absence that turns out to have
    # rendered is the other way round -- the entry's reason has lapsed,
    # and the line is now telling its next reader something untrue.
    : > "$cwork/declared"
    if [ -f "$dir/nopage" ]; then
        sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
            "$dir/nopage" | grep -v '^$' > "$cwork/declared"
    fi
    undeclared=0
    lapsed=0
    while read -r u; do
        grep -qxF "$u" "$cwork/declared" && continue
        echo "  $u  no page, and $corpus/nopage does not say it makes none"
        undeclared=$((undeclared + 1))
    done < "$cwork/missing"
    while read -r u; do
        # an entry for a program this run did not render -- held out, or
        # not fetched -- is not one this run can speak to either way
        grep -qxF "${u%% *}" "$cwork/ran" || continue
        grep -qxF "$u" "$cwork/missing" && continue
        echo "  $u  declared in $corpus/nopage as making no page, but it rendered"
        lapsed=$((lapsed + 1))
    done < "$cwork/declared"

    # What displaced ink, against what the corpus says displaces some.
    # Anti-aliasing accounts for thousands of differing pixels on a page
    # of text and for none of these, so a page here differs by something
    # that moved, or was drawn in a place the other engine drew nothing,
    # or was not the same page at all. Some of those are this
    # interpreter's to close and some are not -- a font neither engine
    # has, substituted differently by each; a medium selected through a
    # dictionary only one of them carries -- and the ones that are not
    # are written down with the reason, in a "displaced" file keyed as
    # nopage is: a basename and " pN".
    #
    # Both directions are news, as they are for nopage. An undeclared
    # displacement is a difference nobody has looked at, sitting in the
    # log beside the ones that have been; a declared one that no longer
    # displaces is a reason that has lapsed, and a file saying something
    # untrue about the corpus is read as a known cost and stops the next
    # reader looking.
    #
    # A corpus is held to this once it carries the file. Until it does,
    # the pages that displace ink are counted and said, at the end of
    # the run and in the summary line, and nothing fails: a corpus whose
    # displacements have not been read through yet is work outstanding
    # rather than a tree that is broken, and turning it red would only
    # teach its reader to pass over the line. What the count is for is
    # to make the outstanding work a number somebody can see rather than
    # an absence nobody meets.
    : > "$cwork/dispdeclared"
    undisplaced=0
    unmoved=0
    unheld=0
    if [ -f "$dir/displaced" ]; then
        sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
            "$dir/displaced" | grep -v '^$' > "$cwork/dispdeclared"
        while read -r u; do
            grep -qxF "$u" "$cwork/dispdeclared" && continue
            echo "  $u  displaced ink, and $corpus/displaced does not say why"
            undisplaced=$((undisplaced + 1))
        done < "$cwork/displaced"
        while read -r u; do
            grep -qxF "${u%% *}" "$cwork/ran" || continue
            grep -qxF "$u" "$cwork/displaced" && continue
            echo "  $u  declared in $corpus/displaced as displacing ink, but it does not"
            unmoved=$((unmoved + 1))
        done < "$cwork/dispdeclared"
    else
        unheld=$(grep -c . "$cwork/displaced" 2>/dev/null || echo 0)
        case ${unheld:-} in ''|*[!0-9]*) unheld=0 ;; esac
        if [ "$unheld" != 0 ]; then
            echo "  $unheld pages displaced ink and there is no $corpus/displaced to say why"
        fi
    fi

    # The registers, against the corpus they describe. An entry naming a
    # program that is not there excuses nothing and measures nothing,
    # and the reason written beside it is read as a known cost by
    # whoever finds it: a name that has outlived its program keeps a
    # question closed that nothing is asking any more.
    stale=0
    for reg in heldout nondeterministic pages displaced; do
        [ -f "$dir/$reg" ] || continue
        while read -r u; do
            case $u in ''|'#'*) continue ;; esac
            grep -qxF "${u%% *}" "$cwork/all" && continue
            echo "  ${u%% *}  named in $corpus/$reg and not in the corpus"
            stale=$((stale + 1))
        done < "$dir/$reg"
    done

    # and the nondeterminism register to the reason each entry stands
    # on. Nothing measures a declaration of nondeterminism: it says what
    # a difference may be read as, not what any two runs will be, and a
    # program that draws on the clock is free to give the same answer
    # twice. So the reason is the entry's whole value -- it is what says
    # where the program's output comes from, and so what a later reader
    # holds against the tree to see whether the entry still earns its
    # place. A name with no reason beside it excuses every difference
    # that program ever shows and says nothing about why, which is the
    # one thing the entry was for. The reason is a comment naming the
    # program, as in the other registers.
    noreason=0
    if [ -f "$dir/nondeterministic" ]; then
        while read -r u rest; do
            case $u in ''|'#'*) continue ;; esac
            awk -v b="$u" '($1 == "#" && $2 == b) || $1 == "#" b { f = 1 }
                           END { exit !f }' "$dir/nondeterministic" && continue
            echo "  $u  named in $corpus/nondeterministic with no reason beside it"
            noreason=$((noreason + 1))
        done < "$dir/nondeterministic"
    fi

    # and the page counts as declarations in their own right. A count
    # that is not a number is a program declared to draw none of its
    # pages, and a program counted twice is two answers of which a
    # reader takes whichever it meets first; either reads as a
    # declaration and holds nothing.
    malformed=0
    if [ -f "$dir/pages" ]; then
        while read -r u v rest; do
            case $u in ''|'#'*) continue ;; esac
            case ${v:-} in
                ''|*[!0-9]*)
                    echo "  $u  has no page count in $corpus/pages"
                    malformed=$((malformed + 1)); continue ;;
            esac
            [ -z "$rest" ] || {
                echo "  $u  has more than a page count in $corpus/pages"
                malformed=$((malformed + 1)); }
        done < "$dir/pages"
        for u in $(grep -v '^[[:space:]]*#' "$dir/pages" \
                   | awk 'NF { print $1 }' | sort | uniq -d); do
            echo "  $u  counted more than once in $corpus/pages"
            malformed=$((malformed + 1))
        done
    fi

    note=
    [ "$held" = 0 ] || note=", $held held out"
    [ -z "$nondet" ] || note="$note, nondeterministic:$nondet"
    # The count of programs is what was reached; the counts of pages are
    # what the corpus said there was to compare, what was compared, and
    # what was not. A run that reached everything and drew nothing
    # cannot inflate the second, and cannot leave the third at zero.
    # They are printed in a fixed order at the end of the line and
    # arrive at the same three numbers the corpus on disk gives, so a
    # reader outside this script can work them out for itself and hold
    # this line to them.
    [ "$unheld" = 0 ] || note="$note, $unheld displacing and unheld"
    note="$note, $declared pages declared, $pages compared, $absent absent"
    if [ "$seen" != "$n" ]; then
        echo "$corpus: NOT EVALUATED -- $seen of $n programs reported$note"
    elif [ "$stale" != 0 ]; then
        echo "$corpus: REGISTER NAMES NOTHING -- $stale entries name no program of this corpus; $n programs evaluated$note"
    elif [ "$noreason" != 0 ]; then
        echo "$corpus: REGISTER GIVES NO REASON -- $noreason entries of $corpus/nondeterministic have none; $n programs evaluated$note"
    elif [ "$malformed" != 0 ]; then
        echo "$corpus: REGISTER MALFORMED -- $malformed entries of $corpus/pages declare no count; $n programs evaluated$note"
    elif [ "$uncounted" != 0 ]; then
        echo "$corpus: PAGES NOT DECLARED -- $uncounted programs have no count in $corpus/pages; $n programs evaluated$note"
    elif [ "$undeclared" != 0 ] || [ "$lapsed" != 0 ]; then
        echo "$corpus: NO-PAGE SET DIFFERS -- $undeclared undeclared, $lapsed lapsed; $n programs evaluated$note"
    elif [ "$undisplaced" != 0 ] || [ "$unmoved" != 0 ]; then
        echo "$corpus: DISPLACED SET DIFFERS -- $undisplaced undeclared, $unmoved lapsed; $n programs evaluated$note"
    elif [ "$miscount" != 0 ]; then
        echo "$corpus: PAGE COUNT DIFFERS -- $miscount programs drew other than the pages declared for them; $n programs evaluated$note"
    else
        echo "$corpus: $n programs evaluated$note"
    fi
    rm -rf "$cwork"
}

# the per-program entry point xargs re-invokes this script through. It is
# told the directory to work in, so it makes none of its own and this has
# to come before the run's directory is made.
if [ "${1:-}" = "--one" ]; then
    shift
    evaluate_one "$@"
    exit 0
fi

mkdir -p "$here/.work" 2>/dev/null
work=$(mktemp -d "$here/.work/run.XXXXXX" 2>/dev/null) || work=
if [ -z "$work" ] || [ ! -d "$work" ] || [ ! -w "$work" ]; then
    echo "evaluate: could not make a working directory under $here/.work" >&2
    exit 1
fi
# and taken away however the run ends: a signal reaches the trap below,
# which exits, which reaches the one on EXIT.
trap 'rm -rf "$work"' EXIT
trap 'exit 1' INT TERM HUP

for name in ${*:-ghostscript casselman bwipp adobe}; do
    evaluate_corpus "$name"
done
