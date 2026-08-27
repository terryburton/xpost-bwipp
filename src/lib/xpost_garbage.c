/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_garbage.c
 * @brief The collector: what is still reachable, and what the sweep takes back.
 *
 * Mark and sweep over both banks. Marking walks the object graph from the
 * roots a context holds, over a worklist of the collector's own rather than
 * over the C stack, so that a graph deeper than the stack is a slower
 * collection rather than a crash.
 *
 * The sweep hands what nothing reached to the free lists, and describes
 * each reclaimed entity to a memory checker as storage the interpreter has
 * taken back -- which is what makes a reference something still holds
 * report against whoever made it rather than read as an ordinary byte.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h> /* close */
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_free.h"
#include "xpost_context.h"
#include "xpost_array.h"
#include "xpost_string.h"
#include "xpost_dict.h"
#include "xpost_save.h"
#include "xpost_name.h"
#include "xpost_file.h"
#include "xpost_handle.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_garbage.h"

#ifdef DEBUG_GC
#include <stdio.h>
#endif


/* Marking walks the object graph over the worklist below rather than
   over the C stack. Nothing in the language bounds how deeply a program
   may nest one composite inside another -- a chain of arrays each
   holding the next costs only the entity numbers and the virtual memory
   it spends -- so a walk that took a C frame per link would reach the
   end of the stack while the program was still inside every limit the
   specification names, and end the process rather than the program.

   The worklist is storage of the collector's own, fixed at load time, so
   walking never allocates: a mark that had to allocate in order to walk
   would fail exactly where the collector is what a shortage calls for.
   What fixed storage cannot promise is room for a frontier of any width,
   so an entity is marked before it is queued, and one that finds the
   worklist full is left carrying a bit in its mark word in place of a
   seat in the queue. When the worklist next empties, the tables are
   walked for entities carrying that bit and the descent resumes from
   each. An entity is queued only in the step that marks it, and an
   entity is marked once, so every such pass takes up entities no earlier
   pass reached and the walk ends. */

/* the mark word packs four fields into its low 31 bits (see
   Xpost_Memory_Table_Mark_Data); the top bit is the collector's, and
   says that an entity is marked but not yet descended into */
#define XPOST_GARBAGE_MARK_WAITING 0x80000000u

#define XPOST_GARBAGE_WORK_MAX 8192

/* an entity whose contents are still to be walked, with the memory file
   it belongs to: a composite in one bank may hold objects in the other */
typedef struct
{
    Xpost_Memory_File *mem;
    unsigned int ent;
    unsigned int isdict;
} Xpost_Garbage_Work;

static Xpost_Garbage_Work _xpost_garbage_work[XPOST_GARBAGE_WORK_MAX];
static unsigned int _xpost_garbage_work_n;
static int _xpost_garbage_work_waiting;
static int _xpost_garbage_work_ready;

/* the bank the current collection is of, set at each collection's
   entry. A walk asked for one bank stops at any reference that leaves
   this file: local may name global freely, so a local walk that crossed
   would traverse the whole global graph to protect a bank it does not
   sweep. The references the other way are anchored separately (see
   _xpost_garbage_mark_systemdict_exceptions). */
static Xpost_Memory_File *_xpost_garbage_collect_bank;

/* clear the waiting flag over a table, leaving the marks as they are */
static
void _xpost_garbage_unwait(Xpost_Memory_File *mem)
{
    unsigned int i;

    if (!mem) return;

    for (i = mem->start; i < mem->table.nextent; i++)
    {
        mem->table.tab[i].mark &= ~(unsigned int)XPOST_GARBAGE_MARK_WAITING;
    }
}

/* queue a marked entity for its contents to be walked, or, if the
   worklist is full, leave it for the table pass to take up.

   The flag is read back out of the mark word, so a bit found set has to
   be one this walk set: a table slot handed out for the first time
   carries whatever the table's growth left in that word. Both banks are
   swept clear of it here, on the first entity of a collection that has
   to wait, rather than where a collection begins -- a walk whose
   frontier stays inside the worklist never reads the flag at all, and
   most do. */
static
void _xpost_garbage_work_push(Xpost_Context *ctx,
                              Xpost_Memory_File *mem,
                              unsigned int ent,
                              unsigned int isdict)
{
    if (_xpost_garbage_work_n < XPOST_GARBAGE_WORK_MAX)
    {
        _xpost_garbage_work[_xpost_garbage_work_n].mem = mem;
        _xpost_garbage_work[_xpost_garbage_work_n].ent = ent;
        _xpost_garbage_work[_xpost_garbage_work_n].isdict = isdict;
        ++_xpost_garbage_work_n;
        return;
    }

    if (!_xpost_garbage_work_ready)
    {
        _xpost_garbage_unwait(ctx->lo);
        _xpost_garbage_unwait(ctx->gl);
        _xpost_garbage_work_ready = 1;
    }
    mem->table.tab[ent].mark |= XPOST_GARBAGE_MARK_WAITING;
    _xpost_garbage_work_waiting = 1;
}

/* iterate through all tables,
    clear the MARK in the mark. */
static
void _xpost_garbage_unmark(Xpost_Memory_File *mem)
{
    unsigned int i;

    if (!mem) return;

    for (i = mem->start; i < mem->table.nextent; i++)
    {
        mem->table.tab[i].mark &= ~XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK;
    }
}

/* set the MARK in the mark in the tab[ent] */
static
int _xpost_garbage_mark_ent(Xpost_Memory_File *mem,
                            unsigned int ent)
{
    if (!mem) return 0;

    if (ent < mem->start)
    {
        XPOST_LOG_ERR("attempt to mark ent %u < mem->start", ent);
        return 1;
    }

    if (!xpost_ent_valid(mem, ent))
    {
        XPOST_LOG_ERR("cannot find ent %u", ent);
        return 0;
    }
    mem->table.tab[ent].mark |= XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK;
    return 1;
}

