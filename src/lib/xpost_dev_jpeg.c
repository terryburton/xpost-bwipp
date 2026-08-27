/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_jpeg.c
 * @brief The JPEG output device.
 *
 * As the PNG device: the page is rastered, then encoded once at the end.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifdef HAVE_STDLIB_H
# undef HAVE_STDLIB_H
#endif

#ifdef HAVE_LIBJPEG

#include <stddef.h> /* offsetof */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <jpeglib.h>
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
#include "xpost_dev_jpeg.h" /* check prototypes */

typedef struct _JPEG_error_mgr *emptr;
struct _JPEG_error_mgr
{
   struct jpeg_error_mgr pub;
   jmp_buf setjmp_buffer;
};

typedef struct
{
    unsigned char red, green, blue;
} Xpost_Jpeg_Pixel;

typedef struct
{
    int width, height, byte_stride;
    /* the block this raster is part of. A client is handed the raster
       and gives the block back, so the block's own address is kept
       here, immediately before the raster, where the release entry
       point reads it. */
    void *block;
    Xpost_Jpeg_Pixel data[1];
} Xpost_Jpeg_Buffer;

XPOST_DEV_ASSERT_BLOCK_PRECEDES_RASTER(jpeg, Xpost_Jpeg_Buffer, block, data);

/* The compressor a page is written through, and the error manager it
   reports a fatal condition through. The two are kept together because
   the compressor holds the manager's address and the manager holds the
   landing a fatal condition returns to, so neither may move while a
   page is being written. */
typedef struct
{
    struct jpeg_compress_struct cinfo;
    struct _JPEG_error_mgr jerr;
} Xpost_Jpeg_Writer;

/* A JPEG stream holds exactly one image, so the file a page is
   compressed into belongs to the page and not to the device: it is
   opened as a page begins and closed as it ends. What the instance
   keeps between pages is the raster.

   A page arriving a band at a time is compressed across several Emit
   calls (doc/xpost_design.dox), and the file and the compressor outlive each
   of them -- which is the whole of what makes a band of any height
   right. A scanline goes into a unit of eight or sixteen rows, and the
   compressor holds the part of a unit it has not filled between one
   call and the next for as long as it is alive, so the device keeps the
   compressor rather than choosing a band height to suit it. */
