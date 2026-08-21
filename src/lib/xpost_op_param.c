/*
 * Xpost - a Level-2 Postscript interpreter
 * Copyright (C) 2013-2016, Michael Joshua Ryan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Xpost software product nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdio.h>

#include "xpost.h"
#include "xpost_memory.h"
/* what paces a collection that runs of its own accord, which the
   VMThreshold user parameter below names */
#include "xpost_free.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_save.h"
#include "xpost_context.h"
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_dict.h"
#include "xpost_error.h"

#include "xpost_garbage.h"
#include "xpost_interpreter.h" /* the stack capacities the parameters name */
#include "xpost_operator.h"
#include "xpost_op_math.h"   /* a count, as the object the PLRM gives it */
#include "xpost_op_param.h"

static
int vmreclaim (Xpost_Context *ctx, Xpost_Object I)
{
    switch (I.int_.val)
    {
        default: return rangecheck;

        /* PLRM 8.2: the negative operands turn automatic collection off,
           for one bank or for both, and zero turns it on again. What is
           turned off is only the collection that runs of its own accord;
           an immediate collection the operator is asked for below still
           runs, which is what makes a program able to say when it would
           rather pay for one. These three are also the VMReclaim user
           parameter (PLRM 8.2 vmreclaim), which is read back from the
           setting they leave rather than from a copy of the operand. */
        case -2: /* disable automatic collection in local and global vm */
            xpost_garbage_auto_banks_set(ctx, XPOST_GARBAGE_SWEEP_NONE);
            break;
        case -1: /* disable automatic collection in local vm */
            xpost_garbage_auto_banks_set(ctx, xpost_garbage_auto_banks(ctx)
                                              & ~XPOST_GARBAGE_SWEEP_LOCAL);
            break;
        case 0: /* enable automatic collection */
            xpost_garbage_auto_banks_set(ctx, XPOST_GARBAGE_SWEEP_BOTH);
            break;

        /* An immediate collection of both banks marks across them: an
           object in one may be named from the other. A collection of
           local vm alone marks only the local roots and the sanctioned
           references the global systemdict holds (PLRM 3.7.2) -- no
           other global storage may name a local object, so the frozen
           global graph is not walked to protect a bank the sweep never
           touches. */
        case 1: /* perform immediate collection in local vm */
            if (ctx->garbage_collect_function(ctx->lo,
                                              XPOST_GARBAGE_SWEEP_LOCAL, 0) == -1)
                return VMerror;
            /* Closing the arena up cannot happen here: it moves the bytes
               under every pointer derived from an entity's address, and
               the machinery running this operator holds such pointers.
               Asked for instead, and done at the interpreter's safe
               point, which also hands the pages back once the free
               storage has gathered above the live entities. */
            ctx->lo->compact_pending = 1;
            break;
        case 2: /* perform immediate collection in local and global vm */
            if (ctx->garbage_collect_function(ctx->lo,
                                              XPOST_GARBAGE_SWEEP_BOTH, 1) == -1)
                return VMerror;
            ctx->lo->compact_pending = 1;
            ctx->gl->compact_pending = 1;
            break;
    }
    return 0;
}

static
int vmstatus (Xpost_Context *ctx)
{
    int lev;
    Xpost_Memory_File *vm;
    unsigned int vstk;

    vstk = xpost_memory_save_stack_ent(ctx->lo);
    lev = xpost_stack_count(ctx->lo, vstk);
    /* PLRM 8.2: the two counts are of the bank the allocation mode
       selects, virtual memory being accounted for separately in each.
       The level is not: it is the depth of save nesting, which a
       program has one of. */
    vm = (ctx->vmmode == GLOBAL) ? ctx->gl : ctx->lo;

    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(lev)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_count_cons(vm->high_water)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_count_cons(vm->max)))
        return stackoverflow;
    return 0;
}

/* -  .vmentcount  local global
   The number of entity slots each memory table has handed out. Entity
   numbers are a budget of their own, separate from the byte counts
   vmstatus reports: an entity freed goes on a free list and is handed
   out again, so this number rises only where nothing reclaims what a
   job has stopped using. */
