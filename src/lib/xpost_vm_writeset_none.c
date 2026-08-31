/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2024 Michael Joshua Ryan
 * Copyright (c) 2013-2024 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_vm_writeset_none.c
 * @brief Which pages of a bank a job wrote: not answerable here.
 */

/* The hosts that have not been asked the question.

   Linux, Windows and Darwin each answer it, in three different ways and
   for three different reasons; every other host arrives here, and the
   caller copies the whole baseline, which is what is always right.

   Nothing here is a stub for want of trying, and a host reaching it is
   not thereby judged incapable: it is a host whose way of answering has
   not been found and measured. What the other three have in common is
   that the answer had to be measured rather than reasoned about --
   Darwin's costs three times the copy it saves at one worker and less
   than it at eight, and the obvious Windows answer was not merely slow
   but wrong -- so a fourth should be added the same way, with the
   measurement in hand, rather than by pattern-matching one of theirs.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_vm_writeset.h"

int xpost_vm_writeset_begin(Xpost_Memory_File *mem,
                            const unsigned char *baseline, size_t used)
{
    (void)mem; (void)baseline; (void)used;
    return 0;
}

int xpost_vm_writeset_restore(Xpost_Memory_File *mem,
                              const unsigned char *baseline, size_t used)
{
    (void)mem; (void)baseline; (void)used;
    return 0;
}

void xpost_vm_writeset_end(Xpost_Memory_File *mem)
{
    (void)mem;
}

void xpost_vm_writeset_forget(Xpost_Memory_File *mem)
{
    (void)mem;
}
