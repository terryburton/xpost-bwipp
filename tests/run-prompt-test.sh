#!/bin/sh
# Meson test wrapper: the interactive preview prompt digests the operand
# stack into a line of type letters for the executive session (prepr.ps).
# The executive is never entered by the suite -- every test runs a batch
# job -- so the prompt's behaviour would otherwise ship untested. This
# drives the digest directly on a stack holding one of each of ten types,
# the three integer ranges among them, and holds the output to the string
# it must be, so a change to the ti table that still parses is caught.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

prog=$(mktemp)
trap 'rm -f "$prog"' EXIT INT TERM
cat > "$prog" <<'EOF'
mark 1 -5 42 3.14 true (str) /name [1 2] << >> prompt
(\n) print
EOF

out=$("$xpost" -q -d null "$prog" </dev/null 2>&1)
verdict_run "$?" "$out" "the prompt run" || exit 1
printf '%s\n' "$out" | grep -q 'PS<\[1-+rtS/\]D>' || {
    echo "FAILURES: the prompt digest is not the expected PS<[1-+rtS/]D>:"
    printf '%s\n' "$out" | sed 's/^/      /'
    exit 1
}
echo "prompt digest OK"
