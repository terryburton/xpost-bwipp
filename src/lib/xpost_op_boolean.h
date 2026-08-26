/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_boolean.h
 * @brief Declares the one function that installs the boolean and bitwise operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_BOOLEAN_H
#define XPOST_OP_BOOLEAN_H

/*
 * The verdict each of eq, ne, lt, le, gt and ge draws from the
 * three-way comparison of its two operands, shared between those
 * operators and the interpreter's fused procedure execution so the
 * relations exist once.
 */
typedef enum
{
    XPOST_OP_REL_EQ,
    XPOST_OP_REL_NE,
    XPOST_OP_REL_LT,
    XPOST_OP_REL_LE,
    XPOST_OP_REL_GT,
    XPOST_OP_REL_GE
} Xpost_Op_Relation;

/**
 * @brief whether a relation is one of the ordered four.
 *
 * eq and ne compare any two objects; lt, le, gt and ge order them, and
 * only a pair of numbers or a pair of strings can be ordered. Asked by
 * name rather than by where the relation sits in the enumeration, so
 * that reordering the enumeration cannot quietly change which relations
 * are restricted on one road and not the other.
 */
static inline int xpost_op_relation_is_ordered(Xpost_Op_Relation rel)
{
    switch (rel)
    {
        case XPOST_OP_REL_LT: /*@fallthrough@*/
        case XPOST_OP_REL_LE: /*@fallthrough@*/
        case XPOST_OP_REL_GT: /*@fallthrough@*/
        case XPOST_OP_REL_GE: return 1;
        default:              return 0;
    }
}

static inline int xpost_op_relation(Xpost_Op_Relation rel, int cmp)
{
    switch (rel)
    {
        case XPOST_OP_REL_EQ: return cmp == 0;
        case XPOST_OP_REL_NE: return cmp != 0;
        case XPOST_OP_REL_LT: return cmp < 0;
        case XPOST_OP_REL_LE: return cmp <= 0;
        case XPOST_OP_REL_GT: return cmp > 0;
        default:              return cmp >= 0;
    }
}

/**
 * @brief whether the ordered relations may be applied to this pair.
 *
 * lt, le, gt and ge take two numbers or two strings; anything else,
 * including one of each, is a typecheck (PLRM 8.2). eq and ne are the
 * general pair and take any two objects, so they do not ask this.
 * Shared with the interpreter's fused execution, which reaches the same
 * comparison by another road.
 */
static inline int xpost_op_ordered_comparable(Xpost_Object x, Xpost_Object y)
{
    int xt = xpost_object_get_type(x);
    int yt = xpost_object_get_type(y);
    int xnum = (xt == integertype) || (xt == realtype);
    int ynum = (yt == integertype) || (yt == realtype);

    if (xnum && ynum)
        return 1;
    return (xt == stringtype) && (yt == stringtype);
}

int xpost_oper_init_bool_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
