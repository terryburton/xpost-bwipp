/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_driver.h
 * @brief The contract every output device answers to.
 *
 * A device is a dictionary of methods and a block of instance state held
 * outside virtual memory. This declares what the methods are; a device that
 * keeps a buffer of its own must override every inherited one that would
 * otherwise reach for image data it does not have.
 */

#ifndef XPOST_DEV_DRIVER_H
#define XPOST_DEV_DRIVER_H

#include <limits.h> /* INT_MAX: what a buffer's row width and count are */
#include <math.h> /* the device-space geometry below rounds by floor */
#include <stddef.h> /* a buffer position is counted in the platform's size */
#include <stdio.h> /* FILE: the page a device writes goes to one */
#include <stdlib.h> /* free: a device gives back the raster it allocated */

/*
 * The output-device driver contract.
 *
 * An output device is a PostScript dictionary: a class dictionary whose
 * Create method returns an instance dictionary, on which the graphics
 * pipeline looks methods up by name and executes them with the instance
 * as the topmost operand. The reference implementations are the
 * PostScript base classes in data/ppmimage.ps (DeviceRGB) and
 * data/pgmimage.ps (DeviceGray); data/nulldev.ps is the minimal
 * conforming device. A C device specializes a base class: it copies the
 * class dictionary (.copydict) and replaces method slots with operators.
 *
 * Method slots, with <colour> standing for one number per component of
 * the device's native colour space (see the colour rule below):
 *
 *     width height CLASS  Create    ->  IMAGE     (mandatory)
 *              <colour> x y IMAGE  PutPix    ->  -  (mandatory, raster)
 *                       x y IMAGE  GetPix    ->  <colour>  (optional)
 *      <colour> x1 y1 x2 y2 IMAGE  DrawLine  ->  -  (optional)
 *         <colour> x y w h IMAGE  DrawRect  ->  -  (optional, outline)
 *         <colour> x y w h IMAGE  FillRect  ->  -  (optional)
 *          <colour> polygon IMAGE  FillPoly  ->  -  (optional)
 *                           IMAGE  Emit      ->  -  (mandatory)
 *                           IMAGE  Flush     ->  -  (optional)
 *                     IMAGE  ScreenChanged   ->  -  (optional)
 *                           IMAGE  Destroy   ->  -  (mandatory)
 *                     dict1  .copydict  ->  dict2   (mandatory, class)
 *
 * Optional means the base class supplies a fallback built on PutPix, or
 * the pipeline probes the slot with `known` before calling it (Flush,
 * called by flushpage; ScreenChanged, called where a cell is installed;
 * FillRect and the probe paths in the device contract test). A device
 * that omits PutPix must bring its own implementation of every marking
 * method the pipeline can reach (the vector devices do).
 *
 * ScreenChanged is how a device learns that the screen it paints
 * through has changed, and only a device declaring /ScreenPaint is ever
 * told: the machinery that maintains the cell (.setscreencell,
 * data/gstate.ps) calls it where a cell is installed, which is where a
 * cell is built and so not per mark. A device painting through its own
 * cell has no use for it -- it reads the cell as it paints -- and none
 * of them declares it. What does is a device recording a page to be
 * painted again, for which the screen is state no marking call carries
 * and which would otherwise replay a page under whichever screen its
 * target held by then.
 *
 * A device keeps the marking methods it brings. Selecting a device
 * finishes it (.completedevice, data/device.ps), and part of that
 * finishing is replacing the base class's PostScript fills with the
 * compiled ones; those write the raster as the array of row strings the
 * base classes keep under /ImgData, and paint in the colour spaces they
 * know a component count for, so a device that keeps its pixels in a
 * buffer of its own, or declares another space, is left holding its
 * own. That rule is stated there rather than by the class-install check
 * below: the check reads a class as the driver loads it, and which fill
 * an instance ends up with is settled per instance, after Create, by
 * what the instance turns out to hold.
 *
 * Colour arity rule: the class's /nativecolorspace value determines the
 * component count of every <colour> operand: /DeviceRGB methods take
 * r g b, /DeviceGray methods take a single value. Components are unit
 * range (0..1) and every numeric operand may arrive as integertype or
 * realtype; a device folds them with the xpost_dev_num_* helpers below.
 *
 * Marking geometry: a marking method paints one defined set of pixels
 * for given operands, and that set is stated once, below, rather than
 * by each implementation. A device coordinate names the pixel that
 * contains it (floor). xpost_dev_rect_normalize() with the span
 * clippers is the rectangle FillRect paints -- a negative extent
 * reflects through the origin, and the span is inclusive of the far
 * corner, so an integral w gives w+1 columns. Xpost_Dev_Line is the
 * line DrawLine paints -- the pixels whose centres the segment covers
 * along its major axis, so a run from a to b covers a..b-1 whichever
 * end it is drawn from. The compiled base-class fills reach both
 * through this header, and the PostScript
 * base class reaches them through .rectspan and .linepix, so there is
 * no second statement to drift from.
 *
 * A vector device has no pixels, so it converts: the operands describe
 * an inclusive pixel span and a vector rectangle is half-open, so what
 * it emits is x, y, w+1, h+1 -- the rectangle covering the same pixels
 * a raster device would paint.
 *
 * Destroy must be idempotent: it is called at setpagedevice, at job end,
 * and possibly by the program itself, so it clears each resource handle
 * in the private struct and stores the struct back, making a repeated
 * Destroy a no-op rather than a double free.
 *
 * Destroy releases the buffer but not the instance dictionary, so a
 * destroyed instance stays reachable and its slots stay callable. A
 * program reaches one by calling Destroy itself, and the interpreter
 * reaches one without being asked: setpagedevice retires the outgoing
 * device, and PLRM 6.1 makes the device an element of the graphics state
 * rather than a global fixture, so a saved graphics state still names the
 * retired one and a restore or grestore back past the change makes it
 * current again.
 *
 * Every slot therefore tests the handle it is about to follow instead of
 * assuming Create left one. The recorded width and height are no stand-in
 * for that test: they live in the private struct and outlive the buffer,
 * so an in-range pixel on a released instance passes the bounds check and
 * arrives at the read. A released raster reads as the ground, the same
 * answer a pixel outside the raster gets; it takes no marks; and it emits
 * nothing, its output having been finalised when it was released.
 *
 * The ground is the colour erasepage last cleared the page to, which is
 * grey 1.0 through the transfer function in force (PLRM 8.2) and so is
 * the page's to say rather than white by assumption. The base class
 * records it on the instance as it clears, and every device reads it
 * back through xpost_device_ground_scaled() (xpost_dev_generic.h), or
 * through the xpost_device_ground_channels() face of it where the
 * device's channels are bytes. The record is kept in the range a colour
 * operand arrives in and each device folds it to the channel it stores,
 * so that what a read answers is what that device's own PutPix would
 * have written.
 *
 * It is what a read answers wherever the device holds no pixel to answer
 * from: outside the page, over a row the device does not hold, on a
 * released raster, and at every pixel of a device whose raster is a
 * surface it cannot read back. A read is answered there rather than
 * refused, because a mark aimed there is dropped rather than refused. A
 * device reading back as the ground is a statement about its raster and
 * not about the colour: a page cleared to a light grey reads light.
 *
 * The fold a device applies is the fold its own PutPix applies, and for a
 * device that screens or halftones what it stores that fold depends on
 * the pixel as well as on the colour: what such a device stores for a
 * grey is a pattern repeating over a cell that tiles the device plane.
 * Its ground is answered for the pixel that was asked about, the tiling
 * counted the same way off the page as across it, and one pixel of it is
 * no more the colour than one bit of a byte is -- a cell of it is.
 *
 * Instance state: C-level device state lives in a block outside virtual
 * memory, so it is exempt from `restore` (raster memory is not part of
 * VM, PLRM 3.7.3). What the instance dictionary holds under /Private is
 * a handle on that block, which xpost_handle.c issues and resolves; a
 * device creates one with xpost_handle_cons() and loads and stores its
 * struct through xpost_dev_private_get()/xpost_dev_private_put()
 * below.
 *
 * This header holds only static inline helpers; include it after the
 * standard device includes (xpost_object.h, xpost_memory.h,
 * xpost_context.h, xpost_stack.h, xpost_error.h, xpost_dict.h,
 * xpost_string.h, xpost_name.h, xpost_operator.h and <string.h>).
 */

