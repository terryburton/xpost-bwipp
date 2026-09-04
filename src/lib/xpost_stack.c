/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_stack.c
 * @brief The stacks: operand, execution and dictionary.
 *
 * A stack is a chain of fixed segments in the arena rather than one block,
 * so it grows without moving what is already on it.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h> /* memcpy */

#include "xpost_log.h"
#include "xpost_object.h"
#include "xpost_memory.h"
#include "xpost_error.h"
#include "xpost_stack.h"

/*
 * The stack type is a chain of segments.
 *
 * root->prevseg == top segment
 * tail->nextseg == 0
 *

typedef struct
{
    unsigned int nextseg;
    unsigned int prevseg;
    unsigned int top;
    Xpost_Object data[XPOST_STACK_SEGMENT_SIZE];
} Xpost_Stack;
*/

/* Make one stack segment, as an entity of its own.
 *
 * A segment is an entity and every holder names it by number: the four
 * stacks a context runs on, the name stack, the record stack a save
 * owns, and each segment's two links to its neighbours. Zero means no
 * segment.
 *
 * The number is what makes a segment movable. Segments are the largest
 * blocks the arena carries after the page itself and they arrive as a
 * program deepens, so they lie wherever the arena had room; a pass that
 * closes the arena up walks the table and slides them, and the only
 * place their storage is written down is the row it rewrites. A holder
 * that named the address instead would be left pointing at whatever
 * moved into it.
 *
 * The collector reaches segments as it walks a stack and marks each one,
 * for the reason given there: an entity nothing marks is one the sweep
 * takes back. */
XPOST_TEST_VISIBLE int xpost_stack_init(Xpost_Memory_File *mem,
                                unsigned int *pent)
{
    unsigned int ent;

    if (!xpost_memory_table_alloc(mem, sizeof(Xpost_Stack), 0, &ent))
    {
        XPOST_LOG_ERR("cannot allocate a stack segment");
        return 0;
    }
    if (!xpost_stack_init_in(mem, ent))
        return 0;
    *pent = ent;
    return 1;
}

/* The same, for a segment whose entity is already made: the name stack
   and the master save stack are special entities, numbered before any
   constructor runs, and each is its own stack's first segment rather
   than a row holding the number of one. */
XPOST_TEST_VISIBLE int xpost_stack_init_in(Xpost_Memory_File *mem,
                                           unsigned int ent)
{
    Xpost_Stack *s;
    unsigned int sz;

    if (!xpost_memory_table_get_size(mem, ent, &sz) || sz < sizeof(Xpost_Stack))
    {
        XPOST_LOG_ERR("%d entity %u is too small to hold a stack segment",
                      VMerror, ent);
        return 0;
    }
    s = xpost_stack_at(mem, ent);
    s->nextseg = 0;
    s->prevseg = ent;
    s->top = 0;
    return 1;
}

void xpost_stack_clear(Xpost_Memory_File *mem,
                       unsigned int stackent)
{
    Xpost_Stack *s = xpost_stack_at(mem, stackent);
    s->top = 0;
    s->prevseg = stackent;
}

void xpost_stack_dump(Xpost_Memory_File *mem,
                      unsigned int stackent)
{
    Xpost_Stack *s = xpost_stack_at(mem, stackent);
    unsigned int i;
    unsigned int a;

    a = 0;
    while (1)
    {
        for (i = 0; i < s->top; i++)
        {
            XPOST_LOG_DUMP("%d:", a++);
            xpost_object_dump(s->data[i]);
        }
        if (i != XPOST_STACK_SEGMENT_SIZE)
            break;
        if (s->nextseg == 0)
            break;
        s = xpost_stack_step(mem, s->nextseg);
    }
}

/* deallocate stack segment and any chained segments */
int xpost_stack_count(Xpost_Memory_File *mem,
                      unsigned int stackent)
{
    Xpost_Stack *s = xpost_stack_at(mem, stackent);
    unsigned int ct = 0;
    while (s->top == XPOST_STACK_SEGMENT_SIZE)
    {
        ct += XPOST_STACK_SEGMENT_SIZE;
        if (s->nextseg == 0)
            return ct; /* a full segment with no successor is a legal topmost state */
        s = xpost_stack_step(mem, s->nextseg);
    }
    return ct + s->top;
}

