/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_interpreter.c
 * @brief The main loop: what executes an object, and what a run is.
 *
 * The interpreter proper. An object is executed by pushing it and taking the
 * top of the execution stack until there is nothing left, so a procedure is a
 * frame rather than a recursion.
 *
 * This is also where a context is created and brought up -- out of the boot
 * files, or out of an image of virtual memory -- and where the boundary
 * between one job and the next winds the context back.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <signal.h> /* sig_atomic_t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
# include <process.h> /* _getpid, to name a file no other run is writing */
# define _xpost_getpid() _getpid()
#else
# include <unistd.h> /* getpid, likewise */
# define _xpost_getpid() getpid()
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_compat.h" /* xpost_isatty */
#include "xpost_memory.h"
#include "xpost_free.h"  /* itp contexts contain mfiles and mtabs */
#include "xpost_object.h"  /* eval functions examine objects */
#include "xpost_stack.h"  /* eval functions manipulate stacks */
#include "xpost_error.h"
#include "xpost_context.h"
#include "xpost_save.h"  /* save/restore vm */
#include "xpost_string.h"  /* eval functions examine strings */
#include "xpost_font.h"    /* the job boundary flushes the glyph mask cache */
#include "xpost_array.h"  /* eval functions examine arrays */
#include "xpost_name.h"  /* eval functions examine names */
#include "xpost_dict.h"  /* eval functions examine dicts */
#include "xpost_file.h"  /* eval functions examine files */

#include "xpost_op_misc.h"
#include "xpost_interpreter.h" /* uses: context itp MAXCONTEXT MAXMFILE */
#include "xpost_vm_image.h"
#include "xpost_garbage.h"  /*  test gc, install collect() in context's memory files */
#include "xpost_operator.h"  /* eval functions call operators */
#include "xpost_op_dict.h"  /* the shared def fast path */
#include "xpost_op_math.h"  /* the shared range-preserving arithmetic */
#include "xpost_op_control.h"  /* record the run outcome when a job ends */
#include "xpost_op_type.h"  /* the shared type naming */
#include "xpost_op_array.h"  /* the shared array element access */
#include "xpost_op_boolean.h"  /* the shared relations */
#include "xpost_op_stack.h"  /* the shared index and roll rules */
#include "xpost_op_token.h"  /* the shared scan, whose access rule is the caller's */
#include "xpost_op_context.h"  /* the context switcher keys on whether DPS is enabled */
#include "xpost_oplib.h"
#include "xpost_handle.h"  /* the release a device's block was issued to be given up by */
#include "xpost_dev_raster.h"  /* the arrangements a lent page may be asked for in */
#include "xpost_dev_generic.h"  /* retire a job's page device at the boundary */

static
Xpost_Object namedollarerror; /* cached result of xpost_name_cons(ctx, "$error")
                                 to reduce time in error handler */
static Xpost_Object nameerrordict;

int _xpost_interpreter_is_tracing = 0;             /* output trace log */
Xpost_Interpreter *itpdata;  /* the global interpreter instance, containing all contexts and memory files */

/* an external interrupt request: raised from a signal handler,
   consumed between evaluation steps */
static volatile sig_atomic_t _interrupt_pending = 0;

void xpost_interrupt(void)
{
    _interrupt_pending = 1;
}
static int _initializing = 1;  /* garbage collect does not run while _initializing is true.
                                  a getter function is exported in the memory file struct
                                  for the gc to access this global without #include'ing interpreter.h
                                  which would create a circular dependency. */

int eval(Xpost_Context *ctx);
static int _other_runnable(void);

/* What mainloop answers, named so that no two of them are the same
   number. A yield is a program's own doing -- the returntocaller
   operator, which showpage reaches under the returning semantics -- and
   a context that did not validate is the opposite of that: nothing ran
   and nothing can. Reported as a yield it hands the caller a context to
   resume, which validates no better on the next call and answers the
   same way, so an embedder driving a run to its end never reaches one.
   The failure is negative because every error number the interpreter
   carries is positive, and one of them stands where a yield stands. */
#define XPOST_MAINLOOP_DONE     0
#define XPOST_MAINLOOP_YIELDED  1
#define XPOST_MAINLOOP_INVALID (-1)

int mainloop(Xpost_Context *ctx);
void init(void);
void xit(void);

/* getter function for _initializing, for export */
int xpost_interpreter_get_initializing(void)
{
    return _initializing;
}

/* setter function for _initializing, for consistency */
void xpost_interpreter_set_initializing(int i)
{
    _initializing = i;
}

/* --- the interpreter and its contexts --------------------------------
   One interpreter to a process, holding the context table and the two
   memory files. A context is a thread of execution inside it, not a second
   interpreter: fork shares both banks and gives the new context stacks of
   its own. */

/*  allocate a global memory file
    find the next unused mfile in the global memory table */
static Xpost_Memory_File *xpost_interpreter_alloc_global_memory(void)
{
    int i;

    for (i = 0; i < MAXMFILE; i++)
    {
        if (itpdata->gtab[i].base == NULL)
        {
            return &itpdata->gtab[i];
        }
    }
    XPOST_LOG_ERR("cannot allocate Xpost_Memory_File, gtab exhausted");
    return NULL;
}

/* allocate a local memory file
   find the next unused mfile in the local memory table */
static Xpost_Memory_File *xpost_interpreter_alloc_local_memory(void)
{
    int i;
    for (i = 0; i < MAXMFILE; i++)
    {
        if (itpdata->ltab[i].base == NULL)
        {
            return &itpdata->ltab[i];
        }
    }
    XPOST_LOG_ERR("cannot allocate Xpost_Memory_File, ltab exhausted");
    return NULL;
}


/* cursor to next cid number to try to allocate */
static
unsigned int nextid = 0;

/* allocate a context-id and associated context struct
   returns cid;
   a context in state zero is considered available for allocation,
   this corresponds to the C_FREE enumeration constant.
 */
static int xpost_interpreter_cid_init(unsigned int *cid)
{
    unsigned int startid = nextid;

    /* Within an instance the counter only moves forward, and what is
       filed under a context identifier depends on that: a context object
       a program holds is valid exactly while the slot it names still
       holds the context that claimed the identifier, and the resolved
       graphics state cached against an identifier is served to whichever
       context presents it again. Both read a number this counter has
       handed out once. It restarts where the instance does -- both of
       those go with the instance, the object into its virtual memory and
       the cache into the operator table's set-up -- but reaching the end
       of the range inside one instance and starting over there would
       hand the same numbers out beside what they already name. There is
       no more range to give, so the fork is refused rather than answered
       with a number that means something else. */
    if (nextid > (unsigned int)-1 - MAXCONTEXT)
    {
        XPOST_LOG_ERR("context identifiers exhausted; cannot create new process");
        return 0;
    }
    /*printf("cid_init\n"); */
    while ( xpost_interpreter_cid_get_context(++nextid)->state != 0 )
    {
        if (nextid == startid + MAXCONTEXT)
        {
            XPOST_LOG_ERR("ctab full. cannot create new process");
            return 0;
        }
    }
    *cid = nextid;
    return 1;
}

/* adapter:
           ctx <- cid
   yield pointer to context struct given cid
   this function is exported via function-pointer in the memory file struct
   so the garbage collector can discover relevant contexts given only a memory file.
 */
Xpost_Context *xpost_interpreter_cid_get_context(unsigned int cid)
{
    /* A cid names a slot from one, so that zero can mean "no context" in
       the context list. Zero here would be that emptiness read as a name:
       unsigned, it takes the subtraction below to the top of the range and
       answers a slot that exists and is the wrong one. Every caller already
       knows better -- the allocator hands out ++nextid, the collector walks
       the list only while cid[i] is non-zero -- so this says so rather than
       answering NULL, which would put a branch in three callers for a value
       none of them can produce. */
    assert(cid != 0);
    return &itpdata->ctab[ (cid - 1) % MAXCONTEXT ];
}


/* initialize the name string stacks and name search trees (per memory file).
   seed the search trees.
   initialize and populate the optab and systemdict (global memory file).
   push systemdict on dict stack.
   allocate and push globaldict on dict stack.
   allocate and push userdict on dict stack.
   return 1 on success, 0 on failure
 */
static
int _xpost_interpreter_extra_context_init(Xpost_Context *ctx, const char *device)
{
    int ret;
    ret = xpost_name_init(ctx); /* NAMES NAMET */
    if (!ret)
    {
        xpost_memory_file_exit(ctx->lo);
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }
    ctx->vmmode = GLOBAL;

    ret = xpost_operator_init_optab(ctx); /* allocate and zero the optab structure */
    if (!ret)
    {
        xpost_memory_file_exit(ctx->lo);
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }

    /* seed the tree with a word from the middle of the alphabet */
    /* middle of the start */
    /* middle of the end */
    if (xpost_object_get_type(xpost_name_cons(ctx, "maxlength")) == invalidtype)
        return 0;
    if (xpost_object_get_type(xpost_name_cons(ctx, "getinterval")) == invalidtype)
        return 0;
    if (xpost_object_get_type(xpost_name_cons(ctx, "setmiterlimit")) == invalidtype)
        return 0;
    if (xpost_object_get_type((namedollarerror = xpost_name_cons(ctx, "$error"))) == invalidtype)
        return 0;
    if (xpost_object_get_type((nameerrordict = xpost_name_cons(ctx, "errordict"))) == invalidtype)
        return 0;

    /* populate the optab (and systemdict) with operators */
    if (!xpost_oplib_init_ops(ctx))
    {
        xpost_memory_file_exit(ctx->lo);
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }

    {
        Xpost_Object gd; /*globaldict */
        gd = xpost_dict_cons (ctx, 100);
        if (xpost_object_get_type(gd) == nulltype)
        {
            XPOST_LOG_ERR("cannot allocate globaldict");
            return 0;
        }
        ret = xpost_dict_put(ctx, xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0), xpost_name_cons(ctx, "globaldict"), gd);
        if (ret)
            return 0;
        xpost_stack_push(ctx->lo, ctx->ds, gd);
    }

    ctx->vmmode = LOCAL;
    /* seed the tree with a word from the middle of the alphabet */
    /* middle of the start */
    /* middle of the end */
    if (xpost_object_get_type(xpost_name_cons(ctx, "minimal")) == invalidtype)
        return 0;
    if (xpost_object_get_type(xpost_name_cons(ctx, "interest")) == invalidtype)
        return 0;
    if (xpost_object_get_type(xpost_name_cons(ctx, "solitaire")) == invalidtype)
        return 0;
    {
        Xpost_Object ud; /*userdict */
        ud = xpost_dict_cons (ctx, 100);
        if (xpost_object_get_type(ud) == nulltype)
        {
            XPOST_LOG_ERR("cannot allocate userdict");
            return 0;
        }
        ret = xpost_dict_put(ctx, ud, xpost_name_cons(ctx, "userdict"), ud);
        if (ret)
            return 0;
        xpost_stack_push(ctx->lo, ctx->ds, ud);
    }

    ctx->device_str = device;

    return 1;
}


/* initialize itpdata.
   create and initialize a single context in ctab[0]
 */
int xpost_interpreter_init(Xpost_Interpreter *itpptr, const char *device)
{
    int ret;

    /* what a blocked read asks before deciding whether to wait */
    xpost_file_other_runnable_set(_other_runnable);

    ret = xpost_context_init(&itpptr->ctab[0],
                             xpost_interpreter_cid_init,
                             xpost_interpreter_cid_get_context,
                             xpost_interpreter_get_initializing,
                             xpost_interpreter_set_initializing,
                             xpost_interpreter_alloc_local_memory,
                             xpost_interpreter_alloc_global_memory,
                             xpost_garbage_collect);
    if (!ret)
    {
        return 0;
    }
    ret = _xpost_interpreter_extra_context_init(&itpptr->ctab[0], device);
    if (!ret)
    {
        return 0;
    }

    itpptr->cid = itpptr->ctab[0].id;

    return 1;
}

/* destroy context in ctab[0] */
void xpost_interpreter_exit(Xpost_Interpreter *itpptr)
{
    xpost_context_exit(&itpptr->ctab[0]);
}


/*
 *  Interpreter eval##type() actions.
 *
 */

/* function type for interpreter action pointers.
   eval() has already popped the object from the execution stack. */
typedef
int evalfunc(Xpost_Context *ctx, Xpost_Object t);

/* Ceiling on errors handled back-to-back without the run reaching `stop`.
   A well-formed program recovers from every error through the error
   machinery, which ends in `stop`; that resets the count (see
   xpost_op_stop). Only a runaway cascade -- an error raised from inside
   the error machinery itself, before it can reach `stop` -- accumulates
   without bound. Left unchecked it spins until VM exhaustion; this makes
   it abort the job cleanly instead. The bound is far above any legitimate
   volume of caught-and-recovered errors, which never advance this count. */
#define XPOST_ERROR_CASCADE_LIMIT 4096

/* raise a stack's overflow error on crossing its ceiling; 0 if every
   stack is within bounds or already reported */
static int _stack_ceilings(Xpost_Context *ctx)
{
    if (ctx->es_over == 0)
    {
        if (xpost_stack_count(ctx->lo, ctx->es) > XPOST_EXEC_STACK_LIMIT)
        {
            ctx->es_over = 1;
            return execstackoverflow;
        }
    }
    else if (xpost_stack_count(ctx->lo, ctx->es) <= XPOST_EXEC_STACK_LIMIT)
        ctx->es_over = 0;
    if (ctx->os_over == 0)
    {
        if (xpost_stack_count(ctx->lo, ctx->os) > XPOST_OPER_STACK_LIMIT)
        {
            ctx->os_over = 1;
            return stackoverflow;
        }
    }
    else if (xpost_stack_count(ctx->lo, ctx->os) <= XPOST_OPER_STACK_LIMIT)
        ctx->os_over = 0;
    if (ctx->ds_over == 0)
    {
        if (xpost_stack_count(ctx->lo, ctx->ds) > XPOST_DICT_STACK_LIMIT)
        {
            ctx->ds_over = 1;
            return dictstackoverflow;
        }
    }
    else if (xpost_stack_count(ctx->lo, ctx->ds) <= XPOST_DICT_STACK_LIMIT)
        ctx->ds_over = 0;
    return 0;
}

/* --- the interpreter loop --------------------------------------------
   One executable object per turn, dispatched by type. Between turns comes
   the work that must not happen inside one -- a pending collection, a
   pending compaction, an interrupt raised from a signal handler -- which
   is what makes those safe points rather than interruptions. */

/* quit the interpreter */
static
int evalquit(Xpost_Context *ctx, Xpost_Object t)
{
    (void)t;
    ++ctx->quit;
    return 0;
}

/* discard the object */
static
int evalpop(Xpost_Context *ctx, Xpost_Object t)
{
    (void)ctx;
    (void)t;
    return 0;
}

/* push the object on the operand stack */
static
int evalpush(Xpost_Context *ctx, Xpost_Object t)
{
    if (!xpost_stack_push(ctx->lo, ctx->os, t))
        return stackoverflow;
    return 0;
}

/* load executable name:
   search the dictionary stack for the topmost definition,
   as per the load operator, then push the value on the
   execution stack (if executable) or the operand stack (if literal) */
