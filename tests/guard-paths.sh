# Sourced by the check-*.sh guards: refuse a path that is not what the
# guard was promised.
#
# A guard that cannot find what it was pointed at must fail, not answer
# about something else. That is not a theoretical worry here.
# XPOST_DATA_DIR is only the first candidate the interpreter tries: if
# init.ps is not there it moves on without complaint, to the directory of
# the shared library and then to data, ../data and ../../data relative to
# wherever it was started. A guard handed the wrong source root therefore
# does not fail -- it finds the working tree by one of those routes and
# reports a perfectly true result about a tree the caller did not mean.
# That is worse than reading something stale, because every number in the
# report is real and nothing about it looks wrong.
#
# Every guard that derives a path from an argument passes it through here
# before use. tests/check-guard-paths.sh holds them to that.

# A tab, as the character itself.
#
# A guard that reads a table of tab-separated rules tells awk so between
# the files it hands it, and an assignment written among awk's file
# operands is taken as a string literal by some awks and left as the two
# characters it was spelt with by others. Where it is left, no line ever
# splits: every rule becomes one field, $2 is empty, and a pass asking
# which rules match reports that none of them do -- a table read as
# entirely stale, which is the same shape as a table that is. The
# separator is therefore passed as the character and not as an escape.
# The trailing period holds it through the substitution, which strips
# newlines and would otherwise be free to strip anything else.
guard_tab=$(printf '\t.'); guard_tab=${guard_tab%.}

guard_require_dir() {
    if [ ! -d "$1" ] || [ ! -r "$1" ]; then
        echo "FAILURES: $2 is not a readable directory: $1"
        exit 1
    fi
}

# -s alone answers yes for a file that cannot be opened, and a guard that
# cannot read its own register reports whatever an empty read gives it,
# which is usually agreement.
guard_require_file() {
    if [ ! -s "$1" ] || [ ! -r "$1" ]; then
        echo "FAILURES: $2 is missing, empty or unreadable: $1"
        exit 1
    fi
}

# The source root a guard is given must be the tree it is meant to read,
# not a subdirectory of it and not the build directory. Naming the
# subdirectory the caller most often passes by mistake makes the failure
# say what went wrong rather than only that something did.
guard_require_srcroot() {
    if [ ! -d "$1" ]; then
        echo "FAILURES: the source root is not a directory: $1"
        exit 1
    fi
    if [ ! -d "$1/data" ] || [ ! -d "$1/tests" ]; then
        echo "FAILURES: not a source root (no data/ and tests/ under it): $1"
        if [ -f "$1/init.ps" ]; then
            echo "      that looks like the data directory itself; pass its parent"
        fi
        exit 1
    fi
    # Two directories of the right names prove nothing: an empty pair
    # passes, and the interpreter then finds the real tree by its own
    # search and answers about that instead -- a true report about a tree
    # nobody asked for. Name a file that only the tree being checked has.
    #
    # Asked for with content, not merely readable: a tree of empty files
    # of the right names is readable throughout, and a guard handed one
    # scans it, finds nothing wrong in it and says so. That is the shape
    # a guard reports agreement in while measuring nothing, and it is
    # what the emptied decoy in check-guard-paths.sh is built to catch.
    if [ ! -s "$1/data/init.ps" ] || [ ! -s "$1/tests/guard-paths.sh" ]; then
        echo "FAILURES: not a source root (data/init.ps and tests/guard-paths.sh must be readable and not empty under it): $1"
        exit 1
    fi
}

# A guard whose scratch directory was never made writes its intermediate
# files to /, reads nothing back, and reports agreement between two empty
# sets. Checked here so every guard that sources this is covered without
# each having to remember.
# Sets `work` in the caller, rather than answering on stdout: an exit
# inside a command substitution ends only the subshell, so a guard that
# wrote `work=$(guard_workdir)` would print the refusal and carry on with
# an empty path -- which is the failure this exists to stop.
guard_workdir() {
    work=$(mktemp -d 2>/dev/null) || work=
    if [ -z "$work" ] || [ ! -d "$work" ] || [ ! -w "$work" ]; then
        echo "FAILURES: could not make a scratch directory (is TMPDIR writable?)"
        exit 1
    fi
    # Removing it is arranged here rather than left to the caller. A trap on
    # EXIT alone does not run when the shell is killed by a signal, and the
    # test runner enforces its time limits with one, so the signals it sends
    # are caught here too: without them a guard that runs long leaves its
    # directory behind, and nothing about the guard says so.
    trap 'rm -rf "$work"' EXIT INT TERM
}

