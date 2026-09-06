/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_record.c
 * @brief The recorded page: what was painted, kept so it can be painted again.
 *
 * A device that cannot draw the whole page at once is given the marks
 * instead of the pixels. Each is recorded in the order it was made and
 * replayed once per band, so a page larger than memory is drawn a strip at
 * a time from one description.
 *
 * The store grows in blocks and can spill to a file when a page outgrows
 * what it may hold in memory; what it holds is marks and the state each
 * was made under, never a raster.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "xpost_object.h"
#include "xpost_error.h"
#include "xpost_op_path.h"  /* XPOST_PATH_BREAK: what a subpath break is */
#include "xpost_strbuf.h"

#include "xpost_record.h"

/* A mark's place in the run of values, and the rows it reaches.
   Keeping the extent beside the mark rather than working it out at
   replay is what makes a replay's cost the marks it plays: a polygon's
   reach is a walk of its vertices, and a page is replayed once per band
   rather than once. */
typedef struct
{
    Xpost_Record_Kind kind;
    size_t at;                  /* where its values begin */
    int nops;                   /* how many operands follow the colour */
    real lo, hi;                /* the rows it reaches */
} _Mark;

/* Both runs are kept in the buffer the tree grows buffers with, so
   that a record's storage is grown the one way everything else is. The
   marks and the values are each a run of one kind of thing rather than
   text, and are read back through a pointer of that kind: what a
   buffer holds is the caller's to say, and its alignment is the
   allocator's, which suits anything. */
/* One threshold cell, kept whole because a screen is read by the pixel
   and there is no part of it a replay can do without. Where the record
   has spilled the cell is in the file and @c cell is nothing; @c at is
   where the bytes begin. */
typedef struct
{
    int w, h;
    unsigned char *cell;
    Xpost_Spill_Off at;
} _Screen;

/* One coverage mask, and a digest of it. The digest is kept because
   taking a mask up asks whether the record already holds it, and a page
   of text asks that once per glyph on it: comparing the bytes against
   every mask held would make a line of text cost the masks times the
   glyphs, where comparing digests makes it cost the glyphs. The bytes
   are still compared where the digests agree, so what is shared is
   masks that are equal and not masks that collide. */
typedef struct
{
    int w, h;
    unsigned long long digest;
    unsigned char *cov;
    Xpost_Spill_Off at;         /* and where, once the record has spilled */
} _Cover;

/* Where a picture's two large parts are, once the record has spilled.
   They are kept beside the entries rather than in them because the entry
   is the caller's own structure and its two pointers are what a caller
   holding the samples in memory reads. */
typedef struct
{
    Xpost_Spill_Off samples;
    Xpost_Spill_Off mbits;
} _ImgAt;

/* What a record that has spilled keeps in memory.
 *
 * One stream, in the order the marks were made, and one buffer for it.
 * Not a chain per band: a band that covers several of those would read
 * them one after another and play a mark spanning two of them out of
 * order with a mark inside one, which is a different page (see
 * doc/xpost_design.dox). Order is what a record is for, so the file keeps it
 * and a run of rows is chosen by reading past what it does not meet.
 *
 * The buffer is the whole of the per-page term a spill adds. It is one
 * buffer for the page rather than one per band, so the retreat to a
 * shallower band costs reading and never memory. */
typedef struct
{
    Xpost_Spill *f;
    Xpost_Spill_Off base;       /* where this page's entries begin */
    Xpost_Spill_Off end;        /* where the next one goes */

    /* what has not reached the file yet; its length is how much */
    Xpost_String_Buffer wbuf;
    Xpost_Spill_Off wat;        /* the offset the buffer's first byte is */

    /* the window a read is answered from; its length is how much of the
       file is in it, beginning at rat */
    Xpost_String_Buffer rbuf;
    Xpost_Spill_Off rat;

    Xpost_String_Buffer hold;   /* the one entry handed back */

    size_t cord;                /* the entry at coff is this one */
    Xpost_Spill_Off coff;

    Xpost_String_Buffer blob;   /* the one mask or cell being read */
    size_t blobwhich;           /* which mask it is, so a resumed replay
                                   does not read it again */
    int blobkind;               /* and of what: a mask or a screen */

    Xpost_String_Buffer run;    /* the one sample run being written */
    Xpost_String_Buffer bits;   /* and a picture's mask bits, whole */

    size_t nmark;               /* what the runs would have counted */
    /* the rows the marks reach and the box they reach, kept as they are
       written down: a spilled record answers both without reading the
       file, and the walk a resident record makes is the walk this
       replaces */
    real ey_lo, ey_hi;
    real bx0, bx1, by0, by1;
    int eany, bany;
} _Spill;

struct _Xpost_Record
{
    int ncomp;
    int short_of_a_mark;        /* a mark was given and could not be held */
    /* and what took it: nothing, memory, or the scratch file */
    int why;
    /* How many holders there are: whoever made it, and every record
       placing it. The record goes with the last of them. */
    int refs;
    /* How many drawings deep this one goes, counting itself: one for a
       record placing none, and one more than the deepest it places. */
    int depth;
    Xpost_String_Buffer mark;   /* a run of _Mark */
    Xpost_String_Buffer val;    /* colour and operands, run together */
    Xpost_String_Buffer img;    /* a run of Xpost_Record_Image */
    Xpost_String_Buffer imgat;  /* and a run of _ImgAt beside it */
    size_t imgbytes;            /* what the copies they point at cost */
    Xpost_String_Buffer msk;    /* a run of _Cover */
    size_t mskbytes;            /* what the coverage they point at costs */
    Xpost_String_Buffer scr;    /* a run of _Screen */
    size_t scrbytes;            /* what the cells they point at cost */
    /* a run of Xpost_Record *, the drawings this one places: one entry
       per distinct drawing, however many placements name it */
    Xpost_String_Buffer sub;
    /* The most the runs have ever held at once. The runs keep their
       storage over a page boundary and are filled again, so what they
       are resident for after one is the largest page rather than the
       page in hand; the lengths themselves go back to nothing there and
       cannot say it. */
    size_t runhigh;
    /* The screen the marks are being made under, kept apart from the
       run so that it outlives a page: a page boundary is not a screen
       change, so the page beginning is written the same screen the page
       ending was painted under. */
    int htw, hth;
    unsigned char *htcell;
    /* How many blocks the entries point at, kept as they are taken and
       given up rather than counted by walking them. What a record costs
       is read once per mark by a caller deciding where to put it, and a
       count that walked the masks, the screens and every table of every
       picture would make that reading the record's own size. */
    size_t covblocks, cellblocks, imgblocks;
    /* Where the marks are, where that is not memory. */
    _Spill *sp;
};

/* One buffer for the page's writes, taken at the spill and never grown.
   Sixty-four kibibytes is under two per cent of the four million bytes
   the band budget allows a raster where a run names no budget of its own
   (data/recorddev.ps), so at that budget what the spill adds to the
   bound is inside the rounding on the term it is bounding. It is the
   page's buffer and not a band's, so it is this size at any budget: a
   run naming a smaller one buys a shallower band and the same buffer,
   which is then a larger share of what a page costs. */
#define _SPILL_BUFFER  (64u * 1024u)

/* and the same again for the window a read is answered from */
#define _SPILL_WINDOW  (64u * 1024u)

/* What one entry looks like in the file: a length that counts the header
   as well, a kind, and then the entry's own bytes.
   A kind of _SPILL_BLOB is not an entry at all but a run of bytes some
   entry names -- a coverage mask, a threshold cell, a picture's samples.
   It carries the same header so that a walk of the entries steps over it
   by its length rather than having to know where it is. */
#define _SPILL_HEAD  8u
#define _SPILL_BLOB  0xFFu

/* what stands at the head of the file, so that a record written by one
   build and read by another says so rather than being believed */
#define _SPILL_MARK  "XPSPILL\1"
#define _SPILL_BASE  32

/* --- reaching what a record holds ------------------------------------
   A record keeps its marks, its pictures, its masks, its screens and its
   placed drawings in blocks of their own, and these are the accessors
   every reader goes through. Nothing else knows the layout, which is what
   lets the whole of it move to a file without the readers changing. */

static _Mark *_marks(const Xpost_Record *rec)
{
    return (_Mark *)rec->mark.s;
}

static real *_vals(const Xpost_Record *rec)
{
    return (real *)rec->val.s;
}

static size_t _nmark(const Xpost_Record *rec)
{
    return rec->mark.len / sizeof(_Mark);
}

static Xpost_Record_Image *_imgs(const Xpost_Record *rec)
{
    return (Xpost_Record_Image *)rec->img.s;
}

static size_t _nimg(const Xpost_Record *rec)
{
    return rec->img.len / sizeof(Xpost_Record_Image);
}

static _ImgAt *_imgats(const Xpost_Record *rec)
{
    return (_ImgAt *)rec->imgat.s;
}

static _Cover *_msks(const Xpost_Record *rec)
{
    return (_Cover *)rec->msk.s;
}

static size_t _nmsk(const Xpost_Record *rec)
{
    return rec->msk.len / sizeof(_Cover);
}

static _Screen *_scrs(const Xpost_Record *rec)
{
    return (_Screen *)rec->scr.s;
}

static size_t _nscr(const Xpost_Record *rec)
{
    return rec->scr.len / sizeof(_Screen);
}

static Xpost_Record **_subs(const Xpost_Record *rec)
{
    return (Xpost_Record **)rec->sub.s;
}

static size_t _nsub(const Xpost_Record *rec)
{
    return rec->sub.len / sizeof(Xpost_Record *);
}

/* What is in the runs now. Read where a page boundary is about to empty
   them, and again where what the record costs is asked for, so it is
   stated once. */
static size_t _runfill(const Xpost_Record *rec)
{
    return rec->mark.len + rec->val.len + rec->img.len + rec->imgat.len
         + rec->msk.len + rec->scr.len + rec->sub.len;
}

/* The columns one entry reaches, from its operands and from what it
   names. Stated for a kind and its operands rather than for a mark in a
   run, because a record that has spilled has no run to point into and
   asks the same question as the entry is written down. */
static int _span_of(const Xpost_Record *rec, Xpost_Record_Kind kind,
                    const real *ops, int nops, real *x0, real *x1);

/* Say the record is short of a mark, and what took it. */
static void _short(Xpost_Record *rec, int why);

/*
 * Where a record puts what it need not hold.
 *
 * Everything below answers for a record whose rec->sp is set. The
 * shape is one append-only stream of entries in the order they were
 * made, one buffer being filled at the end of it, and one window
 * somewhere in it that a read is answered from. A blob -- a mask's
 * coverage, a screen's cell, a picture's samples -- goes into the same
 * stream under a header that says to step over it.
 */

/* --- the record on disk ----------------------------------------------
   Past a threshold a record is worth more than the raster it saves, and
   its marks go to a scratch file. One stream in mark order, one write
   buffer and one read window: order is what a record is for, and a chain
   per band would cost it. Everything below reads the same record whether
   it is in memory or in the file. */

/* Put what the buffer holds where it belongs. */
static int _sp_flush(Xpost_Record *rec)
{
    _Spill *sp = rec->sp;

    if (!sp->wbuf.len)
        return 1;
    if (!xpost_spill_write(sp->f, sp->wat, sp->wbuf.s, sp->wbuf.len))
        return 0;
    sp->wat += (Xpost_Spill_Off)sp->wbuf.len;
    sp->wbuf.len = 0;
    return 1;
}

