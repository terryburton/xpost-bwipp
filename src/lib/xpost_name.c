/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"  // name structures live in mfiles
#include "xpost_object.h"  // names are objects, with associated hidden string objects
#include "xpost_stack.h"  // name strings live on a stack

#include "xpost_context.h"
//#include "xpost_interpreter.h"  // initialize interpreter to test
#include "xpost_error.h"
#include "xpost_free.h"  // give the old node table storage back
#include "xpost_string.h"  // access string objects
#include "xpost_name.h"  // double-check prototypes

#define CNT_STR(s) sizeof(s)-1, s

/* A name is a counted byte sequence: each tree level keys one byte
   (0..255), and a level keyed by the out-of-band terminator 256 ends
   the name and holds the payload, so a nul byte is an ordinary name
   character (PLRM: cvn yields a name lexically the same as the
   string). */
#define TST_END 256u

/* The nodes of the tree live together in one entity and name each other
   by node number rather than by where that entity sits.
 *
 * A node held its own allocation before, taken straight off the memory
 * file with no table row to describe it, and lo/eq/hi were addresses into
 * the arena. Nothing could find such a node: not its size, not its tag,
 * and not whichever other node pointed at it. Names go on being interned
 * for as long as a program runs, so those nodes were scattered the whole
 * height of the arena, and a pass that rearranged it would have relocated
 * a live entity onto one of them.
 *
 * Gathered here the nodes are one entity with one row, and are numbered
 * within it the way virtual memory numbers entities within the memory
 * table. A number survives the storage moving, so lo, eq and hi are no
 * longer places; the table's row is, and it is the single thing a
 * rearrangement rewrites. The root moves as the tree is built and so
 * lives in the table's head rather than in the row, which now means what
 * it means everywhere else.
 *
 * Node 0 is not a node. Every link spells "no subtree" as zero and the
 * first node is 1, so the empty tree needs no separate spelling.
 *
 * Nodes are handed out by counting and are never given back. A name that
 * has been interned stays interned for the life of the bank, so there is
 * nothing to return and no free list to keep -- which is why this is not
 * what xpost_save.c calls a pool, where a parked record stack is chained
 * for a later save to take. Growing is the whole table moving at once.
 */
typedef struct
{
    unsigned int root; /**< the node the tree is entered at, 0 while empty */
    unsigned int n;    /**< nodes handed out, which is the highest number */
    unsigned int cap;  /**< nodes the storage holds */
    unsigned int nnames; /**< names on this bank's name stack, so the next
                              name's index is read here rather than counted
                              off the stack on every intern */
} tsttab;

/* Nodes in the first allocation. A boot interns thousands, so this is a
   starting point to double from rather than an estimate of the total. */
#define TSTTAB_FIRST 64u

/* How many times a string has been offered to the name mechanism and had
   to be looked up. A name already interned still costs a walk of the
   tree -- two, when it is a global name, since the local bank is
   searched first and misses -- so this is the measure of whether a
   caller is resolving a name once or resolving it again for every unit
   of work it does. It saturates rather than wrapping: a count that
   restarted would read as a caller that had stopped looking anything
   up. */
static unsigned int _name_lookups;

unsigned int xpost_name_lookups(void)
{
    return _name_lookups;
}

static void _name_lookup_charge(void)
{
    if (_name_lookups != (unsigned int)-1)
        _name_lookups++;
}

/* The table's head, and a node in it. Both are derived afresh at every use: an
   allocation may grow the memory file, which moves it, so a pointer taken
   before one is stale after it. */
static tsttab *_tsttab(Xpost_Memory_File *mem)
{
    return xpost_vm_ptr(mem, xpost_memory_name_tree_adr(mem));
}

static tst *_tstnode(Xpost_Memory_File *mem, unsigned int i)
{
    return (tst *)(void *)(_tsttab(mem) + 1) + (i - 1);
}

