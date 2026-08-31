#!/bin/sh
# Guard: every route to the glyph render runs the face setup first.
#
# The glyph cache keys a raster on the face together with the transform
# and size recorded for it when they were installed. The recording is
# done by _face_setup, and only there: neither the function that paints
# a glyph nor the one that walks a string's characters records anything.
# So the render is only ever asked for bytes the key describes because
# every operator that reaches it runs _face_setup on its way in -- an
# arrangement of call sites across several levels of xpost_op_font.c,
# which is an invariant held by nothing unless something checks it. A
# new route that skips the setup does not raise: the face's recorded
# state is stale or absent, and the failure is a glyph keyed on -- and
# later replayed for -- a transform nobody installed. A wrong page, not
# an error.
#
# WHAT IS DERIVED. Every call to xpost_font_face_glyph_render, walked
# upward through its callers. A call is covered where the function it
# sits in calls _face_setup earlier in its own body; a call that is not
# covered makes its function one that must itself only be entered after
# the setup, so every caller of THAT function is examined the same way.
# The walk fails on any route it cannot follow to a covered call: a
# function registered as an operator (the interpreter enters it with no
# setup done), a function whose address is taken, or a function no call
# reaches at all -- each of those is a way into the render this guard
# would otherwise report nothing about.
#
#   $1  path to the source root
set -u
src=${1:?usage: check-glyph-render-route.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir

render=xpost_font_face_glyph_render
setup=_face_setup

# A function that runs the setup on its caller's behalf counts as running
# it, and is named here rather than derived: the walk asks whether a call
# is preceded by the setup, and a wrapper is the one way that can be true
# without the name appearing. Each name is held to actually calling the
# setup below, so an exemption cannot outlive the fact it stands on.
establishers='_font_data_current'

# Read the sources as C -- comments and strings removed, one line each of
# "<path><tab><line><tab><code>" -- and attribute every line to the
# function whose body it is in, named by the last column-zero line that
# opens one. Openers are tagged D (a definition or declaration, whose
# mention of a name is not a call) and body lines B. Tabs inside the code
# are flattened to spaces so the code stays one field.
guard_c_source "$src"/src/lib/*.c "$src"/src/bin/*.c \
| awk -F'\t' -v strip="$src/" '
    {
        file = $1
        line = $2
        code = substr($0, length($1) + length($2) + 3)
        gsub(/\t/, " ", code)
        sub(strip, "", file)
        if (file != prev) { cur = "@NOFN@"; prev = file }
        if (code ~ /^[A-Za-z_].*\(/)
        {
            fn = code
            sub(/\(.*/, "", fn)
            sub(/[ \t]+$/, "", fn)
            sub(/^.*[ \t*]/, "", fn)
            if (fn != "")
            {
                cur = fn
                print file "\t" cur "\t" line "\tD\t" code
                next
            }
        }
        print file "\t" cur "\t" line "\tB\t" code
    }' > "$work/annot"

# Where the setup is run: every call to it, with the function and line it
# sits in. Column-zero openers are its definition and declarations, not
# calls.
# Each name that stands in for the setup has to run it. A wrapper that
# stopped calling the setup would otherwise go on covering every route
# through it, which is the one way this guard could come to say nothing
# while reporting success.
for fn in $establishers; do
    if ! awk -F'\t' -v fn="$fn" -v setup="$setup" '
             $2 == fn && $4 == "B" &&
             $5 ~ ("(^|[^A-Za-z0-9_])" setup "[ \t]*\\(") { found = 1 }
             END { exit !found }' "$work/annot"; then
        echo "FAIL: $fn is named here as running $setup for its callers"
        echo "      and does not call it, so every route through it is"
        echo "      covered by nothing."
        exit 1
    fi
done

setups_re=$setup
for fn in $establishers; do setups_re="$setups_re|$fn"; done

awk -F'\t' -v setup="$setups_re" '
    $4 == "B" && $5 ~ ("(^|[^A-Za-z0-9_])(" setup ")[ \t]*\\(") {
        print $1 "\t" $2 "\t" $3
    }' "$work/annot" > "$work/setups"

if ! grep -q . "$work/setups"; then
    echo "FAIL: nothing calls $setup at all, so no face ever has its"
    echo "      transform and size recorded and the cache key describes"
    echo "      nothing. This guard holds routes to the setup and would"
    echo "      be satisfied by the setup having been deleted."
    exit 1
