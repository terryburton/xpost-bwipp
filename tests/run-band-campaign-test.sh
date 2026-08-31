#!/bin/sh
# Meson test wrapper: the whole banding fleet, taken as a fleet.
#
# Every other test of this feature holds one part of it -- the loop
# through one class, the two classes whose emission is not raw rows, the
# two whose raster is a buffer of their own, what a record's lifetime is,
# what region a replay takes. Each was written beside the change it
# covers and each is right about its own part. What none of them asks is
# the question a reader of the feature asks: does all of it still work,
# everywhere, at once.
#
# So this drives every device that says its page may arrive in bands, at
# band heights chosen to fall on the seams, over pages that reach every
# kind of mark there is -- both through a record played into it, which is
# how a job asks for a band, and through the loop driven by hand, which
# is the only way to reach the one device no record can be played into.
# And it drives the devices that say their page may not, where the whole
# of the claim is that none of this happened to them.
#
# Two populations, two standards:
#
#   A device that bands must put out the page it puts out whole, byte
#   for byte. For the two whose output is an encoding the pixels those
#   bytes decode to are compared as well, since a file can differ from
#   another byte for byte while standing for the same picture, or agree
#   in length while standing for a different one. Both are claimed here
#   and neither implies the other.
#
#   A device that does not band must be unchanged. raster and bgr hand
#   their buffer to whoever embedded the interpreter and the page is
#   whole by that contract; the vector writers hold no raster at all.
#   For all five the right outcome is that nothing happened, which a
#   campaign testing only the banding half would not notice going wrong.
#
# And the trap this work keeps relearning: no comparison of pages can
# see a broken bound. A replay that ignored its row range would paint
# exactly the same pixels, because a mark played into rows a raster does
# not hold paints nothing. So this counts as well as compares -- how
# many marks each band was played (.recordplayed), how many times a
# recorded image was painted (.recordplays), how many rows reached the
# writer -- and the counts are what say the range is doing anything.
#
# An instrument that answered zero would make everything pass, so each
# has a control of its own: the run ends by breaking its own instruments
# and its own machinery on purpose and requiring itself to fail. Six
# defects, one per check that could go quiet (see --sabotage below).
#
#   $1  path to the built xpost binary
#   $2  path to band_campaign_test.ps
#
# and, for the run's own controls, which it invokes on itself:
#
#   --sabotage N $1 $2   run a reduced campaign with defect N built in,
#                        and answer as it finds it. The caller requires
#                        a failure; a sabotage that passed would mean
#                        the check it targets sees nothing.
set -u

sab=0
case ${1:-} in
    --sabotage) sab=$2; shift 2 ;;
esac
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

# Each device renders into a directory of its own, so a page names the
# same file whichever device wrote it, and the runs are started there.
xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

ns=$(sandbox_flag "$xpost")

verdict_workdir
fail=0
cells=0

# A cell of the matrix that is wrong is usually every cell of it, and a
# thousand lines of the same complaint hides the one that differs. What
# is printed is the head of the list and how long the list was.
spill() {  # $1 file
    [ -s "$1" ] || return 1
    head -18 "$1"
    s_n=$(grep -c '^FAILURES:' "$1" || true)
    [ "$s_n" -le 4 ] || echo "      ... $s_n complaints in all"
    fail=1
    return 0
}

# The band heights and page heights the program runs, restated here
# because the wrapper reads its results by them. The program is the one
# that decides; a disagreement shows up as a line the wrapper looked for
# and did not find, which is a failure rather than a silence.
HEIGHTS='96 100'
BANDS='1 3 7 8 13 16'
MPBANDS='1 7 16'
MEMW=1000
# The sweep of page heights the bound is read over, one set per meter.
# The interpreter's own count of virtual memory is exact and answers for
# this raster and nothing else, so a short sweep says everything there
# is to say about it. The peak resident size of a process is a
# measurement, and what it moves by on a busy machine is comparable with
# what the band route is allowed to grow by over a short sweep -- so the
# cells that meter answers for are read over a taller page, where what
# the whole-page route grows by, and hence the room the band route is
# given, is several times that movement. The section that uses these
# says what was measured.
MEMLO=1000
MEMMID=2500
MEMHI=4000
# The resident-size sweep starts higher than the virtual-memory one, and
# has to. What that meter reads is the whole process, so its floor is the
# interpreter itself -- the loaded language, the device, the arena a job
# begins from -- and a page is only visible above that floor. MEASURED at
# two thousand rows the two routes read 22056 and 22024 KiB, which is the
# floor twice over and says nothing about either; the tallest banded page
# and the shortest whole one were then separated by less than the noise
# between runs of the same binary, and which of them came out larger was
# decided by the machine's mood. Four thousand rows puts the whole-page
# route about ten mebibytes clear of the floor, which is the separation
# the check below was written to rely on.
RSSLO=4000
RSSMID=6000
RSSHI=8000

# $1 the device selection, $2 the directory, $3 what to call the log,
# $4.. the definitions the run reads its page and band height out of
render() {
    r_sel=$1; r_dir=$2; r_log=$3
    shift 3
    mkdir -p "$work/$r_dir" || return 1
    r_out=$( cd "$work/$r_dir" && "$xpost" -q $ns -d "$r_sel" -o /dev/null \
             "$@" "$script" </dev/null 2>&1 )
    r_st=$?
    printf '%s\n' "$r_out" >> "$work/$r_log.log"
    verdict_run "$r_st" "$r_out" "the $r_sel run" || return 1
    verdict_ok "$r_out" "the $r_sel run" || return 1
    return 0
}

# $1 log name, $2 field name; prints that field's lines
field() { sed -n "s/^$2 //p" "$work/$1.log" 2>/dev/null; }

# What a selection's page ends up on: how many colour values a mark
# carries there, what one row of its raster costs, and whether that
# raster is rows the interpreter can see or a buffer of the device's
# own. Asked of the run rather than worked out here from the device's
# name, because the device a record's page ends up on is the record's
# business and not the selection's spelling.
# Sets ask_nc, ask_row and ask_where.
askdev() {  # $1 selection
    mkdir -p "$work/ask"
    a_line=$( cd "$work/ask" && "$xpost" -q $ns -d "$(whole "$1")" \
              -o /dev/null \
              -DASK=1 "$script" </dev/null 2>&1 ) || return 1
    a_line=$(printf '%s\n' "$a_line" | sed -n 's/^DEV //p' | head -1)
    [ -n "${a_line:-}" ] || return 1
    ask_nc=$(printf '%s\n' "$a_line" | awk '{print $1}')
    ask_row=$(printf '%s\n' "$a_line" | awk '{print $2}')
    ask_where=$(printf '%s\n' "$a_line" | awk '{print $5}')
    case ${ask_where:-} in rows|buffer) ;; *) return 1 ;; esac
    case ${ask_row:-x} in ''|*[!0-9]*) return 1 ;; esac
    [ "$ask_row" -gt 0 ] || return 1
    return 0
}

