#!/bin/sh
# Meson/make-check wrapper: a token longer than the scanner's buffer is
# refused, not truncated. The scanner reads a token into a fixed buffer
# (NBUF), and on filling it must report the overflow so the reader raises a
# limitcheck. The buffer-filling routine returned a zero count instead,
# which read as an empty token: an over-long name was silently cut to
# nothing and the rest of it read as fresh tokens, the program quietly
# meaning something other than it said. The over-long token is read with
# `file token` from a file, the scanner path such input reaches, since a
# token from a string cannot reach the buffer's length -- a string is
# shorter than the buffer -- and only a file source can.
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

tokfile=$(mktemp)
prog=$(mktemp)
trap 'rm -f "$tokfile" "$prog"' EXIT INT TERM

# a bare name token of seventy thousand characters, past the scan buffer
head -c 70000 /dev/zero | tr '\0' 'a' > "$tokfile"
cat > "$prog" <<EOF
{ ($tokfile) (r) file token } stopped
{ \$error /errorname get /limitcheck eq
    { /hello 42 def hello 42 eq
        { (SUCCESS) = }
        { (FAILURE: a normal token did not scan after the refusal) = } ifelse }
    { (FAILURE: the over-long token was refused with ) print
      \$error /errorname get == } ifelse }
{ (FAILURE: the over-long token was not refused) = } ifelse
EOF

# stdout carries the run's own verdict; the scanner logs the overflow on
# stderr, which is not the run's answer and is kept out of the judgement.
out=$("$xpost" -q -d null "$prog" </dev/null 2>/dev/null)
st=$?
verdict_ok "$out" "the over-long token run" || exit 1
[ "$st" -eq 0 ] || { echo "FAILURES: the over-long token run exited with status $st"; exit 1; }
