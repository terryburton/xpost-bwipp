/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_RECORD_H
#define XPOST_RECORD_H

#include <stddef.h>

#include "xpost_object.h"   /* real: what a coordinate arrives as */
#include "xpost_private.h" /* XPOST_TEST_VISIBLE */
#include "xpost_spill.h"    /* where a record puts what it need not hold */

/**
 * @file xpost_record.h
 * @brief What a page was asked to paint, kept to be painted again.
 *
 * A record holds the marking calls a page received, in the order they
 * were made, and gives them back for any run of rows that is asked for.
 * It holds no pixels: what it costs follows the number of marks, not
 * the size of the page, which is what lets a page be painted in a
 * raster smaller than itself.
 *
 * The marking methods are five and their arities are fixed by the
 * driver contract, so a mark is a kind, one colour value per component
 * of the device's space, and that kind's own operands. A record is
 * therefore a flat run of those and not a tree.
 *
 * A sampled image is held whole, as one entry the run names, and is the
 * one place a record is higher-level than the calls a device receives.
 * It has to be: a device holding no rows is painted an image a
 * rectangle at a time, so the five alone would hold a picture at tens
 * of bytes a sample against the one to three bytes a pixel of the page
 * the record exists to avoid holding.
 *
 * A glyph is the same case and is held the same way. Text reaches a
 * device one coverage-weighted pixel at a time, which costs a raster
 * nothing and costs a record a mark of tens of bytes per inked pixel;
 * so a glyph is held as a coverage mask and a placement of it, the mask
 * shared by every placement it is the same mask for. What a page of
 * text then costs is its distinct glyphs, once each, and a placement
 * apiece -- which is the drawing, rather than the ink.
 *
 * A record may also hold a placement of another record. A drawing made
 * once and put in several places is then held once and named from each
 * of them, and what a page pays for the second placement is the
 * placement. The drawing named is an ordinary record and may name
 * further drawings itself, so a drawing built out of drawings costs
 * each of them once however deep it sits and however often it is
 * placed. A placement is resolved when it is played rather than when it
 * is written down: expanding one into the marks it stands for would
 * write those marks once per placement, which is the cost the entry
 * exists to avoid, and would multiply where placements enclose each
 * other.
 *
 * Every mark says where it reaches in y when it is written down, and a
 * replay is given a row range, so a replay visits the marks that reach
 * into that range and steps over the rest. Playing a page of n marks
 * into b ranges then costs the marks each range meets rather than n
 * times b, which is the difference between this being useful on a large
 * page and being useless on one.
 *
 * The five are the whole of what a page can be asked to mark WHERE THE
 * DEVICE DOING THE RECORDING DECLARES NO FillPath. That condition is
 * what makes a record right about clipping, and it is not incidental:
 * .devtakespath (data/paint.ps) hands a whole path, and the clip shape
 * beside it, to any device that says it can fill a path for itself. A
 * device that says nothing gets the clip resolved above it instead, and
 * what reaches it is already cut to the region -- so a record of those
 * marks needs to hold no clip at all, and a replay cannot lose one.
 *
 * A recorder that later declared FillPath to save the cutting would
 * start receiving paths and clips it does not write down, and every
 * clipped page would replay wrong. Declining it is the design.
 *
 * A row range is the caller's to choose and nothing here assumes what
 * it is for. Successive strips of a page paint it in a raster the size
 * of a strip; the rows a window shows paint what someone is looking at;
 * the whole page paints the whole page. A record can be replayed as
 * many times as the caller likes, which is what lets a page already
 * drawn be shown again without running the program that drew it.
 *
 * WHERE THE MARKS ARE KEPT is a second decision and not the first one.
 * Retaining a drawing and holding it in memory are two things, and a
 * record does the first; xpost_record_spill moves what it holds into a
 * scratch file with no name, and from there what it costs in memory is
 * a write buffer, a read window and a table entry per distinct picture,
 * glyph, screen and placed drawing -- terms that follow the page's
 * vocabulary rather than its drawing. A record that has not been asked
 * to spill holds everything in memory, which costs the drawing without
 * limit and is the right answer for a small one.
 */

