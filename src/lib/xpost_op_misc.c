/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_misc.c
 * @brief Installs the operators that belong to no other group.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * bind usertime realtime getenv putenv dumpvm dumpnames
 * debugloadon debugloadoff returntocaller
 *
 * Some are PLRM's, some are this interpreter's own for looking at its
 * insides; the ones that are not PLRM's are removed at the lockdown.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h> /* NULL strtod */
#include <stddef.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h> /* time */

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

#include "xpost.h"
#include "xpost_compat.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_file.h"  /* the sandbox denies environment access once engaged */
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_dict.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_dict.h"
#include "xpost_op_misc.h"

/* the procedures already walked in this bind, so a procedure that
   reaches itself -- directly or through others -- binds once and
   terminates: access flags ride on the reference, not the value, so
   marking the copy in hand cannot break the cycle */
typedef struct
{
    unsigned int *ents;
    int n, cap;
    int depth;   /* live descent depth, so a runtime-built nest cannot
                    recurse the C stack away */
    int toodeep; /* set when the cap was hit; the top level raises limitcheck */
} Bind_Seen;

/* How deep bind will descend into nested executable arrays. The scanner
   caps a SOURCE procedure's nesting far below this, but an array built at
   run time and made executable bypasses that cap, and bind then recurses
   one C frame per level. Real procedures nest a handful deep; this is far
   above anything legitimate and far below what exhausts the stack. */
#define XPOST_BIND_MAX_DEPTH 1000

static
Xpost_Object bind(Xpost_Context *ctx,
                  Xpost_Object p,
                  Bind_Seen *seen)
{
    Xpost_Object t, d;
    unsigned int ent;
    int i, j, z;

    /* a plain read-only procedure -- one made read-only after creation
       rather than by the packing machinery -- is left exactly as it is:
       bind neither rewrites its names nor descends into it. bind does
       rewrite a packed array (it carries the packed flag). PLRM 8.2:
       bind "will ignore a read-only array; that is, it will neither bind
       elements of the array nor examine nested procedures", and "will
       operate on a packed array ... disregarding its access attribute". */
    if (!xpost_object_is_packed(p)
     && xpost_object_get_access(ctx, p) < XPOST_OBJECT_TAG_ACCESS_UNLIMITED)
        return p;

    /* Has this procedure been walked already? seen->ents is an
       open-addressed hash set -- a slot holds ent+1, a zero slot is
       empty, cap is a power of two -- so a procedure that reaches itself
       is recognised in O(1), and one holding very many distinct
       sub-procedures costs O(M) to bind rather than O(M squared). */
    ent = xpost_object_get_ent(p);
    if (seen->cap == 0 || (seen->n + 1) * 10 >= seen->cap * 7)
    {
        int ncap = seen->cap ? seen->cap * 2 : 256;
        unsigned int *nents = calloc((size_t)ncap, sizeof(*nents));
        int k;

        if (!nents)
            return xpost_object_set_access(ctx, p, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
        for (k = 0; k < seen->cap; k++)
            if (seen->ents[k])
            {
                unsigned int h = (seen->ents[k] * 2654435761u) & (unsigned)(ncap - 1);
                while (nents[h]) h = (h + 1) & (unsigned)(ncap - 1);
                nents[h] = seen->ents[k];
            }
        free(seen->ents);
        seen->ents = nents;
        seen->cap = ncap;
    }
    {
        unsigned int key = ent + 1;
        unsigned int h = (key * 2654435761u) & (unsigned)(seen->cap - 1);

        while (seen->ents[h])
        {
            if (seen->ents[h] == key)
                return xpost_object_set_access(ctx, p,
                                               XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
            h = (h + 1) & (unsigned)(seen->cap - 1);
        }
        seen->ents[h] = key;
        seen->n++;
    }

    for (i = 0; i < (integer)p.comp_.sz; i++)
    {
        t = xpost_array_get(ctx, p, i);
        switch(xpost_object_get_type(t))
        {
            default: break;
            case nametype:
                if (!xpost_object_is_exe(t)) break; /* bind only replaces executable names */
                z = xpost_stack_count(ctx->lo, ctx->ds);
                for (j = 0; j < z; j++) {
                    d = xpost_stack_topdown_fetch(ctx->lo, ctx->ds, j);
                    t = xpost_dict_get_name(ctx, d, t);
                    if (xpost_object_get_type(t) != invalidtype) {
                        if (xpost_object_get_type(t) == operatortype) {
                            /* bind rewrites the procedure itself, which
                               is read-only once packed: the raw layer
                               writes without the program-facing access
                               check */
                            /* the index was just read from this same
                               array, so the store reaches it */
                            XPOST_REFUSAL_IMPOSSIBLE(
                                xpost_array_put_memory(
                                    xpost_context_select_memory(ctx, p),
                                    p, i, t));
                        }
                        break;
                    }
                    t = xpost_array_get(ctx, p, i); /* keep searching for the name */
                }
                break;
            case arraytype:
                /* descend into every executable sub-procedure; bind()
                   rewrites the packed and writable ones in place and
                   leaves a plain read-only one (an already-bound
                   procedure keeps its finished contents) untouched */
                if (xpost_object_is_exe(t))
                {
                    if (seen->depth >= XPOST_BIND_MAX_DEPTH)
                    {
                        /* too deep to descend safely: stop here and let
                           the top level report it, rather than recurse the
                           C stack to destruction */
                        seen->toodeep = 1;
                        break;
                    }
                    ++seen->depth;
                    t = bind(ctx, t, seen);
                    --seen->depth;
                    /* as above: i indexes the array being walked */
                    XPOST_REFUSAL_IMPOSSIBLE(
                        xpost_array_put_memory(
                            xpost_context_select_memory(ctx, p), p, i, t));
                }
        }
    }
    return xpost_object_set_access(ctx, p, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
}

/* proc  bind  proc
   replace names with operators in proc and make read-only */
static
int Pbind(Xpost_Context *ctx,
          Xpost_Object P)
{
    Bind_Seen seen;
    Xpost_Object bound;

    seen.ents = NULL;
    seen.n = seen.cap = 0;
    seen.depth = 0;
    seen.toodeep = 0;
    bound = bind(ctx, P, &seen);
    free(seen.ents);
    if (seen.toodeep)
        return limitcheck;
    xpost_stack_push(ctx->lo, ctx->os, bound);
    return 0;
}

/* -  realtime  int
   return real time in milliseconds */
static
int realtime(Xpost_Context *ctx)
{
    long long ms;

    ms = xpost_get_realtime_ms();
    ms &= 0x00000000ffffffff; /* truncate any large value */
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((int)ms)))
        return stackoverflow;

    return 0;
}

/* -  usertime  int
   return execution time in milliseconds */
static
int usertime(Xpost_Context *ctx)
{
    long long ms;

    ms = xpost_get_usertime_ms();
    ms &= 0x00000000ffffffff; /* truncate any large value */
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((int)ms)))
        return stackoverflow;
    return 0;
}