static
int vmentcount (Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)ctx->lo->table.nextent)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)ctx->gl->table.nextent)))
        return stackoverflow;
    return 0;
}

/* -  .vmfreebytes  local global
   The bytes each bank's free lists hold: space a collection has
   recovered, still inside the arena, waiting for an allocation the size
   suits.
   It is the half of the account vmstatus cannot give. The used figure
   vmstatus reports is the high-water mark of the arena, which comes back
   only where a program asks for a reclaim and the pass closes the gaps
   between the live entities; it does not move for a collection alone,
   which puts blocks on a free list and leaves them where they are. The
   maximum figure is the arena the bank holds. So between them they say
   how much has been taken and how much is held, and nothing says how
   much of what is held is in use. That is this number: held, less free,
   is what the bank is actually using, and a large figure here is a
   context sitting on a peak it has finished with. */
static
int vmfreebytes (Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)xpost_free_bytes(ctx->lo))))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)xpost_free_bytes(ctx->gl))))
        return stackoverflow;
    return 0;
}

/* -  .vmfreescan  local global
   The number of free-list entries the allocator has examined in each
   memory file, saturating rather than wrapping. What an allocation
   costs is this number and not the bytes it asks for, so it is the
   measure of whether the cost of allocating tracks the allocations a
   job makes or the memory it has already released. */
static
int vmfreescan (Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)ctx->lo->free_scan)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)ctx->gl->free_scan)))
        return stackoverflow;
    return 0;
}

/* -  .vmcollect  -
   An immediate collection of both banks, and nothing else.

   vmreclaim collects and then asks for the arena to be closed up, which
   absorbs every free block into the run above the cursor and leaves the
   free lists empty. That is what a program asking for a reclaim wants,
   and it is no use to anything measuring the lists themselves: the state
   it wants to read is the one a collection leaves and a rearrangement
   takes away. This performs the collection alone, so that the lists can
   be read as a collection leaves them without waiting for one to happen
   of its own accord. */
static
int vmcollect (Xpost_Context *ctx)
{
    if (ctx->garbage_collect_function(ctx->lo,
                                      XPOST_GARBAGE_SWEEP_BOTH, 1) == -1)
        return VMerror;
    return 0;
}

/* -  .vmstackwalk  local global
   The number of stack segments stepped over in each memory file,
   saturating rather than wrapping. A stack is a chain of segments, so
   what reaching a position in one costs is the segments between it and
   the end the walk starts from. This is the measure of whether a scan of
   a whole stack walks the chain once or walks it again for every element
   it looks at: the first costs the stack's length, the second that
   length squared, and the two return the same answer. */
static
int vmstackwalk (Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)ctx->lo->stack_walk)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)ctx->gl->stack_walk)))
        return stackoverflow;
    return 0;
}

/* nobjects nbytes  .vmreserve  bool
   Whether the virtual memory now being allocated from can take nobjects
   more composite elements and nbytes more bytes of string storage,
   having obtained the room for them.

   A caller that allocates a large structure one piece at a time cannot
   recover from running out partway: the pieces it has already taken are
   reachable from nothing and the memory they occupy is not returned to
   it, so the failure leaves the interpreter with less than it had
   before. Pricing the whole structure and asking for it here moves that
   failure to before the first piece. The file either has the room
   already, or grows once to hold it, or answers false having changed
   nothing -- a grow that cannot be satisfied leaves the existing mapping
   in place -- so the caller's error costs no memory at all.

   The counts arrive as reals because a structure large enough to be
   worth refusing overflows a 32-bit integer. An object addresses its
   memory file through an unsigned 32-bit offset, so a request past that
   span cannot be met however much memory the host has, and is refused
   without asking for it. */
