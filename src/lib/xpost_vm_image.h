/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_VM_IMAGE_H
#define XPOST_VM_IMAGE_H

#include "xpost.h" /* XPAPI, xpost_vm_image_refuse */
#include "xpost_private.h" /* XPOST_TEST_VISIBLE */

/**
 * @file xpost_vm_image.h
 * @brief A context's virtual memory as one file, written and read back.
 *
 * An image is both banks of a context's virtual memory written whole:
 * the bytes of each arena, the table that indexes it, the arena's own
 * bookkeeping, and the few members of the context that name what the
 * arenas hold. What is left out is host state -- where this process
 * mapped the arena, which descriptor it was opened on, and the
 * functions installed in the memory file -- none of which is part of
 * what the arena holds and all of which a reader builds for itself.
 *
 * What the file does NOT hold is as much the point as what it does. The
 * operator table's rows carry the addresses of the C functions that
 * implement the operators, and those are this process's; an entity that
 * names a block held outside virtual memory names it by a handle this
 * process issued. tests/vm_host_state.register is where both are
 * written down and what a reader must do with each.
 *
 * The two are dealt with in opposite ways. A host address is written as
 * a zero and rebuilt at the read, from the operator table this process
 * built for itself before it read anything -- which is safe only
 * because the image carries the operator names in row order and the
 * read holds this build's table to them row for row. An operator object
 * carries the number of its row and nothing else, so a table whose rows
 * came out in another order would raise nothing and dispatch to the
 * wrong operator. A handle is not written at all: an image is refused
 * at the write if any entity carries one, so nothing in a written image
 * stands for a block that only the writing process had.
 *
 * The one exception is a write asked to keep host state, which is for
 * measuring rather than for reading back: it writes the operator
 * functions as this process holds them, so that a comparison of two
 * images meets them rather than a fabrication. Such an image is refused
 * at the read.
 *
 * An image is a picture of one build's memory and not a portable
 * document. The stamps at its head say which build, in the terms that
 * decide whether its bytes mean the same thing here: the object width,
 * the size of a pointer and of each structure the arena holds at a
 * fixed shape, the byte order, this build's version, and a hash of
 * every boot file the language was built out of. A stamp that disagrees
 * is a refusal, and every refusal is a fall back to booting the
 * language from those files.
 *
 * An image answers for its own bytes: the last four are a digest of all
 * the rest, and a read that does not arrive at the same value stops
 * before anything in the file is read as anything. What that catches,
 * and what it does not, is XPOST_VM_IMAGE_DIGEST_SEED.
 *
 * A read that refuses an image says so where a run is told what it is
 * doing rather than where it is told what went wrong: nothing has gone
 * wrong, and what a refusal costs the run is the time it would have
 * saved. A write that was asked for and did not happen is the other way
 * about, and is reported as the failure it is.
 *
 * Return convention: 1 for success and 0 for failure, as the memory
 * module uses.
 */

/**
 * @typedef Xpost_Vm_Image_Stamp
 * @brief What an image says about the build that wrote it.
 *
 * The head of an image is these values in this order, one four-byte
 * quantity each, after the magic. A reader compares each against what
 * this build would have written and refuses the image naming the first
 * that disagrees, so an image is turned away by name rather than read
 * as something it is not.
 *
 * Everything here answers "would these bytes mean the same thing in
 * this process": the layout stamps say the structures the arena holds
 * are the shape this build reads them as, the build stamp says the
 * language was assembled by the same version, and the data stamp says
 * it was assembled out of the same boot files. What no stamp can answer
 * is a change to this build's C that leaves every structure the same
 * shape and every boot file untouched; the version is what carries such
 * a change, and a build that wants no doubt has the environment
 * variable that refuses every image.
 */
