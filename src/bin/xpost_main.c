/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Vincent Torri
 * Copyright (c) 2013 Thorsten Behrens
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_main.c
 * @brief The command line: what a run was asked for, settled before a context exists.
 *
 * Everything here happens before or around the interpreter rather than
 * inside it: which device to draw on, where output goes, what the language
 * is built with, and which of the options change the language itself --
 * those are written into an image of virtual memory, so a run cannot read
 * back a language it did not ask for.
 *
 * The library is what runs the program. This decides what to hand it.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_context.h"
#include "xpost_name.h" /* a word travels to the classes as a name */
#include "xpost_dev_generic.h" /* the device option roster the -p switch is held to */
/* xpost_isatty: the same question the interpreter asks of standard
   input when it decides whether this run has a user at the other end
   of it, so that the program and the interpreter answer it alike */
#include "xpost_compat.h"



#define XPOST_MAIN_IF_OPT(so, lo, opt)  \
if ((!strcmp(argv[i], so)) || \
   (!strncmp(argv[i], lo, sizeof(lo) - 1))) \
{ \
    if (*(argv[i] + 2) == '\0') \
    { \
        if ((i + 1) < argc) \
        { \
            i++; \
            opt = argv[i]; \
        } \
        else \
        { \
            XPOST_LOG_ERR("missing option value"); \
            _xpost_main_usage(stderr, filename); \
            goto quit_xpost; \
        } \
    } \
    else \
    { \
        if (!*(argv[i] + sizeof(lo) - 1)) \
        { \
            XPOST_LOG_ERR("missing option value"); \
            _xpost_main_usage(stderr, filename); \
            goto quit_xpost; \
        } \
        else \
        { \
            opt = argv[i] + sizeof(lo) - 1; \
        } \
    } \
}

static const char *_xpost_main_devices[] =
{
    "pgm",
    "ppm",
    "pbm",
    "tiff",
    "null",
    "bbox",
#ifdef _WIN32
    "gdi",
    "gl",
#endif
#ifdef HAVE_XCB
    "xcb",
#endif
    "bgr",
    "raster",
    "record",
    "pdfwrite",
    "dscwrite",
    "svgwrite",
#ifdef HAVE_LIBPNG
    "png",
    "pngalpha",
#endif
#ifdef HAVE_LIBJPEG
    "jpeg",
#endif
    NULL
};

static void
_xpost_main_license(void)
{
    printf("BSD 3-clause\n");
}

static void
_xpost_main_version(const char *filename)
{
    int maj;
    int min;
    int mic;

    xpost_version_get(&maj, &min, &mic);
    printf("%s %d.%d.%d\n", filename, maj, min, mic);
}

/* The greeting a session opens with: what the program is, and who owns
   it. It is addressed to somebody, so it is printed to somebody and to
   nobody else -- see _xpost_main_greeted below for who that is.

   It goes to the standard output because that is the channel the
   session it opens is conducted on: the prompt that follows it, the
   statements typed at that prompt and the answers to them all travel
   there. A run with nobody at the other end is not given it at all,
   which is what keeps that channel carrying the program's output and
   nothing else. */
static void
_xpost_main_banner(void)
{
    int maj;
    int min;
    int mic;

    xpost_version_get(&maj, &min, &mic);
    printf("Xpost %d.%d.%d\n", maj, min, mic);
    printf("Copyright (C) 2013, Michael Joshua Ryan. All rights reserved.\n");
    printf("Licensed under the BSD 3-clause licence, without any warranty;\n");
    printf("the file COPYING states the terms.\n");
}

/* Whether this run has somebody to greet.

   Two of the three conditions are the ones the interpreter itself reads
   to decide whether to offer the interactive session once the program
   has ended: standard input must be a terminal, and no output file may
   have been named, because naming one says the invocation is something
   waiting for that file rather than somebody typing. A run that will
   not be offered a session is a run with nobody to open one for.

   The third is that the caller did not ask for quiet. -q suppresses the
   interpreter's messages about itself, and a greeting is the first of
   them.

   The three options that report and exit -- -V, -L and -h -- never
   reach here: each is answered inside the option loop and leaves from
   there, so what such a run writes is its report and nothing else. That
   is what makes -V readable by a script: one line, the version, however
   this greeting later changes. */
static int
_xpost_main_greeted(int quiet_asked, const char *output_file)
{
    return !quiet_asked && !output_file && xpost_isatty(fileno(stdin));
}

/* permit the directory containing `path`, for writing when `forwrite` */
static void
_xpost_permit_file_dir(const char *path, int forwrite)
{
    char buf[4096];
    char *slash;

    if (!path || strlen(path) >= sizeof buf)
        return;
    strcpy(buf, path);
    slash = strrchr(buf, '/');
    if (slash)
    {
        *slash = '\0';
        if (buf[0] == '\0')
            strcpy(buf, "/");
    }
    else
    {
        strcpy(buf, ".");
    }
    if (forwrite)
        xpost_path_permit_write(buf);
    else
        xpost_path_permit_read(buf);
}

