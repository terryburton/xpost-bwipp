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

# named absolutely before anything changes directory, since meson names it
# relative to the build directory the run starts in
xpost=$(path_anchor "$xpost")

verdict_workdir

# The token file is named to the interpreter relatively, and the run is made
# from the directory holding it, so the name the program carries is one the
# interpreter can resolve. A path from mktemp cannot be: on Windows the
# interpreter is a native binary and mktemp's is its shell's, so the shell's
# /tmp/... resolves against the current drive instead of the emulation root.
# The argument list is converted for a native program and the file's contents
# are not, so the program ran and the name inside it named nothing -- refused
# with invalidfileaccess, since the sandbox asks whether a path is permitted
# before anything asks whether it exists.
tokfile=over-long-token.txt
prog=token-limit.ps

# a bare name token of seventy thousand characters, past the scan buffer
head -c 70000 /dev/zero | tr '\0' 'a' > "$work/$tokfile"
cat > "$work/$prog" <<EOF
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
out=$(cd "$work" && "$xpost" -q -d null "$prog" </dev/null 2>/dev/null)
st=$?
# A suite that cannot ask its question in this build -- one whose text a
# face answers, under a build carrying no face library -- says so and is a
# skip, not a pass and not a failure. Asked before the success verdict in
# every runner here, because which suites can skip is a property of the
# suites and not of the runner that happens to start them.
verdict_skipped "$out" "the suite"
verdict_ok "$out" "the over-long token run" || exit 1
[ "$st" -eq 0 ] || { echo "FAILURES: the over-long token run exited with status $st"; exit 1; }
