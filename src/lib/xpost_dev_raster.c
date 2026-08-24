/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stddef.h> /* offsetof */
#include <stdlib.h> /* abs */
#include <string.h>

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
#include "xpost_dev_generic.h" /* the raster extent limit */
#include "xpost_dev_driver.h" /* device contract and shared helpers */
#include "xpost_dev_raster.h" /* check prototypes */

enum Xpost_PixelFormat { RGB, ARGB, BGR, BGRA };

/* The formats a "raster:FORMAT" selection may name, and the whole of
   what it may name: a selection carrying any other word is refused
   where a selection is read (setlocalconfig, src/lib/xpost_interpreter.c),
   the way an unrecognised mode is on every other device. It is refused
   rather than taken for one of these because the buffer this device
   lends is read by whoever embedded the interpreter, and the
   arrangement it is read by is the one the selection named: a page
   handed back in another arrangement is read as though it were in the
   one that was asked for.

   In the order of the formats above, so a name's place here is the
   format it names. */
const char *const xpost_raster_formats[] =
{
    "rgb",
    "argb",
    "bgr",
    "bgra",
    NULL
};

/* Say that the roster covers the formats rather than leave it to hold
   by luck: a format added to one and not the other would leave a name
   standing for the wrong arrangement, or none. (A negative array size
   rather than _Static_assert: this builds as C99 with -pedantic-errors,
   which rejects the latter.) */
typedef char xpost_raster_formats_name_every_format[
    sizeof xpost_raster_formats / sizeof *xpost_raster_formats
    == BGRA + 2 ? 1 : -1];

typedef struct
{
    unsigned char blue, green, red, alpha;
} Xpost_Raster_BGRA_Pixel;

typedef struct
{
    unsigned char blue, green, red;
} Xpost_Raster_BGR_Pixel;

typedef struct
{
    unsigned char red, green, blue;
} Xpost_Raster_RGB_Pixel;

typedef struct
{
    unsigned char alpha, red, green, blue;
} Xpost_Raster_ARGB_Pixel;

typedef struct
{
    int width, height, byte_stride;
    /* the block this raster is part of. A client is handed the raster
       and gives the block back, so the block's own address is kept
       here, immediately before the raster, where the release entry
       point reads it. Null for a raster the device did not allocate. */
    void *block;
    /*(Xpost_Raster_*_Pixel)*/ char *data[1];
} Xpost_Raster_Buffer;

XPOST_DEV_ASSERT_BLOCK_PRECEDES_RASTER(raster, Xpost_Raster_Buffer, block, data);

typedef struct
{
    int width, height;
    enum Xpost_PixelFormat pixelformat;
    /*
     * add additional members to private struct
     */
    Xpost_Raster_Buffer *buf;
    int bufowned; /* the device malloc'd buf and has not handed it to the
                     client through OutputBufferOut, so Destroy frees it */
} PrivateData;

/* Give up the raster the instance names, where the device owns it: a
   raster handed to the client is the client's to give back. Called from
   the collector with the block the instance state is kept in, so it
   touches nothing in virtual memory. A device the run retired has given
   the raster up already and leaves this nothing to do. */
static void _reclaim(void *block)
{
    PrivateData *p = block;

    XPOST_DEV_BUFFER_RECLAIM(p->buf, p->bufowned);
}


static Xpost_Object namePrivate;
static Xpost_Object namewidth;
static Xpost_Object nameheight;
static Xpost_Object namedotcopydict;
static Xpost_Object namenativecolorspace;
static Xpost_Object nameDeviceRGB;


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

/* initialize the C-level data
   and define in the device instance */
