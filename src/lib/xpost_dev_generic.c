/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
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

static Xpost_Object namewidth;
static Xpost_Object nameheight;
static Xpost_Object namedotcopydict;
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
       machinery puts on the device before it runs Emit (.transmitpage,
       data/device.ps). It is the name and not the template: the template
       may carry a %d, and the page number that replaces it is the page's
       to know, not the device's. A device that read the template instead
       wrote every page of a job to one name. */
    namestr = xpost_dict_get(ctx, devdic,
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
   one pixel-row band, still in real device coordinates */
struct rspan
{
    int band;
    real lo, hi;
};

static
int _rspan_push(struct rspan **rsp, int *cap, int *n,
                int band, real lo, real hi)
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
int _rspan_collect(Xpost_Span_Consumer *c, int band, real lo, real hi)
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

    top = xpost_dict_get(ctx, devdic, namedotbandtop);
    nrows = xpost_dict_get(ctx, devdic, namedotbandrows);
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
static
int _rect_paint(Xpost_Span_Consumer *c, int band, real lo, real hi)
{
    struct _rect_painter *p = (struct _rect_painter *)c;
    integer xlo = (integer)floor(lo);
    integer xhi = (integer)ceil(hi);
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
           with nothing cached */
        _region_memo_flush();
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
        real lo = (real)floor(r[i].lo);
        real hi = (real)ceil(r[i].hi);

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
                real L = S[i2].lo > C[j2].lo ? S[i2].lo : C[j2].lo;
                real R = S[i2].hi < C[j2].hi ? S[i2].hi : C[j2].hi;

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
    Xpost_Object c = xpost_dict_get(ctx, devdic, namedothtcell);
    Xpost_Object wo = xpost_dict_get(ctx, devdic, namedothtw);
    Xpost_Object ho = xpost_dict_get(ctx, devdic, namedothth);

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

#define COMP(v) xpost_real_cons((real)(((v) + 0.5) / 255.0))
        if (o->ncomp == 3)
        {
            xpost_stack_push(ctx->lo, ctx->os, COMP(r));
            xpost_stack_push(ctx->lo, ctx->os, COMP(g));
            xpost_stack_push(ctx->lo, ctx->os, COMP(b));
        }
        else
            xpost_stack_push(ctx->lo, ctx->os, COMP(gray));
#undef COMP
        /* the contract's rectangle is an inclusive span, so a run of n
           columns on one row is w = n-1 and h = 0 */
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(dx0));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(dy));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(dx1 - dx0 - 1));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(0));
        xpost_stack_push(ctx->lo, ctx->os, o->devdic);
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
   and the masks: mbits, one bit per pixel high-order first in rows
   of mrowb bytes (set = leave unpainted), and mranges, raw min,max
   pairs (a pixel inside every range is left unpainted). Pixels cover
   device pixels by the any-part-of-pixel rule, the high edge
   exclusive, which is the rule the rectangle fills cover them by.

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

    banded = xpost_object_get_type(
                 xpost_dict_get(ctx, devdic,
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
    h = xpost_dict_get(ctx, ctx->globalprivatedict,
                       xpost_name_cons(ctx, ".hostdict"));
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
    }
    *count = n;
    return roster;
}

/* The half of a device's Create that runs before the class procedure
   does. It records the page's size on the class and arranges for the
   device's own continuation to run after the class has copied itself,
   since neither can be done until the other has finished. */
