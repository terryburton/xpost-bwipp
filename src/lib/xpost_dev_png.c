/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_png.c
 * @brief The PNG output device, with and without an alpha channel.
 *
 * A raster held to the end of the page and then encoded, rather than a
 * device drawn on directly.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifdef HAVE_LIBPNG

#include <stddef.h> /* offsetof */
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <setjmp.h>

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
#include "xpost_name.h" /* create names */

#include "xpost_operator.h" /* create operators */
#include "xpost_op_dict.h" /* call load operator for convenience */
#include "xpost_dev_generic.h" /* the page file opener */
#include "xpost_dev_driver.h" /* device contract and shared helpers */
#include "xpost_dev_png.h" /* check prototypes */

typedef struct
{
    unsigned char red, green, blue, alpha;
} Xpost_Png_Pixel;

typedef struct
{
    int width, height, byte_stride;
    /* the block this raster is part of. A client is handed the raster
       and gives the block back, so the block's own address is kept
       here, immediately before the raster, where the release entry
       point reads it. */
    void *block;
    Xpost_Png_Pixel data[1];
} Xpost_Png_Buffer;

XPOST_DEV_ASSERT_BLOCK_PRECEDES_RASTER(png, Xpost_Png_Buffer, block, data);

/* A PNG stream holds exactly one image, so the file and the writer that
   fills it belong to the page and not to the device: both are made as a
   page begins and finished as it ends. What the instance keeps between
   pages is the raster and the two settings the pages are written under.

   A page arriving a band at a time is written across several Emit calls
   (doc/xpost_design.dox), and the file and the writer outlive each of them --
   which is the whole of what makes the filter right at a band's edge. A
   PNG row is filtered against the row before it, and the writer holds
   that row for as long as it is alive, so the device keeps the writer
   rather than keeping the row. */
typedef struct
{
    int width;
    int height;
    /*
     * add additional members to private struct
     */
    Xpost_Png_Buffer *buf;
    /* the run of the page's rows the raster stands for, and how far
       down the page the file has been written */
    Xpost_Dev_Band band;
    FILE *file;
    png_structp png;
    png_infop info;
    /* one row of the page's ground, for the rows this device is not
       holding: written to the file where a device holding the whole
       page would have written what erasepage left there */
    unsigned char *ground;
    unsigned int interlaced : 1;
    unsigned int alpha : 1;
    /* the row filters the codec may choose between, as the library
       spells the set; 0 is the whole menu, which is the library's own
       default and so asks it for nothing */
    unsigned char filters;
    /* the device allocated buf and has not handed it to the client
       through OutputBufferOut, so Destroy frees it */
    unsigned int bufowned : 1;
} PrivateData;

/* Defined below, next to the stream it gives up. */
static void _reclaim(void *block);

static Xpost_Object namePrivate;
static Xpost_Object namewidth;
static Xpost_Object nameheight;
static Xpost_Object namedotcopydict;
static Xpost_Object namenativecolorspace;
static Xpost_Object nameDeviceRGB;
static Xpost_Object namedotbandpage;


static unsigned int _create_cont_opcode;

/* create an instance of the device
   using the class .copydict procedure */
static
int _create(Xpost_Context *ctx,
            Xpost_Object width,
            Xpost_Object height,
            Xpost_Object classdic)
{
    return xpost_dev_create_begin(ctx, width, height, classdic,
                                  _create_cont_opcode);
}

/* Leave a run of the buffer's own rows as a raster fresh from Create:
   opaque white, or the transparent white the alpha device starts on.

   Create lays the whole buffer down this way, and so does every move to
   another run of the page's rows -- a raster standing for one run after
   another has to start each run as a fresh raster would, or what the
   run before painted shows through wherever this one paints nothing.
   The rows are the buffer's, not the page's, since what moves is which
   of the page's rows they stand for. */
static void _clear(PrivateData *p, int from, int to)
{
    Xpost_Png_Pixel init;
    Xpost_Dev_Raster_Offset i, n;

    if (!xpost_dev_band_clamp_rows(&p->band, &from, &to))
        return;

    init.red = init.green = init.blue = 255;
    init.alpha = p->alpha ? 0 : 255;
    i = xpost_dev_raster_offset(0, from, p->width);
    n = xpost_dev_raster_offset(0, to + 1, p->width);
    for (; i < n; i++)
        p->buf->data[i] = init;
}

/* initialize the C-level data
   and define in the device instance */
