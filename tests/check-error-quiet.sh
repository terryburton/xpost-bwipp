#!/bin/sh
#
# An ordinary language error says only what the program did wrong.
#
# The interpreter logs internal faults to stderr with the file, line and
# function they came from. That diagnostic is for a fault in this
# interpreter. An error a conforming program can provoke -- an index past
# the end of an array, a name that is not defined, the wrong type for an
# operator -- is not a fault here: it is the answer, and the program is told
# about it through the error machinery. A program that gets both is handed
# interpreter internals it has no use for, and a caller that reads stderr
# cannot tell a real fault from a rangecheck.
#
# So every case below is an error the language defines. Each one is run and
# stderr is searched for the internal marker, which carries a source path and
# a line number. Finding one fails.
#
# The source tree is not an argument: every case here is a program written
# below and run through the interpreter, so there is nothing in the tree for
# this check to read, and taking a path it does not use would let a wrong one
# pass unnoticed.
#
#   $1  path to the xpost binary
set -u
xpost=${1:?usage: check-error-quiet.sh <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_file "$xpost" "the interpreter"

guard_workdir

# The errors, one per line: a name for the report, then the program.
cases=$work/cases
cat > "$cases" <<'EOF'
an array index past the end|[1 2 3] 99 get
an array index below zero|[1 2 3] -1 get
a put past the end of an array|[1 2 3] 99 42 put
a string index past the end|(abc) 99 get
a put past the end of a string|(abc) 99 65 put
a key that is not in the dictionary|3 dict /nope get
a name that is not defined|nosuchname
the wrong type for an operator|(x) 1 add
an operand stack that is empty|pop
a division by zero|1 0 div
a negative count to copy|1 2 -1 copy
a count to roll that is not there|1 2 3 -9 roll
an index past the operand stack|1 2 9 index
an interval outside an array|[1 2 3] 1 99 getinterval
an interval that does not fit|[1 2 3] 1 [9 9 9 9] putinterval
a file operation on a string|(x) closefile
an empty stack for a typed operator|add
an empty stack for a one-operand typed operator|sqrt
an empty stack for a coordinate operator|moveto
an empty stack for a sized operator|string
a string literal that never closes|(abc
a procedure that never closes|{ 1 2
a hex string holding a character that is not one|<4z>
a hex string that never closes|<41
an angle bracket standing alone|< 1
a base-85 string that never closes|<~87cURD]
a radix number wider than an integer|16#FFFFFFFFFFFFFFFFFF
a real outside the representable range|1e400
an ASCII85 stream holding a character it cannot|(v) /ASCII85Decode filter 64 string readstring pop pop
a hex stream holding a character it cannot|(zzz) /ASCIIHexDecode filter 64 string readstring pop pop
a flate stream that is not flate|(abcdefgh) /FlateDecode filter 64 string readstring pop pop
an LZW stream that is not LZW|(abcdefgh) /LZWDecode filter 64 string readstring pop pop
EOF

fail=0
n=0
while IFS='|' read -r name prog; do
    [ -n "$name" ] || continue
    n=$((n + 1))
    printf '%s\n' "$prog" > "$work/case.ps"
    out=$("$xpost" -q --no-sandbox -d null "$work/case.ps" </dev/null 2>&1)
    # the internal marker is the source location the log prints
    leak=$(printf '%s\n' "$out" | grep -oE 'src/lib/[A-Za-z0-9_]*\.c:[0-9]+' | head -1)
    if [ -n "$leak" ]; then
        echo "FAIL: $name is an error the language defines, and the"
        echo "      interpreter also printed an internal diagnostic for it:"
        echo "        program: $prog"
        echo "        from:    $leak"
        fail=1
    fi
    # and the program must still be told, or the case is proving nothing
    case $out in
        *'Error:'*) ;;
        *)  echo "FAIL: $name did not report an error at all, so this case"
            echo "      cannot show whether the diagnostic was quiet:"
            echo "        program: $prog"
            fail=1 ;;
    esac
done < "$cases"

if [ "$n" -lt 10 ]; then
    echo "FAILURES: only $n case(s) were read out of the table; a check that"
    echo "      runs almost nothing proves almost nothing"
    exit 1
fi

[ "$fail" = 0 ] || exit 1
printf 'check-error-quiet: ok (%s language errors, none printing an internal diagnostic)\n' "$n"
exit 0
