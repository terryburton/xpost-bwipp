/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_type.h
 * @brief Declares the one function that installs the type and access operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_TYPE_H
#define XPOST_OP_TYPE_H

/*
 * The type an object reports, shared between the type operator and the
 * interpreter's fused procedure execution so the naming exists once.
 *
 * The answer is an index rather than a string because the interpreter
 * caches one executable name per index: the object types index
 * themselves, and a packed array -- stored as a read-only array but a
 * type of its own -- takes the one index beyond them. An object whose
 * type word is out of range reports the invalid type.
 */
#define XPOST_OP_TYPE_PACKEDARRAY XPOST_OBJECT_NTYPES
#define XPOST_OP_TYPE_NNAMES (XPOST_OBJECT_NTYPES + 1)

static inline unsigned int xpost_op_type_index(Xpost_Object o)
{
    Xpost_Object_Type type = xpost_object_get_type(o);

    if (type >= XPOST_OBJECT_NTYPES)
        return (unsigned int)invalidtype;
    if (type == arraytype && xpost_object_is_packed(o))
        return XPOST_OP_TYPE_PACKEDARRAY;
    return (unsigned int)type;
}

static inline const char *xpost_op_type_name(unsigned int index)
{
    return index == XPOST_OP_TYPE_PACKEDARRAY ? "packedarraytype"
                                              : xpost_object_type_names[index];
}

/**
 * @brief the type-pattern code a type name denotes, or -1.
 *
 * The names the type operator answers, plus the pattern names the
 * signature machinery understands: numbertype, proctype, anytype.
 */
int xpost_op_type_code(Xpost_Context *ctx, Xpost_Object name);

int xpost_oper_init_type_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