/** The marking calls a record holds, and their operand counts after
    the colour. The order and arities follow xpost_dev_driver.h.

    A polygon's pairs are its vertices with its subpath separators among
    them, a separator being the pair a subpath break is written as in
    the packed path this tree already keeps a polygon in (XPOST_PATH_BREAK
    in both coordinates, xpost_op_path.h). The separators are part of the
    shape and not decoration: the interior of a path with a hole is
    settled by scanning its subpaths together, so a polygon written down
    without them replays as a different region.

    A sampled image is the sixth and is not one of the marking calls: a
    device without rows of its own is painted an image one rectangle per
    sample, so a record built from the five alone would hold a thousand
    by thousand image as a million marks of tens of bytes each against
    the one to three bytes a pixel the page it is escaping costs. It
    carries an index into the images the record holds instead of
    operands of its own.

    A glyph is the eighth and is a mark: a coverage mask, painted in one
    colour at one place. It is here for the reason an image is. Text
    reaches a device one pixel at a time -- fully covered pixels through
    PutPix and partly covered edge pixels through BlendPix with their
    coverage -- which a raster stores nothing for, and which a record
    built from the five alone would hold at tens of bytes per inked
    pixel: a page of ordinary text inks a tenth of itself, so such a
    record runs to twenty times the page it is escaping. It carries an
    index into the masks the record holds and the place to put it, and
    the mask is one copy however many placements name it.

    A placement of another record is the ninth and is a mark: a drawing
    the record holds, put at an offset. It is here because a drawing put
    in several places is one drawing, and writing its marks down once per
    place would cost the places rather than the drawing -- which is the
    same argument the mask makes for a glyph, over whole drawings instead
    of over one character's pixels. It carries which of the record's
    drawings it names and where that drawing's own origin lands, and the
    drawing is one copy however many placements name it, at whatever
    depth: a placed drawing may itself hold placements.

    A screen is the seventh and is not a mark at all: it paints nothing,
    and says instead what the marks after it are to be painted under. A
    device rendering a grey as a pattern of pixels picks each pixel by
    the threshold under it, and which thresholds those are is the screen
    in force -- state a marking call does not carry, because the device
    reads it from itself rather than being told it. A record played back
    later would find whatever screen its target holds by then, so the
    screen is written down where it changes and put back as a replay
    passes it, and the marks either side of it are painted under the
    screens they were made under.

    A screen is met by every run of rows there is, where a mark is met
    only by the runs it reaches. One set before a mark governs that mark
    wherever on the page it lands, so every replay passes through the
    same screens in the same order whatever rows it was asked for --
    which is what makes a band's pixels the pixels that band would have
    had, had the whole page been painted at once. It is skipped by
    everything that asks about marks: what rows the record reaches, and
    which mark had the last word over a run, are questions about ink. */
typedef enum
{
    XPOST_RECORD_PUTPIX,   /**< x y */
    XPOST_RECORD_BLENDPIX, /**< cov x y */
    XPOST_RECORD_DRAWLINE, /**< x1 y1 x2 y2 */
    XPOST_RECORD_FILLRECT, /**< x y w h */
    XPOST_RECORD_FILLPOLY, /**< n, then n pairs of x y */
    XPOST_RECORD_IMAGE,    /**< which of the record's images */
    XPOST_RECORD_SCREEN,   /**< which of the record's screens */
    XPOST_RECORD_GLYPH,    /**< which of the record's masks, then x y */
    XPOST_RECORD_PLACE     /**< which of the record's drawings, then dx dy */
} Xpost_Record_Kind;

/** How many drawings a placement may stand inside.
 *
 * A drawing holding a placement of a drawing that holds one is nested,
 * and a replay descends a level per placement it meets. The descent is
 * bounded so that a drawing nested past this is refused where the
 * placement is written down, with the run still in a position to hear
 * about it, rather than followed until something runs out. Twelve is
 * past any nesting a drawing built out of drawings has -- a page of a
 * figure of a figure of a figure is four -- and short of any depth a
 * bounded descent could not be made at.
 */
#define XPOST_RECORD_NEST 12

typedef struct _Xpost_Record Xpost_Record;

/**
 * @brief A record for a device whose colour takes @p ncomp values.
 * @return the record, or NULL where there is no memory for one
 */
Xpost_Record *xpost_record_new(int ncomp);

/**
 * @brief Give up a reference to a record, and, with the last of them,
 *        the record and everything it holds.
 *
 * A record is held by whoever made it and by every record that places
 * it (xpost_record_place), so what is given up here is one holder's
 * claim; the record goes when the last of them does. A drawing whose
 * maker has finished with it therefore stands while a page still places
 * it, and the page's own boundary is what gives it up.
 */
void xpost_record_free(Xpost_Record *rec);

/**
 * @brief Write one mark down.
 *
 * @param[in] rec the record
 * @param[in] kind which marking call
 * @param[in] colour ncomp values, as the call received them
 * @param[in] ops the call's own operands, in the order above
 * @param[in] nops how many, which the kind settles except for a polygon
 * @return 1, or 0 where there is no memory to hold it
 *
 * A record that could not hold a mark is short of one, and a page
 * played back from it would be missing what it could not hold. So the
 * failure sticks: the record remembers it, every later mark is refused
 * as well, and every replay refuses. A caller that ignores this return
 * therefore cannot go on to emit a page that is quietly wrong -- it
 * gets nothing rather than something short.
 *
 * The values are kept in the type a coordinate arrives in rather than
 * a wider one: a record exists to be smaller than the page it draws,
 * and widening every value would halve how much page a record buys.
 *
 * The operands are kept as they arrived. Rounding a coordinate to a
 * pixel is the painting device's business and is done when the mark is
 * played, so that a record made once can be played into rasters that
 * differ in where their rows begin.
 */
