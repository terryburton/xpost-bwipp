/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_generic.c
 * @brief The device every other device inherits from.
 *
 * It owns the page as an array of pixels and implements every method in
 * terms of that, so a device that has pixels need implement almost nothing.
 * A device without them -- one that records or writes a file -- has to
 * override each method that would reach for them.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h> /* snprintf */
#include <limits.h>
#include <stdint.h> /* SIZE_MAX: the width a buffer position is counted in */
#include <stdlib.h> /* abs */

#include <math.h>
#include <string.h>

#ifdef HAVE_LIBPNG
# include <png.h>
#endif

#ifdef HAVE_ZLIB
# include <zlib.h>
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h" /* access memory */
#include "xpost_object.h" /* work with objects */
#include "xpost_stack.h"  /* push results on stack */
#include "xpost_context.h" /* state */
#include "xpost_error.h"
#include "xpost_dict.h" /* get/put values in dicts */
#include "xpost_string.h" /* get/put values in strings */
#include "xpost_array.h"
#include "xpost_save.h" /* the copy a write takes for the save in force */
#include "xpost_name.h" /* create names */
#include "xpost_file.h" /* raster emission */

#include "xpost_operator.h" /* create operators */
#include "xpost_op_dict.h" /* call xpost_op_any_load operator for convenience */
#include "xpost_op_path.h" /* read a path's fill vertices */
#include "xpost_op_font.h" /* the text route's memo of this clip, dropped with ours */
#include "xpost_dev_driver.h" /* device contract and shared helpers */
#include "xpost_span.h" /* scan conversion, and what takes its spans */
#include "xpost_dev_generic.h" /* check prototypes */
#include "xpost_strbuf.h" /* the growable byte buffer the content accumulates in */

/* marks a subpath separator in a vertex list */
#define SUBPATH_BREAK XPOST_PATH_BREAK

/* Set around one sort and cleared after it, which is what lets a single
   comparison function reach the context. Declared with the rest of the
   library's statics in tests/library_statics.golden, which
   check-library-lifetime holds to naming every one and what resets it. */
static Xpost_Context *localctx;

static Xpost_Object namedotcopydict;
static Xpost_Object namedotinstance;
static Xpost_Object namedotstate;
static Xpost_Object namenativecolorspace;
static Xpost_Object nameDeviceGray;
static Xpost_Object nameDeviceRGB;
static Xpost_Object nameroll;
static Xpost_Object nameDrawLine;
static Xpost_Object nameexec;
static Xpost_Object namerepeat;
static Xpost_Object namecvx;
static Xpost_Object nameRbracket;
static Xpost_Object nameImgData;
static Xpost_Object nameFillRect;
static Xpost_Object namepdfPrivate;
static Xpost_Object namedotground;
static Xpost_Object namedotbandtop;
static Xpost_Object namedotbandrows;
static Xpost_Object namedothtcell;
static Xpost_Object namedothtw;
static Xpost_Object namedothth;

/* --- what every device is given --------------------------------------
   The raster's size and its block, the ground it clears to, the file a
   page goes out through. A device class in PostScript reaches all of it
   through these, so a class does not have to know how any of it is done. */

int xpost_device_raster_bytes(int w, int h, size_t pixel, size_t reserve,
                              size_t *bytes)
{
    size_t pixels;

    /* A buffer with no extent has no pixels rather than too many: it
       comes to no bytes and is built, and whatever the device does with
       an empty page it does on its own terms. A negative extent is not
       an extent at all, and the position of a pixel in one is not a
       number the raster is indexed by. */
    if (w < 0 || h < 0)
        return 0;
    if (w == 0 || h == 0)
    {
        *bytes = reserve;
        return 1;
    }

    /* A device reaches a pixel by its position within the raster, and
       that position is counted in the width the platform expresses the
       size of a block of memory in. Three quantities have to fit that
       width for the block to be one a device can hold and reach the far
       end of: how many pixels there are, what they come to in bytes, and
       what the caller will ask the allocator for. Each is checked by
       division against what is left, so nothing is multiplied before it
       is known to fit.

       A block that fits is not a block the system will give, and the
       refusal there is the allocator's to make. What is refused here is
       the block whose far end has no address on this platform, which is
       a limit of the implementation rather than of the machine and which
       no amount of memory would lift. Refusing before allocating also
       keeps a page nobody can reach from asking the system for the
       memory to hold it. */
    if ((size_t)w > SIZE_MAX / (size_t)h)
        return 0;
    pixels = (size_t)w * (size_t)h;

    /* the bytes those pixels come to are a separate question from how
       many of them there are, and the space the caller keeps in front of
       the raster is a third: a raster that fits on its own and not with
       its header in front of it is one whose allocation size the caller
       would form by wrapping */
    if (pixel && pixels > (SIZE_MAX - reserve) / pixel)
        return 0;

    *bytes = pixels * pixel + reserve;
    return 1;
}

/* The block a raster of @p bytes sits in, or NULL for a count no
   allocator hands out. A size expresses a raster that a machine does not
   hold: half the address space is more than any of them gives to one
   page, and a request that large is answered here rather than put to an
   allocator, so that the refusal names the page it came from. */
void *xpost_device_raster_block(size_t bytes)
{
    if (bytes > SIZE_MAX / 2)
        return NULL;
    return malloc(bytes);
}

/* What an erased pixel of this device holds, in channels of the
   caller's scale. The wrapper below asks at the scale a byte channel
   uses, which is what most callers want. */
void xpost_device_ground_scaled(Xpost_Context *ctx, Xpost_Object devdic,
                                double scale, int *r, int *g, int *b)
{
    Xpost_Object ground;

    /* A device that has not been erased has no ground recorded, and its
       page stands as its Create left it: every device here fills a
       fresh buffer white, so that is what a read off such a page owes,
       which at this device's scale is the top of the channel range. The
       same answer covers a device whose instance holds something other
       than the record under that name, the instance dictionary being an
       ordinary dictionary. */
    *r = *g = *b = (int)scale;

    ground = xpost_dict_get(ctx, devdic, namedotground);
    if (xpost_object_get_type(ground) != arraytype || ground.comp_.sz < 3)
        return;

    /* the components are in the range a colour operand arrives in, and
       fold to a channel through the contract's fold at the scale the
       caller stores its channels in, the same fold this device's PutPix
       and FillRect put a painted colour through: the ground a read
       answers is the value an erased pixel holds */
    *r = xpost_dev_num_to_scaled(xpost_array_get(ctx, ground, 0), scale);
    *g = xpost_dev_num_to_scaled(xpost_array_get(ctx, ground, 1), scale);
    *b = xpost_dev_num_to_scaled(xpost_array_get(ctx, ground, 2), scale);
}

void xpost_device_ground_channels(Xpost_Context *ctx, Xpost_Object devdic,
                                  int *r, int *g, int *b)
{
    xpost_device_ground_scaled(ctx, devdic, 255.0, r, g, b);
}

/* Gives back a page a run handed to the client through a buffer
   variable. Safe to call twice, and on a variable no page was ever
   stored through. */
XPAPI void xpost_output_buffer_release(unsigned char **buffer)
{
    void *block;

    /* Nothing to give back: no variable, or a variable no run has
       stored a page through -- which is what a variable that has been
       released already holds, since releasing clears it. */
    if (!buffer || !*buffer)
        return;

    /* what the client holds is the raster; what was allocated is the
       block around it, whose address the device left in front of it */
    block = xpost_dev_output_buffer_block(*buffer);
    free(block);

    /* the client's pointer named the block until this call, and names
       nothing after it */
    *buffer = NULL;
}

/* Opens the file this page is to be written to, or the standard output
   where that is what was named. The caller closes it through the call
   below rather than directly, a standard stream not being the device's
   to close. */
FILE *xpost_device_page_open(Xpost_Context *ctx, Xpost_Object devdic)
{
    Xpost_Object namestr;
    char *filename;
    FILE *f;
    int err;

    /* The name settled for the page being written, which the page
       machinery puts in the device's state before it runs Emit
       (.transmitpage, data/device.ps). It is the name and not the
       template: the template may carry a page-number marker, and the
       number that replaces it is the page's to know, not the device's.
       A device that read the template instead wrote every page of a job
       to one name. */
    namestr = xpost_dict_get(ctx,
                             xpost_dict_get(ctx, devdic, namedotstate),
                             xpost_name_cons(ctx, ".outputfile"));
    if (xpost_object_get_type(namestr) != stringtype)
        return NULL;

    /* the standard output is where a page goes when nobody said where,
       spelled as the file operator spells it */
    if (namestr.comp_.sz == XPOST_DEV_STDOUT_LEN
        && memcmp(xpost_string_get_pointer(ctx, namestr),
                  XPOST_DEV_STDOUT_NAME, XPOST_DEV_STDOUT_LEN) == 0)
        return stdout;

    filename = malloc(namestr.comp_.sz + 1);
    if (!filename)
        return NULL;
    memcpy(filename, xpost_string_get_pointer(ctx, namestr), namestr.comp_.sz);
    filename[namestr.comp_.sz] = '\0';

    f = xpost_diskfile_fopen(filename, "wb", 0, &err);
    free(filename);

    return f;
}

void xpost_device_page_close(FILE *f)
{
    if (!f)
        return;
    /* a standard stream outlives the page written through it: it is
       flushed so the page is whole where it went, and left open */
    if (f == stdout || f == stderr)
        fflush(f);
    else
        fclose(f);
}

/* --- resolving a mark into spans -------------------------------------
   Scan conversion, and the only place it happens. A polygon, a path or a
   run of points is reduced here to the runs of pixels it covers, sorted so
   a fill costs the total height of its edges rather than edges times the
   height of the page. */

static
int _yxcomp(const void *left, const void *right)
{
    const Xpost_Object *lt = left;
    const Xpost_Object *rt = right;
    Xpost_Object leftx, lefty, rightx, righty;
    integer ltx, lty, rtx, rty;

    leftx = xpost_array_get(localctx, *lt, 0);
    lefty = xpost_array_get(localctx, *lt, 1);
    rightx = xpost_array_get(localctx, *rt, 0);
    righty = xpost_array_get(localctx, *rt, 1);
    ltx = xpost_object_get_type(leftx) == realtype ?
        (integer)leftx.real_.val : leftx.int_.val;
    lty = xpost_object_get_type(lefty) == realtype ?
        (integer)lefty.real_.val : lefty.int_.val;
    rtx = xpost_object_get_type(rightx) == realtype ?
        (integer)rightx.real_.val : rightx.int_.val;
    rty = xpost_object_get_type(righty) == realtype ?
        (integer)righty.real_.val : righty.int_.val;
    if (lty == rty)
    {
        if (ltx < rtx)
        {
            return 1;
        }
        else if (ltx > rtx)
        {
            return -1;
        } else
        {
            return 0;
        }
    }
    else
    {
        if (lty < rty)
            return -1;
        else
            return 1;
    }
}

/* Sorts an array of points into the order the scan conversion below
   walks them: down the page, and left to right within a row. */
static
int _yxsort (Xpost_Context *ctx, Xpost_Object arr)
{
    unsigned char *arrcontents;
    unsigned int arradr;
    Xpost_Memory_File *mem;

    mem = xpost_context_select_memory(ctx, arr);
    if (!xpost_memory_table_get_addr(mem, xpost_object_get_ent(arr), &arradr))
        return VMerror;
    arrcontents = xpost_vm_ptr(mem, arradr);

    localctx = ctx;
    qsort(arrcontents, arr.comp_.sz, sizeof(arr), _yxcomp);
    localctx = NULL;

    return 0;
}

/* a winding-resolved fill span: the x extent the region covers within
   one pixel-row band, still in device coordinates */
struct rspan
{
    int band;
    double lo, hi;
};

static
int _rspan_push(struct rspan **rsp, int *cap, int *n,
                int band, double lo, double hi)
{
    if (*n == *cap)
    {
        struct rspan *tmp;
        int newcap = *cap ? *cap * 2 : 64;

        tmp = realloc(*rsp, newcap * sizeof *tmp);
        if (!tmp)
            return VMerror;
        *rsp = tmp;
        *cap = newcap;
    }
    (*rsp)[*n].band = band;
    (*rsp)[*n].lo = lo;
    (*rsp)[*n].hi = hi;
    ++*n;
    return 0;
}

/* The consumer that keeps the spans instead of painting them: an
   answer about a region, for the callers that state one as PostScript
   or meet one against another. */
struct _rspan_collector
{
    Xpost_Span_Consumer consumer;
    struct rspan *rsp;
    int n, cap;
};

static
int _rspan_collect(Xpost_Span_Consumer *c, int band, double lo, double hi)
{
    struct _rspan_collector *k = (struct _rspan_collector *)c;

    return _rspan_push(&k->rsp, &k->cap, &k->n, band, lo, hi);
}

/* Scan-convert a run of vertices, keeping the winding-resolved band
   spans rather than painting them. A break entry ends one subpath and
   begins the next; evenodd selects the insideness rule (PLRM 4.5.2).
   rows, when given, is the run of the page's rows to keep spans for,
   and a caller wanting the whole shape gives none.
   The vertices are consumed -- the buffer is freed whichever way the
   walk leaves -- and the caller owns the returned spans. 0 on
   success. */
static
int _points_resolved_spans(Xpost_Span_Vertex *points,
                           integer npoints,
                           const Xpost_Span_Rows *rows,
                           struct rspan **out,
                           int *nout,
                           int evenodd)
{
    struct _rspan_collector k;
    int code;

    *out = NULL;
    *nout = 0;

    k.consumer.take = _rspan_collect;
    k.rsp = NULL;
    k.n = k.cap = 0;

    code = xpost_span_scanconvert(points, npoints, evenodd, rows, &k.consumer);
    if (code)
    {
        free(k.rsp);
        return code;
    }

    *out = k.rsp;
    *nout = k.n;
    return 0;
}

/* Read a run of coordinates into a vertex run the scan conversion
   takes: a pair per vertex, a break written as the pair the packed path
   writes a subpath break as. This is the boundary as everything that
   holds one holds it -- the packed path, and a record of a page's marks
   -- so it is the form a caller reaches the conversion through without
   building anything to carry it. The caller owns the returned buffer.
   0 on success. */
static
int _co_vertices(const real *co,
                 integer npts,
                 Xpost_Span_Vertex **out)
{
    Xpost_Span_Vertex *points;
    integer i;

    *out = NULL;

    points = malloc((size_t)npts * sizeof *points);
    if (!points)
        return VMerror;
    for (i = 0; i < npts; i++)
    {
        if (co[2 * i] == SUBPATH_BREAK)
        {
            points[i].x = SUBPATH_BREAK;
            points[i].y = SUBPATH_BREAK;
            continue;
        }
        /* quantize to the 1/256 pixel device grid, the same one the
           contract's line walk uses: geometry meant to lie on a pixel
           boundary arrives with accumulated float noise, and unsnapped
           it would classify to the wrong side of the boundary */
        points[i].x = (real)xpost_dev_line_quantize(co[2 * i]);
        points[i].y = (real)xpost_dev_line_quantize(co[2 * i + 1]);
    }

    *out = points;
    return 0;
}

/* Read a null-separated polygon array into a vertex run the scan
   conversion takes: the array form of the boundary, where a null
   element separates one subpath from the next. One vertex per array
   element, so the run is as long as the array. The caller owns the
   returned buffer. 0 on success. */
static
int _poly_vertices(Xpost_Context *ctx,
                   Xpost_Object poly,
                   Xpost_Span_Vertex **out)
{
    Xpost_Span_Vertex *points;
    integer i;

    *out = NULL;

    points = malloc(poly.comp_.sz * sizeof *points);
    if (!points)
        return VMerror;
    /* the vertex count is widened into the signed type the walk indexes
       with, so the subpath cursor below stays signed throughout: it is
       differenced against a subpath's first index to count vertices */
    for (i = 0; i < (integer)poly.comp_.sz; i++)
    {
        Xpost_Object pair, x, y;

        pair = xpost_array_get(ctx, poly, i);
        if (xpost_object_get_type(pair) != arraytype)
        {
            points[i].x = SUBPATH_BREAK;
            points[i].y = SUBPATH_BREAK;
            continue;
        }
        x = xpost_array_get(ctx, pair, 0);
        y = xpost_array_get(ctx, pair, 1);
        if (xpost_object_get_type(x) == integertype)
            x = xpost_real_cons((real)x.int_.val);
        if (xpost_object_get_type(y) == integertype)
            y = xpost_real_cons((real)y.int_.val);
        /* quantize to the 1/256 pixel device grid, the same one the
           contract's line walk uses: geometry meant to lie on a pixel
           boundary arrives with accumulated float noise, and unsnapped
           it would classify to the wrong side of the boundary */
        points[i].x = (real)xpost_dev_line_quantize(x.real_.val);
        points[i].y = (real)xpost_dev_line_quantize(y.real_.val);
    }

    *out = points;
    return 0;
}

/* The same polygon, scan-converted to winding-resolved band spans the
   caller owns. The whole of it: a region is an answer about a shape and
   not about a device's rows, so nothing here is windowed. 0 on
   success. */
static
int _poly_resolved_spans(Xpost_Context *ctx,
                         Xpost_Object poly,
                         struct rspan **out,
                         int *nout,
                         int evenodd,
                         const Xpost_Span_Rows *rows)
{
    Xpost_Span_Vertex *points;
    int code;

    *out = NULL;
    *nout = 0;

    code = _poly_vertices(ctx, poly, &points);
    if (code)
        return code;

    return _points_resolved_spans(points, (integer)poly.comp_.sz, rows,
                                  out, nout, evenodd);
}

/* Scan-convert a path to winding-resolved band spans, reading its
   vertices straight out of the packed path rather than through an
   array of them. This is how a clipping region is resolved: a region
   cut from another is one pixel-band rectangle per band it covers, and
   a page divided finely across the rows has more of those than any
   single array describes -- but a path holds its extent in a header
   field of its own and has no such bound. The caller owns the returned
   buffer. 0 on success. */
static
int _path_resolved_spans(Xpost_Context *ctx,
                         Xpost_Object path,
                         struct rspan **out,
                         int *nout,
                         int evenodd,
                         const Xpost_Span_Rows *rows)
{
    Xpost_Span_Vertex *points;
    real *co;
    int npts, code;

    *out = NULL;
    *nout = 0;

    code = xpost_path_fill_points(ctx, path, &co, &npts);
    if (code)
        return code;
    if (npts == 0)
        return 0;
    code = _co_vertices(co, (integer)npts, &points);
    free(co);
    if (code)
        return code;

    return _points_resolved_spans(points, (integer)npts, rows,
                                  out, nout, evenodd);
}

/* The run of the page's rows a device takes marks for, as the band
 * range a fill of it states spans for. 1 where the device says which
 * rows those are, 0 where it does not -- and a caller given no range
 * takes the spans of the whole shape, which is what a device holding
 * its page some other way wants.
 *
 * A raster device stands on one run of the page's rows at a time and
 * shows the page's ground over the rest: a mark landing on a row the
 * device does not hold is dropped against the row it was about to be
 * written to (data/image.ps), so the pixels are the same whether that
 * span is formed or not, and the range is what keeps it from being
 * formed. Which matters most where a page is put out a band at a time,
 * since every mark crossing more than one band is played into each of
 * the bands it crosses.
 *
 * The run is read off the device under the names a raster device holds
 * it by, and the device is the only authority on it: a device is free
 * to move which rows it stands on between one mark and the next.
 */
static int _device_rows(Xpost_Context *ctx, Xpost_Object devdic,
                        Xpost_Span_Rows *rows)
{
    Xpost_Object top, nrows;
    integer lo, n;

    /* The rows a device holds are its own where it was built to hold
       them, and its state's where they were written as it painted: what
       a page does to a device is kept beside it (data/device.ps). */
    top = xpost_dict_get(ctx, devdic, namedotbandtop);
    nrows = xpost_dict_get(ctx, devdic, namedotbandrows);
    if (xpost_object_get_type(nrows) != integertype)
    {
        Xpost_Object st = xpost_dict_get(ctx, devdic, namedotstate);

        if (xpost_object_get_type(st) == dicttype)
            nrows = xpost_dict_get(ctx, st, namedotbandrows);
    }
    if (xpost_object_get_type(top) != integertype
        || xpost_object_get_type(nrows) != integertype)
        return 0;

    lo = top.int_.val;
    n = nrows.int_.val;
    /* A run of no rows takes no mark and is not a range; nor is one
       whose end lies past the rows a band range counts in. Either way
       the device has said nothing a fill can be held to, and the shape
       is converted whole. */
    if (n < 1 || lo < 0 || n - 1 > (integer)INT_MAX - lo)
        return 0;

    rows->lo = (int)lo;
    rows->hi = (int)(lo + n - 1);
    return 1;
}

/* The consumer that paints a span where it lands: the device's own
   rectangle fill, called for each span as the scan conversion states it.
 *
 * A span is stated in the page's rows. What the device holds is a
 * raster of its own, whose first row is the page row named by firstrow;
 * every raster device presents the whole page and so begins at its
 * first row, and the difference is where a raster holding some other
 * run of the page's rows would enter. The columns are the device's own
 * business: it clips them against the row it is about to write, which is
 * the only place the width of that row is known.
 *
 * The device's method is reached as an operator call rather than by
 * scheduling PostScript to run once per span. Both arrive at the same
 * method with the same operands, and what the method does with them is
 * not this consumer's affair. */
struct _rect_painter
{
    Xpost_Span_Consumer consumer;
    Xpost_Context *ctx;
    Xpost_Object devdic;
    Xpost_Object comp[3];
    int ncomp;
    unsigned int fillrect;
    int firstrow;
};

/* The span consumer that paints. It turns each resolved span into the
   driver's FillRect over the columns the span reaches. */
/* A span end as a column, brought inside the width of the integer it is
   converted to. A coordinate arrives as whatever the program's
   arithmetic made of it, and converting an infinity, a not-a-number or a
   value past that width is undefined; the ends stand for "left of every
   column" and "right of every one", and a not-a-number takes the low
   end, so a span carrying one closes on the empty test below. */
static integer
_span_column(double v, int up)
{
    if (!(v >= (double)(INT_MIN + 1)))   /* also catches a not-a-number */
        return (integer)(INT_MIN + 1);
    if (v > (double)(INT_MAX - 1))
        return (integer)(INT_MAX - 1);
    return (integer)(up ? ceil(v) : floor(v));
}

static
int _rect_paint(Xpost_Span_Consumer *c, int band, double lo, double hi)
{
    struct _rect_painter *p = (struct _rect_painter *)c;
    integer xlo = _span_column(lo, 0);
    integer xhi = _span_column(hi, 1);
    int i;

    /* Paint columns [floor(lo), ceil(hi)): every pixel whose interior
       the span reaches, and exactly the geometry when the span lies on
       pixel boundaries (PLRM 7.5.1). FillRect fills the inclusive box
       [x, x+w] on row y under the driver contract, so a fill span is
       w = xhi-xlo-1 and h = 0. */
    if (xhi <= xlo)
        return 0;

    for (i = 0; i < p->ncomp; i++)
        xpost_stack_push(p->ctx->lo, p->ctx->os, p->comp[i]);
    xpost_stack_push(p->ctx->lo, p->ctx->os, xpost_int_cons(xlo));
    xpost_stack_push(p->ctx->lo, p->ctx->os,
                     xpost_int_cons(band - p->firstrow));
    xpost_stack_push(p->ctx->lo, p->ctx->os, xpost_int_cons(xhi - xlo - 1));
    xpost_stack_push(p->ctx->lo, p->ctx->os, xpost_int_cons(0)); /* h */
    xpost_stack_push(p->ctx->lo, p->ctx->os, p->devdic);

    return xpost_operator_exec(p->ctx, p->fillrect);
}

/* --- a triangle whose colour varies across it ------------------------

   A smooth shading resolves to triangles, and how one is painted
   decides what a gradient costs. Halving a triangle until each piece is
   flat enough to fill in a single colour spends a fill on every half
   pixel: MEASURED on a five-row lattice mesh, 524288 flat fills for a
   page of 147456 pixels -- three and a half fills for every pixel on it
   -- and two thirds of the time went on the halving rather than on any
   painting.

   A triangle's colour is affine across it: three corners fix a plane
   for each component, and the value at a pixel is that plane read
   there. So it need not be approached by halving at all. The triangle
   is scan-converted once, and each row painted left to right in runs of
   pixels whose colour rounds alike.

   THE RUN IS WHERE THE APPROXIMATION NOW LIVES, and it is a finer one
   than the halving it replaces: a run holds only while every component
   stays inside a 255th, where the halving's own flatness test accepts a
   spread of 0.015 -- a 66th -- before filling a piece in one colour.
   The colour written for a run is the exact value at its first pixel,
   not the rounded one the run was grouped by.

   Declines, leaving the caller to paint it the old way, wherever it
   cannot answer: a device whose rectangle fill is a procedure rather
   than an operator (what runs a procedure is the interpreter, which
   cannot be called from here), or a device painting in a space this
   does not count components for. */
#define GOURAUD_MAXCOMP 3

struct _gouraud_painter
{
    Xpost_Span_Consumer consumer;
    Xpost_Context *ctx;
    Xpost_Object devdic;
    unsigned int fillrect;
    int ncomp;
    int firstrow;
    /* each component across the plane: a * x + b * y + d */
    double a[GOURAUD_MAXCOMP];
    double b[GOURAUD_MAXCOMP];
    double d[GOURAUD_MAXCOMP];
};

/* One run of a row, in the colour it was grouped at. FillRect fills the
   inclusive box [x, x+w] on row y, so a run of columns [x0, x1) is
   w = x1 - x0 - 1 and h = 0 -- the same shape the flat span fill above
   uses, for the same reason. */
static int _gouraud_run(struct _gouraud_painter *p, int band,
                        integer x0, integer x1, const double *v)
{
    int k;

    if (x1 <= x0)
        return 0;
    for (k = 0; k < p->ncomp; k++)
        xpost_stack_push(p->ctx->lo, p->ctx->os, xpost_real_cons((real)v[k]));
    xpost_stack_push(p->ctx->lo, p->ctx->os, xpost_int_cons(x0));
    xpost_stack_push(p->ctx->lo, p->ctx->os,
                     xpost_int_cons(band - p->firstrow));
    xpost_stack_push(p->ctx->lo, p->ctx->os, xpost_int_cons(x1 - x0 - 1));
    xpost_stack_push(p->ctx->lo, p->ctx->os, xpost_int_cons(0));
    xpost_stack_push(p->ctx->lo, p->ctx->os, p->devdic);
    return xpost_operator_exec(p->ctx, p->fillrect);
}

static int _gouraud_take(Xpost_Span_Consumer *c, int band, double lo, double hi)
{
    struct _gouraud_painter *p = (struct _gouraud_painter *)c;
    integer xlo = (integer)floor(lo);
    integer xhi = (integer)ceil(hi);
    double y = (double)band + 0.5;
    double val[GOURAUD_MAXCOMP];
    double run[GOURAUD_MAXCOMP];
    int q[GOURAUD_MAXCOMP];
    int qrun[GOURAUD_MAXCOMP];
    integer x, start;
    int k, ret;

    /* the columns whose interior the span reaches, as the flat fill
       takes them */
    if (xhi <= xlo)
        return 0;
    start = xlo;
    for (k = 0; k < GOURAUD_MAXCOMP; k++)
    {
        run[k] = 0.0;
        qrun[k] = 0;
    }
    for (x = xlo; x < xhi; x++)
    {
        double cx = (double)x + 0.5;

        for (k = 0; k < p->ncomp; k++)
        {
            double v = p->a[k] * cx + p->b[k] * y + p->d[k];

            /* a corner colour is a colour, but the plane through three
               of them runs past the range outside the triangle, and a
               pixel on the boundary can sit a rounding step outside it.
               Written so that anything which is not a number lands at
               the bottom of the range rather than reaching the
               conversion below, which has no integer to convert it to */
            if (!(v > 0.0)) v = 0.0;
            else if (v > 1.0) v = 1.0;
            val[k] = v;
            q[k] = (int)(v * 255.0 + 0.5);
        }
        if (x == xlo)
        {
            for (k = 0; k < p->ncomp; k++)
            {
                run[k] = val[k];
                qrun[k] = q[k];
            }
            continue;
        }
        for (k = 0; k < p->ncomp; k++)
            if (q[k] != qrun[k])
                break;
        if (k < p->ncomp)
        {
            ret = _gouraud_run(p, band, start, x, run);
            if (ret)
                return ret;
            start = x;
            for (k = 0; k < p->ncomp; k++)
            {
                run[k] = val[k];
                qrun[k] = q[k];
            }
        }
    }
    return _gouraud_run(p, band, start, xhi, run);
}

int xpost_dev_gouraud_paint(Xpost_Context *ctx, Xpost_Object devdic,
                            const double *pt, const double *col, int ncomp,
                            int *painted)
{
    struct _gouraud_painter p;
    Xpost_Object colorspace, fillrect;
    Xpost_Span_Rows rows;
    const Xpost_Span_Rows *window;
    Xpost_Span_Vertex *v;
    double det;
    int k;

    *painted = 0;
    if (ncomp < 1 || ncomp > GOURAUD_MAXCOMP)
        return 0;
    colorspace = xpost_dict_get(ctx, devdic, namenativecolorspace);
    if (xpost_dict_compare_objects(ctx, colorspace, nameDeviceGray) == 0)
    {
        if (ncomp != 1)
            return 0;
    }
    else if (xpost_dict_compare_objects(ctx, colorspace, nameDeviceRGB) == 0)
    {
        if (ncomp != 3)
            return 0;
    }
    else
        return 0;

    fillrect = xpost_dict_get(ctx, devdic, nameFillRect);
    if (xpost_object_get_type(fillrect) != operatortype)
        return 0;

    det = (pt[2] - pt[0]) * (pt[5] - pt[1])
        - (pt[4] - pt[0]) * (pt[3] - pt[1]);
    for (k = 0; k < ncomp; k++)
    {
        double c0 = col[k];
        double c1 = col[ncomp + k];
        double c2 = col[2 * ncomp + k];

        /* Three corners on a line fix no plane. Such a triangle encloses
           no area and the conversion states no span for it, but the
           coefficients are computed before that is known, and a
           division by nothing would put a NaN in them. The test is
           written to keep anything that is not a number here too: a
           vertex far enough out that the products overflow leaves the
           determinant not-a-number, which answers false to every
           comparison and would otherwise be divided by. */
        if (!(det > 1e-12 || det < -1e-12))
        {
            p.a[k] = 0.0;
            p.b[k] = 0.0;
            p.d[k] = (c0 + c1 + c2) / 3.0;
        }
        else
        {
            p.a[k] = ((c1 - c0) * (pt[5] - pt[1])
                    - (c2 - c0) * (pt[3] - pt[1])) / det;
            p.b[k] = ((pt[2] - pt[0]) * (c2 - c0)
                    - (pt[4] - pt[0]) * (c1 - c0)) / det;
            p.d[k] = c0 - p.a[k] * pt[0] - p.b[k] * pt[1];
        }
    }

    p.consumer.take = _gouraud_take;
    p.ctx = ctx;
    p.devdic = devdic;
    p.fillrect = fillrect.mark_.padw;
    p.ncomp = ncomp;
    /* the device's raster is the page's own rows, as it is for a flat fill */
    p.firstrow = 0;
    window = _device_rows(ctx, devdic, &rows) ? &rows : NULL;
    /* the conversion takes the vertices over and frees them, as it does
       for the flat fill that hands it a path's points */
    v = (Xpost_Span_Vertex *)malloc(3 * sizeof(Xpost_Span_Vertex));
    if (!v)
        return VMerror;
    v[0].x = pt[0]; v[0].y = pt[1];
    v[1].x = pt[2]; v[1].y = pt[3];
    v[2].x = pt[4]; v[2].y = pt[5];
    *painted = 1;
    return xpost_span_scanconvert(v, 3, 0, window, &p.consumer);
}