/* Give the node table storage for `want` nodes, keeping what it holds.
 *
 * The table is a special entity and its number is fixed, so the storage is
 * moved into that row rather than the row being replaced: a fresh entity
 * is allocated, the contents copied across, the two rows exchange what
 * they describe, and the temporary -- now holding the old storage -- is
 * given back. */
static int _tsttab_grow(Xpost_Memory_File *mem, unsigned int want)
{
    Xpost_Memory_Table *tab;
    unsigned int tmp, oldadr, oldsz, newadr, newsz;

    if (want > (0xffffffffu - sizeof(tsttab)) / sizeof(tst))
    {
        XPOST_LOG_ERR("%d name tree too large to address", VMerror);
        return 0;
    }
    if (!xpost_memory_table_alloc(mem,
            (unsigned int)(sizeof(tsttab) + want * sizeof(tst)), 0, &tmp))
    {
        XPOST_LOG_ERR("%d cannot grow the name tree", VMerror);
        return 0;
    }

    tab = &mem->table;                          /* recalc: the file may have grown */
    oldadr = xpost_memory_name_tree_adr(mem);
    oldsz = xpost_memory_name_tree_size(mem);
    newadr = tab->tab[tmp].adr;
    /* what the row says, which the free list may have made larger than
       the request; taking the row's figure leaves none of it unused and
       keeps the accounting the list was given */
    newsz = tab->tab[tmp].sz;

    if (oldsz)
        memcpy(xpost_vm_ptr(mem, newadr), xpost_vm_ptr(mem, oldadr), oldsz);

    xpost_memory_set_name_tree(mem, newadr, newsz);
    tab->tab[tmp].adr = oldadr;
    tab->tab[tmp].sz = oldsz;
    /* The entity was allocated moments ago in this same function and
       carries no tag, so the free list has nothing to object to; a
       refusal here would be the list refusing what it just handed out. */
    XPOST_REFUSAL_IMPOSSIBLE(xpost_free_memory_ent(mem, tmp));

    _tsttab(mem)->cap =
        (unsigned int)((newsz - sizeof(tsttab)) / sizeof(tst));
    /* A fresh table starts its name count from the stack as it stands --
       the stack carries a reserved slot before any name -- so the index
       addname reads from it is the one the stack would give. A grow
       carried the count across in the copy above. */
    if (!oldsz)
        _tsttab(mem)->nnames =
            xpost_stack_count(mem, xpost_memory_name_stack_ent(mem));
    return 1;
}

/* the index of a fresh node, zeroed, or 0 if none can be had */
static unsigned int _tsttab_new(Xpost_Memory_File *mem)
{
    tsttab *pl;
    tst *p;
    unsigned int i;

    /* The table is built when the first name is interned rather than when
       the tree's entity is made, so a bank that interns nothing carries
       no storage for nodes it never has. */
    if (xpost_memory_name_tree_size(mem) < sizeof(tsttab))
    {
        if (!_tsttab_grow(mem, TSTTAB_FIRST))
            return 0;
    }
    pl = _tsttab(mem);
    if (pl->n >= pl->cap)
    {
        if (!_tsttab_grow(mem, pl->cap * 2))
            return 0;
        pl = _tsttab(mem);
    }
    i = ++pl->n;
    p = _tstnode(mem, i);
    p->val = 0; p->lo = p->eq = p->hi = 0;
    return i;
}

/* the node the tree is entered at, and recording a new one */
static unsigned int _tsttab_root(Xpost_Memory_File *mem)
{
    /* no table yet is no names yet, which is the empty tree */
    if (xpost_memory_name_tree_size(mem) < sizeof(tsttab))
        return 0;
    return _tsttab(mem)->root;
}

static void _tsttab_set_root(Xpost_Memory_File *mem, unsigned int r)
{
    _tsttab(mem)->root = r;
}

/* The name no program can spell, made in the slot reserved for it.
 *
 * It is the one special not built by the entity allocator: it is a
 * string, and the string constructor takes whichever slot is next. The
 * slots are reserved before any constructor runs, so the next one is
 * never the one this belongs in -- the storage is exchanged into the
 * reserved row. What the borrowed row is left holding is the reserved
 * row's own emptiness: no storage, and a tag saying nothing is built
 * there. One row per bank is spent this way, once, during
 * initialisation. */
