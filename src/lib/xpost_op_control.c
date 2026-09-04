#include <stdlib.h>
/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_control.c
 * @brief Installs the control operators.
 *
 * The implementations, and the one function that installs them.
 *
 * The control constructs of PLRM 3.5.4, which is what makes them
 * operators rather than syntax: a conditional takes procedures as
 * operands and executes one. Each is defined in PLRM 8.2.
 *
 * Installed into systemdict as:
 *
 * exec if ifelse for repeat loop exit stop stopped quit execstack countexecstack
 *
 * Control here is the execution stack: each of these pushes what is to be
 * done rather than calling it, so a loop is a frame and not a recursion.
 */

/* control operators */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdio.h> /* printf */

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include <string.h>

#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_dict.h"

#include "xpost_interpreter.h" /* the unwinding an error raised in PostScript needs */
#include "xpost_operator.h"
#include "xpost_op_type.h"
#include "xpost_op_context.h"
#include "xpost_op_control.h"

/* any  exec  -
   execute arbitrary object */
static
int xpost_op_any_exec (Xpost_Context *ctx,
                       Xpost_Object O)
{
    if (!xpost_op_exec_access_ok(ctx, O))
        return invalidaccess;
    if (!xpost_stack_push(ctx->lo, ctx->es, O))
        return execstackoverflow;
    return 0;
}

/* bool proc  if  -
   execute proc if bool is true */
static
int xpost_op_bool_proc_if (Xpost_Context *ctx,
                           Xpost_Object B,
                           Xpost_Object P)
{
    if (!xpost_op_exec_access_ok(ctx, P))
        return invalidaccess;
    if (B.int_.val)
        if (!xpost_stack_push(ctx->lo, ctx->es, P))
            return execstackoverflow;
    return 0;
}

/* bool proc1 proc2  ifelse  -
   execute proc1 if bool is true,
   proc2 if bool is false */
static
int xpost_op_bool_proc_proc_ifelse (Xpost_Context *ctx,
                                    Xpost_Object B,
                                    Xpost_Object Then,
                                    Xpost_Object Else)
{
    if (!xpost_op_exec_access_ok(ctx, Then) || !xpost_op_exec_access_ok(ctx, Else))
        return invalidaccess;
    if (B.int_.val)
    {
        if (!xpost_stack_push(ctx->lo, ctx->es, Then))
        {
            return execstackoverflow;
        }
    }
    else
    {
        if (!xpost_stack_push(ctx->lo, ctx->es, Else))
        {
            return execstackoverflow;
        }
    }
    return 0;
}

/* the control value's type follows initial and increment, not the limit
   (PLRM): an integer counter with a real limit still steps through integers,
   so read the limit as a real for the termination test without promoting the
   counter */
static double _for_limit(Xpost_Object lim)
{
    return xpost_object_number(lim);
}

/* initial increment limit proc  for  -
   execute proc with values from initial by steps
   of increment to limit */
static
int xpost_op_int_int_int_proc_for (Xpost_Context *ctx,
                                   Xpost_Object init,
                                   Xpost_Object incr,
                                   Xpost_Object lim,
              Xpost_Object P)
{
    integer i = init.int_.val;
    integer j = incr.int_.val;
    double n = _for_limit(lim);
    int up = j > 0;
    if (up? i > n : i < n) return 0;
    assert(ctx->gl->base);

    /* loop frame: the sentinel loop operator (which exit searches for)
       under literal state that the iterate operator updates in place.
       The frame goes on as one run resolving the exec stack's top
       segment once; fr is a C array, as the run requires. */
    {
        Xpost_Object fr[7];

        fr[0] = XPOST_OP(ctx, opfor);
        fr[1] = xpost_object_cvlit(P);
        fr[2] = incr;
        fr[3] = lim;
        fr[4] = xpost_int_cons(i + j);
        fr[5] = XPOST_OP(ctx, forcont);
        fr[6] = P;
        if (!xpost_stack_push_run(ctx->lo, ctx->es, fr, 7))
            return execstackoverflow;
    }
    if (!xpost_stack_push(ctx->lo, ctx->os, init))
        return stackoverflow;

    return 0;
}

/* the control value after this iteration: integer counters advance in
   exact integer arithmetic, real counters in real (the control value's
   type was fixed by the for operator from initial and increment) */
static Xpost_Object _for_next(Xpost_Object i, Xpost_Object incr)
{
    if (xpost_object_get_type(i) == realtype)
        return xpost_real_cons(i.real_.val + incr.real_.val);
    return xpost_int_cons(i.int_.val + incr.int_.val);
}

static int _for_done(Xpost_Object i, Xpost_Object incr, Xpost_Object lim)
{
    return xpost_object_number(incr) > 0
        ? xpost_object_number(i) > _for_limit(lim)
        : xpost_object_number(i) < _for_limit(lim);
}

/* continue a for loop, integer or real: es holds (from the top) the
   next value, the limit, the increment, the literal proc, and the
   sentinel */
