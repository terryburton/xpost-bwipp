#!/bin/sh
# Meson test wrapper: the command line options the interpreter documents.
#
# -g takes a geometry as WIDTHxHEIGHT+XOFFSET+YOFFSET and sets the page
# size to it; a geometry that does not parse is an error rather than
# something to carry on past. -V, -L and -h report and exit.
#
# -D and -I are the two options that hand the job something rather than
# configure the run: -Dname=token defines the name in userdict before the
# program starts, and -I adds a directory the resource machinery searches
# when findresource misses in VM. Each is asked for twice, in the form
# that carries its value attached to the letter and the form that carries
# it as the next argument, and each is asked once more with the option
# left out -- an assertion that a name is defined proves nothing unless
# the same program finds it undefined when nothing defined it.
#
# -o names the file the run's output goes to, and in doing so says the
# invocation is not a person at a keyboard. What that has to mean is
# checked at the end: where the run ends.
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
printf 'showpage\nquit\n' > "$work/blank.ps"

fail=0
note() { echo "FAIL: $1"; fail=1; }

# A run the option was meant to be accepted by is read for the page it
# left and for how it left: an option taken and then rendered from a
# page that the interpreter died over leaves the page behind either way.
render() {  # $1 what to call it in a complaint, $2... the arguments
    r_who=$1
    shift
    # The device is named before the caller's arguments rather than left
    # to the build: what a build with no option named makes is whatever
    # its libraries allowed, which on one machine is a window on the
    # screen the run was started from. A caller naming its own device is
    # naming it after this one and is the device used.
    r_out=$("$xpost" -q --no-sandbox -d null "$@" "$work/blank.ps" </dev/null 2>&1)
    verdict_run "$?" "$r_out" "$r_who" || fail=1
}

# The three options that report and exit. Each says its own thing, and
# each says nothing else: what these write is read by scripts -- a
# package asking the interpreter its version, a build asking whether an
# option exists -- and a report with anything else around it is one a
# reader has to know the shape of to use. So the reports are read on
# their own channel and the other one is required to be empty, and
# --version is held to a single line: a reader taking the version off it
# takes the whole of it, and stays right whatever else the program later
# has to say for itself.
#
# The runs below give the reporting options nothing else, which is how a
# script asks. Standard input is left as this wrapper found it rather
# than redirected, because a report is a report whoever is asking and
# none of the three reads standard input.
report() {  # $1 what to call it, $2 the option
    r_err=$("$xpost" "$2" 2>&1 >"$work/report.out")
    r_st=$?
    got=$(cat "$work/report.out")
    [ "$r_st" -eq 0 ] || note "$2 exited $r_st"
    [ -z "$r_err" ] || note "$2 wrote to the log channel: $r_err"
    [ -n "$got" ] || note "$2 reported nothing"
}

report "the version" --version
lines=$(printf '%s\n' "$got" | wc -l)
[ "$lines" -eq 1 ] || note "--version reported $lines lines, and a reader of it takes one"
printf '%s\n' "$got" | grep -q '[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$' \
    || note "--version does not end in a version: $got"

report "the licence" --license
printf '%s\n' "$got" | grep -qi 'redistribution\|license\|licence\|BSD' \
    || note "--license does not state the licence"

report "the usage" --help
printf '%s\n' "$got" | grep -q -- '--geometry\|-g' \
    || note "--help does not list the geometry option"

# and the other direction: usage the caller did not ask for is a
# complaint, so it goes where complaints go and leaves the output
# channel alone. A script reading the output of a run that mistyped an
# option gets nothing, rather than a page of usage to mistake for one.
bad_out=$("$xpost" --no-such-option 2>/dev/null)
bad_st=$?
bad_err=$("$xpost" --no-such-option 2>&1 >/dev/null)
[ "$bad_st" -eq 0 ] && note "an unknown option was accepted"
[ -n "$bad_out" ] && note "an unknown option put its usage on the output channel"
printf '%s\n' "$bad_err" | grep -q -- '--geometry' \
    || note "an unknown option did not put its usage on the log channel"
printf '%s\n' "$bad_err" | grep -q -- 'no-such-option' \
    || note "an unknown option did not say which option it was"

# a geometry sets the page size: the raster carries its dimensions
render "a 200x100 geometry" -g 200x100+0+0 -d pgm -o "$work/g.pgm"
if [ -f "$work/g.pgm" ]; then
    dim=$(head -c 32 "$work/g.pgm" | LC_ALL=C tr '\n' ' ' | awk '{print $2"x"$3}')
    [ "$dim" = "200x100" ] || note "a geometry of 200x100 produced a page of $dim"