/* Fill the region a run of vertices bounds, in the colour on the operand
 * stack under wherever the caller took its own operands from.
 *
 * The whole of the fill, from the boundary onwards. What differs between
 * the callers is only how the boundary reached them, and each turns it
 * into this run first: the vertices are consumed here, whichever way the
 * fill leaves.
 *
 * The spans are taken over the rows this device takes marks for, which
 * for a device holding the whole page is the whole of the shape and for
 * one standing on a band is the part of it that band can show.
 */
static
int _fillpoly_points(Xpost_Context *ctx,
                     Xpost_Span_Vertex *points,
                     integer npoints,
                     Xpost_Object devdic)
{
    Xpost_Object colorspace;
    int ncomp;
    Xpost_Object comp1, comp2, comp3;
    int numlines;
    /* Xpost_Object x1, y1, x2, y2; */
    Xpost_Object drawline;
    Xpost_Object fillrect;
    int usefillrect;
    Xpost_Span_Rows rows;
    const Xpost_Span_Rows *window;
    struct rspan *rsp;
    int nrsp;
    int i;


    colorspace = xpost_dict_get(ctx, devdic, namenativecolorspace);
    if (xpost_dict_compare_objects(ctx, colorspace, nameDeviceGray) == 0)
        ncomp = 1;
    else if (xpost_dict_compare_objects(ctx, colorspace, nameDeviceRGB) == 0)
        ncomp = 3;
    else
    {
        XPOST_LOG_ERR("unimplemented device color space");
        free(points);
        return unregistered;
    }

    /* The colour this fill paints in is on the operand stack under the
       two operands the signature states, one component or three
       according to what the device paints in, and the signature cannot
       say so: how many there are is the device's answer and not the
       call's. So they are counted here. Popping them unasked took
       whatever the stack had -- or, from an empty stack, the object that
       means there was nothing -- and painted a colour the program never
       named, in silence. The operands an operator needs and has not
       been given are a stackunderflow (PLRM 8.2). */
    if (xpost_stack_count(ctx->lo, ctx->os) < ncomp)
    {
        free(points);
        return stackunderflow;
    }
    if (ncomp == 1)
        comp1 = xpost_stack_pop(ctx->lo, ctx->os);
    else
    {
        comp3 = xpost_stack_pop(ctx->lo, ctx->os);
        comp2 = xpost_stack_pop(ctx->lo, ctx->os);
        comp1 = xpost_stack_pop(ctx->lo, ctx->os);
    }

    /* A fill scanline is a horizontal span. When the device provides a
       compiled FillRect, render each span through it (the per-pixel plotting
       then happens in C rather than a PostScript DrawLine/PutPix loop);
       otherwise fall back to DrawLine unchanged. Both take the same colour
       components plus four numbers, so the loop body and colour roll below are
       identical either way. */
    fillrect = xpost_dict_get(ctx, devdic, nameFillRect);
    usefillrect = xpost_object_get_type(fillrect) == operatortype;

    /* and the rows of the page this device takes marks for, which both
       ways of painting a span below are held to */
    window = _device_rows(ctx, devdic, &rows) ? &rows : NULL;

    /* A device whose rectangle fill is compiled takes each span as the
       conversion states it, so no part of the fill is carried by a
       PostScript loop. A device whose FillRect is a procedure -- or
       which has none, and paints spans as lines -- cannot be called from
       here at all: what runs its method is the interpreter, so the spans
       are gathered and handed to it below, one call per span. */
    if (usefillrect)
    {
        struct _rect_painter p;

        p.consumer.take = _rect_paint;
        p.ctx = ctx;
        p.devdic = devdic;
        p.comp[0] = comp1;
        if (ncomp == 3)
        {
            p.comp[1] = comp2;
            p.comp[2] = comp3;
        }
        p.ncomp = ncomp;
        p.fillrect = fillrect.mark_.padw;
        /* the device's raster is the page's own rows */
        p.firstrow = 0;

        return xpost_span_scanconvert(points, npoints, 0, window, &p.consumer);
    }

    {
        int code = _points_resolved_spans(points, npoints, window,
                                          &rsp, &nrsp, 0);

        if (code)
            return code;
    }

    /* Paint columns [floor(lo), ceil(hi)): every pixel whose interior
       the span reaches, and exactly the geometry when the span lies on
       pixel boundaries. DrawLine paints the pixel centres the segment
       covers, which for a run from xlo to xhi is the same columns the
       rectangle fill covers, which is what makes the two interchangeable
       here; this loop does not assume it. */
    numlines = 0;
    for (i = 0; i < nrsp; i++)
    {
        integer xlo = (integer)floor(rsp[i].lo);
        integer xhi = (integer)ceil(rsp[i].hi);
        int b = rsp[i].band;

        if (xhi <= xlo)
            continue;
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(xlo));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(b));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(xhi));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(b));
        numlines++;
    }

    /*call the device's DrawLine generically with continuations.
      each call to DrawLine looks like this

         comp1 (comp2 comp3)? x1 y1 x2 y2 DEVICE >-- DrawLine

     So what we'll do is push all the points on the stack */

    /*for each line: */
    /*
        xpost_stack_push(ctx->lo, ctx->os, x1);
        xpost_stack_push(ctx->lo, ctx->os, y1);
        xpost_stack_push(ctx->lo, ctx->os, x2);
        xpost_stack_push(ctx->lo, ctx->os, y2);
    */

    /*the loop body and continuation are built from operator objects,
     not executable names, so a user definition of /roll or /repeat on
     the dict stack cannot capture them mid-fill */
    /*then we'll use a repeat loop to call DrawLine
     on each set of 4 numbers. But in order to treat the color space
     generically, we construct the loop body dynamically. */

    /*first push the number of elements
     remember we're using a repeat loop which looks like:
         count proc  -repeat-
     so this line places the `count` parameter on the stack
    */
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(numlines));

    /*then push a mark object to begin array construction
     this array is our loop body */
    xpost_stack_push(ctx->lo, ctx->os, mark);

    /*the loop body finds the 4 coordinate numbers on the stack
     and must roll the color values beneath these numbers on the stack  */

    switch (ncomp)
    {
        case 1:
            xpost_stack_push(ctx->lo, ctx->os, comp1);
            xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(5)); /* total elements to roll */
            xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(1)); /* color components to move */
            break;
        case 3:
            xpost_stack_push(ctx->lo, ctx->os, comp1);
            xpost_stack_push(ctx->lo, ctx->os, comp2);
            xpost_stack_push(ctx->lo, ctx->os, comp3);
            xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(7)); /* total elements to roll */
            xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(3)); /* color components to move */
            break;
    }
    xpost_stack_push(ctx->lo, ctx->os, XPOST_OP(ctx, oproll));

      /*at this point (in constructing the (color-space-generic) loop-body) we have the desired stack picture:

             comp1 (comp2 comp3)? x1 y1 x2 y2

        (with possibly more pairs deeper on the stack, waiting for the next iteration),
        just need to push the devdic (ie. the DEVICE object, in OO-speak) and DrawLine,
        then cinch-off the loop-body procedure (array), make it executable, and call
        the `repeat` operator.
       */

    xpost_stack_push(ctx->lo, ctx->os, devdic);
    drawline = xpost_dict_get(ctx, devdic, nameDrawLine);
    xpost_stack_push(ctx->lo, ctx->os, drawline);

    /*if drawline is a procedure, we also need to call exec */
    if (xpost_object_get_type(drawline) == arraytype)
        xpost_stack_push(ctx->lo, ctx->os, XPOST_OP(ctx, exec));

    /*--the rest of the code here calls-back to postscript (by "continuation")
        by pushing executable names on the execution-stack, and then returns.
        The (color-space-) generic loop-body is called with the
        `repeat` looping-operator.-------------------------------------------*/

    /*Then construct the loop-body procedure array. Just showing you the line here.
      Read the whole story-line of comments for why we're not just executing it here. */

    /*Then, after the loop-body array is constructed, we need to call cvx on it. */
    /*"after" means this line, which pushes on the stack, goes *before* the xpost_name_cons("]") line.
     I'll summarize this part again. */

    /*After this, we call `repeat` and we're done. */

    /*Again since these are scheduled on a stack, we need to push them in reverse order
      from the order in which we desire them to execute.
      What we're doing is:

      opstack> xyxy xyxy xyxy ... xyxy numlines [ comp1 5 1 roll DEVICE DrawLine (exec)?
      -or for rgb color values-:
                                   ... numlines [ comp1 comp2 comp3 7 3 roll DEVICE DrawLine (exec)?
      execstack> repeat cvx ]
                            ^ construct array
                         ^ make executable
                   ^ call the loop operator

      So the sequence in C is:
     */

    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, repeat));
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, cvx));
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, rbracket));

    /*performance could be increased by factoring-out calls to xpost_name_cons()  ... DONE!
      or using opcode shortcuts for Rbracket & cvx (or just the arrtomark() function) and repeat.
     */
    free(rsp);
    return 0;
}

/* The opcode the polygon fill below is installed under, kept so that a
   caller holding a device's method can tell it is this one. */
static unsigned int _fillpoly_opcode;

int xpost_dev_fillpoly_compiled(Xpost_Object method)
{
    return xpost_object_get_type(method) == operatortype
        && method.mark_.padw == _fillpoly_opcode;
}

int xpost_dev_fillpoly_run(Xpost_Context *ctx,
                           const real *co,
                           int npts,
                           Xpost_Object devdic)
{
    Xpost_Span_Vertex *points;
    int code;

    if (npts < 0)
        return rangecheck;
    code = _co_vertices(co, (integer)npts, &points);
    if (code)
        return code;
    return _fillpoly_points(ctx, points, (integer)npts, devdic);
}

static
int _fillpoly(Xpost_Context *ctx,
              Xpost_Object poly,
              Xpost_Object devdic)
{
    Xpost_Span_Vertex *points;
    int code;

    code = _poly_vertices(ctx, poly, &points);
    if (code)
        return code;
    return _fillpoly_points(ctx, points, (integer)poly.comp_.sz, devdic);
}

/* How many bands one polygon array carries. A band costs five
   elements, and 65535 is the length one array is allowed to reach.
   The number is a deliberate bound and not the size field's own
   ceiling -- the field is sixteen bits in the ordinary build and
   thirty-two in the large-object one, so a ceiling taken from it would
   let the two builds refuse at different sizes and only one of them
   would ever reach the machinery that answers in parts. It is the
   length .fillpolyargs allows a polygon, so the two ends of the
   pipeline agree, and it keeps one answer's allocation bounded
   whatever the build. */
#define POLY_ARRAY_MAX 65535
#define POLY_BANDS_MAX (POLY_ARRAY_MAX / 5)

/* Build a null-separated polygon array of pixel-band rectangles, one
   per resolved span, in the FillPoly argument format: winding-uniform
   output any consumer may treat by either insideness rule. Consumes
   nothing; the array comes back through polyp. 0 on success. */
static
int _rspans_poly_cons(Xpost_Context *ctx,
                      const struct rspan *out,
                      int nout,
                      Xpost_Object *polyp)
{
    Xpost_Object result;
    int i, ret;

    result = xpost_array_cons(ctx, 5 * nout);
    /* a construction that was refused answers with no object, which is a
       null and not an invalid: test for the type wanted rather than for
       one of the ways of not having it, or the refusal is carried on
       with and reported as whatever the next operation makes of it */
    if (xpost_object_get_type(result) != arraytype)
        return VMerror;
    for (i = 0; i < nout; i++)
    {
        static const int xsel[4] = { 0, 1, 1, 0 };  /* lo hi hi lo */
        static const int ysel[4] = { 0, 0, 1, 1 };  /* b  b  b+1 b+1 */
        int k;

        for (k = 0; k < 4; k++)
        {
            Xpost_Object pair = xpost_array_cons(ctx, 2);

            if (xpost_object_get_type(pair) != arraytype)
                return VMerror;
            ret = xpost_array_put(ctx, pair, 0,
                xpost_real_cons(xsel[k] ? out[i].hi : out[i].lo));
            if (ret)
                return ret;
            ret = xpost_array_put(ctx, pair, 1,
                xpost_real_cons((real)(out[i].band + ysel[k])));
            if (ret)
                return ret;
            ret = xpost_array_put(ctx, result, 5 * i + k, pair);
            if (ret)
                return ret;
        }
        ret = xpost_array_put(ctx, result, 5 * i + 4, null);
        if (ret)
            return ret;
    }

    *polyp = xpost_object_cvlit(result);
    return 0;
}

/* The same polygon, pushed on the operand stack, for the answers that
   are one array or none. 0 on success. */
static
int _rspans_to_poly(Xpost_Context *ctx,
                    struct rspan *out,
                    int nout)
{
    Xpost_Object result;
    int ret;

    if (nout > POLY_BANDS_MAX)
        return limitcheck;

    ret = _rspans_poly_cons(ctx, out, nout, &result);
    if (ret)
        return ret;

    xpost_stack_push(ctx->lo, ctx->os, result);
    return 0;
}

/* The resolved form of a device region, kept against the serial the
   graphics state carries for the clip it belongs to.
 *
 * Resolving a region is the expensive half of meeting one: the whole
 * region scan-converts however small the subject is. A tiling pattern
 * meets every cell and every mark inside it against the same region, so
 * without this the cost of one fill grows with the region rather than
 * with what is being painted.
 *
 * Two are kept rather than one because the clip alternates: the pattern
 * machinery clips to each cell inside a gsave and drops back to the
 * region outside it between cells, so a single entry would be evicted by
 * the cell and refilled from the whole region on the next one. The entry
 * that has gone longest without a hit is the one replaced.
 *
 * The key is a serial from _newregionserial below, which never repeats
 * within a run, so an entry can only ever answer for the region it was
 * built from. The entity the clip array occupied is compared as well, so
 * that even a serial reissued after the counter is restarted cannot
 * match an entry built from something else. Nothing here holds a
 * reference into VM: the entity number is compared, never followed. */
#define REGION_MEMO 2
static struct
{
    int serial;
    int ent;
    unsigned int off, sz;
    struct rspan *rsp;
    int n;
    unsigned long used;
} _region_memo[REGION_MEMO];
static unsigned long _region_memo_clock;
static int _region_serial_next;

/* --- the clip, resolved and remembered -------------------------------
   A clip is a shape, and what a mark needs is the columns it admits on the
   row being painted. Resolving that per mark is what a page of text cannot
   afford, so the answer is memoised against the clip's own identity -- and
   the serial it is filed under is minted outside virtual memory, so a
   restore cannot wind the counter back under an entry still held. */

static
void _region_memo_flush(void)
{
    int i;

    for (i = 0; i < REGION_MEMO; i++)
    {
        free(_region_memo[i].rsp);
        _region_memo[i].rsp = NULL;
        _region_memo[i].serial = 0;
        _region_memo[i].n = 0;
        _region_memo[i].used = 0;
    }
    _region_memo_clock = 0;
    _region_serial_next = 1;
}

/* -  .newregionserial  int
   The serial to name the next device region by. The counter only ever
   moves forward, so no two regions of a run are named alike and a
   resolved region cached against a serial cannot be taken for a later
   region that happened to be given the same number. A counter kept in
   the graphics state could not promise that: restore would wind it back
   and the numbers after it would be handed out a second time. */
static
int _newregionserial(Xpost_Context *ctx)
{
    if (_region_serial_next <= 0)
    {
        /* the counter has run its range: nothing cached can be told
           apart from what the reissued numbers will name, so start over
           with nothing cached. Every cache filed under this serial is
           given up here, not just this file's -- the text route keeps
           its own memo of a clip's bands under the same number, and a
           restart this side that left that side holding would hand a
           later region a stale clip. tests/serial-caches is the roster
           of what is filed under each serial counter. */
        _region_memo_flush();
        xpost_op_font_clip_memo_drop();
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(_region_serial_next));
    return _region_serial_next++, 0;
}

/* The cached spans for a clip, if this is the same clip they were
   resolved from. The serial says the clip has not been rebuilt beneath
   the cache, and the three fields say the object named is the same
   one. */
static
int _region_memo_find(int serial, Xpost_Object clip)
{
    int i;

    for (i = 0; i < REGION_MEMO; i++)
        if (_region_memo[i].rsp
            && _region_memo[i].serial == serial
            && _region_memo[i].ent == xpost_object_get_ent(clip)
            && _region_memo[i].off == clip.comp_.off
            && _region_memo[i].sz == clip.comp_.sz)
        {
            _region_memo[i].used = ++_region_memo_clock;
            return i;
        }
    return -1;
}

/* take ownership of rsp in the entry longest unused */
static
void _region_memo_store(int serial, Xpost_Object clip,
                        struct rspan *rsp, int n)
{
    int i, victim = 0;

    for (i = 1; i < REGION_MEMO; i++)
        if (_region_memo[i].used < _region_memo[victim].used)
            victim = i;
    free(_region_memo[victim].rsp);
    _region_memo[victim].serial = serial;
    _region_memo[victim].ent = xpost_object_get_ent(clip);
    _region_memo[victim].off = clip.comp_.off;
    _region_memo[victim].sz = clip.comp_.sz;
    _region_memo[victim].rsp = rsp;
    _region_memo[victim].n = n;
    _region_memo[victim].used = ++_region_memo_clock;
}

/* Fold a run of resolved band spans to the pixel columns they reach,
 * merging the ones that then touch, and return how many are left. The
 * columns a span reaches are the ones a fill of it would paint: from the
 * floor of its left edge up to, but not including, the ceiling of its
 * right, which is the range the fill loop takes. A span that reaches no
 * pixel's interior drops out. The run stays ascending and disjoint within
 * each band, which the merge below relies on: real spans in a band are
 * ascending and disjoint, and flooring and ceiling preserve the order,
 * so the only pairs the fold can bring together are neighbours. */
static int _rspans_to_columns(struct rspan *r, int n)
{
    int i, m = 0;

    for (i = 0; i < n; i++)
    {
        double lo = floor(r[i].lo);
        double hi = ceil(r[i].hi);

        if (hi <= lo)
            continue;
        if (m > 0 && r[m - 1].band == r[i].band && lo <= r[m - 1].hi)
        {
            if (hi > r[m - 1].hi)
                r[m - 1].hi = hi;
            continue;
        }
        r[m].band = r[i].band;
        r[m].lo = lo;
        r[m].hi = hi;
        m++;
    }
    return m;
}

/* subjectpoly clippoly serial  .regionmeet  spanpoly
 * subjectpoly clippath serial  .regionmeet  spanpoly
 *
 * THE intersection of two device regions. The subject is a
 * null-separated polygon array in the FillPoly argument format, and the
 * region is either such an array or the path a graphics state stores
 * its clip in -- the same vertices, held where an array's bounded
 * length does not reach them, which is what lets a region divided
 * finely across the rows be met at all. The result is one polygon array
 * of pixel-band rectangles, which every consumer downstream treats by
 * either insideness rule because the rectangles are winding-uniform.
 *
 * THE BOUNDARY CONVENTION, stated once. A device region is a set of
 * pixels. Each operand is scan-converted to pixel-row bands under the
 * nonzero winding rule and the any-part-of-pixel rule of PLRM 7.5.1 ("a
 * shape is scan-converted by painting any pixel whose square region
 * intersects the shape, no matter how small the intersection is"), and
 * the x extent each band reaches is then taken out to the columns that
 * rule paints. They meet band by band, taking the later left edge and
 * the earlier right edge of each pair of overlapping runs, which on
 * whole columns is the intersection of the two pixel sets.
 *
 * That is what PLRM 7.5.1 asks for in both axes at once: "the clipping
 * region consists of the set of pixels that would be included by a fill
 * operation. Subsequent painting operations affect a region that is the
 * intersection of the set of pixels defined by the clipping region with
 * the set of pixels for the region to be painted." Meeting the real
 * extents instead would drop the pixels two regions share without their
 * extents overlapping inside one -- a column each reaches, and which a
 * fill of either paints.
 *
 * Whole columns are also what makes the result a region in its own
 * right. The output goes back out as a polygon, and a polygon's vertices
 * are quantized to the 1/256 device grid when they are read again; a
 * boundary already on a pixel edge survives that, an arbitrary real one
 * can cross to the next column and take a column of the region with it.
 *
 * A boundary question about this operation is settled by PLRM 7.5.1 and
 * by self-consistency: a region painted in pieces -- a pattern's cells,
 * a shading's strips -- must cover exactly the pixels one solid fill of
 * the same path covers. The half-plane clipper in clip.ps answers a
 * different question and is not a substitute; its comment says so. */
static
int _regionmeet(Xpost_Context *ctx,
                Xpost_Object subj,
                Xpost_Object clip,
                Xpost_Object serial,
                Xpost_Object height)
{
    struct rspan *S = NULL, *out = NULL;
    struct rspan *C = NULL;
    int Cowned = 1;
    int nS, nC, nout, outcap;
    int si, ci;
    int sn;
    int code;
    /* No pixel outside the page's rows is ever painted, so a region is
       resolved over those rows alone: the scan conversion walks a
       boundary reaching past them but keeps no span there, which bounds
       both the work and the store by the page and not by how far a
       clip path a program hands in happens to run. The meet intersects
       these against the on-page subject regardless, so the pixels the
       result names -- the only ones a paint can reach -- are unchanged.
       A device that states no height (zero) is resolved whole. */
    Xpost_Span_Rows pagerows;
    const Xpost_Span_Rows *rows = NULL;

    if (height.int_.val > 0)
    {
        pagerows.lo = 0;
        pagerows.hi = height.int_.val - 1;
        rows = &pagerows;
    }

    code = _poly_resolved_spans(ctx, subj, &S, &nS, 0, rows);
    if (code)
        return code;
    nS = _rspans_to_columns(S, nS);

    sn = serial.int_.val;
    si = sn > 0 ? _region_memo_find(sn, clip) : -1;
    if (si >= 0)
    {
        C = _region_memo[si].rsp;
        nC = _region_memo[si].n;
        Cowned = 0;
    }
    else
    {
        /* the region operand arrives either as a polygon array or as
           the path a graphics state stores its clip in; a region no
           single array describes has only the second form */
        code = xpost_object_get_type(clip) == stringtype
             ? _path_resolved_spans(ctx, clip, &C, &nC, 0, rows)
             : _poly_resolved_spans(ctx, clip, &C, &nC, 0, rows);
        if (code)
        {
            free(S);
            return code;
        }
        /* the kept form is the folded one: it is what every meeting
           against this region uses */
        nC = _rspans_to_columns(C, nC);
        if (sn > 0)
        {
            _region_memo_store(sn, clip, C, nC);
            Cowned = 0;
        }
    }

    nout = 0;
    outcap = 0;
    si = ci = 0;
    while (si < nS && ci < nC)
    {
        if (S[si].band < C[ci].band)
            si++;
        else if (C[ci].band < S[si].band)
            ci++;
        else
        {
            /* one shared band: both extent runs are disjoint and
               ascending, so a linear merge finds every overlap */
            int b = S[si].band;
            int i2 = si, j2 = ci;

            while (i2 < nS && S[i2].band == b && j2 < nC && C[j2].band == b)
            {
                double L = S[i2].lo > C[j2].lo ? S[i2].lo : C[j2].lo;
                double R = S[i2].hi < C[j2].hi ? S[i2].hi : C[j2].hi;

                if (L < R)
                {
                    code = _rspan_push(&out, &outcap, &nout, b, L, R);
                    if (code)
                    {
                        free(S);
                        if (Cowned) free(C);
                        free(out);
                        return code;
                    }
                }
                if (S[i2].hi < C[j2].hi)
                    i2++;
                else
                    j2++;
            }
            while (si < nS && S[si].band == b)
                si++;
            while (ci < nC && C[ci].band == b)
                ci++;
        }
    }
    free(S);
    if (Cowned)
        free(C);

    code = _rspans_to_poly(ctx, out, nout);
    free(out);
    return code;
}

/* poly  .eospanpoly  spanpoly
   The even-odd interior of a filled region, returned as pixel-band
   rectangles in the FillPoly argument format. The rectangles are
   winding-uniform, so downstream nonzero machinery (the span
   intersection, the device fill) treats them exactly: this is how
   eofill and eoclip obtain the rule the nonzero pipeline lacks. */
static
int _eospanpoly(Xpost_Context *ctx,
                Xpost_Object poly)
{
    struct rspan *rsp = NULL;
    int nrsp;
    int code;

    code = _poly_resolved_spans(ctx, poly, &rsp, &nrsp, 1, NULL);
    if (code)
        return code;

    code = _rspans_to_poly(ctx, rsp, nrsp);
    free(rsp);
    return code;
}

/* poly ylo yhi  .eospanpoly  spanpoly
   The same interior over a window of pixel rows: the bands from ylo up
   to but not including yhi, and nothing else. A path's interior can
   hold more bands than one array describes -- a comb or a stipple over
   a whole page does -- and then it is asked for a window at a time,
   narrowing until each answer fits. The windows are read off the same
   scan conversion, so a band falls in exactly one of them and the
   windows together are the whole interior. */
static
int _eospanpoly_rows(Xpost_Context *ctx,
                     Xpost_Object poly,
                     Xpost_Object ylo,
                     Xpost_Object yhi)
{
    struct rspan *rsp = NULL;
    int nrsp, lo, hi, i, m;
    int code;
    /* Resolve the interior over the window's rows alone, the way a region
       is (see _regionmeet): the scan conversion walks a boundary reaching
       past the window -- a subject a program sizes runs far off the page --
       but keeps no span there, so the work is bounded by the window and not
       by how far the subject happens to reach. Without this the whole
       interior was resolved and then the window cut from it, so the window
       bounded the answer but not the walk that made it. */
    Xpost_Span_Rows winrows;
    const Xpost_Span_Rows *rows = NULL;

    lo = ylo.int_.val;
    hi = yhi.int_.val;
    if (hi > lo)
    {
        winrows.lo = lo;
        winrows.hi = hi - 1;
        rows = &winrows;
    }

    code = _poly_resolved_spans(ctx, poly, &rsp, &nrsp, 1, rows);
    if (code)
        return code;

    for (i = 0, m = 0; i < nrsp; i++)
        if (rsp[i].band >= lo && rsp[i].band < hi)
            rsp[m++] = rsp[i];

    code = _rspans_to_poly(ctx, rsp, m);
    free(rsp);
    return code;
}

/* path evenodd  .pathspanparts  [ spanpoly ... ]
   A path's interior, in the pixel-band rectangles a painter marks one
   after another. The vertices are read straight out of the packed path,
   where an array's bounded length does not reach them, so a path of
   more points than one polygon describes is answered here rather than
   refused: a LanguageLevel 2 path has no length of its own to exceed
   (PLRM 4.4), and the polygon a fill hands a device is the only bound
   in the way.

   The answer is as many polygons as the bands need. They are cut out of
   one scan conversion in band order, so a band falls in exactly one
   part and the parts together are the whole interior; the rectangles
   are winding-uniform, so a painter marking them one after another
   marks what one polygon would have given, and no pixel twice. */
static
int _pathspanparts(Xpost_Context *ctx,
                   Xpost_Object path,
                   Xpost_Object evenodd)
{
    struct rspan *rsp = NULL;
    Xpost_Object parts, poly;
    int nrsp, nparts, i, n;
    int code;

    code = _path_resolved_spans(ctx, path, &rsp, &nrsp,
                                evenodd.int_.val ? 1 : 0, NULL);
    if (code)
        return code;

    nparts = (nrsp + POLY_BANDS_MAX - 1) / POLY_BANDS_MAX;
    if (nparts > POLY_ARRAY_MAX)
    {
        free(rsp);
        return limitcheck;
    }

    parts = xpost_array_cons(ctx, nparts);
    if (xpost_object_get_type(parts) != arraytype)
    {
        free(rsp);
        return VMerror;
    }
    /* the parts already built are reachable only through this array
       while the next one allocates */
    xpost_stack_push(ctx->lo, ctx->hold, parts);

    for (i = 0; i < nparts; i++)
    {
        n = nrsp - i * POLY_BANDS_MAX;
        if (n > POLY_BANDS_MAX)
            n = POLY_BANDS_MAX;
        code = _rspans_poly_cons(ctx, rsp + i * POLY_BANDS_MAX, n, &poly);
        if (!code)
            code = xpost_array_put(ctx, parts, i, poly);
        if (code)
        {
            free(rsp);
            return code;
        }
    }
    free(rsp);

    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(parts));
    return 0;
}

/* --- putting colour on pixels ----------------------------------------
   The compiled marking methods: a rectangle, a span, a blended pixel, a
   line. These are where a device that keeps rows of its own actually gets
   written to, and they are compiled because they run once per pixel or
   once per span rather than once per page. */

/* A colour component scaled to a 0..max channel value, through the
   driver contract's fold: the clamp that keeps an out-of-range
   component from wrapping the channel is stated there, once, for every
   device. */
static double
_channel(Xpost_Object v, double max)
{
    return xpost_dev_num_to_component(v) * max;
}

/* The device's halftone threshold cell, when paint screens through
   one: a bilevel device carries .htcell/.htw/.hth and every grey
   written compares against the threshold under its pixel. */
const unsigned char *
xpost_dev_ht_cell(Xpost_Context *ctx, Xpost_Object devdic, int *w, int *h)
{
    /* The cell in force is written as the screen changes, which is what a
       page does to a device rather than what the device is, so it is kept
       in the state beside it (data/device.ps). */
    Xpost_Object src = devdic;
    Xpost_Object st, c, wo, ho;

    /* Asked of a device and of the blit dictionary a caller copies the
       cell into. A device keeps what a page does to it beside it, in
       its state (data/device.ps); a blit dictionary is handed the cell
       itself and has no state. Whichever holds it answers. */
    st = xpost_dict_get(ctx, devdic, namedotstate);
    if (xpost_object_get_type(st) == dicttype)
        src = st;
    c = xpost_dict_get(ctx, src, namedothtcell);
    wo = xpost_dict_get(ctx, src, namedothtw);
    ho = xpost_dict_get(ctx, src, namedothth);

    if (xpost_object_get_type(c) != stringtype
     || xpost_object_get_type(wo) != integertype
     || xpost_object_get_type(ho) != integertype)
        return NULL;
    *w = wo.int_.val;
    *h = ho.int_.val;
    /* the cell is addressed as h rows of w thresholds, and the
       dimensions come out of a dictionary a program can build, so the
       cell has to hold that many bytes; the row count is compared
       against the length divided by the row width, which holds for
       every pair of dimensions rather than only those whose product
       fits the type a multiplication would form it in */
    if (*w < 1 || *h < 1 || (unsigned int)*h > c.comp_.sz / (unsigned int)*w)
        return NULL;
    return (const unsigned char *)xpost_string_get_pointer(ctx, c);
}

/* A colour raster row is three component planes -- one string of one
   byte per pixel for red, for green and for blue, in that order -- and
   its pixel count is the length the three share. This reads a row into
   the three pointers and that count; pass a null pointer array to
   measure a row without taking pointers into it.

   A caller that writes asks for the pointers with forwrite, which holds
   each plane to its access before its pointer is taken, since a raw
   pointer bypasses the checked mutator. Nothing is copied for a save
   level first: PLRM 3.7.3 exempts strings from save and restore, so the
   bytes written are the bytes that stay. The pointers are good until
   something allocates, which would move the memory file under them.

   Returns nonzero, having written nothing, when the row is not three
   planes of one length or a plane refuses the write. */
static int
_rgb_planes(Xpost_Context *ctx, Xpost_Object row, int forwrite,
            unsigned char **p, int *w)
{
    unsigned int sz = 0;
    int c;

    if (xpost_object_get_type(row) != arraytype || row.comp_.sz != 3)
        return typecheck;
    for (c = 0; c < 3; c++)
    {
        Xpost_Object plane = xpost_array_get(ctx, row, c);

        if (xpost_object_get_type(plane) != stringtype)
            return typecheck;
        if (c == 0)
            sz = plane.comp_.sz;
        else if (plane.comp_.sz != sz)
            return rangecheck;
        if (forwrite && !xpost_object_is_writeable(ctx, plane))
            return invalidaccess;
        if (p)
        {
            p[c] = (unsigned char *)xpost_string_get_pointer(ctx, plane);
            if (!p[c])
                return VMerror;
        }
    }
    *w = (int)sz;
    return 0;
}

