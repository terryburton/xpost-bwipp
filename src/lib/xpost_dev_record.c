/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_record.c
 * @brief The recording device: keeps the marks instead of the pixels.
 *
 * What makes banding possible. Nothing is rastered as it is drawn; the marks
 * are kept in order and replayed once per band, so a page too large to hold
 * as pixels is drawn a strip at a time.
 */

/*
 * The device that writes a page down instead of painting it, and the
 * replay that paints a page it wrote down.
 *
 * Its marking methods put each call into an Xpost_Record and mark
 * nothing, so what it costs follows the number of marks rather than the
 * size of the page; its Emit builds a device that does paint, plays
 * every mark into it and puts out that device's page. See
 * doc/xpost_design.dox for what the record is for and src/lib/xpost_record.h
 * for what it holds.
 *
 * The class this specialises is data/recorddev.ps, which declares
 * exactly the five marking methods this file records -- the whole
 * reason five kinds are enough is that every other call the machinery
 * can make is resolved above a device that declines to declare it.
 *
 * Which device paints the page is the device this run selected: -d pgm
 * and -d pgm:band play into the grayscale raster, and -d record, which
 * selects this class itself and names no device, into the colour one. The
 * class carries the roster of the devices a record can be played into
 * and this file settles one of them at load, because the choice decides
 * the shape of every marking method here -- a mark carries one value per
 * component of the target's colour space, so a grayscale target and a
 * colour one are two suites of entry points and not one suite told
 * which it is.
 *
 * The operands are written down as they arrived. Which pixels a
 * coordinate names is the painting device's answer and is taken when
 * the mark is played, so a device with a different idea of where its
 * rows begin plays the same record to the pixels its own contract
 * gives.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_dict.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_name.h"

#include "xpost_handle.h"
#include "xpost_operator.h"
#include "xpost_op_dict.h"
#include "xpost_dev_generic.h" /* the ground a read answers */
#include "xpost_dev_driver.h"  /* device contract and shared helpers */
#include "xpost_op_path.h"     /* XPOST_PATH_BREAK: a subpath separator */
#include "xpost_record.h"
#include "xpost_compat.h"  /* xpost_temp_dir: a refusal names the place */
#include "xpost_spill.h"
#include "xpost_dev_record.h"

/* The most components a mark can carry here, which is the widest of the
   colour spaces a device this record plays into declares: three for the
   colour rasters, one for the grayscale one. It bounds the buffers the
   entry points below build a colour in; how many of them a given record
   uses is the record's own count, settled when the device is made from
   the class the run selected a target for. */
#define RECORD_MAXCOMP 3

/* Where a replay has got to in one drawing, and where that drawing's
   marks are put on the page.

   A record may hold a placement of another record, so what a replay
   keeps is a stack of these: it descends where it meets a placement and
   comes back to the entry after it. The offset is what every placement
   above has carried the coordinates by, added up, so playing a mark is
   one addition however deep the drawing sits. */
typedef struct
{
    const Xpost_Record *rec;
    real dx, dy;       /* where the drawing's coordinates land */
    real lo, hi;       /* the rows asked of it, in its own coordinates */
    size_t idx;        /* the entry to look at next */
    size_t pixat;      /* how far into a coverage mask */
    int inmask;        /* whether that place belongs to the entry at idx */
} _Level;

/* The walk a record device is making of what it holds.
 *
 * Kept beside the device's state rather than in it: the state is read
 * and written once per mark as a page is drawn, and a walk deep enough
 * to descend into every placement there can be would be carried past
 * every one of those marks. It is taken up at the first replay and given
 * up with the device.
 *
 * One walk, because a device plays one run of rows at a time: a band is
 * played, put out, and the next band asked for. A replay begun while one
 * is running would be walking the same record from two places. */
typedef struct
{
    int depth;                    /* placements descended into */
    _Level at[XPOST_RECORD_NEST];
    /* a placed polygon's coordinates with the placement's offset in
       them, since what the record holds is the drawing's own coordinates
       and the drawing is played wherever it was placed */
    real *poly;
    size_t npoly;
} _Walk;

/* Where a run wants its records held.
 *
 * The default is to weigh it: a record is worth holding in memory while
 * it is smaller than the raster banding the page saves, and past that it
 * is costing more than the page it is buying, so it goes in a file. The
 * other two take the decision away -- one for a run that would rather
 * spend memory than touch a disk at all, one for a run that wants the
 * bound from the first mark. */
typedef enum
{
    SPILL_AUTO,
    SPILL_NEVER,
    SPILL_ALWAYS
} Spill_State;

/* And what became of it, which is the other half of a switch: a state
   nobody can read back is a state whose mistakes are invisible. */
typedef enum
{
    SPILT_MEMORY,   /* the marks are in memory */
    SPILT_FILE,     /* the marks are in a file */
    SPILT_REFUSED   /* they were to go in a file and there was none */
} Spill_Where;

/* How many marks pass between two weighings of the record.
 *
 * What a record costs is a reading rather than a walk, so asking is
 * cheap; asking on the way past every mark is still a comparison a page
 * pays for two hundred thousand times. Sixty-four costs a record 1,164
 * bytes of lateness on a nineteen-megabyte page: that is how far past
 * the threshold it can get before the next weighing catches it. An
 * entry carrying more than this many operands is weighed whatever the
 * count says, so that one large polygon cannot carry a record past the
 * threshold unnoticed. */
#define RECORD_WEIGH_EVERY 64

typedef struct
{
    int width, height;
    /* the components a mark of this record carries, which is the
       component count of the device its page is played into */
    int ncomp;
    Xpost_Record *rec;
    /* where a replay of it has got to, or nothing where none has begun */
    _Walk *walk;
    /* how many times a recorded image has been painted through this
       device, which is what .recordplays answers */
    unsigned int plays;
    /* and how many recorded marks have been played through it, which is
       what .recordplayed answers */
    unsigned int played;
    /* and how many sample rows of a recorded image have gone through
       the image writer, which is what .recordimagerows answers */
    unsigned int imgrows;
    /* how many devices this record has built to paint through, which is
       what tells the one it is carrying now from one it has given up
       (.playbuilt and .playkept below) */
    unsigned int playgen;
    /* What holding this page in bands saves over holding it whole: the
       raster of the run less the raster of one band of it, in bytes, and
       zero for a page held whole. It is the whole of what a device buys
       by writing the page down instead of painting it, so it is the
       quantity anything weighing a record against a raster wants.
       Settled where the band grid is settled and handed down from there
       (.playsaving), and answered by .recordsaving. It is also what the
       record is weighed against: a record worth more than the raster it
       is saving is one to put in a file. */
    size_t saving;
    /* What one band of this device's page may cost, in bytes of raster,
       taken off the device when it was made. It is the other thing the
       record is weighed against, and the one that answers for a page
       whose bands save nothing: such a page has no raster the record
       could cost more than, so the saving above says nothing about it,
       and what bounds it is the budget its device was made to. A record
       held to it costs a budget's worth of marks whatever the drawing,
       against a band of raster that costs the same again. Nought for a
       device carrying no budget, which is a device weighed by the saving
       alone. */
    size_t budget;
    /* where this run wants its records held, and where this one is */
    int spill;
    int where;
} PrivateData;

static Xpost_Object namePrivate;
static Xpost_Object namewidth;
static Xpost_Object nameheight;
static Xpost_Object namedotcopydict;
static Xpost_Object namedotstate;
static Xpost_Object namedotplaypage;
static Xpost_Object namedotplaymake;
static Xpost_Object namedotplaydev;
static Xpost_Object namedotplayrows;
static Xpost_Object namedotplaygen;
static Xpost_Object namedotground;
static Xpost_Object namenativecolorspace;
static Xpost_Object namedotncomp;
static Xpost_Object nametextalphabits;
static Xpost_Object namedotplaytargets;
static Xpost_Object namedotplayloaders;
static Xpost_Object namedotplayclass;
static Xpost_Object nameslot[5];

/* The keys of the blit dictionary the image painter builds, which is
   what an image entry is written from and what one is played back
   through. They are taken up once, at start-up, because both directions
   walk the whole set and a lookup by text on the way past every one of
   them would be the walk's cost rather than its content. */
enum
{
    BK_ROWS, BK_DEVW, BK_DEVH, BK_NAT, BK_RGBROWS, BK_CMYK,
    BK_W, BK_H, BK_NCOMP, BK_BUF, BK_BUFS, BK_XOFF, BK_XSCALE, BK_YOFF,
    BK_YSCALE, BK_CX0, BK_CY0, BK_CX1, BK_CY1, BK_CSPANS, BK_MBITS,
    BK_MROWB, BK_MW, BK_MH, BK_MRANGES, BK_LUT, BK_DLUTS, BK_TLUT, BK_TLUTR,
    BK_TLUTG, BK_TLUTB, BK_INTERP, BK_PREV, BK_PREVS, BK_Y, BK_LAST,
    BK_HTCELL, BK_HTW, BK_HTH, BK_IMGDATA, BK_DIMENSIONS, BK_DEV,
    BK_COUNT
};

static const char *const _bdname[BK_COUNT] =
{
    "rows", "devw", "devh", "nat", "rgbrows", "cmyk",
    "w", "h", "ncomp", "buf", "bufs", "xoff", "xscale", "yoff",
    "yscale", "cx0", "cy0", "cx1", "cy1", "cspans", "mbits",
    "mrowb", "mw", "mh", "mranges", "lut", "dluts", "tlut", "tlutr",
    "tlutg", "tlutb", "interp", "prev", "prevs", "y", "last",
    ".htcell", ".htw", ".hth", "ImgData", "dimensions", "dev"
};

static Xpost_Object namebdkey[BK_COUNT];

static unsigned int _create_cont_opcode;
static unsigned int _replay_step_opcode;
static unsigned int _loadrecorddevicecont_opcode;

/* --- reaching the record a device holds ------------------------------
   The record lives outside virtual memory, named from the device's
   instance dictionary by a handle. These resolve that handle and answer
   for the case where it names nothing -- a record that has been refused a
   mark reports rather than carrying on short. */

/* The slot the device that paints is asked through for each kind of
   mark. A record holds the call, so playing it is making the call. */
static Xpost_Object _slot(Xpost_Record_Kind kind)
{
    /* The five marking calls are the only kinds played by calling a
       method. An image is written through the row writer that wrote it,
       and a screen is put back on the device rather than painted, so
       neither names a method the target declares and neither reaches
       here; answering nothing for them keeps that true of any kind
       added beside them. */
    if ((int)kind < 0
        || (size_t)kind >= sizeof nameslot / sizeof *nameslot)
        return null;
    return nameslot[(int)kind];
}

/* Load this instance's state, or answer that it has none. */
static int _private_get(Xpost_Context *ctx, Xpost_Object devdic,
                        Xpost_Object *privatestr, PrivateData *private)
{
    return xpost_dev_private_get(ctx, devdic, namePrivate, privatestr,
                                 private, sizeof *private);
}

/* Answer @p err, having said that the page this device is holding has
 * lost what the call was carrying.
 *
 * The record refuses a mark it cannot hold and remembers the refusal, so
 * that a page it could not hold whole is refused rather than put out
 * short. A mark that fails here never reaches it to be refused: the
 * device was asked to paint something and returns an error instead, and
 * the record it is keeping goes on describing a page without it. So the
 * loss is carried down to the same flag by hand and the one refusal
 * covers both.
 *
 * Which failures those are is the question this file answers site by
 * site: a call that was adding to the page loses what it was adding,
 * where a call that answers a question about a page, or paints a page
 * already held, leaves the record holding everything it held.
 *
 * A device with no state, and one whose record has been given up, hold
 * no page for anything to be missing from.
 */
static int _lost(Xpost_Context *ctx, Xpost_Object devdic, int err)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (_private_get(ctx, devdic, &privatestr, &private))
        xpost_record_lost(private.rec);
    return err;
}

/* --- deciding where the marks are kept -------------------------------
   A record is held in memory while it is worth less than the raster
   banding saves, and goes to a scratch file past that. The weighing is
   O(1): a running count of blocks, asked every so many marks and at any
   entry big enough to cross the threshold alone -- never a walk over what
   the record already holds. */

/* Which of the three states a run asked for, read from where the run's
   own decisions live.
 *
 * A run that said nothing is weighed, which is the state that touches
 * no disk for a page whose marks are worth less than the raster they
 * save -- an ordinary page, in other words. An unrecognised word is
 * refused where the selection is read (src/lib/xpost_interpreter.c), so
 * what arrives here is one of the three or nothing. */
static int _spill_asked(Xpost_Context *ctx)
{
    Xpost_Object o = xpost_context_host_setting(ctx, "RecordSpill");

    if (xpost_object_get_type(o) == nametype)
    {
        if (xpost_dict_compare_objects(ctx, o,
                                       xpost_name_cons(ctx, "never")) == 0)
            return SPILL_NEVER;
        if (xpost_dict_compare_objects(ctx, o,
                                       xpost_name_cons(ctx, "always")) == 0)
            return SPILL_ALWAYS;
    }
    return SPILL_AUTO;
}

/* Whether a spill file could be made and written, asked once for the
   process rather than once per device: it is a question about the
   machine, and a run that makes a device per page would otherwise ask it
   per page. */
static int _spill_probed;
static int _spill_probe_ok;
static char _spill_probe_why[160];
/* and whether the run has already been told it cannot have one: the
   answer is about the machine and does not change, so a job of many
   pages is told once rather than once per device it makes */
static int _spill_probe_said;

static int _spill_probe(void)
{
    if (!_spill_probed)
    {
        _spill_probe_ok = xpost_spill_probe(_spill_probe_why,
                                            sizeof _spill_probe_why);
        _spill_probed = 1;
    }
    return _spill_probe_ok;
}

/* Put the marks of this record in a file, saying so where it could not
   be done.
 *
 * A spill that fails leaves the record exactly as it was -- holding
 * everything, in memory, and able to go on -- so the page is not lost by
 * it. What is lost is the bound, and that is what the report is about:
 * the run goes on and the reader is told that it is going on unbounded.
 */
static int _spill_now(PrivateData *private)
{
    if (private->where == SPILT_FILE)
        return 1;
    if (xpost_record_spill(private->rec))
    {
        private->where = SPILT_FILE;
        return 1;
    }
    private->where = SPILT_REFUSED;
    return 0;
}

/* Weigh what this record costs against what it is buying and against
   what a band of its page was given to spend, and put the marks in a
   file where it passes either.
 *
 * The comparison is safe to act on because what crossing it commits to
 * is bounded: what a page pays for crossing is a write buffer and a
 * read window, whatever the drawing.
 *
 * Two bounds, because they answer for different pages. The saving is
 * what banding this page buys, and a record worth more than the raster
 * it is saving is one to put in a file; it is the tighter of the two for
 * a page whose bands save little, and it says nothing at all about a
 * page whose bands save nothing -- a page held whole, which has no
 * raster the record could cost more than. The budget answers for that
 * page and for every other: a record held to what a band of the page was
 * given to spend costs no more than the raster standing beside it, so
 * what such a page costs stops following its drawing whatever the saving
 * turns out to be.
 *
 * A device carrying neither is weighed by neither, and what its record
 * costs follows the drawing. That is what a run asking for its marks
 * never to be put in a file has asked for outright.
 *
 * Asked every RECORD_WEIGH_EVERY marks, and at any entry large enough to
 * carry the record past the threshold on its own. What counts the marks
 * is the record's own count rather than a tally kept here, so that
 * nothing has to be written back to the device's state on the way past
 * a mark that changed nothing.
 *
 * @return whether the state changed and has to be stored back
 */
static int _weigh(PrivateData *private, int now)
{
    size_t bytes;

    if (private->spill != SPILL_AUTO || private->where != SPILT_MEMORY)
        return 0;
    if (!now && xpost_record_count(private->rec) % RECORD_WEIGH_EVERY)
        return 0;
    bytes = xpost_record_bytes(private->rec);
    if (!(private->saving && bytes > private->saving)
        && !(private->budget && bytes > private->budget))
        return 0;
    if (!_spill_now(private))
        XPOST_LOG_ERR("a page whose marks came to more than the raster"
                      " banding it saves, or more than a band of it was"
                      " given to spend, could not put them in %s, so this"
                      " page is held in memory and what it costs follows"
                      " the drawing without limit", xpost_temp_dir());
    return 1;
}

/* Whether a rectangle covers every pixel of the page.
 *
 * The pixels it covers are taken from its operands by the normaliser the
 * fill itself takes them from, so what counts as covering here and what
 * the fill paints there are one statement rather than two that agree on
 * the cases anyone tried.
 */
static int _covers_page(const real *ops, int width, int height)
{
    int x0, y0, x1, y1;

    xpost_dev_rect_normalize((double)ops[0], (double)ops[1],
                             (double)ops[2], (double)ops[3],
                             &x0, &y0, &x1, &y1);
    return x0 <= 0 && y0 <= 0 && x1 >= width - 1 && y1 >= height - 1;
}

/* --- writing a mark down ---------------------------------------------
   The marking methods, one per thing the recorder records. There are two
   suites of them, one per colour arity, because a method's operand count
   is fixed when the class is installed and a record made in one space and
   played into a device declaring another would put each value in the place
   of a different one. */

/* Write one mark down. The colour is the components the device's space
   takes, in the range they arrived in: folding one to a channel is the
   painting device's business and is done when the mark is played, by
   whichever device plays it.
 *
 * @p ncomp is the count the entry point calling this was installed with,
 * and the record holds its marks at the count it was made with. The two
 * are one number read twice -- both come from the class -- and a mark of
 * a shape the record has no room for is refused rather than written
 * down, since a colour read back at another count is a colour nobody
 * named.
 */
static int _mark(Xpost_Context *ctx, Xpost_Object devdic,
                 Xpost_Record_Kind kind,
                 const Xpost_Object *comp, int ncomp,
                 const real *ops, int nops)
{
    Xpost_Object privatestr;
    PrivateData private;
    real colour[RECORD_MAXCOMP];
    int i;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;

    /* a released record takes no marks, as a released raster takes none */
    if (!private.rec)
        return 0;

    /* The mark was made and is not going to be written down, so the page
       is short of it however far the two counts are apart. */
    if (ncomp != private.ncomp)
    {
        XPOST_LOG_ERR("%d a mark of %d colour values is offered to a record"
                      " holding %d", rangecheck, ncomp, private.ncomp);
        xpost_record_lost(private.rec);
        return rangecheck;
    }
    for (i = 0; i < ncomp; i++)
        colour[i] = (real)xpost_object_number(comp[i]);

    /* A rectangle covering the page paints over every mark before it, so
       none of them is on the page any longer and the record gives them
       up: what it holds from here is the page this rectangle begins.
       This is where a page boundary reaches a recorder. Clearing the
       page arrives as a rectangle covering it -- the class declares no
       Erase, which is the rule that keeps a record complete -- and a
       page is cleared as it starts, so a record's lifetime is a page's
       and not a job's (doc/xpost_design.dox).

       Reading the boundary off the marks rather than off the emission is
       what keeps a page put out twice right: copypage transmits a page
       without erasing it (PLRM 8.2), and the page after it is the same
       page with more on it, so the marks before it are still the page's
       and are still held. */
    if (kind == XPOST_RECORD_FILLRECT && nops == 4
        && _covers_page(ops, private.width, private.height))
        xpost_record_clear(private.rec);

    /* The record counts a mark it refused for want of memory against
       itself. A mark whose shape it will not take it refuses without
       counting, and the page is short of that one just the same. */
    if (!xpost_record_mark(private.rec, kind, colour, ops, nops))
    {
        xpost_record_lost(private.rec);
        return xpost_record_error(private.rec);
    }
    /* and what the page now costs against what banding it buys. A mark
       carrying many operands is weighed whatever the count says, so that
       one large polygon cannot carry the record past the threshold
       between two weighings. */
    if (_weigh(&private, nops > RECORD_WEIGH_EVERY)
        && !xpost_dev_private_put(ctx, privatestr, &private, sizeof private))
        return VMerror;
    return 0;
}

