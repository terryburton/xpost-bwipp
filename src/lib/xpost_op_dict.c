/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_dict.c
 * @brief Installs the dictionary operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * dict begin end def load store get put known where copy forall
 * currentdict countdictstack dictstack cleardictstack length maxlength undef
 *
 * Lookup walks the dictionary stack from the top, which is what makes a
 * definition shadow rather than replace.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdio.h>

#include "xpost.h"
#include "xpost_log.h"
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
#include "xpost_op_dict.h"

int DEBUGLOAD = 0;
int xpost_op_any_where (Xpost_Context *ctx, Xpost_Object K); /* forward decl.
                                                   store uses where */

/* int  dict  dict
   create dictionary with capacity for int elements */
static
int xpost_op_int_dict(Xpost_Context *ctx,
                      Xpost_Object I)
{
    Xpost_Object dic;

    if (I.int_.val < 0)
        return rangecheck;
    if (I.int_.val > (integer)XPOST_OBJECT_COMP_MAX_SZ) /* the sz field is full */
        return limitcheck;

    dic = xpost_dict_cons (ctx, I.int_.val);
    if (xpost_object_get_type(dic) == nulltype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(dic));
    return 0;
}

/* -  <<  mark
   start dictionary construction */

/* mark k_1 v_1 ... k_N v_N  >>  dict
   construct dictionary from pairs on stack */
static
int xpost_op_dict_to_mark(Xpost_Context *ctx)
{
    integer i; /* the counted objects, in the width the count arrives in */
    integer npairs; /* the entries they make, two objects to each */
    Xpost_Object d, k, v;
    Xpost_Object t;
    int ret;

    if (xpost_op_counttomark(ctx))
        return unmatchedmark;
    t = xpost_stack_pop(ctx->lo, ctx->os);
    if (xpost_object_get_type(t) == invalidtype)
        return stackunderflow;
    i = t.int_.val;
    if ((i % 2) == 1)
        return rangecheck;
    /* what lies between the marks is counted in objects and asked for in
       entries: this is the dict operator with a put for each pair (PLRM
       3.2.4), so it is held to the capacity that operator is held to and
       reaches the same one */
    npairs = i / 2;
    if (npairs > (integer)XPOST_OBJECT_COMP_MAX_SZ) /* the sz field is full, as the dict operator enforces:
                      raise limitcheck rather than let dict_cons truncate the
                      capacity and then fault putting the discarded pairs */
        return limitcheck;
    d = xpost_object_cvlit(xpost_dict_cons (ctx, (unsigned int)npairs));
    if (xpost_object_get_type(d) == nulltype)
        return VMerror;
    for ( ; i > 0; i -= 2)
    {
        v = xpost_stack_pop(ctx->lo, ctx->os);
        if (xpost_object_get_type(v) == invalidtype)
            return stackunderflow;
        k = xpost_stack_pop(ctx->lo, ctx->os);
        if (xpost_object_get_type(k) == invalidtype)
            return stackunderflow;
        if ((ret = xpost_dict_put(ctx, d, k, v)))
            return ret;
    }
    (void)xpost_stack_pop(ctx->lo, ctx->os); // pop mark
    xpost_stack_push(ctx->lo, ctx->os, d);
    return 0;
}

/* dict  length  int
   number of key-value pairs in dict */
static
int xpost_op_dict_length(Xpost_Context *ctx,
                         Xpost_Object D)
{
    if (!xpost_object_is_readable(ctx, D))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons(xpost_dict_length_memory(xpost_context_select_memory(ctx, D) /*D.tag&FBANK?ctx->gl:ctx->lo*/,
                                                             D)));
    return 0;
}

/* dict  maxlength  int
   capacity of dict */
static
int xpost_op_dict_maxlength(Xpost_Context *ctx,
                            Xpost_Object D)
{
    if (!xpost_object_is_readable(ctx, D))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons(xpost_dict_capacity_memory(xpost_context_select_memory(ctx, D),
                                                               D)));
    return 0;
}

/* dict  begin  -
   push dict on dict stack */
