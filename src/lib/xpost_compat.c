/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_compat.c
 * @brief What the hosts do not agree about, behind one name each.
 *
 * The platform-specific halves are in the posix and win32 files beside this.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

/* for dladdr Unix */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

/* for dladdr on OS X */
#ifndef _DARWIN_C_SOURCE
# define _DARWIN_C_SOURCE
#endif

#include <stdlib.h>

#include <stdio.h>
#include <string.h>

#ifdef HAVE_TERMIOS_H
# include <termios.h>
#endif

#ifdef __CYGWIN__
# include <sys/cygwin.h>
#endif

#if defined _WIN32 || defined __CYGWIN__
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
# include <sys/stat.h>
# include <fcntl.h>
# include <io.h>
# include <wincrypt.h>
# include <errno.h>
# undef WIN32_LEAN_AND_MEAN
#else
# ifdef HAVE_DLADDR
#  include <dlfcn.h> /* dladdr */
# endif
# include <unistd.h> /* isatty */
#endif

#include "xpost_log.h"
#include "xpost_compat.h"

/*
   Enable keyboard echo on terminal (console) input
 */
void echoon(FILE *f)
{
#ifdef _WIN32
    if (f == stdin)
    {
        HANDLE h;
        DWORD mode;

        h = GetStdHandle(STD_INPUT_HANDLE);
        if (GetConsoleMode(h, &mode))
        {
            mode |= ENABLE_ECHO_INPUT;
            SetConsoleMode(h, mode);
        }
    }
#elif defined HAVE_TERMIOS_H
    struct termios ts;

    tcgetattr(fileno(f), &ts);
    ts.c_lflag |= ECHO;
    tcsetattr(fileno(f), TCSANOW, &ts);
#else
    (void)f;
#endif
}

/*
   Disable keyboard echoing on terminal (console) input
 */
void echooff(FILE *f)
{
#ifdef _WIN32
    if (f == stdin)
    {
        HANDLE h;
        DWORD mode;

        h = GetStdHandle(STD_INPUT_HANDLE);
        if (GetConsoleMode(h, &mode))
        {
            mode &= ~ENABLE_ECHO_INPUT;
            SetConsoleMode(h, mode);
        }
    }
#elif defined HAVE_TERMIOS_H
    struct termios ts;

    tcgetattr(fileno(f), &ts);
    ts.c_lflag &= ~ECHO;
    tcsetattr(fileno(f), TCSANOW, &ts);
#else
    (void)f;
#endif
}

int xpost_isatty(int fd)
{
#ifdef _WIN32
    DWORD st;
    HANDLE h;

    if (!_isatty(fd))
        return 0;

    h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    if (!GetConsoleMode(h, &st))
        return 0;

    return 1;
#else
    return isatty(fd);
#endif
}

int
xpost_glob(const char *pattern, glob_t *pglob)
{
#ifdef _WIN32
    /* code based on the implementation in unixem (http://synesis.com.au/software/unixem.html) */
    /* BSD license */
    char path[MAX_PATH];
    WIN32_FIND_DATA fd;
    HANDLE f;
    char *buffer;
    char *iter;
    char *file;
    int result;
    size_t max_matches;

    if (!pattern || !pglob)
        return -1;

    /* path and file parts */
    file = NULL;
    iter = (char *)pattern;
    while (*iter)
    {
        if ((*iter == '/') || (*iter == '\\'))
            file = iter;
        iter++;
    }

    if (!file)
    {
        file = (char *)pattern;
        path[0] = '\0';
    }
    else
    {
        /* the directory part keeps the separator that ended it, so that
           what it contributes to a name's length is what it copies into
           the name */
        size_t dirlen = (size_t)(file - pattern) + 1;

        if (dirlen >= sizeof(path))
            return -1;
        memcpy(path, pattern, dirlen);
        path[dirlen] = '\0';
        file++;
    }

    buffer = NULL;

    pglob->gl_pathc = 0;
    pglob->gl_pathv = NULL;
    pglob->gl_offs = 0;

    f = FindFirstFile(pattern, &fd);

    if (f == INVALID_HANDLE_VALUE)
    {
        if ((strpbrk(pattern, "?*") != NULL) && file)
            result = 0;
        else
            result = -1;
    }
    else
    {
        size_t current_len;
        size_t size;
        size_t matches;

        current_len = 0;
        size = 0;
        matches = 0;
        max_matches = ~(size_t)(0);
        result = 0;

        do
        {
            size_t len;
            size_t new_size;

            if ((file == strpbrk(file, "?*")) && (fd.cFileName[0] == '.'))
                continue;

            /* the name part of the pattern, which is the pattern itself
               when it names no directory: the length the directory part
               adds is then zero */
            len = strlen(fd.cFileName) + (file - pattern);

            new_size = current_len + len + 1;
            if (new_size > size)
            {
                char *new_buffer;

                new_size *= 2;
                new_size = (new_size + 31) & ~(31);
                new_buffer = (char *)realloc(buffer, new_size);
                if (!new_buffer)
                {
                    result = -1;
                    free(buffer);
                    buffer = NULL;
                    break;
                }

                buffer = new_buffer;
                size = new_size;
            }
            lstrcpyn(buffer + current_len, path, (file - pattern) + 1);
            lstrcat(buffer + current_len, fd.cFileName);
            current_len += len + 1;
            matches ++;
        } while (FindNextFile(f, &fd) && (matches != max_matches));

        FindClose(f);

        if (result == 0)
        {
            size_t nbr_pointers;
            char *new_buffer;

            nbr_pointers = (matches + pglob->gl_offs + 1) * sizeof(char *);
            new_buffer = (char *)realloc(buffer, size + nbr_pointers);
            if (!new_buffer)
            {
                result = -2;
                free(buffer);
            }
            else
            {
                char**  pp;
                char**  begin;
                char**  end;
                char*   next_str;

                buffer = new_buffer;
                memmove(new_buffer + nbr_pointers, new_buffer, size);
                begin = (char**)new_buffer;
                end = begin + pglob->gl_offs;
                for (; begin != end; ++begin)
                    *begin = NULL;

                pp = (char**)new_buffer + pglob->gl_offs;
                begin = pp;
                end = begin + matches;
                for (begin = pp, next_str = (buffer + nbr_pointers); begin != end; ++begin)
                {
                    *begin = next_str;
                    next_str += 1 + strlen(next_str);
                }

                *begin = NULL;
                pglob->gl_pathc = matches;
                pglob->gl_pathv = (char**)new_buffer;
            }
        }

        if (matches == 0)
            result = -1;
    }

    if (result == 0)
    {
        if (pglob->gl_pathc == max_matches)
            result = -1;
    }

    return (result == 0) ? 0 : -1;
#else
    return glob(pattern, 0, NULL, pglob);
#endif
}

