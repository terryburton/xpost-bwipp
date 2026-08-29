#!/bin/sh
# Meson test wrapper: the page device's imaging bounding box (PLRM 6.2).
#
# A program supplying /ImagingBBox asserts that it paints no marks
# outside the box, and PLRM 6.2 says of the marks that do fall outside
# that they "may or may not be rendered on the output medium". That is
# what lets a device hold less of the page than the page, which is the
# whole of what the parameter is for: PLRM asks applications to supply
# one wherever they can "since it can improve performance by freeing
# raster memory for other purposes".
#
# Three questions, put to every device the build has:
#
#   1. What a program observes. The rows a device holds and the marks it
#      keeps are the device's own business; the answers are in
#      tests/imaging_bbox_test.ps, which reads them off the running
#      device and reports for itself.
#
#   2. What comes out. A program that keeps to its own assertion must get
#      the page it would have got without the box -- the same page, of
#      the same size, to the byte. That is the strongest thing that can
#      be said about the change and the only one that covers a writer's
#      whole output rather than the raster behind it, so it is asked of
#      every device that writes a file, whether or not that device holds
#      any part of the page differently.
#
#   3. Whether the two routes to a page agree about the box. A page over
#      the band budget is painted a band at a time, by the recording
#      class playing what it kept into a raster that stands for one run
#      of the page's rows after another; a page under the budget is
#      painted whole on the device the run named. The box reaches both:
#      it is the run of rows a raster holds one way and the run of rows a
#      band loop goes over the other, and the loop drops the marks
#      outside it exactly as the raster does. So the same page declaring
#      the same box has to come out the same bytes whichever route it
#      took, and the routes are compared here rather than each being
#      compared only against itself.
#
# Five pages carry those two questions, each covering a way of getting
# the run of rows wrong:
#
#   marks    every route a mark takes to the raster -- a fill, a stroke,
#            text, an image and an image mask -- and a first mark that
#            covers the box exactly to the pixel, so a device taking the
#            box a row too far in either direction loses part of it
#   ground   an atypical transfer function, so that the colour the page
#            is cleared to is not white and a device showing plain white
#            over the rows it does not hold is caught
#   scaled   the same marks at a resolution where a point is not a pixel,
#            which is where a box measured in points meets rows
#   offset   the same marks with the page image shifted, which moves the
#            marks and must move the box with them
#   outside  marks above the box and below it as well as inside it, which
#            the third question alone asks, for the reason written beside
#            the page. It is the page that says whether the two routes
#            agree: one keeping to its own assertion is painted the same
#            by a device that honours the box and by one that ignores it,
#            so its bytes are equal either way and say nothing.
#
# WHAT THE SECOND AND THIRD QUESTIONS DO NOT COVER, plainly. They compare
# one of this run's pages against another of them, so they say the box
# changed nothing and the route changed nothing, and cannot say any of
# those pages was right. A fault in what the runs share -- the marking,
# the colour conversion, the format the writer emits -- stands in all of
# them equally and passes here. What a page's bytes have to be is the
# golden-render manifest's, and what a raster format's pixels have to be
# is tests/run-raster-formats-test.sh's.
#
#   $1  path to the built xpost binary
#   $2  path to imaging_bbox_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"
. "$(dirname "$0")/device-fleet.sh"

if "$xpost" -h 2>/dev/null | grep -q -- '--no-sandbox'; then
    ns='--no-sandbox'
else
    ns=''
fi

verdict_workdir

