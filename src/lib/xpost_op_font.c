/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_font.c
 * @brief Installs the font and text-showing operators.
 *
 * The implementations, and the one function that installs them.
 *
 * PLRM 5.1 is what these answer to: a character is the abstract symbol
 * and a glyph one rendering of it, and showing text is selecting a font
 * (5.1.2), then painting the glyphs its encoding names (5.1.1) at
 * positions the font's metrics and the graphics state settle (5.1.4).
 * The encoding that turns a string's bytes into glyph names is 5.3.
 *
 * Installed into systemdict as:
 *
 * findfont setfont show ashow widthshow awidthshow stringwidth
 *
 * What each showing operator adds to show is the whole of the difference
 * between them: a per-glyph displacement, a displacement for one character,
 * or both.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h> /* NULL strtod */
#include <stddef.h>

#include <assert.h>
#include <ctype.h> /* isdigit, isxdigit, isspace */
#include <limits.h>
#include <math.h> /* sqrt */
#include <stdio.h>
#include <string.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_font.h"
#include "xpost_main.h" /* to be called when the library goes down */
#include "xpost_strbuf.h"
#include "xpost_file.h"
#include "xpost_save.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_dict.h"

//#include "xpost_interpreter.h"
#include "xpost_garbage.h"
#include "xpost_operator.h"
#include "xpost_op_font.h"
#include "xpost_dev_generic.h" /* pdfwrite accumulator access for glyph outlines */
#include "xpost_dev_driver.h" /* the device grid the clip region's bounds sit on */
#include "xpost_dev_record.h" /* where a glyph's coverage goes whole */
#include "xpost_handle.h" /* the handle a font dictionary names its face by */

/* What a font dictionary's /Private names: the face its font program
   was opened as. The face is held outside virtual memory, so what the
   dictionary carries under that key is a handle on the block holding it
   rather than the block itself -- a font dictionary is an ordinary
   dictionary and what it holds under any key is whatever was last
   stored there.

   Whether the face is the block's says who gives it up. A face opened
   for one dictionary -- the program a Type 42, Type 1 or CID font
   dictionary carries, assembled and opened here -- is named by that
   block alone and goes with it. A face findfont opened by name is the
   name cache's: the cache hands the same face to every dictionary that
   name produces, and holds it after the last of them has gone. */
typedef struct fontdata
{
    void *face;
    int own;
} fontdata;

/* Every name these operators reach an object by, resolved once when
   the operators are installed. Building a name from characters walks
   the name tree -- twice for a name the interpreter installed, since
   the local bank is searched first and misses -- and the text
   operators resolved a dozen or more per call: a show paid for its
   walk to the graphics state, the font, the device and the colour over
   again for every string it painted. The table is what the installer
   walks; the statics are what the operators read. */
static Xpost_Object name_dotemunits;
static Xpost_Object name_dotfacecache;
static Xpost_Object name_dotgraphicsdict;
static Xpost_Object name_dotmarked;
static Xpost_Object name_dotnotdef;
static Xpost_Object name_dotpagestate;
static Xpost_Object name_dotrecordglyph;
static Xpost_Object name_BlendPix;
static Xpost_Object name_CIDFontType;
static Xpost_Object name_CharStrings;
static Xpost_Object name_Encoding;
static Xpost_Object name_FDArray;
static Xpost_Object name_FID;
static Xpost_Object name_FillRect;
static Xpost_Object name_FontBBox;
static Xpost_Object name_FontMatrix;
static Xpost_Object name_FontName;
static Xpost_Object name_FontType;
static Xpost_Object name_GlyphData;
static Xpost_Object name_GlyphExtents;
static Xpost_Object name_Metrics;
static Xpost_Object name_PaintType;
static Xpost_Object name_Private;
static Xpost_Object name_PutPix;
static Xpost_Object name_ScreenPaint;
static Xpost_Object name_Subrs;
static Xpost_Object name_TextAlphaBits;
static Xpost_Object name_VectorGlyphs;
static Xpost_Object name_VectorSyntax;
static Xpost_Object name_buf;
static Xpost_Object name_c;
static Xpost_Object name_clipbox;
static Xpost_Object name_clipcache;
static Xpost_Object name_clipspans;
static Xpost_Object name_dotmarkedcolor;
static Xpost_Object name_csreal;
static Xpost_Object name_currfont;
static Xpost_Object name_currgstate;
static Xpost_Object name_currmatrix;
static Xpost_Object name_currpath;
static Xpost_Object name_device;
static Xpost_Object name_flushpage;
static Xpost_Object name_h;
static Xpost_Object name_height;
static Xpost_Object name_ink;
static Xpost_Object name_interp;
static Xpost_Object name_l;
static Xpost_Object name_m;
static Xpost_Object name_mark;
static Xpost_Object name_mat;
static Xpost_Object name_sepindex;
static Xpost_Object name_septint;
static Xpost_Object name_serial;
static Xpost_Object name_sfnts;
static Xpost_Object name_width;
static Xpost_Object name_xadvance;
static Xpost_Object name_yadvance;

static struct { Xpost_Object *slot; const char *spelling; } _op_font_names[] =
{
    { &name_dotemunits, ".emunits" },
    { &name_dotfacecache, ".facecache" },
    { &name_dotgraphicsdict, ".graphicsdict" },
    { &name_dotmarked, ".marked" },
    { &name_dotnotdef, ".notdef" },
    { &name_dotpagestate, ".pagestate" },
    { &name_dotrecordglyph, ".recordglyph" },
    { &name_BlendPix, "BlendPix" },
    { &name_CIDFontType, "CIDFontType" },
    { &name_CharStrings, "CharStrings" },
    { &name_Encoding, "Encoding" },
    { &name_FDArray, "FDArray" },
    { &name_FID, "FID" },
    { &name_FillRect, "FillRect" },
    { &name_FontBBox, "FontBBox" },
    { &name_FontMatrix, "FontMatrix" },
    { &name_FontName, "FontName" },
    { &name_FontType, "FontType" },
    { &name_GlyphData, "GlyphData" },
    { &name_GlyphExtents, "GlyphExtents" },
    { &name_Metrics, "Metrics" },
    { &name_PaintType, "PaintType" },
    { &name_Private, "Private" },
    { &name_PutPix, "PutPix" },
    { &name_ScreenPaint, "ScreenPaint" },
    { &name_Subrs, "Subrs" },
    { &name_TextAlphaBits, "TextAlphaBits" },
    { &name_VectorGlyphs, "VectorGlyphs" },
    { &name_VectorSyntax, "VectorSyntax" },
    { &name_buf, "buf" },
    { &name_c, "c" },
    { &name_clipbox, "clipbox" },
    { &name_clipcache, "clipcache" },
    { &name_clipspans, "clipspans" },
    { &name_dotmarkedcolor, ".markedcolor" },
    { &name_csreal, "csreal" },
    { &name_currfont, "currfont" },
    { &name_currgstate, "currgstate" },
    { &name_currmatrix, "currmatrix" },
    { &name_currpath, "currpath" },
    { &name_device, "device" },
    { &name_flushpage, "flushpage" },
    { &name_h, "h" },
    { &name_height, "height" },
    { &name_ink, "ink" },
    { &name_interp, "interp" },
    { &name_l, "l" },
    { &name_m, "m" },
    { &name_mark, "mark" },
    { &name_mat, "mat" },
    { &name_sepindex, "sepindex" },
    { &name_septint, "septint" },
    { &name_serial, "serial" },
    { &name_sfnts, "sfnts" },
    { &name_width, "width" },
    { &name_xadvance, "xadvance" },
    { &name_yadvance, "yadvance" },
};

/* --- the font program a face was opened over -------------------------
   A face reads its program where it lies for as long as it might build
   another glyph, so the bytes are the font's to hold and to give up. They
   live outside virtual memory, reached through a handle, and go when the
   font that named them does. */

/* Give up the face the block holds, where the face is the block's.
   Called from the collector with the block, so it touches nothing in
   virtual memory: what it reaches is the face and what the font
   machinery holds against it. A block already given up leaves this
   nothing to do. */
static void _reclaim(void *block)
{
    fontdata *fd = block;

    if (fd->own)
        xpost_font_face_free(fd->face);
    fd->face = NULL;
    fd->own = 0;
}

/* The block a font dictionary's /Private names, or NULL where the value
   there is not a handle on one. A font dictionary is copied --
   scalefont and makefont copy one, and a re-encoded copy of a findfont
   dictionary shares its face -- so a handle names its block whichever
   dictionary presents it. */
static XPOST_NOINLINE fontdata *_font_data(Xpost_Context *ctx,
                                           Xpost_Object fontdict)
{
    return (fontdata *)xpost_handle_block(ctx,
               xpost_dict_get(ctx, fontdict, name_Private),
               XPOST_HANDLE_FONT, sizeof(fontdata));
}

/* Give a font dictionary a face, saying whether the face is the
   dictionary's to give up, and releasing the face it holds where that
   one was. The dictionary keeps the block it was issued, so the copies
   of it sharing that face reach the new one; a dictionary holding a
   handle issued elsewhere is issued one of its own and leaves the other
   face to the dictionary it belongs to.

   A block issued here is issued with the release the collector runs
   when it reclaims a block nothing told anyone to give up, which is the
   arrangement every other class holding memory out there makes. Nothing
   retires a font: a font dictionary is ordinary virtual memory and goes
   when nothing names it. */
static int _font_data_set(Xpost_Context *ctx,
                          Xpost_Object fontdict,
                          void *face,
                          int own)
{
    Xpost_Object key = name_Private;
    Xpost_Object anchor;
    fontdata *fd;
    int ret;

    fd = (fontdata *)xpost_handle_block_of(ctx,
             xpost_dict_get(ctx, fontdict, key), fontdict,
             XPOST_HANDLE_FONT, sizeof(fontdata));
    if (!fd)
    {
        ret = xpost_handle_cons(ctx, fontdict, key, &anchor,
                                XPOST_HANDLE_FONT, sizeof(fontdata),
                                _reclaim);
        if (ret)
            return ret;
        fd = (fontdata *)xpost_handle_block(ctx, anchor, XPOST_HANDLE_FONT,
                                            sizeof(fontdata));
        if (!fd)
            return VMerror;
    }
    else if (fd->own)
        xpost_font_face_free(fd->face);
    fd->face = face;
    fd->own = own;
    return 0;
}

/* Give a font dictionary the bounding box of the face built for it,
   where the dictionary declares no box of its own.

   FontBBox states the box in the glyph coordinate system (PLRM
   Table 5.3) and FontMatrix is the map from that system to user space
   (PLRM Table 5.2), so the two entries are one statement and neither
   can be read without the other. A box written over a declared one in
   any other space therefore moves the rectangle the font occupies, and
   moves it silently, since the matrix beside it still says what it
   said. The glyph coordinate system is shared with every other entry
   whose values are given in it -- StrokeWidth, the Metrics
   dictionaries, what CDevProc is handed and answers, and the
   underline entries of FontInfo (PLRM 5.8.2) -- so it is not the
   loader's to restate: a program declares that space once, by the two
   entries together, and what it declared stands.

   What is filled in is the entry a dictionary left out. There the box
   is stated in the character space the font's type conventionally
   uses, which is what `em` names.

   The entry is read by presence rather than by value. A Type 1 font's
   FontBBox is frequently an executable array (PLRM 5.2.1), and asking
   whether the key is there neither fetches what is under it nor runs
   it. */
static int _font_bbox_declare(Xpost_Context *ctx,
                              Xpost_Object fontdict,
                              void *face,
                              real em)
{
    Xpost_Object fontbbox;
    Xpost_Object fontbboxarray[4];

    if (xpost_dict_known_key(ctx, xpost_context_select_memory(ctx, fontdict),
                             fontdict, name_FontBBox))
        return 0;

    /* executable: what a program reads back is the array's own
       attribute, and this one is built rather than scanned */
    fontbbox = xpost_object_cvx(xpost_array_cons(ctx, 4));
    xpost_font_face_get_bbox(face, fontbboxarray, em);
    if (!xpost_memory_put(xpost_context_select_memory(ctx, fontbbox),
                          xpost_object_get_ent(fontbbox),
                          0, 4 * sizeof(Xpost_Object), fontbboxarray))
        return VMerror;
    return xpost_dict_put(ctx, fontdict, name_FontBBox, fontbbox);
}

/* One pixel-row band of the clip region: the columns [lo, hi) of row
   band the region covers. This is the region as .regionmeet resolves
   it, read back from the array the clip's cache holder keeps. */
typedef struct clipband
{
    int band;
    int lo, hi;
} clipband;

/* The key a device that writes down what it is asked to paint declares,
   under which the glyph painter finds the entry a glyph's coverage goes
   to whole. Taken up at start-up: the painter asks the device for it
   once per glyph. */

/* The bits of coverage a glyph raster carries. A rasterised glyph
   arrives as one byte of coverage per pixel, so this is the most a
   device asking for a glyph's edge coverage can be sent, whatever
   number it asks for. */
#define COVERAGE_BITS 8

/* How the clip region in force meets device pixels. The glyph raster
   route paints pixels straight through the device, so it meets the
   region itself rather than through the fill pipeline; what it meets is
   the same set of pixels, worked out by .showclip in data/font.ps and
   left in the clip's own cache holder. */
enum
{
    CLIP_ALL,    /* nothing worked out: the raster is not narrowed */
    CLIP_BOX,    /* the region is a rectangle: the pixel bounds below */
    CLIP_BANDS   /* the region resolved to the bands below */
};

/* per-text-operator rendering configuration, gathered once from the
   font dictionary, the device dictionary and the graphics state */
typedef struct textstate
{
    Xpost_Object encoding;  /* the font's /Encoding array, or invalid */
    Xpost_Object charstrings; /* the font's /CharStrings dict, or invalid */
    Xpost_Object metrics;   /* the font's /Metrics dict, or invalid */
    real cdmat[4];          /* character space -> device space (FontMatrix o CTM) */
    int cdmat_ok;           /* the matrix above is usable */
    Xpost_Object blendpix;  /* the device's BlendPix method, or invalid */
    int blend;              /* bits of edge coverage the device is sent, 0 for none */
    int vector;             /* the device consumes glyph outlines, not bitmaps */
    int extents;            /* the device consumes glyph ink extents, not marks */
    Xpost_Object fillrect;  /* the device's FillRect, for extent reporting */
    int sepindex;           /* separation registered with the device, or -1 */
    double septint;         /* the separation's tint */
    int clipkind;           /* one of the CLIP_ constants above */
    int cx0, cy0, cx1, cy1; /* the region's pixel bounds, half-open */
    const clipband *bands;  /* the region's bands, ascending, when CLIP_BANDS */
    int nbands;
} textstate;

/* --- matrices, metrics and the encoded name --------------------------
   The small conversions every showing operator needs before it can ask
   the face for anything: what the font matrix does apart from where it
   puts the origin, what a character code is called, and how far the pen
   moves afterwards. */

/* The linear part of a six-element matrix object: the four numbers that
   map a direction, ahead of the translation the last two describe. Every
   element is written before any is read. */
static
void _matrix_linear_part(Xpost_Context *ctx, Xpost_Object psmat, real m[4])
{
    m[0] = xpost_object_number(xpost_array_get(ctx, psmat, 0));
    m[1] = xpost_object_number(xpost_array_get(ctx, psmat, 1));
    m[2] = xpost_object_number(xpost_array_get(ctx, psmat, 2));
    m[3] = xpost_object_number(xpost_array_get(ctx, psmat, 3));
}

/* the linear part of character space -> device space: the font
   dictionary's FontMatrix composed with the CTM (row convention:
   x' = e0 x + e2 y, y' = e1 x + e3 y). Returns 0 when either matrix
   is unusable. */
static
int _char_device_matrix(Xpost_Context *ctx,
                        Xpost_Object gs,
                        Xpost_Object fontdict,
                        real e[4])
{
    Xpost_Object psmat;
    real fm[4] = { 1.0, 0.0, 0.0, 1.0 };
    real cm[4];
    int i;

    psmat = xpost_dict_get(ctx, fontdict, name_FontMatrix);
    if (xpost_object_get_type(psmat) == arraytype && psmat.comp_.sz == 6)
    {
        for (i = 0; i < 4; i++)
        {
            Xpost_Object el = xpost_array_get(ctx, psmat, i);
            if (xpost_object_get_type(el) == realtype)
                fm[i] = el.real_.val;
            else if (xpost_object_get_type(el) == integertype)
                fm[i] = (real)el.int_.val;
        }
    }
    psmat = xpost_dict_get(ctx, gs, name_currmatrix);
    if (xpost_object_get_type(psmat) != arraytype || psmat.comp_.sz != 6)
        return 0;
    _matrix_linear_part(ctx, psmat, cm);
    e[0] = fm[0] * cm[0] + fm[1] * cm[2];
    e[1] = fm[0] * cm[1] + fm[1] * cm[3];
    e[2] = fm[2] * cm[0] + fm[3] * cm[2];
    e[3] = fm[2] * cm[1] + fm[3] * cm[3];
    return 1;
}

/* the glyph name the font's /Encoding assigns a character code, or the
   invalid object (codes past the array, or entries that are not names) */
static
Xpost_Object _encoded_name(Xpost_Context *ctx,
                           Xpost_Object encoding,
                           unsigned int ch)
{
    if (xpost_object_get_type(encoding) == arraytype
     && ch < (unsigned int)encoding.comp_.sz)
    {
        Xpost_Object en = xpost_array_get(ctx, encoding, ch);
        if (xpost_object_get_type(en) == nametype)
            return en;
    }
    return invalid;
}

/* A number a program supplies, quantized into the 16.16 fixed-point form
   the face's own transforms and advances are held in. A PostScript number
   reaches far past that form, and a value the field cannot hold has no
   fixed-point answer at all: what it would be given is one of the field's
   own values, which some other number also has. The caller is told so
   instead, and does without the fixed-point form rather than sharing one. */
static int
_fixed16(double v, long *out)
{
    /* one past the largest magnitude the field holds. The largest itself
       is not always a value a double names exactly; one past it is a power
       of two and always is. */
    const double lim = (double)LONG_MAX + 1.0;
    double f = v * 65536.0;

    if (!(f >= -lim && f < lim))
        return 0;
    *out = (long)f;
    return 1;
}

/* One element of a /Metrics array, where it is a number. A form whose
   elements are not all numbers is no override at all: the face's own
   metrics stand rather than half of the entry being taken. */
static
int _metrics_number(Xpost_Context *ctx, Xpost_Object arr, int i, real *out)
{
    Xpost_Object el = xpost_array_get(ctx, arr, i);

    if (xpost_object_get_type(el) == realtype)
        *out = el.real_.val;
    else if (xpost_object_get_type(el) == integertype)
        *out = (real)el.int_.val;
    else
        return 0;
    return 1;
}

/* Whether a /Metrics dictionary can be asked about this glyph at all:
   there has to be one, a matrix to carry its values into device space,
   and a key to look the glyph up under. The key is the glyph's name in a
   base font (PLRM 5.9.2) and its CID in a CIDFont (PLRM 5.11.3), so both
   types of key are keys here and a dictionary of one kind answers
   nothing to a key of the other. */
static
int _metrics_askable(const textstate *ts, Xpost_Object glyphkey)
{
    return ts->cdmat_ok
        && xpost_object_get_type(ts->metrics) == dicttype
        && (xpost_object_get_type(glyphkey) == nametype
         || xpost_object_get_type(glyphkey) == integertype);
}

/* A /Metrics entry for this glyph overrides its width (PLRM 5.9.2):
   a number is a new x width, a two-element array carries the width in its
   second element, a four-element array carries the width vector in its
   last two. The values are in character space; deliver the device-space
   advance in 16.16, y-up, the convention the face's advances arrive in.
   (The sidebearing the array forms also carry is _metrics_sidebearing's.) */
static
int _metrics_advance(Xpost_Context *ctx,
                     const textstate *ts,
                     Xpost_Object glyphkey,
                     long *ax,
                     long *ay)
{
    Xpost_Object v;
    real wx, wy = 0.0;

    if (!_metrics_askable(ts, glyphkey))
        return 0;
    v = xpost_dict_get(ctx, ts->metrics, glyphkey);
    if (xpost_object_get_type(v) == integertype)
        wx = (real)v.int_.val;
    else if (xpost_object_get_type(v) == realtype)
        wx = v.real_.val;
    else if (xpost_object_get_type(v) == arraytype
          && (v.comp_.sz == 2 || v.comp_.sz == 4))
    {
        if (!_metrics_number(ctx, v, v.comp_.sz == 2 ? 1 : 2, &wx))
            return 0;
        if (v.comp_.sz == 4 && !_metrics_number(ctx, v, 3, &wy))
            return 0;
    }
    else
        return 0;
    /* a width the fixed-point form does not hold is no override: the
       face's own advance stands rather than a value the form substitutes */
    if (!_fixed16(ts->cdmat[0] * wx + ts->cdmat[2] * wy, ax)
     || !_fixed16(-(ts->cdmat[1] * wx + ts->cdmat[3] * wy), ay))
        return 0;
    return 1;
}

/* A /Metrics entry for this glyph may also override where the glyph
   sits against its origin (PLRM 5.9.2): the two-element array's first
   element is the x component of a new left sidebearing, and the
   four-element array's first two are the x and y components of one. A
   number gives a width alone, and a glyph named by one keeps the
   sidebearing the face drew it with.

   The sidebearing is a position, not a displacement (PLRM 5.4), so what
   moves the glyph is the difference between the one asked for and the
   one the face has -- which is what this answers, in 16.16 device units,
   y-up, the convention the advances arrive in. Answers 0 where this
   glyph is not given a sidebearing, and the glyph then stands where the
   face puts it.

   Called before the glyph is rendered: it loads the glyph to read the
   face's own bearing, and the rendering path hands back a raster that a
   later load would replace. */
static
int _metrics_sidebearing(Xpost_Context *ctx,
                         const textstate *ts,
                         Xpost_Object glyphkey,
                         void *face,
                         unsigned int glyph_index,
                         long *dx,
                         long *dy)
{
    Xpost_Object v;
    real sx = 0.0, sy = 0.0;
    long nx, ny;

    if (!_metrics_askable(ts, glyphkey))
        return 0;
    v = xpost_dict_get(ctx, ts->metrics, glyphkey);
    if (xpost_object_get_type(v) != arraytype
     || (v.comp_.sz != 2 && v.comp_.sz != 4))
        return 0;
    if (!_metrics_number(ctx, v, 0, &sx))
        return 0;
    if (v.comp_.sz == 4 && !_metrics_number(ctx, v, 1, &sy))
        return 0;
    if (!xpost_font_face_glyph_sidebearing(face, glyph_index, &nx, &ny))
        return 0;
    /* the face's own bearing comes back already in device units, so the
       two meet there: the requested one crosses from character space
       through the same matrix the requested width does */
    if (!_fixed16(ts->cdmat[0] * sx + ts->cdmat[2] * sy - nx / 65536.0, dx)
     || !_fixed16(-(ts->cdmat[1] * sx + ts->cdmat[3] * sy) - ny / 65536.0, dy))
        return 0;
    return 1;
}

/* --- reading a Type 1 font program -----------------------------------
   The language lets a program look inside a font, so a face opened over a
   file has to hand back the charstrings it was built from -- eexec
   encrypted, so they are decrypted here rather than by the font library,
   which has no reason to expose them. */

/* Extract the CharStrings of a Type 1 font program on disk: the
   values a font dictionary built by running the program would hold,
   the charstring bytes as the RD procedure reads them, still under
   their own charstring encryption. PFB segment headers unwrap, the
   eexec layer decrypts from its hexadecimal or raw form, and the
   entries parse as /name length RD <bytes> ND, whatever pair of
   names the program chose for RD and ND. Returns a read-only
   dictionary in global VM, or the invalid object. */
static Xpost_Object
_t1_charstrings_from_file(Xpost_Context *ctx, const char *path)
{
    Xpost_Object result = null;
    unsigned char *raw = NULL, *flat = NULL, *plain = NULL;
    size_t rawlen = 0, flatlen = 0, plainlen = 0;
    size_t i, ee;
    int ferrcode = 0;
    FILE *fp;

    fp = xpost_diskfile_fopen(path, "rb", 1, &ferrcode);
    if (!fp)
        return null;
    fseek(fp, 0, SEEK_END);
    {
        long l = ftell(fp);

        if (l <= 0 || l > (16L << 20))
        {
            fclose(fp);
            return null;
        }
        rawlen = (size_t)l;
    }
    fseek(fp, 0, SEEK_SET);
    raw = malloc(rawlen);
    if (!raw || fread(raw, 1, rawlen, fp) != rawlen)
    {
        free(raw);
        fclose(fp);
        return null;
    }
    fclose(fp);

    if (raw[0] == 0x80)
    {
        /* PFB: 0x80, type, little-endian length, payload; type 3 ends */
        size_t off = 0;

        flat = malloc(rawlen);
        if (!flat)
            goto out;
        while (off + 6 <= rawlen && raw[off] == 0x80 && raw[off + 1] != 3)
        {
            size_t seg = (size_t)raw[off + 2]
                       | ((size_t)raw[off + 3] << 8)
                       | ((size_t)raw[off + 4] << 16)
                       | ((size_t)raw[off + 5] << 24);

            off += 6;
            /* keep the segment within the file, without an off+seg wrap
               where size_t is as wide as the length the four bytes
               above can state */
            if (seg > rawlen - off)
                goto out;
            memcpy(flat + flatlen, raw + off, seg);
            flatlen += seg;
            off += seg;
        }
    }
    else
    {
        flat = raw;
        flatlen = rawlen;
        raw = NULL;
    }

    /* the encrypted portion follows the eexec token's white space */
    for (ee = 0; ee + 5 < flatlen; ee++)
        if (memcmp(flat + ee, "eexec", 5) == 0)
            break;
    if (ee + 5 >= flatlen)
        goto out;
    ee += 5;
    while (ee < flatlen && (flat[ee] == '\r' || flat[ee] == '\n'
                         || flat[ee] == ' ' || flat[ee] == '\t'))
        ee++;
    {
        int ishex = 1;
        unsigned short r = 55665;
        size_t n = 0;

        for (i = 0; i < 4 && ee + i < flatlen; i++)
            if (!isxdigit(flat[ee + i]))
                ishex = 0;
        plain = malloc(flatlen);
        if (!plain)
            goto out;
        if (ishex)
        {
            int hi = -1;

            for (i = ee; i < flatlen; i++)
            {
                int c = flat[i], v;

                if (isdigit(c)) v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else continue;
                if (hi < 0)
                    hi = v;
                else
                {
                    plain[n++] = (unsigned char)((hi << 4) | v);
                    hi = -1;
                }
            }
        }
        else
        {
            memcpy(plain, flat + ee, flatlen - ee);
            n = flatlen - ee;
        }
        for (i = 0; i < n; i++)
        {
            unsigned char c = plain[i];

            plain[i] = (unsigned char)(c ^ (r >> 8));
            r = (unsigned short)((unsigned int)(c + r) * 52845u + 22719u);
        }
        if (n <= 4)
            goto out;
        memmove(plain, plain + 4, n - 4);
        plainlen = n - 4;
    }

    /* /CharStrings, then entries until the closing end */
    for (i = 0; i + 12 < plainlen; i++)
        if (memcmp(plain + i, "/CharStrings", 12) == 0)
            break;
    if (i + 12 >= plainlen)
        goto out;
    i += 12;
    {
        unsigned int oldmode = ctx->vmmode;
        Xpost_Object csdict;
        int entries = 0;

        ctx->vmmode = GLOBAL;
        csdict = xpost_dict_cons(ctx, 256);
        while (i < plainlen && entries < 20000)
        {
            char namebuf[128];
            size_t nb = 0;
            long len = 0;

            while (i < plainlen && plain[i] != '/')
            {
                if (i + 3 < plainlen && memcmp(plain + i, "end", 3) == 0
                 && (i == 0 || isspace(plain[i - 1]))
                 && (i + 3 == plainlen || isspace(plain[i + 3])))
                    goto done;
                i++;
            }
            if (i >= plainlen)
                break;
            i++;
            while (i < plainlen && !isspace(plain[i]) && plain[i] != '('
                && plain[i] != '/' && plain[i] != '{'
                && nb + 1 < sizeof namebuf)
                namebuf[nb++] = (char)plain[i++];
            namebuf[nb] = 0;
            while (i < plainlen && isspace(plain[i]))
                i++;
            while (i < plainlen && isdigit(plain[i]))
                len = len * 10 + (plain[i++] - '0');
            while (i < plainlen && isspace(plain[i]))
                i++;
            while (i < plainlen && !isspace(plain[i]))
                i++;                       /* the RD name of the day */
            i++;                           /* the single separator */
            if (len <= 0 || len > 65535 || i + (size_t)len > plainlen
             || nb == 0)
                break;
            {
                Xpost_Object str = xpost_string_cons(ctx, (unsigned int)len,
                                                     (char *)plain + i);

                str = xpost_object_set_access(ctx, str,
                          XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY);
                if (xpost_dict_put(ctx, csdict, xpost_name_cons(ctx, namebuf),
                                   str))
                    break;
            }
            i += (size_t)len;
            entries++;
        }
done:
        if (entries > 0)
        {
            Xpost_Object sealed =
                xpost_object_set_access(ctx, csdict,
                                        XPOST_OBJECT_TAG_ACCESS_READ_ONLY);

            /* The dictionary was made under whatever save level stands
               over this call, so it is not one that level has to back up
               and the seal cannot be declined the room. Taken only when
               it is a dictionary all the same: what a decline answers is
               a null, and a null is not what belongs under the name. */
            if (xpost_object_get_type(sealed) == dicttype)
                csdict = sealed;
            result = csdict;
        }
        ctx->vmmode = oldmode;
    }
out:
    free(raw);
    free(flat);
    free(plain);
    return result;
}