/* string  getenv  string
   return value for environment variable */
static
int Sgetenv(Xpost_Context *ctx,
            Xpost_Object S)
{
    char *str;
    char *r;
    if (xpost_path_control_is_engaged())
        return invalidaccess;
    str = xpost_string_allocate_cstring(ctx, S);
    r = xpost_getenv(str);
    if (r)
    {
        Xpost_Object strobj;
        size_t n = strlen(r);
        /* the environment's values are the environment's own length, and
           a string object records its length in a field narrower than
           that on some builds: a value the field cannot count is refused
           rather than answered as the length it wrapped to */
        if (n > (size_t)XPOST_OBJECT_COMP_MAX_SZ) /* the sz field is full */
        {
            free(str);
            free(r);
            return limitcheck;
        }
        strobj = xpost_string_cons(ctx, (unsigned int)n, r);
        if (xpost_object_get_type(strobj) == nulltype){
            free(str);
            free(r);
            return VMerror;
        }
        xpost_stack_push(ctx->lo, ctx->os, strobj);
    }
    else
    {
        free(str);
        return undefined;
    }
    free(str);
    free(r);
    return 0;
}

/* string string  putenv
   set value for environment variable */
static
int SSputenv(Xpost_Context *ctx,
             Xpost_Object N,
             Xpost_Object S)
{
    char *n;
    char *v;

    if (xpost_path_control_is_engaged())
        return invalidaccess;
    n = xpost_string_allocate_cstring(ctx, N);
    if (!n)
        return VMerror;
    v = xpost_string_allocate_cstring(ctx, S);
    if (!v)
    {
        free(n);
        return VMerror;
    }

    if (xpost_putenv(n, v) != 0)
    {
        free(n);
        free(v);
        return VMerror;
    }

    free(n);
    free(v);
    return 0;
}

