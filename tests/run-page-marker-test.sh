#!/bin/sh
# Meson test wrapper: the page-number marker in an output file name, in
# both of the spellings the interpreter reads and in every spelling it
# refuses.
#
# The name a page is written to may say where the page's number goes.
# %d writes the number as it stands and %0Nd writes it in N digits with
# leading zeros; the parse is .pagemarker (data/device.ps) and the
# substitution is .pagefilename beside it.
#
# WHY THIS IS A TEST OF ITS OWN, and not a case added to multipage. The
# marker is a name a PostScript program controls: it arrives from -o on
# a command line and from /OutputFile in a page-device request, and the
# second of those is written by the program being run. So what the
# parse accepts is a language surface, and what it does with everything
# else is the security question -- a name is never handed to anything
# that reads a conversion out of it, which is a property of the
# interpreter and not of any one device. Every case here is about the
# name; nothing here is about a device's format.
#
# BOTH ROUTES. Every acceptance and every refusal is driven twice, once
# through -o and once through a program's own /OutputFile, because they
# are separately reachable: -o binds the run's output before the program
# starts and /OutputFile is a request the program makes, and a rule
# enforced on one and not the other is enforced nowhere that matters.
#
# WHAT IS REFUSED AND WHAT IS LEFT ALONE. A per-cent that opens a field
# width and ends in d is a page number spelled a way this does not read,
# and it is refused where the page is written: %02d used to be taken as
# text, which put every page of the run in one file and said nothing.
# Any other per-cent is left where it stands, because a per-cent opens
# the names the file operator gives the standard streams and a path a
# URL was encoded into carries one. Both halves are asserted: a refusal
# that has quietly become a silent acceptance and an acceptance that has
# quietly become a refusal are the same defect seen from two sides.
#
# THE SANDBOX. The name still goes through the file-access permission
# check, and a padded number must not be a way to build a path that
# leaves the permitted set. The padding puts decimal digits in and
# nothing else, which is asserted on the name that comes out, and a
# program asking for a padded name outside the permitted directory is
# refused -- with the same request inside it as the positive control, so
# a refusal that would have refused everything is not read as the
# sandbox working.
#
#   $1  path to the built xpost binary
set -u
xpost=$1
. "$(dirname "$0")/verdict.sh"

xpost=$(path_anchor "$xpost")
verdict_workdir
fail=0

# Three pages, each a stroke at its own place, so the pages differ from
# one another and a file holding the wrong one can be told from a file
# holding the right one.
pages="$work/pages.ps"
printf '%s\n' \
    'save 10 10 moveto 40 40 lineto stroke showpage restore' \
    'save 40 40 moveto 70 70 lineto stroke showpage restore' \
    'save 70 10 moveto 95 35 lineto stroke showpage restore' > "$pages"

# The same three pages after a page-device request that names the output
# itself. $1 is the name the program asks for; -o still binds something,
# so what is being tested is the request and not the absence of one.
program_named() {   # $1 output name  ; writes $work/named.ps
    {
        printf '<< /OutputDevice /png /OutputFile (%s) >> setpagedevice\n' "$1"
        cat "$pages"
    } > "$work/named.ps"
}

# One run of the three pages, by whichever route. Leaves the run's output
# in $said and its status in $status, and writes nothing itself.
run_o() {           # $1 output name
    said=$( cd "$work" && "$xpost" -q -d png -o "$1" pages.ps </dev/null 2>&1 )
    status=$?
}
run_outputfile() {  # $1 output name
    program_named "$1"
    said=$( cd "$work" && "$xpost" -q -d png -o bound.png named.ps \
            </dev/null 2>&1 )
    status=$?
}

# What the working directory holds, one name per line, less the two
# programs and the name -o binds for the /OutputFile runs.
left() {
    ( cd "$work" && ls -1 2>/dev/null ) \
        | grep -v '^pages\.ps$' | grep -v '^named\.ps$' | grep -v '^bound\.png$'
}

clean() {
    left | while IFS= read -r c_name; do
        [ -n "$c_name" ] && rm -rf -- "$work/$c_name"
    done
}