/* The row of the device-option roster a -p key names, or NULL. The
   roster is what the compiled writers read off their device
   dictionaries, each driver stating its own (xpost_dev_option_roster),
   so a knob a driver grows joins this switch without a list here. */
static const Xpost_Dev_Option *
_xpost_main_param_find(const char *key, size_t klen)
{
    const Xpost_Dev_Option *roster;
    int i, n;

    roster = xpost_dev_option_roster(&n);
    for (i = 0; i < n; i++)
        if (strlen(roster[i].key) == klen &&
            memcmp(roster[i].key, key, klen) == 0)
            return &roster[i];
    return NULL;
}

/* One -p operand, held to the roster before anything is made or
   rendered: a key no device of this build reads is refused naming the
   keys that exist, and a value is held to its row's vocabulary or
   range, so a misspelling cannot fall back to a default silently --
   the defect class the device modes and the filter vocabulary were
   closed against. Answers 1, or 0 having said why. */
static int
_xpost_main_param_check(const char *filename, const char *kv)
{
    const Xpost_Dev_Option *opt;
    const Xpost_Dev_Option *roster;
    const char *eq = strchr(kv, '=');
    const char *val;
    int i, n;

    if (!eq || eq == kv || !eq[1])
    {
        fprintf(stderr, "%s: a device parameter is key=value, not \"%s\"\n",
                filename, kv);
        return 0;
    }
    val = eq + 1;
    opt = _xpost_main_param_find(kv, (size_t)(eq - kv));
    if (!opt)
    {
        fprintf(stderr, "%s: -p takes no key \"%.*s\"; the keys this build"
                " takes are:", filename, (int)(eq - kv), kv);
        roster = xpost_dev_option_roster(&n);
        for (i = 0; i < n; i++)
            fprintf(stderr, "%s %s", i ? "," : "", roster[i].key);
        if (n == 0)
            fprintf(stderr, " none");
        fprintf(stderr, "\n");
        return 0;
    }
    if (opt->words)
    {
        for (i = 0; opt->words[i]; i++)
            if (strcmp(opt->words[i], val) == 0)
                return 1;
        fprintf(stderr, "%s: %s takes no word \"%s\"; the words it takes"
                " are:", filename, opt->key, val);
        for (i = 0; opt->words[i]; i++)
            fprintf(stderr, "%s %s", i ? "," : "", opt->words[i]);
        fprintf(stderr, "\n");
        return 0;
    }
    else
    {
        char *end;
        long v;

        errno = 0;
        v = strtol(val, &end, 10);
        if (errno || *end || end == val || v < opt->min || v > opt->max)
        {
            fprintf(stderr, "%s: %s takes no value \"%s\"; the value is an"
                    " integer from %d to %d\n", filename, opt->key, val,
                    opt->min, opt->max);
            return 0;
        }
    }
    return 1;
}

/* Record the checked -p operands as the run's defaults, through the
   channel an embedder's options go through: onto the device classes,
   where every instance copied from one carries them and a program's
   own page-device request still overrides. A word travels as a name,
   which lives in global memory like the setting it becomes. */
static int
_xpost_main_params_apply(Xpost_Context *ctx, char **params, int count)
{
    int i;

    for (i = 0; i < count; i++)
    {
        const char *eq = strchr(params[i], '=');
        const Xpost_Dev_Option *opt;
        Xpost_Object v;

        if (!eq)
            return 0;
        opt = _xpost_main_param_find(params[i], (size_t)(eq - params[i]));
        if (!opt)
            return 0;
        if (opt->words)
            v = xpost_object_cvlit(xpost_name_cons(ctx, eq + 1));
        else
            v = xpost_int_cons((integer)strtol(eq + 1, NULL, 10));
        if (xpost_dev_option_default(ctx, opt->key, v,
                                     opt->classname, opt->altclassname))
            return 0;
    }
    return 1;
}