/* A grey raster row is one string of one byte per pixel, and its pixel
   count is the string's length. This reads a row into that pointer and
   count; pass a null pointer to measure a row without taking a pointer
   into it.

   A caller that writes asks with forwrite, which holds the row to its
   access before its pointer is taken, since a raw pointer bypasses the
   checked mutator. Nothing is copied for a save level first: PLRM 3.7.3
   exempts strings from save and restore, so the bytes written are the
   bytes that stay. The pointer is good until something allocates, which
   would move the memory file under it.

   Returns nonzero, having written nothing, when the row is not a string
   or refuses the write. */
static int
_gray_row(Xpost_Context *ctx, Xpost_Object row, int forwrite,
          unsigned char **p, int *w)
{
    if (xpost_object_get_type(row) != stringtype)
        return typecheck;
    if (forwrite && !xpost_object_is_writeable(ctx, row))
        return invalidaccess;
    if (p)
    {
        *p = (unsigned char *)xpost_string_get_pointer(ctx, row);
        if (!*p)
            return VMerror;
    }
    *w = (int)row.comp_.sz;
    return 0;
}

/* Fast FillRect for grayscale (DeviceGray) array-of-strings devices such as
   PGMIMAGE. Writes the ImgData row strings directly rather than looping over
   PutPix in PostScript; erasepage clears the whole page through FillRect, so
   the per-pixel interpreter overhead otherwise dominates page emission.
   The painted rectangle is the driver contract's, reached through
   xpost_dev_rect_normalize; the clip source is this device's own -- rows
   from the ImgData length, columns from each row string's own length. */
static
int _fillrectgray(Xpost_Context *ctx,
                  Xpost_Object val,
                  Xpost_Object x,
                  Xpost_Object y,
                  Xpost_Object w,
                  Xpost_Object h,
                  Xpost_Object devdic)
{
    Xpost_Object imgdata, row;
    int height, iy, iy0, iy1, ix0, ix1;
    unsigned char b;
    int bht;
    const unsigned char *cell;
    int hw = 0, hh = 0;

    cell = xpost_dev_ht_cell(ctx, devdic, &hw, &hh);
    imgdata = xpost_dict_get(ctx, devdic, nameImgData);
    if (xpost_object_get_type(imgdata) != arraytype)
        return undefined;
    height = imgdata.comp_.sz;

    /* value -> byte, matching PGMIMAGE PutPix "255 mul cvi put" */
    b = (unsigned char)(int)_channel(val, 255.0);
    bht = xpost_dev_ht_level(xpost_dev_num_to_component(val));

    xpost_dev_rect_normalize(xpost_object_number(x), xpost_object_number(y),
                             xpost_object_number(w), xpost_object_number(h),
                             &ix0, &iy0, &ix1, &iy1);
    if (!xpost_dev_span_clip(&iy0, &iy1, height))
        return 0;

    for (iy = iy0; iy <= iy1; iy++)
    {
        int cx0 = ix0, cx1 = ix1;
        unsigned char *p;
        int rw, gret;

        row = xpost_array_get(ctx, imgdata, iy);
        gret = _gray_row(ctx, row, 1, &p, &rw);
        if (gret)
            return gret;
        if (!xpost_dev_span_clip(&cx0, &cx1, rw))
            continue;

        if (cell)
        {
            int ix;

            for (ix = cx0; ix <= cx1; ix++)
                p[ix] = xpost_dev_ht_ink(bht, cell, hw, hh, ix, iy);
        }
        else
            memset(p + cx0, b, (size_t)(cx1 - cx0 + 1));
    }

    return 0;
}

/* A blend coverage as the fraction of full ink it is: 0 leaves the
   ground alone, 255 lays the colour down whole. The value is folded into
   that range because the blend below is an interpolation and only stays
   between its endpoints while the weight does: past 255 the blend runs
   beyond the ink it was moving toward and the stored channel wraps
   inside its byte, and the alpha device wraps a fully covered pixel
   round to transparent. The png device folds a coverage the same way. */
static int _coverage(Xpost_Object cov)
{
    int c = xpost_object_get_type(cov) == realtype
                ? xpost_dev_int_of((double)cov.real_.val)
                : cov.int_.val;

    if (c < 0) return 0;
    if (c > 255) return 255;
    return c;
}

/* Blend a coverage-weighted pixel for grayscale array-of-strings devices:
   dst += (val - dst) * cov / 255. The text operators use this for glyph
   edge pixels when the device renders anti-aliased text. */
static
int _blendpixgray(Xpost_Context *ctx,
                  Xpost_Object val,
                  Xpost_Object cov,
                  Xpost_Object x,
                  Xpost_Object y,
                  Xpost_Object devdic)
{
    Xpost_Object imgdata, row;
    int ix, iy, c;
    int src, dst;
    unsigned char *p;

    imgdata = xpost_dict_get(ctx, devdic, nameImgData);
    if (xpost_object_get_type(imgdata) != arraytype)
        return undefined;
    ix = xpost_dev_pixel(xpost_object_number(x));
    iy = xpost_dev_pixel(xpost_object_number(y));
    c = _coverage(cov);
    /* a device coordinate is signed and arrives from anywhere on the
       page, so the raster's extents are widened into the signed type to
       be compared against rather than the coordinate narrowed into
       theirs: off the top and off the bottom both have to miss */
    if (iy < 0 || iy >= (integer)imgdata.comp_.sz)
        return 0;
    row = xpost_array_get(ctx, imgdata, iy);
    {
        int rw, gret = _gray_row(ctx, row, 1, &p, &rw);

        if (gret)
            return gret;
        if (ix < 0 || ix >= rw)
            return 0;
    }
    src = (int)_channel(val, 255.0);
    dst = p[ix];
    p[ix] = (unsigned char)xpost_dev_blend_channel(dst, src, c);
    return 0;
}

/* Blend a coverage-weighted pixel for planar rgb devices (each row
   three component planes): per channel,
   dst += (val - dst) * cov / 255. The text operators use this for
   glyph edge pixels when the device renders anti-aliased text. */
static
int _blendpixrgb(Xpost_Context *ctx,
                 Xpost_Object r,
                 Xpost_Object g,
                 Xpost_Object b,
                 Xpost_Object cov,
                 Xpost_Object x,
                 Xpost_Object y,
                 Xpost_Object devdic)
{
    Xpost_Object imgdata, row;
    unsigned char *pl[3];
    int ix, iy, c, rw, ret;
    int src[3], k;

    imgdata = xpost_dict_get(ctx, devdic, nameImgData);
    if (xpost_object_get_type(imgdata) != arraytype)
        return undefined;
    ix = xpost_dev_pixel(xpost_object_number(x));
    iy = xpost_dev_pixel(xpost_object_number(y));
    c = _coverage(cov);
    /* a device coordinate is signed and arrives from anywhere on the
       page, so the raster's extents are widened into the signed type to
       be compared against rather than the coordinate narrowed into
       theirs: off the top and off the bottom both have to miss */
    if (iy < 0 || iy >= (integer)imgdata.comp_.sz)
        return 0;
    row = xpost_array_get(ctx, imgdata, iy);
    ret = _rgb_planes(ctx, row, 1, pl, &rw);
    if (ret)
        return ret;
    if (ix < 0 || ix >= rw)
        return 0;
    src[0] = (int)_channel(r, 255.0);
    src[1] = (int)_channel(g, 255.0);
    src[2] = (int)_channel(b, 255.0);
    for (k = 0; k < 3; k++)
        pl[k][ix] = (unsigned char)xpost_dev_blend_channel(pl[k][ix], src[k], c);
    return 0;
}

/* Fill a rectangle of a planar rgb device (each row three component
   planes). The painted rectangle is the driver contract's, reached
   through xpost_dev_rect_normalize; the clip source is this device's
   own -- rows from the ImgData length, columns from each row's own
   plane length. The rgb devices render continuous tone, so no halftone
   cell applies. */
static
int _fillrectrgb(Xpost_Context *ctx,
                 Xpost_Object r,
                 Xpost_Object g,
                 Xpost_Object b,
                 Xpost_Object x,
                 Xpost_Object y,
                 Xpost_Object w,
                 Xpost_Object h,
                 Xpost_Object devdic)
{
    Xpost_Object imgdata, row;
    int height, iy, iy0, iy1, ix0, ix1;
    unsigned char chan[3];
    int ret;

    imgdata = xpost_dict_get(ctx, devdic, nameImgData);
    if (xpost_object_get_type(imgdata) != arraytype)
        return undefined;
    height = imgdata.comp_.sz;

    chan[0] = (unsigned char)(int)_channel(r, 255.0);
    chan[1] = (unsigned char)(int)_channel(g, 255.0);
    chan[2] = (unsigned char)(int)_channel(b, 255.0);

    xpost_dev_rect_normalize(xpost_object_number(x), xpost_object_number(y),
                             xpost_object_number(w), xpost_object_number(h),
                             &ix0, &iy0, &ix1, &iy1);
    if (!xpost_dev_span_clip(&iy0, &iy1, height))
        return 0;

    for (iy = iy0; iy <= iy1; iy++)
    {
        int cx0 = ix0, cx1 = ix1;
        unsigned char *pl[3];
        int rw, k;

        row = xpost_array_get(ctx, imgdata, iy);
        ret = _rgb_planes(ctx, row, 1, pl, &rw);
        if (ret)
            return ret;
        if (!xpost_dev_span_clip(&cx0, &cx1, rw))
            continue;
        for (k = 0; k < 3; k++)
            memset(pl[k] + cx0, chan[k], (size_t)(cx1 - cx0 + 1));
    }

    return 0;
}

/* x y w h width height  .rectspan  x0 y0 x1 y1 true
                                    false
   The rectangle FillRect paints, for the PostScript base class: the
   driver contract's normaliser and clip, so the interpreted method and
   the compiled fills beside it paint one pixel set rather than two that
   happen to agree on the cases anyone tried. False when the rectangle
   lies wholly off the device. */
static
int _rectspan(Xpost_Context *ctx,
              Xpost_Object x,
              Xpost_Object y,
              Xpost_Object w,
              Xpost_Object h,
              Xpost_Object width,
              Xpost_Object height)
{
    int x0, y0, x1, y1;

    xpost_dev_rect_normalize(xpost_object_number(x), xpost_object_number(y),
                             xpost_object_number(w), xpost_object_number(h),
                             &x0, &y0, &x1, &y1);
    if (!xpost_dev_rect_clip(&x0, &y0, &x1, &y1,
                             xpost_dev_num_to_int(width),
                             xpost_dev_num_to_int(height)))
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(x0));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(y0));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(x1));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(y1));
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
}

/* x1 y1 x2 y2 width height  .linepix  [x y x y ...]
   The pixels DrawLine paints, for the PostScript base class: the driver
   contract's walk, clipped to the device, as a flat array of coordinate
   pairs. The walk is stated once, in C, so the interpreted method and
   the window devices' compiled ones cover the same pixels -- which is
   what lets the scanline filler treat a fill span and a line as the
   same thing. */
static
int _linepix(Xpost_Context *ctx,
             Xpost_Object x1,
             Xpost_Object y1,
             Xpost_Object x2,
             Xpost_Object y2,
             Xpost_Object width,
             Xpost_Object height)
{
    Xpost_Dev_Line line;
    Xpost_Object out;
    int w = xpost_dev_num_to_int(width);
    int h = xpost_dev_num_to_int(height);
    int px, py, n = 0;
    int cap;

    /* the walk visits at most one pixel per step of the major axis,
       and the major axis is held to the device below, so the count
       pass and the fill pass are each bounded by the device and not by
       how far the segment was drawn: count first and fill second */
    xpost_dev_line_init(&line, xpost_object_number(x1),
                        xpost_object_number(y1),
                        xpost_object_number(x2),
                        xpost_object_number(y2));
    xpost_dev_line_clip_major(&line, line.major_is_x ? w : h);
    while (xpost_dev_line_next(&line, &px, &py))
        if (px >= 0 && px < w && py >= 0 && py < h)
            n++;

    cap = 2 * n;
    if (cap > 65535)
        return limitcheck;
    out = xpost_array_cons(ctx, cap);
    if (xpost_object_get_type(out) == nulltype)
        return VMerror;

    xpost_dev_line_init(&line, xpost_object_number(x1),
                        xpost_object_number(y1),
                        xpost_object_number(x2),
                        xpost_object_number(y2));
    xpost_dev_line_clip_major(&line, line.major_is_x ? w : h);
    n = 0;
    while (xpost_dev_line_next(&line, &px, &py))
    {
        int ret;

        if (px < 0 || px >= w || py < 0 || py >= h)
            continue;
        ret = xpost_array_put(ctx, out, n++, xpost_int_cons(px));
        if (ret)
            return ret;
        ret = xpost_array_put(ctx, out, n++, xpost_int_cons(py));
        if (ret)
            return ret;
    }

    /* literal: the caller indexes it, and an executable array reached
       by name would run its contents onto the operand stack instead */
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(out));
    return 0;
}

/* --- getting a page out of the process -------------------------------
   Encoding and writing: base64 and flate for the writers, the bit and byte
   row formats for the rasters. What differs between the formats is here;
   what a page is does not. */

/* Encode a string's bytes as base64, RFC 4648 alphabet with padding;
   the output string allocates in local VM. The caller chunks input
   at a multiple of three bytes below the string limit. */
static
int _base64(Xpost_Context *ctx, Xpost_Object S)
{
    static const char abc[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *in;
    unsigned int n = S.comp_.sz, on, i, o;
    Xpost_Object out;
    char *op;

    on = (n + 2) / 3 * 4;
    if (on > 65535)
        return rangecheck;
    out = xpost_string_cons(ctx, on, NULL);
    if (xpost_object_get_type(out) == nulltype)
        return VMerror;
    in = (unsigned char *)xpost_string_get_pointer(ctx, S);
    op = xpost_string_get_pointer(ctx, out);
    for (i = 0, o = 0; i + 2 < n; i += 3, o += 4)
    {
        unsigned int v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];

        op[o] = abc[v >> 18];
        op[o + 1] = abc[(v >> 12) & 63];
        op[o + 2] = abc[(v >> 6) & 63];
        op[o + 3] = abc[v & 63];
    }
    if (i < n)
    {
        unsigned int v = in[i] << 16;
        int two = i + 1 < n;

        if (two)
            v |= in[i + 1] << 8;
        op[o] = abc[v >> 18];
        op[o + 1] = abc[(v >> 12) & 63];
        op[o + 2] = two ? abc[(v >> 6) & 63] : '=';
        op[o + 3] = '=';
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(out));
    return 0;
}


/* Write bytes to an emission target, routing through the registered
   stdout/stderr handler when one has claimed the stream, as the
   writestring operator does. */
static
int _emit_write(Xpost_Context *ctx, Xpost_File *f,
                const unsigned char *buf, size_t len)
{
    FILE *stream = xpost_file_stdio_stream_get(f);

    if (stream == stdout && ctx->stdout_fn)
        return ctx->stdout_fn(ctx->stdout_user, (const char *)buf, len) == len ? 0 : -1;
    if (stream == stderr && ctx->stderr_fn)
        return ctx->stderr_fn(ctx->stderr_user, (const char *)buf, len) == len ? 0 : -1;
    return xpost_file_write((const char *)buf, 1, (integer)len, f) == (integer)len ? 0 : -1;
}

/* Emit the packed bytes of a run of grayscale array-of-strings rows as
   a binary PBM's raster: each row's bytes thresholded at half coverage
   (black below 128) and packed most significant bit first, one row to
   the next byte boundary as the format wants.

   The rows alone, and not the header that frames them: the header
   names the whole page's extent, which a run of its rows does not
   carry, so it is written where that extent is known (.writehead,
   data/image.ps). A page put out at once and a page put out a band at
   a time then reach the same bytes through this one walk. */
/* A screening device's own method, so that the ground row and a read
   of a pixel the device holds no storage for meet the cell by the very
   comparison a mark meets it by. That comparison used to be written out
   again in PostScript beside these two compiled writers; all three call
   xpost_dev_ht_ink now.

   Takes the grey, the pixel and the device, and answers 1 where the
   page is white there and 0 where it is inked, leaving the pixel behind
   it so the caller reads back what it passed in. */
static
int _screenink(Xpost_Context *ctx,
               Xpost_Object c,
               Xpost_Object x,
               Xpost_Object y,
               Xpost_Object devdic)
{
    const unsigned char *cell;
    int w = 0, h = 0;

    cell = xpost_dev_ht_cell(ctx, devdic, &w, &h);
    if (!cell)
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons(xpost_dev_ht_ink(
                         xpost_dev_ht_level(xpost_dev_num_to_component(c)),
                         cell, w, h,
                         (int)xpost_object_number(x),
                         (int)xpost_object_number(y)) ? 1 : 0));
    xpost_stack_push(ctx->lo, ctx->os, x);
    xpost_stack_push(ctx->lo, ctx->os, y);
    return 0;
}

/* Writes an image's rows to a stream a bit to the pixel, packing each
   row into bytes as it goes. What arrives is an array of row strings,
   which is how the raster devices hold a page. */
static
int _writebitrows(Xpost_Context *ctx,
                  Xpost_Object imgdata,
                  Xpost_Object F)
{
    Xpost_File *f;
    Xpost_Object row;
    unsigned char *buf;
    int width, height, rb, iy, ix;

    if (!xpost_file_get_status(ctx->lo, F))
        return ioerror;
    if (!xpost_object_is_writeable(ctx, F))
        return invalidaccess;
    f = xpost_file_get_file_pointer(ctx->lo, F);

    height = imgdata.comp_.sz;
    if (height == 0)
        return rangecheck;
    row = xpost_array_get(ctx, imgdata, 0);
    if (xpost_object_get_type(row) != stringtype)
        return typecheck;
    width = row.comp_.sz;
    rb = (width + 7) / 8;

    buf = malloc((size_t)rb);
    if (!buf)
        return VMerror;
    for (iy = 0; iy < height; iy++)
    {
        const unsigned char *p;

        row = xpost_array_get(ctx, imgdata, iy);
        if (xpost_object_get_type(row) != stringtype
            || (integer)row.comp_.sz != width)
        {
            free(buf);
            return typecheck;
        }
        p = (unsigned char *)xpost_string_get_pointer(ctx, row);
        memset(buf, 0, (size_t)rb);
        for (ix = 0; ix < width; ix++)
            if (p[ix] < 128)
                buf[ix / 8] |= 0x80 >> (ix % 8);
        if (_emit_write(ctx, f, buf, (size_t)rb) < 0)
        {
            free(buf);
            return ioerror;
        }
    }
    free(buf);
    return 0;
}

/* The raster walk shared by the rgb emitters: each row's three
   component planes interleave into three bytes per pixel, written a
   row at a time. Emitting from PostScript costs several string
   operations per pixel, which dominates page output time. */
static
int _write_rgb_raster(Xpost_Context *ctx,
                      Xpost_Object imgdata,
                      Xpost_File *f,
                      int width,
                      int height)
{
    Xpost_Object row;
    unsigned char *buf;
    int iy, ix;

    buf = malloc((size_t)width * 3);
    if (!buf)
        return VMerror;
    for (iy = 0; iy < height; iy++)
    {
        unsigned char *pl[3];
        int rw, ret;

        row = xpost_array_get(ctx, imgdata, iy);
        ret = _rgb_planes(ctx, row, 0, pl, &rw);
        if (ret || rw != width)
        {
            free(buf);
            return ret ? ret : typecheck;
        }
        for (ix = 0; ix < width; ix++)
        {
            buf[ix * 3]     = pl[0][ix];
            buf[ix * 3 + 1] = pl[1][ix];
            buf[ix * 3 + 2] = pl[2][ix];
        }
        if (_emit_write(ctx, f, buf, (size_t)width * 3) < 0)
        {
            free(buf);
            return ioerror;
        }
    }
    free(buf);
    return 0;
}

/* Checks a stream can be written and reads the shape of the image that
   is to go to it, so a caller writing rows one at a time settles both
   once rather than per row. */
static
int _rgb_raster_target(Xpost_Context *ctx,
                       Xpost_Object imgdata,
                       Xpost_Object F,
                       Xpost_File **f,
                       int *width,
                       int *height)
{
    Xpost_Object row;

    if (!xpost_file_get_status(ctx->lo, F))
        return ioerror;
    if (!xpost_object_is_writeable(ctx, F))
        return invalidaccess;
    *f = xpost_file_get_file_pointer(ctx->lo, F);

    *height = imgdata.comp_.sz;
    if (*height == 0)
        return rangecheck;
    row = xpost_array_get(ctx, imgdata, 0);
    return _rgb_planes(ctx, row, 0, NULL, width);
}

/* Emit the raster bytes alone. What frames them is the class's own
   header and tail, written either side of this by the one page writer
   every raster class puts its page out through (data/image.ps). */
static
int _writergbrows(Xpost_Context *ctx,
                  Xpost_Object imgdata,
                  Xpost_Object F)
{
    Xpost_File *f;
    int width, height, ret;

    ret = _rgb_raster_target(ctx, imgdata, F, &f, &width, &height);
    if (ret)
        return ret;
    return _write_rgb_raster(ctx, imgdata, f, width, height);
}

/* --- the image blitter -----------------------------------------------
   The fast path for a sampled image: axis-aligned, rectangular clip, a
   device that keeps rows. Each sample's run of device columns is resolved
   once and stated either into those rows or as one call to the device's own
   rectangle fill, with the colour pipeline baked into tables before the
   loop rather than walked per sample. */


/* decode one interleaved normalized sample row to native colour,
   one r,g,b triple per pixel (grey rides in all three), through the
   same tables the direct path uses */
static void
_blit_decode_row(const unsigned char *src, unsigned char *const *planes,
                 int w, int ncomp,
                 const unsigned char *lut, unsigned char *const dlut[4],
                 const unsigned char *tlut,
                 const unsigned char *tlr, const unsigned char *tlg,
                 const unsigned char *tlb,
                 int cmyk, int nat, int *out)
{
    int x, c;

#define DECSAMP(x, c) (planes ? planes[c][x] : src[(x) * ncomp + (c)])
    for (x = 0; x < w; x++)
    {
        int r = 0, g = 0, b = 0;

        if (lut)
        {
            const unsigned char *e = lut + DECSAMP(x, 0) * nat;

            if (nat == 3) { r = e[0]; g = e[1]; b = e[2]; }
            else r = g = b = e[0];
        }
        else
        {
            int v[4] = { 0, 0, 0, 0 };

            for (c = 0; c < ncomp; c++)
                v[c] = dlut[c][DECSAMP(x, c)];
            if (cmyk)
            {
                r = 255 - (v[0] + v[3] > 255 ? 255 : v[0] + v[3]);
                g = 255 - (v[1] + v[3] > 255 ? 255 : v[1] + v[3]);
                b = 255 - (v[2] + v[3] > 255 ? 255 : v[2] + v[3]);
            }
            else
            {
                r = v[0];
                g = ncomp > 1 ? v[1] : v[0];
                b = ncomp > 2 ? v[2] : v[0];
            }
            if (nat == 3)
            {
                if (tlr)
                {
                    r = tlr[r]; g = tlg[g]; b = tlb[b];
                }
                else
                {
                    r = tlut[r]; g = tlut[g]; b = tlut[b];
                }
            }
            else
                r = g = b = tlut[(r * 30 + g * 59 + b * 11) / 100];
        }
        out[x * 3] = r;
        out[x * 3 + 1] = g;
        out[x * 3 + 2] = b;
    }
#undef DECSAMP
}

/* Where the pixels a blit works out end up.
 *
 * A device that keeps its page as rows the interpreter can see is
 * written into: the row a device row names is taken up once and each
 * run of columns is stored into it. A device that keeps its raster in
 * a buffer of its own has no such rows, and is painted through the
 * rectangle fill it declares -- one call per run of columns, carrying
 * the columns the row write would have covered.
 *
 * The sampling is the same either way. Which pixels a sample covers,
 * which colour it comes to and which columns a mask or a clip leaves
 * are all worked out above this, so the two routes are the tail of one
 * writer rather than two writers that would round their own ways.
 *
 * What a span costs differs, and follows from the route rather than
 * hiding inside it: a row write covers a run of columns at the cost of
 * the bytes, and a fill call costs a call. The interpolated path blends
 * per device pixel, so its runs are one column each and the second
 * route pays a call for every pixel it paints.
 */
struct _blit_out
{
    Xpost_Context *ctx;
    /* the rows a device keeps, where it keeps any */
    Xpost_Object rows;
    int haverows;
    /* the device the fill is asked of, where it keeps none, and the
       compiled rectangle fill it declares */
    Xpost_Object devdic;
    unsigned int fillrect;
    /* the same fill, bound for calling without the operand stack. Null
       where the device's method is not one that can be: the stack path
       is then taken, and the page is the same either way */
    Xpost_Op_Func fillfp;
    /* how many components that fill takes, which is the device's
       colour space and not the shape of any row */
    int ncomp;
    int devw;
    /* the rows hold three planes rather than one grey byte */
    int rgbrows;
    /* the screen a device that thresholds every grey stores through */
    const unsigned char *htc;
    int htw, hth;
    /* the row in hand, where the rows are written into */
    unsigned char *rowp[3];
};

/* Take up the device row about to be painted. Answers 0 in *paint where
   the device holds no pixel over it: the page shows the ground there,
   and an image reaching such a row is dropped where every other mark
   that reaches it is. */
static int
_blit_out_row(struct _blit_out *o, int dy, int *paint)
{
    Xpost_Object row;
    int ret, rw;

    *paint = 1;
    if (!o->haverows)
        return 0;
    row = xpost_array_get(o->ctx, o->rows, dy);
    if (o->rgbrows)
    {
        ret = _rgb_planes(o->ctx, row, 1, o->rowp, &rw);
        if (ret)
            return ret;
        if (rw == 0)
        {
            *paint = 0;
            return 0;
        }
    }
    else
    {
        if (row.comp_.sz == 0)
        {
            *paint = 0;
            return 0;
        }
        ret = _gray_row(o->ctx, row, 1, &o->rowp[0], &rw);
        if (ret)
            return ret;
    }
    if (rw < o->devw)
        return rangecheck;
    return 0;
}

/* Paint columns [dx0, dx1) of one device row in one colour: the three
   channels where the device paints in three, the grey where it paints
   in one.
 *
 * Through the device's own fill, where there are no rows, the colour is
 * handed over as the components that method takes -- numbers in [0,1],
 * which it folds to the channel it stores. Each is given at the middle
 * of the byte the sampling arrived at, that being the value the fold
 * answers with exactly that byte whatever scale the device keeps its
 * channels at. A device that thresholds what it stores does it in that
 * fill, which is where the screen it thresholds through is; the cell
 * below is the one a device written into by row states, and belongs to
 * this write.
 */
static int
_blit_out_span(struct _blit_out *o, int dy, int dx0, int dx1,
               int r, int g, int b, int gray)
{
    int dx;

    if (dx0 < 0)
        dx0 = 0;
    if (dx1 > o->devw)
        dx1 = o->devw;
    if (dx1 <= dx0)
        return 0;

    if (!o->haverows)
    {
        Xpost_Context *ctx = o->ctx;

        Xpost_Object a[8];
        int n;

#define COMP(v) xpost_real_cons((real)(((v) + 0.5) / 255.0))
        if (o->ncomp == 3)
        {
            a[0] = COMP(r);
            a[1] = COMP(g);
            a[2] = COMP(b);
            n = 3;
        }
        else
        {
            a[0] = COMP(gray);
            n = 1;
        }
#undef COMP
        /* the contract's rectangle is an inclusive span, so a run of n
           columns on one row is w = n-1 and h = 0 */
        a[n++] = xpost_int_cons(dx0);
        a[n++] = xpost_int_cons(dy);
        a[n++] = xpost_int_cons(dx1 - dx0 - 1);
        a[n++] = xpost_int_cons(0);
        a[n++] = o->devdic;

        /* One span of an image is one of these, so what the call costs is
           paid once per sample the image puts down. The operands are
           built here and their shapes are the ones the method was bound
           against, so the stack they would be matched on carries nothing
           the caller does not already hold. */
        if (o->fillfp)
            return xpost_operator_call_direct(ctx, o->fillfp, n, a);
        {
            int i;

            for (i = 0; i < n; i++)
                xpost_stack_push(ctx->lo, ctx->os, a[i]);
        }
        return xpost_operator_exec(ctx, o->fillrect);
    }

    for (dx = dx0; dx < dx1; dx++)
    {
        if (o->rgbrows)
        {
            o->rowp[0][dx] = (unsigned char)r;
            o->rowp[1][dx] = (unsigned char)g;
            o->rowp[2][dx] = (unsigned char)b;
        }
        else if (o->htc)
            o->rowp[0][dx] = xpost_dev_ht_ink(xpost_dev_ht_level(gray / 255.0),
                                              o->htc, o->htw, o->hth, dx, dy);
        else
            o->rowp[0][dx] = (unsigned char)gray;
    }
    return 0;
}

/* collect the resolved clip spans overlapping one device row as
   x-intervals; shared by the stepped and interpolated writers */
static int
_blit_row_spans(Xpost_Context *ctx, Xpost_Object cspans, int ncspans,
                int dy, double ivl[512][2], int *nivl)
{
    int q;
    double t;

    *nivl = 0;
    for (q = 0; q < ncspans && *nivl < 512; q++)
    {
        double qx0, qy0, qx1, qy1;
        Xpost_Object e;
#define QGET(i, into) do { \
        e = xpost_array_get(ctx, cspans, q * 4 + (i)); \
        if (xpost_object_get_type(e) == realtype) into = e.real_.val; \
        else if (xpost_object_get_type(e) == integertype) into = e.int_.val; \
        else return typecheck; \
    } while (0)
        QGET(0, qx0); QGET(1, qy0); QGET(2, qx1); QGET(3, qy1);
#undef QGET
        if (qy0 > qy1) { t = qy0; qy0 = qy1; qy1 = t; }
        if (qx0 > qx1) { t = qx0; qx0 = qx1; qx1 = t; }
        if (qy0 < dy + 1 && qy1 > dy)
        {
            ivl[*nivl][0] = qx0;
            ivl[*nivl][1] = qx1;
            (*nivl)++;
        }
    }
    return 0;
}

/* --- the page, and the band loop under it ----------------------------
   A page may arrive whole or a band at a time, and the device does not
   choose: it declares what a row costs and whether it will take bands, and
   the machinery above decides. These are the calls that move a band's rows
   and hand a finished page to the class's own Emit. */

/* blitdict  .blitrow  -
   write one image row into the page of the device the dictionary names.
   The dictionary carries either the device raster (rows: the ImgData
   array, three colour planes or grey bytes per the rgbrows flag) or,
   for a device that keeps its raster in a buffer of its own, that
   device itself (dev), whose compiled rectangle fill is then called
   once per run of columns;
   the axis-aligned image-to-device mapping (xoff xscale yoff yscale),
   the clip rectangle (cx0 cy0 cx1 cy1), the normalized sample row
   (buf, one byte per sample, ncomp samples per pixel, row index y of
   w pixels), the colour tables -- lut: a full 256-entry table of
   native bytes for one-component spaces with everything baked in;
   else dluts: per-component decode tables and tlut: the transfer,
   applied after conversion (cmyk converts by additive complement) --
   and the masks: mbits, one bit per mask sample high-order first in
   rows of mrowb bytes (set = leave unpainted) over the grid of mw by
   mh samples covering the page area the image's w by h samples cover,
   and mranges, raw min,max pairs (a pixel inside every range is left
   unpainted). Pixels cover device pixels by the any-part-of-pixel
   rule, the high edge exclusive, which is the rule the rectangle
   fills cover them by.

   It is reachable as a function too (xpost_dev_generic.h), so that a
   page played back from a record writes its image rows through this
   same writer rather than a second one that would round its own way. */
double xpost_dev_dict_number(Xpost_Context *ctx, Xpost_Object dict,
                             Xpost_Object key, double dflt)
{
    Xpost_Object o = xpost_dict_get(ctx, dict, key);
    int t = xpost_object_get_type(o);

    if (t == integertype || t == realtype)
        return xpost_object_number(o);
    return dflt;
}