/* --- reading a compact font format program ---------------------------
   The same question of a CFF container: where the charstrings are, and how
   to hand them back. The index structure is read directly, since what is
   wanted is the bytes rather than the outlines. */

/* CFF INDEX and DICT walking, enough to reach the CharStrings INDEX
   of a bare CFF or an OpenType CFF table: the glyph names come from
   the face, so neither the charset nor the string index is read. */
static unsigned long
_cff_u(const unsigned char *p, int n)
{
    unsigned long v = 0;
    int i;

    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

/* an INDEX at off: sets *count and *first (offset of the offset
   array's data area); returns the offset just past the INDEX, or 0 */
static size_t
_cff_index(const unsigned char *d, size_t len, size_t off,
           unsigned long *count, size_t *dataoff, int *offsz)
{
    unsigned long c;
    int osz;
    unsigned long last;

    if (off + 2 > len)
        return 0;
    c = _cff_u(d + off, 2);
    if (c == 0)
    {
        *count = 0;
        return off + 2;
    }
    if (off + 3 > len)
        return 0;
    osz = d[off + 2];
    if (osz < 1 || osz > 4)
        return 0;
    if (off + 3 + (c + 1) * osz > len)
        return 0;
    last = _cff_u(d + off + 3 + c * osz, osz);
    *count = c;
    *offsz = osz;
    *dataoff = off + 3 + (c + 1) * osz - 1;
    /* keep the data area within the buffer, without a *dataoff+last wrap
       where the size type is as wide as the four bytes last was read
       from: the check above holds *dataoff inside len, so the length is
       weighed against what is left */
    if (last > len - *dataoff)
        return 0;
    return *dataoff + last;
}

/* the CharStrings offset out of the first Top DICT (operator 17) */
static unsigned long
_cff_charstrings_offset(const unsigned char *d, size_t len)
{
    unsigned long count;
    size_t dataoff = 0, off;
    int osz = 0;
    size_t dstart, dend;
    double operands[48];
    int nops = 0;

    if (len < 4)
        return 0;
    off = d[2];                          /* header size */
    off = _cff_index(d, len, off, &count, &dataoff, &osz);   /* Name */
    if (!off)
        return 0;
    if (_cff_index(d, len, off, &count, &dataoff, &osz) == 0 || count == 0)
        return 0;                                            /* Top DICT */
    dstart = dataoff + _cff_u(d + off + 3, osz);
    dend = dataoff + _cff_u(d + off + 3 + osz, osz);
    while (dstart < dend && dstart < len)
    {
        int b = d[dstart];

        if (b <= 21)
        {
            int op = b;

            dstart++;
            if (b == 12)
            {
                if (dstart >= len)
                    return 0;
                op = 1200 + d[dstart];
                dstart++;
            }
            if (op == 17 && nops >= 1)
                return (unsigned long)operands[nops - 1];
            nops = 0;
        }
        else if (b >= 32 && b <= 246)
        {
            if (nops < 48) operands[nops++] = b - 139;
            dstart++;
        }
        else if (b >= 247 && b <= 250)
        {
            if (dstart + 1 >= len)   /* the operand's trailing byte */
                return 0;
            if (nops < 48) operands[nops++] =
                (b - 247) * 256 + d[dstart + 1] + 108;
            dstart += 2;
        }
        else if (b >= 251 && b <= 254)
        {
            if (dstart + 1 >= len)
                return 0;
            if (nops < 48) operands[nops++] =
                -((int)(b - 251) * 256) - (int)d[dstart + 1] - 108;
            dstart += 2;
        }
        else if (b == 28)
        {
            if (dstart + 2 >= len)   /* two trailing operand bytes */
                return 0;
            if (nops < 48) operands[nops++] =
                (short)_cff_u(d + dstart + 1, 2);
            dstart += 3;
        }
        else if (b == 29)
        {
            if (dstart + 4 >= len)   /* four trailing operand bytes */
                return 0;
            if (nops < 48) operands[nops++] =
                (long)_cff_u(d + dstart + 1, 4);
            dstart += 5;
        }
        else if (b == 30)
        {
            /* a real number: nibbles to the stop code */
            dstart++;
            while (dstart < len)
            {
                int lo = d[dstart] & 15, hi = d[dstart] >> 4;

                dstart++;
                if (hi == 15 || lo == 15)
                    break;
            }
            if (nops < 48) operands[nops++] = 0;
        }
        else
            return 0;
    }
    return 0;
}

/* Publish the CharStrings of a CFF-backed face: the Type 2
   charstring of every glyph, keyed by the face's glyph names. A bare
   CFF file reads whole; an OpenType wrapper locates its CFF table.
   Returns a read-only dictionary in global VM, or null. */
static Xpost_Object
_cff_charstrings_from_file(Xpost_Context *ctx, const char *path, void *face)
{
    Xpost_Object result = null;
    unsigned char *raw = NULL;
    const unsigned char *cff;
    size_t rawlen = 0, cfflen;
    unsigned long csoff, count;
    size_t dataoff = 0;
    int osz = 0;
    int ferrcode = 0;
    FILE *fp;

    fp = xpost_diskfile_fopen(path, "rb", 1, &ferrcode);
    if (!fp)
        return null;
    fseek(fp, 0, SEEK_END);
    {
        long l = ftell(fp);

        if (l <= 0 || l > (32L << 20))
        {
            fclose(fp);
            return null;
        }
        rawlen = (size_t)l;
    }
    fseek(fp, 0, SEEK_SET);
    raw = malloc(rawlen);
    if (!raw || fread(raw, 1, rawlen, fp) != rawlen)
    {
        free(raw);
        fclose(fp);
        return null;
    }
    fclose(fp);

    cff = raw;
    cfflen = rawlen;
    if (rawlen > 12 && memcmp(raw, "OTTO", 4) == 0)
    {
        unsigned long ntab = _cff_u(raw + 4, 2), t;

        cff = NULL;
        for (t = 0; t < ntab && 12 + t * 16 + 16 <= rawlen; t++)
        {
            const unsigned char *e = raw + 12 + t * 16;

            if (memcmp(e, "CFF ", 4) == 0)
            {
                unsigned long toff = _cff_u(e + 8, 4);
                unsigned long tlen = _cff_u(e + 12, 4);

                /* keep the table within the file, without a 32-bit
                   toff+tlen wrap */
                if (toff <= rawlen && tlen <= rawlen - toff)
                {
                    cff = raw + toff;
                    cfflen = tlen;
                }
                break;
            }
        }
        if (!cff)
            goto out;
    }
    else if (raw[0] != 1)     /* not a bare CFF either */
        goto out;

    csoff = _cff_charstrings_offset(cff, cfflen);
    if (!csoff || _cff_index(cff, cfflen, csoff, &count, &dataoff, &osz) == 0
     || count == 0)
        goto out;

    {
        unsigned int oldmode = ctx->vmmode;
        Xpost_Object csdict;
        unsigned long gid;
        int entries = 0;

        ctx->vmmode = GLOBAL;
        csdict = xpost_dict_cons(ctx, count < 4096 ? (unsigned int)count : 4096);
        for (gid = 0; gid < count && gid < 65535; gid++)
        {
            char nbuf[128];
            unsigned long a = _cff_u(cff + csoff + 3 + gid * osz, osz);
            unsigned long b = _cff_u(cff + csoff + 3 + (gid + 1) * osz, osz);

            /* keep the glyph's data span within the font buffer: interior
               INDEX offsets are otherwise unchecked, so an oversized or
               non-monotonic offset must not address memory outside it */
            if (b <= a || b - a > 65535 || b > cfflen - dataoff)
                continue;
            if (!xpost_font_face_glyph_name_get(face, (unsigned int)gid,
                                                nbuf, sizeof nbuf))
                continue;
            {
                Xpost_Object str = xpost_string_cons(ctx,
                    (unsigned int)(b - a), (char *)cff + dataoff + a);

                str = xpost_object_set_access(ctx, str,
                          XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY);
                if (xpost_dict_put(ctx, csdict, xpost_name_cons(ctx, nbuf), str))
                    break;
                entries++;
            }
        }
        if (entries > 0)
        {
            Xpost_Object sealed =
                xpost_object_set_access(ctx, csdict,
                                        XPOST_OBJECT_TAG_ACCESS_READ_ONLY);

            /* as above: the seal cannot be declined here, and a null is
               not what belongs under the name if it were */
            if (xpost_object_get_type(sealed) == dicttype)
                csdict = sealed;
            result = csdict;
        }
        ctx->vmmode = oldmode;
    }
out:
    free(raw);
    return result;
}

/* --- the face cache --------------------------------------------------
   Opening a face costs the file and everything derived from it, so faces
   are kept per font name. The cache is split in two because its halves
   have different lifetimes: what the font library holds is a C structure,
   and the objects a font dictionary shares -- CharStrings, sfnts -- are in
   global virtual memory under the private namespace. */

/* A key interned in the bank the cache lives in.

   A name is interned into whichever bank the allocation mode selects, so
   the same text asked for under different modes is two different name
   objects and answers as two different keys.

   What arrives here is a face name, interned when that name is first
   asked for. The machinery's own spellings are not built here: those
   are resolved once as the operators are installed (_op_font_names), so
   asking for a face the cache already holds interns nothing -- whichever
   branch first cached the face's derived objects. */
static
Xpost_Object _face_key(Xpost_Context *ctx, const char *text)
{
    unsigned int vmmode = ctx->vmmode;
    Xpost_Object key;

    ctx->vmmode = GLOBAL;
    key = xpost_name_cons(ctx, text);
    ctx->vmmode = vmmode;
    return key;
}

/* The object half of the face cache, kept where the collector reaches it.

   A face is cached so that every font dictionary one name produces
   shares a synthesized CharStrings dictionary and an sfnts array rather
   than rebuilding them. The cache is a C structure, which suits the half
   of an entry that is a host resource -- the face the font library
   opened, the file it came from -- and does not suit the half that is an
   object: nothing the collector walks would name it, so a collection
   would reclaim it and the cache would then hand the recycled entity to
   the next font of that face.

   The objects therefore live in a dictionary under the private global
   namespace, one subordinate dictionary per face so that a face's
   entries stay together and no face name becomes a name in the namespace
   itself. Being members of a rooted dictionary they are reachable for
   exactly as long as the cache can produce them, with no registration to
   remember. */
static
Xpost_Object _face_entry(Xpost_Context *ctx, const char *facename, int create)
{
    Xpost_Object faces;
    Xpost_Object ent;
    unsigned int vmmode;

    if (xpost_object_get_type(ctx->globalprivatedict) != dicttype)
        return null;

    faces = xpost_context_job_member(ctx, ".facecache");
    if (xpost_object_get_type(faces) != dicttype)
        return null;

    ent = xpost_dict_get(ctx, faces, _face_key(ctx, facename));
    if (xpost_object_get_type(ent) == dicttype || !create)
        return ent;

    vmmode = ctx->vmmode;
    ctx->vmmode = GLOBAL;
    ent = xpost_dict_cons(ctx, 4);
    ctx->vmmode = vmmode;
    if (xpost_object_get_type(ent) != dicttype)
        return null;
    if (xpost_dict_put(ctx, faces, _face_key(ctx, facename), ent) != 0)
        return null;
    return ent;
}

/* The objects derived from a face, kept against the face's own name so
   a second font over the same face does not build them again. They are
   put where the collector reaches them, since what is derived are
   ordinary composite objects. */
static
void _face_put(Xpost_Context *ctx, const char *facename,
               Xpost_Object what, Xpost_Object o)
{
    Xpost_Object ent = _face_entry(ctx, facename, 1);

    if (xpost_object_get_type(ent) != dicttype)
        return;
    if (xpost_dict_put(ctx, ent, what, o) != 0)
        XPOST_LOG_ERR("cannot keep a cached face's derived object where"
                      " the collector reaches it");
}

static
Xpost_Object _face_get(Xpost_Context *ctx, const char *facename,
                       Xpost_Object what)
{
    Xpost_Object ent = _face_entry(ctx, facename, 0);

    if (xpost_object_get_type(ent) != dicttype)
        return null;
    return xpost_dict_get(ctx, ent, what);
}

/* Give up the objects held against a name. Called where the name's
   entry in the C half is given up, so the two halves stop answering for
   a name together: what stays behind would otherwise hold the name's
   derived objects for the life of the run, one name at a time, however
   many names the run asks for. */
static
void _face_drop(Xpost_Context *ctx, const char *facename)
{
    Xpost_Object faces;

    if (xpost_object_get_type(ctx->globalprivatedict) != dicttype)
        return;
    faces = xpost_context_job_member(ctx, ".facecache");
    if (xpost_object_get_type(faces) != dicttype)
        return;
    (void)xpost_dict_undef(ctx, faces, _face_key(ctx, facename));
}

/* Faces held against the names they were asked for. A face maps the
   font file and holds library state, so one per findfont grows the
   process by a mapping a lookup; the entry is what keeps that from
   being repeated. The reference an entry holds is the cache's own:
   every font dictionary a name produces takes a claim of its own on
   the face (xpost_font_face_reference), so an entry can be given up
   while such a dictionary is still in use.

   The cache is full when every slot is taken, and a name arriving then
   takes the place of the one asked for longest ago: used orders the
   entries by when each was last asked for, so what stays is the names
   the run is asking for now rather than the first ones it ever saw. A
   wrap of the clock costs a poor choice of victim, never a wrong
   face. */
static struct { char *name; void *face; char *file; unsigned long used;
                int substitute; }
    face_cache[32];
static int face_cache_n = 0;
static unsigned long face_cache_clock = 0;

/* --- what the text path needs to know about the clip -----------------
   Glyphs are painted through a clip like any other mark, and asking the
   clip machinery per glyph is what a page of text cannot afford. So the
   clip is resolved once into the rows it admits, memoised against the
   clip's own identity, and each glyph asks a row rather than a path. */

/* Give the held faces back. The library the faces belong to goes down
   with the module (xpost_font_quit), and a cache still naming them
   afterwards would hand a later run of the same process a face that had
   been freed -- which is what an embedder doing a second init and asking
   for a font it had asked for before would get. Called from the same
   teardown, so the cache stops naming a face before the face goes. */

/* -  .newfontserial  int
   The serial a font's cached glyph masks are keyed by. The counter only
   ever moves forward, so no two fonts of a run are named alike and a
   mask cached against a serial cannot be taken for a later font that
   happened to be given the same number.

   A counter kept in the graphics dictionary could not promise that. It
   is virtual memory, so a restore past the definefont that raised it
   winds it back and the number is handed out a second time -- while the
   masks it keys are not virtual memory and are not wound back with it.
   The two would then disagree about which font a mask belongs to, and
   the disagreement is only visible as the wrong shape on the page. */
static int _fontserial_next = 1;

/* The next number, and the restart when the range runs out. One counter
   mints both of the identities a font is given here -- the serial its
   cached glyph masks are filed under, and the FID its dictionary is
   stamped with -- so the restart that stops a mask being answered to a
   later font is the same restart that stops two dictionaries carrying
   one FID. */
static int _fontserial(void)
{
    int n;

    if (_fontserial_next <= 0)
    {
        /* the counter has run its range: nothing held can be told apart
           from what the reissued numbers will name, so nothing is held */
        xpost_mask_cache_clear();
        _fontserial_next = 1;
    }
    n = _fontserial_next;
    _fontserial_next++;
    return n;
}

static int _newfontserial(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(_fontserial())))
        return stackoverflow;
    return 0;
}

/* -  .newfontid  fontID
   The object PLRM Table 5.2 has definefont insert under FID. It is of a
   type nothing else in the language builds, so a program can tell one
   font dictionary's FID from another's and cannot write one of its own;
   its value is an identity and is read by nothing. */
static int _newfontid(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_fontid_cons((dword)_fontserial())))
        return stackoverflow;
    return 0;
}

/* Whether a dictionary is one of the two font directories.

   The context holds both, so the question is asked of the interpreter's
   own record rather than of a name a program could rebind. */
static int _isfontdir(Xpost_Context *ctx, Xpost_Object d)
{
    Xpost_Object dir[2];
    int i;

    dir[0] = ctx->localfontdir;
    dir[1] = ctx->globalfontdir;
    for (i = 0; i < 2; i++)
    {
        if (xpost_object_get_type(dir[i]) != dicttype)
            continue;
        if (xpost_object_get_ent(dir[i]) == xpost_object_get_ent(d)
            && ((dir[i].tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)
                == (d.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)))
            return 1;
    }
    return 0;
}

/* dir key font  .fontdirdef  -
   File a font in a font directory under a key.

   FontDirectory and GlobalFontDirectory are read-only to a program
   (PLRM Tables 3.3 and 3.4): they are updated by definefont and consulted
   by findfont, and nothing else may add to them. A dictionary carries its
   access on its value and not on the reference (PLRM 3.3.2), so there is
   no writable reference for definefont to keep and the write has to pass
   the attribute rather than go round it. This is that write, and the only
   one: everything a program can reach refuses.

   Only the two directories the context holds are written; any other
   dictionary is refused, so this is a way to file a font and not a way to
   write a dictionary of a caller's choosing.

   The rule that a global dictionary may not name a local object stands
   (PLRM 3.7.2): a font built in local VM is refused by the global
   directory here exactly as put refuses it. */
static
int _fontdirdef(Xpost_Context *ctx,
                Xpost_Object D,
                Xpost_Object K,
                Xpost_Object V)
{
    if (!_isfontdir(ctx, D))
        return invalidaccess;
    return xpost_dict_put_internal(ctx, D, K, V);
}

/* dir key  .fontdirundef  -
   Take a font out of a font directory, past the same attribute and under
   the same restriction as .fontdirdef. This is the write undefinefont
   makes. A key the directory does not hold is no error, as undef is not
   (PLRM 8.2). */
static
int _fontdirundef(Xpost_Context *ctx,
                  Xpost_Object D,
                  Xpost_Object K)
{
    int ret;

    if (!_isfontdir(ctx, D))
        return invalidaccess;
    ret = xpost_dict_undef(ctx, D, K);
    if (ret == undefined)
        return 0;
    return ret;
}

/* Gives up every face this module opened, and the names and files they
   were opened from. */
static void xpost_op_font_quit(void)
{
    int i;

    for (i = 0; i < face_cache_n; i++)
    {
        xpost_font_face_free(face_cache[i].face);
        free(face_cache[i].name);
        free(face_cache[i].file);
    }
    memset(face_cache, 0, sizeof(face_cache));
    face_cache_n = 0;
    face_cache_clock = 0;
    xpost_op_font_clip_memo_drop();
}

/* --- finding a font, and installing one ------------------------------
   findfont resolves a name to a face, through the font directory and, for
   a host face, through the substitution the platform offers. What comes
   back is a font dictionary; setfont puts it in the graphics state. */

/* -  .fonthostfaces  int
   int  .fonthostface  string|null

   The host's faces as the configuration holds them, so that the Font
   category can enumerate what a program could ask for. Two operators
   rather than one array: the set runs to a thousand faces on an ordinary
   machine, and a caller wanting only the names a template matches should
   not have to be handed every one of them in virtual memory first. */
static
int _fonthostfaces(Xpost_Context *ctx)
{
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_int_cons(xpost_font_host_face_count()));
    return 0;
}

static
int _fonthostface(Xpost_Context *ctx,
                  Xpost_Object index)
{
    const char *nm = xpost_font_host_face_name(index.int_.val);
    Xpost_Object s;

    if (!nm)
    {
        xpost_stack_push(ctx->lo, ctx->os, null);
        return 0;
    }
    s = xpost_string_cons(ctx, (unsigned int)strlen(nm),
                          (/*@temp@*/ char *)nm);
    if (xpost_object_get_type(s) == invalidtype)
        return VMerror;
    /* the flag is LIT and not EXE, so an object built here is executable
       until it is said to be literal -- and a name a caller makes of an
       executable string runs where it meant to look it up */
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(s));
    return 0;
}

/* A font name as C text, or the empty name where the name is not text.

   A face is found by handing the name to the host's font configuration,
   which reads it as C text. C text ends at the first nul, so a name
   carrying one would be resolved as far as that nul and answer with the
   face of the shorter name it begins with -- a font the program did not
   name. A name counts its characters (PLRM 3.3) and a nul is one of
   them, so such a name is not the name it begins with and the
   configuration holds no face under it, exactly as it holds none under a
   name nothing is installed for. The empty name is resolved instead: no
   face carries that either, so what answers is the substitute a name
   with no face of its own gets.

   Returns the allocated text, or NULL where there was no room for it. */
static char *
_font_cname(Xpost_Context *ctx, Xpost_Object fontstr)
{
    if (!xpost_string_is_cstring(ctx, fontstr))
        return calloc(1, 1);
    return xpost_string_allocate_cstring(ctx, fontstr);
}

/* name|string  .fontnameavailable  bool

   Whether the host carries a face of this name, as against a default
   standing in for one it has not got. The resource operators answer with
   it: /Font resourcestatus reports an instance that can be obtained but
   is not yet in VM, and a substituted face is not that instance. Opens
   nothing -- the configuration is asked and the answer given back. */
static
int _fontnameavailable(Xpost_Context *ctx,
                       Xpost_Object fontname)
{
    Xpost_Object fontstr;
    char *fname;
    int avail;

    if (xpost_object_get_type(fontname) == nametype)
        fontstr = xpost_name_get_string(ctx, fontname);
    else
        fontstr = fontname;
    /* the characters of the name are read, and a string names a font
       only where its access permits being read (PLRM 3.3.2) */
    if (!xpost_object_is_readable(ctx, fontstr))
        return invalidaccess;
    fname = _font_cname(ctx, fontstr);
    if (!fname)
        return VMerror;
    avail = xpost_font_name_is_available(fname);
    free(fname);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(avail));
    return 0;
}

