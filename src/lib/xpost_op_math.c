/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_math.c
 * @brief Installs the arithmetic operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * add sub mul div idiv mod neg abs ceiling floor round truncate sqrt
 * atan sin cos exp ln log rand srand rrand
 *
 * Integer arithmetic that overflows becomes real, which PLRM requires and
 * which is why several of these are not simply the C operator.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define _USE_MATH_DEFINES /* needed for M_PI with Visual Studio */
#include <assert.h>
#include <limits.h>
#include <math.h>

//#define PI (4.0 * atan(1.0))
/* the full-precision conversion: a truncated literal (0.0174533) skewed atan,
   sin and cos off the PLRM examples (1 0 atan gave 89.99996, not 90.0) */
#define RAD_PER_DEG (M_PI / 180.0)

#include "xpost.h"
#include "xpost_compat.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_dict.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_math.h"

/* the integer range predicates and the range-preserving add/sub/mul
   live in xpost_op_math.h, shared with the interpreter's fused path */

/* num1 num2  add  sum
   num1 plus num2 */
static
int Iadd(Xpost_Context *ctx,
         Xpost_Object x,
         Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_add(x.int_.val, y.int_.val));
    return 0;
}

static
int Radd(Xpost_Context *ctx,
         Xpost_Object x,
         Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x.real_.val + y.real_.val));
    return 0;
}

/* num1 num2  div  quotient
   num1 divided by num2 */
static
int Rdiv(Xpost_Context *ctx,
         Xpost_Object x,
         Xpost_Object y)
{
    if (y.real_.val == 0.0)
        return undefinedresult;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x.real_.val / y.real_.val));
    return 0;
}

/* num1 num2  idiv  quotient
   integer divide */
static
int Iidiv(Xpost_Context *ctx,
          Xpost_Object x,
          Xpost_Object y)
{
    if (y.int_.val == 0)
        return undefinedresult;
    /* INT_MIN / -1 overflows a two's-complement integer (a hardware fault on
       some platforms); it exceeds the integer range, so yield the real quotient
       the way an overflowing product does */
    if (y.int_.val == -1 && x.int_.val == (-2147483647 - 1))
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(-(real)x.int_.val));
        return 0;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(x.int_.val / y.int_.val));
    return 0;
}

/* num1 num2  mod  remainder
   num1 mod num2 */
static
int Imod(Xpost_Context *ctx,
         Xpost_Object x,
         Xpost_Object y)
{
    /* a zero divisor is undefinedresult (PLRM), not a hardware divide fault;
       guard the INT_MIN % -1 overflow the same way idiv's quotient is guarded */
    if (y.int_.val == 0)
        return undefinedresult;
    if (y.int_.val == -1)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(0));
        return 0;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(x.int_.val % y.int_.val));
    return 0;
}

/* num1 num2  mul  product
   num1 times num2 */
static
int Imul(Xpost_Context *ctx,
         Xpost_Object x,
         Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_mul(x.int_.val, y.int_.val));
    return 0;
}

static
int Rmul(Xpost_Context *ctx,
         Xpost_Object x,
         Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x.real_.val * y.real_.val));
    return 0;
}

/* num1 num2  sub  difference
   num1 minus num2 */
static
int Isub(Xpost_Context *ctx,
         Xpost_Object x,
         Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_sub(x.int_.val, y.int_.val));
    return 0;
}

static
int Rsub(Xpost_Context *ctx,
         Xpost_Object x,
         Xpost_Object y)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x.real_.val - y.real_.val));
    return 0;
}

/* num1  abs  num2
   absolute value of num1 */
static
int Iabs(Xpost_Context *ctx,
         Xpost_Object x)
{
    if (x.int_.val == INT_MIN)
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(- (real)INT_MIN));
    else
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(x.int_.val>0? x.int_.val: -x.int_.val));
    return 0;
}