/* Finishes a page that is being given up rather than emitted: an open
   stream is written out to its last row and closed, and the device's
   private storage is reclaimed either way. Called where a device is
   destroyed with a page still in hand, so it cannot report failure to
   anyone. */
void xpost_dev_page_retire(void *priv, Xpost_Dev_Band *band, int has_raster,
                           int height, const Xpost_Dev_Page_Codec *codec)
{
    if (band->open && has_raster)
    {
        if (!codec->write_rows(priv, height - 1))
            (void)codec->stream_finish(priv);
    }
    codec->reclaim(priv);
}

/* A Destroy method entire, for a device that writes a page through a
   codec. What the retirement is handed is read out of the instance
   before it runs, because retiring may be what lets the raster go, and
   the cleared struct is stored back so a repeated Destroy is a no-op. */
int
xpost_dev_page_destroy_call(Xpost_Context *ctx, Xpost_Object devdic,
                            Xpost_Object nameprivate,
                            void *priv, size_t privsz,
                            const Xpost_Dev_Page_Codec *codec)
{
    Xpost_Object privatestr;
    int has_rows, height;

    if (!xpost_dev_private_get(ctx, devdic, nameprivate,
                               &privatestr, priv, privsz))
        return undefined;

    has_rows = codec->raster(priv) != NULL;
    height = codec->height(priv);
    xpost_dev_page_retire(priv, codec->band(priv), has_rows, height, codec);

    if (!xpost_dev_private_put(ctx, privatestr, priv, privsz))
        return VMerror;
    return 0;
}

/* An Emit method entire, for a device that lends its raster rather than
   writing it. The instance is recorded again ONLY where the offer was
   taken: an offer refused leaves everything as it was, and there is
   nothing to say about it. */
int
xpost_dev_buffer_emit_call(Xpost_Context *ctx, Xpost_Object devdic,
                           Xpost_Object nameprivate,
                           void *priv, size_t privsz,
                           unsigned char *(*raster)(void *),
                           void (*disown)(void *))
{
    Xpost_Object privatestr;
    unsigned char *page;

    if (!xpost_dev_private_get(ctx, devdic, nameprivate,
                               &privatestr, priv, privsz))
        return undefined;

    page = raster(priv);
    if (!page)
        return 0;

    if (xpost_dev_output_buffer_handoff(ctx, page))
    {
        disown(priv);
        if (!xpost_dev_private_put(ctx, privatestr, priv, privsz))
            return VMerror;
    }
    return 0;
}

/* Where a page begins, for the band state. Four flags and no device in
   them; the one that matters is primed, since a page that began without
   clearing it would never have its ground laid again. */
void
xpost_dev_band_page_begin(Xpost_Dev_Band *band)
{
    band->next = 0;
    band->primed = 0;
    band->open = 0;
    band->done = 0;
}

/* A run of rows held to the raster the device has. */
int
xpost_dev_band_clamp_rows(const Xpost_Dev_Band *band, int *from, int *to)
{
    if (*from < 0)
        *from = 0;
    if (*to > band->bufrows - 1)
        *to = band->bufrows - 1;
    return *from <= *to;
}

/* A MoveBand method entire. Which rows the raster next stands for is
   the band machinery's; what a device adds is beginning a page, laying
   the ground and clearing the run, and the order of those three is the
   part that must not be written twice. */
int
xpost_dev_page_moveband_call(Xpost_Context *ctx, Xpost_Object devdic,
                             Xpost_Object nameprivate,
                             void *priv, size_t privsz,
                             Xpost_Object top, Xpost_Object rows,
                             const Xpost_Dev_Page_Codec *codec)
{
    Xpost_Object privatestr;
    Xpost_Dev_Band *band;

    if (!xpost_dev_private_get(ctx, devdic, nameprivate,
                               &privatestr, priv, privsz))
        return undefined;
    if (!codec->raster(priv))
        return 0;

    band = codec->band(priv);
    xpost_dev_band_move(ctx, devdic, band, codec->height(priv),
                        xpost_dev_num_to_int(top),
                        xpost_dev_num_to_int(rows));
    /* rows put in front of a device whose page is finished are the next
       page's: this is where a job's second page begins */
    if (band->done && band->rows > 0)
        codec->page_begin(priv);
    /* and where a device holding every row of the page lays the ground
       over the rows no run of them is going to reach */
    if (band->whole && band->rows > 0 && !band->primed)
        codec->prime(ctx, devdic, priv);
    codec->clear(priv, band->top - band->origin,
                 band->top - band->origin + band->rows - 1);

    if (!xpost_dev_private_put(ctx, privatestr, priv, privsz))
        return VMerror;
    return 0;
}

/* An Emit method entire. The order of these steps is the part that
   must not be written twice: a device that stood down before recording
   the instance would go on writing a page it had already refused, and
   one that recorded before emitting would lose the hand-off. */
int
xpost_dev_page_emit_call(Xpost_Context *ctx, Xpost_Object devdic,
                         Xpost_Object nameprivate,
                         void *priv, size_t privsz,
                         const Xpost_Dev_Page_Codec *codec)
{
    Xpost_Object privatestr;
    unsigned char *raster;
    int ret, handed = 0;

    if (!xpost_dev_private_get(ctx, devdic, nameprivate,
                               &privatestr, priv, privsz))
        return undefined;

    /* a released instance has no raster to put out */
    raster = codec->raster(priv);
    if (!raster)
        return 0;

    ret = xpost_dev_page_emit(ctx, devdic, priv, codec->band(priv),
                              codec->file(priv), codec->height(priv),
                              raster, codec, &handed);
    if (handed)
        codec->disown(priv);

    if (!xpost_dev_private_put(ctx, privatestr, priv, privsz))
        return VMerror;

    return ret;
}

/* One page out through a device's codec, whether the device holds the
   whole page or is handed it a band at a time. The two arrangements
   differ in when a stream is opened and when it is finished, and this
   is the whole of that difference; a codec supplies the writing and
   knows neither. */
int xpost_dev_page_emit(Xpost_Context *ctx, Xpost_Object devdic,
                        void *priv, Xpost_Dev_Band *band, FILE **file,
                        int height, unsigned char *raster,
                        const Xpost_Dev_Page_Codec *codec,
                        int *handed_off)
{
    int banded, last, ret;

    *handed_off = 0;

    /* A device holding its page a band at a time says so in the state it
       writes as it runs, which is where what a page does to a device is
       kept (data/device.ps) rather than on the device itself. */
    banded = xpost_object_get_type(
                 xpost_dict_get(ctx,
                                xpost_dict_get(ctx, devdic, namedotstate),
                                xpost_name_cons(ctx, ".bandpage")))
             != invalidtype;

    if (!banded)
    {
        /* Each call is a page of its own. A stream still open here
           belonged to a page that was arriving in bands and stopped
           doing so, and it is given up rather than left behind: what
           follows opens another. */
        if (band->open)
        {
            codec->stream_drop(priv);
            if (*file)
            {
                xpost_device_page_close(*file);
                *file = NULL;
            }
        }
        codec->page_begin(priv);
    }
    else if (band->done)
        return 0;

    /* a device that writes the page in several passes over it cannot
       give a row up before the last band has been painted */
    if (banded && codec->defers_rows && codec->defers_rows(priv)
        && band->rows != 0)
        return 0;

    /* the last of the page's rows this call can give the file */
    last = (!banded || band->rows == 0)
         ? height - 1
         : band->top + band->rows - 1;

    ret = 0;
    if (!band->open)
    {
        /* The file this page goes to, opened here because its name was
           settled for this page: the page machinery puts the settled
           name on the device and this is the method that writes the
           page (tests/check-page-output.sh). */
        *file = xpost_device_page_open(ctx, devdic);
        if (!*file)
        {
            XPOST_LOG_ERR("cannot open the file this page is written to");
            ret = ioerror;
        }
        else
            ret = codec->stream_open(ctx, devdic, priv);
    }
    if (!ret)
        ret = codec->write_rows(priv, last);
    if (!ret && band->next >= height)
        ret = codec->stream_finish(priv);
    /* The file goes back with the page that was being written through
       it, finished or refused; a page still being written keeps it for
       the call that brings the next band. */
    if (band->done && *file)
    {
        xpost_device_page_close(*file);
        *file = NULL;
    }
    if (ret)
        return ret;

    /* pass data back to client application; the raster then belongs to
       the client, which gives the block it sits in back through the
       release entry point, so Destroy must leave it alone from here on.
       Only a finished page is handed over, and only a device holding
       every row of it has one -- which is what Create gives a device an
       embedder asked a raster of. */
    if (band->done && raster
        && xpost_dev_output_buffer_handoff(ctx, raster))
        *handed_off = 1;

    return 0;
}

/* --- device parameters -----------------------------------------------
   A driver states the knobs it reads beside the code that reads them, and
   this is what setpagedevice and the command line reach them through. An
   unknown key or an out-of-range value is refused loudly rather than
   ignored. */

/* The default an embedder asks of a compiled writer, outside any run.

   A knob of this kind is a key on the device dictionary: a page-device
   request carries it there, and a driver that finds none takes the
   default its class carries. An embedder speaks after the context is
   made and before the driver's class exists -- the class is built on
   first use -- so what it asked is recorded among the host's settings,
   the one dictionary that holds what the invocation decided, and the
   class takes it up as it is installed
   (xpost_dev_class_option_default). A class already installed when the
   embedder speaks is written as well, so the call means the same thing
   whenever it is made. A program's own request still overrides either
   way: the request's keys are written over the class's entries as an
   instance is copied from it (data/device.ps). */
int xpost_dev_option_default(Xpost_Context *ctx,
                             const char *key,
                             Xpost_Object v,
                             const char *classname,
                             const char *altclassname)
{
    Xpost_Object h;
    const char *names[2];
    int i;
    int ret;

    if (!ctx)
        return undefined;
    if (xpost_object_get_type(ctx->globalprivatedict) != dicttype)
        return undefined;
    h = xpost_context_job_member(ctx, ".hostdict");
    if (xpost_object_get_type(h) != dicttype)
        return undefined;
    ret = xpost_dict_put(ctx, h, xpost_name_cons(ctx, key), v);
    if (ret)
        return ret;

    names[0] = classname;
    names[1] = altclassname;
    for (i = 0; i < 2; i++)
    {
        Xpost_Object cd;

        if (!names[i])
            continue;
        if (xpost_object_get_type(ctx->privatedict) != dicttype)
            continue;
        cd = xpost_dict_get(ctx, ctx->privatedict,
                            xpost_name_cons(ctx, names[i]));
        if (xpost_object_get_type(cd) != dicttype)
            continue;
        ret = xpost_dict_put(ctx, cd, xpost_name_cons(ctx, key), v);
        if (ret)
            return ret;
    }
    return 0;
}

/* The other half of xpost_dev_option_default: as a driver's class is
   installed, take up the default the embedder recorded, so that every
   instance copied from the class carries it. A setting nobody made, or
   one of a kind no knob is -- the knobs are numbers and words -- leaves
   the class saying what it said. */
int xpost_dev_class_option_default(Xpost_Context *ctx,
                                   Xpost_Object classdic,
                                   const char *key)
{
    Xpost_Object v = xpost_context_host_setting(ctx, key);

    if (xpost_object_get_type(v) != integertype
     && xpost_object_get_type(v) != nametype)
        return 0;
    return xpost_dict_put(ctx, classdic, xpost_name_cons(ctx, key), v);
}

/* The tuning knobs of every compiled writer this build carries, in one
   roster: each driver states its own beside the reads that give the
   knob meaning (and is held to those reads by tests/check-device-facts.sh),
   and this gathers what the build compiled in, so a knob of a driver
   the build left out is not offered. The command line's -p switch is
   the caller: it refuses a key not named here, and holds a value to
   the row's range or vocabulary, before anything is rendered. */
/* The knobs the PDF writer reads off its device dictionary, stated
   beside the reads that give them meaning and held to those reads by
   tests/check-device-facts.sh.

   An image reaches a written document under one of two encodings. Flate
   gives back exactly the samples it was handed; DCT gives back an
   approximation of them and is a great deal smaller, which is why the
   convention for a photograph is DCT and why a page carrying one is
   otherwise ten times the size it needs to be. Neither is right for
   every image, so the choice is the caller's: auto takes DCT for a
   continuous-tone image and Flate for anything else, and the two named
   values settle it either way.

   The quality is libjpeg's, nought to a hundred, and reaches the
   encoder as the DCTEncode filter's QFactor, which is that number over
   a hundred. It means nothing under Flate, where nothing is lost. */
const Xpost_Dev_Option *xpost_dev_pdf_option_roster(int *count)
{
#ifdef HAVE_LIBJPEG
    static const char *const filterwords[] =
        { "auto", "dct", "flate", NULL };
    static const Xpost_Dev_Option options[] =
    {
        /* No class named: the PDF writer is declared in PostScript and
           its class is sealed, so a default is recorded as a host
           setting and the writer reads it for itself as it is made
           (.hostvalue). A device whose class is a C structure takes the
           other route and has the value written onto the class. */
        { "pdf_image_filter", NULL, NULL, 0, 0, filterwords },
        { "pdf_image_quality", NULL, NULL, 0, 100, NULL }
    };

    *count = (int)(sizeof(options) / sizeof(options[0]));
    return options;
#else
    /* without an encoder there is only the one encoding, and a knob
       that can be set to one value is a knob that should not be offered */
    *count = 0;
    return NULL;
#endif
}

const Xpost_Dev_Option *xpost_dev_option_roster(int *count)
{
    static Xpost_Dev_Option roster[16];
    static int n = -1;

    if (n < 0)
    {
        const Xpost_Dev_Option *part;
        int i, pn;

        n = 0;
        part = xpost_dev_png_option_roster(&pn);
        for (i = 0; i < pn; i++)
            roster[n++] = part[i];
        part = xpost_dev_jpeg_option_roster(&pn);
        for (i = 0; i < pn; i++)
            roster[n++] = part[i];
        part = xpost_dev_pdf_option_roster(&pn);
        for (i = 0; i < pn; i++)
            roster[n++] = part[i];
    }
    *count = n;
    return roster;
}

/* The half of a device's Create that runs before the class procedure
   does. It arranges for the device's own continuation to run after the
   class has made the instance, since neither can be done until the
   other has finished.

   The page's size is handed to the class as operands rather than
   written onto the class: a class is a declaration its instances
   share, and a size left on one is a size the next device made would
   inherit without being asked for it. */
int xpost_dev_create_begin(Xpost_Context *ctx,
                           Xpost_Object width,
                           Xpost_Object height,
                           Xpost_Object classdic,
                           unsigned int cont_opcode)
{
    /* the three the continuation will be called with: it is an operator
       like this one and takes its operands the same way, so they go back
       where they came from rather than being carried in C */
    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);

    /* and the three /.instance takes, which it consumes down to the
       instance it leaves for the continuation above */
    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    xpost_stack_push(ctx->lo, ctx->os, classdic);

    /* the class procedure first, then the device's continuation: pushed
       in the reverse of the order they run in, the execution stack
       being read from the top */
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_operator_cons_opcode(cont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, namedotinstance)))
        return execstackoverflow;

    return 0;
}

/* Which sample of a grid of m samples a point at coordinate u on a
   grid of n samples falls in. The mask covers the page area the image's
   own grid covers, so where a device pixel falls in the image's grid
   says where it falls in the mask's, whatever ratio stands between
   them. Held inside the grid: u comes of dividing by a scale the caller
   states and a coordinate outside the image is a sample at its edge,
   never an index off the bits. */
static int _blit_mask_index(double u, int n, int m)
{
    double v = floor(u * m / n);

    if (!(v >= 0))
        return 0;
    if (v > m - 1)
        return m - 1;
    return (int)v;
}

/* Whether the mask leaves the device pixel at image-space coordinate u
   on mask row mrow unpainted. */
static int _blit_masked(const unsigned char *mbits, int mrowb, int mrow,
                        double u, int w, int mw)
{
    int mx = _blit_mask_index(u, w, mw);

    return mbits[mrow * mrowb + (mx >> 3)] >> (7 - (mx & 7)) & 1;
}

/* Paints one row of a halftoned image: the spans it covers, the
   thresholds they fall against, and the colour each pixel resolves
   to, all read off the dictionary the caller has assembled, which
   is what lets the caller settle them once for the whole image. */
int xpost_dev_blit_row(Xpost_Context *ctx,
                       Xpost_Object dict)
{
    Xpost_Object bufo, luto, dlutso, tluto, mbitso, mrangeso;
    Xpost_Object tro, tgo, tbo;
    Xpost_Object cspans;
    struct _blit_out out;
    unsigned char *plane[4] = { NULL, NULL, NULL, NULL };
    int ncspans = 0, have_cspans = 0, have_planes = 0;
    double ivl[512][2];
    int nivl = 0;
    unsigned char *buf, *lut = NULL, *tlut = NULL, *mbits = NULL;
    unsigned char *tlr = NULL, *tlg = NULL, *tlb = NULL;
    const unsigned char *htc = NULL;
    int htw = 0, hth = 0;
    unsigned char *dlut[4] = { NULL, NULL, NULL, NULL };
    int mranges[8];
    int devw, devh, nat, rgbrows, w, ncomp, y, cmyk, mrowb = 0;
    int h = 0, mw = 0, mh = 0, msame = 1;
    double xoff, xscale, yoff, yscale, cx0, cy0, cx1, cy1;
    double ya, yb, t;
    int dy, x, c, nranges = 0;

#define GETI(name) do { \
        Xpost_Object o_ = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, #name)); \
        if (xpost_object_get_type(o_) != integertype) return typecheck; \
        name = o_.int_.val; \
    } while (0)
#define GETR(name) do { \
        Xpost_Object o_ = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, #name)); \
        if (xpost_object_get_type(o_) == realtype) name = o_.real_.val; \
        else if (xpost_object_get_type(o_) == integertype) name = o_.int_.val; \
        else return typecheck; \
    } while (0)
#define GETB(name) do { \
        Xpost_Object o_ = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, #name)); \
        if (xpost_object_get_type(o_) != booleantype) return typecheck; \
        name = o_.int_.val; \
    } while (0)

    GETI(devw); GETI(devh); GETI(nat); GETI(w); GETI(ncomp); GETI(y);
    GETB(rgbrows); GETB(cmyk);
    /* The component count indexes the per-component tables -- the plane
       pointers, the decode tables, the decoded sample vector and the
       mask range pairs -- and the native count indexes the entries of
       the baked colour table, whose length is checked against it. Both
       are bounded by what those tables hold before anything reads
       through them. The row index multiplies the mask's row stride to
       reach the bits for its row, so it counts forward from the first
       of them. */
    if (ncomp < 1 || ncomp > 4 || nat < 1 || nat > 3 || y < 0)
        return rangecheck;
    GETR(xoff); GETR(xscale); GETR(yoff); GETR(yscale);
    GETR(cx0); GETR(cy0); GETR(cx1); GETR(cy1);
#undef GETI
#undef GETR
#undef GETB

    /* a screening device thresholds every grey written; the caller
       copies the cell into the blit dictionary when the device has
       one. Fetched ahead of both writers so the interpolated path,
       which returns before the stepped one, screens through it too */
    htc = xpost_dev_ht_cell(ctx, dict, &htw, &hth);

    /* Where the pixels go: the rows a device keeps, or the device
       itself where it keeps none. A device named here has to declare a
       compiled rectangle fill, since what runs a method written as a
       procedure is the interpreter and nothing here can call one. */
    memset(&out, 0, sizeof out);
    out.ctx = ctx;
    out.devw = devw;
    out.rgbrows = rgbrows;
    out.ncomp = nat == 3 ? 3 : 1;
    out.htc = htc;
    out.htw = htw;
    out.hth = hth;
    out.rows = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "rows"));
    if (xpost_object_get_type(out.rows) == arraytype)
        out.haverows = 1;
    else
    {
        Xpost_Object fr;

        out.devdic = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "dev"));
        if (xpost_object_get_type(out.devdic) != dicttype)
            return typecheck;
        fr = xpost_dict_get(ctx, out.devdic, nameFillRect);
        if (xpost_object_get_type(fr) != operatortype)
            return typecheck;
        out.fillrect = fr.mark_.padw;
        /* Bind the fill for calling without the operand stack, against a
           sample of exactly what every span will pass. One span is one
           call and an image is one span per sample it puts down, so what
           the ordinary call protocol costs is paid per sample; knowing
           the method here costs it once. A device whose method this
           cannot be done for answers null and is called the ordinary
           way. */
        {
            Xpost_Object sample[8];
            int ns = 0;

            if (out.ncomp == 3)
            {
                sample[ns++] = xpost_real_cons(0.0);
                sample[ns++] = xpost_real_cons(0.0);
                sample[ns++] = xpost_real_cons(0.0);
            }
            else
                sample[ns++] = xpost_real_cons(0.0);
            sample[ns++] = xpost_int_cons(0);
            sample[ns++] = xpost_int_cons(0);
            sample[ns++] = xpost_int_cons(0);
            sample[ns++] = xpost_int_cons(0);
            sample[ns++] = out.devdic;
            out.fillfp = xpost_operator_direct(ctx, out.fillrect, ns, sample);
            /* The other route, asked for. Both paint the same page --
               the operands are the same objects and the method is the
               same function -- and the only way to hold that claim is to
               make one run produce each and compare the two, which is
               what tests/run-fill-route-test.sh does. It is a control
               for a check and not a setting: a run that does not ask for
               it takes the route it would have taken. */
            if (getenv("XPOST_NODIRECT"))
                out.fillfp = NULL;
        }
        /* the screen is the device's own and it thresholds through it
           in the fill below, so nothing is thresholded here */
        out.htc = NULL;
    }
    /* planar sources deliver one row buffer per component */
    {
        Xpost_Object bufso = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "bufs"));
        if (xpost_object_get_type(bufso) == arraytype)
        {
            if (bufso.comp_.sz < (unsigned int)ncomp)
                return rangecheck;
            for (c = 0; c < ncomp; c++)
            {
                Xpost_Object b = xpost_array_get(ctx, bufso, c);
                if (xpost_object_get_type(b) != stringtype
                 || b.comp_.sz < (unsigned int)w)
                    return rangecheck;
                plane[c] = (unsigned char *)xpost_string_get_pointer(ctx, b);
            }
            have_planes = 1;
            buf = NULL;
        }
    }
    if (!have_planes)
    {
        bufo = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "buf"));
        /* the interleaved row holds ncomp samples per pixel, compared
           against the row width by division: the width is the caller's
           and its product with the component count need not fit the
           type the samples are indexed with */
        if (xpost_object_get_type(bufo) != stringtype
         || bufo.comp_.sz / (unsigned int)ncomp < (unsigned int)w)
            return rangecheck;
        buf = (unsigned char *)xpost_string_get_pointer(ctx, bufo);
    }
#define SAMPLE(x, c) (have_planes ? plane[c][x] : buf[(x) * ncomp + (c)])

    luto = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "lut"));
    if (xpost_object_get_type(luto) == stringtype)
    {
        if (luto.comp_.sz < (unsigned int)(256 * nat))
            return rangecheck;
        lut = (unsigned char *)xpost_string_get_pointer(ctx, luto);
    }
    else
    {
        dlutso = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "dluts"));
        tluto = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "tlut"));
        if (xpost_object_get_type(dlutso) != arraytype
         || dlutso.comp_.sz < (unsigned int)ncomp
         || xpost_object_get_type(tluto) != stringtype
         || tluto.comp_.sz < 256)
            return typecheck;
        for (c = 0; c < ncomp && c < 4; c++)
        {
            Xpost_Object d = xpost_array_get(ctx, dlutso, c);
            if (xpost_object_get_type(d) != stringtype || d.comp_.sz < 256)
                return typecheck;
            dlut[c] = (unsigned char *)xpost_string_get_pointer(ctx, d);
        }
        tlut = (unsigned char *)xpost_string_get_pointer(ctx, tluto);
        tro = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "tlutr"));
        tgo = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "tlutg"));
        tbo = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "tlutb"));
        if (xpost_object_get_type(tro) == stringtype && tro.comp_.sz >= 256
         && xpost_object_get_type(tgo) == stringtype && tgo.comp_.sz >= 256
         && xpost_object_get_type(tbo) == stringtype && tbo.comp_.sz >= 256)
        {
            tlr = (unsigned char *)xpost_string_get_pointer(ctx, tro);
            tlg = (unsigned char *)xpost_string_get_pointer(ctx, tgo);
            tlb = (unsigned char *)xpost_string_get_pointer(ctx, tbo);
        }
    }

    /* The mask, on the grid of mw by mh samples it states rather than
       on the image's: the two need not have the same resolution, and a
       mask read onto the image's grid would keep only the sample
       nearest each of the image's. A mask of no samples is no mask,
       which is what a mask dictionary of no width or no height
       describes. */
    mbitso = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "mbits"));
    if (xpost_object_get_type(mbitso) == stringtype && mbitso.comp_.sz)
    {
        Xpost_Object o;

#define GETI(name) do { \
        Xpost_Object o_ = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, #name)); \
        if (xpost_object_get_type(o_) != integertype) return typecheck; \
        name = o_.int_.val; \
    } while (0)
        GETI(h); GETI(mw); GETI(mh);
