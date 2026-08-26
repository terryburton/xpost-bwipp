/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_xcb.c
 * @brief The X11 window device, through XCB.
 *
 * Draws into a window on a running display server rather than into a file.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h> /* abs */
#include <stddef.h>

#include <string.h>

#include <xcb/xcb.h>

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
#include "xpost_op_dict.h" /* call xpost_op_any_load operator for convenience */
#include "xpost_dev_generic.h" /* the page's ground */
#include "xpost_dev_driver.h" /* device contract and shared helpers */
#include "xpost_dev_xcb.h" /* check prototypes */

#define XCB_ALL_PLANES ~0

/* The scale this device holds a colour channel on. Its colours reach the
   display server as 16-bit components, so the contract's fold is applied
   at that scale: a colour operand, and the page's ground, come to a
   number in this range rather than in a byte. */
#define XCB_CHANNEL_SCALE 65535.0

typedef struct
{
    xcb_connection_t *c;
    xcb_screen_t *scr;
    xcb_drawable_t win;
    int width, height;
    xcb_pixmap_t img;
    xcb_gcontext_t gc;
    xcb_colormap_t cmap;
    /* the last ink the colormap was asked for, and what it answered.
       Allocating one is a round trip to the display server, and a mark
       is laid a pixel at a time in one colour -- a glyph is thousands of
       pixels of the same ink -- so the answer is kept and the question
       asked again only when the colour changes. */
    int have_ink;
    int ink_r, ink_g, ink_b;
    unsigned int ink_pixel;
} PrivateData;

/* Give up the display connection the instance names, and the window and
   the drawing state the server holds behind it. Called from the
   collector with the block the instance state is kept in, so it touches
   nothing in virtual memory -- including the context's record of which
   device the window is, which names a device the collector has not
   reached. A device the run retired has given the connection up already
   and leaves this nothing to do. */
static void _reclaim(void *block)
{
    PrivateData *p = block;

    if (p->c)
        xcb_disconnect(p->c);
    p->c = NULL;
}

static int _flush(Xpost_Context *ctx, Xpost_Object devdic);

static
unsigned int _event_handler_opcode;

static Xpost_Object namePrivate;
static Xpost_Object namewidth;
static Xpost_Object nameheight;
static Xpost_Object namedotcopydict;
static Xpost_Object namenativecolorspace;
static Xpost_Object nameDeviceRGB;

static
int _event_handler(Xpost_Context *ctx,
                   Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    xcb_generic_event_t *event;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a destroyed device holds no connection, and nothing is polled
       through one */
    if (!private.c)
        return 0;

    event = xcb_poll_for_event(private.c);
    if (event)
    {
        switch(event->response_type & ~0x80)
        {
            case XCB_EXPOSE:
                /* the only answer _flush has other than success is for a
                   /Private it cannot read, and this frame read the same
                   one from the same dictionary above. Polling for an
                   event runs no PostScript and allocates nothing, so
                   what was there then is there now. */
                _flush(ctx, devdic);
                break;
            default:
                break;
        }
        free(event);
    }
    else if (xcb_connection_has_error(private.c))
        return unregistered;

    return 0;
}


