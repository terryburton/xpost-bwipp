/*
 * Xpost View - a small PostScript Level-3 viewer
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_view.c
 * @brief The viewer: puts a rendered page in a window.
 *
 * Platform-independent half; the window itself is opened by the xcb or win32
 * file beside this.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "xpost.h"
#include "xpost_dsc.h"
#include "xpost_view.h"

static Xpost_Context *ctx = NULL;
static Xpost_Dsc _dsc = { 0 };
static Xpost_Dsc *dsc = &_dsc;
static Xpost_Dsc_File *file = NULL;
static int page_num = 0;
static Xpost_View_Window *win = NULL;
static void *buffer = NULL;
/* Where the run has got to, and what it takes to start another: a run
   only ever goes forwards, so reaching an earlier page means running the
   file again from the beginning in a context that has not seen it. */
static int run_page = -1;
static int page_width = 0;
static int page_height = 0;
static Xpost_Output_Message run_msg = XPOST_OUTPUT_MESSAGE_QUIET;

static void
_xpost_view_license(void)
{
    printf("BSD 3-clause\n");
}

static void
_xpost_view_version(const char *progname)
{
    int maj;
    int min;
    int mic;

    xpost_version_get(&maj, &min, &mic);
    printf("%s %d.%d.%d\n", progname, maj, min, mic);
}

static void
_xpost_view_usage(const char *progname)
{
    printf("Usage: %s [options] file.ps\n\n", progname);
    printf("PostScript level 3 interpreter\n\n");
    printf("Options:\n");
    printf("  -q, --quiet            suppress interpreter messages (default)\n");
    printf("  -v, --verbose          do not go quiet into that good night\n");
    printf("  -t, --trace            add additional tracing messages, implies -v\n");
    printf("  -L, --license          show program license\n");
    printf("  -V, --version          show program version\n");
    printf("  -h, --help             show this message\n");
    printf("\n");
}

static int
_xpost_view_options_read(int argc, char *argv[], Xpost_Output_Message *msg, const char **filename)
{
    const char *psfile;
    Xpost_Output_Message output_msg;
    int i;

    psfile = NULL;
    output_msg = XPOST_OUTPUT_MESSAGE_QUIET;

    i = 0;
    while (++i < argc)
    {
        if (*argv[i] == '-')
        {
            if ((!strcmp(argv[i], "-h")) ||
                (!strcmp(argv[i], "--help")))
            {
                _xpost_view_usage(argv[0]);
                return 0;
            }
            else if ((!strcmp(argv[i], "-V")) ||
                     (!strcmp(argv[i], "--version")))
            {
                _xpost_view_version(argv[0]);
                return 0;
            }
            else if ((!strcmp(argv[i], "-L")) ||
                     (!strcmp(argv[i], "--license")))
            {
                _xpost_view_license();
                return 0;
            }
            else if ((!strcmp(argv[i], "-q")) ||
                     (!strcmp(argv[i], "--quiet")))
            {
                output_msg = XPOST_OUTPUT_MESSAGE_QUIET;
            }
            else if ((!strcmp(argv[i], "-v")) ||
                     (!strcmp(argv[i], "--verbose")))
            {
                output_msg = XPOST_OUTPUT_MESSAGE_VERBOSE;
            }
            else if ((!strcmp(argv[i], "-t")) ||
                     (!strcmp(argv[i], "--trace")))
            {
                output_msg = XPOST_OUTPUT_MESSAGE_TRACING;
            }
            else
            {
                printf("unknown option\n");
                _xpost_view_usage(argv[0]);
                return -1;
            }
        }
        else
            psfile = argv[i];
    }

    if (!psfile)
    {
        printf("Postscript file not provided\n");
        _xpost_view_usage(argv[0]);
        return -1;
    }

    *msg = output_msg;
    *filename = psfile;

    return 1;
}

