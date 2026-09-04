/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_array.c
 * @brief Arrays: a run of objects in the arena.
 *
 * As with strings, an interval shares storage. A packed array is the same
 * run stored more tightly and read-only, which is what a procedure is.
 */

/** \file xpost_array.c
   array functions
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"  /* arrays live in mfile, accessed via mtab */
#include "xpost_object.h"  /* array is an object, containing objects */
#include "xpost_stack.h"  /* may count the save stack */
#include "xpost_free.h"  /* arrays are allocated from the free list */

#include "xpost_save.h"  /* arrays obey save/restore */
#include "xpost_context.h"
//#include "xpost_interpreter.h"  /* banked arrays may be in global or local mfiles */
#include "xpost_error.h"  /* array functions may throw errors */
#include "xpost_array.h"  /* double-check prototypes */



/**
  Allocate array in specified memory file.

  Allocate an entity with xpost_memory_table_alloc,
   find the appropriate mtab,
   set the current save level in the "mark" field,
   wrap it up in an object.
*/
Xpost_Object xpost_array_cons_memory(Xpost_Memory_File *mem,
                                     unsigned int sz)
{
    unsigned int ent;
    Xpost_Object o = { 0 };
    unsigned int i;

    assert(mem->base);

    if (sz == 0)
    {
        ent = 0;
    }
    else
    {
        /* the storage is sz objects; a count whose byte size does not fit
           the width the allocator is asked in would wrap to a smaller
           allocation the fill loop then writes past. Refuse it here, as the
           dictionary and string constructors do. */
        if ((unsigned long long)sz * sizeof(Xpost_Object) > 0xFFFFFFFFULL)
        {
            XPOST_LOG_ERR("array of %u exceeds the storage the allocator counts", sz);
            return null;
        }
        if (!xpost_memory_table_alloc(mem,
                                      (unsigned int)(sz * sizeof(Xpost_Object)),
                                      arraytype,
                                      &ent))
        {
            XPOST_LOG_ERR("cannot allocate array");
            return null;
        }
        xpost_save_stamp_birth(mem, ent);

        /* fill array with the null object */
        for (i = 0; i < sz; i++)
        {
            int ret;

            ret = xpost_memory_put(mem,
                                   ent, i, (unsigned int)sizeof(Xpost_Object), &null);
            if (!ret)
            {
                XPOST_LOG_ERR("cannot fill array value");
                return null;
            }
        }
    }

    /* return (Xpost_Object){ .comp_.tag = arraytype, .comp_.sz = sz, .comp_.ent = ent, .comp_.off = 0}; */
    o.tag = arraytype
        | (XPOST_OBJECT_TAG_ACCESS_UNLIMITED
                << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    o.comp_.sz = (word)sz;
    o.comp_.off = 0;
    o = xpost_object_set_ent(o, ent);
    return o;
}

/**
  Allocate array in context's currently active memory file.

  Select a memory file according to vmmode,
   call xpost_array_cons_memory,
   set BANK flag.   object.tag&BANK?global:local
*/
Xpost_Object xpost_array_cons(Xpost_Context *ctx,
                              unsigned int sz)
{
    Xpost_Object a = xpost_array_cons_memory(ctx->vmmode==GLOBAL? ctx->gl: ctx->lo, sz);
    if (xpost_object_get_type(a) != nulltype)
    {
        if (ctx->vmmode==GLOBAL)
            a.tag |= XPOST_OBJECT_TAG_DATA_FLAG_BANK;
        xpost_stack_push(ctx->lo, ctx->hold, a); /* stash a reference on the hold stack in case of gc in caller */
    }
    return a;
}

/**
  Put object into array with given memory file.
  (Array must be valid for this memory file)

  Copy if necessary for save/restore,
   call memory_put.
*/
int xpost_array_put_memory(Xpost_Memory_File *mem,
                           Xpost_Object a,
                           integer i,
                           Xpost_Object o)
{
    int ret;
    ret = xpost_save_cow(mem, arraytype, a.comp_.sz, xpost_object_get_ent(a));
    if (ret)
        return ret;
    /* An index outside the array is an ordinary rangecheck the caller
       reports to the program, so it is answered quietly. */
    if (i < 0 || (unsigned int)i >= a.comp_.sz)
        return rangecheck;
    ret = xpost_memory_put(mem, xpost_object_get_ent(a),
                           (unsigned int)(a.comp_.off + i),
                           (unsigned int)sizeof(Xpost_Object), &o);
    if (!ret)
        return VMerror;
    return 0;
}

/**
  Put object into array.

  Select Xpost_Memory_File according to BANK flag,
   call xpost_array_put_memory.
*/
int xpost_array_put(Xpost_Context *ctx,
                    Xpost_Object a,
                    integer i,
                    Xpost_Object o)
{
    Xpost_Memory_File *mem = xpost_context_select_memory(ctx, a);

    /* the write needs write access, checked here so every caller
       inherits it (as xpost_dict_put_memory does for dictionaries).
       Interpreter start-up builds read-only structures before any
       program runs. */
    if (!ctx->gl->interpreter_get_initializing())
        if (!xpost_object_is_writeable(ctx, a))
            return invalidaccess;

    /* a value that belongs to local VM may not be made an element of an
       object in global VM, a restore being free to take the value away
       and leave the element naming nothing (PLRM 3.7.2) */
    if (!ctx->ignoreinvalidaccess)
    {
        if ( mem == ctx->gl &&
             xpost_object_is_banked(o) &&
             mem != xpost_context_select_memory(ctx, o))
            return invalidaccess;
    }

    return xpost_array_put_memory(mem, a, i, o);
}

/*
   Get object from array with specified memory file.
   (Array must be valid for this memory file)

   call memory_get.
 */
Xpost_Object xpost_array_get_memory(Xpost_Memory_File *mem,
                                    Xpost_Object a,
                                    integer i)
{
    Xpost_Object o = { 0 };
    int ret;

    /* An index outside the array is an ordinary rangecheck the caller
       reports, so it is answered here rather than left to the bound in
       xpost_memory_get, whose diagnostic is for an offset outside the
       allocation -- a fault in this interpreter, not in the program. */
    if (i < 0 || (unsigned int)i >= a.comp_.sz)
        return invalid;

    ret = xpost_memory_get(mem, xpost_object_get_ent(a),
                           (unsigned int)(a.comp_.off +i),
                           (unsigned int)(sizeof(Xpost_Object)), &o);
    if (!ret)
    {
        return invalid;
    }

    return o;
}

/*
   Get object from array.

   Select Xpost_Memory_File according to BANK flag,
   call xpost_array_get_memory.
 */
Xpost_Object xpost_array_get(Xpost_Context *ctx,
                             Xpost_Object a,
                             integer i)
{
    return xpost_array_get_memory(xpost_context_select_memory(ctx, a), a, i);
}

