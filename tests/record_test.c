/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (C) 2013-2016, Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * (BSD 3-clause; see COPYING)
 */

/* What a record gives back, and to which rows.
 *
 * A record exists so that a page can be painted into a raster smaller
 * than the page, by being played once per run of rows. Two things have
 * to hold for that to be worth anything:
 *
 *   Everything comes back. Playing the whole extent gives every mark,
 *   in the order it was made, with the values it was made with. The
 *   order is not incidental: marks overpaint, so the order they are
 *   played in is the order they were painted in.
 *
 *   Only what reaches comes back. Playing a run of rows gives the marks
 *   that reach those rows and no others -- that is the whole of why a
 *   large page is affordable, and a record that played everything every
 *   time would be correct and useless.
 *
 * The second is the one a test has to be careful about, because a
 * record that ignored the range entirely would pass any check that only
 * asked whether the right marks were present.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include "xpost_object.h"
#include "xpost_op_path.h"   /* XPOST_PATH_BREAK: a subpath separator */
#include "xpost_record.h"

#include "xpost_test.h"

/* what a replay saw */
typedef struct
{
    int n;
    Xpost_Record_Kind kind[64];
    real first[64];   /* the first colour value, to tell marks apart */
    real op0[64];     /* and the first operand */
} Seen;

static int _see(void *data, Xpost_Record_Kind kind, const real *colour,
                const real *ops, int nops)
{
    Seen *s = data;

    if (s->n >= 64)
        return 1;
    s->kind[s->n] = kind;
    s->first[s->n] = colour[0];
    s->op0[s->n] = nops > 0 ? ops[0] : 0.0;
    s->n++;
    return 0;
}

static Seen _play(Xpost_Record *rec, real lo, real hi)
{
    Seen s;

    memset(&s, 0, sizeof s);
    xpost_record_replay(rec, lo, hi, _see, &s);
    return s;
}