/* is it marked? */
static
int _xpost_garbage_ent_is_marked(Xpost_Memory_File *mem,
                                 unsigned int ent,
                                 int *retval)
{
    if (!mem) return 0;

    if (!xpost_ent_valid(mem, ent))
    {
        XPOST_LOG_ERR("cannot find table for ent %u", ent);
        return 0;
    }
    *retval = (mem->table.tab[ent].mark & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK)
        >> XPOST_MEMORY_TABLE_MARK_DATA_MARK_OFFSET;

    return 1;
}

/* mark what an object refers to, queueing it if it has contents to walk */
static
int _xpost_garbage_mark_reach(Xpost_Context *ctx, Xpost_Memory_File *mem, Xpost_Object o, int markall);

/* mark what a dictionary's keys and values refer to */
static
int _xpost_garbage_reach_dict(Xpost_Context *ctx,
                              Xpost_Memory_File *mem,
                              unsigned int adr,
                              int markall)
{
    if (!mem) return 0;

    {
        dichead *dp = xpost_dict_head_at(mem, adr);
        dicrec *tp = xpost_dict_table_of(dp);
        /* the table is more than twice as long as the size it is derived
           from, so it is counted in a type wide enough for that product
           rather than in the type the size itself is stored in */
        unsigned int n = DICTABN(dp->sz);
        unsigned int j;
#ifdef DEBUG_GC
        Xpost_Object_Type type;
        printf("markdict: nused=%u\n", (unsigned int)dp->nused);
#endif

        for (j = 0; j < n; j++)
        {
            if (xpost_object_get_type(tp[j].key) != nulltype){
                if (!_xpost_garbage_mark_reach(ctx,
                            xpost_context_select_memory(ctx,tp[j].key), tp[j].key, markall))
                    return 0;
#ifdef DEBUG_GC
            switch(type = xpost_object_get_type(tp[j].key)){
            default: {
                    printf("%s", xpost_object_type_names[type]);
                }
                break;
            case nametype: {
                unsigned int address;
                Xpost_Object str;

                address = xpost_memory_name_stack_ent(
                    xpost_context_select_memory(ctx, tp[j].key));

                str = xpost_stack_bottomup_fetch(
                    xpost_context_select_memory(ctx,tp[j].key),
                    address, tp[j].key.mark_.padw);
                printf("%*s", str.comp_.sz, xpost_string_get_pointer(ctx,str));

                }
                break;
            }
            printf(":");
            printf("%s\n", xpost_object_type_names[xpost_object_get_type(tp[j].value)]);
#endif
                if (!_xpost_garbage_mark_reach(ctx,
                            xpost_context_select_memory(ctx,tp[j].value), tp[j].value, markall))
                    return 0;
            }
        }
    }

    return 1;
}

/* mark what an array's elements refer to */
static
int _xpost_garbage_reach_array(Xpost_Context *ctx,
                               Xpost_Memory_File *mem,
                               unsigned int adr,
                               unsigned int sz,
                               int markall)
{
    if (!mem) return 0;

#ifdef DEBUG_GC
    printf("markarray: sz=%u\n", sz);
#endif

    {
        Xpost_Object *op = xpost_vm_ptr(mem, adr);
        unsigned int j;

        for (j = 0; j < sz; j++)
        {
#ifdef DEBUG_GC
            printf("%u:%s\n", j, xpost_object_type_names[xpost_object_get_type(op[j])]);
#endif
            if (!_xpost_garbage_mark_reach(ctx,
                                           xpost_context_select_memory(ctx,op[j]),
                                           op[j], markall))
                return 0;
        }
    }

    return 1;
}

/* mark the entity an object names, and queue it if it has contents of
   its own to walk.
   if markall is true, the collection covers both banks and the walk
   crosses between them; otherwise the walk stays in the bank being
   collected and stops at any reference that leaves it -- what lies
   beyond is not swept, and the sanctioned references back into the
   collected bank are anchored separately
 */