static void
_xpost_main_usage(FILE *out, const char *filename)
{
    int i;

    fprintf(out, "Usage: %s [options] [file.ps]\n\n", filename);
    fprintf(out, "PostScript level 3 interpreter\n\n");
    fprintf(out, "Options:\n");
    fprintf(out, "  -o, --output=[FILE]                output file; the run ends with the program\n");
    fprintf(out, "  -d, --device=[STRING]              device name\n");
    fprintf(out, "  -Dname=token, --define name=token  add definition to userdict\n");
    fprintf(out, "  -p key=value, --device-param key=value\n");
    fprintf(out, "                                     a codec tuning key's default for this run;\n");
    fprintf(out, "                                     repeatable, and a program's own\n");
    fprintf(out, "                                     setpagedevice request still overrides\n");
    fprintf(out, "  -I[DIR], --include [DIR]           add a resource search directory\n");
    fprintf(out, "  --no-graphics                      lock down and run without loading graphics\n");
    fprintf(out, "  --no-sandbox                       allow the program unrestricted file access\n");
    fprintf(out, "  --enable-dps                       install the Display PostScript context operators\n");
    fprintf(out, "  --jobserver                        read standard input as a Control-D-framed job stream\n");
    fprintf(out, "  -g, --geometry=WxH{+-}X{+-}Y       geometry specification\n");
    fprintf(out, "  -s, --spill=auto|never|always      where a retained page's marks are held\n");
    fprintf(out, "  -b, --band-bytes=BYTES             what one band of a page may cost\n");
    fprintf(out, "  -q, --quiet                        suppress interpreter messages (default)\n");
    fprintf(out, "  -v, --verbose                      do not go quiet into that good night\n");
    fprintf(out, "  -t, --trace                        add additional tracing messages, implies -v\n");
    fprintf(out, "  -L, --license                      show program license\n");
    fprintf(out, "  -V, --version                      show program version\n");
    fprintf(out, "  -h, --help                         show this message\n");
    fprintf(out, "\n");
    {
        const Xpost_Dev_Option *roster;
        int n, j;

        roster = xpost_dev_option_roster(&n);
        if (n > 0)
        {
            fprintf(out, "  Device tuning keys this build takes:");
            for (j = 0; j < n; j++)
                fprintf(out, "%s %s", j ? "," : "", roster[j].key);
            fprintf(out, "\n\n");
        }
    }
    fprintf(out, "  Supported devices:\n");
    i = 0;
    while (_xpost_main_devices[i])
        fprintf(out, "\t%s\n", _xpost_main_devices[i++]);
    fprintf(out, "\n");
    fprintf(out, "  A device whose page may arrive a band at a time holds a\n");
    fprintf(out, "  band of it rather than the page:");
#define XPOST_BAND_HELP(name) fprintf(out, " %s", name);
    XPOST_BANDS_BY_DEFAULT(XPOST_BAND_HELP)
#undef XPOST_BAND_HELP
    fprintf(out, "\n");
    fprintf(out, "  A page small enough to fit one band is held whole, so\n");
    fprintf(out, "  this costs a small page nothing.\n");
    fprintf(out, "\n");
    fprintf(out, "  How large a band is is --band-bytes, in bytes of raster\n");
    fprintf(out, "  held at once, and it decides both things above: a page the\n");
    fprintf(out, "  budget covers arrives in one band, which is the page, so\n");
    fprintf(out, "  it is painted directly and nothing is written down. The\n");
    fprintf(out, "  default covers every ordinary sheet, so lowering it is\n");
    fprintf(out, "  what bands an ordinary page. It bounds the marks too --\n");
    fprintf(out, "  they go to a scratch file past a budget's worth of them --\n");
    fprintf(out, "  so a banded page costs a band of raster and a budget of\n");
    fprintf(out, "  marks whatever the drawing. currentsystemparams reports\n");
    fprintf(out, "  the budget as MaxBandBytes and the band it bought as\n");
    fprintf(out, "  CurBandHeight.\n");
    fprintf(out, "\n");
    fprintf(out, "  A device may be given a mode after a colon:\n");
    fprintf(out, "\tDEVICE:whole    hold the whole page rather than a band of\n");
    fprintf(out, "\t                it, which is what to compare against\n");
    fprintf(out, "\tDEVICE:band     hold a band of it whatever the page size\n");
    fprintf(out, "\traster:FORMAT   the pixel format a lent framebuffer is in\n");
    fprintf(out, "\t                (rgb, argb, bgr, bgra)\n");
    fprintf(out, "\n");
    fprintf(out, "  record is the class a banded page is held by, and takes no\n");
    fprintf(out, "  mode: selecting it is the same as ppm:band.\n");
    fprintf(out, "\n");
    fprintf(out, "  A page held a band at a time is held as the marks that made\n");
    fprintf(out, "  it, and --spill says where those marks go:\n");
    fprintf(out, "\tauto      in memory while they come to less than the\n");
    fprintf(out, "\t          raster banding the page saves, and in a scratch\n");
    fprintf(out, "\t          file past that. The default, and the only one\n");
    fprintf(out, "\t          that bounds what a page costs without touching\n");
    fprintf(out, "\t          a disk for a page that does not need it\n");
    fprintf(out, "\tnever     in memory whatever they come to, touching no\n");
    fprintf(out, "\t          scratch file at all. What a page costs then\n");
    fprintf(out, "\t          follows its drawing with no limit\n");
    fprintf(out, "\talways    in a scratch file from the first mark; refused\n");
    fprintf(out, "\t          at start-up where no scratch file can be made\n");    fprintf(out, "\n");
    fprintf(out, "  A run confines the program's file access to the current\n");
    fprintf(out, "  directory, the directory the program was read from, the\n");
    fprintf(out, "  directory the output is written to, and any -I directory,\n");
    fprintf(out, "  and refuses every other disk open with invalidfileaccess.\n");
    fprintf(out, "  The temporary directory is not among them, being shared\n");
    fprintf(out, "  with everyone rather than belonging to the run.\n");
    fprintf(out, "  --no-sandbox lifts the whole of it.\n");
}