XPOST_TEST_VISIBLE int xpost_stack_push(Xpost_Memory_File *mem,
                                unsigned int stackent,
                                Xpost_Object obj)
{
    Xpost_Stack *root = xpost_stack_at(mem, stackent);
    Xpost_Stack *s = xpost_stack_at(mem, root->prevseg); /* load top segment */

    if (xpost_object_get_type(obj) == invalidtype)
        return 0;

    /* the segment is left in place when a push fills it, so the top
       segment never rests empty with values below it: direct segment
       accesses can rely on root->prevseg holding the topmost value.
       move to (or link) the next segment when pushing into a full one. */
    if (s->top == XPOST_STACK_SEGMENT_SIZE)
    {
        if (s->nextseg == 0)
        {
            unsigned int stent;
            unsigned int newst;
            int ret;

            /* the segment's own number, which is what the root's
               backward link holds; a pointer no longer says which
               segment it is */
            stent = root->prevseg;
            ret = xpost_stack_init(mem, &newst);
            if (!ret)
            {
                /* the object is on no stack and the caller is several
                   hundred sites that do not carry the answer back; the
                   memory file holds the refusal until the dispatch or
                   the interpreter's safe point reads it. A declined
                   segment is the virtual memory machinery refusing, which
                   PLRM 8.2 gives VMerror for -- unlike a push of an
                   object that was never made, below, which says nothing
                   about memory and is left to say nothing here. */
                mem->push_refused = 1;
                return 0;
            }
            s = xpost_stack_at(mem, stent);
            root = xpost_stack_at(mem, stackent);
            s->nextseg = newst;
            (xpost_stack_at(mem, newst))->prevseg = stent;
        }
        root->prevseg = s->nextseg;
        s = xpost_stack_step(mem, s->nextseg);
        s->top = 0;
    }

    s->data[s->top++] = obj; /* push value */

    return 1;
}

XPOST_TEST_VISIBLE int xpost_stack_push_run(Xpost_Memory_File *mem,
                                            unsigned int stackent,
                                            const Xpost_Object *objs,
                                            int n)
{
    Xpost_Stack *root;
    Xpost_Stack *s;
    int done = 0;
    int k;

    /* each object is held to the same rule the single push applies: an
       object that was never made goes on no stack. Checked up front so
       the run lodges all of its objects or none of them. */
    for (k = 0; k < n; k++)
        if (xpost_object_get_type(objs[k]) == invalidtype)
            return 0;

    /* The stack's root and top segment are resolved once and the run is
       copied through the held pointers. Holding them across the copy is
       sound because nothing here can move the storage they point into:
       collection and compaction run only at the interpreter's mainloop
       safe point, never inside a push, so within one operator's work the
       segments stay where they are. The one act in this function that
       can move the memory file is linking a fresh segment when the top
       one fills, and both pointers are re-derived from their entity
       numbers after it. For the same reason @p objs must be host
       storage, a C array, never a pointer into the file being pushed
       into. Nothing is held past the return. */
    root = xpost_stack_at(mem, stackent);
    s = xpost_stack_at(mem, root->prevseg);

    while (done < n)
    {
        int room = XPOST_STACK_SEGMENT_SIZE - (int)s->top;
        int take = n - done;

        if (take > room)
            take = room;
        if (take > 0)
        {
            memcpy(s->data + s->top, objs + done,
                   (size_t)take * sizeof(Xpost_Object));
            s->top += (unsigned int)take;
            done += take;
        }
        if (done == n)
            break;

        /* the top segment is full: one object goes through the single
           push, which advances to -- or links and allocates -- the next
           segment and leaves the object as its first element */
        if (!xpost_stack_push(mem, stackent, objs[done]))
        {
            /* refused (the single push has recorded why): take back
               what this run lodged, so the caller sees all or nothing */
            while (done--)
                (void)xpost_stack_pop(mem, stackent);
            return 0;
        }
        done++;
        root = xpost_stack_at(mem, stackent);
        s = xpost_stack_at(mem, root->prevseg);
    }

    return 1;
}

Xpost_Object xpost_stack_topdown_fetch(Xpost_Memory_File *mem,
                                       unsigned int stackent,
                                       int idx)
{
    int i = idx;
    Xpost_Stack *s = xpost_stack_at(mem, stackent);

    if (s->prevseg) s = xpost_stack_at(mem, s->prevseg); /* find top seg */

    while (i >= (signed)(s->top)){
        i -= s->top;
        if (s == xpost_stack_at(mem, stackent)){
            XPOST_LOG_INFO("%d can't find stack segment for index -%d in stack of size %u",
                    unregistered, idx,
                    xpost_stack_count(mem, stackent));
            return invalid;
        }
        s = xpost_stack_step(mem, s->prevseg);
    }
    return s->data[s->top - 1 - i];
}

int xpost_stack_topdown_replace(Xpost_Memory_File *mem,
                                unsigned int stackent,
                                int idx,
                                Xpost_Object obj)
{
    int i = idx;
    Xpost_Stack *s = xpost_stack_at(mem, stackent);
    if (s->prevseg) s = xpost_stack_at(mem, s->prevseg); /* find top seg */

    while (i >= (signed)(s->top)){
        i -= s->top;
        if (s == xpost_stack_at(mem, stackent)){
            XPOST_LOG_INFO("%d can't find stack segment for index -%d in stack of size %u",
                    unregistered, idx,
                    xpost_stack_count(mem, stackent));
            return 0;
        }
        s = xpost_stack_step(mem, s->prevseg);
    }
    s->data[s->top - 1 - i] = obj;
    return 1;
}

