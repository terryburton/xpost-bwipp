/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_save.c
 * @brief save and restore: winding virtual memory back to a mark.
 *
 * A save records where the arena stood; a restore puts back everything
 * written since, so what was allocated after the save does not survive it.
 * What is saved is the old contents of anything overwritten, not a copy of
 * the whole arena.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h>

#include "xpost_log.h"
#include "xpost_memory.h"  /* save/restore works with mtabs */
#include "xpost_object.h"  /* save/restore examines objects */
#include "xpost_stack.h"  /* save/restore manipulates (internal) stacks */
#include "xpost_free.h"  /* restore discards the backup copies it made */

#include "xpost_save.h"  /* double-check prototypes */

/*
typedef struct {
    word tag;
    word lev;
    unsigned stk;
} save_;

typedef struct {
    word tag;
    word pad;
    word src;
    word cpy;
} saverec_;
*/

/* create the master save stack, which is the entity in slot
   XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK */
int xpost_save_init(Xpost_Memory_File *mem)
{
    unsigned ent;
    int ret;

    /* The entity IS the stack's first segment, rather than a row holding
       the number of one. A segment is an entity, and this one's number
       is fixed before any constructor runs, so there is nothing left for
       a row of its own to say. */
    ret = xpost_memory_table_alloc_special(mem, sizeof(Xpost_Stack), 0,
                                           XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK,
                                           &ent);
    if (!ret)
    {
        return 0;
    }
    if (!xpost_stack_init_in(mem, ent))
    {
        XPOST_LOG_ERR("cannot create the save stack");
        return 0;
    }

    return 1;
}

/* stamp a fresh entity with the current save level in both save-level
   fields of its mark word: the composite was born at this depth, so
   restore's guards can tell it from one predating the save. Every
   composite constructor performs this immediately after allocation. */
void xpost_save_stamp_birth(Xpost_Memory_File *mem, unsigned int ent)
{
    unsigned int vs;
    unsigned int cnt;

    vs = xpost_memory_save_stack_ent(mem);
    cnt = xpost_stack_count(mem, vs);
    mem->table.tab[ent].mark =
          (cnt << XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET)
        | (cnt << XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_OFFSET);
}

/* push a new save object on the save stack
   this object is itself a stack (contains a stackadr) */
Xpost_Object xpost_save_create_snapshot_object(Xpost_Memory_File *mem)
{
    Xpost_Object v = { 0 };
    unsigned int vs;

    v.tag = savetype;
    vs = xpost_memory_save_stack_ent(mem);
    v.save_.lev = xpost_stack_count(mem, vs);
    if (mem->free_substack)
    {
        /* reuse the record stack a previous restore parked. It is a single
           segment (only those are pooled) and already unreferenced; empty
           it before handing it out. The pool is a chain through each parked
           stack's own prevseg, which for a parked stack otherwise names
           itself, so the last one in the chain is the one pointing at
           itself. */
        Xpost_Stack *s;
        v.save_.stk = mem->free_substack;
        s = xpost_stack_at(mem, v.save_.stk);
        mem->free_substack =
            (s->prevseg == v.save_.stk) ? 0 : s->prevseg;
        s->top = 0;
        s->prevseg = v.save_.stk;
    }
    else
    {
        /* the stack's entity is narrower than the save object's stk
           field on a wide-word build: land it whole */
        unsigned int stk;

        if (!xpost_stack_init(mem, &stk))
        {
            XPOST_LOG_ERR("cannot create the substack for a save level");
            return null;
        }
        v.save_.stk = stk;
    }
    xpost_stack_push(mem, vs, v);
    return v;
}

/* check ent's llev and tlev
   against current save level (save-stack count)
   returns 1 if ent is saved (or not necessary to save),
   returns 0 if ent needs to be saved before changing.
 */