else
    note "a well-formed geometry produced no page at all"
fi

# a geometry that does not parse is refused, and refusing it means not
# rendering a page at some other size
"$xpost" -q --no-sandbox -g 200x100 -d pgm -o "$work/bad.pgm" "$work/blank.ps" \
    </dev/null >/dev/null 2>&1
status=$?
[ "$status" -eq 0 ] && [ -f "$work/bad.pgm" ] \
    && note "a geometry missing its offsets was accepted"

"$xpost" -q --no-sandbox -g nonsense -d pgm -o "$work/junk.pgm" "$work/blank.ps" \
    </dev/null >/dev/null 2>&1
status=$?
[ "$status" -eq 0 ] && [ -f "$work/junk.pgm" ] \
    && note "a geometry that is not a geometry was accepted"

# without -g the default page size stands
render "a run with no geometry" -d pgm -o "$work/def.pgm"
if [ -f "$work/def.pgm" ]; then
    dim=$(head -c 32 "$work/def.pgm" | LC_ALL=C tr '\n' ' ' | awk '{print $2"x"$3}')
    [ "$dim" = "612x792" ] || note "the default page is $dim, not 612x792"
else
    note "no page without a geometry"
fi

# -D reports what it left in userdict, for a name given a token, a name
# given a string and a name given nothing at all
cat > "$work/defs.ps" <<'PSEOF'
/report { % /name  .  -
    dup dup 32 string cvs print (=) print
    userdict exch known { load == }{ (absent) = pop } ifelse
} bind def
/alpha report /beta report /gamma report
quit
PSEOF

# the run's output is left in got rather than handed back through a
# command substitution: a subshell that records a failure records it in a
# copy of the variable and the run it judged passes
run_defs() {  # $1 what to call it, $2... the options before the program
    d_who=$1
    shift
    got=$("$xpost" -q -d null "$@" "$work/defs.ps" </dev/null 2>&1)
    verdict_run "$?" "$got" "$d_who" || fail=1
}

run_defs "a run with three definitions" \
        -Dalpha=42 --define 'beta=(hi)' -Dgamma
printf '%s\n' "$got" | grep -q '^alpha=42$' \
    || note "-Dalpha=42 did not define alpha as 42"
printf '%s\n' "$got" | grep -q '^beta=(hi)$' \
    || note "--define beta=(hi) did not define beta as the string"
printf '%s\n' "$got" | grep -q '^gamma=null$' \
    || note "-Dgamma with no value did not define gamma as null"

# the same program with nothing defining them: the three names are the
# option's doing and not the interpreter's
run_defs "a run with no definitions"
printf '%s\n' "$got" | grep -q '^alpha=absent$' \
    || note "alpha is defined without -D"
printf '%s\n' "$got" | grep -q '^beta=absent$' \
    || note "beta is defined without --define"
printf '%s\n' "$got" | grep -q '^gamma=absent$' \
    || note "gamma is defined without -D"

# -I: an instance the program asks for by name, sitting in a resource
# tree that only the option knows about. Two directories are given so
# that the search reaches past the first, and the run keeps the sandbox
# so that a directory named this way is one the confinement lets in.
mkdir -p "$work/res1/ProcSet" "$work/res2/ProcSet"
printf '/CliProbe << /Greeting (INCLUDE-OK) >> /ProcSet defineresource pop\n' \
    > "$work/res2/ProcSet/CliProbe"
cat > "$work/incs.ps" <<'PSEOF'
{ /CliProbe /ProcSet findresource /Greeting get }
stopped { (include=absent) = }{ (include=) print print (\n) print } ifelse
quit
PSEOF

run_incs() {  # $1 what to call it, $2... the options before the program
    n_who=$1
    shift
    got=$("$xpost" -q -d null "$@" "$work/incs.ps" </dev/null 2>&1)
    verdict_run "$?" "$got" "$n_who" || fail=1
}

run_incs "a run with two include directories" \
        -I "$work/res1" "-I$work/res2"
printf '%s\n' "$got" | grep -q '^include=INCLUDE-OK$' \
    || note "-I did not put the instance's directory on the resource path"

run_incs "a run with no include directory"
printf '%s\n' "$got" | grep -q '^include=absent$' \
    || note "the instance is found without -I"

