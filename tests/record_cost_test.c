/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (C) 2013-2016, Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * (BSD 3-clause; see COPYING)
 */

/* What a record says it cost, against what the process is resident for.
 *
 * xpost_record_bytes is not a report. Two decisions are taken from it --
 * whether to keep a record at all, and whether a page should arrive in
 * bands -- and both are of the form "is the record smaller than the
 * raster it saves holding". A raster's cost is its pixels and cannot be
 * wrong; a record's is a sum, and a sum that has drifted from what the
 * record makes the process resident for turns both decisions over
 * without anything looking wrong. The page still comes out right, which
 * is exactly why nothing else catches it.
 *
 * So the number is weighed here against the process, over the marks that
 * produced it, for every kind of mark a record holds: rectangles and
 * lines, which are the fixed-arity marks; polygons, whose values are as
 * many as the shape has vertices and are the bulk of path-heavy content;
 * glyphs, which are a coverage mask apiece and thousands of placements
 * naming them; images, which are held whole; and screens, which are a
 * threshold cell per change.
 *
 * HOW THE MEASUREMENT IS MADE HONEST
 *
 * A record is built twice per kind. The first build is small and is
 * KEPT: it touches every code and data page the measured build will
 * touch, so what is weighed is the record rather than the first use of
 * the machinery, and it leaves nothing free for the measured build to be
 * handed back -- a build served out of pages the process was already
 * resident for would weigh less than it costs. Every record made here is
 * kept for the same reason, and they are given up together at the end.
 *
 * Before the second weighing the allocator is asked to give back what it
 * is holding free, where it offers a way to ask. Growing a run copies
 * into a new block and gives up the old one, whose pages stay resident
 * until they are handed out again; those pages are the allocator's
 * doing and not the record's, and a measurement that carried them would
 * be measuring fragmentation.
 *
 * WHAT THE TOLERANCE IS, AND WHY IT IS THAT
 *
 * The report is the runs' fill, and what a run was handed is its
 * capacity: a run doubles, so a run just past a doubling has room for
 * almost twice what is in it. None of that room is written into, and
 * whether the process is resident for it is the allocator's business
 * and the machine's -- a block mapped for the run alone costs its
 * touched pages, and one carved out of a heap the process has already
 * grown into costs the whole of what it was carved from. Both are
 * honest readings of the same record, and they differ by up to the
 * whole of the slack.
 *
 * So the bound is the growth policy's own: below twice what the record
 * said. It is a coarse instrument and is not the sharp one -- the
 * differences taken further down weigh each part of a record exactly,
 * and are what would catch a part going uncounted. What this one is for
 * is a report that has drifted from the drawing altogether, which is
 * the failure that turns the two decisions over.
 *
 * The fixed allowance beside it covers the pages the machinery touches
 * once however small the record is: those came to under a quarter of a
 * megabyte. And nothing is allowed the other way: the record does not
 * claim more than it made resident.
 *
 * WHAT PLAYING A RECORD BACK COSTS, WHICH IS NOT WHAT THE RECORD COSTS
 *
 * A replay hands each mark to a device method, and a method may be a
 * procedure, so a mark that cannot be handed to a compiled one reaches
 * it as interpreter objects built for the call -- a polygon as an array
 * of its vertices, one two-element array apiece. That is not in
 * xpost_record_bytes and cannot be: it is the target's and the
 * interpreter's, it follows the drawing rather than the page, and it is
 * paid once for every band a shape reaches rather than once for the
 * page.
 *
 * Which is why it is weighed here beside the record. The two are the
 * whole of what holding a page's marks instead of its pixels costs, the
 * second is the larger of them on a page of paths, and it is the one no
 * page and no comparison of pages can see: a polygon rebuilt for every
 * band paints exactly the pixels a polygon handed over once paints.
 *
 * It is weighed in the interpreter's own virtual memory rather than
 * against the process, because what is being counted is what a replay
 * builds and not what it is left resident for. Automatic collection is
 * turned off for the weighing (PLRM 8.2 vmreclaim): with it on, what
 * vmstatus answers afterwards is whatever the collector had not got to
 * yet, which is a reading of the collector.
 *
 * Two claims, and each has its own control:
 *
 *   A page of polygons costs no more to play than the same number of
 *   rectangles, rectangles being the mark whose operands are numbers and
 *   which therefore builds nothing. The control is that the rectangles
 *   are weighed the same way in the same run.
 *
 *   And it does not grow with the number of bands the page is held in.
 *   The control there is that the band count is shown to have changed:
 *   how many marks were played is read off the record, and a page held
 *   in more bands plays more of them. An instrument that had stopped
 *   dividing the page would report a flat cost for the best of reasons.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __GLIBC__
# include <malloc.h>
#endif

#include "xpost.h"
#include "xpost_object.h"
#include "xpost_record.h"

#include "xpost_test.h"

/* What the process is resident for, in bytes. The build defines this
   test only where the figure can be read (meson.build), so there is no
   answer here standing in for one. */
