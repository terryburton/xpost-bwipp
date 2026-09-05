/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_OPERATOR_H
#define XPOST_OPERATOR_H

/**
 * @file xpost_operator.h
 * @brief operator functions
 *
 * This module is the operator interface.
 * It defines the operator constructor xpost_operator_cons,
 * and the operator handler function xpost_operator_exec.
 * xpost_operator_init_optab is called to initialize the optab structure itself.
 * xpost_oplib.c:initop is called to populate the optab structure.
 *
 * nb. Since xpost_operator_cons does a linear search through the optab,
 * an obvious optimisation would be to factor-out calls to
 * xpost_operator_cons from main-line code. Pre-initialize an object
 * somewhere and re-use it where needed. xpost2 did this with
 * a global struct of "opcuts" (operator object shortcuts),
 * but here it would need to be "global", either in global-vm
 * or in the context struct.
 * One goal of the planned "quick-launch" option is to remove
 * these lookups from the initialization, too. One requirement
 * for the quick-launch is removing all function-pointers from vm.
 *
 * ----
 * To speed-up typechecks,
 * a `int (*check)()` function pointer is added to the signature
 * which directly implements the stack-checking performed by
 * xpost_operator_exec (but without the nasty loops).
 * xpost_operator_exec would then call op.sig[i].check() if not null.
 *
 * @{
 */

/**
 * @brief a "generic" function pointer for operator functions
 *
 * The argument list is deliberately left unspecified: every operator has a
 * different concrete signature, and each is cast to this one type and invoked
 * through it. A (void) prototype would forbid those calls, so the empty list
 * is required here and -Wstrict-prototypes is suppressed only at this site.
 */
#if defined(__GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif
typedef int (*Xpost_Op_Func)();
#if defined(__GNUC__)
# pragma GCC diagnostic pop
#endif

/**
 * @brief operator signature structure
 *
 * A signature contains a stack-pattern, an optional stack-checking
 * function, and an operator function.
 */
typedef struct Xpost_Signature
{
    Xpost_Op_Func fp;  /* function-pointer which implements the operator action */
    int in;       /* number of argument objects */
    unsigned t;   /* the argument types, as an offset within the operator
                     table's own entity rather than a place in the arena */
    int (*checkstack)(Xpost_Context *ctx);  /* stack-checking function to bypass generic type-check loop */
} Xpost_Signature;

/**
 * @brief operator structure
 *
 * An operator structure, which inhabits the operator table,
 * contains a "pointer" to an array of signatures
 * and the length of that array.
 */
typedef struct Xpost_Operator
{
    unsigned name;   /* name-stack index of operator's name */
    int n;           /* number of signatures */
    unsigned sigadr; /* the run of signatures, as an offset within the
                        operator table's own entity */
    Xpost_Object proc; /* procedure a wrapped operator runs (n == 0);
                          the null object otherwise */
} Xpost_Operator;


/**
 * @brief the type pattern enum
 *
 * extend the type enum with "pattern" types
 * anytype matches any object type
 * floattype matches reals and promotes ints to reals
 * numbertype matches reals and ints
 * proctype matches arrays with executable attribute set
 */
enum typepat
{
    anytype = XPOST_OBJECT_NTYPES /* one past the last object type */,
    floattype,
    numbertype,
    proctype };

/**
 * @def XPOST_OPERATOR_MAX_SIG
 * @brief the most operands a signature may state
 *
 * Wide enough for the longest operand list in the language, the
 * twelve of setcolorscreen.
 */
#define XPOST_OPERATOR_MAX_SIG 12

/**
 * @def XPOST_OPERATOR_MAX_ALT
 * @brief the most operand shapes one operator may state
 */
#define XPOST_OPERATOR_MAX_ALT 8

/**
 * @brief constant size of optab structure
 */
#define MAXOPS 1024

/**
 * @brief initial size of systemdict (which then grows, automatically)
 */
#define SDSIZE 10

/**
 * @brief allocate the optab structure
 */
int xpost_operator_init_optab(Xpost_Context *ctx);

/* Fill a row of the table from an image rather than by installing an
   operator: room for its shapes, then each shape's operand types. */
int xpost_operator_take_signatures(Xpost_Memory_File *gl, unsigned int k,
                                   unsigned int n);
int xpost_operator_set_signature(Xpost_Memory_File *gl, unsigned int k,
                                 unsigned int si, unsigned int in,
                                 const unsigned char *types);

/* Keep the table as it stands and put it back: the job boundary reverts
   the banks, and a row holds objects of the global bank. */
int xpost_operator_table_snapshot(Xpost_Memory_File *gl,
                                  unsigned char **buf, unsigned int *len);
int xpost_operator_table_restore(Xpost_Memory_File *gl,
                                 const unsigned char *buf, unsigned int len);

/**
 * @brief output a text dump of the operator contents
 */
void xpost_operator_dump(Xpost_Context *ctx, int opcode);

/**
 * @brief construct an operator object by opcode
 */
Xpost_Object xpost_operator_cons_opcode(int opcode);

/**
 * @def XPOST_OP
 * @brief the operator object a reference-table entry names
 *
 * The way C schedules a standard operator. The entry is one of those
 * listed by XPOST_OP_REFS in xpost_context.h, so a misspelling is a
 * compile error rather than a null object pushed on the execution stack,
 * and the operator is settled here rather than looked up by the
 * dictionary stack when the step runs.
 */
#define XPOST_OP(ctx, ref) xpost_operator_cons_opcode(XPOST_OP_CODE(ctx, ref))

