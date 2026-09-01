#!/bin/sh
# Meson test wrapper: %statementedit reads a whole PostScript statement
# from the standard input, however many lines it takes, and hands back a
# file holding it -- the newline that terminated it included, which PLRM
# 3.8.3 makes part of what the file contains.
#
# The special file gathers input until what it has is syntactically
# complete: a statement that opens a procedure, a string or a hexadecimal
# string is not finished at the end of the line, so it keeps reading. The
# nesting it tracks is the only thing that decides where the statement
# ends, so an escaped parenthesis must not close a string.
#
# Nothing else in the suite supplies standard input, so none of this had
# ever run.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
# an absolute path may begin with a drive letter as well as a slash;
# prepending the working directory to one of those makes every
# invocation a path that does not exist
case $xpost in /* | ?:/* | ?:\\*) ;; *) xpost=$PWD/$xpost ;; esac
. "$(dirname "$0")/verdict.sh"

verdict_workdir

# The programs name their output by a relative path and the interpreter
# is run from the directory holding it. A shell and a program built for
# another environment need not read the same absolute path -- a POSIX
# shell driving a native Windows binary is the case here -- and a name
# the shell composed then reached nothing the interpreter could open.
cat > "$work/read.ps" <<'PSEOF'
/f (%statementedit) (r) file def
/s 400 string def
f s readstring pop
/o (got.txt) (w) file def
o exch writestring
o closefile
quit
PSEOF

cat > "$work/readline.ps" <<'PSEOF'
/f (%lineedit) (r) file def
/s 400 string def
f s readstring pop
/o (got.txt) (w) file def
o exch writestring
o closefile
quit
PSEOF

fail=0
run_check() { # program  description  input  expected
    prog=$1; shift
    rm -f "$work/got.txt"
    out=$(printf '%b' "$2" \
          | ( cd "$work" && "$xpost" -q --no-sandbox -d null "$prog" ) 2>&1)
    if ! verdict_run "$?" "$out" "$1"; then
        fail=1
        return
    fi
    printf '%b' "$3" > "$work/want.txt"
    if ! cmp -s "$work/got.txt" "$work/want.txt"; then
        echo "FAIL: $1"
        echo "      want: $(od -c < "$work/want.txt" | head -2 | tr '\n' ' ')"
        echo "      got:  $(od -c < "$work/got.txt" 2>/dev/null | head -2 | tr '\n' ' ')"
        fail=1
    fi
}

lcheck() { run_check readline.ps "$@"; }

check() { # description  input  expected
    rm -f "$work/got.txt"
    out=$(printf '%b' "$2" \
          | ( cd "$work" && "$xpost" -q --no-sandbox -d null read.ps ) 2>&1)
    if ! verdict_run "$?" "$out" "$1"; then
        fail=1
        return
    fi
    printf '%b' "$3" > "$work/want.txt"
    if ! cmp -s "$work/got.txt" "$work/want.txt"; then
        echo "FAIL: $1"
        echo "      want: $(od -c < "$work/want.txt" | head -2 | tr '\n' ' ')"
        echo "      got:  $(od -c < "$work/got.txt" 2>/dev/null | head -2 | tr '\n' ' ')"
        fail=1
    fi
}

check "a statement on one line is that line, with the newline that ended it" \
      '1 2 add\n' '1 2 add\n'
check "a procedure is read until its brace closes" \
      '{ 1 2\nadd }\n' '{ 1 2\nadd }\n'
check "nested procedures close from the inside out" \
      '{ { 1 }\n2 }\n' '{ { 1 }\n2 }\n'
check "a string is read until its parenthesis closes" \
      '(abc\ndef)\n' '(abc\ndef)\n'
# The escape matters at the end of a line: an escaped parenthesis leaves
# the string open, so reading continues onto the next line. Taken as a
# closing parenthesis it would end the statement there instead.
check "an escaped parenthesis leaves the string open across a line" \
      '(a\\)\nb)\n' '(a\\)\nb)\n'
check "a hexadecimal string is read until its bracket closes" \
      '<0102\n0304>\n' '<0102\n0304>\n'
# The one statement with no terminating newline to carry: the standard
# input ended in the middle of it. PLRM 3.8.3 raises undefinedfilename
# only where end-of-file comes before ANY characters were entered, so
# what was entered is what comes back -- and it comes back as it stands.
check "input that ends mid-statement yields what there was, and no newline it never had" \
      '{ 1 2' '{ 1 2'

# %lineedit is the other half of the pair: it reads one line and stops
# there, whatever the line leaves open. A statement that spans lines is
# the statement editor's business, not its.
lcheck "the line editor reads one line" \
       'hello world\n' 'hello world'
lcheck "the line editor drops the newline that ended the line" \
       'abc\ndef\n' 'abc'
lcheck "the line editor stops at the line even with a procedure open" \
       '{ 1 2\nadd }\n' '{ 1 2'
lcheck "the line editor yields what there was without a final newline" \
       'no newline' 'no newline'

# The pair differ on the newline, and only one of them is legislated.
# PLRM 3.8.3 says the file %statementedit returns holds the statement
# "including the terminating end-of-line character", and says nothing of
# the sort about %lineedit, which is described only as returning after a
# single line. So the two are asked the same question here and are
# expected to answer differently: what makes that a rule rather than an
# accident is that the same input goes to both.
check "the statement carries the newline that terminated it" \
      'x\n' 'x\n'
lcheck "the line does not" \
       'x\n' 'x'

[ "$fail" = 0 ] || { echo "FAILURES: the statements above"; exit 1; }
echo "SUCCESS"