static
int _create_cont(Xpost_Context *ctx,
                 Xpost_Object w,
                 Xpost_Object h,
                 Xpost_Object devdic)
{
    PrivateData private;
    Xpost_Object privatestr;
    Xpost_Object interlaced_o;
    int width, height;
    int ret;

    /* The page the program asked for, as the extent the buffer's row
       arithmetic is done in; a page naming an extent that arithmetic
       does not carry is refused before anything is built for it. How
       many of the page's rows the buffer holds is settled below and is
       a separate question. */
    if (!xpost_dev_page_extent(w.int_.val, h.int_.val, &width, &height))
        return limitcheck;

    /* The block this device's instance state lives in, and, named with
       it rather than after it, what gives up whatever that state names.
       What this device holds is a raster and the stream a page goes out
       through, which are not virtual memory: a device the run never
       retires -- one a restore took back, or one nothing named by the
       time a collection came round -- would take them with it. This is
       what gives them up there. A device the run does retire has given
       them up already and leaves this nothing to do. */
    ret = xpost_handle_cons(ctx, devdic, namePrivate, &privatestr,
                            XPOST_HANDLE_DEVICE, sizeof(PrivateData),
                            _reclaim);
    if (ret)
        return ret;

    private.width = width;
    private.height = height;
    private.file = NULL;
    private.png = NULL;
    private.info = NULL;
    private.ground = NULL;
    {
        Xpost_Object alpha_o = xpost_dict_get(ctx, devdic,
                                              xpost_name_cons(ctx, "AlphaChannel"));
        private.alpha = xpost_object_get_type(alpha_o) == booleantype
                     && alpha_o.int_.val;
    }

    /*
     *
     * initialize additional members of private struct
     *
     */

    interlaced_o = xpost_dict_get(ctx, devdic,
                                  xpost_name_cons(ctx, "png_interlaced"));

    if (xpost_object_get_type(interlaced_o) == invalidtype)
        private.interlaced = PNG_INTERLACE_NONE;
    else
    {
        if (interlaced_o.int_.val)
        {
#ifdef PNG_WRITE_INTERLACING_SUPPORTED
            private.interlaced = PNG_INTERLACE_ADAM7;
#else
            private.interlaced = PNG_INTERLACE_NONE;
#endif
        }
        else
            private.interlaced = PNG_INTERLACE_NONE;
    }
    XPOST_LOG_INFO("PNG interlacing: %s",
                   (private.interlaced == PNG_INTERLACE_ADAM7) ? "Adam7" : "none");

    /* Which row filters the codec may choose between. Choosing is a
       walk of every row under each candidate, so the choice is paid per
       row written and a page of flat runs buys nothing with it; a
       caller who knows its pages can narrow the menu. The default is
       the whole menu, which is what the library does unasked, so a run
       that says nothing gets today's bytes.

       The vocabulary is closed, and a word outside it is refused the
       way a device refuses a mode it does not take: a value that fell
       back to the default silently would leave a misspelling choosing
       the dearest filters and reporting nothing. */
    private.filters = 0;
    {
        Xpost_Object filter_o = xpost_dict_get(ctx, devdic,
                                               xpost_name_cons(ctx, "png_filter"));

        if (xpost_object_get_type(filter_o) != invalidtype)
        {
            Xpost_Object str = filter_o;
            const char *t = NULL;
            unsigned int tn = 0;

            if (xpost_object_get_type(str) == nametype)
                str = xpost_name_get_string(ctx, str);
            if (xpost_object_get_type(str) == stringtype)
            {
                t = xpost_string_get_pointer(ctx, str);
                tn = str.comp_.sz;
            }
            if (t && tn == 8 && memcmp(t, "adaptive", 8) == 0)
                private.filters = 0;
            else if (t && tn == 11 && memcmp(t, "none-sub-up", 11) == 0)
                private.filters = PNG_FILTER_NONE | PNG_FILTER_SUB
                                | PNG_FILTER_UP;
            else if (t && tn == 4 && memcmp(t, "none", 4) == 0)
                private.filters = PNG_FILTER_NONE;
            else
            {
                XPOST_LOG_ERR("%d the png device takes no filter \"%.*s\";"
                              " the filters it takes are: adaptive,"
                              " none-sub-up, none", rangecheck,
                              t ? (int)tn : 0, t ? t : "");
                return rangecheck;
            }
        }
    }

    /* The run of the page's rows this device is to hold. It is held
       whole where no row of it can be given up before the page is
       complete: an interlaced image is written in seven passes over the
       page, so every row is wanted again after the last one has been
       written, and a raster an embedder asked for is the whole page by
       the contract it asked under (XPOST_OUTPUT_BUFFEROUT, xpost.h).
       Either way the run still says which rows take marks, so a caller
       playing a page back a band at a time gets the page it would have
       got; what it does not get is the bound. */
    xpost_dev_band_take(ctx, devdic, height,
                        private.interlaced == PNG_INTERLACE_ADAM7
                        || xpost_object_get_type(
                               xpost_context_host_setting(ctx, "OutputBufferOut"))
                           == stringtype,
                        &private.band);

    /* allocate buffer header and array */
    {
        size_t bytes;

        if (!xpost_device_raster_bytes(width, private.band.bufrows,
                                       sizeof(Xpost_Png_Pixel),
                                       sizeof(Xpost_Png_Buffer), &bytes))
        {
            XPOST_LOG_ERR("%d a raster for a page of %dx%d is larger than"
                          " this platform addresses", limitcheck,
                          width, private.band.bufrows);
            return limitcheck;
        }
        private.buf = xpost_device_raster_block(bytes);
    }
    /* the size was one this platform expresses and addresses; whether
       the memory for it is there is the machine's answer, and a page the
       machine will not hold is a memory error rather than a limit of
       this interpreter */
    if (!private.buf)
    {
        XPOST_LOG_ERR("cannot allocate buffer memory");
        return VMerror;
    }
    private.buf->block = private.buf;
    private.bufowned = 1;

    /* the page starts opaque white; the alpha device starts fully
       transparent, so only marks made by the job carry opacity and an
       erased page is see-through */
    _clear(&private, 0, private.band.bufrows - 1);

    /* save private data struct in string */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
    {
        /* the record is the only thing that would have named the buffer,
           and it is not going to */
        free(private.buf);
        return unregistered;
    }

    /* return device instance dictionary to ps */
    xpost_stack_push(ctx->lo, ctx->os, devdic);
    return 0;
}