#include "xpost_handle.h"
#include "xpost_array.h" /* the run of rows a device is asked to hold */
#include "xpost_op_dict.h" /* the width a row is priced at is read off
                              the dictionary stack */

/* A number as an int, truncating toward zero, saturating at the ends of
   the int's range.

   The number is a device coordinate, an extent or a coverage, and each
   of those is arithmetic the program controls: what arrives can be any
   magnitude a real holds, an infinity, or a value that is not a number
   at all. The int has no such values to be, and the machine's own
   conversion is not defined for them either, so each folds to the end of
   the range it lies past -- and one that is not a number folds to the
   bottom, which is a place off every raster there is. Every caller then
   holds an ordinary int to its own extents, which is what the range ends
   are: outside all of them. */
static inline int
xpost_dev_int_of(double v)
{
    if (!(v > (double)INT_MIN)) return INT_MIN;
    if (!(v < (double)INT_MAX)) return INT_MAX;
    return (int)v;
}

/* fold a numeric operand (integertype or realtype) to an int,
   truncating toward zero */
static inline int
xpost_dev_num_to_int(Xpost_Object obj)
{
    return xpost_dev_int_of(xpost_object_number(obj));
}

/* A colour component as a number in [0,1]. The component is clamped
   here, once, because the colour pipeline can hand a device one outside
   the range: setgray and its siblings substitute the nearest valid
   value (PLRM 8.2), but a Separation or DeviceN tint transform is the
   program's own procedure and its result is whatever it computes.
   Unclamped, the scale below wraps the stored channel -- 1.7 lands as
   0.69 of full scale, the byte having run past the top of its range --
   so the ink comes out a different colour rather than the nearest
   one. */
static inline double
xpost_dev_num_to_component(Xpost_Object obj)
{
    double d = xpost_object_number(obj);

    if (d < 0.0) return 0.0;
    if (d > 1.0) return 1.0;
    return d;
}

/* fold a unit-range colour component to the device's integer scale
   (255 for 8-bit channels, 65535 for 16-bit), truncating toward zero */
static inline int
xpost_dev_num_to_scaled(Xpost_Object obj, double scale)
{
    return (int)(xpost_dev_num_to_component(obj) * scale);
}

/* fold a unit-range colour component to an 8-bit channel value */
static inline int
xpost_dev_num_to_byte(Xpost_Object obj)
{
    return xpost_dev_num_to_scaled(obj, 255.0);
}

/* Load the device's private C struct out of the block the handle under
   key in the instance dictionary names. Returns 1 on success and leaves
   the handle in *privatestr for a later put; returns 0 when the value
   under that key is not a handle on a block of this size issued to this
   instance (the caller reports undefined). That case is reachable: the
   instance dictionary is an ordinary dictionary, and what it holds under
   this key is whatever was last stored there. */
static inline int
xpost_dev_private_get(Xpost_Context *ctx,
                      Xpost_Object devdic,
                      Xpost_Object key,
                      Xpost_Object *privatestr,
                      void *priv,
                      size_t size)
{
    void *block;

    *privatestr = xpost_dict_get(ctx, devdic, key);
    block = xpost_handle_block_of(ctx, *privatestr, devdic,
                                  XPOST_HANDLE_DEVICE, size);
    if (!block)
        return 0;
    memcpy(priv, block, size);
    return 1;
}

/* Store the private struct back into the block its handle names.
   Returns 1 on success, 0 when the handle names no such block -- the
   same reachable case xpost_dev_private_get answers, seen from the
   other side, and the device state the caller was about to record is
   then lost. */
static inline XPOST_MUST_CHECK int
xpost_dev_private_put(Xpost_Context *ctx,
                      Xpost_Object privatestr,
                      const void *priv,
                      size_t size)
{
    void *block = xpost_handle_block(ctx, privatestr,
                                     XPOST_HANDLE_DEVICE, size);

    if (!block)
        return 0;
    memcpy(block, priv, size);
    return 1;
}

/*
 * Device-space geometry.
 *
 * A marking method's operands are positions in device space, which may
 * arrive as reals. The pixel a real coordinate names is the pixel
 * containing it -- floor, not truncation toward zero. The two agree
 * everywhere except across the origin, where truncation maps both -0.5
 * and 0.5 onto pixel 0 and never onto pixel -1, so a shape crossing the
 * origin gains a doubled column. Floor is also what the PostScript
 * classes' .to-int has always done, so the compiled and interpreted
 * methods land on the same pixels.
 */
static inline int
xpost_dev_pixel(double v)
{
    return xpost_dev_int_of(floor(v));
}

/* The rectangle FillRect paints, as an inclusive pixel span: a negative
   extent reflects the rectangle through its origin (w < 0 means the
   rectangle spans x-|w|..x), and the span runs from the pixel holding
   (x, y) to the pixel holding (x+w, y+h) -- so an integral w gives w+1
   columns. Unclipped: the clip source differs between devices (a fixed
   framebuffer, or a row array whose rows carry their own lengths), so
   the caller states it through the span clippers below. */
static inline void
xpost_dev_rect_normalize(double x, double y, double w, double h,
                         int *x0, int *y0, int *x1, int *y1)
{
    if (w < 0) { w = -w; x -= w; }
    if (h < 0) { h = -h; y -= h; }
    *x0 = xpost_dev_pixel(x);
    *y0 = xpost_dev_pixel(y);
    *x1 = xpost_dev_pixel(x + w);
    *y1 = xpost_dev_pixel(y + h);
}

