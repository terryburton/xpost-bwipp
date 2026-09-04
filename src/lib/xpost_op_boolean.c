/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_boolean.c
 * @brief Installs the boolean and bitwise operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * and or not xor eq ne gt ge lt le bitshift
 *
 * The comparisons answer for every type PLRM says they answer for, which is
 * why they are here rather than beside the arithmetic.
 */

/* relational, boolean, and bitwise operators */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_name.h"
#include "xpost_dict.h"
#include "xpost_error.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_boolean.h"

/* Whether the comparison of this pair may read what it is handed.

   Two strings are compared element by element, and a string compared
   with a name is compared against the name's characters; either way the
   string's own characters are read, and a value may be read only where
   its access permits (PLRM 3.3.2, and the invalidaccess entry, which
   names the read of an execute-only string among the violations).

   Only a pair the comparison would take that far is asked. Objects of
   unlike kinds are told apart by their types alone, with no value read,
   so a string of any access compares unequal to an integer rather than
   raising; and access is no part of what makes two objects equal (PLRM
   8.2 eq), so a readable string is compared with whatever it is
   compared with, whatever access that carries. */
static
int _compare_may_read(Xpost_Context *ctx, Xpost_Object x, Xpost_Object y)
{
    int xt = xpost_object_get_type(x);
    int yt = xpost_object_get_type(y);
    int xs = xt == stringtype;
    int ys = yt == stringtype;

    if (xs && (ys || yt == nametype) && !xpost_object_is_readable(ctx, x))
        return 0;
    if (ys && (xs || xt == nametype) && !xpost_object_is_readable(ctx, y))
        return 0;
    return 1;
}

/* any1 any2  eq  bool
   test equal */
static
int xpost_op_any_any_eq (Xpost_Context *ctx,
                         Xpost_Object x,
                         Xpost_Object y)
{
    if (!_compare_may_read(ctx, x, y))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_op_relation(XPOST_OP_REL_EQ,
                         xpost_dict_compare_objects(ctx, x, y))));
    return 0;
}

/* any1 any2  ne  bool
   test not equal */
static
int xpost_op_any_any_ne (Xpost_Context *ctx,
                         Xpost_Object x,
                         Xpost_Object y)
{
    if (!_compare_may_read(ctx, x, y))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_op_relation(XPOST_OP_REL_NE,
                         xpost_dict_compare_objects(ctx, x, y))));
    return 0;
}

/* any1 any2  ge  bool
   test greater or equal */
static
int xpost_op_any_any_ge (Xpost_Context *ctx,
                         Xpost_Object x,
                         Xpost_Object y)
{
    if (!xpost_op_ordered_comparable(x, y))
        return typecheck;
    if (!_compare_may_read(ctx, x, y))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_op_relation(XPOST_OP_REL_GE,
                         xpost_dict_compare_objects(ctx, x, y))));
    return 0;
}

/* any1 any2  gt  bool
   test greater than */
static
int xpost_op_any_any_gt (Xpost_Context *ctx,
                         Xpost_Object x,
                         Xpost_Object y)
{
    if (!xpost_op_ordered_comparable(x, y))
        return typecheck;
    if (!_compare_may_read(ctx, x, y))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_op_relation(XPOST_OP_REL_GT,
                         xpost_dict_compare_objects(ctx, x, y))));
    return 0;
}

/* any1 any2  le  bool
   test less or equal */
static
int xpost_op_any_any_le (Xpost_Context *ctx,
                         Xpost_Object x,
                         Xpost_Object y)
{
    if (!xpost_op_ordered_comparable(x, y))
        return typecheck;
    if (!_compare_may_read(ctx, x, y))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_op_relation(XPOST_OP_REL_LE,
                         xpost_dict_compare_objects(ctx, x, y))));
    return 0;
}

/* any1 any2  lt  bool
   test less than */
static
int xpost_op_any_any_lt (Xpost_Context *ctx,
                         Xpost_Object x,
                         Xpost_Object y)
{
    if (!xpost_op_ordered_comparable(x, y))
        return typecheck;
    if (!_compare_may_read(ctx, x, y))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_op_relation(XPOST_OP_REL_LT,
                         xpost_dict_compare_objects(ctx, x, y))));
    return 0;
}

/* bool1|int1 bool2|int2  and  bool3|int3
   logical|bitwise and */
