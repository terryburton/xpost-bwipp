#!/bin/sh
# Guard the single growable byte buffer. Every builder in the tree that
# assembles a byte stream of unknown final size -- the vector devices'
# page content, the font module's font programs and glyph fragments, a
# deflated stream, a rereadable file's captured source -- grows it
# through src/lib/xpost_strbuf.h, and every one of them reads the same
# answer from it.
#
# A second grow loop, a buffer carried as a raw pointer/length/capacity
# triple, or a caller reading the answer as a success flag means the
# tree is growing byte buffers more than one way again.
#
#   $1  path to the source tree root
set -u
src=${1:?usage: check-one-bytebuf.sh <source root>}
. "$(dirname "$0")/guard-paths.sh"
guard_require_srcroot "$src"
lib=$src/src/lib
fail=0

# the buffer type is declared in exactly one place
hits=$(grep -l '^} Xpost_String_Buffer;' "$lib"/*.h "$lib"/*.c 2>/dev/null || true)
if [ "$hits" != "$lib/xpost_strbuf.h" ]; then
    echo "check-one-bytebuf: expected the only growable byte buffer in src/lib/xpost_strbuf.h, found:"
    printf '%s\n' "${hits:-none}"
    fail=1
fi

# it answers one convention: 0 for no error, the error code otherwise
if grep -qE '^ *return (-1|1);' "$lib/xpost_strbuf.h"; then
    echo "check-one-bytebuf: xpost_strbuf.h answers a value that is neither 0 nor an error code:"
    grep -nE '^ *return (-1|1);' "$lib/xpost_strbuf.h"
    fail=1
fi
if ! grep -q 'return VMerror;' "$lib/xpost_strbuf.h"; then
    echo "check-one-bytebuf: xpost_strbuf.h no longer answers VMerror for failure"
    fail=1
fi

# no byte buffer is threaded through a call as a raw pointer/length/capacity
# triple rather than as the buffer itself
hits=$(grep -rnE 'char \*\*[A-Za-z_]+, *size_t \*' "$lib" "$src/src/bin" 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "check-one-bytebuf: a byte buffer is passed as a raw pointer/length/capacity triple:"
    printf '%s\n' "$hits"
    fail=1
fi

# the byte-stream builders reach the shared buffer
for f in xpost_op_font.c xpost_dev_generic.c xpost_file.c; do
    if ! grep -q '#include "xpost_strbuf.h"' "$lib/$f"; then
        echo "check-one-bytebuf: $f does not reach the shared byte buffer"
        fail=1
    fi
done

# and nothing grows one of its own. The scan is the whole library and the
# programs, not the three builders alone: a second grow loop is a second
# grow loop wherever it lands, and the file it lands in is the one nobody
# thought to name here.
#
# What is left out is not a byte buffer.
#   *sizeof              an array of typed elements, which moves objects
#                        inside the memory file rather than bytes inside
#                        a buffer
#   xpost_memory.c       the memory file itself, the allocation every
#                        object in virtual memory lives inside
#   xpost_compat.c       the glob() shim's result block. It is not a byte
#                        stream: it ends holding a pointer table as well
#                        as the names, and the caller frees the whole
#                        thing through globfree, so the buffer it hands
#                        back cannot be a structure with a lifetime of
#                        its own
#   xpost_operator.c     the operator table's storage, for the same
#                        reason as xpost_memory.c: it is an allocator
#                        rather than a byte stream. What it hands out are
#                        rows and runs of signatures, named by offsets
#                        from its start so that growing it moves nothing
#                        that names what is in it -- which is the whole
#                        of why it cannot be a string buffer, since a
#                        string buffer's business is bytes and their end
hits=$(grep -n 'realloc(' "$lib"/*.c "$src"/src/bin/*.c 2>/dev/null \
       | grep -v 'sizeof' | grep -v 'write_capacity' \
       | grep -v 'xpost_memory\.c:' \
       | grep -v 'xpost_operator\.c:' \
       | grep -v 'xpost_compat\.c:' || true)
if [ -n "$hits" ]; then
    echo "check-one-bytebuf: a byte buffer is grown outside xpost_strbuf.h:"
    printf '%s\n' "$hits"
    fail=1
fi

# nobody reads the answer as a success flag
hits=$(grep -rnE 'if \(!(xpost_strbuf_|xpost_dev_pdf_append)' "$lib" 2>/dev/null || true)
if [ -n "$hits" ]; then
    echo "check-one-bytebuf: a caller reads a buffer answer as a success flag:"
    printf '%s\n' "$hits"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "check-one-bytebuf: the tree grows byte buffers more than one way."
    exit 1
fi
echo "check-one-bytebuf: ok (one buffer, one convention)"
exit 0