/* Clip an inclusive span to [0, extent-1]. Returns 1 when something
   survives, 0 when the span lies wholly outside. An extent counts the
   pixels the device holds along the axis, so one that counts none holds
   no span to survive into and is answered before the last pixel is
   worked out -- the pixel below the count, which the bottom of the int's
   range has nothing below. */
static inline int
xpost_dev_span_clip(int *lo, int *hi, int extent)
{
    if (extent <= 0) return 0;
    if (*lo < 0) *lo = 0;
    if (*hi > extent - 1) *hi = extent - 1;
    return *lo <= *hi;
}

/* Clip an inclusive rectangle to a device of the given size: the clip
   source for every device holding one rectangular framebuffer. */
static inline int
xpost_dev_rect_clip(int *x0, int *y0, int *x1, int *y1,
                    int width, int height)
{
    return xpost_dev_span_clip(x0, x1, width)
         & xpost_dev_span_clip(y0, y1, height);
}

/*
 * Where a pixel sits in the block of memory a device is holding.
 *
 * Two extents are in play wherever a device reaches into its own
 * memory, and they answer different questions. The page is what the
 * program asked for: its extent comes from the page size and is what
 * the marking operators are clipped against. The buffer is what is
 * resident and being indexed: its extent is the block's own, and the
 * arithmetic below is about that one. A device that holds its whole
 * page at once gives the two the same numbers, which is every device
 * here; they are still separate questions, and a position computed
 * from the page's extent against a block holding something else
 * reaches memory the block does not have.
 *
 * The position is counted in the width the platform expresses the size
 * of a block in, not the width the interpreter counts pixels in. Those
 * differ, and the smaller of them is the one an int holds: a block with
 * more pixels than an int counts is a block a device can be given and
 * would then reach past. So the count is taken in size_t throughout,
 * and what a device can hold is settled by what it can allocate and
 * address rather than by the range of a pixel coordinate.
 */
typedef size_t Xpost_Dev_Raster_Offset;

/* The page extent @p v as a buffer extent, or zero for a page no buffer
   here carries. A page size is a number the program chose and arrives as
   an interpreter integer; a buffer's row width and row count are ints,
   which is what the row arithmetic above takes. Where the interpreter
   counts further than an int does, a program can name a page whose
   extent no buffer geometry here holds, and narrowing it in silence
   builds a buffer of some other page: the number the device dictionary
   reports and the number the memory is laid out in would then differ by
   a multiple of what an int counts, and every mark would land somewhere
   other than where the program put it. The caller answers limitcheck,
   PLRM 8.2's name for a limit of the implementation. */
static inline int
xpost_dev_buffer_extent(integer v, int *extent)
{
    /* The bound is taken in a type wider than either of them, so that a
       build whose interpreter integer is itself an int is not asking
       whether a value exceeds the largest one its own type can hold --
       a question with one answer, which a compiler is right to refuse. */
    long long w = (long long)v;

    if (w < 0 || w > (long long)INT_MAX)
        return 0;
    *extent = (int)w;
    return 1;
}

/* Both page extents at once, refused as one. Every device that holds a
   page in a buffer asks the same question of the width and the height
   together and answers limitcheck for either, so the refusal is
   reported here rather than in each of them -- and reported the same
   way, naming the page the program asked for. Answers 1 where both
   carry and 0, having said so, where either does not. */
static inline int
xpost_dev_page_extent(integer w, integer h, int *width, int *height)
{
    if (xpost_dev_buffer_extent(w, width)
     && xpost_dev_buffer_extent(h, height))
        return 1;
    XPOST_LOG_ERR("%d a page of %ldx%ld names an extent no raster carries",
                  limitcheck, (long)w, (long)h);
    return 0;
}

/* The pixel at column @p x of row @p y of a buffer @p stride pixels
   wide, as a count of pixels from the buffer's first. The coordinates
   are the buffer's own, and the caller has already held them inside it
   -- every marking method tests its operands against the extent before
   reaching the memory, so this does not test them again. */
static inline Xpost_Dev_Raster_Offset
xpost_dev_raster_offset(int x, int y, int stride)
{
    return (Xpost_Dev_Raster_Offset)y * (Xpost_Dev_Raster_Offset)stride
         + (Xpost_Dev_Raster_Offset)x;
}

/*
 * The run of the page's rows a device with a buffer of its own holds.
 *
 * A raster costs width times height whatever is painted on it, and what
 * bounds that is holding one run of the page's rows at a time and
 * putting each run out as it is finished (doc/INTERNALS). The run a
 * device is to start on is left for it in the graphics dictionary under
 * /.rasterband, which Create takes; from then on the buffer stands for
 * one run after another and a mark aimed at a row outside the run is
 * dropped where a mark off the page is dropped.
 *
 * Four numbers rather than one, because they answer different
 * questions and a device that conflates any two of them is wrong about
 * a page:
 *
 *   top, rows   the run of the page this device is holding now. It
 *               moves. Whether a mark lands is asked of this.
 *   bufrows     the rows the buffer has. It does not move, and it is
 *               what the bound is a bound on.
 *   origin      the page row the buffer's first row stands for. It is
 *               the top of the run for a buffer smaller than the page,
 *               and the top of the page for one holding all of it --
 *               a device may be asked for a run while holding every
 *               row, and then the run says which marks it takes and
 *               not where they go.
 *
 * The stream a page is written to belongs to the page rather than to
 * the device, and a page arriving a band at a time outlives every run
 * the device stands for while it is written, so where the file has
 * reached is kept here too rather than beside the rows.
 */
typedef struct
{
    int top;
    int rows;
    int bufrows;
    int origin;
    int next;                /* the page row the file has been given up to */
    unsigned int whole : 1;  /* the buffer has every row of the page */
    unsigned int primed : 1; /* the rows no run will reach carry the ground */
    unsigned int open : 1;   /* a stream has been started for this page */
    unsigned int done : 1;   /* the page has been finished */
} Xpost_Dev_Band;

/**
 * @brief What a device does with a page besides encode it.
 *
 * Two devices that write a page to a file differ in how they encode it
 * and in nothing else: the same questions are asked in the same order
 * about whether a page is arriving in bands, when the file is opened,
 * which rows this call may give it, and when the page is finished and
 * the raster handed on. Those questions are asked once, in
 * xpost_dev_page_emit below, and a device answers this for the parts
 * that are its own.
 *
 * Every call is handed the device's own instance state, which the
 * device knows the shape of and this does not.
 */
