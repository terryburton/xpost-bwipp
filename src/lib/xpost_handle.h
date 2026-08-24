/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_HANDLE_H
#define XPOST_HANDLE_H

#include <stddef.h>

#include "xpost_memory.h" /* Xpost_Memory_File */
#include "xpost_object.h" /* Xpost_Object */
#include "xpost_context.h" /* Xpost_Context */

/*
 * Where C-level state that virtual memory names lives.
 *
 * Some of what virtual memory stands for is a struct of pointers and
 * counts held outside it. A device's instance state is: it names raster
 * memory, which is not part of VM (PLRM 3.7.3), so a `restore` reaches
 * neither a page's pixels nor a writer's accumulated content. A font's
 * is: it names the face the font program was opened as, which the font
 * machinery holds outside it. A file's is: it names the stream the
 * file layer reaches its bytes through, along with the coding state and
 * buffers a filter keeps. What virtual memory holds is a handle on the
 * block rather than the block itself.
 *
 * That is what keeps virtual memory position-independent. A composite
 * object names its storage by entity number, an index into the memory
 * table, and never by address; a handle is likewise a number issued
 * here. So nothing virtual memory holds depends on where the process
 * put anything.
 *
 * A handle is issued either to a dictionary, which holds it under a key
 * as an ordinary value, or to an entity of the holder's own, which
 * carries it as the whole of what the entity holds. A file is the
 * second: the filetype object names the entity, and the entity names
 * the stream.
 *
 * Such a dictionary is an ordinary dictionary and what it holds under
 * any key is whatever was last stored there, so the value a reader is
 * handed is the program's to choose. A handle is resolved here against
 * the record of what was issued, and one naming no live block, or a
 * block of another kind, or a block of another size, resolves to
 * nothing and the reader reports rather than follows it. A handle is
 * read out of the entity carrying it and checked back against that
 * entity, so a copy of a genuine handle names the block no more than a
 * string of the program's own does.
 *
 * A kind and a size together say what a block holds, so a block issued
 * for one purpose does not resolve for another that happens to want a
 * struct of the same width.
 *
 * Where the block was issued to is recorded as well, and whether it is
 * asked for bears on what a reader may do. A device's instance state is
 * reached only through the instance it was issued to. A font
 * dictionary is copied -- scalefont and makefont copy one, and a
 * re-encoded copy of a findfont dictionary shares its face -- so one
 * face is reached through many dictionaries, and only a release of the
 * face is held to the dictionary it was issued to.
 *
 * A block outlives the device's Destroy, which releases the resources
 * the struct names and stores the cleared struct back. It is released
 * when the entity carrying its handle is reclaimed: that entity is
 * marked in the memory table, so the collector's sweep, the entity
 * reclaimer and the memory file's teardown each reach it -- the three
 * points at which a file's struct is likewise given up.
 *
 * A block held rather than issued here is the holder's: the file layer
 * allocates a stream's struct, decides when nothing reaches it any
 * longer -- a stream a filter still reads outlives the object naming
 * it -- and frees it. Giving up such a handle gives up the record of
 * the block, not the block.
 */

/**
 * @brief Bytes an entity carrying a handle holds.
 */
#define XPOST_HANDLE_ENTITY_SIZE (sizeof(unsigned int))

/**
 * @brief Memory-table tag bit marking an entity as a handle on a block.
 *
 * It sits above the fields an object tag uses, so an entity carries it
 * alongside the type the allocator recorded.
 */
#define XPOST_MEMORY_TABLE_TAG_HANDLE 0x80000000u

/**
 * @brief What a block holds.
 */
typedef enum
{
    XPOST_HANDLE_DEVICE = 1, /**< a device's instance state */
    XPOST_HANDLE_CONTENT,    /**< a vector writer's accumulated content */
    XPOST_HANDLE_FONT,       /**< a font's face */
    XPOST_HANDLE_FILE        /**< a file's stream */
} Xpost_Handle_Kind;