static
int vmreserve (Xpost_Context *ctx, Xpost_Object nobjects, Xpost_Object nbytes)
{
    Xpost_Memory_File *mem;
    double want;
    int fits;

    /* written as a failed lower bound rather than as a comparison
       against zero, so that a count that is not a number at all is
       refused here instead of being converted to an integer it has no
       value for */
    if (!(nobjects.real_.val >= 0.0) || !(nbytes.real_.val >= 0.0))
        return rangecheck;

    want = (double)nobjects.real_.val * (double)sizeof(Xpost_Object)
         + (double)nbytes.real_.val;

    mem = (ctx->vmmode == GLOBAL) ? ctx->gl : ctx->lo;

    if (want > (double)0xffffffffu
        || (double)mem->high_water + want > (double)0xffffffffu)
        fits = 0;
    else if ((size_t)mem->high_water + (size_t)want < (size_t)mem->max)
        fits = 1;
    else
        fits = xpost_memory_file_grow(mem, (unsigned int)want) ? 1 : 0;

    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(fits)))
        return stackoverflow;
    return 0;
}

/* The count of bytes this interpreter answers a request for the default
   with, and the count a context starts with. */
#define XPOST_VM_THRESHOLD_DEFAULT XPOST_GARBAGE_COLLECTION_THRESHOLD

/* Give the count a run asked for to the banks it paces, and start the
   countdown again from it so that a smaller count takes effect on the
   next allocation rather than after the one already in progress. Both
   banks are set: a collection sweeps them together, so a count that
   reached only one of them would name a frequency neither bank keeps.

   PLRM C.3.5 makes this a user parameter, held per context; PLRM 8.2
   setvmthreshold adds that where virtual memory is shared, a context's
   setting applies while that context is executing, which is what
   writing it through on the way past comes to. */
static void
_vmthreshold_apply(Xpost_Context *ctx, integer bytes)
{
    if (bytes < 0)
        bytes = 0;
    if (ctx->lo)
    {
        ctx->lo->threshold_bytes = (int)bytes;
        ctx->lo->threshold = (int)bytes;
    }
    if (ctx->gl)
    {
        ctx->gl->threshold_bytes = (int)bytes;
        ctx->gl->threshold = (int)bytes;
    }
}

/* The VMReclaim user parameter (PLRM C.3.5) as the collector's own
   setting reads: 0 where a collection running of its own accord
   reclaims both banks, -1 where it leaves local VM alone, -2 where it
   runs for neither. Those three are what vmreclaim sets, so those three
   are what this answers with, and the number a program reads back is
   the setting itself rather than a copy of it kept alongside. */
static
int _vmreclaim_code(Xpost_Context *ctx)
{
    int banks = xpost_garbage_auto_banks(ctx);

    if (banks == XPOST_GARBAGE_SWEEP_BOTH)
        return 0;
    if (banks & XPOST_GARBAGE_SWEEP_GLOBAL)
        return -1;
    return -2;
}

/* int  setvmthreshold  -
   ask for a count of bytes to be allocated between the collections that
   run of their own accord.

   PLRM 8.2: -1 asks for the implementation's default, any other
   negative operand is a rangecheck, and a count outside what the
   implementation can do is replaced by the nearest it can. The count
   paces the collections this interpreter runs of its own accord: it is
   given to both banks of virtual memory, which count it down by the
   bytes they allocate. Every count from zero up is achievable -- a
   request for a collection is recorded and taken at the next safe
   point, so the smallest of them mean a collection at every safe point
   rather than a run that cannot finish -- and only the default has to
   be supplied. The
   count is the VMThreshold user parameter (PLRM 8.2 setvmthreshold), so
   currentuserparams reads it back and restore reverts it. */
static
int setvmthreshold(Xpost_Context *ctx, Xpost_Object I)
{
    if (I.int_.val == -1)
        ctx->vmthreshold = XPOST_VM_THRESHOLD_DEFAULT;
    else if (I.int_.val < 0)
        return rangecheck;
    else
        ctx->vmthreshold = I.int_.val;
    _vmthreshold_apply(ctx, ctx->vmthreshold);
    return 0;
}

/* the names of the user parameters, so that what currentuserparams
   reports and what setuserparams recognises are one list */
static const char *_userparam_vmreclaim = "VMReclaim";
static const char *_userparam_vmthreshold = "VMThreshold";
static const char *_userparam_maxopstack = "MaxOpStack";
static const char *_userparam_maxdictstack = "MaxDictStack";
static const char *_userparam_maxexecstack = "MaxExecStack";
static const char *_userparam_idiomrecognition = "IdiomRecognition";

