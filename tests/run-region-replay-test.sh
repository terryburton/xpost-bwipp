#!/bin/sh
# Meson test wrapper: a page painted region by region is the page painted
# whole.
#
# A record is played back for a run of rows, and a band loop is nothing
# but that done once per run: make a raster holding the run, play the
# marks that reach it, put the rows out, and go on to the next. So the
# claim this makes is the one such a loop rests on -- the page assembled
# from the runs is the page rendered whole, byte for byte -- against a
# directly painted page rather than against another replay.
#
# Four ways this passes without having established anything, and what is
# done about each:
#
#   The runs do not cover the page. Then the assembled page is not the
#   page, and what was compared was a different quantity. The runs are
#   read back from the run that played them, and are held to starting at
#   the first row, meeting end to end, and finishing at the last: a gap
#   and an overlap are named separately, because they are different
#   mistakes and each is a way for a band loop to be wrong.
#
#   Covering the page does not matter. A page assembled from runs that
#   leave rows out has to differ from the whole page over those rows and
#   nowhere else, so the second set of runs here leaves a run of rows to
#   nobody and the difference is required where it belongs.
#
#   The replay ignores the rows it was asked for. The pixels cannot tell:
#   a raster holding one run drops what lands outside it, so playing
#   every mark into every run paints the same page as playing the right
#   ones. What can tell is how many marks each run received, which the
#   run reports by playing the same rows into a device that counts. A run
#   must receive marks, and must receive fewer than the whole record
#   holds.
#
#   No mark crosses a boundary between runs. Then no run has an edge to
#   be wrong about. A mark reaching two runs is counted in both, so the
#   counts summed over the runs exceed the record's own count by exactly
#   the crossings; a rectangle, a line and a polygon are each required to
#   cross one.
#
#   $1  path to the built xpost binary
#   $2  path to region_replay_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

# a face answers for the text this run shows: a build without a face
# library cannot ask this wrapper's question, and says so rather than
# failing it
skip_if_faceless "$xpost" "this run shows text through a face"

# The runs below are started in the directory the pages are written to,
# so what they were handed has to name the same thing from there.
xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0

# The bands are written to the directory the run is started in, under the
# names the run reports, so the run and this agree on where they are.
render() {  # $1 device, $2 output name relative to $work; sets out
    out=$( cd "$work" && "$xpost" -q $ns -d "$1" -o "$2" "$script" </dev/null 2>&1 )
    st=$?
    verdict_run "$st" "$out" "the $1 run" || return 1
    if [ ! -s "$work/$2" ]; then
        echo "FAILURES: the $1 run produced no page"
        return 1
    fi
    return 0
}

# The painter by itself, asked for as the mode that holds the page whole:
# selecting a device by name selects the record in front of it, and the
# comparison here is between a region replay and a whole render.
render ppm:whole whole.ppm || fail=1
direct=$out
render record played.ppm || fail=1
played=$out

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a page could not be rendered"
    exit 1
fi

# The device that paints has no record to replay and must not be
# answering the branch that does: a page that reported bands through both
# devices would be reporting something other than the recorder's.
if printf '%s\n' "$direct" | grep -qE '^(BAND|COUNTS) '; then
    note "the directly painted run reported bands, so the comparison is" \
         "not between a region replay and a whole render"
fi

# ---- the page, and the shape of the file it comes out in ----
set -- $(printf '%s\n' "$played" | sed -n 's/^PAGE //p' | head -1)
width=${1:-}
height=${2:-}
if [ -z "$width" ] || [ -z "$height" ]; then
    echo "FAILURES: the record run did not say what page it painted, so"
    echo "      nothing here can be read out of the pages it wrote"
    exit 1
fi
rowbytes=$((3 * width))
pagebytes=$((rowbytes * height))
whole=$work/whole.ppm
hdr=$(( $(wc -c < "$whole") - pagebytes ))
if [ "$hdr" -le 0 ]; then
    echo "FAILURES: a ${width}x${height} page of three bytes a pixel does not fit"
    echo "      in the $(wc -c < "$whole") bytes the whole render wrote; the"
    echo "      page this reads and the page that was painted are not the same"
    exit 1