static
int _putpix(Xpost_Context *ctx,
            Xpost_Object red,
            Xpost_Object green,
            Xpost_Object blue,
            Xpost_Object x,
            Xpost_Object y,
            Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    int r, g, b, ix, iy, by;

    /* fold numbers per the driver contract */
    r = xpost_dev_num_to_byte(red);
    g = xpost_dev_num_to_byte(green);
    b = xpost_dev_num_to_byte(blue);
    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released raster takes no marks: the recorded dimensions outlive
       the buffer, so the bounds check below does not stand in for this */
    if (!private.buf)
        return 0;

    /* check bounds: the columns of the page, and the rows of it this
       device is holding -- a mark aimed at a row it is not holding is
       dropped where a mark off the page is dropped */
    by = xpost_dev_band_row(&private.band, iy);
    if ((ix < 0) || (ix >= private.width) || (by < 0))
        return 0;

    {
        Xpost_Png_Pixel pixel;
        pixel.blue = b;
        pixel.green = g;
        pixel.red = r;
        pixel.alpha = 255;
        private.buf->data[xpost_dev_raster_offset(ix, by, private.width)]
            = pixel;
    }

    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    return 0;
}

/* Read a pixel back in the device's stored channel scale, the same one
   PutPix writes. The class this device copies reads the base class's
   row array, which this device does not have, so the inherited method
   would answer undefined; a slot the class dictionary offers has to
   work. A pixel outside the raster reads as the page's ground, and so
   does every pixel of an instance whose buffer has been released, and
   every pixel of a row this device is not holding -- which is what the
   file carries there, so the read agrees with the page rather than
   refusing a pixel the page has. The alpha device clears its page
   through /Erase and so records no ground; what it reads is the white
   that erase leaves under the transparency, which is the answer for a
   device that has none recorded. */
static
int _getpix(Xpost_Context *ctx,
            Xpost_Object x,
            Xpost_Object y,
            Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    int ix, iy, by, r, g, b;

    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    by = private.buf ? xpost_dev_band_row(&private.band, iy) : -1;
    if (!private.buf ||
        (ix < 0) || (ix >= private.width) || (by < 0))
        xpost_device_ground_channels(ctx, devdic, &r, &g, &b);
    else
    {
        Xpost_Png_Pixel pixel = private.buf->data
            [xpost_dev_raster_offset(ix, by, private.width)];

        r = pixel.red; g = pixel.green; b = pixel.blue;
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(r));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(g));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(b));

    return 0;
}

/* A blend coverage as the fraction of full ink it is: 0 leaves the
   ground alone, 255 lays the colour down whole. The value is folded into
   that range because the source-over blend below only stays between its
   endpoints while the weight does: past 255 the composited opacity runs
   past full and wraps in the byte it is stored in, so a fully covered
   pixel comes out completely transparent. The generic rasteriser folds a
   coverage the same way. */
static int _coverage(Xpost_Object cov)
{
    int c = xpost_dev_num_to_int(cov);

    if (c < 0) return 0;
    if (c > 255) return 255;
    return c;
}

/* Blend a coverage-weighted pixel: each channel moves toward the colour
   by cov/255. The text operators use this for glyph edge pixels when the
   device renders anti-aliased text. */
static
int _blendpix(Xpost_Context *ctx,
              Xpost_Object red,
              Xpost_Object green,
              Xpost_Object blue,
              Xpost_Object cov,
              Xpost_Object x,
              Xpost_Object y,
              Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    int r, g, b, c, ix, iy, by;

    r = xpost_dev_num_to_byte(red);
    g = xpost_dev_num_to_byte(green);
    b = xpost_dev_num_to_byte(blue);
    c = _coverage(cov);
    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released raster takes no marks */
    if (!private.buf)
        return 0;

    by = xpost_dev_band_row(&private.band, iy);
    if ((ix < 0) || (ix >= private.width) || (by < 0))
        return 0;

    {
        Xpost_Png_Pixel *p = &private.buf->data
            [xpost_dev_raster_offset(ix, by, private.width)];
        int da = p->alpha;
        int oa = c + (da * (255 - c) + 127) / 255;

        if (oa == 0)
            return 0;
        /* source over: the ink contributes c, the ground its own
           opacity of what c leaves uncovered */
        p->red   = (unsigned char)((r * c + p->red   * da * (255 - c) / 255 + oa / 2) / oa);
        p->green = (unsigned char)((g * c + p->green * da * (255 - c) / 255 + oa / 2) / oa);
        p->blue  = (unsigned char)((b * c + p->blue  * da * (255 - c) / 255 + oa / 2) / oa);
        p->alpha = (unsigned char)oa;
    }

    return 0;
}

/* C fast-path for the base-class PS FillRect: fills the buffer directly
   rather than looping over PutPix per pixel. The only caller is erasepage
   (full-page clear), which dominates page-emission time when done in PS. */
