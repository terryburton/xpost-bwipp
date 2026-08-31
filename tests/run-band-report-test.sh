#!/bin/sh
# Meson test wrapper: what became of a page, asked rather than inferred.
#
# A page is held whole or a band at a time. Until it was reported, the
# only way for a program -- or a test -- to find out was to reach into
# the device and read the machinery that decided it, which is a check
# agreeing with the thing it is checking. Two things report it now, and
# both are held here:
#
#   currentsystemparams /CurBandHeight, the rows the device holds of its
#   page at once, and zero where it holds the whole of it. A program can
#   ask it and this is what a test asks instead of sniffing.
#
#   One line per page on the standard error under -v, naming the device
#   asked of, the device that paints, whether the page is held whole or
#   in bands, the band, and the page.
#
# What makes either worth anything is that it is the reading the band
# loop went by, so both are held against what actually happened: the
# runs of rows the class's row writer was handed, counted by the run
# itself, and the rows the raster was left holding when the page ended.
# A report that agrees with neither is caught however plausible it is.
#
# The case the two routes cannot state between them is the one this
# exists for. A record whose band budget buys every row of the page
# holds that page whole, so "went through a record" and "was banded" are
# different questions -- and the budget equal to the page, and the
# budget far past it, must both answer that the page was held whole on
# the very route that bands.
#
# And it ends by breaking the reporter itself, three ways, and requiring
# itself to fail each time. A reporter answering with a constant, and a
# reporter naming the wrong route, are exactly the failures a check
# written against a report cannot otherwise tell from a clean tree.
#
#   $1  path to the built xpost binary
#   $2  path to band_report_test.ps
set -u
xpost=$1
script=$2
. "$(dirname "$0")/verdict.sh"

xpost=$(path_anchor "$xpost")
script=$(path_anchor "$script")

datadir=${XPOST_DATA_DIR:-}
if [ -z "$datadir" ]; then
    echo "FAILURES: no XPOST_DATA_DIR; the run has no boot files to sabotage"
    exit 1
fi
datadir=$(path_anchor "$datadir")

verdict_workdir
fail=0

# The page this run draws, which the PostScript states and everything
# below is read against.
PW=200
PH=400

# run DEVICE OUTFILE REPORTFILE [DATADIR] -- one interpreter, quiet.
run() {
    r_dev=$1; r_out=$2; r_rep=$3; r_data=${4:-$datadir}
    XPOST_DATA_DIR=$r_data "$xpost" -q -d "$r_dev" -o "$r_out" "$script" \
        </dev/null >"$r_rep" 2>&1
    return $?
}

# What one CASE line said. field FILE WANT COLUMN
field() {
    awk -v w="$2" -v c="$3" '$1 == "CASE" && $2 == w { print $(c) }' "$1"
}