#undef GETI
        o = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "mrowb"));
        if (xpost_object_get_type(o) != integertype)
            return typecheck;
        mrowb = o.int_.val;
        /* Both grids are divided into, so neither may be empty, and the
           mask holds one row of mrowb bytes per mask row: the stride
           has to carry a row of mw bits and the bits have to run to mh
           of those rows. The length is divided by the row count rather
           than the count multiplied by the stride, which holds for
           every stride rather than only those whose product with the
           count fits the type. */
        if (w < 1 || h < 1 || mw < 1 || mh < 1)
            return rangecheck;
        if (mrowb < mw / 8 + (mw % 8 ? 1 : 0)
         || (unsigned int)mrowb > mbitso.comp_.sz / (unsigned int)mh)
            return rangecheck;
        mbits = (unsigned char *)xpost_string_get_pointer(ctx, mbitso);
        msame = mw == w && mh == h;
    }
    /* an optional clip region: flat quads x0 y0 x1 y1 in device
       space, the resolved rectangle spans of a non-rectangular clip;
       column runs intersect the quads overlapping the device row */
    {
        Xpost_Object cs = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "cspans"));
        if (xpost_object_get_type(cs) == arraytype)
        {
            cspans = cs;
            ncspans = cs.comp_.sz / 4;
            have_cspans = 1;
        }
    }

    mrangeso = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "mranges"));
    if (xpost_object_get_type(mrangeso) == arraytype)
    {
        nranges = mrangeso.comp_.sz;
        if (nranges > 8)
            return rangecheck;
        for (c = 0; c < nranges; c++)
        {
            Xpost_Object o = xpost_array_get(ctx, mrangeso, c);
            if (xpost_object_get_type(o) != integertype)
                return typecheck;
            mranges[c] = o.int_.val;
        }
    }

    /* Interpolate: between the previous sample row and this one, each
       device pixel takes the bilinear blend of the four surrounding
       decoded colours, so a magnified image ramps between its samples
       instead of stepping with them. The band between the two row
       centres belongs to this call; the first row also owns the band
       from its outer edge, the last also the band to its own. The
       masks decide per device pixel from its nearest sample -- the
       stepped rule -- while the colour still blends, and the resolved
       clip spans clamp writes as they do on the stepped path;
       reductions keep the stepped path. */
    {
        Xpost_Object io = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "interp"));

        if (xpost_object_get_type(io) == booleantype && io.int_.val
         && fabs(xscale) >= 1.0 && fabs(yscale) >= 1.0 && w > 0)
        {
            Xpost_Object po = xpost_dict_get(ctx, dict,
                xpost_name_cons(ctx, have_planes ? "prevs" : "prev"));
            Xpost_Object lasto = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "last"));
            int lastrow = xpost_object_get_type(lasto) == booleantype && lasto.int_.val;
            unsigned char *prevsamp = NULL;
            unsigned char *prevplane[4] = { NULL, NULL, NULL, NULL };
            int prevok = 0;

            if (have_planes)
            {
                if (xpost_object_get_type(po) == arraytype
                 && po.comp_.sz >= (unsigned int)ncomp)
                {
                    prevok = 1;
                    for (c = 0; c < ncomp; c++)
                    {
                        Xpost_Object b = xpost_array_get(ctx, po, c);
                        if (xpost_object_get_type(b) != stringtype
                         || b.comp_.sz < (unsigned int)w)
                            { prevok = 0; break; }
                        prevplane[c] = (unsigned char *)
                            xpost_string_get_pointer(ctx, b);
                    }
                }
            }
            else if (xpost_object_get_type(po) == stringtype
                  && po.comp_.sz / (unsigned int)ncomp >= (unsigned int)w)
            {
                prevsamp = (unsigned char *)xpost_string_get_pointer(ctx, po);
                prevok = 1;
            }

            if (prevok)
            {
                int *cols = malloc(sizeof(int) * (size_t)w * 6);
                int *pc, *cc;
                double xe0, xe1, bandlo[2], bandhi[2];
                int band, nband;

                if (!cols)
                    return VMerror;
                pc = cols;
                cc = cols + w * 3;
                _blit_decode_row(prevsamp, have_planes ? prevplane : NULL,
                                 w, ncomp, lut, dlut, tlut,
                                 tlr, tlg, tlb, cmyk, nat, pc);
                _blit_decode_row(buf, have_planes ? plane : NULL,
                                 w, ncomp, lut, dlut, tlut,
                                 tlr, tlg, tlb, cmyk, nat, cc);

                bandlo[0] = y == 0 ? yoff : yoff + (y - 0.5) * yscale;
                bandhi[0] = yoff + (y + 0.5) * yscale;
                nband = 1;
                if (lastrow)
                {
                    bandlo[1] = bandhi[0];
                    bandhi[1] = yoff + (y + 1) * yscale;
                    nband = 2;
                }

                xe0 = xoff; xe1 = xoff + w * xscale;
                if (xe0 > xe1) { t = xe0; xe0 = xe1; xe1 = t; }
                if (xe0 < cx0) xe0 = cx0;
                if (xe1 > cx1) xe1 = cx1;
                if (xe0 < 0) xe0 = 0;
                if (xe1 > devw) xe1 = devw;

                for (band = 0; band < nband; band++)
                {
                    double blo = bandlo[band], bhi = bandhi[band];
                    double lo = blo < bhi ? blo : bhi;
                    double hi = blo < bhi ? bhi : blo;
                    int *rowa = band ? cc : pc;
                    int *rowb = cc;

                    /* Hold the row span inside the page and finite before
                       it becomes a loop bound: an origin past INT_MAX
                       would wrap the cast to INT_MIN and spin the loop
                       across the int range. Rows off the page paint
                       nothing, so this changes no output. */
                    if (!(lo >= 0)) lo = 0;
                    if (lo > devh) lo = devh;
                    if (!(hi >= 0)) hi = 0;
                    if (hi > devh) hi = devh;

                    for (dy = (int)floor(lo); dy < hi; dy++)
                    {
                        double v;
                        int dx, rret, paint;

                        if (dy < 0 || dy >= devh)
                            continue;
                        if (dy + 0.5 < cy0 || dy + 0.5 >= cy1)
                            continue;
                        v = bhi != blo ? (dy + 0.5 - blo) / (bhi - blo) : 0.0;
                        if (v < 0.0 || v >= 1.0)
                            continue;
                        if (have_cspans)
                        {
                            int ret = _blit_row_spans(ctx, cspans, ncspans,
                                                      dy, ivl, &nivl);
                            if (ret)
                                { free(cols); return ret; }
                            if (nivl == 0)
                                continue;
                        }
                        rret = _blit_out_row(&out, dy, &paint);
                        if (rret)
                            { free(cols); return rret; }
                        if (!paint)
                            continue;
                        {
                        double sxstep = 1.0 / xscale;
                        double sxv = ((int)floor(xe0) + 0.5 - xoff) / xscale - 0.5;

                        for (dx = (int)floor(xe0); dx < xe1;
                             dx++, sxv += sxstep)
                        {
                            double f;
                            int i0, i1, k, px[3];

                            if (dx < 0 || dx >= devw)
                                continue;
                            if (have_cspans)
                            {
                                int q, hit = 0;
                                for (q = 0; q < nivl; q++)
                                    if (dx + 1 > ivl[q][0] && dx < ivl[q][1])
                                        { hit = 1; break; }
                                if (!hit)
                                    continue;
                            }
                            if (mbits || nranges)
                            {
                                /* the nearest sample decides, as the
                                   stepped rule would paint it */
                                int msy = band == 0 && v < 0.5 && y > 0
                                        ? y - 1 : y;
                                int xm = (int)floor((dx + 0.5 - xoff) / xscale);

                                if (xm < 0) xm = 0;
                                if (xm > w - 1) xm = w - 1;
                                /* the mask decides from where the pixel
                                   falls on the mask's own grid, which
                                   is the nearest sample when the two
                                   grids are one */
                                if (mbits)
                                {
                                    int mx = _blit_mask_index(
                                        (dx + 0.5 - xoff) / xscale, w, mw);
                                    int my = _blit_mask_index(
                                        (dy + 0.5 - yoff) / yscale, h, mh);

                                    if (mbits[my * mrowb + (mx >> 3)]
                                        >> (7 - (mx & 7)) & 1)
                                        continue;
                                }
                                if (nranges)
                                {
                                    int inside = 1;

                                    for (k = 0; k < ncomp; k++)
                                    {
                                        int sv = msy == y
                                            ? (int)SAMPLE(xm, k)
                                            : (int)(have_planes
                                                ? prevplane[k][xm]
                                                : prevsamp[xm * ncomp + k]);
                                        if (sv < mranges[2 * k]
                                         || sv > mranges[2 * k + 1])
                                            { inside = 0; break; }
                                    }
                                    if (inside)
                                        continue;
                                }
                            }
                            i0 = (int)floor(sxv);
                            f = sxv - i0;
                            if (i0 < 0) { i0 = 0; f = 0.0; }
                            if (i0 > w - 1) { i0 = w - 1; f = 0.0; }
                            i1 = i0 + 1 > w - 1 ? w - 1 : i0 + 1;
                            for (k = 0; k < 3; k++)
                            {
                                double a = rowa[i0 * 3 + k] * (1.0 - f)
                                         + rowa[i1 * 3 + k] * f;
                                double bl = rowb[i0 * 3 + k] * (1.0 - f)
                                          + rowb[i1 * 3 + k] * f;
                                double m_ = a * (1.0 - v) + bl * v;

                                px[k] = (int)(m_ + 0.5);
                                if (px[k] < 0) px[k] = 0;
                                if (px[k] > 255) px[k] = 255;
                            }
                            {
                                int sret = _blit_out_span(&out, dy, dx, dx + 1,
                                                          px[0], px[1], px[2],
                                                          px[0]);
                                if (sret)
                                    { free(cols); return sret; }
                            }
                        }
                        }
                    }
                }
                free(cols);
                return 0;
            }
        }
    }

    ya = yoff + y * yscale;
    yb = yoff + (y + 1) * yscale;
    if (ya > yb) { t = ya; ya = yb; yb = t; }
    if (ya < cy0) ya = cy0;
    if (yb > cy1) yb = cy1;
    /* Hold both bounds inside the page and finite before the cast below.
       A device-space origin past INT_MAX -- reachable from a large CTM
       scale -- would wrap (int)floor to INT_MIN and spin this loop across
       the whole int range, uninterruptibly; the tests here comparing
       against 0 also reject a non-finite bound, which no ordering would.
       Rows outside the page paint nothing, so clamping changes no output. */
    if (!(ya >= 0)) ya = 0;
    if (ya > devh) ya = devh;
    if (!(yb >= 0)) yb = 0;
    if (yb > devh) yb = devh;

    for (dy = (int)floor(ya); dy < yb; dy++)
    {
        int rret, paint, mrow = 0;

        if (dy < 0)
            continue;
        if (have_cspans)
        {
            int ret = _blit_row_spans(ctx, cspans, ncspans, dy, ivl, &nivl);
            if (ret)
                return ret;
            if (nivl == 0)
                continue;
        }
        rret = _blit_out_row(&out, dy, &paint);
        if (rret)
            return rret;
        if (!paint)
            continue;

        /* Which mask row this device row falls on. The two grids being
           one, that is the sample row being painted; otherwise the row
           the device row's own centre lands on, so that several device
           rows under one sample row each take the mask row above
           them. */
        if (mbits)
            mrow = msame
                 ? (y > mh - 1 ? mh - 1 : y)
                 : _blit_mask_index((dy + 0.5 - yoff) / yscale, h, mh);

        for (x = 0; x < w; x++)
        {
            double xa, xb;
            int r = 0, g = 0, b = 0, gray = 0;

            if (mbits && msame)
            {
                int bit = mbits[mrow * mrowb + (x >> 3)] >> (7 - (x & 7)) & 1;
                if (bit)
                    continue;
            }
            if (nranges)
            {
                int inside = 1;
                for (c = 0; c < ncomp; c++)
                {
                    int v = SAMPLE(x, c);
                    if (v < mranges[2 * c] || v > mranges[2 * c + 1])
                    {
                        inside = 0;
                        break;
                    }
                }
                if (inside)
                    continue;
            }

            if (lut)
            {
                const unsigned char *e = lut + SAMPLE(x, 0) * nat;
                if (nat == 3) { r = e[0]; g = e[1]; b = e[2]; }
                else gray = e[0];
            }
            else
            {
                int v[4] = {0};
                for (c = 0; c < ncomp; c++)
                    v[c] = dlut[c][SAMPLE(x, c)];
                if (cmyk)
                {
                    r = 255 - (v[0] + v[3] > 255 ? 255 : v[0] + v[3]);
                    g = 255 - (v[1] + v[3] > 255 ? 255 : v[1] + v[3]);
                    b = 255 - (v[2] + v[3] > 255 ? 255 : v[2] + v[3]);
                }
                else
                {
                    r = v[0];
                    g = ncomp > 1 ? v[1] : v[0];
                    b = ncomp > 2 ? v[2] : v[0];
                }
                if (nat == 3)
                {
                    if (tlr)
                    {
                        r = tlr[r]; g = tlg[g]; b = tlb[b];
                    }
                    else
                    {
                        r = tlut[r]; g = tlut[g]; b = tlut[b];
                    }
                }
                else
                    gray = tlut[(r * 30 + g * 59 + b * 11) / 100];
            }

            xa = xoff + x * xscale;
            xb = xoff + (x + 1) * xscale;
            if (xa > xb) { t = xa; xa = xb; xb = t; }
            if (xa < cx0) xa = cx0;
            if (xb > cx1) xb = cx1;
            if (xa < 0) xa = 0;
            if (xb > devw) xb = devw;
            {
                int iv, niv = have_cspans ? nivl : 1;

                for (iv = 0; iv < niv; iv++)
                {
                    double sa = xa, sb = xb;
                    int sret;

                    if (have_cspans)
                    {
                        if (ivl[iv][0] > sa) sa = ivl[iv][0];
                        if (ivl[iv][1] < sb) sb = ivl[iv][1];
                    }
                    /* the columns a walk from floor(sa) up to but not
                       including sb covers, stated as an interval so
                       that a device written into by row and a device
                       painted through its own fill are handed the same
                       run rather than each working one out */
                    if (!mbits || msame)
                    {
                        sret = _blit_out_span(&out, dy, (int)floor(sa),
                                              (int)ceil(sb), r, g, b, gray);
                        if (sret)
                            return sret;
                    }
                    else
                    {
                        /* A mask on a grid of its own steps across the
                           sample rather than with it, so the sample's
                           columns are handed over as the runs the mask
                           lets through rather than as one span. */
                        int p = (int)floor(sa), e = (int)ceil(sb);

                        if (p < 0)
                            p = 0;
                        if (e > devw)
                            e = devw;
                        while (p < e)
                        {
                            int s0 = p;

                            while (p < e
                                && !_blit_masked(mbits, mrowb, mrow,
                                                 (p + 0.5 - xoff) / xscale,
                                                 w, mw))
                                p++;
                            if (p > s0)
                            {
                                sret = _blit_out_span(&out, dy, s0, p,
                                                      r, g, b, gray);
                                if (sret)
                                    return sret;
                            }
                            while (p < e
                                && _blit_masked(mbits, mrowb, mrow,
                                                (p + 0.5 - xoff) / xscale,
                                                w, mw))
                                p++;
                        }
                    }
                }
            }
        }
    }
    return 0;
}
#undef SAMPLE

/* --- what the PDF writer needs from C --------------------------------
   The per-byte work of a document that the PostScript side would pay too
   much for: compression, number formatting, and the accumulator a document
   is built up in, which lives outside virtual memory so a restore cannot
   tear a half-written file. */


/* --- a raster the vector writer can embed ------------------------------
   What a reader of an SVG document is required to have is PNG and JPEG,
   and nothing else (SVG 1.1 4.6). So the writer offers PNG where this
   build has the library that writes it, and falls back to the format it
   has always written where it does not: a build without the library
   writes a document some readers show the picture in, rather than no
   document at all.

   Written through the library rather than by hand. The format is a
   deflate stream between two checksums, which is little to write and
   easy to write subtly wrong, and a mistake in it is a file that some
   readers accept and others reject.
 */

#ifdef HAVE_LIBPNG
static void _png_write_cb(png_structp png, png_bytep data, png_size_t len)
{
    Xpost_String_Buffer *b = (Xpost_String_Buffer *)png_get_io_ptr(png);

    if (xpost_strbuf_append(b, data, len))
        png_error(png, "cannot hold the encoded image");
}

static void _png_flush_cb(png_structp png)
{
    (void)png;
}
#endif

/* rows w h .pngencode  ->  [ str ... ] true
                        ->  false
   Each row is w*3 bytes of red, green and blue, or w*4 with an alpha
   after each triple, the first row the top one; which it is follows from
   how long the rows are. What comes back is a whole PNG file in pieces, since a string is
   only as long as the object width allows and a page-sized raster is
   longer; every piece but the last is a multiple of three, so a caller
   encoding them to base64 one at a time gets what encoding the whole
   would have given. A build with no PNG library answers false. */
static
int _pngencode(Xpost_Context *ctx, Xpost_Object arr,
               Xpost_Object wo, Xpost_Object ho)
{
#ifdef HAVE_LIBPNG
    Xpost_String_Buffer out;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep *rowp = NULL;
    int chans = 3;
    int w = wo.int_.val, h = ho.int_.val;
    int y, ret = 0;

    if (w <= 0 || h <= 0 || arr.comp_.sz < (unsigned int)h)
        return rangecheck;
    if ((unsigned long)w > 0x7fffffffUL / 3)
        return limitcheck;

    memset(&out, 0, sizeof out);

    /* the rows are handed over as pointers into virtual memory, which
       nothing moves for the length of this call */
    rowp = malloc((size_t)h * sizeof *rowp);
    if (!rowp)
        return VMerror;
    /* Three bytes a pixel, or four where the caller has an alpha to
       carry: a masked image says which of its samples are to be dropped,
       and a reader of the document is required to read the alpha but has
       no way to be told a range of values to drop. Which it is follows
       from how long the rows are, so a caller that has no mask passes
       what it always did. */
    {
        Xpost_Object s0 = xpost_array_get(ctx, arr, 0);
        if (xpost_object_get_type(s0) == stringtype &&
            s0.comp_.sz >= (unsigned int)(w * 4))
            chans = 4;
    }

    for (y = 0; y < h; y++)
    {
        Xpost_Object s = xpost_array_get(ctx, arr, y);

        if (xpost_object_get_type(s) != stringtype
            || s.comp_.sz < (unsigned int)(w * chans))
        {
            free(rowp);
            return typecheck;
        }
        rowp[y] = (png_bytep)xpost_string_get_pointer(ctx, s);
    }

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png)
    {
        free(rowp);
        return VMerror;
    }
    info = png_create_info_struct(png);
    if (!info)
    {
        png_destroy_write_struct(&png, NULL);
        free(rowp);
        return VMerror;
    }
    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_write_struct(&png, &info);
        xpost_strbuf_free(&out);
        free(rowp);
        return VMerror;
    }

    png_set_write_fn(png, &out, _png_write_cb, _png_flush_cb);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
                 chans == 4 ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_write_image(png, rowp);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(rowp);

    {
        size_t pos = 0;
        int nchunks = (int)((out.len + 47999) / 48000);
        Xpost_Object result;
        int k;

        if (nchunks == 0)
            nchunks = 1;
        result = xpost_object_cvlit(xpost_array_cons(ctx, nchunks));
        for (k = 0; k < nchunks; k++)
        {
            size_t chunk = out.len - pos;

            if (chunk > 48000)
                chunk = 48000;
            ret = xpost_array_put(ctx, result, k,
                                  xpost_object_cvlit(
                                      xpost_string_cons(ctx, (unsigned int)chunk,
                                                        out.s + pos)));
            if (ret)
            {
                xpost_strbuf_free(&out);
                return ret;
            }
            pos += chunk;
        }
        xpost_strbuf_free(&out);
        xpost_stack_push(ctx->lo, ctx->os, result);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    }
    return 0;
#else
    (void)arr; (void)wo; (void)ho;
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
#endif
}

#ifdef HAVE_LIBJPEG
# include <stdio.h>
# include <jpeglib.h>
/* Where libjpeg puts the bytes it makes: into the same growable buffer
   the rest of this file assembles output in, so the encoded image is one
   run of bytes by the time it is chunked. */
typedef struct
{
    struct jpeg_destination_mgr pub;
    Xpost_String_Buffer *out;
    JOCTET buf[8192];
    int failed;
} Pdf_Dct_Dest;

static void _dct_dest_init(j_compress_ptr cinfo)
{
    Pdf_Dct_Dest *d = (Pdf_Dct_Dest *)cinfo->dest;

    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = sizeof(d->buf);
}

static boolean _dct_dest_empty(j_compress_ptr cinfo)
{
    Pdf_Dct_Dest *d = (Pdf_Dct_Dest *)cinfo->dest;

    if (xpost_strbuf_append(d->out, (const char *)d->buf, sizeof(d->buf)))
        d->failed = 1;
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = sizeof(d->buf);
    return TRUE;
}

static void _dct_dest_term(j_compress_ptr cinfo)
{
    Pdf_Dct_Dest *d = (Pdf_Dct_Dest *)cinfo->dest;
    size_t n = sizeof(d->buf) - d->pub.free_in_buffer;

    if (n && xpost_strbuf_append(d->out, (const char *)d->buf, n))
        d->failed = 1;
}
#endif

/* .dctcompress  rows w h ncomp quality  .  chunks true
                                         .  rows false

   The concatenation of an array of interleaved eight-bit rows, encoded
   as a JPEG stream and handed back as strings a PostScript string can
   hold, for the writer to put in a DCTDecode image. Answers false with
   the rows unchanged where this build has no encoder or the image is
   not one this encoding takes, so the caller writes it the other way
   rather than not at all.

   Lossy, which is the whole of why it is worth having: the samples come
   back close rather than equal, and a photograph costs a tenth of what
   it costs kept exact. What is lost is the caller's to decide, through
   the quality this takes. */
static
int _dctcompress(Xpost_Context *ctx, Xpost_Object arr, Xpost_Object wo,
                 Xpost_Object ho, Xpost_Object nco, Xpost_Object qo)
{
#ifdef HAVE_LIBJPEG
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    Pdf_Dct_Dest dest;
    Xpost_String_Buffer out;
    Xpost_Object result;
    int w = wo.int_.val, h = ho.int_.val, nc = nco.int_.val;
    int quality = qo.int_.val;
    int i, n, ret;

    n = arr.comp_.sz;
    if (w <= 0 || h <= 0 || n <= 0 || (nc != 1 && nc != 3 && nc != 4))
    {
        xpost_stack_push(ctx->lo, ctx->os, arr);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    memset(&out, 0, sizeof out);
    memset(&dest, 0, sizeof dest);
    dest.out = &out;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    dest.pub.init_destination = _dct_dest_init;
    dest.pub.empty_output_buffer = _dct_dest_empty;
    dest.pub.term_destination = _dct_dest_term;
    cinfo.dest = (struct jpeg_destination_mgr *)&dest;
    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = nc;
    cinfo.in_color_space = nc == 1 ? JCS_GRAYSCALE
                         : nc == 4 ? JCS_CMYK : JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    /* The samples arrive as strings, and where they are divided is the
       caller's business rather than the image's: one string a row, or
       one string for the whole image, or anything between. So they are
       read as one run of bytes and cut into scanlines here. */
    {
        size_t total = 0, want = (size_t)w * (size_t)nc * (size_t)h;
        size_t at = 0;
        int e = 0;
        unsigned eoff = 0;
        unsigned char *scan;

        for (i = 0; i < n; i++)
        {
            Xpost_Object s = xpost_array_get(ctx, arr, i);

            if (xpost_object_get_type(s) != stringtype)
            {
                total = 0;
                break;
            }
            total += s.comp_.sz;
        }
        if (total < want)
        {
            jpeg_destroy_compress(&cinfo);
            xpost_strbuf_free(&out);
            xpost_stack_push(ctx->lo, ctx->os, arr);
            xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
            return 0;
        }
        scan = (unsigned char *)malloc((size_t)w * (size_t)nc);
        if (!scan)
        {
            jpeg_destroy_compress(&cinfo);
            xpost_strbuf_free(&out);
            return VMerror;
        }
        jpeg_start_compress(&cinfo, TRUE);
        for (i = 0; i < h; i++)
        {
            size_t need = (size_t)w * (size_t)nc, got = 0;
            JSAMPROW row = (JSAMPROW)scan;

            while (got < need && e < n)
            {
                Xpost_Object s = xpost_array_get(ctx, arr, e);
                unsigned avail = s.comp_.sz - eoff;
                size_t take = need - got;

                if (take > avail)
                    take = avail;
                memcpy(scan + got, xpost_string_get_pointer(ctx, s) + eoff, take);
                got += take;
                eoff += (unsigned)take;
                if (eoff >= s.comp_.sz)
                {
                    e++;
                    eoff = 0;
                }
            }
            (void)at;
            jpeg_write_scanlines(&cinfo, &row, 1);
        }
        free(scan);
        i = h;
    }
    if (i < h)
    {
        jpeg_abort_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        xpost_strbuf_free(&out);
        xpost_stack_push(ctx->lo, ctx->os, arr);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    if (dest.failed)
    {
        xpost_strbuf_free(&out);
        return VMerror;
    }
    {
        size_t pos = 0;
        int nchunks = (int)((out.len + 65534) / 65535);

        if (nchunks == 0)
            nchunks = 1;
        result = xpost_object_cvlit(xpost_array_cons(ctx, nchunks));
        for (i = 0; i < nchunks; i++)
        {
            size_t chunk = out.len - pos;

            if (chunk > 65535)
                chunk = 65535;
            ret = xpost_array_put(ctx, result, i,
                                  xpost_object_cvlit(
                                      xpost_string_cons(ctx, chunk, out.s + pos)));
            if (ret)
            {
                xpost_strbuf_free(&out);
                return ret;
            }
            pos += chunk;
        }
    }
    xpost_strbuf_free(&out);
    xpost_stack_push(ctx->lo, ctx->os, result);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
#else
    (void)wo; (void)ho; (void)nco; (void)qo;
    xpost_stack_push(ctx->lo, ctx->os, arr);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
#endif
}

/* Deflate the concatenation of an array of strings, returning the result as an
   array of <=65535-byte strings (the PostScript string limit) plus a boolean
   that is true when compression happened. Used by the pdfwrite device to write
   a FlateDecode content stream. Without zlib the input is returned unchanged
   with false, so the caller falls back to uncompressed output. */
static
int _flatecompress(Xpost_Context *ctx, Xpost_Object arr)
{
#ifdef HAVE_ZLIB
    z_stream strm;
    Xpost_String_Buffer out;
    unsigned char buf[16384];
    Xpost_Object result;
    int i, n, ret;

    memset(&out, 0, sizeof out);
    memset(&strm, 0, sizeof strm);
    if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK)
        return unregistered;

    n = arr.comp_.sz;
    for (i = 0; i <= n; i++)   /* the final pass (i == n) flushes */
    {
        int flush = (i == n) ? Z_FINISH : Z_NO_FLUSH;
        if (i < n)
        {
            Xpost_Object s = xpost_array_get(ctx, arr, i);
            /* the elements are the strings whose bytes are compressed;
               one that is not a string has no bytes to read, so it is
               refused rather than followed through a pointer taken from
               whatever it is */
            if (xpost_object_get_type(s) != stringtype)
            {
                xpost_strbuf_free(&out);
                deflateEnd(&strm);
                return typecheck;
            }
            strm.next_in = (unsigned char *)xpost_string_get_pointer(ctx, s);
            strm.avail_in = s.comp_.sz;
        }
        else
        {
            strm.next_in = NULL;
            strm.avail_in = 0;
        }
        do
        {
            size_t have;
            strm.next_out = buf;
            strm.avail_out = sizeof buf;
            ret = deflate(&strm, flush);
            if (ret == Z_STREAM_ERROR)
            {
                xpost_strbuf_free(&out);
                deflateEnd(&strm);
                return unregistered;
            }
            have = sizeof buf - strm.avail_out;
            if (have && xpost_strbuf_append(&out, buf, have))
            {
                xpost_strbuf_free(&out);
                deflateEnd(&strm);
                return VMerror;
            }
        } while (strm.avail_out == 0);
    }
    deflateEnd(&strm);

    {
        size_t pos = 0;
        int nchunks = (int)((out.len + 65534) / 65535);
        if (nchunks == 0)
            nchunks = 1;
        result = xpost_object_cvlit(xpost_array_cons(ctx, nchunks));
        for (i = 0; i < nchunks; i++)
        {
            size_t chunk = out.len - pos;
            if (chunk > 65535)
                chunk = 65535;
            /* cvlit: strings and arrays are executable by default, and this
               binary content must be written, not executed */
            ret = xpost_array_put(ctx, result, i,
                                  xpost_object_cvlit(
                                      xpost_string_cons(ctx, chunk, out.s + pos)));
            if (ret)
            {
                xpost_strbuf_free(&out);
                return ret;
            }
            pos += chunk;
        }
    }
    xpost_strbuf_free(&out);
    xpost_stack_push(ctx->lo, ctx->os, result);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
#else
    xpost_stack_push(ctx->lo, ctx->os, arr);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
#endif
}

/* Write a decimal integer, returning its length. The digits come off the
   negative side of zero: the most negative value of the type has no
   negation in it, and a formatter that negates first takes the remainder
   of that non-result and writes bytes below '0', which are not digits. */
static int _pdf_fmt_long(char *o, long v)
{
    char t[24];
    int n = 0, neg = 0, len = 0;
    if (v > 0) v = -v; else if (v < 0) neg = 1;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' - (v % 10)); v /= 10; }
    if (neg) o[len++] = '-';
    while (n) o[len++] = t[--n];
    return len;
}

/* The range a number is brought into before it is folded to the integer
   type below: a double outside the type has no value in it and the
   conversion is undefined. The whole-number branch casts directly, so
   1e9 is the bound every platform's long holds; the fractional branch
   scales by 10000 first, so it carries 1e5. Either bound is a hundred
   times any page a consumer will draw, and a coordinate or colour level
   beyond one describes nothing. */
#define PDF_NUM_MAX      1.0e9
#define PDF_NUM_FRAC_MAX 1.0e5

/* write a number: an integer when integral, else up to `digits` decimals
   (scale = 10^digits) with trailing zeros trimmed, never exponential.
   round(v*scale) avoids binary-float print noise. */
static int _fmt_num_prec(char *o, double v, long scale, int digits)
{
    if (v != v)
        v = 0.0;
    else if (v > PDF_NUM_MAX)
        v = PDF_NUM_MAX;
    else if (v < -PDF_NUM_MAX)
        v = -PDF_NUM_MAX;

    if (v == trunc(v) || v > PDF_NUM_FRAC_MAX || v < -PDF_NUM_FRAC_MAX)
        return _pdf_fmt_long(o, (long)v);
    else
    {
        long m = (long)round(v * (double)scale);
        long ip, fp;
        int len = 0;
        if (m < 0) { o[len++] = '-'; m = -m; }
        ip = m / scale;
        fp = m % scale;
        len += _pdf_fmt_long(o + len, ip);
        if (fp)
        {
            while (fp % 10 == 0) { fp /= 10; digits--; }
            o[len++] = '.';
            len += digits;
            { int i = len; while (fp) { o[--i] = (char)('0' + fp % 10); fp /= 10; }
              while (i > len - digits) o[--i] = '0'; }
        }
        return len;
    }
}

/* pdfwrite content accumulator. Held in the device's /Private string and grown
   with malloc/realloc, so the accumulated content lives outside the
   save/restore-managed memory file (like the raster device's pixel buffer) and
   survives a `restore` executed by the job before showpage/Emit. The current
   page's marks are not part of virtual memory, so `restore` must not discard
   them; storing them in the device dict would let it. */
/* one registered separation colour space: the dedup key (the separation
   name as given), the colour-space array body for the page's /ColorSpace
   resource (missing only the function reference, which depends on object
   numbering known at Emit), and the complete function object body
   (dictionary plus stream) defining the tint transform */
typedef struct
{
    char *name;
    size_t namelen;
    char *csdef;
    size_t csdeflen;
    char *func;
    size_t funclen;
} Pdf_Sep;

/* What the content stream already carries of the graphics state.
   A PDF content stream is a state machine: a colour, a line width, a cap,
   a join, a miter limit or an ExtGState selection stands until something
   replaces it, so writing one whose value is already in force adds bytes
   and changes nothing on the page. Each slot holds the exact operator
   text last written for it, and a writer emits only where the text it
   would write differs. Text and not the value it came from: what the
   stream carries is bytes, and comparing the bytes cannot drift from the
   formatting that produced them.

   The levels are the q/Q stack. `q` saves the whole record and `Q`
   brings it back, because that is what those operators do to the state
   the record describes: a value written inside a q and compared against
   after the matching Q would suppress an operator the consumer had
   already undone. An empty slot is one whose value is unknown, which
   costs an operator and never a wrong page -- so every case this cannot
   follow (a stack deeper than the levels here, a Q with no q, an
   operator text longer than a slot) empties slots rather than guessing.

   It hangs off the accumulator rather than living in the device
   dictionary: what it describes is the content, which is not virtual
   memory, and a `restore` that rolled the record back while the content
   stood would leave it claiming bytes the stream does not carry. */
#define PDF_GS_DEPTH 32

/* How deep captures may nest. A description placed inside a description
   inside a description is ordinary; past this the innermost is painted
   where it stands, which is the right page and the route a device
   without any of this takes. */
#define PDF_SUB_DEPTH 8
#define PDF_GS_TEXT  64

typedef struct
{
    unsigned char len;              /* 0: nothing is known of this slot */
    char text[PDF_GS_TEXT];
} Pdf_Gs_Slot;

typedef struct
{
    Pdf_Gs_Slot slot[XPOST_PDF_GS_SLOTS];
} Pdf_Gs_Level;

typedef struct
{
    Pdf_Gs_Level level[PDF_GS_DEPTH];
    int depth;      /* the level in force */
    int over;       /* q's past the deepest level, still to be matched */
    /* One state operator's text, while a writer is building it. It is
       built here and not in a PostScript string because a writer
       appends its operator a piece at a time and every paint builds
       one: a string per piece is virtual memory taken and given back on
       the hot path of every mark on the page. */
    Xpost_String_Buffer pend;
    /* The substream in progress, and whether there is one. A pattern's
       cell is a content stream of its own (PDF 8.7.3.1) built with the
       marking methods that build the page, so it is captured out of the
       page's own buffer; the record has to answer for the cell while
       that lasts. A stream nothing has been written into carries
       nothing, so the slots are emptied for the capture and brought
       back after: a writer that suppressed an operator against what the
       page carries would leave the cell relying on state the cell's own
       stream never states.

       Captures nest, and they nest as a stack: what is opened last is
       always closed first, because a description placed inside another
       finishes before the one placing it. So the record is a stack of
       them and not a single one.

       Whether a caller MAY nest is the caller's own question, and it
       says so when it opens one. A form may: it carries the box and the
       placement its description is written against, so a description
       captured inside another is composed by what is written down. A
       pattern may not: its space is fixed against the page when the
       pattern is instantiated, so a cell captured inside a cell would be
       written in the outer cell's coordinates while its own space says
       otherwise. The pattern machinery refuses such a cell itself; the
       flag here is what keeps that true wherever a capture is reached
       from. */
    int insub;                  /* how many are open */
    size_t subat[PDF_SUB_DEPTH];  /* where in the content each starts */
    int subdepth[PDF_SUB_DEPTH];
    int subover[PDF_SUB_DEPTH];
    Pdf_Gs_Slot subslot[PDF_SUB_DEPTH][XPOST_PDF_GS_SLOTS];
} Pdf_Gs;

/* An image the page's content draws. The content says /Im<i> Do and the
   page's resources have to define /Im<i>, so the samples are kept here,
   outside virtual memory, beside the content that draws them. Kept in
   the device's dictionary instead, a save taken after the page began and
   restored before it ended would take the samples away and leave the
   content drawing an object the document does not carry.

   The samples arrive as one string a row and are held as one run of
   bytes, which is how they are written: a row is a place the caller
   splits them, not a boundary the object has. */
typedef struct
{
    int w, h, nc;
    int interp;
    int mask, haspol, pol;
    int hasmbits, mbits;
    int ndec;
    double dec[16];
    int hasmrng, nmrng;
    double mrng[16];
    char *rows;
    size_t rowslen;
} Pdf_Img;

/* A description the page's content places: a form, or a tiling cell.
   The content says /Fm<i> Do or names /P<i> and the page's resources
   have to define it, so the description is kept here, beside the content
   that places it, and not in the device's dictionary where a restore
   would take it.

   The chunks a caller files are held as one run of bytes. They are
   written out one after another and nothing reads them apart, so where
   the caller divided them is not a property of the description. That
   also makes the comparison that recognises a description already filed
   a comparison of two runs of bytes. */
typedef struct
{
    double bb[4];
    double mat[6];
    double xs, ys;
    int tt;
    int ispat;      /* a cell: xs, ys, tt and mat are part of what it is */
    char *body;
    size_t bodylen;
    int obj;        /* the object it was written as, 0 until it is written */
    int written;
} Pdf_Res;

/* An object the page will carry, written out when the page ends.

   Most of what this writer files can wait until then to be turned into
   bytes, because what it is made of is numbers the record can hold. A
   shading cannot: its description is a tree -- a function, and a
   stitching function's own subfunctions under it -- and a tree of
   values is what the record must not hold, since the values are the
   program's and the program may change or discard them.

   So a shading is written when it is painted, into bytes, and the bytes
   wait here. Its object numbers are taken then too, from the counter in
   the shared record, which a restore does not wind back; only the file
   offsets wait for the page end, because only then is it known where in
   the file the object lands. */
typedef struct
{
    int num;
    char *body;
    size_t len;
} Pdf_Obj;

/* A glyph outline the content has drawn, and how often.

   Filing a description costs an object and a name in every page's
   resources, and saves the outline's own bytes at each occurrence after
   the first. So a glyph drawn over and over pays for itself many times
   and one drawn once never does: MEASURED, filing every glyph took a
   sixteenfold weight off a page of text, and put a waterfall of one
   alphabet at forty sizes -- where nothing repeats -- up by more than
   half again.

   Hence the count, and the bytes beside it. The bytes are kept so that
   two different outlines which happen to hash alike cannot be taken for
   one: a wrong description placed is a wrong glyph on the page, which
   is not a trade worth making for the memory. */
typedef struct
{
    unsigned hash;
    char *body;
    size_t len;
    int count;
    int form;     /* the description once filed, or -1 */
    int gen;      /* which record that number was minted against */
} Pdf_Gly;

/* An ExtGState the page's content selects. The content says /GS<key> gs
   and the page's resources have to define /GS<key>, so the two are one
   fact and are kept in the one place: here, outside virtual memory,
   beside the content they belong to. Held in the device's dictionary
   instead, a save taken after the page began and restored before it
   ended would take the definition away and leave the selection. */
typedef struct
{
    int key;     /* the number the content selects it by */
    int op;      /* overprint */
    int mode;    /* overprint mode */
} Pdf_Op;

/* A base font the content sets text in, and everything the page has to
   say about it: which codes were shown, what each one is called, and
   how wide the show took it to be.

   The widths are the ones the marks were actually made with. A reader
   places the glyphs of a string by them, so declaring what was used is
   what lets the content say the string and nothing more -- the pen
   arrives where this engine put it because it is told the same widths
   this engine advanced by. */
typedef struct
{
    char base[48];            /* the base font's PostScript name */
    unsigned char used[256];
    short width[256];         /* thousandths of the em */
    char *gname[256];         /* what each code is called, or NULL */
    int emitted;              /* a run naming it reached the page */
} Pdf_Fnt;

/* The run of text being gathered. A run holds while the font, the
   matrix and the pen all continue from the glyph before -- the pen
   because a reader advances it by the declared width, so a glyph the
   show put anywhere else has to start a run of its own. */
typedef struct
{
    int open;
    int fnt;
    double mat[4];            /* text space to the page, without the origin */
    double x, y;              /* where the run starts */
    double nx, ny;            /* where its next glyph must land */
    int inked;                /* whether any glyph in it marks the page */
    Xpost_String_Buffer codes;
} Pdf_Txt;

typedef struct
{
    Xpost_String_Buffer content;
    Pdf_Sep *seps;
    int nseps;
    int sepcap;
    Pdf_Op *ops;
    int nops;
    int opcap;
    Pdf_Img *imgs;
    int nimgs;
    int imgcap;
    Pdf_Res *forms;
    int nforms;
    int formcap;
    Pdf_Res *pats;
    int npats;
    int patcap;
    int formgen;   /* which document the filed descriptions belong to */
    Pdf_Gly *glys;
    int nglys;
    int glycap;
    Pdf_Obj *objs;
    int nobjs;
    int objcap;
    int *shs;
    int nshs;
    int shcap;
    int *sephash;    /* name index: a separation's position + 1, 0 empty */
    int sephashcap;  /* a power of two, or 0 when the index is absent */
    Pdf_Gs *gs;      /* what the stream carries, or NULL: then nothing is known */
    Pdf_Fnt *fnts;
    int nfnts;
    int fntcap;
    Pdf_Txt txt;
} Pdf_Acc;

/* Load/store the accumulator struct via the device's /Private string. The raw
   memory accessors record no save/restore backup, so neither the struct nor the
   malloc'd buffer it points at is reverted by `restore`; the pointer is set once
   at device creation and never re-homed into virtual memory.

   The block is asked for as content rather than as a device's instance
   state, which is the other thing a device keeps under this key. The
   two are told apart by what they were issued as and not by how wide
   they are: a driver's private struct that happens to be the width of
   the accumulator would otherwise load as one, and the first member the
   release then frees is a pointer read out of the middle of it. */
static int _pdf_acc_get(Xpost_Context *ctx, Xpost_Object devdic,
                        Xpost_Object *priv, Pdf_Acc *a)
{
    void *block;

    *priv = xpost_dict_get(ctx, devdic, namepdfPrivate);
    block = xpost_handle_block_of(ctx, *priv, devdic,
                                  XPOST_HANDLE_CONTENT, sizeof(*a));
    if (!block)
        return 0;
    memcpy(a, block, sizeof(*a));
    return 1;
}

/* The accumulator a vector device builds its page in, read from and
   written back to the handle block it is kept in rather than to
   virtual memory -- which is what lets the collector reclaim it
   without reaching into virtual memory at all. */
static XPOST_MUST_CHECK int
_pdf_acc_put(Xpost_Context *ctx, Xpost_Object priv, Pdf_Acc *a)
{
    void *block = xpost_handle_block(ctx, priv,
                                     XPOST_HANDLE_CONTENT, sizeof(*a));

    if (!block)
        return 0;
    memcpy(block, a, sizeof(*a));
    return 1;
}

/* Give up the buffer and the separations the accumulator names. Called
   from the collector with the block the accumulator is kept in, so it
   touches nothing in virtual memory. A device the run retired has given
   both up already and leaves this nothing to do. */
static void _pdf_acc_reclaim(void *block)
{
    Pdf_Acc *a = block;
    int i;

    xpost_strbuf_free(&a->content);
    if (a->gs)
        xpost_strbuf_free(&a->gs->pend);
    free(a->gs);
    a->gs = NULL;
    for (i = 0; i < a->nseps; i++)
    {
        free(a->seps[i].name);
        free(a->seps[i].csdef);
        free(a->seps[i].func);
    }
    free(a->seps);
    a->seps = NULL;
    a->nseps = 0;
    a->sepcap = 0;
    free(a->sephash);
    a->sephash = NULL;
    a->sephashcap = 0;
    free(a->ops);
    a->ops = NULL;
    a->nops = 0;
    a->opcap = 0;
    for (i = 0; i < a->nimgs; i++)
        free(a->imgs[i].rows);
    free(a->imgs);
    a->imgs = NULL;
    a->nimgs = 0;
    a->imgcap = 0;
    for (i = 0; i < a->nforms; i++)
        free(a->forms[i].body);
    free(a->forms);
    a->forms = NULL;
    a->nforms = 0;
    a->formcap = 0;
    for (i = 0; i < a->npats; i++)
        free(a->pats[i].body);
    free(a->pats);
    a->pats = NULL;
    a->npats = 0;
    a->patcap = 0;
    for (i = 0; i < a->nobjs; i++)
        free(a->objs[i].body);
    free(a->objs);
    a->objs = NULL;
    a->nobjs = 0;
    a->objcap = 0;
    free(a->shs);
    a->shs = NULL;
    a->nshs = 0;
    a->shcap = 0;
    for (i = 0; i < a->nglys; i++)
        free(a->glys[i].body);
    free(a->glys);
    a->glys = NULL;
    a->nglys = 0;
    a->glycap = 0;
    for (i = 0; i < a->nfnts; i++)
    {
        int c;
        for (c = 0; c < 256; c++)
            free(a->fnts[i].gname[c]);
    }
    free(a->fnts);
    a->fnts = NULL;
    a->nfnts = 0;
    a->fntcap = 0;
    xpost_strbuf_free(&a->txt.codes);
    memset(&a->txt, 0, sizeof a->txt);
}

/* Create the content accumulator and stash it in the device's /Private. Called
   from the device Create method, before any user save/restore. */
static int _pdfinit(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int ret;

    xpost_strbuf_init(&a.content, 4096);
    a.fnts = NULL;
    a.nfnts = 0;
    a.fntcap = 0;
    memset(&a.txt, 0, sizeof a.txt);
    a.seps = NULL;
    a.nseps = 0;
    a.sepcap = 0;
    a.ops = NULL;
    a.nops = 0;
    a.opcap = 0;
    a.imgs = NULL;
    a.nimgs = 0;
    a.imgcap = 0;
    a.forms = NULL;
    a.nforms = 0;
    a.formcap = 0;
    a.pats = NULL;
    a.npats = 0;
    a.patcap = 0;
    a.formgen = 0;
    a.glys = NULL;
    a.nglys = 0;
    a.glycap = 0;
    a.objs = NULL;
    a.nobjs = 0;
    a.objcap = 0;
    a.shs = NULL;
    a.nshs = 0;
    a.shcap = 0;
    a.sephash = NULL;
    a.sephashcap = 0;
    a.gs = NULL;
    /* What this device holds is a buffer, which is not virtual memory: a
       device the run never retires -- one a restore took back, or one
       nothing named by the time a collection came round -- would take
       its buffer with it. This is what gives it up there. */
    ret = xpost_handle_cons(ctx, devdic, namepdfPrivate, &priv,
                            XPOST_HANDLE_CONTENT, sizeof(a),
                            _pdf_acc_reclaim);
    if (ret)
    {
        xpost_strbuf_free(&a.content);
        return ret;
    }
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* append a string's bytes to the accumulator (the marking methods' .put) */
/* Written down where the run is gathered. */
static int _pdf_text_emit(Pdf_Acc *a);

/* Everything that reaches the content goes through here.

   A text object holds the pen and nothing else may be said inside one,
   so whatever is written next closes an open run first. Said in one
   place because there is more than one writer: the PostScript side puts
   operators down, the state machinery writes the operator it has just
   built, a save and a restore write a bracket, a fill writes a path,
   and the glyph machinery appends what it decomposed. A run left open
   while any of them writes past it reaches the stream after that
   writing -- text painted in the colour of whatever was drawn next, or
   written into the page where the description being captured around it
   should have carried it. Both were live defects.
   
   The emitter itself does not come through here: it is what closing a
   run means. */
static int _pdf_content(Pdf_Acc *a, const char *s, size_t n)
{
    if (a->txt.open && _pdf_text_emit(a))
        return -1;
    return xpost_strbuf_append(&a->content, s, n);
}

static int _pdfput(Xpost_Context *ctx, Xpost_Object str, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    ret = _pdf_content(&a, xpost_string_get_pointer(ctx, str), str.comp_.sz);
    if (ret)
        return ret;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* The record of what the stream carries, made if it is not there.
   Every slot starts empty, which is what a stream with nothing written
   into it carries: the first paint writes all of its own state. The
   record is made here rather than at the accumulator's creation because
   giving the accumulator up at the end of a page releases it along with
   the content buffer, and a device painted on afterwards starts the
   buffer again -- the record starts again with it, and an empty record
   describes an empty stream exactly. */
static Pdf_Gs *_pdf_gs_of(Xpost_Context *ctx, Xpost_Object priv, Pdf_Acc *a)
{
    if (!a->gs)
    {
        a->gs = calloc(1, sizeof(*a->gs));
        if (!a->gs)
            return NULL;
        if (!_pdf_acc_put(ctx, priv, a))
        {
            free(a->gs);
            a->gs = NULL;
            return NULL;
        }
    }
    return a->gs;
}

/* Whether a state operator would change what the stream carries, and
   the record of it having been written. See xpost_dev_pdf_state. */
static int _pdf_gs_new(Pdf_Acc *a, int slot, const char *s, size_t n)
{
    Pdf_Gs_Slot *t;

    if (!a->gs || a->gs->over || slot < 0 || slot >= XPOST_PDF_GS_SLOTS)
        return 1;
    t = &a->gs->level[a->gs->depth].slot[slot];
    if (n == 0 || n >= PDF_GS_TEXT)
    {
        /* Longer than the slot describes: the bytes go out and the slot
           forgets, rather than going on to answer for a value it is not
           holding. */
        t->len = 0;
        return 1;
    }
    if (t->len == n && memcmp(t->text, s, n) == 0)
        return 0;
    memcpy(t->text, s, n);
    t->len = (unsigned char)n;
    return 1;
}

int xpost_dev_pdf_state(Xpost_Context *ctx, Xpost_Object devdic,
                        int slot, const char *s, size_t n)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a) || !_pdf_gs_of(ctx, priv, &a))
        return 1;
    /* The record hangs off the accumulator by pointer, so what it holds
       is written where it lives and the struct needs no storing back. */
    return _pdf_gs_new(&a, slot, s, n);
}