typedef struct
{
    /** Begin a page: what the device resets when a page is its own. */
    void (*page_begin)(void *priv);
    /** Give up a stream left open by a page that stopped arriving. */
    void (*stream_drop)(void *priv);
    /** Open the encoder over the file just opened for this page. */
    int (*stream_open)(Xpost_Context *ctx, Xpost_Object devdic, void *priv);
    /** Give the encoder the page's rows as far as @p last. */
    int (*write_rows)(void *priv, int last);
    /** Finish the page: the encoder writes what it was holding. */
    int (*stream_finish)(void *priv);
    /**
     * Whether this call must hold its rows rather than write them.
     * An image written in several passes over the page cannot give a
     * row up before the last band has been painted. NULL where the
     * device never holds rows back, which is the usual case.
     */
    int (*defers_rows)(const void *priv);
    /** Give up what the instance holds: the stream, the file, the raster. */
    void (*reclaim)(void *priv);

    /*
     * Where the page state a device keeps lives inside the instance.
     *
     * The five below are how a device answers for the shape of its own
     * structure, which is the one thing about emitting a page that
     * cannot be written once: the fields are the same fields, and they
     * sit at different offsets behind different types. Everything done
     * WITH them -- fetching the instance, standing down where the
     * raster was released, emitting, noting the hand-off, and recording
     * the outcome whether the page was written or refused -- is
     * xpost_dev_page_emit_call and is written once.
     *
     * A device that does not use that entry point may leave them null.
     */

    /** The instance's raster, or null where it has been released. */
    unsigned char *(*raster)(void *priv);
    /** The band state the page is being written across. */
    Xpost_Dev_Band *(*band)(void *priv);
    /** Where the file this page is being written to is kept. */
    FILE **(*file)(void *priv);
    /** The page's height in rows. */
    int (*height)(const void *priv);
    /** Note the raster as handed on and no longer the instance's. */
    void (*disown)(void *priv);
    /** Lay the ground over the rows no run of them is going to reach. */
    void (*prime)(Xpost_Context *ctx, Xpost_Object devdic, void *priv);
    /** Leave a run of rows as a raster fresh from Create would be. */
    void (*clear)(void *priv, int from, int to);
} Xpost_Dev_Page_Codec;

/**
 * @brief Write as much of a page as this call can, and say what became
 *        of the raster.
 *
 * The page arrives whole or a band at a time, and which it is, is what
 * /.bandpage on the device says (data/image.ps): a device carrying none
 * takes each call as a page of its own, so one cannot be handed part of
 * a page by accident, and a job's second page is a second file. A page
 * arriving in bands is finished once; a further call for the same page
 * has nothing left to give.
 *
 * The file is opened when the first of a page's rows is ready for it,
 * because the name is settled per page, and goes back when the page is
 * finished or refused -- a page still being written keeps it for the
 * call that brings the next band.
 *
 * @param[in] ctx The context.
 * @param[in] devdic The device instance.
 * @param[in] priv The device's own instance state, handed to the codec.
 * @param[in,out] band The band state this page is being written across.
 * @param[in,out] file The file the page is written to.
 * @param[in] height The page's height in rows.
 * @param[in] raster The page's rows, for the handoff, or NULL for a
 *                   device whose raster is not the client's to take.
 * @param[in] codec What the device does that this does not.
 * @param[out] handed_off Set where the raster became the client's, which
 *                        the caller answers for -- it holds the flag.
 * @return 0, or what the writing came to.
 */
/**
 * @brief Retire an instance: finish the page it was writing, then give
 *        up what it holds.
 *
 * A page still being written is finished rather than left truncated. The
 * rows it was never given are the ones the device is not holding, which
 * the ground stands for, and they are what the call at the end of a band
 * loop would have written -- so a device retired part way through a page,
 * by an error or by a restore past the setpagedevice that installed it,
 * leaves an image a reader can open.
 *
 * What follows is the release the collector runs, which is why it is the
 * codec's and stated once: a change to what a device owns cannot reach
 * one path and not the other. Finishing the page is the difference
 * between the two, and it is here because the collector cannot write
 * rows.
 *
 * @param[in] priv The device's own instance state.
 * @param[in,out] band The band state the page was being written across.
 * @param[in] has_raster Whether there are rows to finish the page from.
 * @param[in] height The page's height in rows.
 * @param[in] codec What the device does that this does not.
 */
void xpost_dev_page_retire(void *priv, Xpost_Dev_Band *band, int has_raster,
                           int height, const Xpost_Dev_Page_Codec *codec);

int xpost_dev_page_emit(Xpost_Context *ctx, Xpost_Object devdic,
                        void *priv, Xpost_Dev_Band *band, FILE **file,
                        int height, unsigned char *raster,
                        const Xpost_Dev_Page_Codec *codec,
                        int *handed_off);

/**
 * @brief An Emit method entire, for a device whose codec answers for
 *        the shape of its instance.
 *
 * Fetches the instance, stands down where the raster has been released,
 * emits what this call can of the page, notes a raster handed on, and
 * records the instance again -- whether the page was written or
 * refused, since a page that could not be written is one the device
 * must not go on writing.
 *
 * @param[in] nameprivate the name the instance is kept under
 * @param[in] priv storage for the instance, the device's own shape
 * @param[in] privsz its size
 * @param[in] codec what the device does that this does not, including
 *                  the five accessors that reach the page state
 */
int xpost_dev_page_emit_call(Xpost_Context *ctx, Xpost_Object devdic,
                             Xpost_Object nameprivate,
                             void *priv, size_t privsz,
                             const Xpost_Dev_Page_Codec *codec);

/**
 * @brief A MoveBand method entire, for such a device.
 *
 * Moves the raster onto another run of the page's rows: where a page
 * has finished, the run in front of it is the next page's and begins
 * one; where the device holds every row, the ground is laid over the
 * rows no run will reach; and either way the run about to take marks is
 * left as a raster fresh from Create would be, since a row still
 * carrying the run before's ink would show it wherever this run paints
 * nothing.
 */
int xpost_dev_page_moveband_call(Xpost_Context *ctx, Xpost_Object devdic,
                                 Xpost_Object nameprivate,
                                 void *priv, size_t privsz,
                                 Xpost_Object top, Xpost_Object rows,
                                 const Xpost_Dev_Page_Codec *codec);


/**
 * @brief A Destroy method entire, for such a device.
 *
 * Whether there are rows and how tall the page is are read BEFORE the
 * retirement is asked for: what retiring is handed is the answer, not
 * the raster, and it may be the thing that lets the raster go.
 */
int xpost_dev_page_destroy_call(Xpost_Context *ctx, Xpost_Object devdic,
                                Xpost_Object nameprivate,
                                void *priv, size_t privsz,
                                const Xpost_Dev_Page_Codec *codec);

/**
 * @brief Put the band state back to where a page begins.
 *
 * Four flags, and which four is the whole of it: a page that began
 * without clearing @c primed would never have its ground laid again,
 * and would show the page before's wherever the new one paints nothing.
 * There is nothing device-specific here, which is why it is here.
 */
/**
 * @brief An Emit method entire, for a device that LENDS its raster.
 *
 * The devices that hand their page to an embedding program do not write
 * a file and carry no codec: emitting is offering the raster, and where
 * the offer is taken the instance stops owning it. That sequence is the
 * whole of their Emit and it is the same sequence in each of them.
 *
 * The order is the part that must not be written twice. An instance
 * that recorded before the offer would go on believing it owns a raster
 * the embedder now frees; one that recorded nothing after a taken offer
 * would free the embedder's memory at Destroy.
 *
 * @param raster the instance's raster, or null where it has none
 * @param disown note the raster as no longer the instance's
 */