# Mirror text files into the scratch directory with carriage returns
# taken out, and set `mirror` to where they landed. Requires guard_workdir.
#
# A carriage return is a line ending, not content. A checkout that brought
# CRLF in -- which is what git does on Windows by default, and what the
# .gitattributes file exists to stop -- leaves one at the end of every
# line, where it makes `$` match nothing: a sed range then never closes
# and runs to the end of the file, a grep for a whole line finds none, and
# the guard reports about a fraction of the tree without saying so. One
# guard went green that way with five sixths of its population missing.
# Guards read the mirror, so they hold the same rule on either checkout.
guard_mirror() {
    mirror="$work/mirror-$1"
    shift
    if ! mkdir -p "$mirror"; then
        echo "FAILURES: could not make a scratch directory under $work"
        exit 1
    fi
    for f in "$@"; do
        [ -f "$f" ] || continue
        tr -d '\r' < "$f" > "$mirror/$(basename "$f")"
    done
}

# The same, for a guard that reads across the tree rather than one
# directory: mirrors the source root and sets `mirror` to the copy, which
# the guard then uses as its source root. Requires guard_workdir.
#
# A corpus is left out. Its programs are fetched and belong to their own
# sources, no guard scans them, and its scratch directory is written
# while the corpus tests run -- a walk that copies it races them and
# dies on a file that went away between being listed and being read.
# Which paths those are is stated once, in tests/corpus/.gitignore; that
# file is distributed, so a tarball states it too.
guard_mirror_tree() {
    mirror="$work/tree"
    if ! mkdir -p "$mirror"; then
        echo "FAILURES: could not make a scratch directory under $work"
        exit 1
    fi
    # A file that is not there, or that names no pattern, leaves nothing
    # to prune and the walk copies what it finds. Read in a shell that
    # ends on the first command to fail, the pipeline below is the whole
    # of that shell: an empty list makes the last stage exit non-zero and
    # takes the guard with it, which is a guard exiting 1 having said
    # nothing at all -- red, and mute about why.
    gm_prune=
    gm_pats=
    if [ -r "$1/tests/corpus/.gitignore" ]; then
        gm_pats=$(tr -d '\r' < "$1/tests/corpus/.gitignore" \
            | sed 's/#.*//' | tr -s ' \t' '\n' | grep . || true)
    fi
    set -f
    for gm_p in $gm_pats; do
        case $gm_p in
        */) gm_prune="$gm_prune -path 'tests/corpus/${gm_p%/}' -prune -o" ;;
        *)  gm_prune="$gm_prune -path 'tests/corpus/$gm_p' -prune -o" ;;
        esac
    done
    set +f
    # Each top-level directory is required by name before the walk, so a
    # tree missing one is refused with the reason rather than by a find
    # whose failure, under a caller's errexit, ends the guard mid-word
    # with nothing said. The walk itself is shielded the same way; a
    # shortfall it leaves behind is caught by the counts below, which do
    # say why.
    for gm_d in data examples src tests; do
        if [ ! -d "$1/$gm_d" ]; then
            echo "FAILURES: the tree under $1 has no $gm_d directory, so it"
            echo "      is not the source tree this guard was pointed at"
            exit 1
        fi
    done
    eval "( cd \"\$1\" && find data examples src tests $gm_prune -type f -print ) || :" \
        2>"$work/gm-err" > "$work/gm-list"
    # The directories, and the files a single pass will not reach: an
    # empty one has no line to be read and so is never opened.
    while read -r gm_rel; do
        gm_d=${gm_rel%/*}
        [ "$gm_d" = "$gm_rel" ] || [ -d "$mirror/$gm_d" ] || mkdir -p "$mirror/$gm_d"
        [ -s "$1/$gm_rel" ] || : > "$mirror/$gm_rel"
    done < "$work/gm-list"
    # Then the contents, in one pass over the whole list rather than a
    # process for each file. A tree of a few hundred files costs a few
    # hundred processes that way, which on a platform where starting one
    # is expensive took this longer than the guard that called it.
    # LC_ALL=C: the tree holds files that are not text, and an awk that
    # decodes its input as characters stops at the first byte that is not
    # one -- taking with it every file it had not reached yet. The copy
    # wants bytes anyway, since what it removes is a byte.
    ( cd "$1" && LC_ALL=C xargs awk -v dir="$mirror" '
        FNR == 1 { if (gm_out != "") close(gm_out); gm_out = dir "/" FILENAME }
        { gsub(/\r/, ""); print > gm_out }
      ' < "$work/gm-list" )
    for f in Makefile.am meson.build; do
        [ -f "$1/$f" ] && tr -d '\r' < "$1/$f" > "$mirror/$f"
    done
    if [ ! -r "$mirror/data/init.ps" ] || [ ! -r "$mirror/tests/guard-paths.sh" ]; then
        echo "FAILURES: could not mirror the source tree under $1"
        echo "      files listed: $(wc -l < "$work/gm-list" 2>/dev/null)"
        if [ -s "$work/gm-err" ]; then
            echo "      the walk wrote:"
            sed 's/^/        /' "$work/gm-err"
        fi
        for gm_w in data/init.ps tests/guard-paths.sh; do
            [ -r "$mirror/$gm_w" ] || echo "      absent from the mirror: $gm_w"
        done
        exit 1
    fi
    # Counts in against counts out. The pass above is what puts the tree
    # in the mirror, and one that stopped partway leaves a subset there:
    # every guard then scans less than the tree while reporting on the
    # tree, and their scan loops pass over a file that is not there
    # rather than saying so. Two files being readable is no evidence
    # about the several hundred beside them, which is the shape the
    # guards themselves are written against.
    gm_in=$(grep -c . "$work/gm-list")
    gm_out=$( ( cd "$mirror" && find data examples src tests -type f -print ) \
              2>/dev/null | grep -c . )
    if [ "$gm_in" -eq 0 ] || [ "$gm_out" -ne "$gm_in" ]; then
        echo "FAILURES: $gm_out of $gm_in files reached the mirror of $1;"
        echo "      a guard reading it would scan part of the tree and"
        echo "      report on the whole of it"
        exit 1
    fi
}

# Read C sources as C rather than as text: every named file is emitted as
# "<path><tab><line><tab><code>" with comments and string literals
# removed, so a guard scanning for a construct is not answered by a
# mention of it in a comment or by a word inside a message. Preprocessor
# lines are kept -- a macro that aliases the thing being guarded is a way
# past the rule, not a comment on it -- and a guard that does not want
# them drops them.
#
# The three parts are separated by tabs because a path carries colons: a
# drive letter is one, so a reader splitting on colons takes the file
# name for two fields and finds the code where the rest of the path is.
# Nothing on such a line looks like the construct being searched for, so
# a guard reading it does not fail -- it reports a tree with nothing
# wrong in it. A reader takes the code as everything after the second
# tab, by length rather than by matching a prefix, so a line carrying
# tabs of its own stays one line: `cut -f3-` in a pipe, or in awk with
# `-F'\t'`, substr($0, length($1) + length($2) + 3).
#
# Every guard that reads C goes through here, so that what counts as code
# is stated once. Take the files by name; a build in the tree leaves
# object files beside the sources whose debug information answers to the
# same patterns.
guard_c_source() {
    awk '
        FNR == 1 { inblock = 0; instr = 0 }
        { sub(/\r$/, "") }
        {
            line = $0
            sub(/\r$/, "", line)
            out = ""
            i = 1
            n = length(line)
            while (i <= n) {
                c = substr(line, i, 1)
                d = substr(line, i, 2)
                if (inblock) {
                    if (d == "*/") { inblock = 0; i += 2 } else i++
                    continue
                }
                if (instr) {
                    if (c == "\\") { i += 2; continue }
                    if (c == q) instr = 0
                    i++
                    continue
                }
                if (d == "/*") { inblock = 1; i += 2; continue }
                if (d == "//") break
                if (c == "\"" || c == "'\''") { instr = 1; q = c; i++; continue }
                out = out c
                i++
            }
            print FILENAME "\t" FNR "\t" out
        }' "$@"
}

