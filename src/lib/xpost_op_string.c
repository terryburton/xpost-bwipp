/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_string.c
 * @brief Installs the string operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * string copy length get put getinterval putinterval forall
 * search anchorsearch
 *
 * A string is a run of bytes in the arena; an interval shares that storage,
 * so writing through one is seen through the other.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdlib.h> /* NULL */

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_string.h"
#include "xpost_name.h"
#include "xpost_array.h"
#include "xpost_dict.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_string.h"

static
int Istring(Xpost_Context *ctx,
            Xpost_Object I)
{
    Xpost_Object str;

    if (I.int_.val < 0)
        return rangecheck;
    if (I.int_.val > (integer)XPOST_OBJECT_COMP_MAX_SZ) /* the sz field is full */
        return limitcheck;

    str = xpost_string_cons(ctx, I.int_.val, NULL);
    if (xpost_object_get_type(str) == nulltype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(str));
    return 0;
}

static
int Slength(Xpost_Context *ctx,
            Xpost_Object S)
{
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(S.comp_.sz));
    return 0;
}

static
int Nlength(Xpost_Context *ctx,
            Xpost_Object N)
{
    Xpost_Object str = xpost_name_get_string(ctx, N);
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(str.comp_.sz));
    return 0;
}

static
int s_copy(Xpost_Context *ctx,
           Xpost_Object S,
           Xpost_Object D)
{
    unsigned i;
    int ret;
    integer val;

    for (i = 0; i < S.comp_.sz; i++)
    {
        ret = xpost_string_get(ctx, S, i, &val);
        if (ret)
            return ret;
        ret = xpost_string_put(ctx, D, i, val);
        if (ret)
            return ret;
    }
    return 0;
}

static
int Scopy(Xpost_Context *ctx,
          Xpost_Object S,
          Xpost_Object D)
{
    Xpost_Object subs;
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    if (!xpost_object_is_writeable(ctx, D))
        return invalidaccess;
    if (D.comp_.sz < S.comp_.sz)
        return rangecheck;
    s_copy(ctx, S, D);
    subs = xpost_object_get_interval(D, 0, S.comp_.sz);
    if (xpost_object_get_type(subs) == invalidtype)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, subs);
    return 0;
}

static
int Sget(Xpost_Context *ctx,
         Xpost_Object S,
         Xpost_Object I)
{
    integer val;
    int ret;
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    ret = xpost_string_get(ctx, S, I.int_.val, &val);
    if (ret)
        return ret;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(val));
    return 0;
}

static
int Sput(Xpost_Context *ctx,
         Xpost_Object S,
         Xpost_Object I,
         Xpost_Object C)
{
    if (!xpost_object_is_writeable(ctx, S))
        return invalidaccess;
    /* a string's "elements must be integers in the range 0 to 255"
       (PLRM 3.3.7). A value outside that range is no element of a
       string, so it is refused rather than narrowed to the byte it
       happens to end in: narrowed, it reads back as a character the
       program never wrote and nothing downstream can tell the two
       apart. */
    if (C.int_.val < 0 || C.int_.val > 255)
        return rangecheck;
    return xpost_string_put(ctx, S, I.int_.val, C.int_.val);
}

static
int Sgetinterval(Xpost_Context *ctx,
                 Xpost_Object S,
                 Xpost_Object I,
                 Xpost_Object L)
{
    Xpost_Object subs;
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    subs = xpost_object_get_interval(S, I.int_.val, L.int_.val);
    if (xpost_object_get_type(subs) == invalidtype)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, subs);
    return 0;
}

static
int Sputinterval(Xpost_Context *ctx,
                 Xpost_Object D,
                 Xpost_Object I,
                 Xpost_Object S)
{
    Xpost_Object subs;
    if (!xpost_object_is_writeable(ctx, D))
        return invalidaccess;
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    subs = xpost_object_get_interval(D, I.int_.val, S.comp_.sz);
    if (xpost_object_get_type(subs) == invalidtype)
        return rangecheck;
    s_copy(ctx, S, subs);
    return 0;
}

static
int ancsearch(char *str,
              char *seek,
              int seekn)
{
    int i;
    for (i = 0; i < seekn; i++)
        if (str[i] != seek[i])
            return 0;
    return 1;
}

