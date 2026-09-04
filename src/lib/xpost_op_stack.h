/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_stack.h
 * @brief Declares the one function that installs the operand-stack operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_STACK_H
#define XPOST_OP_STACK_H

/* stack operators */

/*
 * The index and roll rules, shared between the operators and the
 * interpreter's fused procedure execution so each exists once.
 */

/*
 * index counts down from the top, 0 selecting the topmost operand. A
 * negative count is a rangecheck; one that reaches past the operands
 * held is a stackunderflow. The fused walker passes the count of
 * operands it can reach in one stack segment, so a selection beyond
 * that leaves the fast path for the operator, which counts them all.
 */
static inline int xpost_op_index_check(integer n, int count)
{
    if (n < 0)
        return rangecheck;
    if (n >= count)
        return stackunderflow;
    return 0;
}

/*
 * roll takes its shift modulo the count of operands rolled, and a
 * negative shift rolls the other way, so the shift always lands in
 * [0, n).
 */
static inline integer xpost_op_roll_shift(integer n, integer j)
{
    j %= n;
    if (j < 0)
        j += n;
    return j;
}

/*
 * After the roll, the operand at position i counting down from the top
 * of the rolled group is the one that was at position (i + j).
 */
static inline integer xpost_op_roll_source(integer i, integer n, integer j)
{
    return (i + j) % n;
}

int xpost_op_cleartomark(Xpost_Context *ctx);
int xpost_op_counttomark(Xpost_Context *ctx);

int xpost_oper_init_stack_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
