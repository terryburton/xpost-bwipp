/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h> /* calloc malloc qsort free: the slide gathers its live rows */
#include <string.h>

#include "xpost_log.h"
#include "xpost_memory.h" /* Xpost_Memory_File */
#include "xpost_object.h" /* Xpost_Object */
#include "xpost_file.h" /* Xpost_File: what a file entity holds */
#include "xpost_handle.h" /* what a handle entity holds */
#include "xpost_error.h" /* VMerror: what a refused rearrangement is */
#include "xpost_free.h"

/*
   initialize the free-list in the memory file.
   free list head is in slot zero
   sz is 0 so gc will ignore it */
/* collection threshold in allocated bytes; overridable for testing
   and for embedders that want more frequent collections */
static int _xpost_free_gc_threshold(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char *e = getenv("XPOST_GC_THRESHOLD");
        v = e ? atoi(e) : 0;
        if (v <= 0)
            v = XPOST_GARBAGE_COLLECTION_THRESHOLD;
    }
    return v;
}

/* The free list is segregated into size-class buckets so that both
   freeing and allocation are near-constant-time: a single sorted list
   makes every operation walk the entities smaller than the request,
   which dominates once a large collection has populated the list.
   Bucket b holds entities with size in [2^(b+4), 2^(b+5)), clamped to
   the first and last buckets. The head words live in the FREE special
   entity's data area. */
/* the bucket map and count live in xpost_free.h, shared with the sweep */

/* Write a bucket head.

   The FREE entity records a size of zero rather than the size of the
   area it owns, so that a composite object left holding entity zero --
   which is what an object that was never constructed holds -- cannot
   write through it: every bounded accessor refuses an entity of no
   size, and the thousand bytes behind this one are there to be the
   thing such a write would otherwise land in.

   What that costs is this file's own access to the words it keeps
   there: the accessors refuse the free list its heads for exactly the
   reason they refuse everybody else. So a head is reached at the
   address the entity records instead, which is the route the push
   below, the walk in xpost_free_alloc and the collector's sweep all
   take. Bounds are met by construction rather than by asking -- b is
   below XPOST_FREE_NBUCKETS, and the area allocated for the heads is
   many times the sixteen words they come to. */
static void _xpost_free_bucket_head_set(Xpost_Memory_File *mem,
                                        unsigned int b,
                                        unsigned int ent)
{
    memcpy(xpost_vm_ptr(mem, xpost_memory_free_lists_adr(mem)
                             + b * (unsigned int)sizeof(unsigned int)),
           &ent, sizeof ent);
}

static unsigned int _xpost_free_bucket_head(Xpost_Memory_File *mem,
                                            unsigned int b)
{
    unsigned int e;

    memcpy(&e, xpost_vm_ptr(mem, xpost_memory_free_lists_adr(mem)
                                 + b * (unsigned int)sizeof(unsigned int)),
           sizeof e);
    return e;
}

/* The entity after this one on the list it is on. A predecessor of zero
   means the bucket's head, so a walk needs no special case for its first
   step and an unlink none for its first entry. */
static unsigned int _xpost_free_next(Xpost_Memory_File *mem,
                                     unsigned int b, unsigned int prev)
{
    return prev ? mem->table.tab[prev].nextfree
                : _xpost_free_bucket_head(mem, b);
}

static void _xpost_free_next_set(Xpost_Memory_File *mem,
                                 unsigned int b, unsigned int prev,
                                 unsigned int next)
{
    if (prev)
        mem->table.tab[prev].nextfree = next;
    else
        _xpost_free_bucket_head_set(mem, b, next);
}

int xpost_free_init(Xpost_Memory_File *mem)
{
    unsigned int ent;
    int ret;

    /* allocate the free list head: 4 bytes in ent 0
       allocate additional 1k "scratch" space to protect
       interpreter data from NULL writes
     */
    ret = xpost_memory_table_alloc_special(mem, 1024, 0,
                                           XPOST_MEMORY_TABLE_SPECIAL_FREE, &ent);
    if (!ret)
    {
        return 0;
    }

    /* make sure this is the correct ent */

    /* set all bucket heads to zero (== NULL == end of list) */
    {
        unsigned int b;
        for (b = 0; b < XPOST_FREE_NBUCKETS; b++)
            _xpost_free_bucket_head_set(mem, b, 0);
    }

    /* record no size, so that a write through entity zero is refused
       rather than landing on interpreter data -- see the head writer
       above for what the guard is and what it costs */
    {
        Xpost_Memory_Table *tab = &mem->table;
        tab->tab[XPOST_MEMORY_TABLE_SPECIAL_FREE].sz = 0;
    }

    /* make free list available for general memory allocations */
    (void) xpost_memory_register_free_list_alloc_function(mem, xpost_free_alloc);
    mem->threshold_bytes = _xpost_free_gc_threshold();
    mem->threshold = mem->threshold_bytes;

    return 1;
}

