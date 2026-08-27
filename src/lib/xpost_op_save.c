/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_save.c
 * @brief Installs the save, restore and allocation-mode operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * save restore currentglobal setglobal gcheck startjob vmstatus
 *
 * A save is a mark in the arena and a restore winds back to it, so what is
 * allocated after one does not survive it -- which is why the allocation
 * mode, local or global, is settled here too.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h> /* strlen/memcmp for the startjob password */

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_save.h"
#include "xpost_file.h"
#include "xpost_font.h" /* the glyph cache MaxFontItem is written through to */
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_dict.h"
#include "xpost_dev_generic.h"
#include "xpost_garbage.h" /* the collector setting VMReclaim names */

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_save.h"

/* The name FontDirectory denotes the local font directory while the
   allocation mode is local and GlobalFontDirectory while it is global
   (PLRM), so a font defined in terms of another finds the directory its
   own fonts are going into. Rebinding it is a write to systemdict, which
   is read-only once the language has loaded; the write replaces an entry
   that is already there, so it allocates nothing and is safe on the error
   path, where setglobal is reached while an error is being reported. */
static
void _rebind_fontdirectory(Xpost_Context *ctx)
{
    Xpost_Object sd;
    Xpost_Object fd;
    Xpost_Object_Tag_Access access;
    int ignore;

    /* both are null until the boot file has defined them */
    if (xpost_object_get_type(ctx->globalfontdir) != dicttype)
        return;
    sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    if (xpost_object_get_type(sd) != dicttype)
        return;

    fd = (ctx->vmmode == GLOBAL) ? ctx->globalfontdir : ctx->localfontdir;
    ignore = ctx->ignoreinvalidaccess;
    access = xpost_object_get_access(ctx, sd);
    ctx->ignoreinvalidaccess = 1;
    /* Opening the window is a write to systemdict's value, so it backs
       systemdict up to the save level it stands under before it takes
       effect, and a level that ends here gives back the systemdict this
       found rather than the one this made -- a program that may write
       systemdict may redefine the language. Refused, the rebinding is
       abandoned rather than made unrevertable: what it rebinds is a
       convenience the PLRM describes and not something the interpreter's
       own correctness rests on. */
    if (xpost_object_get_type(
            xpost_object_set_access(ctx, sd,
                                    XPOST_OBJECT_TAG_ACCESS_UNLIMITED))
        == invalidtype)
    {
        ctx->ignoreinvalidaccess = ignore;
        XPOST_LOG_ERR("cannot open systemdict to rebind FontDirectory");
        return;
    }
    /* the name is already in systemdict, so the store replaces an entry
       rather than making one: it allocates nothing and cannot be
       refused, which is what makes this safe on the error path */
    XPOST_REFUSAL_IMPOSSIBLE(
        xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "FontDirectory"), fd));
    /* systemdict is backed up at this level now, so shutting the window
       writes its head and takes no further backup: it cannot be refused */
    xpost_object_set_access(ctx, sd, access);
    ctx->ignoreinvalidaccess = ignore;
}

/* -  save  save
   create save object representing vm contents */
static
int Zsave(Xpost_Context *ctx)
{
    unsigned int vs;

    /* each object's mark records the save level (as level+1) in an 8-bit
       field, so the save stack cannot exceed 255 levels without aliasing
       another level's bookkeeping */
    Xpost_Object v;

    vs = xpost_memory_save_stack_ent(ctx->lo);
    if (xpost_stack_count(ctx->lo, vs) >= 255)
        return limitcheck;
    v = xpost_save_create_snapshot_object(ctx->lo);
    /* the snapshot answers null when it could not be recorded, and a
       save object that records nothing would restore nothing */
    if (xpost_object_get_type(v) != savetype)
        return VMerror;
    /* remember the packing mode at this level so restore reverts it */
    if (v.save_.lev < sizeof ctx->packing_hist)
        ctx->packing_hist[v.save_.lev] = (unsigned char)ctx->packing;
    /* and the allocation mode, which restore reverts likewise */
    if (v.save_.lev < sizeof ctx->vmmode_hist)
        ctx->vmmode_hist[v.save_.lev] = (unsigned char)ctx->vmmode;
    /* and the user parameters, which restore reverts likewise. VMReclaim
       is recorded as the collector setting it names, that setting being
       the whole of the parameter. */
    if (v.save_.lev < sizeof ctx->autobanks_hist)
        ctx->autobanks_hist[v.save_.lev] =
            (unsigned char)xpost_garbage_auto_banks(ctx);
    if (v.save_.lev < sizeof ctx->vmthreshold_hist / sizeof ctx->vmthreshold_hist[0])
        ctx->vmthreshold_hist[v.save_.lev] = ctx->vmthreshold;
    if (v.save_.lev < sizeof ctx->idiomrecognition_hist)
        ctx->idiomrecognition_hist[v.save_.lev] =
            (unsigned char)ctx->idiomrecognition;
    if (v.save_.lev < sizeof ctx->maxfontitem_hist / sizeof ctx->maxfontitem_hist[0])
        ctx->maxfontitem_hist[v.save_.lev] = ctx->maxfontitem;
    if (!xpost_stack_push(ctx->lo, ctx->os, v))
        return stackoverflow;
    return 0;
}