# ---------------------------------------------------------------------
# The roster
#
# Which devices this build has, what each says about taking its page in
# bands, and which of them a record can be played into. The last is read
# by asking rather than by listing, so a device added to the record's
# roster joins this campaign the day it is added and one taken off it
# leaves; what is listed here is only what must be true of the answer.
# ---------------------------------------------------------------------
BANDERS=$DEVICE_FLEET_BANDS
# Every device that is not one of those: a mode asking for a page in
# bands has to be refused by each of them rather than answered with a
# page held whole under a word that said otherwise. Taking the roster
# less the banders, instead of naming a few of them, is what makes the
# refusal a rule about the fleet: a device added to the roster is asked
# without anyone remembering to add it here, and the ones easiest to
# leave out of a hand-written list -- the two that paint nothing, the
# alpha writer, the recorder -- are asked like the rest.
KEEPERS=
for c_dev in $DEVICE_FLEET_ALL; do
    case " $BANDERS " in
        *" $c_dev "*) continue ;;
    esac
    KEEPERS="$KEEPERS $c_dev"
done

# The selection that reaches one of these devices without the record
# that selecting it by name now puts in front of it. What this campaign
# compares is the two routes; without this both spellings would name the
# same route and every comparison would be a page against itself. A
# selection that already names a mode, "pgm:band" among them, is left
# as it is.
whole() {
    case " $BANDERS " in
        *" $1 "*) printf '%s:whole\n' "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

probe=$work/probe.ps
cat > "$probe" <<'EOF'
(\nDECL ) print DEVICE /BandedPage known { (yes) }{ (no) } ifelse print (\n) print
(\nMOVE ) print DEVICE /.moveband known { (yes) }{ (no) } ifelse print (\n) print
(\nPLAY ) print
DEVICE /.bandbytes 1 put
currentsystemparams /CurBandHeight get 0 gt { (yes) }{ (no) } ifelse print (\n) print
quit
EOF

# $1 device selection, $2 field; prints the answer, or nothing where the
# selection was refused
asks() {
    a_out=$("$xpost" -q $ns -d "$(whole "$1")" -o /dev/null "$probe" \
            </dev/null 2>&1) \
        || return 1
    printf '%s\n' "$a_out" | sed -n "s/^$2 //p" | head -1
}

present=''
targets=''
for d in $BANDERS; do
    got=$(asks "$d" DECL || true)
    if [ -z "${got:-}" ]; then
        echo "SKIP the $d device is not in this build"
        continue
    fi
    present="$present $d"
    if [ "$got" != yes ]; then
        note "$d does not say its page may arrive a band at a time, and" \
             "this campaign is about the devices that do; either the" \
             "declaration went or the roster here is out of date"
        continue
    fi
    if [ "$(asks "$d" MOVE || true)" != yes ]; then
        note "$d says its page may arrive a band at a time and brings no" \
             ".moveband, so a band loop has no way to move its raster from" \
             "one run of rows to the next"
        continue
    fi
    if asks "$d:band" PLAY >/dev/null 2>&1 &&
       [ "$(asks "$d:band" PLAY || true)" = yes ]; then
        targets="$targets $d"
        echo "OK   $d takes its page in bands, and a record plays into it"
    else
        echo "OK   $d takes its page in bands; no record plays into it, so" \
             "it is driven through the band loop by hand"
    fi
done

ndev=$(printf '%s\n' $present | wc -l)
ntgt=$(printf '%s\n' $targets | grep -c . || true)
if [ "$ndev" -lt 4 ]; then
    note "this build has $ndev of the devices that band; a campaign over" \
         "fewer than four of them is not a campaign over a fleet"
fi
if [ "$ntgt" -lt 2 ]; then
    note "a record can be played into $ntgt device(s) in this build; the" \
         "route a job actually takes to a band is then barely covered"
fi
echo "OK   the fleet: $(echo $present) band; $(echo $targets) take a record"

# A device that cannot take its page a band at a time is refused the
# mode that asks for one, rather than quietly answered with a whole page
# under a word that said otherwise. Both halves matter: a refusal that
# did not fire would leave a run believing it had asked for something,
# and one that fired for everything would take the mode away.
for d in $KEEPERS notadevice; do
    if asks "$d:band" PLAY >/dev/null 2>&1; then
        note "-d $d:band was accepted; that device takes no page in bands" \
             "and no record plays into it, so asking for one has to be" \
             "refused rather than answered with a page held whole"
    else
        echo "OK   -d $d:band is refused"
    fi
done
for d in $targets; do
    asks "$d:band" PLAY >/dev/null 2>&1 ||
        note "-d $d:band was refused although $d is on the record's roster"
done

# Discovery is what the roster is; it is not what the roster may be.
#
# Reading the record's targets off the interpreter is what let two
# devices join this campaign the day they became targets, and that is
# worth keeping. But a roster that is only observed says nothing when it
# shrinks: a device that quietly stopped being a target would drop to
# the loop driven by hand and the run would report that cheerfully. So
# the answer is discovered and then held to a rule -- every device that
# bands takes a record, except the ones named here with the reason they
# cannot, and each of those is expected to decline rather than merely
# seen to.
#
# Nothing is named, and that is the whole of it. The bilevel device was
# named here until the record learned to carry the screen: what it
# stores for a grey is that grey against the cell under the pixel, so a
# mark depends on the screen in force when it was made, and a replay
# happens once the page is put out. The record now writes a screen down
# where it changes and puts it back as a replay passes it, so every
# device that bands takes a record (doc/xpost_design.dox). A device that
# cannot in future is named here, with the reason it cannot.
NORECORD=''
for d in $present; do
    isrec=no
    case " $targets " in *" $d "*) isrec=yes ;; esac
    wants=yes
    case " $NORECORD " in *" $d "*) wants=no ;; esac
    if [ "$isrec" = "$wants" ]; then
        if [ "$wants" = no ]; then
            echo "OK   $d bands and declines a record, which is what" \
                 "NORECORD says is expected of it"
        fi
        continue
    fi
    if [ "$wants" = no ]; then
        note "a record is now played into $d, which is named above as a" \
             "device expected to decline. Either the record now carries" \
             "the state that stopped it -- say so in doc/xpost_design.dox and" \
             "take $d out of NORECORD -- or the roster gained an entry it" \
             "cannot honour"
    else
        note "$d says its page may arrive a band at a time and no record" \
             "can be played into it. Every banding device but the ones" \
             "named in NORECORD is a target, so either $d has lost the" \
             "route a job actually takes to a band, or it has joined the" \
             "exceptions and nothing here says why"
    fi
