#!/bin/sh
# Guard: every route a program has to the glyph raster resolves the
# clipping region first.
#
# A glyph reaches the page by a route of its own. The face renders it to
# a coverage raster and the raster is plotted pixel by pixel straight
# through the device's PutPix and BlendPix, never passing through the
# fill pipeline where a painter meets the clipping region. PLRM 7.5.1
# makes what any painting operation marks the intersection of the pixels
# it would cover with the pixels of the region and names no exemption
# for text, so the region has to be met on that route as well -- and the
# raster route reads it out of the clip's own cache holder, which
# .showclip fills. An operator that reaches the raster without running
# .showclip finds nothing in the holder and paints wherever the glyphs
# fall. Nothing fails, nothing is reported, and the page is simply
# unmasked. That is how the fault survived: show was the only operator
# with the call, and the four beside it had none.
#
# The population is derived rather than listed, because a list is what
# a new entry point is left out of.
#
# From the C: the operator bindings whose call graph reaches
# _draw_bitmap, the one function that plots a coverage raster. Each is
# then read as one of two kinds. A binding that builds its text state
# through _text_state_get takes the region out of the cache holder, and
# every PostScript route to it must have filled the holder. A binding
# that reaches the raster without _text_state_get narrows it by means of
# its own -- the glyph-mask cache refuses a glyph the clip rectangle
# does not hold, the anti-aliased stencil is painted from a fill that
# has already met the region -- and the routes to it are not held to
# this. Which bindings those are is the register's business, so one
# arriving in that class is a line to write rather than a silence.
#
# From the PostScript: every named procedure in data/, what each one
# calls, and where .showclip falls among the calls. A procedure reaches
# the raster unresolved when it names a cache-reading binding, or names
# another procedure that does, before it resolves the clip. That is
# closed over the call graph, so a walker reached only from operators
# that resolve the clip is not itself required to -- which is why
# kshow, xshow, yshow, xyshow and the composite walkers carry no call
# of their own.
#
# What fails: a language operator -- one defined through .defop, which
# is to say one a program can name -- that reaches a cache-reading
# binding without resolving the clip on the way. And the same reached
# from the top level of a file, where there is no operator to blame.
#
# One that does fail is reported on its own line and carries the fault
# no further: once a program can name the operator, that operator is
# where the missing call belongs, and everything that calls it is
# innocent. Only the helpers underneath one pass the fault upward. Take
# that out and a tree missing every call names thirty operators for five
# faults, which is the report saying nothing.
#
# A name is read as PostScript reads it. `//show` is the binding the
# name held when the file was scanned; plain `show` is whatever the name
# holds when it runs, which is the operator defined over it. That
# distinction is the whole of why xshow may call show and need nothing
# else.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-show-clip.sh <srcroot>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
golden="$src/tests/show_clip_routes.golden"
guard_require_file "$golden" "the register of routes to the glyph raster"
guard_require_dir "$src/data" "the interpreter's PostScript"
guard_require_file "$src/src/lib/xpost_op_font.c" "the font module"

guard_workdir
guard_mirror_tree "$src"
tree=$mirror
guard_mirror register "$golden"
golden="$mirror/$(basename "$golden")"

fail=0

# ---- the C side ----------------------------------------------------
guard_c_source "$tree/src/lib/xpost_op_font.c" > "$work/code" 2>/dev/null
if [ ! -s "$work/code" ]; then
    echo "FAILURES: src/lib/xpost_op_font.c could not be read as C"
    exit 1
fi