/* one parameter's present value, into the dictionary being reported */
static
int _param_report(Xpost_Context *ctx, Xpost_Object d, const char *key,
                  integer val)
{
    return xpost_dict_put(ctx, d, xpost_name_cons(ctx, key),
                          xpost_int_cons(val));
}

/* a boolean parameter's present value, into the dictionary being
   reported (IdiomRecognition is the one such) */
static
int _param_report_bool(Xpost_Context *ctx, Xpost_Object d, const char *key,
                       int val)
{
    return xpost_dict_put(ctx, d, xpost_name_cons(ctx, key),
                          xpost_bool_cons(val));
}

/* -  currentuserparams  dict
   the user interpreter parameters and the values they now have, in a
   dictionary of the caller's own (PLRM 8.2 currentuserparams).

   The three stack sizes are the capacities this interpreter holds its
   stacks to, read from the ceilings it enforces, so a program that asks
   for another size and reads the answer back is told what it will
   actually get. */
static
int currentuserparams(Xpost_Context *ctx)
{
    Xpost_Object d;
    int ret;

    d = xpost_dict_cons(ctx, 6);
    if (xpost_object_get_type(d) == invalidtype)
        return VMerror;

    ret = _param_report(ctx, d, _userparam_vmreclaim, _vmreclaim_code(ctx));
    if (ret)
        return ret;
    ret = _param_report(ctx, d, _userparam_vmthreshold, ctx->vmthreshold);
    if (ret)
        return ret;
    ret = _param_report(ctx, d, _userparam_maxopstack, XPOST_OPER_STACK_LIMIT);
    if (ret)
        return ret;
    ret = _param_report(ctx, d, _userparam_maxdictstack, XPOST_DICT_STACK_LIMIT);
    if (ret)
        return ret;
    ret = _param_report(ctx, d, _userparam_maxexecstack, XPOST_EXEC_STACK_LIMIT);
    if (ret)
        return ret;
    ret = _param_report_bool(ctx, d, _userparam_idiomrecognition,
                             ctx->idiomrecognition);
    if (ret)
        return ret;

    if (!xpost_stack_push(ctx->lo, ctx->os, d))
        return stackoverflow;
    return 0;
}

/* the value a dictionary offers for one parameter: absent where it
   offers none, and a typecheck where what it offers is not the integer
   the parameter takes (PLRM 8.2 setuserparams). A caller that only
   needs the value checked passes neither out-parameter. */
static
int _param_request(Xpost_Context *ctx, Xpost_Object D, const char *key,
                   integer *val, int *have)
{
    Xpost_Object v;

    if (have)
        *have = 0;
    v = xpost_dict_get(ctx, D, xpost_name_cons(ctx, key));
    if (xpost_object_get_type(v) == invalidtype)
        return 0;
    if (xpost_object_get_type(v) != integertype)
        return typecheck;
    if (val)
        *val = v.int_.val;
    if (have)
        *have = 1;
    return 0;
}

/* the boolean value a dictionary offers for one parameter: absent where
   it offers none, a typecheck where what it offers is not a boolean
   (IdiomRecognition is the one such parameter) */
static
int _param_request_bool(Xpost_Context *ctx, Xpost_Object D, const char *key,
                        int *val, int *have)
{
    Xpost_Object v;

    if (have)
        *have = 0;
    v = xpost_dict_get(ctx, D, xpost_name_cons(ctx, key));
    if (xpost_object_get_type(v) == invalidtype)
        return 0;
    if (xpost_object_get_type(v) != booleantype)
        return typecheck;
    if (val)
        *val = v.int_.val;
    if (have)
        *have = 1;
    return 0;
}