static
unsigned int _create_cont_opcode;

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
    xcb_screen_iterator_t iter;
    xcb_get_geometry_reply_t *geom;
    integer width = w.int_.val;
    integer height = h.int_.val;
    int scrno;
    unsigned char depth;
    int ret;

    /* The block this device's instance state lives in, and, named with
       it rather than after it, what gives up whatever that state names.
       What this device holds is a display connection, which is not
       virtual memory: a device the run never retires -- one a restore
       took back, or one nothing named by the time a collection came
       round -- would take its connection with it. This is what gives it
       up there. */
    ret = xpost_handle_cons(ctx, devdic, namePrivate, &privatestr,
                            XPOST_HANDLE_DEVICE, sizeof(PrivateData),
                            _reclaim);
    if (ret)
        return ret;

    private.width = width;
    private.height = height;

    /* create xcb connection
       and create and map window */
    private.c = xcb_connect(NULL, &scrno);
    if (xcb_connection_has_error(private.c))
    {
        XPOST_LOG_ERR("Fail to connect to the X server");
        return unregistered;
    }

    /* the screen the connection named. The walk below stops at it, and
       runs out instead where the server has no such screen; nothing is
       read through the answer until it is one. */
    private.scr = NULL;
    iter = xcb_setup_roots_iterator(xcb_get_setup(private.c));
    for (; iter.rem; --scrno, xcb_screen_next(&iter))
    {
        if (scrno == 0)
        {
            private.scr = iter.data;
            break;
        }
    }
    if (!private.scr)
    {
        XPOST_LOG_ERR("the display names a screen the server does not have");
        xcb_disconnect(private.c);
        return unregistered;
    }

    geom = xcb_get_geometry_reply(private.c, xcb_get_geometry(private.c, private.scr->root), 0);
    if (!geom)
    {
        XPOST_LOG_ERR("Fail to the geometry of the root window");
        xcb_disconnect(private.c);
        return unregistered;
    }

    depth = geom->depth;
    free(geom);

    /* a page larger than the screen opens in a smaller window with the
       page scaled to fit: the adopted dimensions land back in the
       device dictionary, and the ratio rides /windowscale for the
       default matrix to fold in */
    {
        int sw = private.scr->width_in_pixels;
        int sh = private.scr->height_in_pixels;
        double s = 1.0;

        if (sw > 0 && width > sw)
            s = (double)sw / (double)width;
        if (sh > 0 && (double)height * s > (double)sh)
            s = (double)sh / (double)height;
        if (s < 1.0)
        {
            width = (integer)((double)width * s + 0.5);
            height = (integer)((double)height * s + 0.5);
            if (width < 1) width = 1;
            if (height < 1) height = 1;
            private.width = width;
            private.height = height;
            ret = xpost_dict_put(ctx, devdic, namewidth, xpost_int_cons(width));
            if (!ret)
                ret = xpost_dict_put(ctx, devdic, nameheight,
                                     xpost_int_cons(height));
            if (!ret)
                ret = xpost_dict_put(ctx, devdic,
                                     xpost_name_cons(ctx, "windowscale"),
                                     xpost_real_cons((real)s));
            if (ret)
            {
                xcb_disconnect(private.c);
                return ret;
            }
        }
    }

    private.win = xcb_generate_id(private.c);
    {
        unsigned int mask = XCB_CW_BACK_PIXMAP |
                            XCB_CW_BACK_PIXEL |
                            XCB_CW_EVENT_MASK;
        unsigned int value[3];
        value[0] = XCB_NONE;
        value[1] = private.scr->white_pixel;
        value[2] = XCB_EVENT_MASK_EXPOSURE;
        xcb_create_window(private.c, XCB_COPY_FROM_PARENT,
                          private.win, private.scr->root,
                          0, 0,
                          width, height,
                          5,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          private.scr->root_visual,
                          mask,
                          value);

        /* set title */
        xcb_change_property(private.c,
                            XCB_PROP_MODE_REPLACE,
                            private.win,
                            XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING,
                            8,
                            sizeof("Xpost") - 1,
                            "Xpost");
    }
    xcb_map_window(private.c, private.win);
    xcb_flush(private.c);

    private.img = xcb_generate_id(private.c);
    xcb_create_pixmap(private.c,
                      depth, private.img,
                      private.win, private.width, private.height);

    /* create graphics context
       and initialize drawing parameters */
    private.gc = xcb_generate_id(private.c);
    {
        unsigned int values[2] =
            {
                private.scr->black_pixel,
                private.scr->white_pixel
            };
        xcb_create_gc(private.c, private.gc, private.win,
                      XCB_GC_FOREGROUND | XCB_GC_BACKGROUND,
                      values);
    }

    /* create colormap */
    private.cmap = xcb_generate_id(private.c);
    xcb_create_colormap(private.c, XCB_COLORMAP_ALLOC_NONE, private.cmap,
                        private.win, private.scr->root_visual);

    xpost_context_install_event_handler(ctx,
                                        xpost_operator_cons_opcode(_event_handler_opcode),
                                        devdic);


    /* save private data struct in string */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    /* return device instance dictionary to ps */
    xpost_stack_push(ctx->lo, ctx->os, devdic);
    return 0;
}

