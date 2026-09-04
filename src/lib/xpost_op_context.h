/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_context.h
 * @brief Declares the one function that installs the context operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_CONTEXT_H
#define XPOST_OP_CONTEXT_H

/* DPS context operators */

int xpost_oper_init_context_ops(Xpost_Context *ctx, Xpost_Object sd);

/* nonzero when the Display PostScript context operators were installed
   (--enable-dps / xpost_dps_set); read by the mainloop's context switcher */
int xpost_dps_enabled(void);

/* nonzero when this context was started by fork, which is what says the
   marker at the bottom of its execution stack is the end of the context
   and not the end of the run */
int xpost_dps_context_is_forked(Xpost_Context *ctx);

#endif
