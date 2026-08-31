/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_MEMORY_H
#define XPOST_MEMORY_H

#include <stddef.h> /* size_t, NULL */

#include "xpost_private.h" /* XPOST_TEST_VISIBLE */


/**
 * @file xpost_memory.h
 * @brief The memory management data structures, #Xpost_Memory_File and
 * #Xpost_Memory_Table.
 *
 * Return convention: the functions of this module answer 1 for success
 * and 0 for failure (the object-mutator modules answer the opposite --
 * 0 for no-error, nonzero for the PostScript error to raise; each
 * header states which convention it uses).
 */


/*
 *
 * Macros
 *
 */


/*
 *
 * Enums
 *
 */

/**
 * @typedef Xpost_Memory_Table_Mark_Data
 *
 * There are 4 "virtual" bitfields packed in what is assumed to be a
 * 32-bit unsigned field. These values are used in masking and
 * shifting operations to access the fields in a direct, portable
 * manner.
 */
typedef enum
{
    XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK       = 0x7F000000,
    XPOST_MEMORY_TABLE_MARK_DATA_MARK_OFFSET     =     24,
    /* reserved: always written 0, read only by the table dumps; kept so
       the LOWLEVEL/TOPLEVEL save-level fields keep their positions */
    XPOST_MEMORY_TABLE_MARK_DATA_REFCOUNT_MASK   = 0x00FF0000,
    XPOST_MEMORY_TABLE_MARK_DATA_REFCOUNT_OFFSET =       16,
    XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK   = 0x0000FF00,
    XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET =         8,
    XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_MASK   = 0x000000FF,
    XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_OFFSET =           0
} Xpost_Memory_Table_Mark_Data;

/**
 * @typedef Xpost_Memory_Table_Special
 * @brief Special entities occupy the first few slots of the first
 * #Xpost_Memory_Table in the #Xpost_Memory_File.
 *
 * These enumerators are named here, in the constructor that builds each
 * entity, and nowhere else. Everywhere else reaches one through the
 * accessor named for it below, which is why those accessors can be total.
 */
typedef enum
{
    XPOST_MEMORY_TABLE_SPECIAL_FREE,
    XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK,
    XPOST_MEMORY_TABLE_SPECIAL_CONTEXT_LIST,
    XPOST_MEMORY_TABLE_SPECIAL_NAME_STACK,
    XPOST_MEMORY_TABLE_SPECIAL_NAME_TREE,
    XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME
} Xpost_Memory_Table_Special;

/**
 * @typedef Xpost_Memory_Collect_Start
 * @brief The first entity the collector owns, per memory file.
 *
 * The special entities are roots, not garbage: the collector's domain
 * begins one past the last of them. Both banks hold the same ones, so
 * both domains begin at the same slot. Derived from the enumerators
 * above so the two cannot drift.
 */
typedef enum
{
    XPOST_MEMORY_COLLECT_START_GLOBAL = XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME + 1,
    XPOST_MEMORY_COLLECT_START_LOCAL = XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME + 1
} Xpost_Memory_Collect_Start;

/**
 * @typedef Xpost_Memory_Table_Pressure
 * @brief When a table's size is reason enough to collect, and how often.
 *
 * XPOST_MEMORY_TABLE_PRESSURE is the number of entity slots a table may
 * reach before its size alone makes a collection worthwhile, whatever
 * the bytes behind those slots come to. It is half the entity numbers
 * the ordinary build's object field can carry, so a job heading for the
 * end of that range is offered collections across the whole second half
 * of it; in a build whose field spans more than the table, it is simply
 * a table large enough to be worth sweeping.
 *
 * XPOST_MEMORY_TABLE_GC_BUDGET is how many entity allocations pass
 * between one request and the next while the table stays that large.
 * It is wide enough that a single operator building one large answer
 * does not spend it several times over before the interpreter reaches
 * the safe point where a collection can run.
 */
typedef enum
{
    XPOST_MEMORY_TABLE_PRESSURE = 262144,
    XPOST_MEMORY_TABLE_GC_BUDGET = 65536
} Xpost_Memory_Table_Pressure;

/**
 * @typedef Xpost_Memory_Table_Reserve
 * @brief The entity numbers held back for reporting their exhaustion.
 *
 * Telling a program that the numbers have run out costs numbers:
 * interning the error's name is a string, recording the error in $error
 * is three arrays, and printing what happened is more of both. A run
 * that spent the last number on the program's own data would have
 * nothing left to say so with, and an error a program is never told
 * about is one it cannot catch.
 *
 * So the last of the range is not the program's to spend while it has
 * anything else: ordinary allocation stops this far short of the end
 * and raises limitcheck there. The interpreter opens the reserve as it
 * raises an error, and it shuts at the first allocation made with room
 * outside it -- the run has recovered, and the reserve is whole again.
 *
 * That leaves it open across the recovery itself, which is deliberate.
 * A program handed back a wall it may not describe is no better off
 * than one never told: its own handler prints, formats and records like
 * any other code. A program that ignores the error and keeps allocating
 * spends the reserve instead, and meets the same wall a thousand
 * composites later with the machinery's own needs by then long since
 * paid for -- so the error stays catchable however often it is met.
 *
 * The size is what the error machinery wants many times over, and the
 * whole of it is a five-hundredth of the ordinary build's range.
 */
typedef enum
{
    XPOST_MEMORY_TABLE_ENT_RESERVE = 1024
} Xpost_Memory_Table_Reserve;


/*
 *
 * Structs
 *
 */

