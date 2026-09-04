/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2024 Michael Joshua Ryan
 * Copyright (c) 2013-2024 Vincent Torri
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_vm_writeset_linux.c
 * @brief Which pages of a bank a job wrote, on Linux.
 */

/* A private mapping of the baseline is what makes the written pages
   legible. A page the job only read is still the baseline's own page,
   shared with the file behind it; a page the job wrote is a private
   anonymous copy the kernel made for us. /proc/self/pagemap reports that
   distinction per page, without privilege, and discarding just the
   private copies shows the baseline again without moving any of it.

   The set that comes back is a superset of the pages whose contents
   differ: a page written back to the bytes it already held is still a
   private copy, and is restored again for nothing. That is the safe
   direction to be wrong in. */

#if defined(__linux__)
# ifndef _GNU_SOURCE
#  define _GNU_SOURCE /* memfd_create, MADV_DONTNEED and syscall */
# endif
#endif

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_error.h"
#include "xpost_memory.h"
#include "xpost_vm_writeset.h"

#ifndef HAVE_MMAP

/* Virtual memory is the allocator's here rather than a mapping, so there
   is nothing to lay a private view of the baseline over -- and laying one
   over storage the allocator owns would take that storage away from it.
   This host answers as the hosts with no answer do, and the caller copies. */

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

void xpost_vm_writeset_end(Xpost_Memory_File *mem) { (void)mem; }
void xpost_vm_writeset_forget(Xpost_Memory_File *mem) { (void)mem; }

#else


/* The kernel's account of this process's pages. One descriptor for the
   life of the process; a boundary must not pay to open it. */
static int _pagemap(void)
{
    static int fd = -2;
    if (fd == -2)
        fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    return fd;
}

static size_t _round_up(size_t n, size_t to)
{
    return ((n + to - 1) / to) * to;
}

static int _discard(void)
{
    static int asked = 0, on = 1;
    if (!asked) { asked = 1; on = getenv("XPOST_REVERT_COPY") == NULL; }
    return on;
}

/* Put a run of written pages back.

   Two ways, and the one that is quicker per boundary is not the one to
   take. Copying the baseline's bytes over a written page leaves the page
   a private copy, so a bank that copies never gives a page back: the
   private set only grows, boundary after boundary, until every page the
   run has ever written is held privately. Discarding gives the page back
   to the baseline it is a view of, and the set stays what one job wrote.

   MEASURED both ways. Per boundary, over a bank of 352 pages with eight
   written, copying costs 0.013 ms and discarding 0.050 ms, and in place
   at eight concurrent workers the difference between them is inside the
   run-to-run spread. Over time they are not close at all:
   tests/check-page-return.sh watched resident memory grow by 1459 pages
   across forty jobs whose virtual memory did not move at all -- storage
   the interpreter had given up and the system had not taken back -- and
   a barcode service's workers grew 62 MiB in eleven minutes against 22
   MiB for the same load without the tracking.

   So the page is given back. A boundary is not where a run's time goes,
   and resident memory a server never gets back is a cost that keeps
   arriving. XPOST_REVERT_COPY takes the other road, because which of a
   fault and a copy is dearer is a property of the machine and this is
   the measurement that decides it on another one. */
static int _put_back(Xpost_Memory_File *mem, const unsigned char *baseline,
                     size_t used, size_t at, size_t n, size_t pgsz)
{
    unsigned char *page = xpost_vm_ptr(mem, (unsigned int)(at * pgsz));
    size_t off = at * pgsz, len = n * pgsz;

    if (_discard())
        return madvise(page, len, MADV_DONTNEED) == 0;

    /* Below the baseline's extent the baseline says what the bytes were;
       above it the arena promises zero, which is what a job that
       allocated up there has to be given back. */
    if (off >= used)
        memset(page, 0, len);
    else
    {
        size_t from = used - off < len ? used - off : len;

        memcpy(page, baseline + off, from);
        if (from < len)
            memset(page + from, 0, len - from);
    }
    return 1;
}

