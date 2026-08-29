/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_context.c
 * @brief An execution context: the stacks, the two banks, and what the collector roots from.
 *
 * A context is the unit a program runs in. It names an operand, execution
 * and dictionary stack, the two memory files it allocates in -- local and
 * global -- and the handful of objects the collector treats as roots.
 *
 * Contexts are numbered and the numbers are what the collector is given: it
 * is handed a memory file and has to find the contexts that run on it, so
 * the list of live identifiers lives in the file itself rather than in this
 * process's memory.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h> /* FILE* */
#include <stdlib.h> /* free */
#include <string.h> /* memset */

#ifdef _WIN32
# include <io.h> /* close */
#else
# include <unistd.h> /* close */
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_private.h" /* XPOST_REFUSAL_IMPOSSIBLE */
#include "xpost_compat.h" /* xpost_mkstemp */
#include "xpost_object.h"
#include "xpost_memory.h"
#include "xpost_stack.h"
#include "xpost_free.h"  //  initializes free list
#include "xpost_save.h"  // initializes save/restore stacks

#include "xpost_context.h"
#include "xpost_dict.h" /* read what the host settled */
#include "xpost_name.h" /* name a setting */
#include "xpost_file.h"
#include "xpost_font.h" /* the glyph cache the MaxFontItem parameter governs */
#include "xpost_handle.h"
#include "xpost_operator.h" /* releasing a wrapped operator's frame */

/* What this run settled under name, or a null where it settled nothing.
   The settings live in .hostdict, a member of the private global
   namespace this context roots -- the one dictionary that holds what the
   invocation decided rather than what the language is. A context whose
   namespace is not built yet has no settings to give, which reads as
   nothing settled. */
/* The machinery's writable state is gathered in one dictionary, the job
   store, so that the namespace its procedures and constants live in holds
   nothing a program could write (data/init.ps). Everything that reaches
   that state from C comes through here, so a member moved into or out of
   the store is one place to change rather than several to find. */
Xpost_Object xpost_context_job_store(Xpost_Context *ctx)
{
    return ctx->jobstore;
}

Xpost_Object xpost_context_job_member(Xpost_Context *ctx, const char *name)
{
    Xpost_Object store = xpost_context_job_store(ctx);

    if (xpost_object_get_type(store) != dicttype)
        return null;
    return xpost_dict_get(ctx, store, xpost_name_cons(ctx, name));
}

Xpost_Object xpost_context_host_setting(Xpost_Context *ctx, const char *name)
{
    Xpost_Object h;

    if (xpost_object_get_type(ctx->globalprivatedict) != dicttype)
        return null;
    h = xpost_context_job_member(ctx, ".hostdict");
    if (xpost_object_get_type(h) != dicttype)
        return null;
    return xpost_dict_get(ctx, h, xpost_name_cons(ctx, name));
}

/* initialize the context list
   special entity in the mfile */
int xpost_context_init_ctxlist(Xpost_Memory_File *mem)
{
    unsigned int ent;
    int ret;

    ret = xpost_memory_table_alloc_special(mem, MAXCONTEXT * sizeof(unsigned int), 0,
                                           XPOST_MEMORY_TABLE_SPECIAL_CONTEXT_LIST, &ent);
    if (!ret)
    {
        return 0; /* was unregistered error */
    }
    memset(xpost_vm_ptr(mem, xpost_memory_context_list_adr(mem)), 0,
           MAXCONTEXT * sizeof(unsigned int));

    return 1;
}

/* add a context ID to the context list in mfile */
int xpost_context_append_ctxlist(Xpost_Memory_File *mem,
                  unsigned int cid)
{
    int i;
    unsigned int *ctxlist;

    ctxlist = xpost_vm_ptr(mem, xpost_memory_context_list_adr(mem));
    // find first empty
    for (i=0; i < MAXCONTEXT; i++)
    {
        if (ctxlist[i] == 0)
        {
            ctxlist[i] = cid;
            return 1;
        }
    }
    return 0;
}