/**
 * @struct Xpost_Memory_Table
 * @brief The Memory Table: one flat array of allocation records,
 * grown by realloc as entities are allocated.
 */
typedef struct Xpost_Memory_Table
{
    unsigned int nextent; /**< next slot in table */
    unsigned int max; /**< allocated size */
    /** The head of the rows that describe nothing, or 0 for none.

        A row that stops being used normally keeps its storage and goes
        on a block free list, where an allocation takes the number and
        the bytes together. A pass that rearranges the arena separates
        them: it slides the live entities down over a free block, so the
        block's bytes are gone while its row remains, and a row on no
        list is a number that can never be issued again. Those rows are
        chained here instead, through the same link a free row carries,
        and the two chains cannot overlap -- a row is either on a block
        list with its storage or on this one with none.

        Derived rather than stored: every row on it has a size of zero,
        so a table read back from an image is scanned for them rather
        than carrying a copy of this head that could disagree. */
    unsigned int freerow;
    struct
    {
        unsigned int adr; /**< allocation address */
        unsigned int used; /**< size in use */
        unsigned int sz; /**< size of allocation */
        unsigned int mark; /**< garbage collection metadata */
        unsigned int tag; /**< type of object using this allocation, if needed */
        /** the next entity on the free list this one is on, or zero at
            the end of it. Meaningful only while the entity is free.
            It is here rather than in the entity's own storage so that
            the storage of a freed entity holds nothing the allocator
            needs: a stale write into it can no longer turn a link into
            an arbitrary entity number, and the pages it sits in can be
            handed back to the system without taking the list with them. */
        unsigned int nextfree;
    } *tab; /**< table entries */
} Xpost_Memory_Table;

/**
 * @struct Xpost_Memory_File
 * @brief A memory region that may be suballocated. Bookkeeping data
 * for region allocator.
 *
 * Used as the basis for the Postscript Virtual Memory.
 */