static
int xpost_op_for_iterate (Xpost_Context *ctx)
{
    Xpost_Stack *root = xpost_stack_at(ctx->lo, ctx->es);
    Xpost_Stack *top = xpost_stack_at(ctx->lo, root->prevseg);
    Xpost_Object i, lim, incr, P;

    if (top->top >= 5 && top->top < XPOST_STACK_SEGMENT_SIZE - 2)
    {
        i    = top->data[top->top - 1];
        lim  = top->data[top->top - 2];
        incr = top->data[top->top - 3];
        P    = top->data[top->top - 4];
        if (_for_done(i, incr, lim))
        {
            top->top -= 5; /* drop the frame */
            return 0;
        }
        if (!xpost_stack_push(ctx->lo, ctx->os, i))
            return stackoverflow;
        /* the push may grow the memory file and move its base:
           re-derive the frame pointers before writing through them */
        root = xpost_stack_at(ctx->lo, ctx->es);
        top = xpost_stack_at(ctx->lo, root->prevseg);
        top->data[top->top - 1] = _for_next(i, incr);
        top->data[top->top]     = XPOST_OP(ctx, forcont);
        top->data[top->top + 1] = xpost_object_cvx(P);
        top->top += 2;
        return 0;
    }

    i    = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 0);
    lim  = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 1);
    incr = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 2);
    P    = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 3);
    if (xpost_object_get_type(i) == invalidtype)
        return execstackunderflow;
    if (_for_done(i, incr, lim))
    {
        int k;
        for (k = 0; k < 5; k++)
            (void)xpost_stack_pop(ctx->lo, ctx->es);
        return 0;
    }
    if (!xpost_stack_push(ctx->lo, ctx->os, i))
        return stackoverflow;
    if (!xpost_stack_topdown_replace(ctx->lo, ctx->es, 0,
                                     _for_next(i, incr)))
        return execstackunderflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          XPOST_OP(ctx, forcont)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvx(P)))
        return execstackoverflow;
    return 0;
}

/* same as IIIPfor but for reals: the same loop frame, with real state,
   driven by the same polymorphic iterate operator */
static
int xpost_op_real_real_real_proc_for (Xpost_Context *ctx,
                                      Xpost_Object init,
                                      Xpost_Object incr,
                                      Xpost_Object lim,
                                      Xpost_Object P)
{
    real i = init.real_.val;
    real j = incr.real_.val;
    real n = lim.real_.val;
    int up = j > 0;
    if (up? i > n : i < n) return 0;

    /* the same loop frame as the integer variant, placed as one run */
    {
        Xpost_Object fr[7];

        fr[0] = XPOST_OP(ctx, opfor);
        fr[1] = xpost_object_cvlit(P);
        fr[2] = incr;
        fr[3] = lim;
        fr[4] = xpost_real_cons(i + j);
        fr[5] = XPOST_OP(ctx, forcont);
        fr[6] = P;
        if (!xpost_stack_push_run(ctx->lo, ctx->es, fr, 7))
            return execstackoverflow;
    }
    if (!xpost_stack_push(ctx->lo, ctx->os, init))
        return stackoverflow;
    return 0;
}

/* int proc  repeat  -
   execute proc int times */
static
int xpost_op_int_proc_repeat (Xpost_Context *ctx,
                              Xpost_Object n,
                              Xpost_Object P)
{
    /* PLRM: the count must be a nonnegative integer -- a negative one is
       rangecheck, not a silent no-op; zero legitimately does nothing */
    if (n.int_.val < 0) return rangecheck;
    if (n.int_.val == 0) return 0;

    /* loop frame, as for the for operator, placed as one run */
    {
        Xpost_Object fr[5];

        fr[0] = XPOST_OP(ctx, repeat);
        fr[1] = xpost_object_cvlit(P);
        fr[2] = xpost_int_cons(n.int_.val - 1);
        fr[3] = XPOST_OP(ctx, repeatcont);
        fr[4] = P;
        if (!xpost_stack_push_run(ctx->lo, ctx->es, fr, 5))
            return execstackoverflow;
    }

    return 0;
}

/* continue a repeat loop: es holds (from the top) the remaining
   count, the literal proc, and the sentinel */
static
int xpost_op_repeat_iterate (Xpost_Context *ctx)
{
    Xpost_Stack *root = xpost_stack_at(ctx->lo, ctx->es);
    Xpost_Stack *top = xpost_stack_at(ctx->lo, root->prevseg);
    Xpost_Object n, P;

    if (top->top >= 3 && top->top < XPOST_STACK_SEGMENT_SIZE - 2)
    {
        n = top->data[top->top - 1];
        P = top->data[top->top - 2];
        if (n.int_.val <= 0)
        {
            top->top -= 3; /* drop the frame */
            return 0;
        }
        top->data[top->top - 1] = xpost_int_cons(n.int_.val - 1);
        top->data[top->top]     = XPOST_OP(ctx, repeatcont);
        top->data[top->top + 1] = xpost_object_cvx(P);
        top->top += 2;
        return 0;
    }

    n = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 0);
    P = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 1);
    if (xpost_object_get_type(n) == invalidtype)
        return execstackunderflow;
    if (n.int_.val <= 0)
    {
        int k;
        for (k = 0; k < 3; k++)
            (void)xpost_stack_pop(ctx->lo, ctx->es);
        return 0;
    }
    if (!xpost_stack_topdown_replace(ctx->lo, ctx->es, 0,
                                     xpost_int_cons(n.int_.val - 1)))
        return execstackunderflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          XPOST_OP(ctx, repeatcont)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvx(P)))
        return execstackoverflow;
    return 0;
}

/* proc  loop  -
   execute proc an indefinite number of times */
static
int xpost_op_proc_loop (Xpost_Context *ctx,
                        Xpost_Object P)
{
    /* loop frame, as for the for operator, placed as one run */
    {
        Xpost_Object fr[4];

        fr[0] = XPOST_OP(ctx, loop);
        fr[1] = xpost_object_cvlit(P);
        fr[2] = XPOST_OP(ctx, loopcont);
        fr[3] = P;
        if (!xpost_stack_push_run(ctx->lo, ctx->es, fr, 4))
            return execstackoverflow;
    }
    return 0;
}

