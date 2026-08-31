/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2024 Michael Joshua Ryan
 * Copyright (c) 2013-2024 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_vm_writeset.h
 * @brief which pages of a bank a job wrote, and putting those back
 *
 * The job boundary restores a whole bank from a copy of it. Copying is
 * memory-bandwidth work, and bandwidth is the one thing more concurrent
 * renders do not bring more of, so it is the copy that makes the boundary
 * cost rise with the number of them. Most of it is waste: MEASURED on a
 * page of dense two-dimensional barcode, a job writes 48 KiB of the
 * 1768 KiB the boundary puts back.
 *
 * Where the host can say which pages a job wrote, the boundary puts back
 * only those. Whether it can, and which way is cheaper once it has,
 * differs by platform and was MEASURED on each rather than reasoned from
 * documentation:
 *
 *   Linux    a private view of the baseline makes the written pages
 *            legible -- an untouched page is still the baseline's, a
 *            written one is a private copy -- and /proc/self/pagemap
 *            reports which is which. Discarding just those private
 *            copies shows the baseline again without moving any of it.
 *
 *   Windows  the same private view, over a section, with
 *            QueryWorkingSetEx reporting which pages stopped being
 *            shared. There the written pages are copied back rather than
 *            discarded, because a view can only be dropped whole: doing
 *            that costs eight times what copying the whole bank costs,
 *            since every page the next job reads faults again.
 *
 *   Darwin answers only where a run asks for it with XPOST_REVERT_WRITTEN,
 *   because there the query costs more than the copy until several renders
 *   are in flight; the file above says what was measured.
 *
 *   others   nothing, and the caller copies. Darwin can answer the
 *            question exactly -- the COPIED disposition that
 *            mach_vm_page_range_query reports names the written pages and
 *            no others -- but MEASURED, asking costs three times the copy
 *            it would save, and mincore's MINCORE_MODIFIED, which is
 *            cheaper to ask, reports every page of a view nothing has
 *            written. Darwin's MADV_DONTNEED does not restore the view
 *            either, so there is no cheaper revert to reach for.
 *
 * So this is not an interface for mapping a bank a particular way. It is
 * "put this bank back", answered by each host the way that host is
 * fastest at, or declined so the caller copies.
 */

#ifndef XPOST_VM_WRITESET_H
#define XPOST_VM_WRITESET_H

#include "xpost_memory.h"

/**
 * @brief Put a bank's write record back to holding nothing.
 *
 * Every host ends its tracking the same way, whatever it used to do the
 * tracking: the record says nothing is arranged and names no baseline.
 * Said once because the cost of saying it per host is not the lines --
 * it is that a field added to the record has to be cleared everywhere it
 * is cleared now, and a host that forgets one carries the last job's
 * answer into a run that is no longer tracking. The file descriptor is
 * not cleared here: only the hosts that open one know they have one.
 */
static inline void
xpost_vm_writeset_record_clear(Xpost_Memory_File *mem)
{
    mem->writeset.tracking = 0;
    mem->writeset.len = 0;
    mem->writeset.used = 0;
    mem->writeset.back_lo = mem->writeset.back_hi = 0;
    mem->writeset.against = NULL;
}

/**
 * @brief Whether this run asked for a bank's writes to be tracked.
 *
 * Where the tracking is a property of the address space rather than
 * something that can be turned on later, the reservation has to ask this
 * when it claims the space.
 */
int xpost_vm_writeset_wanted(void);

/**
 * @brief Arrange for @p mem to be able to say which of its pages are
 *        written from now on, against the baseline at @p baseline.
 *
 * Answers 0 where the host cannot do it, where the bank is backed by a
 * file whose writes must reach it, or where the arrangement would not
 * pay. The caller then copies, and may ask again later.
 */
int xpost_vm_writeset_begin(Xpost_Memory_File *mem,
                            const unsigned char *baseline, size_t used);

/**
 * @brief Put @p mem back to @p baseline by restoring only the pages
 *        written since the arrangement began or was last restored.
 *
 * Answers 0 if the bank has no arrangement, if @p baseline is not the one
 * it was made against, or if the host refuses partway; the caller then
 * copies, which is always correct.
 */
int xpost_vm_writeset_restore(Xpost_Memory_File *mem,
                              const unsigned char *baseline, size_t used);

/**
 * @brief Give up the arrangement, leaving the bank's current bytes in
 *        ordinary memory.
 *
 * Called before the bank grows, because a bank under an arrangement is
 * not one mapping and cannot be grown in place, and when the bank is
 * destroyed. A bank that had asked for the arrangement is left still
 * asking, so the next restore can make it again.
 */
void xpost_vm_writeset_end(Xpost_Memory_File *mem);

/**
 * @brief Give the arrangement up for a bank that is being destroyed.
 *
 * Unlike xpost_vm_writeset_end, this preserves nothing: the bank's storage
 * is about to be released, so putting it back into ordinary memory first
 * is work whose result nothing reads -- and on a host where the putting
 * back means copying the bank through a buffer, it is a context's worth of
 * peak resident memory spent for that nothing, on every context a run
 * creates and destroys.
 */
void xpost_vm_writeset_forget(Xpost_Memory_File *mem);

#endif