typedef struct Xpost_Memory_File
{
    int fd; /**< file descriptor associated with this memory/file,
                  or -1 if not used. */
    char fname[20]; /**< file name associated with this memory/file,
                          or "" if not used. */
    /*@dependent@*/
    unsigned char *base; /**< pointer to mapped memory */
    /* The arena's high-water mark: the offset one past the last byte
       handed out, and so where the next fresh allocation begins. It is a
       place in the arena rather than a quantity of it -- an entity row's
       `used` is the quantity, and the two were one word for long enough
       to be filed as one thing. */
    unsigned int high_water;
    unsigned int max; /**< size available in memory pointed to by base */

    struct Xpost_Memory_Table table;

    unsigned int start; /**< first 'live' entry in the memory_table. */
        /* the domain of the collector is entries >= start */

    /* The operator table, and the runs of signatures it points at.
       Global memory only; the local bank leaves it null.

       It is host storage and not an entity of the arena, because it is
       the one thing that would hold host addresses there: a signature
       keeps the C function implementing it and the one checking its
       operands. Everything else in the arena is an entity number or an
       offset, which is what lets the arena be compacted, handed back to
       the operating system and written out as an image. Keeping the
       table outside it means that statement has no exception, and an
       image needs neither to scrub those addresses on the way out nor
       to put them back on the way in.

       A row names its run of signatures by an offset from optab rather
       than by a pointer, so growing the block moves nothing that names
       what is in it. */
    unsigned char *optab;
    unsigned int optab_max; /**< bytes optab points at */

    /* How this bank's writes are being tracked, so that a job boundary
       can put back only the pages a job wrote instead of copying the
       whole baseline over itself. What is kept here is the host's
       business and differs between them -- see xpost_vm_writeset.h --
       and on a host with no answer worth having it stays empty and the
       boundary copies. */
    struct
    {
        int fd;        /**< the baseline as something mappable, or -1 */
        int tracking;  /**< the host is recording writes to this bank */
        size_t len;    /**< the extent the arrangement covers */
        size_t used;   /**< the baseline's own extent within it */
        size_t back_lo; /**< bytes changed with nothing writing them: */
        size_t back_hi; /**< the range handed back to the system */
        const unsigned char *against;  /**< the baseline it was made against */
        int wanted;    /**< it worked once, so make it again after a grow */
    } writeset;

    unsigned int free_substack; /**< one recycled save-record substack, or 0.
                                     Save-record stacks are raw file allocations,
                                     not table entities, so the collector cannot
                                     reclaim them; restore parks an emptied one
                                     here for the next save to reuse instead of
                                     leaking it (see xpost_save.c). */

    unsigned int free_scan; /**< free-list entries the allocator has
                                  examined over the life of this memory
                                  file. An allocation looks at a bounded
                                  number of entries in each size class it
                                  tries, so this rises with the number of
                                  allocations a job makes and not with the
                                  memory it has already released; a count
                                  that tracks the latter is the shape
                                  xpost_free.c's scan bound exists to
                                  prevent, and this is what reads it. The
                                  count saturates rather than wrapping, so
                                  a large number cannot present itself as
                                  a small one. */

    unsigned int stack_walk; /**< stack segments stepped over in this
                                   memory file, over its life. A stack is
                                   a chain of segments, so reaching a
                                   position in one costs the segments
                                   between it and the end the walk starts
                                   from. A scan of a whole stack that
                                   walks the chain once costs the chain's
                                   length; a scan that asks for each
                                   index in turn costs that length once
                                   per element, which is the stack's
                                   length squared. This is the number
                                   that tells the two apart, and nothing
                                   about a scan's answer does. The count
                                   saturates rather than wrapping, so a
                                   large number cannot present itself as
                                   a small one. */

    /** The packed path in this file whose element chain has been walked
        and found well formed, and how far that walk reached.

        A path is held in one entity, and no writer moves an element
        boundary below the extent a walk has already reached, so what a
        walk established still holds over the part of the path that was
        there at the time and the next walk over the same entity starts
        where the last one stopped. The writers are enumerated where the
        walk is, in xpost_op_path.c.

        The record lives with the memory file so that it is per file and
        so that xpost_memory_table_alloc can drop it. That is where an
        entity number is handed out, and a number handed out names
        contents of somebody else's writing, about which a walk of the
        contents before them establishes nothing. It is dropped there
        and nowhere else: a number is released from several places --
        one at a time in xpost_free.c, and in runs by the collector's
        own sweep -- so a release is a place to be forgotten, while
        every number in existence is issued from that one. */
    struct
    {
        unsigned int ent; /**< the entity walked, 0 for none */
        unsigned int end; /**< the extent walked: every element from the
                                header up to here was well formed, and
                                here is an element boundary */
        unsigned int sps; /**< the subpath-start offset that walk found
                                naming an element */
        unsigned int last; /**< the last-element offset that walk found
                                 naming an element */
        unsigned int steps; /**< path elements walked over the life of
                                  this memory file. Checking a path costs
                                  the elements appended since it was last
                                  checked, so this rises with the length
                                  of the paths a job builds and not with
                                  that length times the number of times
                                  the path is read. The count saturates
                                  rather than wrapping, so a large number
                                  cannot present itself as a small one. */
    } path_walk;

    /** Bytes still to be allocated before a collection is asked for.
        Counted down by every allocation and reloaded from
        threshold_bytes when it runs out, so what paces an automatic
        collection is a count of bytes allocated since the last one --
        which is what the VMThreshold user parameter names (PLRM
        C.3.5). */
    int threshold;
    /** What threshold is reloaded with: the count a run asks for
        through VMThreshold, or the interpreter's default. Held here
        rather than read from the context because the allocator that
        reloads it is reached without one. */
    int threshold_bytes;
    int free_list_alloc_is_installed;
    int (*free_list_alloc)(struct Xpost_Memory_File *mem,
                           unsigned sz,
                           unsigned tag,
                           unsigned int *entity);

    int garbage_collect_is_installed;
    unsigned int gc_ent_budget; /**< entity allocations left before the
                                      next collection is requested while
                                      the table is under size pressure.
                                      Spent on every allocation, from the
                                      free list as well as on a fresh
                                      slot, so the rate of requests does
                                      not depend on how much the table
                                      happens to have grown */
    unsigned int file_births[256]; /**< live file entities by birth stamp
                                         (save depth + 1): restore's close
                                         sweep runs only when a file was born
                                         above the restored depth */
    unsigned int file_birth_max; /**< highest stamp with a nonzero count */
    int garbage_collect_auto; /**< whether a collection that runs of its
                                    own accord reclaims this bank. PLRM
                                    8.2's vmreclaim turns automatic
                                    collection off for one bank or for
                                    both, and on again for both; an
                                    immediate collection the operator
                                    asks for is not automatic and runs
                                    either way */
    /** The program has asked for the arena to be closed up, and the
        interpreter has not reached the point where it can be.

        Rearranging the arena moves the bytes under every pointer derived
        from an entity's recorded address, and the machinery running an
        operator holds such pointers, so the operator that asks cannot be
        the one that acts. The request is left here and read at the same
        safe point between operator executions that a collection is taken
        at, where the only references are the ones the table describes.

        Not carried in a virtual memory image. It stands only between an
        operator and the next step of the interpreter, so an image
        written with it set would be recording that a request had been
        made rather than anything about what the memory holds. */
    int compact_pending;

    /* A census of what the interpreter can reach and the seal's walk did
       not, asked for by .vmblind and taken at the same safe point for the
       same reason: it marks from the collector's roots, which are the
       stacks and the save and name stacks, and marking from inside an
       operator reads a root set the operator's own C frames are still
       holding storage out of. Asking is not doing (PLRM has nothing to
       say here; this is the tree's own rule, and a compaction that
       ignored it killed real renders).

       Not carried in a virtual memory image, for the reason above it. */
    int blind_pending;
    unsigned int blind_reach;   /**< entities the interpreter reaches */
    unsigned int blind_missed;  /**< of those, containers the walk did not */
    unsigned int blind_missed_str; /**< and strings, which hold nothing */

    int garbage_collect_pending; /**< a collection is due; performed at the
                                      interpreter's safe point rather than
                                      inside the triggering allocation, so
                                      operator-internal intermediates held
                                      only in C variables cannot be swept */
    int ent_reserve_open; /**< the entity numbers held back for error
                               reporting are available. Opened by the
                               interpreter as it raises an error and
                               closed by the first allocation made with
                               room outside them, so the shortage a
                               program is being told about cannot
                               silence the telling */
    int ent_exhausted; /**< the last entity allocation was refused for
                            want of entity numbers rather than for want
                            of memory, which is the difference between
                            limitcheck and VMerror */
    int push_refused; /**< a push put its object on no stack in this file.
                           A stack grows in segments and linking a fresh
                           one is an allocation, so a push into a full
                           segment is refusable; the object is then
                           nowhere and the stack is one shorter than
                           whoever pushed believes. The refusal is
                           recorded here rather than carried back by hand
                           because pushing is several hundred calls across
                           two dozen modules, and read where the pushing
                           is done with: by the operator dispatch when an
                           operator returns, and at the interpreter's safe
                           point for the pushes made outside one. Cleared
                           by whichever of those reads it */
    XPOST_MUST_CHECK int (*garbage_collect)(struct Xpost_Memory_File *mem,
                                            int dosweep,
                                            int markall);
    int interpreter_cid_get_context_is_installed;
    struct _Xpost_Context *(*interpreter_cid_get_context)(unsigned int cid);
    int (*interpreter_get_initializing)(void);
    void (*interpreter_set_initializing)(int);
} Xpost_Memory_File;

