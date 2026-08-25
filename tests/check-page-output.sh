#!/bin/sh
#
# Guard: the file a page is written to is named and opened when the page
# is written, and nowhere else.
#
# An output name may carry a %d, which stands for the number of the page
# being written. So the name is not a property of the device: it is a
# property of the page, and it is settled once per page, by the page
# machinery, in .transmitpage (data/device.ps). Every device's Emit then
# writes to what that settled and stored on the device as /.outputfile.
#
# The hazard is a device that reads the output name when it is made
# rather than when a page is written. Such a device opens one file at
# creation and holds it for its whole lifetime, so a job's second page
# overwrites its first; a device that builds an image writer there writes
# into a stream that holds exactly one image, so a second page cannot be
# appended even in principle; and the name it opens is the template, %d
# and all. The devices written in PostScript reach the name the one way
# above, so a guard that held only them would hold half the fleet.
#
# What is held, therefore:
#
#   The substitution has one implementation. .pagefilename is fetched
#   from exactly one place in the interpreter's PostScript, inside
#   .transmitpage, and .transmitpage is defined once.
#
#   Every page goes through it. A device's Emit is reached from
#   .transmitpage and from nowhere else, so a device cannot be given a
#   page without being given the name to write it to.
#
#   No compiled device reads the output name for itself. The key
#   "OutputFileName" is the template, and a device that names it is
#   resolving what the page already resolved.
#
#   No compiled device keeps a stream except for the page it is writing.
#   A FILE * in a device's private struct is a file that outlives the
#   call that opened it, which is what made the second page overwrite
#   the first -- and it is also what a page arriving a band at a time
#   needs, that page being written by several Emit calls in turn
#   (doc/INTERNALS). So the exception is tied to the declaration that
#   earns it: a device keeping a stream says its page may arrive in
#   bands, and gives the stream back.
#
#   A compiled device opens and closes a page's file through the one
#   pair, xpost_device_page_open/xpost_device_page_close, and calls them
#   only from the three functions that can be the last to hold a page's
#   file. Two are the ones its method table registers for the Emit and
#   Destroy slots -- the method that writes a page, and the one that
#   gives back the file of a page that was never finished. The third is
#   the one it registers to run when the block its instance state is
#   kept in is reclaimed, which is all that runs for a device the run
#   never retires: one a restore took back, or one nothing named by the
#   time a collection came round. Such a device reaches no Destroy, so
#   without this its file would be held until the process ended.
#
#   That third is read out of the registration and not from a name, so
#   what earns the permission is having been registered as the reclaim
#   of that block; a function that merely looks like one is outside the
#   rule with everything else. And it may close, never open. Opening
#   reads the settled name off the device dictionary, which is virtual
#   memory a reclaim may not touch (src/lib/xpost_handle.h), so an open
#   there is a call that could not work, and is reported as one that may
#   not be made.
#
# The tests are outside this: a test that drives a device's methods
# one at a time is exercising the device and not transmitting a page, and
# supplies the settled name itself.
#
# Usage: check-page-output.sh <source tree root>

set -eu

