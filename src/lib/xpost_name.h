/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_NM_H
#define XPOST_NM_H

/**
 * @file xpost_name.h
 * @brief array functions
 *
 * The name mechanism associates strings with integers
 * using a ternary search tree
 * and a stack of string objects.
 *
 * @{
 */

#include "xpost_private.h" /* XPOST_TEST_VISIBLE */

typedef struct tst
{
    unsigned val,
             lo,
             eq,
             hi;
} tst;
int xpost_name_init(Xpost_Context *ctx);
XPOST_TEST_VISIBLE Xpost_Object xpost_name_cons(Xpost_Context *ctx, const char *s);

/*
   construct a name object from a counted string, which may contain
   any bytes, embedded nuls included
 */
Xpost_Object xpost_name_cons_n(Xpost_Context *ctx, const char *s, unsigned int n);

/**
 * @brief Construct a name object in global VM regardless of the
 * current allocation mode. Operator names must live in the global
 * name space: the operator table records them by global index.
 */
Xpost_Object xpost_name_cons_global(Xpost_Context *ctx, const char *s);
Xpost_Object xpost_name_get_string(Xpost_Context *ctx, Xpost_Object n);

/**
 * @brief How many times a string has been offered to the name mechanism
 * and had to be looked up in the tree.
 *
 * A name already interned still costs a walk, and a global name costs
 * two -- the local bank is searched first and misses. So this counts
 * the work of resolving names, not the names that exist: a caller that
 * resolves the same name once per unit of work it does is the shape
 * this number is read for. It saturates rather than wrapping.
 */
unsigned int xpost_name_lookups(void);

/**
 * @}
 */

#endif
