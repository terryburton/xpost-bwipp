/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_free.h
 * @brief Declares the free lists, and describes the arena to a memory checker.
 *
 * The poison and reopen macros live here. Off the valgrind-arena profile
 * each is an empty statement, so nothing outside that profile pays for them.
 */

#ifndef XPOST_FREE_H
#define XPOST_FREE_H

/**
 *  @file xpost_free.h
 *  @brief adds de-allocation and re-allocation capabilities to xpost_memory
 *
 *  The free list is implemented to permanently occupy ent 0 of the memory table.
 *  xpost_free_init() should be the first function called after initializing
 *  the first memory table. xpost_free_init() asserts that this is so.
 *
 *  xpost_free_init() installs xpost_free_alloc as an alternate allocator
 *  for the memory file. After this function, calls to xpost_memory_table_alloc
 *  will first call xpost_free_alloc before falling back to increasing the size
 *  of the memory space.

 *  The free list is a chain of unused ents and their associated memory.
 *  The heads live in ent 0's data area, one word per bucket, and each
 *  node names the next through the `nextfree` field of its own row in
 *  the memory table -- not through the storage it stands for. Zero ends
 *  a chain, and ent 0 cannot be a node, so it also stands for "none".
 *
 *  Keeping the links out of the freed storage is what lets that storage
 *  be treated as having no meaning at all once it is released: a write
 *  into it through a stale reference spoils nothing the allocator will
 *  read, the whole extent can be marked inaccessible to a sanitizer
 *  rather than all but its first word, and pages that fall entirely
 *  inside free blocks can be handed back to the system without taking
 *  the chain with them.
 *
 *  (Zero-sized allocations are still not admitted: an entity of no size
 *  begins where the next allocation does, so handing one back out would
 *  give two entity numbers one address.)
 */

/**
 * @enum  Xpost_Garbage_Params
 * @brief what paces a collection that runs of its own accord
 *
 * PLRM C.3.5 gives the VMThreshold user parameter as "the frequency of
 * automatic garbage collection, which is triggered whenever this many
 * bytes have been allocated since the previous collection", and that is
 * what the threshold below counts. A run may name its own through
 * `setvmthreshold` or `setuserparams`; this is what it starts with.
 *
 * The number was measured rather than chosen. Over five programs -- one
 * making and dropping forty page devices, and four from the corpus, the
 * largest of them holding 85 MB live -- wall time is flat from about a
 * megabyte upward, and the count this default replaced (a thousand
 * times larger) retained between 1.8 and 4.1 times as much memory for
 * no time saving at all. Four megabytes costs at most 1.3% of wall time
 * on any of them, which is inside the run-to-run spread, and a run that
 * would rather have the memory back can ask for less.
 *
 * PLRM 8.2 setvmthreshold allows an implementation to raise a count
 * below what it can do, and this one has no such count: a request for a
 * collection is recorded rather than run where it is asked for, and the
 * interpreter takes it at its next safe point between operators. So the
 * smallest counts mean a collection at every safe point, which is slow
 * and finishes, and every count from zero up is achievable and answered
 * with itself.
 */
typedef enum
{
    XPOST_GARBAGE_COLLECTION_THRESHOLD = 4000000  /**< bytes allocated between collections */
} Xpost_Garbage_Params;


/**
 * The free list is bucketed by allocation size: ent 0's data area holds
 * XPOST_FREE_NBUCKETS list-head words, and a freed ent's `nextfree`
 * field names the next ent in its bucket. The allocator and the
 * collector's sweep both address the buckets through this one
 * size-to-bucket map, so the layout cannot drift between them.
 */
#define XPOST_FREE_NBUCKETS 16

/**
 * How many entries an allocation may examine in any one bucket.
 *
 * A bucket's chain is as long as the job's history of releasing that
 * size class, so a walk to the end of one costs what the job has
 * already freed rather than what the request asks for -- a cost that
 * grows for as long as the job runs, and is invisible to anything
 * short. What the walk is looking for does not need the whole chain:
 * a bucket above the request's own holds nothing smaller than the
 * request, so its first entries already serve, and within the
 * request's own bucket the sizes span a single power of two, so a
 * close fit is near the head if it is there at all. Examining a fixed
 * number of entries therefore keeps near-exact recycling while
 * bounding an allocation at #XPOST_FREE_SCAN_LIMIT times
 * #XPOST_FREE_NBUCKETS entries however much has been released.
 *
 * The cost of the bound is that a fit lying deeper in the request's
 * own bucket is passed over: the allocation takes a larger entry from
 * a higher bucket, or a fresh one, and the byte waste is reclaimable
 * by a later collection.
 */
#define XPOST_FREE_SCAN_LIMIT 8

static inline unsigned int
xpost_free_bucket_for_size(unsigned int sz)
{
    unsigned int b = 0;
    unsigned int s = sz >> 5;
    while (s && b < XPOST_FREE_NBUCKETS - 1)
    {
        s >>= 1;
        b++;
    }
    return b;
}