/* continue a loop: es holds (from the top) the literal proc and the
   sentinel; only exit or stop ends the loop */
static
int xpost_op_loop_iterate (Xpost_Context *ctx)
{
    Xpost_Stack *root = xpost_stack_at(ctx->lo, ctx->es);
    Xpost_Stack *top = xpost_stack_at(ctx->lo, root->prevseg);
    Xpost_Object P;

    if (top->top >= 2 && top->top < XPOST_STACK_SEGMENT_SIZE - 2)
    {
        P = top->data[top->top - 1];
        top->data[top->top]     = XPOST_OP(ctx, loopcont);
        top->data[top->top + 1] = xpost_object_cvx(P);
        top->top += 2;
        return 0;
    }

    P = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 0);
    if (xpost_object_get_type(P) == invalidtype)
        return execstackunderflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          XPOST_OP(ctx, loopcont)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvx(P)))
        return execstackoverflow;
    return 0;
}

/* -  exit  -
   exit innermost active loop */
static
int xpost_op_exit (Xpost_Context *ctx)
{
    Xpost_Object opfor = XPOST_OP(ctx, opfor);
    Xpost_Object oprepeat = XPOST_OP(ctx, repeat);
    Xpost_Object oploop = XPOST_OP(ctx, loop);
    Xpost_Object opforall = XPOST_OP(ctx, forall);
    Xpost_Object opfilenameforall = XPOST_OP(ctx, filenameforall);
    Xpost_Object x;


    /* Look for the sentinel before disturbing anything: exit with no
       enclosing looping context is invalidexit (PLRM 8.2), and the
       program's execution stack -- including any stopped context that
       will catch the error -- has to survive to receive it. Unwinding
       first and discovering the absence afterwards destroys exactly the
       frames the error needs. */
    {
        int depth = xpost_stack_count(ctx->lo, ctx->es);
        int i;
        int found = 0;

        for (i = 0; i < depth; i++)
        {
            Xpost_Object t = xpost_stack_topdown_fetch(ctx->lo, ctx->es, i);

            /* a stopped context bounds the search: the false that
               stopped pushed marks it, and exit does not pass one
               (PLRM 8.2). A loop outside it keeps running. */
            if (xpost_object_get_type(t) == booleantype && !t.int_.val)
                break;

            if ((xpost_dict_compare_objects(ctx, t, opfor)    == 0) ||
                (xpost_dict_compare_objects(ctx, t, oprepeat) == 0) ||
                (xpost_dict_compare_objects(ctx, t, oploop)   == 0) ||
                (xpost_dict_compare_objects(ctx, t, opforall) == 0) ||
                (xpost_dict_compare_objects(ctx, t, opfilenameforall) == 0))
            {
                found = 1;
                break;
            }
        }
        if (!found)
            return invalidexit;
    }

    while (1) {
        x = xpost_stack_pop(ctx->lo, ctx->es);
        if (xpost_object_get_type(x) == invalidtype)
            return execstackunderflow;
        if (xpost_object_get_type(x) == globtype)
        {
            /* filenameforall state unwinding with its frame: the matched
               paths are not in VM, so give them back here or never */
            xpost_context_glob_release(ctx, (unsigned int)x.glob_.id);
            continue;
        }
        if (xpost_object_get_type(x) == operatortype &&
            (x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapdone) ||
             x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapsealed)))
        {
            /* a wrapped call going with the loop: its finish marker will
               never run, so the operands saved for it are let go here */
            xpost_operator_wrapped_release(ctx,
                                           xpost_stack_pop(ctx->lo, ctx->es));
            continue;
        }
        if ((xpost_dict_compare_objects(ctx, x, opfor)    == 0) ||
            (xpost_dict_compare_objects(ctx, x, oprepeat) == 0) ||
            (xpost_dict_compare_objects(ctx, x, oploop)   == 0) ||
            (xpost_dict_compare_objects(ctx, x, opforall) == 0) ||
            (xpost_dict_compare_objects(ctx, x, opfilenameforall) == 0))
        {
            break;
        }
    }

    return 0;
}

/* record what ended the run for the embedding caller. $error is the
   authority: a program may raise through the error machinery or set
   $error and stop directly, and either way its errorname and errorinfo
   describe the failure. $error is a name in systemdict (its dictionary is
   local), read from the base of the dict stack so a program's own
   dictionaries above it do not shadow it. */
void xpost_op_record_run_error(Xpost_Context *ctx)
{
    {
        Xpost_Object sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
        Xpost_Object derr = xpost_dict_get(ctx, sd,
                xpost_name_cons(ctx, "$error"));
        if (xpost_object_get_type(derr) == dicttype)
        {
            Xpost_Object nm = xpost_dict_get(ctx, derr,
                    xpost_name_cons(ctx, "errorname"));
            Xpost_Object info = xpost_dict_get(ctx, derr,
                    xpost_name_cons(ctx, "errorinfo"));
            if (xpost_object_get_type(nm) == nametype)
                nm = xpost_name_get_string(ctx, nm);
            if (xpost_object_get_type(nm) == stringtype)
            {
                unsigned int n = nm.comp_.sz;

                /* the embedding caller reads the name as C text and
                   matches it against the error it is watching for, so a
                   name that cannot be given whole -- one longer than the
                   field, or one carrying a nul, which C text ends at --
                   would be handed over as the shorter name it begins
                   with and answer to an error the run never raised. A
                   name counts its characters (PLRM 3.3); one this cannot
                   carry is reported as the catch-all instead, which is
                   what this interpreter says when it cannot be more
                   specific (xpost_error.h) */
                if (n > sizeof ctx->run_error_name - 1
                    || !xpost_string_is_cstring(ctx, nm))
                {
                    strncpy(ctx->run_error_name, errorname[unknownerror],
                            sizeof ctx->run_error_name - 1);
                    ctx->run_error_name[sizeof ctx->run_error_name - 1] = '\0';
                }
                else
                {
                    memcpy(ctx->run_error_name,
                           xpost_string_get_pointer(ctx, nm), n);
                    ctx->run_error_name[n] = '\0';
                }
            }
            if (xpost_object_get_type(info) == stringtype)
            {
                unsigned int n = info.comp_.sz;
                if (n > sizeof ctx->run_error_info - 1)
                    n = sizeof ctx->run_error_info - 1;
                memcpy(ctx->run_error_info,
                       xpost_string_get_pointer(ctx, info), n);
                ctx->run_error_info[n] = '\0';
            }
        }
    }
    ctx->run_uncaught = 1;
}