cat > "$work/marks.body" <<'PSEOF'
% the box, to the pixel: 100 units square with its corners on the box's
% own corners, so a device holding one row too few drops part of it
0.75 setgray
newpath 50 50 moveto 150 50 lineto 150 150 lineto 50 150 lineto
closepath fill
1 0 0 setrgbcolor
newpath 60 60 moveto 140 60 lineto 140 100 lineto closepath fill
0 0 1 setrgbcolor
newpath 70 70 moveto 40 0 rlineto 0 40 rlineto closepath 3 setlinewidth stroke
0 setgray /Helvetica findfont 14 scalefont setfont
60 130 moveto (Ag) show
gsave 40 0 0 40 60 105 6 array astore concat
4 4 8 [4 0 0 -4 0 4] <00408000C0FF40208060A0E01030507090B0D0F0> image
grestore
gsave 30 0 0 30 105 105 6 array astore concat
4 4 true [4 0 0 -4 0 4] <A050A050> imagemask
grestore
showpage
quit
PSEOF

cat > "$work/ground.body" <<'PSEOF'
% erasepage paints the whole page with gray 1.0 through the transfer
% function (PLRM 8.2), which is ordinarily white and here is not
{ 0.35 mul 0.2 add } settransfer
erasepage
0 setgray
newpath 90 90 moveto 110 90 lineto 110 110 lineto 90 110 lineto
closepath fill
showpage
quit
PSEOF

# The page that breaks its own promise: marks inside the box, and marks
# above it and below it by every route a mark takes to the raster. PLRM
# 6.2 leaves those free to be rendered or dropped, so no page's bytes
# are wrong for either -- but a device holding the rows the box reaches
# cannot render them and one holding the whole page can, which makes
# this the page on which the two routes disagree if they are going to.
# It is asked of the third question alone: the second holds the box
# against no box, and a page painting outside its box is a page the box
# is entitled to change.
cat > "$work/outside.body" <<'PSEOF'
0.75 setgray
newpath 50 50 moveto 150 50 lineto 150 150 lineto 50 150 lineto
closepath fill
0 setgray /Helvetica findfont 14 scalefont setfont
60 95 moveto (In) show
% above the box: a fill, an image and text
0.25 setgray
newpath 10 160 moveto 190 160 lineto 190 175 lineto 10 175 lineto
closepath fill
gsave 30 0 0 30 20 180 6 array astore concat
4 4 8 [4 0 0 -4 0 4] <00408000C0FF40208060A0E01030507090B0D0F0> image
grestore
0 setgray 100 182 moveto (Above) show
% below it: a stroke and text
0 0 1 setrgbcolor
newpath 10 12 moveto 190 42 lineto 6 setlinewidth stroke
0 setgray 20 20 moveto (Below) show
showpage
quit
PSEOF

cp "$work/marks.body" "$work/scaled.body"
cp "$work/marks.body" "$work/offset.body"

CASES='marks ground scaled offset'
ROUTE_CASES="$CASES outside"

# The band budget the third question's pages run under, in rows of a page
# this wide.
#
# A page of this size is far under the budget a run carries by default,
# so nothing here would band and the third question would compare the
# whole route against itself. The budget is what decides, so the budget
# is what is lowered -- and lowered in rows of the target's own raster,
# priced by the class that prices it for Create, so that every device
# holds the same number of rows of the page whatever a row of it costs.
# What that must come to is fewer rows than the box reaches, or the run
# arrives in one band and the loop is a loop over one thing; the runs
# below report the rows they held and are held to that.
BANDROWS=16
BOXROWS=101

# A row is priced by the class the record plays into, which is the
# device's own class where the run named a device and the target's where
# it named the recording class. The budget goes onto the recording class
# and onto the copy of it a driver that has already been loaded makes its
# devices from, since a run that starts on a record has taken that copy
# before a line of this has run.
band_prologue() {
    printf '.privatedict /.xpost_RECORD get /.bandbytes\n'
    printf '  2 dict begin /width 200 def\n'
    printf '    DEVICE /.playclass known\n'
    printf '      { .privatedict DEVICE /.playclass get get }{ DEVICE } ifelse\n'
    printf '    /.rowcost get exec\n'
    printf '  end exch pop %s mul\nput\n' "$BANDROWS"
    printf '.privatedict /.xpost_RECORDDEVICE 2 copy known { get /.bandbytes\n'
    printf '  .privatedict /.xpost_RECORD get /.bandbytes get put }{ pop pop } ifelse\n'
}