/**
 * @brief Issue a block of the given kind, say what gives up what the
 * block names, and store its handle in the dictionary under key.
 *
 * The block is zeroed. For a device's instance state the operator the
 * dictionary then holds under /Destroy is recorded with the block as the
 * release it is to be given up by, taken here where a device's own
 * Create has just filled the dictionary from its class and the program
 * has not yet reached it. Returns 0, or an error code.
 *
 * @p reclaim is what gives up whatever the block names, or NULL where
 * the block names nothing beyond itself. It is taken here rather than
 * registered afterwards because a block that names memory and was
 * issued without saying so is a leak nothing reports: the block goes,
 * what it named stays, and the only trace is a total that grows.
 * Issuing and saying are one call, so the question is put to whoever
 * issues a block rather than left for them to remember.
 *
 * A block that names memory of its own is ordinarily given up by
 * whoever was told to give it up -- a device's Destroy, run by the
 * interpreter. A holder that is never told is what @p reclaim is for: a
 * device a restore took back, or one nothing named by the time a
 * collection came round, whose block is reclaimed with the entity
 * carrying its handle and would otherwise take what it named with it
 * for the life of the process.
 *
 * It runs at reclamation, which is inside the collector: it may touch
 * nothing in virtual memory, only what the block names. It runs once,
 * before the block itself goes, and a holder that has already given up
 * what the block named leaves it nothing to do.
 */
int xpost_handle_cons(Xpost_Context *ctx,
                      Xpost_Object dic,
                      Xpost_Object key,
                      Xpost_Object *anchor,
                      Xpost_Handle_Kind kind,
                      size_t size,
                      void (*reclaim)(void *block));

/**
 * @brief Record a block the caller holds against an entity that already
 * exists, and store its handle in that entity.
 *
 * The entity carries the handle and nothing else, so it must be a
 * handle's width. The block stays the caller's to free. Returns 1, or 0
 * where no record can be taken or the handle cannot be stored.
 */
int xpost_handle_hold(Xpost_Memory_File *mem,
                      unsigned int ent,
                      Xpost_Handle_Kind kind,
                      size_t size,
                      void *block);

/**
 * @brief The block the handle an entity carries names, or NULL where it
 * names none of this kind and size.
 *
 * The handle is checked back against the entity it was read from, so a
 * genuine handle written into another entity names that entity's block
 * no more than a number of the program's own making does.
 */
void *xpost_handle_block_at(Xpost_Memory_File *mem,
                            unsigned int ent,
                            Xpost_Handle_Kind kind,
                            size_t size);

/**
 * @brief Give up the record of a block held against an entity, and clear
 * the handle the entity carries.
 *
 * The block is the holder's, and is not freed. The entity is cleared
 * whether or not it carried a handle that named anything. Returns 1, or
 * 0 where the entity cannot be written.
 */
int xpost_handle_drop(Xpost_Memory_File *mem, unsigned int ent);

/**
 * @brief The block a handle names, or NULL where it names none of this
 * kind and size.
 */
XPOST_NOINLINE
void *xpost_handle_block(Xpost_Context *ctx,
                         Xpost_Object anchor,
                         Xpost_Handle_Kind kind,
                         size_t size);

/**
 * @brief The block a handle names, where it was issued to this
 * dictionary; NULL otherwise.
 */
XPOST_NOINLINE
void *xpost_handle_block_of(Xpost_Context *ctx,
                            Xpost_Object anchor,
                            Xpost_Object dic,
                            Xpost_Handle_Kind kind,
                            size_t size);

/**
 * @brief The release operator recorded for the device instance state
 * issued to this dictionary, or zero where the dictionary was issued no
 * device state or the state carries no release.
 *
 * A device's instance state is issued with the release its class named
 * at that moment -- see xpost_handle_cons -- and this reports it. The
 * record is asked rather than the dictionary, so a value the program
 * wrote under /Destroy after the fact changes nothing: what comes back
 * is the operator the state was issued to be released by.
 */
XPOST_NOINLINE
unsigned int xpost_handle_device_release(Xpost_Context *ctx,
                                         Xpost_Object dic);

/**
 * @brief Give up the block an entity's handle names.
 */
void xpost_handle_release_entity(Xpost_Memory_File *mem,
                                 unsigned int ent);

/**
 * @brief Give up every block issued into a memory file.
 */
void xpost_handle_release_memory_file(Xpost_Memory_File *mem);

#endif
