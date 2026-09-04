/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_compat_win32.c
 * @brief The Windows half of the host compatibility layer.
 *
 * The declarations are in xpost_compat.h.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>  /* FILE, TMP_MAX */
#include <stdlib.h> /* free, getenv, malloc, */
#include <string.h> /* strlen, memcpy */

#include <windows.h>
#include <io.h> /* _open */
#include <fcntl.h> /* O_CREAT, etc... */
#include <sys/stat.h> /* S_IREAD, S_IWRITE */

#include "xpost_compat.h"


/*============================================================================*
 *                                  Local                                     *
 *============================================================================*/

static long long _xpost_time_freq;
static long long _xpost_time_start;
/* the execution the process had already had when the interpreter
   started. usertime counts from there, so that it answers what this
   interpreter has done rather than what the process did before it. */
static long long _xpost_cpu_start;
static BCRYPT_ALG_HANDLE _xpost_bcrypt_provider;

/* The execution the process has had, in milliseconds: the time it spent
   running its own instructions and the time the system spent running on
   its behalf, which together are what it has consumed. Both are counted
   in hundreds of nanoseconds. */
static long long
_xpost_cpu_ms(void)
{
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;
    ULARGE_INTEGER k;
    ULARGE_INTEGER u;

    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited,
                         &kernel, &user))
        return 0;

    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;

    return (long long)((k.QuadPart + u.QuadPart) / 10000ULL);
}

static int
_xpost_mkstemp_fill(char *template)
{
    char *buf;

    buf = template;
    while (*buf)
    {
        unsigned char val;

        if (*buf != 'X')
            return 0;

        /*
         * Only characters from 'a' to 'z' and '0' to '9' are considered
         * because on Windows, file system is case insensitive. That means
         * 36 possible values.
         * To increase randomness, we consider the greatest multiple of 36
         * within 255 : 7*36 = 252, that is, values from 0 to 251 and choose
         * a random value in this interval.
         */
        do {
            BCryptGenRandom(_xpost_bcrypt_provider, &val, sizeof(UCHAR), 0);
        } while (val > 251);

        val = '0' + val % 36;
        if (val > '9')
            val += 'a' - '9' - 1;

        *buf = val;
        buf++;
    }

    return 1;
}

/*============================================================================*
 *                                 Global                                     *
 *============================================================================*/

int
xpost_compat_init(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER count;
    WSADATA wsa_data;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
        return 0;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&_xpost_bcrypt_provider,
                                                    BCRYPT_RNG_ALGORITHM,
                                                    NULL, 0)))
        return 0;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    _xpost_time_freq = (long long)freq.QuadPart;
    _xpost_time_start = (long long)count.QuadPart;
    _xpost_cpu_start = _xpost_cpu_ms();

    return 1;
}

void
xpost_compat_quit(void)
{
    BCryptCloseAlgorithmProvider(_xpost_bcrypt_provider, 0);
    WSACleanup();
}

void
xpost_fpurge(FILE *f)
{
    /* Windows has no fpurge()/__fpurge(). On the MSVCRT runtime the mingw
       toolchains build against, fflush() on a stream open for input discards
       the buffered but unread characters -- the same discard-and-skip effect
       __fpurge() has on POSIX, on both regular files and pipes (measured
       identical on the two runtimes). resetfile purges the input side
       (currentfile), which is what this serves. Two boundaries, both harmless
       here: on an output stream fflush() writes the buffer rather than
       discarding it, but the disk files xpost opens for writing (the device
       writers) are never reset; and under the UCRT, fflush() on input is a
       conformant no-op, which is exactly the behaviour this replaces. */
    fflush(f);
}

/* The clock's origin is arbitrary (PLRM 8.2 realtime), and it takes the
   interpreter's own start for it. Counting from there rather than from
   the counter's own zero divides by the frequency once, so an interval
   is not lengthened or shortened by the millisecond a second division
   would truncate away. */
long long
xpost_get_realtime_ms(void)
{
    LARGE_INTEGER count;

    QueryPerformanceCounter(&count);
    return ((count.QuadPart - _xpost_time_start) * 1000LL) / _xpost_time_freq;
}

/* PLRM 8.2 usertime counts the execution the interpreter has done, one
   for every millisecond of it, which is the processor time the process
   has been given and not the time that has passed. */
