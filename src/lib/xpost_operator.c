/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_operator.c
 * @brief The operator table: every operator this build has, in row order.
 *
 * An operator object carries the number of its row and nothing else, so the
 * order of this table is part of what an image of virtual memory means: a
 * table built in another order would dispatch to the wrong operator without
 * raising anything.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h> /* NULL */
#include <string.h> /* memcpy */
#include <stdint.h> /* uintptr_t */

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"  // accesses mfile
#include "xpost_object.h"  // operators are objects
#include "xpost_stack.h"  // uses a stack for argument passing
#include "xpost_free.h"  // give the old storage back when the table grows
#include "xpost_context.h"
#include "xpost_error.h"  // operator functions may throw errors
#include "xpost_string.h"  // uses string function to dump operator name
#include "xpost_name.h"  // operator objects have associated names
#include "xpost_dict.h"  // install operators in systemdict, a dict
#include "xpost_array.h"  // a wrapped call's operands are saved in an array

//#include "xpost_interpreter.h"  // works with context struct
#include "xpost_operator.h"  // double-check prototypes


/* convert an integertype object to a realtype object */
static
Xpost_Object _promote_integer_to_real(Xpost_Object o)
{
    return xpost_real_cons((real)o.int_.val);
}

/* Record that operand at top-down index idx was coerced from the integer orig,
   so an error can restore the original the program pushed (PLRM 3.11). Called
   only when a coercion actually happens, so it is a no-op for the usual case. */
static void _op_restore_note(Xpost_Context *ctx, int idx, Xpost_Object orig)
{
    if (ctx->op_restore_n < (int)(sizeof ctx->op_restore_idx))
    {
        ctx->op_restore_idx[ctx->op_restore_n] = (unsigned char)idx;
        ctx->op_restore_val[ctx->op_restore_n] = orig;
        ctx->op_restore_n++;
    }
}

/* copied from the header file for reference:
   typedef struct Xpost_Signature {
   int (*fp)(Xpost_Context *ctx);
   int in;
   unsigned t;
   int (*checkstack)(Xpost_Context *ctx);
   } Xpost_Signature;

   typedef struct Xpost_Operator {
   unsigned name;
   int n; // number of sigs
   unsigned sigadr;
   } Xpost_Operator;

   enum typepat ( anytype = stringtype + 1,
   floattype, numbertype, proctype };

   #define MAXOPS 20
*/

/* the number of ops in the optab of the context being served; reset with
   the table itself, which each context allocates in its own global VM */
static
int _xpost_noops = 0;

/* How many operators the table holds, so the collector can bound a walk
   of it: an operator installed from C keeps its procedure there and
   nowhere else. */
unsigned int xpost_operator_count(void)
{
    return (unsigned int)(_xpost_noops < 0 ? 0 : _xpost_noops);
}

/* How many operators the table holds, told rather than counted, for a
   context whose table arrived whole. The rows are then already in
   virtual memory and this is what says how many of them are rows: the
   count lives outside virtual memory and does not arrive with them. */
void xpost_operator_set_count(unsigned int count)
{
    _xpost_noops = (int)(count < MAXOPS ? count : MAXOPS - 1);
}

static
int _stack_none(Xpost_Context *ctx)
{
    (void)ctx;
    return 0;
}

static
int _stack_int(Xpost_Context *ctx)
{
    Xpost_Object s0;
    s0 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    switch(xpost_object_get_type(s0))
    {
        case invalidtype:
            return stackunderflow;
        case integertype:
            return 0;
        default:
            return typecheck;
    }
}

static
int _stack_real(Xpost_Context *ctx)
{
    Xpost_Object s0;
    s0 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    switch(xpost_object_get_type(s0))
    {
        case invalidtype:
            return stackunderflow;
        case realtype:
            return 0;
        default:
            return typecheck;
    }
}

static
int _stack_float(Xpost_Context *ctx)
{
    Xpost_Object s0;
    s0 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    switch(xpost_object_get_type(s0))
    {
        case invalidtype:
            return stackunderflow;
        case integertype:
            _op_restore_note(ctx, 0, s0);
            /* the fetch just above reached this index, and nothing has
               touched the stack since, so the store reaches it too */
            XPOST_REFUSAL_IMPOSSIBLE(
                xpost_stack_topdown_replace(ctx->lo, ctx->os, 0,
                                            s0 = _promote_integer_to_real(s0)));
            /* fallthrough */
        case realtype:
            return 0;
        default:
            return typecheck;
    }
}

static
int _stack_any(Xpost_Context *ctx)
{
    Xpost_Stack *os_root = xpost_stack_at(ctx->lo, ctx->os);
    Xpost_Stack *os_top = xpost_stack_at(ctx->lo, os_root->prevseg);
    /* at least one operand without walking the whole stack: the top
       segment holds one, or a full segment sits below it (only the top
       segment is ever partial) -- counting is O(n) in the stack depth */
    if (os_top->top >= 1 || os_top != os_root)
        return 0;
    return stackunderflow;
}

static
int _stack_bool_bool(Xpost_Context *ctx)
{
    Xpost_Object s0, s1;
    s0 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    switch(xpost_object_get_type(s0))
    {
        case invalidtype:
            return stackunderflow;
        case booleantype:
            s1 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 1);
            switch(xpost_object_get_type(s1))
            {
                case invalidtype:
                    return stackunderflow;
                case booleantype:
                    return 0;
                default:
                    return typecheck;
            }
        default:
            return typecheck;
    }
}

static
int _stack_int_int(Xpost_Context *ctx)
{
    Xpost_Object s0, s1;
    s0 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    switch(xpost_object_get_type(s0))
    {
        case invalidtype:
            return stackunderflow;
        case integertype:
            s1 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 1);
            switch(xpost_object_get_type(s1))
            {
                case invalidtype:
                    return stackunderflow;
                case integertype:
                    return 0;
                default:
                    return typecheck;
            }
        default:
            return typecheck;
    }
}

static
int _stack_float_float(Xpost_Context *ctx)
{
    Xpost_Object s0, s1;
    s0 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    switch(xpost_object_get_type(s0))
    {
        case invalidtype:
            return stackunderflow;
        case integertype:
            _op_restore_note(ctx, 0, s0);
            /* the fetch just above reached this index, and nothing has
               touched the stack since, so the store reaches it too */
            XPOST_REFUSAL_IMPOSSIBLE(
                xpost_stack_topdown_replace(ctx->lo, ctx->os, 0,
                                            s0 = _promote_integer_to_real(s0)));
            /* fallthrough */
        case realtype:
            s1 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 1);
            switch(xpost_object_get_type(s1))
            {
                case invalidtype:
                    return stackunderflow;
                case integertype:
                    _op_restore_note(ctx, 1, s1);
                    /* as above: the index was just fetched from */
                    XPOST_REFUSAL_IMPOSSIBLE(
                        xpost_stack_topdown_replace(ctx->lo, ctx->os, 1,
                                                    s1 = _promote_integer_to_real(s1)));
                    /* fallthrough */
                case realtype:
                    return 0;
                default:
                    return typecheck;
            }
        default:
            return typecheck;
    }
}