static
int xpost_op_dict_begin(Xpost_Context *ctx,
                        Xpost_Object D)
{
    if (!xpost_object_is_readable(ctx, D))
        return invalidaccess;

    ++ctx->namebind_gen; /* visibility changes */

    if (!xpost_stack_push(ctx->lo, ctx->ds, D))
        return dictstackoverflow;
    return 0;
}

/* -  end  -
   pop dict stack */
static
int xpost_op_end(Xpost_Context *ctx)
{
    ++ctx->namebind_gen;

    if (xpost_stack_count(ctx->lo, ctx->ds) <= 3)
        return dictstackunderflow;
    (void)xpost_stack_pop(ctx->lo, ctx->ds);
    return 0;
}

/* key value  def  -
   associate key with value in current dict */
static
int xpost_op_any_any_def(Xpost_Context *ctx,
                         Xpost_Object K,
                         Xpost_Object V)
{
    Xpost_Object D = xpost_stack_topdown_fetch(ctx->lo, ctx->ds, 0);
    Xpost_Memory_File *mem = xpost_context_select_memory(ctx, D);
    int ret;

    /* the arguments are held by the operator machinery, so the general
       wrapper's re-holding is unnecessary on the fast path (see
       xpost_dict_def_cached for its contract) */
    if (xpost_object_get_type(K) == nametype &&
        xpost_dict_def_fast_ok(ctx, mem, V))
        return xpost_dict_def_cached(ctx, mem, D, K, V);

    ret = xpost_dict_put(ctx, D, K, V);
    if (ret)
        return ret;
    return 0;
}

/* key  load  value
   search dict stack for key and return associated value */
int xpost_op_any_load(Xpost_Context *ctx,
                      Xpost_Object K)
{
    int i;
    int z = xpost_stack_count(ctx->lo, ctx->ds);
    if (DEBUGLOAD)
    {
        printf("\nload:");
        xpost_object_dump(K);
        xpost_stack_dump(ctx->lo, ctx->ds);
    }

    xpost_stack_push(ctx->lo, ctx->hold, K);

    for (i = 0; i < z; i++)
    {
        Xpost_Object x;
        Xpost_Object D = xpost_stack_topdown_fetch(ctx->lo,ctx->ds,i);

        if (DEBUGLOAD)
        {
            xpost_dict_dump_memory (xpost_context_select_memory(ctx, D), D);
            (void)puts("");
        }

        x = xpost_dict_get(ctx, D, K);
        if (xpost_object_get_type(x) != invalidtype)
        {
            xpost_stack_push(ctx->lo, ctx->os, x);
            return 0;
        }
    }

    if (DEBUGLOAD)
    {
        unsigned int names;
        xpost_memory_file_dump(ctx->lo);
        xpost_memory_table_dump(ctx->lo);
        xpost_memory_file_dump(ctx->gl);
        xpost_memory_table_dump(ctx->gl);
        names = xpost_memory_name_stack_ent(ctx->gl);
        xpost_stack_dump(ctx->gl, names);
        xpost_object_dump(K);
    }

    return undefined;
}

/* Like xpost_op_any_load, but resolves the key in the interpreter's private
   machinery dictionary (privatedict), which is off the dict stack. The device
   drivers reach the device class dictionaries this way: the classes live in
   privatedict, not on the dict stack, so a dict-stack load would not find them. */
int xpost_op_privatedict_load(Xpost_Context *ctx,
                              Xpost_Object K)
{
    Xpost_Object x = xpost_dict_get(ctx, ctx->privatedict, K);
    if (xpost_object_get_type(x) == invalidtype)
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, x);
    return 0;
}

/* key value  store  -
   replace topmost definition of key */
static
int xpost_op_any_store(Xpost_Context *ctx,
                       Xpost_Object K,
                       Xpost_Object V)
{
    Xpost_Object D;
    int ret;

    /* where answers a flag, and a dictionary under it where it found
       one. A key it refuses is a key it pushes nothing for, and what
       lies under a refusal is the program's own operands. */
    ret = xpost_op_any_where(ctx, K);
    if (ret)
        return ret;
    if (xpost_stack_pop(ctx->lo, ctx->os).int_.val) /* booleantype */
    {
        D = xpost_stack_pop(ctx->lo, ctx->os);
    }
    else
    {
        D = xpost_stack_topdown_fetch(ctx->lo, ctx->ds, 0);
    }
    return xpost_dict_put(ctx, D, K, V);
}