# Hold two derived sets to each other, in both directions.
#
# Nineteen of the guards here do this and each wrote the pair of comm
# invocations out by hand. The mechanics are the same every time and the
# messages never are: what makes a guard worth reading is the sentence
# saying why THIS asymmetry matters, so the caller keeps that and this
# keeps the parts that are always identical -- comparing sorted sets,
# indenting the names, and remembering that a difference was found.
#
# Both directions, always. A guard that checks one is blind to whatever
# is absent from the list it started from, which is precisely the state
# a newly added member is in -- the failure this whole family of guards
# exists to prevent.
#
# The two sets are sorted here, in the C collation, rather than taken as
# sorted. comm does not check its input and answers nonsense on a file
# ordered another way, so a caller that sorted in the ambient locale --
# or did not sort at all -- would get a difference that is neither
# direction's answer. Sorting both the same way is the only thing that
# makes the comparison mean what it says.
#
#   $1  file of wanted names
#   $2  file of found names
#   $3  headline when something wanted is not found
#   $4  headline when something found was not wanted
#
# Sets guard_held to 1 when either direction has anything, and leaves it
# alone otherwise, so a caller may run several and test once.
#
# A guard whose sets carry more than a name -- a file and a line, a
# function and the width it reaches -- defines a guard_format function
# taking those records on standard input and writing the lines to show.
# Without one the records are indented and printed as they stand, which
# is what a set of plain names wants.
guard_hold() {
    _gh_want=$(mktemp) || { echo "FAIL: no temporary file for a comparison"; exit 1; }
    _gh_have=$(mktemp) || { echo "FAIL: no temporary file for a comparison"; exit 1; }
    LC_ALL=C sort -u "$1" > "$_gh_want"
    LC_ALL=C sort -u "$2" > "$_gh_have"

    _gh_missing=$(LC_ALL=C comm -23 "$_gh_want" "$_gh_have")
    if [ -n "$_gh_missing" ]; then
        echo "FAIL: $3"
        printf '%s\n' "$_gh_missing" | _gh_show
        guard_held=1
    fi
    _gh_extra=$(LC_ALL=C comm -13 "$_gh_want" "$_gh_have")
    if [ -n "$_gh_extra" ]; then
        echo "FAIL: $4"
        printf '%s\n' "$_gh_extra" | _gh_show
        guard_held=1
    fi

    [ -z "$_gh_want" ] || rm -f "$_gh_want"
    [ -z "$_gh_have" ] || rm -f "$_gh_have"
}