/**
 * @struct Xpost_Memory_Image
 * @brief A whole-VM snapshot of a memory file: the value store and the
 * entity table as they stood at capture, held outside the file so a later
 * restore can put the file back to exactly that state.
 *
 * A memory file's entire content is its value store (the @c base bytes up
 * to @c used) and its entity table (the first @c nextent records of
 * @c table.tab); a few file scalars (the free-list and file-birth
 * bookkeeping) complete it. An image is a copy of all of that. Restoring
 * an image writes the value store and table back and resets the cursors,
 * which reverts every object -- strings and stack contents included, since
 * they are just bytes in the store -- and discards everything allocated
 * since the capture in one stroke, by moving @c used and @c nextent back
 * rather than freeing object by object. The restore allocates nothing and
 * cannot fail: it is the job-encapsulation boundary's revert (PLRM 3.7.7),
 * which must be total and infallible.
 */
typedef struct Xpost_Memory_Image
{
    int valid;                /**< an image has been captured into this */
    unsigned char *store;     /**< copy of base[0 .. used) */
    unsigned int used;        /**< value-store cursor at capture */
    unsigned char *tab;       /**< copy of the first nextent table records */
    unsigned int nextent;     /**< table cursor at capture */
    unsigned int max;         /**< arena size at capture, restored on revert */
    unsigned int start;       /**< first collectable entity at capture */
    unsigned int free_substack;
    unsigned int free_scan;
    unsigned int gc_ent_budget;
    int garbage_collect_auto;   /**< the VMReclaim setting at capture */
    int threshold;              /**< VMThreshold pacing at capture */
    int threshold_bytes;
    unsigned int file_births[256];
    unsigned int file_birth_max;
} Xpost_Memory_Image;

/**
 * @brief Capture a whole-VM image of @p mem into @p img (allocating the
 * copies). Returns 1 on success, 0 on allocation failure. A prior image in
 * @p img is freed first.
 */
int xpost_memory_image_capture(Xpost_Memory_File *mem, Xpost_Memory_Image *img);

/**
 * @brief Restore @p mem to the state captured in @p img. Cannot fail: the
 * file only ever grows, so its store and table are at least as large as the
 * image, and putting the baseline back needs no storage the file does not
 * already own. It may arrange for the next restore to put back only what
 * changed, which does allocate; that arrangement failing costs the speed
 * and not the restore. No-op if @p img is not valid.
 */
void xpost_memory_image_restore(Xpost_Memory_File *mem, const Xpost_Memory_Image *img);

/**
 * @brief Arrange for the boundary to put @p mem back by restoring only
 *        the pages a job wrote, against the baseline at @p store.
 *
 * Answers 0 where the host cannot do it, or where the run turned it off,
 * and the caller then copies the whole baseline instead. Asked for by the
 * job baseline only.
 */
XPOST_TEST_VISIBLE int xpost_memory_revert_arm(Xpost_Memory_File *mem,
                                               const void *store, size_t used);

/**
 * @brief Release the copies an image holds and mark it invalid.
 */
void xpost_memory_image_free(Xpost_Memory_Image *img);

/*
 * The ent -> pointer middle layer.
 *
 * An entity's data is reached by looking its address up in the memory
 * table and offsetting the file's base pointer. These helpers are that
 * translation, in one place, in both the disciplines the code uses:
 * checked for ents that may be corrupt or sentinel, unchecked for ents
 * the caller has already validated (hot paths).
 *
 * The usual caveat governs every returned pointer: it is invalidated by
 * any allocation in the same memory file (the file may realloc and
 * move). Do not hold one across an allocating call.
 */

/**
 * @brief true iff @p ent indexes an allocated slot of @p mem's table.
 *
 * The one statement of what makes an entity number usable. It was three:
 * this, a macro private to the table implementation, and fifteen sites
 * that wrote the comparison out -- one of which, in the free-list walk,
 * writes it twice in the same statement to report the tag of an entity it
 * has just decided is out of range.
 */
static inline int
xpost_ent_valid(Xpost_Memory_File *mem, unsigned int ent)
{
    return ent < mem->table.nextent;
}

/**
 * @brief true iff @p ent is an entity the collector owns in @p mem.
 *
 * The special entities at the foot of the table are roots rather than
 * garbage, so the collector's domain begins at mem->start; the upper
 * bound of the band is validity itself, and is said that way so the two
 * cannot come apart.
 *
 * The sweeps walk this same band as a loop from start to nextent. There
 * the bound is an iteration limit rather than a question asked about a
 * particular entity, and it stays written that way: a call per iteration
 * would reload the limit on every step of the collector's hottest loop to
 * say something the loop already knows.
 */
static inline int
xpost_ent_in_collector_band(Xpost_Memory_File *mem, unsigned int ent)
{
    return ent >= mem->start && xpost_ent_valid(mem, ent);
}

