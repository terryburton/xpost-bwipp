/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_packedarray.c
 * @brief Installs the packed-array operators.
 *
 * The implementations, and the one function that installs them.
 *
 * A packed array is the compact representation of an array PLRM 3.3.6
 * describes, carrying its own type and otherwise behaving as an array
 * does. What follows from that is the reason these operators exist
 * separately at all: executing one is indistinguishable from executing
 * an ordinary procedure, and the two are told apart only when they are
 * read as data. The operators themselves are PLRM 8.2.
 *
 * Installed into systemdict as:
 *
 * packedarray currentpacking setpacking
 *
 * A packed array holds the same objects in less room and is read-only by
 * construction, which is what procedures are stored as.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_dict.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_array.h"
#include "xpost_op_packedarray.h"

/* a packed array is stored as a read-only array carrying the packed flag,
   which sets it apart from a plainly read-only array for bind and type */

static
int packedarray(Xpost_Context *ctx,
                Xpost_Object n)
{
    int i;
    int ret;
    Xpost_Object a, v;

    if (n.int_.val < 0)
        return rangecheck;
    if (n.int_.val > (integer)XPOST_OBJECT_COMP_MAX_SZ) /* the sz field is full */
        return limitcheck;

    a = xpost_array_cons(ctx, n.int_.val);
    if (xpost_object_get_type(a) == nulltype)
        return VMerror;

    for (i = n.int_.val; i > 0; i--)
    {
        v = xpost_stack_pop(ctx->lo, ctx->os);
        if (xpost_object_get_type(v) == invalidtype)
            return stackunderflow;
        ret = xpost_array_put(ctx, a, i-1, v);
        if (ret)
            return ret;
    }
    a = xpost_object_set_packed(
            xpost_object_set_access(ctx, xpost_object_cvlit(a),
                XPOST_OBJECT_TAG_ACCESS_READ_ONLY));
    xpost_stack_push(ctx->lo, ctx->os, a);
    return 0;
}

static
int setpacking(Xpost_Context *ctx,
               Xpost_Object b)
{
    ctx->packing = (b.int_.val != 0);
    return 0;
}

static
int currentpacking(Xpost_Context *ctx)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(ctx->packing));
    return 0;
}

int xpost_oper_init_packedarray_ops(Xpost_Context *ctx,
                                    Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "packedarray", (Xpost_Op_Func)packedarray, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "setpacking", (Xpost_Op_Func)setpacking, 1, booleantype);
    INSTALL;
    op = xpost_operator_cons(ctx, "currentpacking", (Xpost_Op_Func)currentpacking, 0);
    INSTALL;

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
    xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "mark"), mark); */

    return 0;
}
