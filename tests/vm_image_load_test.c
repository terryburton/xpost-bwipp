/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (C) 2013-2016, Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Xpost software product nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Bringing an interpreter up out of an image of virtual memory, and what
 * has to hold for that to be worth doing.
 *
 * The thing an image can do that nothing raises is dispatch to the wrong
 * operator. An operator object carries the number of its table row and
 * nothing else, so an image read into a table whose rows came out in
 * another order runs the wrong code and reports nothing at all. Every
 * other way an image can be wrong announces itself; that one does not.
 * So the read holds the two tables to each other by name, row for row,
 * and this is what says the holding works -- by handing the read an
 * image whose names have been swapped and requiring it to say no.
 *
 * WHAT THIS ESTABLISHES.
 *
 *   Idempotence. A context brought up out of an image writes back the
 *   image it read, to the byte. That is the gate an image has to pass:
 *   a wrong one is a silent wrong answer rather than a crash, so what is
 *   required is not that the interpreter still runs but that the memory
 *   it runs on is the memory the image describes. The two runs are
 *   separate processes, so an image that carried anything of the process
 *   that wrote it would come back different.
 *
 *   Refusal. Every way an image can be unusable ends with the language
 *   built from the boot files instead: the stamps at its head each
 *   damaged in turn, its operator names permuted, its entity table
 *   pointed past its arena, and the file itself cut short, lengthened or
 *   made unrecognisable. Each is required to leave a working
 *   interpreter that says it built the language rather than read it.
 *
 * WHAT IT DOES NOT ESTABLISH.
 *
 *   That an interpreter out of an image behaves as one that booted. That
 *   is the whole suite's question, asked by running the whole suite with
 *   an image in use, and no single test answers it.
 *
 *   That a stamp catches what it names. A stamp is damaged here by
 *   changing the value in the file, which says the comparison happens
 *   and not that the value it compares is the right value to compare.
 *
 * Modes:
 *   boot                 bring a context up as a run does and say
 *                        which way the language arrived; the
 *                        environment says which image to read and
 *                        which to write
 *   damages              how many ways an image can be damaged, and
 *                        what each is called
 *   damage <in> <out> <n>  copy an image, damaging it the nth way
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stddef.h>
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

/* What the runner reads to tell one boot from the other. */
#define SAID_READ "the language was read from an image"
#define SAID_BUILT "the language was built from the boot files"

static unsigned int _u32(const unsigned char *p)
{
    unsigned int v;

    memcpy(&v, p, sizeof v);
    return v;
}

static void _put_u32(unsigned char *p, unsigned int v)
{
    memcpy(p, &v, sizeof v);
}

/*
 *
 * Bringing a context up, the way a run does.
 *
 */

static void _boot(void)
{
    Xpost_Context *ctx;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return;
    }
    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        xpost_quit();
        return;
    }

    /* The language, and then nothing: the run's device is what would be
       made next, and it is what an image must not carry. */
    xpost_interpreter_load_language(ctx);

    printf("%s\n", xpost_vm_image_in_use() ? SAID_READ : SAID_BUILT);

    /* Whatever image this run was told to write has been written by
       now, at the point the interpreter writes one: inside the language
       load, after the language stands and before this run has settled
       anything of its own. Nothing here writes one, so that what is
       compared is the file the interpreter itself produces. */

    /* The interpreter has to be one that runs, whichever way it came up:
       an image read into a context that cannot then execute anything
       would pass a comparison of bytes and be worthless. */
    if (xpost_run(ctx, XPOST_INPUT_STRING,
                  "2 3 add 5 ne { (arithmetic\n) print flush } if\n", 0)
        != XPOST_RUN_COMPLETE)
        report_failure("the interpreter would not run a program");

    xpost_destroy(ctx);
    xpost_quit();
}

/*
 *
 * Damaging an image.
 *
 */

/* Where each part of an image begins, for a damage that has to reach
   into one. Everything up to the banks is a run of fixed-width values
   and length-prefixed names, so it is walked rather than indexed. */
typedef struct
{
    unsigned char *bytes;
    size_t len;
    unsigned int stamp[XPOST_VM_IMAGE_STAMPS];
    size_t operators;       /* the first operator row */
    size_t context;         /* the context's own values */
    size_t roots;           /* the objects the context roots */
    size_t rows[XPOST_VM_IMAGE_BANKS];  /* each bank's entity table */
    size_t arena[XPOST_VM_IMAGE_BANKS]; /* each bank's arena */
    size_t used[XPOST_VM_IMAGE_BANKS];  /* and how long it is */
    size_t bank[XPOST_VM_IMAGE_BANKS];  /* each bank's name */
} Image;