static int
_xpost_atoi(char *str, int *v, char **endptr)
{
    long val;

    errno = 0;
    val = strtol(str, endptr, 10);;

    if (((errno == ERANGE) &&
         ((val == LONG_MAX) || (val == LONG_MIN))) ||
        ((errno != 0) && (val == 0)))
        return 0;

    if (*endptr == str)
        return 0;

    *v = (int)val;

    return 1;
}

static int
_xpost_geometry_parse(const char *geometry, int *width, int *height, int *xoffset, int *xsign, int *yoffset, int *ysign)
{
    char *str;
    char *endptr;
    int val;

    if (!geometry)
        return 0;

    /* width */
    str = (char *)geometry;
    if (!_xpost_atoi(str, &val, &endptr))
        return 0;

    *width = val;

    if (*endptr != 'x')
        return 0;

    /* height */
    str = endptr + 1;
    if (!_xpost_atoi(str, &val, &endptr))
        return 0;

    *height = val;

    if (*endptr == '+')
        *xsign = 1;
    else if (*endptr == '-')
        *xsign = -1;
    else
        return 0;

    /* xoffset */
    str = endptr + 1;
    if (!_xpost_atoi(str, &val, &endptr))
        return 0;

    *xoffset = val;

    if (*endptr == '+')
        *ysign = 1;
    else if (*endptr == '-')
        *ysign = -1;
    else
        return 0;

    /* yoffset */
    str = endptr + 1;
    if (!_xpost_atoi(str, &val, &endptr))
        return 0;

    *yoffset = val;

    if (*endptr != '\0')
        return 0;

    return 1;
}

/* Add one copy of str to a list that grows by one each time. The list and
   its count travel together, and a list that has taken nothing yet is the
   null pointer with a count of zero. Answers zero if the copy or the room
   for it could not be had, leaving the list exactly as it was. */
/* A program that reads each named program in turn. The names are the
   caller's, so each goes into the string literal escaped: a backslash
   and the two parentheses are what a literal cannot carry raw (PLRM
   3.2.2), and a name holding one of them would otherwise end the
   literal early or leave it unclosed. */
static char *
_xpost_main_driver(char **files, int count)
{
    size_t len = 1;
    char *out, *w;
    int i;
    const char *r;

    for (i = 0; i < count; i++)
        len += 2 * strlen(files[i]) + sizeof "() run " - 1;
    out = malloc(len);
    if (!out)
        return NULL;
    w = out;
    for (i = 0; i < count; i++)
    {
        *w++ = '(';
        for (r = files[i]; *r; r++)
        {
            if (*r == '\\' || *r == '(' || *r == ')')
                *w++ = '\\';
            *w++ = *r;
        }
        *w++ = ')';
        *w++ = ' '; *w++ = 'r'; *w++ = 'u'; *w++ = 'n'; *w++ = ' ';
    }
    *w = '\0';
    return out;
}

static int
_xpost_main_list_add(char ***list, int *count, const char *str)
{
    char **grown;
    char *copy;

    copy = strdup(str);
    if (!copy)
        return 0;
    grown = realloc(*list, (*count + 1) * sizeof *grown);
    if (!grown)
    {
        free(copy);
        return 0;
    }
    *list = grown;
    (*list)[(*count)++] = copy;
    return 1;
}

/* Give back a list and everything in it, leaving it as one that has
   taken nothing. Answering for a list already given back costs nothing,
   so an ending may call it without knowing which ones were reached. */
static void
_xpost_main_list_free(char ***list, int *count)
{
    int n;

    for (n = 0; n < *count; ++n)
        free((*list)[n]);
    free(*list);
    *list = NULL;
    *count = 0;
}

static void
_xpost_main_interrupt(int sig)
{
    (void)sig;
#ifdef _WIN32
    /* the C runtime resets the disposition before the handler runs */
    signal(SIGINT, _xpost_main_interrupt);
#endif
    xpost_interrupt();
}