int xpost_record_mark(Xpost_Record *rec, Xpost_Record_Kind kind,
                      const real *colour, const real *ops, int nops);

/**
 * @brief A sampled image, where it is put, and what decodes it.
 *
 * The samples are the normalized rows the image collectors produce --
 * one byte per component per sample, whatever depth the program's data
 * source had -- and everything beside them is the result of the colour
 * setup the painter bakes before it writes a row, not the state that
 * setup was derived from. A replay happens when the page is put out,
 * by which time the graphics state that decoded the image is gone:
 * transfer functions, the current colour space and the space's
 * conversion have all moved on, and there is no re-deriving them. What
 * is kept is what a row write reads, which is these tables.
 *
 *   samples   the one block the record made of the rows it was handed:
 *             height runs of width x ncomp bytes, or, where the rows
 *             are planar, height x ncomp runs of width bytes, a row's
 *             planes together. It is the record's and is filled in by
 *             the record; what a caller hands over is the rows.
 *   lut       one-component spaces bake decode, conversion and transfer
 *             into 256 entries of nat bytes, and nothing else is read.
 *   dluts     otherwise, ncomp runs of 256 decode entries, converted at
 *             the write and passed through tlut (and, where the device
 *             takes three, through the three channel transfers) after.
 *   mbits     one bit per mask sample in rows of mrowb bytes, a set
 *             bit leaving the pixel alone, over a grid of mw by mh
 *             samples covering the page area the sample grid covers;
 *             mranges pairs of raw sample values, a pixel inside every
 *             one of them left alone.
 *   cspans    quads of x0 y0 x1 y1 in device space: the region resolved
 *             above the device where it was not a rectangle.
 *
 * A pointer that is not given is NULL and the thing it names is not
 * read. Nothing here is held by reference: what a caller hands over is
 * copied, since a record outlives the job that made it.
 */
typedef struct
{
    int width, height;   /**< the sample grid the rows hold */
    int ncomp;           /**< components a sample carries */
    int nat;             /**< values a device pixel takes: 1 or 3 */
    int planar;          /**< rows hold planes rather than pixels */
    int rgbrows;         /**< the device row is three colour planes */
    int cmyk;            /**< four components convert by complement */
    int interp;          /**< blend between samples where magnified */
    real xoff, xscale;   /**< where a sample column lands, and how wide */
    real yoff, yscale;   /**< where a sample row lands, and how tall */
    real cx0, cy0;       /**< the region the rows are written through */
    real cx1, cy1;
    const unsigned char *samples;
    const unsigned char *lut;      /**< 256 entries of nat bytes */
    const unsigned char *dluts;    /**< ncomp runs of 256 */
    const unsigned char *tlut;     /**< 256 */
    const unsigned char *tlutrgb;  /**< three runs of 256 */
    const unsigned char *mbits;    /**< mh runs of mrowb bytes */
    int mrowb;
    int mw, mh;                    /**< the grid the mask states */
    const int *mranges;            /**< nranges raw values */
    int nranges;
    const real *cspans;            /**< nspan quads */
    int nspan;
} Xpost_Record_Image;

/**
 * @brief Write one sampled image down, and a mark naming it.
 *
 * @param[in] img everything about the image but its samples; its
 *                @c samples field is the record's to fill and is not
 *                read here
 * @param[in] rows the sample rows, @p nrows runs of width bytes where
 *                 the rows are planar and width x ncomp bytes where
 *                 they are not, a row's planes adjacent
 * @param[in] nrows height, or height x ncomp where the rows are planar
 * @return 1, or 0 where there is no memory to hold it, on the same
 *         terms as a mark: the record is then short of a mark and every
 *         replay of it refuses.
 *
 * Everything handed here is copied. A record outlives the job that made
 * it -- pages either side of the one on screen are held so that moving
 * back is as cheap as moving forward -- so it may hold nothing
 * belonging to the run, and the rows in particular are the painter's
 * own scratch buffers, refilled for the row after.
 *
 * What that costs is the image, at one byte per component per sample:
 * bounded by the picture the job is holding anyway rather than by the
 * page, which is the whole reason an image is one entry here.
 */
int xpost_record_image(Xpost_Record *rec, const Xpost_Record_Image *img,
                       const unsigned char *const *rows, int nrows);

/**
 * @brief How many images a record holds.
 */
size_t xpost_record_image_count(const Xpost_Record *rec);

/**
 * @brief The image at @p i, as it was written down, or NULL.
 *
 * What comes back points into the record and is good until the next
 * image is written down.
 *
 * Its @c samples and @c mbits are NULL where the record has spilled:
 * those two follow the picture's size rather than its description, so
 * they are the two a spilled record does not hold. A caller wanting them
 * asks xpost_record_image_run and xpost_record_image_mbits, which answer
 * for a record either way round.
 */
const Xpost_Record_Image *xpost_record_image_get(const Xpost_Record *rec,
                                                 size_t i);