/* Append @p n bytes to the stream.
 *
 * A run of bytes larger than the buffer is written straight through
 * rather than refused or broken up: a polygon of thousands of vertices
 * and a picture's samples are ordinary things for a page to contain, and
 * a buffer is an arrangement for writing less often rather than a limit
 * on what may be written. */
static int _sp_put(Xpost_Record *rec, const void *p, size_t n)
{
    _Spill *sp = rec->sp;

    /* what has been read is no longer what is there */
    sp->rbuf.len = 0;
    if (n > sp->wbuf.cap)
    {
        if (!_sp_flush(rec))
            return 0;
        if (!xpost_spill_write(sp->f, sp->end, p, n))
            return 0;
        sp->end += (Xpost_Spill_Off)n;
        sp->wat = sp->end;
        return 1;
    }
    if (sp->wbuf.len + n > sp->wbuf.cap && !_sp_flush(rec))
        return 0;
    memcpy(sp->wbuf.s + sp->wbuf.len, p, n);
    sp->wbuf.len += n;
    sp->end += (Xpost_Spill_Off)n;
    return 1;
}

/* Take @p n bytes from @p at.
 *
 * The buffer is emptied first, so that what is read is what has been
 * written rather than what has reached the file. Writing and reading do
 * not interleave -- a page is written down and then played -- so this
 * costs one flush per page and not one per read. */
static int _sp_pull(Xpost_Record *rec, Xpost_Spill_Off at, void *p, size_t n)
{
    _Spill *sp = rec->sp;
    size_t want;

    /* Every way out of here without the bytes is a page the record can
       no longer reproduce: the file has been shortened under a
       descriptor nobody else holds, or the read failed below us. The
       record is short of a mark from here, on the terms a mark it could
       not hold puts it there, so what a caller gets is a refusal naming
       the file rather than a page that stops where the reading did.
       Said here rather than at each caller, because every caller of this
       is reading a mark. */
    if (at < 0 || at + (Xpost_Spill_Off)n > sp->end)
    {
        _short(rec, ioerror);
        return 0;
    }
    if (sp->wbuf.len && !_sp_flush(rec))
    {
        _short(rec, ioerror);
        return 0;
    }
    if (sp->rbuf.len && at >= sp->rat
        && at + (Xpost_Spill_Off)n <= sp->rat + (Xpost_Spill_Off)sp->rbuf.len)
    {
        memcpy(p, sp->rbuf.s + (size_t)(at - sp->rat), n);
        return 1;
    }
    /* more than the window holds is read where it is wanted; the window
       is for the many small reads a walk of the entries makes */
    if (n > sp->rbuf.cap)
    {
        if (xpost_spill_read(sp->f, at, p, n))
            return 1;
        _short(rec, ioerror);
        return 0;
    }
    want = sp->rbuf.cap;
    if ((Xpost_Spill_Off)want > sp->end - at)
        want = (size_t)(sp->end - at);
    if (!xpost_spill_read(sp->f, at, sp->rbuf.s, want))
    {
        _short(rec, ioerror);
        return 0;
    }
    sp->rat = at;
    sp->rbuf.len = want;
    memcpy(p, sp->rbuf.s, n);
    return 1;
}

/* The header of the entry at @p at: how long it is and what kind it is. */
static int _sp_head(Xpost_Record *rec, Xpost_Spill_Off at,
                    unsigned int *len, unsigned int *kind)
{
    unsigned char h[_SPILL_HEAD];

    if (!_sp_pull(rec, at, h, sizeof h))
        return 0;
    memcpy(len, h, sizeof *len);
    *kind = h[4];
    return *len >= _SPILL_HEAD
        && at + (Xpost_Spill_Off)*len <= rec->sp->end;
}

/* Write a run of bytes some entry names, answering where it begins. */
static int _sp_blob(Xpost_Record *rec, const void *p, size_t n,
                    Xpost_Spill_Off *at)
{
    unsigned char h[_SPILL_HEAD];
    unsigned int len;

    /* a blob past what its header counts is one a walk could not step
       over, so it is refused where it is offered */
    if (n > (size_t)0xFFFFFFFFu - _SPILL_HEAD)
        return 0;
    len = (unsigned int)(n + _SPILL_HEAD);
    memset(h, 0, sizeof h);
    memcpy(h, &len, sizeof len);
    h[4] = _SPILL_BLOB;
    if (!_sp_put(rec, h, sizeof h))
        return 0;
    *at = rec->sp->end;
    return _sp_put(rec, p, n);
}

/* How wide one entry's values are, for a record of this shape. */
static size_t _sp_vals(const Xpost_Record *rec, int nops)
{
    return (size_t)(2 + rec->ncomp + nops) * sizeof(real);
}

/* Write one entry down: the kind, the rows it reaches, its colour and
   its operands, all in the width a coordinate arrives in. */
static int _sp_entry(Xpost_Record *rec, Xpost_Record_Kind kind,
                     const real *colour, const real *ops, int nops,
                     real lo, real hi)
{
    unsigned char h[_SPILL_HEAD];
    real ex[2];
    size_t vals = _sp_vals(rec, nops);
    unsigned int len;

    if (vals > (size_t)0xFFFFFFFFu - _SPILL_HEAD)
        return 0;
    len = (unsigned int)(vals + _SPILL_HEAD);
    memset(h, 0, sizeof h);
    memcpy(h, &len, sizeof len);
    h[4] = (unsigned char)kind;
    ex[0] = lo;
    ex[1] = hi;
    if (!_sp_put(rec, h, sizeof h)
        || !_sp_put(rec, ex, sizeof ex)
        || !_sp_put(rec, colour, (size_t)rec->ncomp * sizeof *colour)
        || (nops > 0 && !_sp_put(rec, ops, (size_t)nops * sizeof *ops)))
        return 0;
    rec->sp->nmark++;
    return 1;
}

/* What the entry just written adds to the rows the record reaches and to
   the box it reaches. Kept as the marks arrive because the two are asked
   of a finished drawing -- once per placement of it, and once per page
   that places it -- and a spilled record answering by reading its file
   would read the whole of it each time. */
static void _sp_reach(Xpost_Record *rec, Xpost_Record_Kind kind,
                      const real *ops, int nops, real lo, real hi)
{
    _Spill *sp = rec->sp;
    real a, b;

    /* a screen paints nothing and so reaches no row: what is being
       asked is where the ink is */
    if (kind == XPOST_RECORD_SCREEN)
        return;
    if (!sp->eany)
    {
        sp->ey_lo = lo;
        sp->ey_hi = hi;
        sp->eany = 1;
    }
    else
    {
        if (lo < sp->ey_lo) sp->ey_lo = lo;
        if (hi > sp->ey_hi) sp->ey_hi = hi;
    }
    if (!_span_of(rec, kind, ops, nops, &a, &b))
        return;
    if (!sp->bany)
    {
        sp->bx0 = a; sp->bx1 = b;
        sp->by0 = lo; sp->by1 = hi;
        sp->bany = 1;
        return;
    }
    if (a < sp->bx0) sp->bx0 = a;
    if (b > sp->bx1) sp->bx1 = b;
    if (lo < sp->by0) sp->by0 = lo;
    if (hi > sp->by1) sp->by1 = hi;
}

/* Put the cursor on the entry at @p want.
 *
 * Forward from where it stands, or from the first entry where it stands
 * past the one asked for. A replay asks for one entry after another, so
 * the common step is one entry; a replay begun again asks for the first,
 * which is the other case. */
static int _sp_seek(Xpost_Record *rec, size_t want)
{
    _Spill *sp = rec->sp;

    if (want > sp->nmark)
        return 0;
    if (sp->cord > want)
    {
        sp->cord = 0;
        sp->coff = sp->base;
    }
    while (sp->cord < want)
    {
        unsigned int len, kind;

        if (!_sp_head(rec, sp->coff, &len, &kind))
            return 0;
        sp->coff += (Xpost_Spill_Off)len;
        if (kind != _SPILL_BLOB)
            sp->cord++;
    }
    /* and past whatever blobs stand in front of the entry asked for, so
       that the cursor is on an entry and not on the bytes before one */
    for (;;)
    {
        unsigned int len, kind;

        if (sp->coff >= sp->end)
            return want == sp->nmark;
        if (!_sp_head(rec, sp->coff, &len, &kind))
            return 0;
        if (kind != _SPILL_BLOB)
            return 1;
        sp->coff += (Xpost_Spill_Off)len;
    }
}

/* Read the entry the cursor stands on into the one place a record hands
   an entry back from. */
static int _sp_read(Xpost_Record *rec, Xpost_Record_Kind *kind,
                    const real **colour, const real **ops, int *nops)
{
    _Spill *sp = rec->sp;
    unsigned int len, k;
    size_t vals, n;

    if (!_sp_head(rec, sp->coff, &len, &k) || k == _SPILL_BLOB)
        return 0;
    vals = len - _SPILL_HEAD;
    if (vals < (size_t)(2 + rec->ncomp) * sizeof(real)
        || vals % sizeof(real))
        return 0;
    n = vals / sizeof(real) - (size_t)(2 + rec->ncomp);
    if (n > (size_t)INT_MAX)
        return 0;
    sp->hold.len = 0;
    if (xpost_strbuf_reserve(&sp->hold, vals))
        return 0;
    if (!_sp_pull(rec, sp->coff + _SPILL_HEAD, sp->hold.s, vals))
        return 0;
    *kind = (Xpost_Record_Kind)k;
    /* the rows the entry reaches stand first and are not handed back:
       what a caller asks for is the call, and where it lands is the
       record's own way of choosing which calls a run of rows meets */
    *colour = (const real *)sp->hold.s + 2;
    *ops = n ? (const real *)sp->hold.s + 2 + rec->ncomp : NULL;
    *nops = (int)n;
    return 1;
}

/* The rows the entry the cursor stands on reaches. */
static int _sp_rows(Xpost_Record *rec, real *lo, real *hi,
                    Xpost_Record_Kind *kind)
{
    _Spill *sp = rec->sp;
    unsigned int len, k;
    real ex[2];

    if (!_sp_head(rec, sp->coff, &len, &k) || k == _SPILL_BLOB)
        return 0;
    if (!_sp_pull(rec, sp->coff + _SPILL_HEAD, ex, sizeof ex))
        return 0;
    *lo = ex[0];
    *hi = ex[1];
    *kind = (Xpost_Record_Kind)k;
    return 1;
}

/* Step the cursor to the entry after the one it stands on. */
static int _sp_step(Xpost_Record *rec)
{
    _Spill *sp = rec->sp;

    for (;;)
    {
        unsigned int len, kind;

        if (!_sp_head(rec, sp->coff, &len, &kind))
            return 0;
        sp->coff += (Xpost_Spill_Off)len;
        if (kind != _SPILL_BLOB)
        {
            sp->cord++;
            break;
        }
    }
    return 1;
}

/* Read a blob into the one place a record reads blobs into. @p which and
   @p kind say what is in there now, so that a replay resumed inside a
   coverage mask does not read the mask again for every batch. */
static const unsigned char *_sp_blob_get(Xpost_Record *rec,
                                         Xpost_Spill_Off at, size_t n,
                                         size_t which, int kind)
{
    _Spill *sp = rec->sp;

    if (sp->blob.s && sp->blobwhich == which && sp->blobkind == kind)
        return (const unsigned char *)sp->blob.s;
    sp->blob.len = 0;
    if (xpost_strbuf_reserve(&sp->blob, n))
        return NULL;
    if (!_sp_pull(rec, at, sp->blob.s, n))
        return NULL;
    sp->blobwhich = which;
    sp->blobkind = kind;
    return (const unsigned char *)sp->blob.s;
}