done

# ---------------------------------------------------------------------
# The devices that must not band
#
# What a band loop puts about is a run of rows in graphicsdict, which
# Create takes, and a dictionary on the device for what an emission
# keeps between bands. The page these five put out with both present has
# to be the page they put out with neither. The page is read back
# through the device's own GetPix, because two of them write no file at
# all -- the caller they hand their buffer to is the file -- and the
# file is compared as well where there is one.
#
# The check has a control of its own, and it is the fleet above: a
# device that does honour that state puts out a different page, so the
# same comparison over a banding device has to fail. Without it, a check
# that compared nothing would pass on all five.
# ---------------------------------------------------------------------
if [ "$sab" -eq 0 ]; then
    nbcheck() {  # $1 device; prints "same" or "differ", or nothing
        rm -rf "$work/nb-$1"
        mkdir -p "$work/nb-$1"
        for n in 1 2; do
            # the device itself, since what is put about here is the
            # band loop's own state and what is asked is whether the
            # device notices it
            ( cd "$work/nb-$1" && "$xpost" -q $ns -d "$(whole "$1")" \
              -o /dev/null -DNOBAND=$n "$script" </dev/null \
              >"n$n.log" 2>&1 ) || return 1
            mv "$work/nb-$1/noband.out" "$work/nb-$1/page$n.out" 2>/dev/null
        done
        n_a=$(sed -n 's/^NB //p' "$work/nb-$1/n1.log" | head -1)
        n_b=$(sed -n 's/^NB //p' "$work/nb-$1/n2.log" | head -1)
        [ -n "${n_a:-}" ] && [ -n "${n_b:-}" ] || return 1
        # the page as the device answers for it, and the file if it wrote one
        set -- $n_a; a_sum=$2; a_ink=$3
        set -- $n_b; b_sum=$2; b_ink=$3
        if [ "$a_sum" != "$b_sum" ] || [ "$a_ink" != "$b_ink" ]; then
            echo differ
            return 0
        fi
        if [ -f "$work/nb-$1/page1.out" ] && [ -f "$work/nb-$1/page2.out" ] &&
           ! cmp -s "$work/nb-$1/page1.out" "$work/nb-$1/page2.out"; then
            echo differ
            return 0
        fi
        echo same
    }

    # Every device the mode is refused for, less the recorder. The
    # refusal above is a rule about all of them; this is a narrower
    # question -- whether a device notices state a band loop leaves
    # about -- and the recorder is the one device it cannot be asked of,
    # because holding a page as bands is what a recorder is for and it
    # says so.
    for d in $KEEPERS; do
        # The recorder is out because holding a page as bands is what a
        # recorder is for, and it says so.
        [ "$d" = record ] && continue
        # And so is any device that assembles a page of its own pixels in
        # compiled code and leaves it at the output path: that assembly
        # reads the band state directly, so setting the state about --
        # which is what this does, rather than asking for the mode, which
        # is refused above -- reaches it whether or not the device was
        # ever routed for a band. What is left is the devices with no
        # such assembly to reach, which is the question this asks.
        c_skip=no
        for c_w in $DEVICE_FLEET_BUFFER; do
            case " $DEVICE_FLEET_NOFILE " in *" $c_w "*) continue ;; esac
            [ "$d" = "$c_w" ] && c_skip=yes
        done
        [ "$c_skip" = yes ] && continue
        if [ -z "$(asks "$d" DECL || true)" ]; then
            echo "SKIP the $d device is not in this build"
            continue
        fi
        if [ "$(asks "$d" DECL || true)" != no ]; then
            note "$d says its page may arrive a band at a time, and the" \
                 "mode that asks for one is refused for it; a device that" \
                 "holds no raster a band could bound, or hands the one it" \
                 "holds to whoever embedded the interpreter, has nothing to" \
                 "declare there"
        fi
        got=$(nbcheck "$d" || true)
        case ${got:-} in
            same)
                cells=$((cells + 1))
                echo "OK   $d puts out the same page with a band loop's state" \
                     "about as without it" ;;
            differ)
                note "$d put out a different page with the state a band loop" \
                     "leaves about than without it; a device that must not" \
                     "band took some of it" ;;
            *)  note "$d could not be put through the unchanged check" ;;
        esac
    done

    # ... and the control for it: a device that does band must answer
    # differently, or the comparison above is comparing nothing.
    ctl=$(printf '%s\n' $present | head -1)
    if [ -n "${ctl:-}" ]; then
        got=$(nbcheck "$ctl" || true)
        if [ "${got:-}" = differ ]; then
            echo "OK   $ctl, which does band, answers differently to the same" \
                 "comparison, so it is comparing something"
        else
            note "$ctl bands and answered the same page with a band loop's" \
                 "state about as without it, so the comparison that passed" \
                 "for the five devices above passes on anything"
        fi
    fi
fi

# ---------------------------------------------------------------------
# The matrix
#
# Every device present, both ways it can be driven, over two pages and
# two page heights and six band heights.
# ---------------------------------------------------------------------
# A sabotage runs one device over one page: what is being asked is
# whether a check fires, and it fires on the first cell it can. Which
# route that cell is on is decided by where the defect was built --
# what a record played is a property of the record route, and what a
# raster is standing on is one of the loop driven by hand.
wantdir=yes
wantrec=yes
if [ "$sab" -eq 0 ]; then
    matrixdevs=$present
    matrixpages='1 2'
elif [ "$sab" -eq 6 ]; then
    matrixdevs=$(printf '%s\n' $present | head -1)
    matrixpages=1
    wantrec=no
else
    matrixdevs=$(printf '%s\n' $targets | head -1)
    matrixpages=1
    wantdir=no
fi

sabarg=''
[ "$sab" -eq 0 ] || sabarg="-DSAB=$sab"