/* A polygon is a point list with its subpaths separated, and the
   separators are part of the shape: the interior is settled by scanning
   the subpaths together, so a polygon written down without them replays
   as a region with its holes filled in. The run written down is the
   vertex count and then a pair per element, a separator being the pair
   the packed path already writes a subpath break as. */
static int _polymark(Xpost_Context *ctx, Xpost_Object devdic,
                     const Xpost_Object *comp, int ncomp,
                     Xpost_Object poly)
{
    real *ops;
    int n, i, ret;

    n = (int)poly.comp_.sz;
    ops = malloc((size_t)(1 + 2 * n) * sizeof *ops);
    /* the shape was to be filled and there is nowhere to gather it, so
       the page is short of the fill */
    if (!ops)
        return _lost(ctx, devdic, VMerror);
    ops[0] = (real)n;
    for (i = 0; i < n; i++)
    {
        Xpost_Object pair = xpost_array_get(ctx, poly, i);

        if (xpost_object_get_type(pair) == arraytype && pair.comp_.sz == 2)
        {
            ops[1 + 2 * i] = (real)xpost_object_number(xpost_array_get(ctx, pair, 0));
            ops[2 + 2 * i] = (real)xpost_object_number(xpost_array_get(ctx, pair, 1));
        }
        else
        {
            ops[1 + 2 * i] = XPOST_PATH_BREAK;
            ops[2 + 2 * i] = XPOST_PATH_BREAK;
        }
    }
    ret = _mark(ctx, devdic, XPOST_RECORD_FILLPOLY, comp, ncomp,
                ops, 1 + 2 * n);
    free(ops);
    return ret;
}

/* Read a pixel back. A record holds no pixel to read, so every read
   answers the ground, which is the answer the contract gives wherever a
   device holds no pixel to answer from. The values are in the channel
   scale the raster this device's page is played into stores, and there
   are as many of them as that raster's colour space takes -- a read is
   the one method whose operands do not follow the colour space, so one
   entry point serves a record of either shape and takes the count from
   the record rather than from where it was installed. */
static int _getpix(Xpost_Context *ctx,
                   Xpost_Object x, Xpost_Object y,
                   Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Xpost_Object ground;
    int i;

    (void)x;
    (void)y;
    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;

    ground = xpost_dict_get(ctx, devdic, namedotground);
    for (i = 0; i < private.ncomp; i++)
    {
        /* A page that was never cleared has no ground recorded, and
           every device a record plays into makes its raster white, so
           that is what a read off such a page owes. */
        int v = 255;

        if (xpost_object_get_type(ground) == arraytype
            && ground.comp_.sz >= (unsigned int)private.ncomp)
            v = xpost_dev_num_to_scaled(xpost_array_get(ctx, ground, i),
                                        255.0);
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(v));
    }
    return 0;
}

/*
 * The colour suite: the entry points of a record whose target declares
 * DeviceRGB, so that every mark carries three components.
 */

static int _putpix(Xpost_Context *ctx,
                   Xpost_Object red, Xpost_Object green, Xpost_Object blue,
                   Xpost_Object x, Xpost_Object y,
                   Xpost_Object devdic)
{
    Xpost_Object comp[3];
    real ops[2];

    comp[0] = red; comp[1] = green; comp[2] = blue;
    ops[0] = (real)xpost_object_number(x);
    ops[1] = (real)xpost_object_number(y);
    return _mark(ctx, devdic, XPOST_RECORD_PUTPIX, comp, 3, ops, 2);
}

static int _blendpix(Xpost_Context *ctx,
                     Xpost_Object red, Xpost_Object green, Xpost_Object blue,
                     Xpost_Object cov, Xpost_Object x, Xpost_Object y,
                     Xpost_Object devdic)
{
    Xpost_Object comp[3];
    real ops[3];

    comp[0] = red; comp[1] = green; comp[2] = blue;
    ops[0] = (real)xpost_object_number(cov);
    ops[1] = (real)xpost_object_number(x);
    ops[2] = (real)xpost_object_number(y);
    return _mark(ctx, devdic, XPOST_RECORD_BLENDPIX, comp, 3, ops, 3);
}

static int _drawline(Xpost_Context *ctx,
                     Xpost_Object red, Xpost_Object green, Xpost_Object blue,
                     Xpost_Object x1, Xpost_Object y1,
                     Xpost_Object x2, Xpost_Object y2,
                     Xpost_Object devdic)
{
    Xpost_Object comp[3];
    real ops[4];

    comp[0] = red; comp[1] = green; comp[2] = blue;
    ops[0] = (real)xpost_object_number(x1);
    ops[1] = (real)xpost_object_number(y1);
    ops[2] = (real)xpost_object_number(x2);
    ops[3] = (real)xpost_object_number(y2);
    return _mark(ctx, devdic, XPOST_RECORD_DRAWLINE, comp, 3, ops, 4);
}

static int _fillrect(Xpost_Context *ctx,
                     Xpost_Object red, Xpost_Object green, Xpost_Object blue,
                     Xpost_Object x, Xpost_Object y,
                     Xpost_Object w, Xpost_Object h,
                     Xpost_Object devdic)
{
    Xpost_Object comp[3];
    real ops[4];

    comp[0] = red; comp[1] = green; comp[2] = blue;
    ops[0] = (real)xpost_object_number(x);
    ops[1] = (real)xpost_object_number(y);
    ops[2] = (real)xpost_object_number(w);
    ops[3] = (real)xpost_object_number(h);
    return _mark(ctx, devdic, XPOST_RECORD_FILLRECT, comp, 3, ops, 4);
}

static int _fillpoly(Xpost_Context *ctx,
                     Xpost_Object red, Xpost_Object green, Xpost_Object blue,
                     Xpost_Object poly,
                     Xpost_Object devdic)
{
    Xpost_Object comp[3];

    comp[0] = red; comp[1] = green; comp[2] = blue;
    return _polymark(ctx, devdic, comp, 3, poly);
}

/*
 * The grayscale suite: the same five marks, for a record whose target
 * declares DeviceGray and whose marks therefore carry one component.
 *
 * A method's operand count follows the device's colour space, and it is
 * fixed when the class is installed (xpost_dev_class_install,
 * src/lib/xpost_dev_driver.h) -- so an entry point is written for a
 * count rather than told one, and these are the same calls at the other
 * count. What each does with what it received is the shared code above.
 */

static int _putpix_g(Xpost_Context *ctx,
                     Xpost_Object grey,
                     Xpost_Object x, Xpost_Object y,
                     Xpost_Object devdic)
{
    Xpost_Object comp[1];
    real ops[2];

    comp[0] = grey;
    ops[0] = (real)xpost_object_number(x);
    ops[1] = (real)xpost_object_number(y);
    return _mark(ctx, devdic, XPOST_RECORD_PUTPIX, comp, 1, ops, 2);
}

static int _blendpix_g(Xpost_Context *ctx,
                       Xpost_Object grey,
                       Xpost_Object cov, Xpost_Object x, Xpost_Object y,
                       Xpost_Object devdic)
{
    Xpost_Object comp[1];
    real ops[3];

    comp[0] = grey;
    ops[0] = (real)xpost_object_number(cov);
    ops[1] = (real)xpost_object_number(x);
    ops[2] = (real)xpost_object_number(y);
    return _mark(ctx, devdic, XPOST_RECORD_BLENDPIX, comp, 1, ops, 3);
}

static int _drawline_g(Xpost_Context *ctx,
                       Xpost_Object grey,
                       Xpost_Object x1, Xpost_Object y1,
                       Xpost_Object x2, Xpost_Object y2,
                       Xpost_Object devdic)
{
    Xpost_Object comp[1];
    real ops[4];

    comp[0] = grey;
    ops[0] = (real)xpost_object_number(x1);
    ops[1] = (real)xpost_object_number(y1);
    ops[2] = (real)xpost_object_number(x2);
    ops[3] = (real)xpost_object_number(y2);
    return _mark(ctx, devdic, XPOST_RECORD_DRAWLINE, comp, 1, ops, 4);
}

static int _fillrect_g(Xpost_Context *ctx,
                       Xpost_Object grey,
                       Xpost_Object x, Xpost_Object y,
                       Xpost_Object w, Xpost_Object h,
                       Xpost_Object devdic)
{
    Xpost_Object comp[1];
    real ops[4];

    comp[0] = grey;
    ops[0] = (real)xpost_object_number(x);
    ops[1] = (real)xpost_object_number(y);
    ops[2] = (real)xpost_object_number(w);
    ops[3] = (real)xpost_object_number(h);
    return _mark(ctx, devdic, XPOST_RECORD_FILLRECT, comp, 1, ops, 4);
}

static int _fillpoly_g(Xpost_Context *ctx,
                       Xpost_Object grey,
                       Xpost_Object poly,
                       Xpost_Object devdic)
{
    Xpost_Object comp[1];

    comp[0] = grey;
    return _polymark(ctx, devdic, comp, 1, poly);
}

/* --- pictures, glyphs and placed drawings ----------------------------
   The entries that are bulk pixels rather than geometry. Each is written
   down once and re-emitted per band, which is the whole reason a record
   costs less than the raster it stands in for. */

/* What the blit dictionary carries, read out by key. A key it does not
   carry answers the default, which is what the row writer makes of one
   that is absent. */
static Xpost_Object _bdget(Xpost_Context *ctx, Xpost_Object bd, int k)
{
    return xpost_dict_get(ctx, bd, namebdkey[k]);
}

static int _bdint(Xpost_Context *ctx, Xpost_Object bd, int k, int dflt)
{
    Xpost_Object o = _bdget(ctx, bd, k);
    int t = xpost_object_get_type(o);

    if (t == integertype || t == booleantype)
        return o.int_.val;
    return dflt;
}

static real _bdreal(Xpost_Context *ctx, Xpost_Object bd, int k, real dflt)
{
    return (real)xpost_dev_dict_number(ctx, bd, namebdkey[k], (double)dflt);
}

/* A string the dictionary carries, as bytes, or nothing where it does
   not carry one long enough to be read as far as it will be read. */
static const unsigned char *_bdstr(Xpost_Context *ctx, Xpost_Object bd,
                                   int k, unsigned int need)
{
    Xpost_Object o = _bdget(ctx, bd, k);

    if (xpost_object_get_type(o) != stringtype || o.comp_.sz < need)
        return NULL;
    return (const unsigned char *)xpost_string_get_pointer(ctx, o);
}

/* blitdict rows IMAGE  .recordimage  -
   Write a sampled image down as one entry, rather than as the run of
   one-pixel rectangles a device holding no rows of its own would
   otherwise be painted it a sample at a time.
 *
 * What arrives is the dictionary the image painter builds before it
 * writes its first row and the rows it would have written: the
 * transform placing the image, the region resolved above the device,
 * and the colour tables the painter baked out of the graphics state.
 * The tables are what is kept rather than the state they came from --
 * a replay happens when the page is put out, by which time the
 * transfer functions and the colour space that decoded the image have
 * moved on, and there is no asking them again.
 */
static int _recordimage(Xpost_Context *ctx,
                        Xpost_Object bd,
                        Xpost_Object rows,
                        Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Xpost_Record_Image img;
    Xpost_Object o;
    const unsigned char **runs = NULL;
    unsigned char dl[4 * 256];
    unsigned char tl[3 * 256];
    int mranges[8];
    real *cspans = NULL;
    unsigned int nrun;
    int i, ret = 0;

    memset(&img, 0, sizeof img);
    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    /* a released record takes an image as it takes a mark: not at all */
    if (!private.rec)
        return 0;

    img.width = _bdint(ctx, bd, BK_W, 0);
    img.ncomp = _bdint(ctx, bd, BK_NCOMP, 0);
    img.nat = _bdint(ctx, bd, BK_NAT, 0);
    img.rgbrows = _bdint(ctx, bd, BK_RGBROWS, 0);
    img.cmyk = _bdint(ctx, bd, BK_CMYK, 0);
    img.interp = _bdint(ctx, bd, BK_INTERP, 0);
    img.xoff = _bdreal(ctx, bd, BK_XOFF, 0);
    img.xscale = _bdreal(ctx, bd, BK_XSCALE, 1);
    img.yoff = _bdreal(ctx, bd, BK_YOFF, 0);
    img.yscale = _bdreal(ctx, bd, BK_YSCALE, 1);
    img.cx0 = _bdreal(ctx, bd, BK_CX0, 0);
    img.cy0 = _bdreal(ctx, bd, BK_CY0, 0);
    img.cx1 = _bdreal(ctx, bd, BK_CX1, 0);
    img.cy1 = _bdreal(ctx, bd, BK_CY1, 0);
    img.mrowb = _bdint(ctx, bd, BK_MROWB, 0);
    img.mw = _bdint(ctx, bd, BK_MW, 0);
    img.mh = _bdint(ctx, bd, BK_MH, 0);

    /* one buffer a component says the rows come a plane at a time, and
       the run handed over is then a plane rather than a row */
    img.planar = xpost_object_get_type(_bdget(ctx, bd, BK_BUFS)) == arraytype;

    /* Whether what arrived describes a picture at all, which is settled
       before anything is taken from it. A description with no
       components, no width, no depth or no rows is nothing the caller
       was about to paint, so refusing it leaves the page as it was and
       the record holding everything it holds -- unlike every refusal
       past this point, each of which is a picture the page has lost. */
    if (img.ncomp < 1 || img.ncomp > 4 || img.width < 1
     || img.nat < 1 || img.nat > 3)
        return rangecheck;

    if (xpost_object_get_type(rows) != arraytype)
        return typecheck;
    nrun = rows.comp_.sz;
    if (nrun < 1)
        return rangecheck;
    img.height = (int)(img.planar ? nrun / (unsigned int)img.ncomp : nrun);
    if (img.height < 1)
        return rangecheck;

    /* The rows are the painter's own buffers, refilled for the row
       after, so what is handed over is where each one is now and the
       record takes its own copy. */
    runs = malloc((size_t)nrun * sizeof *runs);
    if (!runs)
    {
        ret = VMerror;
        goto out;
    }
    for (i = 0; i < (int)nrun; i++)
    {
        /* 64-bit: img.width is unbounded here (only < 1 was refused), so a
           32-bit product wraps -- width 2^30 by 4 components comes to zero,
           and a tiny row then passes a check it should fail, after which
           the record layer copies the true (huge) length off the end of
           the row. The comparison is against a composite size, so a need
           past what any string holds simply refuses every row. */
        size_t need = (size_t)img.width
                    * (img.planar ? (size_t)1 : (size_t)img.ncomp);

        o = xpost_array_get(ctx, rows, i);
        if (xpost_object_get_type(o) != stringtype || o.comp_.sz < need)
        {
            ret = typecheck;
            goto out;
        }
        runs[i] = (const unsigned char *)xpost_string_get_pointer(ctx, o);
    }

    /* the tables the painter baked: one for a single-component space,
       which has everything in it, or per-component decode with the
       transfer applied after the conversion */
    img.lut = img.ncomp == 1
        ? _bdstr(ctx, bd, BK_LUT, 256u * (unsigned int)img.nat) : NULL;
    if (img.ncomp == 1 && !img.lut)
    {
        /* an entry with no table to decode by is one the writer would
           refuse when it came to be played, which is a page short of a
           mark rather than a call that failed */
        ret = typecheck;
        goto out;
    }
    if (img.ncomp > 1)
    {
        o = _bdget(ctx, bd, BK_DLUTS);
        if (xpost_object_get_type(o) != arraytype
         || o.comp_.sz < (unsigned int)img.ncomp)
        {
            ret = typecheck;
            goto out;
        }
        for (i = 0; i < img.ncomp; i++)
        {
            Xpost_Object d = xpost_array_get(ctx, o, i);

            if (xpost_object_get_type(d) != stringtype || d.comp_.sz < 256)
            {
                ret = typecheck;
                goto out;
            }
            memcpy(dl + i * 256, xpost_string_get_pointer(ctx, d), 256);
        }
        img.dluts = dl;
    }
    img.tlut = _bdstr(ctx, bd, BK_TLUT, 256u);
    if (img.ncomp > 1 && !img.tlut)
    {
        ret = typecheck;
        goto out;
    }
    if (img.nat == 3)
    {
        const unsigned char *p;
        int k;

        for (k = 0; k < 3; k++)
        {
            p = _bdstr(ctx, bd, BK_TLUTR + k, 256u);
            if (!p)
                break;
            memcpy(tl + k * 256, p, 256);
        }
        if (k == 3)
            img.tlutrgb = tl;
    }

    /* 64-bit, then held to what a string can carry: mrowb and the mask
       row count are both unbounded ints from the dictionary, so their
       32-bit product wraps and a small size is validated against the
       mask string while the record layer copies the true (huge) one off
       the end. A size no string can hold means there is no mask to
       take, and so does a mask grid with no samples in it. */
    if (img.mrowb > 0 && img.mw > 0 && img.mh > 0)
    {
        size_t mneed = (size_t)img.mrowb * (size_t)img.mh;

        img.mbits = mneed <= (size_t)XPOST_OBJECT_COMP_MAX_SZ
            ? _bdstr(ctx, bd, BK_MBITS, (unsigned int)mneed) : NULL;
    }
    else
        img.mbits = NULL;
    if (!img.mbits)
    {
        img.mrowb = 0;
        img.mw = 0;
        img.mh = 0;
    }

    o = _bdget(ctx, bd, BK_MRANGES);
    if (xpost_object_get_type(o) == arraytype && o.comp_.sz <= 8)
    {
        for (i = 0; i < (int)o.comp_.sz; i++)
            mranges[i] = (int)xpost_object_number(xpost_array_get(ctx, o, i));
        img.nranges = (int)o.comp_.sz;
        img.mranges = mranges;
    }

    o = _bdget(ctx, bd, BK_CSPANS);
    if (xpost_object_get_type(o) == arraytype && o.comp_.sz >= 4)
    {
        img.nspan = (int)(o.comp_.sz / 4);
        cspans = malloc((size_t)img.nspan * 4 * sizeof *cspans);
        if (!cspans)
        {
            ret = VMerror;
            goto out;
        }
        for (i = 0; i < img.nspan * 4; i++)
            cspans[i] = (real)xpost_object_number(xpost_array_get(ctx, o, i));
        img.cspans = cspans;
    }

    if (!xpost_record_image(private.rec, &img, runs, (int)nrun))
        ret = xpost_record_error(private.rec);
    /* a picture is the samples rather than a call and carries the record
       further than any mark, so it is weighed where it arrives */
    else if (_weigh(&private, 1)
             && !xpost_dev_private_put(ctx, privatestr, &private,
                                       sizeof private))
        ret = VMerror;

out:
    /* Every way out from here is a picture the caller was painting and
       the record does not hold: the rows it could not read, a table
       without which the writer would refuse the entry when it came to be
       played, the memory it could not find, or the record's own refusal.
       The page is short of the picture in each case, so it is refused
       rather than put out without it. */
    if (ret)
        xpost_record_lost(private.rec);
    free(cspans);
    free(runs);
    return ret;
}