static
int _fillrect(Xpost_Context *ctx,
              Xpost_Object red,
              Xpost_Object green,
              Xpost_Object blue,
              Xpost_Object x,
              Xpost_Object y,
              Xpost_Object w,
              Xpost_Object h,
              Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Xpost_Png_Pixel pixel;
    int ix, iy, x0, y0, x1, y1;

    /* fold numbers per the driver contract */
    pixel.red   = (unsigned char)xpost_dev_num_to_byte(red);
    pixel.green = (unsigned char)xpost_dev_num_to_byte(green);
    pixel.blue  = (unsigned char)xpost_dev_num_to_byte(blue);
    pixel.alpha = 255;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released raster takes no marks */
    if (!private.buf)
        return 0;

    /* the contract's rectangle: inclusive span, clipped to the device,
       and then to the rows this device is holding */
    xpost_dev_rect_normalize(xpost_object_number(x), xpost_object_number(y),
                             xpost_object_number(w), xpost_object_number(h),
                             &x0, &y0, &x1, &y1);
    if (!xpost_dev_rect_clip(&x0, &y0, &x1, &y1,
                             private.width, private.height))
        return 0;
    if (!xpost_dev_band_clip(&private.band, &y0, &y1))
        return 0;

    for (iy = y0; iy <= y1; iy++)
    {
        Xpost_Png_Pixel *row = private.buf->data
                             + xpost_dev_raster_offset(
                                   0, xpost_dev_band_row(&private.band, iy),
                                   private.width);
        for (ix = x0; ix <= x1; ix++)
            row[ix] = pixel;
    }

    return 0;
}

/* Give up what a page was being written through, whether it was
   finished or not. Everything the writer was given goes back here, so a
   page abandoned part way costs the same as one written to the end --
   and the page ends here too, there being no writer left to write any
   more of it with.

   The file is the one thing left: it is the page machinery that named
   it, and it is given back by whichever of this device's callers is the
   last to hold it -- the method that writes the page, the one that
   retires the device, or the reclaim of a device the run never retired
   (tests/check-page-output.sh). What says it is to be given back is the
   page ending, which is what this records. */
static void _stream_drop(void *state)
{
    PrivateData *p = state;

    p->band.open = 0;
    p->band.done = 1;
    if (p->png)
        png_destroy_write_struct(&p->png, p->info ? &p->info : NULL);
    p->png = NULL;
    p->info = NULL;
    free(p->ground);
    p->ground = NULL;
}

/* Give up everything the instance names: the writer a page was going
   out through, the raster where the device owns it, and the file the
   page was being written to. Called from the collector with the block
   the instance state is kept in, so it touches nothing in virtual
   memory.

   This is the last thing to touch a device the run never retires -- one
   a restore took back, or one nothing named by the time a collection
   came round. Such a device reaches no Destroy, so the file of a page it
   was part way through is given back here, through the same closer and
   for the same reason Destroy gives it back: nothing else is going to be
   called on it, and a file nobody closes is one the process holds until
   it ends.

   The page is ended and not finished. What the writer had already handed
   the file stands, and no ending is written after it, so what a job
   leaves behind is what it would have left had this never run: a
   collection decides which descriptors a process holds and never what a
   page came to. A device the run does retire has given all of this up
   already and leaves it nothing to do. */
static void _reclaim(void *block)
{
    PrivateData *p = block;

    _stream_drop(p);
    if (p->file)
    {
        xpost_device_page_close(p->file);
        p->file = NULL;
    }
    XPOST_DEV_BUFFER_RECLAIM(p->buf, p->bufowned);
}

/* Start the stream this page is written through: the file the page
   machinery settled the name of, the writer that fills it, and one row
   of the page's ground for the rows this device is not holding.

   The header goes out before the first row and names the page rather
   than whatever run of rows the call that opened the stream happened to
   be holding, so a page written a band at a time carries at its head
   the bytes a page written whole carries.

   The file is opened by the caller and is already this device's; what
   is made here is everything that writes through it. */
static int _stream_open(Xpost_Context *ctx, Xpost_Object devdic,
                        void *state)
{
    PrivateData *p = state;

    Xpost_Object compression_level_o;
    png_color_8 sig_bit;
    int compression_level;
    size_t bytes;
    int r, g, b;

    /* the ground is asked for in this device's own pixels, through the
       arithmetic the raster itself was sized by */
    if (!xpost_device_raster_bytes(p->width, 1, sizeof(Xpost_Png_Pixel),
                                   0, &bytes))
    {
        _stream_drop(p);
        return limitcheck;
    }
    p->ground = xpost_device_raster_block(bytes);
    if (!p->ground)
    {
        _stream_drop(p);
        return VMerror;
    }
    xpost_device_ground_channels(ctx, devdic, &r, &g, &b);
    {
        Xpost_Png_Pixel px;
        Xpost_Dev_Raster_Offset i, n;

        px.red = (unsigned char)r;
        px.green = (unsigned char)g;
        px.blue = (unsigned char)b;
        /* the alpha device's erased page is see-through, and a row it
           never held is a row it never marked */
        px.alpha = p->alpha ? 0 : 255;
        n = xpost_dev_raster_offset(0, 1, p->width);
        for (i = 0; i < n; i++)
            ((Xpost_Png_Pixel *)p->ground)[i] = px;
    }

    compression_level_o = xpost_dict_get(ctx, devdic,
                                         xpost_name_cons(ctx, "png_compression_level"));
    if (xpost_object_get_type(compression_level_o) == invalidtype)
        compression_level = 3;
    else
        compression_level = compression_level_o.int_.val;
    XPOST_LOG_INFO("PNG compression level: %d", compression_level);

    p->png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!p->png)
    {
        _stream_drop(p);
        return VMerror;
    }
    p->info = png_create_info_struct(p->png);
    if (!p->info)
    {
        _stream_drop(p);
        return VMerror;
    }

    /* libpng reports errors by longjmp, and it may only be aimed at a
       call still on the stack: every call into the library below sets
       its own landing, and each releases what the writer was given */
    if (setjmp(png_jmpbuf(p->png)))
    {
        _stream_drop(p);
        return ioerror;
    }

    png_init_io(p->png, p->file);
    png_set_IHDR(p->png, p->info,
                 p->width, p->height, 8,
                 p->alpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
                 p->interlaced,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    /* every sample fills its channel: the writer hands the library
       values that already occupy the whole of the depth declared above,
       so what the file says carries a value is the depth itself and the
       samples go out as they are */
    sig_bit.red = 8;
    sig_bit.green = 8;
    sig_bit.blue = 8;
    sig_bit.alpha = 8;
    png_set_sBIT(p->png, p->info, &sig_bit);

    png_set_compression_level(p->png, compression_level);
    /* the whole menu is the library's own default: asked for nothing,
       nothing is asked, and the bytes are the bytes it always wrote */
    if (p->filters)
        png_set_filter(p->png, PNG_FILTER_TYPE_BASE, p->filters);
    png_write_info(p->png, p->info);
    if (!p->alpha)
        /* rows carry a fourth byte per pixel; skip it when writing RGB */
        png_set_filler(p->png, 0, PNG_FILLER_AFTER);

    p->band.open = 1;
    return 0;
}

