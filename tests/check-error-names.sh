#!/bin/sh
# Meson test wrapper: assert that the interpreter's error names and
# errordict's handlers are the same set.
#
# An error is named twice: once in C, by the ERRORS enumeration in
# src/lib/xpost_error.h, which is what an operator returns and what the
# interpreter looks up; and once in PostScript, by the list data/err.ps
# builds errordict's standard handlers from. PLRM 3.11 has every error
# name appear as a key in errordict and 3.11.2 has the initial VM provide
# standard handlers for all errors, so the two sets have to agree.
#
# Neither half of a disagreement is loud. A C name with no handler is
# reported through the interpreter's fallback instead of through
# errordict, so nothing records it the way a standard handler does and a
# program cannot fetch, wrap and put back an entry that is not there --
# which is what had happened to execstackunderflow, returned from twelve
# places and handled nowhere. A handler for a name C never returns is
# dead weight that reads as coverage.
#
# The register on the PostScript side is not the source text but a live
# startup: errordict is an ordinary dictionary a program can enumerate,
# so what is compared is what the interpreter really holds, however it
# came to hold it. A guard that re-parsed err.ps would report a pass the
# moment the list was spelled a way its patterns did not anticipate.
#
# Two small sets are exempt, each for a stated reason, and staleness in
# either direction is a failure too, so an exemption cannot outlive it.
#
#   $1  path to the xpost binary
#   $2  path to the source tree root
set -u
xpost=${1:?usage: check-error-names.sh <xpost> <srcroot>}
src=${2:?usage: check-error-names.sh <xpost> <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
cr=$(printf '\r')   # tolerate CRLF line endings (Windows checkouts)

# ---- the C side: the names an operator can return ----
awk '
    /^#define ERRORS\(_\)/ { inlist = 1 }
    inlist {
        while (match($0, /_\([A-Za-z][A-Za-z0-9_]*\)/)) {
            print substr($0, RSTART + 2, RLENGTH - 3)
            $0 = substr($0, RSTART + RLENGTH)
        }
        if ($0 !~ /\\[[:space:]]*$/) inlist = 0
    }
' "$src/src/lib/xpost_error.h" | tr -d "$cr" | LC_ALL=C sort -u > "$work/c"

# ---- the PostScript side: what errordict really holds ----
cat > "$work/dump.ps" <<'PSEOF'
errordict { pop dup type /nametype eq
    { (errordict ) print 60 string cvs print (\n) print }{ pop } ifelse } forall
PSEOF
XPOST_DATA_DIR="$src/data" "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/dump.ps" </dev/null 2>/dev/null \
    | tr -d "$cr" | sed -n 's|^errordict ||p' | LC_ALL=C sort -u > "$work/ps"

if [ ! -s "$work/c" ]; then
    echo "FAILURES: no ERRORS entries found in $src/src/lib/xpost_error.h"
    exit 1
fi
if [ ! -s "$work/ps" ]; then
    echo "FAILURES: the interpreter reported no errordict entries"
    exit 1
fi

# ---- exempt, with the reason ----
# C names that are not errors a program can catch: the absence of an
# error, the three requests that ask the interpreter to change the state
# of the execution context rather than report a fault, and the return
# that hands control back to an embedding caller after a page.
cat > "$work/cexempt" <<'EOF'
noerror
contextswitch
ioblock
collectretry
yieldtocaller
EOF
LC_ALL=C sort -u -o "$work/cexempt" "$work/cexempt"

# errordict entries that are not error names: the operator a program calls
# to raise an error, which errordict carries so a handler can reach it;
# and the one error the language raises entirely in PostScript, from the
# resource machinery, with no C site to return it.
cat > "$work/psexempt" <<'EOF'
signalerror
undefinedresource
EOF
LC_ALL=C sort -u -o "$work/psexempt" "$work/psexempt"

fail=0

# Every error the C side can return has a handler, and every handler
# answers to something the C side can return -- each direction less the
# names excused from it, and each exemption list held to the sets in
# turn, so an excuse that describes nothing is reported rather than left
# to cover the next name that lands where it points.
guard_held=0
guard_hold_except "$work/c" "$work/ps" "$work/cexempt" "$work/psexempt" \
    "named in ERRORS, but errordict has no handler: add the name to the
      handler list in data/err.ps:" \
    "handled by errordict and not carried by ERRORS: add it to ERRORS in
      src/lib/xpost_error.h, or drop the handler:"
[ "$guard_held" -eq 0 ] || fail=1

[ "$fail" = 0 ] || exit 1
LC_ALL=C comm -12 "$work/c" "$work/ps" > "$work/both"
echo "SUCCESS ($(wc -l < "$work/both" | tr -d ' ') error names carry a handler)"
exit 0