/* Paint an image entry into the device that paints, for device rows
   @p lo to @p hi.
 *
 * The rows are written through the same writer that wrote them the
 * first time, driven against the target's raster: a second
 * implementation of sampling would be a second set of rounding
 * decisions and the two pages would part company somewhere nobody
 * looked.
 *
 * Where that target keeps its raster in a buffer of its own it has no
 * rows to be written into, and the writer is driven against the device
 * itself: the same sampling, handed to the target's own rectangle fill
 * one run of columns at a time. What decides which of the two a target
 * takes is the target -- a device holding rows is written into them --
 * and the picture is the same picture either way, since only the last
 * step of the one writer differs.
 *
 * The run of rows is clipped to by choosing which of the image's rows
 * to write and by narrowing the region they are written through -- not
 * by trimming what was recorded, which would leave a record that could
 * only be played back one way.
 */
static int _play_image(Xpost_Context *ctx,
                       const Xpost_Record *rec, size_t which,
                       const Xpost_Record_Image *img,
                       Xpost_Object targetdic,
                       real lo, real hi,
                       int *nrows)
{
    Xpost_Object bd, rows, dims;
    Xpost_Object buf = null, prev = null;
    Xpost_Object bufs = null, prevs = null;
    unsigned int mode = ctx->vmmode;
    unsigned int rowbytes;
    real cy0, cy1;
    int devh, y, y0, y1, c;
    int ret = 0;

    rows = xpost_dict_get(ctx, targetdic, namebdkey[BK_IMGDATA]);
    if (xpost_object_get_type(rows) != arraytype
        && xpost_object_get_type(
               xpost_dict_get(ctx, targetdic, _slot(XPOST_RECORD_FILLRECT)))
           != operatortype)
    {
        /* the device being played into keeps no rows an image can be
           written into and declares no compiled fill it can be painted
           into a span at a time, so this mark cannot be made -- the
           same answer as a device missing one of the marking
           methods */
        XPOST_LOG_ERR("%d a recorded image has nowhere to play it into",
                      undefined);
        return undefined;
    }
    dims = xpost_dict_get(ctx, targetdic, namebdkey[BK_DIMENSIONS]);
    if (xpost_object_get_type(dims) != arraytype || dims.comp_.sz < 2)
        return typecheck;
    devh = (int)xpost_object_number(xpost_array_get(ctx, dims, 1));

    *nrows = 0;
    if (!xpost_record_image_rows(img, lo, hi, &y0, &y1))
        return 0;
    /* The sample rows this run of the page's rows takes, which is what
       a band of a picture costs against the whole of it. A run the
       picture's rows do not reach names an empty span rather than a
       span running backwards, and an empty one costs nothing. */
    *nrows = y1 > y0 ? y1 - y0 : 0;
    cy0 = img->cy0 < lo ? lo : img->cy0;
    cy1 = img->cy1 > hi + 1 ? hi + 1 : img->cy1;

    rowbytes = (unsigned int)img->width
             * (img->planar ? 1u : (unsigned int)img->ncomp);

    /* Built in local memory whatever the run was allocating in: it
       lives as long as this call, and global memory is not collected. */
    ctx->vmmode = LOCAL;
    bd = xpost_dict_cons(ctx, 40);
    if (xpost_object_get_type(bd) != dicttype)
    {
        ctx->vmmode = mode;
        return VMerror;
    }

#define PUT(k, v) do { \
        ret = xpost_dict_put(ctx, bd, namebdkey[k], (v)); \
        if (ret) goto out; \
    } while (0)
#define PUTSTR(k, p, n) do { \
        Xpost_Object s_ = xpost_string_cons(ctx, (unsigned int)(n), \
                                            (const char *)(p)); \
        if (xpost_object_get_type(s_) != stringtype) \
            { ret = VMerror; goto out; } \
        PUT(k, s_); \
    } while (0)

    /* the rows the picture is written into, or the device it is
       painted into where it keeps none */
    if (xpost_object_get_type(rows) == arraytype)
        PUT(BK_ROWS, rows);
    else
        PUT(BK_DEV, targetdic);
    PUT(BK_DEVW, xpost_array_get(ctx, dims, 0));
    PUT(BK_DEVH, xpost_int_cons(devh));
    PUT(BK_NAT, xpost_int_cons(img->nat));
    PUT(BK_RGBROWS, xpost_bool_cons(img->rgbrows));
    PUT(BK_CMYK, xpost_bool_cons(img->cmyk));
    PUT(BK_W, xpost_int_cons(img->width));
    PUT(BK_H, xpost_int_cons(img->height));
    PUT(BK_NCOMP, xpost_int_cons(img->ncomp));
    PUT(BK_XOFF, xpost_real_cons(img->xoff));
    PUT(BK_XSCALE, xpost_real_cons(img->xscale));
    PUT(BK_YOFF, xpost_real_cons(img->yoff));
    PUT(BK_YSCALE, xpost_real_cons(img->yscale));
    PUT(BK_CX0, xpost_real_cons(img->cx0));
    PUT(BK_CY0, xpost_real_cons(cy0));
    PUT(BK_CX1, xpost_real_cons(img->cx1));
    PUT(BK_CY1, xpost_real_cons(cy1));

    if (img->lut)
        PUTSTR(BK_LUT, img->lut, 256 * img->nat);
    if (img->dluts)
    {
        Xpost_Object a = xpost_array_cons(ctx, (unsigned int)img->ncomp);

        if (xpost_object_get_type(a) != arraytype)
            { ret = VMerror; goto out; }
        for (c = 0; c < img->ncomp; c++)
        {
            Xpost_Object s = xpost_string_cons(ctx, 256,
                (const char *)(img->dluts + c * 256));

            if (xpost_object_get_type(s) != stringtype)
                { ret = VMerror; goto out; }
            ret = xpost_array_put(ctx, a, c, s);
            if (ret)
                goto out;
        }
        PUT(BK_DLUTS, a);
    }
    if (img->tlut)
        PUTSTR(BK_TLUT, img->tlut, 256);
    if (img->tlutrgb)
    {
        PUTSTR(BK_TLUTR, img->tlutrgb, 256);
        PUTSTR(BK_TLUTG, img->tlutrgb + 256, 256);
        PUTSTR(BK_TLUTB, img->tlutrgb + 512, 256);
    }
    if (img->mrowb > 0)
    {
        const unsigned char *bits = xpost_record_image_mbits(rec, which);

        if (bits)
        {
            PUTSTR(BK_MBITS, bits, (size_t)img->mrowb * img->mh);
            PUT(BK_MROWB, xpost_int_cons(img->mrowb));
            PUT(BK_MW, xpost_int_cons(img->mw));
            PUT(BK_MH, xpost_int_cons(img->mh));
        }
    }
    if (img->nranges)
    {
        Xpost_Object a = xpost_array_cons(ctx, (unsigned int)img->nranges);

        if (xpost_object_get_type(a) != arraytype)
            { ret = VMerror; goto out; }
        for (c = 0; c < img->nranges; c++)
        {
            ret = xpost_array_put(ctx, a, c, xpost_int_cons(img->mranges[c]));
            if (ret)
                goto out;
        }
        PUT(BK_MRANGES, a);
    }
    if (img->nspan)
    {
        Xpost_Object a = xpost_array_cons(ctx, (unsigned int)img->nspan * 4);

        if (xpost_object_get_type(a) != arraytype)
            { ret = VMerror; goto out; }
        for (c = 0; c < img->nspan * 4; c++)
        {
            ret = xpost_array_put(ctx, a, c,
                                  xpost_real_cons(img->cspans[c]));
            if (ret)
                goto out;
        }
        PUT(BK_CSPANS, a);
    }
    /* the screen a bilevel device thresholds through is that device's
       and not the image's, so it is taken from the one being painted --
       from the state beside it, which is where what a page did to a
       device is kept (data/device.ps) */
    {
        Xpost_Object st = xpost_dict_get(ctx, targetdic, namedotstate);

        if (xpost_object_get_type(st) == dicttype
            && xpost_object_get_type(
                   xpost_dict_get(ctx, st, namebdkey[BK_HTCELL])) == stringtype)
        {
            PUT(BK_HTCELL, xpost_dict_get(ctx, st, namebdkey[BK_HTCELL]));
            PUT(BK_HTW, xpost_dict_get(ctx, st, namebdkey[BK_HTW]));
            PUT(BK_HTH, xpost_dict_get(ctx, st, namebdkey[BK_HTH]));
        }
    }

    /* the row in hand, and -- where the samples are blended -- the row
       before it. The first row blends with itself, which is what the
       painting did with the first row it had. */
    if (img->planar)
    {
        bufs = xpost_array_cons(ctx, (unsigned int)img->ncomp);
        if (img->interp)
            prevs = xpost_array_cons(ctx, (unsigned int)img->ncomp);
        if (xpost_object_get_type(bufs) != arraytype
         || (img->interp && xpost_object_get_type(prevs) != arraytype))
            { ret = VMerror; goto out; }
        for (c = 0; c < img->ncomp; c++)
        {
            Xpost_Object s = xpost_string_cons(ctx, rowbytes, NULL);
            Xpost_Object p = img->interp
                ? xpost_string_cons(ctx, rowbytes, NULL) : null;

            if (xpost_object_get_type(s) != stringtype
             || (img->interp && xpost_object_get_type(p) != stringtype))
                { ret = VMerror; goto out; }
            ret = xpost_array_put(ctx, bufs, c, s);
            if (!ret && img->interp)
                ret = xpost_array_put(ctx, prevs, c, p);
            if (ret)
                goto out;
        }
        PUT(BK_BUFS, bufs);
        if (img->interp)
            PUT(BK_PREVS, prevs);
    }
    else
    {
        buf = xpost_string_cons(ctx, rowbytes, NULL);
        if (xpost_object_get_type(buf) != stringtype)
            { ret = VMerror; goto out; }
        PUT(BK_BUF, buf);
        if (img->interp)
        {
            prev = xpost_string_cons(ctx, rowbytes, NULL);
            if (xpost_object_get_type(prev) != stringtype)
                { ret = VMerror; goto out; }
            PUT(BK_PREV, prev);
        }
    }
    if (img->interp)
        PUT(BK_INTERP, xpost_bool_cons(1));

    /* the two the loop restates, put here so that the dictionary has
       the shape it will keep before any row buffer is filled: a key
       arriving later could grow it, and what a row was written into is
       read back by where it is rather than by where it was */
    PUT(BK_Y, xpost_int_cons(0));
    if (img->interp)
        PUT(BK_LAST, xpost_bool_cons(0));

    /* The rows are taken one run at a time, which is the shape the writer
       below wants and is also what holds a picture's residency to the
       run in hand: a record that has spilled reads the run it is asked
       for and holds no more of the picture than that. Each run is copied
       into the buffer the writer fills before the next is asked for,
       since the one a spilled record answers with is the one place it
       reads a run into. */
    for (y = y0; y < y1; y++)
    {
        int p = y > 0 ? y - 1 : 0;
        const unsigned char *run;

        if (img->planar)
        {
            for (c = 0; c < img->ncomp; c++)
            {
                run = xpost_record_image_run(rec, which, y * img->ncomp + c);
                if (!run)
                    { ret = ioerror; goto out; }
                memcpy(xpost_string_get_pointer(ctx,
                           xpost_array_get(ctx, bufs, c)), run, rowbytes);
                if (img->interp)
                {
                    run = xpost_record_image_run(rec, which,
                                                 p * img->ncomp + c);
                    if (!run)
                        { ret = ioerror; goto out; }
                    memcpy(xpost_string_get_pointer(ctx,
                               xpost_array_get(ctx, prevs, c)), run, rowbytes);
                }
            }
        }
        else
        {
            run = xpost_record_image_run(rec, which, y);
            if (!run)
                { ret = ioerror; goto out; }
            memcpy(xpost_string_get_pointer(ctx, buf), run, rowbytes);
            if (img->interp)
            {
                run = xpost_record_image_run(rec, which, p);
                if (!run)
                    { ret = ioerror; goto out; }
                memcpy(xpost_string_get_pointer(ctx, prev), run, rowbytes);
            }
        }
        PUT(BK_Y, xpost_int_cons(y));
        if (img->interp)
            PUT(BK_LAST, xpost_bool_cons(y == img->height - 1));
        ret = xpost_dev_blit_row(ctx, bd);
        if (ret)
            goto out;
    }

#undef PUTSTR
#undef PUT
out:
    ctx->vmmode = mode;
    return ret;
}

/* Hands a glyph's coverage mask to the record and answers where it was
   put. The caller keeps no copy: what comes back is an offset into the
   record, and a mask that could not be taken is answered as such
   rather than raising, because a glyph the page will be short of is
   not an error the job can act on. */
int xpost_dev_record_takemask(Xpost_Context *ctx, Xpost_Object devdic,
                              const unsigned char *cov, int w, int h,
                              size_t *at)
{
    Xpost_Object privatestr;
    PrivateData private;

    /* a caller with no mask to hand over had no glyph to place, so this
       takes nothing from the page and the record is left as it is */
    if (!cov || !at)
        return rangecheck;
    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    /* a released record takes a mask as it takes a mark: not at all */
    if (!private.rec)
        return undefined;
    /* A mask that could not be taken up is a glyph that will not be
       placed: the caller reads this answer and marks nothing rather than
       naming a mask the record does not hold, and says nothing further
       about it. So the page is short of the glyph, and this is where
       that is said -- an unplaced glyph raises no error of its own. */
    if (!xpost_record_mask(private.rec, cov, w, h, at))
    {
        xpost_record_lost(private.rec);
        return xpost_record_error(private.rec);
    }
    /* a coverage mask is bytes rather than a call and carries the record
       further than a mark does, so it is weighed where it arrives */
    if (_weigh(&private, 1)
        && !xpost_dev_private_put(ctx, privatestr, &private, sizeof private))
        return VMerror;
    return 0;
}

/* Write down one placement of a coverage mask the record already holds.
 *
 * The colour is the components the device's space takes and is carried
 * once for the whole mask, the coverage being in the mask: which is the
 * shape the painting has, since one colour is in force for a glyph and
 * each of its pixels is covered as much as it is covered.
 *
 * @p ncomp is the count the entry point calling this was installed with,
 * and is checked against the record's for the reason a mark's is.
 */
static int _glyphmark(Xpost_Context *ctx, Xpost_Object devdic,
                      const Xpost_Object *comp, int ncomp,
                      Xpost_Object at, Xpost_Object x, Xpost_Object y)
{
    Xpost_Object privatestr;
    PrivateData private;
    real colour[RECORD_MAXCOMP];
    integer i;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;

    /* a released record takes no glyph, as it takes no mark */
    if (!private.rec)
        return 0;

    /* the glyph was painted and is not going to be written down, so the
       page is short of it, as it is of a mark of the wrong shape */
    if (ncomp != private.ncomp)
    {
        XPOST_LOG_ERR("%d a glyph of %d colour values is offered to a record"
                      " holding %d", rangecheck, ncomp, private.ncomp);
        xpost_record_lost(private.rec);
        return rangecheck;
    }
    for (i = 0; i < ncomp; i++)
        colour[i] = (real)xpost_object_number(comp[i]);

    /* A placement naming no mask the record holds replays as nothing,
       which is a page missing a glyph. The record says so of an index
       past the masks it holds; an index below them is the same defect at
       the other end and is answered the same way. */
    i = xpost_dev_num_to_int(at);
    if (i < 0)
    {
        xpost_record_lost(private.rec);
        return rangecheck;
    }
    if (!xpost_record_glyph(private.rec, colour, (size_t)i,
                            (real)xpost_object_number(x),
                            (real)xpost_object_number(y)))
    {
        xpost_record_lost(private.rec);
        return xpost_record_error(private.rec);
    }
    if (_weigh(&private, 0)
        && !xpost_dev_private_put(ctx, privatestr, &private, sizeof private))
        return VMerror;
    return 0;
}

/* r g b at x y IMAGE  .recordglyph  -
   ... and the same at the other colour count. */
static int _recordglyph(Xpost_Context *ctx,
                        Xpost_Object red, Xpost_Object green, Xpost_Object blue,
                        Xpost_Object at, Xpost_Object x, Xpost_Object y,
                        Xpost_Object devdic)
{
    Xpost_Object comp[3];

    comp[0] = red; comp[1] = green; comp[2] = blue;
    return _glyphmark(ctx, devdic, comp, 3, at, x, y);
}

static int _recordglyph_g(Xpost_Context *ctx,
                          Xpost_Object grey,
                          Xpost_Object at, Xpost_Object x, Xpost_Object y,
                          Xpost_Object devdic)
{
    Xpost_Object comp[1];

    comp[0] = grey;
    return _glyphmark(ctx, devdic, comp, 1, at, x, y);
}

/* subdev dx dy IMAGE  .recordplace  -
 * Write down a placement of the drawing another recorder holds.
 *
 * What is written down is a reference: the drawing is held once however
 * many places it is put and however deep it sits, and what this page
 * pays for the placement is the mark. The drawing is played where the
 * page is played, at the offset given, so a drawing made once is painted
 * at every place it was put and at the phase each of those places falls
 * at.
 *
 * The two recorders must hold their marks in the same colour space, for
 * the reason a record must be played into a device declaring the space
 * it was made in: a mark carries one value per component, and a drawing
 * whose marks carry another count would paint a colour nobody named.
 *
 * A drawing that cannot be placed is a mark the page has lost, and is
 * answered as one: the page is refused rather than put out without it.
 */
static int _recordplace(Xpost_Context *ctx,
                        Xpost_Object subdic,
                        Xpost_Object dx, Xpost_Object dy,
                        Xpost_Object devdic)
{
    Xpost_Object privatestr, substr;
    PrivateData private, subprivate;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    /* a released record takes no placement, as it takes no mark */
    if (!private.rec)
        return 0;
    if (!_private_get(ctx, subdic, &substr, &subprivate)
        || !subprivate.rec)
    {
        XPOST_LOG_ERR("%d a placement names no drawing", undefined);
        return _lost(ctx, devdic, undefined);
    }
    if (subprivate.ncomp != private.ncomp
        || xpost_dict_compare_objects(
               ctx, xpost_dict_get(ctx, devdic, namenativecolorspace),
               xpost_dict_get(ctx, subdic, namenativecolorspace)) != 0)
    {
        XPOST_LOG_ERR("%d a drawing made in one colour space is placed in a"
                      " page made in another", rangecheck);
        return _lost(ctx, devdic, rangecheck);
    }
    /* A page placing itself, or placing a drawing nested as deep as a
       replay descends, is refused here rather than when the page is
       painted: the run making the placement is still in a position to
       hear about it. */
    if (subprivate.rec == private.rec
        || xpost_record_depth(subprivate.rec) >= XPOST_RECORD_NEST)
    {
        XPOST_LOG_ERR("%d a page places drawings deeper than a replay"
                      " descends", limitcheck);
        return _lost(ctx, devdic, limitcheck);
    }
    if (!xpost_record_place(private.rec, subprivate.rec,
                            (real)xpost_object_number(dx),
                            (real)xpost_object_number(dy)))
        return _lost(ctx, devdic, VMerror);
    if (_weigh(&private, 0)
        && !xpost_dev_private_put(ctx, privatestr, &private, sizeof private))
        return VMerror;
    return 0;
}

/* IMAGE  .recordplaces  drawings
   How many distinct drawings a record places, which is the count a
   caller asking whether a drawing is held once rather than once per
   place wants. */
static int _recordplaces(Xpost_Context *ctx,
                         Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)
                                    xpost_record_place_count(private.rec)));
    return 0;
}

/* --- what a record can be asked about itself -------------------------
   The operators the machinery above uses to decide things: what the record
   cost, what banding it would save, whether anything was cut on the way
   in, where the marks are being kept. */