static
int _xpost_garbage_mark_reach(Xpost_Context *ctx,
                              Xpost_Memory_File *mem,
                              Xpost_Object o,
                              int markall)
{
    int ret;
    unsigned int ent;
    Xpost_Object_Type type;
    Xpost_Memory_File *objmem;

    if (!mem) return 0;

    if (xpost_object_get_type(o) == filetype)
    {
        unsigned int fent = (unsigned int)o.mark_.padw;
        Xpost_Memory_File *fm = xpost_context_select_memory(ctx, o);

        /* a file in a bank the collection does not cover is not swept,
           and what it holds lives with it */
        if (!markall && fm != _xpost_garbage_collect_bank)
            return 1;
        /* a filter reads through the stream beneath it, which has an
           entity of its own that no object names -- the string forms of
           filter build one and hand it over. Follow the chain down from
           the filter, or the sweep takes a source out from under a filter
           still reading it. The walk stops at a stream already marked,
           which is also what ends it if a chain ever reaches itself.

           Every file object is answered here, so the type switch below
           carries no arm for one. */
        while (fm && xpost_ent_in_collector_band(fm, fent)
               && !(fm->table.tab[fent].mark
                    & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK))
        {
            int nheld, h;

            /* a stream whose bytes a procedure supplies or disposes of
               holds that procedure, and the strings it has exchanged
               with it, in the struct rather than in virtual memory:
               nothing else names any of them, so the sweep would take
               them out from under a filter still reading or writing */
            nheld = xpost_file_held_count(fm, fent);
            for (h = 0; h < nheld; h++)
            {
                Xpost_Object held = xpost_file_held_object(fm, fent, h);

                if (!_xpost_garbage_mark_reach(ctx,
                        xpost_context_select_memory(ctx, held),
                        held, markall))
                    return 0;
            }
            if (!_xpost_garbage_mark_ent(fm, fent))
                break;
            fent = xpost_file_underlying_entity(fm, fent);
        }
        return 1;
    }

    if (!xpost_object_is_composite(o))
        return 1;

    ent = xpost_object_get_ent(o);
    type = xpost_object_get_type(o);

#ifdef DEBUG_GC
            printf("markobject: ent %u, addr %u, %s (size %u)\n",
                   ent,
                   xpost_context_select_memory(ctx,o)==mem?
                       (!xpost_ent_valid(mem, ent)?
                        (unsigned)-1: mem->table.tab[ent].adr) : 0,
                   xpost_object_type_names[type],
                   (unsigned int)o.comp_.sz);
#endif

    switch(type)
    {
        default: break;

        case arraytype:
            if (ent == 0)
            {
                return 1;
            }

            objmem = xpost_context_select_memory(ctx, o);
            if (!objmem) return 0;
            if (!markall && objmem != _xpost_garbage_collect_bank)
                break;
            if (ent < objmem->start)
            {
                XPOST_LOG_ERR("attempt to mark %s object %d",
                        xpost_object_type_names[type],
                        ent);
                return 0;
            }
            if (!_xpost_garbage_ent_is_marked(objmem, ent, &ret))
                return 0;
            if (!ret) {
                ret = _xpost_garbage_mark_ent(objmem, ent);
                if (!ret)
                {
                    XPOST_LOG_ERR("cannot mark array %d", ent);
                    return 0;
                }
                /* subarray views share the entity: the descent covers the
                   whole underlying allocation, not the view window, so
                   elements reachable only through another view stay marked
                   (the ent-level marked flag makes the first view visited
                   the only one queued) */
                _xpost_garbage_work_push(ctx, objmem, ent, 0);
            }
            break;

        case dicttype:
            objmem = xpost_context_select_memory(ctx, o);
            if (!objmem) return 0;
            if (!markall && objmem != _xpost_garbage_collect_bank)
                break;
            if (ent < objmem->start)
            {
                XPOST_LOG_ERR("attempt to mark %s object %d",
                        xpost_object_type_names[type],
                        ent);
                return 0;
            }
            if (!_xpost_garbage_ent_is_marked(objmem, ent, &ret))
                return 0;
            if (!ret)
            {
                ret = _xpost_garbage_mark_ent(objmem, ent);
                if (!ret)
                {
                    XPOST_LOG_ERR("cannot mark dict");
                    return 0;
                }
                _xpost_garbage_work_push(ctx, objmem, ent, 1);
            }
            break;

        case stringtype:
            if (ent == 0)
            {
                return 1;
            }

            objmem = xpost_context_select_memory(ctx, o);
            if (!objmem) return 0;
            if (!markall && objmem != _xpost_garbage_collect_bank)
                break;

            if (ent < objmem->start)
            {
                XPOST_LOG_ERR("attempt to mark %s object %d",
                        xpost_object_type_names[type],
                        ent);
                return 0;
            }
            ret = _xpost_garbage_mark_ent(objmem, ent);
            if (!ret)
            {
                XPOST_LOG_ERR("cannot mark string");
                return 0;
            }
            break;
    }

    return 1;
}

/* mark what a queued entity holds. Its slot is read directly: an entity
   reaches the worklist only through the step that marks it, which is
   where the number is checked against the table. */
static
int _xpost_garbage_mark_contents(Xpost_Context *ctx,
                                 Xpost_Memory_File *mem,
                                 unsigned int ent,
                                 unsigned int isdict,
                                 int markall)
{
    if (isdict)
        return _xpost_garbage_reach_dict(ctx, mem,
                                         mem->table.tab[ent].adr, markall);

    return _xpost_garbage_reach_array(ctx, mem,
                                      mem->table.tab[ent].adr,
                                      mem->table.tab[ent].used
                                          / sizeof(Xpost_Object),
                                      markall);
}

/* walk the worklist until nothing is queued */
static
int _xpost_garbage_mark_drain(Xpost_Context *ctx, int markall)
{
    while (_xpost_garbage_work_n)
    {
        Xpost_Garbage_Work w = _xpost_garbage_work[--_xpost_garbage_work_n];

        if (!_xpost_garbage_mark_contents(ctx, w.mem, w.ent, w.isdict, markall))
        {
            _xpost_garbage_work_n = 0;
            return 0;
        }
    }

    return 1;
}

/* take up the entities the worklist had no room for, descending each
   before the next is queued so that the worklist has room for it */
static
int _xpost_garbage_mark_resume(Xpost_Context *ctx,
                               Xpost_Memory_File *mem,
                               int markall)
{
    unsigned int i;

    if (!mem) return 1;

    for (i = mem->start; i < mem->table.nextent; i++)
    {
        if (!(mem->table.tab[i].mark & XPOST_GARBAGE_MARK_WAITING))
            continue;
        mem->table.tab[i].mark &= ~(unsigned int)XPOST_GARBAGE_MARK_WAITING;
        _xpost_garbage_work_push(ctx, mem, i,
                                 mem->table.tab[i].tag == dicttype);
        if (!_xpost_garbage_mark_drain(ctx, markall))
            return 0;
    }

    return 1;
}

