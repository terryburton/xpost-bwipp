/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_misc.h
 * @brief Declares the one function that installs the operators that belong to no other group.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_MISC_H
#define XPOST_OP_MISC_H

int xpost_oper_init_misc_ops(Xpost_Context *ctx, Xpost_Object sd);

/* The census of what the interpreter reaches and the seal's walk does
   not. Called from the interpreter's safe point only. */
int xpost_vm_blind_measure(Xpost_Context *ctx);

#endif