static
int _stack_number_number(Xpost_Context *ctx)
{
    Xpost_Object s0, s1;
    s0 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    switch(xpost_object_get_type(s0))
    {
        case invalidtype:
            return stackunderflow;
        case integertype: /* fallthrough */
        case realtype:
            s1 = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 1);
            switch(xpost_object_get_type(s1))
            {
                case invalidtype:
                    return stackunderflow;
                case integertype: /* fallthrough */
                case realtype:
                    return 0;
                default:
                    return typecheck;
            }
        default:
            return typecheck;
    }
}

static
int _stack_any_any(Xpost_Context *ctx)
{
    Xpost_Stack *os_root = xpost_stack_at(ctx->lo, ctx->os);
    Xpost_Stack *os_top = xpost_stack_at(ctx->lo, os_root->prevseg);
    /* at least two operands in O(1): two in the top segment, or a full
       segment (never partial) below it */
    if (os_top->top >= 2 || os_top != os_root)
        return 0;
    return stackunderflow;
}

typedef struct {
    int (*checkstack)(Xpost_Context *ctx);
    int n;
    int t[8];
} Xpost_Check_Stack;

static
Xpost_Check_Stack _check_stack_funcs[] = {
    { _stack_none, 0, { 0, 0, 0, 0, 0, 0, 0, 0} },
    { _stack_int, 1, { integertype } },
    { _stack_real, 1, { realtype } },
    { _stack_float, 1, { floattype } },
    { _stack_any, 1, { anytype } },
    { _stack_bool_bool, 2, { booleantype, booleantype } },
    { _stack_int_int, 2, { integertype, integertype } },
    { _stack_float_float, 2, { floattype, floattype } },
    { _stack_number_number, 2, { numbertype, numbertype } },
    { _stack_any_any, 2, { anytype, anytype } }
};


/* What the operator rows point at lives after them in the same entity,
 * and is reached by its offset from the start of that entity.
 *
 * A signature and the array of argument types it names each held their
 * own allocation before, taken straight off the memory file with no table
 * row to describe them, and the rows named them by address into the
 * arena. Nothing could find such a block: not its size, not its tag, and
 * not the row pointing at it. There are several hundred of them and they
 * sit scattered through global virtual memory, so a pass that walked the
 * table to rearrange the arena would have written a live entity over one.
 *
 * Following the rows in the entity the rows already have, they need no
 * row of their own: the entity's one row covers all of it, and an offset
 * from its start survives the storage moving. So sigadr and t are no
 * longer places in the arena. The entity's row is, and it is the single
 * thing a rearrangement rewrites.
 *
 * Storage is handed out by moving a cursor and is not given back. An
 * operator that gains a second signature has its run of signatures
 * allocated afresh and copied, and the run it had before is left where it
 * is with nothing naming it -- so the waste is one abandoned run per
 * regrowth. Every operator is installed while the interpreter starts, so
 * that is a bounded cost paid once and never during a program; it is
 * still waste, and it is the price of not keeping a free list for storage
 * that is never otherwise released.
 *
 * Offset 0 cannot be one of these, since the rows themselves begin there,
 * so it goes on meaning "this operator states nothing".
 */

/* the rows, then the cursor, then what the rows point at */
#define OPTAB_ROWS_BYTES ((unsigned int)(MAXOPS * sizeof(Xpost_Operator)))
#define OPTAB_CURSOR     OPTAB_ROWS_BYTES
#define OPTAB_FIRST      (OPTAB_ROWS_BYTES + 8u)

/* The arena address of a byte of the entity, named by its offset within
   it. An address rather than a pointer, so that what turns it into one
   is xpost_vm_ptr, the same call as everywhere else -- a second way of
   reaching virtual memory would be a second thing for everything that
   reads how virtual memory is reached to know about.

   Derived afresh at every use: an allocation may grow the memory file,
   which moves it, so an address taken before one still stands but a
   pointer does not. */
/* A byte of the table, named by its offset from the table's start.

   void * rather than unsigned char *, as xpost_vm_ptr is for the arena:
   every caller wants it as something else -- a row, a run of signatures,
   a run of type bytes -- and a cast at each of them is a cast that can be
   got wrong at each of them. */
static void *_optab_at(Xpost_Memory_File *gl, unsigned int off)
{
    return gl->optab + off;
}

static unsigned int _optab_cursor(Xpost_Memory_File *gl)
{
    unsigned int c;
    memcpy(&c, _optab_at(gl, OPTAB_CURSOR), sizeof c);
    return c;
}

static void _optab_set_cursor(Xpost_Memory_File *gl, unsigned int c)
{
    memcpy(_optab_at(gl, OPTAB_CURSOR), &c, sizeof c);
}

/* Take `sz` bytes after the rows, growing the entity when they will not
   fit. The offset is 8-aligned because a signature carries function
   pointers, which on some hosts may not be read at any lesser alignment. */
static int _optab_alloc(Xpost_Memory_File *gl, unsigned int sz,
                        unsigned int *off)
{
    unsigned int at, end;

    at = (_optab_cursor(gl) + 7u) & ~7u;
    if (sz > 0xffffffffu - at)
    {
        XPOST_LOG_ERR("%d the operator table cannot address that much", VMerror);
        return 0;
    }
    end = at + sz;
    if (end > gl->optab_max)
    {
        unsigned char *bigger;
        unsigned int want = gl->optab_max * 2;

        if (want < end)
            want = end;
        bigger = realloc(gl->optab, want);
        if (!bigger)
        {
            XPOST_LOG_ERR("%d cannot grow the operator table", VMerror);
            return 0;
        }
        gl->optab = bigger;
        gl->optab_max = want;
    }
    memset(_optab_at(gl, at), 0, sz);
    _optab_set_cursor(gl, end);
    *off = at;
    return 1;
}

/* Keep the table as it stands, for a caller that will put it back.

   The job boundary is the one such caller: it reverts both banks to a
   baseline image, and a row of this table holds objects of the global
   bank -- the procedure a wrapped operator runs, and the name-stack index
   of its name. The table is not in the arena, so the image restore does
   not reach it, and a job that wrapped an operator would leave a row
   naming an entity the revert has dropped.

   Storage for the copy is this file's business rather than the caller's,
   because the table's growth is: a caller holding the size would be
   holding it across the calls that change it. */