/* walk everything queued, and everything queued by walking it */
static
int _xpost_garbage_mark_walk(Xpost_Context *ctx, int markall)
{
    for (;;)
    {
        if (!_xpost_garbage_mark_drain(ctx, markall))
            return 0;
        if (!_xpost_garbage_work_waiting)
            return 1;
        _xpost_garbage_work_waiting = 0;
        /* an entity waits only where it was marked, and a context marks
           in the two banks it selects between, so these are all of them */
        if (!_xpost_garbage_mark_resume(ctx, ctx->lo, markall))
            return 0;
        if (!_xpost_garbage_mark_resume(ctx, ctx->gl, markall))
            return 0;
    }
}

/* mark an object and everything reachable through it */
static
int _xpost_garbage_mark_object(Xpost_Context *ctx,
                               Xpost_Memory_File *mem,
                               Xpost_Object o,
                               int markall)
{
    if (!_xpost_garbage_mark_reach(ctx, mem, o, markall))
    {
        _xpost_garbage_work_n = 0;
        return 0;
    }

    return _xpost_garbage_mark_walk(ctx, markall);
}

/* mark an array's elements and everything reachable through them */
static
int _xpost_garbage_mark_array(Xpost_Context *ctx,
                              Xpost_Memory_File *mem,
                              unsigned int adr,
                              unsigned int sz,
                              int markall)
{
    if (!_xpost_garbage_reach_array(ctx, mem, adr, sz, markall))
    {
        _xpost_garbage_work_n = 0;
        return 0;
    }

    return _xpost_garbage_mark_walk(ctx, markall);
}

/* mark a dictionary's entries and everything reachable through them */
static
int _xpost_garbage_mark_dict(Xpost_Context *ctx,
                             Xpost_Memory_File *mem,
                             unsigned int adr,
                             int markall)
{
    if (!_xpost_garbage_reach_dict(ctx, mem, adr, markall))
    {
        _xpost_garbage_work_n = 0;
        return 0;
    }

    return _xpost_garbage_mark_walk(ctx, markall);
}


/* Mark every segment of a stack, so the sweep does not take back the
   stack a program is running on.

   A segment is an entity, and an entity nothing marks is one the sweep
   takes back. The whole chain is marked, not the part a read of the
   stack would walk: a walk of the contents stops at the topmost segment,
   whereas a stack that grew and then shrank keeps the segments past that
   one chained, and a later push moves straight back into them rather
   than making another. They are as live as the ones below them.

   The walk is by number because the number is what a mark is set on. */
static
int _xpost_garbage_mark_stack_segments(Xpost_Memory_File *mem,
                                       unsigned int stackent)
{
    unsigned int e;

    if (!mem) return 0;

    for (e = stackent; e; )
    {
        /* the first segment of the name stack and of the save stack is a
           special entity, below the band the collector owns and never
           swept; the walk passes through it to reach the rest */
        if (xpost_ent_in_collector_band(mem, e)
            && !_xpost_garbage_mark_ent(mem, e))
            return 0;
        e = xpost_stack_at(mem, e)->nextseg;
    }
    return 1;
}


/* Mark the record stacks a restore parked for reuse.

   The pool is a chain through each parked stack's own prevseg, the last
   naming itself, and nothing else in the heap reaches it. Those stacks
   are entities, so a pool nothing marks is a pool the sweep empties out
   from under the next save that takes one. */
static
int _xpost_garbage_mark_substack_pool(Xpost_Memory_File *mem)
{
    unsigned int e;

    if (!mem) return 0;

    e = mem->free_substack;
    while (e)
    {
        unsigned int next;

        if (!_xpost_garbage_mark_stack_segments(mem, e))
            return 0;
        next = xpost_stack_at(mem, e)->prevseg;
        e = (next == e) ? 0 : next;
    }
    return 1;
}


/* mark all names in stack except 0::BOGUSNAME */
static
int _xpost_garbage_mark_names(Xpost_Context *ctx,
                              Xpost_Memory_File *mem,
                              unsigned int stackent,
                              int markall)
{
    unsigned int start = 1; /* skip 0::BOGUSNAME */
    Xpost_Stack *s;
    unsigned int i;
    if (!mem) return 0;

#ifdef DEBUG_GC
    printf("marking stack of size %d\n", xpost_stack_count(mem, stackent));
#endif

    if (!_xpost_garbage_mark_stack_segments(mem, stackent))
        return 0;

    for (s = xpost_stack_at(mem, stackent); s;
         s = xpost_stack_next_segment(mem, s), start = 0)
    {
        for (i = start; i < s->top; i++)
        {
            if (!_xpost_garbage_mark_object(ctx, mem, s->data[i], markall))
                return 0;
        }
    }

    return 1;
}


/* mark all allocations referred to by objects in stack */
static
int _xpost_garbage_mark_stack(Xpost_Context *ctx,
                              Xpost_Memory_File *mem,
                              unsigned int stackent,
                              int markall)
{
    Xpost_Stack *s;
    unsigned int i;
    if (!mem) return 0;

#ifdef DEBUG_GC
    printf("marking stack of size %d\n", xpost_stack_count(mem, stackent));
#endif

    if (!_xpost_garbage_mark_stack_segments(mem, stackent))
        return 0;

    for (s = xpost_stack_at(mem, stackent); s;
         s = xpost_stack_next_segment(mem, s))
    {
        for (i = 0; i < s->top; i++)
        {
            Xpost_Memory_File *objmem;
            objmem = xpost_context_select_memory(ctx, s->data[i]);
            if (objmem == mem || markall)
                if (!_xpost_garbage_mark_object(ctx, objmem, s->data[i], markall))
                    return 0;
        }
    }

    return 1;
}

