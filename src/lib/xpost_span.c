/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_span.c
 * @brief Spans: a row of pixels a paint covers, and the runs it breaks into.
 *
 * The scan converter's unit of work. A shape becomes spans and the spans are
 * what a device is asked to fill.
 */

/** \file xpost_span.c
   scan conversion: a boundary in, spans out
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h> /* memcpy: the merge settles into a scratch array */

#include "xpost_object.h"
#include "xpost_error.h"
#include "xpost_op_path.h" /* XPOST_PATH_BREAK */
#include "xpost_span.h"

/* marks a subpath separator in a vertex list */
#define SUBPATH_BREAK XPOST_PATH_BREAK

/* The one statement of the order (the record itself is stated in
   xpost_span.h): band, then left edge, then right edge, then direction.
   The qsort-shaped wrapper below and the merge in xpost_span_sort both
   read it from here, so there is no second copy to drift. */
static inline
int _band_span_order (const struct band_span *lt, const struct band_span *rt)
{
    if (lt->band != rt->band)
        return lt->band < rt->band ? -1 : 1;
    if (lt->lo != rt->lo)
        return lt->lo < rt->lo ? -1 : 1;
    if (lt->hi != rt->hi)
        return lt->hi < rt->hi ? -1 : 1;
    return lt->dirn - rt->dirn;
}

static
int _bandspancomp (const void *left, const void *right)
{
    return _band_span_order(left, right);
}

/* A shape's passages are sorted once per scan conversion, and a page of
   small shapes sorts thousands of times: the comparison is the whole
   cost, and a library sort takes it through a function pointer it
   cannot inline. So the merge is written out here with the comparison
   inlined, over a scratch array of the same length. Where that scratch
   cannot be had, the library sort does the same work through the same
   order, only slower.

   Merging never reorders a pair the order calls equal, and the order is
   total over the fields the walk reads, so the walk sees one arrangement
   whichever route sorted it. */
void
xpost_span_sort (struct band_span *spans, int n)
{
    struct band_span *tmp, *src, *dst, *swap;
    int width, i;

    if (n < 2)
        return;
    tmp = malloc((size_t)n * sizeof *tmp);
    if (!tmp)
    {
        qsort(spans, (size_t)n, sizeof *spans, _bandspancomp);
        return;
    }
    src = spans;
    dst = tmp;
    for (width = 1; width < n; width <<= 1)
    {
        for (i = 0; i < n; i += 2 * width)
        {
            int l = i;
            int m = i + width < n ? i + width : n;
            int r = i + 2 * width < n ? i + 2 * width : n;
            int j = m, k = l;

            while (l < m && j < r)
                dst[k++] = _band_span_order(&src[l], &src[j]) <= 0
                         ? src[l++] : src[j++];
            while (l < m)
                dst[k++] = src[l++];
            while (j < r)
                dst[k++] = src[j++];
        }
        swap = src; src = dst; dst = swap;
    }
    if (src != spans)
        memcpy(spans, src, (size_t)n * sizeof *spans);
    free(tmp);
}

/* Append a boundary passage, growing the array as needed; 0 on
   success.

   A passage in a band outside rows is not kept. Insideness is settled
   one band at a time -- a band's spans come from the passages through
   that band and from nothing else -- so keeping only the bands asked
   for settles those bands exactly as keeping every band would, and
   the ones dropped were about to be sorted, wound and thrown away. */
/* The band a coordinate lies in, as an int.

   A coordinate reaches the scan converter as whatever the program's
   arithmetic made of it, and real arithmetic reaches an infinity by
   overflowing and a not-a-number by taking one infinity from another.
   Converting either to an int is undefined, and so is converting a
   finite value past the width of one, so the value is brought inside
   the range first. The ends stand for "below every band" and "above
   every band", which the row window then drops as it drops any band it
   was not asked for; a not-a-number names no row at all and takes the
   same exit. */
static int
_band_of(double y)
{
    if (!(y >= (double)(INT_MIN + 1)))   /* also catches a not-a-number */
        return INT_MIN + 1;
    if (y > (double)(INT_MAX - 1))
        return INT_MAX - 1;
    return (int)floor(y);
}

static
int _span_push(struct band_span **spans, int *cap, int *n,
               const Xpost_Span_Rows *rows,
               int band, int dirn, double lo, double hi)
{
    if (rows && (band < rows->lo || band > rows->hi))
        return 0;

    if (*n == *cap)
    {
        struct band_span *tmp;
        /* Doubled in a width that holds the product: the count is an int,
           so a cap past INT_MAX/2 doubled in int wraps negative and the
           byte size sign-extends to a vast size_t -- a huge or refused
           allocation from what looks like a small one. Refuse the growth
           instead, as a VMerror, before either overflow. */
        size_t newcap = *cap ? (size_t)*cap * 2 : 64;

        if (newcap > (size_t)INT_MAX || newcap > (size_t)-1 / sizeof *tmp)
            return VMerror;
        tmp = realloc(*spans, newcap * sizeof *tmp);
        if (!tmp)
            return VMerror;
        *spans = tmp;
        *cap = (int)newcap;
    }
    (*spans)[*n].band = band;
    (*spans)[*n].dirn = dirn;
    (*spans)[*n].lo = lo;
    (*spans)[*n].hi = hi;
    ++*n;
    return 0;
}