/* Give up a record's spill, and the file with it. */
static void _sp_free(Xpost_Record *rec)
{
    _Spill *sp = rec->sp;

    if (!sp)
        return;
    xpost_spill_close(sp->f);
    xpost_strbuf_free(&sp->wbuf);
    xpost_strbuf_free(&sp->rbuf);
    xpost_strbuf_free(&sp->hold);
    xpost_strbuf_free(&sp->blob);
    xpost_strbuf_free(&sp->run);
    xpost_strbuf_free(&sp->bits);
    free(sp);
    rec->sp = NULL;
}

/* What a spilled record is resident for beyond its tables: the two
   buffers it was made with, and whatever the largest thing it has had to
   read back at once came to. */
static size_t _sp_resident(const _Spill *sp)
{
    return sizeof *sp + sp->wbuf.cap + sp->rbuf.cap + sp->hold.cap
         + sp->blob.cap + sp->run.cap + sp->bits.cap;
}

/* --- making and giving up a record -----------------------------------
   A record is not virtual memory: restore does not reach it and vmstatus
   does not count it, so it is held and released explicitly, and its
   Destroy runs when the device that owns it is retired. */

Xpost_Record *xpost_record_new(int ncomp)
{
    Xpost_Record *rec;

    if (ncomp < 1)
        return NULL;
    rec = calloc(1, sizeof *rec);
    if (!rec)
        return NULL;
    if (xpost_strbuf_init(&rec->mark, 0) ||
        xpost_strbuf_init(&rec->val, 0) ||
        xpost_strbuf_init(&rec->img, 0) ||
        xpost_strbuf_init(&rec->imgat, 0) ||
        xpost_strbuf_init(&rec->msk, 0) ||
        xpost_strbuf_init(&rec->scr, 0) ||
        xpost_strbuf_init(&rec->sub, 0))
    {
        xpost_strbuf_free(&rec->mark);
        xpost_strbuf_free(&rec->val);
        xpost_strbuf_free(&rec->img);
        xpost_strbuf_free(&rec->imgat);
        xpost_strbuf_free(&rec->msk);
        xpost_strbuf_free(&rec->scr);
        xpost_strbuf_free(&rec->sub);
        free(rec);
        return NULL;
    }
    rec->ncomp = ncomp;
    /* the one holder a record begins with is whoever made it */
    rec->refs = 1;
    /* a record placing no drawing is one drawing deep */
    rec->depth = 1;
    return rec;
}

/* How many blocks one image entry is holding, which is one per table it
   was given. Counted as an entry is taken up and again as it is given up,
   so that what a record costs is a reading rather than a walk. */
static size_t _image_blocks(const Xpost_Record_Image *img)
{
    size_t n = 0;

    if (img->samples) n++;
    if (img->lut)     n++;
    if (img->dluts)   n++;
    if (img->tlut)    n++;
    if (img->tlutrgb) n++;
    if (img->mbits)   n++;
    if (img->mranges) n++;
    if (img->cspans)  n++;
    return n;
}

/* Give up the copies one image entry was made from. Every pointer in it
   is the record's own, so there is nothing here a caller still holds. */
static void _image_free(Xpost_Record_Image *img)
{
    free((void *)img->samples);
    free((void *)img->lut);
    free((void *)img->dluts);
    free((void *)img->tlut);
    free((void *)img->tlutrgb);
    free((void *)img->mbits);
    free((void *)img->mranges);
    free((void *)img->cspans);
    memset(img, 0, sizeof *img);
}

/* Give up every image entry, and the run that held them. */
static void _images_free(Xpost_Record *rec)
{
    size_t i, n = _nimg(rec);

    for (i = 0; i < n; i++)
        _image_free(&_imgs(rec)[i]);
    rec->img.len = 0;
    rec->imgat.len = 0;
    rec->imgbytes = 0;
    rec->imgblocks = 0;
}

/* Give up the cells a record's screens were copied into. */
static void _screens_free(Xpost_Record *rec)
{
    size_t i, n = _nscr(rec);

    for (i = 0; i < n; i++)
        free(_scrs(rec)[i].cell);
    rec->scr.len = 0;
    rec->scrbytes = 0;
    rec->cellblocks = 0;
}

/* and the coverage its masks were copied into */
static void _masks_free(Xpost_Record *rec)
{
    size_t i, n = _nmsk(rec);

    for (i = 0; i < n; i++)
        free(_msks(rec)[i].cov);
    rec->msk.len = 0;
    rec->mskbytes = 0;
    rec->covblocks = 0;
}

/* and the drawings it places, one reference apiece however many
   placements name each of them */
static void _places_free(Xpost_Record *rec)
{
    size_t i, n = _nsub(rec);

    for (i = 0; i < n; i++)
        xpost_record_free(_subs(rec)[i]);
    rec->sub.len = 0;
}

/* A record is held by however many pages place it; these are the two
   ends of that count. The drawing goes when the last holder has given
   it up, and not before. */
Xpost_Record *xpost_record_hold(Xpost_Record *rec)
{
    if (rec)
        rec->refs++;
    return rec;
}

void xpost_record_free(Xpost_Record *rec)
{
    if (!rec)
        return;
    /* one holder's claim given up. A drawing a page still places is
       still held, so what a caller finished with a drawing gives up here
       is its own reference and not, on its own, the drawing. */
    if (--rec->refs > 0)
        return;
    _images_free(rec);
    _masks_free(rec);
    _screens_free(rec);
    _places_free(rec);
    _sp_free(rec);
    free(rec->htcell);
    xpost_strbuf_free(&rec->mark);
    xpost_strbuf_free(&rec->val);
    xpost_strbuf_free(&rec->img);
    xpost_strbuf_free(&rec->imgat);
    xpost_strbuf_free(&rec->msk);
    xpost_strbuf_free(&rec->scr);
    xpost_strbuf_free(&rec->sub);
    free(rec);
}

/* --- writing a mark down ---------------------------------------------
   Every mark carries the run of rows it can reach, and that range must be
   a conservative superset: a visit too many costs time, while a mark
   judged out of a band it touches is simply absent from the page. */

/* How many operands a kind carries after its colour, or -1 for one
   whose length is its own to state. */
static int _fixed_nops(Xpost_Record_Kind kind)
{
    switch (kind)
    {
        case XPOST_RECORD_PUTPIX:   return 2;
        case XPOST_RECORD_BLENDPIX: return 3;
        case XPOST_RECORD_DRAWLINE: return 4;
        case XPOST_RECORD_FILLRECT: return 4;
        /* a polygon states its own length, and none of an image, a
           screen, a glyph and a placement is written down through
           xpost_record_mark at all */
        case XPOST_RECORD_FILLPOLY:
        case XPOST_RECORD_IMAGE:
        case XPOST_RECORD_SCREEN:
        case XPOST_RECORD_GLYPH:
        case XPOST_RECORD_PLACE:    return -1;
    }
    return -1;
}

/* The rows a mark reaches, from its own operands. A rectangle's height
   may be negative -- the contract reflects such a rectangle through its
   origin rather than refusing it -- so both ends are taken. */
static void _extent(Xpost_Record_Kind kind, const real *ops, int nops,
                    real *lo, real *hi)
{
    real a, b;
    int i, any;

    switch (kind)
    {
        case XPOST_RECORD_PUTPIX:
            *lo = *hi = ops[1];
            return;
        case XPOST_RECORD_BLENDPIX:
            *lo = *hi = ops[2];
            return;
        case XPOST_RECORD_DRAWLINE:
            a = ops[1]; b = ops[3];
            /* A segment's ends are put on the 1/256 grid before it is
               walked (xpost_dev_line_quantize), and that can carry an
               end sitting a fraction below a row boundary over it: the
               row the segment ends on is then one past the row its own
               coordinates fall in. It cannot go the other way -- a
               whole row is itself a point of that grid, so a coordinate
               at or above one rounds to no less than it -- so the reach
               is taken one grid step further down and no further up. */
            if (a < b) b += 1.0 / 256.0; else a += 1.0 / 256.0;
            break;
        case XPOST_RECORD_FILLRECT:
            a = ops[1]; b = ops[1] + ops[3];
            break;
        case XPOST_RECORD_FILLPOLY:
            /* n, then n pairs: the vertices are walked, which is the
               only kind whose reach is not read off two values. A pair
               marking a subpath break is a separator and not a point,
               and reaches no row: taken as one it would put the reach
               at the sentinel's own value and have the polygon met by
               every range there is. */
            *lo = *hi = 0.0;
            for (i = 0, any = 0; i * 2 + 2 < nops; i++)
            {
                real y;

                if (ops[i * 2 + 1] == XPOST_PATH_BREAK)
                    continue;
                y = ops[i * 2 + 2];
                if (!any) { *lo = *hi = y; any = 1; }
                else if (y < *lo) *lo = y;
                else if (y > *hi) *hi = y;
            }
            return;
        case XPOST_RECORD_IMAGE:
            /* an image's reach follows the transform that places it and
               is taken where it is written down, not from the one
               operand -- which names the entry rather than describing
               it */
            *lo = *hi = 0.0;
            return;
        case XPOST_RECORD_SCREEN:
            /* a screen reaches no row and every run of rows: it paints
               nothing, and governs whatever is painted after it wherever
               that lands, which is settled in _meets rather than here */
            *lo = *hi = 0.0;
            return;
        case XPOST_RECORD_GLYPH:
            /* a glyph's reach is its mask's height at the row it is put
               on, and is taken where it is written down: the operands
               name the mask rather than describing it */
            *lo = *hi = 0.0;
            return;
        case XPOST_RECORD_PLACE:
            /* and a placement's is the reach of the drawing it names,
               carried down by the offset it is placed at, taken where it
               is written down for the same reason */
            *lo = *hi = 0.0;
            return;
    }
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

/* The rows an image can be written into: the box its transform puts it
   in, met with the region its rows are written through.

   The low end is taken down to the row the first write lands in. A
   write starts at the row the box's edge falls inside, so a reach
   stated from the edge itself would leave a range meeting that row
   judging the image not to reach it -- and a mark judged not to reach a
   band is simply absent from the page, which is wrong output rather
   than slow output. Erring the other way costs a visit. */
static void _image_extent(const Xpost_Record_Image *img, real *lo, real *hi)
{
    real a = img->yoff;
    real b = img->yoff + (real)img->height * img->yscale;
    real t;

    if (a > b) { t = a; a = b; b = t; }
    if (a < img->cy0) a = img->cy0;
    if (b > img->cy1) b = img->cy1;
    *lo = (real)floor((double)a);
    *hi = b < *lo ? *lo : b;
}

/* The first answer stands: a record already short of a mark is
   describing a page it cannot reproduce, and a later failure on the way
   out of that is not the reason a caller wants to be told. */
static void _short(Xpost_Record *rec, int why)
{
    if (!rec->short_of_a_mark)
        rec->why = why;
    rec->short_of_a_mark = 1;
}

/* Put one entry into the two runs: its values first, so that a mark is
   only written once there is somewhere for it to point at.
   Or into the file, where the record has spilled: one entry either way,
   and the difference is where it lands. */
static int _put(Xpost_Record *rec, Xpost_Record_Kind kind,
                const real *colour, const real *ops, int nops,
                real lo, real hi)
{
    _Mark m2;

    if (rec->sp)
    {
        if (!_sp_entry(rec, kind, colour, ops, nops, lo, hi))
        {
            _short(rec, ioerror);
            return 0;
        }
        _sp_reach(rec, kind, ops, nops, lo, hi);
        return 1;
    }

    m2.kind = kind;
    m2.at = rec->val.len / sizeof(real);
    m2.nops = nops;
    m2.lo = lo;
    m2.hi = hi;

    if (xpost_strbuf_append(&rec->val, colour,
                            (size_t)rec->ncomp * sizeof *colour) ||
        (nops > 0 &&
         xpost_strbuf_append(&rec->val, ops, (size_t)nops * sizeof *ops)) ||
        xpost_strbuf_append(&rec->mark, &m2, sizeof m2))
    {
        _short(rec, VMerror);
        return 0;
    }
    return 1;
}

/* Takes one mark: what kind it is, the colour it is in, and the
   numbers that place it. The rows it touches are worked out here and
   kept with it, which is what makes a band's replay a walk of the
   marks that meet it rather than of all of them. */
int xpost_record_mark(Xpost_Record *rec, Xpost_Record_Kind kind,
                      const real *colour, const real *ops, int nops)
{
    int fixed;
    real lo, hi;

    if (!rec || !colour || (nops > 0 && !ops))
        return 0;
    /* a record already short of a mark describes a page it cannot
       reproduce, and adding to it would only make the gap harder to
       see */
    if (rec->short_of_a_mark)
        return 0;
    fixed = _fixed_nops(kind);
    if (fixed >= 0)
    {
        if (nops != fixed)
            return 0;
    }
    else if (kind == XPOST_RECORD_FILLPOLY)
    {
        /* the count and its pairs have to agree, so that a walk of the
           vertices stays inside what was written down */
        if (nops < 1 || ops[0] < 0 || nops != 1 + 2 * (int)ops[0])
            return 0;
    }
    else
        return 0;

    _extent(kind, ops, nops, &lo, &hi);
    return _put(rec, kind, colour, ops, nops, lo, hi);
}

/* --- pictures --------------------------------------------------------
   Bulk pixels follow one rule -- a bitmap produced once is held once and
   re-emitted per band -- so a picture is one entry naming its samples, the
   transform that places them, and the tables they decode through. */

/* Copy what a caller owns into memory the record owns, counting what it
   cost. Nothing asked for and nothing available both answer NULL, which
   a caller tells apart by what it asked for. */
static void *_take(const void *p, size_t n, size_t *cost)
{
    void *q;

    if (!p || !n)
        return NULL;
    q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, n);
    *cost += n;
    return q;
}