static
int evalload(Xpost_Context *ctx, Xpost_Object n)
{
    int i;

    if (_xpost_interpreter_is_tracing)
    {
        Xpost_Object s = xpost_name_get_string(ctx, n);
        XPOST_LOG_DUMP("evalload <name \"%*s\">", s.comp_.sz, xpost_string_get_pointer(ctx, s));
    }

    { /* consult the cache of resolutions against the dict stack */
        unsigned int key = ((unsigned int)n.mark_.padw << 1) |
            ((n.mark_.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? 1 : 0);

        if (key < ctx->namecache_size &&
            ctx->namecache_gen[key] == ctx->namebind_gen)
        {
            Xpost_Object x = ctx->namecache_val[key];
            if (xpost_object_is_exe(x))
            {
                if (!xpost_stack_push(ctx->lo, ctx->es, x))
                    return execstackoverflow;
            }
            else
            {
                if (!xpost_stack_push(ctx->lo, ctx->os, x))
                    return stackoverflow;
            }
            return 0;
        }

        /* walk the dictionary stack segments directly, topmost first */
        {
        Xpost_Stack *ds_root = xpost_stack_at(ctx->lo, ctx->ds);
        Xpost_Stack *seg = xpost_stack_at(ctx->lo, ds_root->prevseg);

        for (;;)
        {
            for (i = seg->top; i--; )
            {
                Xpost_Object x = xpost_dict_get_name(ctx, seg->data[i], n);

                if (xpost_object_get_type(x) == invalidtype)
                    continue;

                if (key >= ctx->namecache_size)
                {
                    unsigned int nsz = ctx->namecache_size ? ctx->namecache_size : 4096;
                    unsigned int *ngen;
                    Xpost_Object *nval;
                    while (nsz <= key) nsz *= 2;
                    ngen = realloc(ctx->namecache_gen, nsz * sizeof(unsigned int));
                    nval = realloc(ctx->namecache_val, nsz * sizeof(Xpost_Object));
                    if (ngen)
                        ctx->namecache_gen = ngen;
                    if (nval)
                        ctx->namecache_val = nval;
                    if (ngen && nval)
                    {
                        memset(ctx->namecache_gen + ctx->namecache_size, 0,
                               (nsz - ctx->namecache_size) * sizeof(unsigned int));
                        ctx->namecache_size = nsz;
                    }
                }
                if (key < ctx->namecache_size)
                {
                    ctx->namecache_gen[key] = ctx->namebind_gen;
                    ctx->namecache_val[key] = x;
                }

                if (xpost_object_is_exe(x))
                {
                    if (!xpost_stack_push(ctx->lo, ctx->es, x))
                        return execstackoverflow;
                }
                else
                {
                    if (!xpost_stack_push(ctx->lo, ctx->os, x))
                        return stackoverflow;
                }
                return 0;
            }
            if (seg == ds_root)
                break;
            seg = xpost_stack_at(ctx->lo, seg->prevseg);
        }
        }
    }
    return undefined;
}

/* execute operator */
static
int evaloperator(Xpost_Context *ctx, Xpost_Object op)
{
    if (_xpost_interpreter_is_tracing)
        xpost_operator_dump(ctx, op.mark_.padw);
    return xpost_operator_exec(ctx, op.mark_.padw);
}

/* extract head (&tail) of array.
   steps successive elements of the procedure without re-entering the
   interpreter loop. the remaining interval is kept in the top slot of
   the execution stack, written lazily: literal elements cannot observe
   it, so it is brought up to date only before an element executes or
   the function returns. the loop returns to the interpreter whenever
   an element changes the execution stack, since anything it pushed
   must execute before the remaining interval. */
/* Whether a collection has been asked for.

   Either bank asks: the allocator records the request against the file
   it was allocating from, and a job may spend its memory in either. A
   safe point that read only one of them would leave a job that allocates
   in the other -- fonts and resources are allocated in global memory --
   growing until it ran out, with a collector able to reclaim it and
   nothing ever asking. The request is cleared from both, because the
   collection that follows reclaims both. */
static int _collection_wanted(Xpost_Context *ctx)
{
    int wanted = 0;

    if (ctx->lo && ctx->lo->garbage_collect_pending)
    {
        ctx->lo->garbage_collect_pending = 0;
        wanted = 1;
    }
    if (ctx->gl && ctx->gl->garbage_collect_pending)
    {
        ctx->gl->garbage_collect_pending = 0;
        wanted = 1;
    }
    return wanted;
}

/* The arena is closed up here, and only here.

   A rearrangement moves the bytes under every pointer derived from an
   entity's recorded address, so it cannot be done by the operator that
   asks for it: that operator runs underneath machinery holding such
   pointers. Between operator executions nothing holds one, which is the
   property the collection above relies on and the reason it is taken in
   the same place.

   The outermost loop is the only place that will do. A procedure is run
   by a nested call that holds the storage of the array it is running,
   and the calls below that hold theirs, so a rearrangement anywhere
   inside the nest leaves every frame but the innermost reading bytes
   that have moved. Only here is the nest empty. A reclaim asked for deep
   inside a procedure therefore waits until the procedure has finished,
   which costs nothing: the request is a request, not a promise about
   when.

   Closing the arena up hands the pages back itself, since gathering the
   free storage into one run above the cursor is what lets them go in a
   single call. The request is cleared whether or not a bank moved
   anything, so that one with nothing to close up does not carry it into
   the next reclaim. */
static void _compaction_wanted(Xpost_Context *ctx)
{
    if (ctx->lo && ctx->lo->compact_pending)
    {
        ctx->lo->compact_pending = 0;
        (void)xpost_free_compact(ctx->lo, NULL);
    }
    if (ctx->gl && ctx->gl->compact_pending)
    {
        ctx->gl->compact_pending = 0;
        (void)xpost_free_compact(ctx->gl, NULL);
    }
}

/* Runs a procedure's elements here, in a loop, rather than handing
   them back to the interpreter one at a time. What stands on the
   execution stack in place of the elements is a single interval naming
   the ones not yet reached, kept in one slot and rewritten as the loop
   advances: the stack still shows what is left to run, which execstack
   and an error's report both read, but a procedure no longer costs
   stack in proportion to its length, and its last element costs none
   at all. The interval is dropped rather than rewritten once nothing
   follows, which is what makes a tail call flat. */
static
int evalarray(Xpost_Context *ctx, Xpost_Object a)
{
    Xpost_Object b;
    const Xpost_Object *abase;
    Xpost_Stack *es_root;
    Xpost_Stack *es_top;
    Xpost_Stack *os_root;
    Xpost_Stack *os_top;
    unsigned char *seen_lo_base = ctx->lo->base;
    unsigned char *seen_gl_base = ctx->gl->base;
    /* the two banks: a context's for the life of a run, so the safe-point
       question below reads them rather than the context */
    Xpost_Memory_File *lo_mem = ctx->lo;
    Xpost_Memory_File *gl_mem = ctx->gl;
    unsigned int off = a.comp_.off;
    unsigned int remaining = a.comp_.sz;
    int have_tail = 0;      /* a slot for the interval exists on es */
    unsigned int slot_off = 0; /* the interval offset currently in the slot */

    /* resolve the array's storage once; elements are then direct reads.
       re-derived after any fused call, which may move the memory file. */
#define EVALARRAY_RESOLVE_ABASE() \
    do { \
        Xpost_Memory_File *amem_ = xpost_context_select_memory(ctx, a); \
        unsigned int aent_ = xpost_object_get_ent(a); \
        if (xpost_ent_valid(amem_, aent_) && \
            (a.comp_.off + (unsigned int)a.comp_.sz) * sizeof(Xpost_Object) \
                <= amem_->table.tab[aent_].sz) \
            abase = (const Xpost_Object *)(amem_->base \
                    + amem_->table.tab[aent_].adr); \
        else \
            abase = NULL; \
    } while (0)

#define EVALARRAY_RESOLVE_STACKS() \
    do { \
        es_root = xpost_stack_at(ctx->lo, ctx->es); \
        es_top = xpost_stack_at(ctx->lo, es_root->prevseg); \
        os_root = xpost_stack_at(ctx->lo, ctx->os); \
        os_top = xpost_stack_at(ctx->lo, os_root->prevseg); \
    } while (0)

    /* a stack push can allocate a fresh segment, growing (and so
       relocating) the memory file: re-derive every cached pointer,
       abase included, whenever a base has moved */
#define EVALARRAY_RECHECK_BASES() \
    do { \
        if (ctx->lo->base != seen_lo_base || ctx->gl->base != seen_gl_base) \
        { \
            seen_lo_base = ctx->lo->base; \
            seen_gl_base = ctx->gl->base; \
            EVALARRAY_RESOLVE_ABASE(); \
        } \
        EVALARRAY_RESOLVE_STACKS(); \
    } while (0)

    /* write the remaining interval (elements from off+1) into the es
       slot, or drop the slot when this is the last element */
#define EVALARRAY_SYNC_SLOT() \
    do { \
        if (remaining > 1) \
        { \
            if (!have_tail || slot_off != off + 1) \
            { \
                Xpost_Object tail_ = a; \
                tail_.comp_.off = off + 1; \
                tail_.comp_.sz = remaining - 1; \
                if (have_tail && es_top->top > 0) \
                    es_top->data[es_top->top - 1] = tail_; \
                else if (es_top->top < XPOST_STACK_SEGMENT_SIZE - 1) \
                { \
                    es_top->data[es_top->top++] = tail_; \
                    have_tail = 1; \
                } \
                else \
                { \
                    if (!xpost_stack_push(ctx->lo, ctx->es, tail_)) \
                        return execstackoverflow; \
                    EVALARRAY_RECHECK_BASES(); \
                    have_tail = 1; \
                } \
                slot_off = off + 1; \
            } \
        } \
        else if (have_tail) \
        { \
            if (es_top->top > 0) \
            { \
                --es_top->top; \
                if (es_top->top == 0 && \
                    es_top != xpost_stack_at(ctx->lo, ctx->es)) \
                { \
                    /* the drop can retreat the top segment: the cached \
                       pointer must follow, or a later slot write lands \
                       above the live top and is silently lost */ \
                    es_root->prevseg = es_top->prevseg; \
                    es_top = xpost_stack_at(ctx->lo, es_root->prevseg); \
                } \
            } \
            else \
            { \
                (void)xpost_stack_pop(ctx->lo, ctx->es); \
                es_top = xpost_stack_at(ctx->lo, es_root->prevseg); \
            } \
            have_tail = 0; \
        } \
    } while (0)

    /* like SYNC_SLOT but roots the current element together with the rest, so
       the array's storage stays anchored across a collection even when this is
       the last element (SYNC_SLOT would drop the slot there). The normal slot
       sync corrects it to the tail, or drops it, before the element executes,
       so tail-call flattening is preserved. */
#define EVALARRAY_ROOT_CURRENT() \
    do { \
        Xpost_Object cur_ = a; \
        cur_.comp_.off = off; \
        cur_.comp_.sz = remaining; \
        if (have_tail && es_top->top > 0) \
            es_top->data[es_top->top - 1] = cur_; \
        else if (es_top->top < XPOST_STACK_SEGMENT_SIZE - 1) \
        { \
            es_top->data[es_top->top++] = cur_; \
            have_tail = 1; \
        } \
        else \
        { \
            if (!xpost_stack_push(ctx->lo, ctx->es, cur_)) \
                return execstackoverflow; \
            EVALARRAY_RECHECK_BASES(); \
            have_tail = 1; \
        } \
        slot_off = off; \
    } while (0)

    /* running a procedure reads its elements, which no access forbids
       (see xpost_op_control.h). Every way of reaching a procedure that
       does not schedule it through one of the operators arrives here,
       so the rule is asked once, where the reading begins, rather than
       at each of the many places one is scheduled. */
    if (!xpost_op_exec_access_ok(ctx, a))
        return invalidaccess;

    if (remaining == 0)
        return 0;

    EVALARRAY_RESOLVE_ABASE();
    EVALARRAY_RESOLVE_STACKS();

    for (;;)
    {
        Xpost_Object_Type btype;

        if (ctx->quit)
        {
            EVALARRAY_SYNC_SLOT();
            return 0;
        }

        if (_interrupt_pending)
        {
            /* An external interrupt request -- the CLI's Ctrl-C, or an
               embedder's xpost_interrupt to abort a runaway job -- is read
               between elements here too, not only in the interpreter loop.
               A fused procedure runs its elements without returning to that
               loop, so a tail-recursive procedure (one whose last element
               re-invokes a procedure by name) would otherwise spin with the
               request unseen and be unabortable short of killing the
               process. Raise it as the loop does, by returning the error. */
            _interrupt_pending = 0;
            EVALARRAY_SYNC_SLOT();
            return interrupt;
        }

        /* between elements is a safe point just like the interpreter
           loop: a requested collection must not starve while a fused
           procedure runs through a long allocation-heavy stretch.

           The question is asked of the two banks directly here, through
           pointers taken once above: they are the context's for the life
           of a run, so re-reading them from the context at every element
           of every procedure is a load per bank that answers the same
           thing each time. What clears the flags, and decides whether a
           collection is taken, is still the one function below. */
        if ((lo_mem->garbage_collect_pending
             || gl_mem->garbage_collect_pending)
            && _collection_wanted(ctx))
        {
            /* anchor the current element (not just the tail) so an unrooted
               anonymous procedure is not swept while its last element runs */
            EVALARRAY_ROOT_CURRENT();
            if (ctx->lo->garbage_collect_is_installed
                && ctx->lo->garbage_collect(ctx->lo, xpost_garbage_auto_banks(ctx), 1) < 0)
                return VMerror;
            EVALARRAY_RECHECK_BASES();
        }

        /* likewise a push the stack would not take: a fused procedure
           runs its elements without returning to the interpreter loop,
           so the refusal is read here too rather than waiting for the
           procedure to finish. Read through the bank taken once above,
           for the reason the safe point above it is. */
        if (lo_mem->push_refused)
        {
            lo_mem->push_refused = 0;
            XPOST_LOG_ERR("a stack would not take a pushed object");
            return VMerror;
        }

        if (abase)
            b = abase[off];
        else
        {
            Xpost_Object cur_ = a;
            cur_.comp_.off = off;
            cur_.comp_.sz = remaining;
            b = xpost_array_get(ctx, cur_, 0);
        }
        btype = xpost_object_get_type(b);
        if (btype == invalidtype || btype >= XPOST_OBJECT_NTYPES)
        {
            EVALARRAY_SYNC_SLOT();
            return unregistered;
        }

        if (btype == arraytype || !xpost_object_is_exe(b))
        {
            /* the interpreter cycle would only move it to the operand
               stack; do so directly */
            if (os_top->top < XPOST_STACK_SEGMENT_SIZE - 1)
                os_top->data[os_top->top++] = b;
            else
            {
                EVALARRAY_SYNC_SLOT();
                if (!xpost_stack_push(ctx->lo, ctx->os, b))
                    return stackoverflow;
                EVALARRAY_RECHECK_BASES();
            }
        }
        else if (btype == operatortype || btype == nametype)
        {
            unsigned int seen_seg;
            unsigned int seen_top;
            int ret;

            /* the hottest operators inline when their operands sit in
               the top segment; any precondition failure falls through
               to the generic invocation, keeping error behaviour
               identical */
            if (btype == operatortype)
            {
                unsigned int w = b.mark_.padw;
                unsigned int ot = os_top->top;

                ctx->currentobject = b;
                if (w == (unsigned int)XPOST_OP_CODE(ctx, oppop) && ot >= 1)
                {
                    --os_top->top;
                    goto next_element;
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, opexch) && ot >= 2)
                {
                    Xpost_Object t_ = os_top->data[ot - 1];
                    os_top->data[ot - 1] = os_top->data[ot - 2];
                    os_top->data[ot - 2] = t_;
                    goto next_element;
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, opdup) && ot >= 1 &&
                    ot < XPOST_STACK_SEGMENT_SIZE - 1)
                {
                    os_top->data[ot] = os_top->data[ot - 1];
                    ++os_top->top;
                    goto next_element;
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, opindex) && ot >= 2)
                {
                    Xpost_Object n_ = os_top->data[ot - 1];
                    /* the operator's own selection rule, applied to the
                       operands below n in this segment (see
                       xpost_op_stack.h) */
                    if (xpost_object_get_type(n_) == integertype &&
                        xpost_op_index_check(n_.int_.val, (int)ot - 1) == 0)
                    {
                        os_top->data[ot - 1] = os_top->data[ot - 2 - n_.int_.val];
                        goto next_element;
                    }
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, opget) && ot >= 2)
                {
                    Xpost_Object a_ = os_top->data[ot - 2];
                    Xpost_Object i_ = os_top->data[ot - 1];
                    if (xpost_object_get_type(a_) == arraytype &&
                        xpost_object_get_type(i_) == integertype)
                    {
                        /* the operator's own get, access checks and all
                           (see xpost_op_array.h) */
                        Xpost_Object t_;
                        if (xpost_op_array_get_checked(ctx, a_, i_.int_.val,
                                                       &t_) == 0)
                        {
                            --os_top->top;
                            os_top->data[ot - 2] = t_;
                            goto next_element;
                        }
                        /* on failure fall through: the generic path
                           re-executes the get for the exact protocol */
                    }
                }
                if (ot >= 2 &&
                    (w == (unsigned int)XPOST_OP_CODE(ctx, opadd) ||
                     w == (unsigned int)XPOST_OP_CODE(ctx, opsub) ||
                     w == (unsigned int)XPOST_OP_CODE(ctx, opmul)))
                {
                    Xpost_Object x_ = os_top->data[ot - 2];
                    Xpost_Object y_ = os_top->data[ot - 1];
                    if (xpost_object_get_type(x_) == integertype &&
                        xpost_object_get_type(y_) == integertype)
                    {
                        /* the operators' own range-preserving arithmetic,
                           so an out-of-range result becomes the same real
                           here as it does there (see xpost_op_math.h) */
                        Xpost_Object r_ =
                            w == (unsigned int)XPOST_OP_CODE(ctx, opadd)
                                ? xpost_int_add(x_.int_.val, y_.int_.val)
                            : w == (unsigned int)XPOST_OP_CODE(ctx, opsub)
                                ? xpost_int_sub(x_.int_.val, y_.int_.val)
                                : xpost_int_mul(x_.int_.val, y_.int_.val);
                        --os_top->top;
                        os_top->data[ot - 2] = r_;
                        goto next_element;
                    }
                }
                if (ot >= 2 &&
                    (w == (unsigned int)XPOST_OP_CODE(ctx, opand) ||
                     w == (unsigned int)XPOST_OP_CODE(ctx, opor) ||
                     w == (unsigned int)XPOST_OP_CODE(ctx, opxor) ||
                     w == (unsigned int)XPOST_OP_CODE(ctx, opbitshift) ||
                     w == (unsigned int)XPOST_OP_CODE(ctx, opmod) ||
                     w == (unsigned int)XPOST_OP_CODE(ctx, opidiv)))
                {
                    Xpost_Object x_ = os_top->data[ot - 2];
                    Xpost_Object y_ = os_top->data[ot - 1];
                    if (xpost_object_get_type(x_) == integertype &&
                        xpost_object_get_type(y_) == integertype)
                    {
                        integer a_ = x_.int_.val;
                        integer b_ = y_.int_.val;
                        integer r_;
                        int have_ = 1;

                        /* the three bitwise operators are total over two
                           integers, so what they answer is the answer;
                           the two that divide are not, and the operands
                           they refuse or answer with a real go back to
                           the operator rather than being spelt again
                           here (see xpost_op_math.c). bitshift asks the
                           rule the operator asks, from where the two
                           share it (see xpost_op_boolean.h). */
                        if (w == (unsigned int)XPOST_OP_CODE(ctx, opand))
                            r_ = a_ & b_;
                        else if (w == (unsigned int)XPOST_OP_CODE(ctx, opor))
                            r_ = a_ | b_;
                        else if (w == (unsigned int)XPOST_OP_CODE(ctx, opxor))
                            r_ = a_ ^ b_;
                        else if (w == (unsigned int)XPOST_OP_CODE(ctx, opbitshift))
                            r_ = xpost_int_bitshift(a_, b_);
                        else if (b_ == 0 || b_ == -1)
                            have_ = 0;
                        else if (w == (unsigned int)XPOST_OP_CODE(ctx, opmod))
                            r_ = a_ % b_;
                        else
                            r_ = a_ / b_;

                        if (have_)
                        {
                            --os_top->top;
                            os_top->data[ot - 2] = xpost_int_cons(r_);
                            goto next_element;
                        }
                    }
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, optype) && ot >= 1)
                {
                    /* the operator's own naming, so a packed array is
                       reported as its own type here as it is there
                       (see xpost_op_type.h) */
                    unsigned int k_ = xpost_op_type_index(os_top->data[ot - 1]);
                    if (xpost_object_get_type(ctx->typenames[k_]) != nametype)
                    {
                        ctx->typenames[k_] = xpost_object_cvx(
                            xpost_name_cons(ctx, xpost_op_type_name(k_)));
                        /* interning the name may grow (and so move) the
                           memory file: re-derive the cached pointers */
                        EVALARRAY_RECHECK_BASES();
                        ot = os_top->top;
                    }
                    os_top->data[ot - 1] = ctx->typenames[k_];
                    goto next_element;
                }
                if (ot >= 2)
                {
                    /* the operators' own comparison and their own
                       reading of it serve all six relations, so a pair
                       the comparison settles without reading vm answers
                       the same here as it does there (see xpost_dict.h
                       and xpost_op_boolean.h) */
                    int rel_ =
                        w == (unsigned int)XPOST_OP_CODE(ctx, opeq) ? XPOST_OP_REL_EQ :
                        w == (unsigned int)XPOST_OP_CODE(ctx, opne) ? XPOST_OP_REL_NE :
                        w == (unsigned int)XPOST_OP_CODE(ctx, oplt) ? XPOST_OP_REL_LT :
                        w == (unsigned int)XPOST_OP_CODE(ctx, ople) ? XPOST_OP_REL_LE :
                        w == (unsigned int)XPOST_OP_CODE(ctx, opgt) ? XPOST_OP_REL_GT :
                        w == (unsigned int)XPOST_OP_CODE(ctx, opge) ? XPOST_OP_REL_GE : -1;
                    int cmp_;

                    /* the ordered four are restricted to two numbers
                       or two strings, so the pair is asked the same
                       question the operators ask before either road
                       reaches the comparison */
                    if (rel_ >= 0 &&
                        xpost_op_relation_is_ordered((Xpost_Op_Relation)rel_) &&
                        !xpost_op_ordered_comparable(os_top->data[ot - 2],
                                                     os_top->data[ot - 1]))
                        rel_ = -1;
                    if (rel_ >= 0 &&
                        xpost_dict_compare_simple(os_top->data[ot - 2],
                                                  os_top->data[ot - 1], &cmp_))
                    {
                        --os_top->top;
                        os_top->data[ot - 2] = xpost_bool_cons(
                            xpost_op_relation((Xpost_Op_Relation)rel_, cmp_));
                        goto next_element;
                    }
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, oproll) && ot >= 2)
                {
                    Xpost_Object j_ = os_top->data[ot - 1];
                    Xpost_Object n_ = os_top->data[ot - 2];
                    if (xpost_object_get_type(n_) == integertype &&
                        xpost_object_get_type(j_) == integertype &&
                        n_.int_.val > 0 && n_.int_.val <= 32 &&
                        (unsigned int)n_.int_.val + 2 <= ot)
                    {
                        /* the operator's own shift and its own
                           placement rule, over the operands below n and
                           j: top_[-i] is position i counting down from
                           the top of the group (see xpost_op_stack.h) */
                        Xpost_Object tmp_[32];
                        integer n = n_.int_.val;
                        integer j = xpost_op_roll_shift(n, j_.int_.val);
                        integer k, held;
                        Xpost_Object *top_ = os_top->data + ot - 3;
                        /* the writeback covers exactly what the lift
                           put in the holder */
                        for (held = 0; held < n; held++)
                            tmp_[held] = top_[-xpost_op_roll_source(held, n, j)];
                        for (k = 0; k < held; k++)
                            top_[-k] = tmp_[k];
                        os_top->top -= 2;
                        goto next_element;
                    }
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, opdef) && ot >= 2)
                {
                    Xpost_Object k_ = os_top->data[ot - 2];
                    Xpost_Object v_ = os_top->data[ot - 1];
                    if (xpost_object_get_type(k_) == nametype)
                    {
                        Xpost_Stack *ds_root = xpost_stack_at(ctx->lo, ctx->ds);
                        Xpost_Stack *ds_top = xpost_stack_at(ctx->lo, ds_root->prevseg);
                        if (ds_top->top > 0)
                        {
                            Xpost_Object d_ = ds_top->data[ds_top->top - 1];
                            Xpost_Memory_File *dmem_ = xpost_context_select_memory(ctx, d_);
                            if (xpost_dict_def_fast_ok(ctx, dmem_, v_))
                            {
                                /* the operands stay on the stack through the
                                   put, keeping them visible to the collector
                                   if the dictionary grows; the shared def
                                   core carries the semantics (see
                                   xpost_op_dict.h) */
                                int ret_ = xpost_dict_def_cached(ctx, dmem_, d_, k_, v_);
                                if (ret_ == 0)
                                {
                                    if (ctx->lo->base != seen_lo_base ||
                                        ctx->gl->base != seen_gl_base)
                                    {
                                        seen_lo_base = ctx->lo->base;
                                        seen_gl_base = ctx->gl->base;
                                        EVALARRAY_RESOLVE_ABASE();
                                        EVALARRAY_RESOLVE_STACKS();
                                    }
                                    os_top->top -= 2;
                                    goto next_element;
                                }
                            }
                        }
                    }
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, opput) && ot >= 3)
                {
                    Xpost_Object a_ = os_top->data[ot - 3];
                    Xpost_Object i_ = os_top->data[ot - 2];
                    Xpost_Object v_ = os_top->data[ot - 1];
                    if (xpost_object_get_type(a_) == arraytype &&
                        xpost_object_get_type(i_) == integertype)
                    {
                        /* the operator's own put, access checks and all
                           (see xpost_op_array.h). The operands stay on
                           the stack through it, so a saved array that
                           copies on first write keeps them visible to
                           the collector */
                        int ret_ = xpost_op_array_put_checked(ctx, a_,
                                                              i_.int_.val, v_);
                        if (ret_ == 0)
                        {
                            if (ctx->lo->base != seen_lo_base ||
                                ctx->gl->base != seen_gl_base)
                            {
                                seen_lo_base = ctx->lo->base;
                                seen_gl_base = ctx->gl->base;
                                EVALARRAY_RESOLVE_ABASE();
                                EVALARRAY_RESOLVE_STACKS();
                            }
                            os_top->top -= 3;
                            goto next_element;
                        }
                        /* on failure fall through: the generic path
                           re-executes the put for the exact protocol */
                    }
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, opif) && ot >= 2)
                {
                    Xpost_Object p_ = os_top->data[ot - 1];
                    Xpost_Object b_ = os_top->data[ot - 2];
                    if (xpost_object_get_type(b_) == booleantype &&
                        xpost_object_get_type(p_) == arraytype &&
                        xpost_object_is_exe(p_) &&
                        /* the operator refuses a procedure it may not
                           read, whether or not the condition selects it
                           (see xpost_op_control.h) */
                        xpost_op_exec_access_ok(ctx, p_))
                    {
                        os_top->top -= 2;
                        if (!b_.int_.val)
                            goto next_element;
                        EVALARRAY_SYNC_SLOT();
                        have_tail = 0;
                        a = p_;
                        off = a.comp_.off;
                        remaining = a.comp_.sz;
                        if (remaining == 0)
                            return 0;
                        EVALARRAY_RESOLVE_ABASE();
                        continue;
                    }
                }
                if (w == (unsigned int)XPOST_OP_CODE(ctx, opifelse) && ot >= 3)
                {
                    Xpost_Object p2_ = os_top->data[ot - 1];
                    Xpost_Object p1_ = os_top->data[ot - 2];
                    Xpost_Object b_ = os_top->data[ot - 3];
                    if (xpost_object_get_type(b_) == booleantype &&
                        xpost_object_get_type(p1_) == arraytype &&
                        xpost_object_is_exe(p1_) &&
                        xpost_object_get_type(p2_) == arraytype &&
                        xpost_object_is_exe(p2_) &&
                        /* the operator refuses either procedure it may
                           not read, whichever the condition selects
                           (see xpost_op_control.h) */
                        xpost_op_exec_access_ok(ctx, p1_) &&
                        xpost_op_exec_access_ok(ctx, p2_))
                    {
                        os_top->top -= 3;
                        EVALARRAY_SYNC_SLOT();
                        have_tail = 0;
                        a = b_.int_.val ? p1_ : p2_;
                        off = a.comp_.off;
                        remaining = a.comp_.sz;
                        if (remaining == 0)
                            return 0;
                        EVALARRAY_RESOLVE_ABASE();
                        continue;
                    }
                }
            }

            if (btype == nametype)
            {
                /* resolve via the name cache without leaving the loop */
                unsigned int key = ((unsigned int)b.mark_.padw << 1) |
                    ((b.mark_.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? 1 : 0);
                if (key < ctx->namecache_size &&
                    ctx->namecache_gen[key] == ctx->namebind_gen)
                {
                    Xpost_Object x = ctx->namecache_val[key];
                    if (!xpost_object_is_exe(x))
                    {
                        if (os_top->top < XPOST_STACK_SEGMENT_SIZE - 1)
                        {
                            os_top->data[os_top->top++] = x;
                            goto next_element;
                        }
                    }
                    else if (xpost_object_get_type(x) == operatortype)
                    {
                        /* execute the bound operator via the generic
                           machinery below, sparing the es round-trip */
                        b = x;
                        btype = operatortype;
                        ctx->currentobject = b;
                        goto generic_operator;
                    }
                    else if (xpost_object_get_type(x) == arraytype)
                    {
                        /* a procedure call: continue stepping it here,
                           leaving the current interval behind on es.
                           Recursion deepens the stacks through this
                           site without ever surfacing to the
                           interpreter loop, so the ceilings, and the
                           access the reading of a procedure needs, are
                           kept here */
                        int over;

                        if (!xpost_op_exec_access_ok(ctx, x))
                        {
                            ctx->currentobject = b;
                            EVALARRAY_SYNC_SLOT();
                            return invalidaccess;
                        }
                        /* The ceiling check recounts the operand, exec and
                           dict stacks, which means walking them, and a
                           program that calls procedures with a deep stack
                           standing under them would pay that walk on every
                           call -- the walk's length being the depth, the
                           cost of a run rises with the product. A call
                           deepens a stack by one, so a check taken one time
                           in a window still catches an overflow within a
                           window's worth of calls, long before a stack the
                           limit already bounds could run anything out. Take
                           it on that cadence rather than on every call. */
                        {
                            static unsigned int ceilevery = 0;
                            over = (ceilevery++ & 1023) ? 0
                                                        : _stack_ceilings(ctx);
                        }
                        if (over)
                        {
                            ctx->currentobject = b;
                            EVALARRAY_SYNC_SLOT();
                            return over;
                        }
                        EVALARRAY_SYNC_SLOT();
                        have_tail = 0;
                        a = x;
                        off = a.comp_.off;
                        remaining = a.comp_.sz;
                        if (remaining == 0)
                            return 0;
                        EVALARRAY_RESOLVE_ABASE();
                        continue;
                    }
                }
            }

          generic_operator:
            EVALARRAY_SYNC_SLOT();

            /* remember the execution stack position of our interval */
            seen_seg = es_root->prevseg;
            es_top = xpost_stack_at(ctx->lo, seen_seg);
            seen_top = es_top->top;

            ctx->currentobject = b;
            if (btype == operatortype)
            {
                if (_xpost_interpreter_is_tracing)
                    xpost_operator_dump(ctx, b.mark_.padw);
                ret = xpost_operator_exec(ctx, b.mark_.padw);
            }
            else
                ret = evalload(ctx, b);
            if (ret)
                return ret;
            if (ctx->quit)
                return 0;

            /* if the execution stack changed, what was pushed (or the
               unwound state) takes precedence: resume via the loop */
            es_root = xpost_stack_at(ctx->lo, ctx->es);
            if (es_root->prevseg != seen_seg)
                return 0;
            es_top = xpost_stack_at(ctx->lo, seen_seg);
            if (es_top->top != seen_top)
                return 0;
            if (have_tail)
            {
                Xpost_Object slot = es_top->data[seen_top - 1];
                if (slot.tag != a.comp_.tag ||
                    slot.comp_.sz != remaining - 1 ||
                    slot.comp_.off != off + 1 ||
                    xpost_object_get_ent(slot) != xpost_object_get_ent(a))
                    return 0;
            }
            if (ctx->lo->base != seen_lo_base || ctx->gl->base != seen_gl_base)
            {
                seen_lo_base = ctx->lo->base;
                seen_gl_base = ctx->gl->base;
                EVALARRAY_RESOLVE_ABASE();
            }
            EVALARRAY_RESOLVE_STACKS();
        }
        else
        {
            /* rarer executable types resume via the interpreter loop */
            EVALARRAY_SYNC_SLOT();
            if (!xpost_stack_push(ctx->lo, ctx->es, b))
                return execstackoverflow;
            return 0;
        }

      next_element:
        if (remaining == 1)
        {
            /* the slot, if any, still holds off+1..; it must not
               survive: it was consumed by this call */
            EVALARRAY_SYNC_SLOT();
            return 0;
        }
        ++off;
        --remaining;
    }