/* Where an edge crosses a given y.
 *
 * Multiplied before it is divided, so that an edge through a lattice
 * point crosses there exactly. Dividing first turns the ratio into a
 * number that is usually not exact -- a third, say -- and multiplying
 * that back out lands a little either side of the whole number the
 * crossing is, which the floor and ceiling further on then read as one
 * more column than the shape covers.
 *
 * In double, and not in the interpreter's own number: where that is a
 * single, the crossing is a little past the boundary it should be on
 * and the two builds settle a differently shaped edge. A page does not
 * depend on how wide this build's numbers are.
 *
 * Written once and called from both the walk and the jump into a
 * window, because the two are required to arrive at the same number:
 * the window's correctness is stated as an equality against the
 * unwindowed walk, and two spellings of this that round apart would
 * break it while each stayed self-consistent.
 */
static double _edge_x_at(Xpost_Span_Vertex P, Xpost_Span_Vertex Q, double y)
{
    return P.x + (Q.x - P.x) * (y - P.y) / (Q.y - P.y);
}

/* Scan-convert a run of vertices to winding-resolved spans, stating
   each one to the consumer (the shared first half of the painting
   pipeline: vertices in, sorted boundary passages accumulated to filled
   extents out). A break entry ends one subpath and begins the next.

   evenodd selects the insideness rule of PLRM 4.5.2: 0 accumulates
   winding numbers to zero (the nonzero winding number rule, which fill
   and clip use), 1 counts boundary passages by parity (the even-odd
   rule, which eofill and eoclip use).

   rows, when given, is the inclusive band range to state spans for. The
   whole boundary is walked either way -- a chain's extent in one band
   is where the walk into the next one starts -- but nothing outside the
   range is kept, so the shape's parts above and below the range cost
   the walk and no more.

   The vertices are consumed -- the buffer is freed here whichever way
   the walk leaves. 0 on success; a consumer's refusal is returned
   unchanged and no further span is stated. */