/**
 * @brief One run of an image's samples: run @p run of the @p nrows the
 *        record was handed.
 *
 * @return width bytes where the rows are planar and width x ncomp where
 *         they are not, or NULL where there is no such run
 *
 * A picture is written into a raster a row at a time, and this is the row
 * in hand. It is the whole of what a caller need hold of a picture at
 * once, which is what keeps a spilled record's picture bounded: a
 * hundred megabytes of samples costs the run being written.
 *
 * What comes back is good until the next run is asked for. Where the
 * record holds its samples in memory it points at them; where the record
 * has spilled it points at the one run the record read back, so a caller
 * that wants two runs at once must take a copy of the first.
 */
const unsigned char *xpost_record_image_run(const Xpost_Record *rec,
                                            size_t i, int run);

/**
 * @brief An image's mask bits, or NULL where it has none.
 *
 * mh runs of mrowb bytes, a set bit leaving the pixel alone. Whole,
 * because what reads them reads them whole; where the record has spilled
 * they are read back into a buffer of the record's, good until the next
 * image's are asked for.
 */
const unsigned char *xpost_record_image_mbits(const Xpost_Record *rec,
                                              size_t i);

/**
 * @brief Which of an image's rows reach device rows @p lo to @p hi.
 *
 * @param[out] y0 the first sample row to write, @p y1 one past the last
 * @return 1 where some row reaches the range, 0 where none does
 *
 * An image is clipped to a run of rows the way a shape is: by choosing
 * what to paint rather than by trimming what was recorded. Which rows
 * to choose is what the placing transform decides, and this is that
 * question asked of it. The answer errs outward -- a row written that
 * the region then rejects costs a pass over it, a row not written is
 * missing from the page.
 */
int xpost_record_image_rows(const Xpost_Record_Image *img,
                            real lo, real hi, int *y0, int *y1);

/**
 * @brief Take up a coverage mask, answering which of the record's it is.
 *
 * @param[in] cov @p w x @p h coverage bytes, a row at a time: 0 where
 *                the pixel is not painted, 255 where the whole of it is,
 *                and the coverage between where part of it is
 * @param[out] at which mask it became
 * @return 1, or 0 where there is no memory to hold it, on the same
 *         terms as a mark: the record is then short of a mark and every
 *         replay of it refuses.
 *
 * A mask equal to one the record already holds is that one: the record
 * answers its index and takes no second copy. That is the whole of what
 * a glyph entry buys -- a line of text is the same few dozen masks over
 * and over, and holding one copy of each is the difference between a
 * page of text costing its drawing and costing its ink.
 *
 * Equal means equal in every byte and in both extents, which is what
 * makes the sharing safe to do here rather than at the caller: two
 * placements of one glyph reach this with the same bytes, and anything
 * that reaches it with different bytes is a different mask whatever it
 * was named by. A caller that knows two placements are the same glyph
 * need not say so, and one that is wrong about it cannot mislead this.
 *
 * The mask is copied. A record outlives the job that made it, and what
 * a caller hands over is a rendering cache's entry or a scratch buffer,
 * either of which may be given up while the record still names it.
 *
 * Taking the mask up and writing the placement down are two calls
 * because they happen at two times. A glyph's coverage is rendered when
 * the text operator reaches the character; the mark is made when the
 * page is painted, which for a string of text is not the same order.
 * A record holds its marks in the order they were painted, so the
 * placement is written down where the painting happens and the mask,
 * which is the same mask whenever it is taken up, is taken up here.
 */
int xpost_record_mask(Xpost_Record *rec, const unsigned char *cov,
                      int w, int h, size_t *at);

/**
 * @brief Write down a placement of the mask at @p at.
 *
 * @param[in] colour ncomp values, the colour the mask is painted in
 * @param[in] x where the mask's first column lands, @p y its first row
 * @return 1, or 0 where the record holds no such mask or has no memory
 *         to hold the placement, on the same terms as a mark.
 *
 * The colour is carried once for the whole mask and the coverage per
 * pixel is in the mask, which is the shape the painting has: one colour
 * is in force for a glyph and each pixel of it is covered as much as it
 * is covered.
 */
int xpost_record_glyph(Xpost_Record *rec, const real *colour,
                       size_t at, real x, real y);

/**
 * @brief How many coverage masks playing a record would put down,
 *        counting through the drawings it places as well as its own.
 */
size_t xpost_record_mask_count(const Xpost_Record *rec);

/**
 * @brief What the masks a record holds cost, in bytes.
 *
 * The quantity a page of text is judged on, beside xpost_record_bytes:
 * a record of text is worth what it is worth because it holds a glyph
 * once however often the page shows it, and the way that stops being
 * true is silent.
 */
size_t xpost_record_mask_bytes(const Xpost_Record *rec);

/**
 * @brief The mask at @p i, as it was taken up, or NULL.
 *
 * @param[out] w its width, @p h its height
 *
 * What comes back points into the record and is good until the next
 * mask is taken up.
 */
