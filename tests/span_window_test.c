/* A band window keeps exactly the spans the whole page would have kept
 * there, and nothing else.
 *
 * Scan conversion may be told an inclusive band range, and then it keeps
 * only the passages that fall in it. That is a performance device: the
 * whole boundary is still walked, because the insideness rule needs the
 * whole boundary to be right about any part of it, but the bands outside
 * the range settle no span. Which makes the window's correctness a
 * statement that can be checked without any reference to a previous
 * build, and without a golden file:
 *
 *     scanconvert(shape, window) == filter(scanconvert(shape, NULL), window)
 *
 * bit for bit, on the band and both ends of every span. Anything that
 * changes how the walk reaches the window -- skipping bands rather than
 * stepping through them, say -- is held by that equality, because the
 * unwindowed conversion never skips anything.
 *
 * The comparison is on the exact doubles, not within a tolerance. A span
 * end is interpolated from the crossing's y, so a walk that arrives at a
 * band another way has to arrive at the same number, and a tolerance
 * here would hide precisely the drift worth catching.
 *
 * WHAT IS DRIVEN, and the shapes are chosen for where band-edge
 * insideness is decided rather than for looking like drawings:
 *
 *   random shapes at three coordinate grains -- free, snapped to band
 *   boundaries, and on a 1/256 grid, which is where a coordinate that
 *   is nearly a boundary is not one;
 *
 *   and the cases a random shape almost never produces: flat subpaths on
 *   and off a boundary, boundary-aligned boxes, tall slivers narrower
 *   than a pixel, nested subpaths of opposite winding, a single point, a
 *   spike touching a boundary from above and from below, and a shape
 *   entirely outside its window on each side.
 *
 * Each shape is driven under both insideness rules and against several
 * windows, including windows that fall entirely outside the shape, one
 * band tall, and covering the whole page.
 *
 * AND THE CROSSING ITSELF, which is the number that equality is stated
 * on. Two families of edges are converted for their crossings alone:
 * ones passing through lattice points, whose crossings the conversion
 * has to land on exactly rather than a fraction past, and ones far
 * enough out that a single precision interpolation cannot land on them
 * at all. The second is what says the conversion works in a double and
 * not in the interpreter's own number, which is as wide as the object
 * is -- a page does not depend on how wide this build's numbers are,
 * and a suite running one width at a time holds that by writing the
 * double's answer down.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_span.h"
#include "xpost_op_path.h" /* XPOST_PATH_BREAK: the subpath separator */
#include "xpost_test.h"

#define MAXSPANS 200000
#define MAXPTS   256

typedef struct
{
    int band;
    double lo, hi;
} Span;

typedef struct
{
    Xpost_Span_Consumer consumer;
    Span *v;
    int n;
    int overflow;
} Collect;

static int _take(Xpost_Span_Consumer *c, int band, double lo, double hi)
{
    Collect *k = (Collect *)c;

    if (k->n >= MAXSPANS)
    {
        k->overflow = 1;
        return 0;
    }
    k->v[k->n].band = band;
    k->v[k->n].lo = lo;
    k->v[k->n].hi = hi;
    k->n++;
    return 0;
}

/* the conversion frees the vertices it is given, so each call is handed
   its own copy of the shape */
static int convert(const Xpost_Span_Vertex *pts, integer n, int evenodd,
                   const Xpost_Span_Rows *rows, Collect *out)
{
    Xpost_Span_Vertex *copy = malloc((size_t)n * sizeof *copy);

    if (!copy)
        return 1;
    memcpy(copy, pts, (size_t)n * sizeof *copy);
    out->consumer.take = _take;
    out->n = 0;
    out->overflow = 0;
    return xpost_span_scanconvert(copy, n, evenodd, rows, &out->consumer);
}

/* what the unwindowed conversion kept inside the window */
static int filter(const Collect *all, const Xpost_Span_Rows *w, Span *out)
{
    int i, n = 0;

    for (i = 0; i < all->n; i++)
        if (all->v[i].band >= w->lo && all->v[i].band <= w->hi)
            out[n++] = all->v[i];
    return n;
}

static unsigned int seed = 20260815u;
static unsigned int nextrand(void)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) & 0xffffffu;
}
static real randcoord(int grain, real span)
{
    real v = (real)(nextrand() % 100000) / 100000.0 * span;

    if (grain == 1)
        return (real)(long)v;                  /* snapped to a band boundary */
    if (grain == 2)
        return (real)(long)(v * 256.0) / 256.0; /* on a 1/256 grid */
    return v;
}

static long cases;
static long mismatches;
static Span *ubuf, *wbuf, *fbuf;