# an option whose value is the next argument and has no next argument is
# refused rather than taken as empty
for opt in --define --include; do
    "$xpost" -q -d null "$opt" </dev/null >/dev/null 2>&1
    status=$?
    [ "$status" -eq 0 ] && note "$opt with no value was accepted"
done

# Where a run ends. A program named on the command line is a job, and
# PLRM 3.7.7 ends a job where its program ends: the server "executes the
# standard input file until it reaches end-of-file or an error occurs"
# and then closes it. The interactive executive is a separate facility --
# PLRM 2.4.4 has it entered by the executive operator, says it "is
# intended solely for direct interaction with the user", and warns that a
# program "will behave differently when sent through the interactive
# executive than when executed directly by the PostScript interpreter".
# So a run with nobody at a keyboard ends where its program ends, and a
# program that neither calls quit nor fails is a program that ended.
#
# The assertion is not about the prompt. What the executive does before
# printing PS> is read standard input and execute it: bytes a caller sent
# for its own purposes are run as PostScript, after and outside the
# program it asked for. So standard input carries a program that
# announces itself here, and the announcement is what is looked for. A
# prompt is easy to suppress while leaving the reading behind it intact,
# and the reading is the half that does damage; the prompt is checked
# too, and second.
#
# None of this is visible in the usual harness invocation. Standard input
# at /dev/null reaches end-of-file immediately, so the executive exits
# straight away with a zero status and a scripted run looks clean. It is
# a pipe, or a terminal, that shows what is really happening, so the runs
# below are given one or the other.
printf '(program-ran) print flush\n' > "$work/noquit.ps"
# and the same program leaving three objects behind it: an executive
# counts what is on the operand stack into its prompt, so this one would
# be answered with PS<3> rather than PS> and needs asking for separately
printf '(program-ran) print flush\n1 2 3\n' > "$work/leftovers.ps"

# $1 what to call it, $2 the program, $3... the options before it
run_to_end() {
    e_who=$1
    e_prog=$2
    shift 2
    got=$(printf '(STDIN-EXECUTED) print flush\n' \
          | "$xpost" -q --no-sandbox -d null "$@" "$e_prog" 2>&1)
    verdict_run "$?" "$got" "$e_who" || fail=1
    # the program itself has to have run, or the two assertions that
    # follow it are satisfied by an interpreter that did nothing at all
    printf '%s\n' "$got" | grep -q 'program-ran' \
        || note "$e_who did not run the program it was given"
    printf '%s\n' "$got" | grep -q 'STDIN-EXECUTED' \
        && note "$e_who executed what standard input carried after the program"
}

run_to_end "a run with an output file" "$work/noquit.ps" -d null -o "$work/end.null"
printf '%s\n' "$got" | grep -qF 'PS>' \
    && note "a run with an output file was left prompting for input"

run_to_end "a run with an output file leaving a stack" \
        "$work/leftovers.ps" -d null -o "$work/stack.null"
printf '%s\n' "$got" | grep -qF 'PS<' \
    && note "a run with an output file was left prompting over its stack"

# and with no output file named: the executive is for a user, and a
# standard input that is a pipe is not one. This is the same assertion
# without the option, so that -o is what makes the interpreter certain
# rather than what makes it careful.
run_to_end "a run with no output file" "$work/noquit.ps" -d null
printf '%s\n' "$got" | grep -qF 'PS>' \
    && note "a run reading a pipe was left prompting for input"

# The other direction, and the reason the three above are not simply an
# interpreter with its executive removed. PLRM 2.4.4 gives a program one
# way to ask for the executive -- the executive operator -- and asking
# still works, output file and all. A caller that wants to be dropped
# into a session after its program spells it in the language.
printf '(program-ran) print flush\nexecutive\n' > "$work/asksexec.ps"
got=$(printf '(STDIN-EXECUTED) print flush\n' \
      | "$xpost" -q --no-sandbox -d null -o "$work/exec.null" \
                 "$work/asksexec.ps" 2>&1)
verdict_run "$?" "$got" "a run whose program asks for the executive" || fail=1
printf '%s\n' "$got" | grep -q 'STDIN-EXECUTED' \
    || note "a program that invoked executive was given no session"

