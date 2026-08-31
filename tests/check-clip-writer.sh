#!/bin/sh
# Meson test wrapper: the clip has one writer.
#
# The clip is a single value wearing three faces -- the resolved region
# the painters meet against, the shape a device that clips for itself is
# handed, and the cache holding what has been worked out about the
# region. A site that replaces one and not the others leaves the rest
# describing a clip that is no longer in force, and that failure does not
# show up where it is made: the painters go on enforcing the region while
# a vector device writes an older shape into its page, so every
# rasterised render of the same job agrees with itself and nothing says
# anything is wrong. Four sites had drifted that way before the writer
# existed.
#
# So the rule is that .setclipregion in data/clip.ps is the only place
# any of the three is written. Everywhere else the names may only be
# read.
#
# The check is a token scan rather than a line scan because a write need
# not fit on a line: the one this replaced was spelled over three, with
# the dictionary and the key on the first and the `put` on the third. It
# is also why the rule tested is the tight one -- the token after one of
# these names is `get` or `known` -- rather than "no `put` nearby". A
# window big enough to catch a write spread over three lines is big
# enough to catch a `put` that belongs to something else.
#
# The two places the names appear otherwise are cut out before the scan,
# and each is required to be found, so that renaming or deleting one does
# not quietly turn the check off and report success about a tree it never
# looked at.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-clip-writer.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$src/data/clip.ps" "the clip module"
guard_require_file "$src/data/gstate.ps" "the graphics state module"

guard_workdir
fail=0

# Every scan below anchors to the end of a line, so it is run against a
# mirror with the line endings taken out. On a CRLF checkout this check
# used to report SUCCESS over three references where there are eight: the
# range that cuts the writer out never closed, so most of clip.ps went
# with it, taking any violation written there along too.
guard_mirror data "$src"/data/*.ps
data=$mirror

# ---- the writer, and that it really writes all three ----
sed -n '/^\.xpostsys \/\.setclipregion {/,/^} bind put$/p' \
    "$data/clip.ps" > "$work/writer"
guard_require_file "$work/writer" "the .setclipregion writer in data/clip.ps"
for name in clipregion clipsource clipcache; do
    if ! grep -q "/$name exch put" "$work/writer"; then
        echo "FAILURES: the writer does not write /$name"
        fail=1
    fi
done

# ---- the graphics state template, which declares the slots ----
sed -n '/\/\.gstatetemplate <</,/>> def/p' "$data/gstate.ps" \
    > "$work/template"
guard_require_file "$work/template" "the .gstatetemplate literal in data/gstate.ps"
for name in clipregion clipsource clipcache; do
    if ! grep -q "^[[:space:]]*/$name[[:space:]]" "$work/template"; then
        echo "FAILURES: the graphics state template declares no /$name slot"
        fail=1
    fi
done

# ---- everything else: the names may only be read ----
# each file is emitted as one token per line, with the exempt block cut
# out of the two files that have one, and with comments dropped
seen=0
for f in "$data"/*.ps; do
    base=$(basename "$f")
    case $base in
        clip.ps)
            sed '/^\.xpostsys \/\.setclipregion {/,/^} bind put$/d' "$f" ;;
        gstate.ps)
            sed '/\/\.gstatetemplate <</,/>> def/d' "$f" ;;
        *)  cat "$f" ;;
    esac | sed 's/%.*//' | tr -s ' \t\r\n' '\n' > "$work/tok"
    n=$(awk -v file="$base" '
        { tok[NR] = $0 }
        END {
            bad = 0
            for (i = 1; i <= NR; i++) {
                t = tok[i]
                if (t == "/clipregion" || t == "/clipsource" || t == "/clipcache") {
                    seen++
                    nx = (i < NR) ? tok[i + 1] : "(end of file)"
                    if (nx != "get" && nx != "known") {
                        printf "WRITE OUTSIDE THE WRITER: %s %s in data/%s\n", t, nx, file > "/dev/stderr"
                        bad = 1
                    }
                }
            }
            print seen
            exit bad
        }' "$work/tok") || fail=1
    seen=$((seen + n))
done

# a scan that found none of the names is a scan of the wrong tree
if [ "$seen" -eq 0 ]; then
    echo "FAILURES: no reference to the clip slots anywhere in $src/data"
    fail=1
fi

# ---- C may not write them either ----
#
# This half looks for something that must not be there, so it agrees
# with a directory holding nothing exactly as it agrees with a clean
# one: an unmatched glob leaves the loop unentered and the rule reads as
# kept. The sources it opened are counted for that reason, and a count
# near zero is the scan reading the wrong tree.
ncsrc=0
for f in "$src"/src/lib/*.c; do
    [ -f "$f" ] || continue
    ncsrc=$((ncsrc + 1))
    if grep -n 'xpost_dict_put' "$f" \
       | grep -q 'nameclipregion\|nameclipsource\|nameclipcache'; then
        echo "WRITE OUTSIDE THE WRITER: $(basename "$f") puts a clip slot"
        fail=1
    fi
done
if [ "$ncsrc" -lt 40 ]; then
    echo "FAILURES: the C scan read $ncsrc sources under $src/src/lib,"
    echo "      which is not that directory; it holds some fifty"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: the clip is written somewhere other than .setclipregion"
    exit 1
fi
echo "SUCCESS ($seen clip slot references, all reads, $ncsrc C sources read)"
exit 0