# The same, where each direction has a list of members excused from it.
#
# An exemption list is a second register and rots the same way: an entry
# excusing something that no longer needs excusing reads as cover, and
# the next member to land in that state is excused by a line written for
# something else. So each list is also held to the sets -- an exemption
# for a member that is now on both sides is reported, in the direction
# whose difference it was written to suppress.
#
#   $1  file of wanted names          $3  excused from the first direction
#   $2  file of found names           $4  excused from the second
#   $5  headline when something wanted is not found and not excused
#   $6  headline when something found was not wanted and not excused
guard_hold_except() {
    _ghe_w=$(mktemp) || { echo "FAIL: no temporary file for a comparison"; exit 1; }
    _ghe_h=$(mktemp) || { echo "FAIL: no temporary file for a comparison"; exit 1; }
    _ghe_m=$(mktemp) || { echo "FAIL: no temporary file for a comparison"; exit 1; }
    LC_ALL=C sort -u "$1" > "$_ghe_w"
    LC_ALL=C sort -u "$2" > "$_ghe_h"

    LC_ALL=C comm -23 "$_ghe_w" "$_ghe_h" > "$_ghe_m"
    guard_hold "$_ghe_m" "$3" "$5" \
        "excused from the difference just checked, and not in that state
      any more. An exemption nothing needs excuses whatever lands
      there next:"

    LC_ALL=C comm -13 "$_ghe_w" "$_ghe_h" > "$_ghe_m"
    guard_hold "$_ghe_m" "$4" "$6" \
        "excused by $(basename "$4") and not in that state any more.
      An exemption nothing needs excuses whatever lands there next:"

    [ -z "$_ghe_w" ] || rm -f "$_ghe_w"
    [ -z "$_ghe_h" ] || rm -f "$_ghe_h"
    [ -z "$_ghe_m" ] || rm -f "$_ghe_m"
}