/**
 * @brief the pointer an address in @p mem denotes.
 *
 * The one derivation of a pointer into virtual memory. Every other
 * spelling is built on this one, so the arithmetic that turns an offset
 * into an address in this process appears once.
 *
 * That is worth a function because the base MOVES. Any allocation in a
 * memory file may reallocate it, and every pointer taken before that
 * moment is then stale -- pointing into freed memory, or into the middle
 * of somebody else's entity. The hazard is why XPOST_GROW_MOVES and
 * tests/run-reloc-stress-test.sh exist, and it is what a SIGSEGV in
 * dictionary growth turned out to be, at a site whose comment still
 * described the defence a refactor had removed. A rule spelled a hundred
 * and fifty ways has a hundred and fifty places to lapse.
 */
static inline void *
xpost_vm_ptr(Xpost_Memory_File *mem, unsigned int adr)
{
    return mem->base + adr;
}

/**
 * @brief pointer to entity @p ent's data; @p ent must be valid.
 */
static inline void *
xpost_ent_ptr(Xpost_Memory_File *mem, unsigned int ent)
{
    return xpost_vm_ptr(mem, mem->table.tab[ent].adr);
}

/**
 * @brief pointer to entity @p ent's data, or NULL if @p ent is out of
 * range (a corrupt object, or the constructors' -1 sentinel).
 */
static inline void *
xpost_ent_ptr_checked(Xpost_Memory_File *mem, unsigned int ent)
{
    if (!xpost_ent_valid(mem, ent))
        return NULL;
    return xpost_ent_ptr(mem, ent);
}

/*
 * The special entities.
 *
 * Each of the first few table slots holds one structure the interpreter
 * always has: the free lists, the save stack, the context list, the two
 * halves of the name table, the operator table. They are built once, by
 * the constructor named in each accessor's comment, before any of this
 * runs.
 *
 * Their rows are reserved together, before any of those constructors
 * runs (xpost_memory_reserve_specials), so which slot each lands on is
 * stated rather than left to the order they allocate in. nextent starts
 * at zero and is only ever incremented (xpost_memory.c,
 * _xpost_memory_table_alloc_new); the table is allocated with room for a
 * thousand rows before the first one is claimed. So a special entity's
 * row exists for the life of the memory file and cannot stop existing,
 * and its address is therefore TOTAL.
 *
 * That matters because the fallible spelling of this lookup was ignored at
 * two sites in three: the caller passed an uninitialised local for the
 * address, dropped the refusal, and then used the local as an offset from
 * mem->base. There was no failure to observe -- the refusal cannot happen
 * -- but nothing in the shape of the code said so, and five files spelled
 * the same lookup with four different messages for a branch none of them
 * could take. An accessor that cannot refuse deletes the question.
 *
 * The one genuine question left open is whether a special entity has been
 * built YET, during initialisation, which is what the _ready accessors
 * answer. A reserved row is not an answer to it: the row is there from
 * the start, so what those ask is whether the row has been given
 * storage. No allocation in a memory file begins at address zero -- the
 * table stands in front of every one of them -- so an address of zero is
 * a row nothing has been built in.
 */

/**
 * @brief address of the free lists (xpost_free_init).
 */
static inline unsigned int
xpost_memory_free_lists_adr(Xpost_Memory_File *mem)
{
    return mem->table.tab[XPOST_MEMORY_TABLE_SPECIAL_FREE].adr;
}

/**
 * @brief true iff the free lists have been built.
 *
 * Asked where a memory file is worked on before it has been given the
 * rest of the machinery: a file grows whether or not the lists exist,
 * and one made on its own has no table for them to be an entity in.
 */
static inline int
xpost_memory_free_lists_ready(Xpost_Memory_File *mem)
{
    return xpost_ent_valid(mem, XPOST_MEMORY_TABLE_SPECIAL_FREE)
        && mem->table.tab[XPOST_MEMORY_TABLE_SPECIAL_FREE].adr != 0;
}

/**
 * @brief the save stack (xpost_save_init).
 *
 * The entity IS the stack's first segment rather than a row holding the
 * number of one: a segment is an entity, and this one's number is fixed
 * before any constructor runs, so a row of its own would have nothing
 * left to say.
 */
static inline unsigned int
xpost_memory_save_stack_ent(Xpost_Memory_File *mem)
{
    (void)mem;
    return XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK;
}

/**
 * @brief true iff the save stack has been built.
 *
 * The only question about a special entity that is genuinely open, and
 * only during initialisation: a file may be born before the save stack
 * exists to stamp it with a save depth (xpost_file.c).
 */
static inline int
xpost_memory_save_stack_ready(Xpost_Memory_File *mem)
{
    return xpost_ent_valid(mem, XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK)
        && mem->table.tab[XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK].adr != 0;
}

/**
 * @brief address of the context list (xpost_context_init_ctxlist).
 */
static inline unsigned int
xpost_memory_context_list_adr(Xpost_Memory_File *mem)
{
    return mem->table.tab[XPOST_MEMORY_TABLE_SPECIAL_CONTEXT_LIST].adr;
}

/**
 * @brief the name stack (xpost_name_init).
 *
 * Its own first segment, as the master save stack is, and named the same
 * way.
 */
static inline unsigned int
xpost_memory_name_stack_ent(Xpost_Memory_File *mem)
{
    (void)mem;
    return XPOST_MEMORY_TABLE_SPECIAL_NAME_STACK;
}

