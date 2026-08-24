/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
# undef WIN32_LEAN_AND_MEAN
# include <io.h>
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_compat.h" /* xpost_mkstemp, snprintf */
#include "xpost_private.h"


/*============================================================================*
 *                                  Local                                     *
 *============================================================================*/

static const char *_xpost_log_level_names[] =
{
    "ERR",
    "WRN",
    "INF",
    "DBG"
};

#ifdef _DEBUG
static Xpost_Log_Level _xpost_log_level = XPOST_LOG_LEVEL_DBG;
#else
static Xpost_Log_Level _xpost_log_level = XPOST_LOG_LEVEL_ERR;
#endif

static Xpost_Log_Print_Cb _xpost_log_print_cb = xpost_log_print_cb_stderr;
static void *_xpost_log_print_cb_data = NULL;
static FILE *_xpost_log_dump_file = NULL;

#ifdef _WIN32

static unsigned char _xpost_log_is_posix = 1;
static HANDLE _xpost_log_handle_stdout = NULL;
static HANDLE _xpost_log_handle_stderr = NULL;

static WORD
_xpost_log_print_level_color_get(int level, WORD original_background)
{
    WORD foreground;

    switch (level)
    {
        case XPOST_LOG_LEVEL_ERR:
            foreground = FOREGROUND_INTENSITY | FOREGROUND_RED;
            break;
        case XPOST_LOG_LEVEL_WARN:
            foreground = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN;
            break;
        case XPOST_LOG_LEVEL_INFO:
            foreground = FOREGROUND_INTENSITY | FOREGROUND_GREEN;
            break;
        case XPOST_LOG_LEVEL_DBG:
          foreground = FOREGROUND_INTENSITY | FOREGROUND_BLUE;
          break;
        default:
            foreground = FOREGROUND_BLUE;
            break;
    }

    return original_background | foreground;
}

static void
_xpost_log_win32_print_prefix_func(FILE *stream,
                                   Xpost_Log_Level level,
                                   const char *file,
                                   const char *fct,
                                   int line)
{
    CONSOLE_SCREEN_BUFFER_INFO scbi_stdout;
    CONSOLE_SCREEN_BUFFER_INFO scbi_stderr;
    CONSOLE_SCREEN_BUFFER_INFO *scbi;
    HANDLE handle;
    WORD color;
    BOOL use_color;

    if (_xpost_log_handle_stdout != INVALID_HANDLE_VALUE)
    {
        if (!GetConsoleScreenBufferInfo(_xpost_log_handle_stdout, &scbi_stdout))
            return;
    }

    if (_xpost_log_handle_stderr != INVALID_HANDLE_VALUE)
    {
        if (!GetConsoleScreenBufferInfo(_xpost_log_handle_stderr, &scbi_stderr))
            return;
    }

    handle  = (stream == stdout) ? _xpost_log_handle_stdout : _xpost_log_handle_stderr;
    scbi = (stream == stdout) ? &scbi_stdout : &scbi_stderr;
    use_color = (_isatty(_fileno(stream)) != 1) && (handle != INVALID_HANDLE_VALUE);
    color = use_color ? _xpost_log_print_level_color_get(level, scbi->wAttributes & ~7) : 0;
    if (use_color)
    {
        fflush(stream);
        SetConsoleTextAttribute(handle, color);
    }

    fprintf(stream, "%s", _xpost_log_level_names[level]);
    if (use_color)
    {
        fflush(stream);
        SetConsoleTextAttribute(handle, scbi->wAttributes);
    }

    fprintf(stream, ": %s:%d %s() ", file, line, fct);
}

#endif

static void
_xpost_log_posix_print_prefix_func(FILE *stream,
                                   Xpost_Log_Level level,
                                   const char *file,
                                   const char *fct,
                                   int line)
{
    const char *color;

    switch (level)
    {
        case XPOST_LOG_LEVEL_ERR:
            color = "\033[31m";
            break;
        case XPOST_LOG_LEVEL_WARN:
            color = "\033[33;1m";
            break;
        case XPOST_LOG_LEVEL_INFO:
            color = "\033[32;1m";
            break;
        case XPOST_LOG_LEVEL_DBG:
            color = "\033[34;1m";
            break;
        default:
            color = "\033[34m";
            break;
    }

    fprintf(stream, "%s%s" "\033[0m" ": %s:%d "
            "\033[1m" "%s()" "\033[0m" " ",
            color, _xpost_log_level_names[level], file, line, fct);
}

static void
_xpost_log_fprint_cb(FILE *stream,
                     Xpost_Log_Level level,
                     const char *file,
                     const char *fct,
                     int line,
                     const char *fmt,
                     void *data, /* later for XML output */
                     va_list args)
{
    va_list args_copy;
    char *str;
    int res;
    int s;

    /* args arrives started: the caller opened it and closes it, and this
       is one of two readers of it. A list is readable once, so the second
       reading -- the one that writes the text after the first measured it
       -- goes through a copy made before either. */
    /* cppcheck-suppress-begin va_list_usedBeforeStarted */
    va_copy(args_copy, args);

    s = vsnprintf(NULL, 0, fmt, args);
    if (s == -1)
    {
        va_end(args_copy);
        return;
    }

    str = (char *)malloc((s + 2) * sizeof(char));
    if (!str)
    {
        va_end(args_copy);
        return;
    }

    s = vsnprintf(str, s + 1, fmt, args_copy);
    va_end(args_copy);
    /* cppcheck-suppress-end va_list_usedBeforeStarted */
    if (s == -1)
    {
        free(str);
        return;
    }
    str[s] = '\n';
    str[s + 1] = '\0';

#ifdef _WIN32
    if (!_xpost_log_is_posix)
        _xpost_log_win32_print_prefix_func(stream, level, file, fct, line);
    else
#endif
    _xpost_log_posix_print_prefix_func(stream, level, file, fct, line);
    res = fprintf(stream, "%s", str);
    free(str);

    if (res < 0)
        return;

    if ((int)res != (s + 1))
        fprintf(stderr, "ERROR: %s(): want to write %d bytes, %d written\n", __func__, s + 1, res);

    (void)data;
}