/* dict  setuserparams  -
   set the user interpreter parameters the dictionary names, leaving the
   rest as they were (PLRM 8.2 setuserparams).

   A key naming no parameter of this implementation is ignored, and a
   value the implementation cannot achieve is replaced by the nearest it
   can, without an error indication. The three stack sizes name
   capacities this interpreter fixes, so the nearest achievable value is
   the capacity it already has and the request is answered by
   currentuserparams reporting that capacity back; VMReclaim can hold
   only the three collector settings, so any other number leaves the
   setting where it stands; every VMThreshold from zero up is
   achievable, so only a negative one is replaced.

   Every value offered is read and checked before any of them is
   applied, so a dictionary that is refused leaves the whole set as it
   found it. */
static
int setuserparams(Xpost_Context *ctx, Xpost_Object D)
{
    integer reclaim = 0, threshold = 0;
    int have_reclaim = 0, have_threshold = 0;
    int idiom = 0, have_idiom = 0;
    int ret;

    /* the dictionary is searched, so it needs read access */
    if (!xpost_object_is_readable(ctx, D))
        return invalidaccess;

    ret = _param_request(ctx, D, _userparam_vmreclaim, &reclaim, &have_reclaim);
    if (ret)
        return ret;
    ret = _param_request(ctx, D, _userparam_vmthreshold,
                         &threshold, &have_threshold);
    if (ret)
        return ret;
    ret = _param_request(ctx, D, _userparam_maxopstack, NULL, NULL);
    if (ret)
        return ret;
    ret = _param_request(ctx, D, _userparam_maxdictstack, NULL, NULL);
    if (ret)
        return ret;
    ret = _param_request(ctx, D, _userparam_maxexecstack, NULL, NULL);
    if (ret)
        return ret;
    ret = _param_request_bool(ctx, D, _userparam_idiomrecognition,
                              &idiom, &have_idiom);
    if (ret)
        return ret;

    if (have_reclaim && (reclaim == 0 || reclaim == -1 || reclaim == -2))
    {
        ret = vmreclaim(ctx, xpost_int_cons(reclaim));
        if (ret)
            return ret;
    }
    if (have_threshold)
    {
        ctx->vmthreshold = threshold < 0 ? XPOST_VM_THRESHOLD_DEFAULT
                                         : threshold;
        _vmthreshold_apply(ctx, ctx->vmthreshold);
    }
    if (have_idiom)
        ctx->idiomrecognition = idiom ? 1 : 0;
    return 0;
}

static
int globalvmstatus (Xpost_Context *ctx)
{
    int lev;
    unsigned int vstk;

    vstk = xpost_memory_save_stack_ent(ctx->gl);
    lev = xpost_stack_count(ctx->gl, vstk);
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(lev)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_count_cons(ctx->gl->high_water)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_count_cons(ctx->gl->max)))
        return stackoverflow;
    return 0;
}


/* -  .idiomrecognition  bool
   the present value of the IdiomRecognition user parameter, for the
   bind machinery to read cheaply without building the whole parameter
   dictionary each time it binds */
static
int idiomrecognition_get(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_bool_cons(ctx->idiomrecognition)))
        return stackoverflow;
    return 0;
}

int xpost_oper_init_param_ops(Xpost_Context *ctx,
                              Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "vmreclaim", (Xpost_Op_Func)vmreclaim, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "setvmthreshold", (Xpost_Op_Func)setvmthreshold, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "currentuserparams", (Xpost_Op_Func)currentuserparams, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "setuserparams", (Xpost_Op_Func)setuserparams, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, "vmstatus", (Xpost_Op_Func)vmstatus, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "globalvmstatus", (Xpost_Op_Func)globalvmstatus, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".idiomrecognition", (Xpost_Op_Func)idiomrecognition_get, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".vmentcount", (Xpost_Op_Func)vmentcount, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".vmfreebytes", (Xpost_Op_Func)vmfreebytes, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".vmfreescan", (Xpost_Op_Func)vmfreescan, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".vmcollect", (Xpost_Op_Func)vmcollect, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".vmstackwalk", (Xpost_Op_Func)vmstackwalk, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".vmreserve", (Xpost_Op_Func)vmreserve, 2, floattype, floattype);
    INSTALL;

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
    op = xpost_operator_cons(ctx, "save", (Xpost_Op_Func)Zsave, 1, 0);
    INSTALL;
    xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "mark"), mark); */

    return 0;
}