int xpost_dev_buffer_emit_call(Xpost_Context *ctx, Xpost_Object devdic,
                               Xpost_Object nameprivate,
                               void *priv, size_t privsz,
                               unsigned char *(*raster)(void *),
                               void (*disown)(void *));

/**
 * @brief Define the two accessors such a device answers with.
 *
 * @param P the device's own instance type
 *
 * Fewer than a file-writing device needs, because there is no file, no
 * band and no ground to reach -- only the raster and whether it is
 * still the instance's.
 */
#define XPOST_DEV_BUFFER_ACCESSORS(P)                                   \
    static unsigned char *_raster_of(void *p)                           \
    {                                                                   \
        P *d = p;                                                       \
                                                                        \
        return d->buf ? (unsigned char *)d->buf->data : NULL;           \
    }                                                                   \
    static void _disown(void *p)                                        \
    {                                                                   \
        ((P *)p)->bufowned = 0;                                         \
    }

void xpost_dev_band_page_begin(Xpost_Dev_Band *band);

/**
 * @brief Hold a run of rows to the raster the device actually has.
 *
 * Clamps @p from and @p to onto the buffer and answers whether anything
 * is left. Three bounds and an emptiness test, none of them obvious
 * enough to be worth writing twice.
 *
 * @return nonzero where the run has rows in it
 */
int xpost_dev_band_clamp_rows(const Xpost_Dev_Band *band,
                              int *from, int *to);

/**
 * @brief Define the five accessors a device's codec answers with.
 *
 * @param P the device's own instance type
 *
 * They are one line each and the same line in every device, differing
 * only in the type behind them, so they are written here rather than
 * once per device -- which is what they were, and what made a device
 * carrying them cost more text than the body they were introduced to
 * share.
 *
 * A device using this holds its raster in @c buf (null once released,
 * with the pixels at @c buf->data), the band state in @c band, the file
 * in @c file, the page's height in @c height, and whether the raster is
 * still its own in @c bufowned. A device whose instance is not that
 * shape writes its own five and does not use this.
 */
#define XPOST_DEV_PAGE_ACCESSORS(P)                                     \
    static unsigned char *_raster_of(void *p)                           \
    {                                                                   \
        P *d = p;                                                       \
                                                                        \
        return d->buf ? (unsigned char *)d->buf->data : NULL;           \
    }                                                                   \
    static Xpost_Dev_Band *_band_of(void *p)                            \
    {                                                                   \
        return &((P *)p)->band;                                         \
    }                                                                   \
    static FILE **_file_of(void *p)                                     \
    {                                                                   \
        return &((P *)p)->file;                                         \
    }                                                                   \
    static int _height_of(const void *p)                                \
    {                                                                   \
        return ((const P *)p)->height;                                  \
    }                                                                   \
    static void _disown(void *p)                                        \
    {                                                                   \
        ((P *)p)->bufowned = 0;                                         \
    }                                                                   \
    static void _page_begin(void *p)                                    \
    {                                                                   \
        xpost_dev_band_page_begin(&((P *)p)->band);                     \
    }                                                                   \
    static void _prime_of(Xpost_Context *c, Xpost_Object d, void *p)    \
    {                                                                   \
        _prime(c, d, (P *)p);                                           \
    }                                                                   \
    static void _clear_of(void *p, int from, int to)                    \
    {                                                                   \
        _clear((P *)p, from, to);                                       \
    }


/*
 * State on the device dictionary the run of the page's rows this device
 * is taking marks for, under the two names every device holding a run
 * states it by (.bandtop and .bandrows, data/image.ps).
 *
 * It is the answer to one question -- which of the page's rows a mark
 * may land on -- and the fill pipeline asks it of the dictionary before
 * converting a shape, so that a shape crossing the page is converted
 * over this run and not over the whole of it. A device that says
 * nothing is asked for nothing and has its shapes converted whole,
 * which is what a device holding its page some other way wants.
 *
 * Written where the run is set and nowhere else, so the two cannot come
 * apart: the run is what xpost_dev_band_move() assigns, and this is the
 * tail of it.
 *
 * A pair that cannot be written is left unwritten rather than half
 * written, and a run with no dictionary to be stated on is not stated.
 * The entries are how a conversion is bounded and not where the pixels
 * are -- every marking method asks xpost_dev_band_row() -- so a device
 * with neither of them paints the same page more slowly, while one
 * carrying a run it has since left would have marks dropped that it
 * does hold.
 */
static inline void
xpost_dev_band_publish(Xpost_Context *ctx, Xpost_Object devdic,
                       const Xpost_Dev_Band *b)
{
    Xpost_Object top, rows;

    if (xpost_object_get_type(devdic) != dicttype)
        return;

    top = xpost_name_cons(ctx, ".bandtop");
    rows = xpost_name_cons(ctx, ".bandrows");

    if (xpost_dict_put(ctx, devdic, top, xpost_int_cons((integer)b->top))
        || xpost_dict_put(ctx, devdic, rows, xpost_int_cons((integer)b->rows)))
    {
        (void)xpost_dict_undef(ctx, devdic, top);
        (void)xpost_dict_undef(ctx, devdic, rows);
    }
}

/* Hold a run to the page there is and to the buffer there is, and say
   where the buffer's first row sits, and state the run the device is
   left standing on. A run naming rows the page does not have, or more
   rows than the buffer holds, is cut to what it can be rather than
   followed off the end of either -- and it is the cut run that is
   stated, this being where a device says what it holds rather than what
   it was offered. */
static inline void
xpost_dev_band_move(Xpost_Context *ctx, Xpost_Object devdic,
                    Xpost_Dev_Band *b, int height, int top, int rows)
{
    if (top < 0 || top > height)
        top = 0;
    if (rows < 0)
        rows = 0;
    if (top + rows > height)
        rows = height - top;
    if (rows > b->bufrows)
        rows = b->bufrows;
    b->top = top;
    b->rows = rows;
    b->whole = b->bufrows >= height;
    b->origin = b->whole ? 0 : top;
    xpost_dev_band_publish(ctx, devdic, b);
}

/* The buffer row page row @p y is held in, or -1 for a row this device
   is not holding. Every marking method asks this before it reaches the
   memory: a row outside the run has no storage here, whether because
   the buffer is smaller than the page or because the device has been
   asked to take marks for part of it. */
static inline int
xpost_dev_band_row(const Xpost_Dev_Band *b, int y)
{
    if (y < b->top || y >= b->top + b->rows)
        return -1;
    return y - b->origin;
}