typedef enum
{
    XPOST_VM_IMAGE_STAMP_VERSION,       /**< the layout below */
    XPOST_VM_IMAGE_STAMP_ENDIAN,        /**< a known value, written natively */
    XPOST_VM_IMAGE_STAMP_OBJECT_SIZE,   /**< sizeof(Xpost_Object) */
    XPOST_VM_IMAGE_STAMP_ENT_MAX,       /**< the highest entity number */
    XPOST_VM_IMAGE_STAMP_POINTER_SIZE,  /**< sizeof(void *) */
    XPOST_VM_IMAGE_STAMP_SIGNATURE_SIZE,/**< sizeof(Xpost_Signature) */
    XPOST_VM_IMAGE_STAMP_OPERATOR_SIZE, /**< sizeof(Xpost_Operator) */
    XPOST_VM_IMAGE_STAMP_CONTEXT_SIZE,  /**< sizeof(Xpost_Context) */
    XPOST_VM_IMAGE_STAMP_BUILD,         /**< this build's version and sources */
    XPOST_VM_IMAGE_STAMP_CONFIG,        /**< the options that change the language */
    XPOST_VM_IMAGE_STAMP_DATA,          /**< every boot file, hashed */
    XPOST_VM_IMAGE_STAMP_BANKS,         /**< how many banks follow */
    XPOST_VM_IMAGE_STAMP_CONTEXT_FIELDS,/**< how many context values follow */
    XPOST_VM_IMAGE_STAMP_ROOTS,         /**< how many context objects follow */
    XPOST_VM_IMAGE_STAMP_TYPENAMES,     /**< how many type names follow */
    XPOST_VM_IMAGE_STAMP_HOST_STATE,    /**< the operator functions are written */
    XPOST_VM_IMAGE_STAMP_OPERATORS,     /**< how many operator rows follow */
    XPOST_VM_IMAGE_STAMP_BANK_FIELDS,   /**< how many values each bank carries */
    XPOST_VM_IMAGE_STAMPS
} Xpost_Vm_Image_Stamp;

/**
 * @typedef Xpost_Vm_Image_Bank_Field
 * @brief The arena bookkeeping an image carries, one field per name.
 *
 * These are the members of the memory file that describe the arena
 * rather than the host: what of it is in use, where the collector's
 * domain begins, what the allocator and the collector have counted, and
 * the flags a bank carries between allocations. They are written as one
 * run of values in this order, so that a reader can name each one it
 * reads back and a comparison can say which differed.
 *
 * Deliberately absent: the arena's mapped capacity and the entity
 * table's allocated capacity. Both say how much room this process asked
 * for around what the bank holds -- one is rounded by the page size and
 * a growth policy, the other doubles from a fixed start -- and a reader
 * arrives at its own for whatever it reads.
 */
typedef enum
{
    XPOST_VM_IMAGE_BANK_HIGH_WATER,
    XPOST_VM_IMAGE_BANK_START,
    XPOST_VM_IMAGE_BANK_NEXTENT,
    XPOST_VM_IMAGE_BANK_FREE_SUBSTACK,
    XPOST_VM_IMAGE_BANK_FREE_SCAN,
    XPOST_VM_IMAGE_BANK_THRESHOLD,
    XPOST_VM_IMAGE_BANK_GC_ENT_BUDGET,
    XPOST_VM_IMAGE_BANK_FILE_BIRTH_MAX,
    XPOST_VM_IMAGE_BANK_GC_AUTO,
    XPOST_VM_IMAGE_BANK_GC_PENDING,
    XPOST_VM_IMAGE_BANK_ENT_RESERVE_OPEN,
    XPOST_VM_IMAGE_BANK_ENT_EXHAUSTED,
    XPOST_VM_IMAGE_BANK_PUSH_REFUSED,
    XPOST_VM_IMAGE_BANK_FIELDS
} Xpost_Vm_Image_Bank_Field;

/**
 * @def XPOST_VM_IMAGE_CTX_FIELDS
 * @brief The context's own values an image carries, one per name.
 *
 * A context is mostly host state -- the memory files it runs on, the
 * caches it keeps for the process, the answers to how this run was
 * started -- and none of that belongs in an image. What does belong is
 * what the boot settled about virtual memory and left in the context
 * rather than in the arenas: where the stacks are, what the interpreter
 * is allocating in, and the one-shots the lockdown spent. A reader that
 * did not take these back would have the language in its arenas and no
 * way to reach it.
 *
 * Deliberately absent: everything the host decides for a run (the
 * device, the output, whether there is a user at the other end),
 * everything a running program leaves behind (the object being
 * executed, the operands an error would restore, the error a run ended
 * with), every pointer the context holds, and the generation counter
 * that says whether the cache of name resolutions is current -- which
 * counts this process's own bindings and belongs to the cache a reader
 * throws away.
 *
 * Written as one run of four-byte values in this order. The named
 * members are read back into the same members, so the list and the
 * reading cannot come apart.
 */