/* save  restore  -
   rewind vm to saved state */
static
int Vrestore(Xpost_Context *ctx,
             Xpost_Object V)
{
    int z;
    unsigned int vs;
    ++ctx->namebind_gen; /* restored dicts may change bindings */

    vs = xpost_memory_save_stack_ent(ctx->lo);
    z = xpost_stack_count(ctx->lo, vs);
    /* the depth is counted, the level recorded: comparing them in the
       wider signed type keeps a depth that came back short of the level
       below it rather than above it */
    while(z > (integer)V.save_.lev)
    {
        xpost_save_restore_snapshot(ctx->lo);
        z--;
    }
    /* the packing mode is save/restore-subject: revert it to this level */
    if (V.save_.lev < sizeof ctx->packing_hist)
        ctx->packing = ctx->packing_hist[V.save_.lev];

    /* restore reverts the page device (PLRM 6.1): the snapshots above
       have just put the device the saved graphics state named back into
       it, and the device that was installed over it is displaced. Retire
       that one here, while it can still be reached -- what it holds is
       outside virtual memory, so nothing later in the run will, and the
       collector is free to take its dictionary from this point on. */
    xpost_device_retire_restored(ctx, (unsigned int)V.save_.lev);

    /* restore closes a file created since the corresponding save (PLRM
       3.8.2): sweep the local table for file entities born above the
       restored depth and close them. A file still referenced from a
       stack stays open: the spec answers that situation with
       invalidrestore, which is not implemented -- see the note in
       tests/save_restore_test.ps -- and closing under a live reference
       would be worse.

       The close is all this does. It does not reclaim the entity, and
       the scan below is why: it sees the objects lying on the stacks,
       and a file named from inside an array or a dictionary is named
       just as surely without appearing there. Reclaiming an entity on
       that evidence hands its number back to the free list while an
       object still holds it, and the object then names the list's own
       link word, or the next file the program opens. The payload of a
       file entity is a pointer this interpreter calls through, so the
       first is a jump to an address the free list wrote and the second
       is one file's operations landing on another.

       Which entities nothing reaches is the collector's question, and
       the collector descends into composites to answer it. A file
       closed here is left for that sweep to reclaim: its stream is
       gone, so what remains is one pointer's worth of table row.

       Each stack is read a segment at a time, in one pass from its
       root. A stack is a chain of segments and an index into one is
       reached by walking that chain, so asking for index 0, then index
       1, and so on to the top would walk the chain again for every
       element and cost the stack's depth once per element. Nothing
       allocates inside the walk, so the segment pointer stays good
       across it. */
    if (ctx->lo->file_birth_max > (unsigned int)V.save_.lev + 1)
    {
        unsigned int ent, stamp;
        unsigned int stacks[4];
        int k;

        stacks[0] = ctx->os; stacks[1] = ctx->es;
        stacks[2] = ctx->ds; stacks[3] = ctx->hold;
        for (ent = ctx->lo->start; ent < ctx->lo->table.nextent; ent++)
        {
            if (ctx->lo->table.tab[ent].tag != filetype)
                continue;
            stamp = (ctx->lo->table.tab[ent].mark
                     & XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK)
                    >> XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET;
            if (stamp == 0 || stamp <= (unsigned int)V.save_.lev + 1)
                continue;
            for (k = 0; k < 4; k++)
            {
                Xpost_Stack *s;

                for (s = xpost_stack_at(ctx->lo, stacks[k]); s;
                     s = xpost_stack_next_segment(ctx->lo, s))
                {
                    unsigned int i;

                    for (i = 0; i < s->top; i++)
                    {
                        Xpost_Object o = s->data[i];

                        if (xpost_object_get_type(o) == filetype
                         && (unsigned int)o.mark_.padw == ent)
                            goto keep;
                    }
                }
            }
            {
                /* the vtable close releases the stream and clears the
                   entity's stored pointer; a reusable stream survives
                   its close rewound, keeps its pointer, and then keeps
                   its entity too */
                Xpost_Object o = { 0 };

                o.mark_.tag = filetype;
                o.mark_.pad0 = 0;
                o.mark_.padw = ent;
                /* restore is not a place a stream can refuse to close:
                   the file object is going away with the save level
                   whatever the close had left to write (PLRM 3.7.2) */
                (void)xpost_file_object_close(ctx->lo, o);
            }
        keep:;
        }
    }

    /* The allocation mode is save/restore-subject too (PLRM 8.2 restore),
       and the name FontDirectory denotes whichever directory the mode
       calls for, so reverting the one rebinds the other exactly as
       setglobal does.

       Both come last, after the rewind and after the teardown the rewind
       sets off: the device retirement runs a device's own release method
       and the sweep above closes files, and each of them works in the
       mode the level being discarded was running under. What the write
       to systemdict here is backed up against is virtual memory already
       rewound. */
    if (V.save_.lev < sizeof ctx->vmmode_hist
        && ctx->vmmode_hist[V.save_.lev] != (unsigned char)ctx->vmmode)
    {
        ctx->vmmode = ctx->vmmode_hist[V.save_.lev];
        _rebind_fontdirectory(ctx);
    }

    /* The user interpreter parameters are named in the same sentence
       (PLRM 8.2 restore, PLRM C.1.1), and each of them is a number a
       program reads back and a way the interpreter then behaves, so
       giving the number back means putting the behaviour back with it.
       VMReclaim is both at once: what a program reads is the setting
       that says which banks a collection running of its own accord
       reclaims, so putting that setting back is the whole of reverting
       it, and the two cannot come apart. VMThreshold is a count this
       interpreter records and reports and nothing else reads, so
       reverting the count is all there is to revert.

       These come last with the allocation mode, and for the same
       reason: the teardown above allocates, and both it and the
       collector it may set off belong to the level being discarded. */
    if (V.save_.lev < sizeof ctx->autobanks_hist)
        xpost_garbage_auto_banks_set(ctx, ctx->autobanks_hist[V.save_.lev]);
    if (V.save_.lev < sizeof ctx->vmthreshold_hist / sizeof ctx->vmthreshold_hist[0])
        ctx->vmthreshold = ctx->vmthreshold_hist[V.save_.lev];
    if (V.save_.lev < sizeof ctx->idiomrecognition_hist)
        ctx->idiomrecognition = ctx->idiomrecognition_hist[V.save_.lev];
    /* MaxFontItem is named by the same sentence and is the one parameter
       whose value something outside the context goes by: the glyph cache
       admits or refuses an entry by it. So giving the number back means
       writing it through to the store as well, or a program would read the
       reverted ceiling back while the cache went on using the one the
       discarded level asked for (PLRM 8.2 setcachelimit). */
    if (V.save_.lev < sizeof ctx->maxfontitem_hist / sizeof ctx->maxfontitem_hist[0])
    {
        ctx->maxfontitem = ctx->maxfontitem_hist[V.save_.lev];
        (void) xpost_font_cache_setlimit(ctx->maxfontitem);
    }

    return 0;
}