# The caller's guard_format if it has defined one, indentation if not.
# Compared against the bare name so that a function is told apart from a
# program of the same name somewhere on the path.
_gh_show() {
    if [ "$(command -v guard_format 2>/dev/null)" = "guard_format" ]; then
        guard_format
    else
        sed 's/^/      /'
    fi
}

# The raster bytes of a binary PNM file, one to a line, with the header
# stepped over.
#
# A guard that wants to know what reached the page renders one and counts
# what it finds, and to reach the pixels it must first pass a header: a
# magic number, the two dimensions, and -- for every format except the
# bilevel one -- a maximum value, separated by any whitespace and
# interruptible by a comment running to the end of its line. How many
# tokens that is depends on the format, so it is read here from the magic
# number rather than taken from the caller. A caller that named the count
# itself would still be handed a raster when it named the wrong one, just
# one shifted by a few bytes, and every figure taken from it would be
# quietly wrong rather than refused -- which is the way a guard goes
# green while measuring the wrong thing.
#
# What to ask of the bytes is the caller's own business and stays there:
# a bilevel page carries eight pixels to the byte, a grey one is counted
# as marked against the ground by some callers and as dark by others, and
# those are different questions rather than one written several ways.
guard_pnm_pixels() {    # <file> -> the raster bytes, one to a line
    od -An -v -tu1 "$1" | awk '
        { for (i = 1; i <= NF; i++) v[n++] = $i }
        END {
            if (n < 2 || v[0] != 80) {
                print "the file does not open with a PNM magic number" > "/dev/stderr"
                exit 1
            }
            f = v[1] - 48
            if (f == 4) want = 3
            else if (f == 5 || f == 6) want = 4
            else {
                printf "P%d is not a binary PNM format\n", f > "/dev/stderr"
                exit 1
            }

            t = 0; i = 0
            while (t < want && i < n) {
                while (i < n && (v[i] == 32 || v[i] == 10 || v[i] == 9 || v[i] == 13)) i++
                if (v[i] == 35) { while (i < n && v[i] != 10) i++; continue }
                while (i < n && !(v[i] == 32 || v[i] == 10 || v[i] == 9 || v[i] == 13)) i++
                t++
            }
            i++     # the one whitespace byte that closes the header

            for (; i < n; i++) print v[i]
        }'
}

# How much ink a rendered page carries: "ink <n>", or "blank" for a page
# that carries none.
#
# The distinction is the point. A guard asking whether an operator painted
# reads the answer as "did it produce a page rather than an error", and a
# page with no marks on it answers a count like any other -- so a construct
# that runs cleanly and paints nothing is recorded as painting, which is
# the one behaviour PLRM 4.8.5 requires of a Separation named None and
# forbids of most other things. Naming the empty page rather than counting
# it to zero puts that case in front of the caller instead of leaving it to
# be pattern-matched by whoever remembers.
#
# The ground is the format's own: a grey or colour page is marked wherever
# a byte is not white, and a bilevel page wherever a bit is set.
guard_pnm_ink() {   # <file> -> "ink <n>" or "blank"
    guard_pnm_pixels "$1" | awk -v bilevel="$(guard_pnm_bilevel "$1")" '
        bilevel == "yes" { for (k = 0; k < 8; k++) if (int($1 / 2^k) % 2) ink++; next }
        $1 != 255        { ink++ }
        END              { print (ink ? "ink " ink : "blank") }'
}

