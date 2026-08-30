#!/bin/sh
# Meson test wrapper: assert that a standard operator written in
# PostScript holds the operators it calls, and not their names.
#
# An executable name inside a procedure is resolved against the dictionary
# stack at the moment it runs, so a name left in a standard operator's
# body is a program's redefinition waiting to be picked up -- redefining
# gsave would change what rectfill does, from inside rectfill. bind exists
# to close that, and the interpreter binds every promoted body once the
# whole set has been promoted, which is the only moment at which a call
# from one of them to another can be frozen.
#
# What bind cannot reach is a body that was already bound at its
# definition, before any of these names answered with an operator: bind
# makes what it binds read-only and then ignores a read-only array, so
# that body keeps the names it had and no later sweep can finish it. Those
# are the entries listed in the golden file, each one an operator body
# reaching an operator by name. The list only shrinks: an entry that has since
# been frozen is a failure too, so it cannot go stale, and a body that
# starts reaching a new name fails immediately.
#
# tamper_dispatch_test.ps is the same invariant for a name held as data in
# a dispatch dictionary; this is the invariant for a name in a procedure.
# The two are paired, and the pairing is load-bearing: that sweep exempts
# the promoted bodies outright, because its scan never reaches them, so
# this script is the only thing standing behind that exemption. Narrowing
# what is walked here silently widens what is unchecked there. Neither is
# a local decision; change both or neither.
#
#   $1  path to the xpost binary
#   $2  path to the source tree root
#   $3  path to the golden list
set -u
xpost=${1:?usage: check-wrapped-bind.sh <xpost> <srcroot> <golden>}
src=${2:?usage: check-wrapped-bind.sh <xpost> <srcroot> <golden>}
golden=${3:?usage: check-wrapped-bind.sh <xpost> <srcroot> <golden>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
cr=$(printf '\r')   # tolerate CRLF line endings (Windows checkouts)

# Walk every operator body systemdict publishes and report each executable
# name in it that the dictionary stack answers with an operator. Every
# mention of a procedure-valued name goes through load: a bare mention is
# a call.
#
# Two populations, because a body reaches systemdict two ways. The
# promoted ones are operator objects now, so their procedures are read out
# of .wrappedprocs, where the promotion filed them. The rest are still
# procedures in systemdict itself -- the names the language does not make
# operators, and the ones this interpreter adds -- and they are walked
# where they sit. A body in either set that was bound at its definition
# keeps the names it had, and walking only the first set left the second
# unchecked.
cat > "$work/probe.ps" <<'PSEOF'
/walk { % array depth operator  .  -
    3 dict begin
    /w.op exch def /w.d exch def /w.a exch def
    w.d 0 gt {
        /w.a load rcheck {
            0 1 /w.a load length 1 sub {
                /w.a load exch get
                dup type /nametype eq {
                    dup xcheck {
                        dup where {
                            1 index get type /operatortype eq {
                                (loose ) print w.op 60 string cvs print
                                ( ) print 60 string cvs print (\n) print
                            }{ pop } ifelse
                        }{ pop } ifelse
                    }{ pop } ifelse
                }{
                    dup type /arraytype eq 1 index type /packedarraytype eq or
                        { w.d 1 sub w.op walk }{ pop } ifelse
                } ifelse
            } for
        } if
    } if
    end
} def
/n 0 def
[ .privatedict /.wrappedprocs get { pop } forall ]
{ /n n 1 add store
  dup .privatedict /.wrappedprocs get exch get exch 24 exch walk } forall
(promoted ) print n 20 string cvs print (\n) print

/m 0 def
[ systemdict { pop } forall ]
{ dup systemdict exch get
  dup xcheck 1 index type /arraytype eq 2 index type /packedarraytype eq or and
  { /m m 1 add store exch 24 exch walk }{ pop pop } ifelse } forall
(unpromoted ) print m 20 string cvs print (\n) print
PSEOF

XPOST_DATA_DIR="$src/data" XPOST_CENSUS=1 "$xpost" -q --no-sandbox -d null -o /dev/null \
    "$work/probe.ps" </dev/null 2>/dev/null | tr -d "$cr" > "$work/out"

n=$(sed -n 's|^promoted ||p' "$work/out" | head -1)
if [ -z "${n:-}" ] || [ "$n" -lt 100 ]; then
    echo "FAILURES: the interpreter reported ${n:-no} promoted operators; the walk is unusable"
    exit 1
fi

# A second population that walked nothing would be a check reporting on
# the first alone, which is the state this replaced.
m=$(sed -n 's|^unpromoted ||p' "$work/out" | head -1)
if [ -z "${m:-}" ] || [ "$m" -lt 10 ]; then
    echo "FAILURES: the interpreter reported ${m:-no} unpromoted bodies in systemdict; the walk is unusable"
    exit 1
fi

if [ ! -s "$golden" ] || [ ! -r "$golden" ]; then
    echo "FAILURES: the declared list $golden is empty; the check is unusable"
    exit 1
fi

sed -n 's|^loose ||p' "$work/out" | LC_ALL=C sort -u > "$work/have"
grep -v '^[[:space:]]*#' "$golden" | grep -v '^[[:space:]]*$' | tr -d "$cr" \
    | LC_ALL=C sort -u > "$work/want"

fail=0

guard_held=0
guard_hold "$work/have" "$work/want" \
    "reaching a standard operator by name and not listed: the body must
      be bound after the whole set is promoted:" \
    "listed as reached by name, but frozen now: remove them from
      $(basename "$golden"):"
[ "$guard_held" -eq 0 ] || fail=1

[ "$fail" = 0 ] || exit 1
echo "SUCCESS ($n promoted and $m unpromoted operator bodies, $(wc -l < "$work/have" | tr -d ' ') declared dynamic references)"
exit 0
