/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_spill.c
 * @brief Spilling a recorded page to a file when it outgrows memory.
 *
 * The record keeps marks rather than pixels, but a page can carry more marks
 * than may be held; this is where the overflow goes and how it comes back.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <io.h>
# include <fcntl.h>
# include <sys/stat.h>
# include <windows.h>
#else
# include <fcntl.h>
# include <unistd.h>
#endif

#include <errno.h>

#include "xpost_compat.h" /* xpost_temp_dir: where scratch goes */
#include "xpost_spill.h"

struct _Xpost_Spill
{
    int fd;
    Xpost_Spill_Off high;   /* the furthest anything has been written to */
};

/* Build "<dir><sep>xpost-spill-XXXXXX" in a buffer the caller owns. */
static char *_scratch_path(void)
{
    const char *dir = xpost_temp_dir();
    size_t n = strlen(dir);
    char *p;

#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    /* the tail is the template and its terminator */
    p = malloc(n + 1 + 19 + 1);
    if (!p)
        return NULL;
    memcpy(p, dir, n);
    memcpy(p + n, sep, 1);
    memcpy(p + n + 1, "xpost-spill-XXXXXX", 19);
    return p;
}

Xpost_Spill *xpost_spill_open(void)
{
    Xpost_Spill *sp;
    char *path;
    int fd;

    path = _scratch_path();
    if (!path)
        return NULL;

#ifdef _WIN32
    {
        /* _O_TEMPORARY is delete-on-close and shares delete, which is
           the promise this wants and the one the tree's own mkstemp
           cannot make. The name is filled in by hand rather than by
           _mktemp, which gives up after twenty-six tries. */
        int tries;

        fd = -1;
        for (tries = 0; tries < 512 && fd < 0; tries++)
        {
            char *x = path + strlen(path) - 6;
            int i;

            for (i = 0; i < 6; i++)
                x[i] = "0123456789abcdefghijklmnopqrstuvwxyz"
                       [(unsigned)(rand() ^ (tries << (i & 7))) % 36];
            fd = _open(path,
                       _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_TEMPORARY,
                       _S_IREAD | _S_IWRITE);
        }
    }
#else
    fd = mkstemp(path);
    /* The name goes at once. What is left is a descriptor onto storage
       nothing can open, which the kernel gives back when the last
       reference to it goes -- including the one it drops for a process
       killed by a signal that runs nothing. */
    if (fd >= 0 && unlink(path) != 0)
    {
        close(fd);
        fd = -1;
    }
#endif
    free(path);
    if (fd < 0)
        return NULL;

    sp = calloc(1, sizeof *sp);
    if (!sp)
    {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return NULL;
    }
    sp->fd = fd;
    return sp;
}

void xpost_spill_close(Xpost_Spill *sp)
{
    if (!sp)
        return;
#ifdef _WIN32
    _close(sp->fd);
#else
    close(sp->fd);
#endif
    free(sp);
}

/* Put the descriptor at @p at, answering whether it went there. */
static int _seek(Xpost_Spill *sp, Xpost_Spill_Off at)
{
#ifdef _WIN32
    return _lseeki64(sp->fd, at, SEEK_SET) == at;
#else
    return lseek(sp->fd, (off_t)at, SEEK_SET) == (off_t)at;
#endif
}

int xpost_spill_write(Xpost_Spill *sp, Xpost_Spill_Off at,
                      const void *p, size_t n)
{
    const unsigned char *b = p;

    if (!sp || (n && !p) || at < 0)
        return 0;
    if (!n)
        return 1;
    if (!_seek(sp, at))
        return 0;
    while (n)
    {
        /* the count a write takes is narrower than a spill's own offsets
           on some platforms, so what is offered is held to it */
        size_t want = n > 1u << 20 ? 1u << 20 : n;
#ifdef _WIN32
        int got = _write(sp->fd, b, (unsigned int)want);
#else
        ssize_t got = write(sp->fd, b, want);
#endif

        if (got <= 0)
            return 0;
        b += got;
        n -= (size_t)got;
        at += got;
    }
    if (at > sp->high)
        sp->high = at;
    return 1;
}

int xpost_spill_read(Xpost_Spill *sp, Xpost_Spill_Off at,
                     void *p, size_t n)
{
    unsigned char *b = p;

    if (!sp || (n && !p) || at < 0)
        return 0;
    if (!n)
        return 1;
    if (!_seek(sp, at))
        return 0;
    while (n)
    {
        size_t want = n > 1u << 20 ? 1u << 20 : n;
#ifdef _WIN32
        int got = _read(sp->fd, b, (unsigned int)want);
#else
        ssize_t got = read(sp->fd, b, want);
#endif

        /* nothing, or less than the file was supposed to hold: the file
           has been shortened under a descriptor nobody else holds */
        if (got <= 0)
            return 0;
        b += got;
        n -= (size_t)got;
    }
    return 1;
}

int xpost_spill_truncate(Xpost_Spill *sp, Xpost_Spill_Off n)
{
    if (!sp || n < 0)
        return 0;
    if (n < sp->high)
        sp->high = n;
#ifdef _WIN32
    return _chsize_s(sp->fd, n) == 0;
#else
    return ftruncate(sp->fd, (off_t)n) == 0;
#endif
}

Xpost_Spill_Off xpost_spill_size(const Xpost_Spill *sp)
{
    return sp ? sp->high : 0;
}

int xpost_spill_probe(char *why, size_t n)
{
    static const char bytes[8] = "XPSPILL";
    Xpost_Spill *sp;

    if (why && n)
        why[0] = '\0';
    errno = 0;
    sp = xpost_spill_open();
    if (!sp)
    {
        if (why && n)
        {
            const char *m = errno ? strerror(errno) : "no scratch file";

            strncpy(why, m, n - 1);
            why[n - 1] = '\0';
        }
        return 0;
    }
    errno = 0;
    if (!xpost_spill_write(sp, 0, bytes, sizeof bytes))
    {
        if (why && n)
        {
            const char *m = errno ? strerror(errno) : "the write was short";

            strncpy(why, m, n - 1);
            why[n - 1] = '\0';
        }
        xpost_spill_close(sp);
        return 0;
    }
    /* and it goes here, before this answers: the file had no name to
       begin with and closing it is what gives the space back */
    xpost_spill_close(sp);
    return 1;
}
