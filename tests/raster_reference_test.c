/*
 * A buffer device's page, held to a page that can be looked at.
 *
 * The devices that hand their raster to an embedding program -- bgr and
 * the raster family -- are outside the golden-byte comparison, and for
 * a stated reason: that comparison reads emitted files and these emit
 * none. What they had instead was a check of the arrangement of a
 * pixel, over a page of three painted rows.
 *
 * That is enough to catch a device storing blue where red goes. It is
 * not enough to hold a change to the machinery around the pixels -- the
 * creation, the page's extent, the clearing, the handoff -- which is
 * where these two devices repeat each other most and where a
 * consolidation would touch. Such a change moves whole rows or shifts a
 * page by one, and three rows of flat colour can absorb that.
 *
 * So the page here has geometry in it, and it is held to the SAME page
 * rendered by ppm. ppm is a fair witness for three reasons: its bytes
 * are already in the golden manifest, so they are held to something
 * themselves; it writes a file, so it is read by a different route
 * altogether; and it is not one of the devices a consolidation of these
 * two would touch, so it cannot move with them.
 *
 * That last one is the whole point. Holding bgr to raster would prove
 * nothing about a change that puts them both on shared code: they would
 * move together and agree all the way down.
 *
 * WHAT THIS DOES NOT REACH, for the same reason turned around. The
 * rectangle fill is compiled once and every raster device uses it, so a
 * change there moves the reference too and this cannot see it -- the
 * golden manifest is what holds that. What is device-specific, and is
 * what this holds, is the arrangement of a pixel, the geometry of the
 * buffer, the ground it starts from, and the handoff at the end.
 *
 * The page therefore paints through all three store methods rather than
 * through rectangles alone. Written with rectangles first, it passed
 * while the pixel store was mirrored and while its channels were
 * swapped, because a page of rectangles never reaches PutPix at all.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "xpost.h"
#include "xpost_test.h"

/* Wide enough that a row is not a handful of pixels, small enough that
   every byte of every page is compared. */
#define PAGE_W 24
#define PAGE_H 18

/* The page is compared against ppm's, so it has to be a page ppm and a
   buffer device agree about the meaning of: flat fills of stated
   colours, at stated places, with edges on whole pixels. A blend or a
   shading would bring a device's own arithmetic into it and this would
   be measuring that instead.

   What it does have is boundaries -- eight of them across the page and
   six down it -- so a row moved, a page shifted by one, or a run
   written past its end shows up as a colour in the wrong place rather
   than being absorbed by flat ground. */
static const char paint_program[] =
    "/dev DEVICE def\n"
    /* the ground, laid through the device rather than assumed */
    "1 1 1  0 0 23 17 dev dup /FillRect get exec\n"
    /* blocks, through the rectangle fill: the far corner of a fill is
       the pixel it names rather than the one past it */
    "0.75 0.25 0.125  1 1 7 5 dev dup /FillRect get exec\n"
    "0.125 0.625 0.875  9 1 15 5 dev dup /FillRect get exec\n"
    "0.5 0.5 0.5  17 1 22 5 dev dup /FillRect get exec\n"
    /* a row stored a pixel at a time, in two colours so that a mirrored
       or shifted row cannot land on itself */
    "0 1 23 { /px exch def\n"
    "  px 3 mod 0 eq { 0.25 0.875 0.375 }{ 0.875 0.375 0.25 } ifelse\n"
    "  px 8 dev dup /PutPix get exec } for\n"
    /* a row through the coverage-weighted store at full coverage */
    "0 1 23 { /px exch def\n"
    "  0 0 0 255 px 10 dev dup /BlendPix get exec } for\n"
    /* and blocks below, so the page is asymmetric top to bottom too */
    "0.25 0.875 0.375  1 12 10 16 dev dup /FillRect get exec\n"
    "0.875 0.375 0.25  13 12 22 16 dev dup /FillRect get exec\n"
    "showpage\n";

/* The page ppm wrote, as width * height pixels of three bytes.

   The header is three tokens after the magic and a comment may sit
   among them, which is not a detail to guess at: a reader that took the
   bytes at a fixed offset would read the tail of the header as picture
   and compare rubbish against rubbish. */
static unsigned char *
read_ppm(const char *path, int *w, int *h)
{
    FILE *f;
    unsigned char *pix;
    char magic[3];
    int tok[3];
    int i, c;
    size_t want;

    f = fopen(path, "rb");
    if (!f)
    {
        report_failure("the reference page was not written to %s", path);
        return NULL;
    }
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0)
    {
        report_failure("the reference page is not a P6 raster");
        fclose(f);
        return NULL;
    }
    for (i = 0; i < 3; i++)
    {
        for (;;)
        {
            c = fgetc(f);
            if (c == '#')
            {
                while (c != '\n' && c != EOF)
                    c = fgetc(f);
                continue;
            }
            if (c == EOF)
            {
                report_failure("the reference page's header ends early");
                fclose(f);
                return NULL;
            }
            if (c != ' ' && c != '\n' && c != '\t' && c != '\r')
                break;
        }
        ungetc(c, f);
        if (fscanf(f, "%d", &tok[i]) != 1)
        {
            report_failure("the reference page's header does not read");
            fclose(f);
            return NULL;
        }
    }
    (void)fgetc(f);             /* the single byte after the maximum */
    *w = tok[0];
    *h = tok[1];
    if (*w <= 0 || *h <= 0)
    {
        report_failure("the reference page is %dx%d", *w, *h);
        fclose(f);
        return NULL;
    }
    want = (size_t)*w * (size_t)*h * 3;
    pix = malloc(want);
    if (!pix)
    {
        report_failure("no memory for the reference page");
        fclose(f);
        return NULL;
    }
    if (fread(pix, 1, want, f) != want)
    {
        report_failure("the reference page is shorter than it says");
        free(pix);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return pix;
}