int xpost_span_scanconvert(Xpost_Span_Vertex *points,
                           integer npoints,
                           int evenodd,
                           const Xpost_Span_Rows *rows,
                           Xpost_Span_Consumer *consumer)
{
    struct band_span *spans;
    int nspans, spancap;
    integer i;

    /* Scan-convert under the any-part-of-pixel rule (PLRM 7.5): a
       pixel is painted when the filled region meets its interior.
       Device space divides into unit pixel-row bands (row b covers
       b <= y < b+1). Each subpath boundary is cut into y-monotone
       chains -- walking from a least-y vertex, so a chain never wraps
       the start/end seam -- and each chain deposits, for every band it
       passes through, the x extent of its passage tagged with its y
       direction. Horizontal travel widens the open extent, except
       travel exactly on a band boundary, which meets no band interior
       (an integer-aligned bottom edge must not leak into the band
       below). Sorting each band's extents by left edge and
       accumulating winding numbers then yields the fill spans.

       A band the caller did not ask for keeps no extent: the walk still
       passes through it, since that is how it arrives at the bands
       below and above, but nothing it deposits there is sorted or
       wound. */
    spans = NULL;
    nspans = 0;
    spancap = 0;
    i = 0;
    for (;;)
    {
        integer s0, nv, base, k;
        int dirn, ib, code;
        double lo, hi, submin, submax;

        while (i < npoints && points[i].x == SUBPATH_BREAK)
            i++;
        if (i == npoints)
            break;
        s0 = i;
        while (i < npoints && points[i].x != SUBPATH_BREAK)
            i++;
        nv = i - s0;

        base = 0;
        for (k = 1; k < nv; k++)
            if (points[s0 + k].y < points[s0 + base].y)
                base = k;

        /* chain state: the open extent, its band, and its direction
           (0 until the first non-horizontal edge; starting at a
           least-y vertex the first direction can only be upward) */
        dirn = 0;
        ib = _band_of(points[s0 + base].y);
        lo = hi = points[s0 + base].x;
        submin = submax = lo;
        code = 0;

        for (k = 0; k < nv && code == 0; k++)
        {
            Xpost_Span_Vertex P = points[s0 + (base + k) % nv];
            Xpost_Span_Vertex Q = points[s0 + (base + k + 1) % nv];
            int d, eb;

            if (Q.x < submin) submin = Q.x;
            if (Q.x > submax) submax = Q.x;

            if (P.y == Q.y)
            {
                if (P.y == floor(P.y))
                {
                    /* on a band boundary: deposits nothing; until the
                       chain has a direction just track the position */
                    if (dirn == 0)
                        lo = hi = Q.x;
                }
                else
                {
                    if (Q.x < lo) lo = Q.x;
                    if (Q.x > hi) hi = Q.x;
                }
                continue;
            }

            d = Q.y > P.y ? 1 : -1;
            /* the band this edge starts in: a start exactly on a band
               boundary belongs to the band ahead of travel */
            eb = _band_of(P.y);
            if (d < 0 && (double)eb == P.y)
                eb--;

            if (d != dirn)
            {
                /* direction reversal: the vertex row holds two passages */
                if (dirn != 0)
                {
                    code = _span_push(&spans, &spancap, &nspans, rows,
                                      ib, dirn, lo, hi);
                    lo = hi = P.x;
                }
                dirn = d;
                ib = eb;
            }
            else if (eb != ib)
            {
                /* the previous edge ended exactly on our starting boundary */
                code = _span_push(&spans, &spancap, &nspans, rows,
                                  ib, dirn, lo, hi);
                lo = hi = P.x;
                ib = eb;
            }

            /* The bands before the window keep nothing, and stepping
               through them exists only to arrive at the first one that
               does. Where this edge reaches the window at all, go
               straight to the boundary it enters by.

               It arrives at the same numbers. Each step of the walk
               below leaves the next band's open extent at the crossing
               it just cut, and every crossing is interpolated from its
               own y alone -- not from the crossing before it -- so the
               extent this lands on is the extent the steps would have
               left. What the skipped steps would have deposited is what
               a band outside the window keeps, which is nothing. */
            if (rows)
            {
                int entry = ib;
                double ey = 0;

                if (d > 0 && ib < rows->lo)
                {
                    entry = rows->lo;
                    ey = (double)rows->lo;
                }
                else if (d < 0 && ib > rows->hi)
                {
                    entry = rows->hi;
                    ey = (double)(rows->hi + 1);
                }
                /* only where the edge actually crosses that boundary: one
                   that stops short of the window never enters it, and the
                   walk below has to end in the band it really stops in */
                if (entry != ib && (d > 0 ? Q.y > ey : Q.y < ey))
                {
                    lo = hi = _edge_x_at(P, Q, ey);
                    ib = entry;
                }
            }

            /* walk the edge band to band, cutting at each boundary */
            while (code == 0)
            {
                double yb;

                /* Past the window, and travelling away from it: every
                   band left keeps nothing, so finish the edge here
                   rather than cutting it at each boundary on the way to
                   its end.

                   What the walk would have left behind is a band number
                   outside the window and the extent within it. Neither
                   is read: the next edge overwrites the band with its
                   own starting one, and the only use of this one in
                   between is a deposit the window rejects. */
                if (rows && (d > 0 ? ib > rows->hi : ib < rows->lo))
                {
                    if (Q.x < lo) lo = Q.x;
                    if (Q.x > hi) hi = Q.x;
                    break;
                }

                yb = (double)(d > 0 ? ib + 1 : ib);

                if (d > 0 ? Q.y > yb : Q.y < yb)
                {
                    double xb = _edge_x_at(P, Q, yb);

                    if (xb < lo) lo = xb;
                    if (xb > hi) hi = xb;
                    code = _span_push(&spans, &spancap, &nspans, rows,
                                      ib, dirn, lo, hi);
                    ib += d;
                    lo = hi = xb;
                }
                else
                {
                    if (Q.x < lo) lo = Q.x;
                    if (Q.x > hi) hi = Q.x;
                    break;
                }
            }
        }

        if (code == 0)
        {
            if (dirn != 0)
                code = _span_push(&spans, &spancap, &nspans, rows,
                                  ib, dirn, lo, hi);
            else
            {
                /* no vertical travel at all: the subpath still meets its
                   row; deposit a balanced pair over its whole x extent */
                code = _span_push(&spans, &spancap, &nspans, rows,
                                  ib, 1, submin, submax);
                if (code == 0)
                    code = _span_push(&spans, &spancap, &nspans, rows,
                                      ib, -1, submin, submax);
            }
        }
        if (code)
        {
            free(points);
            free(spans);
            return code;
        }
    }
    free(points);

    /* nspans can be zero for a degenerate row, leaving spans NULL; passing a
       null pointer to qsort is undefined even for a zero count, and there is
       nothing to order below two spans anyway */
    if (nspans > 1)
        xpost_span_sort(spans, nspans);

    /* Walk each band accumulating winding: a span opens at the first
       extent's left edge and closes where the winding count returns to
       zero (or the band runs out), covering the rightmost extent seen.
       Every span the walk settles on passes through the consumer, and
       nothing here knows what becomes of it. Every one of them is in a
       band the caller asked for, no other band having kept an extent to
       settle a span out of. */
    {
        int s = 0;

        while (s < nspans)
        {
            int b = spans[s].band;
            int wind = 0;
            double L = spans[s].lo, R = spans[s].hi;
            int code;

            do
            {
                if (spans[s].hi > R)
                    R = spans[s].hi;
                wind += spans[s].dirn;
                s++;
            } while ((evenodd ? (wind & 1) : wind) != 0
                     && s < nspans && spans[s].band == b);

            code = consumer->take(consumer, b, L, R);
            if (code)
            {
                free(spans);
                return code;
            }
        }
    }
    free(spans);

    return 0;
}
