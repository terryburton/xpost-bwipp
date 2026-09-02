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
#include "xpost_garbage.h"
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

/* -  .buildtime  int
   The stamp naming this build, which currentsystemparams reports as
   BuildTime (PLRM Appendix C). XPOST_BUILD_TIME is taken from
   SOURCE_DATE_EPOCH when the build is configured rather than read from the
   clock, so two builds of one source agree; a build whose packager set no
   epoch carries nought, which is no stamp rather than a wrong one.

   An operator rather than the number itself, and the reason is the image of
   virtual memory. A number would be frozen into currentsystemparams by the
   // that reads it, and an image carrying that body would go on reporting
   the stamp of the build that wrote the image. The image is stamped with a
   build identifier, but that identifier is made from the sources and the
   compiler flags, and two builds of one source configured with different
   epochs are identical to it. An operator freezes the call instead, so what
   is reported is the running build's own answer.

   The name is private: Appendix C names the parameter and names nothing
   underneath it. */
static
int buildtime(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((integer)XPOST_BUILD_TIME)))
        return stackoverflow;
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
    /* the variable's name is read out of the string (PLRM 3.3.2) */
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    /* the environment names its variables in C text, which ends at the
       first nul: a string carrying one would be looked up as the shorter
       name it begins with and answer with that variable's value. A string
       counts its characters (PLRM 3.3), so such a string names no
       variable the environment holds */
    if (!xpost_string_is_cstring(ctx, S))
        return undefined;
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
    /* both the name and the value it is given are read (PLRM 3.3.2) */
    if (!xpost_object_is_readable(ctx, N) || !xpost_object_is_readable(ctx, S))
        return invalidaccess;
    /* the environment holds a name and a value as C text, which ends at
       the first nul: a name carrying one would set the variable whose
       name it begins with, and a value carrying one would be set as the
       shorter value it begins with. A string counts its characters (PLRM
       3.3), so neither is text the environment can be given */
    if (!xpost_string_is_cstring(ctx, N) || !xpost_string_is_cstring(ctx, S))
        return rangecheck;
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

/* dict  .setjobstore  -
   Record the job store in the context, where the collector roots it, the job
   boundary puts it back with the banks, and C reaches it without naming it.

   A root of its own rather than a member of the private global namespace,
   because that namespace is meant to hold nothing writable: a store kept
   inside it would be the one exception every statement about it has to carry.

   The dictionary must be global. What it holds is what a restore may not take
   back -- the page a job has reached, the advance a glyph declared -- and only
   global memory outlives a restore (PLRM 3.7.2). Recorded once, like the
   namespaces. */
static
int op_setjobstore(Xpost_Context *ctx,
                   Xpost_Object D)
{
    if (xpost_object_get_type(ctx->jobstore) == dicttype)
        return invalidaccess;
    if (xpost_context_select_memory(ctx, D) != ctx->gl)
        return invalidaccess;
    ctx->jobstore = D;
    return 0;
}

/* dict  .setgraphicsdict  -
   Root the live graphics state in the context. Said once, as the
   graphics language builds: privatedict is reachable by decision --
   driver prologs name it -- so a member of it naming the graphics state
   is a slot a program writes to hand the machinery a device of its own.
   Rooted here it is reached by the machinery and by the collector, and
   named by nothing a program can enumerate. */
static
int op_setgraphicsdict(Xpost_Context *ctx, Xpost_Object D)
{
    /* Said as the graphics language builds, and again by a fork, which
       gives the child its own graphics state (PLRM 2nd ed 7.1) -- so
       this is not the job store's one-shot. What keeps a program from
       saying it is that the operator is not one a program can name. */
    if (xpost_context_select_memory(ctx, D) != ctx->lo)
        return invalidaccess;
    ctx->graphicsdict = D;
    return 0;
}

/* -  .graphicsdictroot  dict
   The live graphics state of the running context. A forked context has
   its own, which is what the accessor this answers used to reach
   through privatedict for. */
static
int op_graphicsdictroot(Xpost_Context *ctx)
{
    if (xpost_object_get_type(ctx->graphicsdict) != dicttype)
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, ctx->graphicsdict);
    return 0;
}

/* -  .jobstore  dict
   Push the job store, so the boot files can begin it and freeze references to
   its members as they are scanned. */
static
int op_jobstore(Xpost_Context *ctx)
{
    if (xpost_object_get_type(ctx->jobstore) != dicttype)
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, ctx->jobstore);
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

/* int  .setmaxformcache  -
   Set MaxFormCache, the form cache's ceiling (PLRM C.3.3).

   A system parameter, so a system administrator job and no other may
   change it: what it bounds is a cache the jobs after this one will run
   under, and PLRM 8.2 refuses a capacity stated outside such a job for the
   glyph cache for exactly that reason. The refusal is the same one, asked
   the same way.

   A ceiling below nothing is not achievable and is replaced by the nearest
   that is, which is nothing: a cache that keeps no drawing. */
static
int op_setmaxformcache(Xpost_Context *ctx, Xpost_Object N)
{
    if (!XPOST_MAY_SET_SYSTEM_PARAM(ctx))
        return invalidaccess;
    ctx->maxformcache = N.int_.val < 0 ? 0 : N.int_.val;
    return 0;
}

/* -  .formlimits  wholecache peritem
   The form cache's two byte parameters, for the cache itself to ask
   (data/init.ps). MaxFormCache bounds what the cache holds altogether and
   MaxFormItem the largest drawing it will keep (PLRM C.3.3).

   Both together, because the capture path wants both at the same moment
   and a second lookup could be answered from a different job's state. */
static
int op_formlimits(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ctx->maxformcache)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ctx->maxformitem)))
        return stackoverflow;
    return 0;
}

/* --- sealing the machinery ------------------------------------------

   A machinery object in global virtual memory has to be read-only. A
   restore does not reach global virtual memory (PLRM 3.7.2), so a write
   into one stands for the rest of the job, and the private namespaces are
   not out of a program's reach: a bound procedure carries the dictionary
   it names as a value, so walking the bodies the public private dictionary
   anchors arrives at every one of them.

   The rule cannot be kept from PostScript. An object's access is a
   property of the reference and not of the value for every composite but
   the dictionary (PLRM 3.3.2, and PLRM 8.2 readonly: "readonly affects the
   access attribute only of the object that it returns"), so sealing a
   value means storing a sealed reference back where it came from -- and
   that write is refused when the container is itself already sealed.
   Measured over the reachable set, four hundred and two of the writable
   objects sit inside a sealed container and no order of sealing reaches
   them.

   So the access bits are written into the stored word here, where a
   container's own access is not in the way. That leaves nothing for an
   order to get wrong and nothing for a list to be forgotten from: the
   population is everything the walk reaches, and the rule is asked of each
   of them. Local objects are left alone, and the bank is the whole of the
   reason -- a write to one is put back by the restore that ends the level
   it was made in.

   The walk is breadth-first over entities, which bounds it on a cyclic
   graph. An entity is entered once; every slot it holds is still asked,
   because one value reached from two containers is two references and
   each carries its own access. */

/* dict  .vmsweep  array
   Every composite the dictionary reaches that is writable and lives in
   global virtual memory, named by the path the walk took to it.

   A census, not a change: nothing here writes to what it finds. The point
   is to be able to ask the question of the whole reachable graph at once,
   in a test, rather than of whichever corner a reader thought to look in.

   The forbidden property is stated in one place, in _sweep_forbidden
   below: writable, in the global bank, and reached from a machinery root.
   A restore does not reach global virtual memory (PLRM 3.7.2), so a write
   into such an object stands for the rest of the job.

   Breadth-first over entities, which bounds the walk on a cyclic graph.
   An entity is entered once; every slot it holds is still examined,
   because one value reached from two containers is two references and
   each carries its own access (PLRM 3.3.2). */