fi

# rows of a page, as bytes: <file> <first row> <how many>
rows_of() {
    tail -c +$((hdr + $2 * rowbytes + 1)) "$1" | head -c $(($3 * rowbytes))
}

# whether two pages carry the same bytes over a run of rows
same_rows() {  # <file> <file> <first row> <how many>
    rows_of "$1" "$3" "$4" > "$work/cmp-a"
    rows_of "$2" "$3" "$4" > "$work/cmp-b"
    cmp -s "$work/cmp-a" "$work/cmp-b"
}

# ---- what the record gave each run ----
counts=$(printf '%s\n' "$played" | sed -n 's/^COUNTS //p')
whole_counts=$(printf '%s\n' "$counts" | sed -n 's/^whole //p' | head -1)
if [ -z "$whole_counts" ]; then
    echo "FAILURES: the record run did not report what the whole record"
    echo "      holds, so nothing establishes that anything was recorded"
    exit 1
fi
set -- $whole_counts
if [ $# -ne 6 ]; then
    note "the whole record was reported as $# counts and this expects 6" \
         "(five marking kinds and the subpath separators)"
fi
wp=${1:-0} wb=${2:-0} wl=${3:-0} wr=${4:-0} wf=${5:-0} ws=${6:-0}
whole_total=$((wp + wb + wl + wr + wf))
zero=
n=0
for c in $wp $wb $wl $wr $wf $ws; do
    n=$((n + 1))
    [ "$c" -gt 0 ] || zero="$zero $n"
done
if [ -n "$zero" ]; then
    note "the page reached nothing at position(s)$zero of" \
         "putpix blendpix drawline fillrect fillpoly separators"
else
    echo "OK   recorded: $whole_counts"
fi

# Every run of rows, summed: a mark reaching two runs is counted in each,
# so a sum above the record's own count is a mark across a boundary.
sl=0; sr=0; sf=0; nrun=0
printf '%s\n' "$counts" | grep -v '^whole ' > "$work/runcounts"
while read -r label p b l r f s; do
    [ -n "$label" ] || continue
    nrun=$((nrun + 1))
    total=$((p + b + l + r + f))
    if [ "$total" -le 0 ]; then
        note "run $label received no mark at all, so the pixels it painted" \
             "are the pixels of an empty page"
    fi
    if [ "$total" -ge "$whole_total" ]; then
        note "run $label received $total of the record's $whole_total marks;" \
             "a replay that leaves nothing out is not honouring its rows"
    fi
    if [ $((p + b + l + f)) -le 0 ]; then
        note "run $label received nothing but the page clear, so no content" \
             "of the page reaches it"
    fi
    sl=$((sl + l)); sr=$((sr + r)); sf=$((sf + f))
    echo "OK   run $label received $total of $whole_total marks: $p $b $l $r $f $s"
done < "$work/runcounts"

if [ "$nrun" -lt 2 ]; then
    echo "FAILURES: $nrun run(s) of rows were counted; there is no boundary"
    echo "      between runs for a mark to cross"
    exit 1
fi
for k in "line:$sl:$wl" "rectangle:$sr:$wr" "polygon:$sf:$wf"; do
    kind=${k%%:*}; rest=${k#*:}; got=${rest%%:*}; held=${rest#*:}
    if [ "$got" -le "$held" ]; then
        note "the runs received $got $kind mark(s) between them where the" \
             "record holds $held: no $kind crosses a boundary, so no run has" \
             "an edge through one and the boundaries prove nothing"
    else
        echo "OK   $kind marks cross a boundary: $got played over $held recorded"
    fi
done

# ---- the pages the runs put out ----
printf '%s\n' "$played" | sed -n 's/^BAND //p' > "$work/bands"
if [ ! -s "$work/bands" ]; then
    echo "FAILURES: the record run put out no run of rows"
    exit 1
fi

# Assemble a page from one set of runs: each run's own rows out of the
# page it put out, in order, and the rows no run holds out of a page that
# does not hold them either -- which is where the ground shows, and is
# what a band loop leaves such a row as. Sets gaps to the runs of rows
# nobody held.
gaps_shown() {  # the runs of rows nobody held, as row numbers
    for g_one in $gaps; do
        g_top=${g_one%%:*}
        printf ' %s..%s' "$g_top" "$((g_top + ${g_one#*:} - 1))"
    done
}

assemble() {  # <set> <outfile>
    as_set=$1
    as_out=$2
    grep "^$as_set " "$work/bands" > "$work/set" || :
    if [ ! -s "$work/set" ]; then
        note "the record run put out no page for the $as_set set"
        return 1
    fi
    head -c "$hdr" "$whole" > "$as_out"
    next=0
    prev=
    gaps=
    as_bad=0
    while read -r label top rows file; do
        [ -n "$label" ] || continue
        if [ ! -s "$work/$file" ]; then
            note "the $as_set run of rows $top..$((top + rows - 1)) put out" \
                 "no page at $file"
            as_bad=1
            continue
        fi
        if [ "$top" -lt "$next" ]; then
            note "the $as_set runs overlap: rows $top..$((next - 1)) are in two" \
                 "of them, and a page assembled from them carries those rows" \
                 "twice"
            as_bad=1
        elif [ "$top" -gt "$next" ]; then
            gaps="$gaps $next:$((top - next))"
            rows_of "${prev:-$work/$file}" "$next" $((top - next)) >> "$as_out"
        fi
        rows_of "$work/$file" "$top" "$rows" >> "$as_out"
        prev=$work/$file
        next=$((top + rows))
    done < "$work/set"
    if [ "$next" -ne "$height" ]; then
        note "the $as_set runs reach row $next of a page of $height rows"
        as_bad=1
    fi
    return "$as_bad"
}

if assemble cover "$work/cover.ppm"; then
    if [ -n "$gaps" ]; then
        note "the cover runs leave rows$(gaps_shown) to no run, so they do not" \
             "cover the page they are being compared over"
    elif cmp -s "$whole" "$work/cover.ppm"; then
        echo "OK   the page assembled from its regions is the page painted whole"
    else
        note "a page assembled from its regions is not the page painted whole"
        cmp "$whole" "$work/cover.ppm" 2>&1 | sed 's/^/      /' | head -3
    fi
else
    note "the covering set of regions could not be assembled into a page"
fi

# A run put out the whole page rather than its own rows if its page is
# the whole page: then the comparison above was between a whole render
# and four of them.
while read -r label top rows file; do
    [ "$label" = cover ] || continue
    [ -s "$work/$file" ] || continue
    if cmp -s "$whole" "$work/$file"; then
        note "the run holding rows $top..$((top + rows - 1)) put out the whole" \
             "page, so it did not paint a region of it"
    fi
done < "$work/bands"

# ---- and the runs that do not cover the page ----
if assemble gap "$work/gap.ppm"; then
    if [ -z "$gaps" ]; then
        note "the second set of runs covers the page, so nothing here says" \
             "what leaving rows out would have done"
    else
        for g in $gaps; do
            gtop=${g%%:*}; grows=${g#*:}
            if same_rows "$whole" "$work/gap.ppm" "$gtop" "$grows"; then
                note "rows $gtop..$((gtop + grows - 1)) were left to no run and" \
                     "came out as the whole page has them, so the rows a run" \
                     "is given do not decide what it paints"
            else
                echo "OK   rows $gtop..$((gtop + grows - 1)) left to no run differ" \
                     "from the page painted whole"
            fi
            # and the rows either side of the gap, which are covered
            if [ "$gtop" -gt 0 ] && \
               ! same_rows "$whole" "$work/gap.ppm" 0 "$gtop"; then
                note "rows 0..$((gtop - 1)) are covered by a run and differ from" \
                     "the page painted whole, so leaving rows out reached rows" \
                     "that were not left out"
            fi
            gend=$((gtop + grows))
            if [ "$gend" -lt "$height" ] && \
               ! same_rows "$whole" "$work/gap.ppm" "$gend" $((height - gend)); then
                note "rows $gend..$((height - 1)) are covered by a run and differ" \
                     "from the page painted whole, so leaving rows out reached" \
                     "rows that were not left out"
            fi
        done
    fi
else
    note "the non-covering set of regions could not be assembled into a page"
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