static
int _array_swap(Xpost_Context *ctx,
                Xpost_Object a,
                Xpost_Object i,
                Xpost_Object j)
{
    Xpost_Object a_i, a_j;
    int ret;

    /* both indices name elements of the array on both sides: the
       element accessors bound an index against the entity behind the
       array rather than against the array's own extent, so an index
       outside it reaches an element of whatever else the entity holds
       -- the elements a subarray was cut from, among them */
    if (i.int_.val < 0 || i.int_.val >= (integer)a.comp_.sz
     || j.int_.val < 0 || j.int_.val >= (integer)a.comp_.sz)
        return rangecheck;
    a_i = xpost_array_get(ctx, a, i.int_.val);
    a_j = xpost_array_get(ctx, a, j.int_.val);
    ret = xpost_array_put(ctx, a, i.int_.val, a_j);
    if (ret)
        return ret;
    ret = xpost_array_put(ctx, a, j.int_.val, a_i);
    if (ret)
        return ret;
    return 0;
}


static
int debugloadon(Xpost_Context *ctx)
{
    (void)ctx;
    DEBUGLOAD = 1;
    return 0;
}
static
int debugloadoff(Xpost_Context *ctx)
{
    (void)ctx;
    DEBUGLOAD = 0;
    return 0;
}

/* -  .namelookups  int
   The number of times a string has been offered to the name mechanism
   and had to be looked up, saturating rather than wrapping. Resolving
   a name costs a walk of the tree whether or not it is already
   interned, so this is the measure of whether a caller resolves a name
   once or resolves it again for every unit of work it does. */
static
int _namelookups(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((integer)xpost_name_lookups())))
        return stackoverflow;
    return 0;
}

static
int Odumpnames(Xpost_Context *ctx)
{
    unsigned int names;
    printf("\nGlobal Name stack: ");
    names = xpost_memory_name_stack_ent(ctx->gl);
    xpost_stack_dump(ctx->gl, names);
    (void)puts("");
    printf("\nLocal Name stack: ");
    names = xpost_memory_name_stack_ent(ctx->lo);
    xpost_stack_dump(ctx->lo, names);
    (void)puts("");
    return 0;
}

static
int dumpvm(Xpost_Context *ctx)
{
    xpost_memory_file_dump(ctx->lo);
    xpost_memory_table_dump(ctx->lo);
    xpost_memory_file_dump(ctx->gl);
    xpost_memory_table_dump(ctx->gl);
    return 0;
}

static
int returntocaller(Xpost_Context *ctx)
{
    (void)ctx;
    return yieldtocaller;
}

/* -  .sysdictunlock  -
   Make systemdict writeable so the graphics language can define into it. This
   is a one-shot: once the language is loaded (.sysdictrelock has run), it does
   nothing, so a program that reaches the name cannot reopen systemdict. The
   window it opens runs only the interpreter's own graphics files, before any
   program, and the error handler relocks systemdict if a load faults. */
static
int op_sysdictunlock(Xpost_Context *ctx)
{
    Xpost_Object sd;
    if (ctx->sysdict_load_done)
        return 0;
    sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    /* opening systemdict writes its value, which backs it up to whatever
       save level stands over the load first; refused, systemdict stays
       shut and the load reports it rather than proceeding against a
       dictionary it cannot define into */
    if (xpost_object_get_type(
            xpost_object_set_access(ctx, sd,
                                    XPOST_OBJECT_TAG_ACCESS_UNLIMITED))
        == invalidtype)
        return VMerror;
    ctx->sysdict_unlocked = 1;
    return 0;
}

/* -  .sysdictrelock  -
   Restore systemdict to read-only after the graphics language has loaded, and
   spend the one-shot. */