#ifdef XPOST_VALGRIND_ARENA
/* Close every entity the free lists hold again.
 *
 * A grow reopens the whole extent, because the file copies it forward
 * and zeroes the part above the high-water mark, and the host allocator
 * hands back a block that is accessible throughout in any case. The
 * entities the collector has reclaimed are exactly the ones the free
 * lists chain -- which the table cannot say, a freed entity carrying
 * the same zero tag as a live raw allocation -- so they are read back
 * from the lists themselves and closed again here.
 */
void xpost_free_repoison(Xpost_Memory_File *mem)
{
    unsigned int headz;
    unsigned int b;
    unsigned int rows;

    /* A memory file grows whether or not it has been given the rest of
       the machinery, and the free lists are one of the entities in its
       table rather than a part of the file itself. A file made on its
       own has no table for them to be in, and one still being built has
       a table that has not reached them yet; either way nothing has been
       chained, so there is nothing to close again and no address to read
       the chains from. */
    if (!mem || !mem->base || !xpost_memory_free_lists_ready(mem))
        return;
    headz = xpost_memory_free_lists_adr(mem);
    /* no chain can hold more entities than the table has rows, which is
       the bound the walk below is held to */
    rows = mem->table.nextent;
    for (b = 0; b < XPOST_FREE_NBUCKETS; b++)
    {
        unsigned int e;
        unsigned int seen = 0;

        e = _xpost_free_bucket_head(mem, b);
        /* the walk is bounded by the table it indexes, so a link spoiled
           by a stale write cannot spin it */
        while (e && xpost_ent_valid(mem, e) && seen <= rows)
        {
            unsigned int a = mem->table.tab[e].adr;
            unsigned int s = mem->table.tab[e].sz;

            ++seen;
            e = mem->table.tab[e].nextfree;
            XPOST_VG_POISON_ENT(mem->base, a, s);
        }
    }
}
#endif

/* free this ent! returns reclaimed size or -1 on error */
int xpost_free_memory_ent(Xpost_Memory_File *mem,
                          unsigned int ent)
{
    Xpost_Memory_Table *tab;
    unsigned int rent = ent; /* relative ent index */
    unsigned int sz; /* sz associated with adr */
    /* return; */

    if (ent < mem->start)
        return 0;

    if (!xpost_ent_valid(mem, ent))
    {
        XPOST_LOG_ERR("cannot free ent %u", ent);
        return -1;
    }
    tab = &mem->table;
    sz = tab->tab[rent].sz;
    if (sz == 0) return 0; /* do not add zero-size allocations to list */

    if (tab->tab[rent].tag == filetype)
    {
        /* retire this file from its birth-stamp bucket */
        {
            unsigned int b = (tab->tab[rent].mark
                              & XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK)
                             >> XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET;

            if (b < 256 && mem->file_births[b] > 0)
            {
                mem->file_births[b]--;
                while (mem->file_birth_max > 0
                    && mem->file_births[mem->file_birth_max] == 0)
                    mem->file_birth_max--;
            }
        }
        /* A file entity carries a handle on the stream abstraction, which
           is asked for as such. A stream is closed through its own method
           table, never by fclose on something that merely happens to be
           the same width, and a handle of one kind does not resolve for
           another.

           Reaching here with a live stream would be a caller's mistake,
           not a case to handle: the entity is only offered for reclaim
           once its stream has been closed and its handle given up, which
           is why the one caller that can present a file entity tests for
           no stream before asking. Say so and decline, rather than guess
           at a close for a stream something else still believes it
           owns. */
        if (xpost_handle_block_at(mem, ent, XPOST_HANDLE_FILE,
                                  XPOST_FILE_BLOCK_SIZE))
        {
            XPOST_LOG_ERR("refusing to reclaim file ent %u: its stream is "
                          "still open", ent);
            return -1;
        }
    }
    /* a handle is an entity in here and a block outside, and the
       entity is on its way to the free list */
    if (tab->tab[rent].tag & XPOST_MEMORY_TABLE_TAG_HANDLE)
        xpost_handle_release_entity(mem, ent);
    tab->tab[rent].tag = 0;

    /* push onto the bucket. The link is in the table rather than in the
       entity's own storage, so nothing the allocator needs survives in
       the bytes just released. */
    {
        unsigned int b = xpost_free_bucket_for_size(sz);

        mem->table.tab[ent].nextfree = _xpost_free_bucket_head(mem, b);
        _xpost_free_bucket_head_set(mem, b, ent);
    }
    XPOST_VG_POISON_ENT(mem->base, tab->tab[rent].adr, sz);

    return sz;
}