static void
_xpost_view_page_set(void)
{
    const unsigned char *base = xpost_dsc_file_base_get(file);
    size_t len = xpost_dsc_file_length_get(file);

    /* The file runs as one program: a page's bytes alone do not carry
       what the prolog defined, and a showpage may live in the trailer
       rather than the page body. What selects the page is where the run
       is stopped -- the context returns at every showpage -- so the page
       wanted is the one reached after that many returns.

       Every page starts the run over, in a context that has not run the
       file. A resumption carries the page before it: the buffer the
       device paints into is not cleared between one showpage and the
       next, so a page reached by resuming arrives with the earlier page
       still under it. A fresh context is the one way to be given the
       page and nothing else. */
    {
        if (ctx)
            xpost_destroy(ctx);
        ctx = xpost_create("raster:bgra",
                           XPOST_OUTPUT_BUFFEROUT,
                           &buffer,
                           XPOST_SHOWPAGE_RETURN,
                           run_msg,
                           XPOST_USE_SIZE, page_width, page_height);
        if (!ctx)
        {
            fprintf(stderr, "Xpost failed to create interpreter context\n");
            return;
        }
        xpost_run(ctx, XPOST_INPUT_STRING, (void *)base, len);
        run_page = 0;
    }

    /* The buffer a returned page is left in is not cleared when the next
       page begins, so a page reached by resuming would arrive with the
       one before it still under it. The page about to be drawn is given
       a blank sheet. */
    for (run_page = 0; run_page < page_num; run_page++)
    {
        if (buffer)
            memset(buffer, 0xff, (size_t)4 * page_width * page_height);
        xpost_run(ctx, XPOST_INPUT_RESUME, (void *)base, len);
    }

    xpost_view_page_display(win, buffer);
}

void xpost_view_page_change(int i)
{
    int page = page_num + i;

    if (page < 0) page = 0;
    if (page > (dsc->header.pages - 1)) page = dsc->header.pages - 1;

    if (page != page_num)
    {
        page_num = page;
        _xpost_view_page_set();
    }
}

int main(int argc, char *argv[])
{
    const char *psfile;
    Xpost_Dsc_Status status;
    Xpost_Output_Message output_msg;
    int width;
    int height;
    int ret;

    psfile = NULL;
    output_msg = XPOST_OUTPUT_MESSAGE_QUIET;

    ret = _xpost_view_options_read(argc, argv, &output_msg, &psfile);
    if (ret == -1) return EXIT_FAILURE;
    else if (ret == 0) return EXIT_SUCCESS;

    file = xpost_dsc_file_new_from_file(psfile);
    if (!file)
        return EXIT_FAILURE;

    status = xpost_dsc_parse(file, dsc);

    /*
     * status:
     * XPOST_DSC_STATUS_ERROR: DSC, but ps file not conforming to mandatory DSC
     * XPOST_DSC_STATUS_NO_DSC: no error, but no DSC
     * XPOST_DSC_STATUS_SUCCESS: no error and DSC
     */

    /* What the structuring comments are read for is the page count and the
       page size. A file that does not carry them, or carries them in a form
       the parser refuses, is still a program this interpreter runs -- so it
       is shown at the default size rather than declined. Refusing here would
       make the viewer answer for the file's structure rather than for what
       the interpreter draws from it. */
    if (status == XPOST_DSC_STATUS_ERROR || status == XPOST_DSC_STATUS_NO_DSC)
    {
        width = 612;
        height = 792;
    }
    else
    {
        width = dsc->header.bounding_box.urx;
        height = dsc->header.bounding_box.ury;
    }

    if (!xpost_init())
    {
        fprintf(stderr, "Xpost failed to initialize\n");
        goto free_dsc;
    }

    /* What a run is made from, kept because every backward step makes
       another one. */
    page_width = width;
    page_height = height;
    run_msg = output_msg;

    win = xpost_view_win_new(10, 10, width, height);
    if (!win)
    {
        fprintf(stderr, "Can not create window\n");
        goto quit_xpost;
    }

    /* Render the first page. The prolog is run together with the page inside
       _xpost_view_page_set so the prolog's definitions are in scope. */
    _xpost_view_page_set();

    xpost_view_main_loop(win);

    xpost_view_win_del(win);
    xpost_destroy(ctx);
    xpost_quit();
    xpost_dsc_free(dsc);
    xpost_dsc_file_del(file);

    return EXIT_SUCCESS;

  quit_xpost:
    xpost_quit();
  free_dsc:
    xpost_dsc_free(dsc);
    xpost_dsc_file_del(file);

    return EXIT_FAILURE;
}