/* The stopped context is a boolean 'false' on the exec stack,
   so normal execution simply falls through and pushes the
   false onto the operand stack. 'stop' then merely has to
   search for 'false' and push a 'true', popping as it goes.  */

/* -  stop  -
   terminate stopped context */
int xpost_op_stop(Xpost_Context *ctx)
{
    Xpost_Object f = xpost_bool_cons(0);
    Xpost_Object x;
    /* Reaching `stop` means the error machinery ran to completion and the
       run is recovering (or quitting cleanly): the error cascade, if any,
       has broken. Clear the consecutive-error count that _onerror keeps. */
    ctx->onerr_run = 0;
    /* A wrapped operator between here and that context is abandoned
       part-way through, so it leaves nothing behind and its caller has
       its operands back (PLRM 3.11.1 step 1). This is where every way
       of abandoning one arrives: an error the interpreter raised, an
       error a body raised with signalerror, and a failure a body caught
       in a stopped context of its own and raised again with a bare
       stop, which passes no error hook at all and so is reached here
       only. The raise sites that unwind before recording $error do it
       again here, over the same frames, to the same end. A call the
       failure happened in a procedure of, rather than in the operator
       itself, is left alone: the boundary beneath that procedure ends
       the walk. */
    (void)xpost_op_errorunwind(ctx);
    /* Unwind the exec stack to the nearest enclosing stopped context --
       the false that `stopped` pushed. Pop straight to it: counting the
       whole stack first to bound the loop is O(depth) yet the marker is
       usually a few frames down, and an emptied stack (the pop yields
       invalidtype) is itself the no-context case handled below. */
    for (;;)
    {
        x = xpost_stack_pop(ctx->lo, ctx->es);
        if (xpost_object_get_type(x) == invalidtype)
            break;
        if (xpost_object_get_type(x) == globtype)
        {
            /* a filenameforall frame unwinding with the stopped context:
               give back its matched paths as exit does */
            xpost_context_glob_release(ctx, (unsigned int)x.glob_.id);
            continue;
        }
        if (xpost_object_get_type(x) == operatortype &&
            (x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapdone) ||
             x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapsealed)))
        {
            /* a wrapped call going with the stopped context: its finish
               marker will never run, so the operands saved for it are
               let go here instead */
            xpost_operator_wrapped_release(ctx,
                                           xpost_stack_pop(ctx->lo, ctx->es));
            continue;
        }
        if(xpost_dict_compare_objects(ctx, f, x) == 0) {
            if (!xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1)))
                return stackoverflow;
            return 0;
        }
    }
    /* PLRM: stop with no enclosing stopped context prints a message
       and executes quit.  Returning an error here would re-enter
       errordict, whose handlers themselves finish with `stop`,
       recursing without bound. */
    XPOST_LOG_ERR("no stopped context in 'stop'");
    xpost_op_record_run_error(ctx);
    ctx->quit = 1;
    return 0;
}

/* -  wrap.done  -
   -  wrap.sealed  -
   the finish marker of a wrapped-operator call: the recorded
   procedure ran to completion, so the frame beneath the marker --
   the saved operands, the dict and operand depths at the call and the
   operator itself -- leaves the exec stack with it. The call ended the
   way it meant to, so the copies taken against its failing go too.
   The sealed spelling marks a call a failure left through a boundary,
   and reads the same to everything that discards a frame. */
static
int xpost_op_wrapdone(Xpost_Context *ctx)
{
    xpost_operator_wrapped_release(ctx, xpost_stack_pop(ctx->lo, ctx->es));
    (void)xpost_stack_pop(ctx->lo, ctx->es);
    (void)xpost_stack_pop(ctx->lo, ctx->es);
    (void)xpost_stack_pop(ctx->lo, ctx->es);
    return 0;
}

/* -  callout.done  -
   the boundary of a call back into a procedure of the program's: it
   marks how far up the execution stack the failure of that procedure
   belongs. Above it the object being executed is the program's own
   procedure, so an error restores the operand stack to what that
   procedure found (PLRM 3.11.1); the operator that called back sits
   below, its operands consumed by work it did before calling. It
   carries nothing, so reaching it in the ordinary way ends it. */
static
int xpost_op_calloutdone(Xpost_Context *ctx)
{
    (void)ctx;
    return 0;
}

/* The two names the bracket reads: the graphics state stack's depth,
   which it puts back, and the operator that puts it back. Interned once
   at registration rather than per call. */
static Xpost_Object name_gptr;
static Xpost_Object name_grestore;