static
int _findfont(Xpost_Context *ctx,
              Xpost_Object fontname)
{
    Xpost_Object fontstr;
    Xpost_Object fontdict;
    struct fontdata data;
    char *fname;
    Xpost_Object sfnts_obj = null;
    const char *ffile = NULL;
    int istt = 0;
    int cffreal = 0;
    int uncached = 0;
    int referenced = 0;
    int substitute = 0;
    int ret;

    data.face = NULL;
    if (xpost_object_get_type(fontname) == nametype)
        fontstr = xpost_name_get_string(ctx, fontname);
    else
        fontstr = fontname;
    /* the characters of the name are read, and a string names a font
       only where its access permits being read (PLRM 3.3.2) */
    if (!xpost_object_is_readable(ctx, fontstr))
        return invalidaccess;
    /* The host's configuration is asked with the characters as C text,
       which ends at a nul the name may carry. A name carrying one is
       therefore resolved as the shorter name it begins with, and answers
       with that name's face. The configuration cannot be asked anything
       else usefully: handed the empty name it may decline to answer at
       all rather than substitute, and handed the characters with the nul
       disguised it matches the same face back by resemblance. What a
       program can still tell is that the name is not available --
       .fontnameavailable and /Font resourcestatus both answer for the
       whole of it (COMPLIANCE: font names). */
    fname = xpost_string_allocate_cstring(ctx, fontstr);

    fontdict = xpost_dict_cons (ctx, 10);
    ret = xpost_dict_put(ctx, fontdict, name_FontName, fontname);
    if (ret)
    {
        free(fname);
        return ret;
    }

    /* Every font dictionary carries an FID (PLRM Table 5.2), and this is
       a font dictionary the interpreter builds rather than one a program
       hands to definefont, so it is stamped here: a name resolved through
       the host answers with a font as complete as a defined one. Each
       lookup builds its own dictionary and each gets its own identity --
       what the entry says is which dictionary this is, and two of these
       are two dictionaries even where one face answers both. */
    ret = xpost_dict_put(ctx, fontdict, name_FID,
                         xpost_fontid_cons((dword)_fontserial()));
    if (ret)
    {
        free(fname);
        return ret;
    }

    /* initialize font data, with x-scale and y-scale set to 1.
       Faces are cached per name: each face maps the font file and
       holds FreeType state, so creating one per findfont grows the
       process by a mapping per lookup. The face is shared between
       font dictionaries exactly as a FontDirectory-cached dictionary
       already shares it.

       The key is the name that was asked for, not the name of the face
       that answered. It has to be: the lookup happens before any face
       is open, so the name asked for is the only thing there is to key
       on, and it is also what the run asks again with. Two names that
       resolve to one face therefore hold an entry each, which costs
       the derived objects built twice and cannot produce a wrong font
       -- everything below reads the face, not the entry.

       The cache holds a fixed number of names and the run may ask for
       more: a name arriving at a full cache takes the place of the one
       asked for longest ago. What being out of the cache costs a name
       is a face of its own and the derived objects built again when it
       is next asked for; what it does not cost is a different font.
       Everything below reads the name and the file the face was opened
       from rather than the cache entry, so a name states the same
       FontType, the same CharStrings and the same sfnts however many
       times it has come and gone. The entry is what keeps that work
       from being repeated -- not what decides its outcome. */
    {
        Xpost_Object cs_cached;
        int fi, slot = -1;
        for (fi = 0; fi < face_cache_n; fi++)
        {
            if (strcmp(face_cache[fi].name, fname) == 0)
            {
                data.face = face_cache[fi].face;
                ffile = face_cache[fi].file;
                substitute = face_cache[fi].substitute;
                face_cache[fi].used = ++face_cache_clock;
                slot = fi;
                break;
            }
        }
        if (data.face == NULL)
        {
            data.face = xpost_font_face_new_from_name(fname);
            /* the file the face was just opened from, and whether the
               name reached the face it asked for: the library answers
               for the last open, so both are read here and not after
               anything else may have opened a face */
            ffile = xpost_font_face_last_file();
            substitute = xpost_font_face_last_is_substitute();
            if (data.face != NULL)
            {
                int at = face_cache_n;

                /* the cache is about to hold a face, which belongs to a
                   library that goes down at the teardown: asking to be
                   called is part of taking it */
                (void)xpost_at_quit(xpost_op_font_quit);
                if (at == (int)(sizeof face_cache / sizeof face_cache[0]))
                {
                    /* every slot is taken: the name asked for longest
                       ago gives up its place, so a run asking for more
                       names than the cache holds keeps the ones it is
                       asking for now. The face given up here is the
                       cache's own reference; a font dictionary still
                       using that face holds a claim of its own. */
                    int vi;

                    at = 0;
                    for (vi = 1;
                         vi < (int)(sizeof face_cache / sizeof face_cache[0]);
                         vi++)
                        if (face_cache[vi].used < face_cache[at].used)
                            at = vi;
                    _face_drop(ctx, face_cache[at].name);
                    xpost_font_face_free(face_cache[at].face);
                    free(face_cache[at].name);
                    free(face_cache[at].file);
                }
                else
                    face_cache_n++;
                face_cache[at].file = ffile ? strdup(ffile) : NULL;
                face_cache[at].name = strdup(fname);
                face_cache[at].face = data.face;
                face_cache[at].substitute = substitute;
                face_cache[at].used = ++face_cache_clock;
                slot = at;
                ffile = face_cache[slot].file;
            }
        }
        uncached = slot < 0;
        if (data.face == NULL){
            free(fname);
            return invalidfont;
        }

        /* What the dictionary is a font of. PLRM Table 5.7 has
           /FontName state "the name of this font", and the name asked
           for is not always the name of what was found: the platform's
           configuration answers nearly every name with some face, so a
           name nothing supplies produces a font all the same -- which
           is what PLRM 8.2 requires of findfont, and what would leave a
           program unable to tell the font it asked for from the one it
           was handed. Where the face carries the name it was asked for,
           that name is its own and stands; where it does not, the
           dictionary states the name the face carries.

           The name is stamped here rather than at the construction
           above, that being before there is a face to read it from.
           It is interned in global virtual memory, as the glyph names
           below are and for the same reason: a font found under one
           allocation mode and read under the other names itself with
           the same object.

           A substitution is also the build's own decision, so it says
           so on the channel it says such things on. It goes below the
           level a run prints by default: findfont produced a font,
           which is what the specification asks of it, so nothing has
           gone wrong -- and a page setting text in a dozen substituted
           names would otherwise say so a dozen times on a run that
           asked for none of it. */
        if (substitute)
        {
            char namebuf[256];

            if (xpost_font_face_name_get(data.face, namebuf, sizeof namebuf))
            {
                unsigned int oldmode = ctx->vmmode;
                Xpost_Object facename;

                XPOST_LOG_WARN("Font %s not found, using %s", fname, namebuf);
                ctx->vmmode = GLOBAL;
                facename = xpost_object_cvlit(xpost_name_cons(ctx, namebuf));
                ctx->vmmode = oldmode;
                if (xpost_object_get_type(facename) == invalidtype)
                {
                    ret = VMerror;
                    goto fail;
                }
                ret = xpost_dict_put(ctx, fontdict, name_FontName, facename);
                if (ret)
                    goto fail;
            }
        }

        /* a base font publishes its glyph complement: programs size
           tables from /CharStrings, test membership with known, and
           re-encode from its keys. Synthesize the name-to-glyph-index
           dictionary from the face's glyph names, once per face and
           shared read-only between every dictionary the name produces,
           in global VM so a restore cannot unwind it from under the
           cache. The values are glyph indices, which the text
           machinery accepts directly. A face carrying no glyph names is
           enumerated by the standard names its character map answers
           instead, which is what a TrueType program's dictionary holds;
           a face that is neither publishes nothing. */
        istt = xpost_font_face_is_truetype(data.face);
        /* a TrueType-backed dictionary is a Type 42 font outright:
           publish the program as sfnts, chunked under the string
           limit, read once per cached face and shared between every
           dictionary the name produces. Only a plain sfnt file
           qualifies: a compressed wrapper or a collection is not
           the program a Type 42 dictionary carries, and such a
           face keeps the Type 1 presentation */
        if (istt)
        {
            if (slot >= 0)
                sfnts_obj = _face_get(ctx, fname, name_sfnts);
            if (xpost_object_get_type(sfnts_obj) != arraytype && ffile)
            {
                int ferrcode = 0;
                FILE *fp = xpost_diskfile_fopen(ffile, "rb", 1, &ferrcode);

                if (fp)
                {
                    long len;
                    unsigned char magic[4] = { 0, 0, 0, 0 };
                    int plain;

                    plain = fread(magic, 1, 4, fp) == 4
                         && ((magic[0] == 0 && magic[1] == 1
                           && magic[2] == 0 && magic[3] == 0)
                          || memcmp(magic, "true", 4) == 0);
                    fseek(fp, 0, SEEK_END);
                    len = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    if (plain && len > 0)
                    {
                        int nchunks = (int)((len + 65531) / 65532);
                        unsigned int oldmode = ctx->vmmode;
                        Xpost_Object arr;
                        int ci;
                        unsigned char *cbuf = malloc(65532);

                        ctx->vmmode = GLOBAL;
                        arr = xpost_array_cons(ctx, nchunks);
                        for (ci = 0; ci < nchunks && cbuf; ci++)
                        {
                            long rem = len - (long)ci * 65532;
                            size_t want = rem > 65532 ? 65532 : (size_t)rem;
                            Xpost_Object str;

                            if (fread(cbuf, 1, want, fp) != want)
                                break;
                            str = xpost_string_cons(ctx, (unsigned int)want,
                                                    (char *)cbuf);
                            if (xpost_array_put(ctx, arr, ci, str) != 0)
                                break;
                        }
                        free(cbuf);
                        if (cbuf && ci == nchunks)
                        {
                            sfnts_obj = arr;
                            if (slot >= 0)
                                _face_put(ctx, fname, name_sfnts, arr);
                        }
                        ctx->vmmode = oldmode;
                    }
                    fclose(fp);
                }
            }
            if (xpost_object_get_type(sfnts_obj) != arraytype)
                istt = 0;
        }

        cs_cached = slot >= 0 ? _face_get(ctx, fname, name_CharStrings) : null;
        if (xpost_object_get_type(cs_cached) == dicttype)
        {
            /* whether the held dictionary is the program's own
               charstrings is a fact about that dictionary, so it is
               held beside it rather than in the entry: the entry may
               be given up and the name asked for again while the
               objects are still held here */
            Xpost_Object csr = _face_get(ctx, fname, name_csreal);

            if (xpost_object_get_type(csr) == booleantype && csr.int_.val
                && xpost_font_face_is_cff(data.face))
                cffreal = 1;
            ret = xpost_dict_put(ctx, fontdict, name_CharStrings,
                                 cs_cached);
            if (ret)
                goto fail;
        }
        else
        {
            unsigned int nglyphs;

            /* a Type 1 program on disk yields the genuine article:
               the charstring bytes its RD procedure reads, so the
               dictionary holds what running the program would build;
               a CFF face likewise publishes its Type 2 charstrings,
               and the dictionary then states FontType 2 */
            if (ffile && xpost_font_face_is_type1(data.face))
            {
                Xpost_Object cs = _t1_charstrings_from_file(ctx, ffile);

                if (xpost_object_get_type(cs) == dicttype)
                {
                    if (slot >= 0)
                    {
                        _face_put(ctx, fname, name_CharStrings, cs);
                        _face_put(ctx, fname, name_csreal, xpost_bool_cons(1));
                    }
                    ret = xpost_dict_put(ctx, fontdict,
                                       name_CharStrings, cs);
                    if (ret)
                        goto fail;
                    goto have_charstrings;
                }
            }
            if (ffile && xpost_font_face_is_cff(data.face))
            {
                Xpost_Object cs =
                    _cff_charstrings_from_file(ctx, ffile, data.face);

                if (xpost_object_get_type(cs) == dicttype)
                {
                    if (slot >= 0)
                    {
                        _face_put(ctx, fname, name_CharStrings, cs);
                        _face_put(ctx, fname, name_csreal, xpost_bool_cons(1));
                    }
                    cffreal = 1;
                    ret = xpost_dict_put(ctx, fontdict,
                                       name_CharStrings, cs);
                    if (ret)
                        goto fail;
                    goto have_charstrings;
                }
            }
            nglyphs = xpost_font_face_glyph_name_count(data.face);
            if (nglyphs)
            {
                Xpost_Object csdict;
                char nbuf[128];
                unsigned int gi;
                const char *snm;
                unsigned int sgi;
                unsigned int i;
                unsigned int nstd = 0;
                unsigned int oldmode = ctx->vmmode;

                for (i = 0; xpost_font_face_std_name_at(data.face, i,
                                                        &snm, &sgi); i++)
                    if (sgi)
                        nstd++;

                ctx->vmmode = GLOBAL;
                csdict = xpost_dict_cons(ctx, nglyphs + nstd);
                for (gi = 0; gi < nglyphs; gi++)
                {
                    if (!xpost_font_face_glyph_name_get(data.face, gi, nbuf,
                                                        sizeof nbuf))
                        continue;
                    ret = xpost_dict_put(ctx, csdict,
                                         xpost_name_cons(ctx, nbuf),
                                         xpost_int_cons((integer)gi));
                    if (ret)
                    {
                        ctx->vmmode = oldmode;
                        goto fail;
                    }
                }
                /* A file names its glyphs as it pleases, and a standard
                   name it happens not to use is still a name this face
                   answers: the character map reaches the glyph, so a
                   lookup by that name finds it whatever the dictionary
                   says. The ligatures are the ones that show -- a file
                   naming its fi glyph after the character rather than
                   after the ligature is ordinary -- and a dictionary
                   without them would be missing names the same font
                   paints, which is the way round that tells a program
                   nothing is there when something is. The file's own
                   naming stands where it has one; this only adds what
                   it left unnamed. */
                for (i = 0; xpost_font_face_std_name_at(data.face, i,
                                                        &snm, &sgi); i++)
                {
                    Xpost_Object sname;

                    if (!sgi)
                        continue;
                    sname = xpost_name_cons(ctx, snm);
                    if (xpost_object_get_type(xpost_dict_get(ctx, csdict,
                                                             sname))
                        != invalidtype)
                        continue;
                    ret = xpost_dict_put(ctx, csdict, sname,
                                         xpost_int_cons((integer)sgi));
                    if (ret)
                    {
                        ctx->vmmode = oldmode;
                        goto fail;
                    }
                }
                ctx->vmmode = oldmode;
                {
                    Xpost_Object sealed =
                        xpost_object_set_access(ctx, csdict,
                                                XPOST_OBJECT_TAG_ACCESS_READ_ONLY);

                    /* as elsewhere: the dictionary was made under the
                       save level standing over this call, so the seal
                       cannot be declined the room, and a null is not
                       what belongs under the name if it were */
                    if (xpost_object_get_type(sealed) == dicttype)
                        csdict = sealed;
                }
                if (slot >= 0)
                    _face_put(ctx, fname, name_CharStrings, csdict);
                ret = xpost_dict_put(ctx, fontdict, name_CharStrings,
                                   csdict);
                if (ret)
                    goto fail;
            }
            else if (istt)
            {
                /* A TrueType program whose file names no glyph still
                   makes a Type 42 dictionary, and PLRM Table 5.7 states
                   CharStrings among the entries such a dictionary has,
                   with an entry whose key is .notdef. The names the
                   face can be asked for are then the standard ones,
                   each reaching its glyph through the character map --
                   the same resolution a name goes through when it is
                   looked up one at a time -- so those are what the
                   dictionary holds, and a name the character map does
                   not answer is left out of it rather than published
                   against a glyph that is not there.

                   The glyph complement is what programs read this
                   dictionary for: they size tables from it, ask it
                   whether a name is known, and build an encoding out of
                   its keys. Publishing the names the face cannot reach
                   would answer all three wrongly. */
                Xpost_Object csdict;
                const char *snm;
                unsigned int gi;
                unsigned int i;
                unsigned int nnames = 1; /* .notdef */
                unsigned int oldmode;
                int symbolic = xpost_font_face_is_symbol_encoded(data.face);

                for (i = 0; xpost_font_face_std_name_at(data.face, i,
                                                        &snm, &gi); i++)
                    if (gi)
                        nnames++;
                if (symbolic)
                    nnames += 256;

                oldmode = ctx->vmmode;
                ctx->vmmode = GLOBAL;
                csdict = xpost_dict_cons(ctx, nnames);
                /* glyph zero is the one a TrueType program has no name
                   for, which is the glyph .notdef stands for */
                ret = xpost_dict_put(ctx, csdict,
                                     name_dotnotdef,
                                     xpost_int_cons(0));
                for (i = 0; !ret
                         && xpost_font_face_std_name_at(data.face, i,
                                                        &snm, &gi); i++)
                {
                    if (!gi)
                        continue;
                    ret = xpost_dict_put(ctx, csdict,
                                         xpost_name_cons(ctx, snm),
                                         xpost_int_cons((integer)gi));
                }
                /* A face read through the symbol map answers to none of
                   those names: its codes are the font's own and reach
                   their glyphs in the private-use area of the code
                   space, where no standard name points. What it does
                   answer to is the point each of its codes reaches a
                   glyph at, which is the name the encoding of such a
                   face carries -- so the two say the same thing about
                   the same glyphs, which is what a program re-encoding
                   from this dictionary's keys depends on. */
                for (i = 0; symbolic && !ret && i < 256; i++)
                {
                    char cbuf[128];

                    if (!xpost_font_face_code_glyph_name(data.face, i,
                                                         cbuf, sizeof cbuf))
                        continue;
                    gi = xpost_font_face_glyph_index_get(data.face, (char)i);
                    if (!gi)
                        continue;
                    ret = xpost_dict_put(ctx, csdict,
                                         xpost_name_cons(ctx, cbuf),
                                         xpost_int_cons((integer)gi));
                }
                ctx->vmmode = oldmode;
                if (ret)
                    goto fail;
                {
                    Xpost_Object sealed =
                        xpost_object_set_access(ctx, csdict,
                                                XPOST_OBJECT_TAG_ACCESS_READ_ONLY);

                    /* as above: the dictionary was made under the save
                       level standing over this call, so the seal cannot
                       be declined the room */
                    if (xpost_object_get_type(sealed) == dicttype)
                        csdict = sealed;
                }
                if (slot >= 0)
                    _face_put(ctx, fname, name_CharStrings, csdict);
                ret = xpost_dict_put(ctx, fontdict,
                                     name_CharStrings,
                                     csdict);
                if (ret)
                    goto fail;
            }
have_charstrings: ;
        }
    }

    ret = _font_bbox_declare(ctx, fontdict, data.face, istt ? 1.0 : 1000.0);
    if (ret)
        goto fail;

    /* the dictionary states what backs it: a TrueType program makes
       a Type 42 font, its character space one unit to the em and its
       CharStrings naming glyph indices, as the type defines; any
       other face keeps the Type 1 dictionary conventions, character
       space a thousand units to the em. FontBBox shares the
       character space, and scalefont and makefont concatenate onto
       FontMatrix in dictionary copies */
    ret = xpost_dict_put(ctx, fontdict, name_FontType,
                       xpost_int_cons(istt ? 42 : cffreal ? 2 : 1));
    if (ret)
        goto fail;
    /* PLRM Table 5.4 requires PaintType of a Type 1 font dictionary, and
       the types above keep the Type 1 conventions. Every place that reads
       it here already treats its absence as a filled outline, which is
       what nought means, so stating it says what the interpreter was
       doing anyway -- and lets a program read the entry it is entitled
       to, rather than meeting an undefined where the specification
       promises a number. */
    ret = xpost_dict_put(ctx, fontdict, name_PaintType, xpost_int_cons(0));
    if (ret)
        goto fail;
    {
        /* the constructors answer executable objects; a font's matrix
           is data, so it says so at its construction, as
           doc/xpost_design.dox asks of every composite made here */
        Xpost_Object fontmatrix = xpost_object_cvlit(xpost_array_cons(ctx, 6));
        real diag = istt ? 1.0f : 0.001f;
        int mi;
        for (mi = 0; mi < 6; mi++)
        {
            int mret = xpost_array_put(ctx, fontmatrix, mi,
                            xpost_real_cons(mi == 0 || mi == 3 ? diag : 0.0f));
            if (mret)
            {
                ret = mret;
                goto fail;
            }
        }
        ret = xpost_dict_put(ctx, fontdict, name_FontMatrix, fontmatrix);
        if (ret)
            goto fail;
    }
    if (istt && xpost_object_get_type(sfnts_obj) == arraytype)
    {
        ret = xpost_dict_put(ctx, fontdict, name_sfnts,
                             sfnts_obj);
        if (ret)
            goto fail;
    }

    /* Every dictionary owns the face it names: one made from a cached
       face takes a claim of its own on it, so the cache can give its
       entry up while the dictionary is still in use, and the face goes
       when the last holder does. One the cache did not take is named by
       this dictionary alone. */
    if (!uncached)
    {
        xpost_font_face_reference(data.face);
        referenced = 1;
    }
    ret = _font_data_set(ctx, fontdict, data.face, 1);
    if (ret)
        goto fail;
    xpost_stack_push(ctx->lo, ctx->os, fontdict);
    free(fname);
    return 0;

    /* The claim this call made is this call's to give back: a face the
       cache did not take is named by nothing else, and one it did take
       stays the cache's, so only the reference taken for the handover
       above -- where the failure came after it -- goes with it. */
fail:
    if (uncached || referenced)
        xpost_font_face_free(data.face);
    free(fname);
    return ret;
}

/* -  fontdict stdenc  .faceencoding  array|null

   The encoding the face behind a font dictionary supplies: the glyph
   name that face gives each of the 256 character codes, which is what
   the /Encoding entry of a base font states (PLRM 5.2 and Table 5.7).
   The operand is the standard encoding, offered rather than held here
   so that the one array the language defines is the one this reads.

   A face that names no glyphs of its own has no encoding to give, and
   this answers null so that the caller's standard encoding stands --
   with one exception. A face read through the symbol map states an
   encoding without naming a glyph: its codes are the font's own and
   reach their glyphs in the private-use area of the code space, where
   none of the standard names points. The standard encoding is then a
   statement about that font which is not true, and what is put at each
   code instead is the point its own character map reaches the glyph
   at, spelled the way a name resolves back to a glyph here -- the same
   name the glyph complement beside it is published under, so the two
   agree about the same glyphs.

   Where the face does name its glyphs, what it says depends on what
   kind of font program it is, because the two kinds keep their
   encoding in different places.

   A TrueType program keeps none: it has no PostScript encoding vector
   at all, and what the codes of such a font mean is what its character
   map says they mean. So every code is the name of the glyph the
   character map reaches for it, and .notdef where it reaches none.

   A Type 1 or CFF program carries an encoding vector of its own, and
   for the great majority of text faces that vector is the standard
   one. It is not read out of the file here -- an sfnt-wrapped CFF
   need not carry one at all -- but recovered from the two things the
   face does answer for: the names it gives its glyphs, and the glyphs
   its character map reaches. A code whose standard name the face has
   a glyph of keeps that name, which leaves an ordinary text face with
   the standard encoding exactly. A code whose standard name the face
   does not have takes the name of the glyph its character map reaches
   instead, which is how a symbolic face states the encoding it was
   built with. The codes the standard encoding leaves unnamed are
   filled the same way only where such a face has departed from the
   standard encoding somewhere: a face that agreed with it throughout
   is a text face, and what the standard encoding leaves unnamed is
   unnamed on it too.

   A glyph the encoding already names is not named a second time. A
   character map reaches one glyph from several code points -- the
   no-break space reaches the space glyph -- and only the code the
   encoding names it at is a name for it; the others are the character
   map's business and not the encoding's.

   The names are interned in global virtual memory, as the glyph names
   of the CharStrings dictionary beside them are, so that a font found
   under one allocation mode and read under the other names its glyphs
   with the same objects. The array itself is made where the font
   dictionary is, being the one part of a font a program re-encodes in
   place. */
static
int _faceencoding(Xpost_Context *ctx,
                  Xpost_Object fontdict,
                  Xpost_Object stdenc)
{
    fontdata *fd = _font_data(ctx, fontdict);
    void *face = fd ? fd->face : NULL;
    Xpost_Object names[256];
    unsigned int gids[256];
    Xpost_Object enc;
    unsigned int oldmode;
    int istt;
    int named;
    int departs = 0;
    int c;

    named = face ? xpost_font_face_glyph_name_count(face) != 0 : 0;
    if (!face
     || stdenc.comp_.sz != 256
     || (!named && !xpost_font_face_is_symbol_encoded(face)))
    {
        if (!xpost_stack_push(ctx->lo, ctx->os, null))
            return stackoverflow;
        return 0;
    }

    istt = xpost_font_face_is_truetype(face);
    oldmode = ctx->vmmode;

    for (c = 0; c < 256; c++)
    {
        char nbuf[128];
        unsigned int gi;

        names[c] = xpost_array_get(ctx, stdenc, c);
        gids[c] = 0;

        if (!istt && named)
        {
            char sbuf[128];
            Xpost_Object sstr;

            if (xpost_object_get_type(names[c]) != nametype)
                continue;
            sstr = xpost_name_get_string(ctx, names[c]);
            if (sstr.comp_.sz >= sizeof sbuf)
                continue;
            /* the face's own glyph names are read as C text, which ends
               at the first nul: a name carrying one would be compared
               only that far and take the glyph of the shorter name it
               begins with. Such a name is asked of the face no more than
               a name the face does not carry is */
            if (xpost_string_is_cstring(ctx, sstr))
            {
                /* the characters are copied before anything allocates:
                   they live in virtual memory, which an intern may move */
                memcpy(sbuf, xpost_string_get_pointer(ctx, sstr), sstr.comp_.sz);
                sbuf[sstr.comp_.sz] = '\0';
                if (strcmp(sbuf, ".notdef") == 0)
                    continue;
                gi = xpost_font_face_own_name_index_get(face, sbuf);
                if (gi)
                {
                    gids[c] = gi;
                    continue;
                }
            }
            /* the face has no glyph of that name, so the standard
               encoding is not this face's: what the code means here is
               what the character map says, and where it says nothing
               the code means nothing */
            names[c] = xpost_object_cvlit(name_dotnotdef);
        }

        gi = xpost_font_face_glyph_index_get(face, (char)c);
        if (gi && (xpost_font_face_glyph_name_get(face, gi, nbuf, sizeof nbuf)
                || (!named
                 && xpost_font_face_code_glyph_name(face, (unsigned int)c,
                                                    nbuf, sizeof nbuf))))
        {
            ctx->vmmode = GLOBAL;
            names[c] = xpost_object_cvlit(xpost_name_cons(ctx, nbuf));
            ctx->vmmode = oldmode;
            if (xpost_object_get_type(names[c]) == invalidtype)
                return VMerror;
            gids[c] = gi;
            departs = 1;
        }
        else if (istt || !named)
            names[c] = xpost_object_cvlit(name_dotnotdef);
    }

    if (!istt && departs)
    {
        for (c = 0; c < 256; c++)
        {
            char nbuf[128];
            unsigned int gi;
            int j;

            if (gids[c])
                continue;
            gi = xpost_font_face_glyph_index_get(face, (char)c);
            if (!gi)
                continue;
            for (j = 0; j < 256; j++)
                if (gids[j] == gi)
                    break;
            if (j < 256)
                continue;
            if (!xpost_font_face_glyph_name_get(face, gi, nbuf, sizeof nbuf))
                continue;
            ctx->vmmode = GLOBAL;
            names[c] = xpost_object_cvlit(xpost_name_cons(ctx, nbuf));
            ctx->vmmode = oldmode;
            if (xpost_object_get_type(names[c]) == invalidtype)
                return VMerror;
            gids[c] = gi;
        }
    }

    /* an encoding is data, so it says so at its construction, as
       doc/xpost_design.dox asks of every composite made here */
    enc = xpost_object_cvlit(xpost_array_cons(ctx, 256));
    if (xpost_object_get_type(enc) != arraytype)
        return VMerror;
    for (c = 0; c < 256; c++)
    {
        int ret = xpost_array_put(ctx, enc, c, names[c]);

        if (ret)
            return ret;
    }
    if (!xpost_stack_push(ctx->lo, ctx->os, enc))
        return stackoverflow;
    return 0;
}

/* Load a Type 42 font program: reassemble the /sfnts strings into one
   malloc'd buffer, open it as a memory face, and stash the face in the
   dict's /Private exactly as findfont does for a file face. The buffer
   backs the face for the face's lifetime; like the findfont face cache,
   defined fonts live for the process. */
static
int _loadfont42(Xpost_Context *ctx,
                Xpost_Object fontdict)
{
    Xpost_Object sfnts;
    struct fontdata data;
    unsigned char *buf;
    size_t total;
    word i;
    int ret;

    sfnts = xpost_dict_get(ctx, fontdict, name_sfnts);
    if (xpost_object_get_type(sfnts) != arraytype)
        return invalidfont;
    total = 0;
    for (i = 0; i < sfnts.comp_.sz; i++)
    {
        Xpost_Object s = xpost_array_get(ctx, sfnts, i);
        if (xpost_object_get_type(s) != stringtype)
            return invalidfont;
        total += s.comp_.sz;
    }
    if (total == 0)
        return invalidfont;
    buf = malloc(total);
    if (!buf)
        return VMerror;
    total = 0;
    for (i = 0; i < sfnts.comp_.sz; i++)
    {
        Xpost_Object s = xpost_array_get(ctx, sfnts, i);
        memcpy(buf + total, xpost_string_get_pointer(ctx, s), s.comp_.sz);
        total += s.comp_.sz;
    }

    data.face = xpost_font_face_new_from_memory(buf, total);
    if (data.face == NULL)
    {
        free(buf);
        return invalidfont;
    }

    /* a Type 42 dictionary maps one em to one glyph-space unit */
    ret = _font_bbox_declare(ctx, fontdict, data.face, 1.0);
    if (ret)
        goto fail;

    ret = _font_data_set(ctx, fontdict, data.face, 1);
    if (ret)
        goto fail;
    return 0;

    /* the face reads the program where it lies and no block names
       either yet, so this call is the only thing that can give them
       back; the face takes its program with it */
fail:
    xpost_font_face_free(data.face);
    return ret;
}

/* scalefont and makefont are implemented in font.ps: each returns a
   fresh dictionary with the requested transform concatenated onto the
   font's FontMatrix. No operator mutates the shared face; the text
   operators size it from FontMatrix and the CTM at use time
   (_face_setup below). */



/* The graphics state a text operator paints in. The interpreter keeps it
   in its own dictionary rather than one a program can reach, so it is
   found the same way whatever a program has pushed on the dictionary
   stack. Answers invalid where the graphics language was never loaded,
   which is a configuration these operators are absent from. */
static
Xpost_Object _gstate(Xpost_Context *ctx)
{
    Xpost_Object gd = ctx->graphicsdict;

    return xpost_dict_get(ctx, gd, name_currgstate);
}

/* wx wy  .setglyphadvance  -
   Record the advance a glyph just built declared, in the two entries the
   showing operators read it back from: the current font's xadvance and
   yadvance.

   definefont makes a font dictionary read-only (PLRM 8.2), and a
   dictionary carries its access on its value rather than on the
   reference (PLRM 3.3.2), so sealing it seals every reference and there
   is no writable one left for the font machinery to keep. This write
   therefore passes the attribute rather than going round it.

   It is not a way to write a dictionary of a caller's choosing. The
   dictionary is the one the graphics state names as current, which no
   operand chooses, and the keys are these two and no others.

   The advance belongs to the font rather than to the glyph state the
   build ran under, because a build may show text of its own: the state
   is put back to the enclosing build's when the inner one ends, and
   what the showing operator then reads has to be the advance the glyph
   it asked for declared. */
static
int _setglyphadvance(Xpost_Context *ctx,
                     Xpost_Object X,
                     Xpost_Object Y)
{
    Xpost_Object font;
    int ret;

    font = xpost_dict_get(ctx, _gstate(ctx), name_currfont);
    if (xpost_object_get_type(font) != dicttype)
        return invalidfont;
    ret = xpost_dict_put_internal(ctx, font, name_xadvance, X);
    if (ret)
        return ret;
    return xpost_dict_put_internal(ctx, font, name_yadvance, Y);
}


/* Puts a font dictionary in the graphics state as the current font. */
static
int _setfont(Xpost_Context *ctx,
             Xpost_Object fontdict)
{
    Xpost_Object gs;
    int ret;

    gs = _gstate(ctx);

    ret = xpost_dict_put(ctx, gs, name_currfont, fontdict);
    if (ret)
        return ret;

    return 0;
}

/* The bands last read back, kept against the array they were read from
   and the serial of the clip that array belongs to. Reading the array
   costs five object fetches per band of the whole region, and every
   text operator under one region would pay it again; narrowing a glyph
   against the bands, which is what the reading is for, costs the rows
   the glyph covers. The serial never repeats within a run, so an array
   that has since been reused for something else cannot answer for it.
   Nothing here holds a reference into VM: the entity number is
   compared, never followed. */
static struct
{
    int serial;
    int ent;
    unsigned int off, sz;
    clipband *band;
    int n;
} _clip_memo;

/* Give the read-back bands up. They are one array, replaced as each
   clip is read, so what is held at any moment is the last clip read --
   and it is keyed by a serial and an entity, both of which a later
   lifetime issues again from the start.

   Reached from outside this file as well: the region serial the memo is
   filed under is minted elsewhere (.newregionserial), and a counter that
   restarts hands out a number this memo may still be holding, so the
   restart calls here. */
void xpost_op_font_clip_memo_drop(void)
{
    free(_clip_memo.band);
    memset(&_clip_memo, 0, sizeof(_clip_memo));
}

/* Bands in the order the narrowing below walks them: down the page,
   and left to right within a row. */
static int _band_comp(const void *a, const void *b)
{
    const clipband *p = a, *q = b;

    if (p->band != q->band)
        return p->band < q->band ? -1 : 1;
    if (p->lo != q->lo)
        return p->lo < q->lo ? -1 : 1;
    return 0;
}

/* One number out of the resolved region's array, answering whether the
   slot held a number at all -- the array is built by the interpreter,
   and a slot that is not a number means the tile is not the shape this
   reader expects. */
static int _clip_number(Xpost_Context *ctx, Xpost_Object arr, int i, double *v)
{
    Xpost_Object o = xpost_array_get(ctx, arr, i);

    if (xpost_object_get_type(o) == realtype)
        *v = o.real_.val;
    else if (xpost_object_get_type(o) == integertype)
        *v = (double)o.int_.val;
    else
        return 0;
    return 1;
}