src=${1:?usage: check-page-output.sh <source tree root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
# read a tree whose lines end where the scans below expect them to
guard_mirror_tree "$src"
src=$mirror

libdir="$src/src/lib"
datadir="$src/data"
guard_require_dir "$libdir" "the library source directory"
guard_require_dir "$datadir" "the interpreter's PostScript"
guard_require_file "$datadir/device.ps" "the page machinery"
guard_require_file "$libdir/xpost_dev_generic.c" "the shared device helpers"

fail=0

# The extent of one named body, in lines: from the line that opens it to
# the line whose brace closes it. Found rather than assumed -- a rename
# would otherwise put every use outside a range that no longer exists,
# which reads as a clean tree.
#
# A name may be written before it is defined, and a declaration has no
# body: one taken for a body runs from the declaration to the end of
# whatever function comes next, which is a range covering code that is
# not the named function's at all. So a match whose statement ends
# before any brace is passed over and the scan goes on to the definition.
#   $1 the code file (path, line and text separated by tabs, as
#      guard_c_source writes them), $2 the file to look in,
#   $3 an ERE matching the line that opens the body
extent() {
    awk -F'\t' -v f="$2" -v pat="$3" '
        $1 != f { next }
        {
            line = substr($0, length($1) + length($2) + 3)
            if (!started && line ~ pat) { started = 1; first = $2 }
            if (!started) next
            depth += gsub(/\{/, "&", line) - gsub(/\}/, "&", line)
            if (depth > 0) seen = 1
            if (!seen && line ~ /;[ \t]*$/) { started = 0; next }
            if (seen && depth == 0) { print first " " $2; exit }
        }' "$1"
}

# ---------------------------------------------------------------- the
# PostScript: one substitution, and one way to reach an Emit
# ---------------------------------------------------------------------
#
# Read as code: strings are emptied, because an output name holds a
# per-cent and a comment marker inside one would end the line early, and
# comments are dropped, because a brace or a name written in prose is
# neither. Each record is written as path, line and text separated by
# tabs, the shape guard_c_source gives the C, so that a path carrying a
# colon still reads as three parts.
awk '{
    sub(/\r$/, "")
    line = $0; out = ""; i = 1; n = length(line); d = 0
    while (i <= n) {
        c = substr(line, i, 1)
        if (d > 0) {
            if (c == "\\") { i += 2; continue }
            if (c == "(") d++
            else if (c == ")") { d--; if (d == 0) out = out "()" }
            i++; continue
        }
        if (c == "%") break
        if (c == "(") { d = 1; i++; continue }
        out = out c; i++
    }
    print FILENAME "\t" FNR "\t" out
}' "$datadir"/*.ps > "$work/ps"
if [ ! -s "$work/ps" ]; then
    echo "check-page-output: no PostScript read under $src/data" >&2
    exit 1
fi

read -r tstart tend <<EOF
$(extent "$work/ps" "$datadir/device.ps" '/\\.transmitpage[ \t]*\\{')
EOF
if [ -z "${tstart:-}" ]; then
    echo "check-page-output: .transmitpage was not found in data/device.ps." >&2
    echo "The one place a page's output name is settled has been renamed or" >&2
    echo "removed, and this check would report a tree with no page machinery" >&2
    echo "in it as a tree with one." >&2
    exit 1
fi