/* the depth the graphics state stack stands at */
static
integer _callout_gsdepth(Xpost_Context *ctx)
{
    Xpost_Object v;

    if (xpost_object_get_type(ctx->graphicsdict) != dicttype)
        return 0;
    v = xpost_dict_get(ctx, ctx->graphicsdict, name_gptr);
    return (xpost_object_get_type(v) == integertype) ? v.int_.val : 0;
}

/* grestore, which is written in PostScript and so is reached as the
   operator the promotion left in systemdict rather than called */
static
Xpost_Object _callout_grestore(Xpost_Context *ctx)
{
    int n = (int)xpost_stack_count(ctx->lo, ctx->ds);
    int i;

    /* Looked up the way a name is looked up, from the top of the
       dictionary stack down, rather than assumed to sit in whichever
       dictionary is at the bottom: the bracket runs with the caller's
       stack, and a caller that has taken dictionaries off has moved
       what is where. */
    for (i = 0; i < n; i++)
    {
        Xpost_Object d = xpost_stack_topdown_fetch(ctx->lo, ctx->ds, i);
        Xpost_Object v;

        if (xpost_object_get_type(d) != dicttype)
            continue;
        v = xpost_dict_get(ctx, d, name_grestore);
        if (xpost_object_get_type(v) != invalidtype)
            return v;
    }
    return null;
}

/* --- the callout bracket ---------------------------------------------

   The bracket the machinery runs a procedure of the program's under.
   It hides the machinery's own dictionaries, remembers what the caller
   parked in the graphics dictionary, and puts both back however the
   procedure ends -- returning, or failing and being raised again.

   This is mechanism rather than language: no program can call it or see
   it, and what makes it correct is the depth of two stacks rather than
   anything the manual describes (doc/xpost_design.dox, "What is C and
   what is PostScript"). What it protects, though, is policy and stays
   with the caller: the caller says which slots it parked in and how
   many of its dictionaries stand between the procedure and the
   program's scope.

   The frame rides the execution stack beneath the procedure, the way a
   wrapped operator's frame does, so that nothing of the bracket's lies
   on the operand stack the procedure takes its operands from and leaves
   its results on. A stopped context -- the boolean false the stopped
   operator leaves -- sits between the frame and the procedure, so a
   failure unwinds to there and no further, and the continuation beneath
   reads that boolean to learn which way the procedure ended.

   MEASURED: the same bracket written in PostScript built a program of
   eighteen elements per call and allocated twice. An image whose
   run-length data source is called once per run spent seventy per cent
   of its whole run inside it. */

/* Take down what each named slot of the graphics dictionary holds, so
   that a procedure re-entering the machinery and writing the same slots
   cannot cost the caller what it parked there. A slot that is not there
   on the way in is recorded as absent rather than as null, which is a
   value a slot may legitimately hold. */
static
int _callout_save_slots(Xpost_Context *ctx, Xpost_Object keys,
                        Xpost_Object vals, Xpost_Object absent)
{
    int n = keys.comp_.sz;
    int i;

    for (i = 0; i < n; i++)
    {
        Xpost_Object k = xpost_array_get(ctx, keys, i);
        Xpost_Object v = xpost_dict_get(ctx, ctx->graphicsdict, k);

        if (xpost_object_get_type(v) == invalidtype)
            v = absent;
        if (xpost_array_put(ctx, vals, i, v))
            return unregistered;
    }
    return 0;
}

/* The undo of the above: each slot gets its value back, and a slot that
   was absent is undefined again rather than left holding one. */
static
int _callout_restore_slots(Xpost_Context *ctx, Xpost_Object keys,
                           Xpost_Object vals, Xpost_Object absent)
{
    int n = keys.comp_.sz;
    int i;

    for (i = 0; i < n; i++)
    {
        Xpost_Object k = xpost_array_get(ctx, keys, i);
        Xpost_Object v = xpost_array_get(ctx, vals, i);
        int ret;

        if (xpost_dict_compare_objects(ctx, v, absent) == 0)
        {
            /* A slot absent on the way in is absent again. Undefining a
               key the dictionary does not hold is not a failure here:
               the procedure may never have defined it, and PLRM 3.3.9
               has undef ignore a key that is not there. */
            ret = xpost_dict_undef(ctx, ctx->graphicsdict, k);
            if (ret == undefined)
                ret = 0;
        }
        else
            ret = xpost_dict_put(ctx, ctx->graphicsdict, k, v);
        if (ret)
            return ret;
    }
    return 0;
}

/* {proc} keys ndict absent  .calloutframe  -
   Run the program's procedure under the bracket. keys names the graphics
   dictionary slots to put back afterwards and may be null; ndict is how
   many of the machinery's own dictionaries stand between the procedure
   and the program's scope, and come off before it runs; absent is what
   stands for a slot that was not there on the way in.

   The frame is pushed object by object rather than gathered into an
   array, so that the bracket allocates nothing at all where the caller
   names no slots -- which is every caller but one. Nothing allocated
   means nothing to keep the collector's hands off between the taking
   down and the frame reaching the execution stack, which is a root. */