int xpost_operator_table_snapshot(Xpost_Memory_File *gl,
                                  unsigned char **buf, unsigned int *len)
{
    unsigned char *keep;

    if (!gl->optab_max)
        return 1;
    keep = realloc(*buf, gl->optab_max);
    if (!keep)
        return 0;
    memcpy(keep, gl->optab, gl->optab_max);
    *buf = keep;
    *len = gl->optab_max;
    return 1;
}

/* Put back what the snapshot above kept. */
int xpost_operator_table_restore(Xpost_Memory_File *gl,
                                 const unsigned char *buf, unsigned int len)
{
    if (!buf || !len)
        return 1;
    if (gl->optab_max < len)
    {
        unsigned char *bigger = realloc(gl->optab, len);

        if (!bigger)
            return 0;
        gl->optab = bigger;
        gl->optab_max = len;
    }
    memcpy(gl->optab, buf, len);
    return 1;
}

/* Give row `k` room for `n` operand shapes, for a context whose table is
   being filled from an image rather than by installing operators.

   The run of shapes is cut from the table's own storage exactly as an
   install cuts one, so a row filled this way is indistinguishable from a
   row that was installed. */
int xpost_operator_take_signatures(Xpost_Memory_File *gl, unsigned int k,
                                   unsigned int n)
{
    Xpost_Operator *optab;
    unsigned int off;

    if (!_optab_alloc(gl, n * (unsigned int)sizeof(Xpost_Signature), &off))
        return 0;
    optab = xpost_operator_table(gl);
    optab[k].sigadr = off;
    optab[k].n = (int)n;
    return 1;
}

/* One shape of row `k`: what it takes, and the types it takes.

   The C function implementing the operator is not set. A row filled from
   an image is one the boot files declared in PostScript, which states a
   shape and runs a procedure; a row this build installs from C keeps the
   signatures it installed and never reaches here. */
int xpost_operator_set_signature(Xpost_Memory_File *gl, unsigned int k,
                                 unsigned int si, unsigned int in,
                                 const unsigned char *types)
{
    Xpost_Operator *optab = xpost_operator_table(gl);
    Xpost_Signature *sig;
    unsigned int at;

    if (in > XPOST_OPERATOR_MAX_SIG)
        return 0;
    if (!_optab_alloc(gl, in ? in : 1u, &at))
        return 0;
    if (in)
        memcpy(_optab_at(gl, at), types, in);
    optab = xpost_operator_table(gl);
    sig = (Xpost_Signature *)(void *)_optab_at(gl, optab[k].sigadr);
    sig[si].in = (int)in;
    sig[si].t = at;
    sig[si].fp = NULL;
    sig[si].checkstack = NULL;
    return 1;
}

/* Take the operator table's storage, outside the arena.

   It is the interpreter's, not a program's: one table per interpreter,
   built as the operators install and never reclaimed until the memory
   file goes. Host storage rather than an entity because a signature
   carries this process's function pointers, and the arena holds no
   addresses -- which is what lets it be compacted, given back and
   written out whole. */
int xpost_operator_init_optab(Xpost_Context *ctx)
{
    ctx->gl->optab = malloc(OPTAB_FIRST);
    if (!ctx->gl->optab)
    {
        XPOST_LOG_ERR("cannot allocate the operator table");
        return 0;
    }
    memset(ctx->gl->optab, 0, OPTAB_FIRST);
    ctx->gl->optab_max = OPTAB_FIRST;
    _xpost_noops = 0;
    _optab_set_cursor(ctx->gl, OPTAB_FIRST);

    return 1;
}

/* print a dump of the operator struct given opcode */
void xpost_operator_dump(Xpost_Context *ctx,
                         int opcode)
{
    Xpost_Operator *optab;
    Xpost_Operator op;
    Xpost_Object o = { 0 };
    Xpost_Object str;
    char *s;
    Xpost_Signature *sig;
    uintptr_t fp;

    optab = xpost_operator_table(ctx->gl);
    op = optab[opcode];
    o.mark_.tag = nametype | XPOST_OBJECT_TAG_DATA_FLAG_BANK;
    o.mark_.pad0 = 0;
    o.mark_.padw = op.name;
    str = xpost_name_get_string(ctx, o);
    s = xpost_string_get_pointer(ctx, str);
    sig = (Xpost_Signature *)(void *)_optab_at(ctx->gl, op.sigadr);
    memcpy(&fp, &sig[0].fp, sizeof fp);
    /*
    printf("<operator %d %d:%*s %p>",
           opcode,
           str.comp_.sz, str.comp_.sz, s,
           (void *)fp );
    */
    XPOST_LOG_DUMP("%.*s ", str.comp_.sz, s);
}

/* create operator object by opcode number */
Xpost_Object xpost_operator_cons_opcode(int opcode)
{
    Xpost_Object op = { 0 };
    op.mark_.tag = operatortype;
    op.mark_.pad0 = 0;
    op.mark_.padw = opcode;
    if (opcode >= _xpost_noops)
    {
        XPOST_LOG_ERR("opcode does not index a valid operator");
        return null;
    }
    return op;
}

