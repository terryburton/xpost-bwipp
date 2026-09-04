/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_control.h
 * @brief Declares the one function that installs the control operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_CONTROL_H
#define XPOST_OP_CONTROL_H

/*
 * The access rule for scheduling an object for execution, shared
 * between the operators that schedule one and the interpreter's fused
 * procedure execution so the rule exists once.
 *
 * Executing an array or a string reads its contents, which an object
 * with no access forbids (PLRM 3.3.2); a dictionary is only pushed onto
 * the dictionary stack, so it is exempt. A file is read a token at a
 * time as it runs rather than scheduled whole, so its own rule is asked
 * where that reading happens, in the interpreter's evalfile.
 *
 * The two types the rule applies to carry their access in the tag of
 * the object in hand, so the field is read there. That is what the
 * general accessor returns for them; going through it instead would
 * put a call on the path every procedure call takes, to reach the
 * indirection only a dictionary or a file needs.
 *
 * The test on the type is therefore load-bearing for the answer and not
 * only for the cost, which is worth saying because the shape invites
 * widening it for a new type or for symmetry. Files do arrive here --
 * exec takes anytype, and running one is how a program executes a file
 * -- and they are turned away by the left of the conjunction before the
 * tag is ever read. That order is what keeps the answer right: an
 * execute-only file and a no-access file hold the SAME access field,
 * and are told apart only by a flag outside the mask read here, so the
 * field alone calls an execute-only file no-access -- the one state that
 * must still run. A file's rule lives in evalfile because a file's
 * access is a set of capabilities rather than a rung, and only the
 * accessor's own indirection folds that flag in.
 */
static inline int xpost_op_exec_access_ok(Xpost_Context *ctx, Xpost_Object O)
{
    Xpost_Object_Type type = xpost_object_get_type(O);

    (void)ctx;
    return !((type == arraytype || type == stringtype)
             && (O.tag & XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK)
                == (XPOST_OBJECT_TAG_ACCESS_NONE
                    << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET));
}

/* terminate the innermost stopped context; with none, report and quit */
int xpost_op_stop(Xpost_Context *ctx);

/* record what ended the run for the embedding caller, from $error */
void xpost_op_record_run_error(Xpost_Context *ctx);

int xpost_oper_init_control_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