/* Lay the page's ground over the whole buffer.

   For a device holding every row of the page while its marks arrive a
   run at a time. Such a device writes its file from what the buffer
   holds, so a row no run ever reaches is written from the buffer as
   well -- and what the page carries there is the ground, not the white
   a fresh raster starts on. It is laid once per page, before the first
   run is painted, so that a run that is painted overwrites it.

   A device whose buffer is the size of a band needs none of this: a row
   it is not holding is written from the ground row its emission keeps,
   there being no buffer row to write it from. */
static void _prime(Xpost_Context *ctx, Xpost_Object devdic, PrivateData *p)
{
    Xpost_Png_Pixel px;
    Xpost_Dev_Raster_Offset i, n;
    int r, g, b;

    xpost_device_ground_channels(ctx, devdic, &r, &g, &b);
    px.red = (unsigned char)r;
    px.green = (unsigned char)g;
    px.blue = (unsigned char)b;
    px.alpha = p->alpha ? 0 : 255;
    n = xpost_dev_raster_offset(0, p->band.bufrows, p->width);
    for (i = 0; i < n; i++)
        p->buf->data[i] = px;
    p->band.primed = 1;
}

/* Begin a page: nothing of it written and no stream open for it. What
   the raster holds is not touched -- the marks of the page about to be
   written are already on it by the time anything here runs. */

/* The row of the page at @p y as the writer wants it: the buffer's,
   where this device is holding that row, and the ground where it is
   not. A writer is handed the same rows either way and does not have to
   know the difference. */
static png_bytep _page_row(PrivateData *p, int y)
{
    int by = xpost_dev_band_stored(&p->band, y);

    if (by < 0)
        return (png_bytep)p->ground;
    return (png_bytep)(p->buf->data
                       + xpost_dev_raster_offset(0, by, p->width));
}

/* Give the writer the page's rows from where the file has reached down
   to @p to, and remember where that leaves it.

   The rows go out in order and none of them is revisited. What a PNG
   row's filter is taken against is the row before it, and the writer
   holds that row itself for as long as it is alive -- so a band's first
   row is filtered against the last row of the band before it without
   this device keeping either, which is what makes the seam between two
   bands the same bytes as the middle of a page written whole.

   An interlaced image is the exception and is written by the caller
   that has every row: the passes go over the page again and again, so
   nothing can be given up until the page is done. */
static int _write_rows(void *state, int to)
{
    PrivateData *p = state;

    if (setjmp(png_jmpbuf(p->png)))
    {
        _stream_drop(p);
        return ioerror;
    }

#ifdef PNG_WRITE_INTERLACING_SUPPORTED
    if (p->interlaced != PNG_INTERLACE_NONE)
    {
        int passes = png_set_interlace_handling(p->png);
        int pass, y;

        for (pass = 0; pass < passes; pass++)
            for (y = 0; y < p->height; y++)
            {
                png_bytep row = _page_row(p, y);

                png_write_rows(p->png, &row, 1);
            }
        p->band.next = p->height;
        return 0;
    }
#endif

    while (p->band.next <= to)
    {
        png_bytep row = _page_row(p, p->band.next);

        png_write_rows(p->png, &row, 1);
        p->band.next++;
    }
    return 0;
}

/* Finish the image and close the file behind it. A page finished stays
   finished: what says so is recorded before this returns, so a further
   call writes nothing rather than a second page into the same file. */
static int _stream_finish(void *state)
{
    PrivateData *p = state;

    if (setjmp(png_jmpbuf(p->png)))
    {
        _stream_drop(p);
        return ioerror;
    }
    png_write_end(p->png, p->info);
    _stream_drop(p);
    return 0;
}

/* Write the page, or as much of it as this device is holding: the
   whole of what a page arriving in bands means is written once, in
   xpost_dev_page_emit (xpost_dev_generic.c), and this says what
   this device does inside it. */
/* what this device does that the shared page writer does not */
/* An interlaced image is written in seven passes over the page, so no
   row of it can be given up before the last band has been painted: such
   a device holds every row (Create) and writes them all at the call that
   finds nothing held. */