#undef EVALARRAY_RESOLVE_ABASE
#undef EVALARRAY_RESOLVE_STACKS
#undef EVALARRAY_SYNC_SLOT
#undef EVALARRAY_ROOT_CURRENT
}

/* extract token from string */
static
int evalstring(Xpost_Context *ctx, Xpost_Object s)
{
    Xpost_Object b,t;
    int ret;

    /* A string reached as code is executed, not read on the program's
       behalf, so the access asked of it is the one for execution: PLRM
       3.3.2 permits an execute-only object to be executed and withholds
       that from a no-access one. Going through the token operator asked
       for read access instead, which no-access and execute-only both
       withhold, so an execute-only string would not run -- leaving the
       string with three rungs where the ladder has four. The scan is
       shared with that operator; only the rule differs. */
    if (xpost_object_get_access(ctx, s) == XPOST_OBJECT_TAG_ACCESS_NONE)
        return invalidaccess;
    assert(ctx->gl->base);
    ret = xpost_token_string_scan(ctx, s);
    if (ret)
        return ret;
    b = xpost_stack_pop(ctx->lo, ctx->os);
    if (xpost_object_get_type(b) == invalidtype)
        return stackunderflow;
    if (b.int_.val)
    {
        t = xpost_stack_pop(ctx->lo, ctx->os);
        if (xpost_object_get_type(t) == invalidtype)
            return stackunderflow;
        s = xpost_stack_pop(ctx->lo, ctx->os);
        if (xpost_object_get_type(s) == invalidtype)
            return stackunderflow;
        if (!xpost_stack_push(ctx->lo, ctx->es, s))
            return execstackoverflow;
        if (xpost_object_get_type(t)==arraytype && ctx->scanner_defer)
        {
            if (!xpost_stack_push(ctx->lo, ctx->os , t))
                return stackoverflow;
        }
        else
        {
            if (!xpost_stack_push(ctx->lo, ctx->es , t))
                return execstackoverflow;
        }
    }
    return 0;
}

/* extract token from file */
static
int evalfile(Xpost_Context *ctx, Xpost_Object f)
{
    Xpost_Object b,t;
    Xpost_Object_Tag_Access access;
    int ret;

    /* Executing a file reads it, a token at a time, so the access
       attribute the file object carries governs execution (PLRM 3.8.2).
       A file that may be read may be executed, and execute-only leaves a
       file that may be executed and not read, which is the access whose
       whole purpose is to let a program run something it may not look at
       (PLRM 3.3.2). The attribute belongs to the object rather than to
       the stream, so it is asked before the stream's state is, and every
       route by which a file reaches the execution stack -- exec, run, a
       file object stored in a procedure -- arrives here. It is asked once:
       this runs for every token of every file the interpreter executes. */
    access = xpost_object_get_access(ctx, f);
    if (!(access & XPOST_OBJECT_TAG_ACCESS_FILE_EXEC))
        return invalidaccess;

    /* a program may close the file it is executing from -- the
       Type 1 font idiom mark currentfile closefile -- and a closed
       file simply has nothing further to run */
    if (!xpost_file_get_status(ctx->lo, f))
        return 0;

    /* The token comes out through the token operator, which asks the file
       for read access as it would for a program calling it. Where that
       read is the interpreter's rather than the program's -- a file left
       executable and not readable, which is what execute-only is for --
       the operator is given a reader over the same stream instead. The
       file object the program can reach, the one going back on the
       execution stack below and the one currentfile answers with, keeps
       the access it was reduced to, so the program still cannot read what
       it is running. */
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          (access & XPOST_OBJECT_TAG_ACCESS_FILE_READ) ? f
                          : xpost_object_set_access(ctx, f, access
                                | XPOST_OBJECT_TAG_ACCESS_FILE_READ)))
        return stackoverflow;
    assert(ctx->gl->base);
    ret = xpost_operator_exec(ctx, XPOST_OP_CODE(ctx, token));
    if (ret)
        return ret;
    b = xpost_stack_pop(ctx->lo, ctx->os);
    if (b.int_.val)
    {
        t = xpost_stack_pop(ctx->lo, ctx->os);
        if (!xpost_stack_push(ctx->lo, ctx->es, f))
            return execstackoverflow;
        if (xpost_object_get_type(t)==arraytype && ctx->scanner_defer)
        {
            if (!xpost_stack_push(ctx->lo, ctx->os, t))
                return stackoverflow;
        }
        else
        {
            if (!xpost_stack_push(ctx->lo, ctx->es, t))
                return execstackoverflow;
        }
    }
    else
    {
        ret = xpost_file_object_close_at_eod(ctx->lo, f);
        if (ret)
            XPOST_LOG_ERR("%s error closing file", errorname[ret]);
    }
    return 0;
}

/* interpreter actions for executable types */
evalfunc *evalinvalid = evalquit;
evalfunc *evalmark = evalpush;
evalfunc *evalnull = evalpop;
evalfunc *evalinteger = evalpush;
evalfunc *evalboolean = evalpush;
evalfunc *evalreal = evalpush;
evalfunc *evalsave = evalpush;
evalfunc *evaldict = evalpush;
evalfunc *evalextended = evalquit;
evalfunc *evalglob = evalpush;
evalfunc *evalmagic = evalquit;

evalfunc *evalcontext = evalpush;
evalfunc *evalname = evalload;

/* install the evaltype functions (possibly via pointers) in the jump table */
evalfunc *evaltype[XPOST_OBJECT_NTYPES + 1];
#define AS_EVALINIT(_) evaltype[ _ ## type ] = eval ## _ ;

/* use above macro to initialize function table
   keyed by enum types;
 */
static
void initevaltype(void)
{
    XPOST_OBJECT_TYPES(AS_EVALINIT)
}


/*
   call window device's event_handler function
   which should check for Events or Messages from the
   underlying Window System, process one or more of them,
   and then return 0.
   it should leave all stacks undisturbed.
 */
int idleproc (Xpost_Context *ctx)
{
    int ret;

    if ((xpost_object_get_type(ctx->event_handler) == operatortype) &&
        (xpost_object_get_type(ctx->window_device) == dicttype))
    {
        if (!xpost_stack_push(ctx->lo, ctx->os, ctx->window_device))
        {
            return stackoverflow;
        }
        ret = xpost_operator_exec(ctx, ctx->event_handler.mark_.padw);
        if (ret)
        {
            XPOST_LOG_ERR("event_handler returned %d (%s)",
                    ret, errorname[ret]);
            XPOST_LOG_ERR("disabling event_handler");
            ctx->event_handler = null;
            return ret;
        }
    }
    return 0;
}

/*
   check basic pointers and addresses for sanity
 */
static
int validate_context(Xpost_Context *ctx)
{
    /*assert(ctx); */
    /*assert(ctx->lo); */
    /*assert(ctx->lo->base); */
    /*assert(ctx->gl); */
    /*assert(ctx->gl->base); */
    if (!ctx)
    {
        XPOST_LOG_ERR("ctx invalid");
        return 0;
    }
    if (!ctx->lo)
    {
        XPOST_LOG_ERR("ctx->lo invalid");
        return 0;
    }
    if (!ctx->lo->base)
    {
        XPOST_LOG_ERR("ctx->lo->base invalid");
        return 0;
    }
    if (!ctx->gl)
    {
        XPOST_LOG_ERR("ctx->gl invalid");
        return 0;
    }
    if (!ctx->gl->base)
    {
        XPOST_LOG_ERR("ctx->gl->base invalid");
        return 0;
    }
    return 1;
}

/*
   one iteration of the central loop
   called repeatedly by mainloop()
 */
int eval(Xpost_Context *ctx)
{
    int ret;
    Xpost_Object t;
    Xpost_Stack *es_root;
    Xpost_Stack *es_top;
    Xpost_Object_Type type;

    /* pop the next object, directly off the top segment when possible */
    es_root = xpost_stack_at(ctx->lo, ctx->es);
    es_top = xpost_stack_at(ctx->lo, es_root->prevseg);
    if (es_top->top > 0)
        t = es_top->data[--es_top->top];
    else
        t = xpost_stack_pop(ctx->lo, ctx->es);

    ctx->currentobject = t; /* for _onerror to determine if hold stack contents are restoreable.
                               if opexec(opcode) discovers opcode != ctx->currentobject.mark_.padw
                               it sets a flag indicating the hold stack does not contain
                               ctx->currentobject's arguments.
                               if an error is encountered, currentobject is reported as the
                               errant object since it is the "entry point" to the interpreter.
                             */

    if (_xpost_interpreter_is_tracing)
    {
        xpost_object_dump(t);
    }

    if (xpost_object_get_type(ctx->event_handler) == operatortype)
    {
        ret = idleproc(ctx); /* periodically process asynchronous events */
        if (ret)
            return ret;
    }

    /* check object for sanity before using jump table */
    type = xpost_object_get_type(t);
    if (type == invalidtype || type >= XPOST_OBJECT_NTYPES)
        return unregistered;

    if ( xpost_object_is_exe(t) ) /* if executable */
    {
        /* dispatch the common types with predictable direct calls;
           the jump table's indirect branch mispredicts heavily */
        switch (type)
        {
            case operatortype: ret = evaloperator(ctx, t); break;
            case arraytype:
            {
                /* the run resolves the array's storage to a pointer and
                   reads its elements through it, so the array has to
                   outlive the run whatever else stops naming it. The
                   execution stack carries the part not yet reached, and
                   for the element in hand that is not enough: a
                   collection arriving between two elements would find
                   the last one named by nothing. Recorded for the length
                   of the run and put back afterwards, so a procedure run
                   from inside another leaves the outer one rooted. */
                Xpost_Object outer = ctx->executingarray;

                ctx->executingarray = t;
                ret = evalarray(ctx, t);
                ctx->executingarray = outer;
                break;
            }
            case nametype:     ret = evalload(ctx, t);     break;
            case integertype:  /*@fallthrough@*/
            case realtype:     /*@fallthrough@*/
            case booleantype:  ret = evalpush(ctx, t);     break;
            default:           ret = evaltype[type](ctx, t);
        }
    }
    else
        ret = evalpush(ctx, t);

    return ret;
}

/* --- what happens when an operator refuses ---------------------------
   An operator leaves nothing behind when it refuses: the stacks are
   unwound to the depths they had at the innermost live wrapped call before
   anything is recorded. Then the PostScript side is handed the error, and
   what it does with it is the language's business rather than this
   file's. */

/* An error leaving a wrapped operator is the operator's error.
   Each live call left its frame on the exec stack -- the operator, the
   operand and dict depths at the call, and the operands it was called
   with, under the finish marker -- so the frames above the nearest
   stopped context are exactly the calls the coming stop will unwind
   out of: the innermost names the command, and the stacks go back to
   what the outermost found. A call whose frame sits below the stopped
   context is left alone: its procedure keeps running and its stacks
   are its own business.

   The walk ends at a callout boundary for the same reason. An operator
   that calls back into a procedure of the program's puts one on the
   exec stack beneath the procedure, and a failure above it is the
   procedure's: the object being executed is the program's own, so the
   stack goes back to what the procedure found and no further. The
   operator underneath consumed its operands doing the work that led to
   the call, and they stay consumed. The boundary is one-sided -- a call
   made from inside the procedure has its frame above it, and refuses on
   its own operands as any other call does.

   A failure leaves through the boundary in more than one step: the
   bracket around the procedure catches it, unwinds what it had open and
   raises it again, and so may a bracket further out, each re-raise
   arriving after the boundary has gone with the stopped context it sat
   under. So the walk marks the call beneath the boundary as it passes,
   and every later step reads the mark and leaves that call, and
   everything under it, alone.

   Two things stand between the stacks as they are and the state the
   outermost call found (PLRM 3.11.1 step 1): values the calls pushed
   and did not consume, dropped by going back to the shallowest depth
   any of them recorded, and operands they did consume, put back from
   the copies the outermost call took. Where a body consumed deeper
   than the copies reach, the stack keeps what the truncation leaves
   it, which is the whole of what an unwind could do before there were
   any copies.

   Every way an error is raised comes here: the interpreter's own, the
   one a PostScript body raises with signalerror, and stop, which is
   where all of them end and the only place a re-raise reaches. An
   operator leaves nothing behind however it failed, so no path may
   skip this. Several paths therefore run it over the same frames, and
   it is written to be read twice: it reads the copies and never spends
   them, and the frames are let go by whatever finally discards them.

   Returns 1 if a frame was found, having set ctx->currentobject to the
   operator it names. */
static int
_unwind_wrapped_calls(Xpost_Context *ctx)
{
    Xpost_Object fmark = xpost_bool_cons(0);
    Xpost_Object outrun = null; /* the outermost call's saved operands */
    int found = 0;
    int minos = 0, minds = 0, outos = 0;
    unsigned int cmdop = 0;
    /* Walk the exec stack top-down in a SINGLE pass over its
       segments -- O(depth), not the O(depth^2) that repeated
       topdown_fetch would cost. A deep stack at error time (a
       runaway or a cascading error handler) would otherwise make
       error handling itself the bottleneck. Stop at the nearer of
       the stopped context (a bool false) and the callout boundary;
       above it, each wrapped call's finish marker is followed,
       deeper, by its saved operands and its ds, os and opcode
       integers. */
    Xpost_Stack *esroot = xpost_stack_at(ctx->lo, ctx->es);
    Xpost_Stack *seg = esroot->prevseg
        ? xpost_stack_at(ctx->lo, esroot->prevseg) : esroot;
    int p = (int)seg->top - 1;
    int pending = 0; /* frame slots to read: 4->operands 3->ds 2->os 1->opcode */
    int sealing = 0; /* past a boundary, looking for the call under it */
    int fds = 0, fos = 0;
    Xpost_Object frun = null;

    for (;;)
    {
        Xpost_Object x;
        if (p < 0)
        {
            if (seg == esroot)
                break;
            seg = xpost_stack_at(ctx->lo, seg->prevseg);
            p = (int)seg->top - 1;
            continue;
        }
        x = seg->data[p];
        p--;
        if (sealing)
        {
            /* Past the boundary. The call under it is the one that
               handed control to the program, and it keeps what it
               consumed getting there -- not just for this walk but for
               every later step of the same failure, which arrives after
               the boundary itself has gone with the stopped context it
               sat under. Its marker is changed in place to say so, and
               the walk ends there: what is under it is under the
               program's procedure too. */
            if (xpost_object_get_type(x) == operatortype)
            {
                if (x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapsealed))
                    break; /* said already */
                if (x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapdone))
                {
                    seg->data[p + 1] = XPOST_OP(ctx, wrapsealed);
                    break;
                }
            }
            continue;
        }
        if (pending)
        {
            if (pending == 4)
                frun = x; /* an array of operands, or null for none */
            else if (xpost_object_get_type(x) != integertype)
            {
                pending = 0; /* malformed frame -- ignore it */
                continue;
            }
            else if (pending == 3)
                fds = (int)x.int_.val;
            else if (pending == 2)
                fos = (int)x.int_.val;
            else
            {
                if (!found)
                {
                    found = 1;
                    cmdop = (unsigned int)x.int_.val;
                    minos = fos;
                    minds = fds;
                }
                else
                {
                    if (fos < minos) minos = fos;
                    if (fds < minds) minds = fds;
                }
                /* the walk reads the innermost call first, so the last
                   frame it reads is the outermost: the call whose
                   caller is about to be handed the stack */
                outos = fos;
                outrun = frun;
            }
            --pending;
            continue;
        }
        if (xpost_dict_compare_objects(ctx, fmark, x) == 0)
            break; /* the coming stop unwinds to here */
        if (xpost_object_get_type(x) == operatortype)
        {
            if (x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapsealed))
                break; /* a call already known to be under a procedure */
            if (x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, calloutdone))
            {
                sealing = 1; /* the failure is the program's procedure's */
                continue;
            }
            if (x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapdone))
                pending = 4;
        }
    }
    if (found)
    {
        int oscount, dscount;

        ctx->currentobject = xpost_operator_cons_opcode(cmdop);
        oscount = xpost_stack_count(ctx->lo, ctx->os);
        while (oscount > minos)
        {
            (void)xpost_stack_pop(ctx->lo, ctx->os);
            --oscount;
        }
        if (xpost_object_get_type(outrun) == arraytype)
        {
            /* The copies are the operands the call was made with, the
               deepest of them at the depth outos - size. All of them go
               back, not only as many as are missing: a body that
               consumed two and pushed two again stands at the depth it
               started from holding values of its own. */
            int base = outos - (int)outrun.comp_.sz;

            if (oscount >= base)
            {
                while (oscount > base)
                {
                    (void)xpost_stack_pop(ctx->lo, ctx->os);
                    --oscount;
                }
                while (oscount < outos)
                {
                    if (!xpost_stack_push(ctx->lo, ctx->os,
                                          xpost_array_get(ctx, outrun,
                                                          oscount - base)))
                        break;
                    ++oscount;
                }
            }
        }
        dscount = xpost_stack_count(ctx->lo, ctx->ds);
        if (dscount > minds && minds >= 3)
        {
            ++ctx->namebind_gen; /* visibility changes */
            while (dscount > minds)
            {
                (void)xpost_stack_pop(ctx->lo, ctx->ds);
                --dscount;
            }
        }
    }
    return found;
}

/* The same unwinding, asked for from PostScript. An error a body
   raises with signalerror never reaches _onerror, and the hook that
   handles it wants the stacks unwound before it records them in
   $error, so it asks here. stop asks too, for the calls it abandons. */
int xpost_op_errorunwind(Xpost_Context *ctx)
{
    (void)_unwind_wrapped_calls(ctx);
    return 0;
}

/* called by mainloop() after propagated error codes.
   pushes postscript-level error procedures
   and resumes normal execution.
 */
