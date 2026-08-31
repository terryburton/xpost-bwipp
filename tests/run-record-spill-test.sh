#!/bin/sh
# Meson test wrapper: where a retained page's marks are held.
#
# A page too large for the band budget is held as the marks that made it,
# and where those marks go is settled three ways. The default weighs
# them: while they come to less than the raster banding the page saves
# they stay in memory, and past that they go into a scratch file with no
# name. "never" keeps them in memory whatever they come to and touches no
# scratch file at all. "always" puts them in a file from the first mark.
#
# What this holds:
#
#   the rule fires  a light drawing keeps its marks in memory and a heavy
#                   one puts them in a file, on the same page, chosen by
#                   nothing but how much was drawn
#   the states      each does what it says whatever the drawing, and each
#                   is reported back -- both what was asked for and what
#                   became of it, since a machine that will take no file
#                   makes those two differ and that is the case a reader
#                   most needs told
#   a word that is none of the three is refused naming what was given
#   no scratch      a page under the rule creates no scratch file, which
#                   is observed on the filesystem rather than read off
#                   the code, and "never" reaches the scratch directory
#                   not at all
#   nowhere to put  a scratch directory that takes no file refuses the
#                   state that asked for one outright, and warns and goes
#                   on for the state that would have weighed it
#   the page        every state puts out the same bytes, and the same
#                   bytes as the route that never records. Where a page's
#                   marks were kept is not something a page shows, which
#                   is the requirement the whole mechanism is under.
#
#   $1  path to the built xpost binary
#   $2  path to record_spill_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

datadir=${XPOST_DATA_DIR:-}
if [ -z "$datadir" ]; then
    echo "FAILURES: no XPOST_DATA_DIR; the run has no boot files to break"
    exit 1
fi
datadir=$(path_anchor "$datadir")

verdict_workdir
trap 'chmod u+w "$work/shut" 2>/dev/null; rm -rf "$work"' EXIT
fail=0
checks=0

note() {
    echo "FAILURES: $1"
    shift
    for n_line in "$@"; do
        echo "      $n_line"
    done
    checks=$((checks + 1))
    fail=1
}

ok() {
    echo "OK   $*"
    checks=$((checks + 1))
}

# How much is drawn. The light drawing's marks are far under the raster
# banding this page saves and the heavy one's far past it, so neither
# side of the rule rests on a few bytes either way.
LIGHT=400
HEAVY=202500

# run TAG SCRATCH M [ARGS...] -- one interpreter, with a scratch
# directory of its own so that what it does to one can be watched.
# Judged on what it said and on what it left, which are separate answers.
run() {
    r_tag=$1; r_scratch=$2; r_m=$3
    shift 3
    r_dir=$work/$r_tag
    rm -rf "$r_dir"; mkdir -p "$r_dir" || return 1
    ( cd "$r_dir" && TMPDIR=$r_scratch TMP=$r_scratch TEMPDIR=$r_scratch \
        TEMP=$r_scratch XPOST_DATA_DIR=$datadir \
        "$xpost" -q -d pgm:band -o page.pgm -DM="$r_m" "$@" "$script" \
        </dev/null ) >"$r_dir/out.txt" 2>"$r_dir/err.txt"
    r_st=$?
    laststatus=$r_st
    lastout=$(cat "$r_dir/out.txt" 2>/dev/null)
    return $r_st
}

# ... and whether the run that just went is one this arm can read
# anything off: a run that died on the way out has left every figure
# below it. The stderr is not judged here, because one of the arms below
# requires a warning on it.
judged() {                      # <what to call it>
    verdict_run "$laststatus" "$lastout" "$1"
}

# What one run said, and whether it finished.
said() {
    awk -v k="$2" '$1 == k { print $2 }' "$work/$1/out.txt" | head -1
}
finished() {
    grep -q '^DONE' "$work/$1/out.txt" 2>/dev/null
}

scratch=$work/scratch
mkdir -p "$scratch"

# ---- the rule fires, and only where it should ----
for arm in "light $LIGHT memory" "heavy $HEAVY file"; do
    set -- $arm
    a_tag=$1; a_m=$2; a_want=$3
    if ! run "$a_tag" "$scratch" "$a_m" \
        || ! judged "the $a_tag run" || ! finished "$a_tag"; then
        note "the $a_tag page did not finish" \
             "$(sed -n 1,3p "$work/$a_tag/err.txt" 2>/dev/null)"
        continue
    fi
    a_where=$(said "$a_tag" WHERE)
    a_cost=$(said "$a_tag" COST)
    a_save=$(said "$a_tag" SAVING)
    a_band=$(said "$a_tag" BAND)
    if [ "${a_band:-0}" -le 0 ]; then
        note "the $a_tag page was not held in bands, so nothing here is" \
             "about a retained page"
        continue
    fi
    if [ "$a_where" = "$a_want" ]; then
        ok "$a_tag: $a_cost bytes of marks against the $a_save banding" \
           "saves, held in $a_where"
    else
        note "the $a_tag page holds $a_cost bytes of marks against the" \
             "$a_save banding it saves, and they are in $a_where rather" \
             "than in $a_want"
    fi