static void _dump_chain(Xpost_Memory_File *mem, unsigned int b)
{
    unsigned int e = _xpost_free_bucket_head(mem, b);

    while (e)
    {
        unsigned int sz;
        if (!xpost_memory_table_get_size(mem, e, &sz)) return;
        printf("%u(%u) ", e, sz);
        e = mem->table.tab[e].nextfree;
    }
}

unsigned int xpost_free_bytes(Xpost_Memory_File *mem)
{
    unsigned int total = 0;
    unsigned int b;
    /* no chain can hold more entities than the table has rows, which is
       the bound the walk below is held to: a walk that passes it is
       following a cycle rather than a list, and stops with what it has
       instead of never returning */
    unsigned int rows = mem->table.nextent;

    for (b = 0; b < XPOST_FREE_NBUCKETS; b++)
    {
        unsigned int e = _xpost_free_bucket_head(mem, b);
        unsigned int seen = 0;

        while (e && seen <= rows)
        {
            unsigned int sz;

            if (!xpost_memory_table_get_size(mem, e, &sz))
                break;
            total += sz;
            ++seen;
            e = mem->table.tab[e].nextfree;
        }
    }
    return total;
}

/* print a dump of the free list */
void xpost_free_dump(Xpost_Memory_File *mem)
{
    unsigned int e;
    unsigned int z;
    unsigned int b;
    unsigned int headz;

    headz = xpost_memory_free_lists_adr(mem);

    printf("freelist: ");
    for (b = 0; b < XPOST_FREE_NBUCKETS; b++)
    {
        z = headz + b * sizeof(unsigned int);
        memcpy(&e, xpost_vm_ptr(mem, z), sizeof(unsigned int));
        if (e) printf("[bucket %u] ", b);
        _dump_chain(mem, z);
    }
}

/* Scan the free list for a suitably-sized bit of memory.

   A collection is asked for when the bytes allocated since the last one
   reach the count the run named through VMThreshold (PLRM C.3.5), which
   is what mem->threshold counts down.

   Returns 1 on success, 0 on failure, 2 to request garbage collection
   and re-call. */