# defined once, and in the page machinery
ndef=$(awk -F'\t' '{ line = substr($0, length($1) + length($2) + 3)
                  if (line ~ /\/\.transmitpage[ \t]*\{/) n++ }
                END { print n + 0 }' "$work/ps")
if [ "$ndef" -ne 1 ]; then
    echo "check-page-output: .transmitpage is written $ndef times." >&2
    echo "It is one procedure: a second is a second answer to what a page's" >&2
    echo "output is called." >&2
    fail=1
fi

# the substitution is fetched once, inside it
awk -F'\t' -v f="$datadir/device.ps" -v a="$tstart" -v b="$tend" '
    {
        line = substr($0, length($1) + length($2) + 3)
        if (line !~ /\/\/\.pagefilename/ && line !~ /\/\.pagefilename[ \t]+get/) next
        if ($1 == f && $2 >= a && $2 <= b) next
        print $1 ":" $2
    }' "$work/ps" > "$work/pf-outside"
npf=$(awk -F'\t' -v f="$datadir/device.ps" -v a="$tstart" -v b="$tend" '
    {
        line = substr($0, length($1) + length($2) + 3)
        if ($1 == f && $2 >= a && $2 <= b &&
            (line ~ /\/\/\.pagefilename/ || line ~ /\/\.pagefilename[ \t]+get/)) n++
    }
    END { print n + 0 }' "$work/ps")
if [ -s "$work/pf-outside" ]; then
    echo "check-page-output: the page number is substituted outside .transmitpage:" >&2
    sed "s|^$src/||; s|^|  |" "$work/pf-outside" >&2
    echo "A device is handed the settled name in /.outputfile; it does not" >&2
    echo "settle one of its own." >&2
    fail=1
fi
if [ "$npf" -ne 1 ]; then
    echo "check-page-output: .transmitpage fetches .pagefilename $npf times," >&2
    echo "and settling a page's name is one call." >&2
    fail=1
fi

# and every Emit is reached from there, bar the one device whose page is
# another device's page.
#
# The recording device holds no pixels: it writes down the marks a page
# makes and, at Emit, builds a device that paints, plays the marks into
# it and puts out that device's page. So it reaches an Emit that is not
# the one .transmitpage ran, and doing that is the device working. It
# reaches one per band besides, where the page it plays into holds a run
# of the page's rows at a time: such a page is put out once per band and
# once more to finish it (doc/INTERNALS), which is a count this cannot
# have an opinion about.
#
# What the rule is really about it still keeps, and the keeping is
# checked rather than assumed: the name is settled once, for this page,
# by .transmitpage, and the class carries that settled name across to
# the device that writes it. A class reaching a second Emit and settling
# a name of its own -- reading the template under /OutputFileName, or
# substituting a page number again -- is the hazard this guard exists
# for and fails here.
recps="$datadir/recorddev.ps"
awk -F'\t' -v f="$datadir/device.ps" -v a="$tstart" -v b="$tend" -v r="$recps" '
    {
        line = substr($0, length($1) + length($2) + 3)
        if ($1 == r && line ~ /\/\.outputfile[ \t]+get/) carried++
        if ($1 == r && line ~ /OutputFileName/) settles++
        if (line !~ /\/Emit[ \t]+get/) next
        if ($1 == f && $2 >= a && $2 <= b) { n++; next }
        if ($1 == r) { seen = 1; next }
        print $1 ":" $2
    }
    END {
        if (n != 1) print "COUNT " n + 0
        if (seen && (!carried || settles))
            print "CARRY " carried + 0 " " settles + 0
    }' "$work/ps" > "$work/emit"
if grep -q '^CARRY' "$work/emit"; then
    echo "check-page-output: the recording device puts out a page through" >&2
    echo "another device without carrying over the name .transmitpage settled" >&2
    echo "for it, or settles one of its own:" >&2
    sed -n 's|^CARRY \([0-9]*\) \([0-9]*\)|      /.outputfile carried \1 times, /OutputFileName named \2|p' \
        "$work/emit" >&2
    fail=1
fi
if grep -q '^COUNT' "$work/emit"; then
    echo "check-page-output: .transmitpage reaches an Emit $(awk '$1=="COUNT"{print $2}' "$work/emit") times, and it runs one." >&2
    fail=1
fi
if grep -vE '^(COUNT|CARRY)' "$work/emit" | grep -q .; then
    echo "check-page-output: a device's Emit is run outside .transmitpage:" >&2
    grep -vE '^(COUNT|CARRY)' "$work/emit" | sed "s|^$src/||; s|^|  |" >&2
    echo "A page is transmitted through .transmitpage (data/device.ps), which" >&2
    echo "settles the name the page is written to first." >&2
    fail=1
fi

# ---------------------------------------------------------------- the
# compiled devices: no template, no kept stream, one opener
# ---------------------------------------------------------------------
set -- "$libdir"/xpost_dev_*.c
guard_c_source "$@" > "$work/code"
if [ ! -s "$work/code" ]; then
    echo "check-page-output: no device sources read under $src/src/lib" >&2
    exit 1
fi

# 1. the template is the page machinery's to read
awk 'FNR == 1 { file = FILENAME }
     { sub(/\r$/, "")
       if ($0 ~ /"OutputFileName"/) print file ":" FNR }' \
    "$libdir"/xpost_dev_*.c > "$work/template"
if [ -s "$work/template" ]; then
    echo "check-page-output: a compiled device reads the output name template:" >&2
    sed "s|^$src/||; s|^|  |" "$work/template" >&2
    echo "The template may carry a %d and the page number that replaces it is" >&2
    echo "the page's to know. Read /.outputfile, which the page machinery has" >&2
    echo "already settled, through xpost_device_page_open()." >&2
    fail=1
fi

# 2. a stream in the private struct outlives the call that opened it,
#    and may outlive only the call
#
#    A page that arrives a band at a time is written across several Emit
#    calls -- once per band, and once more at the end to say it is
#    finished (doc/INTERNALS) -- so its file cannot be opened and closed
#    within one of them, and the device holds it between them. That is
#    the one reason to hold a stream, and a device that holds one for any
#    other is back at the fault this rule was written for: a file opened
#    once and kept for the device's whole life, over which a job's second
#    page writes its first.
#
#    So the exception is tied to the declaration that earns it. A device
#    keeping a stream must say its page may arrive in bands, which is
#    what makes several Emit calls into one page; and it must name the
#    shared closer, so that the page it holds the file for ends by giving
#    it back rather than by being forgotten.
#
#    And a page whose device the run never retires is not written by an
#    Emit and not retired by a Destroy, so a device holding a stream
#    gives it back from the reclaim of its instance state as well. That
#    is where the rule is checked; here it is only that such a device
#    holds one at all.

# Where a device declares a stream in the struct its instance state is
# kept in, and nothing if it declares none.
#   $1 the device source
privfile() {
    awk -v F="$1" '
        /^typedef struct/ { n = 0; delete buf; inb = 1; next }
        inb && /^\}[ \t]*PrivateData[ \t]*;/ {
            for (i = 1; i <= n; i++)
                if (buf[i] ~ /(^|[^A-Za-z0-9_])FILE([^A-Za-z0-9_]|$)/)
                    printf "%s:%d\n", F, lno[i]
            inb = 0; next
        }
        inb && /^\}/ { inb = 0; next }
        inb { buf[++n] = $0; lno[n] = FNR }
    ' "$1"
}

