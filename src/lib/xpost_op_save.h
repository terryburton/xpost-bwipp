/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_save.h
 * @brief Declares the one function that installs the save, restore and allocation-mode operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_SAVE_H
#define XPOST_OP_SAVE_H

int xpost_oper_init_save_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