static Xpost_Object _bogus_name(Xpost_Context *ctx, Xpost_Memory_File *mem,
                                unsigned int want)
{
    Xpost_Object str;
    unsigned int borrowed;

    str = xpost_string_cons(ctx, CNT_STR("_not_a_name_"));
    if (xpost_object_get_type(str) == nulltype)
        return str;
    borrowed = xpost_object_get_ent(str);
    if (borrowed == want)
        return str;

    xpost_ent_swap(mem, borrowed, want);
    mem->table.tab[want].tag = mem->table.tab[borrowed].tag;
    mem->table.tab[borrowed].tag = 0;
    return xpost_object_set_ent(str, want);
}

/* initialize the name special entities XPOST_MEMORY_TABLE_SPECIAL_NAME_STACK, NAME_TREE */
int xpost_name_init(Xpost_Context *ctx)
{
    unsigned int ent;
    unsigned int mode;
    unsigned int nstk;
    int ret;

    mode = ctx->vmmode;
    ctx->vmmode = GLOBAL;
    /* the entity is the name stack's first segment, for the reason
       given where the save stack's is made */
    ret = xpost_memory_table_alloc_special(ctx->gl, sizeof(Xpost_Stack), 0,
                                           XPOST_MEMORY_TABLE_SPECIAL_NAME_STACK,
                                           &ent);
    if (!ret)
    {
        return 0;
    }
    ret = xpost_memory_table_alloc_special(ctx->gl, 0, 0,
                                           XPOST_MEMORY_TABLE_SPECIAL_NAME_TREE,
                                           &ent);
    if (!ret)
    {
        return 0;
    }

    if (!xpost_stack_init_in(ctx->gl,
                             XPOST_MEMORY_TABLE_SPECIAL_NAME_STACK))
    {
        XPOST_LOG_ERR("cannot create the name stack");
        return 0;
    }
    xpost_memory_set_name_tree(ctx->gl, 0, 0);
    nstk = xpost_memory_name_stack_ent(ctx->gl);
    xpost_stack_push(ctx->gl, nstk, _bogus_name(ctx, ctx->gl,
                    XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME));
    /* The name no program can spell takes the slot after the tree's. It is
       not allocated through the table allocator -- it is the first string
       pushed on the name stack -- so it is held to its slot here, for the
       reason given where that allocator's own check is defined. */
    if (xpost_object_get_ent(xpost_stack_topdown_fetch(ctx->gl, nstk, 0))
        != XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME)
    {
        XPOST_LOG_ERR("%d the bogus name is not in its special position", VMerror);
        return 0;
    }

    ctx->vmmode = LOCAL;
    /* the entity is the name stack's first segment, for the reason
       given where the save stack's is made */
    ret = xpost_memory_table_alloc_special(ctx->lo, sizeof(Xpost_Stack), 0,
                                           XPOST_MEMORY_TABLE_SPECIAL_NAME_STACK,
                                           &ent);
    if (!ret)
    {
        return 0;
    }
    ret = xpost_memory_table_alloc_special(ctx->lo, 0, 0,
                                           XPOST_MEMORY_TABLE_SPECIAL_NAME_TREE,
                                           &ent);
    if (!ret)
    {
        return 0;
    }

    if (!xpost_stack_init_in(ctx->lo,
                             XPOST_MEMORY_TABLE_SPECIAL_NAME_STACK))
    {
        XPOST_LOG_ERR("cannot create the name stack");
        return 0;
    }
    xpost_memory_set_name_tree(ctx->lo, 0, 0);
    nstk = xpost_memory_name_stack_ent(ctx->lo);
    xpost_stack_push(ctx->lo, nstk, _bogus_name(ctx, ctx->lo,
                    XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME));
    if (xpost_object_get_ent(xpost_stack_topdown_fetch(ctx->lo, nstk, 0))
        != XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME)
    {
        XPOST_LOG_ERR("%d the bogus name is not in its special position", VMerror);
        return 0;
    }

    ctx->vmmode = mode;

    return 1;
}