/* Run the painting program on a device that hands its page back. */
static unsigned char *
render_to_buffer(const char *device)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;
    unsigned char *buf = NULL;

    ctx = xpost_create(device, XPOST_OUTPUT_BUFFEROUT, &buf,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, PAGE_W, PAGE_H);
    if (!ctx)
    {
        report_failure("%s: no context", device);
        return NULL;
    }
    xpost_job_snapshots_set(ctx, 0);
    st = xpost_run(ctx, XPOST_INPUT_STRING, paint_program, 0);
    if (st != XPOST_RUN_COMPLETE)
    {
        report_failure("%s: the painting run did not complete", device);
        xpost_destroy(ctx);
        xpost_output_buffer_release(&buf);
        return NULL;
    }
    xpost_destroy(ctx);
    if (!buf)
    {
        report_failure("%s: the run handed back no buffer", device);
        return NULL;
    }
    return buf;
}

/* Run it on ppm, which writes the page to a file. */
static int
render_to_file(const char *path)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;

    ctx = xpost_create("ppm", XPOST_OUTPUT_FILENAME, path,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, PAGE_W, PAGE_H);
    if (!ctx)
    {
        report_failure("ppm: no context for the reference page");
        return 0;
    }
    xpost_job_snapshots_set(ctx, 0);
    st = xpost_run(ctx, XPOST_INPUT_STRING, paint_program, 0);
    xpost_destroy(ctx);
    if (st != XPOST_RUN_COMPLETE)
    {
        report_failure("ppm: the reference run did not complete");
        return 0;
    }
    return 1;
}

/* One device's buffer against the reference, a pixel at a time.

   order says where a pixel's red, green and blue bytes sit in this
   device's arrangement, which is the promise its name makes. */
static void
holds(const char *device, const unsigned char *buf, int bpp,
      const int order[3], const unsigned char *ref, int w, int h)
{
    int x, y, k;
    int wrong = 0;

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            const unsigned char *p = buf + ((size_t)y * w + x) * bpp;
            const unsigned char *r = ref + ((size_t)y * w + x) * 3;

            for (k = 0; k < 3; k++)
            {
                if (p[order[k]] == r[k])
                    continue;
                if (wrong++ == 0)
                    report_failure("%s: at %d,%d the page reads %d %d %d and"
                                   " the reference reads %d %d %d",
                                   device, x, y,
                                   p[order[0]], p[order[1]], p[order[2]],
                                   r[0], r[1], r[2]);
                break;
            }
        }
    }
    if (wrong)
        report_failure("%s: %d of %d pixels differ from the reference page",
                       device, wrong, w * h);
}

/* How many colours the page actually holds.

   Two pages of one flat colour agree for a reason that says nothing
   about either device, so the reference is asked how much it varies
   before anything is concluded from it agreeing. */
static int
distinct_colours(const unsigned char *ref, int w, int h)
{
    unsigned long seen[64];
    int nseen = 0;
    int i, j;

    for (i = 0; i < w * h; i++)
    {
        unsigned long c = ((unsigned long)ref[i * 3] << 16)
                        | ((unsigned long)ref[i * 3 + 1] << 8)
                        | (unsigned long)ref[i * 3 + 2];

        for (j = 0; j < nseen; j++)
            if (seen[j] == c)
                break;
        if (j == nseen)
        {
            if (nseen == (int)(sizeof seen / sizeof seen[0]))
                return nseen;
            seen[nseen++] = c;
        }
    }
    return nseen;
}

int
main(void)
{
    static const int rgb[3] = { 0, 1, 2 };
    static const int bgr[3] = { 2, 1, 0 };
    const char *refpath = "raster_reference_test.ppm";
    unsigned char *ref, *buf;
    int w = 0, h = 0, ncol;

    if (!xpost_init())
    {
        report_failure("the library did not initialise");
        return verdict();
    }

    if (!render_to_file(refpath))
    {
        xpost_quit();
        return verdict();
    }
    ref = read_ppm(refpath, &w, &h);
    if (!ref)
    {
        xpost_quit();
        return verdict();
    }

    if (w != PAGE_W || h != PAGE_H)
    {
        report_failure("the reference page is %dx%d and the run asked for"
                       " %dx%d", w, h, PAGE_W, PAGE_H);
        free(ref);
        xpost_quit();
        return verdict();
    }

    ncol = distinct_colours(ref, w, h);
    if (ncol < 5)
    {
        report_failure("the reference page holds %d colours; a page this"
                       " flat agrees with anything and proves nothing",
                       ncol);
        free(ref);
        xpost_quit();
        return verdict();
    }

    buf = render_to_buffer("bgr");
    if (buf)
    {
        holds("bgr", buf, 3, bgr, ref, w, h);
        xpost_output_buffer_release(&buf);
    }

    buf = render_to_buffer("raster:rgb");
    if (buf)
    {
        holds("raster:rgb", buf, 3, rgb, ref, w, h);
        xpost_output_buffer_release(&buf);
    }

    buf = render_to_buffer("raster:bgra");
    if (buf)
    {
        holds("raster:bgra", buf, 4, bgr, ref, w, h);
        xpost_output_buffer_release(&buf);
    }

    free(ref);
    remove(refpath);
    xpost_quit();

    printf("%dx%d, %d colours, three buffer devices held to ppm\n",
           w, h, ncol);
    return verdict();
}