/* A device bound as the pixel index it names. The region's own bounds
   are page-sized, but they are read from a path a program may have put
   any number into, and a raster is indexed by int: a bound past the
   range is taken to the end of it, which is outside every raster either
   way and so narrows exactly as the true value would. */
#define CLIP_COORD_MAX 16777216.0
static
int _clip_floor(double v)
{
    v = floor(v);
    return v < -CLIP_COORD_MAX ? (int)-CLIP_COORD_MAX
         : v > CLIP_COORD_MAX ? (int)CLIP_COORD_MAX : (int)v;
}
static
int _clip_ceil(double v)
{
    v = ceil(v);
    return v < -CLIP_COORD_MAX ? (int)-CLIP_COORD_MAX
         : v > CLIP_COORD_MAX ? (int)CLIP_COORD_MAX : (int)v;
}

/* Read the region's resolved form into the band table above. The form
   is an array of tiles, each a null-separated array of pixel-band
   rectangles of five objects apiece, as .regionmeet returns one. The
   tiles meet along row boundaries and no two describe the same row, so
   the bands only need ordering.
   Answers the band count, or -1 when the array is not in that form. */
static
int _clip_bands_get(Xpost_Context *ctx, Xpost_Object spans, int serial)
{
    clipband *b;
    int nslice = (int)spans.comp_.sz;
    int n = 0;
    int i, k, at;

    if (_clip_memo.band
     && _clip_memo.serial == serial
     && _clip_memo.ent == xpost_object_get_ent(spans)
     && _clip_memo.off == spans.comp_.off
     && _clip_memo.sz == spans.comp_.sz)
        return _clip_memo.n;

    for (k = 0; k < nslice; k++)
    {
        Xpost_Object sl = xpost_array_get(ctx, spans, k);

        if (xpost_object_get_type(sl) != arraytype || sl.comp_.sz % 5)
            return -1;
        n += (int)(sl.comp_.sz / 5);
    }
    /* an empty region resolves to no bands at all, and covers no pixel */
    b = NULL;
    if (n)
    {
        b = malloc((size_t)n * sizeof *b);
        if (!b)
            return -1;
    }
    at = 0;
    for (k = 0; k < nslice; k++)
    {
        Xpost_Object sl = xpost_array_get(ctx, spans, k);
        int m = (int)(sl.comp_.sz / 5);

        /* the table was sized by a first pass over the same array, and
           the write stays inside what that pass counted; the count the
           region is described by is what this pass wrote, so no band
           beyond it is ever read */
        for (i = 0; i < m && at < n; i++)
        {
            Xpost_Object p0 = xpost_array_get(ctx, sl, 5 * i);
            Xpost_Object p1 = xpost_array_get(ctx, sl, 5 * i + 1);
            double lo, hi, band;

            if (xpost_object_get_type(p0) != arraytype || p0.comp_.sz != 2
             || xpost_object_get_type(p1) != arraytype || p1.comp_.sz != 2
             || !_clip_number(ctx, p0, 0, &lo)
             || !_clip_number(ctx, p0, 1, &band)
             || !_clip_number(ctx, p1, 0, &hi))
            {
                free(b);
                return -1;
            }
            /* the rectangles sit on pixel boundaries: they are the
               columns and the row a fill of the region covers */
            b[at].band = _clip_floor(band + 0.5);
            b[at].lo = _clip_floor(lo + 0.5);
            b[at].hi = _clip_floor(hi + 0.5);
            at++;
        }
    }
    if (at > 1)
        qsort(b, (size_t)at, sizeof *b, _band_comp);
    (void)xpost_at_quit(xpost_op_font_quit);
    free(_clip_memo.band);
    _clip_memo.serial = serial;
    _clip_memo.ent = xpost_object_get_ent(spans);
    _clip_memo.off = spans.comp_.off;
    _clip_memo.sz = spans.comp_.sz;
    _clip_memo.band = b;
    _clip_memo.n = at;
    return at;
}

/* The pixels the clip region covers, as the glyph raster route meets
   them. .showclip in data/font.ps leaves the region's description in
   the clip's own cache holder before any text operator reaches here:
   /clipbox when the region is a rectangle, its device bounds, and
   /clipspans otherwise, the region resolved through the same span
   intersection every fill meets a region by, a slice of rows per
   element. A holder carrying neither describes no region and narrows
   nothing. */
static
void _text_clip_get(Xpost_Context *ctx, Xpost_Object gs, textstate *ts)
{
    Xpost_Object cache, o;
    int serial = 0;

    ts->clipkind = CLIP_ALL;
    ts->bands = NULL;
    ts->nbands = 0;
    cache = xpost_dict_get(ctx, gs, name_clipcache);
    if (xpost_object_get_type(cache) != dicttype)
        return;
    o = xpost_dict_get(ctx, cache, name_serial);
    if (xpost_object_get_type(o) == integertype)
        serial = o.int_.val;

    o = xpost_dict_get(ctx, cache, name_clipbox);
    if (xpost_object_get_type(o) == arraytype && o.comp_.sz == 4)
    {
        double c[4];

        if (!_clip_number(ctx, o, 0, &c[0])
         || !_clip_number(ctx, o, 1, &c[1])
         || !_clip_number(ctx, o, 2, &c[2])
         || !_clip_number(ctx, o, 3, &c[3]))
            return;
        /* the bounds meet the pixel grid under the any-part-of-pixel
           rule of PLRM 7.5.1, on the quantized coordinates the scan
           conversion reads a region's vertices as */
        ts->cx0 = _clip_floor(xpost_dev_line_quantize(c[0]));
        ts->cy0 = _clip_floor(xpost_dev_line_quantize(c[1]));
        ts->cx1 = _clip_ceil(xpost_dev_line_quantize(c[2]));
        ts->cy1 = _clip_ceil(xpost_dev_line_quantize(c[3]));
        ts->clipkind = CLIP_BOX;
        return;
    }

    o = xpost_dict_get(ctx, cache, name_clipspans);
    if (xpost_object_get_type(o) == arraytype)
    {
        int n = _clip_bands_get(ctx, o, serial);

        if (n < 0)
            return;
        ts->bands = _clip_memo.band;
        ts->nbands = n;
        ts->cy0 = n ? _clip_memo.band[0].band : 0;
        ts->cy1 = n ? _clip_memo.band[n - 1].band + 1 : 0;
        ts->clipkind = CLIP_BANDS;
    }
}

/* the index of the first band of row y, or the band count when the
   region covers no part of that row */
static
int _clip_band_row(const textstate *ts, int y)
{
    int lo = 0, hi = ts->nbands;

    while (lo < hi)
    {
        int mid = lo + (hi - lo) / 2;

        if (ts->bands[mid].band < y)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* How many bits of a glyph's partly covered edge pixels reach this
   device, and what blends them where any do. Zero says the device is
   sent whole pixels and the edge is hard.

   Three things have to hold. The device must offer BlendPix, since that
   is the call a blend is made through. Its class must state
   TextAlphaBits above one, which is what says its stored pixel can hold
   a value between covered and not; a page-device request may name that
   number, so what arrives here is the run's answer rather than the
   class's. Eight is as many as there are: the rasteriser resolves a
   coverage to a byte, so a device asking for more is answered with the
   whole of what there is rather than with a precision nobody has.

   And the device must not be one that shows a grey as a pattern of
   pixels. Such a device declares ScreenPaint and stores a grey by
   ranking the pixel against the threshold the screen in force picks for
   its position -- that ranking is the only means it has of showing a
   grey at all, and it lives in the pixel store the blend does not go
   through. A blend on such a device writes a coverage straight into the
   stored pixel, so the pixels a glyph's edges cover are the pixels its
   screen is not applied to, and what it holds afterwards is a range of
   values it has no way to show. Reading it back reports them.

   That is a property of the device rather than a setting, so it settles
   the question here rather than being guarded: a run naming any number
   it likes reaches a device that screens and still gets whole pixels. */
static
int _text_blends(Xpost_Context *ctx,
                 Xpost_Object devdic,
                 Xpost_Object *blendpix)
{
    Xpost_Object bp, tab, key;

    bp = xpost_dict_get(ctx, devdic, name_BlendPix);
    if (blendpix)
        *blendpix = bp;
    if (xpost_object_get_type(bp) != operatortype
        && !(xpost_object_get_type(bp) == arraytype
             && xpost_object_is_exe(bp)))
        return 0;

    tab = xpost_dict_get(ctx, devdic, name_TextAlphaBits);
    if (xpost_object_get_type(tab) != integertype || tab.int_.val <= 1)
        return 0;

    key = name_ScreenPaint;
    if (xpost_object_get_type(key) != invalidtype
        && xpost_dict_known_key(ctx, xpost_context_select_memory(ctx, devdic),
                                devdic, key))
        return 0;

    return tab.int_.val >= COVERAGE_BITS ? COVERAGE_BITS : (int)tab.int_.val;
}

/* A coverage rounded to the nearest of the values a device asking for
   that many bits of it can tell apart. Eight bits is every value the
   rasteriser produced, and the coverage passes through.

   The steps are the gaps between the values, not the values: n bits
   name (1 << n) values with one fewer gap between them, and a coverage
   lands on the step it is nearest and is carried back out over the
   whole range. Full coverage and none of it are the ends of that range
   whatever n is, so a pixel a glyph fills stays filled and one it
   misses stays missed. */
static
unsigned char _cov_bits(int cov, int bits)
{
    int steps;

    if (bits >= COVERAGE_BITS)
        return (unsigned char)cov;
    steps = (1 << bits) - 1;
    return (unsigned char)(((cov * steps + 127) / 255 * 255 + steps / 2)
                           / steps);
}

/* Everything a text operator needs to know before it paints a
   character: how the font maps codes to glyphs, what the device wants
   a mark in, and where the colour comes from. Gathered once for a
   whole string rather than per character. */
static
textstate _text_state_get(Xpost_Context *ctx,
                          Xpost_Object gs,
                          Xpost_Object fontdict,
                          Xpost_Object devdic)
{
    textstate ts;
    Xpost_Object vec, sep;

    ts.encoding = xpost_dict_get(ctx, fontdict, name_Encoding);
    ts.charstrings = xpost_dict_get(ctx, fontdict, name_CharStrings);
    ts.metrics = xpost_dict_get(ctx, fontdict, name_Metrics);
    ts.cdmat_ok = xpost_object_get_type(ts.metrics) == dicttype
               && _char_device_matrix(ctx, gs, fontdict, ts.cdmat);
    ts.blend = _text_blends(ctx, devdic, &ts.blendpix);
    vec = xpost_dict_get(ctx, devdic, name_VectorGlyphs);
    ts.vector = xpost_object_get_type(vec) == booleantype && vec.int_.val;
    /* an extent-tracking device (the bbox device) needs no glyph
       rasterization: each glyph contributes its ink box through the
       device's FillRect instead */
    vec = xpost_dict_get(ctx, devdic, name_GlyphExtents);
    ts.extents = xpost_object_get_type(vec) == booleantype && vec.int_.val;
    memset(&ts.fillrect, 0, sizeof ts.fillrect);  /* invalidtype */
    if (ts.extents)
        ts.fillrect = xpost_dict_get(ctx, devdic, name_FillRect);
    /* a separation the graphics state registered with the device:
       glyph outlines fill in the separation, not the process colour */
    ts.sepindex = -1;
    ts.septint = 0.0;
    sep = xpost_dict_get(ctx, gs, name_sepindex);
    if (xpost_object_get_type(sep) == integertype)
    {
        Xpost_Object tint = xpost_dict_get(ctx, gs, name_septint);
        ts.sepindex = sep.int_.val;
        if (xpost_object_get_type(tint) == realtype)
            ts.septint = tint.real_.val;
        else if (xpost_object_get_type(tint) == integertype)
            ts.septint = (double)tint.int_.val;
    }
    _text_clip_get(ctx, gs, &ts);
    return ts;
}

/* --- which glyph a character is --------------------------------------
   Two routes, because a program may name a character or hand over the
   glyph itself: an encoded code goes through the encoding to a name and
   the name to an index, while glyphshow starts at the name. */

/* Map a character code to a glyph index. When the font carries an
   /Encoding array with a glyph name at this code, the name selects the
   glyph. A Type 42 font's /CharStrings dictionary maps glyph names to
   glyph indices and is authoritative when it holds an integer for the
   name: subset fonts strip the sfnt's own name and character-map
   tables and carry the name-to-index mapping only here. Otherwise the
   name is resolved against the face's glyph names; codes whose entry
   is not a name (the findfont wrapper fills /Encoding with nulls), or
   whose name resolves nowhere, fall back to the face's character map,
   preserving the plain-text behaviour of an unencoded font. */
static
unsigned int _glyph_index_for_char(Xpost_Context *ctx,
                                   Xpost_Object encoding,
                                   Xpost_Object charstrings,
                                   void *face,
                                   unsigned int ch)
{
    if (xpost_object_get_type(encoding) == arraytype
     && ch < (unsigned int)encoding.comp_.sz)
    {
        Xpost_Object en = xpost_array_get(ctx, encoding, ch);
        if (xpost_object_get_type(en) == nametype)
        {
            Xpost_Object str;
            char *cname;
            unsigned int gi = 0;

            if (xpost_object_get_type(charstrings) == dicttype)
            {
                Xpost_Object gid = xpost_dict_get(ctx, charstrings,
                                                  xpost_object_cvlit(en));
                if (xpost_object_get_type(gid) == integertype
                 && gid.int_.val >= 0)
                    return (unsigned int)gid.int_.val;
            }
            str = xpost_name_get_string(ctx, en);
            /* past the CharStrings dictionary, which is keyed by the
               name itself, the face's own glyph names are read as C
               text and a name carrying a nul would be compared only as
               far as that nul, taking the glyph of the shorter name it
               begins with. Such a name is asked of the face no more
               than a name the face does not carry is, and the code
               falls back to the character map for both alike */
            if (xpost_string_is_cstring(ctx, str))
            {
                cname = xpost_string_allocate_cstring(ctx, str);
                if (cname)
                {
                    if (strcmp(cname, ".notdef") == 0)
                    {
                        free(cname);
                        return 0;
                    }
                    gi = xpost_font_face_glyph_name_index_get(face, cname);
                    free(cname);
                }
            }
            if (gi)
                return gi;
        }
    }
    return xpost_font_face_glyph_index_get(face, (char)ch);
}

/* Map a glyph name to a glyph index without passing through a
   character code, as glyphshow selects glyphs: the CharStrings
   dictionary decides when it holds an integer for the name, then the
   face's own glyph names; an unknown name selects the notdef glyph,
   there being no code to fall back to the character map with. */
static
unsigned int _glyph_index_for_name(Xpost_Context *ctx,
                                   Xpost_Object charstrings,
                                   void *face,
                                   Xpost_Object gname)
{
    Xpost_Object str;
    char *cname;
    unsigned int gi = 0;

    if (xpost_object_get_type(charstrings) == dicttype)
    {
        Xpost_Object gid = xpost_dict_get(ctx, charstrings,
                                          xpost_object_cvlit(gname));
        if (xpost_object_get_type(gid) == integertype
         && gid.int_.val >= 0)
            return (unsigned int)gid.int_.val;
    }
    str = xpost_name_get_string(ctx, gname);
    /* as above: the face's glyph names are C text, so a name carrying a
       nul is not one of them and selects notdef */
    if (!xpost_string_is_cstring(ctx, str))
        return 0;
    cname = xpost_string_allocate_cstring(ctx, str);
    if (cname)
    {
        if (strcmp(cname, ".notdef") != 0)
            gi = xpost_font_face_glyph_name_index_get(face, cname);
        free(cname);
    }
    return gi;
}

/* Whether the half-open pixel box [x0,x1) x [y0,y1) meets the clip
   region. PLRM 7.5.1 confines a painting operation to the pixels the
   region covers, so a box meeting none of them covers no pixel and
   leaves the page as it found it. An empty box meets nothing. */
static
int _clip_meets(const textstate *ts, int x0, int y0, int x1, int y1)
{
    int y;

    if (x1 <= x0 || y1 <= y0)
        return 0;
    if (ts->clipkind == CLIP_ALL)
        return 1;
    if (x1 <= ts->cx0 || x0 >= ts->cx1 || y1 <= ts->cy0 || y0 >= ts->cy1)
        return 0;
    if (ts->clipkind == CLIP_BOX)
        return 1;
    for (y = y0 > ts->cy0 ? y0 : ts->cy0; y < y1 && y < ts->cy1; y++)
    {
        int run = _clip_band_row(ts, y);

        for (; run < ts->nbands && ts->bands[run].band == y; run++)
            if (ts->bands[run].lo < x1 && ts->bands[run].hi > x0)
                return 1;
    }
    return 0;
}

/* --- painting a rendered glyph ---------------------------------------
   A glyph arrives as a coverage mask and leaves as marks on the device.
   What happens between is the colour, the clip and the blend: the mask
   says how much of each pixel the glyph covers, and the device is asked
   for that coverage of the current colour. */

/* Record that something has been painted on the current page.
   The record is the one the page machinery keeps and the painting
   operators write as they mark (data/device.ps), reached here through
   the local machinery dictionary it is registered in. Text does not
   pass a painting operator: a glyph's pixels go straight to the
   device, so the routes below record what they left for themselves,
   and a job that painted nothing but text still has a page to give
   when it ends without asking for one.
   An interpreter running without the page machinery has no record to
   write and nothing to say about it. */
static
int _page_mark(Xpost_Context *ctx)
{
    Xpost_Object state = xpost_dict_get(ctx, ctx->privatedict,
                                        name_dotpagestate);

    if (xpost_object_get_type(state) != dicttype)
        return 0;
    return xpost_dict_put(ctx, state, name_dotmarked,
                          xpost_bool_cons(1));
}

/* Prepare the shared face for use under the current graphics state.
   The font dictionary's FontMatrix carries the size (and any rotation,
   shear or anisotropy concatenated by makefont); the CTM carries the
   device mapping. Neither is sticky on the font: scalefont and
   makefont only build dictionaries, so two sizes of one face coexist
   and the CTM matters when the glyphs are marked, not when the font
   was scaled. Compose the two linear parts, split the result into a
   pixel-per-em scale for the face and a unit-magnitude transform
   (conjugated by the y flip that relates FreeType's y-up glyph space
   to the device's y-down raster), and install both. The face is
   shared through the findfont cache, so every text operator must call
   this before touching glyphs. A missing or malformed FontMatrix
   reads as the identity, serving font programs defined without one. */
static
void _face_setup(Xpost_Context *ctx,
                 Xpost_Object gs,
                 Xpost_Object fontdict,
                 void *face)
{
    real e[4];
    real q;
    real r;
    float mat[6] = { 0 };

    /* text space -> device space: FontMatrix then CTM */
    if (!_char_device_matrix(ctx, gs, fontdict, e))
        return;

    q = (real)sqrt(e[0] * e[0] + e[1] * e[1]);
    if (q == 0)
        q = (real)sqrt(e[2] * e[2] + e[3] * e[3]);
    if (q == 0)
        return;

    /* the em in pixels: FontMatrix maps character space to text space,
       so the composed magnitude q is per character-space unit, and the
       units per em are a convention of the font type (1000 for Type 1
       dictionaries, whose FontMatrix carries the 0.001 factor; one for
       Type 42, whose FontMatrix is an identity over the em) */
    {
        Xpost_Object ft = xpost_dict_get(ctx, fontdict,
                                         name_FontType);
        Xpost_Object cft = xpost_dict_get(ctx, fontdict,
                                          name_CIDFontType);
        real qem = q;
        if ((xpost_object_get_type(ft) == integertype
             && (ft.int_.val == 1 || ft.int_.val == 2))
         || (xpost_object_get_type(cft) == integertype && cft.int_.val == 0))
        {
            /* a Type 1 character-space unit is usually a thousandth
               of the em -- the convention findfont dictionaries
               declare whatever their face's native units -- but an
               embedded program keeps its design count (a converted
               2048-unit font arrives with a 1/2048 matrix), recorded
               in the dictionary when its face was assembled */
            Xpost_Object emu = xpost_dict_get(ctx, fontdict,
                                              name_dotemunits);
            int units = xpost_object_get_type(emu) == integertype
                      ? emu.int_.val : 1000;

            qem = q * (units > 0 ? units : 1000);
        }

        /* the face serves a well-conditioned base size (an extreme em
           would fail inside FreeType); the residual ratio to the true
           size rides in the transform, which scales outlines, extents
           and linear advances alike */
        r = qem / xpost_font_face_scale(face, qem);
    }

    mat[0] = (float)( e[0] / q * r);   /* xx */
    mat[1] = (float)( e[2] / q * r);   /* xy */
    mat[2] = (float)(-e[1] / q * r);   /* yx */
    mat[3] = (float)(-e[3] / q * r);   /* yy */
    xpost_font_face_transform(face, mat);
}

/* Whether a member of a font dictionary may be read. A member the
   dictionary does not carry, and one that is not a composite object, has
   no access attribute to withhold the read and answers yes. */
static
int _member_readable(Xpost_Context *ctx, Xpost_Object fontdict, Xpost_Object key)
{
    Xpost_Object m = xpost_dict_get(ctx, fontdict, key);

    if (!xpost_object_is_composite(m))
        return 1;
    return xpost_object_is_readable(ctx, m);
}

/* The font data a dict names, with its face made current, or nothing.
 *
 * The two go together. A face is shared through the findfont cache, so
 * every text operator has to set it up for this dict before touching a
 * glyph -- and every one of them has to answer the dict that names no
 * usable face, which is the ordinary case of a font whose program never
 * loaded. Eight operators did both by hand, in the same eight lines, and
 * a ninth written from the same shape would have had to remember the
 * setup as well as the check. Here it cannot be had without the other.
 *
 * Answers invalidfont where the dict names no face, leaving *data
 * untouched, and invalidaccess where a member the glyphs are read out of
 * may not be read. Zero is the answer that means the face is current.
 *
 * The access question belongs with the setup for the same reason the
 * face check does. What a text operator paints comes out of the font's
 * Encoding, CharStrings and Metrics, so it reads them, and a value may
 * be read only where its access permits (PLRM 3.3.2, and the
 * invalidaccess entry, which names reading a value in violation of its
 * access among the causes). The other road to a glyph -- a font whose
 * build procedure runs PostScript -- reaches those members through get,
 * which asks the same question of its own accord, so asking it here is
 * what makes the two roads answer alike. */
static
int _font_data_current(Xpost_Context *ctx,
                       Xpost_Object gs,
                       Xpost_Object fontdict,
                       fontdata *data)
{
    fontdata *fd;

    if (!_member_readable(ctx, fontdict, name_Encoding)
        || !_member_readable(ctx, fontdict, name_CharStrings)
        || !_member_readable(ctx, fontdict, name_Metrics))
        return invalidaccess;

    fd = _font_data(ctx, fontdict);
    if (!fd || fd->face == NULL)
    {
        XPOST_LOG_INFO("face is NULL");
        return invalidfont;
    }
    *data = *fd;
    _face_setup(ctx, gs, fontdict, data->face);
    return 0;
}


/* The current colour as the device takes it: the components in the
   device's own space with the transfer functions applied (PLRM 7.3).

   The painting machinery resolves it and leaves it in the graphics
   dictionary immediately before the call that paints from it, so a
   glyph and a fill under one graphics state reach the device in the
   same components. Where the conversions and the transfer live is the
   point: they are stated once, in the colour and painting machinery, and
   a raster of coverage reads the answer rather than working it out a
   second time -- two statements of the same arithmetic agree only until
   one of them is changed.

   A route that paints coverage without resolving the colour first
   leaves nothing there, and the paint is refused rather than made in
   whatever the graphics state reads as: a mark in a colour no operator
   asked for is a wrong page, and a wrong page nothing can tell from a
   right one. Returns 0 on success. */
static
int _device_color(Xpost_Context *ctx,
                  Xpost_Object gs,
                  Xpost_Object devdic,
                  int *ncomp,
                  Xpost_Object comp[4])
{
    Xpost_Object marked;
    int i, n;

    (void)gs;
    (void)devdic;
    marked = xpost_dict_get(ctx, ctx->graphicsdict, name_dotmarkedcolor);
    if (xpost_object_get_type(marked) != arraytype)
    {
        XPOST_LOG_ERR("no marked colour resolved for this paint");
        return unregistered;
    }
    n = (int)marked.comp_.sz;
    if ((n < 1) || (n > 4))
    {
        XPOST_LOG_ERR("marked colour of %d components", n);
        return unregistered;
    }
    /* the components a device is not given still travel to it as
       arguments, so all four are values rather than whatever the
       caller's frame held */
    for (i = 0; i < 4; i++)
        comp[i] = xpost_real_cons((real)0.0);
    for (i = 0; i < n; i++)
    {
        Xpost_Object o = xpost_array_get(ctx, marked, i);

        if (xpost_object_get_type(o) == integertype)
            o = xpost_real_cons((real)o.int_.val);
        else if (xpost_object_get_type(o) != realtype)
        {
            XPOST_LOG_ERR("marked colour component is not a number");
            return unregistered;
        }
        comp[i] = o;
    }
    *ncomp = n;
    return 0;
}

/* Plot a rendered glyph bitmap through the device. An 8-bit coverage
   bitmap is thresholded at half coverage -- the sharp rasterization a
   scan conversion of the outline would produce -- unless the device
   anti-aliases text (ts->blend), in which case fully covered pixels go
   through PutPix and partially covered edge pixels through the
   device's BlendPix with their coverage, rounded to the number of
   values the bits the device asked for can tell apart.
   A device that writes down what it is asked to paint rather than
   painting it declares /.recordglyph, and is handed the coverage that
   walk resolves as one mask and one placement of it instead of a call
   per inked pixel. The two routes resolve the same coverage in the same
   loop -- which pixels the region keeps and how much of each is covered
   is stated once, here -- and differ only in where the answer goes: a
   device that paints is told a pixel at a time because that is what
   painting is, and a device that stores is told a glyph at a time
   because a mark per inked pixel costs it tens of bytes where the page
   it is escaping costs one to three (doc/xpost_design.dox).
   The raster is narrowed to the pixels the clip region covers, as
   PLRM 7.5.1 has every painting operation meet the region: the whole
   raster is rejected or accepted against the region's bounds first, so
   a glyph clear of the boundary costs the comparison and nothing more,
   and only a glyph the boundary crosses is walked run by run.
   inked is raised where a pixel is written and left alone otherwise,
   so the caller learns whether this raster reached the page: a glyph
   with no ink in it, and one the region keeps nothing of, leave the
   page as they found it. */
static
void _draw_bitmap(Xpost_Context *ctx,
                  Xpost_Object devdic,
                  Xpost_Object putpix,
                  const textstate *ts,
                  const unsigned char *buffer,
                  int rows,
                  int width,
                  int pitch,
                  char pixel_mode,
                  int xpos,
                  int ypos,
                  int ncomp,
                  Xpost_Object comp1,
                  Xpost_Object comp2,
                  Xpost_Object comp3,
                  Xpost_Object comp4,
                  int *inked)
{
    int i, j;
    const unsigned char *tmp;
    unsigned int pix;
    Xpost_Object exec_op;
    Xpost_Object glyphop;
    int i0 = 0, i1 = rows;
    int inside;
    /* the coverage a device that stores is handed, and whether any of
       it was written: a glyph the region keeps nothing of, and one with
       no ink in it, reach the page nowhere and are not written down */
    unsigned char *mask = NULL;
    int any = 0;

    /* The operator itself, not its name: a name pushed for execution is
       resolved against the dictionary stack at that moment, so a program
       that has defined /exec would supply the body instead. Held in a local
       because the glyph loop pushes it for every set pixel when putpix is a
       procedure rather than an operator. */
    exec_op = XPOST_OP(ctx, exec);
    XPOST_LOG_INFO("bitmap rows = %d, bitmap width = %d", rows, width);
    XPOST_LOG_INFO("bitmap pitch = %d", pitch);
    XPOST_LOG_INFO("bitmap pixel_mode = %d", pixel_mode);

    if (ts->clipkind != CLIP_ALL)
    {
        if (ts->cy0 - ypos > i0)
            i0 = ts->cy0 - ypos;
        if (ts->cy1 - ypos < i1)
            i1 = ts->cy1 - ypos;
    }
    /* the raster lies wholly within the region: every run below is the
       whole row, so the walk skips the run machinery altogether */
    inside = ts->clipkind == CLIP_ALL
          || (ts->clipkind == CLIP_BOX
           && i0 == 0 && i1 == rows
           && ts->cx0 <= xpos && ts->cx1 >= xpos + width);

    /* Where a device writes marks down, the coverage is gathered into a
       mask of the rows the region keeps rather than being paid out a
       call at a time. It is the whole of those rows and not the runs
       within them: a pixel the region drops is a pixel with no coverage,
       which is what the buffer already holds. The colour is not in it --
       one colour is in force for the glyph and it goes with the
       placement -- and neither is the position, so two placements of one
       glyph hand over the same bytes and the record holds one copy.
       A device whose space takes a colour no record holds, and a mask
       there is no memory for, both leave this at the route that paints. */
    glyphop = xpost_dict_get(ctx, devdic, name_dotrecordglyph);
    if (xpost_object_get_type(glyphop) == operatortype
        && (ncomp == 1 || ncomp == 3) && width > 0 && i1 > i0)
        mask = calloc((size_t)(i1 - i0), (size_t)width);

    for (i = i0; i < i1; i++)
    {
        int run = 0;      /* the band cursor, while walking runs */
        int j0 = 0, j1 = width;

        /* the pitch is signed: a raster whose rows run the other way
           steps backwards through its buffer */
        tmp = buffer + (ptrdiff_t)i * pitch;
        if (!inside)
        {
            if (ts->clipkind == CLIP_BOX)
            {
                if (ts->cx0 - xpos > j0)
                    j0 = ts->cx0 - xpos;
                if (ts->cx1 - xpos < j1)
                    j1 = ts->cx1 - xpos;
            }
            else if (ts->clipkind == CLIP_BANDS)
                run = _clip_band_row(ts, ypos + i);
        }
      next_run:
        if (!inside && ts->clipkind == CLIP_BANDS)
        {
            if (run >= ts->nbands || ts->bands[run].band != ypos + i)
                continue;
            j0 = ts->bands[run].lo - xpos;
            j1 = ts->bands[run].hi - xpos;
            run++;
            if (j0 < 0)
                j0 = 0;
            if (j1 > width)
                j1 = width;
        }
        for (j = j0; j < j1; j++)
        {
            int cov = -1;  /* -1 solid, 0 skip, else blend coverage */

            switch (pixel_mode)
            {
                case XPOST_FONT_PIXEL_MODE_MONO:
                    pix = (tmp[j / 8] >> (7 - (j % 8))) & 1;
                    cov = pix ? -1 : 0;
                    break;
                case XPOST_FONT_PIXEL_MODE_GRAY:
                    pix = tmp[j];
                    if (ts->blend)
                    {
                        /* the coverage the device asked for as many bits
                           of as it can tell apart: full coverage is the
                           whole pixel PutPix lays, none of it is no
                           pixel at all, and what is between goes to the
                           blend */
                        pix = _cov_bits((int)pix, ts->blend);
                        cov = pix == 255 ? -1 : (int)pix;
                    }
                    else
                        cov = pix >= 128 ? -1 : 0;
                    break;
                default:
                    XPOST_LOG_ERR("unsupported pixel_mode");
                    free(mask);
                    return;
            }
            if (cov)
            {
                *inked = 1;
                any = 1;
                if (mask)
                {
                    /* the whole of a pixel where the whole of it is
                       covered, and as much of one as is covered where
                       part of it is: the two the calls below make */
                    mask[(size_t)(i - i0) * (size_t)width + j]
                        = cov < 0 ? 255 : (unsigned char)cov;
                    continue;
                }
                switch (ncomp)
                {
                    case 1:
                        xpost_stack_push(ctx->lo, ctx->os, comp1);
                        break;
                    case 3:
                        xpost_stack_push(ctx->lo, ctx->os, comp1);
                        xpost_stack_push(ctx->lo, ctx->os, comp2);
                        xpost_stack_push(ctx->lo, ctx->os, comp3);
                        break;
                    case 4:
                        xpost_stack_push(ctx->lo, ctx->os, comp1);
                        xpost_stack_push(ctx->lo, ctx->os, comp2);
                        xpost_stack_push(ctx->lo, ctx->os, comp3);
                        xpost_stack_push(ctx->lo, ctx->os, comp4);
                        break;
                }
                if (cov > 0)
                    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(cov));
                xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(xpos + j));
                xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ypos + i));
                xpost_stack_push(ctx->lo, ctx->os, devdic);
                if (cov > 0)
                    xpost_stack_push(ctx->lo, ctx->es, ts->blendpix);
                else if (xpost_object_get_type(putpix) == operatortype)
                    xpost_stack_push(ctx->lo, ctx->es, putpix);
                else
                {
                    xpost_stack_push(ctx->lo, ctx->os, putpix);
                    xpost_stack_push(ctx->lo, ctx->es, exec_op);
                }
            }
        }
        if (!inside && ts->clipkind == CLIP_BANDS)
            goto next_run;
    }

    if (!mask)
        return;
    if (any)
    {
        size_t at;

        /* The coverage goes over as the glyph is rendered and the
           placement is left for the device to be told when the glyph is
           painted, which for a string is not the same order: the calls
           queued here are made from the top of the execution stack down,
           so the last character of a show reaches the device first. A
           record holds its marks in the order they were painted, so the
           mark belongs where the painting happens; the mask is the same
           mask whenever it is taken up, so it goes now.

           A device that could not take the mask is a device with no
           record left to hold it or no memory to hold it in. It is left
           unmarked either way: a record that could not hold something it
           was given refuses every replay of itself, so the page is
           refused rather than put out short of a glyph. */
        if (xpost_dev_record_takemask(ctx, devdic, mask,
                                      width, i1 - i0, &at) == 0)
        {
            switch (ncomp)
            {
                case 1:
                    xpost_stack_push(ctx->lo, ctx->os, comp1);
                    break;
                default:
                    xpost_stack_push(ctx->lo, ctx->os, comp1);
                    xpost_stack_push(ctx->lo, ctx->os, comp2);
                    xpost_stack_push(ctx->lo, ctx->os, comp3);
                    break;
            }
            xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)at));
            xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(xpos));
            xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ypos + i0));
            xpost_stack_push(ctx->lo, ctx->os, devdic);
            xpost_stack_push(ctx->lo, ctx->es, glyphop);
        }
    }
    free(mask);
}