int xpost_free_alloc(Xpost_Memory_File *mem,
                     unsigned int sz,
                     unsigned int tag,
                     unsigned int *entity)
{
    unsigned int e;                     /* working pointer */
    int ret;

    if (!mem->interpreter_get_initializing())
    {
        if ((mem->threshold -= sz) <= 0)
        {
            mem->threshold = mem->threshold_bytes;
            /* A collection is due: ask for one, and carry on to the free
               list. The count paces collection; it says nothing about
               whether this request can be met from memory already free,
               and refusing the list here would take fresh memory for a
               block that is sitting in it. The interpreter runs the
               collection at its safe point between operators. */
            if (mem->garbage_collect_is_installed)
                mem->garbage_collect_pending = 1;
        }
    }

    {
    unsigned int b;

    for (b = xpost_free_bucket_for_size(sz); b < XPOST_FREE_NBUCKETS; b++)
    {
        unsigned int best = 0, bestprev = 0, bestsz = 0;
        unsigned int prev = 0;
        unsigned int seen = 0;

        e = _xpost_free_next(mem, b, prev);
        while (e && seen < XPOST_FREE_SCAN_LIMIT) /* e is not zero */
        {
            unsigned int tsz;

            ++seen;
            /* saturating, so a count this large cannot present itself
               as a small one to whatever is reading it */
            if (mem->free_scan < (unsigned int)INT_MAX)
                ++mem->free_scan;

            /* Handing out an entity that is not actually free aliases two
               owners onto one allocation, so validate every node: freed
               entities carry a zero tag. The links are in the memory
               table, out of reach of a write through a stale reference,
               but the table is written from more places than this one --
               the reclaimer, the collector's sweep, an image read -- and
               a chain is only as sound as the rows it runs through. On
               any inconsistency discard the lists and request a
               collection to rebuild them. */
            if (e > XPOST_OBJECT_COMP_MAX_ENT ||
                !xpost_ent_valid(mem, e) ||
                mem->table.tab[e].tag != 0)
            {
                unsigned int bb;
                XPOST_LOG_ERR("free list corrupt at ent %u (tag %u): discarding",
                        e, xpost_ent_valid(mem, e) ? mem->table.tab[e].tag : 0);
                /* Every bucket, not just this one: a write that spoiled
                   one link says nothing about the others, and a bucket
                   left standing is a later walk back into the same
                   state. The heads are written at their address for the
                   reason given where that writer is defined. Nothing
                   here can fail, so the request for a collection is not
                   conditional on it -- and it must not be, because
                   returning a plain failure would leave the caller
                   allocating afresh with the lists never rebuilt. */
                for (bb = 0; bb < XPOST_FREE_NBUCKETS; bb++)
                    _xpost_free_bucket_head_set(mem, bb, 0);
                return XPOST_FREE_WANT_COLLECTION; /* refill the list first */
            }
            ret = xpost_memory_table_get_size(mem, e, &tsz);
            if (!ret)
            {
                XPOST_LOG_ERR("cannot retrieve size of ent %u", e);
                return 0;
            }

            /* Best fit among the entries this bucket is allowed to
               offer: entity numbers are a fixed budget, so near-exact
               recycling matters more than the byte waste an oversized
               entry leaves, which a later collection reclaims. An
               exact fit ends the search outright; otherwise the
               closest of the first XPOST_FREE_SCAN_LIMIT entries is
               taken, and the rest of the chain -- whose length is the
               job's release history rather than anything about this
               request -- is left unwalked. */
            if (tsz >= sz && (best == 0 || tsz < bestsz))
            {
                best = e;
                bestprev = prev;
                bestsz = tsz;
                if (tsz == sz)
                    break;
            }

            prev = e;
            e = mem->table.tab[e].nextfree;
        }

        if (best)
        {
            Xpost_Memory_Table *tab = &mem->table;
            unsigned int ad;

            ret = xpost_memory_table_get_addr(mem, best, &ad);
            if (!ret)
            {
                XPOST_LOG_ERR("cannot retrieve address of ent %u", best);
                return 0;
            }
            /* unlink: the predecessor was recorded when the node was
               reached, and zero means the bucket's head */
            _xpost_free_next_set(mem, b, bestprev, tab->tab[best].nextfree);
            tab->tab[best].nextfree = 0;
            /* the entity is being handed out again: its storage is
               readable once more, and holds nothing yet */
            XPOST_VG_UNPOISON_ENT(mem->base, ad, bestsz);
            tab->tab[best].tag = tag;
            *entity = best;
            return 1; /* found, return SUCCESS */
        }
    }
    }
    /* finished scanning free list */

    return 0; /* not found, fall-back to _new allocator */
}


/* Sort by address, so the slide visits the entities in the order they
   lie in the arena and each one moves down into space already vacated. */
typedef struct { unsigned int adr, ent; } _Xpost_Slide;

static int _by_adr(const void *a, const void *b)
{
    const _Xpost_Slide *x = a, *y = b;

    return (x->adr < y->adr) ? -1 : (x->adr > y->adr);
}

/* Close the gaps between the live entities, so that the free storage
   gathers above them.

   The free lists let a released block serve a later request of its own
   size, but nothing moves, so a job that releases many small blocks and
   then asks for large ones holds an arena full of holes it cannot use
   and cannot hand back. This walks the table, which is a complete
   account of the arena only because every block in it carries a row, and
   slides each live entity down over whatever free blocks lie below it.
   The row's address is rewritten as the bytes move: it is the one place
   an entity's location is written down, which is what makes moving one
   possible at all.

   The blocks slid over cease to exist. Their storage is inside what the
   live entities now occupy, so the lists that named them are emptied and
   their rows are handed back to be issued again.

   MUST NOT BE CALLED FROM INSIDE AN OPERATOR. Every pointer into virtual
   memory is derived from an entity's recorded address, and a caller
   between deriving one and using it holds a pointer this invalidates.
   The interpreter's safe point between operator executions is where no
   such pointer is held, which is the same reason a collection is taken
   there; tests/check-compaction-safe-point.sh holds the callers to it. */
