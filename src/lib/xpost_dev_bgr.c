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
//#include <stdio.h>
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
#include "xpost_dev_driver.h" /* device contract and shared helpers */
#include "xpost_dev_generic.h"
#include "xpost_dev_bgr.h" /* check prototypes */

typedef struct
{
    unsigned char blue, green, red;
} Xpost_Bgr_Pixel;

typedef struct
{
    int width, height, byte_stride;
    /* the block this raster is part of. A client is handed the raster
       and gives the block back, so the block's own address is kept
       here, immediately before the raster, where the release entry
       point reads it. */
    void *block;
    Xpost_Bgr_Pixel data[1];
} Xpost_Bgr_Buffer;

XPOST_DEV_ASSERT_BLOCK_PRECEDES_RASTER(bgr, Xpost_Bgr_Buffer, block, data);

typedef struct
{
    int width, height;
    /*
     * add additional members to private struct
     */
    Xpost_Bgr_Buffer *buf;
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
    Xpost_Object privatestr;
    PrivateData private;
    int width, height;
    int ret;

    /* The page the program asked for, as the extent of the buffer that
       will hold it. Every device here holds a whole page in one block,
       so the two carry the same numbers; a page naming an extent no
       buffer's row arithmetic carries is refused before anything is
       built for it. */
    if (!xpost_dev_page_extent(w.int_.val, h.int_.val, &width, &height))
        return limitcheck;

    /* The block this device's instance state lives in, and, named with
       it rather than after it, what gives up whatever that state names.
       What this device holds is a raster, which is not virtual memory:
       a device the run never retires -- one a restore took back, or one
       nothing named by the time a collection came round -- would take
       its raster with it. */
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

    {
        /* allocate buffer header and array */
        {
            size_t bytes;

            if (!xpost_device_raster_bytes(width, height,
                                           sizeof(Xpost_Bgr_Pixel),
                                           sizeof(Xpost_Bgr_Buffer), &bytes))
            {
                XPOST_LOG_ERR("%d a raster for a page of %dx%d is larger"
                              " than this platform addresses", limitcheck,
                              (int)width, (int)height);
                return limitcheck;
            }
            /* the size covers header and raster both; the memory to
               hold it is the machine's to give or refuse */
            private.buf = xpost_device_raster_block(bytes);
        }
        if (!private.buf)
            return VMerror;
        private.buf->block = private.buf;
        private.bufowned = 1;
    }

    /* save private data struct in string */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
    {
        /* the record is the only thing that would have named the
           buffer, and it is not going to */
        if (private.bufowned)
            free(private.buf);
        return VMerror;
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

    /* a released raster takes no marks: the recorded dimensions outlive
       the buffer, so the bounds check below does not stand in for this */
    if (!private.buf)
        return 0;

    if ((ix < 0) || (ix >= private.width) ||
        (iy < 0) || (iy >= private.height))
        return 0;

    if (c <= 0)
        return 0;
    if (c > 255)
        c = 255;

    {
        Xpost_Bgr_Pixel *p = &private.buf->data
            [xpost_dev_raster_offset(ix, iy, private.width)];

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

    /* a released raster takes no marks */
    if (!private.buf)
        return 0;

    /* check bounds */
    if ((ix < 0) || (ix >= private.width) ||
        (iy < 0) || (iy >= private.height))
        return 0;

    {
        Xpost_Bgr_Pixel pixel;
        pixel.blue = b;
        pixel.green = g;
        pixel.red = r;
        private.buf->data[xpost_dev_raster_offset(ix, iy, private.width)]
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
   erasepage over the whole of it, so that walk is the page's own area in
   dispatches before a program has drawn anything.

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
    Xpost_Bgr_Pixel pixel;
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

    for (iy = y0; iy <= y1; iy++)
    {
        Xpost_Bgr_Pixel *row = private.buf->data
                             + xpost_dev_raster_offset(0, iy, private.width);

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
   does every pixel of an instance whose buffer has been released. */
static
int _getpix(Xpost_Context *ctx,
            Xpost_Object x,
            Xpost_Object y,
            Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    int ix, iy, r, g, b;

    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    if (!private.buf ||
        (ix < 0) || (ix >= private.width) ||
        (iy < 0) || (iy >= private.height))
        xpost_device_ground_channels(ctx, devdic, &r, &g, &b);
    else
    {
        Xpost_Bgr_Pixel pixel = private.buf->data
            [xpost_dev_raster_offset(ix, iy, private.width)];

        r = pixel.red; g = pixel.green; b = pixel.blue;
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(r));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(g));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(b));

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

/* -  .rowcost  elements bytes
   What one row of this device's raster costs, at the width the caller
   states on the dictionary stack. A pixel here is blue, green and red,
   one byte each, interleaved in a buffer this device allocates -- so a
   row is three bytes a pixel and no elements of the memory the
   interpreter allocates rows out of. The same sizeof the buffer is
   measured with, so the price and the allocation cannot drift apart. */
static
int _rowcost(Xpost_Context *ctx)
{
    return xpost_dev_rowcost(ctx, (int)sizeof(Xpost_Bgr_Pixel));
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
int newbgrdevice(Xpost_Context *ctx,
                 Xpost_Object width,
                 Xpost_Object height)
{
    Xpost_Object classdic;
    int ret;

    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_BGRDEVICE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, xpost_name_cons(ctx, "Create"))))
        return execstackoverflow;

    return 0;
}

static
unsigned int _loadbgrdevicecont_opcode;

/* Specializes or sub-classes the .xpost_PPMIMAGE device class.
   load .xpost_PPMIMAGE
   load and call ps procedure .copydict which leaves copy on stack
   call loadbgrdevicecont by continuation.
 */
static
int loadbgrdevice(Xpost_Context *ctx)
{
    Xpost_Object classdic;
    int ret;

    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PPMIMAGE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_operator_cons_opcode(_loadbgrdevicecont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;

    return 0;
}

/* replace procedures in the class with newly created special operators.
   defines the device class bgrDEVICE in the private dictionary.
   defines its maker beside it: newbgrdevice
 */
static
int loadbgrdevicecont(Xpost_Context *ctx,
                      Xpost_Object classdic)
{
    /* this device's method suite; the arities follow from its
       declared colour space */
    static const Xpost_Dev_Method methods[] =
    {
        { "Create", "bgrCreate", (Xpost_Op_Func)_create, XPOST_DEV_M_CREATE },
        { "PutPix", "bgrPutPix", (Xpost_Op_Func)_putpix, XPOST_DEV_M_PUTPIX },
        { "FillRect", "bgrFillRect", (Xpost_Op_Func)_fillrect, XPOST_DEV_M_RECT },
        { "GetPix", "bgrGetPix", (Xpost_Op_Func)_getpix, XPOST_DEV_M_GETPIX },
        { "BlendPix", "bgrBlendPix", (Xpost_Op_Func)_blendpix, XPOST_DEV_M_BLEND },
        { "Emit", "bgrEmit", (Xpost_Op_Func)_emit, XPOST_DEV_M_PAGE },
        { "Flush", "bgrFlush", (Xpost_Op_Func)_flush, XPOST_DEV_M_PAGE },
        { "Destroy", "bgrDestroy", (Xpost_Op_Func)_destroy, XPOST_DEV_M_PAGE }
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

    /* What one row of this device's raster costs. The bytes come to
       what the colour raster class this one is a copy of states and the
       elements do not, and a copy carries what it was copied from, so
       this says both rather than inheriting them. */
    ret = xpost_dev_class_rowcost(ctx, classdic, "bgrRowCost",
                                  (Xpost_Op_Func)_rowcost);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "bgrCreateCont", (Xpost_Op_Func)_create_cont, 3, integertype, integertype, dicttype);
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
                         xpost_name_cons(ctx, ".xpost_BGRDEVICE"), classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "newbgrdevice", (Xpost_Op_Func)newbgrdevice, 2, integertype, integertype);
    ret = xpost_dict_put(ctx, ctx->privatedict, xpost_name_cons(ctx, "newbgrdevice"), op);
    if (ret)
        return ret;

    return 0;
}

/*
   install the loadXXXdevice which may be called during graphics initialization
   to produce the operator newXXXdevice which instantiates the device dictionary.
*/
int xpost_oper_init_bgr_device_ops(Xpost_Context *ctx,
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
    op = xpost_operator_cons(ctx, "loadbgrdevice", (Xpost_Op_Func)loadbgrdevice, 0); INSTALL;
    op = xpost_operator_cons(ctx, "loadbgrdevicecont", (Xpost_Op_Func)loadbgrdevicecont, 1, dicttype);
    _loadbgrdevicecont_opcode = op.mark_.padw;

    return 0;
}