for d in $matrixdevs; do
    isrec=no
    case " $targets " in *" $d "*) isrec=yes ;; esac
    # Whether this device's output is an encoding, which is what decides
    # whether the pixels are compared beside the bytes. That is a
    # property of the format and is named rather than derived: the
    # decode is chosen by the file's own first byte, and a portable-map
    # page put through it would not be a page that decoded differently
    # but a page with no decoder at all.
    encoded=no
    case $d in png|pngalpha|jpeg) encoded=yes ;; esac
    # ... and whether it keeps its raster in a buffer of its own, which
    # is asked of the device rather than named, because it is the same
    # question the meter below turns on.
    buffered=no
    if askdev "$d"; then
        [ "$ask_where" = buffer ] && buffered=yes
    else
        note "$d did not say where its raster lives"
    fi
    for p in $matrixpages; do
        rm -f "$work/$d.log" "$work/$d-rec.log"
        # An image reaches a device that keeps no rows as a rectangle per
        # sample, and the loop driven by hand offers every mark again for
        # every band, so the page of images through such a device costs
        # the samples times the bands. It runs the reduced sweep there
        # and the full one everywhere else -- both routes of it, so that
        # the two write the same set of pages and every one of them has
        # its counterpart to be compared against. A band of one row over
        # that page and those devices is what
        # tests/run-band-writer-test.sh already puts them through.
        bset=''
        [ "$buffered" = yes ] && [ "$p" = 2 ] && bset='-DBSET=1'
        if [ "$wantdir" = yes ]; then
            render "$(whole "$d")" "$d" "$d" -DPAGE=$p $bset $sabarg || {
                note "$d could not put out page $p"
                continue
            }
        fi
        if [ "$isrec" = yes ] && [ "$wantrec" = yes ]; then
            render "$d:band" "$d" "$d-rec" -DPAGE=$p $bset $sabarg || {
                note "a record played into $d could not put out page $p"
                continue
            }
        fi

        # ---- the bytes ----
        # Three files have to agree: the page assembled from bands, the
        # page the same route puts out whole, and the page a device that
        # never had a record paints as it goes.
        for tg in dir rec; do
            lg=$d; [ "$tg" = rec ] && lg=$d-rec
            [ -f "$work/$lg.log" ] || continue
            field "$lg" BAND | while read -r bp bh bb bfile nb sum mx mn \
                                            calls rows outside plays; do
                [ -n "${bfile:-}" ] || continue
                b=$work/$d/$bfile
                w=$work/$d/whole-$bp-$bh-$tg.out
                r=$work/$d/whole-$bp-$bh-dir.out
                # What a run of rows the device is not standing on
                # answers is what it answered before that run was
                # painted. Asked first, and not after the bytes, because
                # a raster standing on more than its run is caught by
                # both and the reader wants the nearer statement of it.
                if [ "$outside" -ne 0 ]; then
                    echo "FAILURES: $d changed what $outside row(s) outside"
                    echo "      the run it stands on answer, at $bb rows a"
                    echo "      band on page $bp of $bh rows; the raster is"
                    echo "      the page rather than the band"
                    continue
                fi
                if [ ! -s "$b" ]; then
                    echo "FAILURES: $d put out nothing for page $bp of $bh"
                    echo "      rows in bands of $bb ($tg)"
                    continue
                fi
                if ! cmp -s "$b" "$w"; then
                    echo "FAILURES: $d page $bp of $bh rows in bands of $bb"
                    echo "      ($tg) is not the page the same route puts out"
                    echo "      whole"
                    continue
                fi
                if [ -s "$r" ] && ! cmp -s "$b" "$r"; then
                    echo "FAILURES: $d page $bp of $bh rows in bands of $bb"
                    echo "      ($tg) is not the page a device that paints"
                    echo "      puts out"
                    continue
                fi
                if [ "$nb" -lt 2 ]; then
                    echo "FAILURES: $d put page $bp of $bh rows out in $nb"
                    echo "      band(s) at $bb rows a band; a page one band"
                    echo "      covers has no seam on it and every"
                    echo "      arrangement of marks passes on one"
                    continue
                fi
                # every row of the page reached the writer, once, where
                # the device writes its rows through the class at all
                if [ "$calls" -gt 0 ] && [ "$rows" -ne "$bh" ]; then
                    echo "FAILURES: $d handed its writer $rows row(s) of the"
                    echo "      $bh-row page $bp at $bb rows a band ($tg)"
                    continue
                fi
            done > "$work/out.$$"
            if ! spill "$work/out.$$"; then
                n=$(field "$lg" BAND | grep -c . || true)
                cells=$((cells + n))
                echo "OK   $d page $p: $n banded pages ($tg) are the page" \
                     "put out whole"
            fi
            rm -f "$work/out.$$"
        done

        # ---- the marks each band was played ----
        # The half no page can show. A replay handed the whole page for
        # every band puts out exactly this page and pays the marks times
        # the bands for it; the pixels cannot tell the two apart and
        # these numbers can.
        if [ "$isrec" = yes ] && [ "$wantrec" = yes ]; then
            field "$d-rec" BAND | while read -r bp bh bb bfile nb sum mx mn \
                                              calls rows outside plays; do
                [ -n "${bfile:-}" ] || continue
                marks=$(field "$d-rec" WHOLE |
                        awk -v p="$bp" -v h="$bh" \
                            '$1 == p && $2 == h { print $4 }' | head -1)
                [ -n "${marks:-}" ] || {
                    echo "FAILURES: $d did not say what page $bp of $bh rows"
                    echo "      comes to in marks, so the bands' own counts"
                    echo "      are read against nothing"
                    continue
                }
                if [ "$sum" -eq 0 ] && [ "$marks" -gt 0 ]; then
                    echo "FAILURES: $d played 0 marks over the bands of page"
                    echo "      $bp of $bh rows at $bb rows a band, and the"
                    echo "      page has $marks; the count is not counting"
                    continue
                fi
                # a page with too little on it says nothing about which
                # marks a band was given, so the discriminating checks
                # are asked only where there is something to discriminate
                [ "$marks" -ge 20 ] || continue
                if [ "$mx" -ge "$marks" ]; then
                    echo "FAILURES: a band of $d page $bp of $bh rows was"
                    echo "      played all $marks of its marks at $bb rows a"
                    echo "      band, so the rows a band asked for are the"
                    echo "      page's and not the band's"
                    continue
                fi
                if [ "$mx" -le "$mn" ]; then
                    echo "FAILURES: every band of $d page $bp of $bh rows was"
                    echo "      played the same $mx mark(s) at $bb rows a band"
                    continue
                fi
                # ... and by a margin, against what a replay ignoring
                # its row range would pay, which is every mark once per
                # band. Stated as a fraction of that rather than as a
                # multiple of the page's marks, because how many bands a
                # mark meets is how tall the mark is: a page whose marks
                # reach its full height costs nearly as much either way,
                # and a page of marks a row tall costs almost nothing, so
                # a multiple of the marks measures the page's shape and
                # not the replay's bound.
                if [ "$sum" -ge $((marks * nb)) ] ||
                   [ $((sum * 3)) -gt $((marks * nb * 2)) ]; then
                    echo "FAILURES: $d played $sum mark(s) over $nb bands of"
                    echo "      $bb rows on page $bp of $bh, against $marks in"
                    echo "      the page; what a band replay costs is"
                    echo "      following the bands rather than the drawing"
                    continue
                fi
            done > "$work/cnt.$$"
            if ! spill "$work/cnt.$$"; then
                echo "OK   $d page $p: every band was played the marks its" \
                     "own rows meet and no more"
            fi
            rm -f "$work/cnt.$$"

            # ... and the image, which is the one entry whose replay
            # costs the picture rather than a mark. It has to be painted
            # for the bands it reaches and not once per mark stepped
            # over on the way to it.
            field "$d-rec" BAND | while read -r bp bh bb bfile nb sum mx mn \
                                              calls rows outside plays; do
                [ -n "${plays:-}" ] || continue
                nimg=$(field "$d-rec" COST |
                       awk -v p="$bp" -v h="$bh" \
                           '$1 == p && $2 == h { print $4 }' | head -1)
                [ -n "${nimg:-}" ] || continue
                [ "$nimg" -gt 0 ] || continue
                if [ "$plays" -lt 1 ]; then
                    echo "FAILURES: $d page $bp of $bh rows holds $nimg image(s)"
                    echo "      and painted none of them at $bb rows a band"
                    continue
                fi
                if [ "$plays" -gt $((nb * nimg)) ]; then
                    echo "FAILURES: $d painted a recorded image $plays time(s)"
                    echo "      over $nb bands of $bb rows on page $bp, with"
                    echo "      $nimg image(s) in the record; a band replay"
                    echo "      paints one for the bands it reaches"
                fi
            done > "$work/img.$$"
            spill "$work/img.$$" || true
            rm -f "$work/img.$$"

            # ---- what the record costs ----
            # A record holds an image as one entry naming its samples
            # rather than as a mark per sample. Got wrong, that is not a
            # broken page but a record tens of times larger than the
            # page it exists to avoid holding, which no comparison of
            # pages can see either.
            if [ "$p" = 2 ]; then
                field "$d-rec" COST | while read -r cp ch cm ci cb pb; do
                    [ -n "${pb:-}" ] || continue
                    if [ "$ci" -lt 1 ]; then
                        echo "FAILURES: $d holds no image in the record of a"
                        echo "      page whose content is images"
                        continue
                    fi
                    if [ "$cm" -gt 16 ]; then
                        echo "FAILURES: $d holds $cm mark(s) for a page of"
                        echo "      $ci image(s) and little else; an image"
                        echo "      arriving as a mark per sample is what"
                        echo "      the record's own image entry exists to"
                        echo "      prevent"
                        continue
                    fi
                    if [ "$cb" -ge "$pb" ]; then
                        echo "FAILURES: $d holds $cb bytes of record for a"
                        echo "      page of $pb bytes; a record larger than"
                        echo "      the raster it saves holding saves nothing"
                    fi
                done > "$work/cost.$$"
                if ! spill "$work/cost.$$"; then
                    echo "OK   $d: a page of images costs a record of" \
                         "$(field "$d-rec" COST | head -1 | awk '{print $5}')" \
                         "bytes against a page of" \
                         "$(field "$d-rec" COST | head -1 | awk '{print $6}')"
                fi
                rm -f "$work/cost.$$"
            fi
        fi

        # ---- the pixels, for a page whose bytes are an encoding ----
        # Both routes are decoded, not one and the other by inference:
        # the two write the same bytes, so decoding one would settle the
        # other -- but that is a step of reasoning rather than a
        # measurement, and it costs less to take the measurement.
        #
        # The direct cell asks for the page whole, as every direct cell
        # here does: the bare name is weighed, so a cell spelling it
        # would be whichever route the page's size chose and the pair
        # would be one route decoded twice on a page large enough.
        for tg in dir rec; do
            [ "$sab" -eq 0 ] && [ "$encoded" = yes ] || continue
            csel=$(whole "$d")
            if [ "$tg" = rec ]; then
                [ "$isrec" = yes ] || continue
                csel="$d:band"
            fi
            rm -f "$work/$d-cmp.log"
            if render "$csel" "$d" "$d-cmp" -DPAGE=$p -DCMP=1 $bset; then
                field "$d-cmp" PIXELS | while read -r ch cb la lb diff; do
                    [ -n "${diff:-}" ] || continue
                    if [ "$la" -eq 0 ]; then
                        echo "FAILURES: the $csel page of $ch rows decoded to"
                        echo "      no pixels, so the comparison of pixels is"
                        echo "      comparing nothing"
                        continue
                    fi
                    if [ "$la" -ne "$lb" ]; then
                        echo "FAILURES: the $csel page of $ch rows decodes to"
                        echo "      $la pixels whole and $lb in bands of $cb"
                        continue
                    fi
                    if [ "$diff" -ge 0 ]; then
                        echo "FAILURES: the $csel page of $ch rows in bands of"
                        echo "      $cb decodes to different pixels from the"
                        echo "      page written whole, first at $diff"
                    fi
                done > "$work/pix.$$"
                if ! spill "$work/pix.$$"; then
                    n=$(field "$d-cmp" PIXELS | grep -c . || true)
                    if [ "$n" -eq 0 ]; then
                        note "$csel decoded no page of page $p, so nothing" \
                             "here compared any pixels"
                    else
                        cells=$((cells + n))
                        echo "OK   $csel page $p: $n banded pages decode to" \
                             "the pixels of the page written whole"
                    fi
                fi
                rm -f "$work/pix.$$"
            else
                note "$csel could not decode what it wrote for page $p"
            fi
        done
    done

    # ---- what all of that is per page rather than per job ----
    # A job of two pages, the first transmitted without being erased and
    # the second that same page with more on it (copypage, PLRM 8.2). A
    # record reads its page boundary off the rectangle that clears the
    # page, and copypage sends no such rectangle, so a record that gave
    # its page up at the emission would put out a second page missing
    # the first's marks.
    if [ "$sab" -eq 0 ]; then
        rm -f "$work/$d-mp.log"
        mpok=yes
        for h in $HEIGHTS; do
            render "$(whole "$d")" "$d" "$d-mp" -DPAGE=3 -DMPH=$h -DMPB=0 || mpok=no
            render "$(whole "$d")" "$d" "$d-mp" -DPAGE=3 -DMPH=$h -DMPB=7 || mpok=no
            if [ "$isrec" = yes ]; then
                for b in 0 $MPBANDS; do
                    render "$d:band" "$d" "$d-mp" \
                           -DPAGE=3 -DMPH=$h -DMPB=$b || mpok=no
                done
            fi
        done
        if [ "$mpok" != yes ]; then
            note "$d could not put out the two-page job"
        else
            for h in $HEIGHTS; do
                for n in 1 2; do
                    ref=$work/$d/mp-$h-0-dir-$n.out
                    if [ ! -s "$ref" ]; then
                        note "$d wrote no page $n of the two-page job of $h" \
                             "rows to hold the rest against"
                        continue
                    fi
                    for f in "$work/$d"/mp-$h-*-$n.out; do
                        [ -e "$f" ] || continue
                        cells=$((cells + 1))
                        cmp -s "$f" "$ref" ||
                            note "$(basename "$f") is not page $n of the" \
                                 "two-page job $d puts out whole"
                    done
                done
            done
            echo "OK   $d: both pages of a two-page job, in bands and whole," \
                 "are the pages a device that paints puts out"
        fi
    fi

    # ---- what the record holds, by kind ----
    # A page that reached only some of the five kinds would pass every
    # comparison above and say nothing about the rest.
    if [ "$sab" -eq 0 ] && [ "$isrec" = yes ]; then
        rm -f "$work/$d-kinds.log"
        if render "$d:band" "$d" "$d-kinds" -DPAGE=1 -DCENSUS=1; then
            nocount=$(field "$d-kinds" NOCOUNT | head -1)
            set -- $(field "$d-kinds" KINDS | head -1)
            if [ -n "${nocount:-}" ]; then
                note "the record played into $d carries $nocount colour" \
                     "value(s) a mark and no class that keeps its rows" \
                     "carries that many, so nothing counted what the mixed" \
                     "page's record holds and this campaign says nothing" \
                     "about which kinds of mark reached it"
            elif [ $# -lt 5 ]; then
                note "$d did not say what the record of the mixed page holds"
            else
                # A device asking for a glyph's partly covered edge
                # pixels as whole pixels is sent no coverage-weighted
                # blend, so the blend count is read against what the
                # target asked for rather than against a constant. Both
                # ways: such a device reaching a blend would mean it was
                # sent one it never asked for.
                tab=$(field "$d-kinds" TAB | head -1)
                : "${tab:=1}"
                blends=$2
                miss=''
                i=0
                for k in PutPix BlendPix DrawLine FillRect FillPoly; do
                    i=$((i + 1))
                    eval "v=\${$i}"
                    if [ "$k" = BlendPix ] && [ "$tab" -le 1 ]; then
                        continue
                    fi
                    [ "$v" -gt 0 ] || miss="$miss $k"
                done
                if [ "$tab" -le 1 ] && [ "$blends" -gt 0 ]; then
                    note "$d asks for a glyph's edge pixels as whole" \
                         "pixels and its record holds $blends blend(s)," \
                         "so it was sent the coverage-weighted marks it" \
                         "declared it did not want"
                elif [ -n "$miss" ]; then
                    note "the mixed page reaches no$miss, so what this" \
                         "campaign says about those kinds of mark is nothing"
                elif [ "$tab" -le 1 ]; then
                    echo "OK   $d: the mixed page reaches the four kinds a" \
                         "device taking whole pixels of text can reach," \
                         "and no blend ($1 pixels, $3 lines, $4 rectangles," \
                         "$5 polygons)"
                else
                    echo "OK   $d: the mixed page reaches all five kinds" \
                         "($1 pixels, $2 blends, $3 lines, $4 rectangles," \
                         "$5 polygons)"
                fi
            fi
        else
            note "$d could not count what the record of the mixed page holds"
        fi
    fi
done

# ---------------------------------------------------------------------
# One record, played the same into every device it is played into
#
# Everything above holds a device to its own accounting: that its page
# came out in more than one band, that an image was painted for the
# bands it reaches and no more. None of it compares one device with
# another, and there is a statement across them that no single device's
# numbers can make.
#
# The record is the same record and the band loop is the same loop
# whichever device it plays into, so the marks it plays and the bands it
# plays them over have to come out the same for all of them. What
# differs between these devices is what they do with a band once they
# have it -- rows through the class for one, a compiled assembly for
# another -- and that is downstream of the count.
#
# It is worth asking because a compiled assembly is where a difference
# could hide: the row accounting the class devices carry cannot reach
# one (it writes its own rows and never calls the class's writer), so a
# compiled writer running its loop at another granularity, or playing a
# band twice, passes every check it faces on its own. This is the check
# it does not.
key_of() { printf '%s/%s/%s' "$1" "$2" "$3"; }
: > "$work/agree"
for d in $matrixdevs; do
    [ -f "$work/$d-rec.log" ] || continue
    field "$d-rec" BAND | while read -r bp bh bb bfile nb sum mx mn \
                                       calls rows outside plays; do
        [ -n "${plays:-}" ] || continue
        printf '%s %s %s %s %s %s\n' "$(key_of "$bp" "$bh" "$bb")" \
               "$nb" "$sum" "$mx" "$mn" "$d"
    done >> "$work/agree"
done
if [ ! -s "$work/agree" ]; then
    note "no device reported what a record played it, so the accounting" \
         "was compared across nothing"
else
    # one line per route, with the devices that took it and what each said
    sort "$work/agree" | awk '
        { k = $1; c = $2 " " $3 " " $4 " " $5
          if (!(k in seen)) { seen[k] = c; who[k] = $6; n[k] = 1; next }
          n[k]++
          if (seen[k] != c) { bad[k] = bad[k] " " $6 "(" c ")" }
          who[k] = who[k] " " $6 }
        END {
          for (k in seen) {
            if (n[k] < 2) continue
            routes++
            if (k in bad)
              printf "MISMATCH %s first %s (%s) then%s\n", k, seen[k], who[k], bad[k]
          }
          printf "ROUTES %d\n", routes
        }' > "$work/agree.out"
    shared=$(sed -n 's/^ROUTES //p' "$work/agree.out")
    if [ "${shared:-0}" -lt 1 ]; then
        note "no route was taken by two devices, so the accounting was" \
             "compared with nothing on the other side of it"
    else
        if grep -q '^MISMATCH' "$work/agree.out"; then
            grep '^MISMATCH' "$work/agree.out" | while read -r _ k rest; do
                echo "FAILURES: the same record played into different devices"
                echo "      was played differently on page/rows/band $k: $rest"
                echo "      The record and the band loop are the same for"
                echo "      each of them, so the marks and the bands are the"
                echo "      record's answer and not the device's."
            done
            fail=1
        else
            cells=$((cells + shared))
            echo "OK   $shared route(s) played the same record the same way" \
                 "into every device that took them"
        fi
    fi
fi

# ---------------------------------------------------------------------
# The bound
#
# A loop that held the whole page anyway writes exactly the same bytes,
# so no comparison of pages can see it. What can see it is the memory,
# and which meter answers for a device is a property of where its raster
# is rather than a choice:
#
#   The PostScript raster classes take their rows from the global half
#   of virtual memory, which the interpreter reports on, so the run
#   reads it around its own page. A record's raster is one of these
#   whatever device the record was selected beside.
#
#   The two whose raster is a buffer of their own are invisible to that,
#   and what answers for them is the peak resident size of the process.
#
# Both are reported for every cell and the one that answers for the
# device is the one that decides, because a meter that cannot see the
# page would pass everything. That is the control: the whole-page route
# is required to grow by most of a row of pixels for every row of page.
#
# The second meter reads the process and not the page, so it carries the
# machine, and the sweep it is read over is chosen for that. Measured
# here with sixteen renders running beside it, each of these readings
# moved by two to three hundred kibibytes across ten rounds, and the
# same reading elsewhere in the suite has been seen to move by nearly
# seven hundred. Over the sweep the exact meter uses, what these cells
# were allowed came to around two mebibytes -- one movement of the
# instrument away from a verdict about the machine rather than about the
# code. Over the taller sweep they are read on instead, the room is four
# to six, several times any movement measured, and the cost is one
# taller run per cell rather than the same runs over again. Neither
# loosens what is caught: a route holding what the page's height reaches
# grows by the whole page's own growth, which is ten times what any of
# this moves by.
# ---------------------------------------------------------------------
if peak_rss_reads "$xpost"; then
    havetime=yes
else
    havetime=no
    echo "SKIP $peak_rss_why, so the two devices whose raster is a" \
         "buffer of their own are not weighed here"
fi

# $1 selection, $2 directory, $3 page height, $4 band; sets memvm, memrss
#
# The selection is spelled the way the rest of this file spells one: the
# route is weighed at the page a device is made at, and the pages here
# are the tall ones, so a device named without a mode is a device this
# arm would find a record in front of at exactly the heights it is
# weighing. The whole-page spelling is what says which of the two routes
# this cell is, which is what the comparison below is between.
weigh() {
    w_sel=$(whole "$1")
    w_dir=$work/mem-$2
    rm -rf "$w_dir"; mkdir -p "$w_dir"
    if [ "$havetime" = yes ]; then
        ( cd "$w_dir" && /usr/bin/time -f '%M' -o rss.txt \
          "$xpost" -q $ns -d "$w_sel" -o /dev/null -DW=$MEMW \
          -DMEMH="$3" -DMEMB="$4" $sabarg "$script" </dev/null ) \
          >"$w_dir/run.log" 2>&1
    else
        ( cd "$w_dir" && "$xpost" -q $ns -d "$w_sel" -o /dev/null -DW=$MEMW \
          -DMEMH="$3" -DMEMB="$4" $sabarg "$script" </dev/null ) \
          >"$w_dir/run.log" 2>&1
    fi
    w_st=$?
    # A weighing is a run and is judged like one, both ways round. A run
    # that died on its way to a page took little, and little is exactly
    # the reading the band route is here to produce, so a reading nobody
    # judged is a reading a broken run could forge.
    w_out=$(cat "$w_dir/run.log")
    verdict_run "$w_st" "$w_out" "the $1 weighing" || return 1
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$w_out" "the suite"
    verdict_ok "$w_out" "the $1 weighing" || return 1
    memvm=$(sed -n 's/^MEM //p' "$w_dir/run.log" | awk '{print $NF}' | tail -1)
    memrss=$(tail -1 "$w_dir/rss.txt" 2>/dev/null)
    case ${memrss:-x} in *[!0-9]*) memrss='' ;; esac
    # A meter the run itself provides can be stubbed from inside; one the
    # wrapper reads has to be stubbed here.
    [ "$sab" -eq 5 ] && memrss=12345
    [ -n "${memvm:-}" ]
}

