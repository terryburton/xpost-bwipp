/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2024 Michael Joshua Ryan
 * Copyright (c) 2013-2024 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_vm_writeset_win32.c
 * @brief Which pages of a bank a job wrote, on Windows.
 */

/* Windows keeps the record itself. Address space reserved with
   MEM_WRITE_WATCH has the pages written to it recorded by the memory
   manager, and GetWriteWatch reports and clears that record. So nothing
   here maps the bank differently: the bank stays exactly the reservation
   xpost_memory_file_init made of it, and only the question is asked.
   That is why this is shorter than the other host's answer, and why a
   grow needs nothing from it -- growth commits more of the same
   reservation, and the record spans the whole of it.

   MEASURED, over the shape of this workload -- a bank of 352 pages,
   eight of them written -- putting back only the written pages costs
   0.0100 ms against 0.0463 ms to copy the whole bank.

   The record is the memory manager's, not an inference from what happens
   to be resident, and that is what makes it usable: PROVEN by emptying
   the process working set under the written pages and asking again,
   which still named exactly the pages written. The residency-based
   answer, QueryWorkingSetEx reporting a page that has stopped being
   shared, does not survive that -- after the same trim it reported no
   written pages at all while the writes were still there, which as a
   revert would have left a job's bytes in the bank. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h>
#include <windows.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_error.h"
#include "xpost_memory.h"
#include "xpost_vm_writeset.h"

int xpost_vm_writeset_begin(Xpost_Memory_File *mem,
                            const unsigned char *baseline, size_t used)
{
    ULONG_PTR count;
    ULONG granularity;
    PVOID one;

    if (!mem || !mem->base || !baseline || !used)
        return 0;
    /* A bank whose writes have to reach a file is not one of these: the
       reservation this asks about is made only for a bank with no
       backing file. */
    if (mem->fd != -1)
        return 0;
    if (!xpost_memory_page_size || used > mem->max)
        return 0;

    /* Ask the record a question to find out whether there is one. A
       reservation made without MEM_WRITE_WATCH refuses, which is the
       answer on a build or a run where tracking is off. */
    count = 1;
    if (GetWriteWatch(0, mem->base, mem->max, &one, &count, &granularity) != 0)
        return 0;

    if (ResetWriteWatch(mem->base, mem->max) != 0)
        return 0;

    mem->writeset.tracking = 1;
    mem->writeset.len = mem->max;
    mem->writeset.used = used;
    mem->writeset.back_lo = mem->writeset.back_hi = 0;
    mem->writeset.against = baseline;
    return 1;
}

int xpost_vm_writeset_restore(Xpost_Memory_File *mem,
                              const unsigned char *baseline, size_t used)
{
    PVOID written[4096];
    ULONG_PTR total = 0;
    size_t pgsz, i;

    if (!mem || !mem->writeset.tracking)
        return 0;
    /* The record was started against one baseline. Restoring some other
       image through it would put back the wrong bytes. */
    if (baseline != mem->writeset.against || used > mem->writeset.len)
        return 0;

    pgsz = xpost_memory_page_size;

    /* Take the whole record before putting anything back.

       Restoring writes to the bank, and those writes are recorded by the
       same mechanism that is being read: a loop that restored between
       reads would keep finding the pages it had itself just written, and
       on a job that wrote a full batch of them it would never finish.
       MEASURED as a hang -- a job stream stopped dead on the workload
       that compacts virtual memory, which touches pages by the hundred.

       So the record is drained first, into a buffer that bounds how much
       this will do. A job that wrote more pages than fit is one for which
       putting back only what changed has stopped being the cheaper answer
       anyway, so it declines and the caller copies the whole baseline,
       which covers the pages already drained from the record. */
    for (;;)
    {
        ULONG_PTR room = (sizeof written / sizeof *written) - total;
        ULONG_PTR count = room;
        ULONG granularity = 0;

        if (room == 0)
            return 0;              /* more than fits: the copy is right */
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem->base,
                          mem->writeset.len, written + total,
                          &count, &granularity) != 0)
            return 0;
        if (count == 0)
            break;
        total += count;
    }

    for (i = 0; i < total; i++)
    {
        size_t off = (size_t)((unsigned char *)written[i] - mem->base);
        size_t n = pgsz;
        unsigned char *page;

        if (off >= mem->writeset.len)
            continue;
        if (off + n > mem->writeset.len)
            n = mem->writeset.len - off;
        page = xpost_vm_ptr(mem, (unsigned int)off);
        /* Below the baseline's extent the baseline says what the bytes
           were; above it the arena promises zero, which is what a job
           that allocated up there has to be given back. */
        if (off >= used)
            memset(page, 0, n);
        else
        {
            size_t from = used - off < n ? used - off : n;

            memcpy(page, baseline + off, from);
            if (from < n)
                memset(page + from, 0, n - from);
        }
    }
    return 1;
}

void xpost_vm_writeset_end(Xpost_Memory_File *mem)
{
    if (!mem || !mem->writeset.tracking)
        return;
    /* Nothing was mapped differently, so there is nothing to put back.
       The record itself costs nothing to leave running, and the bank
       stops consulting it. */
    xpost_vm_writeset_record_clear(mem);
}

void xpost_vm_writeset_forget(Xpost_Memory_File *mem)
{
    /* Nothing was mapped differently, so giving it up for a bank that is
       going away is what giving it up always was. */
    xpost_vm_writeset_end(mem);
}
