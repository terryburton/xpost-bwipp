/* Two fresh boots of the interpreter, and what their virtual memory has
 * in common.
 *
 * A memory image written at start-up and read back in a later process
 * would be worth having only if what it holds is a function of the
 * language rather than of the run that produced it. The gate for that is
 * idempotence -- write an image, read it, write another, require the two
 * to match to the byte -- and the gate rests on something nobody had
 * measured: that two fresh boots produce the same virtual memory in the
 * first place. This is that measurement.
 *
 * The image is taken where a run brings its context up: after the
 * language has loaded and been locked down, and before the run's device
 * is made. That is the last point at which nothing of the run has been
 * decided.
 *
 * WHAT THIS ESTABLISHES. Run as a pair of processes, it says whether
 * their virtual memory differs, and where. It holds the two to being
 * identical in every part that carries no host address at all: the size
 * of each bank, the bookkeeping each carries, every row of both entity
 * tables, and every byte of local memory. In the global bank, where the
 * operator table's rows carry the addresses of the C functions that
 * implement the operators, it holds every difference to being one of
 * those addresses: a word whose two values keep the same offset within
 * a page and differ by the one distance between where the two processes
 * put this code. Anything else -- a count, an index, a length, an
 * ordering, storage nobody wrote -- fails that, because none of those
 * moves by a page-aligned distance and none of them keeps its page
 * offset.
 *
 * WHAT IT DOES NOT ESTABLISH, plainly:
 *
 *   It is a comparison and not a proof of what an image would need. Two
 *   runs that agree say nothing about a third under different arguments,
 *   a different device or a different data directory. The pair here is
 *   deliberately identical in all of those, so what is measured is the
 *   process and nothing else.
 *
 *   It does not say the differing words are unreachable. It says only
 *   that each is where an operator's signature keeps one of this
 *   process's own functions. A word that differs anywhere else is a
 *   failure, whether or not anything can still read it: storage handed
 *   out to a new occupant is cleared of what the last one left, so a
 *   host address outside a live signature has nowhere to have come from
 *   that this run understands.
 *
 *   It is blind where the two runs put this code at the same address:
 *   the difference it exists to characterise is then not there to see.
 *   That case is recognised rather than passed over -- the whole of both
 *   images is then required to match to the byte, which is the stronger
 *   answer -- and the run says which of the two it gave.
 *
 *   An image of one object width is not comparable with one of the
 *   other: the objects in it are a different size and the structures
 *   around them a different shape. Two images are refused unless they
 *   agree with each other and with this build.
 *
 * THE PAIR IN ONE PROCESS. The same two boots taken one after the other
 * in a single process answer a second question, and answer the first one
 * without any excuse. A context created after another has lived and died
 * boots from a heap that has already been used rather than a fresh one,
 * so it says whether what a boot arrives at is a function of the language
 * or of what the process did before it; and the two boots are the same
 * load of this code at one address, so there is no relocation to allow
 * for. Nothing in either bank may differ, down to the byte, and the
 * comparison of the two is the same comparison as for the pair of
 * processes -- which reads a slide of zero and requires exactly that.
 *
 * Modes:
 *   write <path>        boot to the point above and write the image
 *   write2 <a> <b>      two boots in this one process, an image of each
 *   compare <a> <b>     read two images and judge them
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_context.h"
#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_vm_image.h"

#include "xpost_test.h"

/* Floors. A comparison that finds no operator signature to read is
   reading something other than an image of this interpreter, and would
   report nothing wrong about it. */
#define SIGNATURES_AT_LEAST 100
#define FUNCTIONS_AT_LEAST 50

/* How much of a difference to spell out before the report is just
   volume. The counts above it are the whole population either way. */
#define DETAIL_LINES 8

typedef struct
{
    char name[16];
    unsigned int field[XPOST_VM_IMAGE_BANK_FIELDS];
    const unsigned char *rows;   /* nextent * XPOST_VM_IMAGE_ROW_FIELDS */
    const unsigned char *arena;  /* used bytes */
} Bank;