static
void _onerror(Xpost_Context *ctx,
        unsigned int err)
{
    Xpost_Object sd;
    Xpost_Object ed;
    Xpost_Object handler;
    Xpost_Object dollarerror;

    if (err > unknownerror) err = unknownerror;

    /* An allocation refused for want of entity numbers is an
       implementation limit reached, not memory spent: the object field
       that carries an entity number is only so wide, while the memory
       behind the table has room and vmstatus reports it. PLRM Appendix
       B has limitcheck for exactly this and keeps VMerror for VM
       resources exhausted. The composite that was refused is a
       constructor's, several call frames below, so the distinction
       arrives here on the memory file rather than in the returned
       code. */
    if (err == VMerror && (ctx->lo->ent_exhausted || ctx->gl->ent_exhausted))
        err = limitcheck;

    /* Whatever the error, reporting it starts here and wants entities
       of its own: the error's name, $error's stack snapshots, the text
       the handler prints, and whatever the program's own handler makes
       of them. Open the numbers held back for that, in both memory
       files -- an error over one is reported with names and snapshots
       drawn from either. They shut themselves again at the first
       allocation the run makes with room to spare. */
    ctx->lo->ent_reserve_open = 1;
    ctx->gl->ent_reserve_open = 1;

    strncpy(ctx->run_error_name, errorname[err], sizeof ctx->run_error_name - 1);
    ctx->run_error_name[sizeof ctx->run_error_name - 1] = '\0';

    if (!validate_context(ctx))
        XPOST_LOG_ERR("context not valid");

    /* if a fault interrupts loading the graphics language into systemdict,
       restore systemdict to read-only so the writeable window never outlives
       the load. The window was opened by a write that backed systemdict up
       to any save level standing over the load, so shutting it takes no
       further backup and cannot be refused. */
    if (ctx->sysdict_unlocked)
    {
        xpost_object_set_access(ctx,
                xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0),
                XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
        ctx->sysdict_unlocked = 0;
        ctx->sysdict_load_done = 1;
    }

    if (itpdata->in_onerror > 5)
    {
        fprintf(stderr, "LOOP in error handler\nabort\n");
        ++ctx->quit;
        /*exit(undefinedresult); */
    }

    /* A runaway error cascade re-enters here without ever reaching `stop`
       (the error machinery raises before it can recover), so the nested
       `in_onerror` guard above -- reset on every completed pass -- never
       trips. Count consecutive handled errors instead and abort the job
       when they run away, turning an otherwise unbounded spin into a
       clean errored exit. xpost_op_stop clears the count on recovery. */
    if (++ctx->onerr_run > XPOST_ERROR_CASCADE_LIMIT)
    {
        fprintf(stderr, "runaway error cascade (%s)\nabort\n",
                errorname[err]);
        ctx->run_uncaught = 1;
        ++ctx->quit;
        return;
    }

    ++itpdata->in_onerror;

#ifdef EMITONERROR
    fprintf(stderr, "err: %s\n", errorname[err]);
#endif

    /* reset stack */
    if ((xpost_object_get_type(ctx->currentobject) == operatortype) &&
        ctx->opargsinhold)
    {
        int n = ctx->currentobject.mark_.pad0;
        int i;
        for (i = 0; i < n; i++)
        {
            xpost_stack_push(ctx->lo, ctx->os,
                    xpost_stack_bottomup_fetch(ctx->lo, ctx->hold, i));
        }
        /* the restored args carry the dispatcher's integer->real coercions;
           put back the integers the program actually pushed (PLRM 3.11) */
        for (i = 0; i < ctx->op_restore_n; i++)
        {
            int idx = ctx->op_restore_idx[i];
            /* the n arguments were just pushed back, so an index
               below n is one the stack now has */
            if (idx < n)
                XPOST_REFUSAL_IMPOSSIBLE(
                    xpost_stack_topdown_replace(ctx->lo, ctx->os, idx,
                                                ctx->op_restore_val[i]));
        }
    }

    (void)_unwind_wrapped_calls(ctx);

    /* printf("1\n"); */
    sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);

    /* printf("2\n"); */
    dollarerror = xpost_dict_get(ctx, sd, namedollarerror);
    if (xpost_object_get_type(dollarerror) == invalidtype)
    {
        XPOST_LOG_ERR("cannot load $error dict for error: %s",
                errorname[err]);
        xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, stop));
        /*itpdata->in_onerror = 0; */
        return;
    }

    /* printf("3\n"); */
    /* printf("4\n"); */
    /* printf("5\n"); */
    xpost_stack_push(ctx->lo, ctx->os, ctx->currentobject);

    ed = xpost_dict_get(ctx, sd, nameerrordict);
    handler = xpost_dict_get(ctx, ed, xpost_name_cons(ctx, errorname[err]));
    /* the handler runs when the interpreter schedules it, so it has to
       be executable: a literal is pushed on the operand stack and the
       error goes no further */
    if (xpost_object_get_type(handler) != invalidtype &&
        xpost_object_is_exe(handler) &&
        xpost_stack_push(ctx->lo, ctx->es, handler))
    {
        itpdata->in_onerror = 0;
        return;
    }

    /* errordict carries no handler for this error, or it cannot be
       scheduled. errordict is writable by design (PLRM 3.11.1: a program
       substitutes its own handlers there), and removing an entry is the
       program's own doing, but the error must still take effect: resuming
       as though the operator had succeeded is the one outcome the language
       does not allow.
       Raise it the way signalerror does instead -- the errorname beside
       the command already on the operand stack -- so $error records the
       error, its name and the stack snapshots exactly as a standard
       handler would, and the stop those handlers end with surfaces to any
       enclosing stopped context. */
    {
        Xpost_Object sig = xpost_dict_get(ctx, sd,
                                          xpost_name_cons(ctx, "signalerror"));

        XPOST_LOG_ERR("no errordict handler for /%s; raising it directly",
                      errorname[err]);
        if (xpost_object_get_type(sig) != invalidtype &&
            xpost_stack_push(ctx->lo, ctx->os,
                             xpost_object_cvlit(xpost_name_cons(ctx, errorname[err]))) &&
            xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvx(sig)))
        {
            itpdata->in_onerror = 0;
            return;
        }
        /* the error machinery itself is unusable: end the job rather
           than continue past the error */
        (void)xpost_op_stop(ctx);
    }

    /* printf("8\n"); */
    itpdata->in_onerror = 0;
}

/* --- scheduling, and running a program from C ------------------------
   Cooperative: a context runs until it yields, blocks or finishes, and
   the switch happens at one place so a context is validated once per
   switch rather than once per object. The nested run is the other
   direction -- C asking the interpreter to execute something, which the
   graphics machinery does constantly. */


/*
   select a new context to execute and return it
   scan for the next context in the C_RUN state
   along the way, change C_WAIT contexts to C_RUN
   to retry wait conditions.
 */
/* Whether any context other than the current one could run now.

   What a blocked read asks before deciding whether to wait. With nobody
   else to run, giving the read up would buy nothing and cost a pass
   through the scheduler, so the read waits where it is. The states
   counted are the ones the round-robin below will hand control to: a
   context waiting on a join or blocked on I/O is made runnable as it is
   passed, so it counts as something to run. */
static int _other_runnable(void)
{
    int i;

    /* Without the context operators there is only ever the running
       context, so there is nothing to give a read up for. */
    if (!itpdata || !xpost_dps_enabled())
        return 0;
    for (i = 0; i < MAXCONTEXT; i++)
    {
        const Xpost_Context *c = &itpdata->ctab[i];

        if (c->id == itpdata->cid)
            continue;
        if (c->state == C_RUN || c->state == C_IDLE
            || c->state == C_WAIT || c->state == C_IOBLOCK)
            return 1;
    }
    return 0;
}

static
Xpost_Context *_switch_context(Xpost_Context *ctx)
{
    unsigned int start, i;

    /* Scheduling is a Display PostScript feature. With the context
       operators uninstalled the table holds only the running context, so
       there is nothing to switch to and a default run keeps exactly the
       path it had: the running context, returned unchanged. */
    if (!xpost_dps_enabled())
        return ctx;

    /* Cooperative round-robin over the context table. A context runs until
       it yields, blocks on I/O, or returns from its top-level procedure --
       the model doc/xpost_design.dox describes and PLRM 2nd ed 7.1 permits
       (concurrency, not preemption). Starting one past the caller's slot
       and wrapping visits every context once with the caller last, so a
       lone runnable context keeps running.

       A context waiting for a join (C_WAIT) or blocked on I/O (C_IOBLOCK)
       is made runnable again as it is passed, so it re-checks its
       condition when next chosen: a joiner re-runs join and finds its
       child finished; a blocked read retries. This is a busy re-check, not
       a sleep -- correct, and cheap because a context reaches here only at
       a switch point, not between every object. A genuine deadlock (two
       contexts each waiting on the other) shows as a busy loop the
       mainloop's interrupt check breaks, which is as much as PLRM promises
       (it detects only the simplest deadlocks). Freed (C_FREE) and
       finished-and-unjoined (C_ZOMB) slots are not runnable and are passed
       over. */
    start = (ctx->id - 1) % MAXCONTEXT;
    for (i = 1; i <= MAXCONTEXT; i++)
    {
        Xpost_Context *c = &itpdata->ctab[(start + i) % MAXCONTEXT];
        if (c->state == C_WAIT || c->state == C_IOBLOCK)
            c->state = C_RUN;
        if (c->state == C_RUN || c->state == C_IDLE)
        {
            /* A context that freed itself did so while it was the one
               running, so what it was holding could not go with it then:
               the loop below it still had its execution stack to unwind.
               Here it has handed control to another context and will not
               be chosen again, which is where what it held goes and where
               its place in the context list comes free. */
            if (c != ctx && ctx->state == C_FREE)
                xpost_context_release(ctx);
            return c;
        }
    }

    /* Nothing runnable was found -- not even the caller, which has finished
       (C_ZOMB) or detached (C_FREE). Keep it current so the loop unwinds
       its now-empty execution stack and the run ends, rather than handing
       back a table slot that holds no live context. */
    return ctx;
}



/* Put a blocked operator back the way it was found.

   An operator that answers ioblock has done nothing: it read no byte and
   pushed no result. But the dispatcher has already taken its operands
   off the operand stack and into the hold, and eval has already taken
   the operator itself off the execution stack -- so leaving it there
   would lose both. Both go back, the operands the way an error puts them
   back (hold order, with the dispatcher's coercions undone), and the
   operator on top of the execution stack, where it will be the first
   thing this context runs when the scheduler comes back to it.

   The operand restore is conditional on the same flag an error's is: the
   hold holds this operator's arguments only while nothing else has
   called through the dispatcher since. */
static void _reexecute_current(Xpost_Context *ctx)
{
    int n;
    int i;

    /* Both halves or neither. An operator put back without its operands
       would run again against whatever the stack now holds, which is a
       different call and not the same one; so where the hold cannot be
       trusted to still carry this operator's arguments, nothing is put
       back. Nothing reaches here in that state -- the answer is only
       offered to a read whose arguments are in the hold -- and this says
       what would have to be true for it to be reachable. */
    if (xpost_object_get_type(ctx->currentobject) != operatortype
        || !ctx->opargsinhold)
        return;

    n = ctx->currentobject.mark_.pad0;
    for (i = 0; i < n; i++)
        xpost_stack_push(ctx->lo, ctx->os,
                xpost_stack_bottomup_fetch(ctx->lo, ctx->hold, i));
    for (i = 0; i < ctx->op_restore_n; i++)
    {
        int idx = ctx->op_restore_idx[i];

        /* the n arguments were just pushed back, so an index below n is
           one the stack now has */
        if (idx < n)
            XPOST_REFUSAL_IMPOSSIBLE(
                xpost_stack_topdown_replace(ctx->lo, ctx->os, idx,
                                            ctx->op_restore_val[i]));
    }

    xpost_stack_push(ctx->lo, ctx->es, ctx->currentobject);
}

/*
   the big main central interpreter loop.
   processes return codes from eval().
   The answer is one of XPOST_MAINLOOP_DONE, _YIELDED or _INVALID.
   yieldtocaller indicates `showpage` has been called using SHOWPAGE_RETURN semantics.
   ioblock indicates a blocked io operation.
   contextswitch indicates the `yield` operator has been called.
   all other values indicate an error condition to be returned to postscript.
 */
int mainloop(Xpost_Context *ctx)
{
    int ret;
    unsigned int evalcount = 0;

ctxswitch:
    ctx = _switch_context(ctx);
    itpdata->cid = ctx->id;
    /* MaxFontItem is maintained separately for each context (PLRM 8.2
       setcachelimit) while the glyph cache it governs is one store for the
       process. The context about to run writes its own ceiling through, so
       what the store admits an entry by is the setting of the context
       executing rather than of whichever context set it last. */
    (void) xpost_font_cache_setlimit(ctx->maxfontitem);

    /* the context's memory pointers are fixed for the life of a run;
       validate them once when a context becomes current rather than
       before every evaluation step */
    if (!validate_context(ctx))
        return XPOST_MAINLOOP_INVALID;

    while(!ctx->quit)
    {
        /* safe point: between evaluation steps every live object is
           reachable from the stacks, so a requested collection cannot
           sweep an operator's C-held intermediates.

           A collection that cannot mark its roots returns before its
           sweep, so it reclaims nothing, and the next one refuses in the
           same place: reclamation is over for the rest of the run. The
           run is told so here. PLRM 8.2 gives VMerror for an error in
           the virtual memory machinery, naming an internal error in the
           interpreter among its causes, and a marker that cannot read
           the root set is one. Carrying on instead spends fresh entity
           numbers until they run out, and reports that against whichever
           operator was allocating at the time. */
        if (_collection_wanted(ctx))
        {
            if (ctx->lo->garbage_collect_is_installed
                && ctx->lo->garbage_collect(ctx->lo, xpost_garbage_auto_banks(ctx), 1) < 0)
            {
                XPOST_LOG_ERR("collection abandoned before its sweep");
                _onerror(ctx, VMerror);
                continue;
            }
        }
        _compaction_wanted(ctx);
        if (ctx->gl && ctx->gl->blind_pending)
        {
            ctx->gl->blind_pending = 0;
            (void)xpost_vm_blind_measure(ctx);
        }
        /* a push the stack would not take, made somewhere other than
           inside an operator -- the dispatch answers for those itself.
           The object is on no stack and the step that pushed it carried
           on as though it were, so the run is told before the next step
           reads a stack it is not the depth of. */
        if (ctx->lo->push_refused)
        {
            ctx->lo->push_refused = 0;
            XPOST_LOG_ERR("a stack would not take a pushed object");
            _onerror(ctx, VMerror);
            continue;
        }
        if (_interrupt_pending)
        {
            /* an external interrupt request lands between operations */
            _interrupt_pending = 0;
            _onerror(ctx, interrupt);
            continue;
        }
        if ((++evalcount & 1023) == 0)
        {
            int over = _stack_ceilings(ctx);
            if (over)
            {
                _onerror(ctx, over);
                continue;
            }
        }
        ret = eval(ctx);
        if (ret)
            switch (ret)
            {
            case yieldtocaller:
                return XPOST_MAINLOOP_YIELDED;
            case collectretry:
                /* the operator did nothing and wants running again once
                   a collection has been taken. It goes back the way a
                   blocked one does; the collection it asked for is the
                   one this loop takes at its top, before the operator
                   runs again. */
                _reexecute_current(ctx);
                continue;
            case ioblock:
                _reexecute_current(ctx);
                ctx->state = C_IOBLOCK; /* fallthrough */
            case contextswitch:
                goto ctxswitch;
            default:
                _onerror(ctx, ret);
            }
    }

    return XPOST_MAINLOOP_DONE;
}

/* How deep a program may drive the nesting. Each level holds a C frame
   of this function and of whichever operator called in, so the bound is
   the C stack's, and it is set well below what the smallest stack the
   interpreter is built for can carry.

   Reaching it is limitcheck and not execstackoverflow. The execution
   stack is not what runs out: a level of nesting spends three entries of
   it against a ceiling of XPOST_EXEC_STACK_LIMIT, so a program told its
   execution stack had grown too large (PLRM 8.2, which points at the
   limit on the size of that stack) could read countexecstack and find
   it all but empty. What has been reached is an implementation limit of
   this interpreter's own, which is what limitcheck is for. */
#define XPOST_NEST_MAX 64

/* the error a procedure run from inside an operator failed with, named
   by the errorname the error machinery recorded */
static unsigned int _nested_error(Xpost_Context *ctx)
{
    Xpost_Object sd, ed, en;
    char *nm;
    unsigned int i;
    /* What a name this side cannot match comes back as. Some errors are
       raised only in PostScript -- undefinedresource is one, and a
       program may signal a name of its own -- so the walk below can fail
       to find a match on a run where nothing went wrong with the error
       machinery itself. unknownerror is what this interpreter says when
       it cannot be more specific (xpost_error.h), and errordict carries
       a handler for it; ioerror would be a claim about a device that was
       never touched. */
    unsigned int ret = unknownerror;

    sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    ed = xpost_dict_get(ctx, sd, xpost_name_cons(ctx, "$error"));
    if (xpost_object_get_type(ed) != dicttype)
        return ret;
    en = xpost_dict_get(ctx, ed, xpost_name_cons(ctx, "errorname"));
    if (xpost_object_get_type(en) != nametype)
        return ret;
    nm = xpost_string_allocate_cstring(ctx, xpost_name_get_string(ctx, en));
    if (!nm)
        return ret;
    for (i = 0; i <= (unsigned int)unknownerror; i++)
        if (strcmp(nm, errorname[i]) == 0)
        {
            ret = i;
            break;
        }
    free(nm);
    return ret;
}

/* Runs a procedure from inside an operator, with the operator still on
   the C stack waiting to push its own answer. */
int xpost_interpreter_run_nested(Xpost_Context *ctx, Xpost_Object P)
{
    int base;
    Xpost_Object saved;
    Xpost_Object caught;
    Xpost_Object held[XPOST_OPERATOR_MAX_SIG];
    int held_n;
    int i;
    int ret = 0;

    if (ctx->nest_depth >= XPOST_NEST_MAX)
        return limitcheck;

    base = xpost_stack_count(ctx->lo, ctx->es);

    /* The procedure runs under two frames the ordinary machinery reads.

       The boundary marks where a failure belongs: above it the failure
       is the procedure's, and an error restores the operand stack to
       what the procedure found (PLRM 3.11.1).

       The false beneath it is a stopped context, the same frame the
       stopped operator leaves. It bounds the unwinding of an error the
       procedure does not catch to this call, while the operator that
       made the call is still on the C stack with its own answer to
       push. The failure reaches the program as that operator's. */
    if (!xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, calloutdone)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_bool_cons(0)))
    {
        (void)xpost_stack_pop(ctx->lo, ctx->es);
        return execstackoverflow;
    }
    if (!xpost_stack_push(ctx->lo, ctx->es, P))
    {
        (void)xpost_stack_pop(ctx->lo, ctx->es);
        (void)xpost_stack_pop(ctx->lo, ctx->es);
        return execstackoverflow;
    }

    /* The hold stack holds the operands of the operator that called in,
       for the error machinery to give back if that operator fails. The
       procedure about to run reaches operators of its own, and each of
       them loads the hold stack with its own operands, so the caller's
       would be gone by the time they were wanted. They are kept here
       and put back, rather than the caller being told they are lost:
       the operator that called in is exactly the one an error in the
       procedure is going to be raised against. */
    {
        Xpost_Stack *h = xpost_stack_at(ctx->lo, ctx->hold);
        held_n = (int)h->top;
        if (held_n > (int)(sizeof held / sizeof *held))
            held_n = (int)(sizeof held / sizeof *held);
        for (i = 0; i < held_n; i++)
            held[i] = h->data[i];
    }

    saved = ctx->currentobject;
    ++ctx->nest_depth;
    while (xpost_stack_count(ctx->lo, ctx->es) > base && !ctx->quit)
    {
        if (_collection_wanted(ctx))
        {
            if (ctx->lo->garbage_collect_is_installed
                && ctx->lo->garbage_collect(ctx->lo, xpost_garbage_auto_banks(ctx), 1) < 0)
            {
                XPOST_LOG_ERR("collection abandoned before its sweep");
                _onerror(ctx, VMerror);
                continue;
            }
        }
        ret = eval(ctx);
        if (ret)
        {
            /* an operator wanting to run again after a collection is
               answered here rather than passed up: the collection it
               asked for is taken at the top of this loop too */
            if (ret == collectretry)
            {
                _reexecute_current(ctx);
                continue;
            }
            /* the run itself is ending, or moving to another context:
               neither is this procedure's to answer, so it goes back to
               the interpreter that can */
            if (ret == yieldtocaller || ret == contextswitch
                || ret == ioblock)
                break;
            _onerror(ctx, ret);
            ret = 0;
        }
    }
    --ctx->nest_depth;
    ctx->currentobject = saved;
    {
        Xpost_Stack *h = xpost_stack_at(ctx->lo, ctx->hold);
        h->prevseg = ctx->hold;
        for (i = 0; i < held_n; i++)
            h->data[i] = held[i];
        h->top = (unsigned int)held_n;
    }

    if (ret || ctx->quit)
    {
        /* the run was cut short with the frames still standing */
        while (xpost_stack_count(ctx->lo, ctx->es) > base)
            (void)xpost_stack_pop(ctx->lo, ctx->es);
        return ret;
    }

    /* the stopped context has answered by now, whether the procedure
       ran to its end or an error unwound to it */
    caught = xpost_stack_pop(ctx->lo, ctx->os);
    if (xpost_object_get_type(caught) != booleantype)
        return ioerror;
    if (caught.int_.val)
        return (int)_nested_error(ctx);
    return 0;
}





/*
   string constructor helper for literals
   sizeof("") is 1, ie. it includes the terminating \0 byte.
   our ps strings are counted and do not need (and should not have)
   a nul byte, or this byte may produce garbage output when printed.
 */
#define CNT_STR(s) sizeof(s) - 1, s

/* --- what a run was asked for ----------------------------------------
   Between the interpreter existing and the language being loaded: which
   device, which page, where the boot files are, what the invocation asked
   of the record and the band budget. All of it is this run's rather than
   the language's, which is why it lands in the host dictionary and not in
   systemdict. */

/*
   set global pagesize,
   initialize eval's jump-tabl
   allocate global itpdata interpreter instance
   call xpost_interpreter_init
        which initializes the first context
 */
static
int initalldata(const char *device)
{
    int ret;

    initevaltype();
    xpost_object_install_dict_get_access(xpost_dict_get_access);
    xpost_object_install_dict_set_access(xpost_dict_set_access);
    xpost_object_install_file_get_access(xpost_file_get_access);
    xpost_object_install_file_set_access(xpost_file_set_access);

    /* allocate the top-level itpdata data structure. */
    null = xpost_object_cvlit(null);
    itpdata = malloc(sizeof*itpdata);
    if (!itpdata)
    {
        XPOST_LOG_ERR("itpdata=malloc failed");
        return 0;
    }
    memset(itpdata, 0, sizeof*itpdata);

    /* allocate and initialize the first context structure
       and associated memory structures.
       populate OPTAB and systemdict with operators.
       push systemdict, globaldict, and userdict on dict stack
     */
    ret = xpost_interpreter_init(itpdata, device);
    if (!ret)
    {
        return 0;
    }


    return 1;
}

/* The devices a run may be started with, and the whole of what the
   interpreter accepts as a selection. Which of them this build can
   actually make is the boot files' answer -- a device whose driver the
   build left out registers no maker -- and a selection is held to that
   where the device is made. What is held here is that the name is one
   the interpreter knows at all, which is answerable before the language
   is loaded and is therefore answerable to the caller of xpost_create
   rather than to a run that has already begun.

   tests/check-device-roster.sh holds this list, the option parser's, the
   .devicemakers dictionary a page-device request is made from, and the
   roster the test wrappers run, to naming the same devices.

   The binary keeps its own list of what it can offer, which this one is
   not: this answers whether a string names a device at all, that answers
   which are built. tests/check-device-roster.sh holds them to agreeing,
   along with the .devicemakers dictionary and the roster the test
   wrappers run. */
static const char *const device_strings[] =
{
    "pgm",
    "ppm",
    "pbm",
    "tiff",
    "null",
    "bbox",
    "xcb",
    "gdi",
    "gl",
    "bgr",
    "raster",
    "record",
    "pdfwrite",
    "svgwrite",
    "dscwrite",
    "png",
    "pngalpha",
    "jpeg",
    NULL
};

/* The devices whose page may arrive a band at a time and which a record
   can be played into, so that selecting one selects banding.

   Kept here as well as in the recording class's own roster
   (.playtargets, data/recorddev.ps) because a selection is settled
   before any boot file is read: what a run is started with has to be
   answerable to the caller of xpost_create, and the roster is a
   dictionary that does not exist yet. tests/check-device-roster.sh holds
   the two to naming the same devices, so the duplication cannot drift.

   The names are the list stated in xpost_interpreter.h, expanded here as
   a table. The usage text expands the same list as a sentence, so the
   two cannot come to describe different fleets.

   The mode selector a run may write after a colon says which of the two
   ways such a page is held: banded whatever its size, or whole whatever
   its size. A selection naming neither is the one that is weighed. */
#define XPOST_BAND_ENTRY(name) name,
static const char *const bands_by_default[] =
{
    XPOST_BANDS_BY_DEFAULT(XPOST_BAND_ENTRY)
    NULL
};
#undef XPOST_BAND_ENTRY

/* The class a banded page is held by. It is a device of its own -- a
   page-device request may name it, and the boot files make it from the
   same roster of makers as any other -- so it is a selection a run may
   make directly. A run that does so gets the page held a band at a time
   and painted by the colour raster, which is what "ppm:band" selects,
   and it is kept for the runs that spell it that way.
   doc/MANUAL says so where it says how a run asks for banding. */
#define XPOST_RECORD_DEVICE "record"