/* Take a context ID out of the context list in mfile.

   The list is what the collector reads to find the contexts a memory
   file serves, and it holds MAXCONTEXT entries. It has room for every
   context that exists at once and no more, so an entry belongs to a
   context for exactly as long as the context does: an entry left behind
   by one that has ended spends a place that a later one then cannot
   have, and spends it against a bound that counts contexts alive rather
   than contexts ever created.

   The entries after the one taken move down, so the list stays a run of
   identifiers followed by zeroes -- which is how every reader of it
   stops. */
void xpost_context_remove_ctxlist(Xpost_Memory_File *mem,
                                  unsigned int cid)
{
    int i, j;
    unsigned int *ctxlist;

    ctxlist = xpost_vm_ptr(mem, xpost_memory_context_list_adr(mem));
    for (i = 0; i < MAXCONTEXT; i++)
    {
        if (ctxlist[i] != cid)
            continue;
        for (j = i; j < MAXCONTEXT - 1; j++)
            ctxlist[j] = ctxlist[j + 1];
        ctxlist[MAXCONTEXT - 1] = 0;
        return;
    }
}

/* Unwind an execution stack to a depth, releasing the side state its
   frames hold outside virtual memory.

   A frame on that stack may name something the arena does not carry: a
   filenameforall enumeration holds the paths it matched, and a wrapped
   operator's frame holds the operands it was called with. Dropping the
   entry does not give either of them up, so an unwind that only popped
   would leave them behind -- once per unwind, for the life of the
   process. */
void xpost_context_unwind_exec(Xpost_Context *ctx, unsigned int base)
{
    while (xpost_stack_count(ctx->lo, ctx->es) > (int)base)
    {
        Xpost_Object x = xpost_stack_pop(ctx->lo, ctx->es);

        if (xpost_object_get_type(x) == globtype)
            xpost_context_glob_release(ctx, (unsigned int)x.glob_.id);

        if (xpost_object_get_type(x) == operatortype &&
            (x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapdone) ||
             x.mark_.padw == (unsigned int)XPOST_OP_CODE(ctx, wrapsealed)))
            xpost_operator_wrapped_release(ctx,
                    xpost_stack_pop(ctx->lo, ctx->es));
    }
}

/* End a context: it is no longer one, and holds nothing.

   A context's stacks and object roots are entities of the virtual memory
   it shares with the contexts it was forked from. Nothing else names
   them, so an ended context that kept them would keep everything it was
   holding when it ended alive for the rest of the run -- and, where the
   bank it named has since been wound back to an image that predates
   them, would name storage that is not there at all. Both are answered
   the same way: what it holds goes when it does.

   The identifier goes back too, so that the table slot and the place in
   the context list a later fork needs are given up together. */
void xpost_context_release(Xpost_Context *ctx)
{
    if (!ctx || ctx->state == C_FREE)
        return;
    ctx->state = C_FREE;
    if (ctx->lo)
        xpost_context_remove_ctxlist(ctx->lo, ctx->id);
    if (ctx->gl)
        xpost_context_remove_ctxlist(ctx->gl, ctx->id);
    ctx->os = ctx->es = ctx->ds = ctx->hold = 0;
#define XPOST_CONTEXT_DROP_ROOT(f) ctx->f = null;
    XPOST_CONTEXT_OBJECT_ROOTS(XPOST_CONTEXT_DROP_ROOT)
#undef XPOST_CONTEXT_DROP_ROOT
}


/* Build a stack and answer its address, or zero if the memory file
   would not give one. Zero is not a stack address: the memory table
   itself occupies the foot of every memory file, so a caller can tell a
   refusal from an address. */
static
unsigned int makestack(Xpost_Memory_File *mem)
{
    unsigned int adr;

    if (!xpost_stack_init(mem, &adr))
        return 0;
    return adr;
}