static int _read(const char *path, Image *im)
{
    FILE *f;
    long len;
    size_t at;
    unsigned int i;
    unsigned int b;

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
    /* an image carries a magic and a digest, so a file with room for
       neither is no image; refusing it here makes the allocation below
       exactly the file's length rather than a length or a stand-in */
    if (len <= 0)
    {
        report_failure("the image %s is empty", path);
        fclose(f);
        return 0;
    }
    im->len = (size_t)len;
    /* One byte more than the image, kept spare: a damage that lengthens
       an image lengthens it by one, and having the room already means
       the bytes never move and the buffer is never shorter than the
       length the image carries. */
    im->bytes = malloc(im->len + 1);
    if (!im->bytes || fread(im->bytes, 1, im->len, f) != im->len)
    {
        report_failure("cannot read the image %s", path);
        fclose(f);
        return 0;
    }
    fclose(f);

    at = XPOST_VM_IMAGE_MAGIC_LEN;
    if (im->len < at + XPOST_VM_IMAGE_STAMPS * sizeof(unsigned int))
    {
        report_failure("%s is too short to be an image", path);
        return 0;
    }
    for (i = 0; i < XPOST_VM_IMAGE_STAMPS; i++, at += sizeof(unsigned int))
        im->stamp[i] = _u32(im->bytes + at);

    im->operators = at;
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
    im->context = at;
    at += im->stamp[XPOST_VM_IMAGE_STAMP_CONTEXT_FIELDS] * sizeof(unsigned int);
    im->roots = at;
    at += (size_t)(im->stamp[XPOST_VM_IMAGE_STAMP_ROOTS]
                   + im->stamp[XPOST_VM_IMAGE_STAMP_TYPENAMES])
          * sizeof(Xpost_Object);

    for (b = 0; b < XPOST_VM_IMAGE_BANKS; b++)
    {
        unsigned int used;
        unsigned int nextent;

        if (im->len < at + 8 + (XPOST_VM_IMAGE_BANK_FIELDS
                                + XPOST_VM_IMAGE_FILE_BIRTHS)
                              * sizeof(unsigned int))
        {
            report_failure("%s ends in the middle of the %s bank", path,
                           xpost_vm_image_bank_name(b));
            return 0;
        }
        im->bank[b] = at;
        at += 8;
        used = _u32(im->bytes + at
                    + XPOST_VM_IMAGE_BANK_HIGH_WATER * sizeof(unsigned int));
        nextent = _u32(im->bytes + at
                       + XPOST_VM_IMAGE_BANK_NEXTENT * sizeof(unsigned int));
        at += (XPOST_VM_IMAGE_BANK_FIELDS + XPOST_VM_IMAGE_FILE_BIRTHS)
              * sizeof(unsigned int);
        im->rows[b] = at;
        at += (size_t)nextent * XPOST_VM_IMAGE_ROW_FIELDS * sizeof(unsigned int);
        im->arena[b] = at;
        im->used[b] = used;
        at += used;
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

/* The first row of a bank's entity table. */
static size_t _rows(const Image *im, unsigned int bank)
{
    return im->rows[bank];
}

/* What an image answers for itself with. The digest is the last value in
   the file and covers every byte before it, so a damage that means to be
   met by a check further in has to leave the file answering for itself,
   and one that means to be met by the digest has to not. */
static void _reseal(Image *im)
{
    unsigned int h = XPOST_VM_IMAGE_DIGEST_SEED;
    size_t len = im->len;
    size_t i;

    /* The digest goes in the last four bytes of the image, so an image
       with no room for one names no place to put it. The length an
       image carries is the length of the image, whatever the buffer it
       was read into has since become, so it is the one bound here. */
    if (len < sizeof h)
        return;

    for (i = 0; i + sizeof h <= len; i++)
    {
        h ^= im->bytes[i];
        h *= 16777619u;
    }
    memcpy(im->bytes + len - sizeof h, &h, sizeof h);
}

/* Swap the names of two operator rows, leaving everything else where it
   was. Two of the same length, so that nothing after them moves and the
   only difference between the two files is which operator each row says
   it is: an image that says row seven is the operator row eight used to
   be, and nothing else. */
static int _swap_two_names(Image *im)
{
    unsigned int n = im->stamp[XPOST_VM_IMAGE_STAMP_OPERATORS];
    unsigned char tmp[256];
    size_t at = im->operators;
    size_t first = 0;
    unsigned int firstlen = 0;
    unsigned int i;

    for (i = 0; i < n; i++)
    {
        size_t rowat = at;
        unsigned int namelen = _u32(im->bytes + at
                                    + 2 * sizeof(unsigned int)
                                    + sizeof(Xpost_Object));
        size_t name = at + 3 * sizeof(unsigned int) + sizeof(Xpost_Object);

        if (namelen && namelen == firstlen &&
            memcmp(im->bytes + first, im->bytes + name, namelen) != 0)
        {
            memcpy(tmp, im->bytes + first, namelen);
            memcpy(im->bytes + first, im->bytes + name, namelen);
            memcpy(im->bytes + name, tmp, namelen);
            return 1;
        }
        if (namelen && namelen <= sizeof tmp)
        {
            first = name;
            firstlen = namelen;
        }
        at = name + namelen + (4u - (namelen % 4u)) % 4u;
        {
            /* and past the operand shapes the row states, which follow
               its name */
            unsigned int nsig = _u32(im->bytes + rowat);
            unsigned int si;

            for (si = 0; si < nsig; si++)
            {
                unsigned int in = _u32(im->bytes + at);

                at += sizeof(unsigned int) + in + (4u - (in % 4u)) % 4u;
            }
        }
    }
    report_failure("this image has no two operators named alike in length");
    return 0;
}

/* Every way an image is damaged here, in two kinds.

   The first kind damages what an image says: the value it begins with,
   the stamps at its head, which operator each row of its table is, what
   an entity of it claims to cover, how long the file is. Each of those
   is met by a check written for it, and each is resealed after the
   damage so that it is met by that check and not by the digest -- an
   image that no longer answers for itself would be turned away at the
   door and the check further in would never be reached.

   The second kind damages a byte and leaves the file no longer answering
   for itself. Those are met by the digest, and there is nowhere in an
   image they are not: every region the file has is flipped in, and so is
   the digest itself, and so are seven places spread across the whole
   length of it. The digest is the only thing standing between a file
   somebody else has written and this interpreter's virtual memory, and a
   flip that reached the arena would put a chosen byte among the
   procedures the language is made of. */
typedef enum
{
    DAMAGE_MAGIC,
    DAMAGE_NAMES,
    DAMAGE_ROW,
    DAMAGE_SHORT,
    DAMAGE_LONG,
    DAMAGE_STAMP        /* and XPOST_VM_IMAGE_STAMPS of these */
} Damage;

/* the damages that leave the file answering for itself */
#define SEALED (DAMAGE_STAMP + XPOST_VM_IMAGE_STAMPS)
/* and the flips that do not */
#define FLIPS ((unsigned int)(sizeof _flip_names / sizeof *_flip_names))
#define DAMAGES (SEALED + FLIPS)

/* The places a flip lands in, and the fraction of the way through the
   file the last of them are. Named apart from the image so that asking
   what the damages are does not need one. */
static const char *const _flip_names[] =
{
    "a byte of an operator's name",
    "a byte of the context's own values",
    "a byte of the objects the context roots",
    "a byte of the global bank's entity table",
    "a byte near the start of the global arena",
    "a byte in the middle of the global arena",
    "a byte near the end of the global arena",
    "a byte of the local bank's entity table",
    "a byte in the middle of the local arena",
    "a byte of the digest itself",
    "a byte a tenth of the way through",
    "a byte a quarter of the way through",
    "a byte two fifths of the way through",
    "a byte eleven twentieths of the way through",
    "a byte seven tenths of the way through",
    "a byte seventeen twentieths of the way through",
    "a byte nineteen twentieths of the way through"
};

static const unsigned int _flip_parts[] = { 10, 25, 40, 55, 70, 85, 95 };

/* Where one flip lands. The offsets are worked out from the image rather
   than written down, so a flip goes on landing in the region it names
   when the format around it moves. */
static size_t _flip_at(const Image *im, unsigned int f)
{
    switch (f)
    {
        case 0: return im->operators + 3 * sizeof(unsigned int)
                       + sizeof(Xpost_Object);
        case 1: return im->context;
        case 2: return im->roots;
        case 3: return im->rows[0] + 8 * XPOST_VM_IMAGE_ROW_FIELDS
                                       * sizeof(unsigned int);
        case 4: return im->arena[0] + 64;
        case 5: return im->arena[0] + im->used[0] / 2;
        case 6: return im->arena[0] + im->used[0] - 8;
        case 7: return im->rows[1] + 8 * XPOST_VM_IMAGE_ROW_FIELDS
                                       * sizeof(unsigned int);
        case 8: return im->arena[1] + im->used[1] / 2;
        case 9: return im->len - 1;
        default: break;
    }
    return im->len * _flip_parts[f - 10] / 100;
}

static const char *_damage_name(unsigned int which)
{
    static char buf[80];

    switch (which)
    {
        case DAMAGE_MAGIC: return "what an image begins with";
        case DAMAGE_NAMES: return "two operator names, swapped";
        case DAMAGE_ROW: return "an entity pointed past its arena";
        case DAMAGE_SHORT: return "an image cut short";
        case DAMAGE_LONG: return "an image with a byte on the end";
        default: break;
    }
    if (which < SEALED)
    {
        snprintf(buf, sizeof buf, "the stamp for %s",
                 xpost_vm_image_stamp_name(which - DAMAGE_STAMP));
        return buf;
    }
    return _flip_names[which - SEALED];
}

static void _damages(void)
{
    unsigned int i;

    printf("%u\n", (unsigned int)DAMAGES);
    for (i = 0; i < DAMAGES; i++)
        printf("%s\n", _damage_name(i));
}

static void _damage(const char *in, const char *out, unsigned int which)
{
    Image im;
    FILE *f;
    size_t len;

    if (which >= DAMAGES)
    {
        report_failure("there is no %uth way to damage an image", which);
        return;
    }
    if (!_read(in, &im))
    {
        free(im.bytes);
        return;
    }
    len = im.len;

    if (which >= SEALED)
    {
        /* a byte, and the file no longer answering for itself */
        size_t off = _flip_at(&im, which - SEALED);

        if (off >= len)
        {
            report_failure("%s lies outside an image of %lu bytes",
                           _flip_names[which - SEALED], (unsigned long)len);
            free(im.bytes);
            return;
        }
        im.bytes[off] ^= 0xff;
    }
    else
    {
        switch (which)
        {
            case DAMAGE_MAGIC:
                im.bytes[0] ^= 0xff;
                break;
            case DAMAGE_NAMES:
                if (!_swap_two_names(&im))
                {
                    free(im.bytes);
                    return;
                }
                break;
            case DAMAGE_ROW:
                /* the first entity of global memory, told it lies past
                   the end of the arena it is in */
                _put_u32(im.bytes + _rows(&im, 0)
                         + XPOST_VM_IMAGE_ROW_ADR * sizeof(unsigned int),
                         0xffff0000u);
                break;
            case DAMAGE_SHORT:
                if (len < 64)
                {
                    report_failure("the image is too short to cut short");
                    free(im.bytes);
                    return;
                }
                len -= 64;
                im.len = len;
                break;
            case DAMAGE_LONG:
                im.bytes[len] = 0;
                len++;
                im.len = len;
                break;
        }
        /* left answering for itself, so that the check written for this
           damage is what meets it */
        _reseal(&im);
    }

    f = fopen(out, "wb");
    if (!f)
    {
        report_failure("cannot open %s to write a damaged image to", out);
        free(im.bytes);
        return;
    }
    /* the stream is closed whichever way the write went, so a failed
       write gives it up rather than holding it to the end of the run */
    {
        int wrote = fwrite(im.bytes, 1, len, f) == len;

        if (fclose(f) != 0 || !wrote)
            report_failure("cannot write a damaged image to %s", out);
    }
    free(im.bytes);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "boot") == 0)
    {
        _boot();
        return verdict();
    }
    if (argc == 2 && strcmp(argv[1], "damages") == 0)
    {
        _damages();
        return verdict();
    }
    if (argc == 5 && strcmp(argv[1], "damage") == 0)
    {
        _damage(argv[2], argv[3], (unsigned int)strtoul(argv[4], NULL, 10));
        return verdict();
    }

    report_failure("this was asked for something other than `boot`, "
                   "`damages` or `damage <in> <out> <n>`, and so measured "
                   "nothing");
    return verdict();
}
