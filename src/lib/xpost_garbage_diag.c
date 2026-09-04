/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_garbage_diag.c
 * @brief Reporting what the collector did, for a run that asks.
 *
 * Diagnosis only: nothing here decides anything the collector does.
 */

/* The collector's independent diagnostics, out of the mainline collect
   path: a BFS reachability verifier with its own visited set
   (XPOST_GC_VERIFY), an entity census (XPOST_GC_CENSUS piggybacked on
   the verifier), and the cross-bank scan reporting global containers
   that reference a dying local entity (XPOST_GC_XBANK_CHECK). All are
   environment-gated at their call sites in xpost_garbage.c and cost
   nothing when the variables are unset. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_dict.h"
#include "xpost_garbage.h"

/* P0 diagnostic: independent reachability verifier. BFS from the same
   roots with a private visited set and unconditional full descent;
   report any reachable entity whose mark bit is clear, with its parent. */
typedef struct { unsigned int ent; int bank; unsigned int parent; int pbank; } _vq_item;
static _vq_item *_vq; static unsigned int _vq_n, _vq_cap;
static unsigned char *_vseen_lo, *_vseen_gl; static unsigned int _vseen_lo_n, _vseen_gl_n;

static void _verify_push(Xpost_Context *ctx, Xpost_Object o,
                         unsigned int parent, int pbank)
{
    unsigned int ent; int bank;
    Xpost_Memory_File *m;
    unsigned char *seen;
    Xpost_Object_Type t = xpost_object_get_type(o);
    if (t != arraytype && t != dicttype && t != stringtype && t != filetype)
        return;
    m = xpost_context_select_memory(ctx, o);
    bank = (m == ctx->gl);
    ent = (t == filetype) ? (unsigned int)o.mark_.padw : (unsigned int)xpost_object_get_ent(o);
    seen = bank ? _vseen_gl : _vseen_lo;
    if (ent >= (bank ? _vseen_gl_n : _vseen_lo_n)) return;
    if (seen[ent]) return;
    seen[ent] = 1;
    if (_vq_n == _vq_cap)
    {
        unsigned int newcap = _vq_cap ? _vq_cap * 2 : 65536;
        _vq_item *tmp = realloc(_vq, newcap * sizeof *_vq);
        if (!tmp)
            return; /* keep the existing queue; the verify pass skips this node */
        _vq = tmp;
        _vq_cap = newcap;
    }
    _vq[_vq_n].ent = ent; _vq[_vq_n].bank = bank;
    _vq[_vq_n].parent = parent; _vq[_vq_n].pbank = pbank;
    _vq_n++;
}

static void _verify_stack(Xpost_Context *ctx, Xpost_Memory_File *mem,
                          unsigned int stackadr)
{
    Xpost_Stack *s;
    unsigned int i;
    for (s = xpost_stack_at(mem, stackadr); s;
         s = xpost_stack_next_segment(mem, s))
        for (i = 0; i < s->top; i++)
            _verify_push(ctx, s->data[i], 0xFFFFFFFF, 2);
}