/* Copy the sample rows into one block the record owns. They arrive as a
   run pointer apiece because that is how the painter holds them -- a
   buffer it refills per row -- and they are laid end to end here so
   that the entry holds one thing rather than a list of them. */
static unsigned char *_take_rows(const unsigned char *const *rows, int nrows,
                                 size_t each, size_t *cost)
{
    unsigned char *block;
    int i;

    if (!rows || nrows < 1 || !each)
        return NULL;
    block = malloc((size_t)nrows * each);
    if (!block)
        return NULL;
    for (i = 0; i < nrows; i++)
    {
        if (!rows[i])
        {
            free(block);
            return NULL;
        }
        memcpy(block + (size_t)i * each, rows[i], each);
    }
    *cost += (size_t)nrows * each;
    return block;
}

/* Put the sample rows in the file instead, one run after another, and
   answer where the first of them begins. The rows are what follows the
   picture's size, so they are what a spilled record does not hold: a
   hundred megabytes of photograph costs the run being written. */
static int _spill_rows(Xpost_Record *rec, const unsigned char *const *rows,
                       int nrows, size_t each, Xpost_Spill_Off *at)
{
    unsigned char h[_SPILL_HEAD];
    unsigned int len;
    size_t n = (size_t)nrows * each;
    int i;

    if (!rows || nrows < 1 || !each)
        return 0;
    if (n > (size_t)0xFFFFFFFFu - _SPILL_HEAD)
        return 0;
    for (i = 0; i < nrows; i++)
        if (!rows[i])
            return 0;
    len = (unsigned int)(n + _SPILL_HEAD);
    memset(h, 0, sizeof h);
    memcpy(h, &len, sizeof len);
    h[4] = _SPILL_BLOB;
    if (!_sp_put(rec, h, sizeof h))
        return 0;
    *at = rec->sp->end;
    for (i = 0; i < nrows; i++)
        if (!_sp_put(rec, rows[i], each))
            return 0;
    return 1;
}

/* Takes a picture and the mark that places it. The rows arrive as the
   caller holds them and are copied, since nothing promises they
   outlive the call. */
int xpost_record_image(Xpost_Record *rec, const Xpost_Record_Image *src,
                       const unsigned char *const *rows, int nrows)
{
    Xpost_Record_Image img;
    _ImgAt where;
    real *colour;
    real idx;
    real lo, hi;
    size_t each;
    size_t cost = 0;
    int ok;

    if (!rec || !src || rec->short_of_a_mark)
        return 0;
    /* what the row writer indexes with is bounded here, once, rather
       than on the way past every sample */
    if (src->width < 1 || src->height < 1
     || src->ncomp < 1 || src->ncomp > 4
     || src->nat < 1 || src->nat > 3
     || (src->mbits && (src->mrowb < 1 || src->mw < 1 || src->mh < 1))
     || src->nranges < 0 || src->nranges > 8
     || (src->nranges && !src->mranges)
     || src->nspan < 0 || (src->nspan && !src->cspans))
        return 0;
    if (nrows != src->height * (src->planar ? src->ncomp : 1))
        return 0;

    img = *src;
    each = (size_t)src->width * (src->planar ? 1u : (size_t)src->ncomp);
    memset(&where, 0, sizeof where);
    /* The samples and the mask bits are the two of a picture's parts
       that follow how large it is; the tables beside them follow how it
       is described and are a few kilobytes whatever the picture. So a
       spilled record puts those two in the file and keeps the rest,
       which is what makes a picture cost its description here. */
    if (rec->sp)
    {
        if (!_spill_rows(rec, rows, nrows, each, &where.samples))
        {
            _short(rec, ioerror);
            return 0;
        }
        if (src->mbits
            && !_sp_blob(rec, src->mbits,
                         (size_t)src->mrowb * (size_t)src->mh,
                         &where.mbits))
        {
            _short(rec, ioerror);
            return 0;
        }
        img.samples = NULL;
        img.mbits = NULL;
    }
    else
        img.samples = _take_rows(rows, nrows, each, &cost);
    img.lut = _take(src->lut, 256u * (size_t)src->nat, &cost);
    img.dluts = _take(src->dluts, 256u * (size_t)src->ncomp, &cost);
    img.tlut = _take(src->tlut, 256u, &cost);
    img.tlutrgb = _take(src->tlutrgb, 3u * 256u, &cost);
    if (!rec->sp)
        img.mbits = _take(src->mbits,
                          (size_t)src->mrowb * (size_t)src->mh, &cost);
    img.mranges = _take(src->mranges,
                        (size_t)src->nranges * sizeof *src->mranges, &cost);
    img.cspans = _take(src->cspans,
                       4u * (size_t)src->nspan * sizeof *src->cspans, &cost);

    ok = (rec->sp || img.samples)
      && (!src->lut || img.lut)
      && (!src->dluts || img.dluts)
      && (!src->tlut || img.tlut)
      && (!src->tlutrgb || img.tlutrgb)
      && (rec->sp || !src->mbits || img.mbits)
      && (!src->nranges || img.mranges)
      && (!src->nspan || img.cspans);
    if (!ok)
    {
        _image_free(&img);
        _short(rec, VMerror);
        return 0;
    }

    /* the colour a mark carries is one value per component of the
       device's space; an image carries its colours in its samples, so
       the place is filled with zeros and every mark's values stay laid
       out the same way */
    colour = calloc((size_t)rec->ncomp, sizeof *colour);
    if (!colour)
    {
        _image_free(&img);
        _short(rec, VMerror);
        return 0;
    }

    idx = (real)_nimg(rec);
    if (xpost_strbuf_append(&rec->img, &img, sizeof img))
    {
        free(colour);
        _image_free(&img);
        _short(rec, VMerror);
        return 0;
    }
    /* the two runs are one entry between them and are counted together,
       so an entry only half of them took comes off again */
    if (xpost_strbuf_append(&rec->imgat, &where, sizeof where))
    {
        rec->img.len -= sizeof img;
        free(colour);
        _image_free(&img);
        _short(rec, VMerror);
        return 0;
    }
    /* the run holds the entry now, so what it points at is the
       record's to give up and no longer this call's */
    rec->imgbytes += cost;
    rec->imgblocks += _image_blocks(&img);

    _image_extent(&img, &lo, &hi);
    ok = _put(rec, XPOST_RECORD_IMAGE, colour, &idx, 1, lo, hi);
    free(colour);
    return ok;
}

size_t xpost_record_image_count(const Xpost_Record *rec)
{
    return rec ? _nimg(rec) : 0;
}

const Xpost_Record_Image *xpost_record_image_get(const Xpost_Record *rec,
                                                 size_t i)
{
    if (!rec || i >= _nimg(rec))
        return NULL;
    return &_imgs(rec)[i];
}

const unsigned char *xpost_record_image_run(const Xpost_Record *rec,
                                            size_t i, int run)
{
    const Xpost_Record_Image *img;
    Xpost_Record *w = (Xpost_Record *)rec;
    size_t each;
    int nrun;

    if (!rec || i >= _nimg(rec) || run < 0)
        return NULL;
    img = &_imgs(rec)[i];
    nrun = img->height * (img->planar ? img->ncomp : 1);
    if (run >= nrun)
        return NULL;
    each = (size_t)img->width * (img->planar ? 1u : (size_t)img->ncomp);
    if (img->samples)
        return img->samples + (size_t)run * each;
    if (!rec->sp)
        return NULL;
    /* The one run a spilled record holds. It is read where it is asked
       for rather than kept, because what asks for it copies it into the
       row buffer the writer fills and has no use for it after. */
    w->sp->run.len = 0;
    if (xpost_strbuf_reserve(&w->sp->run, each))
        return NULL;
    if (!_sp_pull(w, _imgats(rec)[i].samples + (Xpost_Spill_Off)run * each,
                  w->sp->run.s, each))
        return NULL;
    return (const unsigned char *)w->sp->run.s;
}

const unsigned char *xpost_record_image_mbits(const Xpost_Record *rec,
                                              size_t i)
{
    const Xpost_Record_Image *img;
    Xpost_Record *w = (Xpost_Record *)rec;
    size_t n;

    if (!rec || i >= _nimg(rec))
        return NULL;
    img = &_imgs(rec)[i];
    if (img->mbits)
        return img->mbits;
    if (!rec->sp || !_imgats(rec)[i].mbits)
        return NULL;
    n = (size_t)img->mrowb * (size_t)img->mh;
    w->sp->bits.len = 0;
    if (xpost_strbuf_reserve(&w->sp->bits, n))
        return NULL;
    if (!_sp_pull(w, _imgats(rec)[i].mbits, w->sp->bits.s, n))
        return NULL;
    return (const unsigned char *)w->sp->bits.s;
}

