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

/* A file is an entity inside virtual memory and a stream struct outside
   it, and what the entity holds is a handle on the stream.
 *
   Everything else virtual memory holds names its storage by entity
   number, so nothing in it depends on where the process put anything: an
   image of it read back at another address in another process resolves
   the same. A stream's address would be the exception, and the four file
   constructors -- a disk file, a memory file, a stream a procedure
   supplies or disposes of, and a filter -- are where one could be
   written in. So each is built here and its entity read: an entity
   carrying a handle is the width of a handle, and what it holds is not
   the address of the struct it resolves to.
 *
   The handle is also the whole of what an object naming the file has to
   go on, and every read of it is followed by a call through the method
   table the stream begins with. So a handle that names no live stream,
   and a genuine handle read out of an entity it was not issued against,
   each resolve to no stream -- the answer every caller already handles:
   status reports the file closed, closefile has nothing left to do, and
   a read or a write reports an ioerror. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_context.h"
#include "xpost_array.h"
#include "xpost_file.h"

#include "xpost_test.h"

/* The width of the handle an entity carries. Stated here rather than
   taken from the file layer, so a change of width has to be made in both
   places to go unreported. */
#define HANDLE_WIDTH (sizeof(unsigned int))

/* Enough of an entity to hold either a handle or a host address, which
   is what the two are compared over. */
#define PAYLOAD_WIDTH (HANDLE_WIDTH > sizeof(void *) \
                       ? HANDLE_WIDTH : sizeof(void *))

enum { NKINDS = 5 };

/* Read what an entity holds, up to what an address would take. The free
   list hands back the closest entity it has rather than one of exactly
   the size asked for, so an entity may be wider than the handle written
   into it. */
static int payload_of(Xpost_Memory_File *mem, unsigned int ent,
                      unsigned char *out, unsigned int *sz)
{
    if (!xpost_memory_table_get_size(mem, ent, sz))
        return 0;
    if (*sz > PAYLOAD_WIDTH)
        *sz = (unsigned int)PAYLOAD_WIDTH;
    memset(out, 0, PAYLOAD_WIDTH);
    return xpost_memory_get(mem, ent, 0, *sz, out);
}

/* The object naming a file entity. Two of the checks below arrive with
   an entity and no object naming it, and everything they ask takes the
   object. */
static Xpost_Object file_object_of(unsigned int ent)
{
    Xpost_Object o = { 0 };

    o.mark_.tag = filetype;
    o.mark_.pad0 = 0;
    o.mark_.padw = ent;
    return o;
}

/* Whether what an entity holds is the address of the stream it resolves
   to. An entity holding no stream is not asked: there is no address for
   it to be. */
static int holds_the_address(Xpost_Memory_File *mem, unsigned int ent)
{
    Xpost_File *fp = xpost_file_get_file_pointer(mem, file_object_of(ent));
    unsigned char payload[PAYLOAD_WIDTH];
    unsigned int sz = 0;

    if (!fp)
        return 0;
    if (!payload_of(mem, ent, payload, &sz))
        return 0;
    if (sz > sizeof fp)
        sz = (unsigned int)sizeof fp;
    return memcmp(payload, &fp, sz) == 0;
}

/* A procedure a stream may be built over. It is never asked for bytes
   here -- these files are examined, not read -- so what it does is
   immaterial; that it is an executable array is not, since the stream a
   procedure disposes of calls it as it closes. */
static Xpost_Object a_procedure(Xpost_Context *ctx)
{
    return xpost_object_cvx(xpost_array_cons(ctx, 0));
}