long long
xpost_get_usertime_ms(void)
{
    long long now = _xpost_cpu_ms();

    /* the clock counts up from the interpreter's start, so a reading
       below that start is no reading at all */
    if (now < _xpost_cpu_start)
        return 0;

    return now - _xpost_cpu_start;
}

const char *
xpost_temp_dir(void)
{
    static char composed[XPOST_PATH_MAX];
    const char *d;

    if ((d = getenv("TEMP")) && *d) return d;
    if ((d = getenv("TMP")) && *d) return d;
    /* LOCALAPPDATA is where a user's data lives, not where scratch does;
       the temporary directory is beneath it. Composed into storage of
       the module's own, since there is no variable holding the joined
       form to answer with. */
    if ((d = getenv("LOCALAPPDATA")) && *d)
    {
        if (snprintf(composed, sizeof composed, "%s\\Temp", d)
            < (int)sizeof composed)
            return composed;
    }
    if ((d = getenv("USERPROFILE")) && *d) return d;
    return ".";
}

int
xpost_mkstemp(char *template, int *fd)
{
    const char *tmpdir;
    char *filename = NULL;
    char *iter;
    size_t len;
    size_t len_tmp;
    int f = -1;
    int count = TMP_MAX;

    if (!template || !*template)
        return 0;

    len = strlen(template);

    /* path is $(tmpdir)\xpost_$(template) */
    tmpdir = xpost_temp_dir();
    len_tmp = strlen(tmpdir);
    filename = (char *)malloc(len_tmp + 7 /* \xpost_ */ + len + 1);
    if (filename)
    {
        iter = filename;
        memcpy(iter, tmpdir, len_tmp);
        iter += len_tmp;
        memcpy(iter, "\\xpost_", 7);
        iter += 7;
        memcpy(iter, template, len + 1);
    }

    if (!filename)
        return 0;

    while ((f < 0) && (count-- > 0))
    {
        char *trail;

        CopyMemory(iter, template, len + 1);
        trail = iter + len - 6;

        if (!_xpost_mkstemp_fill(trail))
            break;

        f = _open(filename,
                   O_CREAT | O_EXCL | O_RDWR | O_BINARY,
                   S_IREAD | S_IWRITE);
        if (f != -1)
            memcpy(template, iter, len + 1);
        else
        {
            if (errno != EEXIST)
                count = 0;
        }
    }

    free(filename);

    if (f == -1)
        return 0;

    *fd = f;

    return 1;
}

/* Write `path` into `buf` without the prefix GetFinalPathNameByHandle
   puts in front of a resolved name, so that a resolved path is spelt the
   way a path is spelt everywhere else here: a drive letter and a colon,
   or two backslashes for a share. Answers 0 when it will not fit. */
static int
_xpost_path_unprefix(const char *path, char *buf, size_t buflen)
{
    if (_strnicmp(path, "\\\\?\\UNC\\", 8) == 0) /* \\?\UNC\server\share -> \\server\share */
    {
        if ((size_t)(2 + strlen(path + 8)) >= buflen)
            return 0;
        buf[0] = '\\';
        buf[1] = '\\';
        strcpy(buf + 2, path + 8);
        return 1;
    }
    if (_strnicmp(path, "\\\\?\\", 4) == 0)
    {
        if (strlen(path + 4) >= buflen)
            return 0;
        strcpy(buf, path + 4);
        return 1;
    }
    if (strlen(path) >= buflen)
        return 0;
    strcpy(buf, path);
    return 1;
}

/* The canonical name of an existing file or directory: the name is
   resolved through whatever stands between it and the volume -- symbolic
   links, junctions, substituted drives, short names -- and a path that
   names nothing is refused. Resolving is done by opening the thing and
   asking the handle what it is, which is what makes existence part of
   the answer; FILE_FLAG_BACKUP_SEMANTICS is what lets a directory be
   opened, and attributes are all the access asked for, so a directory
   whose contents the caller may not read still resolves. */
