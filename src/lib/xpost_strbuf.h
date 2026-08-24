/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_STRBUF_H
#define XPOST_STRBUF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpost_error.h"
#include "xpost_private.h" /* XPOST_PRINTF */

/* A growable byte buffer: the one allocation discipline behind every
   builder that assembles a byte stream of unknown final size -- the
   vector devices' page content, the font module's font programs, a
   compressed stream, a rereadable file's captured bytes. Growth doubles
   the capacity, so a build is linear in its output.

   Every function answers the operator convention the rest of the tree
   answers: 0 for no error, and otherwise the error code to raise. The
   only error a byte buffer has is VMerror, so a caller may return the
   result of a call directly. A call that answers VMerror leaves the
   buffer exactly as it found it, still holding its bytes and still
   usable; the caller releases it with xpost_strbuf_free.

   A zero-filled Xpost_String_Buffer is a valid empty buffer, so a holder
   that arrives by byte copy rather than by construction needs no
   initialiser. */
typedef struct
{
    char *s;
    size_t len;
    size_t cap;
} Xpost_String_Buffer;

static inline int
xpost_strbuf_init(Xpost_String_Buffer *b, size_t initial)
{
    if (initial < 16)
        initial = 16;
    b->s = (char *)malloc(initial);
    b->len = 0;
    b->cap = b->s ? initial : 0;
    return b->s ? 0 : VMerror;
}

/* Make room for extra more bytes past the current length.

   A capacity is only ever a capacity of something: a buffer answering 0
   holds bytes, so whatever reads or writes them past this call needs no
   further test. The room already being there is the only way out that
   does not allocate, and it is a way out only for a buffer that has
   allocated once.

   The largest buffer is the largest object. The bytes are one allocation
   and every read and write of them is pointer arithmetic within it, and a
   distance inside an object wider than PTRDIFF_MAX is not a value that
   arithmetic has, so a length past that bound is answered here rather
   than put to the allocator. What a growing buffer is given is the
   smallest doubling of sixteen that covers the length it must reach, or
   that length itself where a further doubling would carry past the bound.
   The doubling starts from sixteen rather than from the capacity the
   structure arrives holding, so a capacity is a distance the buffer's own
   pointers express whatever the structure held before. */
static inline int
xpost_strbuf_reserve(Xpost_String_Buffer *b, size_t extra)
{
    size_t need, cap;
    char *ns;

    if (extra > (size_t)PTRDIFF_MAX || b->len > (size_t)PTRDIFF_MAX - extra)
        return VMerror;
    need = b->len + extra;
    if (b->s && need <= b->cap)
        return 0;
    cap = 16;
    while (cap < need)
    {
        if (cap > (size_t)PTRDIFF_MAX / 2)
        {
            cap = need;
            break;
        }
        cap *= 2;
    }
    ns = (char *)realloc(b->s, cap);
    if (!ns)
        return VMerror;
    b->s = ns;
    b->cap = cap;
    return 0;
}

static inline int
xpost_strbuf_append(Xpost_String_Buffer *b, const void *p, size_t n)
{
    int ret;

    if (!n)
        return 0;
    ret = xpost_strbuf_reserve(b, n);
    if (ret)
        return ret;
    memcpy(b->s + b->len, p, n);
    b->len += n;
    return 0;
}

/* Append the formatted text, sized before it is written so the buffer
   grows once and the format runs at most twice.

   This one is declared here and defined in xpost_strbuf.c rather than
   written into every caller. A function reading a variable argument
   list is not a function a compiler can copy into a call site, so the
   copies the rest of this header hands out would each be a call to the
   same body under a different name.

   The format and the arguments are checked against each other: the
   buffer is the first parameter, so the format is the second and the
   arguments it names begin at the third. */
XPOST_PRINTF(2, 3)
int xpost_strbuf_appendf(Xpost_String_Buffer *b, const char *fmt, ...);

static inline void
xpost_strbuf_free(Xpost_String_Buffer *b)
{
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

#endif
