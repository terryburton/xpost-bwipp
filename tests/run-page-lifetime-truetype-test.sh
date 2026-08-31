#!/bin/sh
# Meson test wrapper: the page-lifetime steady state holds over a
# TrueType-backed face.
#
# The main page-lifetime run asks whether a multi-page job grows the
# memory a raster comes out of, and asks it through whatever face the
# host resolves the standard names to. Which format that face arrives in
# decides real branches: a Type 1 or CFF file has its charstrings read
# and cached on the first findfont, a TrueType file has its program
# published as sfnts and its glyph names synthesized -- different
# derived objects, cached under different keys, on different pages of
# the job. A cost conditional on one of those branches is invisible on
# every host whose faces take the other, and the hosts where all
# pre-push gates run resolve the standard names to CFF: the defect that
# motivated this wrapper grew the job by one interned name per process,
# on the first findfont to hit the face cache, and only on
# TrueType-backed faces.
#
# So this run pins the face format. A private fontconfig configuration
# is written whose only font directory holds one plain TrueType file
# taken from the host, so every name resolves to that file whatever the
# host's own configuration says. That the pin took is asserted, not
# assumed: the dictionary findfont builds from a plain TrueType file
# states FontType 42, and a run that cannot say so is a run measuring
# the wrong axis and skips -- loudly, never as a pass. A host carrying
# no plain TrueType file skips the same way: the question cannot be
# asked there, and a skip that names its reason is the only honest
# answer.
#
# What is then held is the same steady state the main run holds, on the
# route that needs no recording machinery: the device that paints
# directly is the control that says what flat looks like, and global
# memory read after each page of the job must not grow from page to
# page. The meter is checked first, exactly as in the main run: a
# reading that could not see memory being taken would report every
# route flat and pass everything put in front of it.
#
#   $1  path to the built xpost binary
#   $2  path to page_lifetime_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

ns=$(sandbox_flag "$xpost")

skip() { echo "page-lifetime-truetype: $*"; exit 77; }

# a build without a face library resolves no face at all: the format
# axis this wrapper pins cannot be asked of it, whatever the host's
# fonts say
faceless_build "$xpost" \
    && skip "the build carries no face library, so no face format" \
            "can be pinned; the format axis cannot be asked of it"

command -v fc-list >/dev/null 2>&1 \
    || skip "fc-list is not available, so no TrueType face can be found" \
            "to pin; the format axis cannot be asked on this host"

verdict_workdir

# A plain TrueType file: the sfnt magic, not the file's name. A
# collection or a compressed wrapper is not the program a Type 42
# dictionary carries, and a face from one keeps the Type 1
# presentation, which is the branch every host already runs. The
# roster is walked a line at a time, because a face's file name may
# carry blanks.
fc-list --format '%{file}\n' 2>/dev/null | LC_ALL=C sort -u > "$work/roster"
ttf=
while IFS= read -r f; do
    [ -r "$f" ] || continue
    magic=$(od -An -tx1 -N4 "$f" 2>/dev/null | tr -d ' \n')
    case $magic in
        00010000|74727565) ttf=$f; break ;;
    esac
done < "$work/roster"
[ -n "$ttf" ] || skip "no plain TrueType file among the host's faces;" \
                      "the format axis cannot be asked here"

# The pin: a configuration whose only directory holds the one file, so
# every name resolves to it. The file is copied rather than pointed at,
# so nothing outside the scratch directory is in the configuration.
mkdir -p "$work/fonts" "$work/fccache" || exit 1
cp "$ttf" "$work/fonts/" || exit 1
cat > "$work/fonts.conf" <<CONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>$work/fonts</dir>
  <cachedir>$work/fccache</cachedir>
</fontconfig>
CONF
FONTCONFIG_FILE=$work/fonts.conf
export FONTCONFIG_FILE

# That the pin took: the face the standard name now produces is the
# TrueType program, stated by the dictionary itself. This half fails
# rather than skips -- a plain TrueType file was found above, so a run
# that was not given it is this wrapper's pinning broken, and a skip
# here would retire the whole axis on every host the day the pin
# stopped taking, without a gate going red anywhere.
cat > "$work/fonttype.ps" <<PS
/Times-Roman findfont /FontType get
(FONTTYPE ) print 20 string cvs print (\n) print flush
PS
ftout=$( cd "$work" && "$xpost" -q $ns -d null fonttype.ps </dev/null 2>&1 )
ftype=$(printf '%s\n' "$ftout" | LC_ALL=C sed -n 's/^FONTTYPE \([0-9][0-9]*\)$/\1/p' | tail -1)
if [ "${ftype:-}" != 42 ]; then
    echo "FAILURES: the pinned face did not produce a Type 42 dictionary"
    echo "      (FontType ${ftype:-unreported}) although the host offers a"
    echo "      plain TrueType file, so the pin is broken and the run would"
    echo "      not be measuring the TrueType branch."
    echo "      Face file: $(basename "$ttf")"
    exit 1
fi

fail=0
out=$( cd "$work" && "$xpost" -q $ns -d ppm:whole -o unused.ppm "$script" \
       </dev/null 2>&1 )
st=$?
verdict_run "$st" "$out" "the pinned-face run" || exit 1

lines=$(printf '%s\n' "$out" | tr -s '-' '\n')
field() { printf '%s\n' "$lines" | sed -n "s/^$1 //p"; }

# ---- the meter, before anything is read with it ----
meter=$(field METER | head -1)
asked=$(printf '%s\n' "$meter" | awk '{ print $1 }')
saw=$(printf '%s\n' "$meter" | awk '{ print $2 }')
if [ -z "${asked:-}" ] || [ -z "${saw:-}" ]; then
    echo "FAILURES: the run did not say what the memory reading moved by"
    echo "      when memory was taken, so nothing it reports about memory"
    echo "      can be read"
    exit 1
fi
if [ "$saw" -lt "$asked" ]; then
    note "taking $asked bytes moved the reading by $saw; the meter cannot" \
         "see memory being taken, so this run would pass whatever it was" \
         "given"
else
    echo "OK   taking $asked bytes moves the reading by $saw"
fi

# ---- the steady state, on the whole-page route ----
mem=$(field GVM | awk '$1 == "whole" { print $3 }')
npages=$(printf '%s\n' "$mem" | grep -c .)
if [ "$npages" -lt 3 ]; then
    note "the job reported memory on $npages page(s); a job of one or two" \
         "pages says little about what the page after costs"
else
    lo=$(printf '%s\n' "$mem" | head -1)
    hi=$(printf '%s\n' "$mem" | tail -1)
    echo "OK   held: whole-page job $lo -> $hi bytes over $npages pages," \
         "Type 42 face $(basename "$ttf")"
    if [ "$hi" -ne "$lo" ]; then
        note "over a $npages-page job through a TrueType-backed face the" \
             "job grew by $((hi - lo)) bytes; a cost conditional on the" \
             "TrueType branches of the font machinery shows here and on no" \
             "host whose faces resolve to Type 1 or CFF"
    fi
fi

verdict_exit
