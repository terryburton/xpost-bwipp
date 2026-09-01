/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_string.c
 * @brief Strings: a run of bytes in the arena.
 *
 * An interval of a string shares its storage rather than copying it, which
 * is what makes writing through one visible through the other.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h> /* size_t */
#include <string.h> /* memcpy */

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"  // strings live in mfile, accessed via mtab
#include "xpost_object.h"  // strings are objects
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_save.h"  /* a string carries a birth level like any entity */

#include "xpost_string.h"  // double-check prototypes

/* construct a stringtype object, with optional string value,
   in specified memory file
   */
Xpost_Object xpost_string_cons_memory(Xpost_Memory_File *mem,
                                      unsigned int sz,
                                      /*@NULL@*/ const char *ini)
{
    unsigned int ent;
    unsigned int room;
    Xpost_Object o = { 0 };
    int ret;

    /* A string object counts its length in comp_.sz, which is narrower
       than this argument on the narrow build. Sizes past what that field
       counts are refused: the answer is no string, which callers read as
       a refusal. A caller that has its own length to answer for tests
       the size before reaching here and says limitcheck. */
#ifndef WANT_LARGE_OBJECT
    /* the field is as wide as this argument on the large-object build */
    if (sz > XPOST_OBJECT_COMP_MAX_SZ)
    {
        XPOST_LOG_ERR("string of %u exceeds the length a string can count", sz);
        return null;
    }
#endif

    /* Asked for in whole objects, so that a string's storage begins and
       ends where one does. What that rounding adds is between one and
       eight bytes a string of this length does not count and nothing can
       read through it. */
    {
        /* Rounded in a width the sum cannot wrap: sz is as wide as the
           length a string counts on the large-object build, so a size in
           the top few of that range rounds to more than the width can
           hold. Left to wrap it would round to a tiny allocation the zero
           fill below then writes sz bytes into. Refuse it here -- no
           string, which the caller reads as a refusal -- rather than round
           into an allocation smaller than asked for. */
        size_t rounded = (((size_t)sz + sizeof(Xpost_Object))
                          / sizeof(Xpost_Object)) * sizeof(Xpost_Object);
        if (rounded > (size_t)~(unsigned int)0)
        {
            XPOST_LOG_ERR("string of %u rounds to more storage than the allocator counts", sz);
            return null;
        }
        room = (unsigned int)rounded;
    }
    if (!xpost_memory_table_alloc(mem, room, stringtype, &ent))
    {
        XPOST_LOG_ERR("cannot allocate string");
        return null;
    }
    /* The save level the storage was made at. A string takes no part in
       the copy-on-write snapshots -- PLRM 3.7.3 exempts string contents
       from restore, so nothing here ever asks whether this entity is
       backed up -- but restore still has to tell a string made since a
       save from one that predates it, and the birth stamp is where every
       entity says so. A row from the free list carries its last tenant's
       stamp until this writes one. */
    xpost_save_stamp_birth(mem, ent);
    if (ini)
    {
        ret = xpost_memory_put(mem, ent, 0, sz, ini);
        if (!ret)
        {
            XPOST_LOG_ERR("cannot store initial value in string");
            return null;
        }
    }
    else
    {
        /* the PLRM specifies zero-initialized strings; storage reused
           from the free list still holds the previous tenant's bytes */
        void *data = xpost_ent_ptr_checked(mem, ent);
        if (data)
            memset(data, 0, sz);
    }

    /* The rounding's own bytes, past the length the string counts. They
       are storage the file has handed out and nobody has written, so
       whatever the last tenant of a recycled block left is what stands
       there -- which for a block that once held an operator's signature
       is the address of a C function in this process. Nothing reads them
       through the string, and everything that reads virtual memory whole
       does. */
    {
        unsigned char *data = xpost_ent_ptr_checked(mem, ent);

        if (data)
            memset(data + sz, 0, room - sz);
    }
    /* every field carries a value before the object is passed by value */
    o.tag = stringtype | (XPOST_OBJECT_TAG_ACCESS_UNLIMITED << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    o.comp_.sz = sz;
    o.comp_.off = 0;
    o = xpost_object_set_ent(o, ent);

    return o;
}

/* construct a banked string object, with optional string value,
   using currently active memory file
   */
Xpost_Object xpost_string_cons(Xpost_Context *ctx,
                               unsigned int sz,
                               /*@NULL@*/ const char *ini)
{
    Xpost_Object s;
    s = xpost_string_cons_memory((ctx->vmmode == GLOBAL) ? ctx->gl : ctx->lo, sz, ini);
    if (xpost_object_get_type(s) != nulltype)
    {
        if (ctx->vmmode == GLOBAL)
            s.tag |= XPOST_OBJECT_TAG_DATA_FLAG_BANK;
        xpost_stack_push(ctx->lo, ctx->hold, s); /* stash a reference on the hold stack in case of gc in caller */
    }
    return s;
}

/* adapter:
            char* <- string object
    yield a real, honest-to-goodness pointer to the
    string in a stringtype object
    */
/*@dependent@*/
char *xpost_string_get_pointer(Xpost_Context *ctx,
                               Xpost_Object S)
{
    Xpost_Memory_File *mem;
    unsigned int ent = xpost_object_get_ent(S);
    char *data;
    mem = xpost_context_select_memory(ctx, S) /*S.tag&FBANK?ctx->gl:ctx->lo*/;
    data = xpost_ent_ptr_checked(mem, ent);
    if (!data)
    {
        /* a corrupt or sentinel ent (get_ent's -1 wraps to UINT_MAX) must
           not index past the table into a wild pointer */
        XPOST_LOG_ERR("%d entity number %u not found", VMerror, ent);
        return NULL;
    }
    return data + S.comp_.off;
}


/*
   put a value at index into a string using specified memory file
   (string must be valid for this memory file)

   No copy-on-write here, deliberately: PLRM 3.7.3 exempts strings from
   save/restore ("resets the values of all composite objects in local VM,
   except strings"), so unlike the array and dict mutators no
   xpost_save_ent call precedes the write and a written character
   survives restore.
 */
int xpost_string_put_memory(Xpost_Memory_File *mem,
                            Xpost_Object s,
                            integer i,
                            integer c)
{
    byte b = c;
    int ret;

    if (i < 0 || i >= s.comp_.sz)
        return rangecheck;
    ret = xpost_memory_put(mem, xpost_object_get_ent(s), s.comp_.off + i, 1, &b);
    if (!ret)
    {
        return rangecheck;
    }
    return 0;
}

/* put a value at index into a string */
int xpost_string_put(Xpost_Context *ctx,
                     Xpost_Object s,
                     integer i,
                     integer c)
{
    /* the write needs write access, checked here so every caller
       inherits it. Interpreter start-up builds read-only strings
       before any program runs. */
    if (!ctx->gl->interpreter_get_initializing())
        if (!xpost_object_is_writeable(ctx, s))
            return invalidaccess;

    return xpost_string_put_memory(xpost_context_select_memory(ctx, s) /*s.tag&FBANK? ctx->gl: ctx->lo*/, s, i, c);
}

/*
   get a value from a string at index using specified memory file
   (string must be valid for this memory file)
 */
int xpost_string_get_memory(Xpost_Memory_File *mem,
                            Xpost_Object s,
                            integer i,
                            integer *retval)
{
    byte b;
    int ret;

    if (i < 0 || i >= s.comp_.sz)
        return rangecheck;
    ret = xpost_memory_get(mem, xpost_object_get_ent(s), s.comp_.off + i, 1, &b);
    if (!ret)
    {
        return rangecheck;
    }

    *retval = b;
    return 0;
}

/* get a value from a string at index */
int xpost_string_get(Xpost_Context *ctx,
                     Xpost_Object s,
                     integer i,
                     integer *retval)
{
    return xpost_string_get_memory(xpost_context_select_memory(ctx, s) /*s.tag&FBANK? ctx->gl: ctx->lo*/, s, i, retval);
}

/* allocate and return a C-style nul-terminated string */
char *xpost_string_allocate_cstring(Xpost_Context *ctx,
                                    Xpost_Object s)
{
    char *p = calloc( s.comp_.sz + 1, 1 );
    char *bytes;

    if (!p)
        return NULL;
    /* a string object whose entity no longer describes a string yields no
       pointer; the nul the allocation already holds is then the whole
       answer, so the caller gets an empty string rather than a copy of
       whatever the entity now holds */
    bytes = xpost_string_get_pointer(ctx, s);
    if (bytes)
        memcpy( p, bytes, s.comp_.sz );
    return p;
}

