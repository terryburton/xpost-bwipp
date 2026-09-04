/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_generic.h
 * @brief This file provides utilify functions for all devices.
 *
 * This header provides utility functions for all devices.
 * It opens and closes the file a device writes one page to.
 * And implements lower-level sorting and polygon filling
 * routines for speed.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_DEV_GENERIC_H
#define XPOST_DEV_GENERIC_H

#include <stdio.h> /* FILE */

#include "xpost_private.h" /* XPOST_MUST_CHECK */

/** the standard output, as the file operator names it */
#define XPOST_DEV_STDOUT_NAME "%stdout"
#define XPOST_DEV_STDOUT_LEN (sizeof(XPOST_DEV_STDOUT_NAME) - 1)

/**
 * @brief open the file the page being written goes to
 *
 * The one opener a compiled device writes a page through. The name is
 * the one the page machinery settled on the device before running Emit,
 * so the page number a marker in the output name asks for is already in
 * it and every device numbers its pages alike. A device holds the stream
 * no longer than the page: it opens here, is written, and is closed
 * through xpost_device_page_close() before Emit returns.
 *
 * Returns NULL when the device carries no settled name or the name
 * cannot be opened.
 */
FILE *xpost_device_page_open(Xpost_Context *ctx, Xpost_Object devdic);

/**
 * @brief finish the file a page was written to
 *
 * Closes what xpost_device_page_open() opened. A standard stream is
 * flushed and left open, since it outlives the page.
 */
void xpost_device_page_close(FILE *f);

/**
 * @brief install operator .yxsort to improve performance of 'fill'
 *
 * also C fillpoly implementation that uses device DrawLine method.
 */
int xpost_oper_init_generic_device_ops(Xpost_Context *ctx,
                                       Xpost_Object sd);

/**
 * @brief append bytes to the pdfwrite device's content accumulator
 *
 * For the text operators, which emit glyph outlines as content-stream
 * path fragments. Answers 0 for no error, undefined when the device
 * carries no accumulator and VMerror when memory is exhausted.
 */
XPOST_MUST_CHECK int xpost_dev_pdf_append(Xpost_Context *ctx, Xpost_Object devdic,
                         const char *s, size_t n);

/**
 * @brief the graphics-state slots a PDF content stream carries
 *
 * A PDF content stream is a state machine (PDF 8.4): a colour, a line
 * width, a cap, a join, a miter limit or an ExtGState selection stands
 * until something replaces it. Each slot names one such piece of state,
 * and the accumulator remembers the operator text last written for it,
 * so a writer emits only where what it would write differs from what
 * the stream already carries.
 *
 * The fill slot is shared by every writer of a fill colour -- the
 * device's own marking methods and the glyph outlines the text
 * operators emit -- because they write into one stream and one paint
 * follows another there whichever produced it.
 */
#define XPOST_PDF_GS_FILL       0
#define XPOST_PDF_GS_STROKE     1
#define XPOST_PDF_GS_LINE       2   /**< width, cap, join and miter limit together */
#define XPOST_PDF_GS_EXTGSTATE  3
#define XPOST_PDF_GS_SLOTS      4

/**
 * @brief whether a state operator would change what the stream carries
 *
 * Answers non-zero when @p slot does not already hold the @p n bytes at
 * @p s -- in which case the caller writes them and the slot is recorded
 * as holding them -- and zero when the stream carries them already and
 * the caller may leave them out.
 *
 * The record is only ever consulted with the bytes about to be
 * appended: a caller that asks and then does not append would leave the
 * slot claiming bytes the stream does not carry, and the next paint
 * would go out without its colour.
 *
 * A device with no accumulator, or a text longer than a slot, answers
 * non-zero and records nothing: an unknown slot costs an operator and
 * never a wrong page.
 */
int xpost_dev_pdf_state(Xpost_Context *ctx, Xpost_Object devdic,
                        int slot, const char *s, size_t n);