/* construct an operator object by name
   If function-pointer fp is not NULL, attempts to install a new operator
   in OPTAB, otherwise just perform a lookup.
   If installing a new operator, in specifies the number of input
   values whose presence and types should be checked.
   There should follow 'in' number of typenames passed after 'in'.
*/
Xpost_Object xpost_operator_cons(Xpost_Context *ctx,
                                 const char *name,
                                 /*@null@*/ Xpost_Op_Func fp,
                                 int in, ...)
{
    Xpost_Object nm;
    Xpost_Object o = { 0 };
    int opcode;
    int i;
    unsigned si;
    unsigned t;
    unsigned vmmode;
    Xpost_Signature *sp;
    Xpost_Operator *optab;
    Xpost_Operator  op;

    assert(ctx->gl->base);

    optab = xpost_operator_table(ctx->gl);

    if (!(in < XPOST_STACK_SEGMENT_SIZE))
    {
        printf("!(in < XPOST_STACK_SEGMENT_SIZE) in xpost_operator_cons(%s, %d)\n", name, in);
        fprintf(stderr, "!(in < XPOST_STACK_SEGMENT_SIZE) in xpost_operator_cons(%s, %d)\n", name, in);
        exit(EXIT_FAILURE);
    }

    vmmode=ctx->vmmode;
    ctx->vmmode = GLOBAL;
    /* the optab records names by their global index: a locally
       interned name with the same numeric index would alias a
       different operator entirely */
    nm = xpost_name_cons_global(ctx, name);
    if (xpost_object_get_type(nm) == invalidtype)
        return invalid;
    ctx->vmmode = vmmode;

    optab = xpost_operator_table(ctx->gl);
    for (opcode = 0; optab[opcode].name != nm.mark_.padw; opcode++)
    {
        if (opcode == _xpost_noops) break;
    }

    /* install a new signature (prototype) */
    if (fp)
    {
        if (opcode == _xpost_noops)
        { /* a new operator */
            unsigned adr;
            if (_xpost_noops == MAXOPS-1)
            {
                XPOST_LOG_ERR("optab too small in xpost_operator.h");
                XPOST_LOG_ERR("operator %s NOT installed", name);
                return null;
            }
            if (!_optab_alloc(ctx->gl, sizeof(Xpost_Signature), &adr))
            {
                XPOST_LOG_ERR("cannot allocate signature block");
                XPOST_LOG_ERR("operator %s NOT installed", name);
                return null;
            }
            optab = xpost_operator_table(ctx->gl); // recalc
            /* A row goes into virtual memory whole, and a row is wider
               than the fields named below wherever the object it ends
               with wants an alignment they do not reach. Cleared first,
               so that what lands between them is a value this put there
               rather than whatever the call stack was holding. */
            memset(&op, 0, sizeof op);
            op.name = nm.mark_.padw;
            op.n = 1;
            op.sigadr = adr;
            op.proc = null;
            optab[opcode] = op;
            ++_xpost_noops;
            si = 0;
        }
        else
        { /* increase sig table by 1 */
            /* the run of signatures is taken afresh and copied; what it
               had before is left where it is, for the reason given where
               the storage after the rows is described */
            {
                unsigned int oldoff = optab[opcode].sigadr;
                unsigned int oldn = (unsigned int)optab[opcode].n;

                if (!_optab_alloc(ctx->gl,
                                  (oldn + 1u) * (unsigned int)sizeof(Xpost_Signature),
                                  &t))
                {
                    XPOST_LOG_ERR("cannot allocate new sig table");
                    XPOST_LOG_ERR("operator %s NOT installed", name);
                    return null;
                }
                memcpy(_optab_at(ctx->gl, t),
                       _optab_at(ctx->gl, oldoff),
                       oldn * sizeof(Xpost_Signature));
                /* and the run it came from is cleared as it is left. A
                   signature carries two host function addresses, and an
                   abandoned run is storage no operator names any longer:
                   the writer of a memory image zeroes the addresses in
                   the runs the rows point at, and would pass over these.
                   An image is meant to be readable by another process,
                   so a copy of this one's code left anywhere in it is a
                   copy that gets read as a function there. */
                memset(_optab_at(ctx->gl, oldoff), 0,
                       oldn * sizeof(Xpost_Signature));
            }
            optab = xpost_operator_table(ctx->gl); // recalc
            optab[opcode].sigadr = t;

            si = optab[opcode].n++; /* index of last sig */
        }

        sp = _optab_at(ctx->gl, optab[opcode].sigadr);
        {
            unsigned int ad;
            if (!_optab_alloc(ctx->gl, (unsigned int)in, &ad))
            {
                XPOST_LOG_ERR("cannot allocate type block");
                XPOST_LOG_ERR("operator %s NOT installed", name);
                return null;
            }
            optab = xpost_operator_table(ctx->gl); // recalc
            sp = _optab_at(ctx->gl, optab[opcode].sigadr); // recalc
            sp[si].t = ad;
        }
        {
            va_list args;
            byte *b = _optab_at(ctx->gl, sp[si].t);
            va_start(args, in);
            for (i = in-1; i >= 0; i--) {
                b[i] = va_arg(args, int);
            }
            va_end(args);
            sp[si].in = in;
            sp[si].fp = (int(*)(Xpost_Context *))fp;
            sp[si].checkstack = NULL;
            {
                int j;
                int k;
                int pass;
                for (j = 0; j < (int)(sizeof _check_stack_funcs/sizeof*_check_stack_funcs); j++)
                {
                    if (_check_stack_funcs[j].n == sp[si].in)
                    {
                        pass = 1;
                        for (k=0; k < _check_stack_funcs[j].n; k++)
                        {
                            if (b[k] != _check_stack_funcs[j].t[k])
                            {
                                pass = 0;
                                break;
                            }
                        }
                        if (pass)
                        {
                            sp[si].checkstack = _check_stack_funcs[j].checkstack;
                            break;
                        }
                    }
                }
            }
        }
    }
    else if (opcode == _xpost_noops)
    {
        XPOST_LOG_ERR("operator not found");
        return null;
    }

    /* Capture the opcode of any operator the interpreter itself reaches
       for. Doing it here, keyed by the name being registered, is what
       makes the reference table impossible to get wrong: a registration
       cannot forget its capture, and a capture cannot end up holding the
       operator registered on the line above. The cost is one pass of
       first-character comparisons per registration, at startup only. */
#define XPOST_OP_REF_CAPTURE(ref, refname) \
    if (name[0] == (refname)[0] && !strcmp(name, refname)) \
    { \
        XPOST_OP_CODE(ctx, ref) = opcode; \
        if ((size_t)opcode < sizeof ctx->op_inline / sizeof *ctx->op_inline) \
            ctx->op_inline[opcode] = 1; \
    }
    XPOST_OP_REFS(XPOST_OP_REF_CAPTURE)
#undef XPOST_OP_REF_CAPTURE

    /* every field, so that what the object carries is what this says it
       carries wherever the storage it was built in came from */
    o.mark_.tag = operatortype;
    o.mark_.pad0 = 0;
    o.mark_.padw = opcode;
    return o;
}