int xpost_free_compact(Xpost_Memory_File *mem, unsigned int *freed)
{
    unsigned char *isfree;
    _Xpost_Slide *live;
    unsigned int rows, nlive = 0, i, b, cursor, floor_adr = 0xffffffffu;
    unsigned int before;

    if (freed) *freed = 0;
    if (!mem || !mem->base || !xpost_memory_free_lists_ready(mem))
        return 0;

    before = mem->high_water;
    rows = mem->table.nextent;
    isfree = calloc(rows ? rows : 1, 1);
    live = malloc(sizeof(*live) * (rows ? rows : 1));
    if (!isfree || !live)
    {
        free(isfree);
        free(live);
        XPOST_LOG_ERR("%d out of memory rearranging the arena", VMerror);
        return 0;
    }

    /* The blocks the live ones are slid over. Read before anything
       moves: the heads live in virtual memory themselves, inside an
       entity this pass relocates. */
    for (b = 0; b < XPOST_FREE_NBUCKETS; b++)
    {
        unsigned int e = _xpost_free_bucket_head(mem, b);
        unsigned int seen = 0;

        while (e && xpost_ent_valid(mem, e) && seen <= rows)
        {
            isfree[e] = 1;
            ++seen;
            e = mem->table.tab[e].nextfree;
        }
    }

    for (i = 0; i < rows; i++)
    {
        if (mem->table.tab[i].sz == 0)
            continue;
        /* The lowest address any entity holds is the floor the slide
           stops at. Below it is the reservation the bank opens with,
           which nothing addresses and nothing may be moved onto. */
        if (mem->table.tab[i].adr < floor_adr)
            floor_adr = mem->table.tab[i].adr;
        if (isfree[i])
            continue;
        live[nlive].adr = mem->table.tab[i].adr;
        live[nlive].ent = i;
        nlive++;
    }
    if (floor_adr == 0xffffffffu)
    {
        free(isfree);
        free(live);
        return 0;
    }
    qsort(live, nlive, sizeof(*live), _by_adr);

    cursor = floor_adr;
    /* The slide writes over the blocks it passes, and a block the lists
       hold is closed to a checker the arena is described to. So the
       extent is opened for the length of the pass and closed again as
       the pass goes: the padding as each entity lands, and the whole run
       above the cursor once the slide is done. Without this every move
       is a write the checker refuses, and every later read of an entity
       that moved is a read it refuses -- the arena's own bookkeeping
       reported as the error it exists to find. */
    XPOST_VG_REOPEN_RANGE(mem->base, floor_adr, before - floor_adr);
    for (i = 0; i < nlive; i++)
    {
        unsigned int e = live[i].ent;
        unsigned int a = mem->table.tab[e].adr;
        unsigned int s = mem->table.tab[e].sz;
        unsigned int pad;

        if (a != cursor)
        {
            /* bytes, whatever the entity holds: the pass moves storage
               and does not read it */
            unsigned char *to = xpost_vm_ptr(mem, cursor);
            unsigned char *from = xpost_vm_ptr(mem, a);

            memmove(to, from, s);
            mem->table.tab[e].adr = cursor;
        }
        /* alignment is kept as the allocator hands it out, so that an
           entity's address means the same thing after the pass as before */
        pad = ((s + 7u) & ~7u) - s;
        if (pad)
            XPOST_VG_POISON_RANGE(mem->base, cursor + s, pad);
        cursor += (s + 7u) & ~7u;
    }

    for (b = 0; b < XPOST_FREE_NBUCKETS; b++)
        _xpost_free_bucket_head_set(mem, b, 0);
    for (i = 0; i < rows; i++)
        if (isfree[i])
        {
            /* the storage is inside what the live entities now occupy,
               so the row describes nothing before it is handed back */
            mem->table.tab[i].sz = 0;
            if (!xpost_memory_table_release_row(mem, i))
                XPOST_LOG_ERR("%d a row emptied by the rearrangement was "
                              "refused, so its number is spent", VMerror);
        }

    mem->high_water = cursor;

    /* and what stands above the mark is nobody's to read, whether or not
       the storage under it goes back to the system below */
    if (before > cursor)
        XPOST_VG_POISON_RANGE(mem->base, cursor, before - cursor);

    /* The storage the blocks occupied is now one run above the cursor,
       which is the whole reason for gathering it there: the pages under
       it go back to the system in a single call, where before they could
       only be given up a free block at a time and only where a block
       happened to cover a whole page. Anything allocated here later
       takes the range afresh, and a page given up this way faults back
       in cleared, which is what an allocation above the cursor gets in
       any case. */
    if (before > cursor)
        (void)xpost_memory_file_release_range(mem, cursor, before - cursor);

    if (freed) *freed = before - cursor;

    free(isfree);
    free(live);
    return 1;
}