/* perform a search using the ternary search tree */
static
unsigned int tstsearch(Xpost_Memory_File *mem,
                       unsigned int ti,
                       const char *s,
                       unsigned int n)
{
    while (ti) {
        tst *p = _tstnode(mem, ti);
        unsigned int key = n ? (unsigned char)*s : TST_END;

        if (key < p->val) {
            ti = p->lo;
        } else if (key == p->val) {
            if (key == TST_END) return p->eq; /* payload at the terminator */
            s++, n--;
            ti = p->eq;
        } else {
            ti = p->hi;
        }
    }
    return 0;
}

/* add a counted string to the ternary search tree

   Walking the tree with a loop rather than by recursion. A name descends
   the equal-child chain one character per step, so a recursive insert
   would call itself as deep as the name is long, and a long enough name
   -- one a program can build with a string and cvn, or write as a single
   token -- would run the C stack out. The loop keeps the stack it uses
   flat whatever the name, the way the sibling lookup tstsearch already
   does. Node numbers survive the storage moving under a growth, so the
   walk carries the current node as a number and re-reads it after any
   allocation rather than holding a pointer across one. */
static
int tstinsert(Xpost_Memory_File *mem,
              unsigned int ti,
              const char *s,
              unsigned int n,
              unsigned int *retval)
{
    unsigned int cur;
    unsigned int key = n ? (unsigned char)*s : TST_END;

    if (!ti) {
        ti = _tsttab_new(mem);
        if (!ti)
        {
            XPOST_LOG_ERR("cannot allocate tree node");
            return VMerror;
        }
        _tstnode(mem, ti)->val = key;
    }

    cur = ti;
    for (;;) {
        tst *p = _tstnode(mem, cur);
        unsigned int val = p->val;
        unsigned int child;

        key = n ? (unsigned char)*s : TST_END;

        if (key < val) {
            child = p->lo;
            if (!child) {
                child = _tsttab_new(mem);
                if (!child)
                {
                    XPOST_LOG_ERR("cannot allocate tree node");
                    return VMerror;
                }
                _tstnode(mem, child)->val = key;
                _tstnode(mem, cur)->lo = child; //recalc pointer after alloc
            }
            cur = child;
        } else if (key == val) {
            if (key == TST_END) {
                /* payload at the terminator: the index this name will
                   take, which is the count of names on the stack -- read
                   from the tree header in constant time, as addname does
                   when it pushes the name there, rather than counted off
                   the stack (an O(n) walk that made interning n names
                   O(n^2)). The name is not pushed until addname runs, so
                   the count here is the index it gets. */
                _tstnode(mem, cur)->eq = _tsttab(mem)->nnames;
                break;
            }
            s++, n--;
            child = p->eq;
            if (!child) {
                unsigned int nkey = n ? (unsigned char)*s : TST_END;
                child = _tsttab_new(mem);
                if (!child)
                {
                    XPOST_LOG_ERR("cannot allocate tree node");
                    return VMerror;
                }
                _tstnode(mem, child)->val = nkey;
                _tstnode(mem, cur)->eq = child; //recalc pointer after alloc
            }
            cur = child;
        } else {
            child = p->hi;
            if (!child) {
                child = _tsttab_new(mem);
                if (!child)
                {
                    XPOST_LOG_ERR("cannot allocate tree node");
                    return VMerror;
                }
                _tstnode(mem, child)->val = key;
                _tstnode(mem, cur)->hi = child; //recalc pointer after alloc
            }
            cur = child;
        }
    }

    *retval = ti;
    return 0;
}