# every function and its body, so a call can be followed
awk '
{
    code = substr($0, length($1) + length($2) + 3)
    if (code ~ /^[ \t]*#/) code = ""
    text[++n] = code
}
END {
    depth = 0; pend = ""; cur = ""
    for (k = 1; k <= n; k++) {
        c = text[k]
        m = length(c)
        for (i = 1; i <= m; i++) {
            ch = substr(c, i, 1)
            if (ch == "{") {
                if (depth == 0) {
                    hdr = pend
                    gsub(/^[ \t]+/, "", hdr)
                    cur = ""
                    if (hdr !~ /;/ && hdr !~ /(^|[^A-Za-z0-9_])(if|for|while|switch|else|do|return)[ \t]*\(/ \
                        && match(hdr, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
                        cur = substr(hdr, RSTART, RLENGTH)
                        sub(/[ \t]*\($/, "", cur)
                    }
                }
                depth++; pend = ""; continue
            }
            if (ch == "}") {
                depth--
                if (depth <= 0) { depth = 0; cur = "" }
                pend = ""; continue
            }
            if (depth >= 1 && cur != "") body[cur] = body[cur] ch
            if (ch == ";") pend = ""; else pend = pend ch
        }
        pend = pend " "
        if (depth >= 1 && cur != "") body[cur] = body[cur] " "
    }
    for (f in body) print f "\t" body[f]
}' FS="$guard_tab" "$work/code" > "$work/bodies"

if [ ! -s "$work/bodies" ]; then
    echo "FAILURES: no function body could be read from src/lib/xpost_op_font.c"
    exit 1
fi

# the callers of a function, closed over: which functions can reach it
closure() { # seed-name  ->  file of function names
    echo "$1" > "$work/cl"
    while : ; do
        before=$(grep -c . "$work/cl")
        awk -F'\t' '
            NR == FNR { seed[$1] = 1; next }
            { for (s in seed) if (index($2, s "(") > 0 && $1 != s) { print $1; break } }
        ' "$work/cl" "$work/bodies" >> "$work/cl"
        sort -u "$work/cl" -o "$work/cl"
        after=$(grep -c . "$work/cl")
        [ "$after" -gt "$before" ] || break
    done
    cat "$work/cl"
}

closure _draw_bitmap > "$work/plots"
closure _text_state_get > "$work/reads"

if ! grep -qx _show_glyph "$work/plots"; then
    echo "FAILURES: nothing in src/lib/xpost_op_font.c reaches _draw_bitmap"
    echo "      through _show_glyph; the raster route has been renamed or"
    echo "      restructured and this check is following nothing"
    exit 1
fi

# the operator names, and the functions behind them. guard_c_source
# takes string literals out, which is where the names are: the lines
# that are code are found there and read back off the mirrored file.
awk -F'\t' 'substr($0, length($1) + length($2) + 3) ~ /xpost_operator_cons/ { print $2 }' \
    "$work/code" | sort -un > "$work/oplines"
awk 'NR == FNR { want[$1] = 1; next }
     (FNR in want) {
         if (match($0, /xpost_operator_cons\([^,]*,[ \t]*"[^"]*"[ \t]*,[ \t]*\(Xpost_Op_Func\)[A-Za-z_][A-Za-z0-9_]*/)) {
             s = substr($0, RSTART, RLENGTH)
             nm = s; sub(/^[^"]*"/, "", nm); sub(/".*/, "", nm)
             fn = s; sub(/.*\(Xpost_Op_Func\)/, "", fn)
             if (nm != "" && fn != "") print nm "\t" fn
         }
     }' "$work/oplines" "$tree/src/lib/xpost_op_font.c" | sort -u > "$work/ops"

if [ ! -s "$work/ops" ]; then
    echo "FAILURES: no operator registration could be read from"
    echo "      src/lib/xpost_op_font.c; the population below would be empty"
    exit 1
fi

# the operators that reach the raster, and how each meets the region
: > "$work/bindings"
: > "$work/derived"
while IFS="$(printf '\t')" read -r opname opfn; do
    grep -qx "$opfn" "$work/plots" || continue
    if grep -qx "$opfn" "$work/reads"; then
        echo "reads-cache $opname" >> "$work/derived"
        echo "$opname" >> "$work/bindings"
    else
        echo "narrows-itself $opname" >> "$work/derived"
    fi
done < "$work/ops"
sort -u "$work/bindings" -o "$work/bindings"
sort -u "$work/derived" -o "$work/derived"

if ! grep -qx show "$work/bindings"; then
    echo "FAILURES: show is not among the operators that reach the glyph"
    echo "      raster with the region taken from the cache; the derivation"
    echo "      is reading the wrong thing and would agree with anything"
    exit 1
fi

# ---- the PostScript side -------------------------------------------
#
# Tokenise as PostScript does, then read the definitions out of the
# tokens: a literal name followed by a procedure (with an operand
# signature allowed between them) defines that name.
set -- "$tree"/data/*.ps
if [ ! -r "$1" ]; then
    echo "FAILURES: no PostScript could be read under $src/data"
    exit 1
fi

awk -v BINDINGS="$(paste -sd, - < "$work/bindings")" '
BEGIN {
    n = split(BINDINGS, b, ",")
    for (i = 1; i <= n; i++) if (b[i] != "") BIND[b[i]] = 1
    nt = 0
}
function emit(t, f, l) {
    nt++
    TK[nt] = t; TF[nt] = f; TL[nt] = l
}
# a string runs across lines but not across files
FNR == 1 { instr = 0 }
{
    line = $0
    len = length(line)
    i = 1
    while (i <= len) {
        c = substr(line, i, 1)
        if (instr > 0) {
            if (c == "\\") { i += 2; continue }
            if (c == "(") instr++
            else if (c == ")") { instr--; if (instr == 0) emit("(STR)", FILENAME, FNR) }
            i++
            continue
        }
        if (c == " " || c == "\t") { i++; continue }
        if (c == "%") break
        if (c == "(") { instr = 1; i++; continue }
        if (c == "{" || c == "}" || c == "[" || c == "]") {
            emit(c, FILENAME, FNR); i++; continue
        }
        # a doubled angle is a dictionary mark; a single one opens a
        # string written in hex or base 85, whose contents are no more
        # code than a parenthesised one
        if (c == "<") {
            if (substr(line, i + 1, 1) == "<") { i += 2; continue }
            j = index(substr(line, i + 1), ">")
            if (j == 0) { i = len + 1 } else { i = i + j + 1 }
            emit("(STR)", FILENAME, FNR)
            continue
        }
        if (c == ">") { i += (substr(line, i + 1, 1) == ">") ? 2 : 1; continue }
        # a name: literal (/x), immediate (//x) or executable (x). A
        # slash starts a name and ends the one before it, so the leading
        # one or two are taken first and the rest runs to the next
        # delimiter.
        j = i
        if (c == "/") {
            j++
            if (substr(line, j, 1) == "/") j++
        }
        while (j <= len) {
            d = substr(line, j, 1)
            if (d == " " || d == "\t" || d == "%" || d == "(" || d == ")" \
             || d == "{" || d == "}" || d == "[" || d == "]" || d == "<" || d == ">") break
            if (d == "/") break
            j++
        }
        emit(substr(line, i, j - i), FILENAME, FNR)
        i = j
    }
}
END {
    # The machinery defines its operators through .defop; a file that
    # names it nowhere is a program that came with the tree rather than
    # part of the interpreter, and its top level is its own business.
    for (i = 1; i <= nt; i++)
        if (TK[i] == "/.defop" || TK[i] == "//.defop" || TK[i] == ".defop")
            MACH[TF[i]] = 1

    # ---- the definitions ----
    nd = 0
    for (i = 1; i <= nt; i++) {
        t = TK[i]
        if (t !~ /^\/[^\/]/) continue
        # allow an operand signature between the name and the procedure
        j = i + 1
        if (TK[j] == "[") {
            d = 0
            while (j <= nt) {
                if (TK[j] == "[") d++
                else if (TK[j] == "]") { d--; if (d == 0) { j++; break } }
                j++
            }
        }
        if (TK[j] != "{") continue
        # the matching close
        d = 0; k = j
        while (k <= nt) {
            if (TK[k] == "{") d++
            else if (TK[k] == "}") { d--; if (d == 0) break }
            k++
        }
        if (k > nt) continue
        nd++
        DNAME[nd] = substr(t, 2)
        DFILE[nd] = TF[i]; DLINE[nd] = TL[i]
        DFROM[nd] = j + 1; DTO[nd] = k - 1
        # a language operator is defined through .defop, just past the
        # procedure; a helper is put or def-ed
        DOP[nd] = 0
        for (m = k + 1; m <= k + 8 && m <= nt; m++)
            if (TK[m] == "/.defop" || TK[m] == "//.defop" || TK[m] == ".defop") { DOP[nd] = 1; break }
        DEFINED[DNAME[nd]] = 1
    }
    if (nd == 0) { print "ERROR\tno PostScript definition was parsed"; exit }

    # ---- what each definition names, and where ----
    for (d = 1; d <= nd; d++) {
        CLIP[d] = 0
        for (i = DFROM[d]; i <= DTO[d]; i++) {
            t = TK[i]
            if (t == "/.showclip" || t == "//.showclip" || t == ".showclip") {
                CLIP[d] = i - DFROM[d] + 1
                break
            }
        }
        if (CLIP[d] == 0) CLIP[d] = DTO[d] - DFROM[d] + 2
    }

    # ---- the fixed point ----
    for (d = 1; d <= nd; d++) UNG[d] = 0
    for (d = 1; d <= nd; d++) BADNAME[d] = ""
    changed = 1
    while (changed) {
        changed = 0
        for (d = 1; d <= nd; d++) {
            if (UNG[d]) continue
            for (i = DFROM[d]; i <= DTO[d]; i++) {
                pos = i - DFROM[d] + 1
                if (pos >= CLIP[d]) break
                who = use(i)
                if (who == "") continue
                nm = substr(who, 3)
                if (substr(who, 1, 1) == "B" || unguardedname(nm)) {
                    UNG[d] = 1; BADNAME[d] = nm; BADLINE[d] = TL[i]; changed = 1; break
                }
            }
        }
    }

    # ---- what fails ----
    for (d = 1; d <= nd; d++) {
        if (!DOP[d] || !UNG[d]) continue
        rel = DFILE[d]; sub(/^.*\/data\//, "data/", rel)
        print "OPFAIL\t" DNAME[d] "\t" rel ":" DLINE[d] "\t" BADNAME[d] "\t" BADLINE[d]
    }

    # The top level of a file: outside every procedure, and so outside
    # every operator that could have resolved the region. Only the
    # machinery is held to this -- a file that
    # defines no operator through .defop is a program, and a program
    # painting text at its top level is what all of this is for.
    depth = 0
    for (i = 1; i <= nt; i++) {
        t = TK[i]
        if (t == "{") { depth++; continue }
        if (t == "}") { if (depth > 0) depth--; continue }
        if (depth > 0 || !(TF[i] in MACH)) continue
        who = use(i)
        if (who == "") continue
        nm = substr(who, 3)
        if (substr(who, 1, 1) == "B" || unguardedname(nm)) {
            rel = TF[i]; sub(/^.*\/data\//, "data/", rel)
            print "TOPFAIL\t" rel ":" TL[i] "\t" nm
        }
    }

    # ---- what the tree holds, for the register ----
    for (d = 1; d <= nd; d++)
        if (DOP[d] && CLIP[d] <= DTO[d] - DFROM[d] + 1) print "RESOLVES\t" DNAME[d]
    print "COUNT\t" nd
}

# What a token calls, as "B:name" for the binding underneath a name or
# "D:name" for the definition over it, and "" for neither. A doubled
# slash names the binding the name held when the file was scanned; a
# plain token names whatever the name holds when it runs, which is the
# definition over it wherever there is one; a literal name is a call
# only where it is fetched out of a dictionary. That distinction is the
# whole of why xshow may call show and need nothing else.
function use(i,   t, nm) {
    t = TK[i]
    if (t ~ /^\/\/./) {
        nm = substr(t, 3)
        if (nm in BIND) return "B:" nm
        if (nm in DEFINED) return "D:" nm
        return ""
    }
    if (t ~ /^\/[^\/]/) {
        if (TK[i + 1] != "get") return ""
        nm = substr(t, 2)
        if (nm in DEFINED) return "D:" nm
        if (nm in BIND) return "B:" nm
        return ""
    }
    if (t !~ /^[A-Za-z.][A-Za-z0-9._]*$/) return ""
    if (t in DEFINED) return "D:" t
    if (t in BIND) return "B:" t
    return ""
}

# Whether calling this name reaches the raster unresolved. A language
# operator that does is answerable for itself and is reported on its own
# line, so it does not carry the fault out to everything that calls it:
# once a program can name the operator, that operator is where the call
# belongs. Only the helpers underneath one pass it on.
function unguardedname(nm,   d) {
    for (d = 1; d <= nd; d++)
        if (DNAME[d] == nm && UNG[d] && !DOP[d]) return 1
    return 0
}
' "$@" > "$work/ps"

if grep -q '^ERROR' "$work/ps"; then
    echo "FAILURES: no PostScript definition could be parsed under $src/data;"
    echo "      the tokeniser no longer reads the sources and every route"
    echo "      below would look resolved"
    exit 1
fi
ndefs=$(awk -F'\t' '$1 == "COUNT" { print $2 }' "$work/ps")
case ${ndefs:-} in
    ''|*[!0-9]*) echo "FAILURES: the PostScript scan reported no definition count"; exit 1 ;;
esac
if [ "$ndefs" -lt 100 ]; then
    echo "FAILURES: only $ndefs PostScript definitions were parsed; the"
    echo "      interpreter's own sources hold far more, so the scan is"
    echo "      reading a fraction of the tree"
    exit 1
fi

awk -F'\t' '$1 == "RESOLVES" { print "resolves " $2 }' "$work/ps" | sort -u > "$work/resolves"

if grep -q '^OPFAIL' "$work/ps"; then
    echo "FAILURES: a text operator reaches the glyph raster without"
    echo "      resolving the clipping region. The raster route plots a"
    echo "      glyph straight through the device's pixel methods and takes"
    echo "      the region out of the clip's cache holder, which .showclip"
    echo "      fills; an operator that skips it paints wherever the glyphs"
    echo "      fall, under any region a program has set. Run"
    echo "      .showclip before the text:"
    awk -F'\t' '$1 == "OPFAIL" {
            print "      " $3 "  " $2 "  reaches " $4 " at line " $5
        }' "$work/ps" | sort -u
    fail=1
fi

if grep -q '^TOPFAIL' "$work/ps"; then
    echo "FAILURES: the glyph raster is reached from the top level of a"
    echo "      file, where no operator has resolved the clipping region:"
    awk -F'\t' '$1 == "TOPFAIL" { print "      " $2 "  reaches " $3 }' "$work/ps" | sort -u
    fail=1
fi

# An empty set of resolving operators is what a tree with none of the
# calls left in it looks like, and the failures above have already said
# so; standing alone it means the scan found nothing and is agreeing
# with anything.
if [ ! -s "$work/resolves" ] && [ "$fail" = 0 ]; then
    echo "FAILURES: not one language operator resolves the clip before the"
    echo "      glyph raster; either the sources or this scan have changed"
    echo "      shape and nothing here is being held to anything"
    exit 1
fi

# ---- the register ---------------------------------------------------
sort -u "$work/derived" "$work/resolves" > "$work/found"
grep -vE '^[[:space:]]*(#|$)' "$golden" | tr -d '\r' \
  | grep -vE '^entries ' | awk '{ print $1 " " $2 }' | sort -u > "$work/recorded"

if [ ! -s "$work/recorded" ]; then
    echo "FAILURES: the register at $golden names no routes"
    exit 1
fi

guard_held=0
guard_hold "$work/recorded" "$work/found" \
    "in the register and no longer in the tree. Retire the line and the
      count above it together, so that a population which shrank says
      so:" \
    "routes to the glyph raster that are not in the register. Add them
      to tests/show_clip_routes.golden in the same commit, with the
      reason the route meets the clipping region:"
[ "$guard_held" -eq 0 ] || fail=1

entries=$(awk '/^entries /{ print $2; found = 1 } END { if (!found) print "" }' "$golden")
have=$(grep -c . "$work/recorded")
case $entries in
    ''|*[!0-9]*)
        echo "FAILURES: the register has no 'entries <n>' line"
        fail=1 ;;
    *)  if [ "$entries" -ne "$have" ]; then
            echo "FAILURES: the register records $entries routes and holds $have"
            fail=1
        fi ;;
esac

[ "$fail" = 0 ] || exit 1

nbind=$(grep -c . "$work/bindings")
nres=$(grep -c . "$work/resolves")
echo "SUCCESS ($nbind bindings read the clip cache, $nres operators resolve it, $ndefs procedures walked)"
exit 0