# ---- a name the interpreter reads: the pages are numbered as it says

accepts() {   # $1 name  $2..$4 the three file names it must leave
    for route in o outputfile; do
        clean
        run_$route "$1"
        verdict_run "$status" "$said" "$1 (via $route)" || { fail=1; continue; }
        if [ -n "$said" ]; then
            note "$1 (via $route) said something on a run that is a page" \
                 "numbered as asked:" "$(printf '%s' "$said" | head -3)"
            continue
        fi
        got=$(left | sort | tr '\n' ' ')
        want=$(printf '%s\n' "$2" "$3" "$4" | sort | tr '\n' ' ')
        if [ "$got" != "$want" ]; then
            note "$1 (via $route) left [$got] and the pages are [$want]"
            continue
        fi
        # three pages that differ, in a name that numbers them: two files
        # holding the same bytes is the counter having been rewound, which
        # is what the numbering is for
        if cmp -s "$work/$2" "$work/$3" || cmp -s "$work/$3" "$work/$4" \
           || cmp -s "$work/$2" "$work/$4"; then
            note "$1 (via $route) numbered its pages and wrote the same page twice"
            continue
        fi
        for p in "$2" "$3" "$4"; do
            [ -s "$work/$p" ] || note "$1 (via $route) left $p empty"
        done
        echo "OK   $1 (via $route): $want"
    done
}

# %d is the spelling that was here before %0Nd was read, and it is kept:
# a page number written as it stands is what a name wants when the run is
# short, and dropping it would move behaviour nothing asked to move. It
# is asserted rather than assumed, so that what happens to it is this
# suite's answer and not the parser's accident.
accepts 'a%d.png'   a1.png a2.png a3.png
accepts 'b%02d.png' b01.png b02.png b03.png
accepts 'c%03d.png' c001.png c002.png c003.png

# The marker need not end the stem, and a name may carry text after it.
accepts 'd%02d-page.png' d01-page.png d02-page.png d03-page.png

# The width is a field and not a limit: a number that does not fit is
# written whole. A three-page run through a one-digit field is the
# smallest case of that, and it must not cut a page's number or collide
# two pages onto one name.
accepts 'e%01d.png' e1.png e2.png e3.png

# A doubled per-cent is not an escape. The name is scanned for a marker
# wherever it stands, so the first per-cent here is text and the second
# opens one; a name is not a format, and there is nothing for an escape
# to protect. Asserted because it is the one place the reading of a
# per-cent depends on what stands before it.
accepts 'z%%d.png' 'z%1.png' 'z%2.png' 'z%3.png'

# ---- past the padding width, where a run outgrows its field

clean
( cd "$work" && printf '%s\n' \
    'save 10 10 moveto 40 40 lineto stroke showpage restore' > one.ps
  i=0
  while [ $i -lt 12 ]; do cat one.ps; i=$((i + 1)); done > twelve.ps )
said=$( cd "$work" && "$xpost" -q -d png -o 'f%02d.png' twelve.ps \
        </dev/null 2>&1 )
status=$?
if ! verdict_run "$status" "$said" 'twelve pages through f%02d.png'; then
    fail=1
else
    got=$(cd "$work" && ls -1 f*.png 2>/dev/null | sort | tr '\n' ' ')
    want='f01.png f02.png f03.png f04.png f05.png f06.png f07.png f08.png f09.png f10.png f11.png f12.png '
    if [ "$got" != "$want" ]; then
        note "twelve pages through f%02d.png left [$got]" "and want [$want]"
    else
        echo "OK   f%02d.png past the field: twelve pages, twelve names"
    fi
fi
( cd "$work" && rm -f one.ps twelve.ps )

# ---- a name the interpreter refuses

