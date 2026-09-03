/*
 * Xpost - a Level-2 Postscript interpreter
 * Copyright (C) 2013-2016, Michael Joshua Ryan
 * All rights reserved.
 * (BSD 3-clause; see COPYING)
 */

/* Where a record puts what it need not hold, and what happens when the
 * place it put it stops working.
 *
 * The first half is that a spilled record answers exactly what the same
 * record answers in memory. Everything a caller can ask a record --
 * which marks reach a run of rows, in what order, with what values,
 * which mark had the last word, what the drawing reaches, what a mask
 * and a screen and a picture hold -- is asked of one record twice, once
 * before it spills and once after, and the two answers are compared. A
 * spill that changed any of them would change the page, and a page is
 * the one thing the whole mechanism is not allowed to change.
 *
 * The second half is the failure paths, and they are here because a
 * failure path with no test is a failure path that does not work. Each
 * has to end in a named error or in a step that cannot fail, and never
 * in a page that stops where the trouble started -- a page missing
 * something looks like a page. The three that can be arranged from
 * inside a process are arranged:
 *
 *   the file fills          a limit on how large a file this process may
 *                           write, which the writes then run into
 *   the file is shortened   the record's own file cut under it, which is
 *                           what a truncation by a fault below would do
 *   the directory closes    the scratch directory made unwritable after
 *                           the file was opened, which must not reach a
 *                           descriptor whose name is already gone
 *
 * The fourth -- no scratch space at all when the device is made -- is
 * about a device and a run rather than about a record, and is held by
 * tests/run-record-spill-test.sh.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
# include <sys/resource.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <signal.h>
# include <unistd.h>
#endif

#include "xpost_error.h"
#include "xpost_object.h"
#include "xpost_record.h"
#include "xpost_compat.h"
#include "xpost_spill.h"

#include "xpost_test.h"

/* what a replay saw, in the order it saw it */
typedef struct
{
    int n;
    Xpost_Record_Kind kind[64];
    real colour[64];
    real op0[64];
    int nops[64];
} Seen;

static int _see(void *data, Xpost_Record_Kind kind, const real *colour,
                const real *ops, int nops)
{
    Seen *s = data;

    if (s->n >= 64)
        return 1;
    s->kind[s->n] = kind;
    s->colour[s->n] = colour[0];
    s->op0[s->n] = nops > 0 ? ops[0] : (real)0;
    s->nops[s->n] = nops;
    s->n++;
    return 0;
}

static int _same(const Seen *a, const Seen *b)
{
    int i;

    if (a->n != b->n)
        return 0;
    for (i = 0; i < a->n; i++)
        if (a->kind[i] != b->kind[i] || a->colour[i] != b->colour[i]
            || a->op0[i] != b->op0[i] || a->nops[i] != b->nops[i])
            return 0;
    return 1;
}

/* Put a page of every kind an entry can be into @p rec: the five marking
   calls, a screen, a picture, a glyph, and a placement of a drawing.
   What is drawn matters only in that every kind is here and the marks
   land on different rows, so that a run of rows chooses between them. */