/* dict key  get  any
   get value associated with key in dict */
static
int xpost_op_dict_any_get(Xpost_Context *ctx,
                          Xpost_Object D,
                          Xpost_Object K)
{
    Xpost_Object v;

    if (!xpost_object_is_readable(ctx, D))
        return invalidaccess;
    v = xpost_dict_get(ctx, D, K);
    if (xpost_object_get_type(v) == invalidtype)
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, v);
    return 0;
}

/* dict key value  put  -
   associate key with value in dict */
static
int xpost_op_dict_any_any_put(Xpost_Context *ctx,
                              Xpost_Object D,
                              Xpost_Object K,
                              Xpost_Object V)
{
    return xpost_dict_put(ctx, D, K, V);
}

/* dict key  undef  -
   remove key and its value in dict */
static
int xpost_op_dict_any_undef(Xpost_Context *ctx,
                            Xpost_Object D,
                            Xpost_Object K)
{
    int ret;
    if (!xpost_object_is_writeable(ctx, D))
        return invalidaccess;
    ret = xpost_dict_undef(ctx, D, K);
    if (ret == undefined)
        return 0; /* PLRM: undef of an unknown key has no effect */
    return ret;
}

/* dict key  known  bool
   test whether key is in dict */
static
int xpost_op_dict_any_known(Xpost_Context *ctx,
                            Xpost_Object D,
                            Xpost_Object K)
{
    /* the dictionary is searched, so it needs read access */
    if (!xpost_object_is_readable(ctx, D))
        return invalidaccess;
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(xpost_dict_known_key(ctx, xpost_context_select_memory(ctx, D), D, K)));
    return 0;
}


/* key  where  dict true -or- false
   find dict in which key is defined */