int xpost_record_image_rows(const Xpost_Record_Image *img,
                            real lo, real hi, int *y0, int *y1)
{
    double s, a, b, t;
    int first, last;

    if (!img || !y0 || !y1)
        return 0;
    s = (double)img->yscale;
    /* a transform putting every row in the same place writes nothing;
       answering the whole image there costs a pass and cannot lose one */
    if (s > -1e-9 && s < 1e-9)
    {
        *y0 = 0;
        *y1 = img->height;
        return img->height > 0;
    }
    /* where the range's two edges fall in the image's own rows. A row
       is a whole one either side of that, since a row magnified with
       its neighbours blended reaches half a row beyond its own band at
       each end and the last row reaches a whole one. */
    a = ((double)lo - (double)img->yoff) / s;
    b = ((double)hi + 1.0 - (double)img->yoff) / s;
    if (a > b) { t = a; a = b; b = t; }
    a -= 2.0;
    b += 2.0;
    *y0 = *y1 = 0;
    /* Written as the tests the range must pass rather than the ones it
       must fail. The edges come of a division by the scale, so a scale
       that is not a number leaves them not numbers, and one answers
       false to every comparison -- an ordinary pair of rejections would
       pass it to the conversion below, which has no integer for it. A
       range that is not a number names no rows. */
    if (!(b >= 0.0) || !(a <= (double)img->height))
        return 0;
    /* brought inside the image before it is counted in rows, so that a
       range far off the page does not name a row number no int holds */
    if (a < 0.0) a = 0.0;
    if (b > (double)img->height) b = (double)img->height;
    first = (int)floor(a);
    last = (int)ceil(b);
    *y0 = first;
    *y1 = last;
    return first < last;
}

/* --- coverage masks, and the glyphs that share them ------------------
   A mask is shared on its bytes rather than on a name, because
   driver-generated text paints the same few masks over and over and the
   cache that rendered them is bounded and may drop one a record still
   names. So the record keeps its own copy, and finds a duplicate by
   hashing the content. */

/* A digest of a mask's bytes, and of the extents that say how to read
   them. It answers whether two masks might be equal; the bytes answer
   whether they are. */
static unsigned long long _digest(const unsigned char *p, size_t n,
                                  int w, int h)
{
    unsigned long long d = 1469598103934665603ULL;
    size_t i;

    d = (d ^ (unsigned long long)(unsigned)w) * 1099511628211ULL;
    d = (d ^ (unsigned long long)(unsigned)h) * 1099511628211ULL;
    for (i = 0; i < n; i++)
        d = (d ^ p[i]) * 1099511628211ULL;
    return d;
}

/* Whether the run of bytes a spilled record wrote at @p at is these
 * bytes.
 *
 * A spilled record keeps a coverage mask's entry in memory and its bytes
 * in the file, so the entry has nothing to compare and the comparison is
 * made against the file. A piece at a time, into a buffer on the stack:
 * the one place a record reads a whole blob into is holding the mask
 * being painted, which has to stay standing across every pixel of it,
 * and a comparison made while the next page is being written down must
 * not take it away.
 *
 * A read that fails leaves the record short of a mark, which the caller
 * reads for itself.
 */
static int _cov_same(Xpost_Record *rec, Xpost_Spill_Off at,
                     const unsigned char *cov, size_t n)
{
    unsigned char buf[256];
    size_t done = 0;

    if (!rec->sp)
        return 0;
    while (done < n)
    {
        size_t want = n - done;

        if (want > sizeof buf)
            want = sizeof buf;
        if (!_sp_pull(rec, at + (Xpost_Spill_Off)done, buf, want))
            return 0;
        if (memcmp(buf, cov + done, want) != 0)
            return 0;
        done += want;
    }
    return 1;
}

/* Takes a coverage mask on its own, answering where in the record it
   was put. A mask is taken apart from the mark that paints it because
   the same mask is usually painted many times -- every occurrence of a
   glyph -- and is worth holding once. */
int xpost_record_mask(Xpost_Record *rec, const unsigned char *cov,
                      int w, int h, size_t *at)
{
    _Cover m;
    _Cover *held;
    size_t n, i, count;

    if (!rec || !cov || !at || w < 1 || h < 1)
        return 0;
    /* a record already short of a mark describes a page it cannot
       reproduce, and adding to it would only make the gap harder to
       see */
    if (rec->short_of_a_mark)
        return 0;
    /* what the two extents multiply to has to be a count this can walk,
       which is asked here rather than on the way past every byte */
    if (h > INT_MAX / w)
        return 0;
    n = (size_t)w * (size_t)h;

    /* the one the record already holds, if it holds it. Backwards,
       because a page's text repeats the glyph it last used far more
       often than the one it used first.
       The digest and the extents narrow it to a candidate and the bytes
       settle it, which is the whole of the saving this makes: a page of
       text costs its distinct glyphs and a placement apiece, and a
       digest believed on its own would cost it a glyph that was not the
       one it asked for. Where the record has spilled the bytes are in
       the file rather than under the entry, and the comparison is made
       there. */
    m.digest = _digest(cov, n, w, h);
    held = _msks(rec);
    count = _nmsk(rec);
    for (i = count; i--; )
    {
        int same;

        if (held[i].digest != m.digest
            || held[i].w != w || held[i].h != h)
            continue;
        same = held[i].cov ? (memcmp(held[i].cov, cov, n) == 0)
                           : _cov_same(rec, held[i].at, cov, n);
        /* a record that could not read back what it wrote is short of
           the mark it is holding, and gives nothing back from here */
        if (rec->short_of_a_mark)
            return 0;
        if (!same)
            continue;
        *at = i;
        return 1;
    }

    m.w = w;
    m.h = h;
    m.cov = NULL;
    m.at = 0;
    /* Where the record has spilled the coverage goes into the file and
       the entry keeps where it went. What stays in memory is the entry:
       the extents, the digest that answers whether the record already
       holds this glyph, and the place the bytes are -- which is the
       page's distinct glyphs rather than its ink. */
    if (rec->sp && !_sp_blob(rec, cov, n, &m.at))
    {
        _short(rec, ioerror);
        return 0;
    }
    if (xpost_strbuf_append(&rec->msk, &m, sizeof m))
    {
        _short(rec, VMerror);
        return 0;
    }
    if (rec->sp)
    {
        *at = count;
        return 1;
    }
    /* The entry goes down before the coverage is taken into it, so the
       copy is the record's from the moment it is made and nothing here
       is ever the only thing naming it. An entry the coverage could not
       be taken into comes straight back off the run, so the run holds
       whole entries and the count over it stays a count of them. */
    held = &_msks(rec)[count];
    held->cov = malloc(n);
    if (!held->cov)
    {
        rec->msk.len -= sizeof m;
        _short(rec, VMerror);
        return 0;
    }
    memcpy(held->cov, cov, n);
    rec->mskbytes += n;
    rec->covblocks++;
    *at = count;
    return 1;
}

/* Paints a mask the record already holds, at a place and in a colour.
   What it costs is the mark, the mask having been paid for once. */
int xpost_record_glyph(Xpost_Record *rec, const real *colour,
                       size_t at, real x, real y)
{
    const _Cover *m;
    real ops[3];

    if (!rec || !colour)
        return 0;
    if (rec->short_of_a_mark)
        return 0;
    /* a placement naming a mask the record does not hold would replay
       as nothing, which is a page missing a glyph: it is refused on the
       terms a mark that cannot be held is refused, so that the page is
       refused rather than put out short */
    if (at >= _nmsk(rec))
    {
        _short(rec, VMerror);
        return 0;
    }
    m = &_msks(rec)[at];

    ops[0] = (real)at;
    ops[1] = x;
    ops[2] = y;
    /* the rows the mask covers from where it was put, which is the one
       kind whose reach is read off the entry it names rather than off
       its own operands */
    return _put(rec, XPOST_RECORD_GLYPH, colour, ops, 3,
                y, y + (real)(m->h - 1));
}

/* The masks playing this record would put down, counting through the
   drawings it places as well as the ones it holds itself.

   A caller asks this to find out whether playing the record lays down
   glyph coverage, because coverage is the one thing a drawing cannot be
   moved by a fraction of a pixel: a mask carries the pixels a glyph
   rasterised to, and where those pixels take their coverage from is the
   fraction of a pixel the origin fell at. A record that holds no mask
   itself but places a drawing that does would answer none, and a caller
   trusting that would move it anywhere.

   The table of drawings names each one once however many placements
   name it, so a drawing placed many times is counted once. The walk is
   bounded by the depth a placement is allowed to reach, which
   xpost_record_place refuses to exceed. */
static size_t _mask_count_at(const Xpost_Record *rec, int depth)
{
    size_t n, i, ns;

    if (!rec || depth > XPOST_RECORD_NEST)
        return 0;
    n = _nmsk(rec);
    ns = _nsub(rec);
    for (i = 0; i < ns; i++)
        n += _mask_count_at(_subs(rec)[i], depth + 1);
    return n;
}

size_t xpost_record_mask_count(const Xpost_Record *rec)
{
    return rec ? _mask_count_at(rec, 0) : 0;
}

size_t xpost_record_mask_bytes(const Xpost_Record *rec)
{
    return rec ? rec->mskbytes : 0;
}

const unsigned char *xpost_record_mask_get(const Xpost_Record *rec, size_t i,
                                           int *w, int *h)
{
    const _Cover *m;

    /* a record short of a mark gives none of what it holds back, on the
       same terms as a replay of one */
    if (!rec || rec->short_of_a_mark || i >= _nmsk(rec))
        return NULL;
    m = &_msks(rec)[i];
    if (w) *w = m->w;
    if (h) *h = m->h;
    if (m->cov)
        return m->cov;
    if (!rec->sp)
        return NULL;
    /* The mask being painted, read back and held while it is painted.
       A mask is painted a pixel at a time and the interpreter runs
       between pixels, so what is standing here has to stay standing
       across as many returns as the mask has inked pixels -- which is
       why the one it is on is the one thing a spilled record always
       holds, and why asking for the same one again does not read it
       again. */
    return _sp_blob_get((Xpost_Record *)rec, m->at,
                        (size_t)m->w * (size_t)m->h, i, 0);
}

/* --- a drawing placed more than once ---------------------------------
   A form is captured as marks rather than as pixels, so one execution
   serves every placement under the same linear transformation. A use is
   one entry, which is why twenty-five placements cost one drawing. */

int xpost_record_place(Xpost_Record *rec, Xpost_Record *sub,
                       real dx, real dy)
{
    Xpost_Record **held;
    real *colour;
    real ops[3];
    real lo, hi;
    size_t i, at, count;
    int ok;

    if (!rec || !sub)
        return 0;
    /* a record already short of a mark describes a page it cannot
       reproduce, and adding to it would only make the gap harder to
       see */
    if (rec->short_of_a_mark)
        return 0;
    /* a drawing short of a mark cannot be played, so a page placing it
       is a page that cannot be painted whole */
    if (sub->short_of_a_mark)
    {
        _short(rec, VMerror);
        return 0;
    }
    /* A record placed inside itself would be played until something ran
       out, and a drawing nested deeper than a replay descends could not
       be played at all. Both are refused here, where the run making the
       placement is still in a position to be told, rather than when the
       page is painted. */
    if (sub == rec || sub->depth >= XPOST_RECORD_NEST)
        return 0;

    /* the one entry this drawing already has, if it has one: a drawing
       is named once however many placements name it, so what the table
       holds is the drawings and what the marks hold is the places */
    held = _subs(rec);
    count = _nsub(rec);
    at = count;
    for (i = 0; i < count; i++)
    {
        if (held[i] == sub)
        {
            at = i;
            break;
        }
    }
    if (at == count)
    {
        if (xpost_strbuf_append(&rec->sub, &sub, sizeof sub))
        {
            _short(rec, VMerror);
            return 0;
        }
        /* the run holds the drawing now, so the reference is the
           record's and the drawing outlives whatever else named it */
        (void)xpost_record_hold(sub);
    }

    /* a placement paints in the colours the drawing's own marks carry,
       so the place a mark's colour takes is filled with zeros and every
       entry's values stay laid out the one way */
    colour = calloc((size_t)rec->ncomp, sizeof *colour);
    if (!colour)
    {
        _short(rec, VMerror);
        return 0;
    }

    /* the rows the drawing reaches, carried down to where it is put. A
       drawing that reaches no row paints nothing wherever it is placed,
       and the placement is met only by the run its own origin falls in. */
    if (!xpost_record_extent(sub, &lo, &hi))
        lo = hi = 0.0;
    lo += dy;
    hi += dy;

    ops[0] = (real)at;
    ops[1] = dx;
    ops[2] = dy;
    ok = _put(rec, XPOST_RECORD_PLACE, colour, ops, 3, lo, hi);
    free(colour);
    if (!ok)
        return 0;
    /* and how deep the page now goes, which is what says whether a
       further placement of it could be played */
    if (sub->depth + 1 > rec->depth)
        rec->depth = sub->depth + 1;
    return 1;
}