/* mark all allocations referred to by objects in save object's stack of saverec_'s */
static
int _xpost_garbage_mark_save_stack(Xpost_Context *ctx,
                                   Xpost_Memory_File *mem,
                                   unsigned int stackent,
                                   int markall)
{
    if (!mem) return 0;

    {
        Xpost_Stack *s;
        unsigned int i;
        unsigned int ad;
        int ret;
        (void)ctx;

#ifdef DEBUG_GC
        printf("marking saverec stack of size %d\n", xpost_stack_count(mem, stackent));
#endif

    if (!_xpost_garbage_mark_stack_segments(mem, stackent))
        return 0;

    for (s = xpost_stack_at(mem, stackent); s;
         s = xpost_stack_next_segment(mem, s))
    {
        for (i = 0; i < s->top; i++)
        {
            /* saverec entity numbers may exceed a word; decode their full
               value and type through the accessors (see xpost_save.h)
               rather than reading the packed fields raw. */
            unsigned int rsrc = XPOST_SAVEREC_SRC(s->data[i]);
            unsigned int rcpy = XPOST_SAVEREC_CPY(s->data[i]);
            unsigned int rtype = XPOST_SAVEREC_TYPE(s->data[i]);
            /* _xpost_garbage_mark_object(ctx, mem, s->data[i]); */
            /* _xpost_garbage_mark_save_stack(ctx, mem, s->data[i].save_.stk); */
            ret = _xpost_garbage_mark_ent(mem, rsrc);
            if (!ret)
            {
                XPOST_LOG_ERR("cannot mark array");
                return 0;
            }
            ret = _xpost_garbage_mark_ent(mem, rcpy);
            if (!ret)
            {
                XPOST_LOG_ERR("cannot mark array");
                return 0;
            }
            if (rtype == dicttype)
            {
                ret = xpost_memory_table_get_addr(mem, rsrc, &ad);
                if (!ret)
                {
                    XPOST_LOG_ERR("cannot retrieve address for ent %u",
                                  rsrc);
                    return 0;
                }
                if (!_xpost_garbage_mark_dict(ctx, mem, ad, markall))
                    return 0;
                ret = xpost_memory_table_get_addr(mem, rcpy, &ad);
                if (!ret)
                {
                    XPOST_LOG_ERR("cannot retrieve address for ent %u",
                                  rcpy);
                    return 0;
                }
                if (!_xpost_garbage_mark_dict(ctx, mem, ad, markall))
                    return 0;
            }
            if (rtype == arraytype)
            {
                /* Descend the whole entity, not the length of the object
                   the save record was made for. A record made for a
                   getinterval view of a shared array holds the view's
                   length in saverec_.pad, fewer elements than the entity
                   holds; marking only that many leaves the tail of a
                   shared array untraced, so a child reached only through
                   the tail is swept while the array is still live. The
                   entity's own used-extent is what the root walk marks. */
                ret = xpost_memory_table_get_addr(mem, rsrc, &ad);
                if (!ret)
                {
                    XPOST_LOG_ERR("cannot retrieve address for array ent %u",
                                  rsrc);
                    return 0;
                }
                if (!_xpost_garbage_mark_array(ctx, mem, ad,
                        mem->table.tab[rsrc].used / sizeof(Xpost_Object),
                        markall))
                    return 0;
                ret = xpost_memory_table_get_addr(mem, rcpy, &ad);
                if (!ret)
                {
                    XPOST_LOG_ERR("cannot retrieve address for array ent %u",
                                  rcpy);
                    return 0;
                }
                if (!_xpost_garbage_mark_array(ctx, mem, ad,
                        mem->table.tab[rcpy].used / sizeof(Xpost_Object),
                        markall))
                    return 0;
            }
        }
    }
    }

    return 1;
}

/* mark all allocations referred to by objects in save stack */
static
int _xpost_garbage_mark_save(Xpost_Context *ctx,
                             Xpost_Memory_File *mem,
                             unsigned int stackent,
                             int markall)
{
    Xpost_Stack *s;
    unsigned int i;
    if (!mem) return 0;

#ifdef DEBUG_GC
    printf("marking save stack of size %d\n", xpost_stack_count(mem, stackent));
#endif

    if (!_xpost_garbage_mark_stack_segments(mem, stackent))
        return 0;

    for (s = xpost_stack_at(mem, stackent); s;
         s = xpost_stack_next_segment(mem, s))
    {
        for (i = 0; i < s->top; i++)
        {
            if (!_xpost_garbage_mark_save_stack(ctx, mem, s->data[i].save_.stk,
                                               markall))
                return 0;
        }
    }
    return 1;
}

/* discard the free list.
   iterate through tables,
        if element is unmarked and not zero-sized,
            free it.
   return reclaimed size
 */