# And the case -o is really about, which needs a terminal to see. A pipe
# on standard input is not a user by itself, so the runs above end where
# their program ends whether -o was given or not, and every one of them
# would go on passing if -o were dropped from the interpreter entirely.
# What -o says is that the invocation is not interactive even where
# somebody is sitting at a terminal -- there is a file to produce and a
# caller waiting for it -- and only a terminal can be asked that.
#
# script(1) provides one. It is spelt two ways: `script -qec CMD FILE`,
# which util-linux takes, and `script -q FILE CMD ARGS`, which the BSD
# one macOS carries takes. Each is offered a child that reports whether
# it was handed a terminal, and the reply -- not the exit status, which
# not every version passes back -- is what says the form works here.
# Where neither does, the two cases below are not asked and the run says
# so; they are the only ones that cannot be asked without one.
#
# A shell that emulates terminals for its own programs can make one that
# a native program is not handed: the interpreter reads standard input as
# the platform presents it, which on Windows means a console handle, and
# what script(1) provides there is the shell runtime's own pseudo
# terminal -- a pipe to anything built against the platform. `test -t 0`
# in that shell answers for the shell, so the probe below would report a
# terminal the interpreter cannot be given, and the interpreter deciding
# there is nobody at a keyboard would be the right answer read as a
# failure. The two cases are held unasked there, with the reason said.
pty=none
held='script(1) provides no terminal here'
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        held='the terminals here are the shell runtime'\''s, not the platform'\''s' ;;
    *)
        case $(script -qec 'test -t 0 && echo HAVE-TTY' /dev/null </dev/null 2>/dev/null) in
            *HAVE-TTY*) pty=util ;;
            *) case $(script -q /dev/null /bin/sh -c 'test -t 0 && echo HAVE-TTY' \
                      </dev/null 2>/dev/null) in
                   *HAVE-TTY*) pty=bsd ;;
               esac ;;
        esac ;;
esac

# What the terminal is fed is arithmetic rather than a message, because a
# terminal echoes what is typed at it: a marker sent in would come back
# in the output whether anything executed it or not, and the assertion
# would hold over an interpreter that read the line and threw it away.
# The sum is not in what was typed, so it is in the output only if
# something ran it.
printf '1966 1 add ==\n' > "$work/feed.ps"

# The two spellings differ in more than their options. The util-linux
# one runs the command with the feed already on the terminal, so the
# interpreter reads it when it reaches its executive. The BSD one copies
# its own standard input to the terminal as it goes, and sends end of
# file when that input runs out -- so a feed handed to it whole is echoed
# into the terminal before the interpreter has finished starting, and the
# end of file that follows closes the session before anything is read.
# The feed is therefore delivered after the interpreter has had time to
# reach its executive, and the terminal is held open afterwards for long
# enough for what it ran to come back.
on_terminal() {  # $1 the command line to run with a terminal on stdin
    case $pty in
        util) script -qec "$1" /dev/null < "$work/feed.ps" 2>&1 ;;
        bsd)  { sleep 3; cat "$work/feed.ps"; sleep 2; } \
                  | script -q /dev/null /bin/sh -c "$1" 2>&1 ;;
    esac
}

# A page the interpreter announces and waits at, for the terminal cases
# below. The wait is answered by the line the terminal is fed.
printf '%%!PS\nshowpage\n(program-ran) print flush\n' > "$work/showpage.ps"

if [ "$pty" = none ]; then
    echo "held unasked: a run at a terminal, with and without an output"
    echo "      file, and what such a run is told about itself -- $held"