void _xpost_garbage_diag_verify(Xpost_Context *ctx, Xpost_Memory_File *mem,
                                int bothbanks)
{
    unsigned int head = 0;
    int bad = 0;

    (void)mem;
    _vq_n = 0;
    _vseen_lo_n = ctx->lo->table.nextent; _vseen_gl_n = ctx->gl->table.nextent;
    _vseen_lo = calloc(_vseen_lo_n, 1); _vseen_gl = calloc(_vseen_gl_n, 1);

    _verify_stack(ctx, ctx->lo, ctx->os);
    _verify_stack(ctx, ctx->lo, ctx->ds);
    _verify_stack(ctx, ctx->lo, ctx->es);
    _verify_stack(ctx, ctx->lo, ctx->hold);
    /* The same roots the collector marks from, expanded from the same
       list. A verifier that walks fewer of them cannot report a gap in
       what it does not reach, and answers "no gap" for a heap the
       collector is mis-marking -- which is worse than not asking. */
#define XPOST_VERIFY_CONTEXT_ROOT(f) \
    _verify_push(ctx, ctx->f, 0xFFFFFFFF, 3);
    XPOST_CONTEXT_OBJECT_ROOTS(XPOST_VERIFY_CONTEXT_ROOT)
#undef XPOST_VERIFY_CONTEXT_ROOT
    _verify_stack(ctx, ctx->gl, xpost_memory_name_stack_ent(ctx->gl));
    _verify_stack(ctx, ctx->lo, xpost_memory_name_stack_ent(ctx->lo));

    while (head < _vq_n)
    {
        _vq_item it = _vq[head++];
        Xpost_Memory_File *m = it.bank ? ctx->gl : ctx->lo;
        unsigned int tag = m->table.tab[it.ent].tag;
        unsigned int used = m->table.tab[it.ent].used;
        unsigned int adr = m->table.tab[it.ent].adr;
        unsigned int off;

        /* reachable and unmarked is the gap, in whichever bank the
           collection covered.
           A file entity counts: the collector marks and sweeps them like
           any other, so one still reachable must be marked too. */
        /* A bank the collection did not mark carries no marks to read,
           so an unmarked entity there says nothing. Only a bank in play
           is reported against. */
        if ((!it.bank || bothbanks) && it.ent >= m->start &&
            (m->table.tab[it.ent].mark & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK) == 0
            && m->table.tab[it.ent].sz != 0)
        {
            fprintf(stderr, "VERIFY GAP: %s ent %u (tag %u used %u) reachable "
                    "via parent ent %u (bank %d)\n",
                    it.bank ? "gl" : "lo",
                    it.ent, tag, used, it.parent, it.pbank);
            bad++;
            if (bad > 8) break;
        }
        if (tag == arraytype)
        {
            for (off = 0; off + sizeof(Xpost_Object) <= used; off += sizeof(Xpost_Object))
            {
                Xpost_Object o;
                memcpy(&o, xpost_vm_ptr(m, adr + off), sizeof o);
                _verify_push(ctx, o, it.ent, it.bank);
            }
        }
        else if (tag == dicttype)
        {
            dichead *dp = xpost_dict_head_at(m, adr);
            dicrec *tp = xpost_dict_table_of(dp);
            /* the table is more than twice as long as the size it is
               derived from, so it is counted in a type wide enough for
               that product rather than in the type the size is stored in */
            unsigned int n = DICTABN(dp->sz);
            unsigned int j;
            for (j = 0; j < n; j++)
            {
                if (xpost_object_get_type(tp[j].key) != nulltype)
                {
                    _verify_push(ctx, tp[j].key, it.ent, it.bank);
                    _verify_push(ctx, tp[j].value, it.ent, it.bank);
                }
            }
        }
    }
    if (bad)
        fprintf(stderr, "VERIFY: %d gaps found (%u reachable ents)\n", bad, _vq_n);
    if (getenv("XPOST_GC_CENSUS"))
    {
        unsigned int i2, cnt_lo = 0, cnt_gl = 0;
        unsigned long bytes_lo = 0, bytes_gl = 0;
        unsigned int bytag[32] = {0};
        for (i2 = 0; i2 < _vq_n; i2++)
        {
            Xpost_Memory_File *m2 = _vq[i2].bank ? ctx->gl : ctx->lo;
            unsigned int t2 = m2->table.tab[_vq[i2].ent].tag;
            if (_vq[i2].bank) { cnt_gl++; bytes_gl += m2->table.tab[_vq[i2].ent].sz; }
            else { cnt_lo++; bytes_lo += m2->table.tab[_vq[i2].ent].sz; }
            if (t2 < 32) bytag[t2]++;
        }
        fprintf(stderr, "CENSUS: lo %u ents %lu bytes | gl %u ents %lu bytes |"
                " arr %u dict %u str %u | lo_nextent %u\n",
                cnt_lo, bytes_lo, cnt_gl, bytes_gl,
                bytag[5], bytag[6], bytag[16], ctx->lo->table.nextent);
    }
    free(_vseen_lo); free(_vseen_gl);
    _vseen_lo = _vseen_gl = NULL;
}


/* report any global array or dict still holding a reference to an
   unmarked (about-to-die) local entity of the file being collected --
   the cross-bank reference a local mark cannot see */
void _xpost_garbage_diag_xbank(Xpost_Context *ctx, Xpost_Memory_File *mem)
{
    Xpost_Memory_File *gl = ctx->gl;
    unsigned int ge;

    for (ge = gl->start; ge < gl->table.nextent; ge++)
    {
        unsigned int gtag = gl->table.tab[ge].tag;
        unsigned int gused = gl->table.tab[ge].used;
        unsigned int gadr = gl->table.tab[ge].adr;
        unsigned int slot = 0;
        unsigned int off;
        unsigned int step;
        unsigned int base;

        if (gtag == arraytype)
        {
            /* an array entity is its elements, one after another */
            base = 0;
            step = sizeof(Xpost_Object);
        }
        else if (gtag == dicttype)
        {
            /* a dictionary entity is a header followed by its records,
               each a hash and the key and value it pairs */
            base = sizeof(dichead);
            step = sizeof(dicrec);
        }
        else
            continue;

        for (off = base; off + step <= gused; off += step, slot++)
        {
            Xpost_Object pair[2];
            unsigned int n;
            unsigned int i;

            if (gtag == arraytype)
            {
                memcpy(&pair[0], xpost_vm_ptr(gl, gadr + off), sizeof pair[0]);
                n = 1;
            }
            else
            {
                dicrec rec;

                memcpy(&rec, xpost_vm_ptr(gl, gadr + off), sizeof rec);
                pair[0] = rec.key;
                pair[1] = rec.value;
                n = 2;
            }

            for (i = 0; i < n; i++)
            {
                Xpost_Object o = pair[i];
                unsigned int te;

                if (!xpost_object_is_composite(o)) continue;
                if (o.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) continue; /* global ref: fine */
                te = xpost_object_get_ent(o);
                if (!xpost_ent_valid(mem, te)) continue;
                if ((mem->table.tab[te].mark & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK) == 0
                    && mem->table.tab[te].sz != 0)
                {
                    fprintf(stderr, "XBANK: gl ent %u (tag %u, %s) slot %u %s -> dying lo ent %u "
                            "(type %u sz %u)",
                            ge, gtag,
                            (gl->table.tab[ge].mark & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK)
                                ? "MARKED" : "unmarked",
                            slot,
                            (gtag == dicttype) ? (i ? "value" : "key") : "element",
                            te, (unsigned int)xpost_object_get_type(o),
                            (unsigned int)o.comp_.sz);
                    if (xpost_object_get_type(o) == stringtype && o.comp_.sz < 200)
                    {
                        unsigned int k;

                        fprintf(stderr, " content=\"");
                        for (k = 0; k < o.comp_.sz; k++)
                        {
                            unsigned char c =
                                ((unsigned char *)xpost_ent_ptr(mem, te))
                                    [o.comp_.off + k];

                            fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
                        }
                        fprintf(stderr, "\"");
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
    }
}