static
unsigned int _xpost_garbage_sweep(Xpost_Memory_File *mem)
{
    unsigned int zero = 0;
    unsigned int z;
    unsigned int i;
    unsigned int sz = 0;
    int quarantine = 0;

#ifdef WANT_DEBUG_HOOKS
    /* Quarantine: sweep nothing, so a freed entity is never handed out
       again. What that separates is a holder of a recycled entity, whose
       symptom goes away when nothing is recycled, from corruption in
       place, whose symptom does not.

       It is compiled in only for a build configured to want it
       (-Ddebug-hooks=true, --enable-debug-hooks), because the switch is
       read from the environment and the environment belongs to the host,
       not to the library. Reclamation is the whole of what a collector
       promises its host; a shipped library that could be told from a
       variable not to reclaim would let whoever can set one in the
       process turn a long-running embedding into unbounded growth, with
       nothing in the run saying so. A variable may configure a run --
       where the data is, how loud the log is, how often to collect --
       and it may ask for a diagnostic that only reports, as the ones
       below do. Taking a guarantee away is a build's decision. */
    if (getenv("XPOST_GC_NO_REUSE"))
        return 0;
    /* Withhold: reclaim as usual but never offer the entity again. An
       entity the free list hands back is opened again by the allocation
       that takes it, so a reference still held to it reads storage that
       is by then legitimately live and says nothing. Withholding leaves
       the stale read on storage nothing has taken, which a described
       arena reports against whoever read it. This is what makes such a
       read observable rather than merely wrong, and it is a build's
       decision for the same reason the switch above is. */
    quarantine = getenv("XPOST_GC_WITHHOLD") != NULL;
#endif

    z = xpost_memory_free_lists_adr(mem); /* address of the free list heads */

    /* discard the lists; previously-freed entities are gathered again */
    for (i = 0; i < XPOST_FREE_NBUCKETS; i++)
        memcpy(xpost_vm_ptr(mem, z + i * sizeof(unsigned int)), &zero, sizeof zero);

    {
    unsigned int bstat[XPOST_FREE_NBUCKETS] = {0};
    for (i = mem->start; i < mem->table.nextent; i++)
    {
        if (((mem->table.tab[i].mark & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK) == 0) &&
            (mem->table.tab[i].sz != 0))
        {
            unsigned int bz;
            unsigned int b;
            unsigned int head;
            /* a file is an entity in here and a struct outside, and the
               entity's storage is what names the struct: give the
               struct up while that storage still means something, since
               from the moment the entity joins the free list its bytes
               are treated as holding nothing. */
            if (mem->table.tab[i].tag == filetype)
                xpost_file_release_entity(mem, i);
            /* a handle is likewise an entity in here and a block
               outside, and this is the last reach anything has to
               either */
            if (mem->table.tab[i].tag & XPOST_MEMORY_TABLE_TAG_HANDLE)
                xpost_handle_release_entity(mem, i);
            mem->table.tab[i].tag = 0;
            if (quarantine)
            {
                XPOST_VG_POISON_ENT(mem->base, mem->table.tab[i].adr,
                                    mem->table.tab[i].sz);
                sz += mem->table.tab[i].sz;
                continue;
            }
            b = xpost_free_bucket_for_size(mem->table.tab[i].sz);
            bstat[b]++;
            bz = z + b * sizeof(unsigned int);
            /* the link goes in the table beside the entity, not in the
               storage being reclaimed: the sweep and the allocator are
               the two writers of these lists and they have to agree */
            memcpy(&head, xpost_vm_ptr(mem, bz), sizeof head);
            mem->table.tab[i].nextfree = head;
            memcpy(xpost_vm_ptr(mem, bz), &i, sizeof(unsigned int));
            /* the entity has been reclaimed: a reference something still
               holds now reads storage the interpreter has taken back,
               which is the defect the arena is described for */
            XPOST_VG_POISON_ENT(mem->base, mem->table.tab[i].adr,
                                mem->table.tab[i].sz);
            sz += mem->table.tab[i].sz;
        }
    }
    if (getenv("XPOST_GC_BUCKETSTAT"))
    {
        fprintf(stderr, "BUCKETS:");
        for (i = 0; i < XPOST_FREE_NBUCKETS; i++)
            fprintf(stderr, " %u", bstat[i]);
        fprintf(stderr, "\n");
    }
    }

    return sz;
}

/* the independent reachability verifier and entity census live in
   xpost_garbage_diag.c; the collector calls them behind their
   XPOST_GC_* environment gates */

/* PLRM 3.7.2: an object in global VM must not reference an object in local
   VM -- with one interpreter-maintained exception (PLRM 3.7.7 and the note
   under systemdict): "systemdict, a global dictionary, contains several
   entries whose values are local dictionaries, such as userdict and $error."
   errordict, statusdict, serverdict and FontDirectory are local for the same
   reason. A local collection marks only local roots and does not descend the
   global systemdict, so these sanctioned local dictionaries -- reachable only
   through global systemdict -- would be swept and their storage recycled,
   corrupting the error machinery and other services on the next job. Mark the
   local values systemdict holds so the exception references keep them alive.
   No other global container may hold a local reference (invalidaccess bars
   it), so systemdict is the only one that needs this treatment. */
static int _xpost_garbage_mark_systemdict_exceptions(Xpost_Context *ctx,
                                                     Xpost_Memory_File *mem,
                                                     int markall)
{
    Xpost_Object sd;
    Xpost_Memory_File *sdmem;
    unsigned int adr, ent;
    dichead *dp;
    dicrec *tp;
    unsigned int n;
    unsigned int j;

    /* systemdict is the permanent bottom entry of the dictionary stack */
    sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    if (xpost_object_get_type(sd) != dicttype)
        return 1;
    sdmem = xpost_context_select_memory(ctx, sd);
    if (!sdmem || sdmem != ctx->gl)
        return 1; /* systemdict already covered if it is not global */
    ent = xpost_object_get_ent(sd);
    if (!xpost_ent_valid(sdmem, ent))
        return 1;
    adr = sdmem->table.tab[ent].adr;
    dp = xpost_dict_head_at(sdmem, adr);
    tp = xpost_dict_table_of(dp);
    /* the table is more than twice as long as the size it is derived
       from, so it is counted in a type wide enough for that product
       rather than in the type the size itself is stored in */
    n = DICTABN(dp->sz);
    for (j = 0; j < n; j++)
    {
        Xpost_Object pair[2];
        unsigned int i;

        if (xpost_object_get_type(tp[j].key) == nulltype)
            continue;
        pair[0] = tp[j].key;
        pair[1] = tp[j].value;
        for (i = 0; i < 2; i++)
        {
            Xpost_Object v = pair[i];

            /* every reference systemdict holds into the bank being
               swept, whichever slot carries it and whatever entity kind
               it names: the anchor is derived from the dictionary, not
               from a list of names, so a reference admitted through the
               sanctioned window is anchored without being enrolled */
            if (!xpost_object_is_composite(v)
                && xpost_object_get_type(v) != filetype)
                continue;
            /* only the values in the swept bank are the sanctioned
               exceptions; a global value is reached through the global
               roots, not swept here */
            if (xpost_context_select_memory(ctx, v) != mem)
                continue;
            if (!_xpost_garbage_mark_object(ctx, mem, v, markall))
                return 0;
        }
    }
    return 1;
}