typedef struct
{
    Xpost_Object obj;
    unsigned int parent;      /* index of the container, or self for a root */
    unsigned char isroot;
    unsigned char leadstocode; /* an executable array lies at or below this */
    unsigned char inbody;      /* a body lies above it: it is frozen into code */
    unsigned char instore;     /* the job store lies at or above it: it is
                                 state the machinery writes, not a constant
                                 a body froze in */
    unsigned char outside;     /* and it is reachable WITHOUT passing through
                                 the store, which makes it the machinery's
                                 own and not the store's, however the store
                                 may also reach it */
    unsigned char readable;    /* some route to it passes only through
                                 containers a program may read, so a program
                                 can obtain it and not merely run past it */
    char         label[48];   /* the key or index this was reached by */
} Xpost_Sweep_Node;

/* The property, in one place: an array the machinery executes, living in
   global virtual memory, that a program can write.

   Executable, because what a writable machinery object costs is that a
   program decides what the interpreter runs. Global, because a restore does
   not reach that bank (PLRM 3.7.2), so a write there stands for the rest of
   the job while a write to the local bank is put back. Reachable, because
   bind freezes the dictionary a helper names into the helper's body and a
   program can read a body.

   Data is a separate question with a separate answer. The record a page is
   counted in is written on every page and has to outlive a restore -- made
   local, three pages collapse onto one file -- so it is global and writable
   on purpose, and no program can make the machinery RUN it. */
