# Sourced by the wrappers that run one workload once per device: the
# roster of every device, and the two subsets a cross-product runs.
#
# DEVICE_FLEET_ALL names every device the interpreter can make without a
# display. tests/check-device-roster.sh holds it to the interpreter's
# maker table, so a device added there and left out here fails, and
# run-devices-test.sh renders a page through all of it -- no device is
# built, selectable and never once exercised.
#
# A cross-product wrapper asks one question of every device, and two
# devices that answer it out of the same code are the same run twice.
# Which two those are depends on the question, so there are two subsets
# rather than one, each named for what its members implement separately:
#
#   DEVICE_FLEET_LIFETIME   what a device owns and when it releases it.
#                           Create, Destroy, GetPix after a Destroy, and
#                           the retirement a page-device change makes.
#   DEVICE_FLEET_MARKING    what a device does with a mark: the marking
#                           methods, the colour components they take and
#                           the raster (or the record) they leave.
#
# The membership is read off the sources rather than chosen by count:
#
#   pgm ppm pbm tiff  one class, built by .makerasterclass (data/image.ps)
#                     over a parameter set. Create, Destroy, GetPix and
#                     the Emit dispatch are the prototype's on all four,
#                     so one of them stands for the lifetime of all four.
#                     They diverge where they mark: pgm carries one grey
#                     component, ppm three colour ones, and pbm has a
#                     PutPix of its own that screens and thresholds to
#                     bilevel. tiff is data/ppmimage.ps with the page
#                     writer replaced, and the page writer is not a mark:
#                     golden-render, raster-formats and multipage hold
#                     its bytes.
#   pdfwrite dscwrite svgwrite
#                     the vector writers. dscwrite is data/pdfwrite.ps
#                     copied with its three page writers replaced --
#                     Create, Destroy, the Emit dispatch and every
#                     marking method are inherited whole -- so pdfwrite
#                     stands for it in both subsets, and pdf-device,
#                     golden-render and multipage hold what it writes.
#                     svgwrite implements its own, and stands for itself.
#   raster bgr png pngalpha jpeg
#                     the devices whose raster is a buffer outside the
#                     PostScript virtual machine, which is the class a
#                     released buffer can be marked through. Four
#                     implementations: raster, bgr and jpeg one each,
#                     and png with pngalpha, which is the same C with an
#                     alpha flag -- the same Create, Destroy and GetPix,
#                     and an alpha branch through the blend that only
#                     the marking subset has anything to ask about.
#   null bbox         the devices that paint nothing, which is the
#                     opposite fault: a method that does something is as
#                     wrong as one that does not. Neither owns a raster
#                     and neither derives from the other.
#   record            the device that paints nothing and keeps what it
#                     was asked to paint, playing it into a device that
#                     does when the page is put out. Every marking
#                     method is its own, written to record rather than
#                     to mark, and so is its lifetime: what it owns is a
#                     record outside virtual memory. It derives from no
#                     other member and no other member derives from it.
#
# A device leaves a subset only where every line it would run there is
# another member's too. Adding one is free; taking one out is a claim
# about the sources, and the claim is written above.

# DEVICE_FLEET_OPTIONAL names the members that need a library the build
# may not have, and so the only ones that may legitimately answer "wrong
# device". It is what lets a wrapper hold itself to a floor: a device
# that is not built in skips, a roster that skipped from end to end
# leaves every verdict untaken, and a wrapper with nothing to say prints
# the same SUCCESS as one that asked its question of everything. The
# floor is the roster less this list, so it follows the roster instead
# of being a number typed beside it.
DEVICE_FLEET_ALL='pgm ppm pbm tiff null bbox raster bgr png pngalpha
                  pdfwrite svgwrite dscwrite jpeg record'

DEVICE_FLEET_OPTIONAL='png pngalpha jpeg'

# The writers: devices whose page is a program or a drawing rather than
# pixels. A question a register answers for "a vector device" is answered
# by all of them or by none, so a probe naming that class runs over this
# and not over whichever one of them came to mind -- which is how two of
# them came to differ from the third without anything saying so.
DEVICE_FLEET_VECTOR='pdfwrite svgwrite dscwrite'

DEVICE_FLEET_LIFETIME='pgm null bbox raster bgr png jpeg pdfwrite svgwrite
                       record'

DEVICE_FLEET_MARKING='pgm ppm pbm null bbox raster bgr png pngalpha jpeg
                      pdfwrite svgwrite record'

# The devices that hold a band of the page rather than the page, which
# selecting by name selects. A wrapper whose question is about the
# raster -- what a pixel reads back as, what a device holds -- asks for
# the mode that holds the page whole instead, since a record holds no
# pixel to answer from and answers the ground.
#
# tests/check-device-roster.sh holds this list, the C table the
# selection is rewritten from, and the recording class's own roster to
# naming the same devices.
DEVICE_FLEET_BANDS='pgm ppm pbm tiff png jpeg'

# The devices whose page never arrives at the output path: the two that
# hand their raster to the program embedding the interpreter, and the two
# that paint nothing at all. A run naming -o leaves that name untouched,
# with a %d in it or without.
#
# It is what every wrapper comparing the bytes of a page has to know, and
# each of them used to know it separately: the byte-identity gate, the
# multi-page shapes and the smoke wrapper each carried the same four
# names, so a device added to the roster and to none of them was rendered
# and never compared. Naming them once makes the other direction the
# default -- a device the roster gains and this list does not is a device
# whose page is a file, and every wrapper here compares it.
#
# tests/check-device-roster.sh holds this list by running each device and
# looking at the output path, both ways round: a device that left nothing
# and is not named here fails, and a device named here that wrote a file
# fails too.
DEVICE_FLEET_NOFILE='null bbox raster bgr'