/* set up global vm in the context
 */
static
int initglobal(Xpost_Context *ctx,
               Xpost_Context *(*xpost_interpreter_cid_get_context)(unsigned int cid),
               int (*xpost_interpreter_get_initializing)(void),
               void (*xpost_interpreter_set_initializing)(int),
               Xpost_Memory_File *(*xpost_interpreter_alloc_global_memory)(void),
               int (*garbage_collect_function)(Xpost_Memory_File *mem, int dosweep, int markall))
{
    int ret;
    unsigned int safeadr;

    ctx->vmmode = GLOBAL;

    /* allocate and initialize global vm */
    ctx->gl = xpost_interpreter_alloc_global_memory();
    if (ctx->gl == NULL)
    {
        return 0;
    }

    /* anonymous mapping: nothing ever reads these pages back from disk */
    ret = xpost_memory_file_init(ctx->gl, NULL, -1, xpost_interpreter_cid_get_context,
            xpost_interpreter_get_initializing, xpost_interpreter_set_initializing);
    if (!ret)
    {
        return 0;
    }
    ret = xpost_memory_table_init(ctx->gl, XPOST_MEMORY_COLLECT_START_GLOBAL);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }
    /* safety buffer: nothing addresses it, but the allocation must
       succeed for the memory file to be in the state the rest of
       initialisation assumes */
    if (!xpost_memory_file_alloc(ctx->gl, 64, &safeadr))
    {
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }
    ret = xpost_free_init(ctx->gl);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }
    xpost_memory_register_garbage_collect_function(ctx->gl, garbage_collect_function);
    ret = xpost_save_init(ctx->gl);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }
    ret = xpost_context_init_ctxlist(ctx->gl);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }
    ret = xpost_context_append_ctxlist(ctx->gl, ctx->id);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->gl);
        return 0;
    }

            /* so OPTAB is not collected and not scanned. */
    ctx->gl->start = XPOST_MEMORY_COLLECT_START_GLOBAL;

    return 1;
}


/* set up local vm in the context
   allocates all stacks
 */
static
int initlocal(Xpost_Context *ctx,
              Xpost_Context *(*xpost_interpreter_cid_get_context)(unsigned int cid),
              int (*xpost_interpreter_get_initializing)(void),
              void (*xpost_interpreter_set_initializing)(int),
              Xpost_Memory_File *(*xpost_interpreter_alloc_local_memory)(void),
              int (*garbage_collect_function)(Xpost_Memory_File *mem, int dosweep, int markall))
{
    int ret;
    unsigned int safeadr;

    ctx->vmmode = LOCAL;

    /* allocate and initialize local vm */
    ctx->lo = xpost_interpreter_alloc_local_memory();
    if (ctx->lo == NULL)
    {
        return 0;
    }

    /* anonymous mapping: nothing ever reads these pages back from disk */
    ret = xpost_memory_file_init(ctx->lo, NULL, -1, xpost_interpreter_cid_get_context,
            xpost_interpreter_get_initializing, xpost_interpreter_set_initializing);
    if (!ret)
    {
        return 0;
    }

    ret = xpost_memory_table_init(ctx->lo, XPOST_MEMORY_COLLECT_START_LOCAL);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->lo);
        return 0;
    }
    /* safety buffer: nothing addresses it, but the allocation must
       succeed for the memory file to be in the state the rest of
       initialisation assumes */
    if (!xpost_memory_file_alloc(ctx->lo, 64, &safeadr))
    {
        xpost_memory_file_exit(ctx->lo);
        return 0;
    }
    ret = xpost_free_init(ctx->lo);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->lo);
        return 0;
    }
#ifndef XPOST_NO_GC
    xpost_memory_register_garbage_collect_function(ctx->lo, garbage_collect_function);
