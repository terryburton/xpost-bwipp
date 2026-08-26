/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_main.c
 * @brief The library's own entry points: starting a run and taking it down again.
 *
 * A caller that embeds the interpreter reaches it here. This is the small
 * file of the two that share the name: the large one is the command-line
 * program in src/bin, which is one caller of this among others.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h>

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h> /* WSAStartup WSACleanup */
# undef WIN32_LEAN_AND_MEAN
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_compat.h"
#include "xpost_object.h"
#include "xpost_memory.h"
#include "xpost_font.h"
#include "xpost_main.h"
#include "xpost_private.h"


/*============================================================================*
 *                                  Local                                     *
 *============================================================================*/

static int _xpost_init_count = 0;
static char _xpost_lib_dir[XPOST_PATH_MAX];
static char *_xpost_data_dir = NULL;

/*============================================================================*
 *                                 Global                                     *
 *============================================================================*/

/*============================================================================*
 *                                   API                                      *
 *============================================================================*/

static void _xpost_run_at_quit(void);
static void _data_dir_drop(void);

/* An init that gives up partway takes down what it had already brought
   up. Its caller is told the library is not up, and rightly does not
   call the teardown -- so what came up before the refusal would be left
   up, and the next init would bring those modules up a second time over
   what the first left. */
static int _init_gave_up(void)
{
    _xpost_run_at_quit();
    return --_xpost_init_count;
}

XPAPI int
xpost_init(void)
{
    char tmp1[XPOST_PATH_MAX];
    size_t l;

    if (++_xpost_init_count != 1)
        return _xpost_init_count;

    if (!xpost_compat_init())
        return _init_gave_up();
    /* Compat and the log are shared with libxpost_dsc, which has no
       lifetime of its own to be registered against, so they cannot ask
       for themselves the way a module of this library does. Where they
       are brought up is then the one place that knows they are, and it
       asks on their behalf. */
    (void)xpost_at_quit(xpost_compat_quit);

    if (!xpost_log_init())
        return _init_gave_up();
    (void)xpost_at_quit(xpost_log_quit);

    /* The library's own path is the first data-dir candidate: an
       installed build finds <libdir>/../share/xpost beside itself. A
       host that cannot name the path -- one without dladdr -- loses
       only this candidate; _xpost_lib_dir stays empty, _xpost_data_dir
       stays unset, and the interpreter's own search still has the
       environment and the compiled-in directories to try. */
    if (xpost_module_path_get(xpost_init, _xpost_lib_dir, XPOST_PATH_MAX))
    {
        l = strlen(_xpost_lib_dir);
        memcpy(tmp1, _xpost_lib_dir, l);
        memcpy(tmp1 + l, "/../share/xpost", sizeof("/../share/xpost"));
        /* An uninstalled build has no <libdir>/../share/xpost: try the
           source tree's data directory next, relative to the built
           library (build/src/lib and src/lib/.libs both sit three
           levels below the tree root), so the built interpreter runs
           from any directory. A miss is not fatal -- the interpreter's
           init.ps search verifies each candidate and falls back to
           XPOST_DATA_DIR and to data/ relative paths (see
           setlocalconfig). */
        _xpost_data_dir = xpost_realpath(tmp1);
        if (!_xpost_data_dir)
        {
            memcpy(tmp1 + l, "/../../../data", sizeof("/../../../data"));
            _xpost_data_dir = xpost_realpath(tmp1);
        }
    }
    /* coming up is what asks to be taken down */
    (void)xpost_at_quit(_data_dir_drop);

    if (!xpost_memory_init())
        return _init_gave_up();

    if (!xpost_font_init())
        return _init_gave_up();

    return _xpost_init_count;
}

static void _data_dir_drop(void)
{
    free(_xpost_data_dir);
    _xpost_data_dir = NULL;
}

/* What has asked to be called when the library goes down. Sixteen is
   more than the modules that hold anything for a lifetime, and the
   refusal is reported rather than silent, so a seventeenth arrives as a
   message and not as a module that quietly stopped being torn down. */
#define XPOST_AT_QUIT_MAX 16
static void (*_xpost_at_quit[XPOST_AT_QUIT_MAX])(void);
static int _xpost_at_quit_n = 0;

int
xpost_at_quit(void (*fn)(void))
{
    int i;

    if (!fn)
        return 1;
    for (i = 0; i < _xpost_at_quit_n; i++)
        if (_xpost_at_quit[i] == fn)
            return 1;
    if (_xpost_at_quit_n == XPOST_AT_QUIT_MAX)
    {
        XPOST_LOG_ERR("no room to be called at the library teardown:"
                      " %d are registered", _xpost_at_quit_n);
        return 0;
    }
    _xpost_at_quit[_xpost_at_quit_n++] = fn;
    return 1;
}

/* Call what asked to be called, in the reverse of the order it asked in,
   so each module reaches what it was built on before that goes. The list
   is emptied as it runs: a later lifetime registers afresh, and a second
   pass finds nothing to call. */
static void _xpost_run_at_quit(void)
{
    while (_xpost_at_quit_n > 0)
        _xpost_at_quit[--_xpost_at_quit_n]();
}

XPAPI int
xpost_quit(void)
{
    if (_xpost_init_count <= 0)
    {
        XPOST_LOG_ERR("Init count not greater than 0 in shutdown.");
        return 0;
    }

    if (--_xpost_init_count != 0)
        return _xpost_init_count;

    _xpost_run_at_quit();

    return _xpost_init_count;
}

XPAPI void
xpost_version_get(int *maj, int *min, int *mic)
{
    if (maj) *maj = XPOST_VERSION_MAJ;
    if (min) *min = XPOST_VERSION_MIN;
    if (mic) *mic = XPOST_VERSION_MIC;
}

XPAPI const char *
xpost_lib_dir_get(void)
{
    return _xpost_lib_dir;
}

XPAPI const char *
xpost_data_dir_get(void)
{
    return _xpost_data_dir;
}