#define XPOST_VM_IMAGE_CTX_FIELDS(_) \
    _(os) \
    _(es) \
    _(ds) \
    _(hold) \
    _(id) \
    _(vmmode) \
    _(state) \
    _(quit) \
    _(nest_depth) \
    _(callback_error) \
    _(pagedevice_depth) \
    _(packing) \
    _(sysdict_unlocked) \
    _(sysdict_load_done) \
    _(operator_install_refused) \
    _(es_run_base)

/**
 * @typedef Xpost_Vm_Image_Row_Field
 * @brief The fields of one entity table row, in the order written.
 */
typedef enum
{
    XPOST_VM_IMAGE_ROW_ADR,
    XPOST_VM_IMAGE_ROW_USED,
    XPOST_VM_IMAGE_ROW_SZ,
    XPOST_VM_IMAGE_ROW_MARK,
    XPOST_VM_IMAGE_ROW_TAG,
    XPOST_VM_IMAGE_ROW_NEXTFREE,
    XPOST_VM_IMAGE_ROW_FIELDS
} Xpost_Vm_Image_Row_Field;

/**
 * @def XPOST_VM_IMAGE_MAGIC
 * @brief What an image begins with, so a reader knows what it has.
 */
#define XPOST_VM_IMAGE_MAGIC "XPOSTVM\n"
#define XPOST_VM_IMAGE_MAGIC_LEN 8

/**
 * @def XPOST_VM_IMAGE_VERSION
 * @brief The layout below, which a reader must know in full.
 */
#define XPOST_VM_IMAGE_VERSION 7u

/**
 * @def XPOST_VM_IMAGE_DIGEST_SEED
 * @brief What the digest of an image's bytes begins from.
 *
 * The last four bytes of an image are a digest of every byte before
 * them, and a read that does not arrive at the same value stops there.
 *
 * WHAT IT ANSWERS. That the file is the one that was written. Every
 * single-byte difference is caught, and caught with certainty rather
 * than with high probability: each step of the digest exclusive-ors a
 * byte into the running value and multiplies by an odd constant, and
 * multiplication by an odd constant is one-to-one over the width of the
 * value, so a difference introduced at any byte cannot be cancelled by
 * the bytes after it. A difference of several bytes is caught unless the
 * bytes conspire, which for damage that is not aimed comes to about one
 * chance in four thousand million.
 *
 * WHAT IT DOES NOT ANSWER, plainly: that the image came from anywhere in
 * particular. It is a check and not a signature. Anyone able to write
 * the file is able to work out the digest of what they wrote -- the
 * constants are here in the open and there is no secret anywhere -- so
 * an image assembled on purpose passes as readily as a true one. What
 * this catches is damage: a write that stopped part way, storage that
 * decayed, a file half copied, a byte changed by somebody who did not
 * expect to be checked.
 *
 * So an image is exactly as trusted as the boot files beside it and the
 * executable that reads it, and wants the same permissions: it is read
 * into virtual memory, which holds the procedures the language is made
 * of and the numbers that index the operator table, and a program that
 * can choose what goes in there can choose what the interpreter does.
 * The defence against a hostile image is that nobody hostile can write
 * the file. This is the defence against an unlucky one.
 */
#define XPOST_VM_IMAGE_DIGEST_SEED 2166136261u

/**
 * @def XPOST_VM_IMAGE_ENDIAN
 * @brief A value whose bytes say which order they were written in.
 *
 * Every number in an image is written as this build's own bytes. One
 * written by a build of the other byte order reads as a different
 * number here, and this is the number that says so.
 */
#define XPOST_VM_IMAGE_ENDIAN 0x01020304u

/**
 * @def XPOST_VM_IMAGE_BANKS
 * @brief Global then local, which is the order the banks are written in.
 */
#define XPOST_VM_IMAGE_BANKS 2u

/**
 * @def XPOST_VM_IMAGE_FILE_BIRTHS
 * @brief How many birth-stamp counters a bank carries.
 */
#define XPOST_VM_IMAGE_FILE_BIRTHS 256u

/**
 * @brief The name of one stamp, by its position at the head of an image.
 */
XPOST_TEST_VISIBLE const char *xpost_vm_image_stamp_name(unsigned int stamp);

/**
 * @brief The name of one arena bookkeeping field.
 *
 * Answers the empty string for an index outside the set, so a reader of
 * an image written by a version it does not know cannot walk off the
 * end of the names while reporting.
 */
XPOST_TEST_VISIBLE const char *xpost_vm_image_bank_field_name(unsigned int field);

/**
 * @brief The name of one entity table row field.
 */
