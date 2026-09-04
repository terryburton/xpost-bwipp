/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_STACK_H
#define XPOST_STACK_H

#ifndef XPOST_OBJECT_H
# error MUST #include "xpost_object.h" before this file
#endif

#ifndef XPOST_MEMORY_H
# error MUST #include "xpost_memory.h" before this file
#endif

#include <limits.h> /* INT_MAX */

/**
 * @file xpost_stack.h
 * @brief stack functions
 *
 * Return convention: the functions of this module answer 1 for success
 * and 0 for failure (the object-mutator modules answer the opposite --
 * 0 for no-error, nonzero for the PostScript error to raise; each
 * header states which convention it uses).
 *
 * Stacks are built from a chain of tables, a hybrid
 * array/linked-list data structure.
 * @{
 */

/**
 * @brief Number of objects in one segment of the stack.
 *
 * This parameter may be tuned for performance.
 * Most stack operations are performed on the top few entries,
 * which are at the tail of a linked list of stack segments.
 *
 * For performance, this parameter should be set to be slightly
 * larger than expected maximum size (or the average, if the
 * maximum is very large; however, there is currently no provision
 * for shrinking the size of a stack and returning memory).
 *
 * For testing, this parameter should be set very small,
 * but it must be large enough to hold all parameters in a
 * type-checked postscript operator. cf. xpost_operator.c:holdn()
 */
#define XPOST_STACK_SEGMENT_SIZE 1000

typedef struct
{
    unsigned int nextseg;
    unsigned int prevseg;
    unsigned int top;
    Xpost_Object data[XPOST_STACK_SEGMENT_SIZE];
} Xpost_Stack;

/**
 * @brief the stack segment entity @p stackent names in @p mem.
 *
 * The one spelling of a stack pointer. Stacks are reached far more often
 * than anything else in virtual memory is, so the cast was written out at
 * eighty-four sites; it is written here instead. As with every pointer
 * into a memory file, an allocation in @p mem may move the file and
 * invalidate the result, so it is derived where it is used rather than
 * held across a call that can allocate.
 *
 * A segment is an entity, so what every holder of a stack stores is the
 * segment's number and the table says where its bytes are. A number
 * survives the storage moving; an address would not.
 */
static inline Xpost_Stack *
xpost_stack_at(Xpost_Memory_File *mem, unsigned int stackent)
{
    return (Xpost_Stack *)xpost_ent_ptr(mem, stackent);
}

/**
 * @brief the segment @p stackent names, as one step of a chain walk.
 *
 * The one spelling of a move to a segment's immediate neighbour, which
 * is what a walk of the chain is made of. Reaching a position in a stack
 * costs the steps taken to get there, and how many were taken is the
 * only thing separating a scan of a whole stack that walks the chain
 * once from one that walks it again for every element -- the two answer
 * the same thing. So every step is counted here, into
 * Xpost_Memory_File::stack_walk, which xpost_op_param.c reports.
 *
 * The root's handle on the top segment is not a step: it names the top
 * outright, however many segments lie between them.
 */
static inline Xpost_Stack *
xpost_stack_step(Xpost_Memory_File *mem, unsigned int stackent)
{
    if (mem->stack_walk < (unsigned int)INT_MAX)
        ++mem->stack_walk;
    return xpost_stack_at(mem, stackent);
}

/**
 * @brief the next segment of a full walk, or NULL when the walk is done.
 *
 * The termination rule for walking a segmented stack, in one place: a
 * segment shorter than full is the stack's top and ends the walk; an
 * exactly-full segment continues into its successor if it has one. Walk
 * a whole stack as
 *     for (s = xpost_stack_at(mem, stackent); s;
 *          s = xpost_stack_next_segment(mem, s))
 *         for (i = 0; i < s->top; i++) ... s->data[i] ...
 * The usual caveat applies: an allocation in @p mem invalidates @p s.
 */
static inline Xpost_Stack *
xpost_stack_next_segment(Xpost_Memory_File *mem, Xpost_Stack *s)
{
    if (s->top == XPOST_STACK_SEGMENT_SIZE && s->nextseg)
        return xpost_stack_step(mem, s->nextseg);
    return NULL;
}