/* The buffer row page row @p y is stored in, or -1 where the buffer has
   no row for it.
 *
 * This is the other of the two readings, and an emission wants this one:
 * which of the page's rows there are pixels to write. A buffer holding
 * every row of the page has them for every row, whatever run of them the
 * device is taking marks for at this moment -- the runs before this one
 * painted into the same buffer and their rows are still there. A buffer
 * the size of a band has them only for the run it is standing on, every
 * other row of the page having been given up or not yet reached.
 *
 * xpost_dev_band_row() is the reading a marking method wants: where a
 * mark may land. The two differ exactly where a device holds more of the
 * page than it is being offered marks for, and writing the file from the
 * marking method's reading would put the ground over every row but the
 * band in hand.
 */
static inline int
xpost_dev_band_stored(const Xpost_Dev_Band *b, int y)
{
    if (b->whole)
        return (y >= 0 && y < b->bufrows) ? y : -1;
    return xpost_dev_band_row(b, y);
}

/* Cut a run of the page's rows to the ones this device is holding,
   answering zero where none of it is held. It is the row half of what
   xpost_dev_rect_clip() does against the page, applied after it: a
   rectangle is held to the page first and to the rows there are for it
   second. */
static inline int
xpost_dev_band_clip(const Xpost_Dev_Band *b, int *y0, int *y1)
{
    if (*y0 < b->top)
        *y0 = b->top;
    if (*y1 > b->top + b->rows - 1)
        *y1 = b->top + b->rows - 1;
    return *y0 <= *y1;
}

/*
 * Take the run of rows the device about to be made is asked to hold.
 *
 * The run is left in the graphics dictionary by whatever is making the
 * device -- the band loop that puts a page out a run at a time, and the
 * page-device request that carries an imaging bounding box, which PLRM
 * 6.2 lets a device answer by holding no more of the page than the box
 * reaches. A device asked for nothing holds the page, which is what a
 * device that has not been asked expects.
 *
 * Taken, and not merely read: a device that holds its page some other
 * way never asks, and the next device made must not inherit a run that
 * was not asked of it.
 *
 * @p wholepage says this device cannot give a row up before the page is
 * complete -- because its writer goes over the page more than once, or
 * because the raster is one an embedder asked for and holds. Such a
 * device is still told which rows to take marks for, since that is what
 * a caller playing a page back band by band relies on, but it holds
 * every row of the page and banding it bounds nothing. Saying so here
 * is what keeps the two questions apart: which rows a mark may land on,
 * and how many rows there are to land in.
 *
 * The run taken is stated on @p devdic as it is set, that being where
 * the fill pipeline reads which rows to convert a shape over.
 */
static inline void
xpost_dev_band_take(Xpost_Context *ctx, Xpost_Object devdic,
                    int height, int wholepage, Xpost_Dev_Band *b)
{
    Xpost_Object gd, run, key;
    int top = 0, rows = height;

    b->bufrows = height;
    b->next = 0;
    b->primed = 0;
    b->open = 0;
    b->done = 0;

    gd = xpost_dict_get(ctx, ctx->privatedict,
                        xpost_name_cons(ctx, ".graphicsdict"));
    key = xpost_name_cons(ctx, ".rasterband");
    if (xpost_object_get_type(gd) == dicttype)
    {
        run = xpost_dict_get(ctx, gd, key);
        if (xpost_object_get_type(run) == arraytype && run.comp_.sz >= 2)
        {
            top = (int)xpost_object_number(xpost_array_get(ctx, run, 0));
            rows = (int)xpost_object_number(xpost_array_get(ctx, run, 1));
            if (top < 0 || top >= height)
                top = 0;
            if (rows < 1)
                rows = 1;
            if (top + rows > height)
                rows = height - top;
            if (!wholepage)
                b->bufrows = rows;
            (void)xpost_dict_undef(ctx, gd, key);
        }
    }
    xpost_dev_band_move(ctx, devdic, b, height, top, rows);
}

/*
 * The pixels DrawLine paints, walked one at a time.
 *
 * DrawLine paints the pixels whose centres the segment covers along its
 * major axis: for each integer coordinate c on that axis between the
 * two endpoints' pixels, the segment is sampled where it crosses
 * c + 0.5, and the pixel it passes through there is painted when that
 * crossing lies within the segment.
 *
 * Two consequences worth stating, because implementations that walk
 * endpoints rather than centres have neither. The set does not depend
 * on which end the segment is drawn from -- reversing the operands
 * reverses the order and nothing else. And two collinear segments
 * meeting at a shared endpoint continue each other exactly, painting it
 * once, wherever within a pixel that endpoint falls; a horizontal run
 * from a to b with a < b integral therefore covers a..b-1, which is why
 * the scanline filler can hand a fill span to either DrawLine or
 * FillRect and get the same row.
 *
 * A segment too short to reach any centre still marks the pixel holding
 * its midpoint, so nothing a program draws vanishes.
 *
 * Endpoints are first quantised to the 1/256 device grid the fill
 * pipeline works on: an endpoint meant to sit on a pixel boundary
 * arrives carrying accumulated float noise, and the walk floors it.
 *
 * The walk does not clip. Which pixels exist is the device's business,
 * and every caller already rejects a pixel outside its raster; clipping
 * the segment first would give the same set anyway, since the sample
 * positions do not move.
 *
 *     Xpost_Dev_Line l;
 *     int px, py;
 *     xpost_dev_line_init(&l, x1, y1, x2, y2);
 *     while (xpost_dev_line_next(&l, &px, &py))
 *         plot(px, py);
 */
typedef struct
{
    double a1, b1, da, db;   /* major axis a, minor axis b */
    int major_is_x;
    int c, cend, step;
    int painted;
    int degenerate;          /* no extent at all: one pixel, then done */
    int done;
} Xpost_Dev_Line;

static inline double
xpost_dev_line_quantize(double v)
{
    return floor(v * 256.0 + 0.5) / 256.0;
}

static inline void
xpost_dev_line_init(Xpost_Dev_Line *l,
                    double x1, double y1, double x2, double y2)
{
    double dx, dy;

    x1 = xpost_dev_line_quantize(x1);
    y1 = xpost_dev_line_quantize(y1);
    x2 = xpost_dev_line_quantize(x2);
    y2 = xpost_dev_line_quantize(y2);
    dx = x2 - x1;
    dy = y2 - y1;

    l->painted = 0;
    l->done = 0;
    l->degenerate = (dx == 0.0 && dy == 0.0);
    if (l->degenerate)
    {
        /* one pixel, the one holding the point */
        l->major_is_x = 1;
        l->a1 = x1; l->b1 = y1; l->da = 0.0; l->db = 0.0;
        l->c = xpost_dev_pixel(x1);
        l->cend = l->c;
        l->step = 1;
        return;
    }

    l->major_is_x = (fabs(dx) >= fabs(dy));
    if (l->major_is_x)
    {
        l->a1 = x1; l->da = dx;
        l->b1 = y1; l->db = dy;
    }
    else
    {
        l->a1 = y1; l->da = dy;
        l->b1 = x1; l->db = dx;
    }
    l->c = xpost_dev_pixel(l->a1);
    l->cend = xpost_dev_pixel(l->a1 + l->da);
    l->step = (l->da < 0.0) ? -1 : 1;
}