typedef struct
{
    int width;
    int height;
    /*
     * add additional members to private struct
     */
    Xpost_Jpeg_Buffer *buf;
    /* the run of the page's rows the raster stands for, and how far
       down the page the file has been written */
    Xpost_Dev_Band band;
    FILE *file;
    /* The compressor, held by its address rather than in this struct.
       What is written here is copied in and out of a string the
       interpreter holds (xpost_dev_private_get/put), so a member of it
       has no address that lasts from one call to the next -- and a
       compressor is reached by address: its error manager is what a
       fatal condition longjmps through, and the library is given the
       one address for the length of a page. */
    Xpost_Jpeg_Writer *out;
    /* one row of the page's ground, for the rows this device is not
       holding: compressed where a device holding the whole page would
       have compressed what erasepage left there */
    unsigned char *ground;
    /* the device allocated buf and has not handed it to the client
       through OutputBufferOut, so Destroy frees it */
    int bufowned;
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

static void
_JPEGFatalErrorHandler(j_common_ptr cinfo)
{
   emptr errmgr;

   errmgr = (emptr) cinfo->err;
   longjmp(errmgr->setjmp_buffer, 1);
   return;
}

/* The library's message emitters, replaced so that it writes nothing to
   the process's error stream: what it has to say about an image arrives
   through the fatal handler above, which longjmps back to the caller. */
static void
_JPEGErrorHandler(j_common_ptr cinfo)
{
   (void)cinfo;
}

static void
_JPEGErrorHandler2(j_common_ptr cinfo, int msg_level)
{
   (void)cinfo;
   (void)msg_level;
}

/* Leave a run of the buffer's own rows as a raster fresh from Create:
   white, this format carrying no transparency.

   Create lays the whole buffer down this way, and so does every move to
   another run of the page's rows -- a raster standing for one run after
   another has to start each run as a fresh raster would, or what the
   run before painted shows through wherever this one paints nothing.
   The rows are the buffer's, not the page's, since what moves is which
   of the page's rows they stand for. */
static void _clear(PrivateData *p, int from, int to)
{
    Xpost_Jpeg_Pixel init;
    Xpost_Dev_Raster_Offset i, n;

    if (!xpost_dev_band_clamp_rows(&p->band, &from, &to))
        return;

    init.red = init.green = init.blue = 255;
    i = xpost_dev_raster_offset(0, from, p->width);
    n = xpost_dev_raster_offset(0, to + 1, p->width);
    for (; i < n; i++)
        p->buf->data[i] = init;
}

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
    private.out = NULL;
    private.ground = NULL;

    /*
     *
     * initialize additional members of private struct
     *
     */

    /* The run of the page's rows this device is to hold. A baseline
       JPEG is written in one pass, so a row compressed is a row given
       up and the device need hold no more of the page than the run it
       is working on -- except where the raster is one an embedder asked
       for, which is the whole page by the contract it asked under
       (XPOST_OUTPUT_BUFFEROUT, xpost.h). The run still says which rows
       take marks either way. */
    xpost_dev_band_take(ctx, devdic, height,
                        xpost_object_get_type(
                            xpost_context_host_setting(ctx, "OutputBufferOut"))
                        == stringtype,
                        &private.band);

    /* allocate buffer header and array */
    {
        size_t bytes;

        if (!xpost_device_raster_bytes(width, private.band.bufrows,
                                       sizeof(Xpost_Jpeg_Pixel),
                                       sizeof(Xpost_Jpeg_Buffer), &bytes))
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

    /* the page starts white; this format carries no transparency, so a
       pixel the job never marks is written out as it stands here */
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

/* Blend a coverage-weighted pixel: each channel moves toward the colour
   by cov/255. The text operators use this for the partly covered pixels
   at a glyph's edges, and a device without it inherits the base class's,
   which blends into a raster held as PostScript arrays -- this device
   keeps its pixels in a buffer of its own instead, so it needs its own.
*/
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
    c = xpost_dev_num_to_int(cov);
    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released raster takes no marks: the recorded dimensions outlive
       the buffer, so the bounds check below does not stand in for this */
    if (!private.buf)
        return 0;

    by = xpost_dev_band_row(&private.band, iy);
    if ((ix < 0) || (ix >= private.width) || (by < 0))
        return 0;

    if (c <= 0)
        return 0;
    if (c > 255)
        c = 255;

    {
        Xpost_Jpeg_Pixel *p = &private.buf->data
            [xpost_dev_raster_offset(ix, by, private.width)];

        p->red = (unsigned char)xpost_dev_blend_channel(p->red, r, c);
        p->green = (unsigned char)xpost_dev_blend_channel(p->green, g, c);
        p->blue = (unsigned char)xpost_dev_blend_channel(p->blue, b, c);
    }

    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

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

    /* a released raster takes no marks */
    if (!private.buf)
        return 0;

    /* check bounds: the columns of the page, and the rows of it this
       device is holding -- a mark aimed at a row it is not holding is
       dropped where a mark off the page is dropped */
    by = xpost_dev_band_row(&private.band, iy);
    if ((ix < 0) || (ix >= private.width) || (by < 0))
        return 0;

    {
        Xpost_Jpeg_Pixel pixel;
        pixel.blue = b;
        pixel.green = g;
        pixel.red = r;
        private.buf->data[xpost_dev_raster_offset(ix, by, private.width)]
            = pixel;
    }

    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    return 0;
}

/* Fill the buffer directly rather than through a loop over PutPix.

   A device that does not offer this takes the base class's, which walks
   the rectangle a pixel at a time and reaches the buffer through the
   operator dispatch for each of them. Every page begins with an
   erasepage over the whole of it, so that walk costs the page's own area
   in dispatches before a program has drawn anything.

   The rectangle is the contract's: an inclusive span, normalised and
   clipped to the device by the shared helpers, so a rectangle given
   inside out or reaching past an edge covers what the other devices
   cover. */
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
    Xpost_Jpeg_Pixel pixel;
    int ix, iy, x0, y0, x1, y1;

    pixel.red   = (unsigned char)xpost_dev_num_to_byte(red);
    pixel.green = (unsigned char)xpost_dev_num_to_byte(green);
    pixel.blue  = (unsigned char)xpost_dev_num_to_byte(blue);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released raster takes no marks */
    if (!private.buf)
        return 0;

    xpost_dev_rect_normalize(xpost_object_number(x), xpost_object_number(y),
                             xpost_object_number(w), xpost_object_number(h),
                             &x0, &y0, &x1, &y1);
    if (!xpost_dev_rect_clip(&x0, &y0, &x1, &y1,
                             private.width, private.height))
        return 0;
    /* ... and then to the rows this device is holding */
    if (!xpost_dev_band_clip(&private.band, &y0, &y1))
        return 0;

    for (iy = y0; iy <= y1; iy++)
    {
        Xpost_Jpeg_Pixel *row = private.buf->data
                              + xpost_dev_raster_offset(
                                    0, xpost_dev_band_row(&private.band, iy),
                                    private.width);

        for (ix = x0; ix <= x1; ix++)
            row[ix] = pixel;
    }

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
   refusing a pixel the page has. */
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
        Xpost_Jpeg_Pixel pixel = private.buf->data
            [xpost_dev_raster_offset(ix, by, private.width)];

        r = pixel.red; g = pixel.green; b = pixel.blue;
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(r));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(g));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(b));

    return 0;
}