/* The device that lends its page to whoever embedded the interpreter.
   The mode a selection of it carries is the arrangement that buffer is
   read back in, which is a roster the device itself keeps
   (xpost_raster_formats, src/lib/xpost_dev_raster.c). */
#define XPOST_RASTER_DEVICE "raster"

/* The two ways a page whose device can take it a band at a time may be
   held, as a run spells them after the colon.

   They are spelled as modes of the device rather than as options of
   their own because what they choose between is chosen by the device
   selection, and a run comparing the two ways wants to change one word
   rather than to add and remove a flag. The device is the head of the
   selection and the way its page is held is the tail, which is where
   every other selector in the tree keeps them. */
#define XPOST_WHOLE_PAGE_MODE "whole"
#define XPOST_BAND_MODE "band"

static const char *const banding_modes[] =
{
    XPOST_WHOLE_PAGE_MODE,
    XPOST_BAND_MODE,
    NULL
};

/* No mode at all, which is what most devices take: a mode says how a
   page is held or how it is read back, and a device that holds its page
   one way and hands it back one way has nothing for a colon to choose
   between. It is what the recording class takes too -- what a record
   plays into is the device the run selected, and a run selects that by
   naming the device and the band mode. */
static const char *const no_modes[] =
{
    NULL
};

static int _bands_by_default(const char *name, size_t n)
{
    int i;

    for (i = 0; bands_by_default[i]; i++)
        if (strlen(bands_by_default[i]) == n
            && strncmp(name, bands_by_default[i], n) == 0)
            return 1;
    return 0;
}

/* The modes a selection of this device may carry, which is the whole of
   what a colon after its name may spell.

   Two kinds of device declare one. A device that bands by default takes
   the two words that say how its page is held; and the raster device
   takes the arrangements it can lend its page back in, which it
   declares itself since it is the one that reads the name. Every other
   device takes none, which is what a device is answered to be unless it
   says otherwise: a device added to the roster refuses a mode until
   something here gives it one, rather than accepting every word until
   someone notices.

   Held at all because the answers differ in a direction nothing would
   report. A selection carrying a mode is one whose route is not
   weighed, so a mode nobody recognised would read as a run having
   asked for something specific and quietly turn the weighing off; and
   a lent page is read back by the arrangement its selection named, so
   an arrangement nobody recognised would hand a caller a page in one
   arrangement to be read in another. */
static const char *const *_device_modes(const char *selected, size_t n)
{
    if (strcmp(selected, XPOST_RASTER_DEVICE) == 0)
        return xpost_raster_formats;
    if (_bands_by_default(selected, n))
        return banding_modes;
    return no_modes;
}

static int _mode_taken(const char *const *modes, const char *mode)
{
    int i;

    for (i = 0; modes[i]; i++)
        if (strcmp(modes[i], mode) == 0)
            return 1;
    return 0;
}

/* The modes as a caller can be shown them, in as much of buf as they
   fit: a refusal that names what was given is only half of what the
   caller needs to fix it. */
static void _mode_roster(const char *const *modes, char *buf, size_t sz)
{
    size_t len = 0;
    int i;

    buf[0] = '\0';
    for (i = 0; modes[i]; i++)
    {
        size_t l = strlen(modes[i]);

        if (len + l + 2 > sz)
            break;
        if (len)
            buf[len++] = ' ';
        memcpy(buf + len, modes[i], l);
        len += l;
        buf[len] = '\0';
    }
}

/* Which device this run selected, without the mode selector a
   "device:mode" selection carries. Answers NULL for a name that is not a
   device, with n left holding the length of the name examined. */
static const char *_device_selected(const char *device, size_t *n)
{
    const char *colon;
    int i;

    *n = 0;
    if (!device)
        return NULL;
    colon = strchr(device, ':');
    *n = colon ? (size_t)(colon - device) : strlen(device);
    for (i = 0; device_strings[i]; i++)
        if (strlen(device_strings[i]) == *n
            && strncmp(device, device_strings[i], *n) == 0)
            return device_strings[i];
    return NULL;
}

/* What the interpreter is configured with before the language is read:
   the build's own facts, which are the same for every run of it. What
   this run decided goes elsewhere, with the rest of what a run decides,
   once the language is in place to hold it.

   The device selection is checked here rather than acted on. Making the
   device needs the device classes, which arrive with the graphics
   modules long after this; what can be answered now is whether the name
   is one the interpreter has, and answering it now is what lets a
   caller be told that its selection was not a device instead of
   watching a run fail. */
static
int setlocalconfig(Xpost_Context *ctx,
                   Xpost_Object sd,
                   const char *device)
{
    size_t n;

#ifndef _WIN32
    (void)ctx;
    (void)sd;
#endif

    {
        const char *selected = _device_selected(device, &n);
        const char *const *modes;
        const char *colon;

        if (!selected)
        {
            XPOST_LOG_ERR("unknown device %.*s", (int)n, device ? device : "");
            return undefined;
        }

        colon = strchr(device, ':');
        modes = _device_modes(selected, n);
        if (colon && !_mode_taken(modes, colon + 1))
        {
            char takes[64];

            /* What a caller is told is what it gave and what would have
               served instead, and the four readings below are the four
               things the word it gave can be. A device with a
               vocabulary shows it. A device with none is asked what the
               word was reaching for: the name of a device that bands
               spells a recorded page the other way round, and a word
               for holding a page is a word for something this device
               cannot do -- except at the recording class, which is
               where a banded page is held and so is told no more than
               that it takes no mode. */
            _mode_roster(modes, takes, sizeof takes);
            if (takes[0])
                XPOST_LOG_ERR("%d the %s device takes no mode \"%s\"; the"
                              " modes it takes are: %s", rangecheck, selected,
                              colon + 1, takes);
            else if (_bands_by_default(colon + 1, strlen(colon + 1)))
                XPOST_LOG_ERR("%d the %s device takes no mode; a page held a"
                              " band at a time and painted by the %s device"
                              " is selected as \"%s:%s\"", rangecheck,
                              selected, colon + 1, colon + 1,
                              XPOST_BAND_MODE);
            else if (_mode_taken(banding_modes, colon + 1)
                     && strcmp(selected, XPOST_RECORD_DEVICE) != 0)
                XPOST_LOG_ERR("%d the %s device does not take its page a band"
                              " at a time, so it takes no mode \"%s\"",
                              rangecheck, selected, colon + 1);
            else
                XPOST_LOG_ERR("%d the %s device takes no mode, and \"%s\" was"
                              " given", rangecheck, selected, colon + 1);
            return rangecheck;
        }
    }

#ifdef _WIN32
    {
        /* the flag is a name in systemdict, which is global, so it is
           built in global memory whatever the caller was allocating in */
        unsigned int vmmode = ctx->vmmode;
        int ret;

        ctx->vmmode = GLOBAL;
        ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "WIN32"),
                             xpost_bool_cons(1));
        ctx->vmmode = vmmode;
        if (ret)
            return ret;
    }
#endif

#ifndef HAVE_FREETYPE2
    {
        /* This build carries no face library: the font operators exist
           and answer as a font system with no faces would -- findfont
           and the font-program loaders refuse with invalidfont -- while
           procedural (Type 3) fonts still render through their build
           procedures. The name states which kind of build this is, so a
           program or a test harness can ask rather than read a refusal
           that a host with no fonts installed can also produce. Like
           WIN32 above, it is a name in systemdict, which is global. */
        unsigned int vmmode = ctx->vmmode;
        int ret;

        ctx->vmmode = GLOBAL;
        ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "NOFACES"),
                             xpost_bool_cons(1));
        ctx->vmmode = vmmode;
        if (ret)
            return ret;
    }
#endif

    return 0;
}

/* Where the boot files are, or the empty string where they are nowhere
   this looks. What is looked for is init.ps, which is the file the run
   would begin with and the one every other is reached from.

   The answer is one of the things a run settles rather than something
   true of the build, so a caller records it with the rest of them. It is
   asked for twice over: once by the load that runs those files, and once
   by the image of virtual memory, which stamps what it holds with what
   the files it was built out of say. Both must be asking about the same
   directory, so both ask here. */
/* A path with no backslashes in it. A path reaches PostScript as a
   string the boot files build a file name out of, and a backslash there
   begins an escape (PLRM 3.2.2). */
static void _forward_slashes(char *path)
{
#ifdef _WIN32
    while (*path)
    {
        if (*path == '\\')
            *path = '/';
        path++;
    }
#else
    (void)path;
#endif
}

/* Finds the directory the language is loaded from, by looking for
   init.ps in each candidate in turn and taking the first that has one.
   What the candidates are, and their order, is the paragraph of macro
   below. */
XPOST_TEST_VISIBLE void xpost_interpreter_data_dir(char *datadir,
                                                   size_t datadirsz)
{
    char path_init_ps[XPOST_PATH_MAX];
    struct stat statbuf;
    char *path;

    datadir[0] = '\0';

#define XPOST_PATH_INIT \
    do \
    { \
        snprintf(path_init_ps, sizeof(path_init_ps), "%s/init.ps", path); \
        if (stat(path_init_ps, &statbuf) == 0) \
        { \
            snprintf(datadir, datadirsz, "%s", path); \
            _forward_slashes(datadir); \
            return; \
        } \
        else \
            XPOST_LOG_DBG("init.ps not present in %s", path_init_ps); \
    } while (0)

    /* environment variable XPOST_DATA_DIR.

       The candidates below are places the interpreter looks of its own
       accord, and one of them not holding the boot files is ordinary --
       that is what having several is for. This one is different: it was
       named by whoever started the run, so a run that does not find the
       files there was told where to look and looked somewhere else in
       the end. Said out loud for that reason, and only for this
       candidate: an embedder that mis-sets it otherwise gets a working
       interpreter reading somebody else's boot files, with nothing
       anywhere saying which. The search still goes on, because the
       variable names the first place to try rather than the only one. */
    if ((path = getenv("XPOST_DATA_DIR")))
    {
        XPOST_PATH_INIT;
        /* Reported at the level a run shows by default, and not at the
           one below it, because a message nobody sees leaves this
           exactly as it was. The run is not stopped: what was wrong is
           the caller's belief about where the files are, and the
           interpreter can still start. Complaining and carrying on is
           what the program does with a -D definition it cannot store,
           for the same reason. */
        XPOST_LOG_ERR("XPOST_DATA_DIR names %s, which holds no init.ps;"
                      " looking in the places this build knows of instead",
                      path);
    }

    /* directory of the shared library (absent for an uninstalled build) */
    path = (char *)xpost_data_dir_get();
    if (path)
        XPOST_PATH_INIT;

    /* the data directory of the source tree this build was configured
       from. The candidate above reaches the tree by walking up from
       where the library sits, which supposes the build directory is
       inside the tree; a build directory is free to be anywhere, and
       this candidate is what holds for one that is not. It is a path on
       the machine the build was configured on, so on any other machine
       it is one more candidate that fails the stat and passes the
       search along -- which is why it sits after the library-relative
       candidate and before the installed one: an uninstalled build
       reads the tree it was built from in preference to whatever an
       earlier install left behind. */
#ifdef XPOST_SOURCE_DATA_DIR
    {
        static char x[] = XPOST_SOURCE_DATA_DIR;
        path = x;
    }
    XPOST_PATH_INIT;
#endif

#ifdef PACKAGE_DATA_DIR
    {
        static char x[] = PACKAGE_DATA_DIR;
        path = x;
    }
    XPOST_PATH_INIT;
#endif

    {
        static char x[] = "data";
        path = x;
    }
    XPOST_PATH_INIT;

    {
        static char x[] = "../data";
        path = x;
    }
    XPOST_PATH_INIT;

    {
        static char x[] = "../../data";
        path = x;
    }
    XPOST_PATH_INIT;

    XPOST_LOG_ERR("init.ps can not be found");
}
#undef XPOST_PATH_INIT

/* Where an image of virtual memory is read from, and where one is
   written to. Neither is an argument: an image is a property of an
   installation -- one file beside the boot files it was built out of --
   and every caller of the library gets it or does not without knowing
   it exists.

   A run that is told nothing finds one for itself, at the two places
   xpost_vm_image_default_path names, and writes one where it found
   none. That is what makes the image worth having: the saving belongs
   to whoever runs the interpreter rather than to whoever knew to
   arrange it. The environment still overrides both ends --
   XPOST_VM_IMAGE names one to read and XPOST_VM_IMAGE_WRITE one to
   write -- and XPOST_NO_VM_IMAGE names nothing and means it, turning
   both off for a run that wants the long way without moving any file.

   Only a quiet run reads or writes one. The boot files narrate their own
   loading, and a run that reads an image does no loading to narrate; the
   flag that silences that narration is itself part of what an image
   carries, so an image and the run reading it must agree about it. The
   census flag is part of what an image carries for the same reason -- it
   decides which entry points the lockdown keeps -- so it is named into
   the file, and the two kinds of run never read each other's. */
static const char *_image_read_path(int quiet, const char *datadir)
{
    static char found[XPOST_PATH_MAX];
    const char *path;

    if (!quiet || xpost_vm_image_refused() || getenv("XPOST_NO_VM_IMAGE"))
        return NULL;
    path = getenv("XPOST_VM_IMAGE");
    if (path && path[0])
        return path;
    return xpost_vm_image_default_path(found, sizeof(found), datadir, 0)
           ? found : NULL;
}

/* A run that read an image has nothing to write: what it would write is
   what it read. A run that built the language writes what it built, so
   that the next one does not have to. */
static const char *_image_write_path(const char *datadir)
{
    static char chosen[XPOST_PATH_MAX];
    const char *path = getenv("XPOST_VM_IMAGE_WRITE");

    if (path && path[0])
        return path;
    if (getenv("XPOST_NO_VM_IMAGE") || xpost_vm_image_in_use())
        return NULL;
    /* An image describes the language this build boots to, and a run
       given the device instrumentation does not boot to it: the classes
       are left open so that a test can reach in and say what is there.
       Writing that run's memory would leave every later run on the
       machine booting from an image whose devices were never closed --
       the seal gone, and nothing in the later run to say so. The
       instrumented run boots the long way and leaves the cache alone.

       The census run is the same case for the same reason: it keeps the
       entry points that report on the interpreter, which a run that did
       not ask for them leaves in no dictionary at all. An image written
       from it would hand those entry points to every later run on the
       machine, with nothing in the later run to say where they came
       from. The census run is not stopped from writing one, though --
       it is given a file of its own, named apart in _image_name, so the
       saving still belongs to whoever runs the tests. */
    if (getenv("XPOST_UNSEALED_DEVICES"))
        return NULL;
    return xpost_vm_image_default_path(chosen, sizeof(chosen), datadir, 1)
           ? chosen : NULL;
}

/* Written where the language stands complete; defined below, beside the
   step that reaches that point. */
static void _write_image(Xpost_Context *ctx, const char *datadir);

/* How many contexts this process has created. An image is written from
   the first and only from the first: the file describes the language
   this build boots to, which is one thing and is written once, so a
   process that has already written it spends nothing writing it again
   from every context after. */
static unsigned int _contexts_created = 0;

/* Say that the boot files may be read, whichever way the language
   arrives. init.ps is read from here now and callout.ps lazily from the
   same directory, so a later sandbox must not deny the interpreter its
   own start-up files -- and a run whose language came out of an image
   reaches the same directory for whatever it did not carry. */
void xpost_interpreter_permit_data_dir(const char *datadir)
{
    if (datadir[0])
        xpost_path_permit_read(datadir);
}

/* --- loading the language --------------------------------------------
   Reading the boot files, or reading back an image of the memory they
   would have built. Both end at the same place: the language complete, the
   machinery locked down, and no device made yet. */

/*
   load init.ps (which also loads err.ps) while systemdict is writeable
   ignore invalidaccess errors.
 */
static
void loadinitps(Xpost_Context *ctx, const char *datadir)
{
    char buf[1024];
    char path_init_ps[XPOST_PATH_MAX];
    int n;

    assert(ctx->gl->base);
    if (!datadir[0])
        return;
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, quit));
    /* a data directory long enough to leave no room for the file's name
       beside it names no file, so the load is not attempted with a name
       that was cut to fit */
    if (snprintf(path_init_ps, sizeof(path_init_ps), "%s/init.ps", datadir)
        >= (int)sizeof(path_init_ps))
    {
        XPOST_LOG_ERR("the data directory %s leaves no room for the name of"
                      " the file to load beside it", datadir);
        return;
    }

    _forward_slashes(path_init_ps);
    n = snprintf(buf, sizeof(buf),
                 "(%s) (r) file cvx "
                 "/DATA_DIR (%s) def exec ", path_init_ps, datadir);
    xpost_stack_push(ctx->lo, ctx->es,
                     xpost_object_cvx(xpost_string_cons(ctx, n, buf)));

    ctx->quit = 0;
    /* mainloop answers that it is done, that it yielded, or that the
       context did not validate. The context was built and every part of
       it checked before this is reached, which is what the assertion
       above reads; and a yield is the returntocaller operator's answer,
       which only showpage under return semantics reaches and init.ps
       paints no page. Running the start-up file leaves done the only
       answer. */
    mainloop(ctx);
}


/* What a run settles, and the whole of it.

   The interpreter's dictionaries hold the language: the same names with
   the same values however the interpreter was started. A few values are
   not like that. The directory the boot files were found in, the
   directories a resource search covers, whether there is a user at the
   other end of standard input, what a page does when it ends and where
   it goes are decided afresh on every launch, by the command line, the
   environment, the caller, or the state of the process. Those live in a
   dictionary of their own -- .hostdict, a member of the private global
   namespace -- so that whether a value is the same for every run of this
   build is answered by which dictionary holds it rather than by knowing
   what the name means.

   Every name here is written on every launch and written from here. One
   the host has nothing to say about is written as a null rather than
   left out, so nothing a run reads under one of these names can have
   been settled by anything other than this run.

   tests/host_settings.golden registers the set, and
   tests/check-host-settings.sh holds this table, the register and the
   readers among the boot files to one another. */
static const char *const host_settings[] =
{
    "DATA_DIR",
    ".resourcepath",
    ".interactive",
    "ShowpageSemantics",
    "StartDevice",
    "StartDeviceAsked",
    "StartPageSize",
    "SUBDEVICE",
    "RecordSpill",
    "MaxBandBytes",
    "OutputFileName",
    "OutputBufferIn",
    "OutputBufferOut",
    NULL
};

/* Where this run wants a retained page's marks held: weighed against
   what banding the page saves, never in a file, or in one from the first
   mark. It is a property of the machine and of what the caller is
   willing to spend rather than of the page description, so it is
   settled before the context exists and read into the settings below.

   PLRM Appendix G bars a page description from choosing how its marks
   are held, so this is not a page-device parameter and must not become
   one. What a program may do is be told; currentsystemparams reports
   both this and what became of it.

   Kept for the process rather than passed to xpost_create, the way a
   run's refusal to read a virtual memory image is: it is asked for
   before the context is made and read as the context is made. */
static const char *_record_spill = "auto";

XPAPI int
xpost_record_spill_set(const char *state)
{
    if (!state)
        return 0;
    if (strcmp(state, "auto") && strcmp(state, "never")
        && strcmp(state, "always"))
        return 0;
    _record_spill = state;
    return 1;
}

/* What one band of a page may cost this run, in bytes of raster held at
   once, and nought where this run named nothing. The recording class
   carries the budget a run that names none works to, and states what
   that number is for (data/recorddev.ps); this is where a run puts
   another in its place, so there is one number in the tree and one place
   it is argued for.

   Settled by whoever starts the run and not by the page description, for
   the reason above it: PLRM Appendix G bars a page description from
   choosing how its marks are held. currentsystemparams reports the
   budget in force as MaxBandBytes and the band it bought as
   CurBandHeight.

   Kept for the process rather than passed to xpost_create, the way the
   state above is. */
static long _band_bytes = 0;

XPAPI int
xpost_band_bytes_set(long bytes)
{
    if (bytes < 1 || bytes > XPOST_BAND_BYTES_MAX)
        return 0;
    _band_bytes = bytes;
    return 1;
}

/* The dictionary those settings live in. The namespace holding it is
   sealed read-only once the language is loaded, and the seal is shallow,
   so writing into this member goes on working for as long as the context
   does -- which is what lets a setting be written afresh for each run
   rather than only at start-up. */
static Xpost_Object _host_dict(Xpost_Context *ctx)
{
    Xpost_Object store;

    if (xpost_object_get_type(ctx->globalprivatedict) != dicttype)
        return null;
    /* Through the job store: the machinery's writable state is gathered
       there, so the namespace its procedures live in holds nothing a
       program could write (data/init.ps). */
    (void)store;
    return xpost_context_job_member(ctx, ".hostdict");
}

/* Write one setting, answering the refusal so the caller can report it:
   a setting that did not go in is one whose reader answers with whatever
   stood under the name before. */
static int _host_put(Xpost_Context *ctx, const char *name, Xpost_Object value)
{
    Xpost_Object h = _host_dict(ctx);

    if (xpost_object_get_type(h) != dicttype)
        return undefined;
    return xpost_dict_put(ctx, h, xpost_name_cons(ctx, name), value);
}

/* What stands under a setting's name now. */
static Xpost_Object _host_get(Xpost_Context *ctx, const char *name)
{
    Xpost_Object h = _host_dict(ctx);

    if (xpost_object_get_type(h) != dicttype)
        return null;
    return xpost_dict_get(ctx, h, xpost_name_cons(ctx, name));
}

/* Whether a value already under a name is somewhere this run's own
   value can be put without making anything.

   Every setting is written on every launch, and a launch whose language
   came out of an image is writing over what the run that wrote the image
   left. Where the two runs settled the same thing, virtual memory has to
   come out where it went in -- an image read and written back must be
   the image that was read -- so a value of the same kind and the same
   size is filled in rather than replaced. It is reached only through the
   name it is under, and the name is about to hold this run's answer
   either way. */
static int _reusable(Xpost_Context *ctx, const char *name,
                     Xpost_Object_Type type, unsigned int sz,
                     Xpost_Object *had)
{
    *had = _host_get(ctx, name);
    return xpost_object_get_type(*had) == type
           && (*had).comp_.sz == sz
           && (*had).comp_.off == 0;
}

/* Write a setting whose value is text the caller gave. Nothing said is
   written as a null, so a reader tells a setting the host declined to
   make from one it made empty. */