Xpost_Object xpost_operator_cons_wrapped(Xpost_Context *ctx,
                                         Xpost_Object name,
                                         Xpost_Object proc,
                                         int nsig,
                                         const Xpost_Wrapped_Signature *sigs)
{
    Xpost_Operator *optab;
    Xpost_Operator op;
    Xpost_Object nm;
    Xpost_Object str;
    char buf[128];
    unsigned int len;
    unsigned vmmode;
    int opcode;

    if (xpost_object_get_type(proc) != arraytype)
        return null;

    str = xpost_name_get_string(ctx, name);
    if (xpost_object_get_type(str) != stringtype)
        return null;
    len = str.comp_.sz;
    /* the row records the name as C text, and the operator is thereafter
       known by what the row holds: a name too long for the row, or one
       carrying a nul, would install the operator under a shorter name
       than the one asked for and answer to that instead. A name counts
       its characters (PLRM 3.3), so neither is the name it was given and
       neither is installed */
    if (len > sizeof buf - 1 || !xpost_string_is_cstring(ctx, str))
        return null;
    memcpy(buf, xpost_string_get_pointer(ctx, str), len);
    buf[len] = '\0';

    if (_xpost_noops == MAXOPS-1)
    {
        XPOST_LOG_ERR("optab too small in xpost_operator.h");
        XPOST_LOG_ERR("operator %s NOT installed", buf);
        return null;
    }

    /* the optab records names by their global index: a locally
       interned name with the same numeric index would alias a
       different operator entirely */
    vmmode = ctx->vmmode;
    ctx->vmmode = GLOBAL;
    nm = xpost_name_cons_global(ctx, buf);
    ctx->vmmode = vmmode;
    if (xpost_object_get_type(nm) == invalidtype)
        return invalid;

    /* always a fresh entry: the name may already denote a C operator,
       which lookups by name must keep finding */
    opcode = _xpost_noops;
    optab = xpost_operator_table(ctx->gl);
    /* cleared first, for the reason the other row constructor gives */
    memset(&op, 0, sizeof op);
    op.name = nm.mark_.padw;
    op.n = 0;
    op.sigadr = 0;
    op.proc = proc;
    optab[opcode] = op;
    ++_xpost_noops;

    if (nsig > 0)
    {
        /* the operator states the operands it takes, and the dispatcher
           enforces the statement exactly as it does for one written in
           C: signatures with no function of their own, whose procedure
           runs once one of them has matched. An operator taking more
           than one operand shape states each, and the dispatcher tries
           them in turn, as it does for a C operator installed under
           several prototypes. */
        unsigned int sigadr;
        int s;

        if (!_optab_alloc(ctx->gl,
                          (unsigned int)(nsig * sizeof(Xpost_Signature)), &sigadr))
        {
            XPOST_LOG_ERR("cannot allocate signature block for %s", buf);
            return null;
        }
        for (s = 0; s < nsig; s++)
        {
            unsigned int tadr;
            Xpost_Signature *sig;
            byte *b;
            int in = sigs[s].in;
            int k;

            if (!_optab_alloc(ctx->gl,
                              (unsigned int)(in ? in : 1), &tadr))
            {
                XPOST_LOG_ERR("cannot allocate type block for %s", buf);
                return null;
            }
            /* an allocation moves the file, so every pointer into it is
               taken afresh from its address */
            sig = _optab_at(ctx->gl, sigadr);
            b = _optab_at(ctx->gl, tadr);
            for (k = 0; k < in; k++)
                b[k] = sigs[s].types[k];
            sig[s].in = in;
            sig[s].t = tadr;
            sig[s].fp = NULL;
            sig[s].checkstack = NULL;
        }
        /* the count goes in last: an allocation that failed part way
           leaves the operator stating nothing rather than stating a
           signature that was never filled in */
        optab = xpost_operator_table(ctx->gl);
        optab[opcode].sigadr = sigadr;
        optab[opcode].n = nsig;
    }

    return xpost_operator_cons_opcode(opcode);
}

/* clear hold and pop n objects from opstack to hold stack.
   The hold stack is used as temporary storage to hold the
   arguments for an operator-function call.
   If the operator-function does not itself call xpost_operator_exec,
   the arguments may be restored by xpost_interpreter.c:_onerror().
   xpost_operator_exec checks its argument with ctx->currentobject
   and sets a flag indicating consistency which is then checked by
   on_error()
   Composite Object constructors also add their objects to the
   hold stack, in defense against garbage collection occurring
   from a subsequent allocation before the object is returned
   to the stack.
   on_error() also uses the number of args from ctx->currentobject.mark_.pad0
   instead of the stack count so these extra gc-defense stack objects
   will not be erroneously returned to postscript in response to an
   operator error.
*/
static
void _xpost_operator_push_args_to_hold(Xpost_Context *ctx,
                                       Xpost_Memory_File *mem,
                                       unsigned stacadr,
                                       int n)
{
    int j;
    Xpost_Stack *s;
    Xpost_Stack *hold;

    int k;

    /* the hold stack is cracked as a single segment below: an
       operator's declared arity fits one segment */
    assert(n < XPOST_STACK_SEGMENT_SIZE);

    /* when all args sit in the stack's top segment, copy them into the
       hold segment directly, sparing a segment walk per fetch/push/pop */
    s = xpost_stack_at(mem, stacadr);
    s = xpost_stack_at(mem, s->prevseg); /* load top segment */
    hold = xpost_stack_at(ctx->lo, ctx->hold);
    if ((int)s->top >= n)
    {
        hold->prevseg = ctx->hold;
        s->top -= n;
        for (k = 0; k < n; k++)
            hold->data[k] = s->data[s->top + k];
        hold->top = n;
        return;
    }

    xpost_stack_clear(ctx->lo, ctx->hold);

    for (j = n; j--;)
    {  /* copy */
        xpost_stack_push(ctx->lo, ctx->hold,
                         xpost_stack_topdown_fetch(mem, stacadr, j));
    }
    for (j = n; j--;)
    {  /* pop */
        (void)xpost_stack_pop(mem, stacadr);
    }
}

/* An operator's operands are its caller's to keep: an error leaving the
   operator puts them back (PLRM 3.11.1 step 1). For an operator written
   in C the dispatcher below already holds them -- it took them off the
   operand stack into the hold stack to pass them as arguments. An
   operator written in PostScript is passed nothing: its body reads the
   operand stack itself, and what it consumes there is gone. So the call
   copies them first.

   The copies go into one array per context, kept in privatedict, where
   the collector roots it and a program cannot write it: a composite
   operand stays reachable for as long as it is saved. Slot zero counts
   the slots in use; each live call owns a run above that, and the run is
   named by an array object in the call's frame -- its offset and size
   are the run, so the frame grows by one slot rather than by a base and
   a count. Releasing a run clears it, so nothing stays reachable through
   the array once the call it belongs to is over.

   A call saves the widest operand list an operator can state, and no
   more. That is a bound on the operands rather than a count of them: a
   statement may name fewer operands than its body takes, so the arity
   stated is not the measure. A body that consumes more than the bound
   keeps whatever the truncation leaves it, as every body did before
   there were any copies at all. */
#define XPOST_WRAPPED_SAVE_MAX XPOST_OPERATOR_MAX_SIG

/* The array is a fixed size rather than a growing one, because growth
   would move the runs live calls have already been handed. Nesting
   deeper than it holds saves nothing, which is a weaker guarantee for
   those calls and not a wrong one.

   It is small because it is allocated once and never dies, and local VM
   grows in steps: an array of this size is lost in the noise, while one
   eight times it moved the point at which a long job's VM grew and cost
   a graphics-heavy page a couple of megabytes of peak. The room it does
   hold is a hundred or so nested calls at the operand depths calls are
   really made at, and forty at the widest a call can save. */
