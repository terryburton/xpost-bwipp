/*
 * The arrangement of the buffer a raster device hands its embedder.
 *
 * These devices exist to give a program that linked the library a page
 * as one block of memory in a named arrangement of components. The name
 * after the colon in the device string -- raster:rgb, raster:argb,
 * raster:bgr, raster:bgra -- is the whole of what the family promises,
 * and the standalone bgr device makes the same promise under its own
 * name. That arrangement is observable in one place only: the pointer a
 * run started with XPOST_OUTPUT_BUFFEROUT stores back through the
 * address the embedder handed it.
 *
 * Nothing reachable from a program can observe it. The page is read from
 * PostScript through GetPix, which answers red, green and blue whatever
 * the buffer holds them as, so a device whose store and whose load agree
 * on a wrong arrangement reads back correct through every
 * PostScript-level test in the suite while the bytes the embedder
 * receives name a different colour. The only reading that settles it is
 * the embedder's own -- bytes at offsets -- and that is what this is.
 *
 * What the colours have to be. Three properties, and the reading means
 * nothing without them:
 *
 *   The three components of a colour differ from one another. A colour
 *   with two equal components is unchanged by the exchange of those
 *   two, so a page painted a pure primary cannot tell rgb from a device
 *   that swapped the two channels that are zero in it -- two of the six
 *   orderings of a primary are the same three bytes.
 *
 *   None of them is 255, which is what the alpha byte carries. A colour
 *   with full red in it cannot tell argb from a device that wrote red
 *   where the alpha goes, because both bytes read 255.
 *
 *   The two colours share no component, so no byte of one can pass for
 *   a byte of the other and a row read at the wrong offset is a row
 *   that disagrees.
 *
 * 0.75 0.5 0.25 folds to 191 127 63 and 0.125 0.625 0.375 to 31 159 95;
 * with the alpha's 255 that is seven values, no two alike, and each is
 * held at the one offset of the one format that is supposed to carry
 * it. So no rearrangement of a pixel's bytes, and no exchange of a
 * component with the alpha, leaves a buffer reading as it should.
 *
 * What is painted, and by what. A device stores a pixel through three
 * separate methods, each carrying its own statement of the arrangement:
 * the rectangle fill, the single-pixel store, and the coverage-weighted
 * store the text operators reach for a glyph's edges. One of them right
 * and another wrong is a page whose fills are the colour asked for and
 * whose text is not, so each paints a row of its own here and every row
 * is read. They are called at device coordinates rather than reached
 * through the painting operators because what is being asked is where a
 * byte lands, and a coordinate that arrives already in the device's own
 * space is the one that names the offset. That the painting operators
 * reach these methods at all is tests/run-raster-formats-test.sh's.
 *
 * The rows nothing paints are read too. They carry the white the page
 * was erased to, which says the buffer covers the whole page and that
 * the stride between rows is the pixel size the format names -- a
 * four-byte format sized at three would shift every row after the first.
 * White is the same bytes under every rearrangement and so settles no
 * ordering; the ordering is settled by the painted rows alone.
 *
 * When it is read, and what becomes of it. The buffer is the embedder's
 * from the handoff, and the interpreter is not what holds it: each page
 * here is read after the context that painted it has been destroyed,
 * which is what an embedder that keeps a rendering and ends the job
 * does, and a page that did not outlive its context would be read as
 * freed memory. It is then given back with xpost_output_buffer_release()
 * -- the only call in this process that gives it back, so what a leak
 * checker says about this run is a statement about that call.
 *
 * What this does not cover: whether the marks are in the right places
 * (the golden-render manifest's), the file-writing devices' formats
 * (tests/run-raster-formats-test.sh's), and the compiled image writers,
 * whose bytes come from the library that encodes them.
 */

#include <stdio.h>

#include "xpost.h"

#include "xpost_test.h"

/* Small enough that every byte of every buffer is read, and tall enough
   to hold a row for each store method with rows left over that nothing
   paints. */
#define PAGE_W 8
#define PAGE_H 6

/* Where a pixel's bytes come from. The fourth position of a
   three-component arrangement is never reached: a pixel is read to the
   width its format names. */
#define RED   0
#define GREEN 1
#define BLUE  2
#define ALPHA 3
#define UNUSED (-1)

/* The two colours, as the components a device folds 0.75 0.5 0.25 and
   0.125 0.625 0.375 to, the white a page is erased to, and the value an
   alpha byte carries. */
static const int colour_a[3] = { 191, 127, 63 };
static const int colour_b[3] = { 31, 159, 95 };
static const int white[3] = { 255, 255, 255 };
#define OPAQUE 255

/* One run: the device string it is started with, and the arrangement
   that device's name promises -- how many bytes a pixel takes, and what
   each of those bytes holds. */
typedef struct
{
    const char *device;
    int bpp;
    int order[4];
} Format;

static const Format formats[] =
{
    { "raster:rgb",  3, { RED, GREEN, BLUE, UNUSED } },
    { "raster:argb", 4, { ALPHA, RED, GREEN, BLUE } },
    { "raster:bgr",  3, { BLUE, GREEN, RED, UNUSED } },
    { "raster:bgra", 4, { BLUE, GREEN, RED, ALPHA } },

    /* The device with no arrangement named, which takes the one the
       family falls back to: three bytes red green blue as the embedder
       sees them. An arrangement outside the four is refused rather than
       made, so there is no device here to read. */
    { "raster",              3, { RED, GREEN, BLUE, UNUSED } },

    /* The other device that keeps its page in a buffer of its own and
       hands it over the same way. */
    { "bgr", 3, { BLUE, GREEN, RED, UNUSED } }
};