int main(void)
{
    Xpost_Context *ctx;
    Xpost_Memory_File *mem;
    Xpost_Object f[NKINDS];
    const char *name[NKINDS];
    Xpost_Object source;
    FILE *disk;
    unsigned int ent, i;
    char message[160];

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }
    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_RETURN, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        xpost_quit();
        return verdict();
    }
    mem = ctx->lo;

    disk = tmpfile();
    if (!disk)
    {
        report_failure("cannot open a temporary file");
        xpost_destroy(ctx);
        xpost_quit();
        return verdict();
    }

    name[0] = "a disk file";
    f[0] = xpost_file_cons(mem, disk, 1);
    name[1] = "a memory file";
    f[1] = xpost_file_cons_readstring(mem, (const unsigned char *)"4142>", 5);
    name[2] = "a stream a procedure supplies";
    f[2] = xpost_file_cons_procsource(ctx, a_procedure(ctx));
    name[3] = "a stream a procedure disposes of";
    f[3] = xpost_file_cons_proctarget(ctx, a_procedure(ctx));
    /* a filter over a stream of its own, which is the source it reads */
    source = xpost_file_cons_readstring(mem, (const unsigned char *)"4142>", 5);
    name[4] = "a filter";
    {
        int ferr = 0;

        f[4] = xpost_file_cons_filter_hex(mem, source, &ferr);
    }

    for (i = 0; i < NKINDS; i++)
        if (xpost_object_get_type(f[i]) != filetype)
        {
            report_failure("cannot build %s", name[i]);
            xpost_destroy(ctx);
            xpost_quit();
            return verdict();
        }

    /* ---- what the entity holds is a handle, not an address ---- */
    for (i = 0; i < NKINDS; i++)
    {
        ent = f[i].mark_.padw;
        sprintf(message, "%s resolves to its stream", name[i]);
        check(xpost_file_get_file_pointer(mem, f[i]) != NULL, message);

        sprintf(message, "%s holds no address of its stream", name[i]);
        check(!holds_the_address(mem, ent), message);
    }

    /* ---- a handle names no stream from an entity it was not issued
       against ---- */
    {
        unsigned char borrowed[PAYLOAD_WIDTH];
        unsigned char kept[PAYLOAD_WIDTH];
        unsigned int szfrom = 0, szto = 0;
        unsigned int from = f[0].mark_.padw;
        unsigned int to = f[1].mark_.padw;

        if (payload_of(mem, from, borrowed, &szfrom) &&
            payload_of(mem, to, kept, &szto) &&
            szfrom >= HANDLE_WIDTH && szto >= HANDLE_WIDTH)
        {
            check(xpost_memory_put(mem, to, 0, HANDLE_WIDTH, borrowed),
                  "one file's handle can be written into another's entity");
            check(xpost_file_get_file_pointer(mem, f[1]) == NULL,
                  "a handle read from an entity it was not issued against "
                  "resolves to no stream");
            check(xpost_memory_put(mem, to, 0, HANDLE_WIDTH, kept),
                  "the entity's own handle can be written back");
            check(xpost_file_get_file_pointer(mem, f[1]) != NULL,
                  "and the file resolves to its stream again");
        }
        else
            report_failure("cannot read the two entities to compare them");
    }

    /* ---- a handle naming no live stream resolves to none ---- */
    for (i = 0; i < NKINDS; i++)
    {
        unsigned char kept[PAYLOAD_WIDTH];
        unsigned int sz = 0;
        unsigned int fabricated = 0x7ffffffeu;

        ent = f[i].mark_.padw;
        if (!payload_of(mem, ent, kept, &sz) || sz < HANDLE_WIDTH)
        {
            report_failure("cannot read the entity of %s", name[i]);
            continue;
        }
        check(xpost_memory_put(mem, ent, 0, sizeof fabricated, &fabricated),
              "a handle of the program's own making can be written in");
        if (xpost_file_get_file_pointer(mem, f[i]) != NULL)
        {
            /* what it resolved to is nothing this test may call through,
               so the two answers below go unasked */
            report_failure("%s carrying a handle naming nothing resolves to "
                           "a stream", name[i]);
        }
        else
        {
            sprintf(message, "%s carrying such a handle reports itself closed",
                    name[i]);
            check(xpost_file_get_status(mem, f[i]) == 0, message);
            sprintf(message,
                    "closing %s carrying such a handle has nothing to do",
                    name[i]);
            check(xpost_file_object_close(mem, f[i]) == 0, message);
        }
        check(xpost_memory_put(mem, ent, 0, sz, kept),
              "the entity's own handle can be written back");
        sprintf(message, "%s resolves to its stream again", name[i]);
        check(xpost_file_get_file_pointer(mem, f[i]) != NULL, message);
    }

    /* ---- a closed file gives its place in the record back ---- */
    {
        unsigned int before = 0, after = 0;
        Xpost_Object one, two;

        /* Files come and go as a job runs -- every filter it builds is
           one -- so a record entry a closed file kept would be a record
           that only grows. What a file's entity carries is its entry, so
           opening one after another has closed says whether the entry
           went back: the record hands out the lowest it has free. */
        one = xpost_file_cons_readstring(mem, (const unsigned char *)"x", 1);
        if (xpost_object_get_type(one) != filetype ||
            !xpost_memory_get(mem, one.mark_.padw, 0, sizeof before, &before))
            report_failure("cannot build a file to close again");
        else
        {
            check(xpost_file_object_close(mem, one) == 0,
                  "a file built to be closed again closes");
            two = xpost_file_cons_readstring(mem,
                                             (const unsigned char *)"x", 1);
            if (xpost_object_get_type(two) != filetype ||
                !xpost_memory_get(mem, two.mark_.padw, 0, sizeof after,
                                  &after))
                report_failure("cannot build the file that follows it");
            else
            {
                check(after <= before,
                      "the entry a closed file held is free for the next");
                check(xpost_file_object_close(mem, two) == 0,
                      "and that file closes too");
            }
        }
    }

    /* ---- and every file entity the interpreter itself holds carries a
       handle too: the streams a job is started with are built by the same
       constructors ---- */
    {
        Xpost_Memory_File *banks[2];
        unsigned int b;
        unsigned int seen = 0;

        banks[0] = ctx->lo;
        banks[1] = ctx->gl;
        for (b = 0; b < 2; b++)
        {
            Xpost_Memory_File *m = banks[b];

            for (ent = m->start; ent < m->table.nextent; ent++)
            {
                if (m->table.tab[ent].tag != filetype)
                    continue;
                seen++;
                if (holds_the_address(m, ent))
                {
                    report_failure("file entity %u holds the address of the "
                                   "stream it resolves to", ent);
                    break;
                }
            }
        }
        check(seen >= NKINDS,
              "the sweep reached at least the files built here");
    }

    /* the streams close as the context goes: a file left open is a file
       the teardown answers for, and this test's business is what its
       entity holds while it is open */
    xpost_destroy(ctx);
    xpost_quit();

    return verdict();
}
