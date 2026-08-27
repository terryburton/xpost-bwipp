/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dict.h
 * @brief Declares the dictionary operations.
 *
 * The implementation is in the .c beside this.
 */

#ifndef XPOST_DI_H
#define XPOST_DI_H

/**
 * @file di.h
 * @brief dictionary functions
 *
 * Return convention: the mutators of this module answer 0 for no-error
 * and otherwise the PostScript error code to raise (the memory-layer
 * modules answer the opposite -- 1 for success, 0 for failure; each
 * header states which convention it uses).
 *
 * A dictionary object is 8 bytes
 * consisting of 4 16bit fields common to composite objects:

 *   tag, type enum and flags
 *   sz, count of objects in array
 *   ent, entity number  --- nb. ents have outgrown their field! use xpost_object_get/set_ent()
 *   off, offset into allocation

 * The entity data is a header structure
 * followed by header->sz+1 key/value pairs of objects in a linear array.
 * Null keys denote empty slots in the hash table.

 * Dicts are implicitly 1 entry larger than declared
 * in order to simplify searching (terminate on null)
 */

/** @typedef typedef struct {} dichead
*/
typedef struct
{
    word tag;
    word sz;
    word nused;
    word pad;
} dichead;

typedef struct
{
    unsigned int hash;
    Xpost_Object key;
    Xpost_Object value;
} dicrec;

/**
 * @brief the dictionary header at an entity, and at a raw address.
 *
 * A dictionary's storage is a dichead followed by DICTABN(sz) records.
 * These derive the two pointers; the record table always follows its own
 * header, so it is taken from the header rather than re-offsetting the
 * memory file's base. Both are invalidated by any allocation in @p mem.
 */
static inline dichead *
xpost_dict_head(Xpost_Memory_File *mem, unsigned int ent)
{
    return (dichead *)xpost_ent_ptr(mem, ent);
}

static inline dichead *
xpost_dict_head_at(Xpost_Memory_File *mem, unsigned int adr)
{
    return (dichead *)xpost_vm_ptr(mem, adr);
}

static inline dicrec *
xpost_dict_table_of(dichead *dp)
{
    return (dicrec *)((char *)dp + sizeof(dichead));
}

/**
 * @brief yields the number of real entries in the table for a dict of size n
 */
#define DICTABN(n) (2*(n)+1)

/**
 * @brief yields the size in bytes of the table for a dict of size n
 */
#define DICTABSZ(n) (DICTABN(n) * sizeof(dicrec))

/**
 * @brief yield the access field from the dichead in vm
 */
Xpost_Object_Tag_Access xpost_dict_get_access(Xpost_Context *ctx, Xpost_Object d);

/**
 * @brief set the access field in the dichead in vm
 *
 * The head is inside the entity a save level copies, so this takes the
 * backup before writing. Answers null where the backup was refused,
 * having left the access as it found it.
 */
Xpost_Object xpost_dict_set_access(Xpost_Context *ctx, Xpost_Object d, Xpost_Object_Tag_Access access);

/**
   compare objects (<,=,>) :: (-(x),0,+(x))
*/
int xpost_dict_compare_objects(Xpost_Context *ctx, Xpost_Object l, Xpost_Object r);