/**
 * @brief format a number in PDF content-stream syntax
 *
 * Writes an integer when integral, else two decimals (never
 * exponential). Returns the number of bytes written.
 */
/* File a description the content will place, and answer with the number
   the content names it by. The bytes are the description's own content
   stream, written in its own space; the box is the extent they span in
   that space. A description already filed whose bytes and box are these
   is not filed again -- which is what lets the same glyph drawn a
   thousand times be written once. */
/* Whether an outline drawn again is worth a description of its own yet:
   1 with the number to place, 0 to write the outline where it stands. */
int xpost_dev_pdf_glyph_form(Xpost_Context *ctx, Xpost_Object devdic,
                             const double *bbox, const char *body,
                             size_t len, int *index);

XPOST_MUST_CHECK int xpost_dev_pdf_form_file(Xpost_Context *ctx,
                                             Xpost_Object devdic,
                                             const double *bbox,
                                             const char *body, size_t len,
                                             int *index);

/* Paint a triangle whose colour varies across it: the device components
   at its three corners, read as a plane at each pixel it covers. `pt`
   is six device coordinates, `col` three groups of `ncomp` components.
   Sets *painted to 1 where it painted and 0 where it declined, which
   leaves the caller to paint the triangle the way it would have. */
int xpost_dev_gouraud_paint(Xpost_Context *ctx, Xpost_Object devdic,
                            const double *pt, const double *col, int ncomp,
                            int *painted);

/* Take one glyph as text rather than as a shape: names the base font,
   the code, what the code is called and how wide the show took it, and
   the matrix and origin the glyph is set at. Sets *taken when the
   content took it, and leaves it clear when the caller must draw the
   glyph itself. */
int xpost_dev_pdf_text_glyph(Xpost_Context *ctx, Xpost_Object devdic,
                             const char *base, int code, const char *gname,
                             double width, const double *mat,
                             double px, double py, int *taken);

/* Close any run of text in hand, so that what follows it in the content
   is not inside a text object. */
int xpost_dev_pdf_text_flush(Xpost_Context *ctx, Xpost_Object devdic);

int xpost_dev_pdf_fmt_num(char *o, double v);

/**
 * @brief retire the page device a restore to the given save level displaces
 *
 * PLRM 6.1 keeps the page device in the graphics state, so a restore
 * back past the setpagedevice that installed one reactivates the device
 * the saved state names and deactivates the replacement. The replacement
 * holds its raster or its content accumulator outside virtual memory,
 * which the collector neither reaches nor owns, so it is released here.
 * A restore that leaves the install standing retires nothing.
 */
void xpost_device_retire_restored(Xpost_Context *ctx, unsigned int level);
void xpost_device_retire_job(Xpost_Context *ctx, unsigned int baseline_depth);

/**
 * @brief the page's ground, as channel values on the scale of @p scale
 *
 * The colour erasepage last cleared the page to, which the raster base
 * class records on the instance as it clears (data/image.ps, /Ground).
 * It is grey 1.0 through the transfer function in force (PLRM 8.2), so
 * what it comes to is the page's business rather than white by
 * assumption.
 *
 * A device whose page is a buffer of its own reads it for the pixels
 * that buffer does not hold: one outside the page, where a mark is
 * dropped and so a read answers rather than refusing, and every pixel of
 * an instance whose buffer has been released. A device that has not been
 * erased has no record, and the answer is the full scale its Create
 * filled the buffer with.
 *
 * The record is kept in the range a colour operand arrives in, which is
 * the only range every device shares, and each device folds it to the
 * channel it stores. @p scale is that device's integer channel scale,
 * the one it hands xpost_dev_num_to_scaled() -- 255 for an 8-bit
 * channel, 65535 for a 16-bit one. Folding once to a byte and stretching
 * the byte to a wider channel is not the same number as folding to the
 * wider channel, and the value a read answers is the value an erased
 * pixel of that device holds, which is whatever the device's own PutPix
 * would have written.
 */