for f in "$libdir"/xpost_dev_*.c; do
    hits=$(privfile "$f")
    [ -n "$hits" ] || continue
    if ! grep -qE 'xpost_dict_put\(ctx, classdic, xpost_name_cons\(ctx, "BandedPage"\)' "$f"; then
        echo "check-page-output: a device keeps a stream in its private struct" >&2
        echo "and does not say its page may arrive a band at a time:" >&2
        printf '%s\n' "$hits" | sed "s|^$src/||; s|^|  |" >&2
        echo "A page that arrives whole is written by one Emit, which opens its" >&2
        echo "file and closes it; one held past that is one the next page" >&2
        echo "writes over." >&2
        fail=1
    fi
    if ! grep -q 'xpost_device_page_close' "$f"; then
        echo "check-page-output: a device keeps a stream in its private struct" >&2
        echo "and never gives one back:" >&2
        printf '%s\n' "$hits" | sed "s|^$src/||; s|^|  |" >&2
        echo "A page holding the file open ends by closing it." >&2
        fail=1
    fi
done

# 3. the one opener and the one closer
read -r ostart oend <<EOF
$(extent "$work/code" "$libdir/xpost_dev_generic.c" '(^|[^A-Za-z0-9_])xpost_device_page_open[ \t]*\\(')
EOF
read -r cstart cend <<EOF
$(extent "$work/code" "$libdir/xpost_dev_generic.c" '(^|[^A-Za-z0-9_])xpost_device_page_close[ \t]*\\(')
EOF
if [ -z "${ostart:-}" ] || [ -z "${cstart:-}" ]; then
    echo "check-page-output: xpost_device_page_open()/xpost_device_page_close()" >&2
    echo "were not both found in src/lib/xpost_dev_generic.c -- the pair a page's" >&2
    echo "file is opened and closed through has been renamed, and this check" >&2
    echo "would report a tree with no opener in it as a tree with one." >&2
    exit 1
fi