/* Emit one glyph's outline into the pdfwrite device's content
   accumulator as filled path segments: "r g b rg", the contours as
   m/l/c/h operators, and a nonzero-winding fill. Coordinates arrive
   from the face in y-up pixels relative to the pen and are placed at
   the y-down device pen position, exactly where the bitmap path puts
   the rendered glyph. */
typedef struct glyphfrag
{
    Xpost_String_Buffer d;
    double px, py;
    int has;   /* any contour emitted */
    int oom;
    int svg;   /* emit SVG path commands instead of PDF operators */
} glyphfrag;

/* --- a glyph as an outline, for a device that wants one --------------
   A vector writer would rather have the curve than the coverage, so the
   face's outline is walked and turned into the writer's own path
   operators. The same walk serves charpath, which needs the outline as
   PostScript rather than as text. */

/* the outline walker aborts on a non-zero return, and oom records that the
   fragment is incomplete for the caller that emits it */
static int _frag_put(glyphfrag *f, const char *s, size_t n)
{
    int ret = xpost_strbuf_append(&f->d, s, n);

    if (ret)
        f->oom = 1;
    return ret;
}

/* A point into a vector device's stream, in the device's own
   coordinates: the glyph's origin plus the offset, with y taken the
   other way up because the page is. */
static int _frag_xy(glyphfrag *f, double x, double y)
{
    char t[64];
    int n;

    n = xpost_dev_pdf_fmt_num(t, f->px + x);
    t[n++] = ' ';
    n += xpost_dev_pdf_fmt_num(t + n, f->py - y);
    t[n++] = ' ';
    return _frag_put(f, t, n);
}

/* PDF and SVG spell the same commands differently: PDF postfixes the
   operator ("x y m"), SVG prefixes it ("M x y"). */
static int _frag_cmd_xy(glyphfrag *f, const char *pdfop, const char *svgop, double x, double y)
{
    if (f->svg)
        return _frag_put(f, svgop, 1) || _frag_xy(f, x, y);
    return _frag_xy(f, x, y) || _frag_put(f, pdfop, 2);
}

static int _frag_moveto(void *user, double x, double y)
{
    glyphfrag *f = user;
    f->has = 1;
    return _frag_cmd_xy(f, "m\n", "M", x, y);
}

static int _frag_lineto(void *user, double x, double y)
{
    glyphfrag *f = user;
    return _frag_cmd_xy(f, "l\n", "L", x, y);
}

static int _frag_curveto(void *user, double x1, double y1, double x2, double y2, double x3, double y3)
{
    glyphfrag *f = user;
    if (f->svg)
        return _frag_put(f, "C", 1)
            || _frag_xy(f, x1, y1) || _frag_xy(f, x2, y2) || _frag_xy(f, x3, y3);
    return _frag_xy(f, x1, y1) || _frag_xy(f, x2, y2) || _frag_xy(f, x3, y3)
        || _frag_put(f, "c\n", 2);
}

static int _frag_closepath(void *user)
{
    glyphfrag *f = user;
    if (f->svg)
        return _frag_put(f, "Z", 1);
    return _frag_put(f, "h\n", 2);
}

static
int _show_char_outline(Xpost_Context *ctx,
                       Xpost_Object devdic,
                       const textstate *ts,
                       void *face,
                       unsigned int glyph_index,
                       real xpos,
                       real ypos,
                       int ncomp,
                       Xpost_Object comp1,
                       Xpost_Object comp2,
                       Xpost_Object comp3,
                       Xpost_Object comp4,
                       long *advance_x,
                       long *advance_y,
                       int *inked)
{
    glyphfrag f;
    Xpost_Font_Outline_Sink sink;
    double r, g, b;
    char t[96];
    int n;

    memset(&f, 0, sizeof f);
    if (xpost_strbuf_init(&f.d, 256))
        f.oom = 1;
    f.px = xpos;
    f.py = ypos;

    /* the target syntax is the device's choice: PDF operators unless the
       device declares /VectorSyntax /svg */
    {
        Xpost_Object syn = xpost_dict_get(ctx, devdic, name_VectorSyntax);
        if (xpost_object_get_type(syn) == nametype)
        {
            Xpost_Object ss = xpost_name_get_string(ctx, syn);
            f.svg = ss.comp_.sz == 3
                 && memcmp(xpost_string_get_pointer(ctx, ss), "svg", 3) == 0;
        }
    }

    r = xpost_object_number(comp1);
    g = ncomp >= 3 ? xpost_object_number(comp2) : r;
    b = ncomp >= 3 ? xpost_object_number(comp3) : r;
    if (ts->sepindex >= 0 && !f.svg)
    {
        /* the fill colour is a separation registered with the device:
           paint in its /CS<i> resource space at the recorded tint */
        memcpy(t, "/CS", 3); n = 3;
        n += xpost_dev_pdf_fmt_num(t + n, (double)ts->sepindex);
        memcpy(t + n, " cs ", 4); n += 4;
        n += xpost_dev_pdf_fmt_num(t + n, ts->septint);
        memcpy(t + n, " scn\n", 5); n += 5;
    }
    else if (ncomp == 1 && !f.svg)
    {
        /* the paint's space is DeviceGray: the glyph fills in it */
        n = xpost_dev_pdf_fmt_num(t, r);
        memcpy(t + n, " g\n", 3);
        n += 3;
    }
    else if (ncomp == 4 && !f.svg)
    {
        /* the paint's space is DeviceCMYK: the glyph fills in it */
        n = xpost_dev_pdf_fmt_num(t, r);
        t[n++] = ' ';
        n += xpost_dev_pdf_fmt_num(t + n, g);
        t[n++] = ' ';
        n += xpost_dev_pdf_fmt_num(t + n, b);
        t[n++] = ' ';
        n += xpost_dev_pdf_fmt_num(t + n, xpost_object_number(comp4));
        memcpy(t + n, " k\n", 3);
        n += 3;
    }
    else if (f.svg)
    {
        memcpy(t, "<path fill=\"rgb(", 16); n = 16;
        n += xpost_dev_pdf_fmt_num(t + n, r * 100); t[n++] = '%'; t[n++] = ',';
        n += xpost_dev_pdf_fmt_num(t + n, g * 100); t[n++] = '%'; t[n++] = ',';
        n += xpost_dev_pdf_fmt_num(t + n, b * 100); t[n++] = '%';
        memcpy(t + n, ")\" d=\"", 6); n += 6;
    }
    else
    {
        n = xpost_dev_pdf_fmt_num(t, r);
        t[n++] = ' ';
        n += xpost_dev_pdf_fmt_num(t + n, g);
        t[n++] = ' ';
        n += xpost_dev_pdf_fmt_num(t + n, b);
        memcpy(t + n, " rg\n", 4);
        n += 4;
    }
    /* The SVG prefix opens the element the outline is the body of, so it
       goes into the fragment. The PDF colour is a state operator and is
       held back until the outline is known to be reaching the content:
       a glyph that decomposes to nothing writes nothing, and a colour
       recorded for a fragment that was never appended would leave the
       stream's record claiming bytes it does not carry. */
    if (f.svg)
        _frag_put(&f, t, n);

    sink.moveto = _frag_moveto;
    sink.lineto = _frag_lineto;
    sink.curveto = _frag_curveto;
    sink.closepath = _frag_closepath;
    sink.user = &f;
    if (!xpost_font_face_glyph_outline(face, glyph_index, &sink, advance_x, advance_y))
    {
        xpost_strbuf_free(&f.d);
        return 0;
    }
    /* a blank glyph (e.g. space) decomposes to nothing: advance only */
    if (f.has && !f.oom)
    {
        if (f.svg)
            _frag_put(&f, "\"/>\n", 4);   /* glyphs fill nonzero: SVG's default rule */
        else
            _frag_put(&f, "f\n", 2);
        if (!f.oom)
        {
            /* One paint follows another in the content stream whatever
               produced it, so a glyph states its fill colour only where
               the stream does not carry it already -- a page of text is
               otherwise a colour operator per glyph, in the colour the
               glyph before it was painted in. */
            if (!f.svg && xpost_dev_pdf_state(ctx, devdic,
                                              XPOST_PDF_GS_FILL, t, (size_t)n)
                       && xpost_dev_pdf_append(ctx, devdic, t, (size_t)n))
            {
                xpost_strbuf_free(&f.d);
                return 0;
            }
            if (xpost_dev_pdf_append(ctx, devdic, f.d.s, f.d.len))
            {
                /* the glyph's content did not reach the page */
                xpost_strbuf_free(&f.d);
                return 0;
            }
        }
        /* the contours are in the device's content, which is what this
           device paints with; the region they are seen through is the
           device's own to record beside them */
        if (!f.oom)
            *inked = 1;
    }
    xpost_strbuf_free(&f.d);
    return 1;
}

/* --- showing a string ------------------------------------------------
   The operators the language exposes, and the one path they all reach:
   step the string, find each glyph, paint it, move the pen. What the
   variants add is where the pen goes between glyphs. */


static
int _show_glyph(Xpost_Context *ctx,
                Xpost_Object devdic,
                Xpost_Object putpix,
                struct fontdata data,
                const textstate *ts,
                real *xpos,
                real *ypos,
                unsigned int glyph_index,
                Xpost_Object glyphkey,
                int ncomp,
                Xpost_Object comp1,
                Xpost_Object comp2,
                Xpost_Object comp3,
                Xpost_Object comp4,
                int *inked)
{
    unsigned char *buffer;
    int rows;
    int width;
    int pitch;
    char pixel_mode;
    int left;
    int top;
    long advance_x;
    long advance_y;
    long bx0, by0, bx1, by1;
    /* A /Metrics entry may put this glyph somewhere other than where the
       face draws it (PLRM 5.9.2). Read before the glyph is served: it
       loads the glyph to find the face's own sidebearing, and each of
       the three routes below hands back something a later load replaces.
       Zero where no entry names one, and the glyph then stands where the
       face puts it. */
    long sbx = 0, sby = 0;

    _metrics_sidebearing(ctx, ts, glyphkey, data.face, glyph_index,
                         &sbx, &sby);

    if (ts->vector)
    {
        if (!_show_char_outline(ctx, devdic, ts, data.face, glyph_index,
                                *xpos + sbx / 65536.0, *ypos - sby / 65536.0,
                                ncomp, comp1, comp2, comp3, comp4,
                                &advance_x, &advance_y, inked))
            return 0;
    }
    else if (ts->extents
        && xpost_font_face_glyph_extents(data.face, glyph_index,
                                         &bx0, &by0, &bx1, &by1,
                                         &advance_x, &advance_y))
    {
        /* an extent-tracking device needs no glyph rasterization (whose
           cost grows with the square of the resolution): the glyph
           contributes its ink box through the device's FillRect. The
           box is 26.6 glyph space, y-up around the pen; the device is
           y-down. An empty box (a space) advances only, and so does one
           the clip region keeps nothing of: what such a glyph leaves is
           what a fill of its outline would leave there, which is
           nothing. A glyph with no outline takes the rendering path
           below instead. */
        double sdx = sbx / 65536.0;
        double sdy = sby / 65536.0;

        if (bx1 > bx0 && by1 > by0
         && _clip_meets(ts, _clip_floor(*xpos + sdx + bx0 / 64.0),
                            _clip_floor(*ypos - sdy - by1 / 64.0),
                            _clip_ceil(*xpos + sdx + bx1 / 64.0),
                            _clip_ceil(*ypos - sdy - by0 / 64.0)))
        {
            *inked = 1;
            switch (ncomp)
            {
                case 4:
                    xpost_stack_push(ctx->lo, ctx->os, comp1);
                    xpost_stack_push(ctx->lo, ctx->os, comp2);
                    xpost_stack_push(ctx->lo, ctx->os, comp3);
                    xpost_stack_push(ctx->lo, ctx->os, comp4);
                    break;
                case 3:
                    xpost_stack_push(ctx->lo, ctx->os, comp1);
                    xpost_stack_push(ctx->lo, ctx->os, comp2);
                    xpost_stack_push(ctx->lo, ctx->os, comp3);
                    break;
                default:
                    xpost_stack_push(ctx->lo, ctx->os, comp1);
                    break;
            }
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)(*xpos + sdx + bx0 / 64.0)));
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)(*ypos - sdy - by1 / 64.0)));
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)((bx1 - bx0) / 64.0)));
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)((by1 - by0) / 64.0)));
            xpost_stack_push(ctx->lo, ctx->os, devdic);
            if (xpost_object_get_type(ts->fillrect) == operatortype)
                xpost_stack_push(ctx->lo, ctx->es, ts->fillrect);
            else
            {
                xpost_stack_push(ctx->lo, ctx->os, ts->fillrect);
                xpost_stack_push(ctx->lo, ctx->es,
                                 XPOST_OP(ctx, exec));
            }
        }
    }
    else
    {
        if (!xpost_font_face_glyph_render(data.face, glyph_index))
            return 0;
        xpost_font_face_glyph_buffer_get(data.face,
                                         &buffer, &rows, &width, &pitch, &pixel_mode,
                                         &left, &top, &advance_x, &advance_y);
        /* the pen rides at fractional device positions but the glyph
           bitmap sits on the pixel grid: place it at the nearest
           pixel, not the floor, so a pen an epsilon shy of a pixel
           boundary (the linear advance's 16.16 quantization) lands
           where exact arithmetic would put it */
        {
            double px = floor(*xpos + sbx / 65536.0 + left + 0.5);
            double py = floor(*ypos - sby / 65536.0 - top + 0.5);
            /* The pen can be driven far off the page, or to a non-finite
               position by an overflowing advance. Converting a double past
               int range is undefined, and _draw_bitmap's clip arithmetic
               subtracts the origin, which overflows near the int extremes;
               the glyph is off the page either way. Draw it only where the
               origin is a representable pixel coordinate with room for that
               arithmetic -- any real page sits far inside this -- and drop
               it otherwise. */
            if (isfinite(px) && isfinite(py)
                && px > -1.0e9 && px < 1.0e9 && py > -1.0e9 && py < 1.0e9)
                _draw_bitmap(ctx, devdic, putpix, ts,
                             buffer, rows, width, pitch, pixel_mode,
                             (int)px, (int)py,
                             ncomp, comp1, comp2, comp3, comp4, inked);
        }
    }
    /* a /Metrics entry for this glyph overrides the face's advance */
    _metrics_advance(ctx, ts, glyphkey, &advance_x, &advance_y);
    /* the face transform leaves the advance in y-up glyph space; the
       pen advances in y-down device space, keeping the fractional part
       (truncating each glyph's advance drifts the line's length) */
    *xpos += (real)(advance_x / 65536.0);
    *ypos -= (real)(advance_y / 65536.0);
    return 1;
}

/* Paint the glyph the encoding selects for one character code and
   advance the pen over it. Answers 0 when the face would not give the
   glyph up: nothing reached the page and the pen stands where it was. */
static
int _show_char(Xpost_Context *ctx,
               Xpost_Object devdic,
               Xpost_Object putpix,
               struct fontdata data,
               const textstate *ts,
               real *xpos,
               real *ypos,
               unsigned int ch,
               int ncomp,
               Xpost_Object comp1,
               Xpost_Object comp2,
               Xpost_Object comp3,
               Xpost_Object comp4,
               int *inked)
{
    /* show does not kern: pair adjustment in PostScript is the
       program's business (kshow, ashow); the advance is the glyph
       widths alone */
    unsigned int glyph_index = _glyph_index_for_char(ctx, ts->encoding,
                                                     ts->charstrings,
                                                     data.face, ch);
    return _show_glyph(ctx, devdic, putpix, data, ts, xpos, ypos,
                       glyph_index, _encoded_name(ctx, ts->encoding, ch),
                       ncomp, comp1, comp2, comp3, comp4, inked);
}

/* The pen position, read out of the packed path string in the graphics
   state. */
static
int _get_current_point (Xpost_Context *ctx,
                        Xpost_Object gs,
                        real *xpos,
                        real *ypos)
{
    Xpost_Object path;
    char *p;
    unsigned int used, last;
    real co[6];
    int n;

    /* get the current pen position from the packed path string
       (device coordinates; layout as described in xpost_op_path.c) */
    path = xpost_dict_get(ctx, gs, name_currpath);
    if (xpost_object_get_type(path) != stringtype)
        return nocurrentpoint;
    /* currpath sits in a program-reachable dictionary, so its header may be
       forged; bound the extent and the last-element offset against the
       string's own allocation before dereferencing them */
    {
        Xpost_Memory_File *mem = xpost_context_select_memory(ctx, path);
        unsigned int ent = xpost_object_get_ent(path);
        unsigned int entsz = mem->table.tab[ent].sz;
        unsigned int avail = path.comp_.off < entsz ? entsz - path.comp_.off : 0;

        if (avail < 16)
            return nocurrentpoint;
        p = xpost_string_get_pointer(ctx, path);
        memcpy(&used, p, sizeof used);
        if (used <= 16 || used > avail)
            return nocurrentpoint;
        memcpy(&last, p + 8, sizeof last);
        n = last < used && p[last] == 2 ? 6 : 2; /* curve carries three points */
        if (last >= used || last + 1 + n * sizeof(real) > used)
            return nocurrentpoint;
        memcpy(co, p + last + 1, n * sizeof(real));
        *xpos = co[n - 2];
        *ypos = co[n - 1];
    }
    XPOST_LOG_INFO("currentpoint: %f %f", *xpos, *ypos);

    return 0;
}

/* Build the procedure a show operator leaves on the execution stack to
   run once its glyphs are painted: it restores the current point in
   user space and flushes the page. Elements 0 and 1 hold the position,
   rewritten by _show_finalize_pos with the point the operator ended
   at.

   The procedure runs after the show operator has returned, with the
   program's own dictionaries on the dictionary stack, so it holds what
   it means to call rather than the names of it: an executable name here
   would be resolved then, and a program that had defined /moveto would
   have its own procedure called to finish the show. itransform and
   moveto are operators and go in as operator objects; flushpage is a
   procedure, so its value is taken from systemdict and run by the exec
   that follows it. */
static
int _show_finalize_cons(Xpost_Context *ctx,
                        real xpos,
                        real ypos,
                        Xpost_Object *out)
{
    Xpost_Object f = xpost_object_cvx(xpost_array_cons(ctx, 6));
    Xpost_Object flushpage;
    int ret;

    if (xpost_object_get_type(f) == nulltype)
        return VMerror;
    flushpage = xpost_dict_get(ctx,
                               xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0),
                               name_flushpage);
    if (xpost_object_get_type(flushpage) == invalidtype
     || xpost_object_get_type(flushpage) == nulltype)
        return undefined;
    if ((ret = xpost_array_put(ctx, f, 0, xpost_real_cons(xpos))) != 0
     || (ret = xpost_array_put(ctx, f, 1, xpost_real_cons(ypos))) != 0
     || (ret = xpost_array_put(ctx, f, 2,
             XPOST_OP(ctx, itransform))) != 0
     || (ret = xpost_array_put(ctx, f, 3,
             XPOST_OP(ctx, moveto))) != 0
     || (ret = xpost_array_put(ctx, f, 4, flushpage)) != 0
     || (ret = xpost_array_put(ctx, f, 5,
             XPOST_OP(ctx, exec))) != 0)
        return ret;
    *out = f;
    return 0;
}

/* Record the point the show ended at in the finalize procedure. */
static
int _show_finalize_pos(Xpost_Context *ctx,
                       Xpost_Object f,
                       real xpos,
                       real ypos)
{
    int ret = xpost_array_put(ctx, f, 0, xpost_real_cons(xpos));

    if (ret)
        return ret;
    return xpost_array_put(ctx, f, 1, xpost_real_cons(ypos));
}

/* Paints a string at the current point and leaves the point past it.
   The operators that follow -- those spacing on a nominated character,
   those adding a displacement per character, and glyphshow -- differ
   only in what they add between characters and in how the character is
   named; each reaches the same per-character path. */
static
int _show(Xpost_Context *ctx,
          Xpost_Object str)
{
    Xpost_Object gs;
    Xpost_Object fontdict;
    struct fontdata data;
    char *cstr;
    real xpos, ypos;
    char *ch;
    char *chend;
    Xpost_Object devdic;
    Xpost_Object putpix;
    textstate ts;
    int ncomp;
    Xpost_Object comp[4];
    Xpost_Object finalize;
    int painted = 1;
    int inked = 0;
    int ret;


    /* load the graphicsdict, current graphics state, and current font */
    gs = _gstate(ctx);
    fontdict = xpost_dict_get(ctx, gs, name_currfont);
    if (xpost_object_get_type(fontdict) == invalidtype)
        return invalidfont;
    XPOST_LOG_INFO("loaded graphicsdict, graphics state, and current font");

    /* load the device and PutPix member function */
    devdic = xpost_dict_get(ctx, gs, name_device);
    putpix = xpost_dict_get(ctx, devdic, name_PutPix);
    XPOST_LOG_INFO("loaded DEVICE and PutPix");
    ts = _text_state_get(ctx, gs, fontdict, devdic);

    /* get the font data from the font dict */
    ret = _font_data_current(ctx, gs, fontdict, &data);
    if (ret)
        return ret;
    XPOST_LOG_INFO("loaded font data from dict");

    /* the string's characters, copied out of VM so that a device method
       reached through a glyph -- which is the program's own procedure and
       may write the string -- cannot move the text out from under the
       walk. The walk runs to the count the string carries: every byte of
       a string is one of its characters, a nul among them (PLRM 3.3.7),
       and the terminator the copy ends with is there for the C
       interfaces a glyph's name goes through */
    cstr = xpost_string_allocate_cstring(ctx, str);
    if (!cstr)
        return VMerror;
    chend = cstr + str.comp_.sz;

    ret = _get_current_point(ctx, gs, &xpos, &ypos);
    if (ret){
        free(cstr);
        return ret;
    }

    if (_device_color(ctx, gs, devdic, &ncomp, comp))
    {
        free(cstr);
        return unregistered;
    }
    XPOST_LOG_INFO("ncomp = %d", ncomp);

    ret = _show_finalize_cons(ctx, xpos, ypos, &finalize);
    if (ret)
    {
        free(cstr);
        return ret;
    }
    xpost_stack_push(ctx->lo, ctx->es, finalize);

    /* render text in char *cstr  with font data  at pen position xpos ypos */
    for (ch = cstr; ch < chend; ch++) {
        if (!_show_char(ctx, devdic, putpix, data, &ts, &xpos, &ypos, (unsigned char)*ch,
                ncomp, comp[0], comp[1], comp[2], comp[3], &inked))
        {
            painted = 0;
            break;
        }
    }

    /* update current position in the graphics state */
    ret = _show_finalize_pos(ctx, finalize, xpos, ypos);
    /* the ink the glyphs left is a mark on the page; a string that
       painted none -- an empty one, blanks, or text the clip keeps
       nothing of -- leaves the page as it found it */
    if (!ret && inked)
        ret = _page_mark(ctx);
    /* the glyphs the string asked for are the font's to supply, and one
       it will not is that font failing the operator (PLRM 8.2 show) */
    if (!ret && !painted)
        ret = invalidfont;

    free(cstr);
    return ret;
}