/* IMAGE  .recordbox  x0 y0 x1 y1 true
                     false
   The box the marks a record holds reach, in the coordinates they were
   made in, or false where it holds none.

   What it is for is the question a caller holding a drawing has to ask
   before placing it: a drawing is placed whole, and nothing clips it
   where it lands, so a drawing reaching outside the region its maker
   was cut to is a drawing that would paint outside that region wherever
   it is put. */
static int _recordbox(Xpost_Context *ctx,
                      Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    real x0, y0, x1, y1;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    if (!private.rec || !xpost_record_box(private.rec, &x0, &y0, &x1, &y1))
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x0));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(y0));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x1));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(y1));
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
}

/* IMAGE  .recordcost  marks images bytes
   What a record holds and what holding it costs. The mechanism is worth
   having exactly while a record is smaller than the raster it saves
   holding, which is a measurement and not a guess, and this is where
   the measurement is taken. */
static int _recordcost(Xpost_Context *ctx,
                       Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)xpost_record_count(private.rec)));
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)xpost_record_image_count(private.rec)));
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)xpost_record_bytes(private.rec)));
    return 0;
}

/* IMAGE bytes  .playsaving  -
   What holding this page in bands saves over holding it whole, handed
   down from where the band grid is settled (.playmake, data/recorddev.ps)
   to the device holding the record.

   It is one number read twice rather than two that agree: the saving is
   the raster of the run less the raster of one band of it, and how deep
   a band is is the reading the band loop itself goes by. A page held
   whole saves nothing and is handed nothing. */
static int _playsaving(Xpost_Context *ctx,
                       Xpost_Object devdic,
                       Xpost_Object n)
{
    Xpost_Object privatestr;
    PrivateData private;
    double v;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    v = xpost_object_number(n);
    private.saving = v > 0 ? (size_t)v : 0;
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof private))
        return VMerror;
    return 0;
}

/* IMAGE  .recordsaving  bytes
   What holding this page in bands saves over holding it whole, which is
   what holding the record instead is worth. Zero for a page held whole,
   which saves nothing.

   The other side of the comparison .recordcost states: what a record
   costs against what having it is buying. */
static int _recordsaving(Xpost_Context *ctx,
                         Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)private.saving));
    return 0;
}

/* IMAGE  .recordspill  /asked /where
   Where this run wants its records held, and where this one's marks
   actually are.

   Two answers rather than one, because they can differ and the
   difference is the interesting case: a run that asked for its records
   to be weighed, on a machine with no scratch space, is holding its
   marks in memory and has been told so, and a reader given only the
   state asked for would not know. A switch nobody can read back is half
   a feature, and the missing half is the half that catches a mistake.

   /never, /auto or /always for the first; /memory, /file or /refused for
   the second. */
static int _recordspill(Xpost_Context *ctx,
                        Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    const char *asked = "auto";
    const char *where = "memory";

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    if (private.spill == SPILL_NEVER)  asked = "never";
    if (private.spill == SPILL_ALWAYS) asked = "always";
    /* what the record says rather than what this device remembers, so
       that the answer is the record's own state and not a second copy of
       it that could come to disagree */
    if (xpost_record_spilled(private.rec))
        where = "file";
    else if (private.where == SPILT_REFUSED)
        where = "refused";
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_object_cvlit(xpost_name_cons(ctx, asked)));
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_object_cvlit(xpost_name_cons(ctx, where)));
    return 0;
}

/* IMAGE  .recordplays  int
   How many times this device has painted a recorded image.

   An image is the one entry whose replay costs the picture rather than a
   mark, so what a band replay of one costs is the count of bands the
   image reaches -- and no page can say whether that is what was paid.
   A row written twice carries what one write left, so an image played
   again over rows it has already been played into leaves the page it
   left the first time. The quantity is stated here for the same reason
   .recordcost states the other one: what the mechanism is judged on is
   not what the page shows. */
static int _recordplays(Xpost_Context *ctx,
                        Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)private.plays));
    return 0;
}

/* IMAGE  .recordglyphs  masks bytes
   How many coverage masks this record holds, and what they cost.

   The quantity a page of text is judged on. A glyph entry is worth
   having because one mask serves every placement of it, so a page of
   text costs its distinct glyphs and a placement apiece; the way that
   stops being true -- a mask per placement -- puts out exactly the same
   page and no comparison of pages can see it. This is what can
   (tests/run-record-glyph-test.sh). */
static int _recordglyphs(Xpost_Context *ctx,
                         Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)
                                    xpost_record_mask_count(private.rec)));
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)
                                    xpost_record_mask_bytes(private.rec)));
    return 0;
}

/* How many screens the record holds, which is how many times the page
   changed the screen it was being painted under, and one more for the
   screen it opened under. A page whose target does not screen holds
   none. */
static int _recordscreens(Xpost_Context *ctx,
                          Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)
                                    xpost_record_screen_count(private.rec)));
    return 0;
}

/* IMAGE  .recordimagerows  int
   How many sample rows of a recorded image this device has put through
   the image writer.

   What .recordplays says is how many times a picture was painted, and a
   replay that painted the whole of it into every run of the page's rows
   would say exactly what one painting each run its own part of it says.
   The page cannot tell them apart either: a row written outside the run
   the device is holding is dropped against the row it was about to be
   written to, so the picture comes out the same and the cost is the
   picture times the runs. This is the quantity that separates them --
   the rows the writer was handed, which for a run of the page's rows is
   the samples reaching that run and not the samples there are. */
static int _recordimagerows(Xpost_Context *ctx,
                            Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons((integer)private.imgrows));
    return 0;
}

/* IMAGE  .recordplayed  int
   How many recorded marks this device has played.

   What a page put out a run of rows at a time costs is the marks each
   run meets rather than every mark once per run, and no page can say
   which it paid: a mark played into a run of rows it does not reach
   paints nothing, so a replay handed the whole page for every band puts
   out the page a replay handed each band's own rows puts out. The
   quantity is stated here for the same reason .recordcost and
   .recordplays state theirs -- what the mechanism is judged on is not
   what the page shows. */
static int _recordplayed(Xpost_Context *ctx,
                         Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)private.played));
    return 0;
}

/* Whether one mark leaves a run of rows the ground and nothing else: a
   rectangle at exactly the colour the page was cleared to, covering
   every row of the run and the whole width of the page.

   The pixels it covers are taken from its operands by the normaliser
   the fill itself takes them from, so what counts as covering here and
   what the fill paints there are one statement rather than two that
   agree on the cases anyone tried.

   The colour is compared as it was written down, before any fold to a
   stored channel. Two colours a fold would bring to one byte are
   answered no here, which costs a pass over the rows rather than a page
   that is missing what they held. */
static int _grounds(Xpost_Context *ctx, Xpost_Object devdic,
                    const Xpost_Record *rec, size_t at,
                    real lo, real hi, int width, int ncomp)
{
    Xpost_Object ground;
    Xpost_Record_Kind kind;
    const real *colour;
    const real *ops;
    int nops, i, x0, y0, x1, y1;

    ground = xpost_dict_get(ctx, devdic, namedotground);
    if (xpost_object_get_type(ground) != arraytype ||
        ground.comp_.sz < (unsigned int)ncomp)
        return 0;   /* a page that was never cleared has no ground */
    if (!xpost_record_get(rec, at, &kind, &colour, &ops, &nops))
        return 0;
    if (kind != XPOST_RECORD_FILLRECT || nops < 4)
        return 0;
    for (i = 0; i < ncomp; i++)
        if ((real)xpost_object_number(xpost_array_get(ctx, ground, i))
            != colour[i])
            return 0;
    xpost_dev_rect_normalize((double)ops[0], (double)ops[1],
                             (double)ops[2], (double)ops[3],
                             &x0, &y0, &x1, &y1);
    return x0 <= 0 && x1 >= width - 1
        && (double)y0 <= (double)lo && (double)y1 >= (double)hi;
}

/* recdev lo hi  .recordground  bool
   Whether a run of the page's rows comes to nothing but the ground.

   A run that does need not be painted at all: the ground is what a
   device holding no pixel over a row answers, and what an emitted page
   carries there, so leaving such a run unpainted puts out the page
   painting it would have put out. The band loop asks this of each band
   and passes over the ones that answer yes (data/recorddev.ps).

   What decides it is the last mark reaching the run, since everything
   before it is painted over wherever it covers and nothing follows it.
   A run no mark reaches at all is not the ground: what a device shows
   over such a run is its raster as it was made, which is the ground
   only where the page was cleared to it, and a record says a page was
   cleared by holding the rectangle that cleared it.

   The answer is no wherever anything here is not as it should be, that
   being the direction that costs a pass over rows rather than a page
   missing what those rows held. */
static int _recordground(Xpost_Context *ctx,
                         Xpost_Object devdic,
                         Xpost_Object lo,
                         Xpost_Object hi)
{
    Xpost_Object privatestr;
    PrivateData private;
    size_t at;
    int answer = 0;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;
    if (private.rec && private.width > 0 &&
        xpost_record_last(private.rec, (real)xpost_object_number(lo),
                          (real)xpost_object_number(hi), &at))
        answer = _grounds(ctx, devdic, private.rec, at,
                          (real)xpost_object_number(lo),
                          (real)xpost_object_number(hi), private.width,
                          private.ncomp);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(answer));
    return 0;
}

/* --- the screen a band is played under -------------------------------
   A screen is state rather than a mark: played in order, exempt from the
   row filter, and surviving the page boundary, because a screen set before
   a band governs that band wherever on the page it was set. */

/* Put a cell on the device that paints. It is what playing a screen
   entry comes to, and what a raster is given before it is moved onto
   its first band. */
static int _install_screen(Xpost_Context *ctx, Xpost_Object targetdic,
                           const unsigned char *cell, int w, int h)
{
    Xpost_Object s;
    unsigned int mode;
    int ret;

    /* the cell is made in the memory the device it is put on lives in:
       a dictionary in global memory holding a string from local memory
       is the reference across banks that a restore would leave
       dangling, and the check refuses it */
    mode = ctx->vmmode;
    ctx->vmmode = (targetdic.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)
                ? GLOBAL : LOCAL;
    s = xpost_string_cons(ctx, (unsigned int)w * (unsigned int)h,
                          (const char *)cell);
    ctx->vmmode = mode;
    if (xpost_object_get_type(s) != stringtype)
        return VMerror;
    /* Literal, because the cell is read by name from a procedure as
       well as by the compiled fills. A name holding an executable
       string runs the string, so a cell left executable would be
       scanned as program text the moment a device asked for its own
       screen -- which is what the rows a page holds no pixel of do. */
    s = xpost_object_cvlit(s);

    /* The screen a page was painted under is what the page did to the
       device, so it goes where that is kept: the state beside the device
       rather than the device itself (data/device.ps). */
    {
        Xpost_Object st = xpost_dict_get(ctx, targetdic, namedotstate);

        if (xpost_object_get_type(st) != dicttype)
            return typecheck;
        ret = xpost_dict_put(ctx, st, namebdkey[BK_HTCELL], s);
        if (!ret)
            ret = xpost_dict_put(ctx, st, namebdkey[BK_HTW],
                                 xpost_int_cons((integer)w));
        if (!ret)
            ret = xpost_dict_put(ctx, st, namebdkey[BK_HTH],
                                 xpost_int_cons((integer)h));
    }
    return ret;
}

/* The screen the raster paints its ground under.
 *
 * A device rendering a grey as a pattern of pixels lays the rows it
 * holds no pixel of under the screen it holds, and a band lays those
 * rows before anything is played into it -- so a raster about to stand
 * on its first band needs a screen already, which no entry has yet put
 * there.
 *
 * The screen given is the last the page set, because that is the one a
 * page held whole is put out under: a page's rows are laid before its
 * marks are played and read back after, so what a whole page shows
 * where nothing was painted is the ground under the screen the page
 * ended with. A band's rows carry the same, and a page painted in bands
 * comes to the page painted whole.
 *
 * A record whose target does not screen holds no screen, and this puts
 * nothing. */
static int _playscreen(Xpost_Context *ctx,
                       Xpost_Object recdic,
                       Xpost_Object targetdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    const unsigned char *cell;
    size_t n;
    int w = 0, h = 0;

    if (!_private_get(ctx, recdic, &privatestr, &private))
        return undefined;
    if (!private.rec)
        return 0;
    n = xpost_record_screen_count(private.rec);
    if (!n)
        return 0;
    cell = xpost_record_screen_get(private.rec, n - 1, &w, &h);
    if (!cell)
        return 0;
    return _install_screen(ctx, targetdic, cell, w, h);
}

/* What this device is told when the screen changes.
 *
 * A record's target may render a grey as a pattern of pixels, choosing
 * each pixel by the threshold under it. Which thresholds those are is
 * the screen in force, and it is state a marking call does not carry:
 * the device reads it from itself when it paints. A page played back
 * later would find whatever screen its target held by then, so the
 * screen is written down here, where the machinery maintaining it says
 * it has changed, and put back as a replay passes it.
 *
 * Being told costs a page one entry per screen it sets rather than one
 * per mark, because that machinery rebuilds the cell only where the
 * screen it is built from has changed. */
static int _recordscreen(Xpost_Context *ctx,
                         Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    const unsigned char *cell;
    int w, h;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;

    /* a released record takes no screen, as it takes no mark */
    if (!private.rec)
        return 0;

    /* read by the rule the painting device reads it by, so that a
       screen written down is one a page could have been painted under */
    cell = xpost_dev_ht_cell(ctx, devdic, &w, &h);
    /* a change offering no cell says nothing about what the marks after
       it are painted under, so there is nothing here for the page to be
       short of and the record is left as it is */
    if (!cell)
    {
        XPOST_LOG_ERR("%d a screen change offers no cell to record",
                      typecheck);
        return typecheck;
    }
    /* A cell that was offered and is not written down leaves every mark
       after it replaying under the screen before it, which is a page the
       record cannot reproduce. It is refused on the terms a mark it
       cannot hold is refused. */
    if (!xpost_record_screen(private.rec, w, h, cell))
    {
        xpost_record_lost(private.rec);
        return xpost_record_error(private.rec);
    }
    if (_weigh(&private, 1)
        && !xpost_dev_private_put(ctx, privatestr, &private, sizeof private))
        return VMerror;
    return 0;
}

/* --- playing the record back -----------------------------------------
   A band replay visits only the entries whose row range meets the band,
   and hands each to the same marking calls a direct paint would have made
   -- which is what keeps a page painted directly and a page replayed from
   a record byte-identical. The walk is batched so collections and
   interrupts still get their safe point. */

/* A polygon mark's coordinates as the array a device method takes.
 *
 * For the method that is not the compiled fill. That one is handed the
 * coordinates as they are held (xpost_dev_fillpoly_run), and this is
 * what a polygon costs when it cannot be: a device method may be a
 * procedure, and what a procedure takes is an object.
 *
 * It is built in local memory whatever the run was allocating in: it
 * lives as long as the call it is made for, and global memory is not
 * collected. One array per mark, and one two-element array per vertex
 * inside it -- the shape the method's operand is, which is not a shape
 * a run of coordinates can be lent. It is not carried from one mark to
 * the next either: the length differs mark by mark, and a method is
 * given its operand to keep for as long as it wants it, so an array
 * handed to two calls is an array the first of them may still be
 * holding.
 *
 * Built once for each band the shape reaches, since each band's replay
 * makes the call afresh. */
static int _play_poly(Xpost_Context *ctx, const real *ops,
                      Xpost_Object *poly)
{
    unsigned int mode = ctx->vmmode;
    int n = (int)ops[0];
    int i;

    ctx->vmmode = LOCAL;
    *poly = xpost_array_cons(ctx, (unsigned int)n);
    if (xpost_object_get_type(*poly) != arraytype)
    {
        ctx->vmmode = mode;
        return VMerror;
    }
    for (i = 0; i < n; i++)
    {
        Xpost_Object pair;
        int ret;

        if (ops[1 + 2 * i] == XPOST_PATH_BREAK)
        {
            ret = xpost_array_put(ctx, *poly, i, null);
            if (ret)
            {
                ctx->vmmode = mode;
                return ret;
            }
            continue;
        }
        pair = xpost_array_cons(ctx, 2);
        if (xpost_object_get_type(pair) != arraytype)
        {
            ctx->vmmode = mode;
            return VMerror;
        }
        ret = xpost_array_put(ctx, pair, 0, xpost_real_cons(ops[1 + 2 * i]));
        if (!ret)
            ret = xpost_array_put(ctx, pair, 1,
                                  xpost_real_cons(ops[2 + 2 * i]));
        if (!ret)
            ret = xpost_array_put(ctx, *poly, i, pair);
        if (ret)
        {
            ctx->vmmode = mode;
            return ret;
        }
    }
    ctx->vmmode = mode;
    return 0;
}

/* A mark's operands where the drawing holding it is placed somewhere:
   the coordinates carried by the placement's offset, everything else as
   it stands.
 *
 * The operands a record holds are the drawing's own, so a drawing placed
 * in three places is played three times against three offsets and
 * written down once. The offset is added to the coordinate rather than
 * to the pixel it lands in -- which pixels a coordinate names is the
 * painting device's answer and is taken after this -- so a placement
 * paints at the sub-pixel phase it was placed at.
 *
 * A polygon's coordinates are as many as its vertices and are carried in
 * the walk's own buffer; the other kinds carry four at most and are
 * carried in the caller's.
 */
static void _place_ops(Xpost_Record_Kind kind, const real *ops, int nops,
                       real dx, real dy, real *dst)
{
    int i;

    for (i = 0; i < nops; i++)
        dst[i] = ops[i];
    switch (kind)
    {
        case XPOST_RECORD_PUTPIX:
            dst[0] = ops[0] + dx;
            dst[1] = ops[1] + dy;
            return;
        case XPOST_RECORD_BLENDPIX:
            /* the coverage is not a coordinate */
            dst[1] = ops[1] + dx;
            dst[2] = ops[2] + dy;
            return;
        case XPOST_RECORD_DRAWLINE:
            dst[0] = ops[0] + dx;
            dst[1] = ops[1] + dy;
            dst[2] = ops[2] + dx;
            dst[3] = ops[3] + dy;
            return;
        case XPOST_RECORD_FILLRECT:
            /* a rectangle is a corner and an extent, and an extent is
               not carried anywhere */
            dst[0] = ops[0] + dx;
            dst[1] = ops[1] + dy;
            return;
        default:
            return;
    }
}

/* and a polygon's, into a buffer the walk keeps and grows.
 *
 * A pair marking a subpath break is a separator and not a point, so it
 * is copied as it stands: carried, it would become a coordinate pair
 * nothing recognises and the subpaths either side of it would be scanned
 * as one shape.
 *
 * Answers the coordinates to play, or NULL where there was no memory to
 * carry them. */
static const real *_place_poly(_Walk *w, const real *ops, int nops,
                               real dx, real dy)
{
    int n, i;

    if (nops < 1)
        return NULL;
    if ((size_t)nops > w->npoly)
    {
        real *grown = realloc(w->poly, (size_t)nops * sizeof *grown);

        if (!grown)
            return NULL;
        w->poly = grown;
        w->npoly = (size_t)nops;
    }
    n = (int)ops[0];
    w->poly[0] = ops[0];
    for (i = 0; i < n; i++)
    {
        if (ops[1 + 2 * i] == XPOST_PATH_BREAK)
        {
            w->poly[1 + 2 * i] = ops[1 + 2 * i];
            w->poly[2 + 2 * i] = ops[2 + 2 * i];
            continue;
        }
        w->poly[1 + 2 * i] = ops[1 + 2 * i] + dx;
        w->poly[2 + 2 * i] = ops[2 + 2 * i] + dy;
    }
    return w->poly;
}

/* Whether an object is the walk's own continuation, which is how the
   loop below tells an execution stack a call left alone from one it
   left work on. */