unsigned xpost_save_ent_is_saved(Xpost_Memory_File *mem,
                                 unsigned ent)
{
    Xpost_Memory_Table *tab;
    unsigned int llev;
    unsigned int tlev;
    unsigned int vs;
    Xpost_Object sav;

    vs = xpost_memory_save_stack_ent(mem);

    if (xpost_stack_count(mem, vs) == 0)
        return 1;

    sav = xpost_stack_topdown_fetch(mem, vs, 0);
    tab = &mem->table;
    if (!xpost_ent_valid(mem, ent))
    {
        XPOST_LOG_ERR("cannot find table for ent %u", ent);
        return 0;
    }
    tlev = (tab->tab[ent].mark & XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_MASK)
        >> XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_OFFSET;
    llev = (tab->tab[ent].mark & XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK)
        >> XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET;

    /* An object must be backed up at the current save if it was born
       before that save established its level and has not been backed up
       here yet. create_snapshot_object records .lev as the stack count
       taken *before* the save pushes itself, so an object whose birth
       count llev equals sav.lev was created just before this save and
       still needs protecting: the test is llev <= sav.lev, not < (the
       missing boundary was the off-by-one that left a top-level
       save/restore from reverting anything). Backups stamp tlev with
       sav.lev + 1 (see save_save_ent) so the "already backed up" marker
       cannot be confused with an object's birth stamp, which equals its
       birth count and is therefore <= sav.lev for anything protectable. */
    return llev <= (unsigned int)sav.save_.lev ?
        (tlev == (unsigned int)sav.save_.lev + 1) : 1;
}

/* make a clone of ent, return new ent */
static
unsigned int _copy_ent(Xpost_Memory_File *mem,
                       unsigned ent)
{
    Xpost_Memory_Table *tab;
    unsigned new;
    unsigned int adr;
    unsigned int extent;
    int ret;

    tab = &mem->table;
    if (!xpost_ent_valid(mem, ent))
    {
        XPOST_LOG_ERR("cannot find table for ent %u", ent);
        return 0;
    }
    /* An entity's capacity and its extent are two numbers: the block it
       holds, and how much of that block its owner asked for. They come
       apart whenever the free list serves a request out of a roomier
       corpse. The backup takes the whole block, so that restore hands
       the object back a block as large as the one it had; but the
       extent it records is the object's own, because that is what the
       collector reads an allocation's contents by. A backup claiming
       the capacity would have the object claim it too once restore
       swapped the storage identity in, and the collector would then
       read the block's previous owner's leavings as objects. */
    extent = tab->tab[ent].used;
    if (!xpost_memory_table_alloc(mem, tab->tab[ent].sz, tab->tab[ent].tag, &new))
    {
        XPOST_LOG_ERR("cannot allocate entity to backup object");
        return 0;
    }
    if (new > XPOST_OBJECT_COMP_MAX_ENT)
    {
        XPOST_LOG_ERR("ent number %u exceeds object storage max %u",
                      new, XPOST_OBJECT_COMP_MAX_ENT);
        return 0;
    }
    tab = &mem->table; //recalc
    ret = xpost_memory_table_get_addr(mem, new, &adr);
    if (!ret)
    {
        XPOST_LOG_ERR("cannot find table for ent %u", ent);
        return 0;
    }
    memcpy(xpost_vm_ptr(mem, adr),
           xpost_ent_ptr(mem, ent),
           tab->tab[ent].sz);
    tab->tab[new].used = extent;

    XPOST_LOG_INFO("ent %u copied to ent %u in %s", ent, new, mem->fname);
    return new;
}

/* set tlev for ent to current save level
   push saverec relating ent to saved copy */
int xpost_save_save_ent(Xpost_Memory_File *mem,
                        unsigned tag,
                        unsigned pad,
                        unsigned ent)
{
    Xpost_Memory_Table *tab;
    Xpost_Object o = { 0 };
    unsigned tlev;
    Xpost_Object sav;
    unsigned int adr;
    unsigned int cpy;

    adr = xpost_memory_save_stack_ent(mem);
    sav = xpost_stack_topdown_fetch(mem, adr, 0);

    tab = &mem->table;
    if (!xpost_ent_valid(mem, ent))
    {
        XPOST_LOG_ERR("cannot find table for ent %u", ent);
        return 0;
    }
    tlev = sav.save_.lev + 1; /* +1 keeps this "backed up here" marker
                                 distinct from a birth stamp; see the note
                                 in xpost_save_ent_is_saved */
    tab->tab[ent].mark &= ~XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_MASK; // clear TLEV field
    tab->tab[ent].mark |= (tlev << XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_OFFSET);  // set TLEV field

    cpy = _copy_ent(mem, ent);
    if (cpy == 0)
    {
        XPOST_LOG_ERR("unable to make copy of ent %d", ent);
        return 0;
    }

    /* src and cpy may be wider than a word; their high bits ride in the
       tag (see XPOST_SAVEREC_TAG in xpost_save.h). pad keeps its meaning:
       an array's element count for the collector, zero otherwise. */
    o.saverec_.tag = XPOST_SAVEREC_TAG(tag, ent, cpy);
    o.saverec_.pad = pad;
    o.saverec_.src = (word)ent;
    o.saverec_.cpy = (word)cpy;
    xpost_stack_push(mem, sav.save_.stk, o);
    return 1;
}