int main(int argc, char *argv[])
{
    Xpost_Context *ctx;
    const char *geometry = NULL;
    const char *output_file = NULL;
    const char *device = NULL;
    const char *spill = NULL;
    const char *band_bytes = NULL;
    char **psfiles = NULL;
    int num_psfiles = 0;
    const char *filename = argv[0];
    const char *define = NULL;
    char **defs = NULL;
    int num_defs = 0;
    char **params = NULL;
    const char *param;
    int num_params = 0;
    char **incs = NULL;
    int num_incs = 0;
    int no_graphics = 0;
    int enable_dps = 0;
    int no_sandbox = 0;
    int jobserver = 0;
    int output_msg = XPOST_OUTPUT_MESSAGE_QUIET;
    int have_device;
    int width = -1;
    int height = -1;
    int xoffset = 0;
    int yoffset = 0;
    int xsign = 1;
    int ysign = 1;
    int have_geometry = 0;
    int quiet_asked = 0;
    int i;

#ifdef HAVE_SIGACTION
    struct sigaction sa, oldsa;

    sa.sa_handler = SIG_IGN;
    sigaction(SIGTRAP, &sa, &oldsa);
#endif

#ifdef DEBUG_ENTS
    fprintf(stderr, "EXTRA_BITS_SIZE = %u\n",
            (unsigned int)XPOST_OBJECT_TAG_EXTRA_BITS_SIZE);
    fprintf(stderr, "COMP_MAX_ENT = %u\n",
            (unsigned int)XPOST_OBJECT_COMP_MAX_ENT);
#endif

#ifdef _WIN32
    device = "gdi";
#elif defined HAVE_XCB
    device = "xcb";
#else
    device = "pgm";
#endif

    if (!xpost_init())
    {
        fprintf(stderr, "Fail to initialize xpost\n");
        return -1;
    }

    /* control-C requests the PostScript interrupt error rather than
       killing the process; a blocked read resumes and the request
       lands at the next evaluation step */
#ifdef _WIN32
    signal(SIGINT, _xpost_main_interrupt);
#else
    {
        struct sigaction sa;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = _xpost_main_interrupt;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGINT, &sa, NULL);
    }