static int _is_replay_cont(Xpost_Object o)
{
    return xpost_object_get_type(o) == operatortype
        && o.mark_.padw == _replay_step_opcode;
}

/* How many marks one call plays before handing the interpreter back.
 *
 * The loop below is bounded rather than open, and what bounds it is
 * what returning is for. Between two evaluation steps the interpreter
 * takes the collection a run has asked for and the interrupt an outside
 * caller has raised; neither can be taken while this is running, so a
 * band played in one go is a band during which memory is not reclaimed
 * and a request to stop is not heard. A batch this size leaves the
 * round trip at a fifth of a per cent of the marks and holds both to
 * the length of one batch. A collection asked for inside a batch ends
 * it early besides, since that is the one of the two that a long band
 * can be made to need. */
#define XPOST_REPLAY_BATCH 512

/* Make the call standing on the operand stack into a device.
 *
 * Where the method is a compiled operator the call is made here, without
 * going round the interpreter for it: what that saves is not the call
 * but the journey, two evaluation steps a call.
 *
 * Where it is not, the call is left on the stacks: a device method may
 * be a procedure and what runs a procedure is the interpreter. Whatever
 * such a method leaves behind it then runs before the caller of this
 * gets on, which is where a call made straight at a device leaves it.
 */
static int _callplay(Xpost_Context *ctx, Xpost_Object m)
{
    Xpost_Object caller = ctx->currentobject;
    int ret;

    if (xpost_object_get_type(m) != operatortype)
    {
        xpost_stack_push(ctx->lo, ctx->os, m);
        if (!xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, exec)))
            return execstackoverflow;
        return 0;
    }
    /* The operator is named as the object being executed for the length
       of the call, so that an error raised inside it is reported against
       the method and unwinds onto the method's own operands. */
    ctx->currentobject = m;
    ret = xpost_operator_exec(ctx, m.mark_.padw);
    if (ret)
        return ret;  /* the method is left named, for the error */
    ctx->currentobject = caller;
    return 0;
}

/* Make one call into the device that paints, its operands being on the
 * operand stack already.
 *
 * The call itself is _callplay above, which makes it here where the
 * method is a compiled operator and leaves it on the stacks where it is
 * not. What is added here is the walk's own continuation, since the
 * calls after one that left work behind must be made after that work
 * rather than before it: the continuation goes on the execution stack
 * before the call and is taken off again after it -- if it is still on
 * top, nothing was left; if it is not, the interpreter has been given
 * something to do.
 *
 * A polygon whose method is the compiled fill is called through @p co
 * instead, the run of coordinates the record already holds it as, over
 * @p npts vertices and into @p devdic. An operator takes its operands
 * from the operand stack because that is where the interpreter leaves
 * them, and the shape of a polygon operand there is an array of arrays
 * built for the one call; the same fill reached as a function takes the
 * run and builds nothing. The colour still goes on the stack, that being
 * where the fill reads it whichever way it was entered.
 *
 * Answers the error to raise, and sets @p left where the caller must
 * return to let the interpreter reach what is waiting for it.
 */
static int _play_call(Xpost_Context *ctx, Xpost_Object m,
                      Xpost_Object cont, Xpost_Object caller,
                      Xpost_Object devdic, const real *co, int npts,
                      int *left)
{
    int ret;

    *left = 0;
    /* the continuation goes on first, so that the call runs before it
       and anything the call leaves behind runs before it too */
    if (!xpost_stack_push(ctx->lo, ctx->es, cont))
        return execstackoverflow;

    if (co && xpost_object_get_type(m) == operatortype)
    {
        /* The operator is named as the object being executed for the
           length of the call, so that an error raised inside it is
           reported against the method and unwinds onto the method's own
           operands, which is what an error from a mark played by the
           interpreter does. */
        ctx->currentobject = m;
        ret = xpost_dev_fillpoly_run(ctx, co, npts, devdic);
        if (ret)
            return ret;  /* the method is left named, for the error */
        ctx->currentobject = caller;
    }
    else
    {
        ret = _callplay(ctx, m);
        if (ret)
            return ret;
        if (xpost_object_get_type(m) != operatortype)
        {
            *left = 1;
            return 0;
        }
    }

    if (!_is_replay_cont(xpost_stack_topdown_fetch(ctx->lo, ctx->es, 0)))
    {
        *left = 1;
        return 0;
    }
    (void)xpost_stack_pop(ctx->lo, ctx->es);
    return 0;
}

/* Play the marks that reach the rows asked for, from the one at @p idx
 * on, into the device that paints.
 *
 * A mark is played by calling the target's method for its kind. Where
 * that method is a compiled operator the call is made here, in a loop:
 * the operands go on the operand stack, which is where an operator
 * takes them from and where its declared shape is checked, and the
 * operator is run without going round the interpreter for it. What that
 * saves is not the call but the journey -- a mark played by leaving the
 * call on the stacks and returning costs two evaluation steps, and with
 * them a fresh look at this device's state and at the target's method
 * table, for every mark on the page.
 *
 * Where the method is not an operator the loop cannot make the call: a
 * device method may be a procedure, and what runs a procedure is the
 * interpreter. Such a mark is played by leaving the call on the stacks
 * and returning, and the walk resumes here afterwards and goes back to
 * looping. The same applies to a method that is an operator and leaves
 * work behind it. Both are _play_call above, and both have this return
 * to let the interpreter reach the work, with the walk's continuation
 * already sitting under it in the right place.
 *
 * The walk's operands sit on the operand stack for the whole batch
 * rather than being pushed and dropped per mark. The method consumes
 * what it was given and leaves them where they were, so the only ones
 * that change are the place in the record and the place inside a glyph,
 * which are written over in place as each call is made.
 *
 * A glyph is the one entry a batch can end in the middle of. It stands
 * for a run of pixel calls rather than one call, so the walk keeps how
 * far into its mask it has got and comes back to the same entry: a
 * glyph of a thousand pixels played in one go would be a thousand calls
 * during which memory is not reclaimed and a request to stop is not
 * heard, which is what the batch exists to bound.
 *
 * A placement is not played by calling anything: it names a drawing the
 * record holds and says where that drawing's coordinates land, so the
 * walk descends into it and plays its marks against the offset,
 * returning to the entry after the placement when it runs out. What it
 * keeps is a level per placement descended into, in the walk state, so
 * a drawing placed at several depths is played wherever it was placed
 * and is written down once. The descent is bounded: a page nested deeper
 * than the levels there are is refused with a limitcheck rather than
 * followed until something runs out.
 *
 * The rows asked of a drawing are the rows asked of the page, less the
 * offset it was placed at, so a placement whose drawing falls outside
 * the run is stepped over as a mark is and its marks are never looked
 * at.
 */
static int _replay_step(Xpost_Context *ctx,
                        Xpost_Object recdic,
                        Xpost_Object targetdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Xpost_Object caller = ctx->currentobject;
    Xpost_Object cont;
    /* the target's method for each of the five marking kinds, looked up
       as each kind is first met and kept for the batch: the table is the
       target's and does not change while its own methods are running */
    Xpost_Object method[5];
    char looked[5];
    _Walk *w;
    int batch, k;
    int ret = 0;

    if (!_private_get(ctx, recdic, &privatestr, &private))
        return undefined;
    /* a record given up, and one no replay has been started on, have
       nothing to go on with */
    if (!private.rec || !private.walk)
        return 0;
    w = private.walk;

    memset(looked, 0, sizeof looked);
    cont = xpost_operator_cons_opcode(_replay_step_opcode);

    /* The walk's own operands, which every return below leaves in place
       for the continuation to be resumed with. They go on once for the
       batch rather than once for each mark, the marks being played over
       the top of them. How far the walk has got is not among them: it is
       a stack of levels rather than a place, and it is kept where the
       record is. */
    xpost_stack_push(ctx->lo, ctx->os, recdic);
    xpost_stack_push(ctx->lo, ctx->os, targetdic);
    /* a push the stack would not take leaves the walk standing on
       operands that are not there; the run is told about it at the next
       evaluation step, and nothing is played in the meantime */
    if (ctx->lo->push_refused)
        return 0;

    for (batch = 0; batch < XPOST_REPLAY_BATCH; )
    {
        _Level *f = &w->at[w->depth];
        Xpost_Record_Kind kind;
        const real *colour;
        const real *ops;
        real placed[4];
        Xpost_Object m;
        Xpost_Object poly = null;
        /* a polygon's coordinates where the call takes them as they are
           held, and nothing for every other mark */
        const real *co = NULL;
        size_t at, waspix;
        /* the rows asked for, on the page, which is where the drawing
           this level is walking has been carried to */
        real rlo = f->lo + f->dy, rhi = f->hi + f->dy;
        int nops, npts = 0, i, left, fresh, inmask;

        /* The rows asked for choose which marks are played, and the
           marks between are stepped over rather than played and
           dropped: what makes a page affordable to paint a run of rows
           at a time is that playing it into a set of runs costs the
           marks each run meets rather than every mark once per run.

           Nothing further reaching those rows, or an entry that cannot
           be read: the drawing is finished with, so the walk comes back
           out to the record that placed it and goes on after the
           placement. At the drawing it began in there is nothing to come
           back to, and the walk is over: its operands go back. */
        if (!xpost_record_next(f->rec, f->idx, f->lo, f->hi, &at)
            || !xpost_record_get(f->rec, at, &kind, &colour, &ops, &nops))
        {
            /* Unless the drawing stopped answering rather than running
               out. A record whose scratch file could not be read is
               short of a mark from that point, and a walk that took the
               refusal for the end of the drawing would leave the band
               painted as far as the reading got -- which is a page
               missing something, and a page missing something looks like
               a page. */
            if (xpost_record_failed(f->rec))
            {
                XPOST_LOG_ERR("%d a page cannot be painted from a record"
                              " that stopped answering for it",
                              xpost_record_error(f->rec));
                ret = xpost_record_error(f->rec);
                goto refused;
            }
            if (w->depth > 0)
            {
                w->depth--;
                continue;
            }
            goto refused;
        }
        batch++;

        /* Whether the walk is coming back into the middle of this
           entry, and how far into it -- read before the walk is moved
           past the entry below. Only a coverage mask is left in the
           middle of: it stands for a run of pixel calls rather than one
           call, and a batch may end inside it. */
        inmask = f->inmask;
        waspix = f->pixat;

        /* Every entry played moves the walk past itself, whether it was
           a mark, a picture, a screen or a placement. Resuming where it
           was looking from instead would find the same entry again for
           every mark the rows asked for had it step over. A mask left
           part way puts the walk back on its own entry below. */
        f->idx = at + 1;
        f->inmask = 0;

        /* A placement names a drawing this record holds and says where
           the drawing's own coordinates land. It is played by playing
           that drawing, so the walk descends: the level below carries
           the offset this one carries and the placement's on top of it,
           and is asked for the rows this level was asked for, less the
           offset -- the drawing's own coordinates being what its marks
           are in.

           A drawing may place drawings itself, and the descent is
           bounded rather than open. What bounds it is the levels there
           are: a page nested past them is refused here, with a
           limitcheck, rather than descended into until the run has no
           stack left. */
        if (kind == XPOST_RECORD_PLACE)
        {
            const Xpost_Record *sub;
            _Level *into;

            sub = xpost_record_place_get(f->rec,
                                         nops > 0 ? (size_t)ops[0]
                                                  : (size_t)-1);
            if (!sub)
            {
                XPOST_LOG_ERR("%d a recorded placement names no drawing",
                              undefined);
                ret = undefined;
                goto refused;
            }
            if (w->depth + 1 >= XPOST_RECORD_NEST)
            {
                XPOST_LOG_ERR("%d a page places drawings deeper than a replay"
                              " descends", limitcheck);
                ret = limitcheck;
                goto refused;
            }
            into = &w->at[++w->depth];
            into->rec = sub;
            into->dx = f->dx + (nops > 1 ? ops[1] : (real)0);
            into->dy = f->dy + (nops > 2 ? ops[2] : (real)0);
            /* The rows asked of the drawing are the rows asked of this
               level, less where the placement puts it, and a row either
               side of those. A placement at a fractional distance puts
               a mark of the drawing's row on one of two rows of the
               page, so a range taken exactly would leave the drawing
               judging a mark not to reach a run it is painted into --
               and a mark judged not to reach a band is absent from the
               page. Erring outward costs a visit to a mark that then
               paints nothing there. */
            into->lo = f->lo - (nops > 2 ? ops[2] : (real)0) - (real)1;
            into->hi = f->hi - (nops > 2 ? ops[2] : (real)0) + (real)1;
            into->idx = 0;
            into->pixat = 0;
            into->inmask = 0;
            continue;
        }

        /* An image is not one of the marking calls and is not made by
           calling one: its rows are written into the target's raster
           through the writer that wrote them the first time. Nothing is
           left on the stacks for a method to consume, so the loop goes
           straight on to the next entry rather than round the
           interpreter.

           The rows painted are the whole of the target's page. A replay
           into part of it hands that part down here instead, and the
           entry writes the rows meeting it -- which is where a band
           enters. */
        if (kind == XPOST_RECORD_IMAGE)
        {
            const Xpost_Record_Image *img;
            Xpost_Record_Image put;
            Xpost_Object dims;
            int nrows;

            img = xpost_record_image_get(f->rec,
                                         nops > 0 ? (size_t)ops[0]
                                                  : (size_t)-1);
            if (!img)
            {
                XPOST_LOG_ERR("%d a recorded image names no entry", undefined);
                ret = undefined;
                goto refused;
            }
            /* Where the drawing holding it has been placed, the picture
               goes there: what places it is the transform written down
               with it, and the region it is written through moves with
               it. The samples do not move and are not copied. */
            if (f->dx != (real)0 || f->dy != (real)0)
            {
                put = *img;
                put.xoff += f->dx;
                put.yoff += f->dy;
                put.cx0 += f->dx;
                put.cx1 += f->dx;
                put.cy0 += f->dy;
                put.cy1 += f->dy;
                img = &put;
            }
            dims = xpost_dict_get(ctx, targetdic, namebdkey[BK_DIMENSIONS]);
            if (xpost_object_get_type(dims) != arraytype || dims.comp_.sz < 2)
            {
                ret = typecheck;
                goto refused;
            }
            /* an image is held to the rows asked for the same way a mark
               is: the replay chooses the sample rows that reach them and
               narrows the region it paints to them, so a run of rows
               takes only its own part of an image that crosses it */
            ret = _play_image(ctx, f->rec, (size_t)ops[0], img,
                              targetdic, rlo, rhi, &nrows);
            if (ret)
                goto refused;
            private.plays++;
            private.imgrows += (unsigned int)nrows;
            continue;
        }

        /* A screen is not a mark and paints nothing. It says what the
           marks after it were made under, and playing it is putting that
           back on the device about to paint them, so that each mark is
           screened by the screen it was made under rather than by
           whichever one the target happens to hold when the page is put
           out.

           Every run of rows passes through the same screens in the same
           order, a screen being met by every range, so the pixels a band
           paints are the pixels the whole page would have carried
           there. */
        if (kind == XPOST_RECORD_SCREEN)
        {
            const unsigned char *cell;
            int cw = 0, ch = 0;

            cell = xpost_record_screen_get(f->rec,
                                           nops > 0 ? (size_t)ops[0]
                                                    : (size_t)-1,
                                           &cw, &ch);
            if (!cell)
            {
                XPOST_LOG_ERR("%d a recorded screen names no entry",
                              undefined);
                ret = undefined;
                goto refused;
            }
            ret = _install_screen(ctx, targetdic, cell, cw, ch);
            if (ret)
                goto refused;
            continue;
        }

        /* A glyph is a coverage mask put at a place, and is played as
           the run of pixel calls that comes to: a fully covered pixel
           through the target's PutPix and a partly covered one through
           its BlendPix, carrying how much of it is covered. Which is
           what the painting did with the same mask, one glyph at a time
           rather than one page of text at a time.

           The rows asked for chose this entry and the mask is played
           whole into them, as every mark is: a pixel outside the rows
           the target is standing on is dropped against the row it was
           about to be written to, so a glyph crossing a band's edge
           leaves each band its own part of itself. */
        if (kind == XPOST_RECORD_GLYPH)
        {
            const unsigned char *cov;
            int mw = 0, mh = 0;
            int r0, r1;
            real gx, gy;
            size_t pixat, first, last;

            cov = xpost_record_mask_get(f->rec,
                                        nops > 0 ? (size_t)ops[0]
                                                 : (size_t)-1, &mw, &mh);
            if (!cov)
            {
                XPOST_LOG_ERR("%d a recorded glyph names no mask", undefined);
                ret = undefined;
                goto refused;
            }
            /* Where the mask goes, carried by the placement of the
               drawing holding it and put back on the pixel grid: a mask
               is a rectangle of pixels painted a pixel at a time, and it
               is rendered against the grid rather than against the phase
               its origin falls at (src/lib/xpost_op_font.c), so it lands
               on whole pixels wherever the drawing was placed. */
            gx = (real)floor((double)(nops > 1 ? ops[1] : (real)0)
                             + (double)f->dx + 0.5);
            gy = (real)floor((double)(nops > 2 ? ops[2] : (real)0)
                             + (double)f->dy + 0.5);
            /* whether the walk is meeting this entry for the first
               time, rather than coming back into the middle of it */
            pixat = inmask ? waspix : 0;
            fresh = !inmask;

            /* Which of the mask's rows reach the rows asked for. Row k
               of it lands on device row gy + k, so this is that question
               turned round; the answer errs outward, a row painted that
               the target then drops costing a pass over it where a row
               not painted is missing from the page.

               A mask is held to the rows asked for the way an image is
               and not the way a shape is. A shape has to be converted
               whole to be right about any part of it, so a range chooses
               which shapes are played and never trims one; a mask is
               already resolved into pixels that stand alone, so the rows
               outside the range are rows the device would drop, and
               painting them would cost a tall mask its whole height once
               per band it crosses. */
            r0 = (int)floor((double)rlo - (double)gy);
            r1 = (int)floor((double)rhi - (double)gy) + 1;
            if (r0 < 0) r0 = 0;
            if (r1 > mh) r1 = mh;
            if (r1 < r0) r1 = r0;
            first = (size_t)r0 * (size_t)mw;
            last = (size_t)r1 * (size_t)mw;
            if (pixat < first)
                pixat = first;

            /* One more mark played -- the glyph, and not its pixels.
               What this counts is the marks of the page, and a page of
               text holds a glyph wherever it shows one; counting the
               pixels would answer the ink, which is the quantity the
               entry exists to stop the record paying. Counted once
               however many batches the mask takes. */
            if (fresh)
                private.played++;

            /* the walk stays on this entry while any of the mask is
               left, so what it comes back to is this one and not the
               one after it */
            f->idx = at;
            f->inmask = 1;

            while (pixat < last)
            {
                int c = cov[pixat];
                int which;

                if (!c)
                {
                    pixat++;
                    continue;
                }
                which = c == 255 ? XPOST_RECORD_PUTPIX : XPOST_RECORD_BLENDPIX;
                if (!looked[which])
                {
                    method[which] = xpost_dict_get(ctx, targetdic,
                                                   _slot(which));
                    looked[which] = 1;
                }
                m = method[which];
                if (xpost_object_get_type(m) == invalidtype ||
                    xpost_object_get_type(m) == nulltype)
                {
                    /* the device being played into does not offer one
                       of the two methods a coverage mask is painted
                       through, so the glyph cannot be made */
                    XPOST_LOG_ERR("%d a recorded glyph has no method to play"
                                  " it into", undefined);
                    ret = undefined;
                    goto refused;
                }

                /* where inside the mask to come back to, written before
                   the call is made */
                f->pixat = pixat + 1;

                for (i = 0; i < private.ncomp; i++)
                    xpost_stack_push(ctx->lo, ctx->os,
                                     xpost_real_cons(colour[i]));
                if (which == XPOST_RECORD_BLENDPIX)
                    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(c));
                xpost_stack_push(ctx->lo, ctx->os,
                                 xpost_real_cons(gx + (real)(pixat
                                                             % (size_t)mw)));
                xpost_stack_push(ctx->lo, ctx->os,
                                 xpost_real_cons(gy + (real)(pixat
                                                             / (size_t)mw)));
                xpost_stack_push(ctx->lo, ctx->os, targetdic);

                pixat++;
                batch++;
                ret = _play_call(ctx, m, cont, caller, targetdic, NULL, 0,
                                 &left);
                if (ret)
                    goto done;
                if (left)
                    goto done;
                if (batch >= XPOST_REPLAY_BATCH
                    || (ctx->lo && ctx->lo->garbage_collect_pending)
                    || (ctx->gl && ctx->gl->garbage_collect_pending))
                    goto resume;
            }

            /* the mask is spent: the walk moves past it and the entry
               after it starts at its own first pixel */
            f->idx = at + 1;
            f->pixat = 0;
            f->inmask = 0;
            continue;
        }

        if ((int)kind < 0 || (int)kind >= (int)(sizeof looked / sizeof *looked))
        {
            XPOST_LOG_ERR("%d a recorded mark is of no kind a record holds",
                          undefined);
            ret = undefined;
            goto refused;
        }
        if (!looked[(int)kind])
        {
            method[(int)kind] = xpost_dict_get(ctx, targetdic, _slot(kind));
            looked[(int)kind] = 1;
        }
        m = method[(int)kind];
        if (xpost_object_get_type(m) == invalidtype ||
            xpost_object_get_type(m) == nulltype)
        {
            /* the device being played into does not offer one of the
               five marking methods a record holds, so the mark cannot be
               made */
            XPOST_LOG_ERR("%d a recorded mark has no method to play it into",
                          undefined);
            ret = undefined;
            goto refused;
        }

        /* One more mark played, which is what .recordplayed answers.
           Counted here, where the call about to be made is known to be
           one the target offers, so what the count says is marks made
           and not marks looked at. */
        private.played++;

        /* Where the drawing holding this mark has been placed, the mark
           goes there. The coordinates are carried into a buffer and the
           call is made from that, the record holding the drawing's own
           and the drawing being played wherever it was placed. Nothing
           carries a mark of a drawing nobody placed, which is every mark
           of an ordinary page. */
        if (f->dx != (real)0 || f->dy != (real)0)
        {
            if (kind == XPOST_RECORD_FILLPOLY)
            {
                const real *put = _place_poly(w, ops, nops, f->dx, f->dy);

                if (!put)
                {
                    XPOST_LOG_ERR("%d no memory to place a recorded polygon",
                                  VMerror);
                    ret = VMerror;
                    goto refused;
                }
                ops = put;
            }
            else if (nops > 0 && nops <= (int)(sizeof placed / sizeof *placed))
            {
                _place_ops(kind, ops, nops, f->dx, f->dy, placed);
                ops = placed;
            }
        }

        /* A polygon reaches the compiled fill as the run the record
           holds it as, and reaches any other method as the array that
           method's operand is. Which of the two it is decides what goes
           on the stack below: the run is handed to the call and the
           array is an operand, and a device it is an operand for takes
           the device dictionary after it as every method call does. */
        if (kind == XPOST_RECORD_FILLPOLY)
        {
            if (xpost_dev_fillpoly_compiled(m))
            {
                co = ops + 1;
                npts = (int)ops[0];
            }
            else
            {
                ret = _play_poly(ctx, ops, &poly);
                if (ret)
                    goto refused;
            }
        }

        /* the colour it was made with, one value per component of the
           space it was made in, which is the space the device being
           played into declares and so the operands its method takes */
        for (i = 0; i < private.ncomp; i++)
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(colour[i]));
        /* A run handed to the call is not an operand and neither is the
           device it is handed with, so a polygon reaching the fill that
           way leaves the colour standing on its own. */
        if (!co)
        {
            if (kind == XPOST_RECORD_FILLPOLY)
                xpost_stack_push(ctx->lo, ctx->os, poly);
            else
                for (i = 0; i < nops; i++)
                    xpost_stack_push(ctx->lo, ctx->os,
                                     xpost_real_cons(ops[i]));
            xpost_stack_push(ctx->lo, ctx->os, targetdic);
        }

        ret = _play_call(ctx, m, cont, caller, targetdic, co, npts, &left);
        if (ret)
            goto done;
        if (left)
            goto done;

        /* A collection the run has asked for is taken between evaluation
           steps and not inside this, so a batch that has been asked for
           one ends here and the interpreter takes it before the walk is
           resumed. */
        if ((ctx->lo && ctx->lo->garbage_collect_pending) ||
            (ctx->gl && ctx->gl->garbage_collect_pending))
            break;
    }

  resume:
    /* the batch is full, or a collection is waiting: the walk goes back
       on to be resumed, standing on the operands it was left with */
    if (!xpost_stack_push(ctx->lo, ctx->es, cont))
        ret = execstackoverflow;

    goto done;

  refused:
    /* Nothing was played for this entry and nothing will be: the walk's
       operands come back off, so that a caller reached by the error
       finds the stack the entry point left rather than the walk's own
       working state. An error raised by a method that did run leaves
       them where they are, which is where a mark played by the
       interpreter leaves them. */
    for (k = 0; k < 2; k++)
        (void)xpost_stack_pop(ctx->lo, ctx->os);

  done:
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof private))
        return ret ? ret : VMerror;
    return ret;
}