# check_report FILE ROUTE -- everything one run's CASE lines must say.
# ROUTE is rec or dir and comes from the device this wrapper launched,
# never from the run: the run is the thing being asked.
#
# Columns: CASE want before after runs maxrun sumrows rasterrows
check_report() {
    c_file=$1; c_route=$2
    c_bad=0
    c_seen=0

    if ! grep -q '^DONE' "$c_file"; then
        note "the $c_route run did not finish its cases"
        return 1
    fi

    for want in 1 7 25 $PH 100000; do
        c_before=$(field "$c_file" "$want" 3)
        c_after=$(field "$c_file" "$want" 4)
        c_runs=$(field "$c_file" "$want" 5)
        c_max=$(field "$c_file" "$want" 6)
        c_sum=$(field "$c_file" "$want" 7)
        c_raster=$(field "$c_file" "$want" 8)
        if [ -z "${c_before:-}" ]; then
            note "the $c_route run said nothing about a budget of $want rows"
            c_bad=1
            continue
        fi
        c_seen=$((c_seen + 1))

        # What the page was held in, on this route at this budget. A
        # device that paints holds every page whole whatever is asked of
        # it -- it has no budget to set -- and a record holds the page
        # whole exactly when the budget buys every row of it.
        if [ "$c_route" = dir ] || [ "$want" -ge "$PH" ]; then
            c_wantband=0
        else
            c_wantband=$want
        fi

        if [ "$c_before" != "$c_wantband" ]; then
            note "at a budget of $want rows the $c_route run was told its" \
                 "page would be held in $c_before rows and it is held in" \
                 "$c_wantband"
            c_bad=1
        fi
        if [ "$c_after" != "$c_wantband" ]; then
            note "at a budget of $want rows the $c_route run was told after" \
                 "the page that it was held in $c_after rows and it is" \
                 "$c_wantband"
            c_bad=1
        fi

        # ... against what the row writer was actually handed. This is
        # the half that makes the answer above worth reading: the band
        # the report names must be the deepest run of rows that went to
        # the writer, every row of the page must have gone there once,
        # and the number of runs must be the page divided by the band.
        if [ "$c_sum" != "$PH" ]; then
            note "at a budget of $want rows the $c_route run wrote $c_sum" \
                 "rows of a $PH-row page"
            c_bad=1
        fi
        if [ "$c_wantband" -eq 0 ]; then
            c_expmax=$PH
            c_expruns=1
        else
            c_expmax=$c_wantband
            c_expruns=$(( (PH + c_wantband - 1) / c_wantband ))
        fi
        if [ "$c_max" != "$c_expmax" ]; then
            note "at a budget of $want rows the $c_route run reported a band" \
                 "of $c_before rows and handed its writer runs of up to" \
                 "$c_max rows, where $c_expmax was due"
            c_bad=1
        fi
        if [ "$c_runs" != "$c_expruns" ]; then
            note "at a budget of $want rows the $c_route run handed its" \
                 "writer $c_runs runs of rows, where $c_expruns was due"
            c_bad=1
        fi

        # ... and against what the raster was left holding. A page held
        # whole leaves the raster holding the page; a page put out band
        # by band ends by giving its rows up, so the raster holds
        # nothing. It is a second reading of the same question, taken
        # from the other end.
        if [ "$c_wantband" -eq 0 ]; then
            c_expraster=$PH
        else
            c_expraster=0
        fi
        if [ "$c_raster" != "$c_expraster" ]; then
            note "at a budget of $want rows the $c_route run left its raster" \
                 "holding $c_raster rows, where $c_expraster was due"
            c_bad=1
        fi
    done

    if [ "$c_seen" -lt 5 ]; then
        note "the $c_route run answered $c_seen of 5 budgets; a run that" \
             "answered none would be held to nothing"
        c_bad=1
    fi
    return $c_bad
}

# check_verbose FILE ROUTE -- the line a page leaves on the standard
# error. One per page, and it must name the route the wrapper launched.
check_verbose() {
    v_file=$1; v_route=$2
    v_bad=0
    v_n=$(grep -c '^xpost: page ' "$v_file" 2>/dev/null) || v_n=0
    if [ "$v_n" -ne 5 ]; then
        note "the $v_route run put out 5 pages and left $v_n report lines" \
             "on the standard error"
        return 1
    fi
    # The first page is drawn at a budget of one row, which the record
    # bands and the device that paints does not.
    v_first=$(grep '^xpost: page 1 ' "$v_file" | head -1)
    case $v_route in
        rec)
            case $v_first in
                *", in bands: 1 rows held at once of a ${PW}x${PH} page")
                    echo "OK   a banded page says so: $v_first" ;;
                *) note "a page the record held in bands of one row reported:" \
                        "$v_first"
                   v_bad=1 ;;
            esac
            case $v_first in
                *"painted by .xpost_PGMIMAGE"*) ;;
                *) note "the record's page named no device painting it:" \
                        "$v_first"
                   v_bad=1 ;;
            esac ;;
        dir)
            case $v_first in
                *", whole: ${PH} rows held at once of a ${PW}x${PH} page")
                    echo "OK   a page held whole says so: $v_first" ;;
                *) note "a page a device held whole reported: $v_first"
                   v_bad=1 ;;
            esac ;;
    esac
    return $v_bad
}