/* .pdfsput  % str devdic  .  -
   Append a piece to the state operator being built. */
static int _pdfsput(Xpost_Context *ctx, Xpost_Object str, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (!_pdf_gs_of(ctx, priv, &a))
        return VMerror;
    return xpost_strbuf_append(&a.gs->pend,
                               xpost_string_get_pointer(ctx, str), str.comp_.sz);
}

/* .pdfsend  % slot devdic  .  bool
   Finish the state operator being built: write it into the content, or
   leave it out where the stream carries it already, answering which it
   did. The building and the writing are the one call sequence, so a
   writer cannot ask whether an operator is needed and then not write it
   -- which would leave the slot answering for bytes the stream does not
   hold, and the next paint going out with no colour.

   The answer is for a writer whose operator names a page resource: a
   resource is declared because the content refers to it, so what
   reached the content is what settles the declaration. */
static int _pdfsend(Xpost_Context *ctx, Xpost_Object slot, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int wrote, ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (!_pdf_gs_of(ctx, priv, &a))
        return VMerror;
    wrote = _pdf_gs_new(&a, slot.int_.val, a.gs->pend.s, a.gs->pend.len);
    if (wrote)
    {
        ret = _pdf_content(&a, a.gs->pend.s, a.gs->pend.len);
        if (ret)
            return ret;
        if (!_pdf_acc_put(ctx, priv, &a))
            return VMerror;
    }
    a.gs->pend.len = 0;
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(wrote));
    return 0;
}

/* .pdfscarried  % slot devdic  .  -
   Record the state operator being built as already in force, without
   writing it. What it is for is the state a content stream is in before
   anything is written into it: PDF 8.4.1 Table 52 gives the graphics
   state parameters their starting values, so an operator restating one
   of those at the head of a page would say what the consumer already
   has.

   It is a separate call from .pdfsend and not a flag on it, because the
   two are opposite obligations: .pdfsend is told only what is about to
   be written, and this only what will not be. */
static int _pdfscarried(Xpost_Context *ctx, Xpost_Object slot, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (!_pdf_gs_of(ctx, priv, &a))
        return VMerror;
    (void)_pdf_gs_new(&a, slot.int_.val, a.gs->pend.s, a.gs->pend.len);
    a.gs->pend.len = 0;
    return 0;
}

/* .pdfsave / .pdfrestore  % devdic  .  -
   The content-stream q and Q, written with the record of what the
   stream carries saved and brought back alongside. They are one call
   each so that the two cannot come apart: a q whose state was not saved
   leaves the writer suppressing an operator the matching Q has undone,
   which is a paint in the wrong colour and not a larger file. */
static int _pdfsave(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (!_pdf_gs_of(ctx, priv, &a))
        return VMerror;
    if (a.gs)
    {
        if (a.gs->over || a.gs->depth + 1 >= PDF_GS_DEPTH)
        {
            /* Deeper than the record follows. The level in force is
               emptied and the excess counted, so every writer emits in
               full until the matching Q brings the depth back. */
            a.gs->over++;
            memset(&a.gs->level[a.gs->depth], 0, sizeof(a.gs->level[0]));
        }
        else
        {
            a.gs->level[a.gs->depth + 1] = a.gs->level[a.gs->depth];
            a.gs->depth++;
        }
    }
    ret = _pdf_content(&a, "q\n", 2);
    if (ret)
        return ret;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

static int _pdfrestore(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (!_pdf_gs_of(ctx, priv, &a))
        return VMerror;
    if (a.gs)
    {
        if (a.gs->over)
            a.gs->over--;
        else if (a.gs->depth > 0)
            a.gs->depth--;
        else
            /* A Q with no q of this writer's making: what the consumer
               restores is a state from before this stream, which nothing
               here knows. */
            memset(&a.gs->level[0], 0, sizeof(a.gs->level[0]));
    }
    ret = _pdf_content(&a, "Q\n", 2);
    if (ret)
        return ret;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* .pdfsubbegin  % devdic  .  n true | false
   .pdfsubend    % n devdic  .  [str ...]

   Open and close a content stream captured out of the page's own
   accumulator. A tiling pattern's cell is a stream of its own (PDF
   8.7.3.1) written with the same marking methods the page is written
   with, so it is written where they write and lifted out again
   afterwards: .pdfsubbegin answers where the cell starts, and
   .pdfsubend answers with the bytes from there to the end as
   <=65535-byte strings (the PostScript string limit) and winds the
   content back so the page reads as though the cell had never been
   written into it.

   What the record of the stream's graphics state carries goes with
   them: the cell's stream starts carrying nothing, and what the page
   carried is back in force once the cell is out. See Pdf_Gs.

   .pdfsubbegin answers false rather than refusing when there is no
   accumulator to capture out of, or a capture already open: the caller
   then paints the cells itself, which is the route a device without
   these has. */
static int _pdfsubbegin(Xpost_Context *ctx, Xpost_Object nest,
                        Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a) || !_pdf_gs_of(ctx, priv, &a))
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    /* A caller that does not nest is refused while any capture is open,
       which is what it asked for; one that does is refused only past the
       depth the stack holds. Either way the answer is false and the
       caller paints where it stands. */
    if ((a.gs->insub && !nest.int_.val) || a.gs->insub >= PDF_SUB_DEPTH)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    /* A run open when a capture begins belongs to the stream the
       capture is being taken out of, so it is closed before the mark is
       taken: left open it would be written after the capture ended and
       land past the marks it was gathered among. */
    if (a.txt.open && _pdf_text_emit(&a))
        return VMerror;
    i = a.gs->insub;
    a.gs->subat[i] = a.content.len;
    a.gs->subdepth[i] = a.gs->depth;
    a.gs->subover[i] = a.gs->over;
    memcpy(a.gs->subslot[i], a.gs->level[a.gs->depth].slot,
           sizeof(a.gs->subslot[i]));
    memset(&a.gs->level[a.gs->depth], 0, sizeof(a.gs->level[0]));
    a.gs->over = 0;
    a.gs->pend.len = 0;
    a.gs->insub = i + 1;
    /* the record hangs off the accumulator by pointer, so what it holds
       is written where it lives and the struct needs no storing back */
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)a.gs->subat[i]));
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
}

static int _pdfsubend(Xpost_Context *ctx, Xpost_Object at, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv, result;
    size_t pos, start, len;
    int nchunks, i, ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (!_pdf_gs_of(ctx, priv, &a))
        return VMerror;
    /* And a run open when it ends belongs to what was captured: the
       glyphs were drawn by the description being taken, so they go into
       it. Left open, the run is written when something else next
       reaches the content -- which is after the capture, into the
       stream the description was taken out of, naming a font only the
       description declares. */
    if (a.txt.open && _pdf_text_emit(&a))
        return VMerror;

    /* Where the cell started is read off the record and not off the
       operand: the two are the same number, and the one the capture
       itself wrote is the one that cannot have been altered between
       the calls. The operand stands in only for a capture this never
       opened, where winding back to it is the caller's own statement
       of what to discard. */
    start = a.gs->insub ? a.gs->subat[a.gs->insub - 1]
                       : (size_t)(at.int_.val < 0 ? 0 : at.int_.val);
    if (start > a.content.len)
        start = a.content.len;
    len = a.content.len - start;

    if (a.gs->insub)
    {
        /* the one closing is the one opened last */
        int sub = --a.gs->insub;
        a.gs->depth = a.gs->subdepth[sub];
        a.gs->over = a.gs->subover[sub];
        memcpy(a.gs->level[a.gs->depth].slot, a.gs->subslot[sub],
               sizeof(a.gs->subslot[sub]));
    }
    a.gs->pend.len = 0;

    nchunks = (int)((len + 65534) / 65535);
    if (nchunks == 0)
        nchunks = 1;
    result = xpost_object_cvlit(xpost_array_cons(ctx, nchunks));
    pos = start;
    for (i = 0; i < nchunks; i++)
    {
        size_t chunk = a.content.len - pos;
        if (chunk > 65535)
            chunk = 65535;
        ret = xpost_array_put(ctx, result, i,
                              xpost_object_cvlit(
                                  xpost_string_cons(ctx, chunk,
                                                    a.content.s + pos)));
        if (ret)
            return ret;
        pos += chunk;
    }
    a.content.len = start;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, result);
    return 0;
}

/* Exported accumulator access for the text operators: they build a
   complete content-stream fragment per glyph outline and append it in
   one call. */
/* --- text, said as text ----------------------------------------------

   A glyph drawn as an outline costs its shape at every occurrence, and
   filing the shape and placing it costs a placement. Neither is what a
   page of text is: the reader already has the face, so the content need
   only name it and say which characters, and the reader draws them.

   MEASURED on a waterfall of two faces at nine sizes, drawing the
   outlines came to thirty-seven times what naming them does.

   What makes it exact rather than approximate is the widths. This
   declares, for every code shown, the width the show ACTUALLY advanced
   by, so a reader stepping the pen through a string arrives where this
   engine arrived. Where it would not -- a show that spaces its
   characters itself, a glyph placed by anything but its own width --
   the pen is checked against expectation at every glyph and the run is
   broken where they part, so the next run states its own origin. */

/* Find, or start, the record for a base font. */
static int _pdf_fnt_index(Pdf_Acc *a, const char *base)
{
    int i;

    for (i = 0; i < a->nfnts; i++)
        if (strcmp(a->fnts[i].base, base) == 0)
            return i;
    if (a->nfnts == a->fntcap)
    {
        int nc = a->fntcap ? a->fntcap * 2 : 4;
        Pdf_Fnt *nf = (Pdf_Fnt *)realloc(a->fnts, (size_t)nc * sizeof(Pdf_Fnt));

        if (!nf)
            return -1;
        a->fnts = nf;
        a->fntcap = nc;
    }
    memset(&a->fnts[a->nfnts], 0, sizeof(Pdf_Fnt));
    strncpy(a->fnts[a->nfnts].base, base, sizeof(a->fnts[0].base) - 1);
    return a->nfnts++;
}

/* Write the run gathered so far into the content, and close it. */
static int _pdf_text_emit(Pdf_Acc *a)
{
    char h[256];
    int n = 0;
    int i;

    /* A run of blanks marks nothing, and writing it would leave content
       on a page that the same text drawn as outlines leaves empty: an
       outline with no contour covers no pixel (PLRM 7.5.1). The glyphs
       stay in the run while it gathers, so a space between two words
       does not break the run in two. */
    if (!a->txt.open || a->txt.codes.len == 0 || !a->txt.inked)
    {
        a->txt.open = 0;
        a->txt.codes.len = 0;
        a->txt.inked = 0;
        return 0;
    }
    if (a->txt.fnt >= 0 && a->txt.fnt < a->nfnts)
        a->fnts[a->txt.fnt].emitted = 1;
    memcpy(h + n, "BT /F", 5); n += 5;
    n += sprintf(h + n, "%d", a->txt.fnt);
    /* the size rides in the matrix, so the font is set at one and the
       widths are read as the thousandths of the em they are */
    memcpy(h + n, " 1 Tf ", 6); n += 6;
    for (i = 0; i < 4; i++)
    {
        n += xpost_dev_pdf_fmt_num(h + n, a->txt.mat[i]);
        h[n++] = ' ';
    }
    n += xpost_dev_pdf_fmt_num(h + n, a->txt.x); h[n++] = ' ';
    n += xpost_dev_pdf_fmt_num(h + n, a->txt.y);
    memcpy(h + n, " Tm (", 5); n += 5;
    if (xpost_strbuf_append(&a->content, h, (size_t)n))
        return VMerror;
    if (xpost_strbuf_append(&a->content, a->txt.codes.s, a->txt.codes.len))
        return VMerror;
    if (xpost_strbuf_append(&a->content, ") Tj ET\n", 8))
        return VMerror;
    a->txt.open = 0;
    a->txt.codes.len = 0;
    return 0;
}

/* dev  .pdftextflush  -
   Close any run of text, so the content stream is complete before it is
   written out. The append path closes a run whenever anything else is
   said, so this is only for the end, where nothing else is said. */
static int _pdftextflush(Xpost_Context *ctx, Xpost_Object devdic)
{
    return xpost_dev_pdf_text_flush(ctx, devdic);
}

/* -  .pdffontres  string
   The /Font entry for the page's resources, and the empty string where
   the page set no text.

   Written whole rather than as objects of its own: a font this names is
   one the reader already has, so all the page carries is which one,
   which codes it used, what each is called and how wide it was taken to
   be. That is a few hundred bytes, and an indirect object apiece would
   cost more to refer to than to say.

   Reading does NOT clear the record. The resources are written once for
   the page and again for every pattern and every filed description on
   it, because each of those is a content stream of its own and carries
   its own resources; a read that emptied the record would hand the
   whole set to whichever stream was written first and leave the rest --
   the page among them -- declaring no font at all, while their content
   went on naming one. The record is given up at the page end instead,
   where the page's other records are.  */
static int _pdffontres(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv, str;
    Xpost_String_Buffer b;
    char t[128];
    int i, c, ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (a.txt.open && _pdf_text_emit(&a))
        return VMerror;
    if (a.nfnts == 0)
    {
        if (!_pdf_acc_put(ctx, priv, &a))
            return VMerror;
        str = xpost_string_cons(ctx, 0, NULL);
        if (xpost_object_get_type(str) == invalidtype)
            return VMerror;
        xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(str));
        return 0;
    }
    xpost_strbuf_init(&b, 512);
#define PUT(lit) do { if (xpost_strbuf_append(&b, lit, sizeof(lit) - 1)) \
                          goto oom; } while (0)
#define PUTN(v) do { int n_ = sprintf(t, "%d", (int)(v)); \
                     if (xpost_strbuf_append(&b, t, (size_t)n_)) goto oom; \
                   } while (0)
    PUT(" /Font <<");
    for (i = 0; i < a.nfnts; i++)
    {
        int lo = -1, hi = -1;

        /* A font only a run of blanks named was never written, and
           declaring it would put a resource on a page that has nothing
           on it. */
        if (!a.fnts[i].emitted)
            continue;
        for (c = 0; c < 256; c++)
            if (a.fnts[i].used[c])
            {
                if (lo < 0) lo = c;
                hi = c;
            }
        if (lo < 0)
            continue;
        PUT(" /F"); PUTN(i);
        PUT(" << /Type /Font /Subtype /Type1 /BaseFont /");
        if (xpost_strbuf_append(&b, a.fnts[i].base, strlen(a.fnts[i].base)))
            goto oom;
        PUT(" /FirstChar "); PUTN(lo);
        PUT(" /LastChar "); PUTN(hi);
        PUT(" /Widths [");
        for (c = lo; c <= hi; c++)
        {
            PUT(" ");
            PUTN(a.fnts[i].used[c] ? a.fnts[i].width[c] : 0);
        }
        PUT(" ] /Encoding << /Type /Encoding /Differences [");
        for (c = lo; c <= hi; c++)
            if (a.fnts[i].used[c] && a.fnts[i].gname[c])
            {
                PUT(" "); PUTN(c); PUT(" /");
                if (xpost_strbuf_append(&b, a.fnts[i].gname[c],
                                        strlen(a.fnts[i].gname[c])))
                    goto oom;
            }
        PUT(" ] >> >>");
    }
    PUT(" >>");
#undef PUT
#undef PUTN
    str = xpost_string_cons(ctx, (unsigned int)b.len, b.s);
    ret = xpost_object_get_type(str) == invalidtype ? VMerror : 0;
    xpost_strbuf_free(&b);
    if (ret)
        return ret;
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(str));
    return 0;
oom:
    xpost_strbuf_free(&b);
    return VMerror;
}

/* -  .pdffontclear  -
   Give up the fonts the page named, at the page end. Every stream the
   page is made of has had its resources written by then, and the page
   after this one begins with none of them. */
static int _pdffontclear(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i, c;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    for (i = 0; i < a.nfnts; i++)
        for (c = 0; c < 256; c++)
            free(a.fnts[i].gname[c]);
    free(a.fnts);
    a.fnts = NULL;
    a.nfnts = 0;
    a.fntcap = 0;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

int xpost_dev_pdf_text_flush(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return 0;
    if (!a.txt.open)
        return 0;
    if (_pdf_text_emit(&a))
        return VMerror;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

int xpost_dev_pdf_text_glyph(Xpost_Context *ctx, Xpost_Object devdic,
                             const char *base, int code, const char *gname,
                             double width, const double *mat,
                             double px, double py, int marks, int *taken)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Fnt *f;
    int fi, i;
    char esc[4];
    int en = 0;
    double dx;

    *taken = 0;
    if (code < 0 || code > 255 || !gname || !*base)
        return 0;
    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return 0;
    fi = _pdf_fnt_index(&a, base);
    if (fi < 0)
        return 0;
    f = &a.fnts[fi];
    if (f->used[code])
    {
        /* the same code twice with different widths is a font this
           cannot describe: one width is declared for one code */
        if (f->width[code] != (short)(width + 0.5))
        {
            if (!_pdf_acc_put(ctx, priv, &a))
                return VMerror;
            return 0;
        }
    }

    /* does it continue the run in hand? */
    if (a.txt.open)
    {
        int same = a.txt.fnt == fi;

        for (i = 0; i < 4 && same; i++)
            if (a.txt.mat[i] - mat[i] > 1e-9 || mat[i] - a.txt.mat[i] > 1e-9)
                same = 0;
        if (same &&
            (px - a.txt.nx < 5e-4 && a.txt.nx - px < 5e-4) &&
            (py - a.txt.ny < 5e-4 && a.txt.ny - py < 5e-4))
            ;                       /* it does */
        else if (_pdf_text_emit(&a))
            return VMerror;
    }
    if (!a.txt.open)
    {
        a.txt.open = 1;
        a.txt.fnt = fi;
        for (i = 0; i < 4; i++)
            a.txt.mat[i] = mat[i];
        a.txt.x = px;
        a.txt.y = py;
        a.txt.codes.len = 0;
        a.txt.inked = 0;
    }
    if (marks)
        a.txt.inked = 1;

    /* the three characters a string may not simply carry (PLRM 3.2.2
       gives the same three to a PostScript string, and PDF took them) */
    if (code == '(' || code == ')' || code == '\\')
        esc[en++] = '\\';
    esc[en++] = (char)code;
    if (xpost_strbuf_append(&a.txt.codes, esc, (size_t)en))
        return VMerror;

    if (!f->used[code])
    {
        f->used[code] = 1;
        f->width[code] = (short)(width + 0.5);
        f->gname[code] = (char *)malloc(strlen(gname) + 1);
        if (!f->gname[code])
            return VMerror;
        strcpy(f->gname[code], gname);
    }

    /* where the reader will leave the pen, which is where the next
       glyph has to be for the run to go on */
    dx = (double)f->width[code] / 1000.0;
    a.txt.nx = px + mat[0] * dx;
    a.txt.ny = py + mat[1] * dx;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    *taken = 1;
    return 0;
}

int xpost_dev_pdf_append(Xpost_Context *ctx, Xpost_Object devdic,
                         const char *s, size_t n)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    ret = _pdf_content(&a, s, n);
    if (ret)
        return ret;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* A PDF number carries four decimals: 0.0001pt is finer than any raster
   grid a consumer will draw on. */
static int _pdf_fmt_num(char *o, double v)
{
    return _fmt_num_prec(o, v, 10000, 4);
}

/* Exported PDF number formatter (see _pdf_fmt_num) */
int xpost_dev_pdf_fmt_num(char *o, double v)
{
    return _pdf_fmt_num(o, v);
}

/* An SVG coordinate carries two. The writer emits marks in device space --
   a scale a program set is already in the numbers rather than in a
   transform above them -- so a decimal here is a hundredth of a point,
   which no output resolution resolves. Matrices are not written through
   this: a matrix multiplies everything drawn under it, so what would be
   lost in its last decimals is not bounded by the page. */
int xpost_dev_svg_fmt_num(char *o, double v)
{
    return _fmt_num_prec(o, v, 100, 2);
}


/* Emit the content-stream operators for a filled path into the accumulator:
   the flattened subpaths ("x y m" / "x y l", closed with "h") and a
   nonzero-winding fill ("f") -- the rule the fill operator has: overlapping
   subpaths union, and hole subpaths are counter-wound by their producers.
   This is the per-coordinate hot loop of the pdfwrite FillPoly,
   in C; the fill colour is the device's business, emitted beforehand. */
/* Whether the subpath starting at i is four points making an
   axis-aligned rectangle, and where the next one starts.

   PDF says a rectangle in one operator where a subpath says it in five,
   and what draws thousands of them is anything built out of bars.
   MEASURED on one page of a barcode label: fourteen thousand six
   hundred subpaths of exactly this shape, every one written the long
   way.

   Compared at the precision the numbers are written to and not as the
   doubles they arrive as: a corner comes through a matrix and can miss
   its neighbour by a fraction the writer would never print, and what is
   asked here is whether the rectangle written would be the subpath
   written. */
static int _poly_rect_at(Xpost_Context *ctx, Xpost_Object poly,
                         int i, int n, double *x, double *y,
                         double *w, double *h, int *next)
{
#define PDFNUMVAL(o) (xpost_object_get_type(o) == realtype ? (o).real_.val \
                                                           : (double)(o).int_.val)
#define SAMENUM(a, b) (((a) - (b)) < 5e-5 && ((b) - (a)) < 5e-5)
    double c[4][2];
    int k;

    for (k = 0; k < 4; k++)
    {
        Xpost_Object e;

        if (i + k >= n)
            return 0;
        e = xpost_array_get(ctx, poly, i + k);
        if (xpost_object_get_type(e) != arraytype || e.comp_.sz != 2)
            return 0;
        c[k][0] = PDFNUMVAL(xpost_array_get(ctx, e, 0));
        c[k][1] = PDFNUMVAL(xpost_array_get(ctx, e, 1));
    }
    /* the subpath has to end here: a fifth point makes it something else */
    if (i + 4 < n)
    {
        Xpost_Object e = xpost_array_get(ctx, poly, i + 4);

        if (xpost_object_get_type(e) == arraytype && e.comp_.sz == 2)
            return 0;
        *next = i + 5;          /* past the separator */
    }
    else
        *next = i + 4;
    if (SAMENUM(c[0][1], c[1][1]) && SAMENUM(c[1][0], c[2][0]) &&
        SAMENUM(c[2][1], c[3][1]) && SAMENUM(c[3][0], c[0][0]))
        ;
    else if (SAMENUM(c[0][0], c[1][0]) && SAMENUM(c[1][1], c[2][1]) &&
             SAMENUM(c[2][0], c[3][0]) && SAMENUM(c[3][1], c[0][1]))
        ;
    else
        return 0;
    *x = c[0][0] < c[2][0] ? c[0][0] : c[2][0];
    *y = c[0][1] < c[2][1] ? c[0][1] : c[2][1];
    *w = c[0][0] < c[2][0] ? c[2][0] - c[0][0] : c[0][0] - c[2][0];
    *h = c[0][1] < c[2][1] ? c[2][1] - c[0][1] : c[0][1] - c[2][1];
    return (*w > 0.0 && *h > 0.0);
#undef SAMENUM
#undef PDFNUMVAL
}