/*============================================================================*
 *                                 Global                                     *
 *============================================================================*/


int
xpost_log_init(void)
{
    char dump_filename[] = "xdumpXXXXXX";
    int fd;
    char *endptr;
    const char *level;

#ifdef _WIN32
    DWORD mode;

    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode))
    {
        _xpost_log_handle_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        _xpost_log_handle_stderr = GetStdHandle(STD_ERROR_HANDLE);
        _xpost_log_is_posix = 0;
    }
    else
        _xpost_log_is_posix = 1;
#endif

    level = getenv("XPOST_LOG_LEVEL");
    if (level)
    {
        long l;

        l = strtol(level, &endptr, 10);
        if (!((errno == ERANGE && (l == LONG_MAX || l == LONG_MIN)) ||
              (errno != 0 && l == 0) ||
              (endptr == level)))
            _xpost_log_level = (int)l;
    }

    (void)dump_filename;
    (void)fd;
    /* The file the tracing dump goes to is not made here. It is made by
       the first thing written to it, which on a run that is not tracing
       is nothing at all -- and a run that is not tracing has no business
       leaving a file in the scratch directory, nor any business refusing
       to start where one cannot be made. */
    return 1;
}

/* The file a dump goes to, made where the first dump is written.
 *
 * A run makes one of these only if something is dumped into it, which
 * means only if the run is tracing. It is named and stays named, unlike
 * the scratch a record spills into: a dump is written to be read
 * afterwards, so it is a file in the ordinary sense and not storage with
 * no name.
 *
 * A run that cannot make one loses its dump and goes on, which is the
 * right way round for a diagnostic: an interpreter that would not start
 * because it could not open a debugging aid is refusing the job for the
 * sake of the notes about the job. */
static FILE *_xpost_log_dump(void)
{
    static int tried = 0;
    char dump_filename[] = "xdumpXXXXXX";
    int fd;

    if (_xpost_log_dump_file || tried)
        return _xpost_log_dump_file;
    tried = 1;
    if (!xpost_mkstemp(dump_filename, &fd))
        return NULL;
    _xpost_log_dump_file = fdopen(fd, "wb");
    if (!_xpost_log_dump_file)
        return NULL;
    XPOST_LOG_INFO("dump interpreter errors in file %s", dump_filename);
    return _xpost_log_dump_file;
}

void
xpost_log_quit(void)
{
    if (_xpost_log_dump_file)
        fclose(_xpost_log_dump_file);
}


/*============================================================================*
 *                                   API                                      *
 *============================================================================*/


XPAPI void
xpost_log_print_cb_set(Xpost_Log_Print_Cb cb, void *data)
{
    _xpost_log_print_cb = cb;
    _xpost_log_print_cb_data = data;
}

XPAPI void
xpost_log_print_cb_stderr(Xpost_Log_Level level,
                          const char *file,
                          const char *fct,
                          int line,
                          const char *fmt,
                          void *data,
                          va_list args)
{
    _xpost_log_fprint_cb(stderr, level, file, fct, line, fmt, data, args);
}

XPAPI void
xpost_log_print_cb_stdout(Xpost_Log_Level level,
                          const char *file,
                          const char *fct,
                          int line,
                          const char *fmt,
                          void *data,
                          va_list args)
{
    _xpost_log_fprint_cb(stdout, level, file, fct, line, fmt, data, args);
}

XPAPI void
xpost_log_print(Xpost_Log_Level level,
                const char *file,
                const char *fct,
                int line,
                const char *fmt, ...)
{
    va_list args;

    if (!fmt)
    {
        fprintf(stderr, "ERROR: %s() fmt == NULL\n", __func__);
        return;
    }

    if (level > _xpost_log_level)
        return;

    va_start(args, fmt);
    _xpost_log_print_cb(level, file, fct, line, fmt, _xpost_log_print_cb_data, args);
    va_end(args);
}

XPAPI void
xpost_log_print_dump(Xpost_Log_Level level,
                     const char *fct,
                     const char *fmt, ...)
{
    va_list args_copy;
    va_list args;
    char *str;
    int res;
    int s;

    if (!fmt)
    {
        fprintf(stderr, "ERROR: %s() fmt == NULL\n", __func__);
        return;
    }

    if (level > _xpost_log_level)
        return;

    va_start(args, fmt);
    va_copy(args_copy, args);

    s = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (s == -1)
    {
        va_end(args_copy);
        return;
    }

    str = (char *)malloc((s + 2) * sizeof(char));
    if (!str)
    {
        va_end(args_copy);
        return;
    }

    s = vsnprintf(str, s + 1, fmt, args_copy);
    va_end(args_copy);
    if (s == -1)
    {
        free(str);
        return;
    }
    str[s] = '\n';
    str[s + 1] = '\0';

    if (!_xpost_log_dump())
    {
        free(str);
        return;
    }
    fprintf(_xpost_log_dump_file, "%s() : ", fct);
    res = fprintf(_xpost_log_dump_file, "%s", str);
    free(str);

    if (res < 0)
        return;

    if ((int)res != (s + 1))
        fprintf(stderr, "ERROR: %s(): want to write %d bytes, %d written\n", __func__, s + 1, res);
}