/**
 * @brief Make a stack, returning the entity of its first segment.
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_stack_init(Xpost_Memory_File *mem, unsigned int *ent);

/**
 * @brief Make a stack in a segment-sized entity that already exists.
 *
 * The name stack and the master save stack are special entities, whose
 * numbers are fixed before any constructor runs, and each is its own
 * stack's first segment rather than a row holding the number of one.
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_stack_init_in(Xpost_Memory_File *mem, unsigned int ent);

/**
 * @brief Empty the stack.
 */
void xpost_stack_clear(Xpost_Memory_File *mem, unsigned int stackent);

/**
 * @brief Dump the contents of a stack to stdout using xpost_object_dump.
 */
void xpost_stack_dump(Xpost_Memory_File *mem, unsigned int stackent);

/**
 * @brief Count elements in stack.
 */
int xpost_stack_count(Xpost_Memory_File *mem, unsigned int stackent);

/**
 * @brief Put an object on top of the stack.
 */
XPOST_TEST_VISIBLE int xpost_stack_push(Xpost_Memory_File *mem,
                                unsigned int stackent,
                                Xpost_Object obj);

/**
 * @brief Put a run of @p n objects on top of the stack, first last on top.
 *
 * One resolution of the stack's top segment covers the whole run, where
 * a loop of xpost_stack_push resolves the root and the top segment
 * again for every object. @p objs must be host storage -- a C array --
 * never a pointer into @p mem, whose base may move when the run links a
 * fresh segment. The run lodges all of its objects or none of them:
 * a refusal takes back whatever part of the run was already placed, so
 * on 0 the stack stands as it did before the call.
 */
XPOST_TEST_VISIBLE int xpost_stack_push_run(Xpost_Memory_File *mem,
                                unsigned int stackent,
                                const Xpost_Object *objs,
                                int n);

/**
 * @brief Index the stack from the top down, fetching object.
 */
Xpost_Object xpost_stack_topdown_fetch(Xpost_Memory_File *mem,
                                       unsigned stackent,
                                       int i);

/**
 * @brief Index the stack from the top down, replacing object.
 */
XPOST_MUST_CHECK int xpost_stack_topdown_replace(Xpost_Memory_File *mem,
                                unsigned stackent,
                                int i,
                                Xpost_Object obj);

/**
 * @brief Top-down index of the topmost element whose object type is @p type,
 * or -1 if none. When @p out is non-NULL and a match is found it receives
 * that element. One top-down segment pass: O(elements scanned), where a loop
 * of xpost_stack_topdown_fetch per index would be O(n^2) on a segmented stack.
 */
int xpost_stack_topdown_find_type(Xpost_Memory_File *mem,
                                  unsigned stackent,
                                  int type,
                                  Xpost_Object *out);

/**
 * @brief Copy the top @p n elements into @p out (out[0] is the topmost) in one
 * top-down segment pass. The caller must ensure the stack holds at least @p n
 * elements. Returns the number copied. This snapshots in O(n); reading the top
 * n with xpost_stack_topdown_fetch per index would be O(n^2) on a segmented
 * stack.
 */
int xpost_stack_peek_top(Xpost_Memory_File *mem,
                         unsigned stackent,
                         int n,
                         Xpost_Object *out);

/**
 * @brief Index the stack from the bottom up, fetching object.
 */
Xpost_Object xpost_stack_bottomup_fetch(Xpost_Memory_File *mem,
                                        unsigned stackent,
                                        int i);

/**
 * @brief Index the stack from the bottom up, replacing object.
 */
XPOST_MUST_CHECK int xpost_stack_bottomup_replace(Xpost_Memory_File *mem,
                                 unsigned stackent,
                                 int i,
                                 Xpost_Object obj);

/**
 * @brief Pop the stack, remove and return top object.
 */
XPOST_TEST_VISIBLE Xpost_Object xpost_stack_pop(Xpost_Memory_File *mem,
                                        unsigned stackent);

/**
 * @}
 */

#endif