/**
 * @brief address of the name tree's table of nodes (xpost_name_init).
 *
 * The nodes of the ternary search tree live together in this one entity
 * and name each other by node number, so the tree is reached by asking
 * that table for its root rather than by holding an address to a node.
 */
static inline unsigned int
xpost_memory_name_tree_adr(Xpost_Memory_File *mem)
{
    return mem->table.tab[XPOST_MEMORY_TABLE_SPECIAL_NAME_TREE].adr;
}

/**
 * @brief bytes the name tree's table of nodes holds.
 */
static inline unsigned int
xpost_memory_name_tree_size(Xpost_Memory_File *mem)
{
    return mem->table.tab[XPOST_MEMORY_TABLE_SPECIAL_NAME_TREE].sz;
}

/**
 * @brief give the name tree's node table different storage.
 *
 * The table outgrows its allocation as names are interned. Its entity
 * number is fixed, so what is replaced is the storage under that row.
 */
static inline void
xpost_memory_set_name_tree(Xpost_Memory_File *mem,
                           unsigned int adr,
                           unsigned int sz)
{
    mem->table.tab[XPOST_MEMORY_TABLE_SPECIAL_NAME_TREE].adr = adr;
    mem->table.tab[XPOST_MEMORY_TABLE_SPECIAL_NAME_TREE].sz = sz;
}

/**
 * @brief exchange the storage identity of two entities.
 *
 * An ent's (adr, sz, used) travel together: a composite that GREW under
 * a save (growth reallocates and swaps in a larger allocation) otherwise
 * keeps its grown sz while pointing at the smaller backup, its byte range
 * overrunning the entities that follow. Every identity exchange -- the
 * restore revert and dictionary growth alike -- goes through here so the
 * triple can never be swapped piecemeal again.
 */
static inline void
xpost_ent_swap(Xpost_Memory_File *mem, unsigned int a, unsigned int b)
{
    unsigned int hold;
    hold = mem->table.tab[a].adr;
           mem->table.tab[a].adr = mem->table.tab[b].adr;
                                   mem->table.tab[b].adr = hold;
    hold = mem->table.tab[a].sz;
           mem->table.tab[a].sz = mem->table.tab[b].sz;
                                  mem->table.tab[b].sz = hold;
    hold = mem->table.tab[a].used;
           mem->table.tab[a].used = mem->table.tab[b].used;
                                    mem->table.tab[b].used = hold;
}

/*
 *
 * Variables
 *
 */

/**
 * @var xpost_memory_page_size
 * @brief The 'grain' of the memory-file size.
 */
extern size_t xpost_memory_page_size;

/**
 * @var xpost_memory_return_grain
 * @brief The grain in which storage can be handed back to the system.
 *
 * Not the same number as the one above on every host. A memory file is
 * sized in whatever unit the host hands address space out in, which on
 * Windows is the allocation granularity and is sixteen times the page;
 * storage is given up a page at a time. Handing back in the larger unit
 * would return nothing at all for every block smaller than it, which on
 * that host is most of them.
 */
extern size_t xpost_memory_return_grain;


/*
 *
 * Functions
 *
 */

/**
 * @brief Initialize the memory module.
 *
 * This function initializes the memory module. Currently, it only set
 * the value of #xpost_memory_page_size. It is called by
 * xpost_init().
 */
int xpost_memory_init(void);

/*
   Xpost_Memory_File functions
*/

/**
 * @brief Initialize the memory file, possibly from file specified by
 * the given file descriptor.
 *
 * @param[in,out] mem The memory file.
 * @param[in] fname The file name.
 * @param[in] fd The file descriptor.
 * @param[in] xpost_interpreter_cid_get_context How to reach a context
 *            from the identifier an object carries.
 * @param[in] xpost_interpreter_get_initializing Whether the interpreter
 *            is still being brought up.
 * @param[in] xpost_interpreter_set_initializing How to say that it is,
 *            or is no longer.
 * @return 1 on success, 0 on failure.
 *
 * This function initializes the memory file @p mem, possibly from
 * file specified by the file descriptor @p fd, if not -1.
 *
 * The three functions are handed in rather than called directly, so that
 * this module does not depend on the one that runs programs. What the
 * file wants of them is narrow: which context an object belongs to, and
 * whether the interpreter is still coming up -- a collection may not run
 * before the roots it would mark from exist.
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_memory_file_init(Xpost_Memory_File *mem,
                                      const char *fname,
                                      int fd,
                                      struct _Xpost_Context *(*xpost_interpreter_cid_get_context)(unsigned int cid),
                                      int (*xpost_interpreter_get_initializing)(void),
                                      void (*xpost_interpreter_set_initializing)(int));


/**
 * @brief Destroy the given memory file, possibly writing to file.
 *
 * @param[in,out] mem The memory file.
 * @return 1 on success, 0 on failure.
 *
 * This function destroys the memory file @p mem, possibly writing to
 * the file passed to xpost_memory_file_init().
 */
XPOST_TEST_VISIBLE int xpost_memory_file_exit(Xpost_Memory_File *mem);