static
int op_sysdictrelock(Xpost_Context *ctx)
{
    Xpost_Object sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    /* the window this shuts was opened by a write that backed systemdict
       up to any save level standing over the load, so shutting it takes
       no further backup and cannot be refused */
    xpost_object_set_access(ctx, sd, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
    ctx->sysdict_unlocked = 0;
    ctx->sysdict_load_done = 1;
    return 0;
}

/* dict  .setprivatedict  -
   Record the interpreter's private local machinery dictionary in the context,
   where the collector roots it and the C reaches it, without it ever going on
   the dict stack. Called once from init.ps.

   The dictionary must be local. What it holds are local objects -- the device
   class dictionaries, the graphics scratch and template, the anchor procedures
   of the wrapped operators -- and a global dictionary may hold none of them, so
   a global one leaves the machinery that writes there refused at every write.

   It is recorded once. The record is context state rather than virtual memory,
   so no `restore` puts back a dictionary displaced from it, and a second
   install would stand for the rest of the run. A context fork makes is given
   its own private dictionary directly, in xpost_context_fork3, not through this
   operator, so this refusal still holds for every context: the operator is
   reachable through internaldict, and a program that replaced the private
   dictionary would displace the machinery that runs through it. */
static
int op_setprivatedict(Xpost_Context *ctx,
                      Xpost_Object D)
{
    if (xpost_object_get_type(ctx->privatedict) == dicttype)
        return invalidaccess;
    if (xpost_context_select_memory(ctx, D) != ctx->lo)
        return invalidaccess;
    ctx->privatedict = D;
    return 0;
}

/* dict  .setglobalprivatedict  -
   Record the interpreter's private global namespace in the context, where the
   collector roots it and the C reaches it. Called once from init.ps.

   The namespace drops its userdict anchor at lockdown, leaving it reachable
   only through the references frozen into procedure bodies. That is enough for
   PostScript, which holds such a reference wherever it needs one, and is
   nothing at all for C, which holds none. Recording it here gives both a way
   in and makes the namespace rooted in its own right rather than through
   whichever procedure happens to have frozen a reference to it.

   The dictionary must be global. What C keeps here is the half of a cache that
   is an object, whose other half is a host resource held in a static: the two
   must stay reachable together, and a record belonging to one context cannot
   promise that when the contexts share these memory banks and the one that
   filled the cache may end first. A global object may hold no local one, which
   is the same reason the local machinery has a dictionary of its own.

   It is recorded once. The record is context state rather than virtual memory,
   so no `restore` puts back a namespace displaced from it. */
static
int op_setglobalprivatedict(Xpost_Context *ctx,
                            Xpost_Object D)
{
    if (xpost_object_get_type(ctx->globalprivatedict) == dicttype)
        return invalidaccess;
    if (xpost_context_select_memory(ctx, D) != ctx->gl)
        return invalidaccess;
    ctx->globalprivatedict = D;
    return 0;
}

/* -  .privatedict  dict
   Push the private local machinery dictionary. Like .gscratch, it hands a local
   object to whatever asks; a global procedure may use the result transiently
   without holding a local reference. */
static
int op_privatedict(Xpost_Context *ctx)
{
    xpost_stack_push(ctx->lo, ctx->os, ctx->privatedict);
    return 0;
}

int xpost_oper_init_misc_ops(Xpost_Context *ctx,
                             Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    const char *productstr = "Xpost";
    const char *versionstr = "0.0";
    int revno = 1;
    int serno = 0;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "bind", (Xpost_Op_Func)Pbind, 1, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".sysdictunlock", (Xpost_Op_Func)op_sysdictunlock, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".setprivatedict", (Xpost_Op_Func)op_setprivatedict, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".setglobalprivatedict", (Xpost_Op_Func)op_setglobalprivatedict, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".privatedict", (Xpost_Op_Func)op_privatedict, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".sysdictrelock", (Xpost_Op_Func)op_sysdictrelock, 0);
    INSTALL;
    if (xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "null"), null))
        return VMerror;
    if (xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "version"),
                       xpost_object_cvlit(xpost_string_cons(ctx,
                               strlen(versionstr), versionstr))))
        return VMerror;
    op = xpost_operator_cons(ctx, "realtime", (Xpost_Op_Func)realtime, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "usertime", (Xpost_Op_Func)usertime, 0);
    INSTALL;
    //languagelevel
    if (xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "product"),
                       xpost_object_cvlit(xpost_string_cons(ctx,
                               strlen(productstr), productstr))))
        return VMerror;
    if (xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "revision"),
                       xpost_int_cons(revno)))
        return VMerror;
    if (xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "serialnumber"),
                       xpost_int_cons(serno)))
        return VMerror;
    //executive: see init.ps
    //echo: see opf.c
    //prompt: see init.ps

    op = xpost_operator_cons(ctx, "getenv", (Xpost_Op_Func)Sgetenv, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "putenv", (Xpost_Op_Func)SSputenv, 2, stringtype, stringtype);
    INSTALL;

    op = xpost_operator_cons(ctx, ".swap", (Xpost_Op_Func)_array_swap, 3,
                             arraytype, integertype, integertype);
    INSTALL;

    op = xpost_operator_cons(ctx, "debugloadon", (Xpost_Op_Func)debugloadon, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "debugloadoff", (Xpost_Op_Func)debugloadoff, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".namelookups", (Xpost_Op_Func)_namelookups, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "dumpnames", (Xpost_Op_Func)Odumpnames, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "dumpvm", (Xpost_Op_Func)dumpvm, 0);
    INSTALL;

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
    xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "mark"), mark); */

    op = xpost_operator_cons(ctx, "returntocaller", (Xpost_Op_Func)returntocaller, 0);
    INSTALL;

    return 0;
}
