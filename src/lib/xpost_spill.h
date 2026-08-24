/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_SPILL_H
#define XPOST_SPILL_H

#include <stddef.h>

/**
 * @file xpost_spill.h
 * @brief Scratch storage with no name, for what a run holds and does not
 *        want resident.
 *
 * A spill file is somewhere to put bytes that must be kept and need not
 * be in memory. It differs from every other file this tree opens in that
 * nothing may ever find it: it has no name a program, a user or another
 * process can reach, and it goes when the handle goes, however the
 * process ends. So it is not a file in the sense the language means, it
 * is not reached through the file machinery, and the path sandbox does
 * not answer for it -- there is no path to confine.
 *
 * It cannot be had from xpost_mkstemp, and that is a fact about the
 * primitive rather than a preference. The POSIX form builds the full path
 * in a buffer of its own and frees it, so the caller is never told the
 * name the file got and has nothing to unlink; the Win32 form opens
 * without FILE_SHARE_DELETE, and Windows will not unlink a file that is
 * open without it. Neither can be asked for "created and immediately
 * unnamed", which is the whole of what this is.
 *
 * The directory is the one the platform names for scratch, in the order
 * the rest of the tree already asks in.
 */

/** How far into a spill file something is. It is a file offset and not a
    size in memory: a spill may be larger than the address space that
    holds the run writing it. */
typedef long long Xpost_Spill_Off;

typedef struct _Xpost_Spill Xpost_Spill;

/**
 * @brief Whether a spill file can be made and written, by making one and
 *        writing to it.
 *
 * @param[out] why what stopped it, as the system reported it, where it
 *                 could not be done
 * @param n the room @p why has
 * @return 1 where a spill file was made, written and given back again
 *
 * A permissions check would be a lie. A directory can be writable and
 * the filesystem full; writable to a stat and refused by a sandbox's
 * system-call filter; writable and mounted read-only under the caller.
 * The only reliable test of whether a spill file can be made and written
 * is to make one and write to it, so that is what this does.
 *
 * A few bytes, and the file goes before this returns -- it is not a
 * spill and holds no page. What it is for is that a run learns at its
 * beginning what it would otherwise learn twenty minutes into a page:
 * everything drawn before the discovery is wasted, and the discovery was
 * available at the start.
 *
 * The answer is what the machine permits now. A directory that stops
 * permitting it afterwards cannot reach a file already open -- the name
 * is gone by then and directory permissions are not consulted again.
 */
int xpost_spill_probe(char *why, size_t n);

/**
 * @brief Make a scratch file that nothing can reach and that goes with
 *        the handle.
 *
 * @return the file, or NULL where the scratch directory would not take one
 *
 * On POSIX the file is created and unlinked at once, so the name exists
 * for no longer than the two calls take and the space goes back when the
 * last descriptor closes -- including the close the kernel does for a
 * process killed by a signal. On Windows it is opened delete-on-close,
 * which gives the same for a process that ends normally or abnormally; a
 * handle leaked by a hard kill can leave the file, and that is the one
 * place the two platforms differ.
 */
Xpost_Spill *xpost_spill_open(void);

/**
 * @brief Give up a spill file, and the space it was holding.
 */
void xpost_spill_close(Xpost_Spill *sp);

/**
 * @brief Put @p n bytes at @p at.
 *
 * @return 1, or 0 where the write failed or was short
 *
 * A short write is a failure and not something to go back to: what would
 * be left is a spill missing part of what it was given, which is a page
 * missing part of what it was asked to paint.
 */
int xpost_spill_write(Xpost_Spill *sp, Xpost_Spill_Off at,
                      const void *p, size_t n);

/**
 * @brief Take @p n bytes from @p at.
 *
 * @return 1, or 0 where the read failed or was short
 *
 * A short read is a file that has been truncated under a descriptor
 * nobody else holds, which is a defect here or a fault below it. It is
 * answered as a failure rather than retried.
 */
int xpost_spill_read(Xpost_Spill *sp, Xpost_Spill_Off at,
                     void *p, size_t n);

/**
 * @brief Give back everything past @p n.
 *
 * @return 1, or 0 where the file could not be shortened
 *
 * What a page boundary does with the page before it. Nothing depends on
 * it -- the writes of the page after reclaim the space either way -- so a
 * platform that will not shorten a file loses room and not correctness.
 */
int xpost_spill_truncate(Xpost_Spill *sp, Xpost_Spill_Off n);

/**
 * @brief What has been written into @p sp altogether, in bytes.
 *
 * The high-water mark of the offsets written and not what the filesystem
 * has allocated: it is a statement about the drawing, which is what a
 * caller weighing a spilled record wants.
 */
Xpost_Spill_Off xpost_spill_size(const Xpost_Spill *sp);

#endif