/* Paint the single glyph a caller selects directly, bypassing the
   encoding, and advance the current point by the glyph's width. The
   PostScript-level glyphshow sends Type 3 fonts to their build
   procedures instead of here.

   The glyph is selected by name, as glyphshow selects one, or by index,
   as a CMap walk selects one. The key is what the font's /Metrics
   dictionary would name this glyph's metrics under -- the name in a base
   font (PLRM 5.9.2), the CID in a CIDFont (PLRM 5.11.3) -- which the
   index route cannot derive from the index and is therefore told. */
static
int _glyphshow_common(Xpost_Context *ctx,
                      Xpost_Object gname,
                      int byname,
                      unsigned int gid,
                      Xpost_Object glyphkey)
{
    Xpost_Object gs;
    Xpost_Object fontdict;
    struct fontdata data;
    real xpos, ypos;
    Xpost_Object devdic;
    Xpost_Object putpix;
    textstate ts;
    int ncomp;
    Xpost_Object comp[4];
    Xpost_Object finalize;
    unsigned int glyph_index;
    int painted;
    int inked = 0;
    int ret;

    gs = _gstate(ctx);
    fontdict = xpost_dict_get(ctx, gs, name_currfont);
    if (xpost_object_get_type(fontdict) == invalidtype)
        return invalidfont;

    devdic = xpost_dict_get(ctx, gs, name_device);
    putpix = xpost_dict_get(ctx, devdic, name_PutPix);
    ts = _text_state_get(ctx, gs, fontdict, devdic);

    ret = _font_data_current(ctx, gs, fontdict, &data);
    if (ret)
        return ret;

    ret = _get_current_point(ctx, gs, &xpos, &ypos);
    if (ret)
        return ret;

    if (_device_color(ctx, gs, devdic, &ncomp, comp))
        return unregistered;

    ret = _show_finalize_cons(ctx, xpos, ypos, &finalize);
    if (ret)
        return ret;
    xpost_stack_push(ctx->lo, ctx->es, finalize);

    glyph_index = byname
        ? _glyph_index_for_name(ctx, ts.charstrings, data.face, gname)
        : gid;
    painted = _show_glyph(ctx, devdic, putpix, data, &ts, &xpos, &ypos,
                          glyph_index, glyphkey,
                          ncomp, comp[0], comp[1], comp[2], comp[3], &inked);

    /* the point the operator reached is the point it leaves behind,
       whether or not the glyph was painted from it */
    ret = _show_finalize_pos(ctx, finalize, xpos, ypos);
    if (ret)
        return ret;
    /* the ink the glyph left is a mark on the page; a glyph with no
       ink in it, and one the clip keeps nothing of, leave the page as
       they found it */
    if (inked)
    {
        ret = _page_mark(ctx);
        if (ret)
            return ret;
    }
    /* A glyph the face would not give up is nothing on the page and no
       advance over it. Selecting by index reaches the CIDFont's glyphs,
       and past them is out of range; selecting by name asks the font for
       a glyph it turns out not to be able to supply (PLRM 8.2). */
    if (!painted)
        return byname ? invalidfont : rangecheck;
    return 0;
}

/* Paints one glyph named directly, rather than through the font's
   encoding. A base font's metrics are named for the glyph, so the name
   is the key as well as the selector. */
static
int _glyphshow(Xpost_Context *ctx,
               Xpost_Object gname)
{
    return _glyphshow_common(ctx, gname, 1, 0, gname);
}

/* cid index  .glyphshowidx  -
   paint the single glyph at the given index in the current font's
   face and advance the current point by its width; the composite
   font machinery reaches glyphs by index once a CMap has resolved
   the character code to a CID and the CIDMap has resolved the CID to
   an index. The CID comes with the index because it is what the
   descendant's /Metrics dictionary keys this glyph's metrics under
   (PLRM 5.11.3), and nothing downstream of the walk can recover it. */
static
int _glyphshowidx(Xpost_Context *ctx,
                  Xpost_Object cid,
                  Xpost_Object gidx)
{
    /* The index is carried on to the face as an unsigned int, so one
       that does not fit is refused rather than narrowed. A wide build's
       integer is wider than that field: narrowed, an index past the top
       lands back inside the range and names some other glyph, which is
       a glyph painted for a character that does not have one and no
       error to say so. */
    if (gidx.int_.val < 0)
        return rangecheck;
    if ((integer)(unsigned int)gidx.int_.val != gidx.int_.val)
        return rangecheck;
    return _glyphshow_common(ctx, null, 0,
                             (unsigned int)gidx.int_.val, cid);
}

/* --- building a font from a program the job supplied -----------------
   definefont with a font program rather than a name. A CIDFontType 0 or
   2, or a Type 1, is assembled into what the font library can open --
   which for a CID font means writing out a container the library
   recognises, since it has no other way in. */

/* big-endian field readers over the assembled font program */
static unsigned int _sfnt_u16(const unsigned char *p)
{
    return p[0] << 8 | p[1];
}
static unsigned int _sfnt_u32(const unsigned char *p)
{
    return (unsigned int)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
static void _sfnt_put16(unsigned char *p, unsigned int v)
{
    p[0] = (v >> 8) & 0xff; p[1] = v & 0xff;
}
static void _sfnt_put32(unsigned char *p, unsigned int v)
{
    p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff; p[3] = v & 0xff;
}


/* ciddict  .loadcidfont0  -
   assemble a working face for a CIDFontType 0 dictionary. The glyph
   programs arrived through StartData as the /GlyphData binary block
   -- the CIDMap, then Type 1 charstrings the FDArray's private
   dictionaries describe. The dictionary is written back out as a
   CIDFont resource file around that block and opened as a memory
   face, which serves glyphs directly by CID. */

static int
_cid_emit_num(Xpost_Context *ctx, Xpost_String_Buffer *b, Xpost_Object v)
{
    (void)ctx;
    /* The interpreter's integer is as wide as the build makes it, so the
       decimal is written through the widest integer the language has and
       every digit of the value reaches the font program at either
       width. */
    if (xpost_object_get_type(v) == integertype)
        return xpost_strbuf_appendf(b, "%lld", (long long)v.int_.val);
    if (xpost_object_get_type(v) == realtype)
        return xpost_strbuf_appendf(b, "%g", v.real_.val);
    if (xpost_object_get_type(v) == booleantype)
        return xpost_strbuf_appendf(b, "%s", v.int_.val ? "true" : "false");
    return invalidfont;
}

/* emit "/key value def" for one dictionary entry, an array value as a
   bracketed list. A key the dictionary does not hold emits nothing. */
static int
_cid_emit_entry(Xpost_Context *ctx, Xpost_String_Buffer *b,
                Xpost_Object d, const char *key)
{
    Xpost_Object v = xpost_dict_get(ctx, d, xpost_name_cons(ctx, key));
    word i;
    int ret;

    if (xpost_object_get_type(v) == invalidtype)
        return 0;
    if (xpost_object_get_type(v) == arraytype)
    {
        ret = xpost_strbuf_appendf(b, "/%s [", key);
        if (ret)
            return ret;
        for (i = 0; i < v.comp_.sz; i++)
        {
            if (i)
            {
                ret = xpost_strbuf_append(b, " ", 1);
                if (ret)
                    return ret;
            }
            ret = _cid_emit_num(ctx, b, xpost_array_get(ctx, v, i));
            if (ret)
                return ret;
        }
        return xpost_strbuf_appendf(b, "] def\n");
    }
    ret = xpost_strbuf_appendf(b, "/%s ", key);
    if (ret)
        return ret;
    ret = _cid_emit_num(ctx, b, v);
    if (ret)
        return ret;
    return xpost_strbuf_appendf(b, " def\n");
}

static const char *_cid_private_keys[] = {
    "lenIV", "BlueValues", "OtherBlues", "FamilyBlues", "FamilyOtherBlues",
    "BlueScale", "BlueShift", "BlueFuzz", "StdHW", "StdVW",
    "StemSnapH", "StemSnapV", "LanguageGroup", "ForceBold", "RndStemUp",
    "SubrMapOffset", "SDBytes", "SubrCount",
};

/* Builds a working face out of a CIDFontType 0 dictionary, assembling
   the binary the rendering library will read from the pieces the
   dictionary carries. */
static
int _loadcidfont0(Xpost_Context *ctx,
                  Xpost_Object fontdict)
{
    Xpost_Object gdata, fdarray;
    struct fontdata data;
    Xpost_String_Buffer buf;
    unsigned char *whole;
    size_t glen, gpos, wlen;
    int i;
    unsigned int k;
    int ret;

    gdata = xpost_dict_get(ctx, fontdict, name_GlyphData);
    if (xpost_object_get_type(gdata) == stringtype)
        glen = gdata.comp_.sz;
    else if (xpost_object_get_type(gdata) == arraytype)
    {
        glen = 0;
        /* the emitted program names this index in its own text, so the
           walk counts in the signed type the emitter takes and each
           array's element count is widened into it to be compared */
        for (i = 0; i < (integer)gdata.comp_.sz; i++)
        {
            Xpost_Object s = xpost_array_get(ctx, gdata, i);
            if (xpost_object_get_type(s) != stringtype)
                return invalidfont;
            glen += s.comp_.sz;
        }
    }
    else
        return invalidfont;
    fdarray = xpost_dict_get(ctx, fontdict, name_FDArray);
    if (xpost_object_get_type(fdarray) != arraytype)
        return invalidfont;

    if (xpost_strbuf_init(&buf, 8192))
        return VMerror;
    if (xpost_strbuf_appendf(&buf,
        "%%!PS-Adobe-3.0 Resource-CIDFont\n"
        "%%%%DocumentNeededResources: ProcSet (CIDInit)\n"
        "%%%%IncludeResource: ProcSet (CIDInit)\n"
        "/CIDInit /ProcSet findresource begin\n"
        "20 dict begin\n"
        "/CIDFontName /X def\n"
        "/CIDFontVersion 1 def\n"
        "/CIDFontType 0 def\n"
        "/CIDSystemInfo 3 dict dup begin\n"
        "  /Registry (Adobe) def\n"
        "  /Ordering (Identity) def\n"
        "  /Supplement 0 def\n"
        "end def\n")) goto fail;
    if (_cid_emit_entry(ctx, &buf, fontdict, "FontMatrix")) goto fail;
    if (_cid_emit_entry(ctx, &buf, fontdict, "FontBBox")) goto fail;
    if (_cid_emit_entry(ctx, &buf, fontdict, "CIDCount")) goto fail;
    if (_cid_emit_entry(ctx, &buf, fontdict, "FDBytes")) goto fail;
    if (_cid_emit_entry(ctx, &buf, fontdict, "GDBytes")) goto fail;
    if (_cid_emit_entry(ctx, &buf, fontdict, "CIDMapOffset")) goto fail;
    if (xpost_strbuf_appendf(&buf, "/FDArray %d array\n", fdarray.comp_.sz))
        goto fail;
    for (i = 0; i < (integer)fdarray.comp_.sz; i++)
    {
        Xpost_Object fd = xpost_array_get(ctx, fdarray, i);
        Xpost_Object priv;

        Xpost_Object topfm, fdfm;
        double m[6] = { 0.001, 0, 0, 0.001, 0, 0 };

        if (xpost_object_get_type(fd) != dicttype)
            goto fail2;
        if (xpost_strbuf_appendf(&buf,
            "%%ADOBeginFontDict\n"
            "dup %d 10 dict begin\n/FontType 1 def\n", i)) goto fail;
        /* the face carries one matrix per dictionary: the product of
           the font's matrix and the dictionary's own, so the glyph
           space the charstrings draw in reaches CID font space the
           way the two-level dictionary said it should */
        topfm = xpost_dict_get(ctx, fontdict, name_FontMatrix);
        fdfm = xpost_dict_get(ctx, fd, name_FontMatrix);
        if (xpost_object_get_type(topfm) == arraytype && topfm.comp_.sz == 6
         && xpost_object_get_type(fdfm) == arraytype && fdfm.comp_.sz == 6)
        {
            double a[6], b[6];
            int j;

            for (j = 0; j < 6; j++)
            {
                Xpost_Object v = xpost_array_get(ctx, fdfm, j);
                a[j] = xpost_object_number(v);
                v = xpost_array_get(ctx, topfm, j);
                b[j] = xpost_object_number(v);
            }
            m[0] = a[0]*b[0] + a[1]*b[2];
            m[1] = a[0]*b[1] + a[1]*b[3];
            m[2] = a[2]*b[0] + a[3]*b[2];
            m[3] = a[2]*b[1] + a[3]*b[3];
            m[4] = a[4]*b[0] + a[5]*b[2] + b[4];
            m[5] = a[4]*b[1] + a[5]*b[3] + b[5];
        }
        if (xpost_strbuf_appendf(&buf,
            "/FontMatrix [%g %g %g %g %g %g] def\n",
            m[0], m[1], m[2], m[3], m[4], m[5])) goto fail;
        if (xpost_strbuf_appendf(&buf, "/PaintType 0 def\n/Private 32 dict begin\n"))
            goto fail;
        priv = xpost_dict_get(ctx, fd, name_Private);
        if (xpost_object_get_type(priv) == dicttype)
            for (k = 0; k < sizeof _cid_private_keys / sizeof *_cid_private_keys; k++)
                if (_cid_emit_entry(ctx, &buf, priv,
                                    _cid_private_keys[k])) goto fail;
        if (xpost_strbuf_appendf(&buf,
            "currentdict end def\ncurrentdict end put\n"
            "%%ADOEndFontDict\n")) goto fail;
    }
    if (xpost_strbuf_appendf(&buf, "def\n(Binary) %lu StartData ",
                             (unsigned long)glen)) goto fail;

    wlen = buf.len + glen;
    whole = malloc(wlen);
    if (!whole)
        goto fail2;
    memcpy(whole, buf.s, buf.len);
    gpos = buf.len;
    if (xpost_object_get_type(gdata) == stringtype)
    {
        memcpy(whole + gpos, xpost_string_get_pointer(ctx, gdata), glen);
    }
    else
    {
        for (i = 0; i < (integer)gdata.comp_.sz; i++)
        {
            Xpost_Object s = xpost_array_get(ctx, gdata, i);
            memcpy(whole + gpos, xpost_string_get_pointer(ctx, s), s.comp_.sz);
            gpos += s.comp_.sz;
        }
    }
    xpost_strbuf_free(&buf);

    data.face = xpost_font_face_new_from_memory(whole, wlen);
    if (data.face == NULL)
    {
        free(whole);
        return invalidfont;
    }

    ret = _font_bbox_declare(ctx, fontdict, data.face, 1000.0);
    if (ret)
        goto facefail;

    ret = _font_data_set(ctx, fontdict, data.face, 1);
    if (ret)
        goto facefail;
    return 0;

    /* the face reads the program where it lies and no block names
       either yet, so this call is the only thing that can give them
       back; the face takes its program with it */
facefail:
    xpost_font_face_free(data.face);
    return ret;
fail:
fail2:
    xpost_strbuf_free(&buf);
    return invalidfont;
}


/* fontdict charstrings-flat subrs  .loadfont1  -
   assemble a working face for a Type 1 font defined by an embedded
   program. The interpreted dictionary is written back out as a font
   program -- cleartext header, then an eexec section carrying the
   private dictionary, the subroutines and the charstrings, whose
   own charstring-level encryption the strings still carry -- and
   opened as a memory face. The charstrings arrive flattened as
   name, string pairs, since only the interpreter can walk its
   dictionaries. */

static void
_t1_encrypt(unsigned char *data, size_t n)
{
    unsigned short r = 55665;
    size_t i;

    for (i = 0; i < n; i++)
    {
        unsigned char p = data[i];
        unsigned char c = p ^ (r >> 8);

        data[i] = c;
        r = (unsigned short)((unsigned int)(c + r) * 52845u + 22719u);
    }
}

/* Appends a string's bytes to a font program under construction,
   unencrypted, exactly as they stand. */
static int
_t1_emit_bin(Xpost_Context *ctx, Xpost_String_Buffer *b, Xpost_Object s)
{
    return xpost_strbuf_append(b, xpost_string_get_pointer(ctx, s),
                               s.comp_.sz);
}

/* Builds a working face out of a Type 1 font dictionary by
   reassembling the font program the dictionary was made from: the
   cleartext header, then the encrypted section holding the charstrings
   and subroutines. */
static
int _loadfont1(Xpost_Context *ctx,
               Xpost_Object fontdict,
               Xpost_Object csflat)
{
    Xpost_Object priv, subrs;
    struct fontdata data;
    Xpost_String_Buffer hdr, sec;
    unsigned char *whole;
    size_t wlen;
    int i;
    unsigned int k;
    int ret;

    if (xpost_object_get_type(csflat) != arraytype)
        return invalidfont;
    priv = xpost_dict_get(ctx, fontdict, name_Private);
    if (xpost_object_get_type(priv) != dicttype)
        return invalidfont;
    /* The subroutine array lives in the Private dictionary, which a Type 1
       font seals no-access. Read it here in C, where the access attribute does
       not apply, rather than from the PostScript loader, where it would forbid
       the read. An absent or non-array Subrs means no subroutines. */
    subrs = xpost_dict_get(ctx, priv, name_Subrs);

    if (xpost_strbuf_init(&hdr, 2048))
        return VMerror;
    if (xpost_strbuf_appendf(&hdr,
        "%%!PS-AdobeFont-1.0: X 001.001\n"
        "11 dict begin\n"
        "/FontName /X def\n"
        "/FontType 1 def\n"
        "/PaintType 0 def\n")) goto failh;
    if (_cid_emit_entry(ctx, &hdr, fontdict, "FontMatrix")) goto failh;
    if (_cid_emit_entry(ctx, &hdr, fontdict, "FontBBox")) goto failh;
    if (xpost_strbuf_appendf(&hdr,
        "/Encoding StandardEncoding def\n"
        "currentdict end\n"
        "currentfile eexec\n")) goto failh;

    if (xpost_strbuf_init(&sec, 16384))
        goto failh;
    /* four salt bytes ahead of the program proper */
    if (xpost_strbuf_appendf(&sec, "XPT1"
        "dup /Private 16 dict dup begin\n"
        "/RD {string currentfile exch readstring pop} executeonly def\n"
        "/ND {noaccess def} executeonly def\n"
        "/NP {noaccess put} executeonly def\n"
        "/password 5839 def\n"
        "/MinFeature {16 16} def\n")) goto fails;
    for (k = 0; k < sizeof _cid_private_keys / sizeof *_cid_private_keys; k++)
        if (_cid_emit_entry(ctx, &sec, priv,
                            _cid_private_keys[k])) goto fails;
    if (xpost_object_get_type(subrs) == arraytype && subrs.comp_.sz > 0)
    {
        if (xpost_strbuf_appendf(&sec, "/Subrs %d array\n", subrs.comp_.sz))
            goto fails;
        for (i = 0; i < (integer)subrs.comp_.sz; i++)
        {
            Xpost_Object s = xpost_array_get(ctx, subrs, i);

            /* A Type 1 font may over-allocate its Subrs array and leave
               slots unfilled (null): CMR10 declares 38 and fills only 15.
               Such a slot holds a subroutine the font never wrote, and no
               charstring of that font calls one it never wrote. Emit a
               minimal charstring-encrypted "return" for it, so the array
               is the length the font declared and a call on an unfilled
               slot does nothing -- rather than refusing a font whose
               glyphs are all present over a slot none of them reaches, or
               leaving a hole in the array for freetype to fault on. */
            if (xpost_object_get_type(s) != stringtype)
            {
                unsigned char cs[5];
                unsigned short rr = 4330;
                int j;

                cs[0] = cs[1] = cs[2] = cs[3] = 0; /* lenIV skip bytes */
                cs[4] = 11;                        /* charstring: return */
                for (j = 0; j < 5; j++)
                {
                    unsigned char c = (unsigned char)(cs[j] ^ (rr >> 8));
                    cs[j] = c;
                    rr = (unsigned short)(((unsigned int)(c + rr)) * 52845u + 22719u);
                }
                if (xpost_strbuf_appendf(&sec, "dup %d 5 RD ", i)) goto fails;
                if (xpost_strbuf_append(&sec, cs, 5)) goto fails;
                if (xpost_strbuf_appendf(&sec, " NP\n")) goto fails;
                continue;
            }
            if (xpost_strbuf_appendf(&sec, "dup %d %u RD ", i,
                                     (unsigned int)s.comp_.sz)) goto fails;
            if (_t1_emit_bin(ctx, &sec, s)) goto fails;
            if (xpost_strbuf_appendf(&sec, " NP\n")) goto fails;
        }
        if (xpost_strbuf_appendf(&sec, "ND\n")) goto fails;
    }
    if (xpost_strbuf_appendf(&sec, "end put\n"
        "dup /CharStrings %d dict dup begin\n", csflat.comp_.sz / 2 + 1))
        goto fails;
    for (i = 0; i + 1 < (integer)csflat.comp_.sz; i += 2)
    {
        Xpost_Object nm = xpost_array_get(ctx, csflat, i);
        Xpost_Object s = xpost_array_get(ctx, csflat, i + 1);
        Xpost_Object nstr;
        char nbuf[128];

        if (xpost_object_get_type(s) != stringtype)
            continue;
        if (xpost_object_get_type(nm) != nametype)
            continue;
        nstr = xpost_name_get_string(ctx, nm);
        if (nstr.comp_.sz >= sizeof nbuf)
            continue;
        /* the program names each glyph as text, which ends at the first
           nul: a name carrying one would be written out as the shorter
           name it begins with, and two names sharing that beginning
           would define one glyph twice over. A name the font program
           cannot carry is left out of it */
        if (!xpost_string_is_cstring(ctx, nstr))
            continue;
        memcpy(nbuf, xpost_string_get_pointer(ctx, nstr), nstr.comp_.sz);
        nbuf[nstr.comp_.sz] = 0;
        if (xpost_strbuf_appendf(&sec, "/%s %u RD ", nbuf,
                                 (unsigned int)s.comp_.sz)) goto fails;
        if (_t1_emit_bin(ctx, &sec, s)) goto fails;
        if (xpost_strbuf_appendf(&sec, " ND\n")) goto fails;
    }
    if (xpost_strbuf_appendf(&sec,
        "end end put put\n"
        "dup /FontName get exch definefont pop\n"
        "mark currentfile closefile\n")) goto fails;

    /* a reader decides hex against the first four cipher bytes, so
       the salt must not encrypt to four hexadecimal characters */
    for (;;)
    {
        unsigned char t[4];
        unsigned short r = 55665;
        int j, allhex = 1;

        for (j = 0; j < 4; j++)
        {
            unsigned char cc = (unsigned char)(sec.s[j] ^ (r >> 8));

            t[j] = cc;
            r = (unsigned short)((unsigned int)(cc + r) * 52845u + 22719u);
        }
        for (j = 0; j < 4; j++)
            if (!( (t[j] >= '0' && t[j] <= '9')
                || (t[j] >= 'a' && t[j] <= 'f')
                || (t[j] >= 'A' && t[j] <= 'F') ))
                allhex = 0;
        if (!allhex)
            break;
        sec.s[0]++;   /* different salt, different ciphertext */
    }

    wlen = hdr.len + sec.len + 30;
    whole = malloc(wlen + 4);
    if (!whole)
        goto fails;
    memcpy(whole, hdr.s, hdr.len);
    memcpy(whole + hdr.len, sec.s, sec.len);
    _t1_encrypt(whole + hdr.len, sec.len);
    memcpy(whole + hdr.len + sec.len, "\n0000000000000000\ncleartomark\n", 30);
    xpost_strbuf_free(&hdr);
    xpost_strbuf_free(&sec);

    data.face = xpost_font_face_new_from_memory(whole, wlen);
    if (data.face == NULL)
    {
        /* The charstrings describe no face that can be built. PLRM 8.2
           has definefont check that the dictionary holds the entries its
           type requires, and this one does: what failed is the content of
           CharStrings rather than the presence of any entry, so the font
           is defined without a face rather than refused.

           That is not a new state. A Type 1 font carrying no CharStrings
           at all is already defined this way, and the showing operators
           answer invalidfont when either is asked to paint, which is
           where the absence belongs -- a program may define a font and
           fill its charstrings afterwards, and definefont is not the
           operator that reads them. */
        free(whole);
        return 0;
    }

    ret = _font_bbox_declare(ctx, fontdict, data.face, 1000.0);
    if (ret)
        goto facefail;

    ret = _font_data_set(ctx, fontdict, data.face, 1);
    if (ret)
        goto facefail;
    /* the block holds the face from here, and gives it back when the
       collector reclaims it, so a failure past this point leaves the
       face where something else answers for it */
    ret = xpost_dict_put(ctx, fontdict, name_dotemunits,
                       xpost_int_cons(xpost_font_face_units(data.face)));
    if (ret)
        return ret;
    return 0;

    /* the face reads the program where it lies and no block names
       either yet, so this call is the only thing that can give them
       back; the face takes its program with it */
facefail:
    xpost_font_face_free(data.face);
    return ret;
fails:
    xpost_strbuf_free(&sec);
failh:
    xpost_strbuf_free(&hdr);
    return invalidfont;
}

/* --- the glyph mask cache --------------------------------------------
   Driver-generated text paints the same few masks over and over, so a
   rendered mask is kept and re-placed rather than rebuilt. The key has to
   outlive a restore, which is why the serial it is filed under is minted
   outside virtual memory. */


/* maskdict  .stencilaa  bool
   paint a small stencil mask with coverage-blended edges, the way
   glyph bitmaps paint: an axis-aligned transform lets each device
   pixel take the box-filtered coverage of the mask cells it spans,
   fully covered pixels going through the device's solid path and
   partial ones through its blend. Anything else -- a rotated or
   skewed matrix, an oversized result, a device without the blending
   machinery -- answers false and the caller keeps the bilevel path. */
static
int _stencilaa(Xpost_Context *ctx,
               Xpost_Object dict)
{
    Xpost_Object gs, devdic, putpix;
    Xpost_Object buf, mat, o;
    textstate ts;
    int inked = 0;
    int w, h, ink, ncomp, interp = 0;
    Xpost_Object comp[4];
    double m[6];
    double fx0, fx1, fy0, fy1, xa, xb, ya, yb, full;
    int ix0, iy0, devw, devh, px, py, i;
    int rowbytes;
    unsigned char *bits, *cov;

    gs = _gstate(ctx);
    devdic = xpost_dict_get(ctx, gs, name_device);
    if (xpost_object_get_type(devdic) != dicttype)
        goto refuse;

    memset(&ts, 0, sizeof ts);
    ts.blend = _text_blends(ctx, devdic, &ts.blendpix);
    if (!ts.blend)
        goto refuse;
    putpix = xpost_dict_get(ctx, devdic, name_PutPix);

    buf = xpost_dict_get(ctx, dict, name_buf);
    if (xpost_object_get_type(buf) != stringtype)
        goto refuse;
    o = xpost_dict_get(ctx, dict, name_width);
    if (xpost_object_get_type(o) != integertype) goto refuse;
    w = o.int_.val;
    o = xpost_dict_get(ctx, dict, name_height);
    if (xpost_object_get_type(o) != integertype) goto refuse;
    h = o.int_.val;
    o = xpost_dict_get(ctx, dict, name_ink);
    if (xpost_object_get_type(o) != integertype) goto refuse;
    ink = o.int_.val;
    mat = xpost_dict_get(ctx, dict, name_mat);
    if (xpost_object_get_type(mat) != arraytype || mat.comp_.sz != 6)
        goto refuse;
    o = xpost_dict_get(ctx, dict, name_interp);
    interp = xpost_object_get_type(o) == booleantype && o.int_.val;
    for (i = 0; i < 6; i++)
    {
        o = xpost_array_get(ctx, mat, i);
        m[i] = xpost_object_get_type(o) == realtype ? o.real_.val
             : xpost_object_get_type(o) == integertype ? (double)o.int_.val
             : 0.0;
    }
    if (w <= 0 || h <= 0)
        goto refuse;
    rowbytes = w / 8 + (w % 8 ? 1 : 0);   /* w may be near INT_MAX; w + 7 would overflow */
    /* the mask has to fit its buffer, and the buffer's capacity counted
       in rows answers that: the byte count the two dimensions multiply
       to need not itself stay within the integer range, and a mask
       whose product wraps must be refused rather than sampled */
    if ((integer)(buf.comp_.sz / (dword)rowbytes) < h)
        goto refuse;
    /* coverage integrates separably only over an axis-aligned map */
    if (fabs(m[1]) > 1e-4 || fabs(m[2]) > 1e-4
     || fabs(m[0]) < 1e-6 || fabs(m[3]) < 1e-6)
        goto refuse;

    xa = m[4]; xb = m[0] * w + m[4];
    fx0 = xa < xb ? xa : xb; fx1 = xa < xb ? xb : xa;
    ya = m[5]; yb = m[3] * h + m[5];
    fy0 = ya < yb ? ya : yb; fy1 = ya < yb ? yb : ya;
    /* fx0..fy1 come from a program-controlled matrix; cast and subtract
       them only where the origin is a representable pixel coordinate with
       room for the arithmetic, as _show_glyph does. A mask placed past
       this is off any real page. */
    if (!(isfinite(fx0) && isfinite(fx1) && isfinite(fy0) && isfinite(fy1)
          && fx0 > -1.0e9 && fx1 < 1.0e9 && fy0 > -1.0e9 && fy1 < 1.0e9))
        goto refuse;
    ix0 = (int)floor(fx0); iy0 = (int)floor(fy0);
    devw = (int)ceil(fx1) - ix0;
    devh = (int)ceil(fy1) - iy0;
    if (devw <= 0 || devh <= 0 || devw > 4096 || devh > 4096
     || devw * devh > (1 << 20))
        goto refuse;

    cov = malloc((size_t)devw * devh);
    if (!cov)
        goto refuse;
    bits = (unsigned char *)xpost_string_get_pointer(ctx, buf);
    full = (1.0 / fabs(m[0])) * (1.0 / fabs(m[3]));

    for (py = 0; py < devh; py++)
    {
        double dy0 = iy0 + py, dy1 = dy0 + 1;
        double my0 = (dy0 - m[5]) / m[3], my1 = (dy1 - m[5]) / m[3];
        double t;
        int yi, yi0, yi1;

        if (my0 > my1) { t = my0; my0 = my1; my1 = t; }
        if (my0 < 0) my0 = 0;
        if (my1 > h) my1 = h;
        yi0 = (int)floor(my0); yi1 = (int)ceil(my1);
        for (px = 0; px < devw; px++)
        {
            double dx0 = ix0 + px, dx1 = dx0 + 1;
            double mx0 = (dx0 - m[4]) / m[0], mx1 = (dx1 - m[4]) / m[0];
            double acc = 0.0;
            int xi, xi0, xi1;

            if (mx0 > mx1) { t = mx0; mx0 = mx1; mx1 = t; }
            if (mx0 < 0) mx0 = 0;
            if (mx1 > w) mx1 = w;
            /* an interpolated mask magnified past its cells ramps
               between them: sample the field bilinearly at the pixel
               centre instead of box-filtering within one cell */
            if (interp && mx1 - mx0 < 1.0 && my1 - my0 < 1.0
             && mx1 > mx0 && my1 > my0)
            {
                double cx = (mx0 + mx1) * 0.5 - 0.5;
                double cy = (my0 + my1) * 0.5 - 0.5;
                int bx = (int)floor(cx), by = (int)floor(cy);
                double fx = cx - bx, fy = cy - by;
                double v = 0.0;
                int dx, dy;

                for (dy = 0; dy < 2; dy++)
                    for (dx = 0; dx < 2; dx++)
                    {
                        int sx = bx + dx, sy = by + dy;
                        double wt = (dx ? fx : 1.0 - fx)
                                  * (dy ? fy : 1.0 - fy);
                        int bit;

                        if (sx < 0 || sx >= w || sy < 0 || sy >= h)
                            continue;
                        bit = (bits[sy * rowbytes + sx / 8]
                               >> (7 - (sx % 8))) & 1;
                        if (bit == ink)
                            v += wt;
                    }
                acc = v * 255.0 + 0.5;
                cov[py * devw + px] = acc >= 255.0 ? 255
                                    : acc <= 0.0 ? 0 : (unsigned char)acc;
                continue;
            }
            xi0 = (int)floor(mx0); xi1 = (int)ceil(mx1);
            for (yi = yi0; yi < yi1; yi++)
            {
                double wy = (yi + 1 < my1 ? yi + 1 : my1)
                          - (yi > my0 ? yi : my0);

                if (wy <= 0)
                    continue;
                for (xi = xi0; xi < xi1; xi++)
                {
                    double wx = (xi + 1 < mx1 ? xi + 1 : mx1)
                              - (xi > mx0 ? xi : mx0);
                    int bit;

                    if (wx <= 0)
                        continue;
                    bit = (bits[yi * rowbytes + xi / 8] >> (7 - (xi % 8))) & 1;
                    if (bit == ink)
                        acc += wx * wy;
                }
            }
            acc = acc / full * 255.0 + 0.5;
            cov[py * devw + px] = acc >= 255.0 ? 255
                                : acc <= 0.0 ? 0 : (unsigned char)acc;
        }
    }

    if (_device_color(ctx, gs, devdic, &ncomp, comp))
    {
        free(cov);
        goto refuse;
    }
    /* the answer goes under the queue: the painter stacks one entry
       per pixel above it, and each entry consumes its own operands
       before the caller sees the boolean */
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    _draw_bitmap(ctx, devdic, putpix, &ts, cov, devh, devw, devw,
                 XPOST_FONT_PIXEL_MODE_GRAY, ix0, iy0,
                 ncomp, comp[0], comp[1], comp[2], comp[3], &inked);
    free(cov);
    /* the coverage went to the device rather than through a painting
       operator, so what it left on the page is recorded here */
    if (inked)
        return _page_mark(ctx);
    return 0;
refuse:
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
}

/* Type 3 glyphs cached through setcachedevice: the key is the font's
   serial and the character code under the exact text-to-device
   transform, quantized as the face transforms are. The store is the
   glyph cache; the raster is a coverage mask captured from the build
   procedure's marks. */

/* The cache holds a caller's key for a mask beside the transform it was
   rendered under, and answers a lookup that matches both. The key is a
   serial and a name's stack index side by side, so the field carrying
   it spans both whole: a field narrower than the two would carry two
   keys to one number, and the entry filed under one would be answered
   to the other. `long` is the integer's width on some platforms and
   half of it on others, which is why it is not the field. */
typedef char xpost_mask_key_spans_serial_and_selector[
    sizeof(unsigned long long) * 8 >= 64 ? 1 : -1];

/* The two halves of the key, and the room each is given in the field
   they share. The selector half takes a name's stack index, plus the
   bank bit saying which of the two name stacks the index is into: a
   name is that pair and nothing narrower tells two of them apart. What
   is left over is the serial's.
 *
 * Both halves are held to their room rather than assumed to fit it.
 * The index is carried in a field whose width is the build's -- half
 * the object's, which is thirty-two bits in one build and sixty-four in
 * the other -- and the serial is an integer object, which is the same
 * two widths. So either could in principle arrive wider than the share
 * it is given here, and one that did would carry into the other's bits
 * and be answered a mask belonging to a different name or a different
 * font. Neither is reduced into range: a key that does not fit is no
 * key, and its glyph builds from its own description, which is the same
 * answer this gives a font carrying no serial at all. */
#define MASK_KEY_SELECTOR_BITS 33
#define MASK_KEY_SERIAL_MAX \
    ((unsigned long long)1 << (64 - MASK_KEY_SELECTOR_BITS))
#define MASK_KEY_INDEX_MAX ((unsigned long long)1 << 32)

/* Turns the caller's description of a mask into the key the cache
   holds it under, and the transform into the fixed point it is
   compared at. */
static int
_mask_key(Xpost_Context *ctx, Xpost_Object key,
          Xpost_Object mat, unsigned long long *k2, long m[4])
{
    Xpost_Object serial, selector;
    unsigned long long sel;
    int i;

    /* What the mask is of: the caller pairs a number naming the font
       the mask was built from with the name of the thing built. For a
       glyph that pair is the font's serial and the character name, as
       PLRM 5.5 identifies a cached glyph -- by the font and by the
       character selector, which in a base font is a name. A caller
       whose masks are not glyphs pairs whatever distinguishes one of
       its masks from another; the cache neither knows nor needs to know
       which. */
    if (xpost_object_get_type(key) != arraytype || key.comp_.sz != 2)
        return 0;
    serial = xpost_array_get(ctx, key, 0);
    selector = xpost_array_get(ctx, key, 1);
    if (xpost_object_get_type(serial) != integertype
     || xpost_object_get_type(selector) != nametype)
        return 0;
    if (serial.int_.val < 0
     || (unsigned long long)serial.int_.val >= MASK_KEY_SERIAL_MAX)
        return 0;
    sel = (unsigned long long)selector.mark_.padw;
    if (sel >= MASK_KEY_INDEX_MAX)
        return 0;
    if (selector.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)
        sel |= (unsigned long long)1 << 32;
    *k2 = ((unsigned long long)serial.int_.val << MASK_KEY_SELECTOR_BITS)
        | sel;
    if (xpost_object_get_type(mat) != arraytype || mat.comp_.sz != 6)
        return 0;
    for (i = 0; i < 4; i++)
    {
        Xpost_Object el = xpost_array_get(ctx, mat, i);
        double v = xpost_object_get_type(el) == realtype ? el.real_.val
                 : xpost_object_get_type(el) == integertype ? (double)el.int_.val
                 : 0.0;

        if (!_fixed16(v, &m[i]))
            return 0;
    }
    return 1;
}

/* x y mat key cliparr  .maskcachehit  n0 n1 true
                                       false
   Paint a cached mask at the device origin (x y) in the current colour,
   and answer the two numbers filed with it. A mask whose raster leaves
   the clip rectangle [x0 y0 x1 y1] answers false, and the caller paints
   it the long way, clipped.

   The mask is coverage, painted in whatever colour is current: that is
   what makes one cache serve a glyph and an uncoloured pattern cell
   alike. What the two numbers mean is the caller's business -- a glyph
   files its advances there. */
static
int _maskcachehit(Xpost_Context *ctx,
                  Xpost_Object x,
                  Xpost_Object y,
                  Xpost_Object mat,
                  Xpost_Object key,
                  Xpost_Object cliparr)
{
    Xpost_Object gs, devdic, putpix;
    textstate ts;
    unsigned long long k2;
    long m[4];
    unsigned char *bits;
    int rows, width, pitch, left, top;
    long ax, ay;
    int ncomp;
    int inked = 0;
    Xpost_Object comp[4];
    double dx, dy;

    if (!_mask_key(ctx, key, mat, &k2, m))
        goto refuse;
    if (!xpost_mask_cache_lookup(NULL, k2, m, 0,
                                      &bits, &rows, &width, &pitch,
                                      &left, &top, &ax, &ay))
        goto refuse;

    gs = _gstate(ctx);
    devdic = xpost_dict_get(ctx, gs, name_device);
    if (xpost_object_get_type(devdic) != dicttype)
        goto refuse;

    memset(&ts, 0, sizeof ts);
    ts.blend = _text_blends(ctx, devdic, &ts.blendpix);
    putpix = xpost_dict_get(ctx, devdic, name_PutPix);

    if (_device_color(ctx, gs, devdic, &ncomp, comp))
        goto refuse;

    dx = xpost_object_number(x);
    dy = xpost_object_number(y);

    /* a non-finite or out-of-int-range pen position is undefined to convert
       to the pixel coordinates below and would overflow the clip
       arithmetic; the cached glyph is off the page, so skip it as the clip
       test further down already does */
    if (!isfinite(dx) || !isfinite(dy)
        || dx <= -1.0e9 || dx >= 1.0e9 || dy <= -1.0e9 || dy >= 1.0e9)
        goto refuse;

    {
        double c[4];
        int i;
        int gx = (int)floor(dx + 0.5) + left;
        int gy = (int)floor(dy + 0.5) - top;

        if (xpost_object_get_type(cliparr) != arraytype
         || cliparr.comp_.sz != 4)
            goto refuse;
        for (i = 0; i < 4; i++)
        {
            Xpost_Object el = xpost_array_get(ctx, cliparr, i);

            c[i] = xpost_object_get_type(el) == realtype ? el.real_.val
                 : xpost_object_get_type(el) == integertype
                 ? (double)el.int_.val : 0.0;
        }
        if (gx < c[0] || gy < c[1]
         || gx + width > c[2] || gy + rows > c[3])
            goto refuse;
    }

    /* the answers go under the queue: the painter stacks one entry
       per pixel above them, and each entry consumes its own operands
       before the caller sees the boolean */
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)(ax / 65536.0)));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)(ay / 65536.0)));
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    _draw_bitmap(ctx, devdic, putpix, &ts,
                 bits, rows, width, pitch,
                 XPOST_FONT_PIXEL_MODE_GRAY,
                 (int)floor(dx + 0.5) + left,
                 (int)floor(dy + 0.5) - top,
                 ncomp, comp[0], comp[1], comp[2], comp[3], &inked);
    /* the mask went to the device rather than through a painting
       operator, so what it left on the page is recorded here */
    if (inked)
        return _page_mark(ctx);
    return 0;
