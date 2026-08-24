/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_save.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_dict.h"
#include "xpost_name.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_stack.h"
#include "xpost_bytes.h"
#include "xpost_op_token.h" /* xpost_scanner_rep_number */
#include "xpost_op_array.h"


/* helper function */
static
int _xpost_op_array_copy_aux (Xpost_Context *ctx,
                              Xpost_Object S,
                              Xpost_Object D)
{
    unsigned i;
    Xpost_Object t;
    int ret;

    for (i = 0; i < S.comp_.sz; i++)
    {
        t = xpost_array_get(ctx, S, i);
        if (xpost_object_get_type(t) == invalidtype)
            return rangecheck;
        ret = xpost_array_put(ctx, D, i, t);
        if (ret)
            return ret;
    }

    return 0;
}

/* int  array  array
   create array of length int */
static
int xpost_op_int_array (Xpost_Context *ctx,
                        Xpost_Object I)
{
    Xpost_Object t;

    if (I.int_.val < 0)
        return rangecheck;
    if (I.int_.val > (integer)XPOST_OBJECT_COMP_MAX_SZ) /* the sz field is full */
        return limitcheck;

    t = xpost_array_cons(ctx, I.int_.val);
    if (xpost_object_get_type(t) == nulltype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(t));

    return 0;
}

/* -  [  mark
   start array construction */
/* [ is defined in systemdict as a marktype object */

/* mark obj0..objN-1  ]  array
   end array construction */
int xpost_op_array_to_mark (Xpost_Context *ctx)
{
    integer i; /* the counted length, in the width the count arrives in */
    Xpost_Object a, v;
    Xpost_Object t;

    if (xpost_op_counttomark(ctx))
        return unmatchedmark;
    t = xpost_stack_pop(ctx->lo, ctx->os);
    if (xpost_object_get_type(t) == invalidtype)
        return stackunderflow;
    i = t.int_.val;
    if (i > (integer)XPOST_OBJECT_COMP_MAX_SZ) /* the sz field is full, as the array operator enforces:
                      raise limitcheck rather than let array_cons truncate the
                      length and then fault putting the discarded elements */
        return limitcheck;
    a = xpost_array_cons(ctx, i);
    if (xpost_object_get_type(a) == nulltype)
        return VMerror;
    for ( ; i > 0; i--)
    {
        v = xpost_stack_pop(ctx->lo, ctx->os);
        if (xpost_object_get_type(v) == invalidtype)
            return stackunderflow;
        {
            /* propagate the VM check: a global array (or procedure, built
               through this same path) may not hold a local element, matching
               the put operators -- array_put raised it but the result was
               discarded, silently dropping the element */
            int ret = xpost_array_put(ctx, a, i-1, v);
            if (ret)
                return ret;
        }
    }
    (void)xpost_stack_pop(ctx->lo, ctx->os); // pop mark
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(a));

    return 0;
}

/* array  length  int
   number of elements in array */
static
int xpost_op_array_length (Xpost_Context *ctx,
                           Xpost_Object A)
{
    if (!xpost_object_is_readable(ctx, A))
        return invalidaccess;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(A.comp_.sz)))
        return stackoverflow;

    return 0;
}

/* array index  get  any
   get array element indexed by index */
static
int xpost_op_array_int_get (Xpost_Context *ctx,
                            Xpost_Object A,
                            Xpost_Object I)
{
    Xpost_Object t;
    int ret = xpost_op_array_get_checked(ctx, A, I.int_.val, &t);

    if (ret)
        return ret;
    if (!xpost_stack_push(ctx->lo, ctx->os, t))
        return stackoverflow;
    return 0;
}

/* array index any  put  -
   put any into array at index */
static
int xpost_op_array_int_any_put(Xpost_Context *ctx,
                               Xpost_Object A,
                               Xpost_Object I,
                               Xpost_Object O)
{
    return xpost_op_array_put_checked(ctx, A, I.int_.val, O);
}

/* array index count  getinterval  subarray
   subarray of array starting at index for count elements */
static
int xpost_op_array_int_int_getinterval (Xpost_Context *ctx,
                                        Xpost_Object A,
                                        Xpost_Object I,
                                        Xpost_Object L)
{
    Xpost_Object subarr;
    if (!xpost_object_is_readable(ctx, A))
        return invalidaccess;
    if (I.int_.val < 0)
        return rangecheck;
    subarr = xpost_object_get_interval(A, I.int_.val, L.int_.val);
    if (xpost_object_get_type(subarr) == invalidtype)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, subarr);
    return 0;
}

/* array1 index array2  putinterval  -
   replace subarray of array1 starting at index by array2 */
static
int xpost_op_array_int_array_putinterval (Xpost_Context *ctx,
                                          Xpost_Object D,
                                          Xpost_Object I,
                                          Xpost_Object S)
{
    Xpost_Object subarr;
    if (!xpost_object_is_writeable(ctx, D))
        return invalidaccess;
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    if (I.int_.val < 0)
        return rangecheck;
    if (I.int_.val + S.comp_.sz > D.comp_.sz)
        return rangecheck;
    subarr = xpost_object_get_interval(D, I.int_.val, S.comp_.sz);
    if (xpost_object_get_type(subarr) == invalidtype)
        return rangecheck;
    return _xpost_op_array_copy_aux(ctx, S, subarr);
}