const unsigned char *xpost_record_mask_get(const Xpost_Record *rec, size_t i,
                                           int *w, int *h);

/**
 * @brief Write down a placement of another record, at @p dx @p dy.
 *
 * @param[in] rec the record the placement is written into
 * @param[in] sub the drawing placed, which @p rec holds a reference to
 *                from here
 * @param[in] dx how far along the drawing's own coordinates are carried,
 *               @p dy how far down
 * @return 1, or 0 where the placement could not be held or would place a
 *         record inside itself, on the same terms as a mark: the record
 *         is then short of a mark and every replay of it refuses.
 *
 * The drawing is named and not copied. One drawing placed twenty-five
 * times is one drawing and twenty-five placements, and a drawing built
 * out of placements of further drawings costs each of those once as
 * well -- which is what makes a page of a form inside a form inside a
 * form cost the three forms rather than their product. What a placement
 * costs is the mark.
 *
 * A reference is taken and given up again where the placement is (at a
 * page boundary, and where the record is given up or released), so the
 * drawing outlives whatever else was holding it: a caller may write a
 * placement down and drop the drawing it named, and the page still
 * plays.
 *
 * The offset is carried in the coordinates the marks were made in and is
 * added to each of them as it is played, so a drawing made at one place
 * is played at another without being written down again. It is not
 * rounded to a pixel: which pixels a coordinate names is the painting
 * device's answer, taken when the mark is played, so a placement lands
 * at the sub-pixel phase it was placed at rather than at the phase the
 * drawing was made at.
 *
 * A record may not be placed inside itself, directly or through the
 * drawings it holds, since playing such a placement would never end.
 * The whole of what a placement can reach is walked here to say so,
 * which costs the drawings held rather than the marks in them.
 */
int xpost_record_place(Xpost_Record *rec, Xpost_Record *sub,
                       real dx, real dy);

/**
 * @brief How many drawings a record places.
 */
size_t xpost_record_place_count(const Xpost_Record *rec);

/**
 * @brief How many drawings deep a record goes, counting itself.
 *
 * One for a record placing none, and one more than the deepest drawing
 * it places. What it is for is the question a caller about to place it
 * asks: a drawing this deep placed in another goes one deeper, and a
 * replay descends a bounded number of levels.
 */
int xpost_record_depth(const Xpost_Record *rec);

/**
 * @brief The drawing at @p i, or NULL where the record places none there.
 *
 * What comes back is held by the record and is good until the record
 * gives its placements up.
 */
Xpost_Record *xpost_record_place_get(const Xpost_Record *rec, size_t i);

/**
 * @brief Take a reference to a record, which xpost_record_free gives up.
 *
 * @return @p rec
 *
 * A record is held by whoever is going to play it, and a drawing placed
 * in another record is held by that record as well. What the last holder
 * gives up is given up.
 */
Xpost_Record *xpost_record_hold(Xpost_Record *rec);

/**
 * @brief Write down the screen the marks after this are made under.
 *
 * @param[in] rec the record
 * @param[in] w the cell's width, @p h its height
 * @param[in] cell w x h thresholds, which is copied
 * @return 1, or 0 where there is no memory to hold it, on the same
 *         terms as a mark: the record is then short of a mark and every
 *         replay of it refuses.
 *
 * Called where the screen changes and not per mark. What decides that
 * is the machinery that maintains the cell, which rebuilds it only when
 * the screen it is built from has changed -- so a record is exactly as
 * sensitive to a screen change as painting straight at the device is,
 * and a page that sets one screen and keeps it holds one of these.
 *
 * A record whose target does not screen never has this called and holds
 * none, which costs it nothing.
 */
int xpost_record_screen(Xpost_Record *rec, int w, int h,
                        const unsigned char *cell);

/**
 * @brief How many screens a record holds.
 */
size_t xpost_record_screen_count(const Xpost_Record *rec);

/**
 * @brief The screen at @p i, or NULL where the record holds none there.
 *
 * @param[out] w the cell's width, @p h its height
 *
 * What comes back points into the record and is good until the next
 * screen is written down.
 */
const unsigned char *xpost_record_screen_get(const Xpost_Record *rec,
                                             size_t i, int *w, int *h);

/**
 * @brief Give up the marks a record holds, keeping the record.
 *
 * What a record describes is a page, and a page ends. A record given up
 * at that boundary costs the drawing on one page; one that is not costs
 * the drawing on every page a job has drawn so far, and a replay of it
 * plays them all -- once per band -- to paint the last of them.
 *
 * When a page ends is the caller's to say, and this says nothing about
 * it. The device holding a record ends a page where the page it
 * describes is painted over whole, which is the one moment at which
 * every mark before it stops being part of the page.
 *
 * The buffers the marks were held in are kept and filled again, so the
 * page after does not buy its storage a second time. What that leaves
 * is a record costing the largest page a job has drawn rather than the
 * page in hand, which is the quantity a caller comparing a record
 * against a raster wants.
 *
 * The screen in force is not given up with the marks: it is written
 * down again as the first entry of the page beginning, because a page
 * boundary is not a screen change and the marks after one are painted
 * under the screen that was in force before it. A record emptied of it
 * would paint the page after under whatever screen its target held.
 *
 * A record short of a mark it was given is not emptied. All it has to
 * say about the page it could not hold is that it could not hold it,
 * and every replay of it refuses on that ground; giving that up would
 * turn a page refused into a page quietly missing something.
 */