void xpost_device_ground_scaled(Xpost_Context *ctx, Xpost_Object devdic,
                                double scale, int *r, int *g, int *b);

/**
 * @brief the page's ground, as 8-bit channel values
 *
 * xpost_device_ground_scaled() for a device whose channels are bytes,
 * which is every device here but the window one.
 */
void xpost_device_ground_channels(Xpost_Context *ctx, Xpost_Object devdic,
                                  int *r, int *g, int *b);

/**
 * @brief write one sample row of an image straight into a device raster
 *
 * The .blitrow operator, reached as a function. What the dictionary
 * carries is described where it is implemented: the device's rows, the
 * axis-aligned mapping that places the image, the region the writes go
 * through, the row of samples, and the colour tables the painter baked
 * before it started.
 *
 * It is a function as well as an operator so that a page played back
 * from a record paints its images through the same writer that painted
 * them the first time. A second implementation of sampling would be a
 * second set of rounding decisions, and a replay is held to the bytes
 * the direct painting produced.
 */
/**
 * @brief Begin a device's Create: the part every device does alike.
 *
 * A Create is asked for a page and a class, and answers an instance --
 * but the instance is made by a procedure in the class, so the C half
 * cannot finish the job in one call. It records the page on the class,
 * then arranges for the class procedure to run and for the device's own
 * continuation to run after it, handing the three operands on through
 * the operand stack because that is how the continuation is reached.
 *
 * Every device does exactly that, differing only in which continuation
 * is theirs, so it is written here once. What a device does before this
 * -- refusing a page it could not hold, say -- is its own and stays with
 * it.
 *
 * @param[in] ctx The context.
 * @param[in] width The page width the program asked for.
 * @param[in] height The page height.
 * @param[in] classdic The device class.
 * @param[in] cont_opcode The device's own continuation.
 * @return 0, or the error the class refused the page with.
 */
int xpost_dev_create_begin(Xpost_Context *ctx,
                           Xpost_Object width,
                           Xpost_Object height,
                           Xpost_Object classdic,
                           unsigned int cont_opcode);

/**
 * @brief One tuning knob of a compiled writer, as its driver states it.
 *
 * The key is the name the driver reads off its device dictionary, and
 * the row says what a value for it may be: a vocabulary of words, or an
 * integer within the range. The classes are where a default for it is
 * recorded. Each driver states its own knobs beside the reads that give
 * them meaning, and tests/check-device-facts.sh holds the statement to
 * the reads; xpost_dev_option_roster() gathers what this build compiled
 * in.
 */
typedef struct
{
    const char *key;
    const char *classname;
    const char *altclassname;   /**< second class of the same body, or NULL */
    int min;
    int max;
    const char *const *words;   /**< NULL-terminated vocabulary, or NULL */
} Xpost_Dev_Option;

const Xpost_Dev_Option *xpost_dev_png_option_roster(int *count);
const Xpost_Dev_Option *xpost_dev_jpeg_option_roster(int *count);
const Xpost_Dev_Option *xpost_dev_pdf_option_roster(int *count);
const Xpost_Dev_Option *xpost_dev_option_roster(int *count);

/**
 * @brief Record a default an embedder asks of a compiled writer.
 *
 * The value is recorded among the host's settings under @p key, and
 * written onto the named device classes where they are already
 * installed; a class installed later takes the setting up through
 * xpost_dev_class_option_default(). A page-device request naming the
 * same key still overrides it, at the copy an instance is made by.
 *
 * @param[in] ctx The context.
 * @param[in] key The key the driver reads off its device dictionary.
 * @param[in] v The value: an integer, or a word as a name object.
 * @param[in] classname The class's name in the private dictionary.
 * @param[in] altclassname A second class the same driver body makes,
 *            or NULL.
 * @return 0, or the error the recording was refused with.
 */
int xpost_dev_option_default(Xpost_Context *ctx,
                             const char *key,
                             Xpost_Object v,
                             const char *classname,
                             const char *altclassname);

