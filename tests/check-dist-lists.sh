#!/bin/sh
# Guard the distribution lists: what `make dist` packs must be what the
# tree holds. A file the build needs and the lists do not name produces a
# release tarball that does not compile, and a line naming a file that
# has gone produces one that does not pack at all -- both invisible to
# every in-tree build and every CI job, which read the working copy.
#
# The check used to look at one directory through one list, src/lib
# against src/lib/Makefile.mk, in one direction. Everything else went
# unheld, and the tests directory was in no list at all: the guards, the
# helper they share and the registers they hold the tree to were absent
# from every tarball, so a release shipped a tree in which not one
# structural invariant could be checked, and nothing anywhere said so.
#
# Each directory below is held to its lists both ways, and to each list
# it carries rather than to the file they are written in: a name
# appearing anywhere in a makefile once satisfied every list in it, which
# is how a boot file came to be packed and never installed.
#
# Usage: check-dist-lists.sh <source tree root>

set -eu
src=${1:?usage: check-dist-lists.sh <source tree root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_workdir
guard_mirror_tree "$src"
tree=$mirror

fail=0

# What a list holds: every "dir/file" token in the assignments that
# distribute, whatever the line shape. Continuations, leading tabs and
# trailing backslashes are not part of a filename.
#
# The assignments, and not the whole file. A name appearing anywhere in a
# makefile used to satisfy every list in it, which is how a boot file
# came to be packed and never installed -- it was named in one list, and
# the reading could not tell that from being named in the other. The same
# reading is what says a source is distributed here, and a makefile
# carries plenty that is not a distribution: the programs it builds, the
# libraries it links, the arguments it hands a checker. A file named only
# in one of those is a file the tarball does not carry and this would
# call listed.
#
# Which assignments distribute is automake's answer rather than a list
# here: EXTRA_DIST, and the _SOURCES, _HEADERS and _DATA of any target.
# A new target's sources are therefore read the day the target is added.
#
# A build target named in the same file -- src/lib/libxpost.la, the
# programs under src/bin -- is not a distributed file, so the comparison
# is confined to the kinds of file the directory contributes.
dist_assignments() {    # <makefile>
    [ -f "$1" ] || return 0
    tr -d '\r' < "$1" | sed 's/#.*//' | awk '
        /^[A-Za-z_][A-Za-z0-9_]*[ \t]*[+]?=/ {
            name = $1
            sub(/[ \t]*[+]?=.*$/, "", name)
            keep = (name == "EXTRA_DIST" || name ~ /_SOURCES$/ ||
                    name ~ /_HEADERS$/ || name ~ /_DATA$/)
        }
        keep { print }
        !/\\[ \t]*$/ { keep = 0 }
    '
}

listed() {          # <makefile> <dir> <kinds>
    [ -f "$1" ] || return 0
    dist_assignments "$1" \
      | tr ' \t\\' '\n\n\n' \
      | grep -E "^$2/[^/]" | grep -E "$3" || true
}

# Compare a directory's contents with the list that is supposed to name
# them, both ways round.
hold() {            # <label> <listfile> <dir> <have-file> <kinds>
    label=$1; listfile=$2; dir=$3; have=$4; kinds=$5
    shown=${listfile#"$tree"/}
    if [ ! -f "$listfile" ]; then
        echo "FAILURES: $label has no distribution list at $shown"
        exit 1
    fi
    listed "$listfile" "$dir" "$kinds" | LC_ALL=C sort -u > "$work/listed"
    grep -E "$kinds" "$have" | LC_ALL=C sort -u > "$work/have"
    if [ ! -s "$work/have" ]; then
        echo "FAILURES: no $label files found under $src/$dir; this check"
        echo "      is reading the wrong tree"
        exit 1
    fi
    if [ ! -s "$work/listed" ]; then
        echo "FAILURES: $shown names no $dir file; the list was emptied or"
        echo "      its shape changed and this check no longer reads it"
        exit 1
    fi
    guard_held=0
    guard_hold "$work/have" "$work/listed" \
        "present in the tree and absent from $shown, so make dist would
      omit them:" \
        "named by $shown and not in the tree, so make dist would fail on
      them:"
    [ "$guard_held" -eq 0 ] || fail=1
}

# ---- the library: every source and header it is built from ----
( cd "$tree" && ls src/lib/*.c src/lib/*.h 2>/dev/null ) > "$work/lib"
hold "library source" "$tree/src/lib/Makefile.mk" src/lib "$work/lib" '\.[ch]$'

# ---- the programs: sources, and the Windows resources they need ----
( cd "$tree" && ls src/bin/*.c src/bin/*.h src/bin/*.rc src/bin/*.ico 2>/dev/null ) \
    > "$work/bin"
hold "program source" "$tree/src/bin/Makefile.mk" src/bin "$work/bin" \
    '\.([ch]|rc|ico)$'

# ... and the second claim those two directories make, which is a
# different one from the first. A file reaches the tarball through any of
# the assignments above; it reaches the compiler only through a target's
# _SOURCES. So a source named in a header list alone is packed, shipped
# and built by nothing -- present in every release and in no binary --
# and the comparison above cannot see it, both lists being satisfied by
# either. It is the same shape as a boot file packed and never installed.
compiled() {        # <makefile> <dir>
    dist_assignments "$1" \
      | awk '/^[A-Za-z_][A-Za-z0-9_]*[ \t]*[+]?=/ {
                 name = $1; sub(/[ \t]*[+]?=.*$/, "", name)
                 keep = (name ~ /_SOURCES$/)
             }
             keep { print }
             !/\\[ \t]*$/ { keep = 0 }' \
      | tr ' \t\\' '\n\n\n' | grep -E "^$2/[^/]*\.c$" || true
}
for dir in src/lib src/bin; do
    ( cd "$tree" && ls "$dir"/*.c 2>/dev/null ) | LC_ALL=C sort -u \
        > "$work/csrc"
    compiled "$tree/$dir/Makefile.mk" "$dir" | LC_ALL=C sort -u \
        > "$work/ccompiled"
    if [ ! -s "$work/ccompiled" ]; then
        echo "FAILURES: $dir/Makefile.mk names no source for any target; the"
        echo "      lists were emptied or their shape changed and this check"
        echo "      no longer reads them"
        exit 1
    fi
    LC_ALL=C comm -23 "$work/csrc" "$work/ccompiled" > "$work/cunbuilt"
    if [ -s "$work/cunbuilt" ]; then
        echo "FAIL: present in the tree and in no target's sources in"
        echo "      $dir/Makefile.mk (a release would carry these and build"
        echo "      none of them):"
        sed 's/^/      /' "$work/cunbuilt"
        fail=1
    fi
done

# ---- the interpreter's PostScript, and both lists that carry it ----
#
# Two lists name it -- what meson installs and what the tarball packs --
# and both are held to the directory rather than to each other. Held to
# each other, a file named in neither is in neither list's difference
# from the other: the two agree, the check is green, and the tree holds
# a boot file that no build installs and no release carries. That is the
# state a file arrives in, so it is the one state the check has to see.
( cd "$tree" && ls data/*.ps 2>/dev/null ) | LC_ALL=C sort -u > "$work/data"
hold "interpreter data" "$tree/data/Makefile.mk" data "$work/data" '\.ps$'

# Two of those lists are in the one file -- what the tarball packs and
# what `make install` copies -- and the helper above reads the file
# rather than either list, so a file in one of them satisfies it. They
# are separated here and each held to the directory, because they buy
# different things: a boot file in the tarball and not the install is
# shipped and never put where a run looks for it.
for pair in 'EXTRA_DIST:packed' 'psfiles_DATA:installed'; do
    head=${pair%%:*}
    what=${pair#*:}
    awk -v h="$head" '
        $0 ~ "^" h "[ \t]*[+]?=" { inlist = 1 }
        inlist { print; if ($0 !~ /\\[ \t]*$/) inlist = 0 }
    ' "$tree/data/Makefile.mk" \
      | tr ' \t\\' '\n\n\n' | grep -E '^data/[^/]*\.ps$' \
      | LC_ALL=C sort -u > "$work/data-$what"
    if [ ! -s "$work/data-$what" ]; then
        echo "FAILURES: data/Makefile.mk's $head names no PostScript file;"
        echo "      the list was emptied or its shape changed and this check"
        echo "      no longer reads it"
        exit 1
    fi
    LC_ALL=C comm -23 "$work/data" "$work/data-$what" > "$work/data-miss"
    if [ -s "$work/data-miss" ]; then
        echo "FAIL: present in the tree, absent from data/Makefile.mk's $head"
        echo "      (these would not be $what):"
        sed 's/^/      /' "$work/data-miss"
        fail=1
    fi
done

# The install list spells a name without its directory, so it is read
# and compared here rather than through the helper above.
sed -n "/xpost_data_src = files(\[/,/\])/p" "$tree/data/meson.build" \
  | grep -oE "'[^']+'" | tr -d "'" | sed 's|^|data/|' \
  | grep '\.ps$' | LC_ALL=C sort -u > "$work/data-meson"
grep '\.ps$' "$work/data" | LC_ALL=C sort -u > "$work/data-have"
if [ ! -s "$work/data-meson" ]; then
    echo "FAILURES: data/meson.build names no PostScript file; the list was"
    echo "      emptied or its shape changed and this check no longer reads it"
    exit 1
fi
guard_held=0
guard_hold "$work/data-have" "$work/data-meson" \
    "present in the tree and absent from data/meson.build, so a build
      would install them nowhere and a run would not find them:" \
    "named by data/meson.build and not in the tree:"
[ "$guard_held" -eq 0 ] || fail=1

# ---- the examples: distributed whole, installed nowhere ----
#
# Every file the directory holds is an example program or the data one
# of them reads, so the comparison takes them all: an example added
# without a list line would be missing from the tarball that documents
# it, and tests/run-examples-test.sh runs whatever the directory holds,
# so a release would advertise a tested example it does not carry.
( cd "$tree" && find examples -type f -print ) > "$work/examples"
hold "example" "$tree/examples/Makefile.mk" examples "$work/examples" '.'

# ---- the test suite, guards and registers included ----
#
# A corpus is fetched, not distributed: the programs it holds belong to
# their own sources. The mirror this reads has already left those out,
# by the statement in tests/corpus/.gitignore, so what is walked here is
# what a release carries. What remains under tests/corpus -- the fetch
# and evaluate scripts, the preludes that let a corpus run -- is
# distributed like anything else, or a tarball can fetch a corpus and
# still not run it.
( cd "$tree" && find tests -type f -print ) > "$work/tests"
hold "test suite" "$tree/tests/Makefile.mk" tests "$work/tests" '.'

# ---- the documents, and the figures the reference draws ----
#
# A release is read before it is built. The documents are the only
# account of what the interpreter covers, how it is gated and how to
# work on it, and the reference the doc target produces is assembled
# from the .dox sources and the figures beside them -- so a tarball
# missing any of them either says nothing or builds a reference with
# holes in it.
#
# doc is not one of the directories the tree mirror carries, so the list
# is mirrored here for the reason the mirror exists: read straight from
# a checkout that brought carriage returns in, every line ends before
# `$` can match and the list reads as empty.
#
# What the doc target makes is not what it reads. It writes the
# reference into doc/html and doc/latex, in the build directory, which
# for a build in the tree is the directory being walked here; the same
# two names are what it cleans. They are pruned from the walk, and left
# out of the list by the kind, which asks for a token that does not end
# in a slash.
#
# Two files in the directory are not named by it either: doc/Makefile.mk
# is the list, distributed by Makefile.am's include of it, and
# doc/meson.build is held by the comparison below.
guard_require_file "$src/doc/Makefile.mk" "the documentation list"
mkdir -p "$tree/doc"
tr -d '\r' < "$src/doc/Makefile.mk" > "$tree/doc/Makefile.mk"
( cd "$src" && find doc -name html -prune -o -name latex -prune \
      -o -type f -print ) \
  | grep -vE '^doc/(Makefile\.mk|meson\.build)$' > "$work/doc"
hold "documentation" "$tree/doc/Makefile.mk" doc "$work/doc" '[^/]$'

# ---- the other build system, which the tarball has to carry too ----
#
# The tests are registered in meson and CI builds with it, so a release
# that ships only the autotools half ships a tree in which nothing here
# can be run -- including the guards the lists above now distribute.
#
# The walk stops at a nested checkout. A working copy may hold other
# trees inside it -- a second worktree, a clone -- and their build
# descriptions belong to those trees, not to this distribution. A
# checkout announces itself by carrying .git; the tarball this all
# exists to protect carries none anywhere, so nothing is pruned there.
( cd "$src" && find . -mindepth 1 -name .git -prune \
      -o -type d -exec test -e '{}/.git' ';' -prune \
      -o \( -name 'meson.build' -o -name 'meson_options.txt' \) -print ) \
  | sed 's|^\./||' | LC_ALL=C sort -u > "$work/meson-have"
tr -d '\r' < "$tree/Makefile.am" | sed 's/#.*//' | tr ' \t\\' '\n\n\n' \
  | grep -E '(^|/)meson(\.build|_options\.txt)$' | LC_ALL=C sort -u \
  > "$work/meson-listed"
LC_ALL=C comm -3 "$work/meson-have" "$work/meson-listed" > "$work/meson-diff"
if [ -s "$work/meson-diff" ]; then
    echo "FAIL: the meson build description and Makefile.am's list of it disagree:"
    sed 's/^\t/      only in Makefile.am: /; s/^\([^ ]\)/      not distributed: \1/' \
        "$work/meson-diff"
    fail=1
fi

# ---- and the lists are actually included by the build ----
for mk in src/lib/Makefile.mk src/bin/Makefile.mk data/Makefile.mk \
          examples/Makefile.mk tests/Makefile.mk; do
    if ! grep -q "^include $mk\$" "$tree/Makefile.am"; then
        echo "FAIL: Makefile.am does not include $mk, so nothing in it is"
        echo "      distributed however complete the list is"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: the distribution lists and the tree disagree"
    exit 1
fi
echo "SUCCESS"
exit 0