static int _sweep_forbidden(Xpost_Context *ctx, Xpost_Object o, int leadstocode,
                            int inbody, int instore, int readable,
                            int banks, int kind, int forseal)
{
    int glob = (o.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? 1 : 0;
    int t = xpost_object_get_type(o);

    /* kind 7 is what closing a body execute-only is for: a body the
       machinery runs that a program can still READ, and so still lift
       what bind froze into it out of. It is asked before the writability
       test below, because a body already sealed read-only is not
       writable and is precisely what this has to go on counting. The
       route must be readable and so must the body: a container a program
       cannot open does not hand over what it holds, and a body it can
       reach but not read is closed. */
    if (kind == 7)
        return t == arraytype && xpost_object_is_exe(o)
            && readable && xpost_object_is_readable(ctx, o);

    /* Counting asks about what a program can still write, so an object
       already closed to writing is not one of them. Sealing is a
       different question: bind leaves a nested procedure read-only, so a
       body can arrive at the lockdown unwritable and still readable, and
       reading a body is how a program lifts out the dictionary bind
       froze into it. Those are exactly the ones left to close, so the
       seal is allowed past this. */
    if (!xpost_object_is_writeable(ctx, o)
        && !(forseal && t == arraytype && xpost_object_is_exe(o)
             && xpost_object_is_readable(ctx, o)))
        return 0;

    /* kind 1 counts rather than seals: the constants bind froze into a body,
       which a program reaches by reading the body and may then write. They
       hold no executable array, so the question the seal asks passes over
       them, and what they decide is how the machinery reads what a program
       gave it. Reported so that each one is either sealed where it is
       defined or answered for. */
    if (kind == 1)
        return inbody && !instore && readable && glob
               && (t == dicttype || t == stringtype
                   || (t == arraytype && !xpost_object_is_exe(o)));

    /* kind 2 is the other half of kind 1: what a body reaches that the job
       store holds. The store is the machinery's own writable state, so the
       constant rule passes over it -- and a hole nothing counts is a hole
       nobody can hold to a size, which is why it is counted here. The two
       kinds partition what a body reaches, so their counts sum to every
       writable constant-shaped object the machinery froze into itself. */
    if (kind == 2)
        return inbody && instore && glob
               && (t == dicttype || t == stringtype
                   || (t == arraytype && !xpost_object_is_exe(o)));

    /* kind 5 is what execute-only closes: a constant a body froze in that
       no route reaches through readable containers, so a program can make
       the machinery run past it and cannot take it. Counted apart from
       kind 1 rather than dropped, because a census that simply stopped
       seeing behind an execute-only body could not tell a hazard closed
       from a question it had stopped asking. */
    /* kind 6 is the question the local pair cannot ask. The seal reduces a
       dictionary only in global virtual memory -- the local ones are where
       the machinery keeps what it writes at every render, and sealing them
       stops the interpreter working (MEASURED: 101 tests) -- so the local
       pair's dictionary rule ends in `&& glob` and can only ever report an
       executable array. That makes `local 0 0` read as "nothing writable
       in local virtual memory" when it means something much weaker.
       Counted here instead: a table in local virtual memory that a program
       can write and the machinery can be made to run out of, which a
       restore and the job boundary take back rather than close. The
       device classes were the bulk of the population and are sealed at
       the lockdown now that nothing writes one after the boot builds it;
       what is left is the graphics state, which holds the procedures a
       program sets -- a transfer, a spot function -- and is written on
       every gsave, so it is state rather than a table to close. */
    if (kind == 6)
        return leadstocode && !instore && !glob;

    if (kind == 5)
        return inbody && !instore && !readable && glob
               && (t == dicttype || t == stringtype
                   || (t == arraytype && !xpost_object_is_exe(o)));
    /* banks selects which bank is being asked about: 1 global, 2 local, 3
       both. The seal asks for both; the census asks for one at a time,
       because what the two answers guarantee is not the same thing. */
    if (!(banks & (glob ? 1 : 2))) return 0;

    /* A body is executed and never written, in either bank, so it is sealed
       in either. In global virtual memory that closes the job (a restore does
       not reach there, PLRM 3.7.2); in local it closes the job's remainder,
       which is the whole of a request.

       A literal array a body froze in is sealed with them, in global virtual
       memory. The arrays the machinery writes as it runs are state and live
       in the job store, which answers for them the way it answers for the
       tables; what is left inside a body is a constant whatever it holds. */
    if (t == arraytype)
        return xpost_object_is_exe(o) || (inbody && !instore && glob);

    /* A table is sealed in global virtual memory when the machinery can be
       made to run out of it, and when a body froze it in. The local ones
       are where the machinery keeps what it writes at every render --
       that is why they are local. The device classes are the exception
       and are sealed where the lockdown can tell one from the graphics
       state, which is in PostScript: this rule cannot make that
       distinction. */
    if (t == dicttype)
        return (leadstocode || (inbody && !instore)) && glob;

    /* A string a body froze in is read to decide how the machinery reads
       what a program gave it -- a delimiter, a name, the text of an answer --
       and a program that reads the body reaches it. Sealed for the reason
       the tables are: a buffer written as the machinery runs is state, and
       state lives in the job store rather than among the procedures. */
    if (t == stringtype)
        return inbody && !instore && glob;

    return 0;
}

static void _sweep_path(Xpost_Sweep_Node *nodes, unsigned int i,
                        char *out, size_t outsz)
{
    const char *parts[64];
    unsigned int n = 0;
    size_t at = 0;

    while (n < 64)
    {
        parts[n++] = nodes[i].label;
        if (nodes[i].isroot) break;
        i = nodes[i].parent;
    }
    while (n-- > 0)
    {
        size_t l = strlen(parts[n]);
        if (at + l + 2 >= outsz) break;
        if (at) out[at++] = '/';
        memcpy(out + at, parts[n], l);
        at += l;
    }
    out[at] = 0;
}

static void _sweep_label(Xpost_Context *ctx, Xpost_Object k, int idx,
                         char *out, size_t outsz)
{
    if (idx >= 0)
    {
        snprintf(out, outsz, "[%d]", idx);
        return;
    }
    if (xpost_object_get_type(k) == nametype)
    {
        Xpost_Object sk = xpost_name_get_string(ctx, k);
        if (xpost_object_get_type(sk) == stringtype)
        {
            char *cp = xpost_string_get_pointer(ctx, sk);
            unsigned int n = sk.comp_.sz < outsz - 1
                           ? sk.comp_.sz : (unsigned int)outsz - 1;
            if (cp) { memcpy(out, cp, n); out[n] = 0; return; }
        }
    }
    snprintf(out, outsz, "<%d>", (int)xpost_object_get_type(k));
}

/* Reduce the access of the reference the container holds, in the container's
   own storage.

   Not through the container's put: an object's access belongs to the
   reference and not to the value for every composite but the dictionary
   (PLRM 3.3.2), so sealing means storing a sealed reference back -- and that
   write is refused by any container that is already read-only, which most of
   them are by the time this runs. Writing the word settles it without an
   order to get wrong. A dictionary is different and simpler: its access is a
   property of the value, so reducing it reaches every reference at once. */
static int _vm_is_root(Xpost_Sweep_Node *nodes, unsigned int nn,
                       Xpost_Object o)
{
    unsigned int i;

    for (i = 0; i < nn; i++)
    {
        if (!nodes[i].isroot) continue;
        /* identity, not the whole tag: the same dictionary reached through a
           member carries different access and literal bits from the object
           the caller named, and it is the same dictionary all the same */
        if (xpost_object_get_type(nodes[i].obj) == xpost_object_get_type(o)
            && nodes[i].obj.comp_.ent == o.comp_.ent
            && (nodes[i].obj.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)
               == (o.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK))
            return 1;
    }
    return 0;
}

static int _vm_seal_at(Xpost_Context *ctx, Xpost_Sweep_Node *nodes,
                       unsigned int nn, unsigned int i)
{
    Xpost_Object o = nodes[i].obj;
    Xpost_Object p;
    Xpost_Memory_File *mem;
    unsigned int ent;

    /* A namespace the caller named is never sealed here, by whatever route
       the walk arrives at it -- and they reach each other, so being a root
       is not something only the first node can be. The lockdown seals them
       itself, each at the point it has finished writing to it; sealing one
       here shuts it while the lockdown still has entries to record. */
    if ((xpost_object_get_type(o) != arraytype || !xpost_object_is_exe(o))
        && (nodes[i].isroot || _vm_is_root(nodes, nn, nodes[i].obj)))
        return 0;

    if (xpost_object_get_type(o) == dicttype)
    {
        (void) xpost_object_set_access(ctx, o,
                                       XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
        return 1;
    }

    p = nodes[nodes[i].parent].obj;
    mem = (p.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? ctx->gl : ctx->lo;
    if (!mem) return 0;
    ent = xpost_object_get_ent(p);
    if (!xpost_ent_valid(mem, ent)) return 0;

    /* A body stored under a name is run and never read, so it is closed
       to reading as well: execute-only leaves the machinery able to run
       it and takes away the route by which a program reads it and lifts
       out the dictionaries bind froze in (PLRM 3.3.2).

       A body nested inside another body keeps the weaker reduction. It
       is not a route of its own -- reaching it means reading the body
       above it, which is now closed -- and bind leaves nested procedures
       read-only by design (PLRM 8.2), with the machinery reading that
       structure as it works. Closing those as well breaks stroking.

       Everything that is not a body keeps the weaker reduction too: a
       constant the machinery reads has to stay readable to it. */
    o.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    o.tag |= ((xpost_object_get_type(o) == arraytype && xpost_object_is_exe(o)
               && xpost_object_get_type(p) == dicttype
               ? XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY
               : XPOST_OBJECT_TAG_ACCESS_READ_ONLY)
              << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);

    if (xpost_object_get_type(p) == dicttype)
    {
        dichead *dp = xpost_dict_head(mem, ent);
        dicrec *tp = xpost_dict_table_of(dp);
        unsigned int sz = DICTABN(dp->sz);
        unsigned int k;

        for (k = 0; k < sz; k++)
            if (xpost_object_get_type(tp[k].key) != nulltype
                && tp[k].value.tag == nodes[i].obj.tag
                && tp[k].value.comp_.ent == nodes[i].obj.comp_.ent
                && tp[k].value.comp_.off == nodes[i].obj.comp_.off
                && tp[k].value.comp_.sz  == nodes[i].obj.comp_.sz)
            {
                tp[k].value = o;
                return 1;
            }
        return 0;
    }
    if (xpost_object_get_type(p) == arraytype)
    {
        unsigned int k;

        for (k = 0; k < p.comp_.sz; k++)
        {
            Xpost_Object e;

            if (!xpost_memory_get(mem, ent, (unsigned int)(p.comp_.off + k),
                                  (unsigned int)sizeof e, &e))
                continue;
            if (e.tag == nodes[i].obj.tag
                && e.comp_.ent == nodes[i].obj.comp_.ent
                && e.comp_.off == nodes[i].obj.comp_.off
                && e.comp_.sz  == nodes[i].obj.comp_.sz)
            {
                XPOST_REFUSAL_IMPOSSIBLE(
                    xpost_memory_put(mem, ent,
                                     (unsigned int)(p.comp_.off + k),
                                     (unsigned int)sizeof o, &o));
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* Whether two references name the same object. A dictionary IS its entity,
   and its offset is a cursor into the table rather than part of which object
   it is; a string or an array is a window onto one -- several of them share
   an entity and are told apart by where they start and how long they are.
   Comparing entities alone answers yes for two different strings cut from
   the same allocation, which is how a planted control string came to be
   taken for the job store's own. */
static
int _vm_same_object(Xpost_Object a, Xpost_Object b)
{
    if (xpost_object_get_type(a) != xpost_object_get_type(b)) return 0;
    if (!xpost_object_is_composite(a) || !xpost_object_is_composite(b))
        return 0;
    if (a.comp_.ent != b.comp_.ent) return 0;
    if ((a.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)
        != (b.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)) return 0;
    if (xpost_object_get_type(a) == dicttype) return 1;
    return a.comp_.off == b.comp_.off && a.comp_.sz == b.comp_.sz;
}

static int _vm_in_job_store(Xpost_Context *ctx, Xpost_Object o);

/* The registry's name, resolved once as the operators install rather than
   interned on every sweep: a spelling handed to the name constructor on a
   path a job re-enters is what tests/name_interning.register exists to
   refuse, and the sweep is asked for on every census. */
static
Xpost_Object name_dotresources;

/* Whether an object is a value of the dictionary D holds under `key`.
   Used to ask whether something is one of the resource registry's
   per-category instance stores, which defineresource writes (PLRM 3.9). */
static
int _vm_value_of_member(Xpost_Context *ctx, Xpost_Object D, Xpost_Object key,
                        Xpost_Object o)
{
    Xpost_Memory_File *mem;
    Xpost_Object R;
    dichead *dp;
    dicrec *tp;
    unsigned int ent, sz, k;

    if (xpost_object_get_type(D) != dicttype) return 0;
    R = xpost_dict_get(ctx, D, key);
    if (xpost_object_get_type(R) != dicttype) return 0;
    mem = (R.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? ctx->gl : ctx->lo;
    if (!mem) return 0;
    ent = xpost_object_get_ent(R);
    if (!xpost_ent_valid(mem, ent)) return 0;
    dp = xpost_dict_head(mem, ent);
    tp = xpost_dict_table_of(dp);
    sz = DICTABN(dp->sz);
    for (k = 0; k < sz; k++)
        if (xpost_object_get_type(tp[k].key) != nulltype
            && _vm_same_object(tp[k].value, o))
            return 1;
    return 0;
}

/* The storage the language hands programs to write, derived rather than
   listed. It used to be named by the caller, and a list has to be kept
   where whoever passes it can reach it -- which is how the machinery's own
   namespaces came to be readable by any program. It is the job store and
   what the store holds, and the resource registry's per-category instance
   stores, which defineresource writes and which hold procedure sets: they
   answer the forbidden shape while being exactly what the language says a
   program may write. */
static
int _vm_declared(Xpost_Context *ctx, Xpost_Object o)
{
    if (_vm_in_job_store(ctx, o)) return 1;
    if (_vm_value_of_member(ctx, ctx->jobstore, name_dotresources, o))
        return 1;
    if (_vm_value_of_member(ctx, ctx->globalprivatedict, name_dotresources, o))
        return 1;
    return 0;
}

/* Whether an object is the job store or one of the members it holds. The
   store is where the machinery keeps what it writes as it runs, so what is
   under it is state and not a constant a body froze in -- reaching it
   through a reference baked into a body does not change that. Held apart
   from the declared list, which also carries the resource registry: what a
   resource holds is program data and stays subject to every rule. */
static
int _vm_in_job_store(Xpost_Context *ctx, Xpost_Object o)
{
    Xpost_Object st = xpost_context_job_store(ctx);
    Xpost_Memory_File *mem;
    dichead *dp;
    dicrec *tp;
    unsigned int ent, sz, k;

    if (xpost_object_get_type(st) != dicttype) return 0;
    if (_vm_same_object(st, o)) return 1;

    mem = (st.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? ctx->gl : ctx->lo;
    if (!mem) return 0;
    ent = xpost_object_get_ent(st);
    if (!xpost_ent_valid(mem, ent)) return 0;
    dp = xpost_dict_head(mem, ent);
    tp = xpost_dict_table_of(dp);
    sz = DICTABN(dp->sz);
    for (k = 0; k < sz; k++)
        if (xpost_object_get_type(tp[k].key) != nulltype
            && _vm_same_object(tp[k].value, o))
            return 1;
    return 0;
}

/* The one walk, in two modes. Mode 0 reports the paths of the objects with
   the forbidden property; mode 1 seals them. Both filter identically and in
   the same order, so the nth object the report names is the nth the seal
   reaches -- which is what lets a breakage be bisected by index without
   anything having to be named by hand.

   lo and hi bound which of them mode 1 acts on. The enforcement passes the
   whole range; a bisection passes a part of it. */
static
int _vm_walk(Xpost_Context *ctx, int banks,
             int kind, int seal, unsigned int lo, unsigned int hi)
{
    unsigned char *bits[2] = { NULL, NULL };
    unsigned int *owner[2] = { NULL, NULL };
    struct { unsigned int from, to; } *alias = NULL;
    unsigned int nalias = 0, acap = 0;
    unsigned int nent[2] = { 0, 0 };
    Xpost_Sweep_Node *nodes = NULL;
    unsigned int nn = 0, ncap = 0, qh = 0;
    Xpost_Object *hits = NULL;
    unsigned int nhits = 0, hcap = 0;
    Xpost_Object arr;
    int ret = 0;
    int b;
    unsigned int i;
    unsigned int ix;
    unsigned int hit = 0;
    unsigned int nsealed = 0;

    for (b = 0; b < 2; b++)
    {
        Xpost_Memory_File *m = b ? ctx->gl : ctx->lo;
        nent[b] = m ? m->table.nextent : 0;
        /* A bank with no entities has nothing to walk, and gets no
           tables. Everything reached through them is written inside the
           branch that allocated them, so that what cannot be reached
           when they are absent is not reachable in the text either. */
        if (nent[b])
        {
            bits[b] = calloc((nent[b] >> 3) + 1, 1);
            if (!bits[b]) { ret = VMerror; goto done; }
            /* Which node expanded each entity. A second reference to the
               same object is not expanded again -- that is what stops a
               cycle -- but it still says something about the object, so
               it is remembered here and joined to the one that did the
               expanding. */
            owner[b] = malloc(nent[b] * sizeof *owner[b]);
            if (!owner[b]) { ret = VMerror; goto done; }
            for (i = 0; i < nent[b]; i++) owner[b][i] = (unsigned int)-1;
        }
        else
        {
            bits[b] = NULL;
            owner[b] = NULL;
        }
    }

#define SWEEP_NODE(o_, par_, root_, lab_)                                  \
    do {                                                                   \
        if (nn == ncap)                                                    \
        {                                                                  \
            unsigned int nc = ncap ? ncap * 2 : 512;                       \
            Xpost_Sweep_Node *nx = realloc(nodes, nc * sizeof *nx);        \
            if (!nx) { ret = VMerror; goto done; }                         \
            nodes = nx; ncap = nc;                                         \
        }                                                                  \
        nodes[nn].obj = (o_);                                              \
        nodes[nn].parent = (par_);                                         \
        nodes[nn].isroot = (root_);                                        \
        nodes[nn].leadstocode = 0;                                         \
        nodes[nn].inbody = 0;                                         \
        nodes[nn].instore = 0;                                            \
        nodes[nn].outside = 0;                                             \
        nodes[nn].readable = 0;                                            \
        snprintf(nodes[nn].label, sizeof nodes[nn].label, "%s", (lab_));   \
        nn++;                                                              \
    } while (0)

    /* The roots the context holds in its own fields -- the array being
       executed, the arc procedure, the page device, the file a run wrapped
       around its program -- are roots of this walk too. They are reachable
       by the interpreter and nameable from no dictionary, so a list written
       in PostScript cannot include them, and leaving them out left three
       arrays of operator objects that nothing here was ever asked about.
       Seeded from the same X-macro that declares them, so a root added to
       the context joins the walk without this being revisited. */
#define XPOST_SWEEP_CTX_ROOT(f_) \
    if (xpost_object_is_composite(ctx->f_)) SWEEP_NODE(ctx->f_, 0, 1, "root");
    /* The namespaces. The caller used to name these in a list, and a list
       has to be kept where whoever reads it can reach it -- which put the
       machinery's own namespaces, and the job store, within reach of any
       program that could read the dictionary holding it. They are context
       fields, so they are taken from there instead and no list exists to
       be read. It also settles what two lists could not: the seal and the
       census walk the same graph because neither chooses it. */
    XPOST_SWEEP_CTX_ROOT(privatedict)
    XPOST_SWEEP_CTX_ROOT(globalprivatedict)
    XPOST_SWEEP_CTX_ROOT(jobstore)
    /* systemdict is the bottom of the dictionary stack. It is left out of
       the local-bank question alone: it reaches userdict, and so reaches
       whatever the running program has defined, which would make that
       number move whenever a test was edited. .internaldict needs no
       seeding -- the procedure that answers the password holds it frozen
       in its body, and this walk descends into bodies.

       The bodies question leaves it out for the same reason: it asks
       which of the MACHINERY's bodies a program can read, and a walk
       that started at systemdict would reach userdict and count the
       asking program's own procedures -- a number that moved whenever
       the test that reads it was edited. */
    if (banks != 2 && kind != 7)
    {
        Xpost_Object sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);

        if (xpost_object_get_type(sd) == dicttype)
            SWEEP_NODE(sd, 0, 1, "root");
    }
    XPOST_SWEEP_CTX_ROOT(arcstartproc)
    XPOST_SWEEP_CTX_ROOT(pagedevice)
    XPOST_SWEEP_CTX_ROOT(pagedevice_destroy)
    XPOST_SWEEP_CTX_ROOT(run_input_file)
    XPOST_SWEEP_CTX_ROOT(window_device)
    XPOST_SWEEP_CTX_ROOT(event_handler)
    XPOST_SWEEP_CTX_ROOT(localfontdir)
    XPOST_SWEEP_CTX_ROOT(globalfontdir)
#undef XPOST_SWEEP_CTX_ROOT

    while (qh < nn)
    {
        unsigned int self = qh;
        Xpost_Object o = nodes[qh++].obj;
        Xpost_Memory_File *mem;
        int bank;
        unsigned int ent;

        if (!xpost_object_is_composite(o)) continue;
        if (xpost_object_get_type(o) == filetype) continue;
        /* Access is not consulted here. This walk is C reading the arena,
           and access is a rule the OPERATORS keep -- so an execute-only
           body, which a program may run and may not read, is still walked
           and still sealed. Refusing to look would make the census go blind
           exactly where execute-only was applied, and a count that fell
           because it stopped looking reads the same as one that fell
           because a hazard closed. Which of the two it is, is what the
           readable mark answers. */

        bank = (o.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? 1 : 0;
        mem = bank ? ctx->gl : ctx->lo;
        if (!mem) continue;
        ent = xpost_object_get_ent(o);
        if (!xpost_ent_valid(mem, ent)) continue;
        if (ent >= nent[bank]) continue;
        if (bits[bank][ent >> 3] & (1 << (ent & 7)))
        {
            /* The bit is per ENTITY, and a string or an array is a window
               onto one: two of them can share an entity and still be
               different objects. A reference to the SAME object is a
               revisit -- the children are not enumerated again, so whatever
               this route says about the object has to travel to the route
               that did enumerate them, or a table reached once outside a
               body and once through one keeps the answer the first route
               gave and what a program reaches through the body goes
               unsealed. A different window on the same allocation is not a
               revisit at all, and is walked in its own right; taking it for
               one carried the job store's mark onto objects that were never
               in the store. */
            unsigned int own = owner[bank][ent];

            if (own == (unsigned int)-1 || _vm_same_object(nodes[own].obj, o))
            {
                if (own != (unsigned int)-1 && own != self)
                {
                    if (nalias == acap)
                    {
                        unsigned int nc = acap ? acap * 2 : 256;
                        void *nx = realloc(alias, nc * sizeof *alias);

                        if (!nx) { ret = VMerror; goto done; }
                        alias = nx; acap = nc;
                    }
                    alias[nalias].from = self;
                    alias[nalias].to = own;
                    nalias++;
                }
                continue;
            }
        }
        else
        {
            bits[bank][ent >> 3] |= (unsigned char)(1 << (ent & 7));
            owner[bank][ent] = self;
        }

        if (xpost_object_get_type(o) == dicttype)
        {
            dichead *dp = xpost_dict_head(mem, ent);
            dicrec *tp = xpost_dict_table_of(dp);
            unsigned int sz = DICTABN(dp->sz);
            unsigned int k;

            for (k = 0; k < sz; k++)
            {
                char lab[48];

                if (xpost_object_get_type(tp[k].key) == nulltype) continue;
                _sweep_label(ctx, tp[k].key, -1, lab, sizeof lab);
                SWEEP_NODE(tp[k].value, self, 0, lab);
                dp = xpost_dict_head(mem, ent);
                tp = xpost_dict_table_of(dp);
            }
        }
        else if (xpost_object_get_type(o) == arraytype)
        {
            unsigned int k;

            for (k = 0; k < o.comp_.sz; k++)
            {
                Xpost_Object e;
                char lab[48];

                if (!xpost_memory_get(mem, ent,
                                      (unsigned int)(o.comp_.off + k),
                                      (unsigned int)sizeof e, &e))
                    continue;
                _sweep_label(ctx, null, (int)k, lab, sizeof lab);
                SWEEP_NODE(e, self, 0, lab);
            }
        }
    }

    /* kind 4 asks a different question and stops here, before the marks
       that answer the other three. The walk has just recorded which
       entities these roots reach; the collector marks from the ones the
       INTERPRETER has -- the stacks, the save stack, the name stack, each
       context -- and what it reaches that this did not is what the roots
       named in data/init.ps do not cover. That is the one number saying
       how much of the arena every other number here is about.

       Marking is safe at the point this runs and nowhere else: the caller
       is the interpreter's safe point, where no operator's C frames hold
       storage the marker would have to know about. It is a mark and not a
       collection -- nothing is swept and nothing moves -- so what the walk
       recorded stays true across it. */
    if (kind == 4)
    {
        Xpost_Memory_File *m = ctx->gl;
        unsigned int e, reach = 0, missed = 0, strings = 0;

        if (m && bits[1] && ctx->lo && ctx->lo->garbage_collect_is_installed)
        {
            /* Asked of the local bank with markall, which is how every
               other caller asks: the walk covers both banks from there. */
            if (ctx->lo->garbage_collect(ctx->lo, 0, 1) < 0)
            {
                ret = VMerror;
                goto done;
            }
            for (e = 0; e < nent[1]; e++)
            {
                unsigned int tg;

                if (!(m->table.tab[e].mark
                      & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK))
                    continue;
                reach++;
                if (bits[1][e >> 3] & (1 << (e & 7))) continue;
                tg = m->table.tab[e].tag;
                /* Split, because the two halves mean different things. A
                   dictionary or an array can hold what the machinery runs,
                   so one the walk never sees is a root the walk has not
                   been given. A string cannot hold anything: the ones here
                   are the text of interned names, which the name tree holds
                   and no object graph reaches, and counting them beside the
                   others would bury the number that matters under one that
                   moves whenever a name is interned. */
                if (tg == dicttype || tg == arraytype)
                    missed++;
                else
                    strings++;
            }
            m->blind_reach = reach;
            m->blind_missed = missed;
            m->blind_missed_str = strings;
        }
        goto done;
    }

    /* What is frozen into a body, and what the job store holds. bind and
       immediate evaluation put a table inside a procedure, and a program
       that reads the body reaches it; the store is where the machinery keeps
       what it writes as it runs, so what is under it is state and not a
       constant, even when a body is what reached it.

       Both marks belong to the OBJECT and not to the route taken to it: a
       program reaches whatever any route reaches. So this is a fixed point
       and not one pass. A parent's index is below its children's, the walk
       being breadth first, so the tree edges settle in a single sweep; the
       joins recorded above run the other way, and are what needs the
       repeat. */
    for (i = 0; i < nn; i++)
    {
        nodes[i].instore = _vm_in_job_store(ctx, nodes[i].obj);
        /* A root that is not the store is the machinery's own ground. */
        nodes[i].outside = nodes[i].isroot && !nodes[i].instore;
        /* A root is readable if a program may read it at all. */
        nodes[i].readable = nodes[i].isroot
                            && xpost_object_is_readable(ctx, nodes[i].obj);
    }
    for (;;)
    {
        int moved = 0;
        unsigned int a;

        for (i = 0; i < nn; i++)
        {
            unsigned int par = nodes[i].parent;

            if (nodes[i].isroot) continue;
            if (!nodes[i].instore && nodes[par].instore)
            {
                nodes[i].instore = 1; moved = 1;
            }
            /* The store is a barrier to this one: a route that goes through
               something the store holds says nothing about whether the
               machinery reaches the object on its own. */
            if (!nodes[i].outside && nodes[par].outside
                && !_vm_in_job_store(ctx, nodes[i].obj))
            {
                nodes[i].outside = 1; moved = 1;
            }
            /* Readable by a route, not by the object: a program that cannot
               read the container cannot obtain what is inside it, however
               freely the machinery runs past. This is what tells a constant
               a program can still take from a body apart from one an
               execute-only body has closed. */
            if (!nodes[i].readable && nodes[par].readable
                && xpost_object_is_readable(ctx, nodes[par].obj))
            {
                nodes[i].readable = 1; moved = 1;
            }
            if (!nodes[i].inbody
                && (nodes[par].inbody
                    || (xpost_object_get_type(nodes[par].obj) == arraytype
                        && xpost_object_is_exe(nodes[par].obj))))
            {
                nodes[i].inbody = 1; moved = 1;
            }
        }
        for (a = 0; a < nalias; a++)
        {
            unsigned int f = alias[a].from, t = alias[a].to;

            if (nodes[f].inbody && !nodes[t].inbody)
            {
                nodes[t].inbody = 1; moved = 1;
            }
            if (nodes[f].instore && !nodes[t].instore)
            {
                nodes[t].instore = 1; moved = 1;
            }
            if (nodes[f].outside && !nodes[t].outside)
            {
                nodes[t].outside = 1; moved = 1;
            }
            if (nodes[f].readable && !nodes[t].readable)
            {
                nodes[t].readable = 1; moved = 1;
            }
        }
        if (!moved) break;
    }

    /* What the store holds, and the machinery cannot reach any other way.
       An object the machinery reaches on its own is the machinery's, even
       where the store reaches it too -- the store's resources hold
       procedure sets, and those reach the dispatch tables, so a mark that
       spread by reachability alone made the machinery's own tables read as
       the store's state and spared them from the rule. Where an object
       genuinely is both, the answer is to split it and not to except it. */
    for (i = 0; i < nn; i++)
        if (nodes[i].outside) nodes[i].instore = 0;

    /* Which dictionaries lead to something the machinery executes.
       A table whose entries are tables of procedures is the same hazard as
       one holding the procedures: the entry can be replaced, and what the
       machinery then looks up is the replacement. So the question is asked
       of the whole path, by marking every container above an executable
       array, and not only of the table the array sits in. */
    for (i = 0; i < nn; i++)
    {
        unsigned int a;

        if (xpost_object_get_type(nodes[i].obj) != arraytype) continue;
        if (!xpost_object_is_exe(nodes[i].obj)) continue;
        a = i;
        while (!nodes[a].isroot)
        {
            a = nodes[a].parent;
            if (nodes[a].leadstocode) break;   /* the rest is marked already */
            nodes[a].leadstocode = 1;
        }
    }

    /* Deepest first. Reducing an object's access means storing the reduced
       reference back into its container, so a container sealed before its
       contents leaves them nowhere to be stored -- the walk is breadth-first,
       so the order it discovered them in is exactly the wrong one. Both modes
       use this order, so the nth object the report names is still the nth the
       seal reaches, which is what lets a breakage be bisected by index.

       The report is built after the walk, so that allocating it cannot
       disturb what the walk was reading. */
    for (ix = 0; ix < nn; ix++)
    {
        i = nn - 1 - ix;
        if (!_sweep_forbidden(ctx, nodes[i].obj, nodes[i].leadstocode,
                              nodes[i].inbody, nodes[i].instore,
                              nodes[i].readable, banks, kind, seal))
            continue;
        /* Storage the language has programs write is not machinery, however
           much code it leads to: the resource registry and its per-category
           instance stores are written by defineresource (PLRM 3.9), so the
           caller declares them and the walk passes over them -- while still
           descending, so what sits below them is held to the rule. */
        if (_vm_declared(ctx, nodes[i].obj)) continue;
        if (seal)
        {
            unsigned int k = hit++;

            if (k < lo || k >= hi) continue;
            if (_vm_seal_at(ctx, nodes, nn, i)) nsealed++;
            continue;
        }
        ++hit;
        if (nhits == hcap)
        {
            unsigned int nc = hcap ? hcap * 2 : 64;
            Xpost_Object *nx = realloc(hits, nc * sizeof *nx);
            if (!nx) { ret = VMerror; goto done; }
            hits = nx; hcap = nc;
        }
        {
            char path[512];
            Xpost_Object s;

            _sweep_path(nodes, i, path, sizeof path);
            /* literal, like the array below: a report is data, and an
               executable string is run as source the moment it is named */
            s = xpost_object_cvlit(
                    xpost_string_cons(ctx, (unsigned int)strlen(path), path));
            if (xpost_object_get_type(s) == invalidtype) { ret = VMerror; goto done; }
            hits[nhits++] = s;
        }
    }

    /* literal: a report is data, and an executable array of strings names
       itself into execution the moment a caller mentions it */
    if (seal)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)nsealed));
        goto done;
    }
    arr = xpost_object_cvlit(xpost_array_cons(ctx, nhits));
    if (xpost_object_get_type(arr) == invalidtype) { ret = VMerror; goto done; }
    for (i = 0; i < nhits; i++)
        XPOST_REFUSAL_IMPOSSIBLE(
            xpost_array_put_memory(xpost_context_select_memory(ctx, arr),
                                   arr, (integer)i, hits[i]));
    xpost_stack_push(ctx->lo, ctx->os, arr);

done:
#undef SWEEP_NODE
    free(nodes);
    free(hits);
    free(bits[0]);
    free(bits[1]);
    free(owner[0]);
    free(owner[1]);
    free(alias);
    return ret;
}

/* banks kind  .vmsweep  array
   banks: 1 global virtual memory, 2 local, 3 both.
   kind:  0 what the seal acts on, 1 the constants frozen into a body. */
static
int op_vmsweep(Xpost_Context *ctx, Xpost_Object B, Xpost_Object K)
{
    /* The introspection operators answer only a run that asked for
       introspection. .vmsweep hands back the PATHS of what is still
       writable, which is the map tests/vm_forbidden.golden refuses to
       carry -- "a list of the objects that still have the property is a
       map for whoever would like to use them" -- and handing it to any
       program that asks undoes that. The others give the machinery's own
       member names and the host's settings, which are smaller maps of the
       same kind. */
    if (!getenv("XPOST_CENSUS")) return invalidaccess;
    if (B.int_.val < 1 || B.int_.val > 3) return rangecheck;
    if (K.int_.val < 0
        || (K.int_.val > 2 && K.int_.val != 5 && K.int_.val != 6
            && K.int_.val != 7))
        return rangecheck;
    return _vm_walk(ctx, (int)B.int_.val, (int)K.int_.val, 0, 0, 0);
}

/* Takes the census kind 4 describes, from the roots the lockdown named.
   Called at the interpreter's safe point and nowhere else -- see the kind
   itself for why -- and answers 0 or an error code. */
int xpost_vm_blind_measure(Xpost_Context *ctx)
{
    if (!ctx->gl) return 0;
    return _vm_walk(ctx, 1, 4, 0, 0, 0);
}

/* name|null  .jobmembernames  array
   The names a job-store member holds, or the store's own when given null.
   Names and nothing else: a register that wants to know WHAT is in there
   does not need a reference it could write through, and handing the
   dictionary over is how the store came to be reachable by any program in
   the first place. */
/* The key names of a dictionary, pushed as a literal array. Shared by
   the accessors below, which each answer for a different dictionary and
   answer the same way: with names. */
static int
_dict_names_push(Xpost_Context *ctx, Xpost_Object D)
{
    Xpost_Memory_File *mem;
    Xpost_Object arr;
    dichead *dp;
    dicrec *tp;
    unsigned int ent, sz, k, n = 0;

    mem = (D.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? ctx->gl : ctx->lo;
    if (!mem) return undefined;
    ent = xpost_object_get_ent(D);
    if (!xpost_ent_valid(mem, ent)) return undefined;
    dp = xpost_dict_head(mem, ent);
    tp = xpost_dict_table_of(dp);
    sz = DICTABN(dp->sz);
    for (k = 0; k < sz; k++)
        if (xpost_object_get_type(tp[k].key) == nametype)
            n++;
    arr = xpost_object_cvlit(xpost_array_cons(ctx, n));
    if (xpost_object_get_type(arr) != arraytype) return VMerror;
    dp = xpost_dict_head(mem, ent);
    tp = xpost_dict_table_of(dp);
    n = 0;
    for (k = 0; k < sz; k++)
        if (xpost_object_get_type(tp[k].key) == nametype)
        {
            int ret = xpost_array_put(ctx, arr, (integer)n++, tp[k].key);

            if (ret) return ret;
            dp = xpost_dict_head(mem, ent);
            tp = xpost_dict_table_of(dp);
        }
    xpost_stack_push(ctx->lo, ctx->os, arr);
    return 0;
}

static
int op_jobmembernames(Xpost_Context *ctx, Xpost_Object N)
{
    Xpost_Object D = ctx->jobstore;

    if (!getenv("XPOST_CENSUS")) return invalidaccess;
    if (xpost_object_get_type(D) != dicttype) return undefined;
    if (xpost_object_get_type(N) == nametype)
    {
        D = xpost_dict_get(ctx, D, N);
        if (xpost_object_get_type(D) != dicttype) return undefined;
    }
    else if (xpost_object_get_type(N) != nulltype)
        return typecheck;
    return _dict_names_push(ctx, D);
}

/* The two spellings a namespace is asked for by, resolved once as the
   operators install: interning them here would intern on every call, and
   this is asked for in a loop. */
static Xpost_Object name_xpostsys;
static Xpost_Object name_privatedict;

/* Which namespace a name selects. */
static Xpost_Object
_namespace_of(Xpost_Context *ctx, Xpost_Object N)
{
    if (xpost_dict_compare_objects(ctx, N, name_xpostsys) == 0)
        return ctx->globalprivatedict;
    if (xpost_dict_compare_objects(ctx, N, name_privatedict) == 0)
        return ctx->privatedict;
    return null;
}

/* name  .namespacewritable  bool
   Whether one of the machinery's namespaces is still writable. The
   question is worth asking directly rather than inferring from the
   census, and a yes-or-no answers it without handing over the reference
   the answer is about. */
static
int op_namespacewritable(Xpost_Context *ctx, Xpost_Object N)
{
    Xpost_Object D;

    if (!getenv("XPOST_CENSUS")) return invalidaccess;
    if (xpost_object_get_type(N) != nametype) return typecheck;
    D = _namespace_of(ctx, N);
    if (xpost_object_get_type(D) != dicttype) return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_object_is_writeable(ctx, D)));
    return 0;
}

/* name key  .namespacevalue  any
   What one of the machinery's namespaces holds under a key. The scanners
   that walk the machinery looking for what must not be there need the
   values, not just the names, and used to reach them by reading a bound
   body -- the route the bodies are now closed to. This answers the same
   question without handing over the namespace itself, which is the
   reference a program would write through. Undefined where the key is
   not held, so a scan cannot mistake absence for an answer. */
static
int op_namespacevalue(Xpost_Context *ctx, Xpost_Object N, Xpost_Object K)
{
    Xpost_Object D, v;

    if (!getenv("XPOST_CENSUS")) return invalidaccess;
    if (xpost_object_get_type(N) != nametype) return typecheck;
    D = _namespace_of(ctx, N);
    if (xpost_object_get_type(D) != dicttype) return undefined;
    v = xpost_dict_get(ctx, D, K);
    if (xpost_object_get_type(v) == invalidtype) return undefined;
    xpost_stack_push(ctx->lo, ctx->os, v);
    return 0;
}

/* name  .namespacenames  array
   The names one of the machinery's namespaces holds. Names and nothing
   else, for the reason the job store's accessor gives: a register that
   wants to know WHAT is in there does not need a reference it could
   write through, and a body closed to reading is exactly the route this
   replaces -- a guard that recovered a namespace by reading the
   dictionary a bound body froze in was reading what the machinery runs
   to find out what the machinery holds. */
static
int op_namespacenames(Xpost_Context *ctx, Xpost_Object N)
{
    Xpost_Object D;

    if (!getenv("XPOST_CENSUS")) return invalidaccess;
    if (xpost_object_get_type(N) != nametype) return typecheck;
    D = _namespace_of(ctx, N);
    if (xpost_object_get_type(D) != dicttype) return undefined;
    return _dict_names_push(ctx, D);
}

/* name  .jobmemberwritable  bool
   Whether a job-store member is still writable. The seal is shallow on
   purpose -- the store's members are what the machinery writes as it runs
   -- and that is a property worth asserting directly rather than inferring
   from the census. A yes-or-no answers it without handing over anything
   that could be written through. */
static
int op_jobmemberwritable(Xpost_Context *ctx, Xpost_Object N)
{
    Xpost_Object D;

    if (!getenv("XPOST_CENSUS")) return invalidaccess;

    if (xpost_object_get_type(N) != nametype) return typecheck;
    if (xpost_object_get_type(ctx->jobstore) != dicttype) return undefined;
    D = xpost_dict_get(ctx, ctx->jobstore, N);
    if (xpost_object_get_type(D) == invalidtype) return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_object_is_writeable(ctx, D)));
    return 0;
}

/* name  .hostsetting  any
   What the host said this run was started with, by name. A value, not the
   dictionary holding it, and undefined where the name is not a setting. */
static
int op_hostsetting(Xpost_Context *ctx, Xpost_Object N)
{
    Xpost_Object v;
    Xpost_Object ns;
    char *cp;
    char buf[128];

    if (!getenv("XPOST_CENSUS")) return invalidaccess;

    if (xpost_object_get_type(N) != nametype) return typecheck;
    ns = xpost_name_get_string(ctx, N);
    cp = xpost_string_get_pointer(ctx, ns);
    if (!cp || ns.comp_.sz >= sizeof buf) return rangecheck;
    /* the settings are named in C text, which ends at the first nul: a
       name carrying one would be read as the shorter name it begins with
       and answer with that setting's value. A name counts its characters
       (PLRM 3.3), so such a name is not a setting at all */
    if (!xpost_string_is_cstring(ctx, ns)) return undefined;
    memcpy(buf, cp, ns.comp_.sz);
    buf[ns.comp_.sz] = 0;
    v = xpost_context_host_setting(ctx, buf);
    /* A name that is not a setting at all is refused where it is asked
       for, rather than answered with something the caller carries off
       and reads somewhere the mistake no longer looks like one. A
       setting the invocation did not supply is a different thing: the
       name is a setting, and its answer is that nothing was given. */
    if (xpost_object_get_type(v) == invalidtype)
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, v);
    return 0;
}

/* -  .vmdeclaredcount  int
   How many objects the sweep passes over as storage the language hands
   programs to write. The set is derived, not listed, so this is the only
   way to ask its size -- and the register ratchets it, because a hole that
   nothing counts is one nobody can hold to a size. */
static
int op_vmdeclaredcount(Xpost_Context *ctx)
{
    Xpost_Object st = ctx->jobstore;
    Xpost_Memory_File *mem;
    dichead *dp;
    dicrec *tp;
    unsigned int ent, sz, k, n = 0;

    if (xpost_object_get_type(st) == dicttype)
    {
        mem = (st.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? ctx->gl : ctx->lo;
        ent = xpost_object_get_ent(st);
        if (mem && xpost_ent_valid(mem, ent))
        {
            dp = xpost_dict_head(mem, ent);
            tp = xpost_dict_table_of(dp);
            sz = DICTABN(dp->sz);
            for (k = 0; k < sz; k++)
                if (xpost_object_get_type(tp[k].key) != nulltype)
                    n++;
            n++;                       /* the store itself */
        }
    }
    {
        /* and the registry's per-category instance stores, from wherever
           the registry is kept */
        int b;
        Xpost_Object where[2];

        where[0] = ctx->jobstore;
        where[1] = ctx->globalprivatedict;
        for (b = 0; b < 2; b++)
        {
            Xpost_Object R;

            if (xpost_object_get_type(where[b]) != dicttype) continue;
            R = xpost_dict_get(ctx, where[b], name_dotresources);
            if (xpost_object_get_type(R) != dicttype) continue;
            mem = (R.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? ctx->gl : ctx->lo;
            ent = xpost_object_get_ent(R);
            if (!mem || !xpost_ent_valid(mem, ent)) continue;
            dp = xpost_dict_head(mem, ent);
            tp = xpost_dict_table_of(dp);
            sz = DICTABN(dp->sz);
            for (k = 0; k < sz; k++)
                if (xpost_object_get_type(tp[k].key) != nulltype)
                    n++;
        }
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)n));
    return 0;
}

/* -  .vmblind  -
   Asks for that census. Taken at the safe point, so this sets the request
   and returns; .vmblindcount reads what the answer was. */
static
int op_vmblind(Xpost_Context *ctx)
{
    if (!ctx->gl) return 0;
    /* Only when asked for. Taking it walks the whole graph and marks from
       the collector's roots, which cost 336 KiB of peak resident memory at
       every startup when this was unconditional -- and a test that holds a
       band route to weighing less than a whole page reads that as the band
       failing to bound what it holds. The census is an instrument, so it
       is paid for by the run that wants a reading; .vmblindcount answers
       zero reached when none was taken, and the guard refuses that rather
       than reporting it as nothing missed. */
    if (!getenv("XPOST_CENSUS")) return 0;
    ctx->gl->blind_pending = 1;
    return 0;
}

/* -  .vmblindcount  reached missed missedstrings */
static
int op_vmblindcount(Xpost_Context *ctx)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)(ctx->gl ? ctx->gl->blind_reach : 0)));
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)(ctx->gl ? ctx->gl->blind_missed : 0)));
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)(ctx->gl ? ctx->gl->blind_missed_str : 0)));
    return 0;
}