int xpost_vm_writeset_begin(Xpost_Memory_File *mem,
                            const unsigned char *baseline, size_t used)
{
    size_t len;
    int fd;
    unsigned char *w;

    if (!mem || !mem->base || !baseline || !used)
        return 0;
    /* A bank whose writes have to reach a file cannot be given a private
       view of something else: the writes would stop arriving. */
    if (mem->fd != -1)
        return 0;
    if (!xpost_memory_page_size)
        return 0;
    if ((size_t)mem->base % xpost_memory_page_size)
        return 0;

    xpost_vm_writeset_end(mem);

    len = _round_up(used, xpost_memory_page_size);
    if (len > mem->max)
        return 0;               /* never map past what the bank committed */

    fd = (int)syscall(SYS_memfd_create, "xpost-baseline", 0u);
    if (fd < 0)
        return 0;
    if (ftruncate(fd, (off_t)len) != 0)
    {
        close(fd);
        return 0;
    }
    w = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w == MAP_FAILED)
    {
        close(fd);
        return 0;
    }
    memcpy(w, baseline, used);
    if (len > used)
        memset(w + used, 0, len - used);
    munmap(w, len);

    if (mmap(mem->base, len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED)
    {
        close(fd);
        return 0;
    }

    mem->writeset.fd = fd;
    mem->writeset.tracking = 1;
    mem->writeset.len = len;
    mem->writeset.used = used;
    mem->writeset.back_lo = mem->writeset.back_hi = 0;
    mem->writeset.against = baseline;
    return 1;
}

int xpost_vm_writeset_restore(Xpost_Memory_File *mem,
                              const unsigned char *baseline, size_t used)
{
    unsigned long long ent[512];
    size_t pgsz, npg, done = 0, run = 0, runat = 0;
    int pm;

    if (!mem || mem->writeset.fd < 0)
        return 0;
    /* The arrangement was made against one baseline and reveals that one.
       Restoring some other image through it would put back the wrong
       bytes, so the caller is sent to the copy that is always right. */
    if (baseline != mem->writeset.against || used > mem->writeset.len)
        return 0;

    pm = _pagemap();
    if (pm < 0)
        return 0;

    pgsz = xpost_memory_page_size;
    npg = mem->writeset.len / pgsz;

    while (done < npg)
    {
        size_t want = npg - done, i;
        off_t at = (off_t)(((size_t)mem->base / pgsz + done) * 8);

        if (want > sizeof ent / sizeof *ent)
            want = sizeof ent / sizeof *ent;
        if (pread(pm, ent, want * 8, at) != (ssize_t)(want * 8))
            return 0;

        for (i = 0; i < want; i++)
        {
            unsigned long long e = ent[i];
            /* Present and not file-backed is a page we wrote. A written
               page that has since been paged out is not present and not
               file-backed either, and says so by the swapped bit. */
            int written = ((e >> 63) & 1) ? !((e >> 61) & 1)
                                          : (int)((e >> 62) & 1);

            if (written)
            {
                if (!run)
                    runat = done + i;
                run++;
            }
            else if (run)
            {
                if (!_put_back(mem, baseline, used, runat, run, pgsz))
                    return 0;
                run = 0;
            }
        }
        done += want;
    }
    if (run && !_put_back(mem, baseline, used, runat, run, pgsz))
        return 0;
    return 1;
}

void xpost_vm_writeset_end(Xpost_Memory_File *mem)
{
    unsigned char *held;
    size_t keep, whole;

    if (!mem || mem->writeset.fd < 0)
        return;

    /* Put the bank back as one mapping, not just the part the view
       covered. Making the view carved it out of the bank's mapping,
       leaving two where there was one; anonymous mappings that have each
       been written carry separate reverse-mapping state, so the kernel
       will not merge them again however alike they look -- and a range of
       more than one mapping is what mremap refuses, which is the grow
       this is called for. Only the bytes below the high-water mark are
       carried across: a fresh anonymous mapping reads as zero, which is
       what the arena promises above the mark. */
    keep = mem->high_water;
    whole = _round_up(mem->max, xpost_memory_page_size);
    held = malloc(keep ? keep : 1);
    if (held)
    {
        memcpy(held, mem->base, keep);
        if (mmap(mem->base, whole, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED)
            memcpy(mem->base, held, keep);
        else
            XPOST_LOG_ERR("%d cannot give up the baseline view", VMerror);
        free(held);
    }
    else
        XPOST_LOG_ERR("%d cannot give up the baseline view", VMerror);

    close(mem->writeset.fd);
    mem->writeset.fd = -1;
    xpost_vm_writeset_record_clear(mem);
}

void xpost_vm_writeset_forget(Xpost_Memory_File *mem)
{
    if (!mem || mem->writeset.fd < 0)
        return;
    /* The bank goes with its own teardown; only the baseline's descriptor
       is this file's to give back. */
    close(mem->writeset.fd);
    mem->writeset.fd = -1;
    xpost_vm_writeset_record_clear(mem);
}

#endif /* HAVE_MMAP */