# Whether a PNM file is the bilevel format, whose ground and marks are bits
# rather than bytes.
#
# Only the line that carries the two bytes is read. What follows the data
# is od's own business and differs by implementation: the od in the base
# system of macOS closes -A n output with a line for the final offset,
# blank under that flag, where GNU od ends with the data. The answer here
# rides into an awk program as a -v value, where a second line would put
# a newline inside a string and the awk on that same system refuses the
# whole program at parse time -- so the answer is one line by
# construction, not by trust in od.
guard_pnm_bilevel() {   # <file> -> "yes" or "no"
    od -An -v -tu1 -N2 "$1" | awk 'NF { print ($2 == 52) ? "yes" : "no"; exit }'
}

# The count a register declares for a kind of entry, held against what the
# derivation actually found.
#
# A register that says how many entries it carries holds itself to a
# number, and that number is what catches an entry deleted rather than
# edited: what is gone leaves no line to disagree with, so the entries
# alone cannot show it. The declaration is only worth carrying if
# something compares it, and a declared count that nothing compares reads
# exactly like one that is checked.
#
# Returns non-zero when the count is missing or wrong, and sets guard_held,
# so a caller may test each call or fold them all at the end.
guard_hold_count() {    # <register> <keyword> <how many were derived>
    _ghc_n=$(awk -v K="$2" '$1 == K && NF == 2 && $2 ~ /^[0-9]+$/ && !f {
                                print $2; f = 1 }' "$1")
    case ${_ghc_n:-} in
        ''|*[!0-9]*)
            echo "FAILURES: the register has no '$2 <n>' line, so the entries"
            echo "          below it are held to nothing but each other"
            guard_held=1
            return 1 ;;
        *)  if [ "$_ghc_n" -ne "$3" ]; then
                echo "FAILURES: the register records $2 $_ghc_n and holds $3"
                guard_held=1
                return 1
            fi ;;
    esac
    return 0
}

# The interpreter a guard was handed, refused if it is not one and made
# absolute if it is.
#
# The test runner names it relative to the build directory, and a guard
# that runs it does so from inside its own scratch directory, where that
# name reaches nothing. Forgetting this does not read as forgetting it:
# the interpreter simply cannot be found, every case answers nothing, and
# the guard reports whatever an empty answer means to it. That has been
# mistaken here for a flaky gate.
guard_require_interpreter() {   # <path to the interpreter>
    # A directory is executable too, so being a file is asked for as well.
    if [ ! -f "$1" ] || [ ! -x "$1" ]; then
        echo "FAILURES: the interpreter is missing or not an executable: $1"
        exit 1
    fi
    case $1 in
        /*) xpost=$1 ;;
        *)  xpost=$(cd "$(dirname "$1")" && pwd)/$(basename "$1") ;;
    esac
}

# Where the interpreter reads its boot files from: the tree this guard was
# handed, named absolutely for the same reason.
#
# Without it the interpreter answers out of the tree its binary was built
# against, so a guard comparing the tree it was given against a run of the
# interpreter is comparing two different trees and finding they agree.
guard_srcdata() {   # <source tree root>
    case $1 in
        /*) srcdata=$1/data ;;
        *)  srcdata=$(cd "$1" && pwd)/data ;;
    esac
}

# The divergence half of a family register, held both ways.
#
# A family register carries what the family does that the specification
# does not require of it, or requires differently, and each such line is
# found by a probe of its own. Both directions matter and for different
# reasons: a line no probe still finds is a difference that has been
# fixed or has moved, and leaving it there makes the register a record of
# what used to be true; a probe finding something no line names is a
# difference nobody has decided about, which is the one that matters,
# because it is the state a newly-written difference arrives in.
#
# The wording is here rather than in each guard because it tells the
# reader which way the disagreement runs and what to do about it, which is
# the same answer whichever family is asking.
guard_hold_divergence() {   # <register> <what the register names> <what was found>
    guard_hold "$2" "$3" \
        "named in the register and no longer found by the probe that finds
      it. A reason that has outlived its difference reads exactly like one
      that still holds; retire the line and the count with it:" \
        "found by a probe here and named by no line in the register. Say
      what the difference is and whether it is settled, a thorn or being
      changed, in tests/$1:"
}
