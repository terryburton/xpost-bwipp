/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_GC_H
#define XPOST_GC_H

/**
 * @file xpost_garbage.h
 * @brief The Garbage Collector
 */


/**
 * @brief  Perform a garbage collection on mfile.
 *
 * dosweep controls whether a sweep is performed; if not, this
 * is just a marking operation. markall controls whether
 * collect() should follow links across vm boundaries.
 *
 * For a local vm, dosweep should be 1 and markall should be 0.
 * For a global vm, dosweep should be 1 and markall should be 1.
 *
 * For a global vm, collect() calls itself recursively upon each
 * associated local vm, with dosweep = 0, markall = 1.
 *
 * returns size collected or -1 if error occurred. A collection that
 * cannot mark its roots returns before its sweep and reclaims nothing,
 * and the next one refuses in the same place, so a caller that reads
 * the answer is the only thing between that and a run whose memory
 * management has silently stopped.
 */
/**
 * @brief which banks a collection reclaims.
 *
 * Marking crosses both banks whenever it is asked to, because an object
 * in one may be named from the other; what a collection then reclaims is
 * a separate choice, and a bank may only be reclaimed by a collection
 * that marked it. PLRM 8.2's vmreclaim distinguishes the two banks, so
 * the caller says which it means.
 */
#define XPOST_GARBAGE_SWEEP_NONE   0
#define XPOST_GARBAGE_SWEEP_LOCAL  1
#define XPOST_GARBAGE_SWEEP_GLOBAL 2
#define XPOST_GARBAGE_SWEEP_BOTH   (XPOST_GARBAGE_SWEEP_LOCAL | \
                                    XPOST_GARBAGE_SWEEP_GLOBAL)

/**
 * @brief which banks a collection running of its own accord reclaims
 *
 * Both, unless a program has turned automatic collection off for one of
 * them through vmreclaim (PLRM 8.2).
 */
int xpost_garbage_auto_banks(Xpost_Context *ctx);

/**
 * @brief say which banks a collection running of its own accord reclaims
 *
 * The setting vmreclaim writes and restore puts back, that setting being
 * the whole of the VMReclaim user parameter (PLRM C.3.5).
 */
void xpost_garbage_auto_banks_set(Xpost_Context *ctx, int banks);

XPOST_MUST_CHECK int xpost_garbage_collect(Xpost_Memory_File *mem,
                                           int dosweep,
                                           int markall);

/*
 * The environment-gated collector diagnostics (xpost_garbage_diag.c):
 * the independent reachability verifier (XPOST_GC_VERIFY, with an
 * entity census under XPOST_GC_CENSUS) and the cross-bank scan for
 * global containers referencing a dying local entity
 * (XPOST_GC_XBANK_CHECK). The collector calls them only when the
 * variable is set.
 */
void _xpost_garbage_diag_verify(Xpost_Context *ctx, Xpost_Memory_File *mem,
                                int bothbanks);
void _xpost_garbage_diag_xbank(Xpost_Context *ctx, Xpost_Memory_File *mem);



#endif