#define XPOST_WRAPPED_SAVE_SLOTS 512

/* the context's saved-operand array, made on first use */
static
Xpost_Object _wrapped_save_array(Xpost_Context *ctx)
{
    Xpost_Object arr;

    if (xpost_object_get_type(ctx->privatedict) != dicttype)
        return null;
    if (xpost_object_get_type(ctx->namewrapsave) != nametype)
    {
        ctx->namewrapsave = xpost_name_cons(ctx, ".wrapsave");
        if (xpost_object_get_type(ctx->namewrapsave) != nametype)
            return null;
    }
    arr = xpost_dict_get(ctx, ctx->privatedict, ctx->namewrapsave);
    /* The array must be local. It is dereferenced against local memory
       below and its declared size is the bound the copy is held to, so
       both hold only for a local array: a copy read out of a local
       array is read out of the array it was written for, and bounded by
       that array's own size. A value that is not a local array -- a
       global array among them, whose entity number names a different
       object of a different size in the local table -- is treated as
       none and rebuilt, which is the same rebuild an absent one gets.

       That it must be local is also what it is for: what it holds are
       the operands of calls being made, which may be local objects, and
       a global array may hold none of those. */
    if (xpost_object_get_type(arr) == arraytype &&
        xpost_context_select_memory(ctx, arr) == ctx->lo)
        return arr;
    /* Literal. What this holds is operands, and the tag's flag is LIT, so an
       array built here without saying so is executable -- which would make a
       buffer of saved operands run as a procedure the moment anything named
       it, and makes it answer to a body everywhere bodies are treated apart
       from data. */
    arr = xpost_object_cvlit(xpost_array_cons_memory(ctx->lo,
                                                    XPOST_WRAPPED_SAVE_SLOTS));
    if (xpost_object_get_type(arr) != arraytype)
        return null;
    if (xpost_array_put_memory(ctx->lo, arr, 0, xpost_int_cons(1)) != 0)
        return null;
    if (xpost_dict_put_internal(ctx, ctx->privatedict, ctx->namewrapsave, arr) != 0)
        return null;
    return arr;
}

/* Copy the operands a wrapped call is about to run on, and answer the
   run holding them: a null object where there is nothing to save or
   nowhere to put it, which an unwind reads as no copies taken. */
static
Xpost_Object _wrapped_save_operands(Xpost_Context *ctx)
{
    Xpost_Object arr;
    Xpost_Object *data;
    Xpost_Stack *s;
    int d, n, top;

    d = xpost_stack_count(ctx->lo, ctx->os);
    if (d <= 0)
        return null;
    n = (d < XPOST_WRAPPED_SAVE_MAX) ? d : XPOST_WRAPPED_SAVE_MAX;
    arr = _wrapped_save_array(ctx);
    if (xpost_object_get_type(arr) != arraytype)
        return null;
    data = xpost_ent_ptr_checked(ctx->lo, xpost_object_get_ent(arr));
    if (!data)
        return null;
    if (xpost_object_get_type(data[0]) != integertype)
        return null;
    top = (int)data[0].int_.val;
    if ((top < 1) || (top + n > (int)arr.comp_.sz))
        return null;
    /* written straight into the entity: the copies are the
       interpreter's own bookkeeping and not program-visible VM state,
       so they neither stash for restore nor copy the array on write */
    s = xpost_stack_at(ctx->lo, ctx->os);
    s = xpost_stack_at(ctx->lo, s->prevseg); /* load top segment */
    if ((int)s->top >= n)
        memcpy(data + top, s->data + s->top - n, (size_t)n * sizeof(*data));
    else
    {
        int j;

        for (j = 0; j < n; j++)
            data[top + j] = xpost_stack_topdown_fetch(ctx->lo, ctx->os,
                                                      n - 1 - j);
    }
    data[0] = xpost_int_cons(top + n);
    arr.comp_.off = (word)top;
    arr.comp_.sz = (word)n;
    return arr;
}

void xpost_operator_wrapped_release(Xpost_Context *ctx, Xpost_Object run)
{
    Xpost_Object *data;
    unsigned int slots;
    int base, top, i;

    if (xpost_object_get_type(run) != arraytype)
        return;
    /* The mark this clears back to is element zero of the array the
       copies live in, which is an ordinary array in a dictionary and
       so holds whatever a program last put there. It is bounded by
       what the array behind this run actually holds, not by the size
       the interpreter's own array is built at: a shorter one would
       otherwise be cleared past its end, over whatever the memory
       file holds next. */
    if (!xpost_memory_table_get_size(ctx->lo, xpost_object_get_ent(run),
                                     &slots))
        return;
    slots /= (unsigned int)sizeof(Xpost_Object);
    if (slots < 1)
        return;
    data = xpost_ent_ptr_checked(ctx->lo, xpost_object_get_ent(run));
    if (!data)
        return;
    if (xpost_object_get_type(data[0]) != integertype)
        return;
    base = (int)run.comp_.off;
    top = (int)data[0].int_.val;
    if ((base < 1) || (top > (int)slots))
        return;
    /* everything from this run up belongs to the calls this one
       enclosed: they are over too, whether or not each got to release
       its own */
    for (i = base; i < top; i++)
        data[i] = null;
    data[0] = xpost_int_cons(base);
}

/* execute an operator function by opcode
   the opcode is the payload of an operator object
*/
/* Schedule a wrapped operator's procedure. The call's frame rides the
   exec stack beneath it -- the operator, the operand and dict depths at
   the call, and the operands themselves -- under a finish marker that
   carries them off when the procedure completes. An unwind that
   discards the marker discards the record with it, and the error path
   reads the live records straight off the stack to name this operator,
   put the stack depths back and hand the operands back (see _onerror).
   The operands stay on the operand stack as well: the procedure takes
   them from there itself. */
static
int _exec_wrapped_proc(Xpost_Context *ctx, unsigned opcode, Xpost_Object proc)
{
    Xpost_Object fr[6];

    fr[0] = xpost_int_cons((integer)opcode);
    fr[1] = xpost_int_cons(xpost_stack_count(ctx->lo, ctx->os));
    fr[2] = xpost_int_cons(xpost_stack_count(ctx->lo, ctx->ds));
    fr[3] = _wrapped_save_operands(ctx);
    fr[4] = XPOST_OP(ctx, wrapdone);
    fr[5] = xpost_object_cvx(proc);
    /* the frame goes on as one run: fr is a C array, and the run either
       lodges all six or puts back what it placed, so a refusal leaves
       the exec stack as this call found it */
    if (!xpost_stack_push_run(ctx->lo, ctx->es, fr, 6))
    {
        xpost_operator_wrapped_release(ctx, fr[3]);
        return execstackoverflow;
    }
    return 0;
}

