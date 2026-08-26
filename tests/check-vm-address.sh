#!/bin/sh
# Guard how a VM address is obtained, and how a VM pointer is derived from
# one. Two rules, one subject: the translation from "which entity" to
# "where its bytes are" happens in xpost_memory.h and nowhere else.
#
# RULE 1 -- a special entity's address is reached through its own accessor.
#
# The first few table slots hold structures the interpreter always has: the
# free lists, the save stack, the context list, the two halves of the name
# table, the operator table. Each is built once, by one constructor, and
# nextent only ever increments -- so once built, a special entity's row
# exists for the life of the memory file and its address cannot fail to be
# found.
#
# It was nevertheless reached through a fallible lookup that returned the
# address by out-parameter, and two callers in three dropped the answer:
# they passed an uninitialised local, ignored the refusal, and then used the
# local as an offset from mem->base. Nothing went wrong, because the refusal
# could not happen -- but nothing said so, the same lookup was spelled with
# four different messages for a branch none of them could take, and every
# one of those call sites read as though the entity might be missing and it
# were fine to carry on regardless.
#
# So the enumerators are named in the header that defines them and in the
# five constructors that build the entities. Everywhere else calls the
# accessor named for the entity, which returns the address directly.
#
# RULE 2 -- a pointer into VM is derived through xpost_vm_ptr.
#
# An address is an offset into a memory file that MOVES: any allocation may
# reallocate it, and every pointer taken before that moment is then stale.
# The hazard is why XPOST_GROW_MOVES and run-reloc-stress-test.sh exist, and
# it is what a SIGSEGV in dictionary growth turned out to be, at a site whose
# comment still described the defence a refactor had removed.
#
# It was spelled out at a hundred and fifty-six sites, eighty-four of them
# the identical cast to a stack pointer. That is a hundred and fifty-six
# places for the rule to lapse and one for it to be stated. It is now stated
# in xpost_memory.h, with the typed spellings -- xpost_stack_at,
# xpost_dict_head, xpost_operator_table -- built on top of it, so a later
# change to how a VM pointer is checked or tagged has one place to go.
#
# Usage: check-vm-address.sh <source root>

