/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_OP_ARRAY_H
#define XPOST_OP_ARRAY_H

/* array operators */

/*
 * The array get and put cores, shared between the operators and the
 * interpreter's fused procedure execution so the checks each element
 * access performs exist once.
 *
 * An array must be readable to be read and writeable to be written
 * (PLRM 3.3.2), and the index must name an element of it; an index
 * outside the array is a rangecheck.
 */
static inline int
xpost_op_array_get_checked(Xpost_Context *ctx, Xpost_Object A, integer i,
                           Xpost_Object *out)
{
    Xpost_Object t;

    if (!xpost_object_is_readable(ctx, A))
        return invalidaccess;
    if (i < 0)
        return rangecheck;
    t = xpost_array_get(ctx, A, i);
    if (xpost_object_get_type(t) == invalidtype)
        return rangecheck;
    *out = t;
    return 0;
}

static inline int
xpost_op_array_put_checked(Xpost_Context *ctx, Xpost_Object A, integer i,
                           Xpost_Object O)
{
    if (!xpost_object_is_writeable(ctx, A))
        return invalidaccess;
    if (i < 0)
        return rangecheck;
    return xpost_array_put(ctx, A, i, O);
}

int xpost_op_array_to_mark(Xpost_Context *ctx);
int xpost_oper_init_array_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