/* What a replay refuses whatever rows it is asked for, settled once
   rather than mark by mark.

   A mark carries one colour value per component of the space it was made
   in, and it is played by handing those values to a method whose
   operands the receiving device's own space decides -- so a record made
   in one space and played into a device declaring another puts each
   value in the place of a different one, and paints a colour nobody
   named. And a record played into the device holding it writes down what
   it plays: the run it is walking grows by a mark for every mark taken
   from it and there is no end to reach. */
static int _replay_refuse(Xpost_Context *ctx,
                          Xpost_Object recdic,
                          Xpost_Object targetdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, recdic, &privatestr, &private))
        return undefined;
    /* a record short of a mark describes a page it cannot reproduce, and
       what would be painted from it is a page missing something -- which
       looks like a page. It is refused here, where a caller still has an
       error to see, rather than played back short. */
    if (private.rec && xpost_record_failed(private.rec))
    {
        XPOST_LOG_ERR("%d a record short of a mark cannot be played back",
                      VMerror);
        return VMerror;
    }
    if (xpost_dict_compare_objects(ctx, recdic, targetdic) == 0)
    {
        XPOST_LOG_ERR("%d a record cannot be played into the device holding"
                      " it", rangecheck);
        return rangecheck;
    }
    if (xpost_dict_compare_objects(
            ctx, xpost_dict_get(ctx, recdic, namenativecolorspace),
            xpost_dict_get(ctx, targetdic, namenativecolorspace)) != 0)
    {
        XPOST_LOG_ERR("%d a record is played into a device whose colour space"
                      " is not the one its marks were made in", rangecheck);
        return rangecheck;
    }
    return 0;
}

/* Start the walk: the marks that reach rows lo to hi, in the order they
   were made, which is the order they were painted in and so the order
   they must be painted in again.
 *
 * The walk begins at the record this device holds and at no offset,
 * which is the drawing the page is. Where it meets a placement it
 * descends, and the levels it descends through are taken up here: they
 * are the walk's and not the page's, so they are made once for the
 * device and filled again by each run of rows it is asked for.
 *
 * @p dx and @p dy carry the whole page, which is what a caller asking
 * for a drawing to be painted somewhere hands down; a page painted where
 * it was drawn hands down nothing. The rows are the rows of the page,
 * and the record is asked for its own, which is the rows less the
 * offset.
 */
static int _replay_walk(Xpost_Context *ctx,
                        Xpost_Object recdic,
                        Xpost_Object targetdic,
                        Xpost_Object lo,
                        Xpost_Object hi,
                        real dx, real dy)
{
    Xpost_Object privatestr;
    PrivateData private;
    _Level *f;

    if (!_private_get(ctx, recdic, &privatestr, &private))
        return undefined;
    if (!private.rec)
        return 0;
    if (!private.walk)
    {
        private.walk = calloc(1, sizeof *private.walk);
        if (!private.walk)
            return VMerror;
        if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof private))
        {
            /* the state is the only thing that would have named it */
            free(private.walk);
            return VMerror;
        }
    }

    /* The walk starts over whatever a walk before it left, which is
       nothing where one ran out and its own end where one finished. A
       run of rows is asked for from the beginning of the record either
       way. */
    private.walk->depth = 0;
    f = &private.walk->at[0];
    f->rec = private.rec;
    f->dx = dx;
    f->dy = dy;
    f->lo = (real)xpost_object_number(lo) - dy;
    f->hi = (real)xpost_object_number(hi) - dy;
    f->idx = 0;
    f->pixat = 0;
    f->inmask = 0;

    xpost_stack_push(ctx->lo, ctx->os, recdic);
    xpost_stack_push(ctx->lo, ctx->os, targetdic);
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_operator_cons_opcode(_replay_step_opcode)))
        return execstackoverflow;
    return 0;
}

/* recdev pagedev lo hi  .replaypage  -
   Play the marks that reach rows lo to hi into a device that paints.

   The rows are a run of the page's own, and what they are for is the
   caller's: successive runs paint a page in a raster the size of a run,
   and the rows a window shows paint what someone is looking at. A mark
   meeting the run at all is played whole and the device it is played
   into keeps what it holds of it, so a shape crossing the far edge of a
   run is played into the run beyond as well and each keeps its part. */
static int _replaypage_rows(Xpost_Context *ctx,
                            Xpost_Object recdic,
                            Xpost_Object targetdic,
                            Xpost_Object lo,
                            Xpost_Object hi)
{
    int ret;

    ret = _replay_refuse(ctx, recdic, targetdic);
    if (ret)
        return ret;
    return _replay_walk(ctx, recdic, targetdic, lo, hi, (real)0, (real)0);
}

/* recdev pagedev  .replaypage  -
   Play every mark a record holds, which is the rows its marks reach.
   A record holding no mark paints nothing and reaches no row, so there
   is no run to name and nothing to walk. */
static int _replaypage(Xpost_Context *ctx,
                       Xpost_Object recdic,
                       Xpost_Object targetdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    real lo, hi;
    int ret;

    ret = _replay_refuse(ctx, recdic, targetdic);
    if (ret)
        return ret;
    if (!_private_get(ctx, recdic, &privatestr, &private))
        return undefined;
    if (!private.rec || !xpost_record_extent(private.rec, &lo, &hi))
        return 0;
    return _replay_walk(ctx, recdic, targetdic,
                        xpost_real_cons(lo), xpost_real_cons(hi),
                        (real)0, (real)0);
}

/* recdev pagedev dx dy  .replayplace  -
   Paint a drawing into a device that paints, put at dx dy.

   What .replaypage is for a page a record holds, this is for a drawing
   the record is: the marks are played into the device with the offset
   added to every coordinate, so a drawing made once is painted wherever
   it is put. The rows are the rows the drawing reaches from where it has
   been put, since a device that paints its page whole holds all of them;
   a device holding a run of them keeps its own part, as it does of a
   page.

   It is how a drawing reaches a device that paints. A device holding a
   record of its own is given the placement to write down instead
   (.recordplace above), so that the drawing is held once for the page
   rather than played once per placement into a raster that does not
   exist yet. */
static int _replayplace(Xpost_Context *ctx,
                        Xpost_Object recdic,
                        Xpost_Object targetdic,
                        Xpost_Object dx,
                        Xpost_Object dy)
{
    Xpost_Object privatestr;
    PrivateData private;
    real ox, oy, lo, hi;
    int ret;

    ret = _replay_refuse(ctx, recdic, targetdic);
    if (ret)
        return ret;
    if (!_private_get(ctx, recdic, &privatestr, &private))
        return undefined;
    if (!private.rec || !xpost_record_extent(private.rec, &lo, &hi))
        return 0;
    ox = (real)xpost_object_number(dx);
    oy = (real)xpost_object_number(dy);
    return _replay_walk(ctx, recdic, targetdic,
                        xpost_real_cons(lo + oy), xpost_real_cons(hi + oy),
                        ox, oy);
}

/* Whether a page of this extent could be painted at all.
 *
 * A record holds marks and no pixels, so it would take a page of any
 * size and refuse it at the far end, when the raster that paints it is
 * finally asked for -- which is after the program has run. The device
 * that paints holds a page as rows: an array of them, one string per
 * row. So the page a record can be given is the page its target could
 * hold, and that is the question asked here, at Create, where every
 * other device answers it.
 *
 * Banding is what makes this worth stating rather than leaving to the
 * raster. Without it, a page too large refuses itself by not fitting in
 * memory; a banded page holds one band whatever its height, so the
 * extent stops being refused by its own weight and has to be refused on
 * purpose. What is left bounding it is the array of rows, which is the
 * one part of a banded page that still grows with the page: one object
 * slot per row of the page, held whether or not the device holds that
 * row's pixels, because a band presents the whole spine and keeps
 * pixels in the rows of its own run (doc/xpost_design.dox).
 *
 * So the page is put to two questions and they are not the same one.
 * What can be held is a quantity of memory; what a composite can count
 * is the range of a field. Asking only the second lets a build whose
 * field is wide accept a page of two thousand million rows, which is
 * tens of gigabytes of array before a pixel exists, and no field width
 * makes that memory appear.
 */

/* The bytes one page's rows may take of the virtual memory they are
   built in.

   The array of rows and the rows themselves are allocated in one bank of
   virtual memory, and a bank is addressed by an unsigned 32-bit offset,
   so a bank runs no further than such an offset reaches: a request past
   that span cannot be met however much memory the host has, which is why
   .vmreserve (src/lib/xpost_op_param.c) refuses one without asking the
   system for it. That is the ceiling this comes out of. What one page's
   rows may take is a share of a bank rather than the whole of it, since
   the band's own pixels, the marks the record holds and everything the
   program allocates come out of the same bank.

   The share is a judgement and is written as one. A round decimal admits
   to being chosen, where a power of two would claim to be a boundary of
   the object or of the machine and there is no such boundary here. A
   hundred million bytes of rows is millions of rows at either object
   width, which is past any page that is going to be painted and short of
   any page that could be held. */
#define PAGE_ROWS_BUDGET 100000000u

/* --- making the device -----------------------------------------------
   The recording device itself: what it declares, what it refuses, and the
   block it holds outside virtual memory, which goes when the device is
   retired. */

static int _extent_ok(Xpost_Object width, Xpost_Object height)
{
    int w, h;

    /* The page as the extent a device takes it as, which is where a
       number that is no extent at all -- a negative one, or one past
       what a device counts a pixel's position in -- is refused. */
    if (!xpost_dev_buffer_extent(width.int_.val, &w)
        || !xpost_dev_buffer_extent(height.int_.val, &h))
        return 0;
    /* What can be held. The array of rows costs an object slot for every
       row of the page, and a row costs at least a byte a pixel -- one
       string of the page's width for the grayscale rasters and three for
       the colour ones. The spine is asked as a division rather than as a
       product, so that a height no memory could answer for is not first
       multiplied into a number nothing counts. */
    if ((size_t)h > PAGE_ROWS_BUDGET / sizeof(Xpost_Object)
        || (size_t)w > PAGE_ROWS_BUDGET)
        return 0;
    /* And then what can be counted. A row is a string of the page's width
       and the rows are an array of the page's height, so each is held to
       what a composite counts; the bound is the object width this build
       was made with, and a page past it cannot be described, never mind
       held. */
    if ((dword)w > XPOST_OBJECT_COMP_MAX_SZ
        || (dword)h > XPOST_OBJECT_COMP_MAX_SZ)
        return 0;
    return 1;
}

/* What a record device holds, given up where the run never got to it:
   the record, and the levels a replay of it descended through. Called
   from the collector with the block this device's state is kept in, so
   it touches nothing in virtual memory. A device the run retired has
   cleared both and leaves this nothing to do. */
static void _reclaim(void *block)
{
    PrivateData *private = block;

    xpost_record_free(private->rec);
    private->rec = NULL;
    if (private->walk)
    {
        free(private->walk->poly);
        free(private->walk);
        private->walk = NULL;
    }
}

/* create an instance of the device, using the class .copydict procedure */
static int _create(Xpost_Context *ctx,
                   Xpost_Object width,
                   Xpost_Object height,
                   Xpost_Object classdic)
{
    if (!_extent_ok(width, height))
    {
        XPOST_LOG_ERR("%d a page of %ldx%ld is larger than the rows a"
                      " device can hold it as", limitcheck,
                      (long)width.int_.val, (long)height.int_.val);
        return limitcheck;
    }

    return xpost_dev_create_begin(ctx, width, height, classdic,
                                  _create_cont_opcode);
}