# $1 lo reading, $2 hi reading, $3 rows between, $4 bytes per reading unit;
# prints bytes a row, scaled by a thousand
slope() {
    echo $(( 1000 * ($2 - $1) * $4 / $3 ))
}

memcells=0
weighed=no
for d in $present; do
    for route in dir rec; do
        sel=$d
        if [ "$route" = rec ]; then
            case " $targets " in *" $d "*) sel="$d:band" ;; *) continue ;; esac
        fi

        # Which meter answers for this cell, taken from the device the
        # page ends up on rather than from the route that reached it. A
        # record played into a device keeping a buffer of its own paints
        # into that buffer, so the interpreter's own count of what it
        # took answers nothing there -- truthfully, which is why it
        # cannot be the meter that decides. A cell whose device will not
        # say where its raster is gets no meter and is a failure, not a
        # pass: a cell weighed by nothing is a cell that cannot fail.
        if ! askdev "$sel"; then
            note "$sel did not say where the raster of the device its page" \
                 "ends up on lives, so no meter can be chosen for it and" \
                 "nothing here would weigh it"
            continue
        fi
        # What a row of this raster costs, taken from the class's own
        # statement of it rather than from the colour space: the two
        # agree only where a component is a byte and the row holds
        # nothing else, and the meter below is being asked whether it
        # can see a page of this device -- so the number it is held
        # against has to be this device's row and not a stand-in for it.
        rowpix=$ask_row
        case $ask_where in
            rows)   meter=vm;  hlo=$MEMLO; hmid=$MEMMID; hhi=$MEMHI ;;
            buffer) meter=rss; hlo=$RSSLO; hmid=$RSSMID; hhi=$RSSHI ;;
        esac
        if [ "$meter" = rss ] && [ "$havetime" != yes ]; then
            echo "SKIP $sel keeps its raster in a buffer of its own and the" \
                 "peak resident size of a run cannot be read here"
            continue
        fi

        # A sabotage weighs one cell, and it has to be a cell the meter
        # it stubbed is the one that answers for.
        if [ "$sab" -ne 0 ]; then
            [ "$weighed" = no ] || continue
            case $sab in
                3) [ "$meter" = vm ] || continue ;;
                5) [ "$meter" = rss ] || continue ;;
                *) continue ;;
            esac
            weighed=yes
        fi

        ok=yes
        for h in $hlo $hmid $hhi; do
            for b in 0 64; do
                weigh "$sel" "$d-$route-$h-$b" "$h" "$b" || { ok=no; break; }
                eval "vm_${h}_${b}=\$memvm"
                eval "rss_${h}_${b}=\${memrss:-0}"
            done
            [ "$ok" = yes ] || break
        done
        if [ "$ok" != yes ]; then
            note "$sel did not report what it took at every page height," \
                 "so there is nothing to compare"
            continue
        fi

        if [ "$meter" = vm ]; then
            u=1
            eval "wlo=\$vm_${hlo}_0;  wmid=\$vm_${hmid}_0;  whi=\$vm_${hhi}_0"
            eval "blo=\$vm_${hlo}_64; bmid=\$vm_${hmid}_64; bhi=\$vm_${hhi}_64"
            what="the global half of virtual memory"
        else
            u=1024
            eval "wlo=\$rss_${hlo}_0;  wmid=\$rss_${hmid}_0;  whi=\$rss_${hhi}_0"
            eval "blo=\$rss_${hlo}_64; bmid=\$rss_${hmid}_64; bhi=\$rss_${hhi}_64"
            what="the peak resident size of the process"
        fi
        w1=$(slope "$wlo" "$wmid" $((hmid - hlo)) "$u")
        w2=$(slope "$wmid" "$whi" $((hhi - hmid)) "$u")
        b1=$(slope "$blo" "$bmid" $((hmid - hlo)) "$u")
        b2=$(slope "$bmid" "$bhi" $((hhi - hmid)) "$u")
        wa=$(slope "$wlo" "$whi" $((hhi - hlo)) "$u")
        ba=$(slope "$blo" "$bhi" $((hhi - hlo)) "$u")
        echo "OK   held: $sel whole $wlo -> $wmid -> $whi, band $blo -> $bmid" \
             "-> $bhi over $hlo -> $hmid -> $hhi rows ($what)"
        echo "OK   held: $sel grows by $w1/1000 then $w2/1000 bytes a row" \
             "holding the page, and $b1/1000 then $b2/1000 holding a band"

        # The meter has to be able to see a page at all, or it would
        # pass anything. A row of this raster is $rowpix bytes.
        seen=yes
        for s in "$w1" "$w2"; do
            [ "$s" -ge $((700 * rowpix)) ] || seen=no
        done
        if [ "$seen" != yes ]; then
            note "holding the whole $sel page grew by $w1/1000 and" \
                 "$w2/1000 bytes a row where a row of it is $rowpix bytes;" \
                 "$what is not seeing the raster here, so it cannot say" \
                 "the band bounds it"
            continue
        fi

        # ... and the banded route must not follow the page's height.
        #
        # The two meters are not equally sharp and holding them to one
        # rule would make the campaign flaky rather than strict. The
        # interpreter's own count of virtual memory is exact and answers
        # for this raster and nothing else, so each step of it is held.
        # The peak resident size of a process is a measurement of the
        # whole process by the host: an arena taken in chunks and a
        # machine busy with something else move it by a megabyte for
        # reasons that are not this page, and over one step of the sweep
        # that is a slope of its own. So it is read over the whole sweep,
        # where a megabyte of noise is spread across every row of it, and
        # given the margin the instrument has rather than the margin the
        # exact one has. Both steps are reported either way, because a
        # reader wants to see them.
        bounded=yes
        if [ "$meter" = vm ]; then
            for s in "$b1" "$b2"; do
                [ $((s * 10)) -lt "$w1" ] || bounded=no
            done
            said="under a tenth of the whole page's growth, at both steps"
        else
            [ $((ba * 5)) -lt "$wa" ] || bounded=no
            # ... and the plainest statement of the same thing: the
            # tallest page in bands weighs less than the shortest page
            # held whole. What separates them is the raster of the short
            # page, which is why the short page of this sweep is not the
            # shortest one worth rendering -- at a thousand rows the two
            # readings stand two mebibytes apart and this says as much
            # about the machine as the other check does.
            [ "$bhi" -lt "$wlo" ] || bounded=no
            said="under a fifth of the whole page's growth over the sweep, \
and the tallest page in bands weighs less than the shortest page held whole"
        fi
        if [ "$bounded" != yes ]; then
            note "the $sel band route grew by $ba/1000 bytes a row over the" \
                 "sweep against the whole page's $wa/1000, and weighs $bhi" \
                 "at $hhi rows against $wlo for a whole page of $hlo;" \
                 "what it holds is following the page's height, so the band" \
                 "is not bounding it"
            continue
        fi
        memcells=$((memcells + 1))
        echo "OK   $sel holds a band and not the page: $said"
    done