/* lo hi  .vmseal  int
   Seals the objects the sweep would report whose place in that report falls
   in [lo,hi), and answers how many it sealed. The enforcement asks for the
   whole range; a bisection asks for a part. */
static
int op_vmseal(Xpost_Context *ctx, Xpost_Object LO, Xpost_Object HI)
{
    if (LO.int_.val < 0 || HI.int_.val < 0) return rangecheck;
    return _vm_walk(ctx, 3, 0, 1, (unsigned int)LO.int_.val,
                    (unsigned int)HI.int_.val);
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
    op = xpost_operator_cons(ctx, ".vmsweep", (Xpost_Op_Func)op_vmsweep, 2, integertype, integertype); INSTALL;
    if (xpost_object_get_type(
            (name_dotresources = xpost_name_cons(ctx, ".resources")))
        == invalidtype)
        return VMerror;
    if (xpost_object_get_type(
            (name_xpostsys = xpost_name_cons(ctx, "xpostsys"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type(
            (name_privatedict = xpost_name_cons(ctx, "privatedict")))
        == invalidtype)
        return VMerror;
    op = xpost_operator_cons(ctx, ".jobmembernames", (Xpost_Op_Func)op_jobmembernames, 1, anytype); INSTALL;
    op = xpost_operator_cons(ctx, ".namespacenames", (Xpost_Op_Func)op_namespacenames, 1, nametype); INSTALL;
    op = xpost_operator_cons(ctx, ".namespacevalue", (Xpost_Op_Func)op_namespacevalue, 2, nametype, anytype); INSTALL;
    op = xpost_operator_cons(ctx, ".namespacewritable", (Xpost_Op_Func)op_namespacewritable, 1, nametype); INSTALL;
    op = xpost_operator_cons(ctx, ".jobmemberwritable", (Xpost_Op_Func)op_jobmemberwritable, 1, nametype); INSTALL;
    op = xpost_operator_cons(ctx, ".hostsetting", (Xpost_Op_Func)op_hostsetting, 1, nametype); INSTALL;
    op = xpost_operator_cons(ctx, ".vmdeclaredcount", (Xpost_Op_Func)op_vmdeclaredcount, 0); INSTALL;
    op = xpost_operator_cons(ctx, ".vmblind", (Xpost_Op_Func)op_vmblind, 0); INSTALL;
    op = xpost_operator_cons(ctx, ".vmblindcount", (Xpost_Op_Func)op_vmblindcount, 0); INSTALL;
    op = xpost_operator_cons(ctx, ".vmseal", (Xpost_Op_Func)op_vmseal, 2, integertype, integertype); INSTALL;
    op = xpost_operator_cons(ctx, ".sysdictunlock", (Xpost_Op_Func)op_sysdictunlock, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".setprivatedict", (Xpost_Op_Func)op_setprivatedict, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".setjobstore", (Xpost_Op_Func)op_setjobstore, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".setgraphicsdict", (Xpost_Op_Func)op_setgraphicsdict, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".graphicsdictroot", (Xpost_Op_Func)op_graphicsdictroot, 0); INSTALL;
    op = xpost_operator_cons(ctx, ".jobstore", (Xpost_Op_Func)op_jobstore, 0); INSTALL;
    op = xpost_operator_cons(ctx, ".setglobalprivatedict", (Xpost_Op_Func)op_setglobalprivatedict, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".privatedict", (Xpost_Op_Func)op_privatedict, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".formlimits", (Xpost_Op_Func)op_formlimits, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".setmaxformcache",
                             (Xpost_Op_Func)op_setmaxformcache, 1, integertype);
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
    op = xpost_operator_cons(ctx, ".buildtime", (Xpost_Op_Func)buildtime, 0);
    INSTALL;
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