static Xpost_Record *_fill(Xpost_Record *rec)
{
    unsigned char cell[4] = { 0, 128, 64, 192 };
    unsigned char cov[6] = { 255, 128, 0, 0, 200, 255 };
    unsigned char row[8];
    const unsigned char *rows[4];
    Xpost_Record_Image img;
    Xpost_Record *sub;
    real colour[1];
    real ops[16];
    size_t at = 0;
    int i;

    memset(row, 0x5a, sizeof row);
    for (i = 0; i < 4; i++)
        rows[i] = row;

    colour[0] = (real)0.25;
    (void)xpost_record_screen(rec, 2, 2, cell);

    ops[0] = (real)3; ops[1] = (real)4;
    (void)xpost_record_mark(rec, XPOST_RECORD_PUTPIX, colour, ops, 2);

    colour[0] = (real)0.5;
    ops[0] = (real)0.75; ops[1] = (real)5; ops[2] = (real)11;
    (void)xpost_record_mark(rec, XPOST_RECORD_BLENDPIX, colour, ops, 3);

    colour[0] = (real)0.125;
    ops[0] = (real)0; ops[1] = (real)20;
    ops[2] = (real)9; ops[3] = (real)26;
    (void)xpost_record_mark(rec, XPOST_RECORD_DRAWLINE, colour, ops, 4);

    colour[0] = (real)1;
    ops[0] = (real)2; ops[1] = (real)30;
    ops[2] = (real)6; ops[3] = (real)8;
    (void)xpost_record_mark(rec, XPOST_RECORD_FILLRECT, colour, ops, 4);

    colour[0] = (real)0.375;
    ops[0] = (real)3;
    ops[1] = (real)1;  ops[2] = (real)50;
    ops[3] = (real)9;  ops[4] = (real)55;
    ops[5] = (real)5;  ops[6] = (real)60;
    (void)xpost_record_mark(rec, XPOST_RECORD_FILLPOLY, colour, ops, 7);

    memset(&img, 0, sizeof img);
    img.width = 8;
    img.height = 4;
    img.ncomp = 1;
    img.nat = 1;
    img.xscale = (real)1;
    img.yscale = (real)1;
    img.yoff = (real)70;
    img.cx1 = (real)8;
    img.cy1 = (real)80;
    (void)xpost_record_image(rec, &img, rows, 4);

    if (xpost_record_mask(rec, cov, 3, 2, &at))
    {
        colour[0] = (real)0.625;
        (void)xpost_record_glyph(rec, colour, at, (real)4, (real)90);
    }

    sub = xpost_record_new(1);
    if (sub)
    {
        colour[0] = (real)0.875;
        ops[0] = (real)1; ops[1] = (real)0;
        ops[2] = (real)3; ops[3] = (real)2;
        (void)xpost_record_mark(sub, XPOST_RECORD_FILLRECT, colour, ops, 4);
        (void)xpost_record_place(rec, sub, (real)10, (real)100);
        xpost_record_free(sub);
    }
    return rec;
}

/* Everything a caller can ask a record, gathered so that two records can
   be compared by asking each of them. */
typedef struct
{
    int nmark;
    int nmask, nscreen, nimage, nplace;
    real elo, ehi;
    real bx0, by0, bx1, by1;
    int haslast;
    size_t last;
    Seen whole;
    Seen band;
    int maskw, maskh;
    unsigned char maskbyte;
    int cellw, cellh;
    unsigned char cellbyte;
    unsigned char sample;
} Answers;

static void _ask(const Xpost_Record *rec, Answers *a)
{
    const unsigned char *p;
    int w = 0, h = 0;

    memset(a, 0, sizeof *a);
    a->nmark = (int)xpost_record_count(rec);
    a->nmask = (int)xpost_record_mask_count(rec);
    a->nscreen = (int)xpost_record_screen_count(rec);
    a->nimage = (int)xpost_record_image_count(rec);
    a->nplace = (int)xpost_record_place_count(rec);
    (void)xpost_record_extent(rec, &a->elo, &a->ehi);
    (void)xpost_record_box(rec, &a->bx0, &a->by0, &a->bx1, &a->by1);
    a->haslast = xpost_record_last(rec, (real)20, (real)40, &a->last);
    (void)xpost_record_replay(rec, (real)-1000, (real)1000, _see, &a->whole);
    (void)xpost_record_replay(rec, (real)20, (real)40, _see, &a->band);
    p = xpost_record_mask_get(rec, 0, &w, &h);
    a->maskw = w;
    a->maskh = h;
    a->maskbyte = p ? p[4] : 0;
    w = h = 0;
    p = xpost_record_screen_get(rec, 0, &w, &h);
    a->cellw = w;
    a->cellh = h;
    a->cellbyte = p ? p[3] : 0;
    p = xpost_record_image_run(rec, 0, 2);
    a->sample = p ? p[5] : 0;
}

/* the marks a page of this size makes, used where the point is to fill a
   record rather than to look at what is in it. Defined where it is
   called: both callers are in the spill checks, which want a temporary
   directory the portable route does not give them. */
#ifndef _WIN32
static int _bulk(Xpost_Record *rec, int n)
{
    real colour[1];
    real ops[4];
    int i;

    colour[0] = (real)0.5;
    for (i = 0; i < n; i++)
    {
        ops[0] = (real)(i % 100);
        ops[1] = (real)(i / 100);
        ops[2] = (real)2;
        ops[3] = (real)2;
        if (!xpost_record_mark(rec, XPOST_RECORD_FILLRECT, colour, ops, 4))
            return i;
    }
    return n;
}
#endif

