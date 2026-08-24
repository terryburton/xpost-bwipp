/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_OP_CONTEXT_H
#define XPOST_OP_CONTEXT_H

/* DPS context operators */

int xpost_oper_init_context_ops(Xpost_Context *ctx, Xpost_Object sd);

/* nonzero when the Display PostScript context operators were installed
   (--enable-dps / xpost_dps_set); read by the mainloop's context switcher */
int xpost_dps_enabled(void);

#endif