static
int _create_cont(Xpost_Context *ctx,
                 Xpost_Object w,
                 Xpost_Object h,
                 Xpost_Object devdic)
{
    Xpost_Object subdevice;
    Xpost_Object privatestr;
    PrivateData private;
    int width, height;
    Xpost_Object inbufstr;
    size_t bytes;
    int ret;

    /* The page the program asked for, as the extent of the buffer that
       will hold it. Every device here holds a whole page in one block,
       so the two carry the same numbers; a page naming an extent no
       buffer's row arithmetic carries is refused before anything is
       built for it. */
    if (!xpost_dev_page_extent(w.int_.val, h.int_.val, &width, &height))
        return limitcheck;

    /* The mode selector of the "device:mode" this run was started with,
       one of the settings the run made. It names a format only where
       this device is the one that selection named: the same setting
       carries the word that holds a page a band at a time, so a run
       started on a device whose page may arrive that way, whose program
       then asks for a raster page by name, has named no format at all
       and takes the format a run that names none takes. A word that is
       neither is refused where the selection is read (setlocalconfig,
       src/lib/xpost_interpreter.c), so what reaches here is one of the
       formats or a word addressed elsewhere. */
    subdevice = xpost_context_host_setting(ctx, "SUBDEVICE");
    if (xpost_object_get_type(subdevice) != stringtype)
    {
        subdevice = xpost_string_cons(ctx, sizeof("rgb") - 1, "rgb");
    }
    XPOST_LOG_INFO("</SUBDEVICE %*s>", subdevice.comp_.sz, xpost_string_get_pointer(ctx, subdevice));
    {
        /* The name is compared against its own length as well as the
           roster's: a shorter one read as though it were as long as
           what it stands beside would take whatever follows it in
           memory with it. The format is assigned before the walk as
           well as by it, so it is assigned whatever the walk finds. */
        const char *sub = xpost_string_get_pointer(ctx, subdevice);
        unsigned int sublen = subdevice.comp_.sz;
        int i;

        private.pixelformat = RGB;
        for (i = 0; xpost_raster_formats[i]; i++)
        {
            if ((sublen == strlen(xpost_raster_formats[i]))
                && (memcmp(sub, xpost_raster_formats[i], sublen) == 0))
            {
                private.pixelformat = (enum Xpost_PixelFormat)i;
                break;
            }
        }
    }

    /* The block this device's instance state lives in, and, named with
       it rather than after it, what gives up whatever that state names.
       What this device holds is a raster, which is not virtual memory:
       a device the run never retires -- one a restore took back, or one
       nothing named by the time a collection came round -- would take
       its raster with it. This is what gives it up there. */
    ret = xpost_handle_cons(ctx, devdic, namePrivate, &privatestr,
                            XPOST_HANDLE_DEVICE, sizeof(PrivateData),
                            _reclaim);
    if (ret)
        return ret;

    private.width = width;
    private.height = height;

    /*
     *
     * initialize additional members of private struct
     *
     */

    /* A pixel is reached by its position within the raster, so a buffer
       whose far end has no address on this platform cannot be indexed
       whatever memory would be given for it. Held before the buffer is
       considered, and for a buffer the caller supplied as much as for
       one of ours: the reach is the device's arithmetic either way. */
    {
        size_t pixel;

        switch(private.pixelformat)
        {
            default: return unregistered;
            case ARGB: pixel = sizeof(Xpost_Raster_ARGB_Pixel); break;
            case RGB:  pixel = sizeof(Xpost_Raster_RGB_Pixel);  break;
            case BGRA: pixel = sizeof(Xpost_Raster_BGRA_Pixel); break;
            case BGR:  pixel = sizeof(Xpost_Raster_BGR_Pixel);  break;
        }
        if (!xpost_device_raster_bytes(width, height, pixel,
                                       sizeof(Xpost_Raster_Buffer), &bytes))
        {
            XPOST_LOG_ERR("%d a raster for a page of %dx%d is larger than"
                          " this platform addresses", limitcheck,
                          width, height);
            return limitcheck;
        }
    }

    /* the framebuffer an embedding caller lent this run, if it lent one */
    inbufstr = xpost_context_host_setting(ctx, "OutputBufferIn");
    if (xpost_object_get_type(inbufstr) == stringtype)
    {
        unsigned char *inbuf;

        memcpy(&inbuf, xpost_string_get_pointer(ctx, inbufstr), sizeof(inbuf));
        private.buf = (Xpost_Raster_Buffer *)inbuf;
        private.bufowned = 0; /* the client's memory, never ours to free */
        /* and a raster the device did not allocate names no block of
           the device's to give back */
        private.buf->block = NULL;
    }
    else
    {
        /* allocate buffer header and array; the size covers both, and
           the memory to hold it is the machine's to give or refuse */
        private.buf = xpost_device_raster_block(bytes);
        if (!private.buf)
            return VMerror;
        private.buf->height = height;
        private.buf->width = width;
        private.buf->block = private.buf;
        private.bufowned = 1;
    }

    /* save private data struct in string */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
    {
        /* the record is the only thing that would have named the
           buffer, and it is not going to. A buffer the caller supplied
           is the caller's, and is left alone. */
        if (private.bufowned)
            free(private.buf);
        return VMerror;
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
    Xpost_Dev_Raster_Offset off;
    int r, g, b, ix, iy;

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

    /* Held inside the buffer, and reached by a position within the same
       buffer: the extent the record carries is the one the raster was
       priced and allocated over, so the bound and the index are about
       one block of memory rather than two. */
    if (ix < 0 || ix >= private.width || iy < 0 || iy >= private.height)
        return 0;
    off = xpost_dev_raster_offset(ix, iy, private.width);

    switch(private.pixelformat)
    {
        case BGRA:
        {
            Xpost_Raster_BGRA_Pixel pixel;

            pixel.blue = b;
            pixel.green = g;
            pixel.red = r;
            pixel.alpha = 255;
            ((Xpost_Raster_BGRA_Pixel*)private.buf->data)[off] = pixel;
        }
        break;
        case BGR:
        {
            Xpost_Raster_BGR_Pixel pixel;

            pixel.blue = b;
            pixel.green = g;
            pixel.red = r;
            ((Xpost_Raster_BGR_Pixel*)private.buf->data)[off] = pixel;
        }
        break;
        case ARGB:
        {
            Xpost_Raster_ARGB_Pixel pixel;

            pixel.alpha = 255;
            pixel.red = r;
            pixel.green = g;
            pixel.blue = b;
            ((Xpost_Raster_ARGB_Pixel*)private.buf->data)[off] = pixel;
        }
        break;
        case RGB:
        {
            Xpost_Raster_RGB_Pixel pixel;

            pixel.red = r;
            pixel.green = g;
            pixel.blue = b;
            ((Xpost_Raster_RGB_Pixel*)private.buf->data)[off] = pixel;
        }
        break;
    }

    /* save private data struct in string */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    return 0;
}

/* Read a pixel back in the device's stored channel scale, the same one
   PutPix writes, whichever of the four pixel layouts the buffer was
   created with. The class this device copies reads the base class's row
   array, which this device does not have, so the inherited method would
   answer undefined; a slot the class dictionary offers has to work. A
   pixel outside the raster reads as the page's ground, and so does every
   pixel of an instance whose buffer has been released. */
static
int _getpix(Xpost_Context *ctx,
            Xpost_Object x,
            Xpost_Object y,
            Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    int ix, iy;
    int r, g, b;

    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    xpost_device_ground_channels(ctx, devdic, &r, &g, &b);

    if (private.buf &&
        ix >= 0 && ix < private.width && iy >= 0 && iy < private.height)
    {
        Xpost_Dev_Raster_Offset i =
            xpost_dev_raster_offset(ix, iy, private.width);

        switch (private.pixelformat)
        {
            case BGRA:
            {
                Xpost_Raster_BGRA_Pixel p =
                    ((Xpost_Raster_BGRA_Pixel *)private.buf->data)[i];
                r = p.red; g = p.green; b = p.blue;
            }
            break;
            case BGR:
            {
                Xpost_Raster_BGR_Pixel p =
                    ((Xpost_Raster_BGR_Pixel *)private.buf->data)[i];
                r = p.red; g = p.green; b = p.blue;
            }
            break;
            case ARGB:
            {
                Xpost_Raster_ARGB_Pixel p =
                    ((Xpost_Raster_ARGB_Pixel *)private.buf->data)[i];
                r = p.red; g = p.green; b = p.blue;
            }
            break;
            case RGB:
            {
                Xpost_Raster_RGB_Pixel p =
                    ((Xpost_Raster_RGB_Pixel *)private.buf->data)[i];
                r = p.red; g = p.green; b = p.blue;
            }
            break;
        }
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(r));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(g));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(b));

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
    Xpost_Dev_Raster_Offset off;
    int r, g, b, c, ix, iy;

    r = xpost_dev_num_to_byte(red);
    g = xpost_dev_num_to_byte(green);
    b = xpost_dev_num_to_byte(blue);
    c = xpost_dev_num_to_int(cov);
    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released raster takes no marks */
    if (!private.buf)
        return 0;

    if (ix < 0 || ix >= private.width || iy < 0 || iy >= private.height)
        return 0;
    off = xpost_dev_raster_offset(ix, iy, private.width);

    if (c <= 0)
        return 0;
    if (c > 255)
        c = 255;