static int _codec_defers_rows(const void *p)
{
    return ((const PrivateData *)p)->interlaced != PNG_INTERLACE_NONE;
}

/* How this device answers for the shape of its own instance, so that
   the emitting itself is written once (xpost_dev_page_emit_call). */
XPOST_DEV_PAGE_ACCESSORS(PrivateData)

static const Xpost_Dev_Page_Codec _codec = {
    _page_begin,
    _stream_drop,
    _stream_open,
    _write_rows,
    _stream_finish,
    _codec_defers_rows,
    _reclaim,
    _raster_of,
    _band_of,
    _file_of,
    _height_of,
    _disown,
    _prime_of,
    _clear_of
};

static
int _emit(Xpost_Context *ctx,
          Xpost_Object devdic)
{
    PrivateData private;

    return xpost_dev_page_emit_call(ctx, devdic, namePrivate,
                                    &private, sizeof(private), &_codec);
}

/* Move the raster onto another run of the page's rows.

   The rows are the ones Create made: one raster the size of a band,
   standing for one run of the page after another, which is what bounds
   what a page costs to put out. A raster per band would be the shorter
   way to say this and it bounds nothing, since nothing gives the band
   before's memory back.

   The run given up is not written anywhere here. Where the file has
   reached is the emission's business and it has already had these rows;
   what this does is say which rows the raster stands for next, and
   leave them as a raster fresh from Create would be -- a row still
   carrying the run before's ink shows it wherever this run paints
   nothing. A device holding every row of the page moves nothing and
   clears the run it is about to take marks for. */
static
int _moveband(Xpost_Context *ctx,
              Xpost_Object top,
              Xpost_Object rows,
              Xpost_Object devdic)
{
    PrivateData private;

    return xpost_dev_page_moveband_call(ctx, devdic, namePrivate,
                                        &private, sizeof(private),
                                        top, rows, &_codec);
}

/* -  .rowcost  elements bytes
   What one row of this device's raster costs, at the width the caller
   states on the dictionary stack. A pixel here is red, green, blue and
   alpha, one byte each, in a buffer of this device's own -- so a row is
   four bytes a pixel and no elements of the memory the interpreter
   allocates rows out of. The same sizeof the buffer is measured with,
   so the price and the allocation cannot drift apart. */
static
int _rowcost(Xpost_Context *ctx)
{
    return xpost_dev_rowcost(ctx, (int)sizeof(Xpost_Png_Pixel));
}

/* clear the page to fully transparent: the alpha device's erasepage.
   An explicit white fill stays opaque; only the page reset is clear.
   PLRM 8.2 erases the entire page, and what a device holding part of
   one can do about the rest is show the ground over it, which is what
   its emission writes there: so what is cleared here is the rows this
   device is holding. */
static
int _erase(Xpost_Context *ctx,
           Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released raster has no page to clear */
    if (!private.buf)
        return 0;

    _clear(&private, private.band.top - private.band.origin,
           private.band.top - private.band.origin + private.band.rows - 1);

    return 0;
}

static
int _destroy(Xpost_Context *ctx,
             Xpost_Object devdic)
{
    PrivateData private;

    return xpost_dev_page_destroy_call(ctx, devdic, namePrivate,
                                       &private, sizeof(private), &_codec);
}

/* operator function to instantiate a new window device.
   installed in the private dictionary by calling 'loadXXXdevice'.
 */
static
int newpngdevice(Xpost_Context *ctx,
                 Xpost_Object width,
                 Xpost_Object height)
{
    Xpost_Object classdic;
    int ret;

    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PNGDEVICE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_dict_get(ctx, classdic, xpost_name_cons(ctx, "Create"))))
        return execstackoverflow;

    return 0;
}

static
int newpngalphadevice(Xpost_Context *ctx,
                      Xpost_Object width,
                      Xpost_Object height)
{
    Xpost_Object classdic;
    int ret;

    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PNGALPHADEVICE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_dict_get(ctx, classdic, xpost_name_cons(ctx, "Create"))))
        return execstackoverflow;

    return 0;
}

static
unsigned int _loadpngdevicecont_opcode;
static
unsigned int _loadpngalphadevicecont_opcode;

/* Specializes or sub-classes the PPMIMAGE device class.
   load PPMIMAGE
   load and call ps procedure .copydict which leaves copy on stack
   call loadpngdevicecont by continuation.
 */
static
int loadpngdevice(Xpost_Context *ctx)
{
    Xpost_Object classdic;
    int ret;

    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PPMIMAGE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_loadpngdevicecont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;

    return 0;
}

static
int loadpngalphadevice(Xpost_Context *ctx)
{
    Xpost_Object classdic;
    int ret;

    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PPMIMAGE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_loadpngalphadevicecont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;

    return 0;
}

/* replace procedures in the class with newly created special operators.
   defines the device class (pngDEVICE or pngalphaDEVICE) in the private dictionary.
   and the matching newXXXdevice operator. The alpha class carries
   /AlphaChannel for Create and an /Erase method for erasepage. */