refuse:
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
}

/* capdict  .maskcacheput  -
   insert a captured glyph mask: buf holds width x height coverage
   bytes whose raster origin sits at device (bx0 by0), the glyph
   origin was at device (ox oy), advances are character-space, and
   mat and key identify the entry as .maskcachehit reads it */
static
int _maskcacheput(Xpost_Context *ctx,
                Xpost_Object dict)
{
    Xpost_Object o, buf, mat, key;
    unsigned long long k2;
    long m[4];
    long qax, qay;
    int w, h, bx0, by0, left, top;
    double ox, oy, advx, advy;
    unsigned char *bytes;

#define DGET(name, var, want) do {     o = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, name));     if (xpost_object_get_type(o) != want) return typecheck;     var = o; } while (0)
#define DNUM(name, var) do {     o = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, name));     if (xpost_object_get_type(o) == realtype) var = o.real_.val;     else if (xpost_object_get_type(o) == integertype) var = (double)o.int_.val;     else return typecheck; } while (0)
    DGET("buf", buf, stringtype);
    DGET("mat", mat, arraytype);
    DGET("key", key, arraytype);
    { double t; DNUM("w", t); if (!(t > -2.0e9 && t < 2.0e9)) return rangecheck; w = (int)t; }
    { double t; DNUM("h", t); if (!(t > -2.0e9 && t < 2.0e9)) return rangecheck; h = (int)t; }
    { double t; DNUM("bx0", t); if (!(t > -2.0e9 && t < 2.0e9)) return rangecheck; bx0 = (int)t; }
    { double t; DNUM("by0", t); if (!(t > -2.0e9 && t < 2.0e9)) return rangecheck; by0 = (int)t; }
    DNUM("ox", ox);
    DNUM("oy", oy);
    DNUM("advx", advx);
    DNUM("advy", advy);
#undef DGET
#undef DNUM
    /* the mask is read as h rows of w coverage bytes out of buf, and
       the dimensions come out of a dictionary a program can build, so
       buf has to hold that many bytes; the row count is compared
       against the length divided by the row width, which holds for
       every pair of dimensions rather than only those whose product
       fits the type a multiplication would form it in */
    if (w <= 0 || h <= 0 || (unsigned int)h > buf.comp_.sz / (unsigned int)w)
        return rangecheck;
    if (!_mask_key(ctx, key, mat, &k2, m))
        return 0;
    /* the advances are filed in the same fixed-point form and come out of
       the same dictionary, so a pair the form does not hold leaves the
       mask unfiled rather than filed against advances it does not have */
    if (!_fixed16(advx, &qax) || !_fixed16(advy, &qay))
        return 0;
    bytes = (unsigned char *)xpost_string_get_pointer(ctx, buf);
    /* ox/oy come out of the dictionary; a non-finite or out-of-range
       origin leaves the mask unfiled rather than casting past the int
       range (the same finite-coordinate rule as _show_glyph). */
    if (!(isfinite(ox) && isfinite(oy)
          && ox > -1.0e9 && ox < 1.0e9 && oy > -1.0e9 && oy < 1.0e9))
        return 0;
    left = bx0 - (int)floor(ox + 0.5);
    top = (int)floor(oy + 0.5) - by0;
    (void)xpost_mask_cache_insert(NULL, k2, m, 0,
                                       bytes, h, w, w, left, top,
                                       qax, qay);
    return 0;
}

/* ciddict glypharray  .loadcidfont2  -
   assemble a working TrueType face for a CIDFontType 2 dictionary.
   The /sfnts strings supply every table but the outlines: the glyphs
   arrive in /GlyphDirectory, delivered incrementally by glyph index,
   and the caller flattens the directory into an array indexed by
   glyph (null where none has arrived). A fresh glyf table and a
   long-format loca are synthesized around the delivered outlines,
   maxp's glyph count and head's loca format patched to match, and
   the whole reassembled program opened as a memory face stored in
   /Private. Called again after the directory has grown, the previous
   face is released and rebuilt around the larger complement. */
static
int _loadcidfont2(Xpost_Context *ctx,
                  Xpost_Object fontdict,
                  Xpost_Object glyphs)
{
    Xpost_Object sfnts;
    struct fontdata data;
    unsigned char *buf = NULL, *out = NULL;
    size_t total, glyftotal, outtotal, pos;
    unsigned int ntab, nglyphs;
    unsigned int headoff = 0, maxpoff = 0;
    int i;
    int ret;

    sfnts = xpost_dict_get(ctx, fontdict, name_sfnts);
    if (xpost_object_get_type(sfnts) != arraytype)
        return invalidfont;
    total = 0;
    /* the walk counts in the signed type the table indexing below uses,
       so the array's element count is widened into it to be compared */
    for (i = 0; i < (integer)sfnts.comp_.sz; i++)
    {
        Xpost_Object s = xpost_array_get(ctx, sfnts, i);
        if (xpost_object_get_type(s) != stringtype)
            return invalidfont;
        total += s.comp_.sz;
    }
    if (total < 12)
        return invalidfont;
    buf = malloc(total);
    if (!buf)
        return VMerror;
    total = 0;
    for (i = 0; i < (integer)sfnts.comp_.sz; i++)
    {
        Xpost_Object s = xpost_array_get(ctx, sfnts, i);
        memcpy(buf + total, xpost_string_get_pointer(ctx, s), s.comp_.sz);
        total += s.comp_.sz;
    }

    ntab = _sfnt_u16(buf + 4);
    if (12 + 16 * (size_t)ntab > total)
    {
        free(buf);
        return invalidfont;
    }

    nglyphs = glyphs.comp_.sz;
    glyftotal = 0;
    for (i = 0; i < (int)nglyphs; i++)
    {
        Xpost_Object g = xpost_array_get(ctx, glyphs, i);
        if (xpost_object_get_type(g) == stringtype)
            glyftotal += (g.comp_.sz + 1) & ~(size_t)1;
    }

    /* rebuild the directory: glyf and loca are synthesized (the
       template may omit them entirely, carrying a gdir placeholder
       for the incremental download instead), the hinting programs
       and the placeholder are dropped -- a subset template's
       bytecode does not survive its stripping and fails every glyph
       under the bytecode interpreter -- and everything else is
       carried over */
    {
        int has_glyf = 0, has_loca = 0;
        unsigned int newntab = 0, w = 0;

        out = NULL;
        for (i = 0; i < (int)ntab; i++)
        {
            unsigned int tag = _sfnt_u32(buf + 12 + 16 * i);
            if (tag == 0x63767420 || tag == 0x6670676d
             || tag == 0x70726570 || tag == 0x67646972)
                continue;
            if (tag == 0x676c7966) has_glyf = 1;
            if (tag == 0x6c6f6361) has_loca = 1;
            newntab++;
        }
        newntab += !has_glyf + !has_loca;

        outtotal = 12 + 16 * (size_t)newntab;
        for (i = 0; i < (int)ntab; i++)
        {
            unsigned char *e = buf + 12 + 16 * i;
            unsigned int tag = _sfnt_u32(e);
            size_t len;
            if (tag == 0x63767420 || tag == 0x6670676d
             || tag == 0x70726570 || tag == 0x67646972)
                continue;
            if (tag == 0x676c7966)
                len = glyftotal;
            else if (tag == 0x6c6f6361)
                len = 4 * ((size_t)nglyphs + 1);
            else
            {
                /* A table is held to lying within the data here, where
                   the room for it is counted, and not only below where
                   it is copied. The directory states the length, so
                   without this the sum is the font program's to choose:
                   a length no data can cover is still counted in, and
                   enough of them carry the sum past what a size_t as
                   wide as the two 32-bit fields will hold -- leaving a
                   small allocation that each table then passes the copy
                   check against and is written beyond. */
                size_t srcoff = _sfnt_u32(e + 8);

                len = _sfnt_u32(e + 12);
                if (srcoff > total || len > total - srcoff)
                {
                    free(buf);
                    return invalidfont;
                }
            }
            outtotal = (outtotal + 3) & ~(size_t)3;
            outtotal += len;
        }
        if (!has_glyf)
        {
            outtotal = (outtotal + 3) & ~(size_t)3;
            outtotal += glyftotal;
        }
        if (!has_loca)
        {
            outtotal = (outtotal + 3) & ~(size_t)3;
            outtotal += 4 * ((size_t)nglyphs + 1);
        }
        out = malloc(outtotal);
        if (!out)
        {
            free(buf);
            return VMerror;
        }
        memcpy(out, buf, 12);
        _sfnt_put16(out + 4, newntab);
        for (i = 0; i < (int)ntab; i++)
        {
            unsigned char *e = buf + 12 + 16 * i;
            unsigned int tag = _sfnt_u32(e);
            if (tag == 0x63767420 || tag == 0x6670676d
             || tag == 0x70726570 || tag == 0x67646972)
                continue;
            memcpy(out + 12 + 16 * w, e, 16);
            w++;
        }
        pos = 12 + 16 * (size_t)w;
        if (!has_glyf)
        {
            memset(out + pos, 0, 16);
            _sfnt_put32(out + pos, 0x676c7966);
            pos += 16;
            w++;
        }
        if (!has_loca)
        {
            memset(out + pos, 0, 16);
            _sfnt_put32(out + pos, 0x6c6f6361);
            pos += 16;
            w++;
        }
        ntab = newntab;
    }

    pos = 12 + 16 * (size_t)ntab;
    for (i = 0; i < (int)ntab; i++)
    {
        unsigned char *e = out + 12 + 16 * i;
        unsigned int tag = _sfnt_u32(e);
        unsigned int srcoff = _sfnt_u32(e + 8);
        unsigned int srclen = _sfnt_u32(e + 12);

        pos = (pos + 3) & ~(size_t)3;
        if (tag == 0x676c7966)
        {
            size_t gp = 0;
            int gi;
            for (gi = 0; gi < (int)nglyphs; gi++)
            {
                Xpost_Object g = xpost_array_get(ctx, glyphs, gi);
                if (xpost_object_get_type(g) == stringtype)
                {
                    memcpy(out + pos + gp,
                           xpost_string_get_pointer(ctx, g), g.comp_.sz);
                    if (g.comp_.sz & 1)
                        out[pos + gp + g.comp_.sz] = 0;
                    gp += (g.comp_.sz + 1) & ~(size_t)1;
                }
            }
            _sfnt_put32(e + 8, (unsigned int)pos);
            _sfnt_put32(e + 12, (unsigned int)glyftotal);
            pos += glyftotal;
        }
        else if (tag == 0x6c6f6361)
        {
            size_t gp = 0;
            int gi;
            for (gi = 0; gi <= (int)nglyphs; gi++)
            {
                _sfnt_put32(out + pos + 4 * gi, (unsigned int)gp);
                if (gi < (int)nglyphs)
                {
                    Xpost_Object g = xpost_array_get(ctx, glyphs, gi);
                    if (xpost_object_get_type(g) == stringtype)
                        gp += (g.comp_.sz + 1) & ~(size_t)1;
                }
            }
            _sfnt_put32(e + 8, (unsigned int)pos);
            _sfnt_put32(e + 12, 4 * (nglyphs + 1));
            pos += 4 * ((size_t)nglyphs + 1);
        }
        else
        {
            /* keep the table within the data, without a srcoff+srclen
               wrap where size_t is as wide as the two 32-bit fields
               the directory states them in */
            if ((size_t)srcoff > total
                || (size_t)srclen > total - (size_t)srcoff)
            {
                free(buf); free(out);
                return invalidfont;
            }
            memcpy(out + pos, buf + srcoff, srclen);
            if (tag == 0x68656164) headoff = (unsigned int)pos;
            if (tag == 0x6d617870) maxpoff = (unsigned int)pos;
            _sfnt_put32(e + 8, (unsigned int)pos);
            pos += srclen;
        }
    }
    free(buf);
    if (headoff && headoff + 52 <= outtotal)
        _sfnt_put16(out + headoff + 50, 1);   /* long loca offsets */
    if (maxpoff && maxpoff + 6 <= outtotal)
        _sfnt_put16(out + maxpoff + 4, nglyphs);

    data.face = xpost_font_face_new_from_memory(out, outtotal);
    if (data.face == NULL)
    {
        free(out);
        return invalidfont;
    }

    ret = _font_bbox_declare(ctx, fontdict, data.face, 1.0);
    if (ret)
        goto facefail;

    ret = _font_data_set(ctx, fontdict, data.face, 1);
    if (ret)
        goto facefail;
    return 0;

    /* the face reads the program where it lies and no block names
       either yet, so this call is the only thing that can give them
       back; the face takes its program with it */
facefail:
    xpost_font_face_free(data.face);
    return ret;

}

static
int _ashow(Xpost_Context *ctx,
           Xpost_Object dx,
           Xpost_Object dy,
           Xpost_Object str)
{
    Xpost_Object gs;
    Xpost_Object fontdict;
    struct fontdata data;
    char *cstr;
    real xpos, ypos;
    char *ch;
    char *chend;
    Xpost_Object devdic;
    Xpost_Object putpix;
    textstate ts;
    int ncomp;
    Xpost_Object comp[4];
    Xpost_Object finalize;
    int painted = 1;
    int inked = 0;
    int ret;


    /* load the graphicsdict, current graphics state, and current font */
    gs = _gstate(ctx);
    fontdict = xpost_dict_get(ctx, gs, name_currfont);
    if (xpost_object_get_type(fontdict) == invalidtype)
        return invalidfont;
    XPOST_LOG_INFO("loaded graphicsdict, graphics state, and current font");

    /* load the device and PutPix member function */
    devdic = xpost_dict_get(ctx, gs, name_device);
    putpix = xpost_dict_get(ctx, devdic, name_PutPix);
    XPOST_LOG_INFO("loaded DEVICE and PutPix");
    ts = _text_state_get(ctx, gs, fontdict, devdic);

    /* get the font data from the font dict */
    ret = _font_data_current(ctx, gs, fontdict, &data);
    if (ret)
        return ret;
    XPOST_LOG_INFO("loaded font data from dict");

    cstr = xpost_string_allocate_cstring(ctx, str);
    if (!cstr)
        return VMerror;
    chend = cstr + str.comp_.sz;

    ret = _get_current_point(ctx, gs, &xpos, &ypos);
    if (ret){
        free(cstr);
        return ret;
    }

    if (_device_color(ctx, gs, devdic, &ncomp, comp))
    {
        free(cstr);
        return unregistered;
    }
    XPOST_LOG_INFO("ncomp = %d", ncomp);

    ret = _show_finalize_cons(ctx, xpos, ypos, &finalize);
    if (ret)
    {
        free(cstr);
        return ret;
    }
    xpost_stack_push(ctx->lo, ctx->es, finalize);

    /* render text in char *cstr  with font data  at pen position xpos ypos */
    for (ch = cstr; ch < chend; ch++)
    {
        if (!_show_char(ctx, devdic, putpix, data, &ts, &xpos, &ypos, (unsigned char)*ch,
                   ncomp, comp[0], comp[1], comp[2], comp[3], &inked))
        {
            painted = 0;
            break;
        }
        xpos += dx.real_.val;
        ypos += dy.real_.val;
    }

    /* update current position in the graphics state */
    ret = _show_finalize_pos(ctx, finalize, xpos, ypos);
    /* the ink the glyphs left is a mark on the page; a string that
       painted none -- an empty one, blanks, or text the clip keeps
       nothing of -- leaves the page as it found it */
    if (!ret && inked)
        ret = _page_mark(ctx);
    /* the glyphs the string asked for are the font's to supply, and one
       it will not is that font failing the operator (PLRM 8.2 show) */
    if (!ret && !painted)
        ret = invalidfont;

    free(cstr);
    return ret;
}