#define XPOST_RASTER_BLEND(dst, src) \
    xpost_dev_blend_channel((dst), (src), c)

    switch(private.pixelformat)
    {
        case BGRA:
        {
            Xpost_Raster_BGRA_Pixel *p =
                &((Xpost_Raster_BGRA_Pixel *)private.buf->data)[off];

            p->red = (unsigned char)XPOST_RASTER_BLEND(p->red, r);
            p->green = (unsigned char)XPOST_RASTER_BLEND(p->green, g);
            p->blue = (unsigned char)XPOST_RASTER_BLEND(p->blue, b);
            p->alpha = 255;
        }
        break;
        case BGR:
        {
            Xpost_Raster_BGR_Pixel *p =
                &((Xpost_Raster_BGR_Pixel *)private.buf->data)[off];

            p->red = (unsigned char)XPOST_RASTER_BLEND(p->red, r);
            p->green = (unsigned char)XPOST_RASTER_BLEND(p->green, g);
            p->blue = (unsigned char)XPOST_RASTER_BLEND(p->blue, b);
        }
        break;
        case ARGB:
        {
            Xpost_Raster_ARGB_Pixel *p =
                &((Xpost_Raster_ARGB_Pixel *)private.buf->data)[off];

            p->red = (unsigned char)XPOST_RASTER_BLEND(p->red, r);
            p->green = (unsigned char)XPOST_RASTER_BLEND(p->green, g);
            p->blue = (unsigned char)XPOST_RASTER_BLEND(p->blue, b);
            p->alpha = 255;
        }
        break;
        case RGB:
        {
            Xpost_Raster_RGB_Pixel *p =
                &((Xpost_Raster_RGB_Pixel *)private.buf->data)[off];

            p->red = (unsigned char)XPOST_RASTER_BLEND(p->red, r);
            p->green = (unsigned char)XPOST_RASTER_BLEND(p->green, g);
            p->blue = (unsigned char)XPOST_RASTER_BLEND(p->blue, b);
        }
        break;
    }