done

# and that the two sides were chosen by the drawing and not by the page:
# same page, same band grid, same saving
if [ "$(said light SAVING)" = "$(said heavy SAVING)" ] \
    && [ -n "$(said light SAVING)" ]; then
    ok "both drawings save the same $(said light SAVING) bytes, so what" \
       "chose between them is what was drawn"
else
    note "the two drawings report different savings, so they are not the" \
         "same page and the rule was not what chose between them"
fi

# ---- each state does what it says, whatever the drawing ----
run never-heavy "$scratch" "$HEAVY" -s never
judged "the never run" || :
if finished never-heavy && [ "$(said never-heavy SPILL)" = never ] \
    && [ "$(said never-heavy WHERE)" = memory ]; then
    ok "never: a drawing far past the rule keeps its marks in memory"
else
    note "asked to keep its marks in memory, a heavy page reports" \
         "$(said never-heavy SPILL)/$(said never-heavy WHERE)"
fi

run always-light "$scratch" "$LIGHT" -s always
judged "the always run" || :
if finished always-light && [ "$(said always-light SPILL)" = always ] \
    && [ "$(said always-light WHERE)" = file ]; then
    ok "always: a drawing far under the rule puts its marks in a file"
else
    note "asked to put its marks in a file, a light page reports" \
         "$(said always-light SPILL)/$(said always-light WHERE)"
fi

if [ "$(said light SPILL)" = auto ]; then
    ok "auto: a run that asked for nothing is weighed"
else
    note "a run that named no state reports $(said light SPILL)"
fi

# ---- a word that is none of the three ----
if run sideways "$scratch" "$LIGHT" -s sideways; then
    note "a run naming a way of holding marks that does not exist was" \
         "started anyway"
else
    if grep -q 'sideways' "$work/sideways/err.txt" \
        && grep -q 'auto' "$work/sideways/err.txt"; then
        ok "a state that is none of the three is refused naming what was" \
           "given and what there is"
    else
        note "a state that is none of the three is refused without naming" \
             "what was given:" "$(head -1 "$work/sideways/err.txt")"
    fi
fi

# ---- what each state does to the scratch directory ----
# Observed rather than reasoned about: the file a spill makes is unlinked
# the moment it is made, so it cannot be found by looking in the
# directory afterwards. What can be watched is the calls, where the
# platform offers a way to watch them.
#
# A program of that name is not the question. A machine can carry one
# that watches something else, or that does not take these options, and
# what it writes then names no file at all -- which reads exactly like a
# run that opened none, and is the answer a spill that had stopped
# happening would want. So the tracer is put to a command that certainly
# opens a named file, and is believed only where the trace names it.
tracer=$(command -v strace 2>/dev/null) || tracer=
if [ -n "$tracer" ]; then
    probe=$work/tracer-probe
    mkdir -p "$probe" && : > "$probe/xpost-tracer-probe"
    "$tracer" -f -e trace=file -o "$probe/trace.txt" \
        cat "$probe/xpost-tracer-probe" >/dev/null 2>&1
    grep -q 'xpost-tracer-probe' "$probe/trace.txt" 2>/dev/null || tracer=
fi
if [ -n "$tracer" ]; then
    trace() {                   # <tag> <M> <args...>
        t_tag=$1; t_m=$2
        shift 2
        t_dir=$work/$t_tag
        rm -rf "$t_dir"; mkdir -p "$t_dir" || return 1
        ( cd "$t_dir" && TMPDIR=$scratch TMP=$scratch TEMPDIR=$scratch \
            TEMP=$scratch XPOST_DATA_DIR=$datadir \
            "$tracer" -f -e trace=file -o trace.txt \
            "$xpost" -q -d pgm:band -o page.pgm -DM="$t_m" "$@" "$script" \
            </dev/null ) >"$t_dir/out.txt" 2>"$t_dir/err.txt"
        t_n=$(grep -c 'xpost-spill-' "$t_dir/trace.txt" 2>/dev/null) || t_n=0
        case ${t_n:-x} in *[!0-9]*) t_n=0 ;; esac
        echo "$t_n"
    }
    t_never=$(trace trace-never "$HEAVY" -s never)
    t_light=$(trace trace-light "$LIGHT")
    t_heavy=$(trace trace-heavy "$HEAVY")
    if [ "${t_never:-1}" -eq 0 ]; then
        ok "never: a heavy page named the scratch directory in no call at all"
    else
        note "a run told to touch no scratch file made $t_never calls" \
             "naming one"
    fi
    if [ "${t_light:-0}" -gt 0 ] && [ "${t_heavy:-0}" -gt "${t_light:-0}" ]; then
        ok "auto: a page under the rule opened a scratch file $t_light" \
           "times -- the start-up probe -- against $t_heavy for a page" \
           "past it"
    else
        note "a page under the rule made $t_light calls naming a scratch" \
             "file and a page past it made $t_heavy; the first is the" \
             "start-up probe alone and the second must be more"
    fi
    # and nothing is left behind by any of it
    if [ -z "$(ls -A "$scratch" 2>/dev/null)" ]; then
        ok "no run left anything in the scratch directory"
    else
        note "the scratch directory holds files after every run has ended:" \
             "$(ls -A "$scratch" | tr '\n' ' ')"
    fi