/* Hold the walk to a device `dim` pixels wide on the major axis. The
   pixel a step yields is a pure function of its major coordinate, so
   the coordinates that land off the device -- which the caller drops
   anyway -- can be skipped before they are walked rather than after,
   which bounds the walk by the device instead of by how far the
   segment was drawn. The minor axis needs no bound of its own: the
   major axis is the longer, so a major run held to `dim` holds the
   minor within `dim` of its start as well. A segment lying wholly to
   one side of the axis leaves an empty range and walks out at once. */
static inline void
xpost_dev_line_clip_major(Xpost_Dev_Line *l, int dim)
{
    if (l->degenerate)
        return;
    if (l->step > 0)
    {
        if (l->c < 0) l->c = 0;
        if (l->cend > dim - 1) l->cend = dim - 1;
    }
    else
    {
        if (l->c > dim - 1) l->c = dim - 1;
        if (l->cend < 0) l->cend = 0;
    }
}

/* The next pixel, or 0 when the segment is walked out. */
static inline int
xpost_dev_line_next(Xpost_Dev_Line *l, int *px, int *py)
{
    if (l->done)
        return 0;

    if (l->degenerate)
    {
        l->done = 1;
        *px = xpost_dev_pixel(l->a1);
        *py = xpost_dev_pixel(l->b1);
        return 1;
    }

    while ((l->step > 0) ? (l->c <= l->cend) : (l->c >= l->cend))
    {
        int c = l->c;
        double t = (c + 0.5 - l->a1) / l->da;

        l->c += l->step;
        if (t >= 0.0 && t <= 1.0)
        {
            double b = l->b1 + l->db * t;

            l->painted = 1;
            if (l->major_is_x) { *px = c; *py = xpost_dev_pixel(b); }
            else               { *px = xpost_dev_pixel(b); *py = c; }
            return 1;
        }
    }

    l->done = 1;
    if (!l->painted)
    {
        /* too short to reach a centre: the midpoint's pixel */
        double a = l->a1 + l->da / 2.0;
        double b = l->b1 + l->db / 2.0;

        l->painted = 1;
        if (l->major_is_x) { *px = xpost_dev_pixel(a); *py = xpost_dev_pixel(b); }
        else               { *px = xpost_dev_pixel(b); *py = xpost_dev_pixel(a); }
        return 1;
    }
    return 0;
}

/*
 * A device's method suite, as data.
 *
 * Every C device installs the same shapes into its class dictionary,
 * and every one of them used to write out each installation by hand:
 * the operator name, the function, the result count, the operand count
 * and one type per operand, then a put whose refusal it had to remember
 * to check. Five of six answered success from a failed PutPix
 * registration, so a device loaded with no PutPix and failed at its
 * first paint.
 *
 * A method's arity is not a free choice: it follows from what the slot
 * is and from the device's declared colour space, since <colour> stands
 * for one operand per component. So the table states the slot, the
 * function and the kind, and the arity is derived. A device cannot
 * declare an arity that disagrees with its colour space, because it
 * does not declare one.
 *
 * xpost_dev_class_install() registers a table and then checks what it
 * produced: every mandatory slot filled, and -- for a device whose
 * raster is a buffer of its own rather than the base class's row array
 * -- every slot that would reach for that row array named by the table
 * that brings Create. A device that fails either does not load, rather
 * than loading with a method that answers undefined the first time the
 * pipeline reaches it.
 */
typedef enum
{
    XPOST_DEV_M_CREATE,   /*        width height CLASS  ->  IMAGE   */
    XPOST_DEV_M_PUTPIX,   /*      <colour> x y IMAGE  ->  -         */
    XPOST_DEV_M_GETPIX,   /*               x y IMAGE  ->  <colour>  */
    XPOST_DEV_M_LINE,     /* <colour> x1 y1 x2 y2 IMAGE  ->  -      */
    XPOST_DEV_M_RECT,     /*    <colour> x y w h IMAGE  ->  -       */
    XPOST_DEV_M_BLEND,    /*  <colour> cov x y IMAGE  ->  -         */
    XPOST_DEV_M_POLY,     /*     <colour> polygon IMAGE  ->  -      */
    XPOST_DEV_M_BAND,     /*          top rows IMAGE  ->  -         */
    XPOST_DEV_M_PAGE      /*                     IMAGE  ->  -       */
} Xpost_Dev_Method_Kind;

typedef struct
{
    const char *slot;    /* the name the pipeline looks up */
    const char *opname;  /* the operator's own name, for the register */
    Xpost_Op_Func func;
    Xpost_Dev_Method_Kind kind;
} Xpost_Dev_Method;

#define XPOST_DEV_METHOD_COUNT(t) ((int)(sizeof(t) / sizeof(*(t))))

/* The slots whose class value reads the raster held as PostScript row
   arrays -- the base class's own method bodies, and the compiled
   operators a class stores in a slot in place of one, which read the
   same row array from the instance dictionary. A device that brings its
   own buffer must override every one of them: what it inherits instead
   reads a name its instance does not carry and answers undefined --
   present, callable and broken, which no probe with `known` can tell
   from working. GetPix was inherited unoverridden by five devices for
   exactly that reason. tests/check-device-skeleton.sh holds this list
   to the classes, and holds every device that keeps its own raster to
   naming all of it in its method table. */
#define XPOST_DEV_RASTER_SLOTS { "Create", "PutPix", "GetPix", "BlendPix", "Emit" }

/* Slots no device may be without, whatever it keeps its raster in. */
#define XPOST_DEV_MANDATORY_SLOTS { "Create", "Emit", "Destroy", ".copydict" }

/* Register a method table into a class dictionary and check the result.
   ncomp is the component count of the device's declared colour space.
   raster_is_compiled says the device keeps its pixels in a buffer of
   its own, so the base class's row-array methods cannot serve it.
   Returns 0, or the error that stopped it -- on the first failure, so a
   device that could not be completed does not load.

   This one is declared here and defined in xpost_dev_driver.c rather
   than written into every device that calls it. It runs once for each
   table a device registers, at the moment the device loads, and what it
   spends is a name lookup and a dictionary store per slot -- so a copy
   of its body in each of the eight callers would buy nothing a call
   does not, and the rest of this header is the per-mark half of the
   contract, where a copy does buy something. */
XPOST_MUST_CHECK int
xpost_dev_class_install(Xpost_Context *ctx,
                        Xpost_Object classdic,
                        int ncomp,
                        int raster_is_compiled,
                        const Xpost_Dev_Method *methods,
                        int nmethods);