/* Report through the shared helper, which records the failure as well as
   printing it, so the exit status carries what the output says. Only the
   first few are spelled out: a window that is wrong is wrong for most of
   the shapes driven here, and 25,000 lines of it says no more than ten
   do. The total is checked at the end either way. */
#define MISMATCH_DETAIL 10
static void mismatch(const char *fmt, ...) XPOST_TEST_PRINTF(1, 2);
static void mismatch(const char *fmt, ...)
{
    mismatches++;
    if (mismatches <= MISMATCH_DETAIL)
    {
        va_list ap;

        va_start(ap, fmt);
        vfprintf(stdout, fmt, ap);
        va_end(ap);
        fputc('\n', stdout);
        report_failure("a window kept something the page did not");
    }
}

static void drive(const Xpost_Span_Vertex *pts, integer n, const char *what)
{
    static const int wins[][2] = {
        { 0, 199 }, { 5, 5 }, { 0, 0 }, { 60, 61 }, { 100, 140 },
        { 199, 199 }, { 250, 300 }, { -40, -1 }, { 40, 45 }
    };
    Collect all, win;
    int eo, w;

    for (eo = 0; eo <= 1; eo++)
    {
        all.v = ubuf;
        if (convert(pts, n, eo, NULL, &all) != 0 || all.overflow)
        {
            mismatch("%s: the unwindowed conversion did not complete\n", what);
            return;
        }
        for (w = 0; w < (int)(sizeof wins / sizeof *wins); w++)
        {
            Xpost_Span_Rows r;
            int want, i;

            r.lo = wins[w][0];
            r.hi = wins[w][1];
            win.v = wbuf;
            if (convert(pts, n, eo, &r, &win) != 0 || win.overflow)
            {
                mismatch("%s: the windowed conversion did not complete\n", what);
                continue;
            }
            want = filter(&all, &r, fbuf);
            cases++;
            if (want != win.n)
            {
                mismatch("%s: window [%d,%d] rule %d gave %d span(s),"
                       " the page kept %d there\n",
                       what, r.lo, r.hi, eo, win.n, want);
                continue;
            }
            for (i = 0; i < want; i++)
            {
                if (win.v[i].band != fbuf[i].band
                    || win.v[i].lo != fbuf[i].lo
                    || win.v[i].hi != fbuf[i].hi)
                {
                    mismatch("%s: window [%d,%d] rule %d span %d is"
                           " band %d [%.17g,%.17g], the page kept"
                           " band %d [%.17g,%.17g]\n",
                           what, r.lo, r.hi, eo, i,
                           win.v[i].band, (double)win.v[i].lo, (double)win.v[i].hi,
                           fbuf[i].band, (double)fbuf[i].lo, (double)fbuf[i].hi);
                    break;
                }
            }
        }
    }
}

#define BRK XPOST_PATH_BREAK

static void handmade(void)
{
    /* a flat subpath exactly on a band boundary, and one just off it */
    { Xpost_Span_Vertex p[] = {{10,60},{90,60},{50,60}}; drive(p,3,"flat on a boundary"); }
    { Xpost_Span_Vertex p[] = {{10,60.5},{90,60.5},{50,60.5}}; drive(p,3,"flat off a boundary"); }
    /* boxes whose edges land on boundaries, and boxes that straddle them */
    { Xpost_Span_Vertex p[] = {{10,60},{90,60},{90,62},{10,62}}; drive(p,4,"boundary-aligned box"); }
    { Xpost_Span_Vertex p[] = {{10,59.5},{90,59.5},{90,62.5},{10,62.5}}; drive(p,4,"straddling box"); }
    /* a sliver narrower than a pixel, spanning many bands */
    { Xpost_Span_Vertex p[] = {{40,5},{40.25,5},{40.25,190},{40,190}}; drive(p,4,"tall sliver"); }
    /* nested subpaths of opposite winding */
    { Xpost_Span_Vertex p[] = {{10,10},{100,10},{100,100},{10,100},{BRK,0},
                               {30,30},{30,80},{80,80},{80,30}};
      drive(p,9,"nested opposite winding"); }
    /* a single point, and a degenerate edge */
    { Xpost_Span_Vertex p[] = {{50,50},{50,50},{50,50}}; drive(p,3,"single point"); }
    /* spikes touching a boundary from each side */
    { Xpost_Span_Vertex p[] = {{20,60},{25,59.999},{30,60}}; drive(p,3,"spike from below"); }
    { Xpost_Span_Vertex p[] = {{20,60},{25,60.001},{30,60}}; drive(p,3,"spike from above"); }
    /* wholly outside a window on each side */
    { Xpost_Span_Vertex p[] = {{10,2},{40,2},{40,4},{10,4}}; drive(p,4,"below every window"); }
    { Xpost_Span_Vertex p[] = {{10,180},{40,180},{40,195},{10,195}}; drive(p,4,"above most windows"); }
    /* an edge whose ends sit exactly on two boundaries */
    { Xpost_Span_Vertex p[] = {{10,60},{50,140},{12,140}}; drive(p,3,"boundary to boundary"); }
}