/* What the display server calls this 16-bit rgb ink, and the graphics
   context set to lay it. Asking costs a round trip -- the reply has to
   arrive before anything can be drawn with it -- so the last answer is
   kept and the question asked again only when the colour changes. A
   mark is laid in one colour, and a glyph is thousands of pixels of it. */
static
int _ink(PrivateData *private, int r, int g, int b)
{
    xcb_alloc_color_reply_t *rep;

    if (private->have_ink
        && private->ink_r == r && private->ink_g == g && private->ink_b == b)
    {
        xcb_change_gc(private->c, private->gc, XCB_GC_FOREGROUND,
                      &private->ink_pixel);
        return 0;
    }

    rep = xcb_alloc_color_reply(private->c,
                                xcb_alloc_color(private->c, private->cmap,
                                                r, g, b),
                                0);
    if (!rep)
        return unregistered;

    private->ink_pixel = rep->pixel;
    free(rep);
    private->ink_r = r; private->ink_g = g; private->ink_b = b;
    private->have_ink = 1;

    xcb_change_gc(private->c, private->gc, XCB_GC_FOREGROUND,
                  &private->ink_pixel);
    return 0;
}

/* Lay one pixel of the drawable in a 16-bit rgb colour, through the
   colormap the device allocates its inks from. */
static
int _point(PrivateData *private, int r, int g, int b, int ix, int iy)
{
    xcb_point_t p;
    int ret;

    p.x = ix;
    p.y = iy;

    if ((ret = _ink(private, r, g, b)))
        return ret;

    xcb_poly_point(private->c, XCB_COORD_MODE_ORIGIN,
                   private->img, private->gc, 1, &p);
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
    int r, g, b, ix, iy, ret;

    /* fold numbers per the driver contract; xcb colour channels are 16-bit */
    r = xpost_dev_num_to_scaled(red, XCB_CHANNEL_SCALE);
    g = xpost_dev_num_to_scaled(green, XCB_CHANNEL_SCALE);
    b = xpost_dev_num_to_scaled(blue, XCB_CHANNEL_SCALE);
    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released device takes no marks: the recorded dimensions outlive
       the connection, so the bounds check below does not stand in for this */
    if (!private.c)
        return 0;

    /* check bounds */
    if ((ix < 0) || (ix >= private.width) ||
        (iy < 0) || (iy >= private.height))
        return 0;

    ret = _point(&private, r, g, b, ix, iy);
    if (ret)
        return ret;

    /* save private data struct in string */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    return 0;
}

/* Blend a coverage-weighted pixel: each channel moves toward the colour
   by cov/255 from the level the pixel already holds. What this device
   reads a pixel back as is the ground (GetPix below), its raster being
   a drawable on the display server reached through a colormap, so the
   ground is what a partly covered pixel is composited over and the
   result goes down through the same point the solid path lays.

   The class this device specialises carries a blend that reads a raster
   held as PostScript row arrays. This device keeps no such array, and
   the driver contract names BlendPix among the slots a device with a
   raster of its own brings itself. */
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
    int r, g, b, c, ix, iy, ret;
    int dr, dg, db;

    /* fold numbers per the driver contract; xcb colour channels are 16-bit */
    r = xpost_dev_num_to_scaled(red, XCB_CHANNEL_SCALE);
    g = xpost_dev_num_to_scaled(green, XCB_CHANNEL_SCALE);
    b = xpost_dev_num_to_scaled(blue, XCB_CHANNEL_SCALE);
    c = xpost_dev_num_to_int(cov);
    ix = xpost_dev_num_to_int(x);
    iy = xpost_dev_num_to_int(y);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released device takes no marks */
    if (!private.c)
        return 0;

    /* check bounds */
    if ((ix < 0) || (ix >= private.width) ||
        (iy < 0) || (iy >= private.height))
        return 0;

    if (c <= 0)
        return 0;
    if (c > 255)
        c = 255;

    xpost_device_ground_scaled(ctx, devdic, XCB_CHANNEL_SCALE, &dr, &dg, &db);

    r = xpost_dev_blend_channel(dr, r, c);
    g = xpost_dev_blend_channel(dg, g, c);
    b = xpost_dev_blend_channel(db, b, c);

    ret = _point(&private, r, g, b, ix, iy);
    if (ret)
        return ret;

    /* save private data struct in string */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;

    return 0;
}