static
int Rabs(Xpost_Context *ctx,
         Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)fabs(x.real_.val)));
    return 0;
}

/* num1  neg  num2
   negative of num1 */
static
int Ineg(Xpost_Context *ctx,
         Xpost_Object x)
{
    if (x.int_.val == INT_MIN)
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(- (real)INT_MIN));
    else
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(-x.int_.val));
    return 0;
}

static
int Rneg(Xpost_Context *ctx,
         Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(-x.real_.val));
    return 0;
}

/* stub for integer  floor, ceiling, round, truncate */
static
int Istet(Xpost_Context *ctx,
          Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os, x);
    return 0;
}

/* num1  ceiling  num2
   ceiling of num1 */
static
int Rceiling(Xpost_Context *ctx,
             Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)ceil(x.real_.val)));
    return 0;
}

/* num1  floor  num2
   floor of num1 */
static
int Rfloor(Xpost_Context *ctx,
           Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)floor(x.real_.val)));
    return 0;
}

/* num1  round  num2
   round num1 to nearest integer */
static
int Rround(Xpost_Context *ctx,
           Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)floor(x.real_.val + 0.5)));
    return 0;
}

/* num1  truncate  num2
   remove fractional part of num1 */
static
int Rtruncate(Xpost_Context *ctx,
              Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)trunc(x.real_.val)));
    return 0;
}

/* num1  sqrt  num2
   square root of num1 */
static
int Rsqrt (Xpost_Context *ctx,
            Xpost_Object x)
{
    /* PLRM: the operand must be nonnegative; a negative one is rangecheck,
       not a silent NaN */
    if (x.real_.val < 0.0)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)sqrt(x.real_.val)));
    return 0;
}

/* num den  atan  angle
   arctangent of num/den in degrees */
static
int Ratan(Xpost_Context *ctx,
          Xpost_Object num,
          Xpost_Object den)
{
    double ang;

    /* PLRM: either operand may be zero, but not both -- the ratio the
       angle is taken of has no value when neither names a direction */
    if (num.real_.val == 0.0 && den.real_.val == 0.0)
        return undefinedresult;

    ang = atan2(((double)num.real_.val * RAD_PER_DEG),
                ((double)den.real_.val * RAD_PER_DEG))
          / RAD_PER_DEG;

    if (ang < 0.0) {
        double t;
        t = ang + 360.0;
        ang = t;
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)ang));
    return 0;
}

/* angle  cos  real
   cosine of angle (degrees) */
static
int Rcos(Xpost_Context *ctx,
         Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_real_cons((real)cos(RAD_PER_DEG * x.real_.val)));
    return 0;
}

/* angle  sin  real
   sine of angle (degrees) */
static
int Rsin(Xpost_Context *ctx,
         Xpost_Object x)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_real_cons((real)sin(RAD_PER_DEG * x.real_.val)));
    return 0;
}

/* base exponent  exp  real
   raise base to exponent power */
static
int Rexp(Xpost_Context *ctx,
         Xpost_Object base,
         Xpost_Object expn)
{
    double r;

    /* PLRM 8.2: with a fractional exponent the result is meaningful only
       for a nonnegative base, so a negative base and a non-integer
       exponent is undefinedresult. A negative base with an integer
       exponent is well defined and kept. */
    if (base.real_.val < 0 &&
        expn.real_.val != (real)trunc(expn.real_.val))
        return undefinedresult;
    r = pow(base.real_.val, expn.real_.val);
    if (!isfinite(r))
        return undefinedresult;
    /* and it has to still be finite once narrowed to the real a stack
       carries, which is the shorter type in the default build: a result
       pow answers finitely can overflow that on the way to the stack.
       A separate test rather than a second term, because in the build
       whose real is already the wider type the two say the same thing */
    if (!isfinite((double)(real)r))
        return undefinedresult;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)r));
    return 0;
}

/* num  ln  real
   natural logarithm of num */