typedef struct
{
    unsigned char *bytes;
    size_t len;
    unsigned int stamp[XPOST_VM_IMAGE_STAMPS];
    Bank bank[XPOST_VM_IMAGE_BANKS];
} Image;

/* Every number in an image is four bytes at whatever offset it fell on:
   an arena is as long as it is, so the bank after one begins wherever
   that leaves off. Read by copy rather than through a pointer of the
   type. */
static unsigned int _u32(const unsigned char *p)
{
    unsigned int v;

    memcpy(&v, p, sizeof v);
    return v;
}

static unsigned int _row(const Bank *b, unsigned int ent, unsigned int field)
{
    return _u32(b->rows + (ent * XPOST_VM_IMAGE_ROW_FIELDS + field)
                          * sizeof(unsigned int));
}

static unsigned int _bank_used(const Bank *b)
{
    return b->field[XPOST_VM_IMAGE_BANK_HIGH_WATER];
}

static unsigned int _bank_nextent(const Bank *b)
{
    return b->field[XPOST_VM_IMAGE_BANK_NEXTENT];
}

static int _read_image(const char *path, Image *im)
{
    FILE *f;
    long len;
    size_t got;
    size_t at;
    unsigned int i;

    memset(im, 0, sizeof *im);

    f = fopen(path, "rb");
    if (!f)
    {
        report_failure("cannot open the image %s", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0)
    {
        report_failure("cannot measure the image %s", path);
        fclose(f);
        return 0;
    }
    im->len = (size_t)len;
    im->bytes = malloc(im->len ? im->len : 1);
    if (!im->bytes)
    {
        report_failure("cannot hold the image %s in memory", path);
        fclose(f);
        return 0;
    }
    got = fread(im->bytes, 1, im->len, f);
    fclose(f);
    if (got != im->len)
    {
        report_failure("the image %s is shorter than it says", path);
        return 0;
    }

    at = XPOST_VM_IMAGE_MAGIC_LEN + XPOST_VM_IMAGE_STAMPS * sizeof(unsigned int);
    if (im->len < at ||
        memcmp(im->bytes, XPOST_VM_IMAGE_MAGIC, XPOST_VM_IMAGE_MAGIC_LEN) != 0)
    {
        report_failure("%s does not begin as an image of virtual memory", path);
        return 0;
    }
    for (i = 0; i < XPOST_VM_IMAGE_STAMPS; i++)
        im->stamp[i] = _u32(im->bytes + XPOST_VM_IMAGE_MAGIC_LEN
                            + i * sizeof(unsigned int));

    if (im->stamp[XPOST_VM_IMAGE_STAMP_VERSION] != XPOST_VM_IMAGE_VERSION)
    {
        report_failure("%s is version %u of the layout and this reads "
                       "version %u", path,
                       im->stamp[XPOST_VM_IMAGE_STAMP_VERSION],
                       (unsigned int)XPOST_VM_IMAGE_VERSION);
        return 0;
    }
    if (im->stamp[XPOST_VM_IMAGE_STAMP_BANKS] != XPOST_VM_IMAGE_BANKS)
    {
        report_failure("%s holds %u banks and this reads %u", path,
                       im->stamp[XPOST_VM_IMAGE_STAMP_BANKS],
                       (unsigned int)XPOST_VM_IMAGE_BANKS);
        return 0;
    }
    /* The comparison below reads the operator table out of the arena as
       this build's structures. An image of the other object width is a
       different shape and would be read as nonsense rather than
       refused. */
    if (im->stamp[XPOST_VM_IMAGE_STAMP_OBJECT_SIZE]
        != (unsigned int)sizeof(Xpost_Object))
    {
        report_failure("%s was written by a build whose objects are %u bytes "
                       "and this build's are %u; an image of one object width "
                       "is not comparable with the other",
                       path, im->stamp[XPOST_VM_IMAGE_STAMP_OBJECT_SIZE],
                       (unsigned int)sizeof(Xpost_Object));
        return 0;
    }
    /* The operator rows, which this steps over: what they are for is
       holding a reader's own operator table to the one the image was
       written with, and this compares two images rather than reading
       either into a context. A row is what the operator states, then the
       procedure it runs if it is a wrapped one, then its name padded out
       to a whole number of values. */
    for (i = 0; i < im->stamp[XPOST_VM_IMAGE_STAMP_OPERATORS]; i++)
    {
        unsigned int namelen;
        size_t rowat = at;
        size_t fixed = 3 * sizeof(unsigned int) + sizeof(Xpost_Object);

        if (im->len < at + fixed)
        {
            report_failure("%s ends among its operator rows", path);
            return 0;
        }
        namelen = _u32(im->bytes + at + 2 * sizeof(unsigned int)
                       + sizeof(Xpost_Object));
        at += fixed + namelen + (4u - (namelen % 4u)) % 4u;
        {
            /* then one operand shape per signature: how many operands it
               takes, and that many type bytes padded out to a value */
            unsigned int si;
            unsigned int nsig = _u32(im->bytes + rowat);

            for (si = 0; si < nsig; si++)
            {
                unsigned int in;

                if (im->len < at + sizeof(unsigned int))
                {
                    report_failure("%s ends among its operand shapes", path);
                    return 0;
                }
                in = _u32(im->bytes + at);
                at += sizeof(unsigned int) + in + (4u - (in % 4u)) % 4u;
            }
        }
    }
    /* and the context's own share of what the boot settled */
    at += im->stamp[XPOST_VM_IMAGE_STAMP_CONTEXT_FIELDS] * sizeof(unsigned int);
    at += (size_t)(im->stamp[XPOST_VM_IMAGE_STAMP_ROOTS]
                   + im->stamp[XPOST_VM_IMAGE_STAMP_TYPENAMES])
          * sizeof(Xpost_Object);
    if (im->len < at)
    {
        report_failure("%s ends among what it says about the context", path);
        return 0;
    }

    for (i = 0; i < XPOST_VM_IMAGE_BANKS; i++)
    {
        Bank *b = &im->bank[i];
        unsigned int j;
        size_t rows;

        if (im->len < at + 8 + XPOST_VM_IMAGE_BANK_FIELDS * sizeof(unsigned int)
                        + XPOST_VM_IMAGE_FILE_BIRTHS * sizeof(unsigned int))
        {
            report_failure("%s ends in the middle of the %s bank", path,
                           xpost_vm_image_bank_name(i));
            return 0;
        }
        memcpy(b->name, im->bytes + at, 8);
        b->name[8] = '\0';
        at += 8;
        for (j = 0; j < XPOST_VM_IMAGE_BANK_FIELDS; j++)
        {
            b->field[j] = _u32(im->bytes + at);
            at += sizeof(unsigned int);
        }
        at += XPOST_VM_IMAGE_FILE_BIRTHS * sizeof(unsigned int);

        rows = (size_t)_bank_nextent(b) * XPOST_VM_IMAGE_ROW_FIELDS
               * sizeof(unsigned int);
        if (im->len < at + rows + _bank_used(b))
        {
            report_failure("%s says its %s bank holds %u entities over %u "
                           "bytes and the file is too short for them",
                           path, xpost_vm_image_bank_name(i),
                           _bank_nextent(b), _bank_used(b));
            return 0;
        }
        b->rows = im->bytes + at;
        at += rows;
        b->arena = im->bytes + at;
        at += _bank_used(b);
    }

    /* the digest of everything above it is the last thing in the file */
    if (at + sizeof(unsigned int) != im->len)
    {
        report_failure("%s carries %lu bytes past what it describes", path,
                       (unsigned long)(im->len - at));
        return 0;
    }
    return 1;
}

/* Where a live operator signature is: the operator table is a special
   entity of global memory whose rows each name a run of signatures by
   its address in the arena. The row's recorded size is zeroed so the
   collector passes it over, so how much of the arena it covers is read
   from what it was allocated. */
typedef struct
{
    unsigned int adr;
    unsigned int opcode;
} Signature;

/* A relocation moves an image by whole pages, so an address it moved
   keeps its offset within one. Four kilobytes is the smallest page any
   platform this builds for has; a larger one only makes this hold more
   firmly. */
#define PAGE_OFFSET_MASK 0xfffu

static void _compare_banks(const Image *a, const Image *b)
{
    unsigned int i;

    for (i = 0; i < XPOST_VM_IMAGE_BANKS; i++)
    {
        const Bank *x = &a->bank[i];
        const Bank *y = &b->bank[i];
        const char *who = xpost_vm_image_bank_name(i);
        unsigned int j;
        unsigned int ndiff = 0;

        if (memcmp(x->name, y->name, 8) != 0)
            report_failure("the two images name their bank %u differently: "
                           "%s and %s", i, x->name, y->name);

        for (j = 0; j < XPOST_VM_IMAGE_BANK_FIELDS; j++)
            if (x->field[j] != y->field[j])
                report_failure("%s bank: %s is %u in one boot and %u in the "
                               "other", who,
                               xpost_vm_image_bank_field_name(j),
                               x->field[j], y->field[j]);

        if (_bank_nextent(x) != _bank_nextent(y))
            continue;   /* the rows are not the same population to compare */

        for (j = 0; j < _bank_nextent(x); j++)
        {
            unsigned int f;

            for (f = 0; f < XPOST_VM_IMAGE_ROW_FIELDS; f++)
            {
                if (_row(x, j, f) == _row(y, j, f))
                    continue;
                ndiff++;
                if (ndiff <= DETAIL_LINES)
                    report_failure("%s bank: entity %u has %s %u in one boot "
                                   "and %u in the other", who, j,
                                   xpost_vm_image_row_field_name(f),
                                   _row(x, j, f), _row(y, j, f));
            }
        }
        if (ndiff > DETAIL_LINES)
            report_failure("%s bank: %u entity table fields differ in all",
                           who, ndiff);
    }
}

/* The local bank carries no operator table and nothing else that names
   a host address, so nothing in it may differ at all. */
static void _compare_local(const Image *a, const Image *b)
{
    const Bank *x = &a->bank[1];
    const Bank *y = &b->bank[1];
    unsigned int used = _bank_used(x);
    unsigned int off;
    unsigned int ndiff = 0;

    if (used != _bank_used(y))
        return;     /* already reported as a field difference */

    for (off = 0; off < used; off++)
        if (x->arena[off] != y->arena[off])
        {
            ndiff++;
            if (ndiff <= DETAIL_LINES)
                report_failure("local bank: byte %u is %02x in one boot and "
                               "%02x in the other", off,
                               x->arena[off], y->arena[off]);
        }
    if (ndiff > DETAIL_LINES)
        report_failure("local bank: %u bytes differ in all, where nothing in "
                       "local memory names this process", ndiff);
    if (ndiff == 0)
        printf("local bank: %u bytes, identical in both boots\n", used);
}

static void _compare_global(const Image *a, const Image *b)
{
    const Bank *x = &a->bank[0];
    const Bank *y = &b->bank[0];
    unsigned int used = _bank_used(x);
    unsigned int off;
    unsigned int ndiff = 0;

    if (used != _bank_used(y))
        return;     /* already reported as a field difference */

    /* The same demand as the local bank, and it used to be a weaker one.
       The operator table lived in this arena, and a signature carries the
       C function implementing an operator and the one checking its
       operands -- this process's addresses, which two boots put in
       different places. So the comparison had to find the table, read
       every live signature out of it, account for each differing word as
       a function pointer inside one, and prove the distance between them
       was the same everywhere: some two hundred lines, all to excuse the
       one thing in the arena that was not an entity number or an offset.

       The table is host storage now and no part of this arena, so nothing
       left in it can differ between two boots of the same build. Anything
       that does is a fault, and needs no reading of what it might have
       been. */
    for (off = 0; off < used; off++)
        if (x->arena[off] != y->arena[off])
        {
            ndiff++;
            if (ndiff <= DETAIL_LINES)
                report_failure("global bank: byte %u is %02x in one boot and "
                               "%02x in the other", off,
                               x->arena[off], y->arena[off]);
        }
    if (ndiff > DETAIL_LINES)
        report_failure("global bank: %u bytes differ in all, where nothing "
                       "in the arena names this process", ndiff);
    if (ndiff == 0)
        printf("global bank: %u bytes, identical in both boots\n", used);
}

/* One boot of the interpreter and the image of it, between an
   xpost_init and an xpost_quit the caller holds. Answers whether the
   boot reached the point the image is taken at. */
static int _boot_and_write(const char *path)
{
    Xpost_Context *ctx;
    int ok;

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        return 0;
    }

    /* The language, and then nothing: the run's device is what would be
       made next, and it is what an image must not carry. */
    xpost_interpreter_load_language(ctx);

    ok = xpost_vm_image_write(ctx, path, 1);
    if (!ok)
        report_failure("cannot write the image to %s", path);

    xpost_destroy(ctx);
    return ok;
}