/* This device's raster is a pixmap on the display server, reached
   through a colormap rather than held as channel values, so a pixel
   read back would be a round trip returning an index this device cannot
   turn into the components it was given. It answers the ground instead,
   as the vector writers do: a method the class dictionary offers must
   answer its declared results, and answering nothing leaves the caller
   reading whatever was beneath.

   The ground is the colour erasepage left, which the base class records
   on the instance and every device here reads back the same way, folded
   at this device's channel scale so that what a read answers is what
   this device's PutPix would have written for it. The window reading
   back as the ground is a statement about the drawable, not about the
   colour: a page cleared to a light grey reads light. BlendPix above
   composites a partly covered pixel over the same value. */

/* A colour as this device holds it: allocated through the colormap and
   asked back, which is the round trip a stored pixel has already been
   through. The server keeps fewer bits than the sixteen a component is
   named in -- eight, on the usual visual -- so a colour named exactly
   and a colour read back off the page differ by the rounding unless
   both have been through here. The ground reported for a pixel outside
   the page is the ground as the page would hold it, so that the two
   answers can be compared. */
static
int _as_stored(PrivateData *private, int *r, int *g, int *b)
{
    xcb_query_colors_reply_t *cols;
    xcb_rgb_t *rgb;
    int ret;

    if ((ret = _ink(private, *r, *g, *b)))
        return ret;

    cols = xcb_query_colors_reply(private->c,
                                  xcb_query_colors(private->c, private->cmap,
                                                   1, &private->ink_pixel),
                                  NULL);
    if (!cols)
        return unregistered;
    rgb = xcb_query_colors_colors(cols);
    if (xcb_query_colors_colors_length(cols) > 0)
    {
        *r = rgb->red; *g = rgb->green; *b = rgb->blue;
    }
    free(cols);
    return 0;
}

/* What the drawable holds at one pixel, as the 16-bit rgb the device
   lays ink in. The pixel the server keeps is a number in the visual's
   own encoding, and the colormap is what turns it back into components
   -- the same colormap the ink was allocated from, so the answer is in
   the terms the question was asked in.

   A pixel outside the drawable, and a device whose connection has been
   given back, are the ground: there is no mark there to report. So is a
   request the server does not answer, which is the reading failing
   rather than the pixel being unmarked -- it is logged, and the ground
   is what a caller can do something with. */
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

    ix = xpost_dev_pixel(xpost_object_number(x));
    iy = xpost_dev_pixel(xpost_object_number(y));

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    xpost_device_ground_scaled(ctx, devdic, XCB_CHANNEL_SCALE, &r, &g, &b);
    /* the ground as this device would hold it, so that a pixel off the
       page and a pixel on it answer in the same terms */
    if (private.c)
        (void)_as_stored(&private, &r, &g, &b);

    if (private.c
        && ix >= 0 && ix < private.width && iy >= 0 && iy < private.height)
    {
        xcb_get_image_reply_t *img;

        img = xcb_get_image_reply(private.c,
                                  xcb_get_image(private.c,
                                                XCB_IMAGE_FORMAT_Z_PIXMAP,
                                                private.img, ix, iy, 1, 1,
                                                (uint32_t)~0UL),
                                  NULL);
        if (!img)
            XPOST_LOG_ERR("the display server did not answer for the pixel"
                          " at %d,%d", (int)ix, (int)iy);
        else
        {
            /* one pixel was asked for, so what came back is exactly the
               bytes of one, however many the visual spends on it */
            const unsigned char *d = xcb_get_image_data(img);
            int n = xcb_get_image_data_length(img);
            uint32_t pixel = 0;
            int i;
            xcb_query_colors_reply_t *cols;

            for (i = 0; i < n && i < (int)sizeof(pixel); i++)
                pixel |= (uint32_t)d[i] << (8 * i);
            free(img);

            cols = xcb_query_colors_reply(private.c,
                                          xcb_query_colors(private.c,
                                                           private.cmap,
                                                           1, &pixel),
                                          NULL);
            if (!cols)
                XPOST_LOG_ERR("the colormap did not answer for the pixel"
                              " at %d,%d", (int)ix, (int)iy);
            else
            {
                xcb_rgb_t *rgb = xcb_query_colors_colors(cols);

                if (xcb_query_colors_colors_length(cols) > 0)
                {
                    r = rgb->red;
                    g = rgb->green;
                    b = rgb->blue;
                }
                free(cols);
            }
        }
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(r));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(g));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(b));
    return 0;
}