/*
   determine GLOBAL/LOCAL
   clear all marks,
   mark all root stacks,
   sweep.
   return reclaimed size or -1 if error occured.
 */
/* Which banks a collection that runs of its own accord reclaims.

   Both, unless a program has said otherwise: marking has to cross the
   two whatever is reclaimed, because an object in one may be named from
   the other, so reclaiming the second costs the walk of its table and
   nothing more. There is therefore no rate to tune and no bank to
   prefer -- a collection reclaims what it has just finished marking.

   PLRM 8.2's vmreclaim can turn automatic collection off for one bank or
   for both, and that is what this reads. */
int xpost_garbage_auto_banks(Xpost_Context *ctx)
{
    int banks = XPOST_GARBAGE_SWEEP_NONE;

    if (!ctx)
        return XPOST_GARBAGE_SWEEP_BOTH;
    if (ctx->lo && ctx->lo->garbage_collect_auto)
        banks |= XPOST_GARBAGE_SWEEP_LOCAL;
    if (ctx->gl && ctx->gl->garbage_collect_auto)
        banks |= XPOST_GARBAGE_SWEEP_GLOBAL;
    return banks;
}

/* The same setting written back. vmreclaim reaches it with the code the
   program gave, and restore with the code the save level was taken
   under, the setting being the whole of the VMReclaim user parameter
   (PLRM C.3.5) and so subject to save and restore with the rest of them
   (PLRM 8.2 restore). */
void xpost_garbage_auto_banks_set(Xpost_Context *ctx, int banks)
{
    if (!ctx)
        return;
    if (ctx->lo)
        ctx->lo->garbage_collect_auto = (banks & XPOST_GARBAGE_SWEEP_LOCAL) != 0;
    if (ctx->gl)
        ctx->gl->garbage_collect_auto = (banks & XPOST_GARBAGE_SWEEP_GLOBAL) != 0;
}