else
    echo "SKIP nothing on this machine reports the files a run opens, so" \
         "what each state does to the scratch directory is not watched"
fi

# ---- a scratch directory that will take no file ----
shut=$work/shut
mkdir -p "$shut"
chmod a-w "$shut" 2>/dev/null || :
if [ -w "$shut" ]; then
    # a run with the power to write anywhere -- root, or a filesystem
    # with no permissions -- cannot be shown a directory that refuses it
    echo "SKIP this run can write into a directory it has no write" \
         "permission on, so a scratch directory that takes no file" \
         "cannot be arranged"
else
    if run shut-always "$shut" "$LIGHT" -s always; then
        note "a run that asked for every page's marks to be in a file was" \
             "started where no file can be made"
    elif grep -q "$shut" "$work/shut-always/err.txt"; then
        ok "always: a run is refused at its device, naming the directory" \
           "it could not write in"
    else
        note "a run that asked for every page's marks to be in a file was" \
             "refused without naming the directory:" \
             "$(head -1 "$work/shut-always/err.txt")"
    fi

    if run shut-auto "$shut" "$HEAVY" && finished shut-auto; then
        if grep -q "$shut" "$work/shut-auto/err.txt"; then
            ok "auto: a run with nowhere to write is warned, naming the" \
               "directory, and puts its page out anyway"
        else
            note "a run with nowhere to write put its page out and said" \
                 "nothing about it"
        fi
        if [ "$(said shut-auto WHERE)" = refused ]; then
            ok "auto: the page says its marks were refused a file rather" \
               "than never having wanted one"
        else
            note "a page whose marks wanted a file and could not have one" \
                 "reports $(said shut-auto WHERE)"
        fi
    else
        note "a run with nowhere to write did not put its page out" \
             "$(sed -n 1,3p "$work/shut-auto/err.txt" 2>/dev/null)"
    fi

    if run shut-never "$shut" "$HEAVY" -s never && finished shut-never; then
        if [ -s "$work/shut-never/err.txt" ]; then
            note "a run that touches no scratch file was told something" \
                 "about a scratch directory:" \
                 "$(head -1 "$work/shut-never/err.txt")"
        else
            ok "never: a run with nowhere to write says nothing, because" \
               "it never asks"
        fi
    else
        note "a run that touches no scratch file did not put its page out"
    fi
fi

# ---- and the page, which is the requirement the mechanism is under ----
# The route that never records is the control: it holds the page whole
# and writes the same bytes, so a difference between any two of these is
# a difference the way the marks were kept made to the page.
whole=$work/whole
rm -rf "$whole"; mkdir -p "$whole"
( cd "$whole" && XPOST_DATA_DIR=$datadir "$xpost" -q -d pgm:whole \
    -o page.pgm -DM="$HEAVY" "$script" </dev/null ) >"$whole/out.txt" 2>&1

same=1
for tag in heavy never-heavy; do
    if [ ! -s "$work/$tag/page.pgm" ]; then
        note "the $tag run wrote no page"
        same=0
        continue
    fi
    cmp -s "$work/$tag/page.pgm" "$whole/page.pgm" || {
        note "the page differs between the $tag run and the route that" \
             "holds the page whole"
        same=0
    }
done
for tag in light always-light; do
    [ -s "$work/$tag/page.pgm" ] || continue
    cmp -s "$work/light/page.pgm" "$work/$tag/page.pgm" || {
        note "the page differs between light and $tag"
        same=0
    }
done
[ "$same" -eq 1 ] && ok "every state puts out the same bytes, and the same" \
                        "bytes as the route that records nothing"

# The count of what was asked, so that a run which asked nothing fails
# rather than reporting a clean tree.
if [ "$checks" -lt 8 ]; then
    note "the wrapper made $checks checks; a run this size makes eight or" \
         "more, so it was not asking what it says it asks"
fi

verdict_exit