done

if [ "$sab" -eq 0 ] && [ "$memcells" -lt 4 ]; then
    note "$memcells device(s) were weighed; the bound is the whole point" \
         "of the mechanism and a campaign that weighed almost none of it" \
         "has not covered it"
fi

# ---------------------------------------------------------------------
# The campaign's own controls
#
# Every check above answers by finding nothing. A check that had stopped
# looking would answer the same way, so each is run once against a
# defect built to trip it and required to trip.
# ---------------------------------------------------------------------
if [ "$sab" -eq 0 ]; then
    for s in 1 2 3 4 5 6; do
        # A sabotage of what a record counts needs a record to count.
        case $s in
            1|2|4)
                if [ -z "$(printf '%s\n' $targets | grep -m1 . || true)" ]; then
                    echo "SKIP no record can be played into any device in" \
                         "this build, so a defect in what a replay plays" \
                         "has nothing to be built into"
                    continue
                fi ;;
        esac
        # A sabotage of the wrapper's own meter needs a device that meter
        # answers for, and a machine that has it.
        if [ "$s" -eq 5 ]; then
            if [ "$havetime" != yes ]; then
                echo "SKIP the peak-resident-size meter cannot be read here," \
                     "so stubbing it says nothing"
                continue
            fi
            case " $present " in
                *" png "*|*" jpeg "*) ;;
                *)  echo "SKIP no device in this build keeps its raster in a" \
                         "buffer of its own, so nothing is weighed by the" \
                         "meter this would stub"
                    continue ;;
            esac
        fi
        case $s in
            1) why="a replay handed the whole page for every band" ;;
            2) why="a mark counter answering zero" ;;
            3) why="the interpreter's own memory meter answering a constant" ;;
            4) why="a band whose marks are not played" ;;
            5) why="the peak-resident-size meter answering a constant" ;;
            6) why="a raster left standing on the whole page" ;;
            *) why="defect $s" ;;
        esac
        if "$self" --sabotage "$s" "$xpost" "$script" >"$work/sab$s.log" 2>&1
        then
            note "the campaign passed with $why; the check that is supposed" \
                 "to catch it is not catching anything" \
                 "$(sed -n 's/^OK  */      passed: /p' "$work/sab$s.log" \
                   | head -1)"
        else
            echo "OK   the campaign fails on $why:" \
                 "$(grep -m1 '^FAILURES:' "$work/sab$s.log" |
                    cut -c11- | cut -c1-60)"
        fi
    done
fi

if [ "$sab" -eq 0 ]; then
    echo "OK   $cells page(s) compared, $memcells device route(s) weighed"
fi

[ "$fail" -eq 0 ] || exit 1
echo "SUCCESS"
exit 0
