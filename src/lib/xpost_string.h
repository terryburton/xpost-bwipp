/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_STRING_H
#define XPOST_STRING_H

#include "xpost_private.h" /* XPOST_MUST_CHECK */

/**
 * @file xpost_string.h
 * @brief string functions
 *
 * An string object is 8 bytes,
 * consisting of 4 16bit fields common to all composite objects
 *   tag, type enum and flags
 *   sz, count of objects in array
 *   ent, entity number   --- nb. ents have outgrown their field: use xpost_object_get/set_ent()
 *   off, offset into allocation
 * the entity data is a "C" array of chars
 *
 * "_memory" functions require a memory file to be specified.
 * functions without "memory" select the memory file from a context, using the FBANK flag.
 * an array object with the FBANK flag properly set, is called a "banked array".
 *
 * @{
 */

/**
 * @brief construct a string object (possibly initialized) in the specified memory
 */
Xpost_Object xpost_string_cons_memory(Xpost_Memory_File *mem,
                                      unsigned sz,
                                      /*@NULL@*/ const char *ini);

/**
 * @brief construct a string object in correctly selected memory
 */
XPOST_TEST_VISIBLE Xpost_Object xpost_string_cons(Xpost_Context *ctx,
                                                  unsigned sz,
                                                  /*@NULL@*/ const char *ini);

/**
 * @brief yield a "C" pointer to the char array of the string contents
 */
/*@dependent@*/
XPOST_TEST_VISIBLE char *xpost_string_get_pointer(Xpost_Context *ctx,
                                                  Xpost_Object S);

/**
 * @brief put a value into a string with specified memory
 */
int xpost_string_put_memory(Xpost_Memory_File *mem,
                            Xpost_Object s,
                            integer i,
                            integer c);

/**
 * @brief put a value into a string
 *
 * Checks the string's write access and returns invalidaccess when it is
 * withheld; the result must be checked.
 */
XPOST_MUST_CHECK int xpost_string_put(Xpost_Context *ctx,
                                      Xpost_Object s,
                                      integer i,
                                      integer c);

/**
 * @brief get a value from a string with specified memory
 */
int xpost_string_get_memory(Xpost_Memory_File *mem,
                            Xpost_Object s,
                            integer i,
                            integer *retval);

/**
 * @brief get a value from a string
 */
int xpost_string_get(Xpost_Context *ctx,
                     Xpost_Object s,
                     integer i,
                     integer *retval);

/**
 * @brief allocate and return a C-style nul-terminated string
 */
char *xpost_string_allocate_cstring(Xpost_Context *ctx,
                                    Xpost_Object s);

/**
 * @brief whether the string's characters are the whole of it read as C text
 *
 * A string counts its characters (PLRM 3.3) rather than ending them at a
 * sentinel, so a nul is a character of a string like any other. C text
 * ends at the first nul instead, and every comparison, length and
 * conversion the host library offers stops there: a string carrying a nul
 * handed to one of them answers to the shorter string it begins with,
 * which is a different string. Answers no for such a string, so that a
 * caller about to read it as C text can refuse it in whatever terms its
 * own operator refuses what it cannot name.
 */
int xpost_string_is_cstring(Xpost_Context *ctx,
                            Xpost_Object s);

/**
 * @}
 */

#endif