#endif
    ret = xpost_save_init(ctx->lo);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->lo);
        return 0;
    }
    ret = xpost_context_init_ctxlist(ctx->lo);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->lo);
        return 0;
    }
    ret = xpost_context_append_ctxlist(ctx->lo, ctx->id);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->lo);
        return 0;
    }

    ctx->os = makestack(ctx->lo);
    ctx->es = makestack(ctx->lo);
    ctx->ds = makestack(ctx->lo);
    ctx->hold = makestack(ctx->lo);
    if (!ctx->os || !ctx->es || !ctx->ds || !ctx->hold)
    {
        XPOST_LOG_ERR("cannot create the interpreter stacks");
        xpost_memory_file_exit(ctx->lo);
        return 0;
    }
    ctx->lo->start = XPOST_MEMORY_COLLECT_START_LOCAL;

    return 1;
}


/* initialize context
   allocates operator table
   allocates systemdict
   populates systemdict and optab with operators
 */
int xpost_context_init(Xpost_Context *ctx,
                       int (*xpost_interpreter_cid_init)(unsigned int *cid),
                       Xpost_Context *(*xpost_interpreter_cid_get_context)(unsigned int cid),
                       int (*xpost_interpreter_get_initializing)(void),
                       void (*xpost_interpreter_set_initializing)(int),
                       Xpost_Memory_File *(*xpost_interpreter_alloc_local_memory)(void),
                       Xpost_Memory_File *(*xpost_interpreter_alloc_global_memory)(void),
                       int (*garbage_collect_function)(Xpost_Memory_File *mem, int dosweep, int markall))
{
    int ret;

    ret = xpost_interpreter_cid_init(&ctx->id);
    if (!ret)
        return 0;
    ctx->state = C_IDLE;
    ctx->nest_depth = 0;
    ctx->callback_error = 0;

    ret = initlocal(ctx, xpost_interpreter_cid_get_context,
            xpost_interpreter_get_initializing, xpost_interpreter_set_initializing,
            xpost_interpreter_alloc_local_memory, garbage_collect_function);
    if (!ret)
    {
        return 0;
    }
    ret = initglobal(ctx, xpost_interpreter_cid_get_context,
            xpost_interpreter_get_initializing, xpost_interpreter_set_initializing,
            xpost_interpreter_alloc_global_memory, garbage_collect_function);
    if (!ret)
    {
        xpost_memory_file_exit(ctx->lo);
        return 0;
    }
    ctx->event_handler = null;
    ctx->namewrapsave = null;
    ctx->operator_install_refused = 0;
    ctx->ignoreinvalidaccess = 0;
    ctx->es_over = 0;
    ctx->os_over = 0;
    ctx->ds_over = 0;
    ctx->onerr_run = 0;
    ctx->skip_graphics = 0;
    ctx->batch = 0;
    ctx->privatedict = null;
    ctx->globalprivatedict = null;
    ctx->executingarray = null;
    ctx->arcstartproc = null;
    ctx->graphicsdict = null;
    ctx->pagedevice = null;
    ctx->pagedevice_destroy = null;
    ctx->pagedevice_depth = 0;
    ctx->job_snapshots = 1;
    /* the VMThreshold user parameter a context starts with, which is the
       count its banks are already paced by, so what currentuserparams
       reports before anybody sets it is what the run is doing */
    ctx->vmthreshold = XPOST_GARBAGE_COLLECTION_THRESHOLD;
    /* The MaxFontItem user parameter a context starts with, written through
       to the glyph cache so the store and this context begin agreeing. The
       write-through matters at the start of a second library lifetime: the
       store is a file-scope ceiling that the teardown has no reason to give
       back, so without it a lifetime would begin at whatever the last
       context left rather than where the first one started. */
    ctx->maxfontitem = (integer)xpost_font_cache_setlimit(
                            XPOST_FONT_ITEM_LIMIT_DEFAULT);
    ctx->idiomrecognition = 1;
    ctx->globs = NULL;
    ctx->globs_size = 0;
    ctx->job_baseline_lo = NULL;
    ctx->job_baseline_gl = NULL;
    ctx->job_baseline_optab = NULL;
    ctx->job_baseline_optab_len = 0;
    ctx->job_rand_next = 0;
    ctx->job_vmmode = LOCAL;
    ctx->job_packing = 0;
    ctx->job_baseline_ds = 0;
    ctx->job_boundary_failed = 0;
    ctx->job_encapsulated = 1;
    ctx->jobserver = 0;
    ctx->startjob_password[0] = '\0';
    ctx->xpost_interpreter_cid_init = xpost_interpreter_cid_init;
    ctx->xpost_interpreter_alloc_local_memory = xpost_interpreter_alloc_local_memory;
    ctx->xpost_interpreter_alloc_global_memory = xpost_interpreter_alloc_global_memory;
    ctx->garbage_collect_function = garbage_collect_function;

    return 1;
}

