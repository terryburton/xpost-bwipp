/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_span.h
 * @brief Declares the span operations.
 *
 * The implementation is in the .c beside this.
 */

#ifndef XPOST_SPAN_H
#define XPOST_SPAN_H

#include "xpost_object.h" /* real, integer */
#include "xpost_private.h" /* XPOST_TEST_VISIBLE */

/*
 * Scan conversion, and the seam between stating a span and taking one.
 *
 * A painting operation reaches the raster in two halves that have no
 * business knowing each other. One half turns a shape into spans: it
 * reads vertices, cuts the boundary into y-monotone chains, sorts each
 * pixel row's boundary passages and applies an insideness rule to them
 * (PLRM 4.5.2 states the two rules; PLRM 7.5.1 states which pixels the
 * result covers). The other half disposes of a span: it may write the
 * pixels, or build a PostScript description of them, or count them.
 *
 * Everything in this file belongs to the first half. What a span is
 * written into is stated by an Xpost_Span_Consumer the caller supplies,
 * and the scan conversion knows nothing else about it. Keeping the two
 * apart is what lets a span be taken somewhere other than the page --
 * held, counted, or replayed -- without the geometry being touched.
 *
 * A span is stated in the coordinates of the page, in the pixel-row
 * band it lies in (band b is the row covering b <= y < b+1) and as the
 * x extent the region covers within that band.
 *
 * That extent is a double and not a `real`, which is the interpreter's
 * number and is as wide as the object is. Where an edge crosses a pixel
 * boundary exactly -- which is every edge through a lattice point, so
 * constantly -- the crossing is that boundary in double and a little
 * past it in single, and the column it lands in is then decided
 * differently by each. The page a program describes does not depend on
 * how wide this build's numbers are, so the arithmetic that resolves it
 * does not either. Turning the extent
 * into columns, and deciding whether those columns are addressable at
 * all, is the consumer's business: what the page names and what a
 * destination holds are separate questions, and only the consumer knows
 * the second one.
 */

/* One boundary-chain passage through a pixel-row band: the x extent
   [lo, hi] the chain covers within the band (row b covers device
   b <= y < b+1) and the chain's y direction (+1 rising, -1 falling).
   Stated here so that the order the conversion sorts passages into can
   be held by a test; nothing outside the conversion builds one. */
struct band_span
{
    int band;
    int dirn;
    double lo, hi;
};

/* Order a shape's passages by band, then left edge, then right edge,
   then direction -- the order the insideness walk below requires. The
   walk reads its input a band at a time and accumulates winding left to
   right, so any array in this order is settled identically. */
XPOST_TEST_VISIBLE void xpost_span_sort(struct band_span *spans, int n);

/* One vertex of the boundary being converted, in device space.
   XPOST_PATH_BREAK in x marks a subpath separator rather than a
   vertex: it ends one subpath and begins the next. */
typedef struct
{
    double x, y;
} Xpost_Span_Vertex;

typedef struct _Xpost_Span_Consumer Xpost_Span_Consumer;

/* What becomes of each span the scan conversion states. `take` is
   called once per span, in increasing band order, with the band and the
   real x extent [lo, hi] the region covers within it. It returns 0 to
   go on, or the error to raise, which the conversion returns unchanged
   without stating any further span.

   A consumer is embedded as the first member of whatever state it
   needs, and recovered from the pointer by the callback. */
struct _Xpost_Span_Consumer
{
    int (*take)(Xpost_Span_Consumer *consumer, int band, double lo, double hi);
};

/* The rows of the page a conversion states spans for: an inclusive
   band range. The whole boundary of a shape reaching outside it is
   walked -- the insideness rule needs the whole boundary to be right
   about any part of it, and a chain's extent in one band is where the
   walk into the next one starts -- but only the bands within the range
   have their passages kept, sorted and wound. So a shape crossing the
   range costs the walk over its whole height and the spans of the
   range alone. */
typedef struct
{
    int lo, hi;
} Xpost_Span_Rows;

int xpost_span_scanconvert(Xpost_Span_Vertex *vertices,
                           integer nvertices,
                           int evenodd,
                           const Xpost_Span_Rows *rows,
                           Xpost_Span_Consumer *consumer);

#endif
