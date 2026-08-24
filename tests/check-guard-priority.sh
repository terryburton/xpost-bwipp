#!/bin/sh
# Guard: every guard test is scheduled ahead of the run.
#
# The guards and the byte-identity render gate are the quick, easily-broken
# checks, and meson.build gives each `priority: guard_priority` so meson
# runs it in the first seconds of any run, where a break is seen at once
# rather than after a suite. A guard added without that line sits at the
# default priority -- behind the slow tests that start early for the clock
# -- and the front-loading quietly stops holding for it. So the rule is a
# build-time check: a test whose program is a tests/check-*.sh guard, or the
# golden-render byte-identity gate, must carry the priority in its block.
#
# Usage: check-guard-priority.sh <source tree root>

set -eu

src=${1:?usage: check-guard-priority.sh <source tree root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_file "$src/meson.build" "the build definition"

report=$(awk '
    # accumulate a test(...) block by paren balance, then judge it
    /^[ \t]*test\(/ { inblk = 1; depth = 0; name = ""; isguard = 0; haspri = 0 }
    inblk {
        if (name == "" && match($0, /test\(.[^,'\'']*/))
            name = substr($0, RSTART + 6, RLENGTH - 6)
        if ($0 ~ /find_program\(.tests\/check-[A-Za-z0-9-]+\.sh.\)/ \
         || $0 ~ /find_program\(.tests\/run-golden-render\.sh.\)/)
            isguard = 1
        if ($0 ~ /priority:[ \t]*guard_priority/)
            haspri = 1
        n = length($0)
        for (i = 1; i <= n; i++) {
            c = substr($0, i, 1)
            if (c == "(") depth++
            else if (c == ")") depth--
        }
        if (depth <= 0) {
            if (isguard && !haspri)
                print "  " name ": a guard test without priority: guard_priority"
            inblk = 0
        }
    }
' "$src/meson.build")

if [ -n "$report" ]; then
    printf '%s\n' "$report"
    echo "FAILURES: a guard is not scheduled first; give it priority: guard_priority"
    exit 1
fi
echo "SUCCESS (every guard and the render gate carry guard_priority)"
exit 0