static
int _drawline(Xpost_Context *ctx,
              Xpost_Object red,
              Xpost_Object green,
              Xpost_Object blue,
              Xpost_Object x1,
              Xpost_Object y1,
              Xpost_Object x2,
              Xpost_Object y2,
              Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    int r, g, b, px, py;
    Xpost_Dev_Line line;

    /* fold numbers per the driver contract; xcb colour channels are 16-bit */
    r = xpost_dev_num_to_scaled(red, XCB_CHANNEL_SCALE);
    g = xpost_dev_num_to_scaled(green, XCB_CHANNEL_SCALE);
    b = xpost_dev_num_to_scaled(blue, XCB_CHANNEL_SCALE);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released device takes no marks */
    if (!private.c)
        return 0;

    {
        xcb_alloc_color_reply_t *rep;
        unsigned int value;

        rep = xcb_alloc_color_reply(private.c,
                                    xcb_alloc_color(private.c, private.cmap,
                                                    r, g, b),
                                    0);
        if (!rep)
            return unregistered;

        value = rep->pixel;
        free(rep);
        xcb_change_gc(private.c, private.gc, XCB_GC_FOREGROUND, &value);
    }

    /* the contract's line, plotted pixel by pixel: the server would
       draw a segment of its own between the endpoints, and its idea of
       which pixels that covers is not the one every other device paints */
    xpost_dev_line_init(&line,
                        xpost_object_number(x1), xpost_object_number(y1),
                        xpost_object_number(x2), xpost_object_number(y2));
    while (xpost_dev_line_next(&line, &px, &py))
    {
        xcb_point_t p;

        if (px < 0 || px >= private.width || py < 0 || py >= private.height)
            continue;
        p.x = px;
        p.y = py;
        xcb_poly_point(private.c, XCB_COORD_MODE_ORIGIN,
                       private.img, private.gc, 1, &p);
    }

    return 0;
}

static
int _fillrect(Xpost_Context *ctx,
              Xpost_Object red,
              Xpost_Object green,
              Xpost_Object blue,
              Xpost_Object x,
              Xpost_Object y,
              Xpost_Object width,
              Xpost_Object height,
              Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    int ret;
    int i,j;
    int r, g, b;
    int x0, y0, x1, y1;

    /* fold numbers per the driver contract; xcb colour channels are 16-bit */
    r = xpost_dev_num_to_scaled(red, XCB_CHANNEL_SCALE);
    g = xpost_dev_num_to_scaled(green, XCB_CHANNEL_SCALE);
    b = xpost_dev_num_to_scaled(blue, XCB_CHANNEL_SCALE);

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released device takes no marks */
    if (!private.c)
        return 0;

    /* the contract's rectangle: inclusive span, clipped to the device */
    xpost_dev_rect_normalize(xpost_object_number(x), xpost_object_number(y),
                             xpost_object_number(width),
                             xpost_object_number(height),
                             &x0, &y0, &x1, &y1);
    if (!xpost_dev_rect_clip(&x0, &y0, &x1, &y1,
                             private.width, private.height))
        return 0;

    if ((ret = _ink(&private, r, g, b)))
        return ret;

    {
        for (i = y0; i <= y1; i++)
        {
            for (j = x0; j <= x1; j++)
            {
                xcb_point_t p;
                p.x = j;
                p.y = i;

                xcb_poly_point(private.c, XCB_COORD_MODE_ORIGIN,
                               private.img, private.gc, 1, &p);
            }
        }
    }
    return 0;
}