int xpost_garbage_collect(Xpost_Memory_File *mem, int dosweep, int markall)
{
    unsigned int i;
    unsigned int *cid;
    Xpost_Context *ctx = NULL;
    Xpost_Memory_File *other;
    int isglobal;
    unsigned int sz = 0;
    unsigned int ad;

    if (mem->interpreter_get_initializing()) /* do not collect while initializing */
        return 0;

    /* printf("\ncollect:\n"); */

    /* determine global/local */
    isglobal = 0;
    ad = xpost_memory_context_list_adr(mem);
    cid = xpost_vm_ptr(mem, ad);
    for (i = 0; i < MAXCONTEXT && cid[i]; i++)
    {
        ctx = mem->interpreter_cid_get_context(cid[i]);
        if (ctx->state != 0)
        {
            if (mem == ctx->gl)
            {
                isglobal = 1;
                break;
            }
        }
    }
    if (ctx == NULL)
    {
        XPOST_LOG_ERR("cannot find context");
        return -1;
    }
#ifdef DEBUG_GC
    printf("using cid=%u\n", ctx->id);
#endif

    /* the walk begins with nothing queued and nothing waiting: a
       collection abandoned partway through leaves both behind, and this
       collection is not the one they were left for */
    _xpost_garbage_work_n = 0;
    _xpost_garbage_work_waiting = 0;
    _xpost_garbage_work_ready = 0;
    _xpost_garbage_collect_bank = mem;

    /* Marking covers the banks the sweep will read, no more. Asked for
       both banks, the walk crosses between them: an object in one may
       be named from the other. Asked for one, it stays in that bank and
       stops at any reference that leaves it, because what lies beyond
       is not swept and local may name global freely -- crossing would
       walk the whole global graph to protect storage the sweep never
       touches. The references the other way are enumerable: a global
       container may hold no reference into local memory (PLRM 3.7.2)
       except the local dictionaries systemdict names, and those are
       marked as anchors below whichever way the walk is asked for.
       Which bank is then swept is a separate decision, and a bank may
       only be swept by a collection that marked it. */
    if (isglobal)
    {
        other = ctx->lo;
    }
    else
    {
        other = ctx->gl;
    }

    {
        /* The name-lookup cache holds objects outside the root set, so
           an entry whose value this collection may recycle must not be
           served from it afterwards. A collection of both banks may
           recycle anything, and drops the whole cache by advancing the
           generation. A collection of one bank cannot touch a value in
           the other -- for a local collection that is the operators and
           procedures of the frozen library, which are most of what a
           run resolves -- so only the entries whose values live in the
           collected bank are taken, each stamped with a generation that
           has already passed. */
        for (i = 0; i < MAXCONTEXT && cid[i]; i++)
        {
            Xpost_Context *cctx = mem->interpreter_cid_get_context(cid[i]);
            unsigned int k;

            if (!cctx)
                continue;
            if (markall)
            {
                ++cctx->namebind_gen;
                continue;
            }
            for (k = 0; k < cctx->namecache_size; k++)
            {
                Xpost_Object v;
                Xpost_Object_Type t;

                if (cctx->namecache_gen[k] != cctx->namebind_gen)
                    continue;
                v = cctx->namecache_val[k];
                t = xpost_object_get_type(v);
                if (t != arraytype && t != dicttype && t != stringtype
                 && t != filetype)
                    continue;
                if (xpost_context_select_memory(cctx, v) != mem)
                    continue;
                cctx->namecache_gen[k] = cctx->namebind_gen - 1;
            }
        }
        _xpost_garbage_unmark(mem);
        if (markall)
            _xpost_garbage_unmark(ctx->gl);

        ad = xpost_memory_save_stack_ent(mem);
        if (!_xpost_garbage_mark_save(ctx, mem, ad, markall))
            return -1;
        if (!_xpost_garbage_mark_substack_pool(mem))
            return -1;
        ad = xpost_memory_name_stack_ent(mem);
#ifdef DEBUG_GC
        printf("marking name stack\n");
#endif
        if (!_xpost_garbage_mark_names(ctx, mem, ad, markall))
            return -1;

        /* and the other bank's, when the walk is asked for both. A name
           is interned into whichever bank was being allocated from when
           it was first seen, so both stacks carry names, and a name's
           characters are an entity of their own that nothing else
           reaches. The save stack likewise holds what a save took a
           copy of, which the object it was copied from no longer
           names. */
        if (markall && other)
        {
            ad = xpost_memory_save_stack_ent(other);
            if (!_xpost_garbage_mark_save(ctx, other, ad, markall))
                return -1;
            if (!_xpost_garbage_mark_substack_pool(other))
                return -1;
            ad = xpost_memory_name_stack_ent(other);
            if (!_xpost_garbage_mark_names(ctx, other, ad, markall))
                return -1;
        }

        for (i = 0; i < MAXCONTEXT && cid[i]; i++)
        {
            ctx = mem->interpreter_cid_get_context(cid[i]);

            /* A slot that holds no context holds the stack addresses and
               roots of the one that ended there, and nothing is entitled
               to keep what an ended context was holding: marking from
               those would keep a whole context's worth of storage alive
               for the rest of the run. Where the bank has since been
               wound back they name storage that is not there at all. */
            if (ctx->state == 0)
                continue;

#ifdef DEBUG_GC
            printf("marking os\n");
#endif
            if (!_xpost_garbage_mark_stack(ctx, mem, ctx->os, markall))
                return -1;

#ifdef DEBUG_GC
            printf("marking ds\n");
#endif
            if (!_xpost_garbage_mark_stack(ctx, mem, ctx->ds, markall))
                return -1;

#ifdef DEBUG_GC
            printf("marking es\n");
#endif
            if (!_xpost_garbage_mark_stack(ctx, mem, ctx->es, markall))
                return -1;

#ifdef DEBUG_GC
            printf("marking hold\n");
#endif
            if (!_xpost_garbage_mark_stack(ctx, mem, ctx->hold, markall))
                return -1;
#ifdef DEBUG_GC
            printf("marking window device\n");
#endif
            /* Everything the context holds on its own account, walked
               from the same list that declares it. An object the context
               roots and the collector does not mark is taken by the
               first collection that reaches it, so the two are kept in
               one place rather than in two that must agree. */
#define XPOST_MARK_CONTEXT_ROOT(f) \
            if (!_xpost_garbage_mark_object(ctx, mem, ctx->f, markall)) \
                return -1;
            XPOST_CONTEXT_OBJECT_ROOTS(XPOST_MARK_CONTEXT_ROOT)
#undef XPOST_MARK_CONTEXT_ROOT

            /* the procedures the operator table holds. An operator
               that runs a procedure keeps it there, and the table is
               not an object, so nothing the walk reaches names it.
               The procedures live in either bank -- the accessors of
               per-context local state are wrapped over local arrays --
               so the table is walked whichever bank is collected, the
               walk stopping at the procedures of a bank not in play. */
            if (ctx->gl && ctx->state != 0)
            {
                unsigned int nops = xpost_operator_count();
                unsigned int oi;

                for (oi = 0; oi < nops; oi++)
                {
                    Xpost_Operator *optab_ = xpost_operator_table(ctx->gl);
                    Xpost_Object proc_ = optab_[oi].proc;

                    if (xpost_object_get_type(proc_) != arraytype)
                        continue;
                    if (!_xpost_garbage_mark_object(ctx, mem, proc_, markall))
                        return -1;
                }
            }

            /* the sanctioned global->local references: the local
               dictionaries systemdict holds (userdict, $error, errordict,
               statusdict, serverdict, FontDirectory) -- see PLRM 3.7.2 */
            if (!_xpost_garbage_mark_systemdict_exceptions(ctx, mem, markall))
                return -1;
        }
    }

    if (dosweep) {
#ifdef DEBUG_GC
        printf("sweep\n");
#endif
        if (getenv("XPOST_GC_VERIFY") && ctx)
            _xpost_garbage_diag_verify(ctx, mem, markall);
        if (!isglobal && getenv("XPOST_GC_XBANK_CHECK") && ctx && ctx->gl)
            _xpost_garbage_diag_xbank(ctx, mem);

        /* A bank is reclaimed only by a collection that marked it: a
           sweep of storage this walk did not cover would take objects
           that are still named. Marking crosses banks when it is asked
           for both, which is what makes reclaiming both possible at all.

           The two are separate choices because PLRM 8.2's vmreclaim
           separates them -- it disables automatic collection in one bank
           or in both, and performs an immediate collection in one or in
           both -- so the caller says which banks it means. */
        {
            Xpost_Memory_File *localmem  = isglobal ? other : mem;
            Xpost_Memory_File *globalmem = isglobal ? mem : other;

            if ((dosweep & XPOST_GARBAGE_SWEEP_LOCAL) && localmem)
                sz += _xpost_garbage_sweep(localmem);
            if ((dosweep & XPOST_GARBAGE_SWEEP_GLOBAL) && globalmem && markall)
                sz += _xpost_garbage_sweep(globalmem);
        }
    }

    XPOST_LOG_INFO("collect recovered %u bytes", sz);
    return sz;
}