# What a banded run says about itself: the class that painted its page
# and the rows it held of that page at once. A run that reports the
# device it was named as, or a run holding the whole box at once, has not
# asked the question this file adds and says so rather than passing.
route_report() {
    printf '(\\nROUTE ) print\n'
    printf 'DEVICE /.playclass known { (record) }{ (direct) } ifelse print\n'
    # the rows a device holds are its own where it was built to hold them,
    # and its state's where a record wrote them as it painted
    printf '(\\nHELD ) print DEVICE /.bandrows known\n'
    printf '  { DEVICE /.bandrows get }\n'
    printf '  { DEVICE /.state get /.bandrows 2 copy known\n'
    printf '      { get }{ pop pop DEVICE /height get } ifelse } ifelse\n'
    printf '  20 string cvs print (\\n) print\n'
}

# each page three times over: declaring the box, declaring none, and
# declaring the box again on a run whose budget sends the page to the
# band loop. Everything below the first lines is the same text, so the
# only difference between a pair of runs is the request itself.
for case in $CASES; do
    rest=
    case $case in
        scaled) rest='/HWResolution [50 91]' ;;
        offset) rest='/PageOffset [0 20]' ;;
    esac
    { printf '<< /PageSize [200 200] %s /ImagingBBox [50 50 150 150] >> setpagedevice\n' \
        "$rest"
      cat "$work/$case.body"; } > "$work/hinted-$case.ps"
    { printf '<< /PageSize [200 200] %s /ImagingBBox null >> setpagedevice\n' \
        "$rest"
      cat "$work/$case.body"; } > "$work/plain-$case.ps"
    { band_prologue
      printf '<< /PageSize [200 200] %s /ImagingBBox [50 50 150 150] >> setpagedevice\n' \
        "$rest"
      route_report
      cat "$work/$case.body"; } > "$work/banded-$case.ps"
done

{ printf '<< /PageSize [200 200] /ImagingBBox [50 50 150 150] >> setpagedevice\n'
  cat "$work/outside.body"; } > "$work/hinted-outside.ps"
{ band_prologue
  printf '<< /PageSize [200 200] /ImagingBBox [50 50 150 150] >> setpagedevice\n'
  route_report
  cat "$work/outside.body"; } > "$work/banded-outside.ps"

fail=0

# The roster less what a build may not have the library for: a roster
# that skipped from end to end asks nothing and would report the same
# success as one that answered everywhere.
floor=0
for dev in $DEVICE_FLEET_ALL; do
    case " $DEVICE_FLEET_OPTIONAL " in *" $dev "*) continue ;; esac
    floor=$((floor + 1))
done