# A refusal is two things at once and both are asserted: the run says
# what is wrong, naming the spellings it does read, and it leaves no
# output at all. A refusal that still wrote a file would be the silent
# loss this feature exists to end, wearing a message.
#
# The status and the complaint are read here rather than through
# verdict_run, which judges a run that was meant to succeed: a refused
# name is a run that has to end non-zero and has to complain, and passing
# one to verdict_run would report the test working as the test failing.
refuses() {   # $1 name
    for route in o outputfile; do
        clean
        run_$route "$1"
        if [ "$status" -eq 0 ]; then
            note "$1 (via $route) was accepted, and it is not a page number" \
                 "this reads"
            continue
        fi
        case $said in
            *'%d, or %0Nd'*) ;;
            *) note "$1 (via $route) was refused without naming what a page" \
                    "number is spelled:" "$(printf '%s' "$said" | head -2)"
               continue ;;
        esac
        got=$(left | tr '\n' ' ')
        if [ -n "$got" ]; then
            note "$1 (via $route) was refused and left [$got]"
            continue
        fi
        echo "OK   $1 (via $route): refused, nothing written"
    done
}

# a field width in every spelling this does not read
refuses 'g%2d.png'
refuses 'g%5d.png'
refuses 'g%-5d.png'
refuses 'g%.2d.png'
refuses 'g%*d.png'
refuses 'g%+d.png'
refuses 'g% d.png'
refuses 'g%#d.png'

# the zero flag without a width, and with one that is not a single digit
refuses 'g%0d.png'
refuses 'g%00d.png'

# THE WIDTH BOUND. Nine digits is the whole of it: the number filling the
# field counts pages transmitted, and nine digits is past any run. A
# wider field is refused rather than cut back to fit, since a name cut
# back is a name the run does not write to. Both sides of the bound are
# asserted, so a bound that has moved is caught in whichever direction it
# moved.
accepts 'h%09d.png' h000000001.png h000000002.png h000000003.png
refuses 'h%010d.png'
refuses 'h%99d.png'

# ONE MARKER. A page has one number, and a name offering it two places
# says nothing about which, so a second is refused rather than one of
# them being picked.
refuses 'i%d-%d.png'
refuses 'i%d-%02d.png'
refuses 'i%02d%02d.png'

# ---- a name the interpreter leaves alone
#
# A per-cent that opens no field width is text. That is not a soft
# answer: %n is the conversion that writes through a pointer, and the
# whole of what happens to it here is that it stays in the name and the
# file is called that. Nothing formats, so nothing reads it.
literal() {   # $1 name  $2 the file it must leave
    for route in o outputfile; do
        clean
        run_$route "$1"
        verdict_run "$status" "$said" "$1 (via $route)" || { fail=1; continue; }
        got=$(left | tr '\n' ' ')
        if [ "$got" != "$2 " ]; then
            note "$1 (via $route) left [$got] and the name is text, so it" \
                 "leaves [$2]"
            continue
        fi
        echo "OK   $1 (via $route): text, one file"
    done
}

literal 'j%s.png'      'j%s.png'
literal 'j%n.png'      'j%n.png'
literal 'j%x.png'      'j%x.png'
literal 'j%02x.png'    'j%02x.png'
literal 'j%%.png'      'j%%.png'
literal 'j%20name.png' 'j%20name.png'
literal 'jtrail%'      'jtrail%'

# A name with no marker at all: the documented default, one file, every
# page having written it and the last one standing. Asserted here as well
# as in the multi-page sweep because it is the answer the refusals above
# are measured against -- what a name that says nothing does.
clean
run_o 'k.png'
if ! verdict_run "$status" "$said" 'k.png'; then
    fail=1
else
    got=$(left | tr '\n' ' ')
    [ "$got" = 'k.png ' ] || note "k.png left [$got] and a name with no marker" \
                                  "is one file"
    # and it is the last page and not the first: every page wrote the one
    # name, so what stands there is the third page rendered on its own
    printf '%s\n' 'save 70 10 moveto 95 35 lineto stroke showpage restore' \
        > "$work/third.ps"
    ( cd "$work" && "$xpost" -q -d png -o third.png third.ps </dev/null \
      >/dev/null 2>&1 )
    if cmp -s "$work/k.png" "$work/third.png"; then
        echo "OK   k.png: one file, the last page standing"
    else
        note "k.png holds a page that is not the last of the run"
    fi
    rm -f "$work/third.ps" "$work/third.png"
fi