void xpost_record_clear(Xpost_Record *rec);

/**
 * @brief Give up everything a record holds, and the room it holds it in.
 *
 * The other way a record is emptied, and the difference is the storage.
 * xpost_record_clear ends a page and keeps what the page filled, because
 * the page after will fill it again; this is for a record that is not
 * going to be filled again, so the room goes back to the allocator with
 * the marks. What is left is a record holding nothing and costing
 * nothing, still usable, which xpost_record_bytes then answers for
 * accordingly -- the high-water mark goes with the storage it was a mark
 * of, since a record that has given its runs back is resident for none
 * of them.
 *
 * It is what a caller does when it wants the room back rather than the
 * record emptied: one that has finished with a record it is holding, or
 * one that has put the marks somewhere else and no longer needs them
 * where they are.
 *
 * The screen in force goes too, unlike at a page boundary. A page
 * boundary is a point the same record goes on past, so the screen the
 * marks after it are made under has to be written down again; a record
 * given up is not going to describe those marks at all, and whatever
 * paints them is being told the screen directly.
 *
 * A record short of a mark is emptied like any other, and stays short of
 * it. Nothing here paints the mark that went missing, so the page it
 * describes is still one it cannot reproduce and every replay of it
 * still refuses.
 */
void xpost_record_release(Xpost_Record *rec);

/**
 * @brief Give up the entries a caller has finished with, keeping the
 *        coverage masks.
 *
 * For a caller that has finished with each entry as it arrived rather
 * than holding a page of them. Such a caller takes an entry up only so
 * that one writer, or one replay, reads it, and the entry is spent the
 * moment that has happened: giving up the spent ones holds what the
 * record costs to the entry in hand.
 *
 * The masks are what stays, and they stay because they are not spent.
 * Which mask a glyph names is an index into the record, and the
 * placements of a string arrive after all of its masks, so a mask has to
 * outlive the placement that named it. What is left is the coverage of
 * the page's distinct glyphs -- what a glyph cache holds -- and it goes
 * at the page boundary with everything else (xpost_record_clear).
 *
 * A mark naming an image or a screen the record no longer holds replays
 * as nothing, so this is only for a record whose marks have all been
 * played.
 */
void xpost_record_spent(Xpost_Record *rec);

/**
 * @brief How many marks a record holds.
 */
size_t xpost_record_count(const Xpost_Record *rec);

/**
 * @brief What holding a record costs, in bytes.
 *
 * The marks, their values, the images, masks and screen cells they name,
 * the record itself, and what an allocator keeps beside each of those
 * blocks. It is the quantity the whole mechanism is judged on: a record
 * is worth holding while it is smaller than the raster it saves holding,
 * and that is a comparison rather than a guess.
 *
 * So that the comparison is one, this is what the record has made
 * resident and not what it has asked for. The runs grow by doubling, and
 * the room past what is in them costs nothing until a mark is written
 * into it, so what is counted is the most they have ever held: that
 * storage stays after a page boundary empties them, because a run keeps
 * it to be filled again, and a record between pages is therefore
 * resident for the largest page the job has drawn. What the entries
 * point at is counted as it stands instead, those blocks being given up
 * at a boundary rather than kept.
 *
 * WHAT IT DOES NOT INCLUDE, so that a caller comparing it against a
 * raster knows what it is comparing:
 *
 *   Playing the record back. A replay hands each mark to a device
 *   method, and a method may be a procedure -- so a mark reaches one as
 *   interpreter objects built for the call, a polygon as an array of its
 *   vertices. What that costs follows the marks a run of rows meets and
 *   is the target's and the interpreter's, not the record's; on
 *   path-heavy content it is several times this number. A caller
 *   weighing a whole banded emission against a whole page raster is
 *   weighing that too, and will not find it here.
 *
 *   The drawings a record places. A placed drawing is a record of its
 *   own, held by every record placing it and by whoever made it, and
 *   what it costs is what it answers here: charging it to each of its
 *   placements would count one drawing as many times as the page shows
 *   it, which is the arithmetic a placement exists to avoid. What is
 *   counted here is the placements themselves.
 *
 *   The pages a run leaves behind when it outgrows a block. Growing
 *   copies into a new block and gives up the old one, whose pages stay
 *   resident until the allocator hands them out again -- which is
 *   fragmentation, and there is no asking a C allocator for it.
 *
 * Both of those are measured in tests/record_cost_test.c, which holds
 * this number to what a process is resident for over the marks that
 * produced it.
 *
 * A record short of a mark describes a page it cannot reproduce, and
 * what it answers here is what it managed to hold rather than what that
 * page would have cost. Nothing is to be concluded from it about the
 * page: xpost_record_failed is the question worth asking of such a
 * record.
 */