int xpost_op_any_where(Xpost_Context *ctx,
                       Xpost_Object K)
{
    int i;
    int z;
    int isname;

    /* a key may be any object except null (PLRM 3.3.5) */
    if (xpost_object_get_type(K) == nulltype)
        return typecheck;

    z = xpost_stack_count(ctx->lo, ctx->ds);
    isname = xpost_object_get_type(K) == nametype;
    for (i = 0; i < z; i++)
    {
        Xpost_Object D = xpost_stack_topdown_fetch(ctx->lo, ctx->ds, i);
        if (isname
                ? xpost_object_get_type(xpost_dict_get_name(ctx, D, K)) != invalidtype
                : xpost_dict_known_key(ctx, xpost_context_select_memory(ctx, D), D, K))
        {
            xpost_stack_push(ctx->lo, ctx->os, D);
            xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
            return 0;
        }
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
}

/* dict1 dict2  copy  dict2
   copy contents of dict1 to dict2 */
int xpost_op_dict_copy(Xpost_Context *ctx,
                       Xpost_Object S,
                       Xpost_Object D)
{
    int i, sz;
    Xpost_Memory_File *mem;
    unsigned ad;
    dicrec *tp;
    int ret;

    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    if (!xpost_object_is_writeable(ctx, D))
        return invalidaccess;
    mem = xpost_context_select_memory(ctx, S);
    sz = xpost_dict_max_length_memory (mem, S);
    ret = xpost_memory_table_get_addr(mem, xpost_object_get_ent(S), &ad);
    if (!ret)
    {
        XPOST_LOG_ERR("cannot retrieve address for dict ent %u",
                xpost_object_get_ent(S));
        return VMerror;
    }
    tp = xpost_dict_table_of(xpost_dict_head_at(mem, ad));
    for (i = 0; i < DICTABN(sz); i++)
    {
        if (xpost_object_get_type(tp[i].key) != nulltype)
        {
            if ((ret = xpost_dict_put(ctx, D, tp[i].key, tp[i].value)))
                return ret;
            tp = xpost_dict_table_of(xpost_dict_head_at(mem, ad)); /* recalc */
        }
    }
    xpost_stack_push(ctx->lo, ctx->os, D);
    return 0;
}

/* find the next occupied slot at or after *cursor, push the pair on the
   operand stack and leave the cursor one past it; shared by forall and
   its iterate continuation. returns 1 while pairs remain.

   The cursor indexes the table of 2*sz+1 records, which is longer than
   the size a composite's own fields count, so it is carried beside the
   dictionary rather than in it and counted in a type wide enough to
   reach the end of the table. The table's length is taken afresh on
   every step: the procedure may put entries and grow the dictionary
   under the enumeration. */
static
int _dict_forall_step (Xpost_Context *ctx,
                       Xpost_Object D,
                       unsigned int *cursor,
                       int *reterr)
{
    Xpost_Memory_File *mem = xpost_context_select_memory(ctx, D);
    unsigned int n;
    unsigned ad;
    dicrec *tp;
    int ret;

    *reterr = 0;
    n = DICTABN(xpost_dict_max_length_memory (mem, D));
    if (*cursor >= n) // cursor past the table
        return 0;

    ret = xpost_memory_table_get_addr(mem, xpost_object_get_ent(D), &ad);
    if (!ret)
    {
        XPOST_LOG_ERR("cannot retrieve address for dict ent %u",
                      xpost_object_get_ent(D));
        *reterr = VMerror;
        return 0;
    }
    tp = xpost_dict_table_of(xpost_dict_head_at(mem, ad));

    for ( ; *cursor < n; ++*cursor) // find next pair
    {
        if (xpost_object_get_type(tp[*cursor].key) != nulltype) // found
        {
            Xpost_Object k,v;

            k = tp[*cursor].key;
            if (xpost_object_get_type(k) == extendedtype)
                k = xpost_dict_convert_extended_to_number(k);
            v = tp[*cursor].value;

            if (!xpost_stack_push(ctx->lo, ctx->os, k))
            {
                *reterr = stackoverflow;
                return 0;
            }
            if (!xpost_stack_push(ctx->lo, ctx->os, v))
            {
                *reterr = stackoverflow;
                return 0;
            }
            ++*cursor;
            return 1;
        }
    }
    return 0;
}

/* dict1 dict2  .gstatecopy  dict2
   Copy dict1's entries into dict2, giving every entry that holds a
   literal array an array of its own.

   The value in a graphics state entry is an object the program still
   holds: setcolorspace records the space it was handed and setdash the
   array it was handed, so copying a restored value over whatever the
   entry already holds would rewrite the program's array underneath it.
   Where the restored value names that same array -- as an uncoloured
   pattern's space names the base it was built over -- each restore
   wraps it one level deeper until nothing can read it as a space at
   all. A packed array is read-only and cannot be written through, and
   an executable array is a procedure a graphics state shares rather
   than copies -- a transfer or spot function is the same procedure
   whichever state names it -- so neither is copied.

   In C because it runs twice for every gsave, once to take the state
   and once to put it back, over a dictionary of fifty-three entries of
   which three to six hold an array: the walk cost several times what
   the copying does. */
static
int _gstatecopy(Xpost_Context *ctx,
                Xpost_Object D1,
                Xpost_Object D2)
{
    unsigned int cursor;

    if (!xpost_object_is_readable(ctx, D1))
        return invalidaccess;
    if (!xpost_object_is_writeable(ctx, D2))
        return invalidaccess;

    for (cursor = 0; ; ++cursor)
    {
        Xpost_Memory_File *mem;
        unsigned int n, ad;
        dicrec *tp;
        Xpost_Object k, v;
        int ret;

        /* The table is located afresh on every step. Consing the copy of
           an array value, and the put that files it, may each grow the
           memory the table sits in, which moves it; a pointer taken
           before either would address whatever now occupies the old
           place. */
        mem = xpost_context_select_memory(ctx, D1);
        n = DICTABN(xpost_dict_max_length_memory(mem, D1));
        if (cursor >= n)
            break;
        if (!xpost_memory_table_get_addr(mem, xpost_object_get_ent(D1), &ad))
        {
            XPOST_LOG_ERR("cannot retrieve address for dict ent %u",
                          xpost_object_get_ent(D1));
            return VMerror;
        }
        tp = xpost_dict_table_of(xpost_dict_head_at(mem, ad));

        k = tp[cursor].key;
        if (xpost_object_get_type(k) == nulltype)
            continue;
        if (xpost_object_get_type(k) == extendedtype)
            k = xpost_dict_convert_extended_to_number(k);
        v = tp[cursor].value;

        if (xpost_object_get_type(v) == arraytype && !xpost_object_is_exe(v))
        {
            Xpost_Object a;
            unsigned int i;
            unsigned int sz = v.comp_.sz;

            /* A value an access withholds is not one this may read
               (PLRM 3.3.2). The elements are taken below by a read that
               does no checking of its own, so the check is made here --
               without it a copy taken through this operator would hand
               back, with unrestricted access, what the original refused
               to give up. */
            if (!xpost_object_is_readable(ctx, v))
                return invalidaccess;

            /* literal, as the value it replaces was: executability
               belongs to a procedure, and the walk above has already
               decided this value is not one. An array minted here
               without saying so is executable, and the entry it lands
               in is then one this copy will never copy again -- the
               state that saved it and the state that restored it would
               share one array from then on. */
            a = xpost_object_cvlit(xpost_array_cons(ctx, sz));
            /* the constructor answers a failure with null, not with the
               invalid object: read for the wrong one and the failure
               goes unnoticed here and is reported, further down, as
               whatever the null object refuses next -- which is not the
               exhaustion that happened. */
            if (xpost_object_get_type(a) != arraytype)
                return VMerror;
            for (i = 0; i < sz; i++)
            {
                ret = xpost_array_put(ctx, a, (integer)i,
                                      xpost_array_get(ctx, v, (integer)i));
                if (ret)
                    return ret;
            }
            v = a;
        }

        ret = xpost_dict_put(ctx, D2, k, v);
        if (ret)
            return ret;
    }

    if (!xpost_stack_push(ctx->lo, ctx->os, D2))
        return stackoverflow;
    return 0;
}

/* dict proc  forall  -
   execute proc for each key value pair in dict */
static
int xpost_op_dict_proc_forall (Xpost_Context *ctx,
                               Xpost_Object D,
                               Xpost_Object P)
{
    unsigned int cursor = 0;
    int err;

    /* forall of an unreadable dict is invalidaccess, per the access rules */
    if (!xpost_object_is_readable(ctx, D))
        return invalidaccess;

    if (!_dict_forall_step(ctx, D, &cursor, &err))
        return err;

    /* loop frame: the sentinel forall operator (which exit searches
       for) under literal state that the iterate operator consumes,
       placed as one run */
    {
        Xpost_Object fr[6];

        fr[0] = XPOST_OP(ctx, forall);
        fr[1] = xpost_object_cvlit(P);
        fr[2] = xpost_object_cvlit(D);
        fr[3] = xpost_int_cons((integer)cursor);
        fr[4] = XPOST_OP(ctx, dictforallcont);
        fr[5] = P;
        if (!xpost_stack_push_run(ctx->lo, ctx->es, fr, 6))
            return execstackoverflow;
    }
    return 0;
}

/* continue a dict forall: es holds (from the top) the slot cursor, the
   dict, the literal proc, and the sentinel */
static
int xpost_op_dict_forall_iterate (Xpost_Context *ctx)
{
    Xpost_Object C, D, P;
    unsigned int cursor;
    int err;

    C = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 0);
    D = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 1);
    P = xpost_stack_topdown_fetch(ctx->lo, ctx->es, 2);
    if (xpost_object_get_type(C) != integertype
        || xpost_object_get_type(D) == invalidtype)
        return execstackunderflow;
    cursor = (unsigned int)C.int_.val;

    if (!_dict_forall_step(ctx, D, &cursor, &err))
    {
        int k;
        if (err)
            return err;
        for (k = 0; k < 4; k++)
            (void)xpost_stack_pop(ctx->lo, ctx->es);
        return 0;
    }

    if (!xpost_stack_topdown_replace(ctx->lo, ctx->es, 0,
                                     xpost_int_cons((integer)cursor)))
        return execstackunderflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          XPOST_OP(ctx, dictforallcont)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvx(P)))
        return execstackoverflow;
    return 0;
}

