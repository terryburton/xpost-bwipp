/*
 * Xpost - a Level-2 Postscript interpreter
 * Copyright (C) 2013-2016, Michael Joshua Ryan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Xpost software product nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
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
             xpost_object_is_composite(V) &&
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