/* add the name to the name stack, return index */
static
unsigned int addname(Xpost_Context *ctx,
                     const char *s,
                     unsigned int n)
{
    Xpost_Memory_File *mem = ctx->vmmode==GLOBAL?ctx->gl:ctx->lo;
    unsigned int names;
    unsigned int u;
    Xpost_Object str;

    names = xpost_memory_name_stack_ent(mem);
    /* The name being interned takes the index that is the count of names
       already on the bank's stack. That count is carried in the tree
       header and read here in constant time: recomputing it by walking
       the stack, which only grows, made interning N distinct names cost
       O(N^2) -- a program of many names spent it in the scanner. The
       header is fetched fresh after each allocation below, since a grow
       may move virtual memory. */
    u = _tsttab(mem)->nnames;

    str = xpost_string_cons(ctx, n, s);
    if (xpost_object_get_type(str) == nulltype)
    {
        XPOST_LOG_ERR("cannot allocate name string");
        return 0;
    }
    xpost_stack_push(mem, names, str);
    _tsttab(mem)->nnames = u + 1;
    return u;
}

/* construct a name object from a string
   searches and if necessary installs string
   in ternary search tree,
   adding string to stack if so.
   returns a generic object with
       nametype tag with FBANK flag,
       mark_.pad0 set to zero
       mark_.padw contains XPOST_MEMORY_TABLE_SPECIAL_NAME_STACK stack index
 */
Xpost_Object xpost_name_cons_n(Xpost_Context *ctx,
                               const char *s,
                               unsigned int n)
{
    unsigned int u;
    unsigned int t;
    Xpost_Object o = { 0 };
    unsigned int tstk;
    int ret;

    /* A name is interned by a per-character recursive descent -- the
       search below, then tstinsert -- whose depth is the name's length.
       The scanner never hands in a name longer than its own token buffer,
       but cvn of a program-built string reaches here directly, and on the
       wide build a string may be far longer than the narrow build's 65535,
       which is the length this recursion is known to survive. Refuse a
       longer name at the single interning entry, before either walk
       begins; the caller reports it as it already reports an intern that
       could not be made. */
    if (n > 65535u)
        return invalid;

    _name_lookup_charge();

    /* LOCAL IS SEARCHED FIRST, AND THE ORDER IS LOAD-BEARING.
       The same characters can be interned in both banks: a name is
       interned into whichever bank is current when its token is first
       scanned, while xpost_name_cons_global always interns into the
       global one, for the reason given where it is defined. A name in
       both banks is two objects, not one -- hash() folds the bank flag
       into the type bits so that keys equal under
       xpost_dict_compare_objects hash together, which makes the two
       different dictionary keys. Everything a program reaches comes
       through here, so searching local first is what keeps one program
       using one of them throughout. Searching global first hands back
       the other object for those names and the interpreter does not
       finish starting up. */
    tstk = _tsttab_root(ctx->lo);
    u = tstsearch(ctx->lo, tstk, s, n);
    if (!u) {
        tstk = _tsttab_root(ctx->gl);
        u = tstsearch(ctx->gl, tstk, s, n);
        if (!u) {
            Xpost_Memory_File *mem = ctx->vmmode==GLOBAL?ctx->gl:ctx->lo;
            char inline_copy[256];
            char *chars = inline_copy;

            /* Interning the name allocates: a node for every character
               and a string to hold them. An allocation may grow the
               memory file, which moves it, and the characters offered
               here may be living in it -- cvn names a string in VM.
               Both walks below read from a copy outside it instead.
               The search above allocates nothing, so it reads the
               caller's characters directly. */
            if (n > sizeof inline_copy)
            {
                chars = malloc(n);
                if (!chars)
                {
                    XPOST_LOG_ERR("cannot copy name characters");
                    return invalid;
                }
            }
            memcpy(chars, s, n);

            ret = tstinsert(mem, _tsttab_root(mem), chars, n, &t);
            if (ret)
            {
                //this can only be a VMerror
                if (chars != inline_copy)
                    free(chars);
                return invalid;
            }
            _tsttab_set_root(mem, t);
            u = addname(ctx, chars, n); // obeys vmmode
            if (chars != inline_copy)
                free(chars);
            o.mark_.tag = nametype | (ctx->vmmode==GLOBAL?XPOST_OBJECT_TAG_DATA_FLAG_BANK:0);
            o.mark_.pad0 = 0;
            o.mark_.padw = u;
        } else {
            o.mark_.tag = nametype | XPOST_OBJECT_TAG_DATA_FLAG_BANK; // global
            o.mark_.pad0 = 0;
            o.mark_.padw = u;
        }
    } else {
        o.mark_.tag = nametype; // local
        o.mark_.pad0 = 0;
        o.mark_.padw = u;
        }
    return o;
}

