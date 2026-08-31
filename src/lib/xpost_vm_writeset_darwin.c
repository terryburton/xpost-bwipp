/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2024 Michael Joshua Ryan
 * Copyright (c) 2013-2024 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_vm_writeset_darwin.c
 * @brief Which pages of a bank a job wrote, on Darwin.
 */

/* Darwin reports a disposition per page, and mach_vm_page_range_query
   answers for a whole range in one call. The bit that names the pages a
   job wrote is COPIED: a private view of the baseline hands out the
   baseline's own pages until something writes, and a written page is a
   copy taken privately. Held to a known set of writes it names that set,
   and to a view nothing has written it names none.

   The other two things Darwin offers do not work, so neither is used
   here. mincore's MINCORE_MODIFIED, which is cheaper to ask, reported
   every page of a view nothing had written. And MADV_DONTNEED does not
   put a private file view back, so the written pages are copied back
   from the baseline rather than discarded -- which is the faster of the
   two on the other hosts as well.

   Asking is dear here: MEASURED over a bank of 352 pages with eight
   written, the query and the copy together cost 0.138 ms where copying
   the whole bank costs 0.041 ms. What makes it worth doing anyway is
   that the two do not answer to the same resource. The copy is memory
   bandwidth, which several renders in flight share; the query is not.
   MEASURED at one, two, four and eight concurrent workers, copying the
   whole bank costs 0.041, 0.043, 0.098 and 0.382 ms while asking and
   putting back what changed costs 0.138, 0.150, 0.167 and 0.243 -- so
   the copy overtakes the query between four workers and eight, and a
   host serving one render at a time would be better off without this.
   That is the shape of the whole idea, and here it is visible in the
   measurement rather than assumed. */

#if defined(__APPLE__)
# ifndef _GNU_SOURCE
#  define _GNU_SOURCE /* the Mach page-query interface */
# endif
#endif

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

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


static size_t _round_up(size_t n, size_t to)
{
    return ((n + to - 1) / to) * to;
}

/* The baseline has to be something a private view can be taken of, and
   Darwin will not take one of a shared memory object: mapping a
   shm_open descriptor MAP_PRIVATE is refused outright. So it is an
   ordinary file, unlinked as soon as it is open, which lives exactly as
   long as the descriptor. */
static int _baseline_file(size_t len, const unsigned char *from, size_t used)
{
    const char *dir = getenv("TMPDIR");
    char path[512];
    unsigned char *w;
    int fd;

    if (!dir || !*dir)
        dir = "/tmp";
    if (snprintf(path, sizeof path, "%s/xpost-baseline-XXXXXX", dir)
        >= (int)sizeof path)
        return -1;
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)len) != 0)
    {
        close(fd);
        return -1;
    }
    w = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w == MAP_FAILED)
    {
        close(fd);
        return -1;
    }
    memcpy(w, from, used);
    if (len > used)
        memset(w + used, 0, len - used);
    munmap(w, len);
    return fd;
}

/* Asked for here rather than assumed, which is the opposite of the other
   two hosts and is what the measurement says.

   The query is dear on this host -- it costs more than the copy it saves
   until there are enough renders in flight to make the copy compete for
   bandwidth, and the crossover measured between four concurrent workers
   and eight. A host serving one render at a time is worse off with it, and
   that is the shape of every test run and most embeddings. It also has a
   standing cost the other two do not: Linux is handed a baseline it can map
   with nothing on disk behind it and Windows maps nothing at all, while
   here the baseline gains a second home in a file, per context, for a run
   that makes and destroys them.

   So this host answers only where a run says it wants it, and a run that
   has eight workers is a run that knows. */
static int _asked(void)
{
    static int asked = 0, on = 0;
    if (!asked)
    {
        const char *v;

        asked = 1;
        v = getenv("XPOST_REVERT_WRITTEN");
        on = v && *v && *v != '0';
    }
    return on;
}

int xpost_vm_writeset_begin(Xpost_Memory_File *mem,
                            const unsigned char *baseline, size_t used)
{
    size_t len;
    int fd;

    if (!_asked())
        return 0;
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

    fd = _baseline_file(len, baseline, used);
    if (fd < 0)
        return 0;

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
    int disp[512];
    size_t pgsz, npg, done = 0;

    if (!mem || !mem->writeset.tracking)
        return 0;
    /* The view was taken of one baseline and reveals that one. Restoring
       some other image through it would put back the wrong bytes. */
    if (baseline != mem->writeset.against || used > mem->writeset.len)
        return 0;

    pgsz = xpost_memory_page_size;
    npg = mem->writeset.len / pgsz;

    while (done < npg)
    {
        size_t want = npg - done, i;
        mach_vm_size_t count;
        unsigned char *at;

        if (want > sizeof disp / sizeof *disp)
            want = sizeof disp / sizeof *disp;
        count = want;
        at = xpost_vm_ptr(mem, (unsigned int)(done * pgsz));
        if (mach_vm_page_range_query(mach_task_self(),
                                     (mach_vm_address_t)at,
                                     (mach_vm_size_t)(want * pgsz),
                                     (mach_vm_address_t)disp,
                                     &count) != KERN_SUCCESS)
            return 0;

        for (i = 0; i < want; i++)
        {
            size_t off;
            unsigned char *page;

            if (!(disp[i] & VM_PAGE_QUERY_PAGE_COPIED))
                continue;
            off = (done + i) * pgsz;
            page = xpost_vm_ptr(mem, (unsigned int)off);
            /* Below the baseline's extent the baseline says what the
               bytes were; above it the arena promises zero, which is what
               a job that allocated up there has to be given back. */
            if (off >= used)
                memset(page, 0, pgsz);
            else
            {
                size_t from = used - off < pgsz ? used - off : pgsz;

                memcpy(page, baseline + off, from);
                if (from < pgsz)
                    memset(page + from, 0, pgsz - from);
            }
        }
        done += want;
    }
    return 1;
}

void xpost_vm_writeset_end(Xpost_Memory_File *mem)
{
    unsigned char *held;
    size_t keep, whole;

    if (!mem || mem->writeset.fd < 0)
        return;

    /* Put the whole bank back as ordinary memory. Taking the view carved
       it out of the reservation the bank was made from, and what is left
       has to be the storage the bank thinks it has: everything below the
       high-water mark holding what it held, and a fresh anonymous
       mapping above it, which reads as zero -- the arena's promise for
       the storage it has not handed out. */
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
    mem->writeset.tracking = 0;
    mem->writeset.len = 0;
    mem->writeset.used = 0;
    mem->writeset.back_lo = mem->writeset.back_hi = 0;
    mem->writeset.against = NULL;
}

void xpost_vm_writeset_forget(Xpost_Memory_File *mem)
{
    if (!mem || mem->writeset.fd < 0)
        return;
    /* The bank goes with its own teardown; only the baseline's descriptor
       is this file's to give back. */
    close(mem->writeset.fd);
    mem->writeset.fd = -1;
    mem->writeset.tracking = 0;
    mem->writeset.len = 0;
    mem->writeset.used = 0;
    mem->writeset.back_lo = mem->writeset.back_hi = 0;
    mem->writeset.against = NULL;
}

#endif /* HAVE_MMAP */