/* bool  setglobal  -
   set vm allocation mode in current context. true is global. */
static
int Bsetglobal(Xpost_Context *ctx,
               Xpost_Object B)
{
    unsigned int mode = B.int_.val? GLOBAL: LOCAL;

    if (mode == ctx->vmmode)
        return 0;
    ctx->vmmode = mode;
    _rebind_fontdirectory(ctx);
    return 0;
}

/* -  currentglobal  bool
   return vm allocation mode for current context */
static
int Zcurrentglobal(Xpost_Context *ctx)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(ctx->vmmode==GLOBAL));
    return 0;
}

/* any  gcheck  bool
   check whether value is a legal element of a global composite
   object: simple objects always are; composite objects are when
   their value lives in global VM */
static
int Agcheck(Xpost_Context *ctx,
            Xpost_Object A)
{
    Xpost_Object r;
    switch(xpost_object_get_type(A))
    {
        default:
            r = xpost_bool_cons(1); break;
        case stringtype:
        case dicttype:
        case arraytype:
        case filetype:
            r = xpost_bool_cons((A.tag&XPOST_OBJECT_TAG_DATA_FLAG_BANK)!=0);
    }
    xpost_stack_push(ctx->lo, ctx->os, r);
    return 0;
}

/* PLRM C.3.1: the password matches when the StartJobPassword is empty (the
   factory default, so a trusted prolog works out of the box) or equals the
   presented one, byte for byte, case-sensitively. An integer password is
   compared as its decimal string, as if by cvs. */