char *
xpost_realpath(const char *path)
{
    char final[XPOST_PATH_MAX];
    char resolved[XPOST_PATH_MAX];
    char *ret;
    HANDLE h;
    DWORD n;

    if (!path || !*path)
        return NULL;

    h = CreateFile(path, FILE_READ_ATTRIBUTES,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    n = GetFinalPathNameByHandle(h, final, sizeof final, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0UL || n >= sizeof final)
        return NULL;

    if (!_xpost_path_unprefix(final, resolved, sizeof resolved))
        return NULL;

    ret = malloc(strlen(resolved) + 1);
    if (!ret)
        return NULL;
    strcpy(ret, resolved);

    return ret;
}

FILE *
xpost_open_beneath(const char *root, const char *rel)
{
    char full[XPOST_PATH_MAX];
    char root_final[XPOST_PATH_MAX];
    char file_final[XPOST_PATH_MAX];
    HANDLE rh;
    HANDLE h;
    DWORD n;
    int fd;
    FILE *fp;
    size_t rl;

    if (!root || !rel || !*rel)
    {
        errno = ENOENT;
        return NULL;
    }

    /* canonical form of root, resolved via a handle (FILE_FLAG_BACKUP_SEMANTICS
       is required to open a directory handle) */
    rh = CreateFile(root, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (rh == INVALID_HANDLE_VALUE)
    {
        errno = ENOENT;
        return NULL;
    }
    n = GetFinalPathNameByHandle(rh, root_final, sizeof root_final, FILE_NAME_NORMALIZED);
    CloseHandle(rh);
    if (n == 0UL || n >= sizeof root_final)
    {
        errno = ENOENT;
        return NULL;
    }

    if (_snprintf(full, sizeof full, "%s\\%s", root, rel) < 0)
    {
        errno = ENAMETOOLONG;
        return NULL;
    }
    full[sizeof full - 1] = '\0';

    /* open the reparse point itself rather than following it; the final-path
       check below rejects anything that resolves outside root */
    h = CreateFile(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                   FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = ENOENT;
        return NULL;
    }
    n = GetFinalPathNameByHandle(h, file_final, sizeof file_final, FILE_NAME_NORMALIZED);
    if (n == 0UL || n >= sizeof file_final)
    {
        CloseHandle(h);
        errno = EACCES;
        return NULL;
    }

    /* the resolved file must sit strictly beneath the resolved root */
    rl = strlen(root_final);
    if (_strnicmp(file_final, root_final, rl) != 0 ||
        (file_final[rl] != '\\' && file_final[rl] != '/'))
    {
        CloseHandle(h);
        errno = EACCES;
        return NULL;
    }

    fd = _open_osfhandle((intptr_t)h, _O_RDONLY);
    if (fd < 0)
    {
        CloseHandle(h);
        errno = EACCES;
        return NULL;
    }
    fp = _fdopen(fd, "rb"); /* takes ownership of fd, and of the handle */
    if (!fp)
    {
        _close(fd);
        errno = EACCES;
        return NULL;
    }
    return fp;
}

/* The atomic beneath-root primitives are Linux-specific (openat2). On Windows
   they report themselves unsupported so the file layer applies its portable
   name-based check instead. */

FILE *
xpost_openat2_beneath(const char *root, const char *rel, const char *mode,
                      int access, int *supported)
{
    (void)root; (void)rel; (void)mode; (void)access;
    *supported = 0;
    errno = ENOSYS;
    return NULL;
}

int
xpost_fd_realpath(int fd, char *buf, size_t buflen)
{
    HANDLE h;
    char tmp[XPOST_PATH_MAX];
    DWORD n;

    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    n = GetFinalPathNameByHandle(h, tmp, sizeof tmp, FILE_NAME_NORMALIZED);
    if (n == 0UL || n >= sizeof tmp)
        return 0;
    /* the same resolved form xpost_realpath answers with, which is the
       form the permit set is stored in */
    return _xpost_path_unprefix(tmp, buf, buflen);
}

int
xpost_path_is_symlink(const char *path)
{
    DWORD attr;

    if (!path || !*path)
        return 0;
    attr = GetFileAttributes(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

int
xpost_unlinkat_beneath(const char *root, const char *rel, int *supported)
{
    (void)root; (void)rel;
    *supported = 0;
    errno = ENOSYS;
    return -1;
}

int
xpost_renameat_beneath(const char *oldroot, const char *oldrel,
                       const char *newroot, const char *newrel,
                       int *supported)
{
    (void)oldroot; (void)oldrel; (void)newroot; (void)newrel;
    *supported = 0;
    errno = ENOSYS;
    return -1;
}

/*============================================================================*
 *                                   API                                      *
 *============================================================================*/