size_t xpost_record_place_count(const Xpost_Record *rec)
{
    return rec ? _nsub(rec) : 0;
}

int xpost_record_depth(const Xpost_Record *rec)
{
    return rec ? rec->depth : 0;
}

/* The drawings this record places, and how many. A placed drawing is a
   record in its own right, which is how a form drawn many times costs
   its marks once. */
Xpost_Record *xpost_record_place_get(const Xpost_Record *rec, size_t i)
{
    /* a record short of a mark gives none of what it holds back, on the
       same terms as a replay of one */
    if (!rec || rec->short_of_a_mark || i >= _nsub(rec))
        return NULL;
    return _subs(rec)[i];
}

/* --- the screen in force ---------------------------------------------
   A screen entry is not a mark. It is played in the order it was made,
   exempt from the row filter a band replay applies, and it survives the
   page boundary -- state set before a band governs that band wherever on
   the page it was set. */

/* Write one screen entry down, its cell copied into the record's own
   memory. Shared by a screen the painting announced and by the one a
   page boundary opens the page after with. */
static int _screen_put(Xpost_Record *rec, int w, int h,
                       const unsigned char *cell)
{
    _Screen s;
    _Screen *held;
    real *colour;
    real idx;
    size_t count;
    size_t n = (size_t)w * (size_t)h;
    int ok;

    /* a screen paints nothing, so the place a mark's colour takes is
       filled with zeros and every entry's values stay laid out the one
       way */
    colour = calloc((size_t)rec->ncomp, sizeof *colour);
    if (!colour)
    {
        _short(rec, VMerror);
        return 0;
    }

    s.w = w;
    s.h = h;
    s.cell = NULL;
    s.at = 0;
    count = _nscr(rec);
    idx = (real)count;
    /* the cell goes where the coverage of a mask goes, and for the same
       reason: it is read by the pixel when the page is painted and never
       between */
    if (rec->sp && !_sp_blob(rec, cell, n, &s.at))
    {
        free(colour);
        _short(rec, ioerror);
        return 0;
    }
    if (xpost_strbuf_append(&rec->scr, &s, sizeof s))
    {
        free(colour);
        _short(rec, VMerror);
        return 0;
    }
    if (!rec->sp)
    {
        /* The entry goes down before the cell is taken into it, so the
           copy is the record's from the moment it is made and nothing
           here is ever the only thing naming it. An entry the cell could
           not be taken into comes straight back off the run, so the run
           holds whole entries and the count over it stays a count of
           them. */
        held = &_scrs(rec)[count];
        held->cell = malloc(n);
        if (!held->cell)
        {
            rec->scr.len -= sizeof s;
            free(colour);
            _short(rec, VMerror);
            return 0;
        }
        memcpy(held->cell, cell, n);
        rec->scrbytes += n;
        rec->cellblocks++;
    }

    ok = _put(rec, XPOST_RECORD_SCREEN, colour, &idx, 1, 0.0, 0.0);
    free(colour);
    return ok;
}

/* Takes a halftone screen. A screen is not a mark: it is the state the
   marks after it are rendered under, so it is held apart from them and
   reaches a replay in its own right. */
int xpost_record_screen(Xpost_Record *rec, int w, int h,
                        const unsigned char *cell)
{
    unsigned char *keep;
    size_t n;

    if (!rec || !cell || w < 1 || h < 1 || h > INT_MAX / w)
        return 0;
    /* a record already short of a mark describes a page it cannot
       reproduce, and adding to it would only make the gap harder to
       see */
    if (rec->short_of_a_mark)
        return 0;
    n = (size_t)w * (size_t)h;

    keep = malloc(n);
    if (!keep)
    {
        _short(rec, VMerror);
        return 0;
    }
    memcpy(keep, cell, n);
    if (!_screen_put(rec, w, h, cell))
    {
        free(keep);
        return 0;
    }
    /* what the page after a boundary is opened under, kept only once
       the run has taken this one */
    free(rec->htcell);
    rec->htcell = keep;
    rec->htw = w;
    rec->hth = h;
    return 1;
}

size_t xpost_record_screen_count(const Xpost_Record *rec)
{
    return rec ? _nscr(rec) : 0;
}

const unsigned char *xpost_record_screen_get(const Xpost_Record *rec,
                                             size_t i, int *w, int *h)
{
    const _Screen *s;

    /* a record short of a mark gives none of what it holds back, on the
       same terms as a replay of one */
    if (!rec || rec->short_of_a_mark || i >= _nscr(rec))
        return NULL;
    s = &_scrs(rec)[i];
    if (w) *w = s->w;
    if (h) *h = s->h;
    if (s->cell)
        return s->cell;
    if (!rec->sp)
        return NULL;
    return _sp_blob_get((Xpost_Record *)rec, s->at,
                        (size_t)s->w * (size_t)s->h, i, 1);
}

/* Empties the record for the next page, keeping what it has learned
   about its own limits. */
void xpost_record_clear(Xpost_Record *rec)
{
    size_t n;

    if (!rec)
        return;
    /* a record short of a mark answers every replay with the refusal,
       and the refusal is the one thing here that is not a mark of the
       page in hand */
    if (rec->short_of_a_mark)
        return;
    /* what the runs came to on the page ending, before the lengths that
       say it go back to nothing: the storage stays and is filled again,
       so it is what the record is resident for from here until a larger
       page fills more of it */
    n = _runfill(rec);
    if (n > rec->runhigh)
        rec->runhigh = n;
    _images_free(rec);
    _masks_free(rec);
    _screens_free(rec);
    /* the drawings the page placed are given up with the placements that
       named them, and one no other page places goes with them */
    _places_free(rec);
    rec->depth = 1;
    /* the runs keep what they took: a record is filled again by the page
       after, and what it costs is then the largest page rather than the
       sum of them */
    rec->mark.len = 0;
    rec->val.len = 0;
    /* The file goes back to the beginning, which is what keeping the
       runs is for a record that holds them. It is shortened as well, so
       that a job of one huge page and many small ones stops holding the
       huge one's space -- nothing depends on that, the writes of the
       page after reclaiming it either way, so a platform that will not
       shorten a file loses room and not correctness. */
    if (rec->sp)
    {
        rec->sp->wbuf.len = 0;
        rec->sp->rbuf.len = 0;
        rec->sp->end = rec->sp->base;
        rec->sp->wat = rec->sp->base;
        rec->sp->cord = 0;
        rec->sp->coff = rec->sp->base;
        rec->sp->nmark = 0;
        rec->sp->eany = rec->sp->bany = 0;
        rec->sp->blobwhich = (size_t)-1;
        (void)xpost_spill_truncate(rec->sp->f, rec->sp->base);
    }
    /* The screen the page ending was painted under opens the page
       beginning, because a page boundary is not a screen change: the
       machinery that announces one rebuilds the cell only where the
       screen it is built from has changed, so nothing would announce
       this one again and the page after would replay under whatever
       screen its target happened to hold. */
    if (rec->htcell)
        _screen_put(rec, rec->htw, rec->hth, rec->htcell);
}

/* Give up the entries a caller has finished with, which is everything
   but the coverage: the marks, the pictures and drawings they name and
   the screens they were made under. The masks stay because a placement
   names one by an index into the record and arrives after it. */
void xpost_record_spent(Xpost_Record *rec)
{
    if (!rec)
        return;
    _images_free(rec);
    _screens_free(rec);
    _places_free(rec);
    rec->depth = 1;
    rec->mark.len = 0;
    rec->val.len = 0;
}

/* Gives up everything the record holds while keeping the record
   itself: what a device does when its page has been played and it will
   take no more marks. */
void xpost_record_release(Xpost_Record *rec)
{
    if (!rec)
        return;
    _images_free(rec);
    _masks_free(rec);
    _screens_free(rec);
    _places_free(rec);
    rec->depth = 1;
    free(rec->htcell);
    rec->htcell = NULL;
    rec->htw = rec->hth = 0;
    /* and the file, which is storage like the runs are: a record that has
       given its runs back is resident for none of them, and one that has
       given its file back is holding no scratch space either */
    _sp_free(rec);
    /* The runs go back to the allocator rather than being emptied. A
       freed buffer is an empty one and takes marks again from nothing,
       which is what makes this a record the caller may go on holding. */
    xpost_strbuf_free(&rec->mark);
    xpost_strbuf_free(&rec->val);
    xpost_strbuf_free(&rec->img);
    xpost_strbuf_free(&rec->imgat);
    xpost_strbuf_free(&rec->msk);
    xpost_strbuf_free(&rec->scr);
    xpost_strbuf_free(&rec->sub);
    /* and the mark of how full they ever were goes with them: it says
       what the runs are resident for, and they are resident for nothing */
    rec->runhigh = 0;
}

/* Make the file and the state that reads and writes it, or answer that
   there is nowhere to make it. */
static int _sp_make(Xpost_Record *rec)
{
    unsigned char head[_SPILL_BASE];
    unsigned int v;
    _Spill *sp;

    sp = calloc(1, sizeof *sp);
    if (!sp)
        return 0;
    if (xpost_strbuf_init(&sp->wbuf, _SPILL_BUFFER)
        || xpost_strbuf_init(&sp->rbuf, _SPILL_WINDOW))
    {
        xpost_strbuf_free(&sp->wbuf);
        xpost_strbuf_free(&sp->rbuf);
        free(sp);
        return 0;
    }
    sp->f = xpost_spill_open();
    if (!sp->f)
    {
        xpost_strbuf_free(&sp->wbuf);
        xpost_strbuf_free(&sp->rbuf);
        free(sp);
        return 0;
    }
    /* What stands at the head of the file. Nothing else ever opens it --
       it has no name to be opened by -- so this is not for a reader to
       find its way with; it is here so that the day something does read
       one of these, it is told what shape it is in rather than left to
       assume. The width the values are in is the build's, because a
       record exists to be smaller than the page it draws and widening
       every coordinate would halve how much page one buys. */
    memset(head, 0, sizeof head);
    memcpy(head, _SPILL_MARK, 8);
    v = (unsigned int)sizeof(real);
    memcpy(head + 8, &v, sizeof v);
    v = (unsigned int)rec->ncomp;
    memcpy(head + 12, &v, sizeof v);
    if (!xpost_spill_write(sp->f, 0, head, sizeof head))
    {
        xpost_spill_close(sp->f);
        xpost_strbuf_free(&sp->wbuf);
        xpost_strbuf_free(&sp->rbuf);
        free(sp);
        return 0;
    }
    sp->base = _SPILL_BASE;
    sp->end = sp->base;
    sp->wat = sp->base;
    sp->coff = sp->base;
    sp->blobwhich = (size_t)-1;
    rec->sp = sp;
    return 1;
}