int main(void)
{
    Xpost_Record *rec;
    real grey[1];
    real ops[16];
    real lo, hi;
    Seen s;

    rec = xpost_record_new(1);
    if (!rec)
    {
        report_failure("a record for one colour component");
        return verdict();
    }

    if (xpost_record_count(rec) != 0)
        report_failure("a new record holds no mark");
    if (xpost_record_extent(rec, &lo, &hi))
        report_failure("a record holding no mark reaches no row");

    /* four marks at known rows: a pixel at 10, a rectangle over 20..30,
       a line over 40..50 (given the other way round, so that the ends
       are taken rather than assumed ordered), and a triangle over
       60..80. The colour tells them apart in what comes back. */
    grey[0] = 1.0; ops[0] = 5.0; ops[1] = 10.0;
    if (!xpost_record_mark(rec, XPOST_RECORD_PUTPIX, grey, ops, 2))
        report_failure("a pixel is written down");

    grey[0] = 2.0; ops[0] = 0.0; ops[1] = 20.0; ops[2] = 8.0; ops[3] = 10.0;
    if (!xpost_record_mark(rec, XPOST_RECORD_FILLRECT, grey, ops, 4))
        report_failure("a rectangle is written down");

    grey[0] = 3.0; ops[0] = 0.0; ops[1] = 50.0; ops[2] = 9.0; ops[3] = 40.0;
    if (!xpost_record_mark(rec, XPOST_RECORD_DRAWLINE, grey, ops, 4))
        report_failure("a line is written down");

    grey[0] = 4.0;
    ops[0] = 3.0;
    ops[1] = 0.0;  ops[2] = 60.0;
    ops[3] = 10.0; ops[4] = 80.0;
    ops[5] = 20.0; ops[6] = 70.0;
    if (!xpost_record_mark(rec, XPOST_RECORD_FILLPOLY, grey, ops, 7))
        report_failure("a polygon is written down");

    if (xpost_record_count(rec) != 4)
        report_failure("the record holds the four marks made");

    if (!xpost_record_extent(rec, &lo, &hi) || lo != 10.0 || hi != 80.0)
        report_failure("the record reaches from the first row marked to"
                       " the last");

    /* everything, in the order it was made */
    s = _play(rec, -1000.0, 1000.0);
    if (s.n != 4)
        report_failure("playing every row gives every mark: %d of 4", s.n);
    else if (s.first[0] != 1.0 || s.first[1] != 2.0 ||
             s.first[2] != 3.0 || s.first[3] != 4.0)
        report_failure("the marks come back in the order they were made");
    else if (s.kind[1] != XPOST_RECORD_FILLRECT ||
             s.kind[3] != XPOST_RECORD_FILLPOLY)
        report_failure("each mark comes back as the kind it was made");
    else if (s.op0[0] != 5.0)
        report_failure("a mark comes back with the operands it was made"
                       " with");

    /* one row, met by one mark */
    s = _play(rec, 10.0, 10.0);
    if (s.n != 1 || s.first[0] != 1.0)
        report_failure("a row met by one mark gives that mark alone:"
                       " %d mark(s)", s.n);

    /* a run met by none: between the pixel and the rectangle */
    s = _play(rec, 12.0, 18.0);
    if (s.n != 0)
        report_failure("a run of rows no mark reaches gives nothing:"
                       " %d mark(s)", s.n);

    /* a rectangle is met anywhere across its height, ends included */
    s = _play(rec, 25.0, 25.0);
    if (s.n != 1 || s.first[0] != 2.0)
        report_failure("a rectangle is met by a row inside it");
    s = _play(rec, 30.0, 30.0);
    if (s.n != 1 || s.first[0] != 2.0)
        report_failure("a rectangle is met by the last row it covers");
    s = _play(rec, 31.0, 39.0);
    if (s.n != 0)
        report_failure("a rectangle is not met past the row it ends on");

    /* a line given from its far end still reaches the rows between */
    s = _play(rec, 45.0, 45.0);
    if (s.n != 1 || s.first[0] != 3.0)
        report_failure("a line reaches the rows between its ends however"
                       " the ends were given");

    /* a polygon reaches the rows its vertices span, and is met by any
       of them -- its reach is a walk of the vertices rather than a pair
       of values, so it is asked at a row only an inner vertex reaches */
    s = _play(rec, 79.0, 79.0);
    if (s.n != 1 || s.first[0] != 4.0)
        report_failure("a polygon is met by a row inside the vertices it"
                       " spans");
    s = _play(rec, 81.0, 90.0);
    if (s.n != 0)
        report_failure("a polygon is not met past its furthest vertex");

    /* a run meeting two marks gives both, still in order */
    s = _play(rec, 10.0, 25.0);
    if (s.n != 2 || s.first[0] != 1.0 || s.first[1] != 2.0)
        report_failure("a run meeting two marks gives both in order:"
                       " %d mark(s)", s.n);

    /* A polygon's subpath separators are pairs among its vertices, and a
       separator is not a point: it reaches no row. Taken as one it would
       put the polygon's reach at the sentinel's own value, and a polygon
       reaching from there would be met by every range there is -- which
       is correct and useless, since the point of a range is to visit the
       marks that meet it. The one written down here has a separator
       between two subpaths at rows 100..120, and the rows it reaches are
       the rows its vertices are on. */
    grey[0] = 5.0;
    ops[0] = 7.0;
    ops[1] = 0.0;   ops[2] = 100.0;
    ops[3] = 10.0;  ops[4] = 110.0;
    ops[5] = 20.0;  ops[6] = 100.0;
    ops[7] = XPOST_PATH_BREAK; ops[8] = XPOST_PATH_BREAK;
    ops[9] = 4.0;   ops[10] = 110.0;
    ops[11] = 8.0;  ops[12] = 120.0;
    ops[13] = 12.0; ops[14] = 110.0;
    if (!xpost_record_mark(rec, XPOST_RECORD_FILLPOLY, grey, ops, 15))
        report_failure("a polygon with a subpath separator is written down");
    s = _play(rec, 100.0, 120.0);
    if (s.n != 1 || s.first[0] != 5.0)
        report_failure("a polygon is met over the rows its vertices span:"
                       " %d mark(s)", s.n);
    s = _play(rec, 90.0, 99.0);
    if (s.n != 0)
        report_failure("a subpath separator is no vertex and reaches no row:"
                       " %d mark(s)", s.n);

    /* The rows a mark can ink are whole rows and what it was made with
       is not, so a reach compared against a run as it stands loses marks
       at the fraction. Two ways, both of which a page put out a run of
       rows at a time meets on its first stroke:

       A shape covering the lower half of a row inks that row. A stroke
       of any width is a rectangle around a segment, so its corners sit
       half a row off whatever its ends were on, and this is the ordinary
       case rather than a corner of one. A run of rows ending there has
       to be given the mark: one judged not to reach a run is simply
       missing from the page, and a page missing a mark looks like a
       page.

       And a segment's ends are put on the 1/256 grid before it is walked
       (xpost_dev_line_quantize), which can carry an end sitting a
       fraction below a row boundary over it. The segment then paints a
       row past the rows its own coordinates fall in, and the run holding
       that row has to be given it too. */
    {
        Xpost_Record *fine = xpost_record_new(1);

        if (!fine)
            report_failure("a record for what a fraction of a row reaches");
        else
        {
            grey[0] = 1.0;
            ops[0] = 3.0;
            ops[1] = 0.0;  ops[2] = 9.5;
            ops[3] = 10.0; ops[4] = 95.5;
            ops[5] = 20.0; ops[6] = 9.5;
            if (!xpost_record_mark(fine, XPOST_RECORD_FILLPOLY, grey, ops, 7))
                report_failure("a shape reaching half a row is written down");
            s = _play(fine, 0.0, 9.0);
            if (s.n != 1)
                report_failure("a shape covering the lower half of a row is"
                               " met by a run ending on that row: %d mark(s)",
                               s.n);
            s = _play(fine, 96.0, 120.0);
            if (s.n != 0)
                report_failure("a shape is not met past the row its furthest"
                               " point falls in: %d mark(s)", s.n);
            xpost_record_free(fine);
        }

        fine = xpost_record_new(1);
        if (!fine)
            report_failure("a record for what a segment's ends round to");
        else
        {
            grey[0] = 2.0;
            ops[0] = 0.0; ops[1] = 4.0; ops[2] = 40.0; ops[3] = 9.999;
            if (!xpost_record_mark(fine, XPOST_RECORD_DRAWLINE, grey, ops, 4))
                report_failure("a segment ending just short of a row is"
                               " written down");
            s = _play(fine, 10.0, 20.0);
            if (s.n != 1)
                report_failure("a segment whose end rounds onto the row below"
                               " is met by the run holding that row:"
                               " %d mark(s)", s.n);
            s = _play(fine, 11.0, 20.0);
            if (s.n != 0)
                report_failure("... and by that row alone: %d mark(s)", s.n);
            xpost_record_free(fine);
        }
    }

    /* The same run of rows, walked a mark at a time. A replay that plays
       into a device returns to the interpreter between marks -- a method
       may be a procedure -- so it cannot be the loop above and asks
       instead which mark comes next. The two have to agree about every
       range, or the same page would be painted differently depending on
       which of them asked. The record here holds the pixel at row 10,
       the rectangle over 20..30, the line over 40..50, the triangle over
       60..80 and the two-subpath polygon over 100..120. */
    {
        size_t at;
        real lo2, hi2;
        int i, agree;

        if (!xpost_record_next(rec, 0, -1000.0, 1000.0, &at) || at != 0)
            report_failure("the first mark of a record is the one a walk of"
                           " every row reaches first");
        if (!xpost_record_next(rec, 1, 12.0, 18.0, &at))
            /* nothing there: the walk answers so rather than running on */
            (void)0;
        else
            report_failure("a walk over rows no mark reaches finds one at %d",
                           (int)at);
        if (!xpost_record_next(rec, 0, 45.0, 45.0, &at) || at != 2)
            report_failure("a walk finds the line at the rows between its"
                           " ends");
        if (xpost_record_next(rec, 3, 45.0, 45.0, &at))
            report_failure("a walk resumed past a mark does not find it"
                           " again");

        /* and they agree, range by range, over every row the record
           reaches and a row either side of it */
        agree = 1;
        if (!xpost_record_extent(rec, &lo2, &hi2))
            report_failure("the record reaches a row to walk");
        for (i = (int)lo2 - 1; i <= (int)hi2 + 1; i++)
        {
            size_t j = 0, k = 0;

            s = _play(rec, (real)i, (real)i);
            while (xpost_record_next(rec, k, (real)i, (real)i, &at))
            {
                j++;
                k = at + 1;
            }
            if ((int)j != s.n)
                agree = 0;
        }
        if (!agree)
            report_failure("a walk of a row finds the marks a replay of that"
                           " row plays");
    }

    /* a record refuses a mark whose operands do not fit its kind, so
       that a walk of what was written down stays inside it */
    grey[0] = 9.0; ops[0] = 1.0; ops[1] = 2.0;
    if (xpost_record_mark(rec, XPOST_RECORD_FILLRECT, grey, ops, 2))
        report_failure("a rectangle needs the four operands a rectangle"
                       " has");
    ops[0] = 5.0;   /* says five vertices and gives one */
    ops[1] = 0.0; ops[2] = 0.0;
    if (xpost_record_mark(rec, XPOST_RECORD_FILLPOLY, grey, ops, 3))
        report_failure("a polygon needs as many vertices as it says it"
                       " has");
    if (xpost_record_count(rec) != 5)
        report_failure("a refused mark is not written down");

    /* An image is one entry rather than one mark a sample. The five
       marking kinds are the wrong shape for it: a device holding no
       rows of its own is painted an image a rectangle at a time, and a
       record of those costs tens of bytes a sample against the one to
       three bytes a pixel of the page it exists to avoid holding.
       This one is four samples across and three rows down, placed ten
       device rows to the sample row from row 300. */
    {
        unsigned char rows[3][4];
        const unsigned char *run[3];
        Xpost_Record_Image img;
        const Xpost_Record_Image *back;
        size_t before;
        int y0, y1, k;

        memset(&img, 0, sizeof img);
        for (k = 0; k < 3; k++)
        {
            rows[k][0] = (unsigned char)(10 * k);
            rows[k][1] = (unsigned char)(10 * k + 1);
            rows[k][2] = (unsigned char)(10 * k + 2);
            rows[k][3] = (unsigned char)(10 * k + 3);
            run[k] = rows[k];
        }
        img.width = 4;
        img.height = 3;
        img.ncomp = 1;
        img.nat = 1;
        img.yoff = 300.0;
        img.yscale = 10.0;
        img.xoff = 0.0;
        img.xscale = 1.0;
        img.cx0 = 0.0;
        img.cy0 = 0.0;
        img.cx1 = 1000.0;
        img.cy1 = 1000.0;

        before = xpost_record_bytes(rec);
        if (!xpost_record_image(rec, &img, run, 3))
            report_failure("an image is written down");
        if (xpost_record_image_count(rec) != 1)
            report_failure("the record holds the one image made");
        if (xpost_record_count(rec) != 6)
            report_failure("an image is one mark in the run, not one a"
                           " sample");
        if (xpost_record_bytes(rec) <= before)
            report_failure("an image costs the record something");

        /* The samples are the painter's own buffers, refilled for the
           row after, and a record outlives the job that made it. So
           what it holds has to be its own: the rows are overwritten
           here and what was written down does not change. */
        back = xpost_record_image_get(rec, 0);
        memset(rows, 0xee, sizeof rows);
        if (!back)
            report_failure("the image comes back");
        else if (back->samples == rows[0])
            report_failure("an image references the rows it was handed"
                           " rather than copying them");
        else if (back->samples[0] != 0 || back->samples[4] != 10
              || back->samples[11] != 23)
            report_failure("an image comes back with the samples it was"
                           " written down with");

        /* it is met where it is placed, and not above or below it */
        s = _play(rec, 300.0, 330.0);
        if (s.n != 1 || s.kind[0] != XPOST_RECORD_IMAGE)
            report_failure("an image is met by the rows it is placed"
                           " over: %d mark(s)", s.n);
        else if (s.op0[0] != 0.0)
            report_failure("an image names the entry it was written to");
        s = _play(rec, 200.0, 299.0);
        if (s.n != 0)
            report_failure("an image is not met above where it is placed:"
                           " %d mark(s)", s.n);
        s = _play(rec, 331.0, 400.0);
        if (s.n != 0)
            report_failure("an image is not met below where it is placed:"
                           " %d mark(s)", s.n);

        /* An image is clipped to a run of rows by choosing which of its
           own rows to write, which is what the placing transform
           decides. Every row for the whole of it; the rows over that
           part of the page for a part of it; none where the run is
           somewhere else entirely. The answer errs outward, so what is
           held is that it covers what it must rather than that it
           covers nothing more. */
        if (!xpost_record_image_rows(back, 0.0, 1000.0, &y0, &y1)
         || y0 != 0 || y1 != 3)
            report_failure("a run covering the page writes every row of"
                           " an image: %d..%d", y0, y1);
        if (!xpost_record_image_rows(back, 310.0, 319.0, &y0, &y1)
         || y0 > 1 || y1 < 2)
            report_failure("a run over one sample row writes that row:"
                           " %d..%d", y0, y1);
        if (xpost_record_image_rows(back, 0.0, 100.0, &y0, &y1))
            report_failure("a run above an image writes none of its rows:"
                           " %d..%d", y0, y1);
        if (xpost_record_image_rows(back, 500.0, 600.0, &y0, &y1))
            report_failure("a run below an image writes none of its rows:"
                           " %d..%d", y0, y1);

        /* the same image placed the other way up: a transform running
           backwards puts the last sample row at the top, and the rows a
           range wants follow the transform rather than the order they
           were given in */
        {
            Xpost_Record_Image flip = *back;

            flip.yoff = 330.0;
            flip.yscale = -10.0;
            if (!xpost_record_image_rows(&flip, 300.0, 309.0, &y0, &y1)
             || y0 > 2 || y1 < 3)
                report_failure("a run at the top of an image placed the"
                               " other way up writes its last row: %d..%d",
                               y0, y1);
            if (xpost_record_image_rows(&flip, 500.0, 600.0, &y0, &y1))
                report_failure("a run off an image placed the other way up"
                               " writes none of its rows");
        }

        /* an image the record cannot index is refused rather than
           written down half way, on the same terms as a malformed mark */
        if (xpost_record_image(rec, &img, run, 2))
            report_failure("an image needs as many rows as it says it"
                           " has");
        if (xpost_record_image_count(rec) != 1)
            report_failure("a refused image is not written down");
    }

    /* A record that could not hold a mark describes a page it cannot
       reproduce. Nothing here can exhaust memory to order, so what is
       held is the rule that follows from it: a record reporting itself
       short refuses to be played, so a caller cannot paint a page that
       is quietly missing something. */
    if (xpost_record_failed(rec))
        report_failure("a record given only marks it could hold reports"
                       " itself whole");

    /* A drawing placed in a page is named and not copied, so what the
       page pays for the second placement is the placement. The drawing
       here is a rectangle over rows 0..9; the page places it three
       times, twice at one offset apart and once further down. */
    {
        Xpost_Record *page;
        Xpost_Record *draw;
        size_t before, after;

        page = xpost_record_new(1);
        draw = xpost_record_new(1);
        if (!page || !draw)
        {
            report_failure("a record to place and a record to place it in");
            return verdict();
        }
        grey[0] = 7.0;
        ops[0] = 0.0; ops[1] = 0.0; ops[2] = 4.0; ops[3] = 9.0;
        if (!xpost_record_mark(draw, XPOST_RECORD_FILLRECT, grey, ops, 4))
            report_failure("a drawing holds a mark");

        if (!xpost_record_place(page, draw, 0.0, 100.0))
            report_failure("a drawing is placed");
        before = xpost_record_bytes(page);
        if (!xpost_record_place(page, draw, 50.0, 100.0))
            report_failure("the same drawing is placed again");
        if (!xpost_record_place(page, draw, 0.0, 300.0))
            report_failure("and placed a third time");
        after = xpost_record_bytes(page);

        if (xpost_record_count(page) != 3)
            report_failure("a page places a drawing once per placement");
        if (xpost_record_place_count(page) != 1)
            report_failure("a drawing placed three times is one drawing:"
                           " %d", (int)xpost_record_place_count(page));
        if (xpost_record_place_get(page, 0) != draw)
            report_failure("the drawing a placement names is the drawing"
                           " placed");
        /* what two further placements cost is two marks, not two
           drawings: the whole of why a placement is a placement */
        if (after - before >= xpost_record_bytes(draw))
            report_failure("two more placements cost less than the drawing"
                           " they place: %d against %d",
                           (int)(after - before), (int)xpost_record_bytes(draw));

        /* a placement reaches the rows the drawing reaches from where it
           was put, so a run meeting neither placement plays neither */
        if (!xpost_record_extent(page, &lo, &hi) || lo != 100.0 || hi != 309.0)
            report_failure("a page reaches the rows its placements reach:"
                           " %d..%d", (int)lo, (int)hi);
        s = _play(page, 100.0, 109.0);
        if (s.n != 2)
            report_failure("the run the first two placements were put on"
                           " plays them and not the third: %d", s.n);
        s = _play(page, 200.0, 250.0);
        if (s.n != 0)
            report_failure("a run between two placements plays neither:"
                           " %d", s.n);
        /* and a placement is handed over as the mark it is: what it
           stands for is the drawing's marks, which a caller that
           descends plays */
        if (s.n == 0)
        {
            s = _play(page, 300.0, 309.0);
            if (s.n != 1 || s.kind[0] != XPOST_RECORD_PLACE)
                report_failure("a placement plays as one mark naming a"
                               " drawing");
        }

        /* A drawing outlives whoever made it while a page still places
           it: what the page holds is a reference. Given up here, the
           drawing is still the page's, and the page still plays. */
        xpost_record_free(draw);
        s = _play(page, 100.0, 109.0);
        if (s.n != 2)
            report_failure("a page plays a drawing its maker has given up");

        /* A record placed inside itself would be played until something
           ran out, and a drawing nested deeper than a replay descends
           could not be played at all. Both are refused where the
           placement is made. */
        if (xpost_record_place(page, page, 0.0, 0.0))
            report_failure("a record is not placed inside itself");
        {
            Xpost_Record *chain[XPOST_RECORD_NEST + 2];
            int k, refused = 0;

            memset(chain, 0, sizeof chain);
            chain[0] = page;
            for (k = 1; k < XPOST_RECORD_NEST + 2 && !refused; k++)
            {
                chain[k] = xpost_record_new(1);
                if (!chain[k])
                    break;
                if (!xpost_record_place(chain[k], chain[k - 1], 0.0, 0.0))
                    refused = k;
            }
            if (!refused)
                report_failure("a drawing nested past what a replay descends"
                               " is refused");
            else if (refused < XPOST_RECORD_NEST - 2)
                report_failure("a drawing is refused at %d and not at the"
                               " depth a replay descends", refused);
            for (k = 1; k < XPOST_RECORD_NEST + 2; k++)
                if (chain[k])
                    xpost_record_free(chain[k]);
        }

        xpost_record_free(page);
    }

    xpost_record_free(rec);
    xpost_record_free(NULL);   /* nothing is not something to give up */

    return verdict();
}