#endif

    i = 0;
    while (++i < argc)
    {
        if (*argv[i] == '-')
        {
            /* The three options that report and stop leave through the
               same shutdown as every other exit from here. xpost_init
               above took what the process holds for as long as it runs --
               the font configuration's cache, and on some platforms the
               socket library and a handle on the system's random source --
               and xpost_quit is what gives each of them back; a path that
               returned without it would hold them to the end of the
               process and be answerable for them there. The label below
               is the failing exit and these three succeeded. */
            if ((!strcmp(argv[i], "-h")) ||
                (!strcmp(argv[i], "--help")))
            {
                /* asked for, so it is this run's output and goes where
                   output goes; the usage printed over a command line
                   nobody could follow is a complaint and goes with the
                   rest of them */
                _xpost_main_usage(stdout, filename);
                goto quit_asked;
            }
            else if ((!strcmp(argv[i], "-V")) ||
                     (!strcmp(argv[i], "--version")))
            {
                _xpost_main_version(filename);
                goto quit_asked;
            }
            else if ((!strcmp(argv[i], "-L")) ||
                     (!strcmp(argv[i], "--license")))
            {
                _xpost_main_license();
                goto quit_asked;
            }
            else if ((!strncmp(argv[i], "-D", 2)) ||
                     (!strcmp(argv[i], "--define")))
            {
                if (argv[i][1]=='D')
                {
                    define = argv[i] + 2;
                }
                else
                {
                    if ((i + 1) < argc)
                    {
                        ++i;
                        define = argv[i];
                    }
                    else
                    {
                        XPOST_LOG_ERR("missing option value");
                        _xpost_main_usage(stderr, filename);
                        goto quit_xpost;
                    }

                }
                if (!_xpost_main_list_add(&defs, &num_defs, define))
                {
                    XPOST_LOG_ERR("out of memory");
                    goto quit_xpost;
                }
            }
            else if ((!strncmp(argv[i], "-p", 2)) ||
                     (!strcmp(argv[i], "--device-param")))
            {
                if (argv[i][1] == 'p' && argv[i][2] != '\0')
                {
                    param = argv[i] + 2;
                }
                else
                {
                    if ((i + 1) < argc)
                    {
                        ++i;
                        param = argv[i];
                    }
                    else
                    {
                        XPOST_LOG_ERR("missing option value");
                        _xpost_main_usage(stderr, filename);
                        goto quit_xpost;
                    }
                }
                /* held to the roster here, before anything is made:
                   an ending taken now has rendered nothing */
                if (!_xpost_main_param_check(filename, param))
                    goto quit_xpost;
                if (!_xpost_main_list_add(&params, &num_params, param))
                {
                    XPOST_LOG_ERR("out of memory");
                    goto quit_xpost;
                }
            }
            else if ((!strncmp(argv[i], "-I", 2)) ||
                     (!strcmp(argv[i], "--include")))
            {
                const char *inc;
                if (argv[i][1] == 'I' && argv[i][2])
                {
                    inc = argv[i] + 2;
                }
                else if ((i + 1) < argc)
                {
                    inc = argv[++i];
                }
                else
                {
                    XPOST_LOG_ERR("missing option value");
                    _xpost_main_usage(stderr, filename);
                    goto quit_xpost;
                }
                if (!_xpost_main_list_add(&incs, &num_incs, inc))
                {
                    XPOST_LOG_ERR("out of memory");
                    goto quit_xpost;
                }
            }
            else if (!strcmp(argv[i], "--no-sandbox"))
            {
                no_sandbox = 1;
            }
            /* Read standard input as a job stream: a run of programs framed
               by Control-D, each reverting the whole of virtual memory to
               the initial-VM baseline at its boundary (PLRM 3.7.7 server
               loop). The mechanism the embedding worker uses, offered to a
               caller that drives the interpreter the same way. */
            else if (!strcmp(argv[i], "--jobserver"))
            {
                jobserver = 1;
            }
            else if (!strcmp(argv[i], "--enable-dps"))
            {
                xpost_dps_set(1);
                enable_dps = 1;
            }
            /* Quiet is where the messages start, so it is also what the
               run is left at when nothing says otherwise -- and the two
               are not the same answer to the same question. Whether the
               caller asked is recorded beside how much was asked for,
               because a greeting is owed to a session that said nothing
               and not to one that asked for silence. -v and -t ask for
               more rather than less, so each takes the request back. */
            else if ((!strcmp(argv[i], "-q")) ||
                     (!strcmp(argv[i], "--quiet")))
            {
                output_msg = XPOST_OUTPUT_MESSAGE_QUIET;
                quiet_asked = 1;
            }
            else if (!strcmp(argv[i], "--no-graphics"))
            {
                no_graphics = 1;
            }
            else if ((!strcmp(argv[i], "-v")) ||
                     (!strcmp(argv[i], "--verbose")))
            {
                output_msg = XPOST_OUTPUT_MESSAGE_VERBOSE;
                quiet_asked = 0;
            }
            else if ((!strcmp(argv[i], "-t")) ||
                     (!strcmp(argv[i], "--trace")))
            {
                output_msg = XPOST_OUTPUT_MESSAGE_TRACING;
                quiet_asked = 0;
            }
            else XPOST_MAIN_IF_OPT("-o", "--output=", output_file)
            else XPOST_MAIN_IF_OPT("-d", "--device=", device)
            else XPOST_MAIN_IF_OPT("-g", "--geometry=", geometry)
            else XPOST_MAIN_IF_OPT("-s", "--spill=", spill)
            else XPOST_MAIN_IF_OPT("-b", "--band-bytes=", band_bytes)
            else
            {
                fprintf(stderr, "%s: unknown option %s\n", filename, argv[i]);
                _xpost_main_usage(stderr, filename);
                goto quit_xpost;
            }
        }
        else
        {
            /* Every program named is run, in the order given. */
            if (!_xpost_main_list_add(&psfiles, &num_psfiles, argv[i]))
            {
                XPOST_LOG_ERR("out of memory");
                goto quit_xpost;
            }
        }
    }

    /* The options are all in, so what this run is is settled and the
       run can be opened to whoever is watching it. Nothing has been
       written to either channel yet: a run given an option it does not
       know leaves above with its complaint and its usage on the log
       channel and its output channel untouched. */
    if (_xpost_main_greeted(quiet_asked, output_file))
        _xpost_main_banner();

    /* the parse answers whether it understood the geometry, so a
       geometry that was given and not understood is the error; one that
       was not given at all leaves the default page size standing */
    if (geometry)
    {
        have_geometry = _xpost_geometry_parse(geometry,
                                              &width, &height,
                                              &xoffset, &xsign,
                                              &yoffset, &ysign);
        if (!have_geometry)
        {
            XPOST_LOG_ERR("bad formatted geometry");
            goto quit_xpost;
        }
        /* a run being narrated is told what its geometry came to, on
           the log channel the rest of the narration goes to */
        if (output_msg != XPOST_OUTPUT_MESSAGE_QUIET)
        {
            fprintf(stderr, "geometry %s reads %dx%d%c%d%c%d\n",
                    geometry, width, height,
                    (xsign == 1) ? '+' : '-', xoffset,
                    (ysign == 1) ? '+' : '-', yoffset);
        }
    }

    {
        char *devstr = strdup(device);
        char *subdevice;
        if (!devstr)
        {
            XPOST_LOG_ERR("out of memory");
            goto quit_xpost;
        }
        if ((subdevice=strchr(devstr,':')))
            *subdevice++='\0';
        /* check devices */
        have_device = 0;
        i = 0;
        while (_xpost_main_devices[i])
        {
            if (strcmp(_xpost_main_devices[i], devstr) == 0)
            {
                have_device = 1;
                break;
            }
            i++;
        }
        free(devstr);
    }

    if (!have_device)
    {
        XPOST_LOG_ERR("wrong device.");
        _xpost_main_usage(stderr, filename);
        goto quit_xpost;
    }

    /* An image of virtual memory carries the language it was written
       with, and it is read as the context is created -- before the
       context can be asked what it wants. So the options that change the
       language rather than what a run does with it are said here, and
       written into the image: a run without graphics reads an image
       without graphics or writes one, rather than every such run having
       to build the language because no image could be told from another. */
    {
        unsigned int image_config = 0;

        if (no_graphics)
            image_config |= XPOST_VM_IMAGE_CONFIG_NO_GRAPHICS;
        if (enable_dps)
            image_config |= XPOST_VM_IMAGE_CONFIG_DPS;
        xpost_vm_image_config_set(image_config);
    }

    /* Where a retained page's marks are held, which the context reads as
       it is made. A word that is none of the three is refused naming
       what was given, the way an unrecognised device mode is: nothing
       further down reads a state it does not recognise, so one passed on
       would be taken for the default and the run would quietly do
       something else. */
    if (spill && !xpost_record_spill_set(spill))
    {
        XPOST_LOG_ERR("there is no way \"%s\" of holding a retained page's"
                      " marks; the ways there are: auto, never, always",
                      spill);
        goto quit_xpost;
    }

    /* What one band of a page may cost, which the context reads as it is
       made. Refused naming what was given and the range it takes, for
       the reason the state above is: a budget nothing recognises would
       be dropped and the run would band to a number the caller never
       chose. What the budget will not buy a single row of is refused
       further on, where a device says what a row of it costs. */
    if (band_bytes)
    {
        char *end;
        long budget;

        errno = 0;
        budget = strtol(band_bytes, &end, 10);
        if (errno || end == band_bytes || *end
            || !xpost_band_bytes_set(budget))
        {
            XPOST_LOG_ERR("\"%s\" is no budget for a band of a page; what one"
                          " may cost is a whole number of bytes from 1 to %ld",
                          band_bytes, XPOST_BAND_BYTES_MAX);
            goto quit_xpost;
        }
    }

    if (!(ctx = xpost_create(device,
                             XPOST_OUTPUT_FILENAME,
                             output_file,
                             XPOST_SHOWPAGE_DEFAULT,
                             output_msg,
                             have_geometry ? XPOST_USE_SIZE : XPOST_IGNORE_SIZE,
                             width, height)))
    {
        XPOST_LOG_ERR("Failed to initialize.");
        goto quit_xpost;
    }

    if (no_graphics)
        xpost_skip_graphics_set(ctx, 1);

    if (jobserver)
        xpost_jobserver_set(ctx, 1);

    /* Naming the file the output goes to says what this invocation is:
       something waiting for that file, not somebody at a keyboard. So the
       run ends where the named program ends, which is where a job ends
       (PLRM 3.7.7), and the interactive executive is never offered after
       it -- an executive would read standard input and execute it, which
       is a second program nobody asked to run. A program that does want a
       session after itself asks for one the way the language provides,
       with the executive operator. */
    if (output_file)
        xpost_batch_set(ctx, 1);

    XPOST_LOG_INFO("defs=%p", (void*)defs);
    if (defs){
        /* the program is about to run against these definitions; one that
           could not be stored is a name the program will find undefined,
           and the report belongs here rather than wherever it is first
           looked up */
        if (!xpost_add_definitions(ctx, num_defs, defs))
            fprintf(stderr, "%s: cannot record the -D definitions\n", filename);
        for (i = 0; i < num_defs; ++i)
        {
            free(defs[i]);
        }
        free(defs);
        defs = NULL;
        num_defs = 0;
    }

    /* the run's device defaults, recorded before the program runs so
       the first device made carries them; each was already held to the
       roster where it was read */
    if (params)
    {
        if (!_xpost_main_params_apply(ctx, params, num_params))
        {
            fprintf(stderr, "%s: cannot record the -p device parameters\n",
                    filename);
            goto quit_xpost;
        }
        _xpost_main_list_free(&params, &num_params);
    }

    /* seed the resource search path from -I directories */
    if (num_incs > 0)
    {
        for (i = 0; i < num_incs; i++)
        {
            /* a directory that did not reach the search path is one no
               resource will ever be found under, and the run's only
               symptom is the lookup that comes up empty much later */
            if (!xpost_add_resource_dir(ctx, incs[i]))
                fprintf(stderr, "%s: cannot add resource directory %s\n",
                        filename, incs[i]);
            /* resource files are read from beneath this directory */
            xpost_path_permit_read(incs[i]);
            free(incs[i]);
        }
        free(incs);
        incs = NULL;
        num_incs = 0;
    }

    /* Confine the program to its working area unless --no-sandbox: the
       current directory, the input file's directory (read) and the
       output file's directory (write). The interpreter permits its own
       data directory (init.ps, callout.ps) during start-up; -I resource
       directories were read-permitted above.

       NOT the temporary directory, though it reads like part of a
       working area and was granted here for a while. It is not the
       program's working area, it is everyone's: a directory shared with
       every account on the machine, world-writable on the systems that
       have one, holding whatever other jobs have left in it. Granting it
       read and write to input that is not trusted is the widest thing a
       default can do, and nothing needed it. The interpreter's own
       scratch -- the spill, the tracing dump, the temporary file a
       filter may want -- is opened internally and never consults the
       permitted set at all, so the grant only ever served the program;
       and a program with somewhere to write already has the current
       directory. It is also what made the escape closed in the file
       layer reachable, since that wanted a write-permitted directory an
       attacker could leave a symbolic link in, and this was the only one
       in the set that a stranger could reach.

       A program that genuinely wants the temporary directory can be run
       from it, be given it with -o, or be run with --no-sandbox, which
       is the switch for input whose file access is not in question. */
    if (!no_sandbox)
    {
        xpost_path_permit_read(".");
        xpost_path_permit_write(".");
        for (i = 0; i < num_psfiles; i++)
            _xpost_permit_file_dir(psfiles[i], 0);
        if (output_file)
        {
            /* One file where the name settles on one, its directory
               where it does not. A name carrying %d is a name per page,
               and which pages there will be is not known until the
               program has run, so nothing narrower can be granted for
               it. Everything else -- which is most invocations, and
               includes the -o /dev/null that would otherwise hand over
               the whole of /dev -- names the single file it means. */
            if (strstr(output_file, "%d"))
                _xpost_permit_file_dir(output_file, 1);
            else
                xpost_path_permit_write_file(output_file);
        }
        xpost_lockdown(ctx);
    }

    {
        Xpost_Run_Status status;

        /* The programs named are one job on one interpreter, run in
           the order they were given: the second begins where the first
           left off, with what it defined still defined, exactly as the
           two texts in one file would. So the run stops where that file
           would have stopped -- a quit takes the interpreter down, and
           an uncaught error ends the job -- and what follows either is
           not read. */
        if (jobserver)
            status = xpost_run(ctx, XPOST_INPUT_FILEPTR, stdin, 0);
        else if (num_psfiles > 1)
        {
            /* Several programs named are one job, in the order given:
               the second begins where the first left off, with what it
               defined still defined. A named program is a job on its
               own, so what runs is a program that reads each of them in
               turn -- which is what puts them in one job, and what
               makes a quit end the run and an uncaught error end it
               with the rest unread, as they would halfway through a
               single file. */
            char *driver = _xpost_main_driver(psfiles, num_psfiles);

            if (!driver)
            {
                XPOST_LOG_ERR("out of memory");
                _xpost_main_list_free(&psfiles, &num_psfiles);
                xpost_destroy(ctx);
                xpost_quit();
                return EXIT_FAILURE;
            }
            status = xpost_run(ctx, XPOST_INPUT_STRING, driver, 0);
            free(driver);
        }
        else
            status = xpost_run(ctx, XPOST_INPUT_FILENAME,
                               num_psfiles ? psfiles[0] : NULL, 0);
        _xpost_main_list_free(&psfiles, &num_psfiles);
        xpost_destroy(ctx);

        xpost_quit();

        /* a job that ended in an uncaught error is a failed job,
           whatever was flushed or rendered along the way */
        return status == XPOST_RUN_COMPLETE || status == XPOST_RUN_YIELDED
             ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* What the run was asked for has been printed and there is nothing
       further to do, which is an ending like the one below and gives
       back what the reading had taken in the same way. Kept apart from
       it only by what it answers the caller with: this is the run doing
       what it was told. */
  quit_asked:
    _xpost_main_list_free(&defs, &num_defs);
    _xpost_main_list_free(&psfiles, &num_psfiles);
    _xpost_main_list_free(&incs, &num_incs);
    _xpost_main_list_free(&params, &num_params);
    xpost_quit();

    return EXIT_SUCCESS;

  quit_xpost:
    /* An ending reached from anywhere in the option reading gives back
       whatever the reading had taken by then. The lists are given back
       on the way through as well, once what they hold has been passed
       on, and giving back a list already given back costs nothing --
       which is what lets one ending answer for every way of reaching
       it. */
    _xpost_main_list_free(&defs, &num_defs);
    _xpost_main_list_free(&incs, &num_incs);
    _xpost_main_list_free(&params, &num_params);
    _xpost_main_list_free(&psfiles, &num_psfiles);
    xpost_quit();

    return EXIT_FAILURE;
}