static
int _loaddevicecont_common(Xpost_Context *ctx,
                           Xpost_Object classdic,
                           int alpha)
{
    /* this device's method suite; the arities follow from DeviceRGB */
    static const Xpost_Dev_Method methods[] =
    {
        { "Create",   "pngCreate",   (Xpost_Op_Func)_create,   XPOST_DEV_M_CREATE },
        { "PutPix",   "pngPutPix",   (Xpost_Op_Func)_putpix,   XPOST_DEV_M_PUTPIX },
        { "GetPix",   "pngGetPix",   (Xpost_Op_Func)_getpix,   XPOST_DEV_M_GETPIX },
        { "FillRect", "pngFillRect", (Xpost_Op_Func)_fillrect, XPOST_DEV_M_RECT   },
        { "BlendPix", "pngBlendPix", (Xpost_Op_Func)_blendpix, XPOST_DEV_M_BLEND  },
        { "Emit",     "pngEmit",     (Xpost_Op_Func)_emit,     XPOST_DEV_M_PAGE   },
        { "Destroy",  "pngDestroy",  (Xpost_Op_Func)_destroy,  XPOST_DEV_M_PAGE   },
        /* the raster is this device's own, so the run of rows it stands
           for moves within it and not within the base class's array of
           rows: the inherited .moveband reaches for rows this instance
           does not carry and would answer undefined */
        { ".moveband", "pngMoveBand", (Xpost_Op_Func)_moveband, XPOST_DEV_M_BAND }
    };
    /* the alpha device clears to transparent rather than to white, so
       it answers erasepage itself */
    static const Xpost_Dev_Method alphamethods[] =
    {
        { "Erase", "pngErase", (Xpost_Op_Func)_erase, XPOST_DEV_M_PAGE }
    };

    Xpost_Object op;
    int ret;

    ret = xpost_dict_put(ctx, classdic, namenativecolorspace, nameDeviceRGB);
    if (ret)
        return ret;

    /* Whether this device's page may arrive a band at a time. Said here
       rather than inherited: the class is a copy of the colour raster
       class, which says yes, and a copy carries what it was copied from,
       so a device that had never considered the question would say yes
       by inheritance. Saying it makes the answer this device's own
       (doc/xpost_design.dox), and the two classes made here answer
       differently.

       The plain device's page may. Its writer takes one row per call and
       holds between calls what it needs of the row before, so a row goes
       out the moment it is finished and nothing written has to be
       revisited.

       The alpha device's may not, and what stands in the way is its
       erased page rather than its writer. A page arrives a band at a
       time by being recorded and played into the device a band at a
       time, and a record stands in front of the device while the page is
       drawn. erasepage runs a device's own Erase where it has one and
       otherwise paints the page as a rectangle in the colour it is being
       cleared to (data/paint.ps); this is the one device with an Erase,
       because the page it clears to is transparent and transparency is
       not a colour a rectangle can be painted in. A record declares no
       Erase and is held to declaring none, an instruction it took being
       an instruction it has no entry for (rule 14,
       tests/check-device-skeleton.sh) -- so on that route the reset is
       written down as an ordinary full-page mark and played back as an
       opaque fill, and every band the replay reaches comes out opaque,
       which is the whole of what this device is for.

       What would make it band is a record that wrote the reset down as a
       reset: an entry of its own for it, and a replay that ran the
       target's Erase. Until there is one the roster a page is routed
       through (.playtargets, data/recorddev.ps) names the plain device
       and not this one, and tests/check-device-roster.sh holds the
       roster and this declaration together.

       A scan-conversion window taken from an ImagingBBox is a separate
       mechanism reaching no record and no band loop, and this device
       takes one. */
    if (alpha)
        ret = xpost_dict_undef(ctx, classdic, xpost_name_cons(ctx, "BandedPage"));
    else
        ret = xpost_dict_put(ctx, classdic, xpost_name_cons(ctx, "BandedPage"),
                             xpost_bool_cons(1));
    if (ret)
        return ret;

    /* What one row of this device's raster costs, which is what the
       budget a band is priced against is divided by. Both halves differ
       from the three-byte planar row of the colour raster class this one
       is a copy of, and a copy carries what it was copied from, so this
       says it rather than inheriting it. */
    ret = xpost_dev_class_rowcost(ctx, classdic, "pngRowCost",
                                  (Xpost_Op_Func)_rowcost);
    if (ret)
        return ret;

    /* the defaults asked of this driver before its classes were built
       -- by an embedder (xpost_dev_png_options_set) or the command
       line's -p switch -- taken up for every knob this driver states,
       so that every instance copied from this class carries them; a
       page-device request naming the same key still overrides, at the
       copy the instance is made by */
    {
        const Xpost_Dev_Option *opts;
        int i, n;

        opts = xpost_dev_png_option_roster(&n);
        for (i = 0; i < n; i++)
        {
            ret = xpost_dev_class_option_default(ctx, classdic, opts[i].key);
            if (ret)
                return ret;
        }
    }

    op = xpost_operator_cons(ctx, "pngCreateCont", (Xpost_Op_Func)_create_cont, 3, integertype, integertype, dicttype);
    _create_cont_opcode = op.mark_.padw;

    ret = xpost_dev_class_install(ctx, classdic, 3, 1,
                                  methods, XPOST_DEV_METHOD_COUNT(methods));
    if (ret)
        return ret;

    if (alpha)
    {
        ret = xpost_dict_put(ctx, classdic, xpost_name_cons(ctx, "AlphaChannel"), xpost_bool_cons(1));
        if (ret)
            return ret;
        ret = xpost_dev_class_install(ctx, classdic, 3, 1, alphamethods,
                                      XPOST_DEV_METHOD_COUNT(alphamethods));
        if (ret)
            return ret;
    }

    /* The class and its maker live in the private dictionary, beside the
       classes the boot files define: a program reaches a device through
       the page-device request, the machinery reaches the class by name
       here, and a record asked to be played into this device is
       specialised from the class it finds here. Nothing of the driver's
       is defined where a program could shadow it. */
    ret = xpost_dev_class_publish(ctx, alpha ? ".xpost_PNGALPHADEVICE"
                                             : ".xpost_PNGDEVICE", classdic);
    if (ret)
        return ret;

    if (alpha)
        op = xpost_operator_cons(ctx, "newpngalphadevice", (Xpost_Op_Func)newpngalphadevice, 2, integertype, integertype);
    else
        op = xpost_operator_cons(ctx, "newpngdevice", (Xpost_Op_Func)newpngdevice, 2, integertype, integertype);
    ret = xpost_dict_put_internal(ctx, ctx->privatedict,
                         xpost_name_cons(ctx, alpha ? "newpngalphadevice" : "newpngdevice"),
                         op);
    if (ret)
        return ret;

    return 0;
}

