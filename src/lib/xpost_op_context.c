/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_context.c
 * @brief Installs the context operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * fork join detach yield currentcontext
 *
 * Concurrency, not preemption: a context runs until it yields, blocks, or
 * returns, which is what PLRM 2nd ed 7.1 permits.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdio.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_private.h" /* XPOST_REFUSAL_IMPOSSIBLE */
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_dict.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_stack.h"
#include "xpost_op_dict.h"   /* copy the parent's privatedict for a forked child */
#include "xpost_op_context.h"


/* The Display PostScript multiple-execution-context operators. They are
   always compiled, but installed only when a program has asked for them at
   run time -- xpost_dps_set, reached from the xpost binary's --enable-dps.
   A default run installs none of them, so naming one gets undefined. The
   scheduler that would drive them (xpost_interpreter.c _switch_context) is a
   stub even when they are installed -- see the note there for what
   activating it needs. */

/* names interned once when the operators install rather than on every fork,
   so a job that forks repeatedly does not re-intern them */
static Xpost_Object namedotforkcontext;
static Xpost_Object namedotchildrun;

/* -  currentcontext  context
   return current context identifier */
static
int xpost_op_currentcontext (Xpost_Context *ctx)
{
    Xpost_Object ctxobj = { 0 };
    ctxobj.mark_.tag = contexttype;
    ctxobj.mark_.padw = ctx->id;
    xpost_stack_push(ctx->lo, ctx->os, ctxobj);
    return 0;
}

/* True if this context has a save the program has not yet matched with a
   restore. PLRM 2nd ed 7.1 makes fork or join illegal then -- fork would give
   the child a share of VM a pending restore is poised to roll back, and join
   would splice a finished context's operands into it -- and both are to raise
   invalidcontext. save and restore act on local VM, so its save stack is what
   is asked; an empty stack is a context with nothing outstanding. */
static int _unmatched_save(Xpost_Context *ctx)
{
    return xpost_stack_count(ctx->lo,
                             xpost_memory_save_stack_ent(ctx->lo)) > 0;
}

/*
   mark obj1..objN proc  fork  context
   create context executing proc with obj1..objN as operands
*/
static
int xpost_op_fork (Xpost_Context *ctx, Xpost_Object proc)
{
    int cid, n, ret;
    Xpost_Context *newctx;

    /* illegal with a save the program has not matched (PLRM 2nd ed 7.1),
       and refused before any operand is read or context created */
    if (_unmatched_save(ctx))
        return invalidcontext;

    /* How many operands go to the new context is a question only a mark
       answers, and it is asked before anything is created so that a
       stack without one costs nothing. counttomark pushes no count when
       it finds no mark, so reading one regardless takes whatever operand
       is on top as the number of slots to hand over -- and the transfer
       then reads that many below the bottom of the stack and copies what
       it finds there into the new context. */
    ret = xpost_op_counttomark(ctx);
    if (ret)
        return ret;
    n = xpost_stack_pop(ctx->lo, ctx->os).int_.val;

    cid = xpost_context_fork3(ctx,
                              ctx->xpost_interpreter_cid_init,
                              ctx->gl->interpreter_cid_get_context,
                              ctx->xpost_interpreter_alloc_local_memory,
                              ctx->xpost_interpreter_alloc_global_memory,
                              ctx->garbage_collect_function);
    /* 0 is not a context id: it is what a fork with no free table slot
       answers. Indexing with it yields (0 - 1) % MAXCONTEXT, an
       unrelated slot. */
    if (cid == 0)
        return limitcheck;
    newctx = ctx->gl->interpreter_cid_get_context(cid);
    if (!newctx)
        return unregistered;

    /* Give the child its own private machinery dictionary so its graphics
       state can be its own (PLRM 2nd ed 7.1). fork3 shared the parent's by the
       struct copy; replace it with a local copy, which .forkcontext then gives
       a private graphics state. This is done here in C, not through
       .setprivatedict, whose once-only refusal guards the machinery against a
       program -- .setprivatedict is reachable through internaldict. */
    if (xpost_object_get_type(ctx->privatedict) == dicttype)
    {
        Xpost_Object npd = xpost_dict_cons_memory(newctx->lo,
                xpost_dict_max_length_memory(newctx->lo, ctx->privatedict));
        if (xpost_object_get_type(npd) == dicttype
            && xpost_op_dict_copy(ctx, ctx->privatedict, npd) == 0)
        {
            /* xpost_op_dict_copy is the copy operator: it leaves the filled
               dictionary on the operand stack. Take it back off -- the
               operands the child is about to be handed sit below it. */
            (void)xpost_stack_pop(ctx->lo, ctx->os);
            newctx->privatedict = npd;
        }
    }

    /* copy n objects to new context's operand stack */
    while (n--)
        xpost_stack_push(newctx->lo, newctx->os,
                         xpost_stack_topdown_fetch(ctx->lo, ctx->os, n));
    /* the mark counttomark found is still beneath the operands it
       counted: nothing above has been removed since, so the clear
       reaches it */
    XPOST_REFUSAL_IMPOSSIBLE(xpost_op_cleartomark(ctx));

    xpost_stack_push(newctx->lo, newctx->es, xpost_operator_cons(newctx, "_i_am_zombie_", NULL,0));
    /* The child runs its procedure through .childrun, which catches an error
       the procedure raises and reports it (handleerror) rather than letting the
       stop unwind into the interpreter with no stopped context to catch it. The
       procedure goes on the operand stack and .childrun on the exec stack above
       it; if .childrun is not loaded (early init), the procedure runs directly.
       .forkcontext, when graphics is loaded, runs first to give the child its
       own graphics state (PLRM 2nd ed 7.1: the graphics state is private). */
    {
        Xpost_Object cr = xpost_dict_get(ctx, ctx->globalprivatedict,
                                         namedotchildrun);
        Xpost_Object fc = xpost_dict_get(ctx, ctx->globalprivatedict,
                                         namedotforkcontext);
        if (xpost_object_get_type(cr) == arraytype)
        {
            xpost_stack_push(newctx->lo, newctx->os, proc);
            xpost_stack_push(newctx->lo, newctx->es, cr);
        }
        else
            xpost_stack_push(newctx->lo, newctx->es, proc);
        if (xpost_object_get_type(fc) == arraytype)
            xpost_stack_push(newctx->lo, newctx->es, fc);
    }
    newctx->state = C_RUN;
    {
        Xpost_Object ctxobj = { 0 };
        ctxobj.mark_.tag = contexttype;
        ctxobj.mark_.padw = newctx->id;
        xpost_stack_push(ctx->lo, ctx->os, ctxobj);
    }
    return contextswitch;
}