/* Make the arena legible to memcheck.
 *
 * Virtual memory is one host allocation that this file suballocates, so
 * an entity reclaimed by the collector and then read through a stale
 * reference is, to memcheck, an ordinary read of memory the process
 * owns: it reports nothing. Marking a reclaimed entity's storage
 * inaccessible turns exactly that read into an error with a stack
 * trace, which is what a collector defect needs to be caught by.
 *
 * A freed entity's storage holds nothing the file reads -- the free
 * list's links are in the memory table -- so the redzone covers the
 * whole extent, first word included.
 */
#ifdef XPOST_VALGRIND_ARENA
# include <valgrind/memcheck.h>
/* the arena begins inaccessible in its whole extent and is opened a
   piece at a time as the file hands the piece out, so that a read of
   arena the file has not allocated -- past the high-water mark, in the
   alignment padding between allocations, or of an entity the collector
   has reclaimed -- is an error naming its reader, and not a plausible
   value read out of bytes the process happens to own. */
# define XPOST_VG_POISON_RANGE(base, adr, len) \
    VALGRIND_MAKE_MEM_NOACCESS((char *)(base) + (adr), (len))
# define XPOST_VG_UNPOISON_RANGE(base, adr, len) \
    VALGRIND_MAKE_MEM_UNDEFINED((char *)(base) + (adr), (len))
/* reopen storage without saying anything about what is in it: used
   where the file rearranges an extent it has already written, so that
   reopening it does not also declare its contents fresh */
# define XPOST_VG_REOPEN_RANGE(base, adr, len) \
    VALGRIND_MAKE_MEM_DEFINED((char *)(base) + (adr), (len))
/* a freed entity's storage holds nothing the file reads, so the whole
   extent is closed */
# define XPOST_VG_POISON_ENT(base, adr, sz) \
    VALGRIND_MAKE_MEM_NOACCESS((char *)(base) + (adr), (sz))
# define XPOST_VG_UNPOISON_ENT(base, adr, sz) \
    VALGRIND_MAKE_MEM_UNDEFINED((char *)(base) + (adr), (sz))
#else
# define XPOST_VG_POISON_RANGE(base, adr, len) do { (void)(base); } while (0)
# define XPOST_VG_UNPOISON_RANGE(base, adr, len) do { (void)(base); } while (0)
# define XPOST_VG_REOPEN_RANGE(base, adr, len) do { (void)(base); } while (0)
# define XPOST_VG_POISON_ENT(base, adr, sz) do { (void)(base); } while (0)
# define XPOST_VG_UNPOISON_ENT(base, adr, sz) do { (void)(base); } while (0)
#endif

#ifdef XPOST_VALGRIND_ARENA
/**
 * @brief  close every entity the free lists hold, after an extent the
 *         host allocator has handed back accessible throughout
 */
void xpost_free_repoison(Xpost_Memory_File *mem);
#endif

/**
 * @brief  initialize the FREE special entity which points
 *         to the head of the free list
 */
int xpost_free_init(Xpost_Memory_File *mem);

/**
 * @brief  print a dump of the free list
 */
void xpost_free_dump(Xpost_Memory_File *mem);

/**
 * @brief  the bytes the free lists hold: released, still inside the
 *         arena, and available to an allocation the size suits
 *
 * The arena is never handed back to the system while the memory file
 * lives, so the space a collection recovers stays in the process and
 * shows up here rather than as a fall in what vmstatus reports used.
 * Subtracting this from the arena size is what tells an embedder that a
 * context is holding a peak it no longer needs.
 */
unsigned int xpost_free_bytes(Xpost_Memory_File *mem);

/**
 * The allocator's answer when a collection should run before retrying:
 * not a success (1) and not a plain failure (0) -- the caller records
 * the request and falls back to fresh allocation, and the interpreter
 * collects at its next safe point.
 */
#define XPOST_FREE_WANT_COLLECTION 2

/**
 * @brief  allocate data, re-using garbage if possible.
 * @return 1 = allocated from the free list into @p entity;
 *         0 = nothing suitable, fall back to fresh allocation;
 *         #XPOST_FREE_WANT_COLLECTION = as 0, and a collection is due.
 */
int xpost_free_alloc(Xpost_Memory_File *mem,
                     unsigned int sz,
                     unsigned int tag,
                     unsigned int *entity);

/**
 * @brief  explicitly add ent to free list
 */
XPOST_MUST_CHECK int xpost_free_memory_ent(Xpost_Memory_File *mem,
                          unsigned int ent);


/**
 * @brief close the gaps between the live entities, gathering the free
 *        storage above them so the pages under it can be handed back.
 *        Sets *freed, when given, to the bytes the high-water mark fell
 *        by. MUST NOT be called from inside an operator: it invalidates
 *        every pointer derived from an entity's recorded address.
 */
int xpost_free_compact(Xpost_Memory_File *mem, unsigned int *freed);

#endif