/**
 * @brief Take up an embedder's recorded default as a class is installed.
 *
 * @param[in] ctx The context.
 * @param[in] classdic The device class being installed.
 * @param[in] key The key to take up.
 * @return 0, or the error the class refused the entry with.
 */
int xpost_dev_class_option_default(Xpost_Context *ctx,
                                   Xpost_Object classdic,
                                   const char *key);

int xpost_dev_blit_row(Xpost_Context *ctx, Xpost_Object dict);

/**
 * @brief whether a device's polygon fill is the one below
 *
 * A device names its methods in its own dictionary and may name
 * anything: a procedure, a fill of its own, or the compiled one every
 * raster class installs. Only the last can be reached as a function, so
 * a caller wanting to hand it a boundary directly asks this first and
 * makes an ordinary method call otherwise.
 */
int xpost_dev_fillpoly_compiled(Xpost_Object method);

/**
 * @brief fill the region a run of coordinates bounds
 *
 * The .fillpoly operator, reached as a function. @p co is a pair per
 * vertex over @p npts vertices, a subpath break written as the pair the
 * packed path writes one as, and the colour is on the operand stack as
 * the operator takes it: one value or three, according to what the
 * device paints in.
 *
 * It is a function as well as an operator so that a caller already
 * holding a boundary in this form -- a record of a page's marks holds
 * every polygon in it -- reaches the fill without building the array
 * operand the method call would take. That array is one two-element
 * array per vertex, in virtual memory, built afresh for each call; on a
 * page of paths played back a band at a time it is built once per band a
 * shape reaches, and it is several times what holding the whole page's
 * marks costs.
 *
 * The fill is the same fill either way, over the same vertices, so what
 * a page comes to does not depend on which way its boundary arrived.
 */
int xpost_dev_fillpoly_run(Xpost_Context *ctx, const real *co, int npts,
                           Xpost_Object devdic);

/**
 * @brief the threshold cell a screening device paints through
 *
 * @param[out] w the cell's width, @p h its height
 * @return the w x h thresholds, or NULL where the device carries no
 *         usable cell
 *
 * A device that renders a grey as a pattern of pixels carries
 * .htcell/.htw/.hth, and every grey written compares against the
 * threshold under its pixel. The dimensions come out of a dictionary a
 * program can build, so they are held against the cell's own length
 * here rather than trusted.
 *
 * It is shared so that a device recording a page for such a device
 * reads the screen by the same rule the device paints by: a recorder
 * that took a cell the painter would have refused would write down a
 * screen no page was ever painted under.
 */
const unsigned char *xpost_dev_ht_cell(Xpost_Context *ctx,
                                       Xpost_Object devdic, int *w, int *h);

/**
 * @brief the level a grey is screened at, on the cell's own scale
 *
 * @param v the grey, 0 for black and 1 for white, clamped here
 * @return 0 to 256
 *
 * The comparison runs on a 0..256 scale rather than 0..255 so that a
 * threshold of 255 whitens just before solid white and solid black
 * stays solid.
 *
 * A caller holding a stored byte rather than a value passes g / 255.0
 * and gets the same answer as the integer form (256 * g + 127) / 255
 * for every one of the 256 bytes -- the two round differently only for
 * a fractional part in [0.5, 0.50196), which no byte over 255 produces.
 * That is why there is one of these and not one per caller.
 */
static inline int
xpost_dev_ht_level(double v)
{
    if (v < 0.0)
        v = 0.0;
    if (v > 1.0)
        v = 1.0;
    return (int)(v * 256.0 + 0.5);
}

