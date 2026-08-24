/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_OP_MATH_H
#define XPOST_OP_MATH_H

int xpost_oper_init_math_ops(Xpost_Context *ctx, Xpost_Object sd);

/*
 * Integer arithmetic with PLRM 3.3.2 range semantics, shared verbatim
 * between the add/sub/mul operators and the interpreter's fused
 * procedure execution so the two can never disagree: a result outside
 * the PostScript integer range becomes a real of the true value rather
 * than wrapping.
 *
 * The bounds are those of the `integer` type, so a build with a wider
 * integer widens them with it.
 */
#define XPOST_INTEGER_MAX \
    ((long long)(((unsigned long long)1 << (sizeof(integer)*8 - 1)) - 1))
#define XPOST_INTEGER_MIN (-XPOST_INTEGER_MAX - 1)

/* The operands arrive in the integer's own width, which is the width the
   bounds above are drawn in. `long` is that width on some platforms and
   half of it on others, so an operand taken as a `long` would be a
   different operand on each. */
typedef char xpost_integer_range_spans_the_integer[
    sizeof(long long) >= sizeof(integer)
    && sizeof(dword) == sizeof(integer) ? 1 : -1];

static inline int xpost_int_add_willover(integer x, integer y)
{
    if (y < 0) return x < XPOST_INTEGER_MIN - y;
    return x > XPOST_INTEGER_MAX - y;
}

static inline int xpost_int_sub_willunder(integer x, integer y)
{
    if (y < 0) return x > XPOST_INTEGER_MAX + y;
    return x < XPOST_INTEGER_MIN + y;
}

static inline int xpost_int_mul_willover(integer x, integer y)
{
    /* the magnitudes are held unsigned, so the most negative operand --
       which has no positive counterpart -- is measured without leaving the
       field, and the division that follows is over two magnitudes */
    dword xx = x < 0 ? (dword)0 - (dword)x : (dword)x;
    dword yy = y < 0 ? (dword)0 - (dword)y : (dword)y;
    if (xx == 0 || yy == 0) return 0;
    return xx > (dword)XPOST_INTEGER_MAX / yy;
}

/**
 * @brief a count, as an integer where the integer type holds it and a
 * real where it does not.
 *
 * The same answer the arithmetic below gives a result that leaves the
 * integer range, for a quantity that is counted rather than computed.
 * A real loses the last digits of a very large count and says how large
 * it is; an integer that has wrapped says the count is small, and says
 * so exactly where the quantity is largest.
 */
static inline Xpost_Object xpost_count_cons(unsigned long long n)
{
    if (n > (unsigned long long)XPOST_INTEGER_MAX)
        return xpost_real_cons((real)n);
    return xpost_int_cons((integer)n);
}

/**
 * @brief the sum/difference/product of two integer objects, as the
 * object the PLRM prescribes: an integer, or a real when the exact
 * result leaves the integer range.
 */
static inline Xpost_Object xpost_int_add(integer x, integer y)
{
    return xpost_int_add_willover(x, y) ? xpost_real_cons((real)x + y)
                                        : xpost_int_cons(x + y);
}

static inline Xpost_Object xpost_int_sub(integer x, integer y)
{
    return xpost_int_sub_willunder(x, y) ? xpost_real_cons((real)x - y)
                                         : xpost_int_cons(x - y);
}

static inline Xpost_Object xpost_int_mul(integer x, integer y)
{
    return xpost_int_mul_willover(x, y) ? xpost_real_cons((real)x * y)
                                        : xpost_int_cons(x * y);
}

#endif