awk -F'\t' -v g="$libdir/xpost_dev_generic.c" \
        -v oa="$ostart" -v ob="$oend" -v ca="$cstart" -v cb="$cend" '
    {
        line = substr($0, length($1) + length($2) + 3)
        if (line !~ /(^|[^A-Za-z0-9_])(xpost_diskfile_fopen|xpost_diskfile_fopen_beneath|fclose)[ \t]*\(/) next
        if ($1 == g && (($2 >= oa && $2 <= ob) || ($2 >= ca && $2 <= cb))) next
        print $1 ":" $2
    }' "$work/code" > "$work/opens"
if [ -s "$work/opens" ]; then
    echo "check-page-output: a device opens or closes a file of its own:" >&2
    sed "s|^$src/||; s|^|  |" "$work/opens" >&2
    echo "A page's file is opened by xpost_device_page_open() and closed by" >&2
    echo "xpost_device_page_close(); those two hold the decision." >&2
    fail=1
fi

# 4. and the pair is called from the Emit slot, in the file's own table
callers=0
reclaimers=0
for f in "$libdir"/xpost_dev_*.c; do
    [ "$f" = "$libdir/xpost_dev_generic.c" ] && continue
    # Each use, and which half of the pair it is: the reclaim below may
    # give a file back and may not ask for one.
    uses=$(awk -F'\t' -v F="$f" '
        $1 != F { next }
        {
            line = substr($0, length($1) + length($2) + 3)
            if (line ~ /(^|[^A-Za-z0-9_])xpost_device_page_open[ \t]*\(/)
                print $2 ":open"
            else if (line ~ /(^|[^A-Za-z0-9_])xpost_device_page_close[ \t]*\(/)
                print $2 ":close"
        }' "$work/code")
    [ -n "$uses" ] || continue
    callers=$((callers + 1))

    # The two slots a page's file belongs to. Emit is where a page is
    # written, and Destroy is where a page that was never finished gives
    # its file back: a device retired part way through one -- by an
    # error, or by a restore past the setpagedevice that installed it --
    # holds the stream of a page it will now never be asked to finish,
    # and nothing else is going to be called on it.
    slotfn() {  # $1 slot name
        awk -v slot="\"$1\"" '{ sub(/\r$/, "")
                    if (index($0, slot) && $0 ~ /\(Xpost_Op_Func\)/) {
                        match($0, /\(Xpost_Op_Func\)[ \t]*[A-Za-z_][A-Za-z0-9_]*/)
                        s = substr($0, RSTART, RLENGTH)
                        sub(/\(Xpost_Op_Func\)[ \t]*/, "", s)
                        print s; exit
                    } }' "$f"
    }
    emitfn=$(slotfn Emit)
    destfn=$(slotfn Destroy)
    if [ -z "$emitfn" ]; then
        echo "check-page-output: ${f#"$src"/} opens a page's file and its method" >&2
        echo "table names no Emit; there is nothing to hold the open to." >&2
        fail=1
        continue
    fi
    read -r estart eend <<EOF
$(extent "$work/code" "$f" "(^|[^A-Za-z0-9_])$emitfn[ \t]*\\\\(")
EOF
    if [ -z "${estart:-}" ]; then
        echo "check-page-output: $emitfn(), the Emit of ${f#"$src"/}, could not be" >&2
        echo "read; the opens below would be held to nothing." >&2
        fail=1
        continue
    fi
    dstart=0; dend=0
    if [ -n "$destfn" ]; then
        read -r dstart dend <<EOF
$(extent "$work/code" "$f" "(^|[^A-Za-z0-9_])$destfn[ \t]*\\\\(")
EOF
        dstart=${dstart:-0}; dend=${dend:-0}
    fi

    # The third: what this file registers to run when the block its
    # instance state is kept in is reclaimed. A device the run never
    # retires reaches no Destroy and nothing else runs for it, so this is
    # where its file goes back.
    #
    # Taken from where the block is issued rather than from a name, so a
    # function is inside this rule because it was named as the block's
    # reclaim and for no other reason. A block is issued and its reclaim
    # named in the one call, so there is one place to read and no way to
    # issue a block without answering this. The call is read whole -- it
    # is written over three lines -- and what it names last is the
    # function.
    recfn=$(awk '{ sub(/\r$/, "")
                   if (buf == "" && !index($0, "xpost_handle_cons")) next
                   buf = buf " " $0
                   if (!index($0, ";")) next
                   sub(/.*xpost_handle_cons[ \t]*\(/, "", buf)
                   sub(/\)[ \t]*;.*$/, "", buf)
                   n = split(buf, a, ",")
                   gsub(/[ \t]+/, "", a[n])
                   if (a[n] ~ /^[A-Za-z_][A-Za-z0-9_]*$/) print a[n]
                   buf = "" }' "$f" | sort -u)
    rstart=0; rend=0
    if [ -n "$recfn" ]; then
        if [ "$(printf '%s\n' "$recfn" | wc -l)" -ne 1 ]; then
            echo "check-page-output: ${f#"$src"/} registers more than one reclaim" >&2
            echo "for its instance state:" >&2
            printf '%s\n' "$recfn" | sed 's/^/  /' >&2
            echo "One block is given up once, so one function gives it up." >&2
            fail=1
            continue
        fi
        read -r rstart rend <<EOF
$(extent "$work/code" "$f" "(^|[^A-Za-z0-9_])$recfn[ \t]*\\\\(")
EOF
        if [ -z "${rstart:-}" ]; then
            echo "check-page-output: $recfn(), the reclaim ${f#"$src"/} registers" >&2
            echo "for its instance state, could not be read; the closes below" >&2
            echo "would be held to nothing." >&2
            fail=1
            continue
        fi
        reclaimers=$((reclaimers + 1))
    fi

    # A device holding a stream between Emit calls gives it back there.
    # Registering a reclaim and leaving the file out of it is the case
    # this whole extent was widened for, and it reads from the outside
    # exactly like a device that never had one.
    if [ -n "$(privfile "$f")" ]; then
        if [ "$rstart" -eq 0 ]; then
            echo "check-page-output: ${f#"$src"/} keeps a page's stream and" >&2
            echo "registers no reclaim for the block it keeps it in. A device" >&2
            echo "the run never retires reaches no Destroy, and its file is then" >&2
            echo "held until the process ends." >&2
            fail=1
        elif ! printf '%s\n' "$uses" | awk -F: -v a="$rstart" -v b="$rend" \
                 '$2 == "close" && $1 >= a && $1 <= b { found = 1 }
                  END { exit !found }'; then
            echo "check-page-output: $recfn(), the reclaim ${f#"$src"/} registers," >&2
            echo "gives back what the block names and not the file. A device the" >&2
            echo "run never retires reaches no Destroy, so this is the last thing" >&2
            echo "that can close it." >&2
            fail=1
        fi
    fi

    for u in $uses; do
        l=${u%:*}
        what=${u##*:}
        if [ "$l" -ge "$estart" ] && [ "$l" -le "$eend" ]; then
            continue
        fi
        if [ "$dstart" -gt 0 ] && [ "$l" -ge "$dstart" ] && [ "$l" -le "$dend" ]; then
            continue
        fi
        if [ "$rstart" -gt 0 ] && [ "$l" -ge "$rstart" ] && [ "$l" -le "$rend" ]; then
            [ "$what" = open ] || continue
            echo "check-page-output: ${f#"$src"/}:$l opens a page's file in" >&2
            echo "$recfn(), the reclaim of this device's instance state." >&2
            echo "A reclaim runs inside the collector and reads nothing in" >&2
            echo "virtual memory, and the name a page is opened under is on the" >&2
            echo "device dictionary. It gives a file back; it does not ask for" >&2
            echo "one." >&2
            fail=1
            continue
        fi
        echo "check-page-output: ${f#"$src"/}:$l opens or closes a page's file" >&2
        echo "outside $emitfn(), the method that writes a page (lines" >&2
        echo "$estart-$eend), outside the Destroy that gives back the file" >&2
        echo "of a page never finished (lines $dstart-$dend), and outside the" >&2
        echo "reclaim of a device the run never retired (lines $rstart-$rend)." >&2
        echo "The name is settled per page, so the file is opened per page." >&2
        fail=1
    done
done

# A scan that found no caller at all found nothing, and a rename that
# made every rule above inert would leave exactly that.
if [ "$callers" -lt 2 ]; then
    echo "check-page-output: $callers compiled devices write a page through the" >&2
    echo "shared opener, and the fleet has more than one that writes a file." >&2
    echo "The scan is reading the wrong thing." >&2
    fail=1
fi

# The same for the third extent, which is reached through a registration
# rather than through a method table and is the one a rename would
# silently retire. A registration nothing found leaves the reclaim
# outside the rule, which is the safe direction and not the intended
# one: every device holding a stream between Emit calls is a device that
# has to give it back where the run never asks.
if [ "$reclaimers" -lt 2 ]; then
    echo "check-page-output: $reclaimers compiled devices give a page's file" >&2
    echo "back from a registered reclaim, and the fleet has more than one that" >&2
    echo "holds a stream. The registration is being read wrong, or a device" >&2
    echo "holding one has stopped registering a reclaim for it." >&2
    fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "check-page-output: ok (one substitution at data/device.ps:$tstart-$tend, $callers compiled devices opening per page, $reclaimers giving a file back on reclaim)"
