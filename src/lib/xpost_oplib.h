/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_OPLIB_H
#define XPOST_OPLIB_H

/**
 * @file xpost_oplib.h
 * @brief install the library of operators in systemdict
 *
 * @{
 */

/**
 * @brief do-nothing function useful as a break target for debugging
 */
int xpost_op_breakhere(Xpost_Context *ctx);

/**
 * @brief initialize the operator library
 */
int xpost_oplib_init_ops(Xpost_Context *ctx);

/**
 * @}
 */

#endif
