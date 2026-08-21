#!/bin/sh
# Resources loaded from files on the search path. A category and an instance
# laid out as <dir>/Category/<cat> and <dir>/<cat>/<name> are found by giving
# the interpreter the directory with -I: findresource loads the instance,
# resourcestatus reports it present-on-disk (status 2) before loading and
# in-VM (status 0) after, an absent instance reports false and -- probed
# repeatedly -- is remembered rather than re-searched, and the standard
# categories answer GenericResourceDir / ResourceFileName for where a resource
# would live. The tree is built here so the test needs nothing external.
#   $1  path to the built xpost binary
set -u
xpost=${1:?usage: check-resource-file.sh <xpost binary>}
tree=$(mktemp -d)
prog=$(mktemp)
trap 'rm -rf "$tree" "$prog"' EXIT INT TERM

mkdir -p "$tree/Resource/Category" "$tree/Resource/MyCat"
cat > "$tree/Resource/Category/MyCat" <<'EOF'
/MyCat << /Category /MyCat >> /Category defineresource pop
EOF
cat > "$tree/Resource/MyCat/foo" <<'EOF'
/foo (foo-instance-value) /MyCat defineresource pop
EOF

cat > "$prog" <<'EOF'
/failcount 0 def
/assert { exch { pop } { (FAIL: ) print print (\n) print /failcount failcount 1 add store } ifelse } def

% present on disk, not yet in VM: status 2, size -1
(foo) /MyCat resourcestatus { -1 eq exch 2 eq and }{ false } ifelse
    (an on-disk instance reports status 2 size -1) assert
% findresource loads and runs the file
(foo) /MyCat findresource (foo-instance-value) eq
    (findresource loads the file instance) assert
% now in VM: status 0
(foo) /MyCat resourcestatus { 0 eq exch 0 eq and }{ false } ifelse
    (a loaded instance reports status 0) assert
% an absent instance is false, and stays false when probed repeatedly
(bar) /MyCat resourcestatus not (an absent instance reports false) assert
0 1 40 { pop mark { (bar) /MyCat findresource } stopped pop cleartomark } for
(bar) /MyCat resourcestatus not (an absent instance stays false after repeated probes) assert

% GenericResourceDir reflects the search dir and ends with the separator
currentsystemparams /GenericResourceDir get
    dup (%null) ne exch dup length 0 gt { dup length 1 sub get 47 eq }{ pop false } ifelse and
    (GenericResourceDir is a real directory ending in the separator) assert
currentsystemparams /GenericResourcePathSep get (/) eq
    (GenericResourcePathSep is the slash) assert
% a standard category answers ResourceFileName in the GenericResourceDir layout
/Font /Category findresource begin /Times-Roman 200 string ResourceFileName end
    (Font/Times-Roman) search { pop pop pop true }{ pop false } ifelse
    (ResourceFileName builds the dir/category/name path) assert

failcount 0 eq { (SUCCESS\n) print }{ (FAILURES: ) print failcount 20 string cvs print (\n) print } ifelse
flush
EOF

out=$("$xpost" -q -I "$tree/Resource" "$prog" </dev/null 2>&1)
printf '%s\n' "$out"
case $out in
    *SUCCESS*) exit 0 ;;
    *) echo "check-resource-file: the interpreter did not report SUCCESS"; exit 1 ;;
esac