static
int xpost_op_callout(Xpost_Context *ctx,
                     Xpost_Object P,
                     Xpost_Object keys,
                     Xpost_Object N,
                     Xpost_Object absent)
{
    Xpost_Object ov;
    int nd = N.int_.val;
    int nk = 0;
    int i;

    if (nd < 0)
        return rangecheck;
    if (xpost_object_get_type(keys) == arraytype)
        nk = keys.comp_.sz;
    else if (xpost_object_get_type(keys) != nulltype)
        return typecheck;
    if (nd > (int)xpost_stack_count(ctx->lo, ctx->ds))
        return dictstackunderflow;

    /* What the named slots hold on the way in. The only allocation the
       bracket makes, and it is made before anything is taken down: from
       here to the frame going on there is nothing that can collect. */
    ov = null;
    if (nk)
    {
        int ret;

        ov = xpost_object_cvlit(xpost_array_cons(ctx, (unsigned int)nk));
        if (xpost_object_get_type(ov) != arraytype)
            return VMerror;
        if (!xpost_stack_push(ctx->lo, ctx->hold, ov))
            return stackoverflow;
        ret = _callout_save_slots(ctx, keys, ov, absent);
        if (ret)
            return ret;
    }

    if (!xpost_stack_push(ctx->lo, ctx->es, absent) ||
        !xpost_stack_push(ctx->lo, ctx->es, ov) ||
        !xpost_stack_push(ctx->lo, ctx->es, keys))
        return execstackoverflow;

    /* The machinery's own dictionaries, innermost taken first and so
       lying deepest, which leaves the continuation popping them
       outermost first -- the order they must go back on. */
    /* Every one of them reaches the execution stack before any leaves the
       dictionary stack. A dictionary taken off and then refused room
       would be on neither, and the dictionary stack it came from is the
       one a caught error carries on running against. */
    for (i = 0; i < nd; i++)
    {
        Xpost_Object d = xpost_stack_topdown_fetch(ctx->lo, ctx->ds, i);

        if (xpost_object_get_type(d) == invalidtype)
            return dictstackunderflow;
        if (!xpost_stack_push(ctx->lo, ctx->es, d))
            return execstackoverflow;
    }
    for (i = 0; i < nd; i++)
        (void)xpost_stack_pop(ctx->lo, ctx->ds);

    /* Taking a dictionary off changes what every name sees, which is
       what begin and end bump this for; the bracket moves the stack
       without going through them, so it says so itself. A cache keyed on
       the generation would otherwise answer for the scope that stood
       before the machinery's dictionaries came off. */
    if (nd)
        ++ctx->namebind_gen;

    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_int_cons(nd)) ||
        !xpost_stack_push(ctx->lo, ctx->es,
                          xpost_int_cons(_callout_gsdepth(ctx))) ||
        !xpost_stack_push(ctx->lo, ctx->es,
                          xpost_int_cons((integer)xpost_stack_count(ctx->lo,
                                                                   ctx->ds))) ||
        !xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, calloutunwind)) ||
        /* the stopped context: a failure unwinds to here and no further,
           and the continuation reads the boolean it leaves to learn
           which way the procedure ended */
        !xpost_stack_push(ctx->lo, ctx->es, xpost_bool_cons(0)) ||
        !xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvx(P)))
        return execstackoverflow;
    return 0;
}

/* -  callout.unwind  -
   put back what the bracket took down, and raise again what the
   procedure raised. */
static
int xpost_op_calloutunwind(Xpost_Context *ctx)
{
    Xpost_Object stopped_, dd, gp, nd, keys, ov, absent;
    int gsnow, want, i, ret, moved;

    stopped_ = xpost_stack_pop(ctx->lo, ctx->os);
    if (xpost_object_get_type(stopped_) != booleantype)
        return unregistered;
    dd = xpost_stack_pop(ctx->lo, ctx->es);
    gp = xpost_stack_pop(ctx->lo, ctx->es);
    nd = xpost_stack_pop(ctx->lo, ctx->es);
    if (xpost_object_get_type(nd) != integertype)
        return unregistered;

    /* A gsave the procedure left open -- or one the machinery's own
       setup left open inside the bracket -- is closed, and the state it
       saved put back. grestore is written in PostScript, so this reaches
       it as the operator the promotion left in systemdict and comes back
       here to finish; the common case is a depth that never moved. */
    gsnow = _callout_gsdepth(ctx);
    want = gp.int_.val;
    if (gsnow > want)
    {
        Xpost_Object gr = _callout_grestore(ctx);

        /* grestore is whatever the language has made of it -- the
           operator the promotion leaves, or the procedure it is before
           that -- and either runs from the execution stack. What must
           not be scheduled is a name that answered with nothing. */
        if (xpost_object_get_type(gr) == invalidtype ||
            xpost_object_get_type(gr) == nulltype)
            return undefined;
        gr = xpost_object_cvx(gr);
        if (!xpost_stack_push(ctx->lo, ctx->es, nd) ||
            !xpost_stack_push(ctx->lo, ctx->es, gp) ||
            !xpost_stack_push(ctx->lo, ctx->es, dd) ||
            !xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, calloutunwind)))
            return execstackoverflow;
        for (i = gsnow; i > want; i--)
            if (!xpost_stack_push(ctx->lo, ctx->es, gr))
                return execstackoverflow;
        if (!xpost_stack_push(ctx->lo, ctx->os, stopped_))
            return stackoverflow;
        return 0;
    }

    /* end until the dictionary stack stands where it stood while the
       procedure ran, then put the machinery's own back on, outermost
       first, which is the order they come off the frame */
    moved = ((integer)xpost_stack_count(ctx->lo, ctx->ds) != dd.int_.val)
            || nd.int_.val;
    while ((integer)xpost_stack_count(ctx->lo, ctx->ds) > dd.int_.val)
        if (xpost_object_get_type(xpost_stack_pop(ctx->lo, ctx->ds)) ==
            invalidtype)
            return dictstackunderflow;
    for (i = 0; i < nd.int_.val; i++)
    {
        Xpost_Object d = xpost_stack_pop(ctx->lo, ctx->es);

        if (xpost_object_get_type(d) != dicttype)
            return unregistered;
        if (!xpost_stack_push(ctx->lo, ctx->ds, d))
            return dictstackoverflow;
    }

    /* the scope is the caller's again, which every name must see: the
       bracket moved the dictionary stack without going through begin and
       end, so it bumps the generation they bump. Asked rather than
       assumed, because a bracket over no dictionaries that the procedure
       left alone has changed nothing and need not spend the cache. */
    if (moved)
        ++ctx->namebind_gen;

    keys = xpost_stack_pop(ctx->lo, ctx->es);
    ov = xpost_stack_pop(ctx->lo, ctx->es);
    absent = xpost_stack_pop(ctx->lo, ctx->es);
    if (xpost_object_get_type(ov) == arraytype && ov.comp_.sz)
    {
        ret = _callout_restore_slots(ctx, keys, ov, absent);
        if (ret)
            return ret;
    }

    /* what failed in there is raised again, so the caller's own error
       handling sees the error the procedure raised */
    if (stopped_.int_.val)
        return xpost_op_stop(ctx);
    return 0;
}