static
int _widthshow(Xpost_Context *ctx,
               Xpost_Object cx,
               Xpost_Object cy,
               Xpost_Object charcode,
               Xpost_Object str)
{
    Xpost_Object gs;
    Xpost_Object fontdict;
    struct fontdata data;
    char *cstr;
    real xpos, ypos;
    char *ch;
    char *chend;
    Xpost_Object devdic;
    Xpost_Object putpix;
    textstate ts;
    int ncomp;
    Xpost_Object comp[4];
    Xpost_Object finalize;
    int painted = 1;
    int inked = 0;
    int ret;


    /* load the graphicsdict, current graphics state, and current font */
    gs = _gstate(ctx);
    fontdict = xpost_dict_get(ctx, gs, name_currfont);
    if (xpost_object_get_type(fontdict) == invalidtype)
        return invalidfont;
    XPOST_LOG_INFO("loaded graphicsdict, graphics state, and current font");

    /* load the device and PutPix member function */
    devdic = xpost_dict_get(ctx, gs, name_device);
    putpix = xpost_dict_get(ctx, devdic, name_PutPix);
    XPOST_LOG_INFO("loaded DEVICE and PutPix");
    ts = _text_state_get(ctx, gs, fontdict, devdic);

    /* get the font data from the font dict */
    ret = _font_data_current(ctx, gs, fontdict, &data);
    if (ret)
        return ret;
    XPOST_LOG_INFO("loaded font data from dict");

    cstr = xpost_string_allocate_cstring(ctx, str);
    if (!cstr)
        return VMerror;
    chend = cstr + str.comp_.sz;

    ret = _get_current_point(ctx, gs, &xpos, &ypos);
    if (ret){
        free(cstr);
        return ret;
    }

    if (_device_color(ctx, gs, devdic, &ncomp, comp))
    {
        free(cstr);
        return unregistered;
    }
    XPOST_LOG_INFO("ncomp = %d", ncomp);

    ret = _show_finalize_cons(ctx, xpos, ypos, &finalize);
    if (ret)
    {
        free(cstr);
        return ret;
    }
    xpost_stack_push(ctx->lo, ctx->es, finalize);

    /* render text in char *cstr  with font data  at pen position xpos ypos */
    for (ch = cstr; ch < chend; ch++)
    {
        if (!_show_char(ctx, devdic, putpix, data, &ts, &xpos, &ypos, (unsigned char)*ch,
                   ncomp, comp[0], comp[1], comp[2], comp[3], &inked))
        {
            painted = 0;
            break;
        }
        if ((unsigned char)*ch == charcode.int_.val)
        {
            xpos += cx.real_.val;
            ypos += cy.real_.val;
        }
    }

    /* update current position in the graphics state */
    ret = _show_finalize_pos(ctx, finalize, xpos, ypos);
    /* the ink the glyphs left is a mark on the page; a string that
       painted none -- an empty one, blanks, or text the clip keeps
       nothing of -- leaves the page as it found it */
    if (!ret && inked)
        ret = _page_mark(ctx);
    /* the glyphs the string asked for are the font's to supply, and one
       it will not is that font failing the operator (PLRM 8.2 show) */
    if (!ret && !painted)
        ret = invalidfont;

    free(cstr);
    return ret;
}

static
int _awidthshow(Xpost_Context *ctx,
                Xpost_Object cx,
                Xpost_Object cy,
                Xpost_Object charcode,
                Xpost_Object dx,
                Xpost_Object dy,
                Xpost_Object str)
{
    Xpost_Object gs;
    Xpost_Object fontdict;
    struct fontdata data;
    char *cstr;
    real xpos, ypos;
    char *ch;
    char *chend;
    Xpost_Object devdic;
    Xpost_Object putpix;
    textstate ts;
    int ncomp;
    Xpost_Object comp[4];
    Xpost_Object finalize;
    int painted = 1;
    int inked = 0;
    int ret;


    /* load the graphicsdict, current graphics state, and current font */
    gs = _gstate(ctx);
    fontdict = xpost_dict_get(ctx, gs, name_currfont);
    if (xpost_object_get_type(fontdict) == invalidtype)
        return invalidfont;
    XPOST_LOG_INFO("loaded graphicsdict, graphics state, and current font");

    /* load the device and PutPix member function */
    devdic = xpost_dict_get(ctx, gs, name_device);
    putpix = xpost_dict_get(ctx, devdic, name_PutPix);
    XPOST_LOG_INFO("loaded DEVICE and PutPix");
    ts = _text_state_get(ctx, gs, fontdict, devdic);

    /* get the font data from the font dict */
    ret = _font_data_current(ctx, gs, fontdict, &data);
    if (ret)
        return ret;
    XPOST_LOG_INFO("loaded font data from dict");

    cstr = xpost_string_allocate_cstring(ctx, str);
    if (!cstr)
        return VMerror;
    chend = cstr + str.comp_.sz;

    ret = _get_current_point(ctx, gs, &xpos, &ypos);
    if (ret){
        free(cstr);
        return ret;
    }

    if (_device_color(ctx, gs, devdic, &ncomp, comp))
    {
        free(cstr);
        return unregistered;
    }
    XPOST_LOG_INFO("ncomp = %d", ncomp);

    ret = _show_finalize_cons(ctx, xpos, ypos, &finalize);
    if (ret)
    {
        free(cstr);
        return ret;
    }
    xpost_stack_push(ctx->lo, ctx->es, finalize);

    /* render text in char *cstr  with font data  at pen position xpos ypos */
    for (ch = cstr; ch < chend; ch++)
    {
        if (!_show_char(ctx, devdic, putpix, data, &ts, &xpos, &ypos, (unsigned char)*ch,
                ncomp, comp[0], comp[1], comp[2], comp[3], &inked))
        {
            painted = 0;
            break;
        }
        xpos += dx.real_.val;
        ypos += dy.real_.val;
        if ((unsigned char)*ch == charcode.int_.val)
        {
            xpos += cx.real_.val;
            ypos += cy.real_.val;
        }
    }

    /* update current position in the graphics state */
    ret = _show_finalize_pos(ctx, finalize, xpos, ypos);
    /* the ink the glyphs left is a mark on the page; a string that
       painted none -- an empty one, blanks, or text the clip keeps
       nothing of -- leaves the page as it found it */
    if (!ret && inked)
        ret = _page_mark(ctx);
    /* the glyphs the string asked for are the font's to supply, and one
       it will not is that font failing the operator (PLRM 8.2 show) */
    if (!ret && !painted)
        ret = invalidfont;

    free(cstr);
    return ret;
}

/* How far the current point would move if the string were painted,
   without painting it. */
static
int _stringwidth(Xpost_Context *ctx,
                 Xpost_Object str)
{
    Xpost_Object gs;
    Xpost_Object fontdict;
    struct fontdata data;
    char *cstr;
    real xpos = 0, ypos = 0;
    char *ch;
    char *chend;
    Xpost_Object encoding;
    Xpost_Object charstrings;
    textstate mts;
    int ret;


    /* load the graphicsdict, current graphics state, and current font */
    gs = _gstate(ctx);
    fontdict = xpost_dict_get(ctx, gs, name_currfont);
    if (xpost_object_get_type(fontdict) == invalidtype)
        return invalidfont;
    XPOST_LOG_INFO("loaded graphicsdict, graphics state, and current font");

    /* get the font data from the font dict */
    ret = _font_data_current(ctx, gs, fontdict, &data);
    if (ret)
        return ret;
    encoding = xpost_dict_get(ctx, fontdict, name_Encoding);
    charstrings = xpost_dict_get(ctx, fontdict, name_CharStrings);
    /* only the /Metrics fields matter here: stringwidth accumulates the
       same per-glyph advances show would take */
    memset(&mts, 0, sizeof mts);
    mts.encoding = encoding;
    mts.metrics = xpost_dict_get(ctx, fontdict, name_Metrics);
    mts.cdmat_ok = xpost_object_get_type(mts.metrics) == dicttype
                && _char_device_matrix(ctx, gs, fontdict, mts.cdmat);
    XPOST_LOG_INFO("loaded font data from dict");

    cstr = xpost_string_allocate_cstring(ctx, str);
    if (!cstr)
        return VMerror;
    chend = cstr + str.comp_.sz;

    /* accumulate the advances without rendering: the outline metrics
       carry the advance; a glyph with no outline (a bitmap strike)
       renders as a fallback */
    for (ch = cstr; ch < chend; ch++)
    {
        unsigned int glyph_index;
        long bx0, by0, bx1, by1;
        long advance_x;
        long advance_y;

        glyph_index = _glyph_index_for_char(ctx, encoding, charstrings,
                                            data.face, (unsigned char)*ch);
        if (!xpost_font_face_glyph_extents(data.face, glyph_index,
                                           &bx0, &by0, &bx1, &by1,
                                           &advance_x, &advance_y))
        {
            unsigned char *buffer;
            int rows, width, pitch, left, top;
            char pixel_mode;

            if (!xpost_font_face_glyph_render(data.face, glyph_index))
            {
                free(cstr);
                return unregistered;
            }
            xpost_font_face_glyph_buffer_get(data.face, &buffer, &rows, &width,
                                             &pitch, &pixel_mode, &left, &top,
                                             &advance_x, &advance_y);
        }
        /* a /Metrics entry for this glyph overrides the face's advance */
        _metrics_advance(ctx, &mts,
                         _encoded_name(ctx, encoding, (unsigned char)*ch),
                         &advance_x, &advance_y);
        xpos += (real)(advance_x / 65536.0);
        ypos += (real)(advance_y / 65536.0);

    }

    /* the advances accumulate in the face's y-up glyph space, sized and
       oriented through the CTM; stringwidth must report the distance in
       user space, so flip to the device's y-down convention and map back
       through the inverse of the CTM's linear part */
    ypos = -ypos;
    {
        Xpost_Object psmat = xpost_dict_get(ctx, gs, name_currmatrix);
        if (xpost_object_get_type(psmat) == arraytype && psmat.comp_.sz == 6)
        {
            real m[4], det;

            _matrix_linear_part(ctx, psmat, m);
            det = m[0] * m[3] - m[1] * m[2];
            if (det != 0)
            {
                real ux = (m[3] * xpos - m[2] * ypos) / det;
                real uy = (-m[1] * xpos + m[0] * ypos) / det;
                xpos = ux;
                ypos = uy;
            }
        }
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(xpos));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(ypos));

    free(cstr);
    return 0;
}

/* str  .stringoutline  array
   the string's glyph outlines as a flat array of path segments in the
   face's y-up glyph space (device-magnitude pixels, oriented by the
   face transform), relative to the pen start: coordinates followed by
   a tag, /m /l /c (cubic) or /h. charpath (in font.ps) maps each
   point to user space about the current point and appends it to the
   current path. Blank glyphs contribute advance only. */
typedef struct outlinecollect
{
    Xpost_Context *ctx;
    Xpost_Object *objs;
    size_t len, cap;
    double px, py;
    int err;
    Xpost_Object nm, nl, nc, nh;
} outlinecollect;

/* --- collecting an outline into PostScript ---------------------------
   What charpath and the outline operators hand back: the same walk as the
   writers', gathering into an array of path operators instead of into a
   document. */

static
int _oc_push(outlinecollect *oc, Xpost_Object o)
{
    if (oc->len == oc->cap)
    {
        Xpost_Object *tmp;
        size_t ncap = oc->cap ? oc->cap * 2 : 256;

        tmp = realloc(oc->objs, ncap * sizeof *tmp);
        if (!tmp)
        {
            oc->err = VMerror;
            return 1;
        }
        oc->objs = tmp;
        oc->cap = ncap;
    }
    oc->objs[oc->len++] = o;
    return 0;
}

/* The sink that collects an outline as PostScript objects rather than
   painting it: each point is pushed in the current space, and each
   command follows its points the way the path operators take them. */
static
int _oc_xy(outlinecollect *oc, double x, double y)
{
    return _oc_push(oc, xpost_real_cons((real)(oc->px + x)))
        || _oc_push(oc, xpost_real_cons((real)(oc->py + y)));
}

static
int _oc_moveto(void *user, double x, double y)
{
    outlinecollect *oc = user;
    return _oc_xy(oc, x, y) || _oc_push(oc, oc->nm);
}

static
int _oc_lineto(void *user, double x, double y)
{
    outlinecollect *oc = user;
    return _oc_xy(oc, x, y) || _oc_push(oc, oc->nl);
}

static
int _oc_curveto(void *user, double x1, double y1, double x2, double y2, double x3, double y3)
{
    outlinecollect *oc = user;
    return _oc_xy(oc, x1, y1) || _oc_xy(oc, x2, y2) || _oc_xy(oc, x3, y3)
        || _oc_push(oc, oc->nc);
}

static
int _oc_closepath(void *user)
{
    outlinecollect *oc = user;
    return _oc_push(oc, oc->nh);
}

/* The collected segments as the array the outline operators answer
   with. The collector is given up either way. */
static
int _oc_array(Xpost_Context *ctx, outlinecollect *oc, Xpost_Object *arr)
{
    size_t i;

    if (oc->len > 65535)
    {
        free(oc->objs);
        return limitcheck;
    }
    *arr = xpost_object_cvlit(xpost_array_cons(ctx, (unsigned int)oc->len));
    if (xpost_object_get_type(*arr) == nulltype)
    {
        free(oc->objs);
        return VMerror;
    }
    for (i = 0; i < oc->len; i++)
    {
        int ret = xpost_array_put(ctx, *arr, (integer)i, oc->objs[i]);

        if (ret)
        {
            free(oc->objs);
            return ret;
        }
    }
    free(oc->objs);
    return 0;
}

/* The string's glyphs as a path, in objects the interpreter can
   execute, rather than as marks on the page. */
static
int _stringoutline(Xpost_Context *ctx,
                   Xpost_Object str)
{
    Xpost_Object gs;
    Xpost_Object fontdict;
    struct fontdata data;
    Xpost_Object encoding;
    Xpost_Object charstrings;
    textstate mts;
    double penx = 0.0, peny = 0.0;
    char *cstr;
    char *ch;
    char *chend;
    outlinecollect oc;
    Xpost_Object arr;
    int ret;

    gs = _gstate(ctx);
    fontdict = xpost_dict_get(ctx, gs, name_currfont);
    if (xpost_object_get_type(fontdict) == invalidtype)
        return invalidfont;

    ret = _font_data_current(ctx, gs, fontdict, &data);
    if (ret)
        return ret;
    encoding = xpost_dict_get(ctx, fontdict, name_Encoding);
    charstrings = xpost_dict_get(ctx, fontdict, name_CharStrings);
    /* only the /Metrics fields matter here: the glyphs are placed and
       advanced by the overrides show places and advances them by, and
       nothing is painted, so the device and the colour answer for
       nothing */
    memset(&mts, 0, sizeof mts);
    mts.encoding = encoding;
    mts.metrics = xpost_dict_get(ctx, fontdict, name_Metrics);
    mts.cdmat_ok = xpost_object_get_type(mts.metrics) == dicttype
                && _char_device_matrix(ctx, gs, fontdict, mts.cdmat);

    cstr = xpost_string_allocate_cstring(ctx, str);
    if (!cstr)
        return VMerror;
    chend = cstr + str.comp_.sz;

    memset(&oc, 0, sizeof oc);
    oc.ctx = ctx;
    oc.nm = xpost_object_cvlit(name_m);
    oc.nl = xpost_object_cvlit(name_l);
    oc.nc = xpost_object_cvlit(name_c);
    oc.nh = xpost_object_cvlit(name_h);

    for (ch = cstr; ch < chend; ch++)
    {
        unsigned int glyph_index;
        long advance_x, advance_y;
        long sbx = 0, sby = 0;
        Xpost_Object gname;
        Xpost_Font_Outline_Sink sink;

        gname = _encoded_name(ctx, encoding, (unsigned char)*ch);
        glyph_index = _glyph_index_for_char(ctx, encoding, charstrings,
                                            data.face, (unsigned char)*ch);
        /* a /Metrics entry may put this glyph somewhere other than where
           the face draws it, as it does on the routes that paint one.
           Read before the outline is walked, since reading it loads the
           glyph. The shift belongs to this glyph alone, so the collector
           starts it from the shifted origin and the pen the next glyph
           starts from keeps none of it. */
        _metrics_sidebearing(ctx, &mts, gname, data.face, glyph_index,
                             &sbx, &sby);
        oc.px = penx + sbx / 65536.0;
        oc.py = peny + sby / 65536.0;
        sink.moveto = _oc_moveto;
        sink.lineto = _oc_lineto;
        sink.curveto = _oc_curveto;
        sink.closepath = _oc_closepath;
        sink.user = &oc;
        if (!xpost_font_face_glyph_outline(data.face, glyph_index, &sink, &advance_x, &advance_y))
        {
            /* a glyph without an outline leaves no path; skip it */
            free(oc.objs);
            free(cstr);
            return invalidfont;
        }
        if (oc.err)
        {
            free(oc.objs);
            free(cstr);
            return oc.err;
        }
        /* and a /Metrics entry for it overrides the face's advance, so
           the glyph after it starts where show would start it */
        _metrics_advance(ctx, &mts, gname, &advance_x, &advance_y);
        penx += advance_x / 65536.0;
        peny += advance_y / 65536.0;
    }
    free(cstr);

    ret = _oc_array(ctx, &oc, &arr);
    if (ret)
        return ret;
    xpost_stack_push(ctx->lo, ctx->os, arr);
    return 0;
}

/* One glyph's outline, in the form .stringoutline gives a string's,
   and the advance it moves the pen by, in the same y-up glyph space
   the outline's points are in. The glyph is selected the way the
   caller selects one, by name or by index, and the advance comes with
   it either way because a string's comes from stringwidth, which has
   no form that names a glyph or numbers one. The key is what the
   font's /Metrics dictionary names this glyph's metrics under, as it
   is on the route that paints one. */
static
int _glyphoutline_common(Xpost_Context *ctx,
                         Xpost_Object gname,
                         int byname,
                         unsigned int gid,
                         Xpost_Object glyphkey)
{
    Xpost_Object gs;
    Xpost_Object fontdict;
    Xpost_Object devdic;
    struct fontdata data;
    textstate ts;
    outlinecollect oc;
    Xpost_Object arr;
    long advance_x = 0, advance_y = 0;
    int ret;

    gs = _gstate(ctx);
    fontdict = xpost_dict_get(ctx, gs, name_currfont);
    if (xpost_object_get_type(fontdict) == invalidtype)
        return invalidfont;
    devdic = xpost_dict_get(ctx, gs, name_device);
    ts = _text_state_get(ctx, gs, fontdict, devdic);

    ret = _font_data_current(ctx, gs, fontdict, &data);
    if (ret)
        return ret;

    memset(&oc, 0, sizeof oc);
    oc.ctx = ctx;
    oc.nm = xpost_object_cvlit(name_m);
    oc.nl = xpost_object_cvlit(name_l);
    oc.nc = xpost_object_cvlit(name_c);
    oc.nh = xpost_object_cvlit(name_h);
    {
        Xpost_Font_Outline_Sink sink;
        unsigned int glyph_index;
        long sbx = 0, sby = 0;

        glyph_index = byname
            ? _glyph_index_for_name(ctx, ts.charstrings, data.face, gname)
            : gid;
        /* a /Metrics entry may put the glyph somewhere other than where
           the face draws it, as it does on the raster route. The
           collector's origin carries the shift, so every point the walk
           reports arrives already moved; the outline's points are y-up,
           which is the convention the shift comes back in. */
        if (_metrics_sidebearing(ctx, &ts, glyphkey,
                                 data.face, glyph_index, &sbx, &sby))
        {
            oc.px = sbx / 65536.0;
            oc.py = sby / 65536.0;
        }
        sink.moveto = _oc_moveto;
        sink.lineto = _oc_lineto;
        sink.curveto = _oc_curveto;
        sink.closepath = _oc_closepath;
        sink.user = &oc;
        if (!xpost_font_face_glyph_outline(data.face, glyph_index, &sink,
                                           &advance_x, &advance_y))
        {
            free(oc.objs);
            return invalidfont;
        }
        if (oc.err)
        {
            free(oc.objs);
            return oc.err;
        }
    }
    /* a /Metrics entry for this glyph overrides the face's advance,
       as it does on the raster route */
    _metrics_advance(ctx, &ts, glyphkey, &advance_x, &advance_y);

    ret = _oc_array(ctx, &oc, &arr);
    if (ret)
        return ret;
    xpost_stack_push(ctx->lo, ctx->os, arr);
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)(advance_x / 65536.0)));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)(advance_y / 65536.0)));
    return 0;
}

/* name  .glyphoutline  array advx advy
   The named glyph's outline and advance. glyphshow selects a glyph by
   name rather than by code, so the outline is taken by name here as
   well. */
static
int _glyphoutline(Xpost_Context *ctx,
                  Xpost_Object gname)
{
    return _glyphoutline_common(ctx, gname, 1, 0, gname);
}

/* cid index  .glyphoutlineidx  array advx advy
   The outline and advance of the glyph at the given index in the
   current font's face. The composite font machinery reaches glyphs by
   index once a CMap has resolved the character code, and reaches
   their outlines the same way -- carrying the CID alongside, as the
   route that paints the glyph does, because that is what the
   descendant's /Metrics dictionary keys it under (PLRM 5.11.3). */
static
int _glyphoutlineidx(Xpost_Context *ctx,
                     Xpost_Object cid,
                     Xpost_Object gidx)
{
    /* The index is carried on to the face as an unsigned int, so one
       that does not fit is refused rather than narrowed, exactly as the
       route that paints the glyph refuses it. A wide build's integer is
       wider than that field: narrowed, an index past the top lands back
       inside the range and names some other glyph, so a character with
       no glyph is handed the outline and the advance of one that has,
       with no error to say so. */
    if (gidx.int_.val < 0)
        return rangecheck;
    if ((integer)(unsigned int)gidx.int_.val != gidx.int_.val)
        return rangecheck;
    return _glyphoutline_common(ctx, null, 0,
                                (unsigned int)gidx.int_.val, cid);
}

/* --- the language's cache parameters ---------------------------------
   The operators that report and set the bounds on what the glyph cache
   holds. The bounds are real: the cache is held to what these say. */

/* -  .cachestatus  bsize bmax msize mmax csize cmax blimit
   the glyph cache's actual figures */
static
int _cachestatus(Xpost_Context *ctx)
{
    long v[7];

    xpost_font_cache_status(&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
    {
        int i;

        for (i = 0; i < 7; i++)
            if (!xpost_stack_push(ctx->lo, ctx->os,
                                  xpost_int_cons((integer)v[i])))
                return stackoverflow;
    }
    return 0;
}

/* num  .setcachelimit  -
   the byte ceiling above which a glyph renders uncached

   A ceiling outside the range the store offers is substituted by the
   nearest one it does, which the setter does, and is not an error here:
   the operator raises stackunderflow and typecheck and nothing besides
   (PLRM 8.2).

   The ceiling is the MaxFontItem user parameter, held by the context (PLRM
   8.2 setcachelimit: maintained separately for each context, and subject to
   save and restore). It is recorded there as well as written through to the
   store, so that the save level this runs at can put it back and a second
   context is not made to inherit it. MaxFontItem is a USER parameter and
   carries none of the system-administrator restriction the cache size does. */
static
int _setcachelimit(Xpost_Context *ctx, Xpost_Object n)
{
    ctx->maxfontitem =
        (integer)xpost_font_cache_setlimit((long)n.int_.val);
    return 0;
}

/* size lower upper  .setcacheparams  -
   the cache's byte capacity and per-glyph ceiling; the middle
   operand, a compression threshold, is accepted and recorded nowhere
   since rasters stay flat

   The capacity is the MaxFontCache system parameter, and PLRM 8.2
   setcacheparams allows it to be changed only in a system administrator
   job -- invalidaccess is among the operator's errors for exactly this.
   Which runs are that job is PLRM C.3.1's question, answered by which
   password startjob was given: the system parameter password starts one,
   the start job password starts an ordinary unencapsulated job, and an
   ordinary one may alter initial VM without being allowed to change a
   limit every job after it will run under. So a request that states a
   capacity is refused unless the run is an administrator job, and refused
   whole -- the per-glyph ceiling in the same request is not applied
   either, an operator that raises an error having done nothing.

   Where no system parameter password is set the two tiers are one and
   every unencapsulated run is an administrator job, which is the factory
   default C.3.1 describes.

   A request that states NO capacity carries a zero here (data/font.ps
   fills the absent operands in) and is not a change to the cache size, so
   it is allowed from any job: it sets only MaxFontItem, which is a user
   parameter and is not restricted.

   The ceiling this settles on is recorded in the context for the reason
   .setcachelimit records it. */
static
int _setcacheparams(Xpost_Context *ctx,
                     Xpost_Object size,
                     Xpost_Object lower,
                     Xpost_Object upper)
{
    if (size.int_.val != 0 && !XPOST_MAY_SET_SYSTEM_PARAM(ctx))
        return invalidaccess;
    ctx->maxfontitem =
        (integer)xpost_font_cache_setparams((long)size.int_.val,
                                            (long)lower.int_.val,
                                            (long)upper.int_.val);
    return 0;
}

/* Installs the font, text and glyph operators, and the names they read
   out of a font dictionary. */
int xpost_oper_init_font_ops(Xpost_Context *ctx,
                             Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    /* The names these operators reach objects by, taken up once, here:
       the text machinery asks them of the graphics state, the font and
       the device for every string it measures or paints -- the key a
       device declaring it takes a glyph's coverage whole is asked for
       by is one of them -- and a lookup by text there would be part of
       what drawing a page of text costs. */
    {
        size_t ni;

        for (ni = 0; ni < sizeof(_op_font_names) / sizeof(*_op_font_names); ni++)
        {
            *_op_font_names[ni].slot =
                xpost_name_cons(ctx, _op_font_names[ni].spelling);
            if (xpost_object_get_type(*_op_font_names[ni].slot) == invalidtype)
                return VMerror;
        }
    }

    op = xpost_operator_cons(ctx, ".fonthostfaces",
                             (Xpost_Op_Func)_fonthostfaces, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".fonthostface",
                             (Xpost_Op_Func)_fonthostface, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".fontnameavailable",
                             (Xpost_Op_Func)_fontnameavailable, 1, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".fontnameavailable",
                             (Xpost_Op_Func)_fontnameavailable, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "findfont", (Xpost_Op_Func)_findfont, 1, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "findfont", (Xpost_Op_Func)_findfont, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".faceencoding", (Xpost_Op_Func)_faceencoding, 2,
            dicttype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".loadfont42", (Xpost_Op_Func)_loadfont42, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, "setfont", (Xpost_Op_Func)_setfont, 1, dicttype);
    INSTALL;

    op = xpost_operator_cons(ctx, "show", (Xpost_Op_Func)_show, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".glyphshow", (Xpost_Op_Func)_glyphshow, 1, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".glyphshowidx", (Xpost_Op_Func)_glyphshowidx, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".loadcidfont0", (Xpost_Op_Func)_loadcidfont0, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".stencilaa", (Xpost_Op_Func)_stencilaa, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".loadfont1", (Xpost_Op_Func)_loadfont1, 2,
            dicttype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".loadcidfont2", (Xpost_Op_Func)_loadcidfont2, 2,
            dicttype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "ashow", (Xpost_Op_Func)_ashow, 3,
        floattype, floattype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "widthshow", (Xpost_Op_Func)_widthshow, 4,
        floattype, floattype, integertype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "awidthshow", (Xpost_Op_Func)_awidthshow, 6,
        floattype, floattype, integertype,
        floattype, floattype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "stringwidth", (Xpost_Op_Func)_stringwidth, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".stringoutline", (Xpost_Op_Func)_stringoutline, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".glyphoutline", (Xpost_Op_Func)_glyphoutline, 1, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".glyphoutlineidx", (Xpost_Op_Func)_glyphoutlineidx, 2, integertype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".cachestatus", (Xpost_Op_Func)_cachestatus, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".maskcachehit", (Xpost_Op_Func)_maskcachehit, 5,
        floattype, floattype, arraytype, arraytype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".newfontserial", (Xpost_Op_Func)_newfontserial, 0); INSTALL;
    op = xpost_operator_cons(ctx, ".newfontid", (Xpost_Op_Func)_newfontid, 0); INSTALL;
    op = xpost_operator_cons(ctx, ".setglyphadvance", (Xpost_Op_Func)_setglyphadvance, 2,
        numbertype, numbertype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".fontdirdef", (Xpost_Op_Func)_fontdirdef, 3,
        dicttype, anytype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".fontdirundef", (Xpost_Op_Func)_fontdirundef, 2,
        dicttype, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".maskcacheput", (Xpost_Op_Func)_maskcacheput, 1, dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".setcachelimit", (Xpost_Op_Func)_setcachelimit, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".setcacheparams", (Xpost_Op_Func)_setcacheparams, 3,
        integertype, integertype, integertype);
    INSTALL;

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
    xpost_dict_put(ctx, sd, name_mark, mark); */

    return 0;
}
