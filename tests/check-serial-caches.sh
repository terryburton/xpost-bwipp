#!/bin/sh
#
# Every cache filed under a serial is given up where its counter restarts.
#
# A cache the interpreter keeps outside virtual memory is filed under a
# number, and that number cannot be minted inside virtual memory: save,
# restore and the job boundary wind such a counter back and the number is
# handed out again while the cache is not wound back with it. Each of
# these counters therefore lives in the library's own storage and only
# moves forward -- until it reaches the end of its range, when it
# restarts.
#
# The restart is the moment every entry still filed under a number about
# to be reissued stops meaning anything. A restart that gives up one
# cache and not another leaves the second holding entries the first
# numbers will match: same serial, and in a long-lived run the same
# entity and the same extent as well. That is the defect the counter was
# introduced to prevent, arriving later and by a different route, and it
# shows only as the wrong shape on a page.
#
# This holds tests/serial-caches to the source in three ways:
#
#   every static counter named for a serial has a line, and every line
#   names a counter that is there
#
#   every file-scope static carrying a serial has a line, and every line
#   naming one in C names a static that is there
#
#   the restart each counter names mentions, by name, every cache the
#   register files under that counter
#
# The third is the one with teeth. A cache in the same file as its
# counter is given up by a call the author was looking at; one in another
# file is given up only if somebody remembered it, and remembering is
# what this replaces.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-serial-caches.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$src/tests/serial-caches" "the serial-cache register"

guard_workdir
guard_held=0

reg="$src/tests/serial-caches"

# --- what the register says ------------------------------------------
awk '$1 == "counter" && NF == 4 { print $2, $3, $4 }' "$reg" > "$work/reg-counters"
awk '$1 == "cache"   && NF == 6 { print $2, $3, $4, $5, $6 }' "$reg" > "$work/reg-caches"

if [ ! -s "$work/reg-counters" ] || [ ! -s "$work/reg-caches" ]; then
    echo "FAILURES: tests/serial-caches states no counters, or no caches, in"
    echo "          the form this reads. Without both there is nothing to"
    echo "          hold the source to."
    exit 1
fi

guard_hold_count "$reg" counters "$(wc -l < "$work/reg-counters")" || :
guard_hold_count "$reg" caches   "$(wc -l < "$work/reg-caches")"   || :

# --- what the source has ---------------------------------------------
#
# Read with comments and string literals removed, so that a counter named
# in prose -- and every one of these is discussed at length above its own
# definition -- is not taken for a second definition of it.
for f in "$src"/src/lib/*.c; do
    rel="src/lib/$(basename "$f")"
    guard_c_source "$f" | awk -v F="$rel" '
        # guard_c_source prefixes each line with its file and number
        { sub(/^[^\t]*\t[0-9]+\t/, "") }
        # a file-scope counter: a static integer whose name says serial
        /^static[ \t]+(int|long|unsigned[ \t]+int|unsigned[ \t]+long)[ \t]+[A-Za-z_0-9]*[Ss]erial[A-Za-z_0-9]*[ \t]*(=|;)/ {
            n = $0
            sub(/^static[ \t]+(unsigned[ \t]+)?(int|long)[ \t]+/, "", n)
            sub(/[ \t]*(=|;).*$/, "", n)
            print "counter", n, F > "/dev/stderr"
            next
        }
        # a file-scope static aggregate: gather it, and if it carries a
        # member called serial, name the instance
        /^static[ \t]+struct[ \t]*\{?[ \t]*$/ { inagg = 1; body = ""; next }
        inagg && /^\}/ {
            inagg = 0
            n = $0
            sub(/^\}[ \t]*/, "", n)
            sub(/[ \t]*(\[[^]]*\])?[ \t]*;.*$/, "", n)
            if (body ~ /(^|[^A-Za-z_])serial([^A-Za-z_0-9]|$)/ && n != "")
                print "cache", n, F > "/dev/stderr"
            next
        }
        inagg { body = body "\n" $0 }
    ' 2>> "$work/found" >/dev/null
done
[ -f "$work/found" ] || : > "$work/found"

awk '$1 == "counter" { print $2, $3 }' "$work/found" > "$work/src-counters"
awk '$1 == "cache"   { print $2, $3 }' "$work/found" > "$work/src-caches"

if [ ! -s "$work/src-counters" ]; then
    echo "FAILURES: no serial counter was found in src/lib, which cannot be"
    echo "          right -- the derivation has stopped matching the source"
    echo "          and would report every register line as stale."
    exit 1