/* -  currentdict  dict
   push current dict on operand stack */
static
int xpost_op_currentdict(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_stack_topdown_fetch(ctx->lo, ctx->ds, 0)))
        return stackoverflow;
    return 0;
}

/* -  errordict  dict   % error handler dictionary : err.ps
   -  $error  dict      % error control and status dictionary : err.ps
   -  systemdict  dict  % system dictionary : op.c init.ps
   -  userdict  dict    % writeable dictionary in local VM : xpost_context.c
   -  globaldict  dict  % writeable dictionary in global VM : xpost_context.c
   %-  statusdict  dict  % product-dependent dictionary
   */

/* -  countdictstack  int
   count elements on dict stack */
static
int xpost_op_countdictstack(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(xpost_stack_count(ctx->lo, ctx->ds))))
        return stackoverflow;
    return 0;
}

/* array  dictstack  subarray
   copy dict stack into array */
static
int xpost_op_array_dictstack(Xpost_Context *ctx,
                             Xpost_Object A)
{
    Xpost_Object subarr;
    int z = xpost_stack_count(ctx->lo, ctx->ds);
    int i;
    int ret;
    for (i = 0; i < z; i++)
    {
        ret = xpost_array_put(ctx, A, i,
                              xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, i));
        if (ret)
            return ret;
    }
    subarr = xpost_object_get_interval(A, 0, z);
    if (xpost_object_get_type(subarr) == invalidtype)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, subarr);
    return 0;
}