/* This device brings no polygon fill of its own. A polygon is filled by
   the shared scan conversion, which resolves it to spans and paints each
   through the FillRect above -- the same route the line walk and the
   rectangle take, and for the same reason: the pixels a shape covers are
   the driver contract's, and the display server's idea of them is not
   the one every other device paints. The server's polygon primitive
   would differ on more than edges. The polygon arrives as one point list
   with a null between subpaths, which that primitive has no form for,
   and the rule PostScript fills a path under is the nonzero winding
   number (PLRM 8.2), which is not the rule a server polygon is drawn
   with. */

static
int _flush(Xpost_Context *ctx,
           Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    /* a released device emits nothing, its page having gone up when the
       connection it was drawn over was released */
    if (!private.c)
        return 0;

    xcb_copy_area(private.c, private.img, private.win, private.gc,
                  0, 0, 0, 0, private.width, private.height);
    xcb_flush(private.c);

    return 0;
}

/* Emit here is the same as Flush
   But Flush is called (if available) by all raster operators
   for smoother previewing.
 */
#define _emit _flush

static
int _destroy(Xpost_Context *ctx,
             Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!xpost_dev_private_get(ctx, devdic, namePrivate,
                               &privatestr, &private, sizeof(private)))
        return undefined;

    if (private.c)
    {
        /* The handler names one device, and by now that may be a
           device made after this one: replacing a page device installs
           the new device before it retires the old, so clearing the
           handler unasked would leave the live window hearing nothing.
           A device gives up only what it was given. */
        if (xpost_dict_compare_objects(ctx, ctx->window_device, devdic) == 0)
            xpost_context_install_event_handler(ctx, null, null);

        /* the same release the collector runs, so what this device owns
           is stated once */
        _reclaim(&private);
        /* store the cleared connection back so a repeated destroy is a
           no-op instead of disconnecting freed state */
        if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
            return VMerror;
    }

    return 0;
}



/* operator function to instantiate a new window device.
   installed in the private dictionary by calling 'loadXXXdevice'.
 */
static
int newxcbdevice(Xpost_Context *ctx,
                 Xpost_Object width,
                 Xpost_Object height)
{
    Xpost_Object classdic;
    int ret;

    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_XCBDEVICE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_dict_get(ctx, classdic, xpost_name_cons(ctx, "Create"))))
        return execstackoverflow;

    return 0;
}

static
unsigned int _loadxcbdevicecont_opcode;

/* Specializes or sub-classes the .xpost_PPMIMAGE device class.
   load .xpost_PPMIMAGE
   load and call ps procedure .copydict which leaves copy on stack
   call loadxcbdevicecont by continuation.
 */
static
int loadxcbdevice(Xpost_Context *ctx)
{
    Xpost_Object classdic;
    int ret;

    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_PPMIMAGE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_loadxcbdevicecont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;

    return 0;
}

/* replace procedures in the class with newly created special operators.
   defines the device class xcbDEVICE in the private dictionary.
   defines its maker beside it: newxcbdevice
 */