/* Release every stream the table still holds.

   A file is an entity in virtual memory and a struct outside it, and the
   memory file goes as a whole: the entities go with it and the structs,
   with the streams and coding state they hold, would be left behind. What
   a job closed for itself is already gone, and what collection reached is
   too; this is the rest, the files and filters a job was still holding
   when it ended.

   Standard input and output are among them. Their streams belong to the
   process rather than to this context and the close knows not to touch
   them, so what goes here is only the struct built around them. */
static void _release_files(Xpost_Memory_File *mem)
{
    unsigned int ent;

    if (!mem || !mem->base)
        return;
    for (ent = mem->start; ent < mem->table.nextent; ent++)
        if (mem->table.tab[ent].tag == filetype)
            xpost_file_release_entity(mem, ent);
}

/* The matched paths of a filenameforall live outside virtual memory, and
   the object the enumeration leaves on the execution stack names them by
   a number issued here. A number is one more than the slot holding the
   paths, so zero names nothing and an object carrying a number the
   enumeration has finished with resolves to nothing rather than to
   whatever the slot was given next.

   Enumerations nest and each holds its paths for as long as it runs, so
   the slots are as many as there are running at once, and a slot given
   back is the next one taken. */
int xpost_context_glob_hold(Xpost_Context *ctx, void *glob, unsigned int *id)
{
    unsigned int i;
    void **grown;

    for (i = 0; i < ctx->globs_size; i++)
    {
        if (!ctx->globs[i])
        {
            ctx->globs[i] = glob;
            *id = i + 1;
            return 1;
        }
    }
    grown = realloc(ctx->globs, (ctx->globs_size + 1) * sizeof(*grown));
    if (!grown)
        return 0;
    ctx->globs = grown;
    ctx->globs[ctx->globs_size] = glob;
    *id = ++ctx->globs_size;
    return 1;
}

void *xpost_context_glob_held(Xpost_Context *ctx, unsigned int id)
{
    if (id == 0 || id > ctx->globs_size)
        return NULL;
    return ctx->globs[id - 1];
}

void xpost_context_glob_release(Xpost_Context *ctx, unsigned int id)
{
    glob_t *globbuf;

    if (id == 0 || id > ctx->globs_size)
        return;
    globbuf = ctx->globs[id - 1];
    if (!globbuf)
        return;
    ctx->globs[id - 1] = NULL;
    xpost_glob_free(globbuf);
    free(globbuf);
}

/* Take a context apart, and with it the two memory files it names.

   Only the interpreter's own context reaches here. xpost_destroy declines
   any other by name, because a forked context shares both memory files
   with its parent and taking one apart under a live sibling would pull the
   arena out from under it. So the files are released exactly once, by the
   context that outlives every fork of it.

   A context that has ended keeps its number in the context list rather
   than being taken out of it. What reads that list is the collector,
   walking the contexts that share a memory file, and it passes over a slot
   whose state is C_FREE -- so a number naming a context that has ended
   costs the walk one comparison and can name nothing a sweep would take.
   Removing it would buy that comparison back at the price of closing a
   hole in a list every collection reads. */