/* proc  .coexec  -
   run proc with that boundary beneath it. The boundary leaves the
   execution stack with proc, whether proc returns or is abandoned. */
static
int xpost_op_proc_coexec(Xpost_Context *ctx,
                         Xpost_Object P)
{
    if (!xpost_op_exec_access_ok(ctx, P))
        return invalidaccess;
    if (!xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, calloutdone)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, P))
    {
        (void)xpost_stack_pop(ctx->lo, ctx->es);
        return execstackoverflow;
    }
    return 0;
}

/* name proc  .wrapop  operator
   install an operator that runs the procedure. A procedure that
   implements a standard operator becomes indistinguishable from a
   C-coded one: load answers operatortype and bind substitutes it.
   The procedure must stay reachable elsewhere; the operator table
   is outside the collector's view. */
static
int xpost_op_wrapop(Xpost_Context *ctx,
                    Xpost_Object name,
                    Xpost_Object proc)
{
    Xpost_Object o;

    o = xpost_operator_cons_wrapped(ctx, name, proc, 0, NULL);
    if (xpost_object_get_type(o) != operatortype)
        return unregistered;
    if (!xpost_stack_push(ctx->lo, ctx->os, o))
        return stackoverflow;
    return 0;
}

/* read one operand shape: an array naming a type per operand, in
   operand order */
static
int _read_operand_shape(Xpost_Context *ctx,
                        Xpost_Object sig,
                        Xpost_Wrapped_Signature *out)
{
    int n = sig.comp_.sz;
    int i;

    if (n > XPOST_OPERATOR_MAX_SIG)
        return limitcheck;
    for (i = 0; i < n; i++)
    {
        Xpost_Object el = xpost_array_get(ctx, sig, i);
        int t;

        if (xpost_object_get_type(el) != nametype)
            return typecheck;
        t = xpost_op_type_code(ctx, el);
        if (t < 0)
            return rangecheck;
        /* the dispatcher reads its type pattern from the top of the
           stack down, and the array reads bottom up */
        out->types[n - 1 - i] = (byte)t;
    }
    out->in = n;
    return 0;
}

/* name proc array  .wrapopsig  operator
   install an operator that runs the procedure, stating the operands it
   takes. The array names one type per operand in operand order, using
   the names the type operator answers plus numbertype for either
   number, proctype for an executable array, and anytype for no
   restriction. The dispatcher enforces the statement before the
   procedure runs, exactly as for an operator written in C.

   An operator that takes more than one shape of operand list states
   each as an array of its own, and gives the array of those. The
   dispatcher tries them in the order stated, so a longer shape stated
   before a shorter one it extends gets first refusal. A shape may
   state fewer operands than the operator consumes, which is all a
   variadic operator can say: the types stated are the ones nearest the
   top of the stack, and the procedure answers for the rest. */
static
int xpost_op_wrapopsig(Xpost_Context *ctx,
                       Xpost_Object name,
                       Xpost_Object proc,
                       Xpost_Object sig)
{
    Xpost_Object o;
    Xpost_Wrapped_Signature sigs[XPOST_OPERATOR_MAX_ALT];
    int n;
    int i;
    int ret;

    /* one shape names types, and a list of shapes holds arrays */
    if ((sig.comp_.sz > 0) &&
        (xpost_object_get_type(xpost_array_get(ctx, sig, 0)) == arraytype))
    {
        n = sig.comp_.sz;
        if (n > XPOST_OPERATOR_MAX_ALT)
            return limitcheck;
        for (i = 0; i < n; i++)
        {
            Xpost_Object alt = xpost_array_get(ctx, sig, i);

            if (xpost_object_get_type(alt) != arraytype)
                return typecheck;
            if ((ret = _read_operand_shape(ctx, alt, &sigs[i])) != 0)
                return ret;
        }
    }
    else
    {
        n = 1;
        if ((ret = _read_operand_shape(ctx, sig, &sigs[0])) != 0)
            return ret;
    }

    o = xpost_operator_cons_wrapped(ctx, name, proc, n, sigs);
    if (xpost_object_get_type(o) != operatortype)
        return unregistered;
    if (!xpost_stack_push(ctx->lo, ctx->os, o))
        return stackoverflow;
    return 0;
}

/* -  .rundied  -
   the run's scheduling guard caught an error that the program did not:
   record it so the embedding caller sees the run as errored */
static
int xpost_op_rundied(Xpost_Context *ctx)
{
    xpost_op_record_run_error(ctx);
    return 0;
}

/* any  stopped  bool
   establish context for catching stop */
static
int xpost_op_any_stopped(Xpost_Context *ctx,
                         Xpost_Object o)
{
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_bool_cons(0)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, o))
        return execstackoverflow;
    return 0;
}