# The devices that keep their page as one block of pixels outside virtual
# memory and assemble it in compiled code rather than in PostScript. Two
# questions asked of a running device tell them apart from the rest of
# the fleet, and between them the two partition it: whether the class's
# Emit is an operator, which says the page is put together in C, and
# whether the class carries a band sink, which says it holds pixels at
# all. The four kinds that makes are these, the raster classes written in
# PostScript (Emit a procedure, a band sink), the recorder (Emit an
# operator, no band sink, because it holds marks and not pixels) and the
# devices that keep no raster of their own.
#
# It is worth naming because several wrappers had reached for it
# separately and spelled it out by hand each time: the writers whose
# bands are compared with their whole pages, the page a device will
# refuse at start-up because a pixel's position is the bound, and the
# devices whose band declaration is read from the class. A device added
# to the fleet and to none of those was asked none of their questions.
#
# tests/check-device-roster.sh holds this list by asking each device both
# questions and comparing, both ways round: a device that answers like
# one of these and is not named here fails, and a device named here that
# stops answering that way fails too.
DEVICE_FLEET_BUFFER='raster bgr png pngalpha jpeg'

# fleet_whole DEVICE
#
# The selection that holds DEVICE's page whole: the device itself where
# it does not band, and the mode that turns banding off where it does.
fleet_whole() {
    case " $DEVICE_FLEET_BANDS " in
        *" $1 "*) printf '%s:whole\n' "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

# fleet_each FUNCTION ITEM...
#
# Runs FUNCTION once per item and replays each run's output in the order
# the items were given.
#
# A wrapper that asks one question of every device spends nearly all of
# its time waiting for an interpreter to start, run and exit, and one
# device's answer does not depend on another's, so the runs overlap.
# What a reader sees does not: the output is held per item and replayed
# in roster order, so the device a line belongs to is the device the
# roster reads next, whichever run finished first.
#
# FUNCTION is called with one item and says how it went by its exit
# status: 0 for a device that answered and held, 2 for one that could
# not be asked and has said so in its output, anything else for a
# failure. fleet_asked counts the devices put to the question, held or
# not, and fleet_unasked the ones that could not be; a wrapper holds its
# floor against the first, so a roster that skipped from end to end is
# told apart from one that answered and disagreed. fleet_each returns
# non-zero if any item failed.
#
# The width is FLEET_JOBS, defaulting to four. A wrapper runs beside
# every other test the suite is running at the time, so the width is a
# share of the machine rather than the whole of it.
fleet_each() {
    _fe_body=$1
    shift
    _fe_dir=$(mktemp -d)
    _fe_width=${FLEET_JOBS:-4}
    _fe_live=0
    _fe_i=0

    for _fe_item in "$@"; do
        _fe_i=$((_fe_i + 1))
        (
            "$_fe_body" "$_fe_item" > "$_fe_dir/$_fe_i.out" 2>&1
            echo $? > "$_fe_dir/$_fe_i.st"
        ) &
        _fe_live=$((_fe_live + 1))
        if [ "$_fe_live" -ge "$_fe_width" ]; then
            wait
            _fe_live=0
        fi
    done
    wait

    _fe_rc=0
    _fe_i=0
    fleet_asked=0
    fleet_unasked=0
    fleet_unasked_list=
    for _fe_item in "$@"; do
        _fe_i=$((_fe_i + 1))
        [ -f "$_fe_dir/$_fe_i.out" ] && cat "$_fe_dir/$_fe_i.out"
        # A run whose status never landed is a run that died without
        # reaching the line that records it, which is a failure and not
        # a device declining to answer.
        _fe_st=1
        [ -f "$_fe_dir/$_fe_i.st" ] && _fe_st=$(cat "$_fe_dir/$_fe_i.st")
        case "$_fe_st" in
            2) fleet_unasked=$((fleet_unasked + 1))
               fleet_unasked_list="$fleet_unasked_list $_fe_item" ;;
            0) fleet_asked=$((fleet_asked + 1)) ;;
            *) fleet_asked=$((fleet_asked + 1)); _fe_rc=1 ;;
        esac
    done

    rm -rf "$_fe_dir"
    return $_fe_rc
}

# fleet_hold_unasked EXPECTED
#
# Holds the devices that could not be asked against the ones the caller
# is written for.
#
# A device establishes whether it can answer at all by being run, which
# is what keeps the wrappers from carrying a list of names that drifts
# from the interpreter. It also means a device that has stopped working
# says the same thing as one that was never able to answer: both go
# quiet, and a count of the rest still reads as a whole roster held. So
# the reading is taken, but it is held to what the caller says it should
# be -- a device that goes quiet and is not named here fails, and a
# device named here that has started answering fails too, because the
# reason written beside its name has stopped being true and the check it
# was excused from is now one it could be held to.
fleet_hold_unasked() {
    _fh_want=$(printf '%s\n' $1 | grep . | sort | tr '\n' ' ')
    _fh_got=$(printf '%s\n' $fleet_unasked_list | grep . | sort | tr '\n' ' ')
    [ "$_fh_want" = "$_fh_got" ] && return 0

    for _fh_d in $_fh_got; do
        case " $_fh_want " in
            *" $_fh_d "*) ;;
            *) echo "FAILURES: $_fh_d could not be asked, and nothing here says it cannot answer" ;;
        esac
    done
    for _fh_d in $_fh_want; do
        case " $_fh_got " in
            *" $_fh_d "*) ;;
            *) echo "FAILURES: $_fh_d answered, and it is named here as a device that cannot" ;;
        esac
    done
    return 1
}
