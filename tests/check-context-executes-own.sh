#!/bin/sh
# What a context runs must be what that context roots.
#
# An object living in a file-scope variable belongs to whichever context
# was starting when the variable was last written. Every context that
# starts installs the operators again, so a static that is built during
# the install names the newest context -- while each context roots the one
# IT built, in a field of its own. Push such a static onto the execution
# stack and a context runs an object it does not root, and which nothing
# roots at all once the context that built it goes away. The two are the
# same object in a run with one context, which is why the arc procedure
# held up for as long as it did while being wrong.
#
# So: a file-scope Xpost_Object may not be pushed onto the execution
# stack. The context field beside it is what to push -- ctx->arcstartproc
# rather than _arc_start_proc -- because a field of the context is rooted
# by the context that runs it and dies with it.
#
# The population is derived from the source rather than listed, so a
# static added later is covered without this being revisited. Both
# spellings of the declaration are read: the keyword may stand alone on
# its line, and a pattern wanting whitespace after it once made a register
# blind to twelve declarations in this tree.
set -u
src=${1:?usage: check-context-executes-own.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
guard_mirror lib "$src"/src/lib/*.c
lib=$mirror

# every file-scope Xpost_Object, in either spelling
awk '
    /^static[ \t]*$/ { pend = 1; next }
    pend && /^Xpost_Object[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*(=|;)/ {
        nm = $2; sub(/[;=].*/, "", nm); print FILENAME "\t" nm; pend = 0; next
    }
    /^static[ \t]+Xpost_Object[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*(=|;)/ {
        nm = $3; sub(/[;=].*/, "", nm); print FILENAME "\t" nm; next
    }
    { pend = 0 }
' "$lib"/*.c > "$work/statics"

nstat=$(grep -c . < "$work/statics" || true)
if [ "$nstat" -lt 1 ]; then
    echo "FAILURES: no file-scope Xpost_Object was found in src/lib, which this"
    echo "      tree has; the reading is broken rather than the tree"
    exit 1
fi

: > "$work/bad"
while IFS='	' read -r file nm; do
    [ -n "$nm" ] || continue
    # a push of that name onto the execution stack, on one line or split
    tr '\n' ' ' < "$file" \
        | grep -oE "xpost_stack_push\([^;]*->es,[ ]*${nm}[ ]*\)" \
        | sed "s|^|$(basename "$file")	${nm}	|" >> "$work/bad" || true
done < "$work/statics"

if [ -s "$work/bad" ]; then
    echo "FAILURES: a file-scope object is pushed onto the execution stack, so a"
    echo "      context runs what another context built and roots:"
    sed 's/^/      /' "$work/bad"
    echo "      Push the context's own field instead -- the one the install"
    echo "      assigns this static to -- so what runs is what the running"
    echo "      context roots."
    exit 1
fi

printf 'context executes its own: SUCCESS (%s file-scope objects, none run)\n' "$nstat"
exit 0