/* array  aload  a0..aN-1 array
   push all elements of array on stack */
static
int xpost_op_array_aload (Xpost_Context *ctx,
                          Xpost_Object A)
{
    word i;

    if (!xpost_object_is_readable(ctx, A))
        return invalidaccess;
    for (i = 0; i < A.comp_.sz; i++)
        if (!xpost_stack_push(ctx->lo, ctx->os, xpost_array_get(ctx, A, i)))
            return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os, A))
        return stackoverflow;
    return 0;
}

/* any0..anyN-1 array  astore  array
   pop elements from stack into array */
static
int xpost_op_anyn_array_astore (Xpost_Context *ctx,
                                Xpost_Object A)
{
    Xpost_Object t;
    int i;
    unsigned int cnt;
    int ret;

    if (!xpost_object_is_writeable(ctx, A))
        return invalidaccess;
    cnt = xpost_stack_count(ctx->lo, ctx->os);
    if (cnt < A.comp_.sz)
        return stackunderflow;
    for (i = A.comp_.sz - 1; i >= 0; i--)
    {
        t = xpost_stack_pop(ctx->lo, ctx->os);
        //if (xpost_object_get_type(t) == invalidtype)
        ret = xpost_array_put(ctx, A, i, t);
        if (ret)
            return ret;
    }
    xpost_stack_push(ctx->lo, ctx->os, A);
    return 0;
}

/* array1 array2  copy  subarray2
   copy elements of array1 to initial subarray of array2 */
static
int xpost_op_array_copy (Xpost_Context *ctx,
                         Xpost_Object S,
                         Xpost_Object D)
{
    Xpost_Object subarr;
    int ret;
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    if (!xpost_object_is_writeable(ctx, D))
        return invalidaccess;
    if (D.comp_.sz < S.comp_.sz)
        return rangecheck;
    ret = _xpost_op_array_copy_aux(ctx, S, D);
    if (ret)
        return ret;
    subarr = xpost_object_get_interval(D, 0, S.comp_.sz);
    if (xpost_object_get_type(subarr) == invalidtype)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, subarr);
    return 0;
}

/* array proc  forall  -
   execute proc for each element of array */
static
int xpost_op_array_proc_forall(Xpost_Context *ctx,
                               Xpost_Object A,
                               Xpost_Object P)
{
    Xpost_Object interval;
    Xpost_Object element;
    if (!xpost_object_is_readable(ctx, A))
        return invalidaccess;
    if (A.comp_.sz == 0)
        return 0;

    assert(ctx->gl->base);
    (void)interval;
    (void)element;
    /* loop frame: the sentinel forall operator (which exit searches
       for) under literal state that the iterate operator consumes,
       placed as one run */
    {
        Xpost_Object fr[4];

        fr[0] = XPOST_OP(ctx, forall);
        fr[1] = xpost_object_cvlit(P);
        fr[2] = xpost_object_cvlit(A);
        fr[3] = XPOST_OP(ctx, arrayforallcont);
        if (!xpost_stack_push_run(ctx->lo, ctx->es, fr, 4))
            return execstackoverflow;
    }
    return 0;
}

/* continue an array forall: es holds (from the top) the remaining
   interval, the literal proc, and the sentinel */
static int xpost_op_array_forall_iterate(Xpost_Context *ctx)
{
    Xpost_Object A, P, element;
    Xpost_Stack *es_root = xpost_stack_at(ctx->lo, ctx->es);
    Xpost_Stack *es_top = xpost_stack_at(ctx->lo, es_root->prevseg);

    /* frame in the top segment, with room for the two pushes */
    if (es_top->top >= 3 && es_top->top < XPOST_STACK_SEGMENT_SIZE - 2)
    {
        Xpost_Stack *os_root = xpost_stack_at(ctx->lo, ctx->os);
        Xpost_Stack *os_top = xpost_stack_at(ctx->lo, os_root->prevseg);

        A = es_top->data[es_top->top - 1];
        P = es_top->data[es_top->top - 2];
        if (A.comp_.sz == 0)
        {
            es_top->top -= 3; /* drop the frame */
            if (es_top->top == 0 &&
                es_top != xpost_stack_at(ctx->lo, ctx->es))
                es_root->prevseg = es_top->prevseg;
            return 0;
        }
        element = xpost_array_get(ctx, A, 0);
        if (xpost_object_get_type(element) == invalidtype)
            return rangecheck;
        if (os_top->top < XPOST_STACK_SEGMENT_SIZE - 1)
            os_top->data[os_top->top++] = element;
        else
        {
            if (!xpost_stack_push(ctx->lo, ctx->os, element))
                return stackoverflow;
            /* the push may grow the memory file and move its base:
               re-derive the frame pointers before writing through them */
            es_root = xpost_stack_at(ctx->lo, ctx->es);
            es_top = xpost_stack_at(ctx->lo, es_root->prevseg);
        }
        es_top->data[es_top->top - 1] =
            xpost_object_cvlit(xpost_object_get_interval(A, 1, A.comp_.sz - 1));
        es_top->data[es_top->top] =
            XPOST_OP(ctx, arrayforallcont);
        es_top->data[es_top->top + 1] = xpost_object_cvx(P);
        es_top->top += 2;
        return 0;
    }

    A = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 0);
    P = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 1);
    if (xpost_object_get_type(A) == invalidtype)
        return execstackunderflow;
    if (A.comp_.sz == 0)
    {
        int k;
        for (k = 0; k < 3; k++)
            (void)xpost_stack_pop(ctx->lo, ctx->es);
        return 0;
    }
    element = xpost_array_get(ctx, A, 0);
    if (xpost_object_get_type(element) == invalidtype)
        return rangecheck;
    if (!xpost_stack_push(ctx->lo, ctx->os, element))
        return stackoverflow;
    if (!xpost_stack_topdown_replace(ctx->lo, ctx->es, 0,
            xpost_object_cvlit(xpost_object_get_interval(A, 1, A.comp_.sz - 1))))
        return execstackunderflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                XPOST_OP(ctx, arrayforallcont)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvx(P)))
        return execstackoverflow;
    return 0;
}