size_t xpost_record_bytes(const Xpost_Record *rec);

/**
 * @brief What a record is resident for, in bytes.
 *
 * The part of xpost_record_bytes that is memory. For a record holding
 * everything in memory the two are the same number; for one that has
 * spilled, this is what is left in memory and the difference is what is
 * in the file. It is what a caller asking whether the bound holds wants,
 * where xpost_record_bytes is what a caller asking what the drawing
 * costs wants.
 */
size_t xpost_record_resident(const Xpost_Record *rec);

/**
 * @brief Put what a record holds into a scratch file, and keep it there.
 *
 * @return 1, or 0 where there is no scratch space or the write failed
 *
 * Everything the record holds goes: the marks and their values, the
 * samples of every picture, the coverage of every glyph, the cells of
 * every screen. What is left in memory is a write buffer, a read window,
 * and one small entry per distinct picture, glyph, screen and placed
 * drawing -- terms that follow how many kinds of thing the page names
 * rather than how much it draws. Marks written after this go straight to
 * the file, so a record asked once stays spilled until it is released.
 *
 * A record that is already spilled answers 1 and does nothing. A record
 * short of a mark is not spilled: it describes a page it cannot
 * reproduce, and moving that page's remains is no use to anybody.
 *
 * A failure here is a failure to make or write the file and leaves the
 * record exactly as it was, holding everything in memory and able to go
 * on. It is the caller's to decide what that means: the page is not lost
 * by it, only unbounded.
 *
 * The drawings a record places are records of their own and are not
 * spilled by this. Each is asked for itself, by whoever holds it.
 */
int xpost_record_spill(Xpost_Record *rec);

/**
 * @brief Whether a record's marks are in a file rather than in memory.
 */
int xpost_record_spilled(const Xpost_Record *rec);

/**
 * @brief Shorten the file a spilled record's marks are in, keeping the
 *        first @p keep bytes of it.
 *
 * @return 1, or 0 where the record holds no file or it could not be
 *         shortened
 *
 * Nothing in a run does this. It is here so that a test can see what a
 * record does when its scratch file has been shortened under a
 * descriptor nobody else holds -- which is a defect here or a fault
 * below, and which must end in a refusal naming the file rather than in
 * a page that stops where the reading did. A failure path with no test
 * is a failure path that does not work.
 */
XPOST_TEST_VISIBLE int xpost_record_spill_shorten(Xpost_Record *rec,
                                                  long long keep);

/**
 * @brief Which failure left a record short of a mark.
 *
 * @return 0 where it is short of none, VMerror where memory ran out,
 *         ioerror where the scratch file did
 *
 * A caller told VMerror because a filesystem filled looks in the wrong
 * place, so the two are told apart. What either comes to is the same:
 * the record describes a page it cannot reproduce and every replay of it
 * refuses.
 */
int xpost_record_error(const Xpost_Record *rec);

/**
 * @brief Say that a mark the page was given never reached the record.
 *
 * The record is short of a mark from here on, on the terms
 * xpost_record_mark states: every later mark is refused, nothing it
 * holds is given back, and every replay of it refuses. What that comes
 * to is a page refused rather than a page put out short.
 *
 * It is here because the marks reach a record through a device, and a
 * device asked to paint something it does not manage to write down has
 * exactly the page a record that could not hold it has. The record
 * cannot learn that from a call it never received, so the device says
 * so, and the one refusal covers both.
 *
 * A record already short of a mark stays short: there is nothing here
 * to undo.
 */
void xpost_record_lost(Xpost_Record *rec);

/**
 * @brief Whether a mark was ever refused for want of memory.
 *
 * @return 1 where the record is short of a mark it was given, 0 where
 *         it holds everything it was given
 *
 * A record answering 1 describes a page it cannot reproduce, and every
 * replay of it refuses. The answer is asked for by whoever is about to
 * emit, so that a page is refused where it cannot be painted whole.
 */
int xpost_record_failed(const Xpost_Record *rec);

/**
 * @brief The rows a record's marks reach, or zero where it holds none.
 *
 * @param[out] lo the first row any mark reaches
 * @param[out] hi the last
 * @return 1 where the record holds a mark, 0 where it holds none
 */
int xpost_record_extent(const Xpost_Record *rec, real *lo, real *hi);

