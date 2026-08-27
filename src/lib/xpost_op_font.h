/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_font.h
 * @brief Declares the one function that installs the font and text-showing operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_FONT_H
#define XPOST_OP_FONT_H

int xpost_oper_init_font_ops(Xpost_Context *ctx, Xpost_Object sd);

/* Drops the bands the text route last read a clip region into. The memo
   is filed under a region serial minted in xpost_dev_generic.c, so the
   restart of that counter is what calls this: a number handed out again
   would otherwise match an entry built from a region long gone. */
void xpost_op_font_clip_memo_drop(void);


#endif