int xpost_stack_topdown_find_type(Xpost_Memory_File *mem,
                                  unsigned int stackent,
                                  int type,
                                  Xpost_Object *out)
{
    Xpost_Stack *root = xpost_stack_at(mem, stackent);
    Xpost_Stack *seg = xpost_stack_at(mem, root->prevseg); /* top segment */
    int idx = 0;

    /* Walk the segment chain once from the top rather than calling
       topdown_fetch per index -- each of those re-walks the chain, so a scan
       of the whole stack was O(n^2). Here each element is visited once. */
    for (;;)
    {
        int k;
        for (k = (int)seg->top; k-- > 0; )
        {
            if ((int)xpost_object_get_type(seg->data[k]) == type)
            {
                if (out)
                    *out = seg->data[k];
                return idx;
            }
            idx++;
        }
        if (seg == root)
            break;
        seg = xpost_stack_step(mem, seg->prevseg);
    }
    return -1;
}

int xpost_stack_peek_top(Xpost_Memory_File *mem,
                         unsigned int stackent,
                         int n,
                         Xpost_Object *out)
{
    Xpost_Stack *root = xpost_stack_at(mem, stackent);
    Xpost_Stack *seg = xpost_stack_at(mem, root->prevseg); /* top segment */
    int got = 0;

    /* One top-down pass: out[0] is the topmost element. Fetching each of the
       top n with xpost_stack_topdown_fetch would re-walk the segment chain per
       index and be O(n^2) on a multi-segment stack. */
    while (got < n)
    {
        int t = (int)seg->top;
        int take = (n - got < t) ? (n - got) : t;
        int m;
        for (m = 0; m < take; m++)
            out[got + m] = seg->data[t - 1 - m];
        got += take;
        if (got < n)
            seg = xpost_stack_step(mem, seg->prevseg);
    }
    return got;
}

Xpost_Object xpost_stack_bottomup_fetch(Xpost_Memory_File *mem,
                                        unsigned int stackent,
                                        int idx)
{
    Xpost_Stack *root = xpost_stack_at(mem, stackent);
    Xpost_Stack *s = root;
    int i = idx;

    /* find desired segment */
    while (i >= XPOST_STACK_SEGMENT_SIZE)
    {
        i -= XPOST_STACK_SEGMENT_SIZE;
        if (s->nextseg == 0)
        {
            XPOST_LOG_INFO("%d can't find stack segment for index %d in stack of size %u",
                    unregistered, idx,
                    xpost_stack_count(mem, stackent));
            return invalid;
        }
        s = xpost_stack_step(mem, s->nextseg);
    }
    if (i >= (signed)s->top){
        return invalid;
    }
    return s->data[i];
}

int xpost_stack_bottomup_replace(Xpost_Memory_File *mem,
                                 unsigned int stackent,
                                 int idx,
                                 Xpost_Object obj)
{
    Xpost_Stack *root = xpost_stack_at(mem, stackent);
    Xpost_Stack *s = root;
    int i = idx;

    /* find desired segment */
    while (i >= XPOST_STACK_SEGMENT_SIZE)
    {
        i -= XPOST_STACK_SEGMENT_SIZE;
        if (s->nextseg == 0)
        {
            XPOST_LOG_INFO("%d can't find stack segment for index %d in stack of size %u",
                          unregistered, idx,
                          xpost_stack_count(mem, stackent));
            return 0;
        }
        s = xpost_stack_step(mem, s->nextseg);
    }
    if (i >= (signed)s->top){
        return 0;
    }
    s->data[i] = obj;
    return 1;
}

XPOST_TEST_VISIBLE Xpost_Object xpost_stack_pop(Xpost_Memory_File *mem,
                                        unsigned int stackent)
{
    Xpost_Stack *root = xpost_stack_at(mem, stackent);
    Xpost_Stack *s = xpost_stack_at(mem, root->prevseg); /* load top seg */
    Xpost_Object val;

    if (s->top == 0) /* back up if top is empty */
    {
        if (s != root)
        {
            unsigned int soff = s->prevseg;
            s = xpost_stack_step(mem, soff);
            root->prevseg = soff; // update root->top
            if (s->top == 0)
                return invalid;
        }
        else /* can't back up if stack is empty */
        {
            return invalid;
        }
    }

    val = s->data[--s->top]; /* pop value */

    /* retreat eagerly when the segment empties, maintaining the
       invariant that the top segment holds the topmost value */
    if (s->top == 0 && s != root)
        root->prevseg = s->prevseg;

    return val;
}