Xpost_Object xpost_name_cons(Xpost_Context *ctx,
                             const char *s)
{
    return xpost_name_cons_n(ctx, s, (unsigned int)strlen(s));
}

/* Resolve a string to an already-interned name without interning.
   The search half of xpost_name_cons_n and nothing more: local bank
   first, then global -- the same load-bearing order, so a string that is
   a name in both banks resolves to the same object a store of it used as
   a key. A string that is not yet a name returns the invalid object; the
   caller reads that as "no such name, therefore in no dictionary". Nothing
   is allocated, so the caller's characters are read in place, and a name
   too long to have been interned (longer than the intern refuses) cannot
   be present and is reported not found. */
Xpost_Object xpost_name_find_n(Xpost_Context *ctx,
                               const char *s,
                               unsigned int n)
{
    unsigned int u;
    Xpost_Object o = { 0 };

    if (n > 65535u)
        return invalid;

    _name_lookup_charge();

    u = tstsearch(ctx->lo, _tsttab_root(ctx->lo), s, n);
    if (u)
    {
        o.mark_.tag = nametype; /* local */
        o.mark_.pad0 = 0;
        o.mark_.padw = u;
        return o;
    }
    u = tstsearch(ctx->gl, _tsttab_root(ctx->gl), s, n);
    if (u)
    {
        o.mark_.tag = nametype | XPOST_OBJECT_TAG_DATA_FLAG_BANK; /* global */
        o.mark_.pad0 = 0;
        o.mark_.padw = u;
        return o;
    }
    return invalid;
}

/* construct a name object in global VM regardless of the current
   allocation mode, ignoring any local interning of the same string.
   The operator table records names by their global index; resolving
   an operator name through the local tree can alias a different
   global name with the same numeric index. */
Xpost_Object xpost_name_cons_global(Xpost_Context *ctx,
                                    const char *s)
{
    unsigned int u;
    unsigned int t;
    Xpost_Object o = { 0 };
    unsigned int tstk;
    int ret;

    _name_lookup_charge();

    tstk = _tsttab_root(ctx->gl);
    u = tstsearch(ctx->gl, tstk, s, (unsigned int)strlen(s));
    if (!u) {
        unsigned int vmmode = ctx->vmmode;

        ctx->vmmode = GLOBAL;
        ret = tstinsert(ctx->gl, _tsttab_root(ctx->gl), s, (unsigned int)strlen(s), &t);
        if (ret)
        {
            ctx->vmmode = vmmode;
            return invalid;
        }
        _tsttab_set_root(ctx->gl, t);
        u = addname(ctx, s, (unsigned int)strlen(s));
        ctx->vmmode = vmmode;
    }
    o.mark_.tag = nametype | XPOST_OBJECT_TAG_DATA_FLAG_BANK;
    o.mark_.pad0 = 0;
    o.mark_.padw = u;
    return o;
}

/* yield the string object from the name string stack
    */
Xpost_Object xpost_name_get_string(Xpost_Context *ctx,
               Xpost_Object n)
{
    Xpost_Memory_File *mem = xpost_context_select_memory(ctx, n);
    unsigned int names;
    Xpost_Object str;
    names = xpost_memory_name_stack_ent(mem);
    str = xpost_stack_bottomup_fetch(mem, names, n.mark_.padw);
    return str;
}