set -u
src=${1:?usage: check-vm-address.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"

guard_workdir
# read a tree whose lines end where the scans below expect them to
guard_mirror_tree "$src"
src=$mirror

lib="$src/src/lib"
guard_require_dir "$lib" "the library source directory"
header="$lib/xpost_memory.h"
guard_require_file "$header" "the header holding the accessors"

fail=0

# The four constructors, each of which allocates its entity through the one
# allocator that holds it to the slot the enumerator names. Nothing else may
# name one, and none of them may go back to checking it by hand.
#
# The operator table was a fifth. It is not an entity of the arena any
# more -- a signature carries this process's function pointers, and the
# arena holds no addresses -- so it is the memory file's own storage and
# builds no special entity.
cat > "$work/permitted" <<'EOF'
xpost_context.c xpost_context_init_ctxlist
xpost_free.c xpost_free_init
xpost_name.c xpost_name_init
xpost_save.c xpost_save_init
EOF

# Scan the sources BY NAME. A build in the tree leaves object files beside
# them whose debug information matches this pattern, so a directory walk is
# green where nothing was built and red where something was.
#
# Comments are stripped before matching -- prose about the mechanism, of
# which there is a good deal, is not a use of it -- and the enclosing
# function is tracked so a permitted site can be named by the constructor it
# belongs to. Function bodies in this tree open and close with a brace at
# column 0.
for f in "$lib"/*.c "$lib"/*.h; do
    [ -e "$f" ] || continue
    [ "$f" = "$header" ] && continue
    awk '
    {
        line = $0; code = ""
        while (length(line)) {
            if (incomment) {
                i = index(line, "*/")
                if (i == 0) { line = ""; break }
                line = substr(line, i + 2); incomment = 0; continue
            }
            i = index(line, "/*"); j = index(line, "//")
            if (j > 0 && (i == 0 || j < i)) { code = code substr(line, 1, j - 1); break }
            if (i == 0) { code = code line; break }
            code = code substr(line, 1, i - 1)
            line = substr(line, i + 2); incomment = 1
        }
        if (code ~ /^[A-Za-z_][A-Za-z0-9_ \t*]*\(/) {
            sig = code; sub(/\(.*/, "", sig); sub(/[ \t]*$/, "", sig)
            n = split(sig, parts, /[ \t*]+/); pending = parts[n]
        }
        if (code ~ /^\{/) curfn = pending
        if (code ~ /^\}/) curfn = ""
        if (code ~ /XPOST_MEMORY_TABLE_SPECIAL_[A-Z]/)
            printf "%s %s %d\n", FILENAME, (curfn == "" ? "<file-scope>" : curfn), FNR
    }' "$f"
done | sed "s|^$lib/||" > "$work/found"

while read -r file fn ln; do
    [ -n "${file:-}" ] || continue
    if ! grep -qx "$file $fn" "$work/permitted"; then
        echo "      $file:$ln (in $fn)"
        fail=1
    fi
done < "$work/found" > "$work/strays"

# Naming the enumerator in the right function is half of it. The other half
# is that the constructor asks for the slot rather than taking whatever it
# is given: the numbering is what every accessor subscripts by and what the
# collector's domain begins one past, and nothing arranges it except the
# order these run in. It was once ten checks in two minds -- five that
# aborted, five that wrote a warning and carried on -- so a constructor that
# writes its own check again is the state this returns to.
for f in "$lib"/*.c; do
    [ -e "$f" ] || continue
    awk '
    {
        line = $0; code = ""
        while (length(line)) {
            if (incomment) {
                i = index(line, "*/")
                if (i == 0) { line = ""; break }
                line = substr(line, i + 2); incomment = 0; continue
            }
            i = index(line, "/*"); j = index(line, "//")
            if (j > 0 && (i == 0 || j < i)) { code = code substr(line, 1, j - 1); break }
            if (i == 0) { code = code line; break }
            code = code substr(line, 1, i - 1)
            line = substr(line, i + 2); incomment = 1
        }
        if (code ~ /^[A-Za-z_][A-Za-z0-9_ \t*]*\(/) {
            sig = code; sub(/\(.*/, "", sig); sub(/[ \t]*$/, "", sig)
            n = split(sig, parts, /[ \t*]+/); pending = parts[n]
        }
        if (code ~ /^\{/) curfn = pending
        if (code ~ /^\}/) curfn = ""
        if (code ~ /xpost_memory_table_alloc_special[ \t]*\(/ && curfn != "")
            printf "%s %s\n", FILENAME, curfn
    }' "$f"
done | sed "s|^$lib/||" | sort -u > "$work/asks"

while read -r file fn; do
    [ -n "${file:-}" ] || continue
    if ! grep -qx "$file $fn" "$work/asks"; then
        echo "FAIL: $fn in $file builds a special entity without asking for"
        echo "      its slot. Allocate it with xpost_memory_table_alloc_special,"
        echo "      which refuses one that landed elsewhere, rather than"
        echo "      checking the number afterwards:"
        fail=1
    fi
done < "$work/permitted"

if [ -s "$work/strays" ]; then
    echo "FAIL: a special entity is named outside its constructor:"
    cat "$work/strays"
    echo "      call the accessor for it in xpost_memory.h --"
    echo "      xpost_memory_save_stack_ent(mem) and its siblings, which"
    echo "      answer directly because the entity cannot be missing"
    fail=1
fi

# Each constructor must still be there, and the header must still hold an
# accessor for every enumerator: otherwise this check passes because the
# thing it guards has moved, not because the rule holds.
while read -r file fn; do
    if ! grep -q "^[A-Za-z_].*[ *]$fn(" "$lib/$file"; then
        echo "FAILURES: $fn is not defined in $file; this check's list of"
        echo "      constructors is stale and it is no longer reading the"
        echo "      code it thinks it is"
        exit 1
    fi
done < "$work/permitted"

nspecial=$(sed -n '/^} Xpost_Memory_Table_Special;/q;p' "$header" \
           | grep -c '^ *XPOST_MEMORY_TABLE_SPECIAL_[A-Z_]*,\{0,1\}$')
naccessor=$(grep -cE '^xpost_memory_[a-z_]*_(adr|ent)\(Xpost_Memory_File \*mem\)$' "$header")
if [ "$nspecial" -lt 6 ]; then
    echo "FAILURES: the special-entity enum parsed as only $nspecial members;"
    echo "      it moved or changed shape and this check no longer reads it"
    exit 1
fi
# FREE, SAVE_STACK, CONTEXT_LIST, NAME_STACK and NAME_TREE each have an
# accessor. Three return an address; the two stacks return an entity,
# because a stack segment is an entity and each of those specials is its own
# stack's first segment. BOGUS_NAME is an entity number, not a structure,
# and is named only where the name stack is built. The operator table has
# no accessor here because it is no longer in virtual memory.
if [ "$naccessor" -ne 5 ]; then
    echo "FAILURES: $naccessor accessors for $nspecial special"
    echo "      entities; an entity without one has nothing to reach it by"
    echo "      and its callers will go back to the fallible lookup"
    exit 1
fi

# ---------------------------------------------------------------- rule 2
#
# Two structures unrelated to virtual memory also have a member called
# base, and adding to one is not a VM derivation: a DSC document's own
# byte buffer, and the record buffer of the binary-object-sequence writer,
# whose base is the header length rather than a memory file. They are
# exempted by the spelling that makes them what they are, so a real VM
# derivation cannot be mistaken for either.
for f in "$lib"/*.c "$lib"/*.h; do
    [ -e "$f" ] || continue
    [ "$f" = "$header" ] && continue
    awk '
    {
        line = $0; code = ""
        while (length(line)) {
            if (incomment) {
                i = index(line, "*/")
                if (i == 0) { line = ""; break }
                line = substr(line, i + 2); incomment = 0; continue
            }
            i = index(line, "/*"); j = index(line, "//")
            if (j > 0 && (i == 0 || j < i)) { code = code substr(line, 1, j - 1); break }
            if (i == 0) { code = code line; break }
            code = code substr(line, 1, i - 1)
            line = substr(line, i + 2); incomment = 1
        }
        # A local holding a memory file\047s base defeats the rule below:
        # once copied, base + adr reads as arithmetic on an ordinary
        # pointer and no arrow is left to find. That is how a stack was
        # reached at seven sites. The relocation detector keeps a base to
        # compare rather than to add to, and is what it is by its
        # spelling.
        if (code ~ /=[ \t]*[A-Za-z_][A-Za-z0-9_>.-]*->base[ \t]*;/ &&
            code !~ /sp->base/ && code !~ /seen_/) {
            printf "%s %d\n", FILENAME, FNR
            next
        }
        if (code !~ /->base[ \t]*\+/) next
        if (code ~ /b->buf[ \t]*\+[ \t]*b->base/) next     # record buffer
        if (code ~ /ctx->base[ \t]*\+[ \t]*ctx->length/) next  # DSC document
        printf "%s %d\n", FILENAME, FNR
    }' "$f"