/* numstring  .numstring2array  array
   decode an encoded number string (PLRM 3.14.5) into a fresh array:
   byte 0 is 149; byte 1 selects the representation R, where 0..31 is
   32-bit fixed point with scale R, 32..47 is 16-bit fixed point with
   scale R-32, 48 is a 32-bit IEEE real, and adding 128 stores the
   multibyte values low-order byte first; bytes 2..3 carry the count
   in the same byte order. Scale zero yields integers, any other
   representation reals. Any other shape is a rangecheck. */
static
int _numstring2array (Xpost_Context *ctx,
                      Xpost_Object str)
{
    unsigned char *p;
    unsigned int sz, n, i, width, rep;
    int le, r;
    Xpost_Object arr;

    sz = str.comp_.sz;
    if (sz < 4)
        return rangecheck;
    p = (unsigned char *)xpost_string_get_pointer(ctx, str);
    if (p[0] != 149)
        return rangecheck;
    rep = p[1];
    r = (int)rep;
    le = r >= 128;
    if (le)
        r -= 128;
    if (r > 48)
        return rangecheck;
    n = le ? xpost_bytes_le16(p + 2) : xpost_bytes_be16(p + 2);
    width = (r >= 32 && r <= 47) ? 2 : 4;
    if (sz < 4 + n * width)
        return rangecheck;
    arr = xpost_array_cons(ctx, n);
    if (xpost_object_get_type(arr) == nulltype)
        return VMerror;
    for (i = 0; i < n; i++)
    {
        Xpost_Object el;
        int ret;

        /* The array's own allocation, and the backup a put records,
           each grow the memory file, which moves it: the encoded bytes
           are reached through the string's entity here rather than
           through the pointer the header was read with.

           rep carries representation and byte order exactly as a binary
           token's rep byte does: decode through the scanner's shared
           routine rather than a fourth copy of the byte math */
        p = (unsigned char *)xpost_string_get_pointer(ctx, str);
        ret = xpost_scanner_rep_number(rep, p + 4 + i * width, &el);
        if (ret)
            return ret;
        ret = xpost_array_put(ctx, arr, (integer)i, el);
        if (ret)
            return ret;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(arr));
    return 0;
}

int xpost_oper_init_array_ops (Xpost_Context *ctx,
                               Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;
    int ret;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "array", (Xpost_Op_Func)xpost_op_int_array, 1,
            integertype);
    INSTALL;
    ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "["), mark);
    if (ret)
        return ret;
    op = xpost_operator_cons(ctx, "]", (Xpost_Op_Func)xpost_op_array_to_mark, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "length", (Xpost_Op_Func)xpost_op_array_length, 1,
            arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "get", (Xpost_Op_Func)xpost_op_array_int_get, 2,
            arraytype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "put", (Xpost_Op_Func)xpost_op_array_int_any_put, 3,
            arraytype, integertype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "getinterval", (Xpost_Op_Func)xpost_op_array_int_int_getinterval, 3,
            arraytype, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "putinterval", (Xpost_Op_Func)xpost_op_array_int_array_putinterval, 3,
            arraytype, integertype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "aload", (Xpost_Op_Func)xpost_op_array_aload, 1,
            arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "astore", (Xpost_Op_Func)xpost_op_anyn_array_astore, 1,
            arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".numstring2array", (Xpost_Op_Func)_numstring2array, 1,
            stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "copy", (Xpost_Op_Func)xpost_op_array_copy, 2,
            arraytype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "forall", (Xpost_Op_Func)xpost_op_array_proc_forall, 2,
            arraytype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, "forall.array.iterate", (Xpost_Op_Func)xpost_op_array_forall_iterate, 0);

    return 0;
}