/* Moves what the record holds to a file, leaving almost nothing
   resident. The record answers exactly as it did before, readers going
   through the accessors above; what changes is where the bytes are. */
int xpost_record_spill(Xpost_Record *rec)
{
    size_t i, n;

    if (!rec)
        return 0;
    if (rec->sp)
        return 1;
    /* a record short of a mark describes a page it cannot reproduce, and
       moving what is left of that page is no use to anybody */
    if (rec->short_of_a_mark)
        return 0;
    if (!_sp_make(rec))
        return 0;

    /* What the record already holds goes into the file before anything
       is given up, so that a write that fails leaves the record exactly
       as it was -- holding everything, in memory, and able to go on. The
       page is not lost by a spill that could not be made; only its bound
       is. */
    n = _nmsk(rec);
    for (i = 0; i < n; i++)
    {
        _Cover *m = &_msks(rec)[i];

        if (!_sp_blob(rec, m->cov, (size_t)m->w * (size_t)m->h, &m->at))
            goto no;
    }
    n = _nscr(rec);
    for (i = 0; i < n; i++)
    {
        _Screen *s = &_scrs(rec)[i];

        if (!_sp_blob(rec, s->cell, (size_t)s->w * (size_t)s->h, &s->at))
            goto no;
    }
    n = _nimg(rec);
    for (i = 0; i < n; i++)
    {
        const Xpost_Record_Image *img = &_imgs(rec)[i];
        _ImgAt *where = &_imgats(rec)[i];
        size_t each = (size_t)img->width
                    * (img->planar ? 1u : (size_t)img->ncomp);
        size_t nrun = (size_t)img->height * (img->planar
                                             ? (size_t)img->ncomp : 1u);

        if (!_sp_blob(rec, img->samples, nrun * each, &where->samples))
            goto no;
        if (img->mbits
            && !_sp_blob(rec, img->mbits,
                         (size_t)img->mrowb * (size_t)img->mh,
                         &where->mbits))
            goto no;
    }
    n = _nmark(rec);
    for (i = 0; i < n; i++)
    {
        const _Mark *m = &_marks(rec)[i];
        const real *colour = _vals(rec) + m->at;
        const real *ops = m->nops ? colour + rec->ncomp : NULL;

        if (!_sp_entry(rec, m->kind, colour, ops, m->nops, m->lo, m->hi))
            goto no;
        _sp_reach(rec, m->kind, ops, m->nops, m->lo, m->hi);
    }

    /* and only now what the file has taken over is given up */
    n = _nmsk(rec);
    for (i = 0; i < n; i++)
    {
        free(_msks(rec)[i].cov);
        _msks(rec)[i].cov = NULL;
    }
    rec->mskbytes = 0;
    rec->covblocks = 0;
    n = _nscr(rec);
    for (i = 0; i < n; i++)
    {
        free(_scrs(rec)[i].cell);
        _scrs(rec)[i].cell = NULL;
    }
    rec->scrbytes = 0;
    rec->cellblocks = 0;
    n = _nimg(rec);
    for (i = 0; i < n; i++)
    {
        Xpost_Record_Image *img = &_imgs(rec)[i];
        size_t each = (size_t)img->width
                    * (img->planar ? 1u : (size_t)img->ncomp);
        size_t nrun = (size_t)img->height * (img->planar
                                             ? (size_t)img->ncomp : 1u);

        free((void *)img->samples);
        img->samples = NULL;
        rec->imgbytes -= nrun * each;
        rec->imgblocks--;
        if (img->mbits)
        {
            free((void *)img->mbits);
            img->mbits = NULL;
            rec->imgbytes -= (size_t)img->mrowb * (size_t)img->mh;
            rec->imgblocks--;
        }
    }
    /* The two runs go back to the allocator with the marks that were in
       them, and the mark of how full they ever were goes with them: they
       are resident for nothing from here. */
    xpost_strbuf_free(&rec->mark);
    xpost_strbuf_free(&rec->val);
    rec->runhigh = 0;
    return 1;

  no:
    _sp_free(rec);
    return 0;
}

int xpost_record_spill_shorten(Xpost_Record *rec, long long keep)
{
    if (!rec || !rec->sp || keep < 0)
        return 0;
    /* the buffers may be holding what is about to stop being there */
    rec->sp->rbuf.len = 0;
    rec->sp->wbuf.len = 0;
    return xpost_spill_truncate(rec->sp->f, (Xpost_Spill_Off)keep);
}

int xpost_record_spilled(const Xpost_Record *rec)
{
    return rec && rec->sp ? 1 : 0;
}

int xpost_record_error(const Xpost_Record *rec)
{
    if (!rec || !rec->short_of_a_mark)
        return 0;
    return rec->why ? rec->why : VMerror;
}

size_t xpost_record_count(const Xpost_Record *rec)
{
    if (!rec)
        return 0;
    return rec->sp ? rec->sp->nmark : _nmark(rec);
}

/* What an allocator keeps beside a block, which a sum of the sizes asked
   for cannot see. Two words is what the common ones take -- a header
   with the block's size in it, and the rounding up to the alignment the
   next block must start on. It is charged per block rather than
   measured, because there is no portable way to ask; the point of
   charging it at all is that a record holding many small blocks holds
   measurably more than their bytes, and a page of text (a coverage mask
   per distinct glyph) and a page that keeps changing its screen (a
   threshold cell per change) are both that record. */
#define _BLOCK_OVER (2 * sizeof(void *))

/* How many blocks the record is holding, so that what an allocator
   keeps beside each of them is charged once per block.
 *
 * The blocks the entries point at are counted where they are taken and
 * given up rather than by walking the entries, so that this is a reading
 * and not a pass. What asks is a caller deciding, once per mark, where a
 * page's marks should be kept; a count that walked every mask, every
 * screen and every table of every picture would make that decision cost
 * the record it is about, and on a page of text it would be the masks
 * times the marks. */
static size_t _blocks(const Xpost_Record *rec)
{
    size_t n = 1;               /* the record itself */

    if (rec->mark.s)  n++;
    if (rec->val.s)   n++;
    if (rec->img.s)   n++;
    if (rec->imgat.s) n++;
    if (rec->msk.s)   n++;
    if (rec->scr.s)   n++;
    if (rec->sub.s)   n++;
    if (rec->htcell)  n++;
    /* a mask's coverage, a screen's cell, and each table a picture was
       given, are one block apiece */
    return n + rec->covblocks + rec->cellblocks + rec->imgblocks;
}

/* What the record is holding in memory at this moment. */
size_t xpost_record_resident(const Xpost_Record *rec)
{
    size_t runs;

    if (!rec)
        return 0;
    /* What the runs hold and not what they have room for. A run grows by
       doubling, so the room past what is in it is up to as much again,
       and none of that room is paid for until a mark is written into it:
       a record answering its capacity would answer up to twice what the
       page it holds has made resident. What is answered instead is the
       most the runs have ever held, which is what they are resident for
       -- storage a run has been filled to stays resident after a page
       boundary empties it, because the run keeps it to be filled
       again. */
    runs = _runfill(rec);
    if (runs < rec->runhigh)
        runs = rec->runhigh;
    /* and what the entries point at as it stands, which is not the same
       question: those blocks are given up at a page boundary rather than
       kept, so what a record is resident for there is the page in hand
       and not the largest one */
    return sizeof *rec + runs
         + rec->imgbytes + rec->mskbytes + rec->scrbytes
         + (rec->htcell ? (size_t)rec->htw * (size_t)rec->hth : 0)
         + (rec->sp ? _sp_resident(rec->sp) : 0)
         + _BLOCK_OVER * _blocks(rec);
}

size_t xpost_record_bytes(const Xpost_Record *rec)
{
    if (!rec)
        return 0;
    /* What holding the drawing costs, wherever it is being held. A
       spilled record is resident for almost none of it and the file
       carries the rest, and it is still the same drawing: a caller
       weighing what a page's marks come to against the raster they save
       is asking about the drawing, and would be told a page of nothing
       by an answer that only counted memory. What is resident is
       xpost_record_resident, which is the other question. */
    return xpost_record_resident(rec)
         + (rec->sp ? (size_t)xpost_spill_size(rec->sp->f) : 0);
}

/* Says the record is short of a mark it could not take, and asks
   whether it is. A record short of a mark describes a page it cannot
   reproduce, so from that point it gives nothing back rather than
   giving back a drawing that is missing something. */
void xpost_record_lost(Xpost_Record *rec)
{
    if (rec)
        _short(rec, VMerror);
}

int xpost_record_failed(const Xpost_Record *rec)
{
    return rec ? rec->short_of_a_mark : 0;
}

/* One mark, by position: its kind, its colour, and the numbers that
   place it. What comes back points into the record and is good until
   the record next changes. */
int xpost_record_get(const Xpost_Record *rec, size_t i,
                     Xpost_Record_Kind *kind, const real **colour,
                     const real **ops, int *nops)
{
    const _Mark *m;

    /* a record short of a mark describes a page it cannot reproduce, so
       it gives none of them back: what a caller would build from what
       is left is a page missing something, which looks like a page */
    if (!rec || rec->short_of_a_mark)
        return 0;
    /* An entry a spilled record hands back is copied out of the file
       first, into the one place it hands entries back from. The pointer
       handed back points there rather than into the record, which is the
       same promise differently backed: the bytes under it are good until
       the next entry is asked for, and the buffer they came through is
       refilled while the interpreter is away between two marks. */
    if (rec->sp)
    {
        Xpost_Record *w = (Xpost_Record *)rec;

        if (i >= w->sp->nmark || !_sp_seek(w, i))
            return 0;
        return _sp_read(w, kind, colour, ops, nops);
    }
    if (i >= _nmark(rec))
        return 0;
    m = &_marks(rec)[i];
    *kind = m->kind;
    *colour = _vals(rec) + m->at;
    *ops = m->nops ? _vals(rec) + m->at + rec->ncomp : NULL;
    *nops = m->nops;
    return 1;
}

/* The rows the record's marks reach between, which is what a caller
   banding a page needs before it can decide how many bands there are. */
int xpost_record_extent(const Xpost_Record *rec, real *lo, real *hi)
{
    const _Mark *m;
    size_t i, n;
    int any = 0;

    if (!rec)
        return 0;
    /* kept as the marks are written down where the record has spilled,
       for the reason the box is */
    if (rec->sp)
    {
        if (!rec->sp->eany)
            return 0;
        *lo = rec->sp->ey_lo;
        *hi = rec->sp->ey_hi;
        return 1;
    }
    m = _marks(rec);
    n = _nmark(rec);
    for (i = 0; i < n; i++)
    {
        /* a screen paints nothing and so reaches no row: what is being
           asked is where the ink is, and a record holding screens and
           no mark reaches nothing */
        if (m[i].kind == XPOST_RECORD_SCREEN)
            continue;
        if (!any)
        {
            *lo = m[i].lo;
            *hi = m[i].hi;
            any = 1;
            continue;
        }
        if (m[i].lo < *lo) *lo = m[i].lo;
        if (m[i].hi > *hi) *hi = m[i].hi;
    }
    return any;
}

/* The columns one entry reaches, from its own operands and from what it
   names. A kind that paints nothing answers an empty span. */