one_device() {
    dev=$1
    d_fail=0
    # The device holding the whole page, asked for as the mode that says
    # so. What the box buys is the rows a device does not hold, and a
    # device already holding a band of the page holds fewer of them than
    # any box would leave it: selecting a device by name selects the
    # record in front of it, and none of the roster would be left
    # reading a page a row at a time. The record answers for itself as a
    # member of the roster.
    d_sel=$(fleet_whole "$dev")

    # 1. what the program observes
    out=$("$xpost" -q $ns -d "$d_sel" -o "$work/probe.$dev" "$script" \
          </dev/null 2>&1)
    st=$?
    case "$out" in
        *"wrong device"*) echo "$dev: not built in; not asked"; return 2 ;;
    esac
    printf '%s\n' "$out" | grep -E '^FAIL' | sed "s/^/$dev: /"
    verdict_run "$st" "$out" "the imaging-bbox job on $dev" || d_fail=1
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
    verdict_ok "$out" "the imaging-bbox check on $dev" || d_fail=1

    # Whether this device's run reached the half of the script that reads
    # the page a row at a time. Most of what the script asserts is inside
    # that, so a roster on which no device ever got there would print the
    # same success as one that asked everywhere; the answers are collected
    # and held to a floor below.
    case $(printf '%s\n' "$out" | sed -n 's/^HOLDSROWS //p' | head -1) in
        yes) echo "$dev" >> "$work/heldrows" ;;
        no)  ;;
        *)   echo "FAILURES: $dev did not say whether it held its page as rows"
             d_fail=1 ;;
    esac

    # 2. what comes out
    wrote=no
    for case in $CASES; do
        rm -f "$work/h-$case.$dev" "$work/p-$case.$dev"
        out=$("$xpost" -q $ns -d "$d_sel" -o "$work/h-$case.$dev" \
              "$work/hinted-$case.ps" </dev/null 2>&1)
        verdict_run "$?" "$out" "the hinted $case page on $dev" || d_fail=1
        out=$("$xpost" -q $ns -d "$d_sel" -o "$work/p-$case.$dev" \
              "$work/plain-$case.ps" </dev/null 2>&1)
        verdict_run "$?" "$out" "the unhinted $case page on $dev" || d_fail=1

        # Whether there is a page to compare is read off the run rather
        # than assumed: a device that keeps its raster for whoever
        # embedded the interpreter, or that paints nothing at all,
        # leaves nothing at the path it was given and is held to the
        # first question alone.
        [ -s "$work/h-$case.$dev" ] || [ -s "$work/p-$case.$dev" ] || continue
        wrote=yes
        if cmp -s "$work/h-$case.$dev" "$work/p-$case.$dev"; then
            echo "$dev: the $case page is the same with the box and without it"
        else
            echo "FAILURES: $dev: declaring the box changed the $case page"
            d_fail=1
        fi
    done
    if [ "$wrote" = no ]; then
        echo "$dev: writes no page file; held to what the program observes"
        echo "$dev" >> "$work/nofile"
    fi

    # 3. the same box, the other route
    #
    # Asked of the devices a page may arrive at a band at a time on. The
    # selection is the plain name, which is the route a run gets without
    # asking for anything: the page is over the budget the run above
    # lowered, so it reaches the band loop, where the arm above reached
    # the device's own raster directly.
    #
    # The recording class is asked too, where naming it is naming a
    # record either way and what changes is the band: the run of rows in
    # bands of sixteen against the same run in the one band the standing
    # budget buys. That is the seam question rather than the route one,
    # and it is the only form of it a device reached by one route has.
    case " $DEVICE_FLEET_BANDS record " in *" $dev "*) ;; *)
        [ "$d_fail" -eq 0 ] || return 1
        return 0 ;;
    esac
    for case in $ROUTE_CASES; do
        # the page the box was declared on, held whole. The four the
        # second question rendered are already here; the fifth is this
        # question's own and is rendered for it.
        if [ ! -f "$work/h-$case.$dev" ]; then
            out=$("$xpost" -q $ns -d "$d_sel" -o "$work/h-$case.$dev" \
                  "$work/hinted-$case.ps" </dev/null 2>&1)
            verdict_run "$?" "$out" "the whole $case page on $dev" || d_fail=1
        fi
        rm -f "$work/b-$case.$dev"
        out=$("$xpost" -q $ns -d "$dev" -o "$work/b-$case.$dev" \
              "$work/banded-$case.ps" </dev/null 2>&1)
        verdict_run "$?" "$out" "the banded $case page on $dev" || d_fail=1

        # What that run did, read off the run itself. A page that took
        # the other route after all, or one whose band held the box
        # whole, is a cell that asks nothing -- and asks it quietly,
        # since its bytes would compare equal for the plainest of
        # reasons.
        r_route=$(printf '%s\n' "$out" | sed -n 's/^ROUTE //p' | head -1)
        r_held=$(printf '%s\n' "$out" | sed -n 's/^HELD //p' | head -1)
        case ${r_held:-x} in *[!0-9]*) r_held= ;; esac
        if [ "$r_route" != record ] || [ -z "$r_held" ]; then
            echo "FAILURES: $dev: the banded $case page was painted by" \
                 "[${r_route:-nothing}] and not by the recording class"
            d_fail=1
            continue
        fi
        if [ "$r_held" -ge "$BOXROWS" ]; then
            echo "FAILURES: $dev: the banded $case page held $r_held rows at" \
                 "once and the box reaches $BOXROWS, so it arrived in one band"
            d_fail=1
            continue
        fi

        [ -s "$work/b-$case.$dev" ] || {
            echo "FAILURES: $dev: the banded $case page wrote nothing"
            d_fail=1
            continue
        }
        if cmp -s "$work/b-$case.$dev" "$work/h-$case.$dev"; then
            echo "$dev: the $case page is the same in $r_held-row bands as" \
                 "it is held whole"
            echo "$dev" >> "$work/banded"
        else
            echo "FAILURES: $dev: the $case page in $r_held-row bands is not" \
                 "the page held whole"
            d_fail=1
        fi
    done

    [ "$d_fail" -eq 0 ] || return 1
    return 0
}