static
int Rln(Xpost_Context *ctx,
        Xpost_Object x)
{
    /* PLRM 8.2: the logarithm's domain is the positive reals; a
       non-positive operand is out of range */
    if (x.real_.val <= 0)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)log(x.real_.val)));
    return 0;
}

/* num  log  real
   logarithm (base 10) */
static
int Rlog(Xpost_Context *ctx,
         Xpost_Object x)
{
    if (x.real_.val <= 0)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)log10(x.real_.val)));
    return 0;
}

/* -  rand  int
   generate pseudo-random integer.

   The answer is assembled from the low sixteen bits of two successive
   states and is held to 0 through 2^31-1, which is the range PLRM 8.2
   states whatever the integer's width. The state itself advances in its
   own field: a field of any width carries the same low bits forward, so
   the sequence this returns is the one sequence at every width. */
static
int Zrand(Xpost_Context *ctx)
{
    unsigned x;
    ctx->rand_next = ctx->rand_next * 1103515245 + 12345;
    x = (unsigned)ctx->rand_next << 16;
    ctx->rand_next = ctx->rand_next * 1103515245 + 12345;
    x |= (unsigned)ctx->rand_next & 0xffff;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(x & 0x7fffffff)))
        return stackoverflow;
    return 0;
}

/* int  srand  -
   set random number seed */
static
int Isrand(Xpost_Context *ctx,
           Xpost_Object seed)
{
    ctx->rand_next = (dword)seed.int_.val;
    return 0;
}

/* -  rrand  int
   return random number seed.

   The state is the integer of the same twos-complement representation, so
   the seed srand was given is the integer this answers with. */
static
int Zrrand(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((integer)ctx->rand_next)))
        return stackoverflow;
    return 0;
}

int xpost_oper_init_math_ops (Xpost_Context *ctx,
                              Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "add", (Xpost_Op_Func)Iadd, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "add", (Xpost_Op_Func)Radd, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "sub", (Xpost_Op_Func)Isub, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "sub", (Xpost_Op_Func)Rsub, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "mul", (Xpost_Op_Func)Imul, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "mul", (Xpost_Op_Func)Rmul, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "idiv", (Xpost_Op_Func)Iidiv, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "div", (Xpost_Op_Func)Rdiv, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "mod", (Xpost_Op_Func)Imod, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "abs", (Xpost_Op_Func)Iabs, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "abs", (Xpost_Op_Func)Rabs, 1, realtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "neg", (Xpost_Op_Func)Ineg, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "neg", (Xpost_Op_Func)Rneg, 1, realtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "floor", (Xpost_Op_Func)Istet, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "floor", (Xpost_Op_Func)Rfloor, 1, realtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "ceiling", (Xpost_Op_Func)Istet, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "ceiling", (Xpost_Op_Func)Rceiling, 1, realtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "round", (Xpost_Op_Func)Istet, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "round", (Xpost_Op_Func)Rround, 1, realtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "truncate", (Xpost_Op_Func)Istet, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "truncate", (Xpost_Op_Func)Rtruncate, 1, realtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "sqrt", (Xpost_Op_Func)Rsqrt, 1, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "atan", (Xpost_Op_Func)Ratan, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cos", (Xpost_Op_Func)Rcos, 1, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "sin", (Xpost_Op_Func)Rsin, 1, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "exp", (Xpost_Op_Func)Rexp, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "ln", (Xpost_Op_Func)Rln, 1, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "log", (Xpost_Op_Func)Rlog, 1, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "rand", (Xpost_Op_Func)Zrand, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "srand", (Xpost_Op_Func)Isrand, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "rrand", (Xpost_Op_Func)Zrrand, 0);
    INSTALL;

    /* op = xpost_operator_cons(ctx, "eq", (Xpost_Op_Func)Aeq, 1, 2, anytype, anytype);
       INSTALL;
    xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "mark"), mark); */
    return 0;
}