static
int _i_am_zombie_ (Xpost_Context *ctx)
{
    ctx->state = C_ZOMB;
    return contextswitch;
}

static
int _i_am_free_ (Xpost_Context *ctx)
{
    ctx->state = C_FREE;
    return contextswitch;
}

/* The context a valid identifier names, or NULL. A context identifier is
   an integer that means the same in every context (PLRM 2nd ed 7.1) and is
   valid only while the table slot it selects still holds the context that
   claimed it. interpreter_cid_get_context maps the identifier to a slot by
   (cid-1) % MAXCONTEXT and never fails, so a stale identifier -- one whose
   context has ended and whose slot a later fork reused -- would otherwise
   name that later context. Holding the slot's own id against the identifier
   rejects that: a reused slot carries a newer id, a freed one is C_FREE, and
   a fabricated or out-of-range identifier fails the same test. cid 0 is the
   no-context sentinel a full table returns and names no context. Callers
   answer NULL with invalidcontext, as PLRM requires of join and detach given
   an identifier that is not a valid context. */
static
Xpost_Context *_context_checked(Xpost_Context *ctx, unsigned int cid)
{
    Xpost_Context *c;
    if (cid == 0)
        return NULL;
    c = ctx->gl->interpreter_cid_get_context(cid);
    if (!c || c->id != cid || c->state == C_FREE)
        return NULL;
    return c;
}

/*
   context  join  mark obj1..objN
   await context termination and return its results
*/
/* the opcode of the join operator, captured when it is installed so the wait
   path below can reschedule join by opcode -- not by a name lookup a program
   could divert by redefining /join, and not through a standing reference the
   interpreter would have to capture whether or not the operators are
   installed. Held as an int, not an object, so it names nothing in VM for
   the collector to trace across a context's life. */
static int _join_opcode = -1;

static
int xpost_op_join (Xpost_Context *ctx, Xpost_Object context)
{
    Xpost_Context *child;
    /* illegal with a save the program has not matched (PLRM 2nd ed 7.1) */
    if (_unmatched_save(ctx))
        return invalidcontext;
    child = _context_checked(ctx, context.mark_.padw);
    /* an invalid identifier, or the current context joining itself, is
       invalidcontext (PLRM 2nd ed 7.1: join of an identifier that is not a
       valid context, or that identifies the current context) */
    if (!child || context.mark_.padw == ctx->id)
        return invalidcontext;
    if (child->state == C_ZOMB) {
        int i,n;
        xpost_stack_push(ctx->lo, ctx->os, mark);
        // Copy operand stack
        n = xpost_stack_count(child->lo, child->os);
        for (i = 0; i < n; i++)
            xpost_stack_push(ctx->lo, ctx->os,
                    xpost_stack_bottomup_fetch(child->lo, child->os, i));
        // Cleanup child
        child->state = C_FREE;
        return 0;
    }

    /* The child is alive but has not finished -- _context_checked above
       refused a freed or invalid identifier, so it is not gone. Wait for
       it: reschedule this join so that when the scheduler next makes this
       context current, after the child has had a turn, it re-checks the
       child's state. The scheduler moves this context out of C_WAIT when it
       is passed, so the re-check happens without any spin bound here. */
    xpost_stack_push(ctx->lo, ctx->os, context);
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_join_opcode));
    ctx->state = C_WAIT;
    return contextswitch;
}