static
int _startjob_password_ok(Xpost_Context *ctx, Xpost_Object P)
{
    const char *pw = ctx->startjob_password;
    size_t pwlen = strlen(pw);

    if (pwlen == 0)
        return 1;
    switch (xpost_object_get_type(P))
    {
        case stringtype:
        {
            char *s = xpost_string_get_pointer(ctx, P);
            return (size_t)P.comp_.sz == pwlen && memcmp(s, pw, pwlen) == 0;
        }
        case integertype:
        {
            char buf[32];
            int n = snprintf(buf, sizeof buf, "%d", (int)P.int_.val);
            return n > 0 && (size_t)n == pwlen && memcmp(buf, pw, pwlen) == 0;
        }
        default:
            return 0;
    }
}

/* bool1 password  startjob  bool2
   (PLRM 3.7.7) Conditionally end the current job and start a new one. It
   succeeds only when the password is correct and the current save nesting
   is no deeper than the level the job started at -- which, with the job
   boundary being a revert to a fixed VM image rather than a save level, is
   the local save stack standing empty. Bracketing startjob in a program's
   own save/restore therefore neutralises it, as the spec requires.

   On success it resets the operand and dictionary stacks (the latter to the
   depth the baseline was captured at, which drops a serverdict an exitserver
   left there) and sets whether the run persists: bool1 true leaves the run
   UNENCAPSULATED, so its boundary folds its state into the baseline and its
   definitions outlive it; bool1 false returns the run to ENCAPSULATED,
   folding first the work an earlier unencapsulated stretch did so the
   encapsulated job reverts to that rather than losing it. It pushes true.

   The revert or fold itself is not done here: it is the run's own boundary,
   in C after the run, so this cannot double-revert and stays infallible. A
   job transition thus takes effect at the end of the run, not the instant
   startjob runs -- see the note in the report on the mid-run limitation.
   On failure startjob pushes false and does nothing else. */
static
int Bstartjob(Xpost_Context *ctx,
              Xpost_Object B,
              Xpost_Object P)
{
    unsigned int vs = xpost_memory_save_stack_ent(ctx->lo);
    unsigned int floor;

    if (!_startjob_password_ok(ctx, P)
        || xpost_stack_count(ctx->lo, vs) != 0)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }

    /* reset the dictionary stack to the baseline depth (never below the
       three permanent dictionaries, which this pops past `end`'s guard) */
    floor = ctx->job_baseline_ds < 3 ? 3u : ctx->job_baseline_ds;
    while ((unsigned int)xpost_stack_count(ctx->lo, ctx->ds) > floor)
        (void) xpost_stack_pop(ctx->lo, ctx->ds);

    xpost_stack_clear(ctx->lo, ctx->os);
    xpost_stack_clear(ctx->lo, ctx->hold);
    /* true leaves the run unencapsulated (its boundary folds its state into
       the baseline, so its definitions persist); false returns it to
       encapsulated (its boundary reverts). The revert or fold is the run's
       own boundary, at its end -- not here: folding mid-run would capture
       the execution stack in mid-flight, and a later revert to that image
       would resume a stale continuation. One consequence is a documented
       deviation: a `true ... startjob` prolog followed by a `false ...
       startjob` in the SAME run does not persist the prolog, because the
       run ends encapsulated and reverts as a whole. Loading a prolog that
       must persist is done the way the worker does it -- with the run left
       unencapsulated to its end, or through xpost_job_baseline_set. */
    ctx->job_encapsulated = B.int_.val ? 0 : 1;
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
}

#if 0
/* -  vmstatus  level used max
   return size information for (local) vm */
static
int Zvmstatus(Xpost_Context *ctx)
{
    unsigned int vs;

    vs = xpost_memory_save_stack_ent(ctx->lo);
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(xpost_stack_count(ctx->lo, vs)));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ctx->lo->used));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ctx->lo->max));
    return 0;
}
#endif

int xpost_oper_init_save_ops(Xpost_Context *ctx,
                             Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "save", (Xpost_Op_Func)Zsave, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "restore", (Xpost_Op_Func)Vrestore, 1, savetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "setglobal", (Xpost_Op_Func)Bsetglobal, 1, booleantype);
    INSTALL;
    op = xpost_operator_cons(ctx, "currentglobal", (Xpost_Op_Func)Zcurrentglobal, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "gcheck", (Xpost_Op_Func)Agcheck, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "startjob", (Xpost_Op_Func)Bstartjob, 2, booleantype, anytype);
    INSTALL;
#if 0
    op = xpost_operator_cons(ctx, "vmstatus", (Xpost_Op_Func)Zvmstatus, 0);
    INSTALL;
#endif

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
    xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "mark"), mark); */

    return 0;
}