fi

# --- direction one: the counters ------------------------------------
awk '{ print $1, $2 }' "$work/reg-counters" > "$work/want-counters"
guard_hold "$work/want-counters" "$work/src-counters" \
    "tests/serial-caches names a serial counter that src/lib does not define:" \
    "src/lib defines a serial counter that tests/serial-caches does not name. A counter with no line is one whose restart nothing is held to:"

# --- direction two: the caches a scan of src/lib can see --------------
awk '$5 == "derived" { print $1, $2 }' "$work/reg-caches" > "$work/want-caches"
guard_hold "$work/want-caches" "$work/src-caches" \
    "tests/serial-caches calls a cache derived and src/lib does not define it:" \
    "src/lib holds a static carrying a serial that tests/serial-caches does not name. A cache with no line is one nothing gives up when its counter restarts:"

# A cache called opaque or caller because no scan finds it, which a scan
# now finds, is a line written about something that has since changed
# shape -- and the derived direction above is not holding it.
awk '$5 != "derived" { print $1, $2 }' "$work/reg-caches" > "$work/want-unseen"
if [ -s "$work/want-unseen" ]; then
    LC_ALL=C sort -u "$work/want-unseen" > "$work/u1"
    LC_ALL=C sort -u "$work/src-caches" > "$work/u2"
    _both=$(LC_ALL=C comm -12 "$work/u1" "$work/u2")
    if [ -n "$_both" ]; then
        echo "FAILURES: tests/serial-caches says this cache cannot be derived"
        echo "          from src/lib, and it now can be. The line should say"
        echo "          derived, so that the scan holds it:"
        printf '%s\n' "$_both" | sed 's/^/      /'
        guard_held=1
    fi
fi

# --- direction three: the restart names every cache filed under it ----
#
# The body of the function the counter's line names is read for a mention
# of each dropper. A cache whose dropper is not named there is one the
# restart walks past.
while read -r cname cfile crestart; do
    body="$work/body-$cname"
    guard_c_source "$src/$cfile" | awk -v FN="$crestart" '
        { sub(/^[^\t]*\t[0-9]+\t/, "") }
        $0 ~ ("^[A-Za-z_].*[^A-Za-z_0-9]" FN "[ \t]*\\(") && !seen { inside = 1; seen = 1 }
        inside { print; if ($0 ~ /^\}/) inside = 0 }
    ' > "$body"
    if [ ! -s "$body" ]; then
        echo "FAILURES: tests/serial-caches says $cname restarts in $crestart,"
        echo "          and $cfile has no such function. The restart this"
        echo "          holds every cache to is not being read."
        guard_held=1
        continue
    fi
    awk -v C="$cname" '$3 == C { print $1, $4, $5 }' "$work/reg-caches" |
    while read -r cachename dropper how; do
        case $how in
        caller)
            # the restart signals by handing out a low number; naming the
            # dropper here would mean it could reach it, and it cannot
            if command grep -q -- "$dropper" "$body"; then
                echo "FAILURES: $crestart names $dropper, so $cachename is not"
                echo "          given up by the caller reading a restart after"
                echo "          all. The line should say opaque or derived."
                echo held > "$work/held3"
            fi
            if ! command grep -q -- "$dropper" "$src/$(awk -v N="$cachename" '$1 == "cache" && $2 == N { print $3 }' "$reg")"; then
                echo "FAILURES: $cachename is said to be dropped by $dropper in"
                echo "          the file its line names, and that file does not"
                echo "          mention $dropper."
                echo held > "$work/held3"
            fi
            ;;
        *)
            if ! command grep -q -- "$dropper" "$body"; then
                echo "FAILURES: $crestart restarts $cname without giving up"
                echo "          $cachename: it does not name $dropper. Entries left"
                echo "          under a reissued serial are matched by a later one."
                echo held > "$work/held3"
            fi
            ;;
        esac
    done
done < "$work/reg-counters"
[ -f "$work/held3" ] && guard_held=1

if [ "$guard_held" -ne 0 ]; then
    exit 1
fi

printf 'serial caches: %s counters, %s caches, every restart names its own: SUCCESS\n' \
    "$(wc -l < "$work/reg-counters" | tr -d ' ')" \
    "$(wc -l < "$work/reg-caches" | tr -d ' ')"
exit 0