static int _pdffillpoly(Xpost_Context *ctx,
                        Xpost_Object poly, Xpost_Object devdic)
{
#define PDFNUMVAL(o) (xpost_object_get_type(o) == realtype ? (o).real_.val \
                                                           : (double)(o).int_.val)
    Pdf_Acc a;
    Xpost_Object priv;
    char tmp[128];
    int i, n, len, needmove = 1, ret = 0;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;

    n = poly.comp_.sz;
    for (i = 0; ret == 0 && i < n; i++)
    {
        Xpost_Object e = xpost_array_get(ctx, poly, i);
        double rx, ry, rw, rh;
        int rnext;

        if (needmove &&
            _poly_rect_at(ctx, poly, i, n, &rx, &ry, &rw, &rh, &rnext))
        {
            len = 0;
            len += _pdf_fmt_num(tmp + len, rx); tmp[len++] = ' ';
            len += _pdf_fmt_num(tmp + len, ry); tmp[len++] = ' ';
            len += _pdf_fmt_num(tmp + len, rw); tmp[len++] = ' ';
            len += _pdf_fmt_num(tmp + len, rh); tmp[len++] = ' ';
            tmp[len++] = 'r'; tmp[len++] = 'e'; tmp[len++] = '\n';
            ret = _pdf_content(&a, tmp, len);
            i = rnext - 1;      /* the loop's own increment finishes it */
            continue;
        }
        if (xpost_object_get_type(e) == arraytype && e.comp_.sz == 2)
        {
            double x = PDFNUMVAL(xpost_array_get(ctx, e, 0));
            double y = PDFNUMVAL(xpost_array_get(ctx, e, 1));
            len = 0;
            len += _pdf_fmt_num(tmp + len, x); tmp[len++] = ' ';
            len += _pdf_fmt_num(tmp + len, y); tmp[len++] = ' ';
            tmp[len++] = needmove ? 'm' : 'l';
            tmp[len++] = '\n';
            needmove = 0;
            ret = _pdf_content(&a, tmp, len);
        }
        else if (!needmove)   /* null subpath separator: close the subpath */
        {
            ret = _pdf_content(&a, "h\n", 2);
            needmove = 1;
        }
    }
    if (ret == 0 && !needmove)
        ret = _pdf_content(&a, "h\n", 2);
    if (ret == 0)
        ret = _pdf_content(&a, "f\n", 2);

    /* the struct is stored back even when an append failed: the appends
       that did land may have moved the buffer, and the stored copy must
       follow it */
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return ret;
}

#undef PDFNUMVAL

/* The svgwrite FillPoly hot loop: emit one SVG path element for a filled
   path into the accumulator -- the fill colour as percentages, a
   nonzero-winding fill rule (the fill operator's), and the flattened subpaths
   as M/L commands, each closed with Z. Device coordinates are y-down, as
   SVG's are, so they pass through unchanged. */
static int _svgfillpoly(Xpost_Context *ctx,
                        Xpost_Object r, Xpost_Object g, Xpost_Object b,
                        Xpost_Object poly, Xpost_Object devdic)
{
#define PDFNUMVAL(o) (xpost_object_get_type(o) == realtype ? (o).real_.val \
                                                           : (double)(o).int_.val)
    Pdf_Acc a;
    Xpost_Object priv;
    char tmp[128];
    int i, n, len, needmove = 1, ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;

    len = xpost_dev_svg_path_open(tmp, PDFNUMVAL(r), PDFNUMVAL(g),
                                  PDFNUMVAL(b), 0);
    ret = xpost_strbuf_append(&a.content, tmp, len);

    n = poly.comp_.sz;
    for (i = 0; ret == 0 && i < n; i++)
    {
        Xpost_Object e = xpost_array_get(ctx, poly, i);
        if (xpost_object_get_type(e) == arraytype && e.comp_.sz == 2)
        {
            double x = PDFNUMVAL(xpost_array_get(ctx, e, 0));
            double y = PDFNUMVAL(xpost_array_get(ctx, e, 1));
            len = 0;
            tmp[len++] = needmove ? 'M' : 'L';
            len += xpost_dev_svg_fmt_num(tmp + len, x); tmp[len++] = ' ';
            len += xpost_dev_svg_fmt_num(tmp + len, y);
            needmove = 0;
            ret = xpost_strbuf_append(&a.content, tmp, len);
        }
        else if (!needmove)   /* null subpath separator: close the subpath */
        {
            ret = xpost_strbuf_append(&a.content, "Z", 1);
            needmove = 1;
        }
    }
    if (ret == 0 && !needmove)
        ret = xpost_strbuf_append(&a.content, "Z", 1);
    if (ret == 0)
        ret = xpost_strbuf_append(&a.content, "\"/>\n", 4);

    /* the struct is stored back even when an append failed: the appends
       that did land may have moved the buffer, and the stored copy must
       follow it */
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return ret;
#undef PDFNUMVAL
}

/* Return the accumulated content as an array of <=65535-byte strings (the
   PostScript string limit) for the Emit method to compress and write. The
   malloc'd source buffer is stable across the string allocations. */
static int _pdfchunks(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv, result;
    size_t pos = 0;
    int nchunks, i, ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    nchunks = (int)((a.content.len + 65534) / 65535);
    if (nchunks == 0)
        nchunks = 1;
    result = xpost_object_cvlit(xpost_array_cons(ctx, nchunks));
    for (i = 0; i < nchunks; i++)
    {
        size_t chunk = a.content.len - pos;
        if (chunk > 65535)
            chunk = 65535;
        ret = xpost_array_put(ctx, result, i,
                              xpost_object_cvlit(
                                  xpost_string_cons(ctx, chunk, a.content.s + pos)));
        if (ret)
            return ret;
        pos += chunk;
    }
    xpost_stack_push(ctx->lo, ctx->os, result);
    return 0;
}

/* format a number in PDF syntax into a fresh string: the marking
   methods format through the accumulator, but separation registration
   builds function source in strings, and both must agree */
static int _pdfnumstr(Xpost_Context *ctx, Xpost_Object num)
{
    char t[32];
    int n;

    n = _pdf_fmt_num(t, xpost_object_number(num));
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_object_cvlit(xpost_string_cons(ctx, n, t)));
    return 0;
}

/* Separation registry: the device's .registersep method files each
   separation colour space used by the page under a small integer, which
   the content stream references as /CS<i>. Entries live beside the
   content in the accumulator -- outside virtual memory -- so a `restore`
   cannot roll the registry away from content that already references it. */

/* A name-keyed index over the registry so a separation is found in
   constant time rather than by scanning every one registered before it,
   which turned a page naming many separations into work rising with their
   number squared. The index holds a separation's position plus one (zero
   marks an empty slot) and is rebuilt when it would fill past half, so an
   insertion is amortised constant. It holds positions, not pointers, so
   growing the registry array leaves it valid; a name is confirmed by
   comparison on a hit, so a collision never returns the wrong separation;
   and if it cannot be allocated the search falls back to the scan and
   stays correct. */
static unsigned _pdf_sep_hash(const char *name, size_t namelen)
{
    unsigned h = 2166136261u;
    size_t k;

    for (k = 0; k < namelen; k++)
    {
        h ^= (unsigned char)name[k];
        h *= 16777619u;
    }
    return h;
}

/* Puts one separation in the open-addressed index, which is what makes
   a page of many separations cost a lookup rather than a scan. */
static void _pdf_sep_hash_place(Pdf_Acc *a, int idx)
{
    unsigned mask = (unsigned)a->sephashcap - 1;
    unsigned h = _pdf_sep_hash(a->seps[idx].name, a->seps[idx].namelen) & mask;

    while (a->sephash[h])
        h = (h + 1) & mask;
    a->sephash[h] = idx + 1;
}

/* size the index to hold every registered separation no more than half
   full and place them all; on allocation failure give it up, so the
   search falls back to the scan. */
static void _pdf_sep_hash_rebuild(Pdf_Acc *a)
{
    int want = 8;
    int i;

    while (want < a->nseps * 2)
        want *= 2;
    free(a->sephash);
    a->sephash = (int *)calloc((size_t)want, sizeof(int));
    if (!a->sephash)
    {
        a->sephashcap = 0;
        return;
    }
    a->sephashcap = want;
    for (i = 0; i < a->nseps; i++)
        _pdf_sep_hash_place(a, i);
}

static int _pdf_sep_find(Pdf_Acc *a, const char *name, size_t namelen)
{
    int i;

    if (a->sephash && a->sephashcap)
    {
        unsigned mask = (unsigned)a->sephashcap - 1;
        unsigned h = _pdf_sep_hash(name, namelen) & mask;
        int slot;

        while ((slot = a->sephash[h]) != 0)
        {
            int idx = slot - 1;
            if (a->seps[idx].namelen == namelen &&
                memcmp(a->seps[idx].name, name, namelen) == 0)
                return idx;
            h = (h + 1) & mask;
        }
        return -1;
    }
    for (i = 0; i < a->nseps; i++)
        if (a->seps[i].namelen == namelen &&
            memcmp(a->seps[i].name, name, namelen) == 0)
            return i;
    return -1;
}

/* look a separation up by name: index true, or false when unregistered */
static int _pdffindsep(Xpost_Context *ctx, Xpost_Object name, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    i = _pdf_sep_find(&a, (char *)xpost_string_get_pointer(ctx, name),
                      name.comp_.sz);
    if (i >= 0)
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(i));
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(i >= 0));
    return 0;
}

static char *_pdf_sep_strdup(Xpost_Context *ctx, Xpost_Object str)
{
    char *p = (char *)malloc(str.comp_.sz ? str.comp_.sz : 1);

    if (p)
        memcpy(p, xpost_string_get_pointer(ctx, str), str.comp_.sz);
    return p;
}

/* register a separation (name, colour-space body, function object body),
   returning its index; an already-registered name just returns its index */
static int _pdfregsep(Xpost_Context *ctx,
                      Xpost_Object name, Xpost_Object csdef, Xpost_Object func,
                      Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Sep *s;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    i = _pdf_sep_find(&a, (char *)xpost_string_get_pointer(ctx, name),
                      name.comp_.sz);
    if (i >= 0)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(i));
        return 0;
    }
    if (a.nseps == a.sepcap)
    {
        int nc = a.sepcap ? a.sepcap * 2 : 4;
        Pdf_Sep *ns = (Pdf_Sep *)realloc(a.seps, nc * sizeof(Pdf_Sep));
        if (!ns)
            return VMerror;
        a.seps = ns;
        a.sepcap = nc;
        /* the grown array must reach /Private even if a copy below
           fails: the old block is gone */
        if (!_pdf_acc_put(ctx, priv, &a))
        {
            /* nothing names the grown block, and what the record still
               names was released by the growth: give the block up and
               leave the record naming an empty list rather than storage
               that is gone */
            free(a.seps);
            a.seps = NULL;
            a.nseps = 0;
            a.sepcap = 0;
            free(a.sephash);
            a.sephash = NULL;
            a.sephashcap = 0;
            if (!_pdf_acc_put(ctx, priv, &a))
                XPOST_LOG_ERR("cannot record the emptied separation list");
            return VMerror;
        }
    }
    s = &a.seps[a.nseps];
    s->name = _pdf_sep_strdup(ctx, name);
    s->namelen = name.comp_.sz;
    s->csdef = _pdf_sep_strdup(ctx, csdef);
    s->csdeflen = csdef.comp_.sz;
    s->func = _pdf_sep_strdup(ctx, func);
    s->funclen = func.comp_.sz;
    if (!s->name || !s->csdef || !s->func)
    {
        free(s->name);
        free(s->csdef);
        free(s->func);
        return VMerror;
    }
    i = a.nseps++;
    /* keep the name index in step: grow and rebuild when it would fill
       past half, otherwise place the one new name */
    if (!a.sephash || a.sephashcap < a.nseps * 2)
        _pdf_sep_hash_rebuild(&a);
    else
        _pdf_sep_hash_place(&a, i);
    if (!_pdf_acc_put(ctx, priv, &a))
    {
        /* Nothing but this local names what has just been made: the three
           strings of the separation, which the record's count does not
           reach, and the index, which the rebuild allocates afresh after
           releasing the one the record still names. Returning here left
           both unreachable and the record naming storage that is gone.

           So they are given up and the accumulator is put back as it was
           found, with no index at all: a search with no index falls back
           to the scan, which is what an index that could not be allocated
           already leaves it doing. */
        free(s->name);
        free(s->csdef);
        free(s->func);
        free(a.sephash);
        a.sephash = NULL;
        a.sephashcap = 0;
        a.nseps = i;
        if (!_pdf_acc_put(ctx, priv, &a))
            XPOST_LOG_ERR("cannot record the separation list without its index");
        return VMerror;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(i));
    return 0;
}

/* How many separations this page has registered. */
/* store under a name, handing the refusal back: a dictionary that would
   not take an entry leaves the caller with an incomplete answer */
static XPOST_MUST_CHECK int _put_named(Xpost_Context *ctx, Xpost_Object d,
                                       const char *k, Xpost_Object v)
{
    return xpost_dict_put(ctx, d, xpost_name_cons(ctx, k), v);
}

static Xpost_Object _img_field(Xpost_Context *ctx, Xpost_Object d, const char *k)
{
    return xpost_dict_get(ctx, d, xpost_name_cons(ctx, k));
}

static int _img_int(Xpost_Context *ctx, Xpost_Object d, const char *k, int dflt)
{
    Xpost_Object v = _img_field(ctx, d, k);
    int t = xpost_object_get_type(v);

    if (t == integertype || t == realtype)
        return (int)xpost_object_number(v);
    if (t == booleantype)
        return v.int_.val ? 1 : 0;
    return dflt;
}

/* a flat array of numbers out of the dictionary, as many as fit */
static int _img_nums(Xpost_Context *ctx, Xpost_Object d, const char *k,
                     double *out, int cap)
{
    Xpost_Object v = _img_field(ctx, d, k);
    int i, n;

    if (xpost_object_get_type(v) != arraytype)
        return -1;
    n = v.comp_.sz;
    if (n > cap)
        n = cap;
    for (i = 0; i < n; i++)
        out[i] = xpost_object_number(xpost_array_get(ctx, v, i));
    return n;
}

/* .pdfimgadd  dict devdic  .  index

   File an image, copying what it says rather than keeping what said it.
   The dictionary is the caller's and may be reclaimed the moment this
   returns; what is kept here is bytes and numbers, which is all the page
   end needs to write the object. */
static int _pdfimgadd(Xpost_Context *ctx, Xpost_Object d, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv, rows;
    Pdf_Img *e;
    size_t total = 0;
    int i, n;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (a.nimgs == a.imgcap)
    {
        int nc = a.imgcap ? a.imgcap * 2 : 4;
        Pdf_Img *ni = (Pdf_Img *)realloc(a.imgs, (size_t)nc * sizeof(Pdf_Img));

        if (!ni)
            return VMerror;
        a.imgs = ni;
        a.imgcap = nc;
    }
    e = &a.imgs[a.nimgs];
    memset(e, 0, sizeof(*e));
    e->w = _img_int(ctx, d, "w", 0);
    e->h = _img_int(ctx, d, "h", 0);
    e->nc = _img_int(ctx, d, "nc", 1);
    e->interp = _img_int(ctx, d, "int", 0);
    e->mask = xpost_object_get_type(_img_field(ctx, d, "mask")) != invalidtype;
    if (e->mask)
    {
        e->haspol = 1;
        e->pol = _img_int(ctx, d, "pol", 0);
    }
    {
        /* The stencil's object number, where there is a stencil. The
           caller states the slot whether or not it filled it, so a null
           here means an image with no stencil of its own -- a colour-key
           image is masked by its sample values and by nothing else, and
           recording a number for it would name object nought. */
        Xpost_Object mb = _img_field(ctx, d, "mbits");

        if (xpost_object_get_type(mb) == integertype)
        {
            e->hasmbits = 1;
            e->mbits = mb.int_.val;
        }
    }
    e->ndec = _img_nums(ctx, d, "dec", e->dec, 16);
    if (e->ndec < 0)
        e->ndec = 0;
    e->nmrng = _img_nums(ctx, d, "mrng", e->mrng, 16);
    if (e->nmrng < 0)
        e->nmrng = 0;
    else
        e->hasmrng = 1;

    rows = _img_field(ctx, d, "rows");
    if (xpost_object_get_type(rows) != arraytype)
        return typecheck;
    n = rows.comp_.sz;
    for (i = 0; i < n; i++)
    {
        Xpost_Object r = xpost_array_get(ctx, rows, i);

        if (xpost_object_get_type(r) != stringtype)
            return typecheck;
        total += r.comp_.sz;
    }
    e->rows = total ? (char *)malloc(total) : NULL;
    if (total && !e->rows)
        return VMerror;
    e->rowslen = total;
    total = 0;
    for (i = 0; i < n && e->rows; i++)
    {
        Xpost_Object r = xpost_array_get(ctx, rows, i);

        memcpy(e->rows + total, xpost_string_get_pointer(ctx, r), r.comp_.sz);
        total += r.comp_.sz;
    }
    i = a.nimgs++;
    /* the grown block must reach /Private even if nothing else does: what
       the record named before the growth has been released */
    if (!_pdf_acc_put(ctx, priv, &a))
    {
        free(e->rows);
        free(a.imgs);
        return VMerror;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(i));
    return 0;
}

/* how many images the page's content draws */
static int _pdfimgcount(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.nimgs));
    return 0;
}

/* .pdfimgget  i devdic  .  dict

   One of them back in the shape it was filed in, for Emit to write the
   object around. The samples come back as a run of strings, each within
   the length a string can count; the object they are written into has no
   rows of its own, so what the run is cut at is nobody else's business. */
static int _pdfimgget(Xpost_Context *ctx, Xpost_Object idx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv, d, arr, str;
    Pdf_Img *e;
    int i = idx.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (i < 0 || i >= a.nimgs)
        return rangecheck;
    e = &a.imgs[i];
    d = xpost_dict_cons(ctx, 12);
    if (xpost_object_get_type(d) == invalidtype)
        return VMerror;
#define IMGPUT(k, v) \
    do { \
        if (_put_named(ctx, d, k, (v))) \
            return VMerror; \
    } while (0)
    IMGPUT("w", xpost_int_cons(e->w));
    IMGPUT("h", xpost_int_cons(e->h));
    IMGPUT("nc", xpost_int_cons(e->nc));
    IMGPUT("int", xpost_bool_cons(e->interp));
    if (e->mask)
    {
        IMGPUT("mask", xpost_bool_cons(1));
        IMGPUT("pol", xpost_bool_cons(e->pol));
    }
    if (e->hasmbits)
        IMGPUT("mbits", xpost_int_cons(e->mbits));
    if (e->ndec)
    {
        arr = xpost_array_cons(ctx, (unsigned)e->ndec);
        if (xpost_object_get_type(arr) == invalidtype)
            return VMerror;
        for (i = 0; i < e->ndec; i++)
            if (xpost_array_put(ctx, arr, i, xpost_real_cons((real)e->dec[i])))
                return VMerror;
        IMGPUT("dec", xpost_object_cvlit(arr));
    }
    if (e->hasmrng)
    {
        arr = xpost_array_cons(ctx, (unsigned)e->nmrng);
        if (xpost_object_get_type(arr) == invalidtype)
            return VMerror;
        for (i = 0; i < e->nmrng; i++)
            if (xpost_array_put(ctx, arr, i, xpost_real_cons((real)e->mrng[i])))
                return VMerror;
        IMGPUT("mrng", xpost_object_cvlit(arr));
    }
    {
        /* The samples come back in strings a string can count. An image
           of any size at all passes this way -- a page-sized one is
           hundreds of thousands of bytes -- and a string carries its
           length in a field that stops at 65535, so the samples are
           handed over in as many pieces as that takes. Every reader of
           them already takes a run of pieces: the compressors take an
           array, and the length is summed across it. */
        size_t pos = 0;
        int nchunks = (int)((e->rowslen + 65534) / 65535);

        if (nchunks == 0)
            nchunks = 1;
        arr = xpost_array_cons(ctx, (unsigned)nchunks);
        if (xpost_object_get_type(arr) == invalidtype)
            return VMerror;
        for (i = 0; i < nchunks; i++)
        {
            size_t chunk = (size_t)e->rowslen - pos;

            if (chunk > 65535)
                chunk = 65535;
            str = xpost_string_cons(ctx, (unsigned)chunk, e->rows + pos);
            if (xpost_object_get_type(str) == invalidtype)
                return VMerror;
            if (xpost_array_put(ctx, arr, i, xpost_object_cvlit(str)))
                return VMerror;
            pos += chunk;
        }
        IMGPUT("rows", xpost_object_cvlit(arr));
    }
#undef IMGPUT
    xpost_stack_push(ctx->lo, ctx->os, d);
    return 0;
}

/* .pdfimgcut  n devdic  .  -   ; keep the first n, give up the rest */
static int _pdfimgcut(Xpost_Context *ctx, Xpost_Object n, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i, k = n.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (k < 0 || k > a.nimgs)
        return rangecheck;
    for (i = k; i < a.nimgs; i++)
    {
        free(a.imgs[i].rows);
        a.imgs[i].rows = NULL;
    }
    a.nimgs = k;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* .pdfcost  devdic  .  bytes

   What the accumulator is holding, outside virtual memory. Everything a
   page files lives here rather than in the device's dictionary, because
   a restore must not reach it; that also puts it beyond what
   globalvmstatus weighs, so a page that went on holding what it filed
   would grow a long-lived context by that much per page and nothing
   measuring virtual memory would say so. This is what says so.

   Capacity and not occupancy: storage a page gave back is storage the
   next page reuses, and a figure that fell as soon as a table emptied
   would report a steady state that the memory does not have. */
static int _pdfcost(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    size_t n;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    n = a.content.cap;
    for (i = 0; i < a.nseps; i++)
        n += a.seps[i].namelen + a.seps[i].csdeflen + a.seps[i].funclen;
    n += (size_t)a.sepcap * sizeof(Pdf_Sep);
    n += (size_t)a.sephashcap * sizeof(int);
    n += (size_t)a.opcap * sizeof(Pdf_Op);
    n += (size_t)a.imgcap * sizeof(Pdf_Img);
    for (i = 0; i < a.nimgs; i++)
        n += a.imgs[i].rowslen;
    n += (size_t)a.formcap * sizeof(Pdf_Res);
    for (i = 0; i < a.nforms; i++)
        n += a.forms[i].bodylen;
    n += (size_t)a.patcap * sizeof(Pdf_Res);
    for (i = 0; i < a.npats; i++)
        n += a.pats[i].bodylen;
    n += (size_t)a.objcap * sizeof(Pdf_Obj);
    for (i = 0; i < a.nobjs; i++)
        n += a.objs[i].len;
    n += (size_t)a.shcap * sizeof(int);
    if (a.gs)
        n += sizeof(Pdf_Gs) + a.gs->pend.cap;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)n));
    return 0;
}

/* .pdfobjadd  num chunks devdic  .  -   ; an object's bytes, to write later */
static int _pdfobjadd(Xpost_Context *ctx, Xpost_Object num,
                      Xpost_Object chunks, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Obj *e;
    size_t total = 0;
    int i, m;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (xpost_object_get_type(chunks) != arraytype)
        return typecheck;
    m = chunks.comp_.sz;
    for (i = 0; i < m; i++)
    {
        Xpost_Object c = xpost_array_get(ctx, chunks, i);

        if (xpost_object_get_type(c) != stringtype)
            return typecheck;
        total += c.comp_.sz;
    }
    if (a.nobjs == a.objcap)
    {
        int nc = a.objcap ? a.objcap * 2 : 4;
        Pdf_Obj *no = (Pdf_Obj *)realloc(a.objs, (size_t)nc * sizeof(Pdf_Obj));

        if (!no)
            return VMerror;
        a.objs = no;
        a.objcap = nc;
    }
    e = &a.objs[a.nobjs];
    e->num = num.int_.val;
    e->body = total ? (char *)malloc(total) : NULL;
    if (total && !e->body)
        return VMerror;
    e->len = total;
    total = 0;
    for (i = 0; i < m && e->body; i++)
    {
        Xpost_Object c = xpost_array_get(ctx, chunks, i);

        memcpy(e->body + total, xpost_string_get_pointer(ctx, c), c.comp_.sz);
        total += c.comp_.sz;
    }
    a.nobjs++;
    /* the grown block must reach /Private even if nothing else does: what
       the record named before the growth has been released */
    if (!_pdf_acc_put(ctx, priv, &a))
    {
        free(e->body);
        free(a.objs);
        return VMerror;
    }
    return 0;
}

/* how many objects are waiting to be written */
static int _pdfobjcount(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.nobjs));
    return 0;
}

/* .pdfobjget  i devdic  .  num string */
static int _pdfobjget(Xpost_Context *ctx, Xpost_Object idx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv, str;
    int i = idx.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (i < 0 || i >= a.nobjs)
        return rangecheck;
    str = xpost_string_cons(ctx, (unsigned)a.objs[i].len, a.objs[i].body);
    if (xpost_object_get_type(str) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.objs[i].num));
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(str));
    return 0;
}

/* .pdfobjclear  devdic  .  -   ; they have been written */
static int _pdfobjclear(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    for (i = 0; i < a.nobjs; i++)
    {
        free(a.objs[i].body);
        a.objs[i].body = NULL;
    }
    a.nobjs = 0;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* .pdfshadd  num devdic  .  index
   .pdfshcount devdic  .  n
   .pdfshget  i devdic  .  num
   .pdfshclear devdic  .  -

   Which object each /Sh the content names is. Kept apart from the bytes
   because the page's resources name these and nothing else: an object
   written for a function is named by the shading that uses it and never
   by a page. */
static int _pdfshadd(Xpost_Context *ctx, Xpost_Object num, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (a.nshs == a.shcap)
    {
        int nc = a.shcap ? a.shcap * 2 : 4;
        int *ns = (int *)realloc(a.shs, (size_t)nc * sizeof(int));

        if (!ns)
            return VMerror;
        a.shs = ns;
        a.shcap = nc;
    }
    a.shs[a.nshs] = num.int_.val;
    i = a.nshs++;
    if (!_pdf_acc_put(ctx, priv, &a))
    {
        free(a.shs);
        return VMerror;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(i));
    return 0;
}

static int _pdfshcount(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.nshs));
    return 0;
}

static int _pdfshget(Xpost_Context *ctx, Xpost_Object idx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i = idx.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (i < 0 || i >= a.nshs)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.shs[i]));
    return 0;
}

static int _pdfshclear(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    a.nshs = 0;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* which of the two tables a call means */
static Pdf_Res **_res_table(Pdf_Acc *a, int kind, int **n, int **cap)
{
    if (kind)
    {
        *n = &a->npats; *cap = &a->patcap; return &a->pats;
    }
    *n = &a->nforms; *cap = &a->formcap; return &a->forms;
}

static int _res_same(const Pdf_Res *e, const Pdf_Res *f)
{
    int i;

    if (e->bodylen != f->bodylen || e->ispat != f->ispat)
        return 0;
    for (i = 0; i < 4; i++)
        if (e->bb[i] != f->bb[i])
            return 0;
    if (e->ispat)
    {
        if (e->xs != f->xs || e->ys != f->ys || e->tt != f->tt)
            return 0;
        for (i = 0; i < 6; i++)
            if (e->mat[i] != f->mat[i])
                return 0;
    }
    return e->bodylen == 0
           || (e->body && f->body
               && memcmp(e->body, f->body, e->bodylen) == 0);
}

/* .pdfresadd  kind dict devdic  .  index

   File a description, or answer with the one already filed that says the
   same thing. A page that draws one form in twenty places files it once,
   which is the whole point of a form; recognising that here rather than
   at the call keeps the comparison next to the bytes it compares. */
static int _pdfresadd(Xpost_Context *ctx, Xpost_Object kind,
                      Xpost_Object d, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv, chunks;
    Pdf_Res **tab, *e, cand;
    int *n, *cap, i, m;
    size_t total = 0;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    tab = _res_table(&a, kind.int_.val, &n, &cap);

    memset(&cand, 0, sizeof(cand));
    cand.ispat = kind.int_.val ? 1 : 0;
    if (_img_nums(ctx, d, "bb", cand.bb, 4) < 0)
        return typecheck;
    if (cand.ispat)
    {
        double one;

        if (_img_nums(ctx, d, "mat", cand.mat, 6) < 0)
            return typecheck;
        cand.tt = _img_int(ctx, d, "tt", 0);
        one = 0.0;
        (void)one;
        cand.xs = xpost_object_number(_img_field(ctx, d, "xs"));
        cand.ys = xpost_object_number(_img_field(ctx, d, "ys"));
    }
    chunks = _img_field(ctx, d, "chunks");
    if (xpost_object_get_type(chunks) != arraytype)
        return typecheck;
    m = chunks.comp_.sz;
    for (i = 0; i < m; i++)
    {
        Xpost_Object c = xpost_array_get(ctx, chunks, i);

        if (xpost_object_get_type(c) != stringtype)
            return typecheck;
        total += c.comp_.sz;
    }
    cand.body = total ? (char *)malloc(total) : NULL;
    if (total && !cand.body)
        return VMerror;
    cand.bodylen = total;
    total = 0;
    for (i = 0; i < m && cand.body; i++)
    {
        Xpost_Object c = xpost_array_get(ctx, chunks, i);

        memcpy(cand.body + total, xpost_string_get_pointer(ctx, c), c.comp_.sz);
        total += c.comp_.sz;
    }
    for (i = 0; i < *n; i++)
        if (_res_same(&(*tab)[i], &cand))
        {
            free(cand.body);
            xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(i));
            return 0;
        }
    if (*n == *cap)
    {
        int nc = *cap ? *cap * 2 : 4;
        Pdf_Res *nr = (Pdf_Res *)realloc(*tab, (size_t)nc * sizeof(Pdf_Res));

        if (!nr)
        {
            free(cand.body);
            return VMerror;
        }
        *tab = nr;
        *cap = nc;
    }
    e = &(*tab)[*n];
    *e = cand;
    i = (*n)++;
    /* the grown block must reach /Private even if nothing else does: what
       the record named before the growth has been released */
    if (!_pdf_acc_put(ctx, priv, &a))
    {
        free(cand.body);
        free(*tab);
        return VMerror;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(i));
    return 0;
}