/* The program each run executes: three rows painted through the three
   methods that store a pixel, at device coordinates, and then the page
   transmitted so the buffer pointer reaches the embedder.

   The rightmost column is written in from the page width the contexts
   are created at rather than spelled here, so the program cannot come
   to span a different page from the one the reading walks.

   The page is not erased on the way out: showpage under the non-pausing
   page semantics transmits the page and ends it without clearing it, so
   what was painted is still in the buffer when the run returns. */
static const char paint_program[] =
    "/dev DEVICE def\n"
    "/last %d def\n"
    /* row 0, the rectangle fill: one call spanning the row, the far
       corner of a fill being the pixel it names rather than the one
       past it */
    "0.75 0.5 0.25 0 0 last 0 dev dup /FillRect get exec\n"
    /* row 1, the single-pixel store */
    "0 1 last { /px exch def\n"
    "  0.125 0.625 0.375 px 1 dev dup /PutPix get exec } for\n"
    /* row 2, the coverage-weighted store at full coverage, which leaves
       the colour itself whatever the ground under it was */
    "0 1 last { /px exch def\n"
    "  0.75 0.5 0.25 255 px 2 dev dup /BlendPix get exec } for\n"
    "showpage\n";

/* What a device row was painted, by the program above. */
static const int *row_colour(int y)
{
    switch (y)
    {
        case 0: return colour_a;
        case 1: return colour_b;
        case 2: return colour_a;
        default: return white;
    }
}

/* The byte a format puts at one position of a pixel of one colour. */
static int byte_of(const Format *f, const int *colour, int position)
{
    int which = f->order[position];

    return which == ALPHA ? OPAQUE : colour[which];
}

/* A pixel written out as the bytes a format gives it, for a report.
   Enough room for four three-digit numbers and the spaces between. */
#define PIXEL_TEXT 20

static const char *pixel_text(char *out, const Format *f, const int *bytes)
{
    int i, n = 0;

    for (i = 0; i < f->bpp; i++)
        n += snprintf(out + n, (size_t)(PIXEL_TEXT - n), "%s%d",
                      i ? " " : "", bytes[i]);
    return out;
}

/* Read one run's buffer against the arrangement its device's name
   promises: every pixel of every row, byte by byte. The first byte that
   disagrees is reported with the position it sits at and the whole pixel
   it belongs to, which is what says which rearrangement happened. */
static void buffer_holds(const Format *f, const unsigned char *buf)
{
    int x, y, i;

    for (y = 0; y < PAGE_H; y++)
    {
        const int *colour = row_colour(y);

        for (x = 0; x < PAGE_W; x++)
        {
            const unsigned char *pixel = buf + (size_t)(y * PAGE_W + x) * f->bpp;
            int got[4], want[4];

            for (i = 0; i < f->bpp; i++)
            {
                got[i] = pixel[i];
                want[i] = byte_of(f, colour, i);
            }
            for (i = 0; i < f->bpp; i++)
            {
                char got_text[PIXEL_TEXT], want_text[PIXEL_TEXT];

                if (got[i] == want[i])
                    continue;

                report_failure("%s: the pixel at (%d,%d) reads %s where %s"
                               " was painted (byte %d of %d)",
                               f->device, x, y,
                               pixel_text(got_text, f, got),
                               pixel_text(want_text, f, want),
                               i, f->bpp);
                return;
            }
        }
    }
}

/* One device: create a context whose output is a buffer, paint through
   it, and read what came back. */
static void run_format(const Format *f)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;
    unsigned char *buf = NULL;
    char program[sizeof paint_program + 32];

    if (snprintf(program, sizeof program, paint_program, PAGE_W - 1)
        >= (int)sizeof program)
    {
        report_failure("%s: the painting program does not fit", f->device);
        return;
    }

    ctx = xpost_create(f->device, XPOST_OUTPUT_BUFFEROUT, &buf,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, PAGE_W, PAGE_H);
    if (!ctx)
    {
        report_failure("%s: no context", f->device);
        return;
    }
    xpost_job_snapshots_set(ctx, 0);

    st = xpost_run(ctx, XPOST_INPUT_STRING, program, 0);
    if (st != XPOST_RUN_COMPLETE)
    {
        report_failure("%s: the painting run did not complete", f->device);
        xpost_destroy(ctx);
        xpost_output_buffer_release(&buf);
        return;
    }

    /* A run that never reached the handoff leaves the pointer as it was,
       and reading through it would be reading the embedder's own
       uninitialised memory rather than a page. */
    if (!buf)
    {
        report_failure("%s: the run handed back no buffer", f->device);
        xpost_destroy(ctx);
        return;
    }

    /* The page is the embedder's from the handoff on, and the context is
       not what holds it: it is read here after the context that painted
       it has gone, because that is what an embedder that keeps a
       rendering and ends the job does. Then it is given back, which is
       the only call in this process that gives it back -- so a leak
       checker watching this run has the last word on whether it was. */
    xpost_destroy(ctx);
    buffer_holds(f, buf);

    xpost_output_buffer_release(&buf);
    if (buf)
        report_failure("%s: the released pointer still names memory",
                       f->device);

    /* the same call again, on the variable it cleared: there is nothing
       left to give back and it is not given back twice */
    xpost_output_buffer_release(&buf);
}

int main(void)
{
    size_t i;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    /* nowhere to read a pointer from is nothing to give back */
    xpost_output_buffer_release(NULL);

    /* One interpreter instance lives at a time, so the runs are
       sequential: each device is created, painted through and destroyed
       before the next is asked for. */
    for (i = 0; i < sizeof(formats) / sizeof(*formats); i++)
        run_format(&formats[i]);

    xpost_quit();

    return verdict();
}