static int _span_of(const Xpost_Record *rec, Xpost_Record_Kind kind,
                    const real *ops, int nops, real *x0, real *x1)
{
    int i, any = 0;

    /* Every kind an entry can be written down under carries at least one
       operand, so an entry with none is an entry naming no position:
       it reaches no column, and the kinds below may read the operands
       their own kind states. */
    if (!ops || nops < 1)
        return 0;
    switch (kind)
    {
        case XPOST_RECORD_PUTPIX:
            *x0 = *x1 = ops[0];
            return 1;
        case XPOST_RECORD_BLENDPIX:
            *x0 = *x1 = ops[1];
            return 1;
        case XPOST_RECORD_DRAWLINE:
            *x0 = ops[0] < ops[2] ? ops[0] : ops[2];
            *x1 = ops[0] < ops[2] ? ops[2] : ops[0];
            return 1;
        case XPOST_RECORD_FILLRECT:
            *x0 = ops[2] < 0 ? ops[0] + ops[2] : ops[0];
            *x1 = ops[2] < 0 ? ops[0] : ops[0] + ops[2];
            return 1;
        case XPOST_RECORD_FILLPOLY:
            for (i = 0; i * 2 + 2 < nops; i++)
            {
                real x = ops[i * 2 + 1];

                if (x == XPOST_PATH_BREAK)
                    continue;
                if (!any) { *x0 = *x1 = x; any = 1; }
                else if (x < *x0) *x0 = x;
                else if (x > *x1) *x1 = x;
            }
            return any;
        case XPOST_RECORD_IMAGE:
        {
            const Xpost_Record_Image *img;
            real a, b, t;

            if (!ops || (size_t)ops[0] >= _nimg(rec))
                return 0;
            img = &_imgs(rec)[(size_t)ops[0]];
            a = img->xoff;
            b = img->xoff + (real)img->width * img->xscale;
            if (a > b) { t = a; a = b; b = t; }
            if (a < img->cx0) a = img->cx0;
            if (b > img->cx1) b = img->cx1;
            *x0 = a;
            *x1 = b;
            return b >= a;
        }
        case XPOST_RECORD_GLYPH:
        {
            const _Cover *c;

            if (!ops || (size_t)ops[0] >= _nmsk(rec))
                return 0;
            c = &_msks(rec)[(size_t)ops[0]];
            *x0 = ops[1];
            *x1 = ops[1] + (real)(c->w - 1);
            return 1;
        }
        case XPOST_RECORD_PLACE:
        {
            real sx0, sy0, sx1, sy1;

            if (!ops || (size_t)ops[0] >= _nsub(rec))
                return 0;
            if (!xpost_record_box(_subs(rec)[(size_t)ops[0]],
                                  &sx0, &sy0, &sx1, &sy1))
                return 0;
            *x0 = sx0 + ops[1];
            *x1 = sx1 + ops[1];
            return 1;
        }
        case XPOST_RECORD_SCREEN:
            return 0;
    }
    return 0;
}

/* The bounding box of everything the record holds, in device
   coordinates, or nothing where it holds no marks. */
int xpost_record_box(const Xpost_Record *rec, real *x0, real *y0,
                     real *x1, real *y1)
{
    const _Mark *m;
    const real *vals;
    size_t i, n;
    int any = 0;

    if (!rec || !x0 || !y0 || !x1 || !y1)
        return 0;
    /* A spilled record keeps the answer as its marks are written down,
       so it is read rather than walked: the walk below is over the file
       and this is asked once per placement of a drawing and once per
       page that places one. */
    if (rec->sp)
    {
        if (!rec->sp->bany)
            return 0;
        *x0 = rec->sp->bx0;
        *x1 = rec->sp->bx1;
        *y0 = rec->sp->by0;
        *y1 = rec->sp->by1;
        return 1;
    }
    m = _marks(rec);
    vals = _vals(rec);
    n = _nmark(rec);
    for (i = 0; i < n; i++)
    {
        real a, b;

        if (!_span_of(rec, m[i].kind,
                      m[i].nops ? vals + m[i].at + rec->ncomp : NULL,
                      m[i].nops, &a, &b))
            continue;
        if (!any)
        {
            *x0 = a; *x1 = b;
            *y0 = m[i].lo; *y1 = m[i].hi;
            any = 1;
            continue;
        }
        if (a < *x0) *x0 = a;
        if (b > *x1) *x1 = b;
        if (m[i].lo < *y0) *y0 = m[i].lo;
        if (m[i].hi > *y1) *y1 = m[i].hi;
    }
    return any;
}

/* Whether a mark reaches a run of rows. A mark meeting the range at all
   is played whole: a shape has to be converted whole to be right about
   any part of it, so the range says which marks are played and never
   trims one. Stated once, because a replay reaches the marks two ways --
   as the loop below, and as the step a replay that returns to its caller
   between marks resumes with -- and the two picking different marks for
   the same rows would paint the same page differently depending on which
   asked. */
static int _meets_rows(Xpost_Record_Kind kind, real mlo, real mhi,
                       real lo, real hi)
{
    /* A screen is met by every run of rows. It paints nothing, and what
       it says governs whatever is painted after it wherever on the page
       that lands -- so a replay of any run has to pass through the same
       screens in the same order as a replay of the whole page, or the
       rows it paints are not the rows the whole page would have had. */
    if (kind == XPOST_RECORD_SCREEN)
        return 1;

    /* A mark's reach is in the coordinates it was made with and a run of
       rows is in whole rows, so the reach is taken out to the rows it
       falls in before the two are compared. A shape reaching from
       halfway down one row to halfway down another inks both of them --
       a stroke of any width has ends at a half row, being a rectangle
       around a segment -- and a run ending at the first of those would
       judge the shape not to reach it and leave the page short of a
       mark. Every kind puts a coordinate on a row by dropping the
       fraction, so that is what taking it out to whole rows is.

       Erring outward here costs a visit to a mark that then paints
       nothing in the run; erring inward loses the mark from the page,
       which is wrong output rather than slow output. */
    return !(floor((double)mhi) < (double)lo
          || floor((double)mlo) > (double)hi);
}

/* Whether a mark reaches any of a range of rows, which is the test
   every band-wise reader below is built on. */
static int _meets(const _Mark *m, real lo, real hi)
{
    return _meets_rows(m->kind, m->lo, m->hi, lo, hi);
}

/* The last mark meeting a range of rows -- what a caller wants when
   only the topmost mark of a band matters. */
int xpost_record_last(const Xpost_Record *rec, real lo, real hi, size_t *at)
{
    const _Mark *marks;
    size_t n;

    /* a record short of a mark gives none of them back, on the same
       terms as a replay of one */
    if (!rec || !at || rec->short_of_a_mark)
        return 0;
    /* Where the marks are in a file it is a pass forward, keeping the
       last one that met the rows, because reading a file backwards is
       the one thing a file is bad at. What it replaces is a walk that
       usually stopped near the end, so a band pays a pass here it did
       not pay before; that pass is the file and the pass the band's own
       replay makes is the file again, so a band reads what it holds
       twice rather than once. It is asked once per band and answers a
       question that saves painting the band at all. */
    if (rec->sp)
    {
        Xpost_Record *w = (Xpost_Record *)rec;
        size_t i, found = 0;
        int any = 0;

        if (!_sp_seek(w, 0))
            return 0;
        for (i = 0; i < w->sp->nmark; i++)
        {
            Xpost_Record_Kind kind;
            real mlo, mhi;

            if (!_sp_rows(w, &mlo, &mhi, &kind))
                return 0;
            if (kind != XPOST_RECORD_SCREEN
                && _meets_rows(kind, mlo, mhi, lo, hi))
            {
                found = i;
                any = 1;
            }
            if (i + 1 < w->sp->nmark && !_sp_step(w))
                return 0;
        }
        if (!any)
            return 0;
        *at = found;
        return 1;
    }
    marks = _marks(rec);
    n = _nmark(rec);
    /* backwards, stopping at the first one found: what is being asked
       is which mark had the last word over the run, and a run with
       anything in it is answered from near the end of the record rather
       than from a pass over the whole of it.

       A screen is stepped over. It is met by every run, so a caller
       asking whether a run comes to nothing but the colour the page was
       cleared to would be told no by the mere presence of one -- and
       every band of a screening device's page would then be painted,
       which is the cost this question exists to avoid. */
    while (n--)
        if (marks[n].kind != XPOST_RECORD_SCREEN
            && _meets(&marks[n], lo, hi))
        {
            *at = n;
            return 1;
        }
    return 0;
}

/* The next mark at or after a position that meets the given rows, for
   a caller walking a band a mark at a time rather than through a
   player. */
int xpost_record_next(const Xpost_Record *rec, size_t from, real lo, real hi,
                      size_t *at)
{
    const _Mark *marks;
    size_t i, n;

    /* a record short of a mark gives none of them back, on the same
       terms as a replay of one */
    if (!rec || !at || rec->short_of_a_mark)
        return 0;
    if (rec->sp)
    {
        Xpost_Record *w = (Xpost_Record *)rec;
        size_t k;

        if (from >= w->sp->nmark || !_sp_seek(w, from))
            return 0;
        for (k = from; k < w->sp->nmark; k++)
        {
            Xpost_Record_Kind kind;
            real mlo, mhi;

            if (!_sp_rows(w, &mlo, &mhi, &kind))
                return 0;
            if (_meets_rows(kind, mlo, mhi, lo, hi))
            {
                *at = k;
                return 1;
            }
            if (!_sp_step(w))
                return 0;
        }
        return 0;
    }
    marks = _marks(rec);
    n = _nmark(rec);
    for (i = from; i < n; i++)
    {
        if (_meets(&marks[i], lo, hi))
        {
            *at = i;
            return 1;
        }
    }
    return 0;
}

/* Walks the marks that meet a range of rows, handing each to the
   player. The order is the order they were taken in, which is the
   order they must be painted in. */
int xpost_record_replay(const Xpost_Record *rec, real lo, real hi,
                        Xpost_Record_Player player, void *data)
{
    const _Mark *marks;
    const real *vals;
    size_t i, n;

    if (!rec || !player)
        return 0;
    /* what is played back has to be the whole of what was recorded: a
       record missing a mark would paint a page missing one, and a page
       missing a mark looks like a page */
    if (rec->short_of_a_mark)
        return rec->why ? rec->why : VMerror;
    /* Where the marks are in a file the same walk is made through the
       one place entries are read into, which is what
       xpost_record_next and xpost_record_get already do: stated once so
       that the two visit the same marks for the same rows. */
    if (rec->sp)
    {
        size_t at = 0;

        while (xpost_record_next(rec, at, lo, hi, &at))
        {
            Xpost_Record_Kind kind;
            const real *colour;
            const real *ops;
            int nops, ret;

            if (!xpost_record_get(rec, at, &kind, &colour, &ops, &nops))
                return 0;
            ret = player(data, kind, colour, ops, nops);
            if (ret)
                return ret;
            at++;
        }
        /* A walk that ended because the record stopped answering is not
           a walk that reached the end of the page. The file has been
           shortened under a descriptor nobody else holds, or a read
           failed below; either way what has been played is part of a
           page, and a part of a page looks like a page. */
        if (rec->short_of_a_mark)
            return rec->why ? rec->why : VMerror;
        return 0;
    }
    marks = _marks(rec);
    vals = _vals(rec);
    n = _nmark(rec);
    for (i = 0; i < n; i++)
    {
        const _Mark *m = &marks[i];
        int ret;

        if (!_meets(m, lo, hi))
            continue;
        ret = player(data, m->kind, vals + m->at,
                     m->nops ? vals + m->at + rec->ncomp : NULL,
                     m->nops);
        if (ret)
            return ret;
    }
    return 0;
}