/* -  countexecstack  int
   count elements on execution stack */
static
int xpost_op_countexecstack(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(xpost_stack_count(ctx->lo, ctx->es))))
        return stackoverflow;
    return 0;
}

/* array  execstack  subarray
   copy execution stack into array */
static
int xpost_op_array_execstack(Xpost_Context *ctx,
                             Xpost_Object A)
{
    Xpost_Object subarr;
    int z = xpost_stack_count(ctx->lo, ctx->es);
    int i;
    for (i = 0; i < z; i++)
    {
        int ret;
        ret = xpost_array_put(ctx, A, i, xpost_stack_bottomup_fetch(ctx->lo, ctx->es, i));
        if (ret)
            return ret;
    }
    subarr = xpost_object_get_interval(A, 0, z);
    if (xpost_object_get_type(subarr) == invalidtype)
        return rangecheck;
    if (!xpost_stack_push(ctx->lo, ctx->os, subarr))
        return stackoverflow;
    return 0;
}

/* -  quit  -
   terminate interpreter */
static
int xpost_op_quit(Xpost_Context *ctx)
{
    /* In an interpreter that supports multiple execution contexts, quit
       terminates the current context only (PLRM 8.2 quit). A context
       started by fork therefore ends where returning from its top-level
       procedure would have ended it: everything above the marker at the
       bottom of its execution stack is discarded, and the marker -- which
       frees the context, or leaves it holding its results for a join -- is
       what runs next. The interpreter's own context is the run, so quit
       there is the end of the run. */
    if (xpost_dps_context_is_forked(ctx))
    {
        xpost_context_unwind_exec(ctx, 1);
        return 0;
    }
    ctx->quit = 1;
    return 0;
}

/* - start -
   executed at interpreter startup */
/* implemented in data/init.ps */

int xpost_oper_init_control_ops (Xpost_Context *ctx,
                                 Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "exec", (Xpost_Op_Func)xpost_op_any_exec, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "if", (Xpost_Op_Func)xpost_op_bool_proc_if, 2, booleantype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "ifelse", (Xpost_Op_Func)xpost_op_bool_proc_proc_ifelse, 3, booleantype, proctype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "for", (Xpost_Op_Func)xpost_op_int_int_int_proc_for, 4, \
                             integertype, integertype, numbertype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "for", (Xpost_Op_Func)xpost_op_real_real_real_proc_for, 4, \
                             floattype, floattype, floattype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "repeat", (Xpost_Op_Func)xpost_op_int_proc_repeat, 2, integertype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "loop", (Xpost_Op_Func)xpost_op_proc_loop, 1, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "wrap.done", (Xpost_Op_Func)xpost_op_wrapdone, 0);
    op = xpost_operator_cons(ctx, "wrap.sealed", (Xpost_Op_Func)xpost_op_wrapdone, 0);
    op = xpost_operator_cons(ctx, "callout.done",
                             (Xpost_Op_Func)xpost_op_calloutdone, 0);
    /* internal loop-continuation operators, referenced by opcode only */
    op = xpost_operator_cons(ctx, "for.iterate", (Xpost_Op_Func)xpost_op_for_iterate, 0);
    op = xpost_operator_cons(ctx, "repeat.iterate", (Xpost_Op_Func)xpost_op_repeat_iterate, 0);
    op = xpost_operator_cons(ctx, "loop.iterate", (Xpost_Op_Func)xpost_op_loop_iterate, 0);
    op = xpost_operator_cons(ctx, "exit", (Xpost_Op_Func)xpost_op_exit, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "stop", (Xpost_Op_Func)xpost_op_stop, 0);
    if (xpost_object_get_type(op) == invalidtype)
        return VMerror;
    if (xpost_dict_put(ctx, sd, xpost_name_cons(ctx, ".rundied"),
                       xpost_operator_cons(ctx, ".rundied",
                                           (Xpost_Op_Func)xpost_op_rundied, 0)))
        return VMerror;
    INSTALL;
    op = xpost_operator_cons(ctx, ".errorunwind",
                             (Xpost_Op_Func)xpost_op_errorunwind, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".coexec", (Xpost_Op_Func)xpost_op_proc_coexec, 1, proctype);
    INSTALL;
    name_gptr = xpost_name_cons(ctx, "gptr");
    name_grestore = xpost_name_cons(ctx, "grestore");
    op = xpost_operator_cons(ctx, ".calloutframe", (Xpost_Op_Func)xpost_op_callout,
                             4, proctype, anytype, integertype, anytype);
    INSTALL;
    /* the bracket's continuation, reached by opcode from the frame it
       rides beneath and never by name, so it is made without being put
       in systemdict -- as the other continuations are */
    op = xpost_operator_cons(ctx, "callout.unwind",
                             (Xpost_Op_Func)xpost_op_calloutunwind, 0);
    op = xpost_operator_cons(ctx, ".wrapop", (Xpost_Op_Func)xpost_op_wrapop, 2, nametype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".wrapopsig", (Xpost_Op_Func)xpost_op_wrapopsig, 3,
                             nametype, proctype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "stopped", (Xpost_Op_Func)xpost_op_any_stopped, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "countexecstack", (Xpost_Op_Func)xpost_op_countexecstack, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "execstack", (Xpost_Op_Func)xpost_op_array_execstack, 1, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "quit", (Xpost_Op_Func)xpost_op_quit, 0);
    INSTALL;
    /*
    op = xpost_operator_cons(ctx, "eq", (Xpost_Op_Func)Aeq, 1, 2, anytype, anytype);
    INSTALL;
    */

    return 0;
}