XPOST_TEST_VISIBLE const char *xpost_vm_image_row_field_name(unsigned int field);

/**
 * @brief The name of one bank, by its position in an image.
 */
XPOST_TEST_VISIBLE const char *xpost_vm_image_bank_name(unsigned int bank);

/**
 * @brief Write both banks of @p ctx's virtual memory to @p path.
 *
 * @param[in] ctx The context whose virtual memory is written.
 * @param[in] path Where to write it.
 * @param[in] host_state Write the operator functions as this process
 *            holds them, for a reader that means to look at them. An
 *            image written this way cannot be read back into a context.
 * @return 1 on success, 0 on failure.
 *
 * The image is of the memory as it stands at the call. Nothing is
 * collected, moved or normalised on the way out: what a reader meets is
 * what the interpreter was running on.
 *
 * Refused where any entity of either bank carries a handle on a block
 * held outside virtual memory. Such a handle stands for nothing in
 * another process, and an image is written to be read in one.
 *
 * The layout, in the writing build's byte order:
 *
 *   XPOST_VM_IMAGE_MAGIC, then XPOST_VM_IMAGE_STAMPS values in the
 *   order the stamp enumeration gives.
 *
 *   then, for each operator row in turn: how many signatures it states,
 *   the length of its name, and the name, padded with zeros to a whole
 *   number of values. This is what holds a reader's own operator table
 *   to the one the image was written with.
 *
 *   then the context: the values XPOST_VM_IMAGE_CTX_FIELDS names, in
 *   that order; the objects the context roots, in the order
 *   XPOST_CONTEXT_OBJECT_ROOTS gives; and the executable name of each
 *   object type.
 *
 *   then, for each bank in turn: its name, eight bytes, padded with
 *   zeros; XPOST_VM_IMAGE_BANK_FIELDS values in the order the field
 *   enumeration gives; XPOST_VM_IMAGE_FILE_BIRTHS counters; five values
 *   for each of the bank's entity table rows, in the order the row
 *   enumeration gives; and finally the bytes of the arena, from its
 *   start to the high-water mark the bank's used field records.
 *
 *   and last, one value: the digest of every byte above it.
 *
 * Every value is a four-byte unsigned quantity. The signed members of
 * the memory file are written as the four bytes they occupy, so a
 * reader takes them back as it stored them.
 */
XPOST_TEST_VISIBLE int xpost_vm_image_write(Xpost_Context *ctx,
                                            const char *path,
                                            int host_state);

/**
 * @brief Read @p path into @p ctx's virtual memory.
 *
 * @param[in] ctx A context whose operator table has been built and
 *            whose arenas hold nothing else worth keeping.
 * @param[in] path The image to read.
 * @return 1 where the context now holds what the image did, 0 where it
 *         holds exactly what it held before the call.
 *
 * Answering 0 is not a failure of the run. Every way an image can be
 * unusable -- absent, unreadable, short, written by another build, of
 * the other object width, holding host state, naming operators this
 * build does not have or has in another order -- ends here, with the
 * context untouched and the caller free to boot the language the long
 * way. Nothing is written into the context until everything the image
 * says has been read and checked, and the arenas are grown to hold it
 * before any of it is copied in, so a refusal late in the read is as
 * clean as one at the magic.
 *
 * What the read rebuilds is the operator table's host addresses. The
 * table this process built for itself is read out before the image
 * displaces it -- every row's name, and the functions each signature
 * carries -- and the image's rows are held to those names one for one
 * before a single function is put back. Beyond the rows this build
 * installs from C the image's rows are the ones the boot files wrapped
 * around procedures, and each is required to carry the procedure that
 * makes it one.
 */
XPOST_TEST_VISIBLE int xpost_vm_image_load(Xpost_Context *ctx,
                                           const char *path);

/**
 * @brief Whether the language this process is running came from an image.
 *
 * Answers what the last read did, so a caller that boots a context can
 * tell which way it was brought up.
 */
XPOST_TEST_VISIBLE int xpost_vm_image_in_use(void);

/**
 * @brief Whether this process has said it will build the language.
 */
XPOST_TEST_VISIBLE int xpost_vm_image_refused(void);

/**
 * @brief The options that change the language, as this process was told.
 *
 * Said before the context exists (xpost_vm_image_config_set), because the
 * language is decided as the context is made. Read there to settle which
 * language to build or to accept.
 */
XPOST_TEST_VISIBLE unsigned int xpost_vm_image_config(void);

#endif