/* Call a compiled operator's function with n operands.

   The cast is what the call costs: the table holds every operator's
   function under one pointer type and each is written with the operands
   its signature declares, so the pointer is cast back to the arity being
   called. The casts live here and nowhere else, so a call written
   anywhere in the tree cannot get one of them wrong. */
static int
_call_fp(Xpost_Context *ctx, Xpost_Op_Func fp, int n, const Xpost_Object *a)
{
    switch (n)
    {
        case 0:
            return ((int(*)(Xpost_Context*))fp)(ctx);
        case 1:
            return ((int(*)(Xpost_Context*,Xpost_Object))fp)
                (ctx, a[0]);
        case 2:
            return ((int(*)(Xpost_Context*,Xpost_Object,Xpost_Object))fp)
                (ctx, a[0], a[1]);
        case 3:
            return ((int(*)(Xpost_Context*,Xpost_Object,Xpost_Object,Xpost_Object))fp)
                (ctx, a[0], a[1], a[2]);
        case 4:
            return ((int(*)(Xpost_Context*,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object))fp)
                (ctx, a[0], a[1], a[2], a[3]);
        case 5:
            return ((int(*)(Xpost_Context*,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object))fp)
                (ctx, a[0], a[1], a[2], a[3], a[4]);
        case 6:
            return ((int(*)(Xpost_Context*,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object))fp)
                (ctx, a[0], a[1], a[2], a[3], a[4], a[5]);
        case 7:
            return ((int(*)(Xpost_Context*,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object))fp)
                (ctx, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
        case 8:
            return ((int(*)(Xpost_Context*,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object,Xpost_Object))fp)
                (ctx, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        default:
            return unregistered;
    }
}

/* Bind a compiled operator so it can be called without the operand
   stack, or answer NULL where it cannot be.

   An operator's operands normally arrive by being pushed onto the
   operand stack, matched against the signature's declared types, and
   moved to the hold stack. That is what makes an operator callable by a
   program, and it is the right price for a call a program made. It is
   the wrong price for a call the machinery makes to a compiled method
   whose operands it built itself: the marshalling states, at run time
   and per call, what the caller already knows.

   So the knowing is done once, here, and the calling is done by
   xpost_operator_call_direct. What is checked is everything the per-call
   matching would have established: that the operator carries exactly one
   signature, that it is implemented in C rather than by a recorded
   procedure, that it takes the arity offered, and that the types it
   declares accept the operands offered -- read off a sample the caller
   will go on to pass, so a device declaring its method with other types
   is refused here and takes the ordinary path instead.

   A signature with a stack-checking function of its own is refused: such
   a function reads the operand stack, which a direct call does not
   fill. */
Xpost_Op_Func
xpost_operator_direct(Xpost_Context *ctx,
                      unsigned opcode,
                      int n,
                      const Xpost_Object *sample)
{
    Xpost_Operator *optab;
    Xpost_Operator op;
    Xpost_Signature *sp;
    byte *t;
    int j;

    if (n <= 0 || n > XPOST_OPERATOR_MAX_SIG)
        return NULL;
    optab = xpost_operator_table(ctx->gl);
    op = optab[opcode];
    if (op.n != 1)
        return NULL;
    sp = _optab_at(ctx->gl, op.sigadr);
    if (!sp->fp || sp->in != n || sp->checkstack)
        return NULL;
    /* The declared types, read against the sample the same way the
       per-call matching reads them against the stack: top-down, so the
       last operand offered is the first type declared. Only the patterns
       that accept an operand as it stands are taken. floattype is not
       one of them -- it accepts an integer by promoting it in place on
       the operand stack, which is a rewrite of an operand a direct call
       does not have to rewrite -- so an operator declaring one is left
       to the ordinary path rather than being called with an operand it
       would have changed. */
    t = _optab_at(ctx->gl, sp->t);
    for (j = 0; j < n; j++)
    {
        int ty = xpost_object_get_type(sample[n - 1 - j]);

        if (t[j] == anytype)
            continue;
        if (t[j] == ty)
            continue;
        if ((t[j] == numbertype) &&
            ((ty == integertype) || (ty == realtype)))
            continue;
        if ((t[j] == floattype) && (ty == realtype))
            continue;
        return NULL;
    }
    return sp->fp;
}

/* Call what xpost_operator_direct bound, with operands the caller holds.

   The bookkeeping kept is the part that is about the call and not about
   the stack. The hold stack is not filled, so the record saying an
   error may take the operands back from it is cleared rather than set;
   the checks after the call are the ones every operator is read for,
   and are kept because they are how an operator that failed without
   returning a failure is caught. */
int
xpost_operator_call_direct(Xpost_Context *ctx,
                           Xpost_Op_Func fp,
                           int n,
                           const Xpost_Object *a)
{
    int ret;

    ctx->op_restore_n = 0;
    ctx->opargsinhold = 0;
    ret = _call_fp(ctx, fp, n, a);
    if (ret)
        return ret;
    if (ctx->callback_error)
    {
        ret = (int)ctx->callback_error;
        ctx->callback_error = 0;
        return ret;
    }
    if (ctx->lo->push_refused)
    {
        ctx->lo->push_refused = 0;
        return VMerror;
    }
    return 0;
}

int xpost_operator_exec(Xpost_Context *ctx,
                        unsigned opcode)
{
    Xpost_Operator *optab;
    Xpost_Operator op;
    Xpost_Signature *sp;
    int i,j;
    int pass;
    int err = unregistered;
    Xpost_Stack *hold;
    Xpost_Stack *os_root;
    Xpost_Stack *os_top;
    int ct;
    int ret;

    ctx->op_restore_n = 0;

    optab = xpost_operator_table(ctx->gl);
    op = optab[opcode];
    sp = _optab_at(ctx->gl, op.sigadr);

    /* An operator stating one signature that takes nothing and is
       implemented by a C function needs none of the matching below: no
       operand count, no type pattern, no operands moved to the hold
       stack. Nearly half the calls a rendering job makes arrive here --
       the looping operators reschedule their step operators through the
       execution stack every iteration -- so this case keeps only what
       the full path gives it: the currentobject record _onerror reads,
       a hold stack left empty exactly as the argument mover leaves it
       for a zero-operand call (composite constructors push their
       collector-defense objects there), and the shared post-call checks.
       Registration gives every such signature _stack_none as its
       stack-checking function, which accepts unconditionally, so it is
       recognised here rather than called. */
    if ((op.n == 1) && (sp->in == 0) && (sp->fp != NULL) &&
        ((sp->checkstack == NULL) || (sp->checkstack == _stack_none)))
    {
        if ((ctx->currentobject.tag == operatortype) &&
            (ctx->currentobject.mark_.padw == opcode))
        {
            ctx->currentobject.mark_.pad0 = 0;
            ctx->opargsinhold = 1;
        }
        else
        {
            ctx->opargsinhold = 0;
        }
        hold = xpost_stack_at(ctx->lo, ctx->hold);
        hold->prevseg = ctx->hold;
        hold->top = 0;
        ret = ((int(*)(Xpost_Context*))sp->fp)(ctx);
        goto post;
    }

    /* a signature states at most XPOST_OPERATOR_MAX_SIG operands, so ct
       only needs to reach that many; no full segment walk is needed. The
       top segment settles it: that many or more in it, or -- when a full
       segment (never partial) sits below it -- at least SEGMENT_SIZE,
       likewise enough. Only a lone segment can hold fewer, and then its
       own top is the count. */
    os_root = xpost_stack_at(ctx->lo, ctx->os);
    os_top = xpost_stack_at(ctx->lo, os_root->prevseg);
    ct = (os_top->top >= XPOST_OPERATOR_MAX_SIG) ? XPOST_OPERATOR_MAX_SIG
        : (os_top == os_root) ? (int)os_top->top
        : XPOST_OPERATOR_MAX_SIG;
    if (op.n == 0)
    {
        /* a wrapped operator carries no C signatures: it runs its
           recorded procedure, which checks its own operands. The
           call's frame rides the exec stack beneath the procedure --
           the operator, the operand and dict depths at the call, and
           the operands themselves -- under a finish marker that
           carries them off when the procedure completes. An unwind
           that discards the marker discards the record with it, and
           the error path reads the live records straight off the
           stack to name this operator, put the stack depths back and
           hand the operands back (see _onerror) */
        if (xpost_object_get_type(op.proc) == arraytype)
            return _exec_wrapped_proc(ctx, opcode, op.proc);
        XPOST_LOG_ERR("operator has no signatures");
        return unregistered;
    }
    for (i =0 ; i < op.n; i++)
    { /* try each signature */
        byte *t;

        /* call signature's stack-checking proc, if available */
        if (sp[i].checkstack)
        {
            if ((ret = sp[i].checkstack(ctx)))
            {
                err = ret;
                continue;
            }
            goto call;
        }

        /* check stack size */
        if (ct < sp[i].in)
        {
            pass = 0;
            /* a higher-arity signature that lacks operands must not mask a
               type mismatch already found against a signature whose arity
               was satisfied: a wrong-typed operand is a typecheck, not a
               stackunderflow */
            if (err != typecheck)
                err = stackunderflow;
            continue;
        }

        /* check type-pattern against stack */
        pass = 1;
        t = _optab_at(ctx->gl, sp[i].t);
        for (j=0; j < sp[i].in; j++)
        {
            Xpost_Object el = (j < (int)os_top->top)
                ? os_top->data[os_top->top - 1 - j]
                : xpost_stack_topdown_fetch(ctx->lo, ctx->os, j);
            if (t[j] == anytype)
                continue;
            if (t[j] == xpost_object_get_type(el))
                continue;
            if ((t[j] == numbertype) &&
                (((xpost_object_get_type(el) == integertype) ||
                  (xpost_object_get_type(el) == realtype))))
                continue;
            if (t[j] == floattype)
            {
                if (xpost_object_get_type(el) == integertype)
                {
                    _op_restore_note(ctx, j, el);
                    el = _promote_integer_to_real(el);
                    if (j < (int)os_top->top)
                        os_top->data[os_top->top - 1 - j] = el;
                    else if (!xpost_stack_topdown_replace(ctx->lo, ctx->os, j, el))
                        return unregistered;
                    continue;
                }
                if (xpost_object_get_type(el) == realtype)
                    continue;
            }
            if ((t[j] == proctype) &&
                (xpost_object_get_type(el) == arraytype) &&
                xpost_object_is_exe(el))
                continue;
            pass = 0;
            err = typecheck;
            break;
        }

        if (pass) goto call;
    }
    /* no signature matched: a rejected trial may have coerced an operand from
       integer to real before failing. The operator never ran, so the operands
       are still on the stack; put back the integers the program pushed. */
    for (i = 0; i < ctx->op_restore_n; i++)
        /* each index was reached when the note was taken, and the trial
           that failed left the stack as it found it */
        XPOST_REFUSAL_IMPOSSIBLE(
            xpost_stack_topdown_replace(ctx->lo, ctx->os,
                                        ctx->op_restore_idx[i],
                                        ctx->op_restore_val[i]));
    return err;

  call:
    /* a signature whose procedure is written in PostScript: the types
       have matched, and the body takes the operands from the stack */
    if (!sp[i].fp && (xpost_object_get_type(op.proc) == arraytype))
        return _exec_wrapped_proc(ctx, opcode, op.proc);

    /* If we're executing the context's "currentobject",
       set the number of arguments consumed in the pad0 of currentobject,
       and set a flag declaring that this has been done.
       This is so onerror() can reset the stack
       (if hold has not been clobbered by another call to xpost_operator_exec).
    */
    if ((ctx->currentobject.tag == operatortype) &&
        (ctx->currentobject.mark_.padw == opcode))
    {
        ctx->currentobject.mark_.pad0 = sp[i].in;
        ctx->opargsinhold = 1;
    }
    else
    {
        /* Not executing current op.
           HOLD may *not* be assumed to contain currentobject's arguments.
           clear the flag.
        */
        ctx->opargsinhold = 0;
    }

    _xpost_operator_push_args_to_hold(ctx, ctx->lo, ctx->os, sp[i].in);
    hold = xpost_stack_at(ctx->lo, ctx->hold);

    ret = _call_fp(ctx, sp[i].fp, sp[i].in, hold->data);  post:
    if (ret)
        return ret;
    /* A stream backed by a procedure answers a read with end of data and
       a write with a refusal, and carries the reason on the context.
       The failure belongs to the operator that reached through the
       stream, which is this one: the procedure ran inside this call. */
    if (ctx->callback_error)
    {
        ret = (int)ctx->callback_error;
        ctx->callback_error = 0;
        return ret;
    }
    /* An operator that pushed its result onto a stack that would not take
       it has finished without producing what it answers for. The push
       sites do not carry that back -- there are several hundred of them
       -- so it is read here, once, for every operator alike, while the
       operator that did the pushing is still the one an error would be
       reported against. */
    if (ctx->lo->push_refused)
    {
        ctx->lo->push_refused = 0;
        return VMerror;
    }
    return 0;
}