static
int loadxcbdevicecont(Xpost_Context *ctx,
                      Xpost_Object classdic)
{
    /* this device's method suite; the arities follow from its
       declared colour space */
    static const Xpost_Dev_Method methods[] =
    {
        { "Create", "xcbCreate", (Xpost_Op_Func)_create, XPOST_DEV_M_CREATE },
        { "PutPix", "xcbPutPix", (Xpost_Op_Func)_putpix, XPOST_DEV_M_PUTPIX },
        { "GetPix", "xcbGetPix", (Xpost_Op_Func)_getpix, XPOST_DEV_M_GETPIX },
        { "BlendPix", "xcbBlendPix", (Xpost_Op_Func)_blendpix, XPOST_DEV_M_BLEND },
        { "DrawLine", "xcbDrawLine", (Xpost_Op_Func)_drawline, XPOST_DEV_M_LINE },
        { "FillRect", "xcbFillRect", (Xpost_Op_Func)_fillrect, XPOST_DEV_M_RECT },
        { "Emit", "xcbEmit", (Xpost_Op_Func)_emit, XPOST_DEV_M_PAGE },
        { "Flush", "xcbFlush", (Xpost_Op_Func)_flush, XPOST_DEV_M_PAGE },
        { "Destroy", "xcbDestroy", (Xpost_Op_Func)_destroy, XPOST_DEV_M_PAGE }
    };

    Xpost_Object op;
    int ret;

    ret = xpost_dict_put(ctx, classdic, namenativecolorspace, nameDeviceRGB);
    if (ret)
        return ret;

    /* This device's page does not arrive a band at a time. Its pixels go
       to a display server, which holds them, so a band could be sent as
       it was finished (doc/INTERNALS) -- but this driver keeps a buffer
       of the page and writes the whole of it, and what a device states
       about itself is what the machinery above it goes by.

       Taken back out rather than left unsaid. The class is a copy of the
       colour raster class, which says its page may arrive that way, and
       a copy carries what it was copied from -- so a device that has not
       considered the question says yes by inheritance, and the safe
       answer is the one that has to be stated. */
    ret = xpost_dict_undef(ctx, classdic, xpost_name_cons(ctx, "BandedPage"));
    if (ret && ret != undefined)
        return ret;

    /* And this class states nothing about what a row of its raster
       costs, because the raster is not here to cost anything: the
       pixels are a pixmap the display server holds, and a mark reaches
       them as a request rather than as a write into memory this process
       allocated. So what the colour raster class this one is a copy of
       states about its own three-byte planar row is taken back out
       rather than answered on behalf of a raster that is elsewhere. */
    ret = xpost_dev_class_no_rowcost(ctx, classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "xcbCreateCont", (Xpost_Op_Func)_create_cont, 3,
                             integertype, integertype, dicttype);
    _create_cont_opcode = op.mark_.padw;

    ret = xpost_dev_class_install(ctx, classdic, 3, 1,
                                  methods, XPOST_DEV_METHOD_COUNT(methods));
    if (ret)
        return ret;



    /* Paint glyphs without blending their edges. What this device reads
       a pixel back as is the page's ground and not what the window holds
       (GetPix above), so a partly covered edge is composited over the
       ground wherever it falls. Where the page is unmarked the ground is
       the colour under the edge and the edge comes out right; where a
       mark is already laid it is not, and the edge is pulled toward the
       ground instead of toward the ink beneath it. A black glyph on a
       grey panel picks up a fringe lighter than the panel, and white
       text on a black one gets no gradation at all -- white over a white
       ground is white at every coverage -- so the glyph thickens where
       it would have softened.

       The blend also reaches every pixel an edge partly covers, around
       two and a half times the pixels the aliased path lays for a page
       of text, and that is what it costs: ten lines of thirty-point
       text take about twice as long blended as aliased. It used to cost
       more, a colour being negotiated with the display server for every
       pixel of either path; the colour is kept now (_ink above) and the
       negotiation is gone, so what remains is the pixels themselves.

       Declaring one bit of text alpha takes the aliased path, which
       paints through PutPix above. */
    ret = xpost_dict_put(ctx, classdic, xpost_name_cons(ctx, "TextAlphaBits"),
                         xpost_int_cons(1));
    if (ret)
        return ret;








    /* The class and its maker live in the private dictionary, beside the
       classes the boot files define: a program reaches a device through
       the page-device request, and the machinery reaches the class by
       name here. Nothing of the driver's is defined where a program
       could shadow it. */
    ret = xpost_dict_put(ctx, ctx->privatedict,
                         xpost_name_cons(ctx, ".xpost_XCBDEVICE"), classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "newxcbdevice", (Xpost_Op_Func)newxcbdevice, 2, integertype, integertype);
    ret = xpost_dict_put(ctx, ctx->privatedict, xpost_name_cons(ctx, "newxcbdevice"), op);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "xcbEventHandler", (Xpost_Op_Func)_event_handler, 1, dicttype);
    _event_handler_opcode = op.mark_.padw;

    return 0;
}

/*
   install the loadXXXdevice which may be called during graphics initialization
   to produce the operator newXXXdevice which instantiates the device dictionary.
*/
int xpost_oper_init_xcb_device_ops (Xpost_Context *ctx,
                Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

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
    op = xpost_operator_cons(ctx, "loadxcbdevice", (Xpost_Op_Func)loadxcbdevice, 0); INSTALL;
    op = xpost_operator_cons(ctx, "loadxcbdevicecont", (Xpost_Op_Func)loadxcbdevicecont, 1, dicttype);
    _loadxcbdevicecont_opcode = op.mark_.padw;

    return 0;
}