static size_t resident(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long total = 0, res = 0;

    if (!f)
        return 0;
    if (fscanf(f, "%lu %lu", &total, &res) != 2)
        res = 0;
    fclose(f);
    return (size_t)res * 4096u;
}

/* Ask the allocator to give back the pages it is holding free, so that
   what is weighed after is what the record holds and not what growing it
   left behind. An allocator that offers no way to ask is left alone; the
   tolerance says what that costs. */
static void give_back(void)
{
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

/* the kinds of content weighed, and how much of each */
typedef enum
{
    K_RECT, K_LINE, K_POLY, K_GLYPH, K_IMAGE, K_SCREEN, K_COUNT
} Kind;

static const char *_name(Kind k)
{
    switch (k)
    {
        case K_RECT:   return "rectangles";
        case K_LINE:   return "lines";
        case K_POLY:   return "polygons";
        case K_GLYPH:  return "glyphs";
        case K_IMAGE:  return "images";
        case K_SCREEN: return "screens";
        case K_COUNT:  break;
    }
    return "?";
}

/* A record of @p n polygons of @p nv vertices apiece. Its own function
   because the two counts are asked for separately below: a polygon's
   values follow its vertices and its entry follows the mark, and the
   parts are told apart by moving one at a time. */
static Xpost_Record *_build_poly(int n, int nv)
{
    Xpost_Record *rec = xpost_record_new(1);
    real colour[1];
    real *ops;
    int i, j;

    if (!rec)
        return NULL;
    colour[0] = 0.5;
    ops = malloc((size_t)(1 + 2 * nv) * sizeof *ops);
    if (!ops)
    {
        xpost_record_free(rec);
        return NULL;
    }
    for (i = 0; i < n; i++)
    {
        ops[0] = (real)nv;
        for (j = 0; j < nv; j++)
        {
            ops[1 + 2 * j] = (real)(j % 600);
            ops[2 + 2 * j] = (real)((j + i) % 780);
        }
        if (!xpost_record_mark(rec, XPOST_RECORD_FILLPOLY, colour,
                               ops, 1 + 2 * nv))
        {
            free(ops);
            xpost_record_free(rec);
            return NULL;
        }
    }
    free(ops);
    return rec;
}

/* A record of @p nplace placements over @p nmask distinct masks, which
   is the shape a page of text has and the shape a glyph entry exists
   for. */
static Xpost_Record *_build_glyph(int nplace, int nmask)
{
    Xpost_Record *rec = xpost_record_new(1);
    unsigned char cov[14 * 20];
    real colour[1];
    size_t *at;
    int i, j;

    if (!rec)
        return NULL;
    colour[0] = 0.5;
    at = malloc((size_t)nmask * sizeof *at);
    if (!at)
    {
        xpost_record_free(rec);
        return NULL;
    }
    for (i = 0; i < nmask; i++)
    {
        for (j = 0; j < (int)sizeof cov; j++)
            cov[j] = (unsigned char)((i * 7 + j * 13) & 0xff);
        if (!xpost_record_mask(rec, cov, 14, 20, &at[i]))
            goto no;
    }
    for (i = 0; i < nplace; i++)
        if (!xpost_record_glyph(rec, colour, at[i % nmask],
                                (real)(i % 600), (real)(i % 780)))
            goto no;
    free(at);
    return rec;

  no:
    free(at);
    xpost_record_free(rec);
    return NULL;
}

/* One record of @p n of the kind. Answers NULL where a mark could not
   be held, which is not what is being weighed and is reported by the
   caller. */
static Xpost_Record *_build(Kind kind, int n)
{
    Xpost_Record *rec = xpost_record_new(1);
    real colour[1];
    int i, j;

    if (!rec)
        return NULL;
    colour[0] = 0.5;

    switch (kind)
    {
        case K_RECT:
        case K_LINE:
        {
            Xpost_Record_Kind k = kind == K_RECT ? XPOST_RECORD_FILLRECT
                                                 : XPOST_RECORD_DRAWLINE;

            for (i = 0; i < n; i++)
            {
                real ops[4];

                ops[0] = (real)(i % 600);
                ops[1] = (real)(i % 780);
                ops[2] = (real)(i % 17) + 1;
                ops[3] = (real)(i % 23) + 1;
                if (!xpost_record_mark(rec, k, colour, ops, 4))
                    goto short_of_one;
            }
            break;
        }
        case K_POLY:
            /* four hundred vertices apiece, which is the order a stroked
               curve flattens to and is what makes a polygon's values the
               bulk of such a page */
            xpost_record_free(rec);
            return _build_poly(n, 400);
        case K_GLYPH:
            /* a mask per hundred placements, which is the order a page
               of text runs to */
            xpost_record_free(rec);
            return _build_glyph(n, n / 100 + 1);
        case K_IMAGE:
        {
            int w = 512, h = 512;
            unsigned char *row = malloc((size_t)w);
            const unsigned char **run = malloc((size_t)h * sizeof *run);
            unsigned char lut[256];
            Xpost_Record_Image img;

            if (!row || !run)
            {
                free(row);
                free((void *)run);
                goto short_of_one;
            }
            for (j = 0; j < w; j++)
                row[j] = (unsigned char)(j & 0xff);
            for (j = 0; j < h; j++)
                run[j] = row;
            for (j = 0; j < 256; j++)
                lut[j] = (unsigned char)j;
            for (i = 0; i < n; i++)
            {
                memset(&img, 0, sizeof img);
                img.width = w;
                img.height = h;
                img.ncomp = 1;
                img.nat = 1;
                img.xscale = 1.0;
                img.yscale = 1.0;
                img.yoff = (real)(i * 10);
                img.cx1 = 10000.0;
                img.cy1 = 10000.0;
                img.lut = lut;
                if (!xpost_record_image(rec, &img, run, h))
                {
                    free(row);
                    free((void *)run);
                    goto short_of_one;
                }
            }
            free(row);
            free((void *)run);
            break;
        }
        case K_SCREEN:
        {
            unsigned char cell[16 * 16];

            for (i = 0; i < n; i++)
            {
                for (j = 0; j < (int)sizeof cell; j++)
                    cell[j] = (unsigned char)((i + j) & 0xff);
                if (!xpost_record_screen(rec, 16, 16, cell))
                    goto short_of_one;
            }
            break;
        }
        case K_COUNT:
            break;
    }
    return rec;

  short_of_one:
    xpost_record_free(rec);
    return NULL;
}

/* The margin allowed above what the record says, and the fixed
   allowance beside it. Both are stated in the file's opening: the
   margin from the runs' growth policy, the allowance from what the
   machinery touches once. */
#define OVER_NUM  1
#define OVER_DEN  1
#define ALLOW     ((size_t)512 * 1024)

/*
 * WHAT ONE EMISSION BUILDS, read out of a run that makes one.
 */

/* the page the marks below are made on, and the rows a row of it costs
   the grayscale raster they are played into: one byte to the pixel, so
   the band budget that holds n rows is n times the width */
#define PAGE_W  600
#define PAGE_H  800

/* Enough polygons, of enough vertices, that what building them would
   cost is far above what one emission costs when nothing is built --
   forty of two hundred vertices would come to some four hundred
   kilobytes a band against the twenty at most that an emission costs on
   its own -- and few enough
   that a run rebuilding them once per band still has the memory and the
   entity numbers to do it. A weighing that ran out of either would
   report the refusal, which says something is wrong without saying what
   it cost. */
#define MARKS     40
#define VERTICES  200

/* What an emission builds when it builds nothing for a mark: the walk's
   own working objects, which are a handful and are the same handful for
   either kind of page. Twenty-one kilobytes at the widest an object is
   built here, so this is three times the reading it allows for and a
   small fraction of what one page of vertices would come to. */
#define BUILT_ALLOW ((size_t)64 * 1024)

/* Each polygon reaches from the top of the page to the bottom, so every
   band holds part of every one of them and a page in n bands plays n
   times the marks. A shape reaching one band would be played once
   however finely the page was divided, and the second claim would hold
   for a reason that had nothing to do with what a replay builds. */
static const char *_program =
    "/poly [ 0 1 %d { /j exch def\n"
    "    [ j 2 mod 500 mul 40 add  j %d mul %d div 5 add ] } for ] def\n"
    "/drawpoly { 1 1 %d { pop\n"
    "    0.5 poly DEVICE dup /FillPoly get exec } for } bind def\n"
    "/drawrect { 1 1 %d { 12 mul 5 add\n"
    "    0.5 exch 20 exch 500 3 DEVICE dup /FillRect get exec } for } bind def\n"
    "/played { DEVICE 1183615869 internaldict /.recordplayed get exec } bind def\n"
    "DEVICE /.bandbytes %d put\n"
    /* the collection that would answer for the collector rather than
       for the replay, and the page that touches everything the weighed
       one touches */
    "-1 vmreclaim\n"
    "%s showpage\n"
    /* the page weighed: drawn, then put out with the two readings
       taken either side of the emission that plays it back */
    "%s\n"
    "vmstatus pop /u0 exch def pop  played /p0 exch def\n"
    "showpage\n"
    "vmstatus pop /u1 exch def pop  played /p1 exch def\n"
    "(BUILT ) print u1 u0 sub 20 string cvs print\n"
    "( ) print p1 p0 sub 20 string cvs print (\\n) print flush\n";

/* the standard output of one run, kept so that the figures the program
   above reports can be read back */
static char out_buf[512];
static size_t out_len;

static size_t _out_sink(void *user, const char *buf, size_t len)
{
    (void)user;
    if (out_len + len < sizeof out_buf)
    {
        memcpy(out_buf + out_len, buf, len);
        out_len += len;
    }
    return len;
}

/* What one emission of a page of @p draw builds in virtual memory, over
   a page held in bands of @p bandrows rows, and how many marks that
   emission played. Answers whether the run made it that far. */
static int _emission(const char *draw, int bandrows,
                     unsigned long *built, unsigned long *played)
{
    Xpost_Context *ctx;
    char prog[2048];
    int ok;

    *built = *played = 0;
    ctx = xpost_create("pgm:band", XPOST_OUTPUT_FILENAME, "/dev/null",
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, PAGE_W, PAGE_H);
    if (!ctx)
        return 0;
    xpost_job_snapshots_set(ctx, 0);
    xpost_stdout_handler_set(ctx, _out_sink, NULL);
    snprintf(prog, sizeof prog, _program, VERTICES - 1, PAGE_H - 10,
             VERTICES - 1, MARKS, MARKS, bandrows * PAGE_W, draw, draw);
    out_len = 0;
    ok = xpost_run(ctx, XPOST_INPUT_STRING, prog, 0) == XPOST_RUN_COMPLETE;
    out_buf[out_len] = '\0';
    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
    if (!ok)
        return 0;
    return sscanf(out_buf, "BUILT %lu %lu", built, played) == 2;
}

/* Weigh the two kinds of page against each other, and each against
   itself over two band counts. Reports its own failures. */
static void _weigh_replay(void)
{
    /* the page whole, and the page in bands of a hundred rows, which is
       eight of them */
    static const int rows[2] = { PAGE_H, 100 };
    unsigned long poly[2], rect[2], polyplayed[2], rectplayed[2];
    int i;

    for (i = 0; i < 2; i++)
    {
        if (!_emission("drawpoly", rows[i], &poly[i], &polyplayed[i]))
        {
            report_failure("a page of polygons in bands of %d rows is put"
                           " out", rows[i]);
            return;
        }
        if (!_emission("drawrect", rows[i], &rect[i], &rectplayed[i]))
        {
            report_failure("a page of rectangles in bands of %d rows is put"
                           " out", rows[i]);
            return;
        }
    }

    /* the instruments first: a page that played no mark, or that played
       the same marks however it was divided, would make everything
       below pass without weighing anything */
    check(polyplayed[0] >= MARKS && rectplayed[0] >= MARKS,
          "an emission plays the marks the page was given");
    check(polyplayed[1] > polyplayed[0],
          "a page held in bands plays more marks than the same page held"
          " whole, each band playing the shapes that reach it");

    /* A polygon carries its vertices and a rectangle carries four
       numbers, and neither is built into anything: the same number of
       either costs the same emission. */
    if (poly[0] > rect[0] + BUILT_ALLOW)
        report_failure("playing back a page of %d polygons built %lu bytes"
                       " where the same number of rectangles built %lu:"
                       " the marks of a path-heavy page are reaching the"
                       " device as objects made for the call",
                       MARKS, poly[0], rect[0]);

    /* And the cost does not follow the band count. A polygon rebuilt for
       each band it reaches would multiply here by the bands, which is
       the number a small band budget exists to raise. */
    if (poly[1] > poly[0] + BUILT_ALLOW)
        report_failure("playing a page back in bands of %d rows built %lu"
                       " bytes where playing the same page whole built"
                       " %lu: what a replay builds is following the band"
                       " count, so it is heaviest where the budget buys"
                       " the most bands",
                       rows[1], poly[1], poly[0]);
}

int main(void)
{
    /* how much of each kind, chosen so that each record comes to several
       megabytes: the fixed allowance is then a few percent of what is
       weighed rather than the whole of it */
    static const int howmany[K_COUNT] = {
        150000,   /* rectangles */
        150000,   /* lines */
        2000,     /* polygons, four hundred vertices apiece */
        200000,   /* glyph placements over two thousand masks */
        24,       /* images of a quarter million samples */
        20000     /* screens */
    };
    Xpost_Record *kept[K_COUNT];
    Xpost_Record *warm[K_COUNT];
    Xpost_Record *page;
    size_t before, after;
    int k, i;

    for (k = 0; k < K_COUNT; k++)
        kept[k] = warm[k] = NULL;

    /* A record is something before it holds anything: it is a structure
       and the runs it grows, and a report answering nothing for it would
       be answering about a record that does not exist. */
    page = xpost_record_new(1);
    check(page != NULL, "a record is made");
    if (page)
        check(xpost_record_bytes(page) > 0,
              "a record with no marks in it still costs what it is");
    xpost_record_free(page);

    for (k = 0; k < K_COUNT; k++)
    {
        size_t said, got, rss0, rss1;

        /* the same build, small, kept: every page the measured build
           touches has been touched, and nothing it could be handed back
           is free */
        warm[k] = _build((Kind)k, 4);
        if (!warm[k])
        {
            report_failure("a small record of %s is held", _name((Kind)k));
            continue;
        }

        give_back();
        rss0 = resident();
        kept[k] = _build((Kind)k, howmany[k]);
        if (!kept[k])
        {
            report_failure("a record of %s is held", _name((Kind)k));
            continue;
        }
        said = xpost_record_bytes(kept[k]);
        give_back();
        rss1 = resident();
        got = rss1 > rss0 ? rss1 - rss0 : 0;

        /* The report is not above what the record made the process
           resident for. A report above it sends a caller comparing a
           record against a raster to the raster, where the record was
           the cheaper of the two. */
        if (said > got + ALLOW)
            report_failure("a record of %s says it cost %lu bytes where"
                           " the process came to %lu: the report is above"
                           " what the record made resident",
                           _name((Kind)k), (unsigned long)said,
                           (unsigned long)got);

        /* and not below it by more than the margin. That way round is
           the worse of the two: it is what keeps a record costing more
           than the page it exists to escape. */
        if (got > said + said / OVER_DEN * OVER_NUM + ALLOW)
            report_failure("a record of %s says it cost %lu bytes where"
                           " the process came to %lu: the report is under"
                           " what the record made resident by more than"
                           " the margin",
                           _name((Kind)k), (unsigned long)said,
                           (unsigned long)got);
    }

    /* WHAT EACH PART OF A RECORD COSTS, BY DIFFERENCE
     *
     * The weighing above is a proportion, and a proportion cannot see a
     * part go missing that is a few percent of the whole -- the coverage
     * a page of text holds is a twentieth of what that page costs, and a
     * report that stopped counting it would still land inside any margin
     * wide enough to allow for an allocator. So each part is also asked
     * for on its own, by building two records differing in that part
     * alone and holding the difference to what the part is known to
     * cost. These are exact where the difference is exact, and are
     * lower bounds where the record's own structures are in it, those
     * being none of a caller's business and not named here. */
    {
        Xpost_Record *a, *b;
        size_t got;

        /* a polygon's values are as many as it has vertices, and are
           what path-heavy content is nearly all of. Two records of the
           same marks over twice the vertices differ by those values and
           by nothing else. */
        a = _build_poly(500, 200);
        b = _build_poly(500, 400);
        check(a != NULL && b != NULL, "two records of polygons are held");
        if (a && b)
        {
            got = xpost_record_bytes(b) - xpost_record_bytes(a);
            check(got == (size_t)500 * 400 * sizeof(real),
                  "a polygon's vertices cost what they are: twice as many"
                  " of them costs the coordinates of the difference");
        }
        xpost_record_free(a);
        xpost_record_free(b);

        /* a mark costs something beside its values. Two records holding
           nearly the same values -- the same vertices over twice the
           polygons -- differ by the entries describing them. */
        a = _build_poly(100, 400);
        b = _build_poly(200, 200);
        check(a != NULL && b != NULL, "two records of like polygons are held");
        if (a && b)
        {
            check(xpost_record_bytes(b)
                      > xpost_record_bytes(a) + (size_t)100 * 16,
                  "a mark costs its own entry beside its values, so twice"
                  " the marks over the same vertices costs more");
        }
        xpost_record_free(a);
        xpost_record_free(b);

        /* the coverage a mask holds, which is what a page of text is
           mostly made of and is held once however often it is placed */
        a = _build_glyph(1000, 100);
        b = _build_glyph(1000, 200);
        check(a != NULL && b != NULL, "two records of glyphs are held");
        if (a && b)
        {
            got = xpost_record_bytes(b) - xpost_record_bytes(a);
            check(got >= xpost_record_mask_bytes(b)
                       - xpost_record_mask_bytes(a),
                  "the coverage a mask holds is counted, so twice the"
                  " distinct masks costs at least the coverage of the"
                  " difference");
        }
        xpost_record_free(a);
        xpost_record_free(b);

        /* an image's samples, which are held whole */
        a = _build((Kind)K_IMAGE, 4);
        b = _build((Kind)K_IMAGE, 8);
        check(a != NULL && b != NULL, "two records of images are held");
        if (a && b)
        {
            got = xpost_record_bytes(b) - xpost_record_bytes(a);
            check(got >= (size_t)4 * 512 * 512,
                  "an image's samples are counted, so twice the images"
                  " costs at least the samples of the difference");
        }
        xpost_record_free(a);
        xpost_record_free(b);

        /* and a screen's threshold cell */
        a = _build((Kind)K_SCREEN, 100);
        b = _build((Kind)K_SCREEN, 200);
        check(a != NULL && b != NULL, "two records of screens are held");
        if (a && b)
        {
            got = xpost_record_bytes(b) - xpost_record_bytes(a);
            check(got >= (size_t)100 * 16 * 16,
                  "a screen's cell is counted, so twice the screens costs"
                  " at least the cells of the difference");
        }
        xpost_record_free(a);
        xpost_record_free(b);
    }

    /* A page boundary empties the runs and keeps their storage, to be
       filled again by the page after. What the record is resident for
       does not fall there, so what it says it cost does not either: a
       record between pages costs the largest page the job has drawn. */
    page = _build(K_RECT, 40000);
    check(page != NULL, "a page of rectangles is held");
    if (page)
    {
        real colour[1];

        colour[0] = 0.5;
        before = xpost_record_bytes(page);
        xpost_record_clear(page);
        after = xpost_record_bytes(page);
        check(xpost_record_count(page) == 0,
              "a page boundary gives up the marks the page held");
        check(after == before,
              "a page boundary keeps the storage those marks were held"
              " in, so what the record costs does not fall there");

        /* and the page after fills that storage rather than buying more,
           so a job of like pages costs one of them */
        for (i = 0; i < 40000; i++)
        {
            real ops[4];

            ops[0] = (real)(i % 600);
            ops[1] = (real)(i % 780);
            ops[2] = (real)(i % 17) + 1;
            ops[3] = (real)(i % 23) + 1;
            if (!xpost_record_mark(page, XPOST_RECORD_FILLRECT,
                                   colour, ops, 4))
            {
                report_failure("the page after a boundary is held");
                break;
            }
        }
        after = xpost_record_bytes(page);
        check(after == before,
              "a second page of the same size costs what the first did,"
              " the storage being the same storage");
        xpost_record_free(page);
    }

    for (k = 0; k < K_COUNT; k++)
    {
        xpost_record_free(kept[k]);
        xpost_record_free(warm[k]);
    }

    /* and the other half of what holding a page's marks costs: what
       playing them back builds, which is the half no page can show */
    if (!xpost_init())
        report_failure("the interpreter a record is played back through"
                       " starts");
    else
    {
        _weigh_replay();
        xpost_quit();
    }

    return verdict();
}