# ---- the two routes ----
# The direct route is asked for as the mode that holds the page whole:
# selecting a device by name selects the record in front of it, and a
# wrapper comparing the two routes would otherwise name one of them
# twice.
for r in dir:pgm:whole rec:pgm:band; do
    route=${r%%:*}
    dev=${r#*:}
    run "$dev" "$work/page.$route" "$work/rep.$route"
    st=$?
    verdict_run "$st" "$(cat "$work/rep.$route")" "the $route run" || fail=1
    check_report "$work/rep.$route" "$route" || fail=1
done

if [ "$fail" -eq 0 ]; then
    echo "OK   both routes reported what became of every page"
fi

# ---- the two routes must not answer alike ----
# A reporter answering with a constant passes everything above that a
# clean tree passes on whichever route the constant happens to suit. It
# does not pass this.
if [ -s "$work/rep.dir" ] && [ -s "$work/rep.rec" ]; then
    dircol=$(awk '$1 == "CASE" { print $3 }' "$work/rep.dir" | tr '\n' ' ')
    reccol=$(awk '$1 == "CASE" { print $3 }' "$work/rep.rec" | tr '\n' ' ')
    if [ "$dircol" = "$reccol" ]; then
        note "both routes were told the same thing about their pages" \
             "($dircol); the report does not follow what happened"
    else
        echo "OK   the routes are told apart: whole [$dircol] against" \
             "banded [$reccol]"
    fi
fi

# ---- the same pages, with the report turned on ----
# The report goes to the standard error because the page goes to the
# standard output whenever the run was given nowhere else to put it. So
# the page a verbose run writes must be the page a quiet one writes, to
# the byte, and the lines must be on the other stream.
for r in dir:pgm:whole rec:pgm:band; do
    route=${r%%:*}
    dev=${r#*:}
    XPOST_DATA_DIR=$datadir "$xpost" -v -d "$dev" -o "$work/vpage.$route" \
        "$script" </dev/null >"$work/vout.$route" 2>"$work/verr.$route"
    if [ ! -s "$work/vpage.$route" ]; then
        note "the verbose $route run wrote no page"
        continue
    fi
    if cmp -s "$work/page.$route" "$work/vpage.$route"; then
        echo "OK   the $route page is the same bytes with the report on"
    else
        note "the $route page changed when the report was turned on"
    fi
    if grep -q '^xpost: page ' "$work/vout.$route"; then
        note "the $route run put its page report on the standard output," \
             "where a page it was given no file for would be"
    fi
    check_verbose "$work/verr.$route" "$route" || fail=1
done

# ---- and the report broken on purpose ----
# Everything above reads a report. What none of it can tell from a clean
# tree is a report that has stopped following the run, so each way it
# could is built here and required to be caught. The boot files are
# copied and the copy is broken, so the interpreter under test is the
# one that ships.
sab_data() {  # $1 tag; $2... lines to append to device.ps
    s_dir=$work/data-$1
    rm -rf "$s_dir"
    cp -R "$datadir" "$s_dir" || return 1
    shift
    { echo 'currentglobal true setglobal'
      for s_line in "$@"; do echo "$s_line"; done
      echo 'setglobal'; } >>"$s_dir/device.ps"
    echo "$s_dir"
}

sabotage() {  # $1 what; $2 tag; $3 route; $4 dev; rest: override lines
    b_what=$1; b_tag=$2; b_route=$3; b_dev=$4
    shift 4
    b_dir=$(sab_data "$b_tag" "$@") || { note "could not copy the boot files"; return; }
    run "$b_dev" "$work/sab.page" "$work/sab.rep" "$b_dir"
    if ( check_report "$work/sab.rep" "$b_route" ) >"$work/sab.out" 2>&1; then
        note "$b_what and the check passed anyway; it is not reading the" \
             "report it says it is"
        head -3 "$work/sab.rep" | sed 's/^/      /'
    else
        echo "OK   $b_what is caught"
    fi
}

sabotage "a reporter answering nought whatever happened" const0 rec pgm:band \
    '.xpostsys /.curbandheight { 0 } bind put'
sabotage "a reporter answering with a constant band" const25 rec pgm:band \
    '.xpostsys /.curbandheight { 25 } bind put'
sabotage "a reporter naming a band on a page held whole" wrongdir dir pgm \
    '.xpostsys /.curbandheight { 7 } bind put'

# ... and the line on the standard error, broken the same way.
b_dir=$(sab_data whole \
    '.xpostsys /.reportpage { pop (%stderr) (w) file dup (xpost: page 1 to record, painted by .xpost_PGMIMAGE, whole: 400 rows held at once of a 200x400 page\n) writestring flushfile } bind put')
if [ -n "${b_dir:-}" ]; then
    XPOST_DATA_DIR=$b_dir "$xpost" -v -d pgm:band -o "$work/sab.page" \
        "$script" </dev/null >/dev/null 2>"$work/sab.err"
    if ( check_verbose "$work/sab.err" rec ) >"$work/sab.out" 2>&1; then
        note "a page report naming the wrong route passed the check"
    else
        echo "OK   a page report naming the wrong route is caught"
    fi
fi

verdict_exit
