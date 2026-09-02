/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_dict.h
 * @brief Declares the one function that installs the dictionary operators.
 *
 * The operators themselves are in the .c beside this. Nothing here is
 * called by anything but the operator table's own set-up, which asks
 * each module in turn to install what it owns.
 */

#ifndef XPOST_OP_DICT_H
#define XPOST_OP_DICT_H

/* dictionary operators */

extern int DEBUGLOAD;

/*
 * The def fast path, shared verbatim between the def operator and the
 * interpreter's fused procedure execution so its semantics exist once.
 *
 * A name key def'd into the topmost dictionary deterministically sets
 * the name's visible binding: the cache entry can be refreshed in place
 * instead of invalidating every resolution. The fast path applies when
 * the global-holds-local rule permits the direct store (K is already a
 * name, hence simple; only V needs the check); otherwise callers fall
 * back to the general banked put, which performs the full check and
 * raises invalidaccess.
 */
static inline int
xpost_dict_def_fast_ok(Xpost_Context *ctx, Xpost_Memory_File *dmem,
                       Xpost_Object V)
{
    return !(dmem == ctx->gl &&
             xpost_object_is_banked(V) &&
             dmem != xpost_context_select_memory(ctx, V));
}

static inline int
xpost_dict_def_cached(Xpost_Context *ctx, Xpost_Memory_File *dmem,
                      Xpost_Object D, Xpost_Object K, Xpost_Object V)
{
    int ret = xpost_dict_put_memory(ctx, dmem, D, K, V);
    if (ret)
        return ret;
    {
        unsigned int key = ((unsigned int)K.mark_.padw << 1) |
            ((K.mark_.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ? 1 : 0);
        if (key < ctx->namecache_size)
        {
            ctx->namecache_gen[key] = ctx->namebind_gen;
            ctx->namecache_val[key] = V;
        }
    }
    return 0;
}

int xpost_op_any_load(Xpost_Context *ctx, Xpost_Object K);
int xpost_op_privatedict_load(Xpost_Context *ctx, Xpost_Object K);
/* copy the contents of dict S into dict D; used to give a forked context its
   own private machinery dictionary (xpost_op_context.c) */
int xpost_op_dict_copy(Xpost_Context *ctx, Xpost_Object S, Xpost_Object D);
int xpost_oper_init_dict_ops(Xpost_Context *ctx, Xpost_Object sd);

#endif