static int _host_put_string(Xpost_Context *ctx, const char *name,
                            const char *text)
{
    Xpost_Object o;
    unsigned int len;

    if (!text)
        return _host_put(ctx, name, null);
    /* the text becomes a string, which counts its length in a field
       narrower than a path or a device selector may be */
    if (strlen(text) > (size_t)XPOST_OBJECT_COMP_MAX_SZ)
        return limitcheck;
    len = (unsigned int)strlen(text);
    if (_reusable(ctx, name, stringtype, len, &o))
    {
        char *p = xpost_string_get_pointer(ctx, o);

        if (!p)
            return VMerror;
        memcpy(p, text, len);
        return 0;
    }
    o = xpost_object_cvlit(xpost_string_cons(ctx, len, text));
    if (xpost_object_get_type(o) != stringtype)
        return VMerror;
    /* What the host said this run was started with is a constant the
       machinery reads to decide: where the pages go, where the boot
       files are. A program reaches it -- the output file's name is
       copied onto the device, and the template graphics state names that
       device -- so it is closed to writing where it is made. The
       interpreter fills it through a pointer, which access does not
       stand in the way of. */
    o = xpost_object_set_access(ctx, o, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
    return _host_put(ctx, name, o);
}

/* Write a setting that carries one of the caller's pointers across to
   the device that uses it. The pointer is the caller's and no business
   of a program, so the string it travels in is closed to one. */
static int _host_put_pointer(Xpost_Context *ctx, const char *name,
                             const void *p, size_t sz)
{
    Xpost_Object o;
    char *data;

    if (!p)
        return _host_put(ctx, name, null);
    if (!_reusable(ctx, name, stringtype, (unsigned int)sz, &o))
    {
        o = xpost_object_cvlit(xpost_string_cons(ctx, (unsigned int)sz, NULL));
        if (xpost_object_get_type(o) != stringtype)
            return VMerror;
        xpost_object_set_access(ctx, o, XPOST_OBJECT_TAG_ACCESS_NONE);
    }
    data = xpost_string_get_pointer(ctx, o);
    if (!data)
        return VMerror;
    memcpy(data, &p, sz);
    return _host_put(ctx, name, o);
}

/* Settle what this run's host decides, once the language is in place to
   be asked. Every name in the table is written here, on every launch:
   one the host has nothing to say about is written as a null rather than
   left out, so nothing a run reads under one of these names can have
   been settled by anything other than this run.

   What the settings are made of goes in global memory. They are settled
   before the program runs and are read for the whole life of the
   context, so they must outlive the restores that end a job; local
   memory would revert them with the job that happened to be running. */
static int _record_host_config(Xpost_Context *ctx,
                               const char *datadir,
                               const char *device,
                               const char *outfile,
                               const char *bufferin,
                               char **bufferout,
                               Xpost_Showpage_Semantics semantics,
                               Xpost_Set_Size set_size,
                               int width,
                               int height)
{
    unsigned int vmmode;
    const char *subdevice;
    const char *selected;
    /* the device a selection given banding was made for, which is the
       device a record made from it paints through */
    const char *banded = NULL;
    Xpost_Object o;
    size_t n;
    int ret;
    int i;

    vmmode = ctx->vmmode;
    ctx->vmmode = GLOBAL;

    if ((ret = _host_put_string(ctx, "DATA_DIR", datadir[0] ? datadir : NULL)))
        goto done;

    /* The directories a resource search covers, empty until the host
       names one. It is data rather than a procedure, so it is literal:
       an executable array would be run instead of read when the path is
       walked. */
    if (!_reusable(ctx, ".resourcepath", arraytype, 0, &o))
    {
        o = xpost_object_cvlit(xpost_array_cons(ctx, 0));
        if (xpost_object_get_type(o) != arraytype)
        {
            ret = VMerror;
            goto done;
        }
    }
    if ((ret = _host_put(ctx, ".resourcepath", o)) != 0)
        goto done;

    /* Whether there is a user at the other end of this run is settled
       per run rather than per launch, by _record_session_kind; a launch
       that never starts one answers as a run with nobody there. */
    if ((ret = _host_put(ctx, ".interactive", xpost_bool_cons(0))) != 0)
        goto done;

    /* What showpage does when a page ends, as the context was created
       with: pause, carry on, or hand control back to the caller. */
    if ((ret = _host_put(ctx, "ShowpageSemantics",
                         xpost_int_cons(semantics))) != 0)
        goto done;

    /* Which device this run was started with, and the page it was
       started at. The boot files make the device from these once the
       language stands, and the start-up report names them when it could
       not be made. The device is a name, being the key the roster of
       makers is kept under; the page is in device pixels, which is what
       a maker takes and what -g gives. */
    selected = _device_selected(device, &n);
    if (!selected)
    {
        /* the selection was held to being a device the interpreter has
           before the language was read; a context that reached here
           without one is one nothing can make a device for */
        ret = undefined;
        goto done;
    }
    /* A device that can take its page a band at a time is given to the
       recording class to paint, which is what makes banding what a run
       gets without asking for it.

       Given to it, not made through it: which of the two routes a page
       actually takes is settled where the device is made, by weighing
       the page's raster against the band budget, and a page the budget
       covers is painted on the device named here with nothing recorded
       at all (.devicefor, data/recorddev.ps). It cannot be settled here.
       What a row of a device's raster costs is stated by the device's
       class and the budget by the recording class, and neither class
       exists yet: the graphics modules are read when a run begins, which
       is after the context this is settling has been handed back. So
       what is settled here is which device a record would paint through
       if one is made, and the weighing waits for the classes that carry
       the two numbers.

       A selection naming the mode that holds the page whole is left
       alone, which is how a run says otherwise: "pgm:whole" is made on
       the device named and records nothing at any page. "pgm:band" is
       given to the recording class like the bare selection and, naming
       the mode, is not weighed once it gets there. Naming the mode
       rather than adding an option is what lets the three be compared by
       changing one word. */
    subdevice = device ? strchr(device, ':') : NULL;
    if (_bands_by_default(selected, n)
        && (!subdevice
            || strcmp(subdevice + 1, XPOST_BAND_MODE) == 0))
    {
        banded = selected;
        selected = XPOST_RECORD_DEVICE;
    }

    /* literal: the name is data here, and an executable one would be
       looked up rather than read wherever the maker roster is keyed by
       it */
    if ((ret = _host_put(ctx, "StartDevice",
                         xpost_object_cvlit(xpost_name_cons(ctx, selected))))
        != 0)
        goto done;
    /* The device the run asked for, which is the one to name back at it.
       A selection given banding is made through another device, and a
       caller told about a device it never named is being told about a
       choice made on its behalf. */
    if ((ret = _host_put(ctx, "StartDeviceAsked",
                         xpost_object_cvlit(
                             xpost_name_cons(ctx, banded ? banded : selected))))
        != 0)
        goto done;
    if (!_reusable(ctx, "StartPageSize", arraytype, 2, &o))
    {
        o = xpost_object_cvlit(xpost_array_cons(ctx, 2));
        if (xpost_object_get_type(o) != arraytype)
        {
            ret = VMerror;
            goto done;
        }
    }
    if (set_size != XPOST_USE_SIZE)
    {
        /* the page a caller that named none is started at (PLRM 6.2.6
           gives the same as the standard PageSize, in points, and a
           device the run names no resolution for is at one pixel to the
           point) */
        width = 612;
        height = 792;
    }
    if ((ret = xpost_array_put(ctx, o, 0, xpost_int_cons(width))) != 0)
        goto done;
    if ((ret = xpost_array_put(ctx, o, 1, xpost_int_cons(height))) != 0)
        goto done;
    if ((ret = _host_put(ctx, "StartPageSize", o)) != 0)
        goto done;

    /* The mode selector of a "device:mode" selection, as the run wrote
       it: the raster device reads it for a pixel format, and the
       weighing reads it for the word that says to band whatever the page
       (.devicefor, data/recorddev.ps). Which device a record plays into
       is not here -- that is the device the run asked for, above.

       The mode that holds the page whole is consumed here rather than
       passed on: it is answered by leaving the selection on the device
       it names, so by this point it has been acted on and the device it
       reaches has no use for it. */
    if (subdevice
        && strcmp(subdevice + 1, XPOST_WHOLE_PAGE_MODE) == 0)
    {
        if ((ret = _host_put_string(ctx, "SUBDEVICE", NULL)) != 0)
            goto done;
    }
    else if ((ret = _host_put_string(ctx, "SUBDEVICE",
                                     subdevice ? subdevice + 1 : NULL)) != 0)
        goto done;

    /* Where a retained page's marks are held. A name, being one of three
       words and read back as one; literal, because a name holding a
       procedure would be run rather than read wherever it is looked at. */
    if ((ret = _host_put(ctx, "RecordSpill",
                         xpost_object_cvlit(
                             xpost_name_cons(ctx, _record_spill)))) != 0)
        goto done;

    /* What one band of a page may cost. A null where this run named no
       budget: the number such a run works to is the recording class's
       own (data/recorddev.ps), and a copy of it written here would agree
       with it only until one of the two was changed. The boot files put
       that class's number here as the run's device is made, so what a
       reader finds afterwards is the budget in force either way
       (.settlebandbudget, data/device.ps); the null stands where the
       graphics never loaded and there is no such class. */
    if ((ret = _host_put(ctx, "MaxBandBytes",
                         _band_bytes
                             ? xpost_int_cons((integer)_band_bytes)
                             : null)) != 0)
        goto done;

    /* Where this run's pages go when the page device names nothing and
       the program has bound no name of its own. */
    if ((ret = _host_put_string(ctx, "OutputFileName", outfile)) != 0)
        goto done;

    /* The framebuffer an embedding caller lends the raster device, and
       the place such a caller wants the finished one written back to. */
    if ((ret = _host_put_pointer(ctx, "OutputBufferIn",
                                 bufferin, sizeof(bufferin))) != 0)
        goto done;
    if ((ret = _host_put_pointer(ctx, "OutputBufferOut",
                                bufferout, sizeof(bufferout))) != 0)
        goto done;

    /* Every name the table holds is written by one of the branches
       above, and one that is not would answer a reader with whatever
       some other run left under it. Written as a null here rather than
       cleared before the branches run: the branches fill a value of the
       right shape in where they find one, and a clearing pass would have
       taken it away first. */
    for (i = 0; host_settings[i]; i++)
        if (!xpost_dict_known_key(ctx, ctx->gl, _host_dict(ctx),
                                  xpost_name_cons(ctx, host_settings[i])))
        {
            XPOST_LOG_ERR("this run settled nothing under %s",
                          host_settings[i]);
            if ((ret = _host_put(ctx, host_settings[i], null)) != 0)
                goto done;
        }

done:
    ctx->vmmode = vmmode;
    return ret;
}

/* --- the run, and the jobs in it -------------------------------------
   xpost_create makes a context, xpost_run executes something in it, and
   between jobs the context is put back to a baseline so that nothing one
   job did reaches the next. The revert is a whole-VM image restore rather
   than a save level: total, and unable to fail part way. */

/* Name the standard local dictionaries in systemdict. systemdict is global, so
   holding a reference to a local dictionary would be an invalidaccess; the PLRM
   sanctions exactly this exception (section 3.7.2), naming userdict, errordict,
   $error and FontDirectory in systemdict so a program reaches each by name. The
   ignoreinvalidaccess window is isolated to these puts; the rest of the
   interpreter, initialisation included, obeys the local/global rule. */
static int copyudtosd(Xpost_Context *ctx, Xpost_Object ud, Xpost_Object sd)
{
    Xpost_Object ed, de, fd, st, sv;
    int ret;

    ctx->ignoreinvalidaccess = 1;
    ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "userdict"), ud);
    if (ret)
        goto done;
    ed = xpost_dict_get(ctx, ud, xpost_name_cons(ctx, "errordict"));
    if (xpost_object_get_type(ed) == invalidtype)
    {
        ret = undefined;
        goto done;
    }
    ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "errordict"), ed);
    if (ret)
        goto done;
    de = xpost_dict_get(ctx, ud, xpost_name_cons(ctx, "$error"));
    if (xpost_object_get_type(de) == invalidtype)
    {
        ret = undefined;
        goto done;
    }
    ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "$error"), de);
    if (ret)
        goto done;
    /* FontDirectory is likewise a name in systemdict for a local dictionary
       (PLRM). It exists in userdict by the time this runs. */
    fd = xpost_dict_get(ctx, ud, xpost_name_cons(ctx, "FontDirectory"));
    if (xpost_object_get_type(fd) == dicttype)
    {
        ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "FontDirectory"), fd);
        if (ret)
            goto done;
        /* GlobalFontDirectory and its older name SharedFontDirectory are a
           different, global dictionary holding only the fonts defined while
           the allocation mode was global; the boot file defines them into
           systemdict itself, being global and so permitted to. Keep both
           directories to hand: the name FontDirectory is rebound to one or
           the other as the allocation mode changes (PLRM), and setglobal
           does that without having to look either up. */
        ctx->localfontdir = fd;
        ctx->globalfontdir = xpost_dict_get(ctx, sd,
                                 xpost_name_cons(ctx, "GlobalFontDirectory"));
    }
    /* statusdict and serverdict are local dictionaries a program mutates, so
       save/restore isolates a job's changes; systemdict names them (PLRM). */
    st = xpost_dict_get(ctx, ud, xpost_name_cons(ctx, "statusdict"));
    if (xpost_object_get_type(st) == dicttype)
    {
        ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "statusdict"), st);
        if (ret)
            goto done;
    }
    sv = xpost_dict_get(ctx, ud, xpost_name_cons(ctx, "serverdict"));
    if (xpost_object_get_type(sv) == dicttype)
    {
        ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "serverdict"), sv);
        if (ret)
            goto done;
    }
    ret = 0;
    /* the window in which a global dictionary may name a local one closes
       here, on every path out */
done:
    ctx->ignoreinvalidaccess = 0;
    return ret;
}


/*
   create an executable context using the given device,
   output configuration, and semantics.
 */
XPAPI Xpost_Context *xpost_create(const char *device,
                                  Xpost_Output_Type output_type,
                                  const void *outputptr,
                                  Xpost_Showpage_Semantics semantics,
                                  Xpost_Output_Message output_msg,
                                  Xpost_Set_Size set_size,
                                  int width,
                                  int height)
{
    Xpost_Context *ctx;
    Xpost_Object sd, ud;
    char datadir[XPOST_PATH_MAX];
    const char *image_path;
    int built;
    int ret;
    const char *outfile = NULL;
    const char *bufferin = NULL;
    char **bufferout = NULL;
    int quiet;

    /* One interpreter instance at a time. itpdata is the whole dynamic
       state -- the context table, both memory-file tables and the
       identity of the running context -- so a second instance would take
       the place of the live one, leaving every handle already handed out
       pointing into memory the interpreter no longer reaches. The
       multiple contexts the interpreter is built around live in this one
       instance's context table; they are not had by creating another. */
    if (itpdata)
    {
        XPOST_LOG_ERR("an interpreter instance is already live");
        return NULL;
    }

    switch (output_msg)
    {
        case XPOST_OUTPUT_MESSAGE_QUIET:
            quiet = 1;
            _xpost_interpreter_is_tracing = 0;
            break;
        case XPOST_OUTPUT_MESSAGE_VERBOSE:
            quiet = 0;
            _xpost_interpreter_is_tracing = 0;
            break;
        case XPOST_OUTPUT_MESSAGE_TRACING:
            quiet = 0;
            _xpost_interpreter_is_tracing = 1;
            break;
        default:
            XPOST_LOG_ERR("Wrong output message value");
            return NULL;;
    }


    switch (output_type)
    {
        case XPOST_OUTPUT_FILENAME:
            outfile = outputptr;
            break;
        case XPOST_OUTPUT_BUFFERIN:
            bufferin = outputptr;
            break;
        case XPOST_OUTPUT_BUFFEROUT:
            bufferout = (char **)outputptr;
            break;
        case XPOST_OUTPUT_DEFAULT:
            break;
    }


    nextid = 0; /*reset process counter */
    _contexts_created++;

    /* The terms the interpreter's own structures are built under: the
       collector neither runs nor counts what is allocated, and the
       access checks a program's writes go through are not applied to
       structures nothing has yet been able to reach.

       Per context and not per process. Every context is brought up by
       the same steps and arrives at the same virtual memory, and it
       does so only where each is built under the same terms: a context
       built with the accounting live spends a collection budget on
       start-up that a context built without it still holds, which is a
       difference between two contexts whose memory is otherwise
       identical.

       The exemption is given back below, where the language begins to
       load and the first thing that could reach these structures is
       about to run -- and on every way out between here and there, so
       that a creation which does not finish leaves the process as it
       found it. */
    xpost_interpreter_set_initializing(1);

    /* Allocate and initialize all interpreter data structures. */
    ret = initalldata(device);
    if (!ret)
    {
        xpost_interpreter_set_initializing(0);
        return NULL;
    }

    /* the context the initialisation just built: one interpreter instance
       at a time, so it is the first slot of the table */
    ctx = &itpdata->ctab[0];

    /* extract systemdict and userdict for additional definitions */
    sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    ud = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 2);

    ret = setlocalconfig(ctx, sd, device);
    if (ret)
    {
        XPOST_LOG_ERR("%s recording the interpreter's configuration",
                      errorname[ret]);
        xpost_interpreter_set_initializing(0);
        return NULL;
    }

    /* A run that has asked to instrument the interpreter says so here.
       The device classes are sealed at the lockdown, which is what stops
       a program putting its own method where the machinery will run it;
       a run measuring the machinery -- counting the bands a page was put
       out in, watching what a class does before any device is made from
       it -- cannot do that through a sealed class and has no other way
       in, the private namespaces being closed to programs. So the seal
       is what this run gives up, and only this run: the setting is the
       invocation's, not the program's, and a program cannot reach it.
       Handed through systemdict for the same reason QUIET is, and moved
       into the private dictionary beside it. */
    if (getenv("XPOST_UNSEALED_DEVICES"))
    {
        ret = xpost_dict_put(ctx, sd,
                             xpost_name_cons(ctx, "UNSEALEDDEVICES"),
                             xpost_bool_cons(1));
        if (ret)
        {
            XPOST_LOG_ERR("%s naming UNSEALEDDEVICES in systemdict",
                          errorname[ret]);
            xpost_interpreter_set_initializing(0);
            return NULL;
        }
    }

    ctx->quiet = quiet;
    if (quiet)
    {
        /* Hand the quiet flag to the boot code through systemdict -- the only
           dictionary that exists this early. init.ps relocates QUIET into the
           private .internaldict as soon as that dictionary is built, so the
           load-time banner guards read it through a frozen reference and a
           program can neither see nor shadow it. */
        ret = xpost_dict_put(ctx,
                             sd /*xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0)*/ ,
                             xpost_name_cons(ctx, "QUIET"),
                             xpost_bool_cons(1));
        if (ret)
        {
            XPOST_LOG_ERR("%s naming QUIET in systemdict", errorname[ret]);
            xpost_interpreter_set_initializing(0);
            return NULL;
        }
    }

    /* And the census flag, handed in the same way and for the same reason.
       The registers and the tests that take them ask the interpreter to
       report on itself -- how many names were looked up, what a walk cost,
       how many entries a bank holds -- and no boot file names any of it. The
       lockdown keeps those entry points only where this says they were asked
       for; a run that did not ask leaves them in no dictionary at all, which
       is where a detail a program has no use for belongs. */
    if (getenv("XPOST_CENSUS"))
    {
        ret = xpost_dict_put(ctx, sd,
                             xpost_name_cons(ctx, "CENSUS"),
                             xpost_bool_cons(1));
        if (ret)
        {
            XPOST_LOG_ERR("%s naming CENSUS in systemdict", errorname[ret]);
            xpost_interpreter_set_initializing(0);
            return NULL;
        }
    }

    xpost_stack_clear(ctx->lo, ctx->hold);
    xpost_interpreter_set_initializing(0);

    xpost_interpreter_data_dir(datadir, sizeof(datadir));
    xpost_interpreter_permit_data_dir(datadir);

    /* The language: read whole out of an image of the virtual memory a
       run of this build already built it in, or built here by running
       the boot files, as every run did before there were images. The two
       arrive at the same place, so what follows a build -- the userdict
       names copied across and the seal on systemdict -- is done only
       where the language was built, the image having been written after
       them.

       Every way an image can be unusable answers that it was not read,
       and the boot files are what happens then. So a missing, stale,
       damaged or foreign image costs a run the time it would have saved
       and nothing else. */
    /* Which language to build, settled before either route to it. The
       caller says so before the context exists, because this point is
       reached while it is being made -- there is nothing to ask yet. A
       caller that says nothing gets the language the boot files build. */
    ctx->skip_graphics =
        (xpost_vm_image_config() & XPOST_VM_IMAGE_CONFIG_NO_GRAPHICS) ? 1 : 0;

    image_path = _image_read_path(quiet, datadir);
    built = !(image_path && xpost_vm_image_load(ctx, image_path));
    if (built)
        loadinitps(ctx, datadir);

    /* Settle what this run's host decides, in the one dictionary that
       holds such things, now that the language is loaded and there is
       somewhere to put them. Everything above this point is the language
       being built and is the same for every run of this build; the
       settings below it are this run's alone -- and a context whose
       language came out of an image writes them over the ones the image
       carries, which is what keeps a run from inheriting the settings of
       whichever run wrote the file.

       A context whose settings could not be recorded is not one to hand
       back: its readers would answer with nothing, or with what some
       other run left. */
    ret = _record_host_config(ctx, datadir, device, outfile,
                              bufferin, bufferout, semantics,
                              set_size, width, height);
    if (ret)
    {
        XPOST_LOG_ERR("%s recording what this run settles", errorname[ret]);
        return NULL;
    }

    if (built)
    {
        ret = copyudtosd(ctx, ud, sd);
        if (ret)
        {
            XPOST_LOG_ERR("%s error in copyudtosd", errorname[ret]);
            return NULL;
        }

        /* systemdict names itself, which is what lets a program reach it
           without one already being current. Sealing it is a separate step
           and happens below, through the access attributes. */
        ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "systemdict"), sd);
        if (ret)
        {
            XPOST_LOG_ERR("%s naming systemdict in itself", errorname[ret]);
            return NULL;
        }
        /* the context is being built and no save level stands over it
           yet, so this seal is not one a level has to back up and
           cannot be refused the room. A context that arrived out of an
           image is already sealed, and does not pass here. */
        xpost_object_set_access(ctx, sd, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
    }

    xpost_interpreter_set_initializing(0);

    /* a push the stacks would not take, made while the interpreter was
       being brought up. The safe points that read this record are inside
       the loop a program runs in, and nothing has run one yet, so a
       refusal recorded here would keep until the first program and be
       reported against it. That program's stacks are short by an object
       it never pushed, and the context it was handed is one whose
       startup did not finish. Both memory files are read: names intern
       into global VM, so a refusal is recordable on either. */
    if (ctx->lo->push_refused || ctx->gl->push_refused)
    {
        XPOST_LOG_ERR("a stack would not take a pushed object while the "
                      "interpreter was starting");
        return NULL;
    }

    _write_image(ctx, datadir);

    return ctx;
}

/* Scan one PostScript token out of str. The token operator answers a
   flag beneath its results, and reads that flag only where the operator
   ran to its end: a scan the operator refused -- text that stops inside
   a literal is the reachable one -- pushes nothing at all, and what lies
   under a refusal belongs to whatever put it there. Answers the scan's
   own refusal, 0 with the token in *out otherwise, and 0 with a null
   there for text that held no token. */