static
int Sanchorsearch(Xpost_Context *ctx,
                  Xpost_Object str,
                  Xpost_Object seek)
{
    char *s, *k;
    Xpost_Object interval;

    if (!xpost_object_is_readable(ctx, str) || !xpost_object_is_readable(ctx, seek))
        return invalidaccess;
    s = xpost_string_get_pointer(ctx, str);
    k = xpost_string_get_pointer(ctx, seek);
    if (seek.comp_.sz <= str.comp_.sz && ancsearch(s, k, seek.comp_.sz))
    {
        interval = xpost_object_get_interval(str, seek.comp_.sz, str.comp_.sz - seek.comp_.sz);
        if (xpost_object_get_type(interval) == invalidtype)
            return rangecheck;
        xpost_stack_push(ctx->lo, ctx->os, interval); /* post */
        interval = xpost_object_get_interval(str, 0, seek.comp_.sz);
        if (xpost_object_get_type(interval) == invalidtype)
            return rangecheck;
        xpost_stack_push(ctx->lo, ctx->os, interval); /* match */
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    }
    else
    {
        xpost_stack_push(ctx->lo, ctx->os, str);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    }
    return 0;
}

static
int Ssearch(Xpost_Context *ctx,
            Xpost_Object str,
            Xpost_Object seek)
{
    char *s, *k;
    Xpost_Object interval;

    if (!xpost_object_is_readable(ctx, str) || !xpost_object_is_readable(ctx, seek))
        return invalidaccess;
    if (seek.comp_.sz > str.comp_.sz)
    {
        /* needle cannot match: report not-found, per PLRM */
        xpost_stack_push(ctx->lo, ctx->os, str);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    s = xpost_string_get_pointer(ctx, str);
    k = xpost_string_get_pointer(ctx, seek);

    /* The leftmost occurrence of seek in str by Knuth-Morris-Pratt, in
       time linear in the two lengths where the plain scan took their
       product -- so a long string searched for a long one that nearly
       matches it everywhere is no longer a way to spend a second of
       uninterruptible work. An empty seek matches at the start, as the
       plain scan did. */
    {
        unsigned int n = str.comp_.sz, m = seek.comp_.sz;
        unsigned int ii, jj;
        long found = -1;

        if (m == 0)
            found = 0;
        else
        {
            int *fail = malloc((size_t)m * sizeof(int));

            if (!fail)
                return VMerror;
            fail[0] = 0;
            for (ii = 1, jj = 0; ii < m; )
            {
                if (k[ii] == k[jj]) fail[ii++] = (int)++jj;
                else if (jj) jj = (unsigned int)fail[jj - 1];
                else fail[ii++] = 0;
            }
            for (ii = 0, jj = 0; ii < n; )
            {
                if (s[ii] == k[jj])
                {
                    ii++; jj++;
                    if (jj == m) { found = (long)(ii - m); break; }
                }
                else if (jj) jj = (unsigned int)fail[jj - 1];
                else ii++;
            }
            free(fail);
        }

        if (found >= 0)
        {
            unsigned int i = (unsigned int)found;

            interval = xpost_object_get_interval(str, i + seek.comp_.sz, str.comp_.sz - seek.comp_.sz - i);
            if (xpost_object_get_type(interval) == invalidtype)
                return rangecheck;
            xpost_stack_push(ctx->lo, ctx->os, interval); /* post */
            interval = xpost_object_get_interval(str, i, seek.comp_.sz);
            if (xpost_object_get_type(interval) == invalidtype)
                return rangecheck;
            xpost_stack_push(ctx->lo, ctx->os, interval); /* match */
            interval = xpost_object_get_interval(str, 0, i);
            if (xpost_object_get_type(interval) == invalidtype)
                return rangecheck;
            xpost_stack_push(ctx->lo, ctx->os, interval); /* pre */
            xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
            return 0;
        }
    }
    xpost_stack_push(ctx->lo, ctx->os, str);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
}

static
int Sforall(Xpost_Context *ctx,
            Xpost_Object S,
            Xpost_Object P)
{
    Xpost_Object interval;
    integer val;
    int ret;

    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    if (S.comp_.sz == 0) return 0;
    assert(ctx->gl->base);
    (void)interval;
    (void)val;
    (void)ret;
    /* loop frame, as for the array forall, placed as one run */
    {
        Xpost_Object fr[4];

        fr[0] = XPOST_OP(ctx, forall);
        fr[1] = xpost_object_cvlit(P);
        fr[2] = xpost_object_cvlit(S);
        fr[3] = XPOST_OP(ctx, stringforallcont);
        if (!xpost_stack_push_run(ctx->lo, ctx->es, fr, 4))
            return execstackoverflow;
    }
    return 0;
}

/* continue a string forall: es holds (from the top) the remaining
   interval, the literal proc, and the sentinel */
static int xpost_op_string_forall_iterate(Xpost_Context *ctx)
{
    Xpost_Object S, P;
    integer val;
    int ret;
    Xpost_Stack *es_root = xpost_stack_at(ctx->lo, ctx->es);
    Xpost_Stack *es_top = xpost_stack_at(ctx->lo, es_root->prevseg);

    /* frame in the top segment, with room for the two pushes */
    if (es_top->top >= 3 && es_top->top < XPOST_STACK_SEGMENT_SIZE - 2)
    {
        Xpost_Stack *os_root = xpost_stack_at(ctx->lo, ctx->os);
        Xpost_Stack *os_top = xpost_stack_at(ctx->lo, os_root->prevseg);

        S = es_top->data[es_top->top - 1];
        P = es_top->data[es_top->top - 2];
        if (S.comp_.sz == 0)
        {
            es_top->top -= 3; /* drop the frame */
            if (es_top->top == 0 &&
                es_top != xpost_stack_at(ctx->lo, ctx->es))
                es_root->prevseg = es_top->prevseg;
            return 0;
        }
        ret = xpost_string_get(ctx, S, 0, &val);
        if (ret)
            return ret;
        if (os_top->top < XPOST_STACK_SEGMENT_SIZE - 1)
            os_top->data[os_top->top++] = xpost_int_cons(val);
        else
        {
            if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(val)))
                return stackoverflow;
            /* the push may grow the memory file and move its base:
               re-derive the frame pointers before writing through them */
            es_root = xpost_stack_at(ctx->lo, ctx->es);
            es_top = xpost_stack_at(ctx->lo, es_root->prevseg);
        }
        es_top->data[es_top->top - 1] =
            xpost_object_cvlit(xpost_object_get_interval(S, 1, S.comp_.sz - 1));
        es_top->data[es_top->top] =
            XPOST_OP(ctx, stringforallcont);
        es_top->data[es_top->top + 1] = xpost_object_cvx(P);
        es_top->top += 2;
        return 0;
    }

    S = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 0);
    P = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 1);
    if (xpost_object_get_type(S) == invalidtype)
        return execstackunderflow;
    if (S.comp_.sz == 0)
    {
        int k;
        for (k = 0; k < 3; k++)
            (void)xpost_stack_pop(ctx->lo, ctx->es);
        return 0;
    }
    ret = xpost_string_get(ctx, S, 0, &val);
    if (ret)
        return ret;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(val)))
        return stackoverflow;
    if (!xpost_stack_topdown_replace(ctx->lo, ctx->es, 0,
            xpost_object_cvlit(xpost_object_get_interval(S, 1, S.comp_.sz - 1))))
        return execstackunderflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                XPOST_OP(ctx, stringforallcont)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvx(P)))
        return execstackoverflow;
    return 0;
}

// token : see optok.c

int xpost_oper_init_string_ops (Xpost_Context *ctx,
                                Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);
    op = xpost_operator_cons(ctx, "string", (Xpost_Op_Func)Istring, 1,
                             integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "length", (Xpost_Op_Func)Slength, 1,
                             stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "length", (Xpost_Op_Func)Nlength, 1,
                             nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "copy", (Xpost_Op_Func)Scopy, 2,
                             stringtype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "get", (Xpost_Op_Func)Sget, 2,
                             stringtype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "put", (Xpost_Op_Func)Sput, 3,
                             stringtype, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "getinterval", (Xpost_Op_Func)Sgetinterval, 3,
                             stringtype, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "putinterval", (Xpost_Op_Func)Sputinterval, 3,
                             stringtype, integertype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "anchorsearch", (Xpost_Op_Func)Sanchorsearch, 2,
                             stringtype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "search", (Xpost_Op_Func)Ssearch, 2,
                             stringtype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "forall", (Xpost_Op_Func)Sforall, 2,
                             stringtype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "forall.string.iterate", (Xpost_Op_Func)xpost_op_string_forall_iterate, 0);
    return 0;
}