void xpost_context_exit(Xpost_Context *ctx)
{
    unsigned int i;

    if (!ctx)
        return;

    _release_files(ctx->lo);
    _release_files(ctx->gl);

    /* the matched paths of any enumeration the context was still running:
       a host allocation the execution stack named, which goes with it */
    for (i = 0; i < ctx->globs_size; i++)
        xpost_context_glob_release(ctx, i + 1);
    free(ctx->globs);
    ctx->globs = NULL;
    ctx->globs_size = 0;

    /* the same for what a device held outside virtual memory: the
       entities carrying the handles go with the memory file, and the
       blocks they name would be left behind */
    xpost_handle_release_memory_file(ctx->lo);
    xpost_handle_release_memory_file(ctx->gl);

    free(ctx->namecache_gen);
    free(ctx->namecache_val);
    ctx->namecache_gen = NULL;
    ctx->namecache_val = NULL;
    ctx->namecache_size = 0;

    if (ctx->job_baseline_lo)
    {
        xpost_memory_image_free(ctx->job_baseline_lo);
        free(ctx->job_baseline_lo);
        ctx->job_baseline_lo = NULL;
    }
    free(ctx->job_baseline_optab);
    ctx->job_baseline_optab = NULL;
    ctx->job_baseline_optab_len = 0;
    if (ctx->job_baseline_gl)
    {
        xpost_memory_image_free(ctx->job_baseline_gl);
        free(ctx->job_baseline_gl);
        ctx->job_baseline_gl = NULL;
    }

    xpost_memory_file_exit(ctx->gl);
    xpost_memory_file_exit(ctx->lo);
}

/* return the appropriate global or local memory file for the composite object */
/*@dependent@*/
Xpost_Memory_File *xpost_context_select_memory(Xpost_Context *ctx,
                                               Xpost_Object o)
{
    return o.tag&XPOST_OBJECT_TAG_DATA_FLAG_BANK? ctx->gl : ctx->lo;
}


/* print a dump of the context struct */
void xpost_context_dump(Xpost_Context *ctx)
{
    xpost_memory_file_dump(ctx->gl);
    xpost_memory_table_dump(ctx->gl);
    xpost_memory_file_dump(ctx->lo);
    xpost_memory_table_dump(ctx->lo);
    /*dumpnames(ctx);*/
}

int xpost_context_install_event_handler(Xpost_Context *ctx,
                                        Xpost_Object operator,
                                        Xpost_Object device)
{
    ctx->event_handler = operator;
    ctx->window_device = device;
    return 1;
}

/*
   fork new process with shared global and shared local vm
   (lightweight process)

   The only fork, and it shares both memory files because a fork here can
   do nothing else. Fresh memory files would come up with their
   collection floor above the special entities -- the name stacks and
   trees, the operator table -- and this module builds none of those:
   they are built a layer up, along with systemdict and the dictionaries
   on the dict stack, by the interpreter that owns both. A context whose
   floor says those entities exist and whose table has never held them
   dispatches its first operator through an uninitialised row. Sharing
   the memory files of a context that is already finished is what leaves
   nothing to claim.
   */