/* What one row of this device's raster costs, as the elements and the
   bytes one row takes: the pair a class answers under /.rowcost
   (data/ppmimage.ps), and the pair the band loop divides its byte
   budget by to arrive at a band height (data/recorddev.ps).

   The body of such a method, for a device that keeps its raster in a
   buffer of its own: @p bytesperpixel is what one pixel of that buffer
   occupies, and the elements are none, since the buffer takes nothing
   from the memory the interpreter allocates rows out of.

   The width a row is being priced at is not an operand. It is read off
   the dictionary stack, where a caller states it, which is how the
   PostScript classes' own bodies read it -- so a class of either kind
   answers the same call. A width whose row runs past what an integer
   counts answers a real, as the multiply in those bodies does; the band
   loop takes such an answer as a row no band holds one of. */
static inline int
xpost_dev_rowcost(Xpost_Context *ctx,
                  int bytesperpixel)
{
    Xpost_Object w;
    double bytes;
    int ret;

    ret = xpost_op_any_load(ctx, xpost_name_cons(ctx, "width"));
    if (ret)
        return ret;
    w = xpost_stack_pop(ctx->lo, ctx->os);
    if (xpost_object_get_type(w) != integertype &&
        xpost_object_get_type(w) != realtype)
        return typecheck;

    bytes = xpost_object_number(w) * (double)bytesperpixel;
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(0)))
        return stackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          (bytes > (double)INT_MAX || bytes < -(double)INT_MAX)
                          ? xpost_real_cons((real)bytes)
                          : xpost_int_cons((integer)bytes)))
        return stackoverflow;
    return 0;
}

/* State what a row of this device's raster costs, by installing @p func
   -- a body built on xpost_dev_rowcost() above -- in the class's
   /.rowcost slot under the operator name @p opname.

   Said rather than inherited, for the reason /BandedPage is said rather
   than inherited. The class is a copy of a PostScript raster class,
   which prices the row it allocates, and a copy carries what it was
   copied from -- so a device whose raster is a different shape would
   answer for the shape it was copied from, and a budget divided by that
   buys a band of some other number of rows than the one it paid for. */
static inline XPOST_MUST_CHECK int
xpost_dev_class_rowcost(Xpost_Context *ctx,
                        Xpost_Object classdic,
                        const char *opname,
                        Xpost_Op_Func func)
{
    Xpost_Object op = xpost_operator_cons(ctx, opname, func, 0);

    if (xpost_object_get_type(op) != operatortype)
        return unregistered;
    return xpost_dict_put(ctx, classdic, xpost_name_cons(ctx, ".rowcost"),
                          op);
}

/* Take back what a row of this device's raster costs.

   For a device that has no row of its own to price: one whose pixel is
   settled per instance rather than by the class, and one whose page is
   the window system's rather than this process's. The class is a copy
   of a PostScript raster class, which prices the row it allocates, so
   what is taken back is a number that would otherwise be answered on
   behalf of a raster that is not there. */
static inline XPOST_MUST_CHECK int
xpost_dev_class_no_rowcost(Xpost_Context *ctx,
                           Xpost_Object classdic)
{
    int ret = xpost_dict_undef(ctx, classdic, xpost_name_cons(ctx, ".rowcost"));

    return (ret == undefined) ? 0 : ret;
}

/* Where the block a handed-over raster sits in begins.

   What a client receives is the raster, which is a position inside the
   block the device allocated rather than the block itself, so the
   address the client holds is not one the block can be given back by. A
   device that allocates a framebuffer therefore keeps the block's own
   address in the pointer immediately before the raster -- for a device
   whose buffer is a struct, the member declared just before the raster
   -- and xpost_output_buffer_release() reads it back from there. A
   raster the device did not allocate names no block, and the pointer
   before it is null. */
static inline void *
xpost_dev_output_buffer_block(unsigned char *data)
{
    void *block;

    memcpy(&block, data - sizeof(block), sizeof(block));
    return block;
}

/* Hand the rendered framebuffer to the embedding client: when the
   client registered an output-buffer hook (a pointer this run settled
   into the /OutputBufferOut string), store the buffer pointer through
   it. Returns 1 when the buffer was handed off -- the block is the
   client's to give back through xpost_output_buffer_release() and
   Destroy must leave it alone -- and 0 when no hook is registered. */
static inline int
xpost_dev_output_buffer_handoff(Xpost_Context *ctx,
                                unsigned char *data)
{
    Xpost_Object outbufstr;

    outbufstr = xpost_context_host_setting(ctx, "OutputBufferOut");
    if (xpost_object_get_type(outbufstr) == stringtype)
    {
        unsigned char **outbuf;

        memcpy(&outbuf, xpost_string_get_pointer(ctx, outbufstr), sizeof(outbuf));
        *outbuf = data;
        return 1;
    }
    return 0;
}

/* Give back the raster a device allocated for itself. Whether it is
   freed is not a question about the device but about who owns it: a
   raster handed over by xpost_dev_output_buffer_handoff() above belongs
   to the client, which gives the block back through
   xpost_output_buffer_release(), and freeing it here would be the second
   free of one block. Either way the device stops naming it and stops
   claiming it, which is what makes a repeated Destroy a no-op rather
   than that second free.

   A macro rather than a function because the three steps are one rule
   and belong together: a device may keep its ownership flag as a
   one-bit field packed with its other flags, and a bit-field has no
   address to pass. */
#define XPOST_DEV_BUFFER_RECLAIM(raster, owned) \
    do { \
        if (owned) \
            free(raster); \
        (raster) = NULL; \
        (owned) = 0; \
    } while (0)

/* Say that the block a lent raster sits in is named immediately in front
   of the raster, rather than leave it to hold by luck:
   xpost_dev_output_buffer_block() above reaches the block by stepping one
   pointer back from the address the client was handed, so a buffer struct
   that put anything between the two would have that read as its block.
   Every device lending a raster states this of its own buffer, naming the
   struct and which member each of the two is.

   (A negative array size rather than _Static_assert: this builds as C99
   with -pedantic-errors, which rejects the latter.) */
#define XPOST_DEV_ASSERT_BLOCK_PRECEDES_RASTER(tag, type, blockmem, rastermem) \
    typedef char xpost_##tag##_block_precedes_the_raster[ \
        offsetof(type, rastermem) \
        == offsetof(type, blockmem) + sizeof(void *) ? 1 : -1]

/* One channel of a coverage-weighted blend: the ground moved toward the
   ink by the fraction c/255, rounded to the nearest whole level. Rounding
   is about a distance and has no sign, so the half step is taken away
   from zero at both ends -- C division truncates toward zero, and a
   half added regardless of direction rounds a darkening step the short
   way, leaving full ink over the opposite ground a level short of it.

   Every device that keeps a buffer of its own blends by this rule, and
   what it blends differs -- three channels or four, in whatever order
   its raster stores them -- while the arithmetic on one channel does
   not. It is inline because it is reached once per channel per covered
   pixel, which is the busiest path a device has. */
static inline int
xpost_dev_blend_channel(int dst, int src, int c)
{
    int d = (src - dst) * c;

    return dst + (d < 0 ? (d - 127) / 255 : (d + 127) / 255);
}

#endif