static
int xpost_op_bool_bool_and (Xpost_Context *ctx,
                            Xpost_Object x,
                            Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(x.int_.val & y.int_.val));
    return 0;
}

static
int xpost_op_int_int_and (Xpost_Context *ctx,
                          Xpost_Object x,
                          Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons(x.int_.val & y.int_.val));
    return 0;
}

/* bool1|int1  not  bool2|int2
   logical|bitwise not */
static
int xpost_op_bool_not (Xpost_Context *ctx,
                       Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons( ! x.int_.val ));
    return 0;
}

static
int xpost_op_int_not (Xpost_Context *ctx,
                      Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons( ~ x.int_.val ));
    return 0;
}

/* bool1|int1 bool2|int2  or  bool3|int3
   logical|bitwise inclusive or */
static
int xpost_op_bool_bool_or (Xpost_Context *ctx,
                           Xpost_Object x,
                           Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(x.int_.val | y.int_.val));
    return 0;
}

static
int xpost_op_int_int_or (Xpost_Context *ctx,
                         Xpost_Object x,
                         Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons(x.int_.val | y.int_.val));
    return 0;
}

/* bool1|int1 bool2|int2  xor  bool3|int3
   exclusive or */
static
int xpost_op_bool_bool_xor (Xpost_Context *ctx,
                            Xpost_Object x,
                            Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(x.int_.val ^ y.int_.val));
    return 0;
}

static
int xpost_op_int_int_xor (Xpost_Context *ctx,
                          Xpost_Object x,
                          Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons(x.int_.val ^ y.int_.val));
    return 0;
}

/* true */
/* false */
/* defined as the booleantype object directly */


/* The shift below moves a bit pattern within the integer's own width, so
   the unsigned type carrying it has to be exactly that wide -- a wider
   one lets a right shift bring down bits the integer does not have, and
   a narrower one drops bits it does. dword is that type in both object
   widths; this says so rather than leaving it to hold by luck. (A
   negative array size rather than _Static_assert: this builds as C99
   with -pedantic-errors, which rejects the latter.) */
typedef char xpost_bitshift_field_is_the_integer_width[
    sizeof(dword) == sizeof(integer) ? 1 : -1];

/* int1 shift  bitshift  int2
   bitwise shift of int1 (positive is left) */
static
int xpost_op_int_int_bitshift (Xpost_Context *ctx,
                               Xpost_Object x,
                               Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons(xpost_int_bitshift(x.int_.val,
                                                       y.int_.val)));
    return 0;
}

int xpost_oper_init_bool_ops(Xpost_Context *ctx,
                             Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;
    int ret;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "eq", (Xpost_Op_Func)xpost_op_any_any_eq, 2, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "ne", (Xpost_Op_Func)xpost_op_any_any_ne, 2, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "ge", (Xpost_Op_Func)xpost_op_any_any_ge, 2, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "gt", (Xpost_Op_Func)xpost_op_any_any_gt, 2, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "le", (Xpost_Op_Func)xpost_op_any_any_le, 2, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "lt", (Xpost_Op_Func)xpost_op_any_any_lt, 2, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "and", (Xpost_Op_Func)xpost_op_bool_bool_and, 2, booleantype, booleantype);
    INSTALL;
    op = xpost_operator_cons(ctx, "and", (Xpost_Op_Func)xpost_op_int_int_and, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "not", (Xpost_Op_Func)xpost_op_bool_not, 1, booleantype);
    INSTALL;
    op = xpost_operator_cons(ctx, "not", (Xpost_Op_Func)xpost_op_int_not, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "or", (Xpost_Op_Func)xpost_op_bool_bool_or, 2, booleantype, booleantype);
    INSTALL;
    op = xpost_operator_cons(ctx, "or", (Xpost_Op_Func)xpost_op_int_int_or, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "xor", (Xpost_Op_Func)xpost_op_bool_bool_xor, 2, booleantype, booleantype);
    INSTALL;
    op = xpost_operator_cons(ctx, "xor", (Xpost_Op_Func)xpost_op_int_int_xor, 2, integertype, integertype);
    INSTALL;
    ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "true"), xpost_bool_cons(1));
    if (ret)
        return ret;
    ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "false"), xpost_bool_cons(0));
    if (ret)
        return ret;
    op = xpost_operator_cons(ctx, "bitshift", (Xpost_Op_Func)xpost_op_int_int_bitshift, 2, integertype, integertype);
    INSTALL;

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL); */

    return 0;
}