/**
 * @brief Resize the given memory file, possibly moving the memory
 * and invalidating all vm pointers.
 *
 * @param[in,out] mem The memory file
 * @param[in] sz The size to increase.
 * @return 1 on success, 0 on failure.
 *
 * This function increases the memory used by @p mem by @p sz bites.
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_memory_file_grow(Xpost_Memory_File *mem,
                                      size_t sz);

/**
 * @brief Give the whole pages inside a range of the arena back to the
 * system, and answer how many bytes went.
 *
 * @param[in,out] mem The memory file
 * @param[in] adr The start of the range, as an offset into the arena
 * @param[in] len How long the range is
 * @return the bytes handed back, which is zero where none could be
 *
 * The range stays addressable and stays part of the file: what goes is
 * the storage behind it, so a later read finds zeros rather than what
 * was there and the pages are charged again when they are next written.
 * Only whole pages can go, so a range shorter than a page, or one that
 * spans no page boundary a page apart, hands back nothing.
 *
 * The caller is what knows the range holds nothing anyone will read.
 * This answers only whether the backing could take it: an arena that is
 * a mapping of a file is not this process's to give back -- the bytes
 * belong to the file -- and where the arena came from the host allocator
 * the file does not own the pages under it. A host whose only way of
 * giving storage up would leave the range unwritable answers zero as
 * well, since a range this leaves behind must stay usable without
 * anything being asked for first.
 */
unsigned int xpost_memory_file_release_range(Xpost_Memory_File *mem,
                                             unsigned int adr,
                                             unsigned int len);

/**
 * @brief Allocate memory in the given memory file and return offset.
 *
 * @param[in,out] mem The memory file.
 * @param[in] sz The new size.
 * @param[out] addr The offset.
 * @return 1 on success, 0 on failure.
 *
 * This function attempts to allocate @p sz bytes to @p mem and
 * if successful, copies the offset into @p addr.
 * If sz is 0, it just copies the offset and does not allocate any
 * memory at that address.
 *
 * @note May call xpost_memory_file_grow which may invalidate all pointers
 * derived from mem->base. MUST recalculate all VM pointers after this
 * function.
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_memory_file_alloc(Xpost_Memory_File *mem,
                                       unsigned int sz,
                                       unsigned int *addr);

/**
 * @brief Dump the given memory file metadata and contents to stdout.
 *
 * @param[in] mem The memory file.
 *
 * This function dumps to stdout the metadata of @p mem.
 */
void xpost_memory_file_dump(const Xpost_Memory_File *mem);


/*
   Xpost_Memory_Table functions
*/

/**
 * @brief Allocate and initialize a new table, with @p nspecials reserved.
 *
 * The specials are reached by number: every accessor above subscripts the
 * table with an enumerator, and the band the collector owns begins one
 * past the last of them. Their rows are reserved here, as the table is
 * made and before any constructor can allocate, so which slot each lands
 * on is stated rather than left to emerge from the order the constructors
 * happen to run in. A constructor may then allocate as freely as it likes
 * on the way to filling its own row.
 *
 * The count is asked for rather than assumed because it is not the same
 * in both banks -- the operator table is global only -- and asking makes
 * the reservation part of building a table, which is the one place it
 * cannot be forgotten.
 *
 * A reserved row exists but describes nothing: it has no storage until
 * its constructor calls xpost_memory_table_alloc_special.
 *
 * @param[in,out] mem The memory file.
 * @param[in] nspecials How many slots this bank's specials occupy.
 * @return 1 on success, 0 on failure.
 *
 * MUST recalculate all VM pointers after this function.
 * See note in xpost_memory_file_alloc().
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_memory_table_init(Xpost_Memory_File *mem,
                                                                unsigned int nspecials);

int xpost_memory_register_free_list_alloc_function(Xpost_Memory_File *mem,
                                                   int (*free_list_alloc)(struct Xpost_Memory_File *mem,
                                                                          unsigned int sz,
                                                                          unsigned int tag,
                                                                          unsigned int *entity));

int xpost_memory_register_garbage_collect_function(Xpost_Memory_File *mem,
                                                   int (*garbage_collect)(struct Xpost_Memory_File *mem,
                                                                          int dosweep,
                                                                          int markall));

/**
 * @brief Allocate memory, returns table index.
 *
 * @param[in,out] mem The memory file.
 * @param[in] sz The allocation size.
 * @param[in] tag The allocation tag.
 * @param[out] entity The table index.
 * @return 1 on success, 0 on failure.
 *
 * This function attempts to allocate a new VM entity and associated
 * memory of size @p sz. If successful, the table index for the new
 * entity is stored through the @p entity pointer.
 *
 * MUST recalculate all VM pointers after this function.
 * See note in xpost_memory_file_alloc().
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_memory_table_alloc(Xpost_Memory_File *mem,
                                        unsigned int sz,
                                        unsigned int tag,
                                        unsigned int *entity);

/**
 * @brief Give a special entity the storage its reserved row describes.
 *
 * The special entities are reached by number: every accessor above
 * subscripts the table with an enumerator, and the collector's domain
 * begins one past the last of them. Both of those read the number rather
 * than search for the entity, so a special that did not land on its own
 * slot is not a cosmetic problem -- an accessor hands back another
 * entity's storage, and the boundary the collector starts at falls in the
 * wrong place, putting a root inside its domain.
 *
 * The slot is therefore not allocated here but claimed: the row was
 * reserved before any constructor ran, and what this adds is the
 * storage. Storage comes straight off the file, which leaves nothing
 * undescribed, since this row is the description.
 *
 * A slot outside the reservation, or one claimed twice, is refused
 * rather than asserted: an interpreter that cannot be built is a failure
 * the caller can report, where an abort takes down a process that only
 * embedded this library, and an assertion is not there at all in a build
 * configured without them.
 *
 * @param[in] mem The memory file.
 * @param[in] sz Bytes the entity is to hold.
 * @param[in] tag The type tag to record.
 * @param[in] want The slot this entity has to land on.
 * @param[out] entity Where the entity number is written.
 * @return 1 on success, 0 if the allocation or the slot was refused.
 */