/* File a description from C, for the font machinery: the same table the
   PostScript side files into, reached without going through an
   operator. Bytes and box only, so a glyph drawn again is recognised by
   what it says rather than by anything the caller has to remember. */
/* Whether this outline is worth a description of its own yet.

   Answers 1 with the description's number when the content should place
   it, and 0 when the content should write the outline out where it
   stands.

   Which sighting files is not a constant, because the trade is not the
   same for every outline. Writing it out n times costs n times its
   length. Filing it costs the description once, an object to hold it,
   and a placement at each occurrence. So filing wins once

       overhead + length + n * placement  <  n * length

   and the sighting to file at is the smallest n that satisfies it. A
   long outline pays for itself the second time it is seen; a short one
   -- a bar, a rule, a comma -- has to come back four or five times
   before it does, and filing it sooner makes the file bigger. MEASURED
   both ways on the same two pages: filing every outline on its second
   sighting takes a nine-page document of ordinary text from 2.16 MB to
   263 kB, and adds three and a half kilobytes to a page of barcodes,
   whose marks are small and come in pairs. The rule below gets the
   first without the second.

   An outline whose length does not exceed a placement can never pay,
   and is never filed.

   The cap is what the record is prepared to hold. Past it an outline is
   written out as it comes, which is what this did before any of it. */
#define PDF_GLY_CAP  4096
/* what an occurrence costs once filed: q, six numbers, cm, the name,
   Do and Q */
#define PDF_GLY_PLACE  45
/* what the description costs to hold: the stream's own dictionary, the
   entry in the page's resources, and the reference to it */
#define PDF_GLY_HOLD   150
/* Open the element a filled path is the body of, in the one place all
   three callers reach for it.

   The rule is written only where it is not SVG's own: nonzero is the
   default a reader assumes, so naming it spends twenty bytes on every
   filled path in the document to say what the document says already --
   and a mesh reaches this a hundred thousand times over. The glyph path
   has always left it out; this is the rest of the tree agreeing.

   Answers the length written. */
/* A colour as the six hex digits an SVG paint takes. A channel is scaled
   and truncated the way the driver contract scales one for a device that
   keeps rows, so the same colour reaches the same eight-bit value however
   it is written down. Eight bits is finer than the flatness a decomposed
   shading is held to, so a fill states the colour the walk settled on. */
static int _svg_fmt_rgb(char *o, double r, double g, double b)
{
    static const char hex[] = "0123456789abcdef";
    double c[3];
    int i, n = 0;

    c[0] = r; c[1] = g; c[2] = b;
    o[n++] = '#';
    for (i = 0; i < 3; i++)
    {
        double t = c[i] * 255.0;
        int v;

        /* Brought inside the channel before the conversion, so what is
           converted is always a value an int holds: a component that
           reached here as a not-a-number or an infinity has no integer
           to convert to. The comparison is written to keep a
           not-a-number, which answers false to every test, at the
           bottom of the channel. */
        if (!(t > 0.0))
            t = 0.0;
        else if (t > 255.0)
            t = 255.0;
        v = (int)t;
        o[n++] = hex[(v >> 4) & 15];
        o[n++] = hex[v & 15];
    }
    return n;
}

int xpost_dev_svg_path_open(char *buf, double r, double g, double b,
                            int evenodd)
{
    int n = 0;

    memcpy(buf + n, "<path fill=\"", 12); n += 12;
    n += _svg_fmt_rgb(buf + n, r, g, b);
    if (evenodd)
    {
        memcpy(buf + n, "\" fill-rule=\"evenodd\" d=\"", 25); n += 25;
    }
    else
    {
        memcpy(buf + n, "\" d=\"", 5); n += 5;
    }
    return n;
}

int xpost_dev_pdf_glyph_form(Xpost_Context *ctx, Xpost_Object devdic,
                             const double *bbox, const char *body,
                             size_t len, int *index)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Gly *g = NULL;
    unsigned h = 2166136261u;
    size_t k;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return 0;
    for (k = 0; k < len; k++)
        h = (h ^ (unsigned char)body[k]) * 16777619u;
    for (i = 0; i < a.nglys; i++)
        if (a.glys[i].hash == h && a.glys[i].len == len &&
            memcmp(a.glys[i].body, body, len) == 0)
        {
            g = &a.glys[i];
            break;
        }
    if (!g)
    {
        if (a.nglys >= PDF_GLY_CAP)
            return 0;
        if (a.nglys == a.glycap)
        {
            int nc = a.glycap ? a.glycap * 2 : 64;
                Pdf_Gly *ng = (Pdf_Gly *)realloc(a.glys, (size_t)nc * sizeof(Pdf_Gly));

            if (!ng)
                return 0;
            a.glys = ng;
            a.glycap = nc;
        }
        g = &a.glys[a.nglys];
        g->hash = h;
        g->len = len;
        g->count = 1;
        g->form = -1;
        g->gen = a.formgen;
        g->body = len ? (char *)malloc(len) : NULL;
        if (len && !g->body)
            return 0;
        if (len)
            memcpy(g->body, body, len);
        a.nglys++;
        if (!_pdf_acc_put(ctx, priv, &a))
        {
            free(g->body);
            free(a.glys);
            return 0;
        }
        return 0;
    }
    g->count++;
    /* The number of a filed description is only good against the record
       that minted it. A writer whose pages are files of their own gives
       the record up at each page end and stamps the next one, and a
       number kept from before that names a description the document
       being written does not carry -- a placement of nothing, or of
       whatever else has since been filed under it. Read as no note at
       all, so the outline is filed again for this document, which the
       count says at once that it will pay for. */
    if (g->form >= 0 && g->gen != a.formgen)
        g->form = -1;
    if (g->form >= 0)
    {
        *index = g->form;
        if (!_pdf_acc_put(ctx, priv, &a))
            return 0;
        return 1;
    }
    if (g->len <= PDF_GLY_PLACE ||
        (double)g->count * (double)(g->len - PDF_GLY_PLACE)
            < (double)(PDF_GLY_HOLD + g->len))
    {
        if (!_pdf_acc_put(ctx, priv, &a))
            return 0;
        return 0;
    }
    /* worth filing now: the store is written back by the filing itself */
    if (!_pdf_acc_put(ctx, priv, &a))
        return 0;
    if (!xpost_dev_pdf_form_file(ctx, devdic, bbox, body, len, index))
        return 0;
    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return 0;
    for (i = 0; i < a.nglys; i++)
        if (a.glys[i].hash == h && a.glys[i].len == len &&
            memcmp(a.glys[i].body, body, len) == 0)
        {
            a.glys[i].form = *index;
            a.glys[i].gen = a.formgen;
            break;
        }
    if (!_pdf_acc_put(ctx, priv, &a))
        return 0;
    return 1;
}

int xpost_dev_pdf_form_file(Xpost_Context *ctx, Xpost_Object devdic,
                            const double *bbox, const char *body, size_t len,
                            int *index)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Res cand, *e;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return 0;
    memset(&cand, 0, sizeof(cand));
    for (i = 0; i < 4; i++)
        cand.bb[i] = bbox[i];
    cand.body = len ? (char *)malloc(len) : NULL;
    if (len && !cand.body)
        return 0;
    if (len)
        memcpy(cand.body, body, len);
    cand.bodylen = len;
    for (i = 0; i < a.nforms; i++)
        if (_res_same(&a.forms[i], &cand))
        {
            free(cand.body);
            *index = i;
            return 1;
        }
    if (a.nforms == a.formcap)
    {
        int nc = a.formcap ? a.formcap * 2 : 4;
        Pdf_Res *nr = (Pdf_Res *)realloc(a.forms, (size_t)nc * sizeof(Pdf_Res));

        if (!nr)
        {
            free(cand.body);
            return 0;
        }
        a.forms = nr;
        a.formcap = nc;
    }
    e = &a.forms[a.nforms];
    *e = cand;
    i = a.nforms++;
    if (!_pdf_acc_put(ctx, priv, &a))
    {
        free(cand.body);
        free(a.forms);
        return 0;
    }
    *index = i;
    return 1;
}

/* how many of that kind the page's content places */
static int _pdfrescount(Xpost_Context *ctx, Xpost_Object kind, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Res **tab;
    int *n, *cap;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    tab = _res_table(&a, kind.int_.val, &n, &cap);
    (void)tab;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(*n));
    return 0;
}

/* .pdfresget  kind i devdic  .  dict  ; one of them, for Emit to write */
static int _pdfresget(Xpost_Context *ctx, Xpost_Object kind,
                      Xpost_Object idx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv, d, arr, str;
    Pdf_Res **tab, *e;
    int *n, *cap, i = idx.int_.val, j;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    tab = _res_table(&a, kind.int_.val, &n, &cap);
    if (i < 0 || i >= *n)
        return rangecheck;
    e = &(*tab)[i];
    d = xpost_dict_cons(ctx, 12);
    if (xpost_object_get_type(d) == invalidtype)
        return VMerror;
#define RESPUT(k, v) \
    do { \
        if (_put_named(ctx, d, k, (v))) \
            return VMerror; \
    } while (0)
    arr = xpost_array_cons(ctx, 4);
    if (xpost_object_get_type(arr) == invalidtype)
        return VMerror;
    for (j = 0; j < 4; j++)
        if (xpost_array_put(ctx, arr, j, xpost_real_cons((real)e->bb[j])))
            return VMerror;
    RESPUT("bb", xpost_object_cvlit(arr));
    if (e->ispat)
    {
        arr = xpost_array_cons(ctx, 6);
        if (xpost_object_get_type(arr) == invalidtype)
            return VMerror;
        for (j = 0; j < 6; j++)
            if (xpost_array_put(ctx, arr, j, xpost_real_cons((real)e->mat[j])))
                return VMerror;
        RESPUT("mat", xpost_object_cvlit(arr));
        RESPUT("xs", xpost_real_cons((real)e->xs));
        RESPUT("ys", xpost_real_cons((real)e->ys));
        RESPUT("tt", xpost_int_cons(e->tt));
    }
    RESPUT("len", xpost_int_cons((int)e->bodylen));
    if (e->obj)
        RESPUT("obj", xpost_int_cons(e->obj));
    if (e->written)
        RESPUT("written", xpost_bool_cons(1));
    {
        /* The body in strings a string can count. A pattern's or a
           form's body is as long as the marks in it, which is longer
           than a string's length field reaches, and the readers of this
           take a run of pieces already. */
        size_t pos = 0;
        int nchunks = (int)((e->bodylen + 65534) / 65535);

        if (nchunks == 0)
            nchunks = 1;
        arr = xpost_array_cons(ctx, (unsigned)nchunks);
        if (xpost_object_get_type(arr) == invalidtype)
            return VMerror;
        for (j = 0; j < nchunks; j++)
        {
            size_t chunk = (size_t)e->bodylen - pos;

            if (chunk > 65535)
                chunk = 65535;
            str = xpost_string_cons(ctx, (unsigned)chunk, e->body + pos);
            if (xpost_object_get_type(str) == invalidtype)
                return VMerror;
            if (xpost_array_put(ctx, arr, j, xpost_object_cvlit(str)))
                return VMerror;
            pos += chunk;
        }
        RESPUT("chunks", xpost_object_cvlit(arr));
    }
#undef RESPUT
    xpost_stack_push(ctx->lo, ctx->os, d);
    return 0;
}

/* .pdfresmark  kind i obj written devdic  .  -

   What the page end learned about one of them: the object it was written
   as, and that it has been written. A document writes each description
   once and every page that places it names that object, so this is the
   part that has to outlast the page it was first placed on. */
static int _pdfresmark(Xpost_Context *ctx, Xpost_Object kind, Xpost_Object idx,
                       Xpost_Object obj, Xpost_Object written,
                       Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Res **tab;
    int *n, *cap, i = idx.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    tab = _res_table(&a, kind.int_.val, &n, &cap);
    if (i < 0 || i >= *n)
        return rangecheck;
    if (obj.int_.val)
        (*tab)[i].obj = obj.int_.val;
    if (written.int_.val)
        (*tab)[i].written = 1;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* .pdfformgen  devdic  .  int
   .pdfformbump devdic  .  -

   Which document the filed descriptions belong to. A caller that keeps
   its own note of a description it filed stamps the note with this, so
   that a note made under a document already closed is read as no note at
   all. It lives with the descriptions because it is only meaningful
   against them: kept where a restore could wind it back, a note from a
   closed document would read as current and name a description that is
   no longer there. */
static int _pdfformgen(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.formgen));
    return 0;
}

static int _pdfformbump(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    a.formgen++;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* .pdfrescut  kind n devdic  .  -   ; keep the first n, give up the rest */
static int _pdfrescut(Xpost_Context *ctx, Xpost_Object kind,
                      Xpost_Object n_, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Res **tab;
    int *n, *cap, i, k = n_.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    tab = _res_table(&a, kind.int_.val, &n, &cap);
    if (k < 0 || k > *n)
        return rangecheck;
    for (i = k; i < *n; i++)
    {
        free((*tab)[i].body);
        (*tab)[i].body = NULL;
    }
    *n = k;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* .pdfopadd  key op mode devdic  .  -

   Record that the content selects the ExtGState numbered key, and what
   that state is. Recording the same number twice is the ordinary case --
   every paint under one overprint setting selects the same one -- and
   says nothing new, so it is a no-op rather than a second entry.

   The number is the caller's: it encodes the state, so equal states
   arrive under equal numbers and the record needs no other key. */
static int _pdfopadd(Xpost_Context *ctx,
                     Xpost_Object key, Xpost_Object op, Xpost_Object mode,
                     Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i, k = key.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    for (i = 0; i < a.nops; i++)
        if (a.ops[i].key == k)
            return 0;
    if (a.nops == a.opcap)
    {
        int nc = a.opcap ? a.opcap * 2 : 8;
        Pdf_Op *no = (Pdf_Op *)realloc(a.ops, (size_t)nc * sizeof(Pdf_Op));

        if (!no)
            return VMerror;
        a.ops = no;
        a.opcap = nc;
    }
    a.ops[a.nops].key = k;
    a.ops[a.nops].op = op.int_.val ? 1 : 0;
    a.ops[a.nops].mode = mode.int_.val;
    a.nops++;
    /* the grown block must reach /Private even if nothing else does: what
       the record named before the growth has been released */
    if (!_pdf_acc_put(ctx, priv, &a))
    {
        free(a.ops);
        return VMerror;
    }
    return 0;
}

/* how many ExtGStates the page's content has selected */
static int _pdfopcount(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.nops));
    return 0;
}

/* .pdfopget  i devdic  .  key op mode  ; one of them, for Emit to
   write the definition the content's selection refers to */
static int _pdfopget(Xpost_Context *ctx, Xpost_Object idx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i = idx.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (i < 0 || i >= a.nops)
        return rangecheck;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.ops[i].key));
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(a.ops[i].op));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.ops[i].mode));
    return 0;
}

/* .pdfopcut  n devdic  .  -

   Keep the first n and drop the rest. The page end keeps none: the next
   page's content selects for itself, and a definition carried over would
   be one the page's own content never asked for. A cell being captured
   marks the count first and cuts back to it if the capture is discarded,
   since a selection made only inside discarded content is a selection
   the page does not carry. The storage is kept for the entries to come. */
static int _pdfopcut(Xpost_Context *ctx, Xpost_Object n, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int k = n.int_.val;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (k < 0 || k > a.nops)
        return rangecheck;
    a.nops = k;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

static int _pdfsepcount(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(a.nseps));
    return 0;
}

/* fetch a registered separation's colour-space body and function object
   body as strings, for Emit to build the resources and objects around */
static int _pdfsepget(Xpost_Context *ctx, Xpost_Object idx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    Pdf_Sep *s;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    if (idx.int_.val < 0 || idx.int_.val >= a.nseps)
        return rangecheck;
    s = &a.seps[idx.int_.val];
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_object_cvlit(xpost_string_cons(ctx, s->csdeflen, s->csdef)));
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_object_cvlit(xpost_string_cons(ctx, s->funclen, s->func)));
    return 0;
}

/* free the accumulator's malloc'd buffer (device Destroy) */
/* truncate the accumulator for the next page, keeping the buffer */
static int _pdfreset(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return 0;
    a.content.len = 0;
    /* The next page is a new stream and carries nothing over from this
       one: the state a consumer reads starts at the PDF defaults again.
       The pending text's buffer is kept and emptied rather than given
       up, since the next page builds its operators in the same one. */
    if (a.gs)
    {
        memset(a.gs->level, 0, sizeof(a.gs->level));
        a.gs->depth = 0;
        a.gs->over = 0;
        a.gs->pend.len = 0;
        /* a page whose capture never closed -- an error out of a paint
           procedure caught by the job -- ends with it */
        a.gs->insub = 0;
    }
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* Gives up everything the accumulator holds, at the end of a page. */
static int _pdffree(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int i;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return 0;
    xpost_strbuf_free(&a.content);
    if (a.gs)
        xpost_strbuf_free(&a.gs->pend);
    free(a.gs);
    a.gs = NULL;
    for (i = 0; i < a.nseps; i++)
    {
        free(a.seps[i].name);
        free(a.seps[i].csdef);
        free(a.seps[i].func);
    }
    free(a.seps);
    a.seps = NULL;
    a.nseps = 0;
    a.sepcap = 0;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* Release the memory a device holds outside virtual memory, without
   executing any PostScript.

   Destroy is the method that does this, and for the devices written in C
   it is an operator, so it is called the way the interpreter's own
   shutdown calls it: the instance goes on the operand stack and the
   operator runs from here. That operator is the release recorded with
   the instance's block when the block was issued, passed in here, rather
   than read out of the instance now -- see _devinstalled below.

   For the devices written in PostScript, Destroy is a procedure, and the
   callers of this are places no procedure can run: restore is one
   operator, not an interpreter loop. What those classes hold outside
   virtual memory is the vector writers' content accumulator, reached
   here directly. The rest keep their raster as PostScript objects the
   collector already owns and have nothing to answer: the fetch resolves
   the content block a vector writer was issued, which nothing else was,
   and returns.

   Destroy is idempotent (xpost_dev_driver.h), so a device this releases
   after something else already has is a no-op rather than a double
   free. */
static void _device_release(Xpost_Context *ctx,
                            Xpost_Object devdic,
                            Xpost_Object destroy)
{
    if (xpost_object_get_type(devdic) != dicttype)
        return;
    if (xpost_object_get_type(destroy) == operatortype)
    {
        int res;

        if (!xpost_stack_push(ctx->lo, ctx->os, devdic))
            return;
        res = xpost_operator_exec(ctx, destroy.mark_.padw);
        if (res)
            XPOST_LOG_ERR("%s retiring a page device", errorname[res]);
        return;
    }
    (void)_pdffree(ctx, devdic);
}

/* devdic  .devinstalled  -
   record the device setpagedevice has just installed in the graphics
   state, with the save depth it was installed at and the method that
   will release it */
static int _devinstalled(Xpost_Context *ctx, Xpost_Object devdic)
{
    unsigned int depth = 0;
    Xpost_Object destroy;

    if (xpost_memory_save_stack_ready(ctx->lo))
        depth = (unsigned int)xpost_stack_count(ctx->lo,
                    xpost_memory_save_stack_ent(ctx->lo));
    /* the depth is recorded as depth + 1 so that zero means nothing is
       recorded; a save stack cannot exceed 255 levels (xpost_op_save.c),
       and the ceiling here keeps the arithmetic in step with that */
    if (depth > 254)
        depth = 254;

    /* What the release will run is settled here rather than read at the
       release, which happens inside restore: one operator, not an
       interpreter loop, with nothing above it to catch what it raises.

       The instance is sealed at the end of its installation and a page
       device request's method-named keys are not taken from it, so what
       stands under /Destroy is the class's own method. This does not
       read it even so: the release runs inside restore, where a lookup
       that resolved to the wrong thing has nothing above it to catch
       what it raises. What runs is settled while the block is issued,
       from a dictionary that is still the device's own.

       The release run is the one the instance's own state was issued to
       be given up by, recorded with the block when the block was issued
       -- from the dictionary the device's own Create had just filled,
       before the program regained control. A device carrying such a
       block is released by that operator whatever stands under /Destroy
       when the release comes, so a device is given up correctly rather
       than left leaking or released by whatever the slot then holds; an
       operator of the program's choosing, and the
       release of another class whose block is the same width, run
       neither here nor from the restore.

       A device carrying no such block -- the vector writers, whose
       Destroy is a procedure, and the devices whose raster the collector
       already owns -- is released the other way, by the accumulator path
       in _device_release, which resolves the content block a vector
       writer was issued and finds none in anything else. */
    {
        unsigned int release = xpost_handle_device_release(ctx, devdic);

        destroy = release ? xpost_operator_cons_opcode((int)release) : null;
    }

    ctx->pagedevice = devdic;
    ctx->pagedevice_destroy = destroy;
    ctx->pagedevice_depth = depth + 1;
    return 0;
}

/* Retires a page device whose installation a restore has just undone. */
void xpost_device_retire_restored(Xpost_Context *ctx, unsigned int level)
{
    Xpost_Object devdic;
    Xpost_Object destroy;

    /* The install is a write to the graphics state template, so a
       restore reverts it exactly when it was made at a depth greater
       than the one being restored to -- which is the test below, the
       recorded depth being one more than the depth of the write. A
       restore that leaves the install standing displaces nothing and
       must retire nothing: a program that saves and restores around a
       page it has already set up goes on painting on that page. */
    if (ctx->pagedevice_depth == 0 || ctx->pagedevice_depth <= level + 1)
        return;

    /* The device is released here rather than left to the collector
       because the collector cannot release it: what it holds is a raster
       or a content accumulator outside virtual memory, reached only
       through the private string in the instance dictionary. Leaving it
       to be swept would be worse than leaving it alone -- the sweep
       hands the dictionary's entity back to the free list, and a release
       run afterwards reads whatever took the entity's place as the
       device's private state and frees a pointer out of it.

       The record is cleared before the release rather than after, so a
       release that fails partway leaves nothing naming a device that has
       begun to give up its memory. */
    devdic = ctx->pagedevice;
    destroy = ctx->pagedevice_destroy;
    ctx->pagedevice = null;
    ctx->pagedevice_destroy = null;
    ctx->pagedevice_depth = 0;
    _device_release(ctx, devdic, destroy);
}

/* Retire a page device a job installed, at the job boundary. Like the
   restore retirement above, but held to the depth the baseline stood at
   rather than a save level: the boundary reverts a whole job, so a device
   installed above that depth is displaced and released here -- while
   virtual memory is intact and the device's Destroy can still run and
   reach its output -- because the image restore that follows drops the
   device dictionary and only the release frees the raster or accumulator
   it holds outside the arena. The baseline's own device, at or below that
   depth, is left standing to serve the next job. */
void xpost_device_retire_job(Xpost_Context *ctx, unsigned int baseline_depth)
{
    Xpost_Object devdic;
    Xpost_Object destroy;

    if (ctx->pagedevice_depth <= baseline_depth)
        return;
    devdic = ctx->pagedevice;
    destroy = ctx->pagedevice_destroy;
    ctx->pagedevice = null;
    ctx->pagedevice_destroy = null;
    ctx->pagedevice_depth = baseline_depth;
    _device_release(ctx, devdic, destroy);
}

/* Installs the operators every device class is built out of: scan
   conversion, the page and buffer machinery, and the vector
   accumulator. */
int xpost_oper_init_generic_device_ops(Xpost_Context *ctx,
                                       Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    optab = xpost_operator_table(ctx->gl);

    op = xpost_operator_cons(ctx, ".yxsort", (Xpost_Op_Func)_yxsort, 1, arraytype); INSTALL;
    op = xpost_operator_cons(ctx, ".fillpoly", (Xpost_Op_Func)_fillpoly, 2, arraytype, dicttype); INSTALL;
    _fillpoly_opcode = op.mark_.padw;
    _region_memo_flush();
    op = xpost_operator_cons(ctx, ".regionmeet", (Xpost_Op_Func)_regionmeet, 4,
                             arraytype, arraytype, integertype, integertype); INSTALL;
    op = xpost_operator_cons(ctx, ".regionmeet", (Xpost_Op_Func)_regionmeet, 4,
                             arraytype, stringtype, integertype, integertype); INSTALL;
    op = xpost_operator_cons(ctx, ".newregionserial", (Xpost_Op_Func)_newregionserial, 0); INSTALL;
    op = xpost_operator_cons(ctx, ".eospanpoly", (Xpost_Op_Func)_eospanpoly, 1, arraytype); INSTALL;
    op = xpost_operator_cons(ctx, ".eospanpoly", (Xpost_Op_Func)_eospanpoly_rows, 3,
                             arraytype, integertype, integertype); INSTALL;
    op = xpost_operator_cons(ctx, ".pathspanparts", (Xpost_Op_Func)_pathspanparts, 2,
                             stringtype, booleantype); INSTALL;
    op = xpost_operator_cons(ctx, ".blitrow", (Xpost_Op_Func)xpost_dev_blit_row, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".rectspan", (Xpost_Op_Func)_rectspan, 6,
            numbertype, numbertype, numbertype, numbertype,
            numbertype, numbertype); INSTALL;
    op = xpost_operator_cons(ctx, ".linepix", (Xpost_Op_Func)_linepix, 6,
            numbertype, numbertype, numbertype, numbertype,
            numbertype, numbertype); INSTALL;
    op = xpost_operator_cons(ctx, ".fillrectgray", (Xpost_Op_Func)_fillrectgray, 6,
            numbertype, numbertype, numbertype, numbertype, numbertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".blendpixgray", (Xpost_Op_Func)_blendpixgray, 5,
            numbertype, numbertype, numbertype, numbertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".blendpixrgb", (Xpost_Op_Func)_blendpixrgb, 7,
            numbertype, numbertype, numbertype, numbertype, numbertype, numbertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".fillrectrgb", (Xpost_Op_Func)_fillrectrgb, 8,
                             numbertype, numbertype, numbertype, numbertype,
                             numbertype, numbertype, numbertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".base64", (Xpost_Op_Func)_base64, 1, stringtype); INSTALL;
    op = xpost_operator_cons(ctx, ".pngencode", (Xpost_Op_Func)_pngencode, 3,
                             arraytype, integertype, integertype); INSTALL;
    op = xpost_operator_cons(ctx, ".writebitrows", (Xpost_Op_Func)_writebitrows, 2,
                             arraytype, filetype); INSTALL;
    op = xpost_operator_cons(ctx, ".writergbrows", (Xpost_Op_Func)_writergbrows, 2,
                             arraytype, filetype); INSTALL;
    op = xpost_operator_cons(ctx, ".screenink", (Xpost_Op_Func)_screenink, 4,
                             numbertype, numbertype, numbertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".flatecompress", (Xpost_Op_Func)_flatecompress, 1, arraytype); INSTALL;
    op = xpost_operator_cons(ctx, ".dctcompress", (Xpost_Op_Func)_dctcompress, 5,
            arraytype, integertype, integertype, integertype, integertype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdffillpoly", (Xpost_Op_Func)_pdffillpoly, 2,
            arraytype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".svgfillpoly", (Xpost_Op_Func)_svgfillpoly, 5,
            numbertype, numbertype, numbertype, arraytype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfinit", (Xpost_Op_Func)_pdfinit, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfput", (Xpost_Op_Func)_pdfput, 2, stringtype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfsput", (Xpost_Op_Func)_pdfsput, 2,
            stringtype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfsend", (Xpost_Op_Func)_pdfsend, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfscarried", (Xpost_Op_Func)_pdfscarried, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfsave", (Xpost_Op_Func)_pdfsave, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfrestore", (Xpost_Op_Func)_pdfrestore, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfsubbegin", (Xpost_Op_Func)_pdfsubbegin, 2,
                             booleantype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfsubend", (Xpost_Op_Func)_pdfsubend, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfchunks", (Xpost_Op_Func)_pdfchunks, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdffree", (Xpost_Op_Func)_pdffree, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".devinstalled", (Xpost_Op_Func)_devinstalled, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfreset", (Xpost_Op_Func)_pdfreset, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfnumstr", (Xpost_Op_Func)_pdfnumstr, 1,
            numbertype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdffindsep", (Xpost_Op_Func)_pdffindsep, 2,
            stringtype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfregsep", (Xpost_Op_Func)_pdfregsep, 4,
            stringtype, stringtype, stringtype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfcost", (Xpost_Op_Func)_pdfcost, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfobjadd", (Xpost_Op_Func)_pdfobjadd, 3,
            integertype, arraytype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfobjcount", (Xpost_Op_Func)_pdfobjcount, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfobjget", (Xpost_Op_Func)_pdfobjget, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfobjclear", (Xpost_Op_Func)_pdfobjclear, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfshadd", (Xpost_Op_Func)_pdfshadd, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfshcount", (Xpost_Op_Func)_pdfshcount, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfshget", (Xpost_Op_Func)_pdfshget, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfshclear", (Xpost_Op_Func)_pdfshclear, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfformgen", (Xpost_Op_Func)_pdfformgen, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfformbump", (Xpost_Op_Func)_pdfformbump, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfresadd", (Xpost_Op_Func)_pdfresadd, 3,
            integertype, dicttype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfrescount", (Xpost_Op_Func)_pdfrescount, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfresget", (Xpost_Op_Func)_pdfresget, 3,
            integertype, integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfresmark", (Xpost_Op_Func)_pdfresmark, 5,
            integertype, integertype, integertype, booleantype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfrescut", (Xpost_Op_Func)_pdfrescut, 3,
            integertype, integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfimgadd", (Xpost_Op_Func)_pdfimgadd, 2,
            dicttype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfimgcount", (Xpost_Op_Func)_pdfimgcount, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfimgget", (Xpost_Op_Func)_pdfimgget, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfimgcut", (Xpost_Op_Func)_pdfimgcut, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfopadd", (Xpost_Op_Func)_pdfopadd, 4,
            integertype, booleantype, integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfopcount", (Xpost_Op_Func)_pdfopcount, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdffontres", (Xpost_Op_Func)_pdffontres, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdffontclear", (Xpost_Op_Func)_pdffontclear, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdftextflush", (Xpost_Op_Func)_pdftextflush, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfopget", (Xpost_Op_Func)_pdfopget, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfopcut", (Xpost_Op_Func)_pdfopcut, 2,
            integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfsepcount", (Xpost_Op_Func)_pdfsepcount, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfsepget", (Xpost_Op_Func)_pdfsepget, 2,
            integertype, dicttype); INSTALL;
    if (xpost_object_get_type((nameImgData = xpost_name_cons(ctx, "ImgData"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameFillRect = xpost_name_cons(ctx, "FillRect"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namepdfPrivate = xpost_name_cons(ctx, "Private"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotground = xpost_name_cons(ctx, ".ground"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotbandtop = xpost_name_cons(ctx, ".bandtop"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotbandrows = xpost_name_cons(ctx, ".bandrows"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedothtcell = xpost_name_cons(ctx, ".htcell"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedothtw = xpost_name_cons(ctx, ".htw"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedothth = xpost_name_cons(ctx, ".hth"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotcopydict = xpost_name_cons(ctx, ".copydict"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotinstance = xpost_name_cons(ctx, ".instance"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotstate = xpost_name_cons(ctx, ".state"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namenativecolorspace = xpost_name_cons(ctx, "nativecolorspace"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameDeviceGray = xpost_name_cons(ctx, "DeviceGray"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameDeviceRGB = xpost_name_cons(ctx, "DeviceRGB"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameroll = xpost_name_cons(ctx, "roll"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameDrawLine = xpost_name_cons(ctx, "DrawLine"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameexec = xpost_name_cons(ctx, "exec"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namerepeat = xpost_name_cons(ctx, "repeat"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namecvx = xpost_name_cons(ctx, "cvx"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameRbracket = xpost_name_cons(ctx, "]"))) == invalidtype)
        return VMerror;

    return 0;
}