/**
 * @brief The box the marks reach, or zero where the record holds none.
 *
 * @param[out] x0 the leftmost coordinate any mark reaches, @p x1 the
 *                rightmost, @p y0 and @p y1 the rows
 * @return 1 where the record holds a mark, 0 where it holds none
 *
 * What xpost_record_extent answers about rows, over both directions. It
 * is a walk of the marks rather than a reading, since a mark says which
 * rows it reaches and not which columns: the rows are what a replay
 * chooses by and are worth the room, and this is asked once of a
 * finished drawing rather than once per band.
 *
 * The box errs outward the way a mark's reach does. A drawing placed
 * inside it paints nothing outside it; a drawing whose box exceeds
 * something is a drawing that may paint outside that thing.
 */
int xpost_record_box(const Xpost_Record *rec, real *x0, real *y0,
                     real *x1, real *y1);

/**
 * @brief The last mark reaching rows @p lo to @p hi.
 *
 * @param[out] at where it was found
 * @return 1 where some mark reaches those rows, 0 where none does
 *
 * The mark that had the last word over the run, which is what a caller
 * asking what those rows come to has to start from: everything painted
 * before it is painted over wherever it covers, and nothing is painted
 * after it at all. It answers by the rule a replay plays by, so the
 * mark it names is one the replay would play.
 *
 * What a caller does with that is its own. A band loop asks it whether
 * a band comes to nothing but the colour the page was cleared to, since
 * such a band need not be painted: the ground is what a device holding
 * no pixel over a row answers and what an emitted page carries there,
 * so leaving those rows alone puts out the page painting them would
 * have put out. Whether a mark leaves a run like that is a question
 * about colour and about the page's width, which is the asking device's
 * to settle and not this record's.
 */
int xpost_record_last(const Xpost_Record *rec, real lo, real hi, size_t *at);

/**
 * @brief The mark at @p i, as it was written down.
 *
 * @param[out] kind which marking call
 * @param[out] colour the ncomp values it was made with
 * @param[out] ops its own operands, or NULL where the kind has none
 * @param[out] nops how many
 * @return 1, or 0 where the record holds no mark there
 *
 * An image's one operand is which of the record's images it names, and
 * its colour values are zero: what colours it is in its samples. A
 * glyph's are which of the record's masks it names and where that mask
 * is put, and its colour values are the colour the mask is painted in.
 * A placement's are which of the record's drawings it names and how far
 * that drawing's coordinates are carried, and its colour values are
 * zero: what colours it is the marks of the drawing it names.
 *
 * What comes back points into the record and is good until the next
 * mark is written down. A record short of a mark it was given gives
 * none of them back, on the same terms as a replay of one: what would
 * be built from what is left is a page missing something, and a page
 * missing a mark looks like a page.
 *
 * A replay that plays a mark into a device returns to the interpreter
 * to do it -- a device method may be a procedure, and what runs a
 * procedure is the interpreter -- so it cannot be handed the run of
 * marks in one call. It asks for them one at a time instead, and what
 * it keeps between marks is how far it has got.
 */
int xpost_record_get(const Xpost_Record *rec, size_t i,
                     Xpost_Record_Kind *kind, const real **colour,
                     const real **ops, int *nops);

/**
 * @brief The first mark from @p from on that reaches rows @p lo to @p hi.
 *
 * @param[in] rec the record
 * @param[in] from the mark to start looking at
 * @param[out] at where one was found
 * @return 1 where there is one, 0 where no mark from there on reaches
 *         those rows
 *
 * The walk a replay makes when it cannot be a loop. A device method may
 * be a procedure and what runs a procedure is the interpreter, so such a
 * replay returns between marks and is resumed rather than continued;
 * what it keeps is how far it has got, and this is how it gets on. It
 * answers by the rule xpost_record_replay plays by, so the two visit the
 * same marks for the same rows.
 */
int xpost_record_next(const Xpost_Record *rec, size_t from, real lo, real hi,
                      size_t *at);

/**
 * @brief What a replay does with each mark it is given.
 *
 * Called once per mark that reaches the rows asked for, in the order
 * the marks were made -- which is the order they were painted in, and
 * so the order they must be painted in again. Returns 0 to go on, or
 * the error to raise, which the replay returns unchanged without
 * playing anything further.
 */
typedef int (*Xpost_Record_Player)(void *data, Xpost_Record_Kind kind,
                                   const real *colour,
                                   const real *ops, int nops);

/**
 * @brief Play back the marks that reach rows @p lo to @p hi inclusive.
 *
 * @return 0, what the player returned when it stopped, or VMerror
 *         where the record is short of a mark it was given and so
 *         describes a page it cannot reproduce
 *
 * A mark that reaches the range at all is played whole. A shape has to
 * be converted whole to be right about any part of it, so the range
 * chooses which marks are played and never trims one.
 *
 * A placement is handed over as the mark it is and is not descended
 * into: what it stands for is another record's marks, at an offset, and
 * a player wanting them plays that record. A caller that plays a page
 * into a device descends instead, so that a drawing placed at several
 * depths is played wherever it was placed
 * (src/lib/xpost_dev_record.c).
 */
int xpost_record_replay(const Xpost_Record *rec, real lo, real hi,
                        Xpost_Record_Player player, void *data);

#endif
