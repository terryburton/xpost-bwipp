/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_save.h
 * @brief Declares save and restore.
 *
 * The implementation is in the .c beside this.
 */

#ifndef XPOST_SAVE_H
#define XPOST_SAVE_H

#include "xpost_error.h" /* the cow helper reports VMerror */

/**
 *  @file xpost_save.h
 *
 *  Each mfile has a special entity (XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK)
 *  which holds the address of the "save stack". This stack holds save objects.
 *  Save objects are ordinary postscript objects and are available to user programs.
 *
 *  The save object contains an address of a(nother) stack,
 *  this one containing saverec_ structures.
 *  A saverec_ object contains 2 entity numbers, one the source,
 *  the other the copy, of the "saved" array or dictionary.
 *
 *  stashed and stash are the interfaces used by composite objects
 *  to check-if-copying-is-necessary
 *  and copy-the-value-and-add-saverec-to-current-savelevel-stack

 *  Illustration:
 *
 *  mem[mtab[XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK].adr] = Master Save stack
 *  -- save object = { lev=0, stk=... }
 *  -- save object = { lev=1, stk=... }
 *     -- saverec
 *     -- saverec
 *  -- save object = { lev=2, stk=... }  <-- top of XPOST_MEMORY_TABLE_SPECIAL_SAVE_STACK, current savelevel stack
 *     mem[save.save_.stk] = Save Object's stack
 *     -- saverec
 *     -- saverec
 *     -- saverec = { src=foo_ent, cpy=bar_ent }
 *
 */

/*
 * @brief initialize the save stack for memory file.
 */
int xpost_save_init(Xpost_Memory_File *mem);

/*
 * @brief create a savetype object that represents a snapshot of virtual memory (array and dict) contents,
         and push a new snapshot object on the stack.
 */
Xpost_Object xpost_save_create_snapshot_object(Xpost_Memory_File *mem);

/*
 * @brief check whether an ent is contained in the current snapshot
 */
unsigned xpost_save_ent_is_saved(Xpost_Memory_File *mem, unsigned ent);

/*
 * @brief add ent to current snapshot
 */
int xpost_save_save_ent(Xpost_Memory_File *mem, unsigned tag, unsigned pad, unsigned ent);

/*
 * @brief stamp a fresh entity's mark word with the current save level
 * (both save-level fields); called by every composite constructor right
 * after allocation.
 */
void xpost_save_stamp_birth(Xpost_Memory_File *mem, unsigned int ent);

/*
 * @brief rewind the stack 1 level, reverting memory to previous snapshot.
 */
void xpost_save_restore_snapshot(Xpost_Memory_File *mem);

/*
 * @brief copy-on-write an ent into the current snapshot before a write.
 *
 * The incantation every array and dict mutator must perform: if the ent
 * is not yet under the current save, back it up now, so restore can
 * revert the write. Returns 0 on success, else the error to raise
 * (VMerror). May move the memory file: any held pointer into @p mem is
 * stale after a successful call. Strings deliberately never call this
 * (PLRM 3.7.3 exempts string contents from restore).
 */
static inline int xpost_save_cow(Xpost_Memory_File *mem,
                                 unsigned int tag,
                                 unsigned int pad,
                                 unsigned int ent)
{
    if (!xpost_save_ent_is_saved(mem, ent))
        if (!xpost_save_save_ent(mem, tag, pad, ent))
            return VMerror;
    return 0;
}

/* A saverec records two entity numbers, src and cpy. An entity number
   can exceed the 16-bit `word` that saverec_.src / saverec_.cpy provide
   (see XPOST_OBJECT_COMP_MAX_ENT, which reaches 2^20 in the small-object
   build): a bare-word store would truncate the high bits, and restore
   and the garbage collector would then revert -- or mark -- the wrong
   allocation. A saverec's tag word carries only its small type value
   (saverecs have no access or flag bits), so the high parts of both
   entity numbers ride in that tag above the 5-bit type field: src's high
   XPOST_OBJECT_TAG_EXTRA_BITS_SIZE bits, then cpy's. `pad` is left alone
   (arrays store their element count there for the collector). In the
   large-object build a `word` already spans the whole entity number, so
   the tag holds the bare type and no bits are borrowed. Saverecs are
   only ever read through these accessors, never through xpost_object_get_ent. */
#define XPOST_SAVEREC_ENT_HI_BITS  XPOST_OBJECT_TAG_EXTRA_BITS_SIZE
#define XPOST_SAVEREC_ENT_HI_MASK  ((1u << XPOST_SAVEREC_ENT_HI_BITS) - 1u)
#define XPOST_SAVEREC_SRC_HI_SHIFT 5u  /* first bit above the 5-bit type field */
#define XPOST_SAVEREC_CPY_HI_SHIFT (XPOST_SAVEREC_SRC_HI_SHIFT + XPOST_SAVEREC_ENT_HI_BITS)

#define XPOST_SAVEREC_TYPE(o) ((o).saverec_.tag & XPOST_OBJECT_TAG_DATA_TYPE_MASK)

#ifdef WANT_LARGE_OBJECT
# define XPOST_SAVEREC_TAG(type, src, cpy) ((word)(type))
# define XPOST_SAVEREC_SRC(o) ((unsigned int)(o).saverec_.src)
# define XPOST_SAVEREC_CPY(o) ((unsigned int)(o).saverec_.cpy)
#else
# define XPOST_SAVEREC_TAG(type, src, cpy) ((word)((unsigned int)(type) \
    | ((((src) >> (8u * sizeof(word))) & XPOST_SAVEREC_ENT_HI_MASK) << XPOST_SAVEREC_SRC_HI_SHIFT) \
    | ((((cpy) >> (8u * sizeof(word))) & XPOST_SAVEREC_ENT_HI_MASK) << XPOST_SAVEREC_CPY_HI_SHIFT)))
# define XPOST_SAVEREC_SRC(o) ((unsigned int)(o).saverec_.src \
    | ((((unsigned int)(o).saverec_.tag >> XPOST_SAVEREC_SRC_HI_SHIFT) & XPOST_SAVEREC_ENT_HI_MASK) << (8u * sizeof(word))))
# define XPOST_SAVEREC_CPY(o) ((unsigned int)(o).saverec_.cpy \
    | ((((unsigned int)(o).saverec_.tag >> XPOST_SAVEREC_CPY_HI_SHIFT) & XPOST_SAVEREC_ENT_HI_MASK) << (8u * sizeof(word))))
#endif

#endif