/**
   compare two objects of the same simple type, without reading vm.

   This is the front of xpost_dict_compare_objects, shared with the
   interpreter's fused relational operators so the ordering of the types
   they settle exists once. Returns 1 with the three-way result in cmp
   for a pair it settles, 0 for every other pair -- those need the full
   comparison, which folds nearly-comparable types and reads composites.
*/
static inline int xpost_dict_compare_simple(Xpost_Object l, Xpost_Object r,
                                            int *cmp)
{
    Xpost_Object_Type type = xpost_object_get_type(l);

    if (type != xpost_object_get_type(r))
        return 0;

    switch (type)
    {
        case booleantype: /*@fallthrough@*/
        /* three-way compare: a subtraction can overflow the return
           type and flip the verdict */
        case integertype:
            *cmp = l.int_.val < r.int_.val ? -1 : l.int_.val > r.int_.val ? 1 : 0;
            return 1;

        case nametype:
            *cmp = (l.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ==
                   (r.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)
                ? (signed)(l.mark_.padw - r.mark_.padw)
                : (signed)((l.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) -
                           (r.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK));
            return 1;

        default:
            return 0;
    }
}

/**
   construct dictionary
   in the memory table of specified memory file
*/
Xpost_Object xpost_dict_cons_memory(/*@dependent@*/ Xpost_Memory_File *mem, unsigned sz);

/**
   construct dictionary
   selected the memory table with ctx->vmmode
*/
XPOST_TEST_VISIBLE Xpost_Object xpost_dict_cons(Xpost_Context *ctx, unsigned sz);

/**
   investigate current number of entries in dictionary
 */
XPOST_TEST_VISIBLE unsigned xpost_dict_length_memory(/*@dependent@*/ Xpost_Memory_File *mem, Xpost_Object d);

/**
   investigate current maximum size of dictionary
 */
unsigned xpost_dict_max_length_memory(/*@dependent@*/ Xpost_Memory_File *mem, Xpost_Object d);

/**
 * @brief the capacity of the dict (for maxlength): the size it was
 *        asked for, or the size of its table once it holds more
 */
unsigned xpost_dict_capacity_memory(/*@dependent@*/ Xpost_Memory_File *mem, Xpost_Object d);

/**
   investigate if size == maximum size.
 */
int xpost_dict_is_full_memory(/*@dependent@*/ Xpost_Memory_File *mem, Xpost_Object d);

/**
   print a dump of the diction contents to stdout
*/
void xpost_dict_dump_memory(Xpost_Memory_File *mem, Xpost_Object d);

/**
   return a double value containing the truncated value from
   an extendedtype object
*/
double xpost_dict_convert_extended_to_double(Xpost_Object e);

/**
   convert an extendedtype object back to its original
   integer- or real-type object.
*/
Xpost_Object xpost_dict_convert_extended_to_number(Xpost_Object e);

/**
   test dictionary for key
 */
int xpost_dict_known_key(Xpost_Context *ctx, /*@dependent@*/ Xpost_Memory_File *mem, Xpost_Object d, Xpost_Object k);

/**
   lookup value using key in dictionary
*/
Xpost_Object xpost_dict_get_memory(Xpost_Context *ctx, /*@dependent@*/ Xpost_Memory_File *mem, Xpost_Object d, Xpost_Object k);

/**
   lookup value using key in banked dictionary
*/
XPOST_TEST_VISIBLE Xpost_Object xpost_dict_get(Xpost_Context *ctx, Xpost_Object d, Xpost_Object k);

Xpost_Object xpost_dict_get_name(Xpost_Context *ctx,
                                 Xpost_Object d,
                                 Xpost_Object k);

/**
   store key and value in dictionary
*/
XPOST_MUST_CHECK int xpost_dict_put_memory(Xpost_Context *ctx, /*@dependent@*/ Xpost_Memory_File *mem, Xpost_Object d, Xpost_Object k, Xpost_Object v);

/**
   store key and value in banked dictionary
*/
XPOST_MUST_CHECK XPOST_TEST_VISIBLE int xpost_dict_put(Xpost_Context *ctx, Xpost_Object d, Xpost_Object k, Xpost_Object v);

/**
 * @brief Put key and value in dict, whatever access attribute the dict carries.
 *
 * @param[in,out] ctx The context.
 * @param[in,out] d The dictionary.
 * @param[in] k The key.
 * @param[in] v The value.
 * @return 0 on success, an error code otherwise.
 *
 * The interpreter's own write. An access attribute states what a
 * program may do with a value (PLRM 3.3.2), so it is xpost_dict_put --
 * the one a program's writes reach a dictionary through -- that holds a
 * dictionary to it. This one writes the interpreter's private
 * dictionary, which is sealed against a program and written by the
 * interpreter for as long as the run lasts.
 */
XPOST_MUST_CHECK int xpost_dict_put_internal(Xpost_Context *ctx, Xpost_Object d, Xpost_Object k, Xpost_Object v);

/**
   undefine key in dictionary, re-slotting any later entry whose probe
   chain ran through the vacated slot (Knuth 6.4R deletion)
*/
int xpost_dict_undef_memory(Xpost_Context *ctx, Xpost_Memory_File *mem, Xpost_Object d, Xpost_Object k);

/**
   undefine key in banked dictionary. Returns 0 or the error to raise;
   `undefined` reports an absent key, which the undef operator ignores
   (PLRM: undef of an unknown key has no effect).
*/
int xpost_dict_undef(Xpost_Context *ctx, Xpost_Object d, Xpost_Object k);

#endif