/*
   -  yield  -
   suspend current context momentarily
*/
static
int xpost_op_yield (Xpost_Context *ctx)
{
    (void)ctx;
    return contextswitch;
}

/*
   context  detach  -
   enable context to terminate immediately when done
*/
static
int xpost_op_detach (Xpost_Context *ctx, Xpost_Object context)
{
    Xpost_Context *child = _context_checked(ctx, context.mark_.padw);
    /* an invalid identifier is invalidcontext (PLRM 2nd ed 7.1); detach of
       the current context is permitted, so no self check */
    if (!child)
        return invalidcontext;

    /* A context that has already run to its end sits in C_ZOMB waiting for
       a join it will now not get: free it and let its results go, which is
       what detach asks for (PLRM: there is no need for it to return
       results). */
    if (child->state == C_ZOMB)
    {
        child->state = C_FREE;
        return 0;
    }

    /* Still running: the zombie marker its start-up left at the bottom of
       its exec stack is what runs when its top-level procedure returns.
       Replace it so the context frees itself there instead of waiting.
       detach does not block, so the caller then continues. */
    if (!xpost_stack_bottomup_replace(child->lo, child->es, 0,
                                      xpost_operator_cons(child, "_i_am_free_",
                                                          NULL, 0)))
        return invalidaccess;
    return 0;
}

/*
   -  lock  lock
   create lock object

   lock proc  monitor  -
   execute proc while holding lock

   -  condition  condition
   create condition object

   lock condition  wait  -
   release lock, wait for condition, reacquire lock

   condition  notify  -
   resume contexts waiting for condition
*/

/* Whether a run has asked for the context operators to be installed. Off by
   default: fork/join/yield/detach/currentcontext are not standard base
   PostScript -- they are Display PostScript (PLRM 2nd ed 7.1) -- and the
   scheduler that gives them meaning is not yet driven, so a program reaches
   them only after an explicit opt-in. Set before xpost_create and read below
   while the operators are installed, as the render-parameter globals in
   xpost_interpreter.c are. */
static int _dps_ops_enabled = 0;

void
xpost_dps_set(int enable)
{
    _dps_ops_enabled = enable;
}

/* Whether the context operators were installed. The mainloop's context
   switcher reads this to keep its scheduling inert on a run that never
   asked for the operators -- there the context table holds only the one
   running context, so switching has nothing to choose anyway, but the
   check keeps a default run on exactly the path it had before. */
int
xpost_dps_enabled(void)
{
    return _dps_ops_enabled;
}

/* -  .dpsenabled  bool
   Whether the context operators were installed. callout.ps reads it once, as
   it loads, to pick the fast single-context graphics-dictionary accessor when
   they were not: only a forked context needs the accessor to resolve per
   context, and a context is forked only when the operators are installed. */
static
int op_dpsenabled (Xpost_Context *ctx)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(_dps_ops_enabled));
    return 0;
}

int xpost_oper_init_context_ops (Xpost_Context *ctx,
                                 Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    /* .dpsenabled is installed on every run -- callout.ps reads it to choose
       its graphics-dictionary accessor -- so it goes in before the opt-in gate */
    op = xpost_operator_cons(ctx, ".dpsenabled", (Xpost_Op_Func)op_dpsenabled, 0);
    INSTALL;

    /* the context operators themselves are installed only on opt-in; a default
       run leaves their names undefined */
    if (!_dps_ops_enabled)
        return 0;

    namedotforkcontext = xpost_name_cons(ctx, ".forkcontext");
    namedotchildrun = xpost_name_cons(ctx, ".childrun");
    op = xpost_operator_cons(ctx, "currentcontext", (Xpost_Op_Func)xpost_op_currentcontext, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "fork", (Xpost_Op_Func)xpost_op_fork, 1, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "_i_am_zombie_", (Xpost_Op_Func)_i_am_zombie_, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "_i_am_free_", (Xpost_Op_Func)_i_am_free_, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "join", (Xpost_Op_Func)xpost_op_join, 1, contexttype);
    INSTALL;
    _join_opcode = op.mark_.padw;   /* opcode, for the wait-reschedule inside join */
    op = xpost_operator_cons(ctx, "yield", (Xpost_Op_Func)xpost_op_yield, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "detach", (Xpost_Op_Func)xpost_op_detach, 1, contexttype);
    INSTALL;
    return 0;
}