static
int get_token(Xpost_Context *ctx, char *str, Xpost_Object *out)
{
    Xpost_Object s;
    int ret;

    /* the text to scan becomes a string, which counts its length in a
       field narrower than the text may be */
    if (strlen(str) > (size_t)XPOST_OBJECT_COMP_MAX_SZ)
        return limitcheck;
    s = xpost_string_cons(ctx, (unsigned int)strlen(str), str);
    if (xpost_object_get_type(s) != stringtype)
        return VMerror;
    if (!xpost_stack_push(ctx->lo, ctx->os, s))
        return stackoverflow;
    ret = xpost_operator_exec(ctx, XPOST_OP_CODE(ctx, token));
    if (ret)
        return ret;
    if (xpost_stack_pop(ctx->lo, ctx->os).int_.val){
        *out = xpost_stack_pop(ctx->lo, ctx->os);
        xpost_stack_pop(ctx->lo, ctx->os);
    } else {
        *out = null;
    }
    return 0;
}

/* Defines each of the given name=value strings in userdict, the value
   scanned as the language would scan it. A string with no = defines
   the name as null, which is how a run says a name is present without
   saying what it is. */
XPAPI int xpost_add_definitions(Xpost_Context *ctx, int cnt, char *defs[])
{
    int i;
    Xpost_Object ud;

    if (!ctx) return 0;
    XPOST_LOG_INFO("adding %d defs", cnt);

    ud = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 2);
    for (i = 0; i < cnt; i++)
    {
        char *eq = strchr(defs[i], '=');

        XPOST_LOG_INFO("%s", defs[i]);
        if (eq)
        {
            Xpost_Object tok;
            int ret;

            *eq++ = '\0';
            ret = get_token(ctx, eq, &tok);
            if (ret)
            {
                XPOST_LOG_ERR("%s scanning the value of %s",
                              errorname[ret], defs[i]);
                eq[-1] = '=';
                return 0;
            }
            ret = xpost_dict_put(ctx, ud, xpost_name_cons(ctx, defs[i]), tok);
            eq[-1] = '=';
            if (ret)
                return 0;
        }
        else
        {
            if (xpost_dict_put(ctx, ud,
                        xpost_name_cons(ctx, defs[i]),
                        null))
                return 0;
        }
    }
    return 1;
}

/* Puts a directory at the front of the path the resource machinery
   searches, so a run can offer resources of its own without displacing
   those the installation carries. */
XPAPI int xpost_add_resource_dir(Xpost_Context *ctx, const char *dir)
{
    Xpost_Object ud;
    Xpost_Object key;
    Xpost_Object rp;
    Xpost_Object newrp;
    Xpost_Object str;
    unsigned int n;
    unsigned int i;
    unsigned int vmmode;

    if (!ctx || !dir)
        return 0;

    key = xpost_name_cons(ctx, ".resourcepath");
    ud = _host_dict(ctx);
    if (xpost_object_get_type(ud) != dicttype)
        return 0;

    /* extend the path this run has settled so far */
    rp = xpost_dict_get(ctx, ud, key);
    n = (xpost_object_get_type(rp) == arraytype) ? rp.comp_.sz : 0;

    /* The settings a run makes are global, so the array and its strings
       are made there too. They are data, not a procedure, so make them
       literal -- an executable array would be run, not read, when the
       path is evaluated. */
    /* the directory becomes a string, which counts its length in a field
       narrower than a path may be */
    if (strlen(dir) > (size_t)XPOST_OBJECT_COMP_MAX_SZ)
    {
        XPOST_LOG_ERR("resource directory longer than a string can count");
        return 0;
    }
    vmmode = ctx->vmmode;
    ctx->vmmode = GLOBAL;
    str = xpost_object_cvlit(xpost_string_cons(ctx, (unsigned int)strlen(dir),
                                               (char *)dir));
    newrp = xpost_object_cvlit(xpost_array_cons(ctx, n + 1));
    /* a refused construction answers with no object, which is a null and
       not an invalid: test for the type wanted rather than for one of
       the ways of not having it */
    if (xpost_object_get_type(str) != stringtype ||
        xpost_object_get_type(newrp) != arraytype)
    {
        ctx->vmmode = vmmode;
        return 0;
    }
    for (i = 0; i < n; i++)
        if (xpost_array_put(ctx, newrp, i, xpost_array_get(ctx, rp, i)) != 0)
        {
            ctx->vmmode = vmmode;
            return 0;
        }
    if (xpost_array_put(ctx, newrp, n, str) != 0)
    {
        ctx->vmmode = vmmode;
        return 0;
    }
    ctx->vmmode = vmmode;

    if (xpost_dict_put(ctx, ud, key, newrp))
        return 0;
    return 1;
}

/* The name and the detail of the error the last run ended on, or empty
   strings where it did not end on one. Both read the same flag, so a
   caller that finds a name has a run to blame it on. */
XPAPI const char *xpost_error_name_get(Xpost_Context *ctx)
{
    return ctx->run_uncaught ? ctx->run_error_name : "";
}

XPAPI const char *xpost_error_info_get(Xpost_Context *ctx)
{
    return ctx->run_uncaught ? ctx->run_error_info : "";
}

/*
   execute ps program until quit, fall-through to quit,
   SHOWPAGE_RETURN semantic, or error (default action: message, purge and quit).
 */
/* The start procedures live in privatedict, off the dict stack, so a program
   cannot name them. Fetch the one named and push it, executable, onto the exec
   stack to prime the run. */
static void push_start_proc(Xpost_Context *ctx, const char *name)
{
    xpost_stack_push(ctx->lo, ctx->es,
        xpost_object_cvx(xpost_dict_get(ctx, ctx->privatedict,
                                        xpost_name_cons(ctx, name))));
}

/* Record whether this run has a user at the other end of it, which is
   what the start procedure over a named program reads to decide whether
   to offer the interactive executive once the program has ended.

   Two things have to hold for it to be worth offering. The host must not
   have said otherwise -- naming an output file says the invocation is
   something waiting for that file rather than somebody typing -- and
   standard input must be a terminal. The second is not politeness about
   a prompt: the executive reads standard input and executes what it
   finds there as PostScript (PLRM 2.4.4), so offering it to a pipe runs
   whatever the pipe was carrying, after and outside the program the run
   was asked for.

   The answer goes with the rest of what this run settles, and is written
   afresh on each run so that no run inherits the answer of the last. It
   is written before the job's snapshot is taken, so the job's own rewind
   leaves it standing. */
static void _record_session_kind(Xpost_Context *ctx)
{
    if (_host_put(ctx, ".interactive",
                  xpost_bool_cons(!ctx->batch && xpost_isatty(fileno(stdin)))))
        XPOST_LOG_ERR("cannot record whether this run has a user");
}

/* What showpage does at the end of a page, as the context was created
   with: one of the run's own settings, read here from where the boot
   files read it and where the run is measured against it when it ends.
   A context that did not finish starting has settled nothing, and takes
   the semantics a caller gets by saying nothing at all. */
static int _showpage_semantic(Xpost_Context *ctx)
{
    Xpost_Object semantic = xpost_context_host_setting(ctx,
                                                       "ShowpageSemantics");

    if (xpost_object_get_type(semantic) != integertype)
        return XPOST_SHOWPAGE_DEFAULT;
    return semantic.int_.val;
}

/* Run one of the boot files' start-up steps to its end, on the exec
   stack this call leaves as it found it. The steps are the procedures
   the boot files keep in privatedict beside the start procedures, and
   each is idempotent, so a context that has already been through one
   repeats nothing. */
static void _run_startup_step(Xpost_Context *ctx, const char *proc,
                              const char *what)
{
    unsigned int base = xpost_stack_count(ctx->lo, ctx->es);

    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, quit));
    push_start_proc(ctx, proc);
    ctx->quit = 0;
    ctx->state = C_RUN;
    if (mainloop(ctx) != XPOST_MAINLOOP_DONE)
        XPOST_LOG_ERR("%s did not run to its end", what);

    while (xpost_stack_count(ctx->lo, ctx->es) > (int)base)
        xpost_stack_pop(ctx->lo, ctx->es);
}

/* Load the language into the context, so that a bracket taken after
   this point encloses a program and nothing else.

   The load runs once in the life of a context. What makes it idempotent
   -- the latch the start procedures read -- is virtual memory, and what
   it leaves behind is not: the operator table it fills is outside the
   memory files, and the window it opens on systemdict closes on a
   one-shot held in the context. A bracket taken over the load therefore
   rewinds the half that is in virtual memory and leaves the half that is
   not, and the job after it finds neither a language nor a context that
   can load one again.

   Whether the load succeeded is read from the context afterwards rather
   than from this run: the procedure reports nothing, and the start
   procedure the caller's own run begins with meets the same failure and
   reports it there. */
XPOST_TEST_VISIBLE void xpost_interpreter_load_language(Xpost_Context *ctx)
{
    /* whether the language stands is read from the context afterwards,
       which is the answer the caller acts on; the run below says only
       how the load's own run ended */
    /* Unless the language arrived whole. An image is written after this
       step and holds what the step leaves behind, so a context that read
       one has been through it; running it again is idempotent in what it
       decides and not in what it spends -- it allocates as it satisfies
       itself there is nothing to do -- and virtual memory would then be
       further on than the image that describes it. */
    if (!xpost_vm_image_in_use())
        _run_startup_step(ctx, ctx->skip_graphics ? "loadlanguagenographics"
                                                  : "loadlanguage",
                          "the language load");
}

/* Write an image of virtual memory, where this process was told to.

   The point it is taken at is the one below and no other: the language
   stands complete and locked down, and nothing of the run has been
   decided. The language load is asked for here rather than waited for,
   because by the time a run asks for it the run has already put its
   program on the operand stack -- and an image taken then would carry
   the name of whatever file the run that wrote it was given.

   Only where a run reading the image would arrive at the same place. A
   process that has said it will build the language wants another one; a
   run whose messages an image cannot reproduce is not one an image is
   read into; and a context that is not the first this process made does
   not reach the memory a first one does. Each of those leaves the file
   alone rather than writing something no reader would expect. */
static void _write_image(Xpost_Context *ctx, const char *datadir)
{
    const char *path = _image_write_path(datadir);
    /* Whether anybody asked for this file, which decides how loudly a
       failure to write it is reported. A run that named the file is
       owed the reason it did not appear; a run that merely stood to
       gain by one is owed nothing, and a cache that cannot be written
       -- a read-only home, a full disk -- must not turn every such run
       into an error. */
    int asked = getenv("XPOST_VM_IMAGE_WRITE") != NULL;
    char tmp[XPOST_PATH_MAX];
    int n;

    if (!path || xpost_vm_image_refused() || !ctx->quiet ||
        _contexts_created != 1)
        return;

    xpost_interpreter_load_language(ctx);

    /* Written beside where it belongs and moved onto it, so that what
       another run finds at the name is either the whole of an image or
       nothing. A run reads this file as the language it is about to
       execute; one that met half of it would be reading whatever the
       writer had reached, and the digest that catches a partial file is
       the last thing written. The name carries the writer's process, so
       that two runs racing to fill an empty cache write two files and
       each moves its own. */
    n = snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path,
                 (long)_xpost_getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp))
    {
        if (asked)
            XPOST_LOG_ERR("no room to name a file to write the image %s"
                          " through", path);
        return;
    }

    if (!xpost_vm_image_write(ctx, tmp, 0))
    {
        remove(tmp);
        if (asked)
            XPOST_LOG_ERR("cannot write an image of virtual memory to %s",
                          path);
        return;
    }

    /* A destination that already exists is replaced. POSIX rename does
       that; the Windows one refuses, so there the old file goes first.
       The gap that leaves is a run finding no image and building the
       language, which is what it would have done before there was one. */
#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp, path) != 0)
    {
        remove(tmp);
        if (asked)
            XPOST_LOG_ERR("cannot move the image of virtual memory onto %s",
                          path);
    }
}

/* Make the device this run was started with, for the same reason the
   language is loaded here: so that a bracket taken after this point
   encloses a program and nothing else.

   A device is made once in the life of a context, and what it reaches
   its instance state through is a handle on a block that is not virtual
   memory. A device made inside the bracket would be unmade by the rewind
   that ends the job, taking the graphics state back to having none while
   the block it held stayed where it was, past the reach of both the
   collector and the rewind; the job after it would make another, and so
   would every job after that.

   The step is idempotent -- a run that has a device keeps it -- so this
   says only when the device is made and not that it is remade. A run
   that loads no graphics has no device to make: nothing there installs
   one and the graphics state it locks down carries none. */
static void _make_start_device(Xpost_Context *ctx)
{
    ctx->device_made = 1;
    if (ctx->skip_graphics)
        return;
    _run_startup_step(ctx, "startdevice",
                      "making the device the run was started with");
}

/* Capture the fixed baseline the job boundary reverts to: a whole-VM image
   of both banks plus the baseline of the per-job interpreter state that
   lives outside virtual memory. Taken once, when the language and any
   server-level prelude have loaded -- the PLRM 3.7.7 "initial VM" a job
   begins from. Returns 1 on success. */
/* The context's own object roots are snapshotted with the arena baseline and
   put back with it: the arena revert cannot reach a field outside the arena,
   and a root left naming an entity the revert discarded is a dangling
   reference the next collection follows. run_input_file is excluded -- the
   stream file a job wrapped is closed and re-primed by the job-stream logic
   around the boundary, not reverted with the rest. */
#define XPOST_JOB_SAVE_ROOT(f)    ctx->job_saved_##f = ctx->f;
#define XPOST_JOB_RESTORE_ROOT(f) ctx->f = ctx->job_saved_##f;

/* Records what a job begins from: both banks of the arena, the
   operator table, and the number of operators there were. The table is
   taken because a job may define operators of its own, and a number
   issued during one job would otherwise still be counted in the next. */
static int _job_capture_baseline(Xpost_Context *ctx)
{
    if (!ctx->job_baseline_lo)
        ctx->job_baseline_lo = calloc(1, sizeof *ctx->job_baseline_lo);
    if (!ctx->job_baseline_gl)
        ctx->job_baseline_gl = calloc(1, sizeof *ctx->job_baseline_gl);
    if (!ctx->job_baseline_lo || !ctx->job_baseline_gl)
        return 0;
    if (!xpost_memory_image_capture(ctx->lo, ctx->job_baseline_lo))
        return 0;
    if (!xpost_memory_image_capture(ctx->gl, ctx->job_baseline_gl))
        return 0;
    /* Only the global bank. The local bank is where the interpreter
       works, so it grows on almost every render, and each grow has to
       collapse the private view and build it again -- churn that costs
       more than the copy it saves on a bank this small.
       MEASURED: arming it too takes the barrier from 2.2% to 8.8% of an
       impression at eight concurrent workers. */
    (void)xpost_memory_revert_arm(ctx->gl, ctx->job_baseline_gl->store,
                                  ctx->job_baseline_gl->used);
    if (!xpost_operator_table_snapshot(ctx->gl, &ctx->job_baseline_optab,
                                       &ctx->job_baseline_optab_len))
        return 0;
    ctx->job_baseline_operators = xpost_operator_count();
    ctx->job_rand_next = ctx->rand_next;
    ctx->job_vmmode = ctx->vmmode;
    ctx->job_packing = ctx->packing;
    ctx->job_baseline_ds = xpost_stack_count(ctx->lo, ctx->ds);
    ctx->job_saved_pagedevice_depth = ctx->pagedevice_depth;
    ctx->job_idiomrecognition = ctx->idiomrecognition;
    ctx->job_vmthreshold = ctx->vmthreshold;
    ctx->job_namewrapsave = ctx->namewrapsave;
    memcpy(ctx->job_typenames, ctx->typenames, sizeof ctx->job_typenames);
    {
        long bsize, msize, csize, mmax, cmax, blimit;

        xpost_font_cache_status(&bsize, &ctx->job_gcache_bmax, &msize, &mmax,
                                &csize, &cmax, &blimit);
        /* the per-glyph ceiling is the context's own parameter, so the
           baseline takes it from there rather than from the store the
           context writes it through to */
        ctx->job_gcache_blimit = ctx->maxfontitem;
    }
    XPOST_CONTEXT_OBJECT_ROOTS(XPOST_JOB_SAVE_ROOT)
    return 1;
}

/* Close the files and filters a job opened, before the arena revert drops
   their entities. The revert returns a bank to its baseline image, but a
   file's stream and its operating-system descriptor live outside the arena,
   in a handle the revert cannot reach; a file left open at the boundary
   would leak both for the life of the server, one per job that opened one,
   until the process runs out of descriptors -- exactly the leak the restore
   operator avoids by closing the files born since a save (PLRM 3.8.2). A
   file the baseline held at this same entity, in the same storage, is one of
   the baseline's own and stays open; every other file entity is one the job
   opened and is closed here, which frees the handle the revert would strand. */
static void _job_close_born_files(Xpost_Memory_File *mem,
                                  const Xpost_Memory_Image *base)
{
    __typeof__(mem->table.tab) btab = (void *)base->tab;
    /* how many entities the baseline's table copy holds -- a bound on the
       copy this reads, not a test of a live entity's validity, so it is
       read into a name of its own rather than compared against in place */
    unsigned int baseline_ents = base->nextent;
    unsigned int ent;

    for (ent = mem->start; ent < mem->table.nextent; ent++)
    {
        if (mem->table.tab[ent].tag != filetype)
            continue;
        /* Keyed on the entity number, which is stable, and not on the
           storage address, which a compaction during the job moves: a
           file the baseline held at this entity is one of its own. (A job
           that closed one of the baseline's files and opened another at
           the freed entity is not distinguished, but that costs one leaked
           handle at most, where the entity number alone catches every file
           a job opens at a fresh entity, which is all of them in practice.) */
        if (ent < baseline_ents && btab[ent].tag == filetype)
            continue;
        {
            Xpost_Object o = { 0 };

            o.mark_.tag = filetype;
            o.mark_.pad0 = 0;
            o.mark_.padw = ent;
            (void)xpost_file_object_close(mem, o);
        }
    }
}

/* Revert the whole context to the fixed baseline: the job-encapsulation
   boundary (PLRM 3.7.7 steps 5 and 6, both banks). It runs in C, after the
   job's execution is over, so no job code -- no redefinition, error handler,
   or unbalanced save -- can intercept, prevent, or partly execute it. The
   two banks are put back by whole-VM image restore, which is
     total       -- every byte of both arenas returns to the baseline, so
                     strings and stack contents revert with the objects and
                     nothing the job wrote survives;
     infallible   -- the restore copies the baseline back into storage the
                     file already owns and allocates nothing, so no VM state
                     a job can arrange makes it fail;
     leak-free    -- the arena cursors return to the baseline, discarding
                     everything the job allocated in one stroke rather than
                     accumulating a save level or garbage per job.
   The handful of per-job values that do not live in the arena are reset
   here beside it (the RNG seed, the allocation mode, the packing mode) and
   the name cache is turned over so a resolution cached against the reverted
   bindings cannot be served to the next job. */
static void _job_revert_to_baseline(Xpost_Context *ctx)
{
    if (!ctx->job_baseline_lo || !ctx->job_baseline_lo->valid
        || !ctx->job_baseline_gl || !ctx->job_baseline_gl->valid)
    {
        ctx->job_boundary_failed = 1;
        return;
    }
    /* Retire a page device the job installed over the baseline's, running
       its Destroy while virtual memory is intact and its output files are
       still open (as restore does, before its own file-close sweep): the
       image restore below drops the device dictionary, and only the Destroy
       frees the raster or accumulator the device holds outside the arena. */
    xpost_device_retire_job(ctx, ctx->job_saved_pagedevice_depth);
    _job_close_born_files(ctx->lo, ctx->job_baseline_lo);
    _job_close_born_files(ctx->gl, ctx->job_baseline_gl);
    xpost_memory_image_restore(ctx->lo, ctx->job_baseline_lo);
    xpost_memory_image_restore(ctx->gl, ctx->job_baseline_gl);
    /* The blocks the revert has just stopped naming. A handle is given up
       where the entity carrying it is taken away, and every other way an
       entity goes -- a free, a collection, the end of the memory file --
       does that as it goes; the revert takes the whole arena back without
       walking an entity, so the sweep is what stands in for it here. A job
       that ended with a font dictionary, or a device it made, still
       reachable would otherwise leave the block behind for the life of the
       server, one per handle per job. */
    xpost_handle_release_orphans(ctx->lo);
    xpost_handle_release_orphans(ctx->gl);
    if (!xpost_operator_table_restore(ctx->gl, ctx->job_baseline_optab,
                                      ctx->job_baseline_optab_len))
    {
        ctx->job_boundary_failed = 1;
        return;
    }
    if (ctx->job_baseline_optab)
        xpost_operator_set_count(ctx->job_baseline_operators);
    {
        /* put the object roots back to their baseline, save the stream file
           the job-stream logic owns across the boundary */
        Xpost_Object keep_run_input_file = ctx->run_input_file;
        XPOST_CONTEXT_OBJECT_ROOTS(XPOST_JOB_RESTORE_ROOT)
        ctx->run_input_file = keep_run_input_file;
    }
    ctx->rand_next = ctx->job_rand_next;
    ctx->vmmode = ctx->job_vmmode;
    ctx->packing = ctx->job_packing;
    ctx->idiomrecognition = ctx->job_idiomrecognition;
    ctx->vmthreshold = ctx->job_vmthreshold;
    /* The caches of interned names the context holds beside the roots: a
       name is an index into a name stack the revert has just wound back,
       so an index a job's own interning produced names nothing there now
       (PLRM 3.7.7). */
    ctx->namewrapsave = ctx->job_namewrapsave;
    memcpy(ctx->typenames, ctx->job_typenames, sizeof ctx->typenames);
    /* The glyph cache's byte parameters, which are the MaxFontCache system
       parameter and the MaxFontItem user parameter (PLRM 8.2
       setcacheparams). Neither is in the arena, so neither is reached by
       the revert above: the ceiling is the context's own field and the
       capacity is the store's.

       The ceiling is what a job can carry across. PLRM C.1.1 has an
       encapsulated job's change to a user parameter leave the value later
       jobs begin from alone, and a job that set the ceiling to nothing
       would otherwise leave every job after it rendering every glyph
       afresh. The capacity is put back with it because the pair is one
       state: an encapsulated job cannot reach it (setcacheparams refuses
       a stated capacity outside a system administrator job) and an
       unencapsulated one is folded into the baseline rather than reverted,
       so this restores what it captured either way. */
    ctx->maxfontitem = (integer)ctx->job_gcache_blimit;
    (void) xpost_font_cache_setparams(ctx->job_gcache_bmax, 0,
                                      ctx->job_gcache_blimit);
    ++ctx->namebind_gen;
    ctx->es_over = ctx->os_over = ctx->ds_over = 0;
    ctx->onerr_run = 0;
    /* the one cache outside virtual memory whose key a job can choose: a
       procedure/Type-3 glyph mask is keyed by the serial the font
       dictionary carries under .fontid, and a font dictionary is the
       program's to write. Two jobs presenting one serial over different
       glyph descriptions would otherwise share the entries filed under it,
       and the second would be answered the first job's glyph. Drop those
       masks (FreeType-face glyphs are kept: their key is the face pointer,
       which nothing a program writes decides). */
    xpost_font_mask_cache_flush();
}