/* An edge through a lattice point crosses there exactly.
 *
 * The conversion cuts an edge at each band boundary and interpolates the
 * x it crosses at. Where that crossing is a whole number -- which every
 * edge passing through a lattice point produces, so constantly -- the
 * interpolation has to land on it and not a fraction past it: the column
 * the extent reaches is decided by a floor and a ceiling further on, and
 * a crossing of 30.000002 where the answer is 30 takes one more column
 * than the shape covers.
 *
 * Held here because it is the one property of the conversion that a
 * single build cannot notice on its own. Interpolated at the width of
 * the interpreter's own number, the crossing is exact where that is a
 * double and a little past where it is a single, so the same page comes
 * out of the two builds with different pixels along an edge -- and each
 * build, asked only to agree with itself, is perfectly consistent.
 *
 * The edge runs (0,0) to (300,900): a third of a unit across per unit
 * down, so at every third band boundary the crossing is a whole number.
 * A third is not representable either way, which is the point -- what
 * differs is whether multiplying it back out recovers the whole number.
 */
static void lattice_crossings(void)
{
    Xpost_Span_Vertex p[4];
    Collect got;
    int k, checked = 0, off = 0;

    p[0].x = 0.0;   p[0].y = 0.0;
    p[1].x = 300.0; p[1].y = 900.0;
    p[2].x = 0.0;   p[2].y = 900.0;
    p[3].x = 0.0;   p[3].y = 0.0;

    got.v = ubuf;
    if (convert(p, 4, 0, NULL, &got))
    {
        check(0, "the lattice-crossing shape converted");
        return;
    }

    /* band 3k-1 is cut at y = 3k, where the edge is at x = k exactly */
    for (k = 1; k <= 300; k++)
    {
        int band = 3 * k - 1;
        int i;

        for (i = 0; i < got.n; i++)
        {
            if (got.v[i].band != band)
                continue;
            if (got.v[i].hi >= (double)(k - 1) && got.v[i].hi <= (double)(k + 1))
            {
                checked++;
                if (got.v[i].hi != (double)k)
                    off++;
            }
        }
    }

    check(checked > 250, "the edge was cut at the lattice boundaries");
    check(off == 0, "every lattice crossing landed on the whole number");
    printf("span-window: %d lattice crossing(s), %d off the whole number\n",
           checked, off);
}

/* And a crossing single precision cannot reach.
 *
 * The lattice crossings above are whole numbers a single recovers as
 * exactly as a double does, so they say nothing about which of the two
 * the conversion works in: they hold the order of the multiply and the
 * divide, and a conversion interpolating at the width of the
 * interpreter's own number answers them just as well.
 *
 * The edges here separate the widths. Each runs from the origin to a
 * lattice point far enough out that the interpolation's product passes
 * what a single holds exactly -- around seventeen million, which a page
 * of a few thousand device rows reaches on its own -- so the whole
 * number the crossing is survives the divide in a double and not in a
 * single. What a single answers is half a thousandth to one side of it,
 * and the floor and the ceiling further on read that as a different
 * column: the same page then carries different pixels along the edge on
 * one build than on the other.
 *
 * Which is the whole of why these are here. A suite runs one width at a
 * time and cannot compare two builds; what it can do is write down the
 * number a double cannot help but produce and hold every build to it.
 * A conversion that reads the interpreter's number fails this on the
 * narrow build and passes it on the wide one, and the disagreement
 * between the two becomes a failure inside one of them.
 *
 * The single-precision answers are computed here rather than assumed,
 * through volatiles so that each step is rounded to a single whatever
 * width the compiler evaluates in, and every edge in the table is
 * required to be one a single gets wrong: a table that stopped
 * separating the widths would otherwise go on passing while holding
 * nothing.
 */
static double _single_crossing(long dx, long dy, long b)
{
    volatile float num = (float)dx * (float)b;
    volatile float q = num / (float)dy;

    return (double)q;
}