/* make the record and name it from the instance */
static int _create_cont(Xpost_Context *ctx,
                        Xpost_Object w,
                        Xpost_Object h,
                        Xpost_Object devdic)
{
    Xpost_Object privatestr;
    Xpost_Object ncomp;
    Xpost_Object make;
    Xpost_Object o;
    PrivateData private;
    int width, height;
    int ret;

    /* The page the program asked for. A record holds no raster, so what
       the extent settles here is what the marking methods are held
       against and what the device the page is played into is built at,
       which is a page extent either way. */
    if (!xpost_dev_page_extent(w.int_.val, h.int_.val, &width, &height))
        return limitcheck;

    /* The components a mark of this record carries, taken from the
       instance rather than from where the entry points were installed:
       the record is made to hold that many values per mark, and a
       colour it gave back at another count would be a colour nobody
       named. The class carries it, put there when the target this
       record plays into was settled, and the instance is a copy of the
       class. */
    ncomp = xpost_dict_get(ctx, devdic, namedotncomp);
    if (xpost_object_get_type(ncomp) != integertype
        || ncomp.int_.val < 1 || ncomp.int_.val > RECORD_MAXCOMP)
    {
        XPOST_LOG_ERR("%d a record device carries no component count a mark"
                      " can be held at", rangecheck);
        return rangecheck;
    }

    /* The block this device's instance state lives in, and, named
       with it rather than after it, what gives up whatever that
       state names. What this device holds is a record, which is not virtual memory:
       a device the run never retires -- a drawing a restore took back,
       or one nothing named by the time a collection came round -- would
       take its record with it. This is what gives it up there. A device
       the run does retire has given it up already and leaves this
       nothing to do. */
    ret = xpost_handle_cons(ctx, devdic, namePrivate, &privatestr,
                            XPOST_HANDLE_DEVICE, sizeof(PrivateData),
                            _reclaim);
    if (ret)
        return ret;

    private.width = width;
    private.height = height;
    private.ncomp = ncomp.int_.val;
    /* no replay has begun, so there are no levels to descend through
       yet: they are taken up at the first one */
    private.walk = NULL;
    private.plays = 0;
    private.played = 0;
    private.imgrows = 0;
    private.playgen = 0;
    /* Nothing to weigh the record against until the band grid is
       settled, which .playmake does below, before any mark arrives. A
       record whose page is held whole is left with nothing here: it
       saves nothing, so there is nothing it could cost more than. */
    private.saving = 0;
    /* and what this run said a band of a page may cost, which is what
       bounds the record where the saving says nothing.
       Read from where the run's own decisions live rather than off the
       device in hand: what the device carries is the grid its page is cut
       into, which whatever made it may cut as fine as it likes, and a
       record weighed against the grid of a one-row band would put a
       page's marks in a file at the first of them. What this bounds is
       what the run said it would spend, which no page may raise or lower
       for itself. The two are settled together and stand equal
       (.settlebandbudget, data/device.ps). */
    o = xpost_context_host_setting(ctx, "MaxBandBytes");
    private.budget = (xpost_object_get_type(o) == integertype
                      && o.int_.val > 0) ? (size_t)o.int_.val : 0;
    private.spill = _spill_asked(ctx);
    private.where = SPILT_MEMORY;
    private.rec = xpost_record_new(private.ncomp);
    if (!private.rec)
        return VMerror;

    /* Whether scratch space can be had, asked here rather than at the
       first page that needs it.
     *
     * A page that runs for twenty minutes and only then discovers it
     * cannot spill has wasted the twenty minutes, and the discovery was
     * available before the first mark. So the question is asked at the
     * beginning, and asked by making a spill file and writing to it: a
     * permissions check would answer yes for a directory on a full
     * filesystem, for one a system-call filter refuses, and for one
     * mounted read-only under the caller. The file holds a few bytes and
     * is given back before the answer comes; it is not a spill.
     *
     * A run that said it wants no disk touched is not asked, and that is
     * the point of it: nothing here reaches the scratch directory in
     * that state, not even to look.
     */
    if (private.spill != SPILL_NEVER && !_spill_probe())
    {
        /* the state the run asked for cannot be had, and a run that
           asked for it outright is told so where it asked */
        if (private.spill == SPILL_ALWAYS)
        {
            XPOST_LOG_ERR("%d no page's marks can be put in %s (%s), and this"
                          " run asked for every page's to be", ioerror,
                          xpost_temp_dir(), _spill_probe_why);
            xpost_record_free(private.rec);
            return ioerror;
        }
        if (!_spill_probe_said)
            XPOST_LOG_ERR("no page's marks can be put in %s (%s); a page"
                          " drawing more than the raster banding it saves"
                          " will be held in memory instead, and what it costs"
                          " will follow the drawing without limit",
                          xpost_temp_dir(), _spill_probe_why);
        _spill_probe_said = 1;
        private.where = SPILT_REFUSED;
    }
    /* and the state that puts them there from the first mark does it
       here, before one has arrived */
    else if (private.spill == SPILL_ALWAYS && !_spill_now(&private))
    {
        XPOST_LOG_ERR("%d this run asked for every page's marks to be put in"
                      " %s and the first page's could not be", ioerror,
                      xpost_temp_dir());
        xpost_record_free(private.rec);
        return ioerror;
    }

    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
    {
        /* the state is the only thing that would have named the record,
           and it is not going to */
        xpost_record_free(private.rec);
        return VMerror;
    }

    xpost_stack_push(ctx->lo, ctx->os, devdic);

    /* The device this record paints through, built now and not at the
       first page. The record wraps it for the whole of its own life --
       it builds it, nothing else names it, and it gives it up when it is
       itself retired -- and a device built at the first page instead
       would be built inside whatever the job had opened by then; virtual
       memory goes back to what a save found, and a raster does not
       (data/recorddev.ps). What runs a procedure is the interpreter, so
       it is left on the execution stack rather than called, over a copy
       of the instance the procedure takes and the one this Create
       answers with. */
    make = xpost_dict_get(ctx, devdic, namedotplaymake);
    /* A recorder that paints no page of its own builds nothing to paint
       it in: what it holds is a drawing, played into a device that was
       going to be painted anyway. */
    if (xpost_object_get_type(make) == nulltype
        || xpost_object_get_type(make) == invalidtype)
        return 0;
    if (!xpost_object_is_exe(make))
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, devdic);
    if (!xpost_stack_push(ctx->lo, ctx->es, make))
        return execstackoverflow;
    return 0;
}

/* Put out the page the record holds, which means painting it: the class
   carries the procedure that builds a device to paint into, plays the
   record through it and puts out its page, and this hands the instance
   to it. What runs a procedure is the interpreter, so it is left on the
   execution stack rather than called. */
static int _emit(Xpost_Context *ctx,
                 Xpost_Object devdic)
{
    Xpost_Object privatestr;
    PrivateData private;
    Xpost_Object play;

    if (!_private_get(ctx, devdic, &privatestr, &private))
        return undefined;

    /* a released record has nothing left to put out */
    if (!private.rec)
        return 0;

    /* and one short of a mark has a page it cannot paint whole, so it
       puts out nothing rather than a page missing something */
    if (xpost_record_failed(private.rec))
    {
        XPOST_LOG_ERR("%d a page short of a mark is not put out", VMerror);
        return VMerror;
    }

    play = xpost_dict_get(ctx, devdic, namedotplaypage);
    if (!xpost_object_is_exe(play))
        return undefined;

    xpost_stack_push(ctx->lo, ctx->os, devdic);
    if (!xpost_stack_push(ctx->lo, ctx->es, play))
        return execstackoverflow;
    return 0;
}

/* rec page  .playbuilt  -
   This record has built this device to paint through: count the build
   and stamp the device with the count it was built at.

   The count is kept in the record's own state, which is not virtual
   memory and which a restore therefore does not rewind; the stamp is
   written into the device's dictionary, which is virtual memory and
   which a restore does. That difference is the whole point of the pair.
   A device built inside a save is gone at the restore, and the name the
   record reached it by goes back to naming the device before it -- one
   this record has already given up, whose page is released memory.
   Afterwards the record's count has moved on and the resurrected
   device's stamp has not, which is what tells them apart. */
static int _playbuilt(Xpost_Context *ctx,
                      Xpost_Object recdic,
                      Xpost_Object playdic)
{
    Xpost_Object privatestr;
    PrivateData private;

    if (!_private_get(ctx, recdic, &privatestr, &private))
        return undefined;
    private.playgen++;
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;
    return xpost_dict_put(ctx, playdic, namedotplaygen,
                          xpost_int_cons((integer)private.playgen));
}

/* rec page  .playkept  bool
   Whether this device is the one the record is carrying now, rather
   than one it built, gave up, and has been left naming again by a
   restore (.playbuilt above). */
static int _playkept(Xpost_Context *ctx,
                     Xpost_Object recdic,
                     Xpost_Object playdic)
{
    Xpost_Object privatestr;
    Xpost_Object stamp;
    PrivateData private;

    if (!_private_get(ctx, recdic, &privatestr, &private))
        return undefined;
    stamp = xpost_dict_get(ctx, playdic, namedotplaygen);
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_object_get_type(stamp) == integertype
                                     && stamp.int_.val >= 0
                                     && (unsigned int)stamp.int_.val
                                        == private.playgen));
    return 0;
}

/* Give up what this device holds: the record, and the device the record
   is painted through.

   The record builds that device, is the only thing that names it, and so
   is the only thing that can retire it. What it holds is a raster --
   memory outside the interpreter's own, which the collector neither
   reaches nor gives back -- so a record retired without retiring it
   leaves that raster behind for the life of the process.

   Released from here rather than by running the class's /Destroy through
   the interpreter, because this method is called from places no
   procedure can run: restore is one operator and not an interpreter loop
   (xpost_dev_generic.c). The operator run is the one recorded with the
   painter's own state when that state was issued, which is what a page
   device is retired by everywhere else and is not the slot the program
   can write to. A painter whose page is virtual memory the collector
   already owns carries no such record and needs none.

   The painter is unnamed before it is released, so a second Destroy
   finds nothing rather than a device that has begun to give up its
   memory. */
static int _destroy(Xpost_Context *ctx,
                    Xpost_Object devdic)
{
    Xpost_Object privatestr;
    Xpost_Object play;
    PrivateData private;
    int held = _private_get(ctx, devdic, &privatestr, &private);

    play = xpost_dict_get(ctx,
                          xpost_dict_get(ctx, devdic, namedotstate),
                          namedotplaydev);
    if (xpost_object_get_type(play) == dicttype)
    {
        unsigned int release = xpost_handle_device_release(ctx, play);

        (void)xpost_dict_undef(ctx,
                   xpost_dict_get(ctx, devdic, namedotstate),
                   namedotplaydev);
        (void)xpost_dict_undef(ctx,
                   xpost_dict_get(ctx, devdic, namedotstate),
                   namedotplayrows);
        /* counted as a build is counted, so that a restore leaving this
           record naming the device again finds a stamp that has been
           left behind (.playbuilt above) */
        if (held)
            private.playgen++;
        if (release && xpost_stack_push(ctx->lo, ctx->os, play))
        {
            int res = xpost_operator_exec(ctx, release);

            if (res)
                XPOST_LOG_ERR("%s retiring the device a record paints"
                              " through", errorname[res]);
        }
    }

    if (!held)
        return undefined;

    /* the record and the levels a replay of it descended through: the
       same release the collector runs, written once above. Retiring the
       device this record paints through is the difference, and it stays
       here -- it runs an operator, which the collector cannot do. */
    _reclaim(&private);
    /* store the cleared pointer back so a repeated destroy is a no-op */
    if (!xpost_dev_private_put(ctx, privatestr, &private, sizeof(private)))
        return VMerror;
    return 0;
}

/* operator function to instantiate a new recording device */
static int newrecorddevice(Xpost_Context *ctx,
                           Xpost_Object width,
                           Xpost_Object height)
{
    Xpost_Object classdic;
    int ret;

    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_RECORDDEVICE"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic,
                                         xpost_name_cons(ctx, "Create"))))
        return execstackoverflow;
    return 0;
}

/* The slots a recorder is built from: the five marking methods, the
   three a page device must have, the read, and the three entries the
   painters look for rather than dispatch.

   Named here because a recorder is built more than once in a run -- one
   for the page and one for each drawing held for it -- and asking for a
   method twice would register a second signature for the same call. The
   suite is made once per colour count and put into every class after. */
static const char *const _suiteslot[] =
{
    "Create", "PutPix", "GetPix", "BlendPix", "DrawLine", "FillRect",
    "FillPoly", "Emit", "Destroy",
    ".recordimage", ".recordglyph", "ScreenChanged", ".recordplace"
};

#define RECORD_SUITE_SLOTS ((int)(sizeof _suiteslot / sizeof *_suiteslot))

/* The opcodes of the suite, and not the operator objects: what is kept
   in C is a number the operator table is indexed by rather than an
   object, so there is nothing here for the collector to be told about
   (tests/check-c-held-objects.sh). */
static unsigned int _suite[2][RECORD_SUITE_SLOTS];
static int _suite_made[2];

/* Fill a class's slots with the recorder's suite at this colour count,
   making the suite if it has not been made. */
static int _install_suite(Xpost_Context *ctx, Xpost_Object classdic, int ncomp)
{
    /* This device's whole suite, for a target declaring DeviceRGB. It is
       the five marking methods a record holds and nothing else that
       marks: a method it did not bring is resolved above the device into
       these, and one it brought would be a call the record has no entry
       for. */
    static const Xpost_Dev_Method methods[] =
    {
        { "Create", "recordCreate", (Xpost_Op_Func)_create, XPOST_DEV_M_CREATE },
        { "PutPix", "recordPutPix", (Xpost_Op_Func)_putpix, XPOST_DEV_M_PUTPIX },
        { "GetPix", "recordGetPix", (Xpost_Op_Func)_getpix, XPOST_DEV_M_GETPIX },
        { "BlendPix", "recordBlendPix", (Xpost_Op_Func)_blendpix, XPOST_DEV_M_BLEND },
        { "DrawLine", "recordDrawLine", (Xpost_Op_Func)_drawline, XPOST_DEV_M_LINE },
        { "FillRect", "recordFillRect", (Xpost_Op_Func)_fillrect, XPOST_DEV_M_RECT },
        { "FillPoly", "recordFillPoly", (Xpost_Op_Func)_fillpoly, XPOST_DEV_M_POLY },
        { "Emit", "recordEmit", (Xpost_Op_Func)_emit, XPOST_DEV_M_PAGE },
        { "Destroy", "recordDestroy", (Xpost_Op_Func)_destroy, XPOST_DEV_M_PAGE }
    };

    /* ... and the same suite for a target declaring DeviceGray, whose
       marks carry one colour value where these carry three. Two tables
       and not one, because a method's operand count is fixed when it is
       installed and an entry point's signature is fixed when it is
       compiled. The read is in both: its operands do not follow the
       colour space, so there is one of it. */
    static const Xpost_Dev_Method greymethods[] =
    {
        { "Create", "recordCreate", (Xpost_Op_Func)_create, XPOST_DEV_M_CREATE },
        { "PutPix", "recordGrayPutPix", (Xpost_Op_Func)_putpix_g, XPOST_DEV_M_PUTPIX },
        { "GetPix", "recordGetPix", (Xpost_Op_Func)_getpix, XPOST_DEV_M_GETPIX },
        { "BlendPix", "recordGrayBlendPix", (Xpost_Op_Func)_blendpix_g, XPOST_DEV_M_BLEND },
        { "DrawLine", "recordGrayDrawLine", (Xpost_Op_Func)_drawline_g, XPOST_DEV_M_LINE },
        { "FillRect", "recordGrayFillRect", (Xpost_Op_Func)_fillrect_g, XPOST_DEV_M_RECT },
        { "FillPoly", "recordGrayFillPoly", (Xpost_Op_Func)_fillpoly_g, XPOST_DEV_M_POLY },
        { "Emit", "recordEmit", (Xpost_Op_Func)_emit, XPOST_DEV_M_PAGE },
        { "Destroy", "recordDestroy", (Xpost_Op_Func)_destroy, XPOST_DEV_M_PAGE }
    };

    Xpost_Object op;
    int suite = ncomp == 1 ? 0 : 1;
    int i, ret;

    /* the suite as it was made, where it has been made: a class built
       after the first is filled from what the first registered */
    if (_suite_made[suite])
    {
        for (i = 0; i < RECORD_SUITE_SLOTS; i++)
        {
            Xpost_Object key = xpost_name_cons(ctx, _suiteslot[i]);

            if (xpost_object_get_type(key) == invalidtype)
                return VMerror;
            ret = xpost_dict_put(ctx, classdic, key,
                                 xpost_operator_cons_opcode(
                                     (int)_suite[suite][i]));
            if (ret)
                return ret;
        }
        return 0;
    }

    op = xpost_operator_cons(ctx, "recordCreateCont",
                             (Xpost_Op_Func)_create_cont, 3,
                             integertype, integertype, dicttype);
    _create_cont_opcode = op.mark_.padw;

    ret = ncomp == 1
        ? xpost_dev_class_install(ctx, classdic, ncomp, 1, greymethods,
                                  XPOST_DEV_METHOD_COUNT(greymethods))
        : xpost_dev_class_install(ctx, classdic, ncomp, 1, methods,
                                  XPOST_DEV_METHOD_COUNT(methods));
    if (ret)
        return ret;

    /* How a sampled image reaches the record. It is not one of the
       device methods and is not dispatched as one: the image painter
       looks for it, and finding it writes the image down instead of
       painting it a rectangle per sample into a device that keeps no
       rows. A device method here would be a marking call the record
       holds no entry for, which is the one thing this class must not
       declare (tests/check-device-skeleton.sh). */
    op = xpost_operator_cons(ctx, "recordImage",
                             (Xpost_Op_Func)_recordimage, 3,
                             dicttype, arraytype, dicttype);
    ret = xpost_dict_put(ctx, classdic,
                         xpost_name_cons(ctx, ".recordimage"), op);
    if (ret)
        return ret;

    /* How a glyph reaches the record, for the same reason and by the
       same route. The glyph painter looks for it, and finding it hands
       over one coverage mask and a placement of it rather than the mark
       per inked pixel a device that paints is sent -- which costs a
       raster nothing and would cost this record tens of bytes a pixel.
       Like the image entry it is not a device method and is not
       dispatched as one: a method here would be a marking call the
       record holds no entry for, which is the one thing this class must
       not declare (tests/check-device-skeleton.sh).

       The mask itself does not come this way. It is handed over as the
       glyph is rendered, through xpost_dev_record_takemask, and this
       writes down which of the record's masks was painted where -- the
       two being separate because the order a string's glyphs are
       rendered in is not the order they are painted in, and a record
       holds its marks in the order they were painted. */
    op = ncomp == 1
        ? xpost_operator_cons(ctx, "recordGrayGlyph",
                              (Xpost_Op_Func)_recordglyph_g, 5,
                              numbertype, numbertype, numbertype, numbertype,
                              dicttype)
        : xpost_operator_cons(ctx, "recordGlyph",
                              (Xpost_Op_Func)_recordglyph, 7,
                              numbertype, numbertype, numbertype, numbertype,
                              numbertype, numbertype, dicttype);
    ret = xpost_dict_put(ctx, classdic,
                         xpost_name_cons(ctx, ".recordglyph"), op);
    if (ret)
        return ret;

    /* What the painting machinery calls where the screen changes, for a
       target that screens. Like the image entry it is not a device
       method and is not dispatched as one: a method here would be a
       marking call the record holds no entry for, which is the one
       thing this class must not declare. A recorder whose target does
       not screen declares no ScreenPaint, and nothing calls this. */
    op = xpost_operator_cons(ctx, "recordScreen",
                             (Xpost_Op_Func)_recordscreen, 1, dicttype);
    ret = xpost_dict_put(ctx, classdic,
                         xpost_name_cons(ctx, "ScreenChanged"), op);
    if (ret)
        return ret;

    /* How a drawing another recorder holds is placed in this one. It is
       not a device method either: what the machinery above a device
       paints is marks, and a placement is something a caller holding a
       drawing writes down. */
    op = xpost_operator_cons(ctx, "recordPlace",
                             (Xpost_Op_Func)_recordplace, 4,
                             dicttype, numbertype, numbertype, dicttype);
    ret = xpost_dict_put(ctx, classdic,
                         xpost_name_cons(ctx, ".recordplace"), op);
    if (ret)
        return ret;

    /* what was registered, kept so that a class built after this one is
       filled rather than registering the same calls again */
    for (i = 0; i < RECORD_SUITE_SLOTS; i++)
    {
        Xpost_Object key = xpost_name_cons(ctx, _suiteslot[i]);
        Xpost_Object slot;

        if (xpost_object_get_type(key) == invalidtype)
            return VMerror;
        slot = xpost_dict_get(ctx, classdic, key);
        if (xpost_object_get_type(slot) != operatortype)
            return undefined;
        _suite[suite][i] = slot.mark_.padw;
    }
    _suite_made[suite] = 1;
    return 0;
}

/* Whether a dictionary carries a name at all, which is what asking
   whether a device declares something comes to. */
static int _declares(Xpost_Context *ctx, Xpost_Object dic, const char *name)
{
    Xpost_Object key = xpost_name_cons(ctx, name);

    if (xpost_object_get_type(key) == invalidtype)
        return 0;
    return xpost_dict_known_key(ctx, xpost_context_select_memory(ctx, dic),
                                dic, key);
}

/* A class for a recorder that holds a drawing for this device, or a null
 * where a drawing cannot be held for it.
 *
 * A drawing is held as a record and is played into the device that
 * paints, so the recorder that takes it must be sent what that device
 * would have been sent and must hold all of it. Four things settle
 * whether it can be:
 *
 *   The colour. A mark carries one value per component of the space it
 *   was made in and is played by handing those values to a method whose
 *   operands the receiving device's space decides, so the recorder
 *   declares the device's space and component count and no other.
 *
 *   The path. A device that declares FillPath is handed whole paths and
 *   the clip shape beside them, neither of which a record holds; what
 *   reaches a device that declares none is already cut to the region.
 *   So a drawing is held for the second kind and not the first.
 *
 *   The screen. A device rendering a grey as a pattern of pixels reads
 *   the threshold under each pixel, which is device state rather than
 *   something a mark carries. A drawing whose marks changed that state
 *   would leave it changed for the marks after it wherever the drawing
 *   was placed, so no drawing is held for such a device.
 *
 *   The marks. A replay plays each mark by calling the method for its
 *   kind, so the device must offer all five.
 *
 * The class is built for each drawing rather than kept. What is kept is
 * the suite it is built from, since asking for a method twice would
 * register a second signature for the same call; the dictionary around
 * that suite costs a dictionary.
 */