fi

# The walk. Each function on the list must be entered only after the
# setup has run; the list starts with the render itself. A call to a
# listed function that is preceded, in its own function's body, by a call
# to the setup is covered; one that is not puts its function on the list.
printf '%s\n' "$render" > "$work/todo"
printf '%s\n' "$render" > "$work/seen"
: > "$work/bad"
: > "$work/covered"

while [ -s "$work/todo" ]; do
    fn=$(head -n 1 "$work/todo")
    tail -n +2 "$work/todo" > "$work/todo.rest"
    mv "$work/todo.rest" "$work/todo"

    # Every mention of the function outside its own definition and
    # declarations, classified: a covered call, a call whose function
    # must itself be walked, an operator registration, or a mention that
    # is not a call at all (its address taken -- a route through a
    # pointer this walk cannot follow).
    awk -F'\t' -v n="$fn" '
        NR == FNR { key = $1 "\t" $2
                    if (!(key in first) || $3 + 0 < first[key])
                        first[key] = $3 + 0
                    next }
        $4 == "D" { next }
        $5 ~ ("(^|[^A-Za-z0-9_])" n "([^A-Za-z0-9_]|$)") {
            if ($5 ~ /(^|[^A-Za-z0-9_])xpost_operator_cons[ \t]*\(/)
                { print "cons\t" $2 "\t" $1 "\t" $3; next }
            if ($5 !~ ("(^|[^A-Za-z0-9_])" n "[ \t]*\\("))
                { print "ref\t" $2 "\t" $1 "\t" $3; next }
            key = $1 "\t" $2
            if (key in first && first[key] < $3 + 0)
                print "ok\t" $2 "\t" $1 "\t" $3
            else
                print "up\t" $2 "\t" $1 "\t" $3
        }' "$work/setups" "$work/annot" > "$work/occ"

    if ! grep -q . "$work/occ"; then
        if [ "$fn" = "$render" ]; then
            echo "FAIL: nothing calls $render at all, so no glyph is ever"
            echo "      rasterized. This guard forbids uncovered routes to"
            echo "      it and would be satisfied by there being none."
        else
            echo "FAIL: $fn reaches $render without running $setup, and no"
            echo "      call to $fn was found -- either it is dead, or its"
            echo "      callers are outside this guard's sight. Neither is"
            echo "      a route the guard can pass over."
        fi
        exit 1
    fi

    while IFS="$guard_tab" read -r kind caller file line; do
        case $kind in
        ok)
            printf '%s -> %s at %s:%s\n' "$caller" "$fn" "$file" "$line" \
                >> "$work/covered"
            ;;
        up)
            if [ "$caller" = "@NOFN@" ]; then
                printf '%s reached from a line no function claims, %s:%s\n' \
                    "$fn" "$file" "$line" >> "$work/bad"
            elif ! grep -Fqx "$caller" "$work/seen"; then
                printf '%s\n' "$caller" >> "$work/seen"
                printf '%s\n' "$caller" >> "$work/todo"
                printf '%s enters %s at %s:%s with no earlier %s\n' \
                    "$caller" "$fn" "$file" "$line" "$setup" >> "$work/why"
            fi
            ;;
        cons)
            printf '%s is registered as an operator at %s:%s\n' \
                "$fn" "$file" "$line" >> "$work/bad"
            ;;
        ref)
            printf '%s has its address taken at %s:%s\n' \
                "$fn" "$file" "$line" >> "$work/bad"
            ;;
        esac
    done < "$work/occ"

    if [ -s "$work/bad" ]; then
        echo "FAIL: the render is reachable without the setup having run."
        echo "      A glyph rasterized on such a route is keyed on a"
        echo "      transform and size nobody installed, and the cache"
        echo "      replays it as if they had been: a wrong page, not an"
        echo "      error. The route:"
        sed 's/^/      /' "$work/bad"
        if [ -s "$work/why" ]; then
            echo "      reached because:"
            sed 's/^/      /' "$work/why"
        fi
        echo "      Run $setup on the way in, or reach the render through"
        echo "      a function that does."
        exit 1
    fi
done

covered=$(grep -c . "$work/covered")
walked=$(grep -c . "$work/seen")
echo "SUCCESS ($covered call sites reach $render or a function that"
echo "         relays it, every one after $setup in the same body;"
echo "         $walked functions walked)"