fleet_each one_device $DEVICE_FLEET_ALL || fail=1

# The devices with no page file to compare, and why: raster and bgr keep
# their raster for whoever embedded the interpreter rather than writing
# it, null paints nothing, and bbox records the extent of a page instead
# of its pixels. The reading is taken from the runs above and held
# against this list, so a device that has quietly stopped writing its
# page fails here rather than leaving one fewer comparison made.
NO_FILE='raster bgr null bbox'
want=$(printf '%s\n' $NO_FILE | sort | tr '\n' ' ')
got=$([ -f "$work/nofile" ] && sort "$work/nofile" | tr '\n' ' ')
if [ "$want" != "$got" ]; then
    echo "FAILURES: the devices with no page to compare are [$got],"
    echo "      and the ones named here as writing none are [$want]"
    fail=1
fi

# The devices that read their page a row at a time, and the floor under
# them. Four classes keep their page as rows of the interpreter's own
# virtual memory and every one of them must reach those checks: rename
# or relocate what they keep it in and the whole raster half of this
# script would go quiet, passing on the strength of the byte comparison
# alone.
ROWS_FLOOR=4
nrows=$([ -f "$work/heldrows" ] && wc -l < "$work/heldrows" || echo 0)
if [ "${nrows:-0}" -lt "$ROWS_FLOOR" ]; then
    echo "FAILURES: $nrows device(s) read their page a row at a time and"
    echo "      $ROWS_FLOOR keep it that way; the checks that read the rows"
    echo "      asked nothing"
    fail=1
else
    echo "OK   $nrows device(s) read their page a row at a time"
fi

# The pages compared across the two routes, and the floor under them.
# Five devices a page may arrive at in bands are in every build -- the
# four raster classes the boot files define and the recording class
# itself -- and each carries the five pages, so a build that has stopped
# reaching the band loop with a box declared reports fewer than this
# rather than reporting a whole roster held.
BANDED_FLOOR=25
nbanded=$([ -f "$work/banded" ] && wc -l < "$work/banded" || echo 0)
if [ "${nbanded:-0}" -lt "$BANDED_FLOOR" ]; then
    echo "FAILURES: $nbanded page(s) declaring the box were compared across"
    echo "      the two routes and $BANDED_FLOOR of them are in every build;"
    echo "      the band loop was not reached with a box"
    fail=1
else
    echo "OK   $nbanded page(s) declaring the box came out the same in bands"
    echo "     as held whole"
fi

if [ "$fleet_asked" -lt "$floor" ]; then
    echo "FAILURES: $fleet_asked of the roster answered and $floor of it is"
    echo "      made without an optional library"
    exit 1
fi
[ "$fail" -eq 0 ] || exit 1
echo "imaging-bbox: held on $fleet_asked device(s)"
echo SUCCESS