done | sed "s|^$lib/||" > "$work/derivations"

if [ -s "$work/derivations" ]; then
    echo "FAIL: a VM pointer is derived without xpost_vm_ptr:"
    sed 's/ /:/; s/^/      /' "$work/derivations"
    echo "      use xpost_vm_ptr(mem, adr), or the typed spelling built on"
    echo "      it -- xpost_stack_at, xpost_dict_head, xpost_operator_table."
    echo "      A line holding a base in a local is reported for the same"
    echo "      reason: it is where the next such derivation comes from."
    echo "      The base moves under any allocation; one spelling is what"
    echo "      lets that be dealt with in one place rather than 156"
    fail=1
fi

# ---------------------------------------------------------------- rule 3
#
# Whether an entity number is usable is one question with one answer.
# It had three: xpost_ent_valid, a macro private to the table
# implementation, and fifteen sites that wrote the comparison out --
# including xpost_memory_table_get_addr, which hand-inlined the check
# rather than use the macro defined immediately above it, and a free-list
# walk that writes the comparison twice in one statement.
#
# Two places may still name the bound. xpost_memory.h states the
# predicate, and xpost_memory.c is the table's own implementation, where
# nextent is a field being maintained rather than a question being asked.
# Everywhere else, a comparison against it is a guard and belongs to the
# predicate -- except as a for-loop's bound, where it is an iteration
# limit and a call per step would reload it through the collector's
# hottest loop to say what the loop already knows.
for f in "$lib"/*.c "$lib"/*.h; do
    [ -e "$f" ] || continue
    case $(basename "$f") in
        xpost_memory.h|xpost_memory.c) continue ;;
    esac
    awk '
    {
        line = $0; code = ""
        while (length(line)) {
            if (incomment) {
                i = index(line, "*/")
                if (i == 0) { line = ""; break }
                line = substr(line, i + 2); incomment = 0; continue
            }
            i = index(line, "/*"); j = index(line, "//")
            if (j > 0 && (i == 0 || j < i)) { code = code substr(line, 1, j - 1); break }
            if (i == 0) { code = code line; break }
            code = code substr(line, 1, i - 1)
            line = substr(line, i + 2); incomment = 1
        }
        # the arrow of a member access carries a > that is not a
        # comparison, and reading nextent into a variable is not asking a
        # question about an entity; flatten the first and require a
        # relational operator so neither is mistaken for a guard
        flat = code
        gsub(/->/, ".", flat)
        if (flat !~ /nextent[ \t]*(==|!=|<=|>=|<|>)/ &&
            flat !~ /(==|!=|<=|>=|<|>)[ \t]*[A-Za-z_.]*nextent/) next
        # an iteration bound: a for whose header opens before the
        # comparison. Anchoring this to the start of the line would
        # exempt the four loops in the tree today and quietly fail to
        # exempt the fifth, written one brace further in.
        if (match(flat, /for[ \t]*\(/) && RSTART < index(flat, "nextent")) next
        printf "%s %d\n", FILENAME, FNR
    }' "$f"