/* A job's execution contexts do not outlive the job.

   A context forked by the job holds stacks and object roots that are
   entities of the virtual memory the boundary is about to wind back to an
   image taken before they existed. Left in the table it would still be
   runnable, and the scheduler would give the next job's run to it: it
   would execute an execution stack that is no longer there, and the
   collector would walk roots that name nothing. So every context but the
   one the boundary is being taken on is ended here, in C, after the job's
   execution is over and before anything is wound back.

   This runs whether the boundary reverts or captures. A run left
   unencapsulated folds its virtual memory into the baseline rather than
   discarding it, and a context of the job captured into that image would
   be handed to every job that reverts to it afterwards. */
static void _job_end_other_contexts(Xpost_Context *ctx)
{
    unsigned int i;

    if (!itpdata)
        return;
    for (i = 0; i < MAXCONTEXT; i++)
    {
        Xpost_Context *c = &itpdata->ctab[i];

        if (c == ctx || c->state == C_FREE)
            continue;
        xpost_context_release(c);
    }
}

/* The job boundary as xpost_run reaches it: release the out-of-VM side state
   any frame the run left on the exec stack holds -- a wrapped-operator frame's
   saved operands and a filenameforall enumeration's matched paths (the image
   restore drops the frame entries but not that side state) -- then either
   revert to the baseline (every job after the first) or, on the first run,
   establish the baseline the later jobs revert to. */
static void _job_boundary(Xpost_Context *ctx)
{
    _job_end_other_contexts(ctx);
    xpost_context_unwind_exec(ctx, ctx->es_run_base);

    if (!ctx->job_snapshots)
        return;

    if (ctx->job_baseline_lo && ctx->job_baseline_lo->valid
        && ctx->job_encapsulated)
    {
        /* an encapsulated run reverts to the baseline */
        _job_revert_to_baseline(ctx);
    }
    else
    {
        /* the first run establishes the baseline the later jobs revert to
           (the loaded language and any prelude the embedder ran), and a run
           left unencapsulated by exitserver / `true password startjob` folds
           its state into the baseline so its definitions persist. Either
           way, clear the operand and scratch stacks so the baseline carries
           the empty operand stack a job begins from (PLRM 3.7.7). */
        xpost_stack_clear(ctx->lo, ctx->os);
        xpost_stack_clear(ctx->lo, ctx->hold);
        if (!_job_capture_baseline(ctx))
            XPOST_LOG_ERR("cannot capture the job baseline image");
    }
}

/* Close the file a run wrapped around the program it was given. A run
   that reads its program to the end closes it there; one that stops
   before the end -- at its quit operator, or on an error that unwinds
   past every stopped context -- would otherwise leave it open, and the
   file a program arrives in as a string is one this run made itself. */
static void _close_run_input(Xpost_Context *ctx)
{
    if (xpost_object_get_type(ctx->run_input_file) == filetype)
    {
        (void) xpost_file_object_close(ctx->lo, ctx->run_input_file);
        ctx->run_input_file = null;
    }
}

/* This job ended at a job-server delimiter (Control-D) and its stream has
   another job after it. Report so, clearing the mark so the next job's own
   delimiter is read fresh. The stream position is the C stream's, which the
   baseline revert does not touch, so the next job reads on from where this
   one stopped -- the reader lives outside the virtual memory the boundary
   reverts, which is what lets a job boundary be taken in mid-run. */
static int _job_stream_continues(Xpost_Context *ctx)
{
    Xpost_File *f;

    if (xpost_object_get_type(ctx->run_input_file) != filetype)
        return 0;
    f = xpost_file_get_file_pointer(ctx->lo, ctx->run_input_file);
    if (!f || !f->job_stream)
        return 0;
    /* A job that quit or errored before reading its own Control-D leaves eot
       clear with the delimiter still ahead in the stream. PLRM 3.7.7 step 4:
       flush the input to end-of-file -- read on and discard to the next
       Control-D -- so it is consumed here and the following job begins at its
       own start rather than reading this job's tail. xpost_file_getc returns
       EOF at the Control-D and sets eot; at the true end of the stream it
       returns EOF with eot still clear, which is the stream's real end and no
       next job. */
    while (!f->eot && xpost_file_getc(f) != EOF)
        ;
    if (!f->eot)
        return 0;
    f->eot = 0;
    return 1;
}

/* Prime the context to run one job of a stream from an empty exec stack: a
   quit sentinel at the base the job winds back to, the stream file on the
   operand stack, and the start procedure that runs it on the exec stack.
   Used for the first job of a stream and again for each job after a boundary,
   so every job of a stream begins the way a lone run does, with its own
   start procedure and so its own error context. */
static void _prime_job_stream(Xpost_Context *ctx)
{
    ctx->es_run_base = xpost_stack_count(ctx->lo, ctx->es);
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, quit));
    xpost_stack_push(ctx->lo, ctx->os, ctx->run_input_file);
    push_start_proc(ctx,
            ctx->skip_graphics ? "startfilenographics" : "startfile");
}

/* Runs one input to its end -- a named file, a string, an open stream,
   or the resumption of a session that returned -- and answers whether it
   ended of its own accord or on an error nothing caught. A run that
   arrives without the language loaded or without a device gets both
   before its program runs, and the device is destroyed and the job
   boundary crossed on the way out, whichever way the run ended. */
XPAPI Xpost_Run_Status xpost_run(Xpost_Context *ctx, Xpost_Input_Type input_type, const void *inputptr, size_t set_size)
{
    const char *ps_str = NULL;
    const char *ps_file = NULL;
    const FILE *ps_file_ptr = NULL;
    int ret;
    Xpost_Object device;

    switch(input_type)
    {
        case XPOST_INPUT_FILENAME:
            ps_file = inputptr;
            break;
        case XPOST_INPUT_STRING:
            ps_str = inputptr;
            ps_file_ptr = tmpfile();
            if (ps_file_ptr == NULL)
            {
                XPOST_LOG_ERR("cannot create temporary file for program");
                return XPOST_RUN_FAILED;
            }
            if (set_size)
                fwrite(ps_str, 1, set_size, (FILE*)ps_file_ptr);
            else
                fwrite(ps_str, 1, strlen(ps_str), (FILE*)ps_file_ptr);
            rewind((FILE*)ps_file_ptr);
            break;
        case XPOST_INPUT_FILEPTR:
            ps_file_ptr = inputptr;
            break;
        case XPOST_INPUT_RESUME: /* resuming a returned session, skip startup */
            /* the resumed run restarts the cascade count and error record */
            ctx->onerr_run = 0;
            ctx->run_error_name[0] = '\0';
            ctx->run_error_info[0] = '\0';
            ctx->run_uncaught = 0;
            goto run;
    }

    /* a fresh run starts with a clean cascade count and error record */
    ctx->onerr_run = 0;
    ctx->run_error_name[0] = '\0';
    ctx->run_error_info[0] = '\0';
    ctx->run_uncaught = 0;
    /* a fresh run is encapsulated until it executes exitserver / a `true`
       startjob; its boundary reverts unless one of those makes it persist */
    ctx->job_encapsulated = 1;

    /* prime the exec stack
       so it starts with a 'start*' procedure,
       and if it ever gets to the bottom, it quits.
       These procedures are all defined in data/init.ps
     */
    ctx->es_run_base = xpost_stack_count(ctx->lo, ctx->es);
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, quit));
    _record_session_kind(ctx);
    /*
       if ps_file is NULL:
         if stdin is a tty
           `start` proc defined in init.ps runs `executive` which prompts for user input
         else
           'startstdin' executes stdin but does not prompt

       if ps_file is not NULL:
       'startfilename' executes a named file wrapped in a stopped context with
       handleerror, and ends there. A named program is a job and a job ends where
       its program ends (PLRM 3.7.7); the executive it offers afterwards when
       this run has a user is the answer recorded just above.
    */
    /* with skip_graphics set, dispatch to the no-graphics start procedures,
       which run the interpreter lockdown without loading the graphics modules.
       The interactive (tty) session always loads graphics. */
    if (ps_file)
    {
        /* The name has to survive becoming a string object, which counts
           its length in a field narrower than a path may be. A name the
           field cannot count would be answered as the length it wrapped
           to, and the shortened name is the one that would be opened --
           a different file from the one asked for, chosen by whoever
           supplied the name. Refuse it here, where the caller can still
           be told which of its calls failed. */
        Xpost_Object nameobj;

        if (strlen(ps_file) > (size_t)XPOST_OBJECT_COMP_MAX_SZ)
        {
            XPOST_LOG_ERR("file name longer than a string can count");
            return XPOST_RUN_FAILED;
        }
        nameobj = xpost_string_cons(ctx, (unsigned int)strlen(ps_file), ps_file);
        if (xpost_object_get_type(nameobj) != stringtype)
        {
            XPOST_LOG_ERR("cannot make a string of the file name");
            return XPOST_RUN_FAILED;
        }
        xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(nameobj));
        push_start_proc(ctx, ctx->skip_graphics ? "startfilenamenographics" : "startfilename");
    }
    else if (ps_file_ptr)
    {
        /* the stream the embedding caller handed over is the program to
           run, so the file object over it carries the attribute of a
           stream opened to be read: the start procedure executes it, and
           execution reads it */
        ctx->run_input_file =
            xpost_object_cvlit(xpost_file_cons(ctx->lo, ps_file_ptr, 1));
        if (xpost_object_get_type(ctx->run_input_file) == filetype)
        {
            ctx->run_input_file.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
            ctx->run_input_file.tag |=
                XPOST_OBJECT_TAG_ACCESS_FILE_READ
                << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET;
            /* an embedder that isolates its jobs may hand over a stream that
               carries more than one, framed by Control-D; mark it so the job
               boundary is taken at each delimiter (PLRM 3.7.7 server loop)
               rather than only when the whole stream ends */
            if (ctx->jobserver && ctx->job_snapshots)
            {
                Xpost_File *jf =
                    xpost_file_get_file_pointer(ctx->lo, ctx->run_input_file);
                if (jf)
                    jf->job_stream = 1;
            }
        }
        xpost_stack_push(ctx->lo, ctx->os, ctx->run_input_file);
        push_start_proc(ctx, ctx->skip_graphics ? "startfilenographics" : "startfile");
    }
    else
    {
        if (xpost_isatty(fileno(stdin)))
            push_start_proc(ctx, "start");
        else
            push_start_proc(ctx, ctx->skip_graphics ? "startstdinnographics" : "startstdin");
    }

    /* The bracket is this call's: it is taken here and rewound before
       this call returns. A job under XPOST_SHOWPAGE_RETURN is not one
       call -- it hands control back at each showpage and ends on a later
       call, which is where its virtual memory stops changing -- so no
       bracket is taken over it, rather than a save level being pushed
       that this call has nothing to do with.

       What the bracket encloses is the program. What the context is made
       of is brought up before it is taken -- the language, and then the
       device this run was started with -- so that rewinding to it rewinds
       what the job wrote and not what it was given to write with; a
       context whose language did not load takes no bracket at all, and
       its run reports the failure the way a run on its own does. Both
       steps run once in the life of the context, so this is where they
       happen and the start procedure below finds them done.

       Both are asked after, because either may already be done without
       the other: a context whose language arrived whole out of an image
       has the language and has made no device, and a device made inside
       the bracket would be unmade by the rewind that ends the job and
       made again by the next one. */
    if (ctx->job_snapshots
        && _showpage_semantic(ctx) != XPOST_SHOWPAGE_RETURN)
    {
        if (!ctx->sysdict_load_done || !ctx->device_made)
        {
            xpost_interpreter_load_language(ctx);
            _make_start_device(ctx);
        }
    }

    /* A job stream's first job is a real job, not a prelude, so the state it
       reverts to is the one before it: the baseline is captured here, with
       the language and device loaded and the stacks empty (PLRM 3.7.7 step
       3), rather than at the first job's boundary, which would fold that
       first job's own work into it. Each job of the stream -- the first and
       every one after a boundary -- is then primed the way a lone run is, so
       each begins with its own start procedure and error context. Captured
       once: the first job reaches here with no baseline, and a job that
       alters the baseline through exitserver only replaces it. */
    {
        Xpost_File *jf =
            xpost_object_get_type(ctx->run_input_file) == filetype
            ? xpost_file_get_file_pointer(ctx->lo, ctx->run_input_file)
            : NULL;
        if (jf && jf->job_stream && ctx->job_snapshots
            && (!ctx->job_baseline_lo || !ctx->job_baseline_lo->valid))
        {
            if (!ctx->sysdict_load_done || !ctx->device_made)
            {
                xpost_interpreter_load_language(ctx);
                _make_start_device(ctx);
            }
            /* drop the priming this run laid down, capture the empty
               baseline (initial VM, empty stacks -- PLRM 3.7.7 step 3), and
               prime the first job onto it afresh */
            while (xpost_stack_count(ctx->lo, ctx->es) > (int)ctx->es_run_base)
                (void) xpost_stack_pop(ctx->lo, ctx->es);
            xpost_stack_clear(ctx->lo, ctx->os);
            xpost_stack_clear(ctx->lo, ctx->hold);
            if (!_job_capture_baseline(ctx))
                XPOST_LOG_ERR("cannot capture the job-stream baseline");
            _prime_job_stream(ctx);
        }
    }

    /* Run! */
run:
    ctx->quit = 0;
    ctx->state = C_RUN;
    ret = mainloop(ctx);

    /* A context that did not validate has no run to report on, and
       nothing below may be asked of it: the memory it failed to answer
       for is where the page semantics are read from, where the device
       stands and where the snapshot records live. So this is read before
       any of them, and the run gives back the file it wrapped around the
       program and nothing else. */
    if (ret == XPOST_MAINLOOP_INVALID)
    {
        XPOST_LOG_ERR("the context did not validate; the run is abandoned");
        _close_run_input(ctx);
        return XPOST_RUN_FAILED;
    }

    /* The job ended -- its input reached a Control-D, or the true end of the
       stream, or the job errored (a yield is not an end; it is handled
       below). If it ended at a Control-D and the stream carries another job,
       revert to the baseline and run that job from the same stream. The
       stream and, when there is one, the device are the server's rather than
       the job's, so they are not given up here the way the last job gives
       them up: the file stays open (its exec left it so) and is run again. */
    if (ret != XPOST_MAINLOOP_YIELDED && _job_stream_continues(ctx))
    {
        /* Revert to the baseline captured before the stream's first job: it
           restores the job priming -- the stream file on the operand stack,
           the start procedure on the exec stack -- with the initial VM, so
           the next job runs from it with nothing re-pushed here. A job that
           ran exitserver folds into the baseline instead, so its definitions
           reach the jobs after it. */
        _job_boundary(ctx);
        ctx->onerr_run = 0;
        ctx->run_error_name[0] = '\0';
        ctx->run_error_info[0] = '\0';
        ctx->run_uncaught = 0;
        ctx->job_encapsulated = 1;
        _prime_job_stream(ctx);
        goto run;
    }

    if (_showpage_semantic(ctx) == XPOST_SHOWPAGE_RETURN)
    {
        if (ret == XPOST_MAINLOOP_YIELDED)
            return XPOST_RUN_YIELDED;

        /* the run stops at its quit operator, leaving the frames beneath
           it -- the run's own scheduling tail -- on the exec stack; the
           boundary discards them (and reverts the whole context to the
           baseline) whether the run completed or errored. This is where a
           XPOST_SHOWPAGE_RETURN job ends: the yield above returns without a
           boundary, so the revert happens once, on the call that finishes
           the multi-call job. */
        /* close the file this run wrapped around its program before the
           boundary reverts virtual memory: the file object is a local
           entity the run created, so the revert discards it, and closing
           the stream has to happen while the entity that names it is still
           there */
        _close_run_input(ctx);
        _job_boundary(ctx);
        return ctx->run_uncaught ? XPOST_RUN_ERRORED : XPOST_RUN_COMPLETE;
    }

    XPOST_LOG_INFO("destroying device");
    /* the device lives in the graphics state; the DEVICE name is an
       accessor operator and no longer holds the dictionary itself */
    device = ctx->graphicsdict;
    if (xpost_object_get_type(device) == dicttype)
        device = xpost_dict_get(ctx, device, xpost_name_cons(ctx, "currgstate"));
    if (xpost_object_get_type(device) == dicttype)
        device = xpost_dict_get(ctx, device, xpost_name_cons(ctx, "device"));
    XPOST_LOG_INFO("device type=%s", xpost_object_type_names[xpost_object_get_type(device)]);
    /*xpost_operator_dump(ctx, 1); // is this pointer value constant? */
    if (xpost_object_get_type(device) == arraytype){
        XPOST_LOG_INFO("running proc");
        xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, quit));
        xpost_stack_push(ctx->lo, ctx->es, device);

        ctx->quit = 0;
        /* the device this leaves on the operand stack is the procedure's
           answer, so a run that did not reach its end has not left one */
        if (mainloop(ctx) == XPOST_MAINLOOP_DONE)
            device = xpost_stack_pop(ctx->lo, ctx->os);
        else
        {
            XPOST_LOG_ERR("the device procedure did not run to its end");
            device = null;
        }
    }
    if (xpost_object_get_type(device) == dicttype)
    {
        Xpost_Object Destroy;
        /* The release run is the one the device's block was issued to be
           given up by, recorded when the block was issued and reached
           from the block rather than from /Destroy -- an ordinary slot
           the program writes to. A device carrying such a block (the
           ones whose Destroy is a C operator) is given up by that
           operator whatever the slot now holds; the vector writers keep
           their state under a content block instead and give it up
           through the procedure under /Destroy, run below. */
        unsigned int release = xpost_handle_device_release(ctx, device);
        XPOST_LOG_INFO("destroying device dict");
        Destroy = xpost_dict_get(ctx, device, xpost_name_cons(ctx, "Destroy"));
        if (release != 0)
        {
            int res;
            xpost_stack_push(ctx->lo, ctx->os, device);
            res = xpost_operator_exec(ctx, release);
            if (res)
                XPOST_LOG_ERR("%s error destroying device", errorname[res]);
            else
                XPOST_LOG_INFO("destroyed device");
        }
        else if (xpost_object_get_type(Destroy) == arraytype)
        {
            XPOST_LOG_INFO("running Destroy proc");
            xpost_stack_push(ctx->lo, ctx->os, device);
            /* the run this is tearing down stopped at its quit with the
               frames it had yet to return through still on the exec
               stack; a stop of this interval's own is what keeps the
               teardown from carrying on down into them */
            xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, quit));
            xpost_stack_push(ctx->lo, ctx->es, Destroy);

            ctx->quit = 0;
            if (mainloop(ctx) != XPOST_MAINLOOP_DONE)
                XPOST_LOG_ERR("the device's Destroy did not run to its end");
        }
    }

    _close_run_input(ctx);
    _job_boundary(ctx);
    return ctx->run_uncaught ? XPOST_RUN_ERRORED : XPOST_RUN_COMPLETE;
}

/* Say whether this context loads the graphics language; a run that
   does not gets the start procedure that leaves it out. */
XPAPI void xpost_skip_graphics_set(Xpost_Context *ctx, int enable)
{
    ctx->skip_graphics = enable;
}

/* say whether this context serves an interactive user; a run over a
   named program ends where the program ends either way once this is set */
XPAPI void xpost_batch_set(Xpost_Context *ctx, int enable)
{
    ctx->batch = enable;
}

/* enable or disable per-job VM snapshots for a context */
XPAPI void xpost_job_snapshots_set(Xpost_Context *ctx, int enable)
{
    ctx->job_snapshots = enable;
}

/* treat a run's embedder-supplied input stream as a Control-D-framed
   job-server channel (PLRM 3.7.7); needs per-job isolation, which frames the
   jobs the delimiter separates */
XPAPI void xpost_jobserver_set(Xpost_Context *ctx, int enable)
{
    ctx->jobserver = enable;
}

/* fold the current state into the baseline every later job reverts to */
XPAPI void xpost_job_baseline_set(Xpost_Context *ctx)
{
    if (!ctx)
        return;
    xpost_stack_clear(ctx->lo, ctx->os);
    xpost_stack_clear(ctx->lo, ctx->hold);
    if (!_job_capture_baseline(ctx))
        XPOST_LOG_ERR("cannot capture the job baseline image");
}

/* set the StartJobPassword that startjob/exitserver check */
XPAPI void xpost_startjob_password_set(Xpost_Context *ctx, const char *password)
{
    if (!ctx)
        return;
    if (!password)
        password = "";
    /* truncation only lengthens a password, never opens the door */
    snprintf(ctx->startjob_password, sizeof ctx->startjob_password, "%s", password);
}

/* revert the whole context to the baseline, readying a fresh job */
XPAPI int xpost_new_job(Xpost_Context *ctx)
{
    if (!ctx)
        return 0;
    /* the job whose end this is takes its contexts with it, whether what
       follows is a revert or the capture of a first baseline */
    _job_end_other_contexts(ctx);
    if (ctx->job_baseline_lo && ctx->job_baseline_lo->valid)
    {
        _job_revert_to_baseline(ctx);
        return !ctx->job_boundary_failed;
    }
    /* no baseline yet: the current state becomes it */
    xpost_stack_clear(ctx->lo, ctx->os);
    xpost_stack_clear(ctx->lo, ctx->hold);
    return _job_capture_baseline(ctx);
}

/* Where this context's standard output and standard error go. Unset,
   both go to the streams of those names; a caller that sets them is
   handed the bytes instead of the file being written. */
XPAPI void xpost_stdout_handler_set(Xpost_Context *ctx,
                                    Xpost_Output_Fn fn,
                                    void *user)
{
    ctx->stdout_fn = fn;
    ctx->stdout_user = user;
}

XPAPI void xpost_stderr_handler_set(Xpost_Context *ctx,
                                    Xpost_Output_Fn fn,
                                    void *user)
{
    ctx->stderr_fn = fn;
    ctx->stderr_user = user;
}

/*
   destroy the interpreter's context and associated memory files,
   and with them the interpreter instance holding them.
 */
XPAPI void xpost_destroy(Xpost_Context *ctx)
{
    if (!ctx)
        return;

    /* The instance hands out one context, so this is the only handle
       that ends it. Anything else names a context this cannot account
       for, and taking a memory file apart under a live instance is
       worse than declining. */
    if (!itpdata || (ctx != &itpdata->ctab[0]))
    {
        XPOST_LOG_ERR("not the interpreter's context");
        return;
    }

    if (!ctx->quiet)
    {
        printf("bye!\n");
        fflush(NULL);
    }

    xpost_context_exit(ctx);

    /* ctx is ctab[0]. A job that forked contexts left each of them its own
       name-resolution cache in C heap, sharing ctx's virtual memory. The
       slots go with itpdata below, but the caches they point to are
       separate allocations that would be left behind, so release each here.
       An unused slot holds a null pointer -- the table was zeroed at
       startup -- which frees cleanly. */
    {
        unsigned int i;
        for (i = 1; i < MAXCONTEXT; i++)
        {
            free(itpdata->ctab[i].namecache_gen);
            free(itpdata->ctab[i].namecache_val);
            itpdata->ctab[i].namecache_gen = NULL;
            itpdata->ctab[i].namecache_val = NULL;
            itpdata->ctab[i].namecache_size = 0;
        }
    }

    free(itpdata);
    itpdata = NULL;
}