#undef XPOST_RASTER_BLEND

    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    return 0;
}

/* C fast-path for the base-class per-pixel FillRect. erasepage clears the
   whole page through FillRect, so this is on the hot path for every page. */
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
    int ix, iy, x0, y0, x1, y1, r, g, b, stride;

    /* fold numbers per the driver contract */
    r = xpost_dev_num_to_byte(red);
    g = xpost_dev_num_to_byte(green);
    b = xpost_dev_num_to_byte(blue);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released raster takes no marks */
    if (!private.buf)
        return 0;

    /* the contract's rectangle: inclusive span, clipped to the buffer */
    xpost_dev_rect_normalize(xpost_object_number(x), xpost_object_number(y),
                             xpost_object_number(w), xpost_object_number(h),
                             &x0, &y0, &x1, &y1);
    if (!xpost_dev_rect_clip(&x0, &y0, &x1, &y1,
                             private.width, private.height))
        return 0;
    stride = private.width;

#define RASTER_FILLRECT(TYPE, SET) \
    do { \
        TYPE pix; \
        SET \
        for (iy = y0; iy <= y1; iy++) \
        { \
            TYPE *row = (TYPE *)private.buf->data \
                      + xpost_dev_raster_offset(0, iy, stride); \
            for (ix = x0; ix <= x1; ix++) \
                row[ix] = pix; \
        } \
    } while (0)

    switch (private.pixelformat)
    {
        case BGRA:
            RASTER_FILLRECT(Xpost_Raster_BGRA_Pixel,
                pix.blue = b; pix.green = g; pix.red = r; pix.alpha = 255;);
            break;
        case BGR:
            RASTER_FILLRECT(Xpost_Raster_BGR_Pixel,
                pix.blue = b; pix.green = g; pix.red = r;);
            break;
        case ARGB:
            RASTER_FILLRECT(Xpost_Raster_ARGB_Pixel,
                pix.alpha = 255; pix.red = r; pix.green = g; pix.blue = b;);
            break;
        case RGB:
            RASTER_FILLRECT(Xpost_Raster_RGB_Pixel,
                pix.red = r; pix.green = g; pix.blue = b;);
            break;
    }