/**
 * @brief construct an operator object by name,
 *        possibly installing a new operator
 */
Xpost_Object xpost_operator_cons(Xpost_Context *ctx,
                                 const char *name,
                                 /*@null@*/ Xpost_Op_Func fp,
                                 int in,
                                 ...);

/**
 * @brief one operand shape a wrapped operator accepts
 *
 * The types are held as the dispatcher reads them, from the top of the
 * operand stack down.
 */
typedef struct
{
    int in;                              /**< operands taken */
    byte types[XPOST_OPERATOR_MAX_SIG];  /**< one type per operand */
} Xpost_Wrapped_Signature;

/**
 * @brief construct an operator that runs a procedure
 *
 * Installs a fresh operator-table entry under the given name whose
 * execution pushes the procedure on the execution stack. A procedure
 * that implements a standard operator becomes indistinguishable from
 * one coded in C: load answers operatortype and bind substitutes it.
 * The caller must keep the procedure reachable by the collector.
 *
 * The signatures state the operand shapes the operator accepts, and
 * the dispatcher tries them in the order given, running the procedure
 * once one has matched. An operator that states none is dispatched
 * unchecked and answers for its own operands.
 */
Xpost_Object xpost_operator_cons_wrapped(Xpost_Context *ctx,
                                         Xpost_Object name,
                                         Xpost_Object proc,
                                         int nsig,
                                         const Xpost_Wrapped_Signature *sigs);

/**
 * @brief execute an operator
 */
Xpost_Op_Func xpost_operator_direct(Xpost_Context *ctx,
                                    unsigned opcode,
                                    int n,
                                    const Xpost_Object *sample);

int xpost_operator_call_direct(Xpost_Context *ctx,
                               Xpost_Op_Func fp,
                               int n,
                               const Xpost_Object *a);

int xpost_operator_exec(Xpost_Context *ctx,
                        unsigned opcode);

/**
 * @brief let go of the operands saved for a wrapped-operator call.
 *
 * @p run is the array object a call's frame carries, naming the operands
 * copied when the call was made. Every way a call ends passes here: the
 * finish marker when its procedure completes, and each unwinder for a
 * call the unwinding discards. Releasing a call's operands releases
 * those of every call it enclosed.
 */
void xpost_operator_wrapped_release(Xpost_Context *ctx, Xpost_Object run);

/**
 * @brief the operator table of @p gl.
 *
 * The one derivation of the table's pointer. It lives in global memory as
 * a special entity built once by xpost_operator_init_optab, so its address
 * is total; the pointer, though, is only good until the next allocation in
 * @p gl, which may move the memory file. Every use re-derives it rather
 * than holding one across a call that can allocate.
 */
/**
 * @brief how many operators the table holds
 */
unsigned int xpost_operator_count(void);

/**
 * @brief say how many operators the table holds.
 *
 * For a context whose operator table was not built here but arrived
 * whole, in an image of the virtual memory it lives in. The rows come
 * with the memory and the count does not, so the count is told.
 */
void xpost_operator_set_count(unsigned int count);

static inline Xpost_Operator *
xpost_operator_table(Xpost_Memory_File *gl)
{
    return (Xpost_Operator *)(void *)gl->optab;
}

/**
 * @brief helper macro for installing an operator
 *
 * The INSTALL macro
 * 1. establishes that @p op is an operator at all
 * 2. refreshes the optab pointer
 * 3. extracts the name index from the operator referred to by object op
 * 4. constructs a name object n
 * 5. defines the name/operator-object pair in systemdict
 * 6. refreshes the optab pointer yet again
 *
 * A systemdict that would not take the pair leaves the interpreter
 * without that operator, which no program can work around. The
 * registrations are several hundred calls across two dozen modules, so
 * rather than each one carrying the answer back by hand the refusal is
 * recorded on the context and xpost_oplib_init_ops reads it once, after
 * they have all run.
 *
 * The first step is there because xpost_operator_cons has five ways to
 * refuse -- a name it could not intern, an operator table with no room,
 * and three allocations global VM can decline -- and answers null for
 * four of them and invalid for the other. Neither carries an operator
 * number: both are file scope objects whose unwritten members are zero,
 * so indexing the table with one reads entry zero, which belongs to the
 * first operator ever registered. Installing under that entry's name
 * replaces an operator every program uses with the object cons would not
 * make, and replacing an entry already in systemdict allocates nothing,
 * so the store succeeds and there is nothing for the record below to
 * catch. Testing the type covers all five refusals at once, which
 * testing for invalid alone would not.
 *
 * The key is a literal name, which is the attribute every other key in
 * the dictionary has: the operator's name is not being executed here,
 * it is being stored, and what forall hands back is the object stored.
 */
#define INSTALL \
    do { \
        if (xpost_object_get_type(op) != operatortype) \
            ctx->operator_install_refused = 1; \
        else \
        { \
            optab = xpost_operator_table(ctx->gl); \
            n.mark_.tag = nametype|XPOST_OBJECT_TAG_DATA_FLAG_LIT|XPOST_OBJECT_TAG_DATA_FLAG_BANK; \
            n.mark_.pad0 = 0; \
            n.mark_.padw = optab[op.mark_.padw].name; \
            if (xpost_dict_put(ctx, sd, n, op)) \
                ctx->operator_install_refused = 1; \
        } \
        optab = xpost_operator_table(ctx->gl); /* recalc */ \
    } while (0)

/**
 * @}
 */

#endif