unsigned int xpost_context_fork3(Xpost_Context *ctx,
                                 int (*xpost_interpreter_cid_init)(unsigned int *cid),
                                 Xpost_Context *(*xpost_interpreter_cid_get_context)(unsigned int cid),
                                 Xpost_Memory_File *(*xpost_interpreter_alloc_local_memory)(void),
                                 Xpost_Memory_File *(*xpost_interpreter_alloc_global_memory)(void),
                                 int (*garbage_collect_function)(Xpost_Memory_File *mem, int dosweep, int markall))
{
    unsigned int newcid;
    Xpost_Context *newctx;
    int ret;

    (void)xpost_interpreter_alloc_global_memory;
    (void)xpost_interpreter_alloc_local_memory;
    (void)garbage_collect_function;
    ret = xpost_interpreter_cid_init(&newcid);
    if (!ret) return 0;
    newctx = xpost_interpreter_cid_get_context(newcid);
    /* the slot may be one a finished context left behind: its
       name-resolution cache is C heap the struct copy below would orphan,
       so release it first. The cache is rebuilt lazily on first lookup,
       so a freed one costs the new context nothing, and a slot never used
       holds a null pointer here (the table is zeroed at startup). */
    free(newctx->namecache_gen);
    free(newctx->namecache_val);
    *newctx = *ctx; // struct copy for defaults
    newctx->id = newcid;
    newctx->state = C_IDLE;
    newctx->nest_depth = 0;
    newctx->callback_error = 0;
    /* the new context runs its own enumerations and holds their paths
       itself: the copy would have it share the table with this one, and
       a hold that grew the table would leave this one naming the table
       as it was */
    newctx->globs = NULL;
    newctx->globs_size = 0;
    /* the name-resolution cache is the new context's own, not a share of
       this one's. The struct copy above duplicated the pointers, which
       would have the two contexts write one another's entries -- and,
       worse, a grow in either reallocs the block the other still names,
       and each frees it at exit. It is rebuilt lazily on first lookup;
       the child's dictionary stack resolves the same names anyway, so
       nothing is lost by starting empty. */
    newctx->namecache_gen = NULL;
    newctx->namecache_val = NULL;
    newctx->namecache_size = 0;
    /* the job baseline is a snapshot the job server captures of one
       context's VM to restore that context between jobs; it belongs to
       the context that took it. A forked context has taken none, so the
       copied pointers above would have it name -- and, on capture,
       replace and free -- the parent's images. Start it with none. */
    newctx->job_baseline_lo = NULL;
    newctx->job_baseline_gl = NULL;
    newctx->job_baseline_optab = NULL;
    newctx->job_baseline_optab_len = 0;
    newctx->job_baseline_ds = 0;
    newctx->lo = ctx->lo;
    /* The list is what the collector walks to find the contexts a memory
       file serves, and it holds MAXCONTEXT entries. One entry is spent
       per context alive -- a context that ends takes its entry back out
       (xpost_context_release) -- so the list is full only when MAXCONTEXT
       contexts exist, and the cid allocation above, whose refusal is
       answered, is the same bound taken from the other side: it hands out
       a cid only for a context table slot that is free. A context that
       reached here has its slot, so the list has room for its entry. */
    XPOST_REFUSAL_IMPOSSIBLE(xpost_context_append_ctxlist(newctx->lo, newcid));
    newctx->gl = ctx->gl;
    XPOST_REFUSAL_IMPOSSIBLE(xpost_context_append_ctxlist(newctx->gl, newcid));

    newctx->os = makestack(newctx->lo);
    newctx->es = makestack(newctx->lo);
    newctx->ds = makestack(newctx->lo);
    newctx->hold = makestack(newctx->lo);
    if (!newctx->os || !newctx->es || !newctx->ds || !newctx->hold)
    {
        XPOST_LOG_ERR("cannot create the stacks for the new context");
        return 0;
    }
    newctx->lo->start = XPOST_MEMORY_COLLECT_START_LOCAL;

    /* Copy the whole dictionary stack, not just systemdict: fork gives the
       child the same name-lookup environment as the parent (PLRM 2nd ed 7.1
       forms the child by "copying the dictionary and graphics state
       stacks"). The dictionaries themselves stay in the shared VM -- only
       the stack of references is the child's own -- so a name the parent
       defined in userdict is found, and a def in the child reaches the same
       shared dictionary. */
    {
        int n = xpost_stack_count(ctx->lo, ctx->ds);
        int i;
        for (i = 0; i < n; i++)
            xpost_stack_push(newctx->lo, newctx->ds,
                    xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, i));
    }
    return newcid;
}


