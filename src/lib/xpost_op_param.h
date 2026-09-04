/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_param.h
 * @brief Declares the one function that installs the parameter and virtual-memory operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_PARAM_H
#define XPOST_OP_PARAM_H

int xpost_oper_init_param_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