/* Give up what a page was being compressed through, whether it was
   finished or not. Everything the compressor was given goes back here,
   so a page abandoned part way costs the same as one written to the end
   -- and the page ends here too, there being no compressor left to
   write any more of it with.

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
    if (p->out)
    {
        jpeg_destroy_compress(&p->out->cinfo);
        free(p->out);
    }
    p->out = NULL;
    free(p->ground);
    p->ground = NULL;
}

/* Give up everything the instance names: the compressor a page was going
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

   The page is ended and not finished. What the compressor had already
   handed the file stands, and no ending is written after it, so what a
   job leaves behind is what it would have left had this never run: a
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

/* Start the stream this page is compressed through: the file the page
   machinery settled the name of, the compressor that fills it, and one
   row of the page's ground for the rows this device is not holding.

   The compressor is told the page's extent and not the run of rows the
   call that opened the stream happens to be holding, so a page written
   a band at a time carries at its head what a page written whole
   carries.

   The file is opened by the caller and is already this device's; what
   is made here is everything that writes through it. */
static int _stream_open(Xpost_Context *ctx, Xpost_Object devdic,
                        void *state)
{
    PrivateData *p = state;

    Xpost_Object quality_o;
    size_t bytes;
    int quality;
    int r, g, b;

    /* the ground is asked for in this device's own pixels, through the
       arithmetic the raster itself was sized by */
    if (!xpost_device_raster_bytes(p->width, 1, sizeof(Xpost_Jpeg_Pixel),
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
        Xpost_Jpeg_Pixel px;
        Xpost_Dev_Raster_Offset i, n;

        px.red = (unsigned char)r;
        px.green = (unsigned char)g;
        px.blue = (unsigned char)b;
        n = xpost_dev_raster_offset(0, 1, p->width);
        for (i = 0; i < n; i++)
            ((Xpost_Jpeg_Pixel *)p->ground)[i] = px;
    }

    p->out = calloc(1, sizeof(*p->out));
    if (!p->out)
    {
        _stream_drop(p);
        return VMerror;
    }

    quality_o = xpost_dict_get(ctx, devdic, xpost_name_cons(ctx, "jpeg_quality"));

    if (xpost_object_get_type(quality_o) == invalidtype)
        quality = 90;
    else
        quality = quality_o.int_.val;
    XPOST_LOG_INFO("JPEG quality: %d", quality);

    p->out->cinfo.err = jpeg_std_error(&(p->out->jerr.pub));
    p->out->jerr.pub.error_exit = _JPEGFatalErrorHandler;
    p->out->jerr.pub.emit_message = _JPEGErrorHandler2;
    p->out->jerr.pub.output_message = _JPEGErrorHandler;
    /* the library reports a fatal condition by longjmp, and it may only
       be aimed at a call still on the stack: every call into it below
       sets its own landing, and each releases what the compressor was
       given */
    if (setjmp(p->out->jerr.setjmp_buffer))
    {
        _stream_drop(p);
        return undefined;
    }
    jpeg_create_compress(&p->out->cinfo);
    jpeg_stdio_dest(&p->out->cinfo, p->file);
    p->out->cinfo.image_width = p->width;
    p->out->cinfo.image_height = p->height;
    p->out->cinfo.input_components = 3;
    p->out->cinfo.in_color_space = JCS_RGB;
    /* One pass over the page: the coder that chooses its own Huffman
       tables reads every block before it writes any, which is the page
       held in the compressor whatever this device holds. */
    p->out->cinfo.optimize_coding = FALSE;
    p->out->cinfo.dct_method = JDCT_ISLOW; /* JDCT_FLOAT JDCT_IFAST(quality loss) */
    if (quality < 60)
        p->out->cinfo.dct_method = JDCT_IFAST;
    jpeg_set_defaults(&p->out->cinfo);
    jpeg_set_quality(&p->out->cinfo, quality, TRUE);
    if (quality >= 90)
    {
        p->out->cinfo.comp_info[0].h_samp_factor = 1;
        p->out->cinfo.comp_info[0].v_samp_factor = 1;
        p->out->cinfo.comp_info[1].h_samp_factor = 1;
        p->out->cinfo.comp_info[1].v_samp_factor = 1;
        p->out->cinfo.comp_info[2].h_samp_factor = 1;
        p->out->cinfo.comp_info[2].v_samp_factor = 1;
    }
    jpeg_start_compress(&p->out->cinfo, TRUE);

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
    Xpost_Jpeg_Pixel px;
    Xpost_Dev_Raster_Offset i, n;
    int r, g, b;

    xpost_device_ground_channels(ctx, devdic, &r, &g, &b);
    px.red = (unsigned char)r;
    px.green = (unsigned char)g;
    px.blue = (unsigned char)b;
    n = xpost_dev_raster_offset(0, p->band.bufrows, p->width);
    for (i = 0; i < n; i++)
        p->buf->data[i] = px;
    p->band.primed = 1;
}

/* Begin a page: nothing of it written and no stream open for it. What
   the raster holds is not touched -- the marks of the page about to be
   written are already on it by the time anything here runs. */

/* The row of the page at @p y as the compressor wants it: the buffer's,
   where this device is holding that row, and the ground where it is
   not. The compressor is handed the same rows either way and does not
   have to know the difference. */
static JSAMPROW _page_row(PrivateData *p, int y)
{
    int by = xpost_dev_band_stored(&p->band, y);

    if (by < 0)
        return (JSAMPROW)p->ground;
    return (JSAMPROW)(p->buf->data
                      + xpost_dev_raster_offset(0, by, p->width));
}

/* Give the compressor the page's rows from where the file has reached
   down to @p to, and remember where that leaves it.

   A scanline goes into a unit of eight or sixteen rows, so a run of
   rows that is not a whole number of those leaves the compressor
   holding the part it cannot code yet. It holds that part itself, for
   as long as it is alive, and this device keeps it alive for the length
   of the page -- so a band may be any height at all, one row included,
   and the file is the file a page handed over whole produces. Choosing
   a band height to suit the unit would be the other answer and it is
   not needed here. */
static int _write_rows(void *state, int to)
{
    PrivateData *p = state;

    if (setjmp(p->out->jerr.setjmp_buffer))
    {
        _stream_drop(p);
        return undefined;
    }

    while (p->band.next <= to
           && p->out->cinfo.next_scanline < p->out->cinfo.image_height)
    {
        JSAMPROW row = _page_row(p, p->band.next);

        jpeg_write_scanlines(&p->out->cinfo, &row, 1);
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

    if (setjmp(p->out->jerr.setjmp_buffer))
    {
        _stream_drop(p);
        return undefined;
    }
    jpeg_finish_compress(&p->out->cinfo);
    _stream_drop(p);
    return 0;
}

/* Write the page, or as much of it as this device is holding: the
   whole of what a page arriving in bands means is written once, in
   xpost_dev_page_emit (xpost_dev_generic.c), and this says what
   this device does inside it. */
/* what this device does that the shared page writer does not */
/* How this device answers for the shape of its own instance, so that
   the emitting itself is written once (xpost_dev_page_emit_call). */
XPOST_DEV_PAGE_ACCESSORS(PrivateData)

static const Xpost_Dev_Page_Codec _codec = {
    _page_begin,
    _stream_drop,
    _stream_open,
    _write_rows,
    _stream_finish,
    NULL,  /* every row this device is given, it can compress at once */
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
   states on the dictionary stack. A pixel here is red, green and blue,
   one byte each, interleaved in a buffer of this device's own -- so a
   row is three bytes a pixel and no elements of the memory the
   interpreter allocates rows out of. The same sizeof the buffer is
   measured with, so the price and the allocation cannot drift apart. */
static
int _rowcost(Xpost_Context *ctx)
{
    return xpost_dev_rowcost(ctx, (int)sizeof(Xpost_Jpeg_Pixel));
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
int newjpegdevice(Xpost_Context *ctx,
                  Xpost_Object width,
                  Xpost_Object height)
{
    Xpost_Object classdic;
    int ret;

    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_JPEGDEVICE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_dict_get(ctx, classdic, xpost_name_cons(ctx, "Create"))))
        return execstackoverflow;

    return 0;
}

static
unsigned int _loadjpegdevicecont_opcode;

/* Specializes or sub-classes the .xpost_PPMIMAGE device class.
   load .xpost_PPMIMAGE
   load and call ps procedure .copydict which leaves copy on stack
   call loadjpegdevicecont by continuation.
 */
static
int loadjpegdevice(Xpost_Context *ctx)
{
    Xpost_Object classdic;
    int ret;

    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PPMIMAGE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_loadjpegdevicecont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;

    return 0;
}

/* replace procedures in the class with newly created special operators.
   defines the device class jpegDEVICE in the private dictionary.
   defines its maker beside it: newjpegdevice
 */
static
int loadjpegdevicecont(Xpost_Context *ctx,
                      Xpost_Object classdic)
{
    /* this device's method suite; the arities follow from its
       declared colour space */
    static const Xpost_Dev_Method methods[] =
    {
        { "Create", "jpegCreate", (Xpost_Op_Func)_create, XPOST_DEV_M_CREATE },
        { "PutPix", "jpegPutPix", (Xpost_Op_Func)_putpix, XPOST_DEV_M_PUTPIX },
        { "FillRect", "jpegFillRect", (Xpost_Op_Func)_fillrect, XPOST_DEV_M_RECT },
        { "GetPix", "jpegGetPix", (Xpost_Op_Func)_getpix, XPOST_DEV_M_GETPIX },
        { "BlendPix", "jpegBlendPix", (Xpost_Op_Func)_blendpix, XPOST_DEV_M_BLEND },
        { "Emit", "jpegEmit", (Xpost_Op_Func)_emit, XPOST_DEV_M_PAGE },
        { "Destroy", "jpegDestroy", (Xpost_Op_Func)_destroy, XPOST_DEV_M_PAGE },
        /* the raster is this device's own, so the run of rows it stands
           for moves within it and not within the base class's array of
           rows: the inherited .moveband reaches for rows this instance
           does not carry and would answer undefined */
        { ".moveband", "jpegMoveBand", (Xpost_Op_Func)_moveband, XPOST_DEV_M_BAND }
    };

    Xpost_Object op;
    int ret;

    ret = xpost_dict_put(ctx, classdic, namenativecolorspace, nameDeviceRGB);
    if (ret)
        return ret;

    /* This device's page may arrive a band at a time: the compressor
       takes one scanline per call and holds between calls the part of a
       coding unit it cannot finish, so a row goes out the moment it is
       finished and nothing written has to be revisited.

       Said here rather than inherited. The class is a copy of the
       colour raster class, which says it, and a copy carries what it
       was copied from -- so a device that had never considered the
       question would say yes by inheritance. Saying it again is what
       makes the answer this device's own (doc/xpost_design.dox). */
    ret = xpost_dict_put(ctx, classdic, xpost_name_cons(ctx, "BandedPage"),
                         xpost_bool_cons(1));
    if (ret)
        return ret;

    /* What one row of this device's raster costs, which is what the
       budget a band is priced against is divided by. The bytes come to
       what the colour raster class this one is a copy of states and the
       elements do not, and a copy carries what it was copied from, so
       this says both rather than inheriting them. */
    ret = xpost_dev_class_rowcost(ctx, classdic, "jpegRowCost",
                                  (Xpost_Op_Func)_rowcost);
    if (ret)
        return ret;

    /* the defaults asked of this driver before its class was built --
       by an embedder (xpost_dev_jpeg_options_set) or the command
       line's -p switch -- taken up for every knob this driver states,
       so that every instance copied from this class carries them; a
       page-device request naming the same key still overrides, at the
       copy the instance is made by */
    {
        const Xpost_Dev_Option *opts;
        int i, n;

        opts = xpost_dev_jpeg_option_roster(&n);
        for (i = 0; i < n; i++)
        {
            ret = xpost_dev_class_option_default(ctx, classdic, opts[i].key);
            if (ret)
                return ret;
        }
    }

    op = xpost_operator_cons(ctx, "jpegCreateCont", (Xpost_Op_Func)_create_cont, 3, integertype, integertype, dicttype);
    _create_cont_opcode = op.mark_.padw;

    ret = xpost_dev_class_install(ctx, classdic, 3, 1,
                                  methods, XPOST_DEV_METHOD_COUNT(methods));
    if (ret)
        return ret;







    /* The class and its maker live in the private dictionary, beside the
       classes the boot files define: a program reaches a device through
       the page-device request, the machinery reaches the class by name
       here, and a record asked to be played into this device is
       specialised from the class it finds here. Nothing of the driver's
       is defined where a program could shadow it. */
    ret = xpost_dict_put_internal(ctx, ctx->privatedict,
                         xpost_name_cons(ctx, ".xpost_JPEGDEVICE"), classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "newjpegdevice", (Xpost_Op_Func)newjpegdevice, 2, integertype, integertype);
    ret = xpost_dict_put_internal(ctx, ctx->privatedict, xpost_name_cons(ctx, "newjpegdevice"), op);
    if (ret)
        return ret;

    return 0;
}

/*
   install the loadXXXdevice which may be called during graphics initialization
   to produce the operator newXXXdevice which instantiates the device dictionary.
*/
int xpost_oper_init_jpeg_device_ops(Xpost_Context *ctx,
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
    op = xpost_operator_cons(ctx, "loadjpegdevice", (Xpost_Op_Func)loadjpegdevice, 0); INSTALL;
    op = xpost_operator_cons(ctx, "loadjpegdevicecont", (Xpost_Op_Func)loadjpegdevicecont, 1, dicttype);
    _loadjpegdevicecont_opcode = op.mark_.padw;

    return 0;
}

XPAPI void
xpost_dev_jpeg_options_set(Xpost_Context *ctx, int quality)
{
    if ((quality < 0) || (quality > 100))
    {
        XPOST_LOG_ERR("wrong quality value for the JPEG device (%d)",
                      quality);
        return;
    }

    /* a default for the class this driver makes: recorded for the
       class to take up as it is installed, and written onto it where
       it already is (xpost_dev_option_default); a program's own
       page-device request still overrides it */
    if (xpost_dev_option_default(ctx, "jpeg_quality",
                                 xpost_int_cons(quality),
                                 ".xpost_JPEGDEVICE", NULL))
        XPOST_LOG_ERR("the JPEG device option could not be recorded");
}

/* The knob this driver reads off its device dictionary, stated beside
   the read that gives it meaning and held to it by
   tests/check-device-facts.sh. */
const Xpost_Dev_Option *xpost_dev_jpeg_option_roster(int *count)
{
    static const Xpost_Dev_Option options[] =
    {
        { "jpeg_quality", ".xpost_JPEGDEVICE", NULL, 0, 100, NULL }
    };

    *count = (int)(sizeof(options) / sizeof(options[0]));
    return options;
}

#else /* ! HAVE_LIBJPEG */

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_context.h"
#include "xpost_dev_generic.h" /* the option roster type */

XPAPI void
xpost_dev_jpeg_options_set(Xpost_Context *ctx, int quality)
{
    (void)ctx;
    (void)quality;
}

/* a build without the library has no JPEG device and no knob */
const Xpost_Dev_Option *xpost_dev_jpeg_option_roster(int *count)
{
    *count = 0;
    return NULL;
}

#endif
