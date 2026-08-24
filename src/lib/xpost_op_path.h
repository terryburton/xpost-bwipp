/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_OP_PATH_H
#define XPOST_OP_PATH_H

/* marks the break between one subpath and the next in a run of path
   vertices; carried in both coordinates of the break */
#define XPOST_PATH_BREAK ((real)-0x7ffffff)

int _currentpoint(Xpost_Context *ctx);
int xpost_path_fill_points(Xpost_Context *ctx, Xpost_Object path,
                           real **out, int *nout);
int xpost_oper_init_path_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