static void width_bearing_crossings(void)
{
    /* dx, dy, b: the edge (0,0)-(dx,dy) crosses y = b at the whole
       number dx*b/dy, and dx*b is past what a single holds exactly */
    static const long edge[][3] = {
        { 6993, 2457, 2405 },   /* crosses at 6845 */
        { 6955, 2461, 2415 },   /* crosses at 6825 */
        { 6943, 2489, 2451 },   /* crosses at 6837 */
        { 6847, 2505, 2475 },   /* crosses at 6765 */
        { 6913, 2453, 2431 },   /* crosses at 6851 */
        { 6875, 2475, 2457 }    /* crosses at 6825 */
    };
    int e, separating = 0, checked = 0, off = 0;

    for (e = 0; e < (int)(sizeof edge / sizeof *edge); e++)
    {
        long dx = edge[e][0], dy = edge[e][1], b = edge[e][2];
        long k = dx * b / dy;
        Xpost_Span_Vertex p[4];
        Collect got;
        int i, found = 0;

        if (dx * b % dy != 0)
        {
            check(0, "an edge in the table does not cross at a whole number");
            continue;
        }
        if (_single_crossing(dx, dy, b) != (double)k)
            separating++;

        p[0].x = 0.0;        p[0].y = 0.0;
        p[1].x = (double)dx; p[1].y = (double)dy;
        p[2].x = 0.0;        p[2].y = (double)dy;
        p[3].x = 0.0;        p[3].y = 0.0;

        got.v = ubuf;
        if (convert(p, 4, 0, NULL, &got))
        {
            check(0, "the width-bearing shape converted");
            continue;
        }
        /* band b-1 is cut at y = b, and the edge is the far end of the
           span the fill covers there */
        for (i = 0; i < got.n; i++)
        {
            if (got.v[i].band != (int)(b - 1))
                continue;
            if (got.v[i].hi < (double)(k - 1) || got.v[i].hi > (double)(k + 1))
                continue;
            found = 1;
            checked++;
            if (got.v[i].hi != (double)k)
            {
                off++;
                printf("span-window: edge (0,0)-(%ld,%ld) crosses y=%ld at"
                       " %.9g, not %ld\n", dx, dy, b, got.v[i].hi, k);
            }
        }
        if (!found)
            check(0, "the width-bearing edge was cut at its lattice band");
    }

    check(separating == (int)(sizeof edge / sizeof *edge),
          "every edge here is one a single precision crossing gets wrong");
    check(checked == (int)(sizeof edge / sizeof *edge),
          "every width-bearing edge was cut at its lattice band");
    check(off == 0,
          "every crossing past single precision landed on the whole number");
    printf("span-window: %d crossing(s) past single precision, %d off the"
           " whole number, %d that a single gets wrong\n",
           checked, off, separating);
}

int main(void)
{
    int grain, s;
    char name[64];

    if (!xpost_init())
    {
        report_failure("the library did not initialise");
        return verdict();
    }
    ubuf = malloc(MAXSPANS * sizeof *ubuf);
    wbuf = malloc(MAXSPANS * sizeof *wbuf);
    fbuf = malloc(MAXSPANS * sizeof *fbuf);
    if (!ubuf || !wbuf || !fbuf)
    {
        report_failure("the span buffers could not be allocated");
        xpost_quit();
        return verdict();
    }

    handmade();

    for (grain = 0; grain < 3; grain++)
    {
        for (s = 0; s < 1400; s++)
        {
            Xpost_Span_Vertex p[MAXPTS];
            integer n = 3 + (integer)(nextrand() % 9);
            integer i;

            for (i = 0; i < n; i++)
            {
                p[i].x = randcoord(grain, 200.0);
                p[i].y = randcoord(grain, 200.0);
            }
            /* a second subpath on half of them, so winding interaction is
               driven and not only single loops */
            if (nextrand() & 1 && n + 5 < MAXPTS)
            {
                integer m = 3 + (integer)(nextrand() % 4);
                p[n].x = BRK; p[n].y = 0; n++;
                for (i = 0; i < m; i++)
                {
                    p[n].x = randcoord(grain, 200.0);
                    p[n].y = randcoord(grain, 200.0);
                    n++;
                }
            }
            snprintf(name, sizeof name, "random grain %d shape %d", grain, s);
            drive(p, n, name);
        }
    }

    lattice_crossings();
    width_bearing_crossings();

    check(cases > 0, "the window was compared against the page at all");
    check(mismatches == 0, "every window kept what the page kept there");
    printf("span-window: %ld comparison(s), %ld mismatch(es)\n",
           cases, mismatches);

    free(ubuf); free(wbuf); free(fbuf);
    xpost_quit();
    return verdict();
}