int xpost_dev_create_begin(Xpost_Context *ctx,
                           Xpost_Object width,
                           Xpost_Object height,
                           Xpost_Object classdic,
                           unsigned int cont_opcode)
{
    int ret;

    /* the three the continuation will be called with: it is an operator
       like this one and takes its operands the same way, so they go back
       where they came from rather than being carried in C */
    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    xpost_stack_push(ctx->lo, ctx->os, classdic);

    ret = xpost_dict_put(ctx, classdic, namewidth, width);
    if (ret)
        return ret;
    ret = xpost_dict_put(ctx, classdic, nameheight, height);
    if (ret)
        return ret;

    /* the class procedure first, then the device's continuation: pushed
       in the reverse of the order they run in, the execution stack
       being read from the top */
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_operator_cons_opcode(cont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;

    return 0;
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

    mbitso = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "mbits"));
    if (xpost_object_get_type(mbitso) == stringtype)
    {
        Xpost_Object o = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "mrowb"));
        if (xpost_object_get_type(o) != integertype)
            return typecheck;
        mrowb = o.int_.val;
        /* the mask holds one row of mrowb bytes per sample row and the
           row index selects among them, so the bits through row y must
           be there; the stride is compared against the length divided
           by the row count, which holds for every stride rather than
           only those whose product with the count fits the type */
        if (mrowb < 0
         || (unsigned int)mrowb > mbitso.comp_.sz / ((unsigned int)y + 1))
            return rangecheck;
        mbits = (unsigned char *)xpost_string_get_pointer(ctx, mbitso);
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
                                if (mbits
                                 && (mbits[msy * mrowb + (xm >> 3)]
                                     >> (7 - (xm & 7)) & 1))
                                    continue;
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
        int rret, paint;

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

        for (x = 0; x < w; x++)
        {
            double xa, xb;
            int r = 0, g = 0, b = 0, gray = 0;

            if (mbits)
            {
                int bit = mbits[y * mrowb + (x >> 3)] >> (7 - (x & 7)) & 1;
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
                    sret = _blit_out_span(&out, dy, (int)floor(sa),
                                          (int)ceil(sb), r, g, b, gray);
                    if (sret)
                        return sret;
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

/* write a PDF number: an integer when integral, else up to four decimals
   with trailing zeros trimmed (never exponential). round(v*10000) avoids
   binary-float print noise; 0.0001pt is finer than any raster grid the
   consumer will draw on. */
static int _pdf_fmt_num(char *o, double v)
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
        long m = (long)round(v * 10000.0);
        long ip, fp;
        int len = 0, digits = 4;
        if (m < 0) { o[len++] = '-'; m = -m; }
        ip = m / 10000;
        fp = m % 10000;
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

typedef struct
{
    Xpost_String_Buffer content;
    Pdf_Sep *seps;
    int nseps;
    int sepcap;
    int *sephash;    /* name index: a separation's position + 1, 0 empty */
    int sephashcap;  /* a power of two, or 0 when the index is absent */
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
}

/* Create the content accumulator and stash it in the device's /Private. Called
   from the device Create method, before any user save/restore. */
static int _pdfinit(Xpost_Context *ctx, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int ret;

    xpost_strbuf_init(&a.content, 4096);
    a.seps = NULL;
    a.nseps = 0;
    a.sepcap = 0;
    a.sephash = NULL;
    a.sephashcap = 0;
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
static int _pdfput(Xpost_Context *ctx, Xpost_Object str, Xpost_Object devdic)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    ret = xpost_strbuf_append(&a.content,
                              xpost_string_get_pointer(ctx, str), str.comp_.sz);
    if (ret)
        return ret;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* Exported accumulator access for the text operators: they build a
   complete content-stream fragment per glyph outline and append it in
   one call. */
int xpost_dev_pdf_append(Xpost_Context *ctx, Xpost_Object devdic,
                         const char *s, size_t n)
{
    Pdf_Acc a;
    Xpost_Object priv;
    int ret;

    if (!_pdf_acc_get(ctx, devdic, &priv, &a))
        return undefined;
    ret = xpost_strbuf_append(&a.content, s, n);
    if (ret)
        return ret;
    if (!_pdf_acc_put(ctx, priv, &a))
        return VMerror;
    return 0;
}

/* Exported PDF number formatter (see _pdf_fmt_num) */
int xpost_dev_pdf_fmt_num(char *o, double v)
{
    return _pdf_fmt_num(o, v);
}

/* Emit the content-stream operators for a filled path into the accumulator:
   the flattened subpaths ("x y m" / "x y l", closed with "h") and a
   nonzero-winding fill ("f") -- the rule the fill operator has: overlapping
   subpaths union, and hole subpaths are counter-wound by their producers.
   This is the per-coordinate hot loop of the pdfwrite FillPoly,
   in C; the fill colour is the device's business, emitted beforehand. */
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
            ret = xpost_strbuf_append(&a.content, tmp, len);
        }
        else if (!needmove)   /* null subpath separator: close the subpath */
        {
            ret = xpost_strbuf_append(&a.content, "h\n", 2);
            needmove = 1;
        }
    }
    if (ret == 0 && !needmove)
        ret = xpost_strbuf_append(&a.content, "h\n", 2);
    if (ret == 0)
        ret = xpost_strbuf_append(&a.content, "f\n", 2);

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

    len = 0;
    memcpy(tmp + len, "<path fill=\"rgb(", 16); len += 16;
    len += _pdf_fmt_num(tmp + len, PDFNUMVAL(r) * 100); tmp[len++] = '%'; tmp[len++] = ',';
    len += _pdf_fmt_num(tmp + len, PDFNUMVAL(g) * 100); tmp[len++] = '%'; tmp[len++] = ',';
    len += _pdf_fmt_num(tmp + len, PDFNUMVAL(b) * 100); tmp[len++] = '%';
    memcpy(tmp + len, ")\" fill-rule=\"nonzero\" d=\"", 26); len += 26;
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
            len += _pdf_fmt_num(tmp + len, x); tmp[len++] = ' ';
            len += _pdf_fmt_num(tmp + len, y);
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

       The instance dictionary is an ordinary dictionary and the program
       writes to it. A page device request's keys are stored into the
       instance setpagedevice builds, so /Destroy is a slot a program
       reaches without naming anything internal, and the instance itself
       is reachable through the graphics state afterwards. So what stands
       there now is not the class's method to read.

       The release run is the one the instance's own state was issued to
       be given up by, recorded with the block when the block was issued
       -- from the dictionary the device's own Create had just filled,
       before the program regained control. A device carrying such a
       block is released by that operator whatever the program has since
       written under /Destroy, so a self-sabotaged device is still given
       up correctly rather than left leaking or released by whatever the
       slot now holds; an operator of the program's choosing, and the
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
    op = xpost_operator_cons(ctx, ".writebitrows", (Xpost_Op_Func)_writebitrows, 2,
                             arraytype, filetype); INSTALL;
    op = xpost_operator_cons(ctx, ".writergbrows", (Xpost_Op_Func)_writergbrows, 2,
                             arraytype, filetype); INSTALL;
    op = xpost_operator_cons(ctx, ".screenink", (Xpost_Op_Func)_screenink, 4,
                             numbertype, numbertype, numbertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".flatecompress", (Xpost_Op_Func)_flatecompress, 1, arraytype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdffillpoly", (Xpost_Op_Func)_pdffillpoly, 2,
            arraytype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".svgfillpoly", (Xpost_Op_Func)_svgfillpoly, 5,
            numbertype, numbertype, numbertype, arraytype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfinit", (Xpost_Op_Func)_pdfinit, 1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".pdfput", (Xpost_Op_Func)_pdfput, 2, stringtype, dicttype); INSTALL;
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
    if (xpost_object_get_type((namewidth = xpost_name_cons(ctx, "width"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameheight = xpost_name_cons(ctx, "height"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotcopydict = xpost_name_cons(ctx, ".copydict"))) == invalidtype)
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