else
    # with -o: a file to produce, so the run ends with the program and
    # what the terminal went on to send is never executed
    got=$(on_terminal "'$xpost' -q --no-sandbox -d null -o '$work/tty.null' '$work/noquit.ps'")
    printf '%s\n' "$got" | grep -q 'program-ran' \
        || note "a run at a terminal with an output file did not run its program"
    printf '%s\n' "$got" | grep -qF '1967' \
        && note "a run at a terminal with an output file executed what was typed after its program"

    # and without it, at the same terminal: the executive follows the
    # program, which is what the option is turning off rather than what
    # the interpreter has stopped doing
    got=$(on_terminal "'$xpost' -q --no-sandbox -d null '$work/noquit.ps'")
    printf '%s\n' "$got" | grep -qF '1967' \
        || note "a run at a terminal with no output file offered no executive"

    # What the interpreter says about itself is said to whoever is there
    # to read it, and the same three things decide that as decide whether
    # a session is offered at all. A session is opened with a greeting.
    got=$(on_terminal "'$xpost' --no-sandbox -d null '$work/noquit.ps'")
    printf '%s\n' "$got" | grep -q '^xpost-BWIPP [0-9]' \
        || note "a session at a terminal was not opened with a greeting"
    printf '%s\n' "$got" | grep -q 'program-ran' \
        || note "a greeted run at a terminal did not run its program"

    # asked for quiet, it is not
    got=$(on_terminal "'$xpost' -q --no-sandbox -d null '$work/noquit.ps'")
    printf '%s\n' "$got" | grep -q '^xpost-BWIPP [0-9]' \
        && note "a run asked for quiet greeted the terminal anyway"

    # and with a file waiting for the run, there is no session to open
    got=$(on_terminal "'$xpost' --no-sandbox -d null -o '$work/tty2.null' '$work/noquit.ps'")
    printf '%s\n' "$got" | grep -q '^xpost-BWIPP [0-9]' \
        && note "a run with an output file greeted the terminal it was started from"

    # The page boundary is the other thing said to a person: the
    # interpreter names the page and waits for a return. It is said at a
    # terminal, where there is somebody to press one --
    got=$(on_terminal "'$xpost' --no-sandbox -d null '$work/showpage.ps'")
    printf '%s\n' "$got" | grep -qF -- '----showpage----' \
        || note "a page shown at a terminal was not announced"

    # -- and not to a run of the same program with nobody there, where
    # the name would land in the middle of what the program is writing
    # and the wait would take a line of whatever the standard input was
    # carrying for its own purposes
    got=$("$xpost" --no-sandbox -d null "$work/showpage.ps" </dev/null 2>/dev/null)
    printf '%s\n' "$got" | grep -qF -- '----showpage----' \
        && note "a page shown with nobody watching was announced on the output channel"
    printf '%s\n' "$got" | grep -q 'program-ran' \
        || note "the unwatched run of the page program did not run it"
fi


# Several programs named on one command line are one job, run in the
# order they were given. The second begins where the first left off:
# what the first defined is still defined, there being no boundary
# between them, which is the whole of the difference between naming two
# files and naming a file that holds both texts.
printf '/carried 42 def\n(one ran) =\n' > "$work/seq1.ps"
printf '(two ran) =\n/carried where { pop (two sees ) print carried == }\n  { (TWO SEES NOTHING) = } ifelse\n' > "$work/seq2.ps"
got=$("$xpost" -q -d null "$work/seq1.ps" "$work/seq2.ps" </dev/null 2>&1)
printf '%s\n' "$got" | grep -qx 'one ran' \
    || note "the first of two programs named did not run"
printf '%s\n' "$got" | grep -qx 'two ran' \
    || note "the second of two programs named did not run"
printf '%s\n' "$got" | grep -qx 'two sees 42' \
    || note "the second program did not begin where the first left off"
[ "$(printf '%s\n' "$got" | grep -n 'ran' | head -1 | cut -d: -f2)" = "one ran" ] \
    || note "the programs did not run in the order they were named"

# A quit takes the interpreter down, so what was named after it is not
# read -- the same as a quit halfway through a single file.
printf '(one ran) =\nquit\n' > "$work/seqq.ps"
got=$("$xpost" -q -d null "$work/seqq.ps" "$work/seq2.ps" </dev/null 2>&1)
status=$?
printf '%s\n' "$got" | grep -qx 'one ran' || note "the quitting program did not run"
printf '%s\n' "$got" | grep -qx 'two ran' \
    && note "a program named after one that quit was read anyway"
[ "$status" = 0 ] || note "a run ended by quit reported failure ($status)"

# An uncaught error ends the job, and ends it for what was named after
# it too, with the run reporting the failure.
printf '(one ran) =\nthisnameisnotdefined\n' > "$work/seqe.ps"
got=$("$xpost" -q -d null "$work/seqe.ps" "$work/seq2.ps" </dev/null 2>&1)
status=$?
printf '%s\n' "$got" | grep -qx 'two ran' \
    && note "a program named after one that failed was read anyway"
[ "$status" = 0 ] \
    && note "a run ended by an uncaught error reported success"

# and one program named is still one program run
got=$("$xpost" -q -d null "$work/seq1.ps" </dev/null 2>&1)
printf '%s\n' "$got" | grep -qx 'one ran' || note "a single program named did not run"

[ "$fail" = 0 ] || { echo "FAILURES: the options above"; exit 1; }
echo "SUCCESS"