static
int loadpngdevicecont(Xpost_Context *ctx,
                      Xpost_Object classdic)
{
    return _loaddevicecont_common(ctx, classdic, 0);
}

static
int loadpngalphadevicecont(Xpost_Context *ctx,
                           Xpost_Object classdic)
{
    return _loaddevicecont_common(ctx, classdic, 1);
}

/*
   install the loadXXXdevice which may be called during graphics initialization
   to produce the operator newXXXdevice which instantiates the device dictionary.
*/
int xpost_oper_init_png_device_ops(Xpost_Context *ctx,
                                   Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    /* factor-out name lookups from the operators (optimization) */
    if (xpost_object_get_type((namePrivate = xpost_name_cons(ctx, "Private"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namewidth = xpost_name_cons(ctx, "width"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameheight = xpost_name_cons(ctx, "height"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotcopydict = xpost_name_cons(ctx, ".copydict"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namenativecolorspace = xpost_name_cons(ctx, "nativecolorspace"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameDeviceRGB = xpost_name_cons(ctx, "DeviceRGB"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotbandpage = xpost_name_cons(ctx, ".bandpage"))) == invalidtype)
        return VMerror;

    optab = xpost_operator_table(ctx->gl);
    op = xpost_operator_cons(ctx, "loadpngdevice", (Xpost_Op_Func)loadpngdevice, 0); INSTALL;
    op = xpost_operator_cons(ctx, "loadpngdevicecont", (Xpost_Op_Func)loadpngdevicecont, 1, dicttype);
    _loadpngdevicecont_opcode = op.mark_.padw;
    op = xpost_operator_cons(ctx, "loadpngalphadevice", (Xpost_Op_Func)loadpngalphadevice, 0); INSTALL;
    op = xpost_operator_cons(ctx, "loadpngalphadevicecont", (Xpost_Op_Func)loadpngalphadevicecont, 1, dicttype);
    _loadpngalphadevicecont_opcode = op.mark_.padw;

    return 0;
}

XPAPI void
xpost_dev_png_options_set(Xpost_Context *ctx,
                          int compression_level,
                          int interlaced)
{
    if ((compression_level < 0) || (compression_level > 9))
    {
        XPOST_LOG_ERR("wrong compression level for the PNG device (%d)",
                      compression_level);
        return;
    }

    /* defaults for the two classes this driver body makes: recorded
       for the classes to take up as they are installed, and written
       onto them where they already are (xpost_dev_option_default);
       a program's own page-device request still overrides them */
    if (xpost_dev_option_default(ctx, "png_compression_level",
                                 xpost_int_cons(compression_level),
                                 ".xpost_PNGDEVICE",
                                 ".xpost_PNGALPHADEVICE")
     || xpost_dev_option_default(ctx, "png_interlaced",
                                 xpost_int_cons(interlaced ? 1 : 0),
                                 ".xpost_PNGDEVICE",
                                 ".xpost_PNGALPHADEVICE"))
        XPOST_LOG_ERR("the PNG device options could not be recorded");
}

/* The knobs this driver reads off its device dictionary, stated beside
   the reads that give them meaning and held to those reads by
   tests/check-device-facts.sh. Both of this body's classes carry every
   one of them. */
const Xpost_Dev_Option *xpost_dev_png_option_roster(int *count)
{
    static const char *const filterwords[] =
        { "adaptive", "none-sub-up", "none", NULL };
    static const Xpost_Dev_Option options[] =
    {
        { "png_compression_level", ".xpost_PNGDEVICE",
          ".xpost_PNGALPHADEVICE", 0, 9, NULL },
        { "png_interlaced", ".xpost_PNGDEVICE",
          ".xpost_PNGALPHADEVICE", 0, 1, NULL },
        { "png_filter", ".xpost_PNGDEVICE",
          ".xpost_PNGALPHADEVICE", 0, 0, filterwords }
    };

    *count = (int)(sizeof(options) / sizeof(options[0]));
    return options;
}

#else /* ! HAVE_LIBPNG */

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_context.h"
#include "xpost_dev_generic.h" /* the option roster type */

XPAPI void
xpost_dev_png_options_set(Xpost_Context *ctx,
                          int compression_level,
                          int interlaced)
{
    (void)ctx;
    (void)compression_level;
    (void)interlaced;
}

/* a build without the library has no PNG devices and no knobs */
const Xpost_Dev_Option *xpost_dev_png_option_roster(int *count)
{
    *count = 0;
    return NULL;
}

#endif