static
int xpost_op_cleardictstack(Xpost_Context *ctx)
{
    int z = xpost_stack_count(ctx->lo, ctx->ds);

    ++ctx->namebind_gen;  /* popped dicts invalidate cached resolutions */

    while (z-- > 3)
    {
        (void)xpost_stack_pop(ctx->lo, ctx->ds);
    }
    /*
    Xpost_Stack *ds;
    unsigned int dsaddr;
    int ret;

    ret = xpost_memory_table_get_addr(ctx->lo, ctx->ds, &dsaddr);
    if (!ret) {
        XPOST_LOG_ERR("cannot retrieve address for dict stack");
        return VMerror;
    }
    ds = xpost_stack_at(ctx->lo, dsaddr);
    ds->top = 3;
    */
    return 0;
}

int xpost_oper_init_dict_ops (Xpost_Context *ctx,
                              Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;
    int ret;

    assert(ctx->gl->base);
    op = xpost_operator_cons(ctx, "dict", (Xpost_Op_Func)xpost_op_int_dict, 1, integertype);
    INSTALL;
    ret = xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "<<"), mark);
    if (ret)
        return 0;
    op = xpost_operator_cons(ctx, ">>", (Xpost_Op_Func)xpost_op_dict_to_mark, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "length", (Xpost_Op_Func)xpost_op_dict_length, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, "maxlength", (Xpost_Op_Func)xpost_op_dict_maxlength, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, "begin", (Xpost_Op_Func)xpost_op_dict_begin, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, "end", (Xpost_Op_Func)xpost_op_end, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "def", (Xpost_Op_Func)xpost_op_any_any_def, 2, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "load", (Xpost_Op_Func)xpost_op_any_load, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "store", (Xpost_Op_Func)xpost_op_any_store, 2, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "get", (Xpost_Op_Func)xpost_op_dict_any_get, 2, dicttype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "put", (Xpost_Op_Func)xpost_op_dict_any_any_put, 3,
            dicttype, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "undef", (Xpost_Op_Func)xpost_op_dict_any_undef, 2, dicttype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "known", (Xpost_Op_Func)xpost_op_dict_any_known, 2, dicttype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "where", (Xpost_Op_Func)xpost_op_any_where, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "copy", (Xpost_Op_Func)xpost_op_dict_copy, 2, dicttype, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, "forall", (Xpost_Op_Func)xpost_op_dict_proc_forall, 2, dicttype, proctype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".gstatecopy", (Xpost_Op_Func)_gstatecopy, 2, dicttype, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, "forall.dict.iterate", (Xpost_Op_Func)xpost_op_dict_forall_iterate, 0);
    op = xpost_operator_cons(ctx, "currentdict", (Xpost_Op_Func)xpost_op_currentdict, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "countdictstack", (Xpost_Op_Func)xpost_op_countdictstack, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "dictstack", (Xpost_Op_Func)xpost_op_array_dictstack, 1, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cleardictstack", (Xpost_Op_Func)xpost_op_cleardictstack, 0);
    INSTALL;
    return 0;
}