void
xpost_glob_free(glob_t *pglob)
{
#ifdef _WIN32
    if (pglob)
    {
        free(pglob->gl_pathv);
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
    }
#else
    globfree(pglob);
#endif
}

char *
xpost_getenv(const char *name)
{
#ifdef _WIN32
    char *buf;
    DWORD len;

    SetLastError(0);
    len = GetEnvironmentVariableA(name, NULL, 0);
    /* a variable that is not set and one holding nothing both measure
       nothing; what tells them apart is whether the read failed */
    if (len == 0 && GetLastError() != 0)
        return NULL;
    if (len == 0)
        len = 1;
    buf = malloc(len);
    if (!buf)
        return NULL;
    buf[0] = '\0';
    SetLastError(0);
    if (GetEnvironmentVariableA(name, buf, len) == 0 && GetLastError() != 0)
    {
        free(buf);
        return NULL;
    }
    return buf;
#else
    const char *v;

    v = getenv(name);
    if (!v)
        return NULL;
    {
        size_t n = strlen(v) + 1;
        char *buf = malloc(n);

        if (buf)
            memcpy(buf, v, n);
        return buf;
    }
#endif
}

int
xpost_putenv(const char *name, const char *value)
{
#ifdef _WIN32
    return SetEnvironmentVariableA(name, value) ? 0 : -1;
#else
    return setenv(name, value, 1);
#endif
}

unsigned char
xpost_module_path_get(int (*fp)(void), char *buf, unsigned int size)
{
#if defined (_WIN32) || defined (__CYGWIN__) || defined (HAVE_DLADDR)
    void *addr;
#endif

#if defined (_WIN32) || defined (__CYGWIN__)
    MEMORY_BASIC_INFORMATION mbi;

    if (sizeof addr != sizeof fp)
    {

        return 0;
    }
    memcpy(&addr, &fp, sizeof addr);

    if (VirtualQuery(addr, &mbi, sizeof(mbi)) &&
        (mbi.State == MEM_COMMIT) &&
        (mbi.AllocationBase))
    {
# define XPOST_UNICODE_PATH_MAX 32768
        TCHAR tpath[XPOST_UNICODE_PATH_MAX];

        if (GetModuleFileName((HMODULE)mbi.AllocationBase,
                              (LPTSTR)&tpath, XPOST_UNICODE_PATH_MAX))
        {
            char *path = NULL;
            char *pos;

# ifdef UNICODE
            int asize;

            asize = WideCharToMultiByte(CP_ACP, 0, tpath, -1,
                                        NULL, 0, NULL, NULL);
            if (asize == 0)
                return 0;
            path = malloc(asize * sizeof(char));
            if (!path)
                return 0;
            asize = WideCharToMultiByte(CP_ACP, 0, tpath, -1,
                                        path, asize, NULL, NULL);
            if (!asize)
            { /* we should never get there */
                free(path);
                return 0;
            }
# else
            path = tpath;
# endif
# undef XPOST_UNICODE_PATH_MAX

            /* the module name is whole and terminated here: the system
               wrote it into the wide buffer above, and the conversion
               that narrows it either wrote all of it or ended the
               lookup */
            /* cppcheck-suppress uninitvar */
            pos = strrchr(path, '\\');
            if (pos)
            {
                size_t length;

                *pos = '\0';
                length = strlen(path) + 1;
                if (length <= size)
                {
                    memcpy(buf, path, length);
# ifdef UNICODE
                    free(path);
# endif
                    return 1;
                }
            }
# ifdef UNICODE
            free(path);
# endif
        }
    }
#elif defined (HAVE_DLADDR)
    Dl_info xpost_info;

    if (sizeof addr != sizeof fp)
    {
        XPOST_LOG_ERR("sizeof uintptr_t != sizeof (int (*)())");
    }
    memcpy(&addr, &fp, sizeof addr);

    if (dladdr(addr, &xpost_info))
    {
        char *pos;

        pos = strrchr(xpost_info.dli_fname, '/');
        if (pos)
        {
            size_t length;

            length = pos - xpost_info.dli_fname;
            if (length <= size)
            {
                memcpy(buf, xpost_info.dli_fname, length);
                buf[length] = '\0';
                return 1;
            }
        }
    }
#else
    /* this host cannot name the module's own path; the caller's
       resolution chain falls back to its remaining candidates */
    (void)fp;
    (void)buf;
    (void)size;
#endif

    return 0;
}