/**
 * @brief what a screening device writes for one pixel
 *
 * @param level the grey's level from xpost_dev_ht_level
 * @param cell the threshold cell, @p w by @p h
 * @param x @p y the pixel, in the device's own coordinates
 * @return 255 where the level reaches the threshold under the pixel,
 *         0 where it does not
 *
 * THE one place a grey meets a threshold. It was three: this
 * comparison, its cell addressing and its 255-or-0 were written out
 * again in the rectangle fill, in the blit an image goes through, and
 * in the device's own method in PostScript. Anything that screens a
 * pixel calls this, and tests/check-screen-paths.sh paints the same
 * grey by more than one route and requires the same page, so a fourth
 * copy shows up as a disagreement rather than as a second opinion.
 *
 * The cell tiles from the origin in both directions, so a pixel left of
 * or below it takes the threshold its position folds onto rather than
 * one off the end.
 */
static inline unsigned char
xpost_dev_ht_ink(int level, const unsigned char *cell,
                 int w, int h, int x, int y)
{
    int cx = x % w;
    int cy = y % h;

    if (cx < 0)
        cx += w;
    if (cy < 0)
        cy += h;
    return level >= cell[cy * w + cx] ? 255 : 0;
}

/**
 * @brief a number that dictionary carries under @p key, or @p dflt
 *
 * The blit dictionary's operands are numbers, and a number reaches a
 * device as either of the two numeric types. This reads one the way the
 * row writer reads its own, so that a caller building such a dictionary
 * or taking one apart -- a record writing an image down and playing it
 * back is both -- puts the same value in the writer's hands.
 *
 * A key the dictionary does not carry, or carries something other than
 * a number under, answers @p dflt: which is what the writer makes of an
 * operand that is not there.
 */
double xpost_dev_dict_number(Xpost_Context *ctx, Xpost_Object dict,
                             Xpost_Object key, double dflt);

/**
 * @brief report what to allocate for a raster of @p w by @p h pixels of
 *        @p pixel bytes each, or refuse a buffer this platform cannot address
 *
 * Returns non-zero having written to @p bytes the whole size to ask the
 * allocator for -- @p reserve bytes for whatever the caller keeps in
 * front of the raster, such as its buffer header, plus the raster's own
 * -- or zero having written nothing, in which case the caller answers
 * limitcheck.
 *
 * The extent asked about is the buffer's: the block that is to be
 * resident and indexed, whose row width is what a pixel's position is
 * computed against. Every device here holds its whole page in one such
 * block, so it asks about the page's extent; the question is still the
 * buffer's, and a device holding less of the page than that would ask
 * about what it holds.
 *
 * A buffer of no extent is reported rather than refused: its raster
 * comes to no bytes and the reserve is all there is to allocate, and
 * whatever a device makes of an empty page it makes on its own terms. A
 * negative extent is not an extent and is refused with the unaddressable
 * ones.
 *
 * A device reaches a pixel by its position within the buffer, counted in
 * the width the platform expresses the size of a block of memory in. A
 * buffer whose pixel count, byte count or allocation size runs past that
 * width has no address for its far end however much memory is to hand,
 * so it is refused here rather than allocated and then indexed past: the
 * caller answers with limitcheck, which PLRM 8.2 gives for a limit of
 * the implementation rather than of the machine. What the machine will
 * actually give is the allocator's answer and not this one -- a size
 * this reports is a size that can be expressed and addressed, not a size
 * that is available -- and a caller whose allocation then fails answers
 * VMerror.
 *
 * Refusing before allocating also keeps a page nobody can reach from
 * asking the system for the memory to hold it.
 */
int xpost_device_raster_bytes(int w, int h, size_t pixel, size_t reserve,
                              size_t *bytes);

/**
 * @brief The block a raster of that many bytes sits in.
 *
 * A size expresses a raster no machine holds, so a count above half the
 * address space is answered here rather than put to an allocator: the
 * refusal then names the page it came from, and the devices that keep a
 * buffer of their own take their block the one way.
 *
 * @param[in] bytes what the raster and whatever sits in front of it come to
 * @return the block, or NULL for a count no allocator hands out
 */
void *xpost_device_raster_block(size_t bytes);

/**
 * @}
 */

#endif