/* for each saverec from current save stack
        exchange adrs between src and cpy
        pop saverec
    pop save stack */
void xpost_save_restore_snapshot(Xpost_Memory_File *mem)
{
    unsigned int v;
    Xpost_Object sav;
    Xpost_Memory_Table *tab = &mem->table;
    unsigned int cnt;
    unsigned int sent, cent;

    v = xpost_memory_save_stack_ent(mem); // the save stack
    sav = xpost_stack_pop(mem, v); // save-object (stack of saverec_'s)
    if (xpost_object_get_type(sav) == invalidtype)
        return;
    cnt = xpost_stack_count(mem, sav.save_.stk);
    XPOST_LOG_INFO("restoring %u save records", cnt);
    while (cnt--)
    {
        Xpost_Object rec;

        rec = xpost_stack_pop(mem, sav.save_.stk);
        if (xpost_object_get_type(rec) == invalidtype)
        {
            /* the record stack ended early; the VM is partially
               reverted */
            XPOST_LOG_ERR("save-record stack exhausted mid-restore: "
                          "%u records unreverted", cnt + 1);
            return;
        }
        sent = XPOST_SAVEREC_SRC(rec);
        cent = XPOST_SAVEREC_CPY(rec);
        XPOST_LOG_INFO("replacing ent %u with copy ent %u", sent, cent);
        if (!xpost_ent_valid(mem, sent))
        {
            XPOST_LOG_ERR("cannot find table for ent %u", sent);
            return;
        }
        if (!xpost_ent_valid(mem, cent))
        {
            XPOST_LOG_ERR("cannot find table for ent %u", cent);
            return;
        }
        /* the revert: the saved copy's storage identity becomes the
           object's (why the whole triple travels: see xpost_ent_swap) */
        xpost_ent_swap(mem, sent, cent);

        /* The copy has done its job. After the swap it holds the
           discarded post-save contents and the popped saverec was its
           only reference, so return its entity and storage to the free
           list -- the "explicit discarding by restore" the design calls
           for (doc/xpost_design.dox). Without it every composite modified
           under a save leaks an entity until the next collection, which
           the collector only runs on a byte/entity threshold; a job with
           enough save/restore traffic drives the entity counter up
           needlessly. Freeing here is safe: restore has no collection
           safe point, nothing else names cent, and the collector's sweep
           rebuilds the free list from the mark bits so a freed entity is
           never double-listed. */
        /* cent is the backup copy this restore has just finished
           reading, so it is an entity of this memory file and free to
           take back */
        XPOST_REFUSAL_IMPOSSIBLE(xpost_free_memory_ent(mem, cent));

        /* the object is back to its pre-save contents, so clear its
           "backed up here" marker (reset tlev to its birth level llev).
           Otherwise a later save that reuses this now-vacated level
           would see the stale marker and skip the backup, and its
           restore would not revert the object. */
        {
            unsigned int llv =
                (tab->tab[sent].mark & XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK)
                >> XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET;
            tab->tab[sent].mark &= ~XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_MASK;
            tab->tab[sent].mark |= (llv << XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_OFFSET);
        }
    }

    /* The record stack is now empty and, with its save object popped, wholly
       unreferenced, so one is parked for a later save to take rather than
       left for the collector to reclaim and a later save to build again.
       The collector is told where the pool is, since nothing else in the
       heap reaches what is on it.
       The pool holds as many as the program has had save levels open at
       once, chained through each parked stack's prevseg -- which a parked
       stack does not otherwise use, and whose value in the last of the
       chain is the stack's own address. Pooling one only would serve a
       program that nests no saves and lose one per level to every program
       that does, because the inner restore fills the single place before
       the outer restore reaches it.
       Only single-segment stacks are pooled -- the overwhelming common
       case, and it keeps reuse a plain top reset; a stack that grew across
       segments is left as it was. */
    {
        Xpost_Stack *sub = xpost_stack_at(mem, sav.save_.stk);
        if (sub->nextseg == 0)
        {
            sub->top = 0;
            sub->prevseg =
                mem->free_substack ? mem->free_substack : sav.save_.stk;
            mem->free_substack = sav.save_.stk;
        }
    }
}