done | sed "s|^$lib/||" > "$work/bounds"

if [ -s "$work/bounds" ]; then
    echo "FAIL: an entity's validity is decided without the predicate:"
    sed 's/ /:/; s/^/      /' "$work/bounds"
    echo "      use xpost_ent_valid(mem, ent), or"
    echo "      xpost_ent_in_collector_band(mem, ent) where the special"
    echo "      entities below mem->start are meant to be excluded too"
    fail=1
fi

if ! grep -q '^xpost_ent_in_collector_band(Xpost_Memory_File' "$header"; then
    echo "FAILURES: xpost_ent_in_collector_band is not defined in"
    echo "      xpost_memory.h; the band is no longer expressed in terms"
    echo "      of validity and the two can drift apart"
    exit 1
fi

# and the one spelling must still be there, or rule 2 passes because
# nothing derives a VM pointer at all any more
for want in xpost_vm_ptr xpost_ent_ptr; do
    if ! grep -q "^$want(Xpost_Memory_File" "$header"; then
        echo "FAILURES: $want is not defined in xpost_memory.h; the one"
        echo "      spelling moved and this check no longer guards it"
        exit 1
    fi
done
if ! grep -q 'return xpost_vm_ptr(' "$header"; then
    echo "FAILURES: xpost_ent_ptr no longer derives through xpost_vm_ptr;"
    echo "      the header has two spellings of its own"
    exit 1
fi
nstack=$(grep -c 'xpost_stack_at(' "$lib"/*.c "$lib"/*.h | grep -v ':0$' | grep -c .)

if [ "$fail" -ne 0 ]; then
    echo "FAILURES: a VM address is obtained or derived off the one path"
    exit 1
fi

echo "SUCCESS ($naccessor accessors; enumerators only in xpost_memory.h and $(grep -c . "$work/permitted") constructors; every VM pointer through xpost_vm_ptr, xpost_stack_at used in $nstack files)"
exit 0
