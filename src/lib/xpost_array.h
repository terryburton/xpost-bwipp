/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_AR_H
#define XPOST_AR_H

#include "xpost_private.h" /* XPOST_MUST_CHECK */

/**
 * @file xpost_array.h
 * @brief array functions
 *
 * An array object is 8 bytes,
 * consisting of 4 16bit fields common to all composite objects
 *   tag, type enum and flags
 *   sz, count of objects in array
 *   ent, entity number   --- nb. ents have outgrown their field: use xpost_object_get/set_ent()
 *   off, offset into allocation
 * the entity data is a "C" array of objects
 *
 * "_memory" functions require a memory file to be specified.
 * functions without "memory" select the memory file from a context, using the FBANK flag.
 * an array object with the FBANK flag properly set, is called a "banked array".
 *
 * @{
 */

/**
 * @brief xpost_array_cons_memory - construct an array object
 * in the memory table of specified memory file
*/
Xpost_Object xpost_array_cons_memory(Xpost_Memory_File *mem, unsigned sz);

/**
 * @brief xpost_array_cons - construct an array object
 * selecting memory file according to ctx->vmmode
*/
Xpost_Object xpost_array_cons(Xpost_Context *ctx, unsigned sz);

/**
 * @brief store value in an array
*/
XPOST_MUST_CHECK int xpost_array_put_memory(Xpost_Memory_File *mem, Xpost_Object a, integer i, Xpost_Object o);

/**
 * @brief store value in a banked array
 *
 * Checks the array's write access and returns invalidaccess when it is
 * withheld; the result must be checked.
*/
XPOST_MUST_CHECK int xpost_array_put(Xpost_Context *ctx, Xpost_Object a, integer i, Xpost_Object o);

/**
 * @brief extract value from an array
*/
Xpost_Object xpost_array_get_memory(Xpost_Memory_File *mem, Xpost_Object a, integer i);

/**
 * @brief extract value from a banked array
*/
Xpost_Object xpost_array_get(Xpost_Context *ctx, Xpost_Object a, integer i);

/**
 * @}
 */

#endif