#undef RASTER_FILLRECT

    return 0;
}

static
int _flush(Xpost_Context *ctx,
           Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    return 0;
}


/* How this device answers for the shape of its own instance, so that
   the offering itself is written once (xpost_dev_buffer_emit_call). */
XPOST_DEV_BUFFER_ACCESSORS(PrivateData)

static
int _emit(Xpost_Context *ctx,
          Xpost_Object devdic)
{
    PrivateData private;

    return xpost_dev_buffer_emit_call(ctx, devdic, namePrivate,
                                      &private, sizeof(private),
                                      _raster_of, _disown);
}

static
int _destroy(Xpost_Context *ctx,
             Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* the same release the collector runs, so there is one statement of
       what this device owns rather than two that must be kept in step */
    _reclaim(&private);
    /* store the cleared pointer back so a repeated destroy is a no-op */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    return 0;
}



/* operator function to instantiate a new window device.
   installed in the private dictionary by calling 'loadXXXdevice'.
 */
static
int newrasterdevice(Xpost_Context *ctx,
                    Xpost_Object width,
                    Xpost_Object height)
{
    Xpost_Object classdic;
    int ret;

    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_RASTERDEVICE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, xpost_name_cons(ctx, "Create"))))
        return execstackoverflow;

    return 0;
}

static
unsigned int _loadrasterdevicecont_opcode;

/* Specializes or sub-classes the .xpost_PPMIMAGE device class.
   load .xpost_PPMIMAGE
   load and call ps procedure .copydict which leaves copy on stack
   call loadrasterdevicecont by continuation.
 */
