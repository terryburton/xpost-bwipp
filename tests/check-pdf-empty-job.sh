#!/bin/sh
# A job that marks no page produces no PDF, and says so.
#
# A PDF whose page tree is empty is not a document: a consumer reports the
# page count as invalid and stops, so a file written for such a job can only
# disappoint whoever opens it. The output is taken back instead and the run
# reports why, leaving nothing rather than something unreadable.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-pdf-empty-job.sh <xpost binary>}
# the runs below happen in the work directory, so the binary is named from
# where it is and not from where this started
case $xpost in /*) ;; *) xpost=$(pwd)/$xpost ;; esac
. "$(dirname "$0")/guard-paths.sh"
guard_workdir
fail=0

# A program that runs to completion, prints, and marks nothing.
cat > "$work/empty.ps" <<'EOF'
(a job that draws nothing) print
EOF
out=$( cd "$work" && "$xpost" -q -d pdfwrite -o empty.pdf empty.ps </dev/null 2>&1 )

test -e "$work/empty.pdf" \
    && { echo "FAIL: a job that marked no page left a file behind"; fail=1; }

case $out in
    *"no PDF was written"*) : ;;
    *) echo "FAIL: no warning was issued for an empty job"
       echo "      got: $out"; fail=1 ;;
esac

# The program still ran: what it printed is not swallowed by the warning.
case $out in
    *"a job that draws nothing"*) : ;;
    *) echo "FAIL: the program's own output was lost"; fail=1 ;;
esac

# THE CONTROL. A job that marks one page still writes its document, so the
# check above is reading the empty job and not pdfwrite failing outright.
cat > "$work/one.ps" <<'EOF'
0 0 100 100 rectfill
showpage
EOF
( cd "$work" && "$xpost" -q -d pdfwrite -o one.pdf one.ps </dev/null >/dev/null 2>&1 )
test -s "$work/one.pdf" \
    || { echo "FAIL: a job that marked a page wrote no document"; fail=1; }
grep -aq '/Count 1' "$work/one.pdf" \
    || { echo "FAIL: the one-page document does not declare one page"; fail=1; }

# A job whose only page is blank still marked a page, and is a document.
cat > "$work/blank.ps" <<'EOF'
showpage
EOF
( cd "$work" && "$xpost" -q -d pdfwrite -o blank.pdf blank.ps </dev/null >/dev/null 2>&1 )
test -s "$work/blank.pdf" \
    || { echo "FAIL: a job whose page is blank wrote no document"; fail=1; }

test $fail -eq 0 && echo "OK: an empty job writes no PDF and reports it"
exit $fail
