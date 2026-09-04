/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_ITP_H
#define XPOST_ITP_H

#include <stddef.h> /* size_t */

#include "xpost_private.h" /* XPOST_TEST_VISIBLE */

/**
 * @file xpost_interpreter.h
 * @brief the interpreter functions
 *
 * The interpreter module manages the itpdata structure, allocating
 * contexts from a table, and allocating memory files to the contexts
 * also from tables. The itpdata structure thus encapsulates the entire
 * dynamic state of the interpreter as a whole.
 *
 * The interpreter module also contains functions for eval actions,
 * the core interpreter loop,
 *
 * @{
 */

/*# define MAXCONTEXT 10 // moved to xpost_context.h. <- must include first!
 */
#define MAXMFILE 10

/* The stacks grow by VM segments without any structural bound, so a
   runaway loop or recursion would grind through memory rather than
   fail. Execution past these depths raises the stack's overflow
   error, checked at the two places depth accumulates: evalarray's
   internal procedure call and the interpreter loop. A latch per
   stack raises once per crossing, so the error machinery runs (and
   the program recovers) above the ceiling without retriggering it,
   and rearms when the depth recedes. The ceilings sit far beyond any
   legitimate job's depth while keeping the error path's walk over
   the stacks cheap. The exec ceiling leaves room for the
   deferred-paint queues the devices stage there: a vector device
   decomposes a large fill into very many queued spans.

   They are what currentuserparams reports for MaxExecStack, MaxOpStack
   and MaxDictStack (PLRM C.3.5), so what a program is told its stacks
   hold is what this interpreter holds them to. */
#define XPOST_EXEC_STACK_LIMIT 1000000
#define XPOST_OPER_STACK_LIMIT 1000000
#define XPOST_DICT_STACK_LIMIT 5000

typedef struct
{
    Xpost_Context ctab[MAXCONTEXT];
    unsigned int cid;
    Xpost_Memory_File gtab[MAXMFILE];
    Xpost_Memory_File ltab[MAXMFILE];
    int in_onerror;
} Xpost_Interpreter;


extern Xpost_Interpreter *itpdata;

/* garbage collection does not run during initializing */
int xpost_interpreter_get_initializing(void);
void xpost_interpreter_set_initializing(int i);

Xpost_Context *xpost_interpreter_cid_get_context(unsigned int cid);

/**
 * The event-handler handler.

 * In a multi-threaded configuration, this may not execute at in every eval()
 * but by a superior strategy.
 */
int idleproc(Xpost_Context *ctx);

extern int _xpost_interpreter_is_tracing;

/**
 * @brief Give the operands of the wrapped-operator calls being
 *        abandoned back to their caller, and drop what those calls
 *        left (PLRM 3.11.1 step 1).
 *
 * The interpreter does this itself for an error it raises, before it
 * records the command in $error. The PostScript error hook asks for it
 * on behalf of an error a body raised with signalerror, whose stop
 * never reaches the interpreter's handler, and stop asks for it for
 * the calls it abandons, which covers a body that caught a failure in
 * a stopped context of its own and raised it again with a bare stop,
 * passing no hook at all. Reading the frames does not spend them, so
 * the paths that ask early and the stop that ends them all reach the
 * same state. The walk ends at the boundary an operator leaves under a
 * procedure of the program's that it calls back into: a failure in
 * there is that procedure's, and the calls beneath it keep what they
 * consumed.
 */
int xpost_op_errorunwind(Xpost_Context *ctx);

int xpost_interpreter_init(Xpost_Interpreter *itp, const char *device);
void xpost_interpreter_exit(Xpost_Interpreter *itp);

/**
 * @brief Load the language into the context.
 *
 * The first of the two steps a run brings a context up with before the
 * program it was given: the modules are read and the interpreter is
 * locked down, and what stands afterwards is the language -- the same
 * names with the same values however the run was started. The second
 * step, making the device this run was started with, is what settles
 * something of the run, and it is not done here.
 *
 * A run does this itself for the context it was handed, so a caller
 * that only runs programs never needs it. It is separate for the sake
 * of the point between the two steps, which is where a context's
 * virtual memory is a picture of the language and of nothing else.
 *
 * The load runs once in the life of a context; whether it succeeded is
 * read from the context afterwards.
 */
XPOST_TEST_VISIBLE void xpost_interpreter_load_language(Xpost_Context *ctx);

/**
 * @brief where the boot files are, or the empty string.
 *
 * What is looked for is init.ps, which is the file a boot begins with
 * and the one every other is reached from. Asked for by the boot that
 * runs those files and by the image of virtual memory that stamps what
 * it holds with what they say, so that both are asking about the same
 * directory.
 */
XPOST_TEST_VISIBLE void xpost_interpreter_data_dir(char *datadir,
                                                   size_t datadirsz);

/**
 * @brief say the boot files may be read, whichever way the language arrives.
 */
void xpost_interpreter_permit_data_dir(const char *datadir);

/**
 * @brief Run a procedure to completion and return, so a C caller
 *        reaches a procedure of the program's as an ordinary call.
 *
 * The procedure runs on the stacks of the surrounding run, stepping the
 * evaluator until the execution stack falls back to the depth it
 * started at. Callers are operators that need the procedure's answer
 * to finish their own work, such as a filter refilling from a data
 * source.
 *
 * An error inside the procedure is raised here. It runs under the
 * boundary an operator leaves beneath a call into the program's own
 * code, so the operands restored are the procedure's rather than those
 * of the operator that called in (PLRM 3.11.1).
 *
 * The nesting is bounded. The depth is the program's to choose, and one
 * past what the C stack carries is limitcheck.
 *
 * @return 0, or the error the run could not handle.
 */
int xpost_interpreter_run_nested(Xpost_Context *ctx, Xpost_Object P);

/**
 * @}
 */

#endif