# ---- the file-access sandbox
#
# What the padding puts into a name is decimal digits and nothing else.
# Asserted on the widest field there is, because that is where a padding
# that could put anything else in would put the most of it: a name that
# came back carrying a dot or a separator it was not given would be a
# path the permission check vetted before the padding made it.
clean
run_o 'l%09d.x'
if [ "$status" -ne 0 ]; then
    note "l%09d.x did not run"
else
    for p in $(left); do
        case $p in
            l[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9].x) ;;
            *) note "l%09d.x produced $p, and a filled field is digits" ;;
        esac
    done
    [ "$(left | wc -l)" -eq 3 ] || note "l%09d.x left $(left | wc -l) files, want 3"
    echo "OK   l%09d.x: the field is filled with digits and nothing else"
fi

# A program asking, under the sandbox, for a padded name outside the
# directory the run was permitted. The run is started in $work with no -o
# naming anything outside it, so the permitted set is that directory; the
# request must be refused and must leave nothing where it pointed.
#
# The positive control is the same request inside the directory. Without
# it a sandbox that refused every padded name would read exactly like a
# sandbox that refused the escape, and the assertion would hold over a
# feature that does not work at all.
clean
esc="$work/esc"
rm -rf "$esc" && mkdir -p "$esc"
printf '<< /OutputDevice /png /OutputFile (../out%%02d.png) >> setpagedevice\n' \
    > "$esc/escape.ps"
printf '%s\n' 'save 10 10 moveto 40 40 lineto stroke showpage restore' \
    >> "$esc/escape.ps"
said=$( cd "$esc" && "$xpost" -q -d png -o inside.png escape.ps </dev/null 2>&1 )
status=$?
if [ "$status" -eq 0 ]; then
    note "a program wrote ../out%02d.png from inside the permitted directory"
elif [ -e "$work/out01.png" ]; then
    note "a program's ../out%02d.png was refused and out01.png was written anyway"
else
    echo "OK   ../out%02d.png: refused, and nothing written outside"
fi
printf '<< /OutputDevice /png /OutputFile (in%%02d.png) >> setpagedevice\n' \
    > "$esc/inside.ps"
printf '%s\n' 'save 10 10 moveto 40 40 lineto stroke showpage restore' \
    >> "$esc/inside.ps"
said=$( cd "$esc" && "$xpost" -q -d png -o bound.png inside.ps </dev/null 2>&1 )
status=$?
if [ "$status" -ne 0 ] || [ ! -s "$esc/in01.png" ]; then
    note "the control for the sandbox case did not write in01.png:" \
         "status $status" "$(printf '%s' "$said" | head -3)"
else
    echo "OK   in%02d.png: written, so the refusal above was the sandbox"
fi

# A padded name outside the directory the run was started in. What the
# run is permitted to write for a name that numbers its pages is that
# name's DIRECTORY, since which pages there will be is not known until
# the program has run and no narrower grant covers them; for a name that
# settles on one file it is that file alone. So a padded name the grant
# did not recognise as numbering its pages is permitted under its
# unexpanded spelling and every page of the run is then refused.
#
# The run is started somewhere else for exactly that reason. Started in
# the directory it writes to, the grant the working directory already
# carries would cover the pages whether the name was recognised or not,
# and the case would pass over a grant that had stopped working.
here="$work/here"
away="$work/away"
rm -rf "$here" "$away" && mkdir -p "$here" "$away"
cp "$pages" "$here/pages.ps"
said=$( cd "$here" && "$xpost" -q -d png -o "$away/m%02d.png" pages.ps \
        </dev/null 2>&1 )
status=$?
got=$(cd "$away" && ls -1 2>/dev/null | sort | tr '\n' ' ')
if ! verdict_run "$status" "$said" 'a padded name outside the run directory'; then
    fail=1
elif [ "$got" != 'm01.png m02.png m03.png ' ]; then
    note "a padded name outside the run directory left [$got]" \
         "and want [m01.png m02.png m03.png]"
else
    echo "OK   $away/m%02d.png: a numbered name is permitted its directory"
fi

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: the page-number marker regressed"
    exit 1
fi
echo "SUCCESS"
exit 0
