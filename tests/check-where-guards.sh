#!/bin/sh
#
# No boot file guards on a name the lockdown has taken away.
#
# `/NAME where { ... } if` asks the dictionary stack, at the moment it runs,
# whether NAME is there. bind cannot bake that: it freezes the OPERATOR a
# bare name stands for, and a search is not a name being used, it is a
# question about the stack. So a body whose bare uses of a name keep working
# can still have a search for that same name answer false -- and everything
# behind the search is then dead, silently, with every suite that drives it
# still passing.
#
# That is not hypothetical. The lockdown sweeps graphicsdict and DEVICE out
# of every dictionary a program can name, because each answers with live
# machinery. Five guards were left standing on them. One retired the
# outgoing device, one transmitted the finished page, and .curbandheight
# opened `/rows 0 def`, set rows only inside its guard, and answered rows --
# so it reported nought rows however the device was banding. A consumer
# embedding the interpreter found the first of them; nothing here did.
#
# A guard is answered for if the name still resolves in a shipped run, or if
# it is declared below as one a run may legitimately not have, or if it
# stands in init.ps ahead of the relocation sweep, where the name is still
# in place when the search looks.
#
#   $1  path to the source tree root
#   $2  path to the built xpost binary
set -u
src=${1:?usage: check-where-guards.sh <srcroot> <xpost>}
xpost=${2:?usage: check-where-guards.sh <srcroot> <xpost>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
guard_require_interpreter "$xpost"
guard_workdir

# Names a run may legitimately be without. Each is a switch or a fact about
# the run rather than a part of the language: the debug flags exist only in a
# run started to trace that subsystem, QUIET and WIN32 and NOFACES say how
# this run was started and what it was built with, and Default is a key a
# dictionary may or may not carry.
cat > "$work/optional" <<'EOF'
DEBUGSTROKE
DEBUGFILL
DEBUGCLIP
QUIET
WIN32
NOFACES
Default
EOF

sweep=$(grep -n 'dup 60 string cvs 0 get 46 eq' "$src/data/init.ps" | head -1 | cut -d: -f1)
case ${sweep:-x} in
    ''|*[!0-9]*) echo "FAILURES: cannot find the relocation sweep in data/init.ps,"
                 echo "      so a guard standing ahead of it cannot be told from one"
                 echo "      standing behind it"; exit 1 ;;
esac

# Read the boot files by their own names, from inside the directory that
# holds them. Handed a path to grep, the file name comes back with the path
# on the front, and a path can carry a colon -- a drive letter does, on the
# host where these run under a shell that does not -- which is the field
# separator the rewrite below splits on. There the rewrite matched nothing,
# and a substitution that matches nothing leaves the line alone: every line
# came through raw, the third field of a raw line is whatever token happens
# to sit there, and the run built from those names asked the interpreter
# about a name that was not one. It answered syntaxerror, this check read
# the silence as a tree where nothing resolves, and it condemned two dozen
# guards including the ones it declares a run may be without.
#
# So the names are bare, and the rewrite prints only what it rewrote --
# a line it cannot parse is dropped rather than passed through as a guard.
( cd "$src/data" && grep -nE '/[A-Za-z_][A-Za-z0-9_.]* +where' *.ps ) \
  | grep -v ':[[:space:]]*%' \
  | sed -n -E 's@^([^:]+):([0-9]+):.*/([A-Za-z_][A-Za-z0-9_.]*) +where.*@\1 \2 \3@p' \
  | sort -u > "$work/guards"

if [ ! -s "$work/guards" ]; then
    echo "FAILURES: no where-guards found at all, which is not what this tree"
    echo "      looks like -- the search that finds them has stopped working"
    exit 1
fi

# Ask a shipped run which of the names it can still see.
awk '{print $3}' "$work/guards" | sort -u > "$work/names"
{ echo '%!PS'
  while read -r n; do
      printf '(%s ) print /%s where { pop (yes) }{ (no) } ifelse print (\\n) print\n' "$n" "$n"
  done < "$work/names"
  # The last thing the run prints, so that a run which did not get that far
  # is told apart from one that got there and found nothing. Without it an
  # interpreter that failed to start reads as a tree in which no name
  # resolves at all, and this check answers that every guard in it is dead
  # -- which is what it did on a host where the run could not load the
  # library, reporting two dozen guards including the ones named below as
  # ones a run may legitimately be without.
  echo '(ASKED\n) print flush'
  echo 'quit'
} > "$work/ask.ps"
XPOST_DATA_DIR="$src/data" XPOST_NO_VM_IMAGE=1 \
    "$xpost" -q -d null -o /dev/null "$work/ask.ps" </dev/null > "$work/answers" 2>&1
if ! grep -q '^ASKED$' "$work/answers"; then
    echo "FAILURES: the run that asks which names resolve did not reach its"
    echo "      end, so its silence means nothing and no guard below could"
    echo "      be judged by it. It reported:"
    sed 's/^/      /' "$work/answers" | head -5
    exit 1
fi
awk '$2=="yes"{print $1}' "$work/answers" | sort -u > "$work/resolves"

dead=0
while read -r f l n; do
    grep -qx "$n" "$work/resolves" && continue
    grep -qx "$n" "$work/optional" && continue
    case $f in
        init.ps|*/init.ps) [ "$l" -lt "$sweep" ] && continue ;;
    esac
    if [ "$dead" -eq 0 ]; then
        echo "FAILURES: a guard asks the dictionary stack for a name the"
        echo "      lockdown has taken away, so it is always false and what"
        echo "      it guards never runs:"
    fi
    echo "      $(basename "$f"):$l  /$n where"
    dead=$((dead + 1))
done < "$work/guards"

if [ "$dead" -gt 0 ]; then
    echo "      Use the bare name, which bind froze in when the body was"
    echo "      bound, or declare the name in this guard as one a run may"
    echo "      legitimately be without."
    exit 1
fi

printf 'every where-guard answers for itself (%s guard(s), %s name(s)): SUCCESS\n' \
    "$(wc -l < "$work/guards" | tr -d ' ')" "$(wc -l < "$work/names" | tr -d ' ')"
exit 0