static void _write(const char *path)
{
    if (!xpost_init())
    {
        report_failure("xpost_init");
        return;
    }
    _boot_and_write(path);
    xpost_quit();
}

/* Two boots in this one process, the second from a heap the first has
   already been through. The second is only worth an image where the
   first reached one: a boot that did not finish leaves nothing to
   compare, and comparing the one image that was written against a file
   that is not there reports a missing file rather than the boot. */
static void _write_two(const char *pa, const char *pb)
{
    if (!xpost_init())
    {
        report_failure("xpost_init");
        return;
    }
    if (_boot_and_write(pa))
        _boot_and_write(pb);
    xpost_quit();
}

static void _compare(const char *pa, const char *pb)
{
    Image a;
    Image b;

    /* both are given up together below, so both name something before
       either is read: the second is not reached when the first fails */
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);

    if (!_read_image(pa, &a) || !_read_image(pb, &b))
    {
        free(a.bytes);
        free(b.bytes);
        return;
    }

    if (a.stamp[XPOST_VM_IMAGE_STAMP_OBJECT_SIZE]
        != b.stamp[XPOST_VM_IMAGE_STAMP_OBJECT_SIZE] ||
        a.stamp[XPOST_VM_IMAGE_STAMP_ENT_MAX]
        != b.stamp[XPOST_VM_IMAGE_STAMP_ENT_MAX])
    {
        report_failure("the two images were written by builds of different "
                       "object widths (%u/%u bytes, %u/%u entity numbers) and "
                       "are not comparable",
                       a.stamp[XPOST_VM_IMAGE_STAMP_OBJECT_SIZE],
                       b.stamp[XPOST_VM_IMAGE_STAMP_OBJECT_SIZE],
                       a.stamp[XPOST_VM_IMAGE_STAMP_ENT_MAX],
                       b.stamp[XPOST_VM_IMAGE_STAMP_ENT_MAX]);
        free(a.bytes);
        free(b.bytes);
        return;
    }
    if (a.len != b.len)
        report_failure("the two boots wrote %lu and %lu bytes of virtual "
                       "memory", (unsigned long)a.len, (unsigned long)b.len);

    _compare_banks(&a, &b);
    _compare_local(&a, &b);
    _compare_global(&a, &b);

    free(a.bytes);
    free(b.bytes);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "write") == 0)
    {
        _write(argv[2]);
        return verdict();
    }
    if (argc == 4 && strcmp(argv[1], "write2") == 0)
    {
        _write_two(argv[2], argv[3]);
        return verdict();
    }
    if (argc == 4 && strcmp(argv[1], "compare") == 0)
    {
        _compare(argv[2], argv[3]);
        return verdict();
    }

    report_failure("this was asked for something other than `write <path>`, "
                   "`write2 <a> <b>` or `compare <a> <b>`, and so measured "
                   "nothing");
    return verdict();
}