static Xpost_Object _form_class(Xpost_Context *ctx, Xpost_Object devdic)
{
    /* what the recorder takes from the class every recorder is made
       from: how a page is cleared, how a class is copied and how an
       instance is made out of one, that it paints nothing, and the
       extent it will carry */
    static const char *const carry[] =
    {
        "Ground", ".copydict", ".instance", "NoOutput", "dimensions"
    };
    Xpost_Object src, cls, o;
    int ncomp, i, ret;

    o = xpost_dict_get(ctx, devdic, namedotncomp);
    if (xpost_object_get_type(o) != integertype
        || o.int_.val < 1 || o.int_.val > RECORD_MAXCOMP)
        return null;
    ncomp = (int)o.int_.val;
    if (!_declares(ctx, devdic, "nativecolorspace")
        || _declares(ctx, devdic, "FillPath")
        || _declares(ctx, devdic, "ScreenPaint"))
        return null;
    for (i = 0; i < (int)(sizeof nameslot / sizeof *nameslot); i++)
        if (!xpost_dict_known_key(ctx,
                                  xpost_context_select_memory(ctx, devdic),
                                  devdic, nameslot[i]))
            return null;

    src = xpost_dict_get(ctx, ctx->privatedict,
                         xpost_name_cons(ctx, ".xpost_RECORD"));
    if (xpost_object_get_type(src) != dicttype)
        return null;

    cls = xpost_dict_cons(ctx, 32);
    if (xpost_object_get_type(cls) != dicttype)
        return null;
    for (i = 0; i < (int)(sizeof carry / sizeof *carry); i++)
    {
        Xpost_Object key = xpost_name_cons(ctx, carry[i]);

        if (xpost_object_get_type(key) == invalidtype)
            return null;
        if (!xpost_dict_known_key(ctx, xpost_context_select_memory(ctx, src),
                                  src, key))
            continue;
        ret = xpost_dict_put(ctx, cls, key, xpost_dict_get(ctx, src, key));
        if (ret)
            return null;
    }
    /* and what it takes from the device it is standing in for */
    ret = xpost_dict_put(ctx, cls, namedotncomp, o);
    if (!ret)
        ret = xpost_dict_put(ctx, cls, namenativecolorspace,
                             xpost_dict_get(ctx, devdic, namenativecolorspace));
    if (!ret && _declares(ctx, devdic, "TextAlphaBits"))
        ret = xpost_dict_put(ctx, cls, nametextalphabits,
                             xpost_dict_get(ctx, devdic, nametextalphabits));
    if (!ret)
        ret = _install_suite(ctx, cls, ncomp);
    return ret ? null : cls;
}

/* w h DEVICE  .newformrecord  recdev
                               null
   A recorder to hold a drawing for this device, at a page of this
   extent, or a null where a drawing cannot be held for it.

   The extent is the page's, not the drawing's: the marks are the
   device's own coordinates and the clip they were cut to is the page's,
   so a recorder standing in for the device stands at the same extent. */
static int _newformrecord(Xpost_Context *ctx,
                          Xpost_Object width,
                          Xpost_Object height,
                          Xpost_Object devdic)
{
    Xpost_Object cls = _form_class(ctx, devdic);
    Xpost_Object create;

    if (xpost_object_get_type(cls) != dicttype)
    {
        xpost_stack_push(ctx->lo, ctx->os, null);
        return 0;
    }
    create = xpost_dict_get(ctx, cls, xpost_name_cons(ctx, "Create"));
    if (xpost_object_get_type(create) != operatortype)
        return undefined;
    xpost_stack_push(ctx->lo, ctx->os, width);
    xpost_stack_push(ctx->lo, ctx->os, height);
    xpost_stack_push(ctx->lo, ctx->os, cls);
    if (!xpost_stack_push(ctx->lo, ctx->es, create))
        return execstackoverflow;
    return 0;
}

/* The device this run's selection is played into, as the name a roster
   is keyed by, or a null where the run named none.

   It is the device the run asked for: selecting one whose page may
   arrive a band at a time is what hands the selection to this class
   (src/lib/xpost_interpreter.c), and the device that selection named is
   the device this record is standing in front of.

   The run has to have selected this class for the question to be about
   it. A run started as -d raster:bgra asked for a raster, and a program
   that switches to a record afterwards has not chosen anything for it to
   play into; such a record takes the target a run that named no device
   takes. So does a run that selected this class itself, which names the
   mechanism and no device to paint through. */
static Xpost_Object _selected_target(Xpost_Context *ctx, const char *device)
{
    Xpost_Object o;

    o = xpost_context_host_setting(ctx, "StartDevice");
    if (xpost_object_get_type(o) != nametype
        || xpost_dict_compare_objects(ctx, o,
                                      xpost_name_cons(ctx, device)) != 0)
        return null;

    o = xpost_context_host_setting(ctx, "StartDeviceAsked");
    if (xpost_object_get_type(o) != nametype
        || xpost_dict_compare_objects(ctx, o,
                                      xpost_name_cons(ctx, device)) == 0)
        return null;
    return o;
}

/* The driver to bring in before this record is specialised, or a null
   where there is none to bring in.
 *
 * The class of the device a record plays into is what the record is
 * specialised from, and a class a C driver defines is not there until
 * the driver has been loaded. Which driver that is the class states
 * (data/recorddev.ps); it is asked for only where the class is not
 * already in the private dictionary, so a driver loaded by anything
 * else is not loaded twice.
 */
static Xpost_Object _play_loader(Xpost_Context *ctx, Xpost_Object classdic)
{
    Xpost_Object target, targets, loaders, clsname, o;

    target = _selected_target(ctx, "record");
    if (xpost_object_get_type(target) != nametype)
        return null;
    targets = xpost_dict_get(ctx, classdic, namedotplaytargets);
    loaders = xpost_dict_get(ctx, classdic, namedotplayloaders);
    if (xpost_object_get_type(targets) != dicttype
        || xpost_object_get_type(loaders) != dicttype)
        return null;
    clsname = xpost_dict_get(ctx, targets, target);
    if (xpost_object_get_type(clsname) != nametype
        || xpost_object_get_type(xpost_dict_get(ctx, ctx->privatedict,
                                                clsname)) == dicttype)
        return null;
    o = xpost_dict_get(ctx, loaders, target);
    if (xpost_object_get_type(o) != nametype)
        return null;
    /* the loaders are operators the drivers install in systemdict, and
       a build without the driver installed none: such a name answers
       nothing here and the target is refused further on, where every
       other unbuilt target is refused */
    o = xpost_dict_get(ctx, xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0), o);
    return xpost_object_get_type(o) == operatortype ? o : null;
}

/* Specialise the .xpost_RECORD class: load it, copy it, and continue
   below with the copy. */
static int loadrecorddevice(Xpost_Context *ctx)
{
    Xpost_Object classdic, loader;
    int ret;

    ret = xpost_op_privatedict_load(ctx, xpost_name_cons(ctx, ".xpost_RECORD"));
    if (ret)
        return ret;
    classdic = xpost_stack_topdown_fetch(ctx->lo, ctx->os, 0);
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_operator_cons_opcode(_loadrecorddevicecont_opcode)))
        return execstackoverflow;
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          xpost_dict_get(ctx, classdic, namedotcopydict)))
        return execstackoverflow;
    /* and, ahead of both, the driver of the device this record plays
       into: the copy above is made from this class and the continuation
       takes the target's colour space off the target's class, so the
       class has to be there by the time it looks */
    loader = _play_loader(ctx, classdic);
    if (xpost_object_get_type(loader) == operatortype
        && !xpost_stack_push(ctx->lo, ctx->es, loader))
        return execstackoverflow;
    return 0;
}

/* Settle which device paints this record's page, and take from it what
   this class must declare to be sent the marks that device would have
   been sent.

   The roster of the devices a record can be played into is the class's,
   with the reasons a device is on it (data/recorddev.ps); what this adds
   is the run's choice among them. The refusal below stands over a class
   whose roster does not carry the device the run selected: a selection
   naming a device that cannot be played into is refused before the run
   begins, where the modes a device takes are read
   (src/lib/xpost_interpreter.c), so it is reached by the two rosters
   disagreeing and not by anything a run can spell. The
   colour space and the component count come off the target because a
   mark carries one value per component of the space it was made in and
   is played by handing those values to a method whose operands the
   target's space decides; the glyph coverage comes off it because a
   target that thresholds a glyph's edge pixels to whole pixels would
   otherwise be played the coverage-weighted blends this device was sent
   in its place.

   Answers the component count the class is to be installed at. */
static int _play_target(Xpost_Context *ctx, Xpost_Object classdic,
                        int *ncomp)
{
    Xpost_Object target, targets, clsname, cls, o;
    int ret;

    targets = xpost_dict_get(ctx, classdic, namedotplaytargets);
    if (xpost_object_get_type(targets) != dicttype)
    {
        XPOST_LOG_ERR("%d the recording class carries no roster of the"
                      " devices it can be played into", undefined);
        return undefined;
    }

    target = _selected_target(ctx, "record");
    if (xpost_object_get_type(target) != nametype)
        target = xpost_name_cons(ctx, "ppm");

    clsname = xpost_dict_get(ctx, targets, target);
    if (xpost_object_get_type(clsname) != nametype)
    {
        XPOST_LOG_ERR("%d a record is asked to be played into a device that"
                      " is not one it can be played into", rangecheck);
        return rangecheck;
    }
    cls = xpost_dict_get(ctx, ctx->privatedict, clsname);
    if (xpost_object_get_type(cls) != dicttype)
    {
        /* a device the roster names and this build did not make */
        XPOST_LOG_ERR("%d the device a record is to be played into was not"
                      " loaded", undefined);
        return undefined;
    }

    o = xpost_dict_get(ctx, cls, namedotncomp);
    if (xpost_object_get_type(o) != integertype
        || o.int_.val < 1 || o.int_.val > RECORD_MAXCOMP)
    {
        XPOST_LOG_ERR("%d the device a record is to be played into takes a"
                      " colour no record holds", rangecheck);
        return rangecheck;
    }
    *ncomp = o.int_.val;
    ret = xpost_dict_put(ctx, classdic, namedotncomp, o);
    if (!ret)
        ret = xpost_dict_put(ctx, classdic, namenativecolorspace,
                             xpost_dict_get(ctx, cls, namenativecolorspace));
    if (!ret)
        ret = xpost_dict_put(ctx, classdic, nametextalphabits,
                             xpost_dict_get(ctx, cls, nametextalphabits));
    /* A device rendering a grey as a pattern of pixels declares
       ScreenPaint, and the painting machinery then keeps that screen's
       cell on it. A recorder standing in for such a device declares it
       as well -- not because it screens anything, but because what it
       writes down will be screened when it is played, and the screen a
       mark was made under is state it can only be told about while the
       page is being drawn. A recorder for a target that does not screen
       is never told, and holds no screen. */
    if (!ret)
    {
        Xpost_Object sp = xpost_name_cons(ctx, "ScreenPaint");

        if (xpost_object_get_type(sp) == invalidtype)
            return VMerror;
        if (xpost_dict_known_key(ctx, xpost_context_select_memory(ctx, cls),
                                 cls, sp))
            ret = xpost_dict_put(ctx, classdic, sp,
                                 xpost_dict_get(ctx, cls, sp));
    }
    /* What the target says about how a page of its is held, said here as
       well. These are answers about the raster this record's page is
       painted in, and a record has no raster of its own to answer them
       from: whether a page may arrive a band at a time, what one row of
       it costs, whether the row shown where the device holds no pixel is
       the same row down the page, and which process colour models the
       device offers. A record answering any of them for itself would be
       answering about a device it is standing in front of, and would
       answer differently from the device that actually holds the page.

       Taken only where the target states one. Each has a default that
       holds where a class is silent -- no bands, no price, one ground
       row, no model but the native one -- and a record standing in for a
       silent class must be silent in the same way. */
    if (!ret)
    {
        static const char *const facts[] =
        {
            "BandedPage", ".rowcost", ".groundvaries", ".colormodels"
        };
        size_t i;

        for (i = 0; !ret && i < sizeof facts / sizeof *facts; i++)
        {
            Xpost_Object key = xpost_name_cons(ctx, facts[i]);

            if (xpost_object_get_type(key) == invalidtype)
                return VMerror;
            if (xpost_dict_known_key(ctx,
                                     xpost_context_select_memory(ctx, cls),
                                     cls, key))
                ret = xpost_dict_put(ctx, classdic, key,
                                     xpost_dict_get(ctx, cls, key));
        }
    }
    if (!ret)
        ret = xpost_dict_put(ctx, classdic, namedotplayclass, clsname);
    return ret;
}

/* Fill the class's method slots with this device's operators and define
   the class and its maker in the private dictionary. */
static int loadrecorddevicecont(Xpost_Context *ctx,
                                Xpost_Object classdic)
{
    Xpost_Object op;
    int ncomp;
    int ret;

    /* which device paints this record's page, before anything is
       installed: it decides what every marking method here looks like */
    ret = _play_target(ctx, classdic, &ncomp);
    if (ret)
        return ret;

    ret = _install_suite(ctx, classdic, ncomp);
    if (ret)
        return ret;

    /* The class and its maker live in the private dictionary, beside the
       classes the boot files define: a program reaches a device through
       the page-device request, and the machinery reaches the class by
       name here. Nothing of the driver's is defined where a program
       could shadow it. */
    ret = xpost_dev_class_publish(ctx, ".xpost_RECORDDEVICE", classdic);
    if (ret)
        return ret;

    op = xpost_operator_cons(ctx, "newrecorddevice",
                             (Xpost_Op_Func)newrecorddevice, 2,
                             integertype, integertype);
    ret = xpost_dict_put_internal(ctx, ctx->privatedict,
                         xpost_name_cons(ctx, "newrecorddevice"), op);
    if (ret)
        return ret;

    return 0;
}

/* Installs the record device's operators and caches the names its
   marks are keyed by. What is installed here is the replay side and
   the record's own accounting; the marking suites are put in place by
   the class, which is where their operand counts are fixed. */
int xpost_oper_init_record_device_ops (Xpost_Context *ctx,
                Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;
    int i;

    /* factor-out name lookups from the operators (optimization) */
    if (xpost_object_get_type((namePrivate = xpost_name_cons(ctx, "Private"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namewidth = xpost_name_cons(ctx, "width"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameheight = xpost_name_cons(ctx, "height"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotstate = xpost_name_cons(ctx, ".state"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotcopydict = xpost_name_cons(ctx, ".copydict"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotplaypage = xpost_name_cons(ctx, ".playpage"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotplaymake = xpost_name_cons(ctx, ".playmake"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotplaydev = xpost_name_cons(ctx, ".playdev"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotplayrows = xpost_name_cons(ctx, ".playrows"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotplaygen = xpost_name_cons(ctx, ".playgen"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotground = xpost_name_cons(ctx, ".ground"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namenativecolorspace = xpost_name_cons(ctx, "nativecolorspace"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotncomp = xpost_name_cons(ctx, ".ncomp"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nametextalphabits = xpost_name_cons(ctx, "TextAlphaBits"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotplaytargets = xpost_name_cons(ctx, ".playtargets"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotplayloaders = xpost_name_cons(ctx, ".playloaders"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedotplayclass = xpost_name_cons(ctx, ".playclass"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameslot[XPOST_RECORD_PUTPIX] = xpost_name_cons(ctx, "PutPix"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameslot[XPOST_RECORD_BLENDPIX] = xpost_name_cons(ctx, "BlendPix"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameslot[XPOST_RECORD_DRAWLINE] = xpost_name_cons(ctx, "DrawLine"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameslot[XPOST_RECORD_FILLRECT] = xpost_name_cons(ctx, "FillRect"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameslot[XPOST_RECORD_FILLPOLY] = xpost_name_cons(ctx, "FillPoly"))) == invalidtype)
        return VMerror;
    for (i = 0; i < BK_COUNT; i++)
        if (xpost_object_get_type((namebdkey[i] = xpost_name_cons(ctx, _bdname[i])))
            == invalidtype)
            return VMerror;

    /* The suite is a set of operators and an operator is an index into
       this context's table, so what was registered in another context's
       is not this one's to hand out. Cleared where a context installs
       its operators, which is the one place a new table appears. */
    _suite_made[0] = _suite_made[1] = 0;

    optab = xpost_operator_table(ctx->gl);
    op = xpost_operator_cons(ctx, "loadrecorddevice", (Xpost_Op_Func)loadrecorddevice, 0); INSTALL;
    op = xpost_operator_cons(ctx, "loadrecorddevicecont", (Xpost_Op_Func)loadrecorddevicecont, 1, dicttype);
    _loadrecorddevicecont_opcode = op.mark_.padw;

    /* The replay is registered here rather than with the device's
       methods, because a run reaches it through .internaldict and what
       puts an operator there is the relocation pass that runs once,
       during start-up, before any device has been made. */
    op = xpost_operator_cons(ctx, ".replaypage", (Xpost_Op_Func)_replaypage, 2,
                             dicttype, dicttype);
    op = xpost_operator_cons(ctx, ".replaypage",
                             (Xpost_Op_Func)_replaypage_rows, 4,
                             dicttype, dicttype, numbertype, numbertype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".replaystep", (Xpost_Op_Func)_replay_step, 2,
                             dicttype, dicttype);
    _replay_step_opcode = op.mark_.padw;
    op = xpost_operator_cons(ctx, ".replayplace", (Xpost_Op_Func)_replayplace,
                             4, dicttype, dicttype, numbertype, numbertype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".newformrecord",
                             (Xpost_Op_Func)_newformrecord, 3,
                             integertype, integertype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordbox", (Xpost_Op_Func)_recordbox, 1,
                             dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordplaces",
                             (Xpost_Op_Func)_recordplaces, 1,
                             dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordcost", (Xpost_Op_Func)_recordcost, 1,
                             dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordplays", (Xpost_Op_Func)_recordplays,
                             1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordplayed", (Xpost_Op_Func)_recordplayed,
                             1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordscreens",
                             (Xpost_Op_Func)_recordscreens, 1,
                             dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordglyphs",
                             (Xpost_Op_Func)_recordglyphs, 1,
                             dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".playscreen", (Xpost_Op_Func)_playscreen,
                             2, dicttype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".playbuilt", (Xpost_Op_Func)_playbuilt,
                             2, dicttype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".playkept", (Xpost_Op_Func)_playkept,
                             2, dicttype, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordimagerows",
                             (Xpost_Op_Func)_recordimagerows, 1,
                             dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordground", (Xpost_Op_Func)_recordground,
                             3, dicttype, numbertype, numbertype); INSTALL;
    op = xpost_operator_cons(ctx, ".playsaving", (Xpost_Op_Func)_playsaving,
                             2, dicttype, numbertype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordsaving", (Xpost_Op_Func)_recordsaving,
                             1, dicttype); INSTALL;
    op = xpost_operator_cons(ctx, ".recordspill", (Xpost_Op_Func)_recordspill,
                             1, dicttype); INSTALL;

    return 0;
}