static
int loadrasterdevice (Xpost_Context *ctx)
{
    Xpost_Object classdic;
    int ret;

    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PPMIMAGE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_operator_cons_opcode(_loadrasterdevicecont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;

    return 0;
}

/* replace procedures in the class with newly created special operators.
   defines the device class rasterDEVICE in the private dictionary.
   defines its maker beside it: newrasterdevice
 */
static
int loadrasterdevicecont(Xpost_Context *ctx,
                         Xpost_Object classdic)
{
    /* this device's method suite; the arities follow from its
       declared colour space */
    static const Xpost_Dev_Method methods[] =
    {
        { "Create", "rasterCreate", (Xpost_Op_Func)_create, XPOST_DEV_M_CREATE },
        { "PutPix", "rasterPutPix", (Xpost_Op_Func)_putpix, XPOST_DEV_M_PUTPIX },
        { "GetPix", "rasterGetPix", (Xpost_Op_Func)_getpix, XPOST_DEV_M_GETPIX },
        { "BlendPix", "rasterBlendPix", (Xpost_Op_Func)_blendpix, XPOST_DEV_M_BLEND },
        { "FillRect", "rasterFillRect", (Xpost_Op_Func)_fillrect, XPOST_DEV_M_RECT },
        { "Emit", "rasterEmit", (Xpost_Op_Func)_emit, XPOST_DEV_M_PAGE },
        { "Flush", "rasterFlush", (Xpost_Op_Func)_flush, XPOST_DEV_M_PAGE },
        { "Destroy", "rasterDestroy", (Xpost_Op_Func)_destroy, XPOST_DEV_M_PAGE }
    };

    Xpost_Object op;
    int ret;

    ret = xpost_dict_put(ctx, classdic, namenativecolorspace, nameDeviceRGB);
    if (ret)
        return ret;

    /* This device's page does not arrive a band at a time. The raster is
       given to whoever embedded the interpreter, which asked for a page
       and holds one, so the page is whole by the contract it asked
       under: holding less of it at once would bound nothing and would
       hand back less than a page (doc/INTERNALS).

       Taken back out rather than left unsaid. The class is a copy of the
       colour raster class, which says its page may arrive that way, and
       a copy carries what it was copied from -- so a device that has not
       considered the question says yes by inheritance, and the safe
       answer is the one that has to be stated. */
    ret = xpost_dict_undef(ctx, classdic, xpost_name_cons(ctx, "BandedPage"));
    if (ret && ret != undefined)
        return ret;

    /* And this class states nothing about what a row of its raster
       costs, because it has no one answer to state: the pixel format is
       asked for when the device is made, and a pixel carrying an alpha
       channel is a byte wider than one that does not. What a row costs
       here is a property of the instance and not of the class, so what
       the colour raster class this one is a copy of states about its own
       three-byte planar row is taken back out rather than answered on
       behalf of a raster of some other shape. */
    ret = xpost_dev_class_no_rowcost(ctx, classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "rasterCreateCont", (Xpost_Op_Func)_create_cont, 3, integertype, integertype, dicttype);
    _create_cont_opcode = op.mark_.padw;

    ret = xpost_dev_class_install(ctx, classdic, 3, 1,
                                  methods, XPOST_DEV_METHOD_COUNT(methods));
    if (ret)
        return ret;









    /* The class and its maker live in the private dictionary, beside the
       classes the boot files define: a program reaches a device through
       the page-device request, and the machinery reaches the class by
       name here. Nothing of the driver's is defined where a program
       could shadow it. */
    ret = xpost_dict_put(ctx, ctx->privatedict,
                         xpost_name_cons(ctx, ".xpost_RASTERDEVICE"), classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "newrasterdevice", (Xpost_Op_Func)newrasterdevice, 2, integertype, integertype);
    ret = xpost_dict_put(ctx, ctx->privatedict, xpost_name_cons(ctx, "newrasterdevice"), op);
    if (ret)
        return ret;

    return 0;
}

/*
   install the loadXXXdevice which may be called during graphics initialization
   to produce the operator newXXXdevice which instantiates the device dictionary.
*/
int xpost_oper_init_raster_device_ops (Xpost_Context *ctx,
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

    optab = xpost_operator_table(ctx->gl);
    op = xpost_operator_cons(ctx, "loadrasterdevice", (Xpost_Op_Func)loadrasterdevice, 0); INSTALL;
    op = xpost_operator_cons(ctx, "loadrasterdevicecont", (Xpost_Op_Func)loadrasterdevicecont, 1, dicttype);
    _loadrasterdevicecont_opcode = op.mark_.padw;

    return 0;
}