int main(void)
{
    Xpost_Record *rec;
    Answers held, spilt;
    char why[160];

    /* Nothing below can be asked of a machine with no scratch space, and
       a run on one is not a failing tree. It is reported rather than
       passed over in silence: a test that skipped without saying so
       would look like a test that ran. */
    if (!xpost_spill_probe(why, sizeof why))
    {
        printf("SKIP no scratch file can be made in %s (%s), so a record"
               " has nowhere to spill to\n", xpost_temp_dir(), why);
        return verdict();
    }

    /* ---- the same answers, before and after ---- */
    rec = xpost_record_new(1);
    if (!rec)
    {
        report_failure("a record could not be made");
        return verdict();
    }
    (void)_fill(rec);
    _ask(rec, &held);
    check(!xpost_record_spilled(rec),
          "a record that has not been asked to spill says it has");
    if (!xpost_record_spill(rec))
        report_failure("a record with somewhere to spill to would not");
    check(xpost_record_spilled(rec), "a record that has spilled says it has not");
    _ask(rec, &spilt);

    check(held.nmark == spilt.nmark && held.nmark > 8,
          "a spilled record holds a different number of marks");
    check(held.nmask == spilt.nmask && held.nscreen == spilt.nscreen
          && held.nimage == spilt.nimage && held.nplace == spilt.nplace,
          "a spilled record holds different masks, screens, pictures or"
          " drawings");
    check(held.elo == spilt.elo && held.ehi == spilt.ehi,
          "a spilled record reaches different rows");
    check(held.bx0 == spilt.bx0 && held.by0 == spilt.by0
          && held.bx1 == spilt.bx1 && held.by1 == spilt.by1,
          "a spilled record reaches a different box");
    check(held.haslast == spilt.haslast && held.last == spilt.last,
          "a different mark had the last word over a run of rows");
    check(_same(&held.whole, &spilt.whole),
          "a spilled record plays the page differently");
    check(_same(&held.band, &spilt.band),
          "a spilled record plays a run of rows differently");
    check(spilt.band.n > 0 && spilt.band.n < spilt.whole.n,
          "a run of rows is not choosing between the marks");
    check(held.maskw == spilt.maskw && held.maskh == spilt.maskh
          && held.maskbyte == spilt.maskbyte,
          "a spilled record gives back a different coverage mask");
    check(held.cellw == spilt.cellw && held.cellh == spilt.cellh
          && held.cellbyte == spilt.cellbyte,
          "a spilled record gives back a different threshold cell");
    check(held.sample == spilt.sample && spilt.sample == 0x5a,
          "a spilled record gives back a different sample run");

    /* and what it is resident for, which is the whole point of it being
       in a file: the tables stay and the marks go */
    check(xpost_record_resident(rec) < xpost_record_bytes(rec),
          "a spilled record is resident for everything it holds");

    /* a page boundary rewinds the file rather than closing it */
    xpost_record_clear(rec);
    check(xpost_record_spilled(rec),
          "a page boundary took a spilled record's file away");
    check(xpost_record_count(rec) <= 1,
          "a page boundary left a spilled record holding the page before");
    xpost_record_free(rec);

    /* ---- the file is shortened under the record ---- */
    rec = xpost_record_new(1);
    if (rec)
    {
        Seen after;

        (void)_fill(rec);
        if (!xpost_record_spill(rec))
            report_failure("a record with somewhere to spill to would not");
        /* the header and no entries: every read of a mark now runs off
           the end, which is what a truncation under a descriptor nobody
           else holds looks like from in here */
        check(xpost_record_spill_shorten(rec, 32),
              "a spilled record's file could not be shortened");
        memset(&after, 0, sizeof after);
        check(xpost_record_replay(rec, (real)-1000, (real)1000, _see, &after)
              == ioerror,
              "a record whose file has been shortened does not refuse a"
              " replay with ioerror");
        check(xpost_record_failed(rec),
              "a record whose file has been shortened is not short of a mark");
        check(xpost_record_error(rec) == ioerror,
              "a record whose file has been shortened blames memory");
        check(xpost_record_count(rec) == 0 || after.n == 0,
              "a record whose file has been shortened played part of the page");
        xpost_record_free(rec);
    }

    /* ---- the primitive's own short read ---- */
    {
        Xpost_Spill *sp = xpost_spill_open();

        if (sp)
        {
            char buf[64];

            memset(buf, 0, sizeof buf);
            check(xpost_spill_write(sp, 0, buf, sizeof buf),
                  "a spill file would not take a write");
            check(xpost_spill_read(sp, 0, buf, sizeof buf),
                  "a spill file would not give back what it took");
            check(xpost_spill_truncate(sp, 16),
                  "a spill file would not be shortened");
            check(!xpost_spill_read(sp, 0, buf, sizeof buf),
                  "a shortened spill file answered a read of what is no"
                  " longer in it");
            xpost_spill_close(sp);
        }
        else
            report_failure("a spill file could not be made after the probe"
                           " said one could");
    }

#ifndef _WIN32
    /* ---- the file fills ---- */
    /* A limit on how large a file this process may write, which the
       spill's own writes then run into. Exceeding it raises SIGXFSZ,
       whose default action would end the process before the code under
       test could answer, so it is ignored for the length of this. */
    {
        struct rlimit was, now;

        if (getrlimit(RLIMIT_FSIZE, &was) == 0)
        {
            void (*old)(int) = signal(SIGXFSZ, SIG_IGN);

            now = was;
            now.rlim_cur = 4096;
            if (setrlimit(RLIMIT_FSIZE, &now) == 0)
            {
                rec = xpost_record_new(1);
                if (rec)
                {
                    int wrote;

                    if (!xpost_record_spill(rec))
                        report_failure("a record would not spill under a file"
                                       " size limit, before any write ran into"
                                       " it");
                    wrote = _bulk(rec, 4000);
                    check(wrote < 4000,
                          "a record went on taking marks after the file it"
                          " was writing them to could take no more");
                    check(xpost_record_failed(rec),
                          "a record whose file filled is not short of a mark");
                    check(xpost_record_error(rec) == ioerror,
                          "a record whose file filled blames memory rather"
                          " than the file");
                    check(xpost_record_replay(rec, (real)-1000, (real)1000,
                                              _see, &spilt.whole) == ioerror,
                          "a record whose file filled does not refuse a"
                          " replay with ioerror");
                    xpost_record_free(rec);
                }
            }
            else
                printf("SKIP a file size limit could not be set, so what a"
                       " record does when its file fills is not asked\n");
            (void)setrlimit(RLIMIT_FSIZE, &was);
            (void)signal(SIGXFSZ, old);
        }
    }

    /* ---- the scratch directory closes after the file is open ---- */
    /* It cannot reach a record: the descriptor is open, the name went at
       the moment the file was made, and directory permissions are not
       consulted again. That is answered by construction rather than
       handled, which is worth holding to rather than assuming. */
    {
        char dir[512];
        const char *tmp = getenv("TMPDIR");
        int made = 0;

        snprintf(dir, sizeof dir, "%s/xpost-spill-shut-%ld",
                 tmp && *tmp ? tmp : "/tmp", (long)getpid());
        if (mkdir(dir, 0700) == 0)
        {
            made = 1;
            setenv("TMPDIR", dir, 1);
        }
        rec = made ? xpost_record_new(1) : NULL;
        if (rec)
        {
            Seen after;

            (void)_fill(rec);
            if (!xpost_record_spill(rec))
                report_failure("a record would not spill into a directory of"
                               " its own");
            /* and now nothing may be made there any more */
            check(chmod(dir, 0500) == 0,
                  "the scratch directory could not be closed");
            check(_bulk(rec, 200) == 200,
                  "a record stopped taking marks when the directory its file"
                  " was made in stopped taking files");
            memset(&after, 0, sizeof after);
            /* the player above stops at the marks it has room for, so
               what is asked here is that the walk reached them at all */
            (void)xpost_record_replay(rec, (real)-1000, (real)1000,
                                      _see, &after);
            check(after.n > 8,
                  "a record could not play back a page after the directory"
                  " its file was made in stopped taking files");
            check(!xpost_record_failed(rec),
                  "a record was left short of a mark by a directory it no"
                  " longer has a name in");
            xpost_record_free(rec);
            (void)chmod(dir, 0700);
        }
        else if (made)
            report_failure("a record could not be made");
        if (made)
        {
            if (tmp && *tmp)
                setenv("TMPDIR", tmp, 1);
            else
                unsetenv("TMPDIR");
            (void)rmdir(dir);
        }
    }
#endif

    return verdict();
}