XPOST_MUST_CHECK int xpost_memory_table_alloc_special(Xpost_Memory_File *mem,
                                                      unsigned int sz,
                                                      unsigned int tag,
                                                      unsigned int want,
                                                      unsigned int *entity);

/**
 * @brief Get the address from an entity.
 *
 * @param[in] mem The memory file.
 * @param[in] ent The entity.
 * @param[out] addr The address.
 * @return 1 on success, 0 on failure.
 *
 * If successful, this function stores the address of the entity
 * @p ent in @p mem, through the @p addr pointer.
 */
int xpost_memory_table_get_addr(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int *addr);

/**
 * @brief Set the address for an entity.
 *
 * @param[in] mem The memory file.
 * @param[in] ent The entity.
 * @param[in] addr The new address.
 * @return 1 on success, 0 on failure.
 *
 * If successful, this function replaces the address for
 * @p ent in @p mem with a new address @p addr.
 */
int xpost_memory_table_set_addr(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int addr);

/**
 * @brief Get the size of an entity.
 *
 * @param[in] mem The memory file.
 * @param[in] ent The entity.
 * @param[out] sz The size.
 * @return 1 on success, 0 on failure.
 *
 * If successful, this function stores the size of the entity
 * @p ent in @p mem through the @p sz pointer.
 */
int xpost_memory_table_get_size(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int *sz);

/**
 * @brief Set the size for an entity.
 *
 * @param[in] mem The memory file.
 * @param[in] ent The entity.
 * @param[in] size The new size.
 * @return 1 on success, 0 on failure.
 *
 * If successful, this function replaces the size for
 * @p ent in @p mem with a new size @p size.
 */
int xpost_memory_table_set_size(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int size);

/**
 * @brief Get the mark field of an entity.
 *
 * @param[in] mem The memory file.
 * @param[in] ent The entity.
 * @param[out] mark The mark field.
 * @return 1 on success, 0 on failure.
 *
 * If successful, this function stores the mark field
 * of the entity @p ent in @p mem through the @p mark pointer.
 */
int xpost_memory_table_get_mark(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int *mark);

/**
 * @brief Set the mark field for an entity.
 *
 * @param[in] mem The memory file.
 * @param[in] ent The entity.
 * @param[in] mark The new mark field.
 * @return 1 on success, 0 on failure.
 *
 * If successful, this function replaces the mark field
 * of the entity @p ent in @p mem with the new value @p mark.
 */
int xpost_memory_table_set_mark(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int mark);

/**
 * @brief Get the tag of an entity.
 *
 * @param[in] mem The memory file.
 * @param[in] ent The entity.
 * @param[out] tag The tag.
 * @return 1 on success, 0 on failure.
 *
 * If successful, this function stores the tag field
 * of the entity @p ent in @p mem through the @p tag pointer.
 */
int xpost_memory_table_get_tag(Xpost_Memory_File *mem,
                               unsigned int ent,
                               unsigned int *tag);

/**
 * @brief Set the tag for an entity.
 *
 * @param[in] mem The memory file.
 * @param[in] ent The entity.
 * @param[in] tag The new tag.
 * @return 1 on success, 0 on failure.
 *
 * If successful, this function replaces the tag field
 * of the entity @p ent in @p mem.
 */
int xpost_memory_table_set_tag(Xpost_Memory_File *mem,
                               unsigned int ent,
                               unsigned int tag);

/**
 * @brief Fetch a value from a composite object.
 *
 * @param[in,out] mem The memory file.
 * @param[in] ent The entity.
 * @param[in] offset The offset, in units of @p sz bytes.
 * @param[in] sz The size of the transfer.
 * @param[out] dest A buffer
 * @return 1 on success, 0 on failure.
 *
 * This function performs a generic "get" operation from a composite object,
 * or other VM entity such as a file.
 * It is used to retrieve bytes from strings, objects from arrays,
 * FILE*s from files.
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_memory_get(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int offset,
                                unsigned int sz,
                                void *dest);

/**
 * @brief Put a value into a composite object.
 *
 * @param[in,out] mem The memory file.
 * @param[in] ent The entity.
 * @param[in] offset The offset, in units of @p sz bytes.
 * @param[in] sz The size of the transfer.
 * @param[in] src A buffer
 * @return 1 on success, 0 on failure.
 *
 * This function performs a generic "put" operation into a composite object,
 * or other VM entity such as a file.
 * It is used to store bytes in strings, and objects in arrays.
 */
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_memory_put(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int offset,
                                unsigned int sz,
                                const void *src);

/**
 * @brief Dump the allocation info for a single ent
 */
void xpost_memory_table_dump_ent(Xpost_Memory_File *mem,
                                 unsigned int ent);

/**
 * @brief Dump the memory table data and associated memory
 * locations from the given memory file to stdout.
 *
 * @param[in] mem The memory file.
 *
 * This function dumps to stdout the data and associated memory of
 * the table at address 0 in @p mem.
 */
void xpost_memory_table_dump(const Xpost_Memory_File *mem);

/**
 * @brief hand back the row of an entity whose storage has gone, so that
 *        its number can be issued again. Refuses a row that still holds
 *        storage: that one belongs on a block free list, which keeps the
 *        bytes for reuse as well as the number.
 */
XPOST_MUST_CHECK int xpost_memory_table_release_row(Xpost_Memory_File *mem,
                                                    unsigned int ent);

#endif
