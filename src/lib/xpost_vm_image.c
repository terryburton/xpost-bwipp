/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_vm_image.c
 * @brief A context's virtual memory as one file, written and read back.
 *
 * Both banks written whole, so that a later run reads the language back
 * instead of building it out of the boot files. What the file does not hold
 * is as much the point as what it does: a host address is written as a zero
 * and rebuilt at the read, and a handle is refused outright.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h> /* EEXIST, for a cache directory already made */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <direct.h> /* _mkdir */
# define _xpost_mkdir(p) _mkdir(p)
#else
# include <sys/types.h>
# include <sys/stat.h> /* mkdir */
# define _xpost_mkdir(p) mkdir((p), 0777)
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_free.h" /* the arena description */
#include "xpost_handle.h" /* the tag marking an entity that carries one */
#include "xpost_compat.h" /* the directory listing */
#include "xpost_context.h"
#include "xpost_interpreter.h" /* where the boot files are */
#include "xpost_name.h"
#include "xpost_operator.h"
#include "xpost_stack.h"
#include "xpost_string.h"
#include "xpost_file.h"
#include "xpost_vm_image.h"
#include "xpost_build_id.h"

/* The names, in the order the enumerations give. Held to that order by
   the count check in each accessor: a name list one shorter than the
   set it names would otherwise report every field under its
   neighbour's name. */
static const char *const _stamp_names[] =
{
    "layout version",
    "byte order",
    "object size",
    "entity number range",
    "pointer size",
    "signature size",
    "operator size",
    "context size",
    "build version",
    "configuration",
    "boot files",
    "banks",
    "context fields",
    "context objects",
    "type names",
    "host state",
    "operators",
    "bank fields"
};

static const char *const _bank_field_names[] =
{
    "high_water",
    "start",
    "nextent",
    "free_substack",
    "free_scan",
    "period",
    "threshold",
    "gc_ent_budget",
    "file_birth_max",
    "gc_auto",
    "gc_pending",
    "ent_reserve_open",
    "ent_exhausted",
    "push_refused"
};

static const char *const _row_field_names[] =
{
    "adr",
    "used",
    "sz",
    "mark",
    "tag"
};

static const char *const _bank_names[] =
{
    "global",
    "local"
};

/* How the last read ended, so a caller that boots a context can say
   which way it was brought up. */
static int _in_use = 0;

/* Whether this process has said it will not read one. */
static int _refused = 0;

/* Which options that change the language are in force. Said before the
   context exists, because the image is read as the context is made and
   there is nothing to ask at that point. Zero is the language the boot
   files build when nothing was asked for. */
static unsigned int _config = 0;

/* The longest operator name an image carries. Longer than any the
   language has; a build that grew one past this writes no image rather
   than writing a name it could not check on the way back. */
#define _NAME_MAX 64

/* How many context objects an image carries, counted off the list that
   declares them. */
#define _ROOT_COUNT_ONE(f) + 1
#define _ROOT_COUNT (0 XPOST_CONTEXT_OBJECT_ROOTS(_ROOT_COUNT_ONE))

#define _CTX_COUNT_ONE(f) + 1
#define _CTX_COUNT (0 XPOST_VM_IMAGE_CTX_FIELDS(_CTX_COUNT_ONE))

#define _TYPENAME_COUNT ((unsigned int)(XPOST_OBJECT_NTYPES + 1))

XPOST_TEST_VISIBLE const char *
xpost_vm_image_stamp_name(unsigned int stamp)
{
    if (stamp >= sizeof _stamp_names / sizeof *_stamp_names)
        return "";
    return _stamp_names[stamp];
}

XPOST_TEST_VISIBLE const char *
xpost_vm_image_bank_field_name(unsigned int field)
{
    if (field >= sizeof _bank_field_names / sizeof *_bank_field_names)
        return "";
    return _bank_field_names[field];
}

XPOST_TEST_VISIBLE const char *
xpost_vm_image_row_field_name(unsigned int field)
{
    if (field >= sizeof _row_field_names / sizeof *_row_field_names)
        return "";
    return _row_field_names[field];
}

XPOST_TEST_VISIBLE const char *
xpost_vm_image_bank_name(unsigned int bank)
{
    if (bank >= sizeof _bank_names / sizeof *_bank_names)
        return "";
    return _bank_names[bank];
}

XPOST_TEST_VISIBLE int
xpost_vm_image_in_use(void)
{
    return _in_use;
}

XPAPI void
xpost_vm_image_config_set(unsigned int mask)
{
    _config = mask;
}

XPAPI void
xpost_vm_image_refuse(void)
{
    _refused = 1;
}

XPOST_TEST_VISIBLE int
xpost_vm_image_refused(void)
{
    return _refused;
}

XPOST_TEST_VISIBLE unsigned int
xpost_vm_image_config(void)
{
    return _config;
}

/*
 *
 * The stamps.
 *
 */

/* One running hash, so that a difference anywhere in what is hashed
   reaches the answer. Nothing here defends against a chosen collision:
   what a stamp is asked is whether two builds are the same build, and
   the answer to that is not being chosen by anyone. */
static unsigned int _hash(unsigned int h, const void *p, size_t n)
{
    const unsigned char *b = p;
    size_t i;

    for (i = 0; i < n; i++)
    {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static unsigned int _hash_text(unsigned int h, const char *s)
{
    return _hash(h, s, strlen(s));
}

/* Every boot file the language is built out of, hashed into one value:
   a build whose data directory has been edited under it must not read
   an image of what those files used to say.

   The files are hashed each on its own -- its name and its bytes -- and
   the answers added together, so the whole does not depend on the order
   a directory listing gives them in. The count goes in as well, so that
   a file removed is a difference even where nothing else moved. */
static unsigned int _data_hash(void)
{
    char pattern[XPOST_PATH_MAX];
    char datadir[XPOST_PATH_MAX];
    glob_t g;
    unsigned int total = 0;
    unsigned int files = 0;
    size_t i;

    xpost_interpreter_data_dir(datadir, sizeof datadir);
    if (!datadir[0])
        return 0;
    if (snprintf(pattern, sizeof pattern, "%s/*.ps", datadir)
        >= (int)sizeof pattern)
        return 0;

    memset(&g, 0, sizeof g);
    if (xpost_glob(pattern, &g) != 0)
    {
        xpost_glob_free(&g);
        return 0;
    }

    for (i = 0; i < g.gl_pathc; i++)
    {
        unsigned char buf[4096];
        unsigned int h = XPOST_VM_IMAGE_DIGEST_SEED;
        const char *path = g.gl_pathv[i];
        const char *base;
        FILE *f;
        size_t got;
        int err = 0;

        base = strrchr(path, '/');
        h = _hash_text(h, base ? base + 1 : path);
        f = xpost_diskfile_fopen(path, "rb", 1, &err);
        if (!f)
            continue;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0)
            h = _hash(h, buf, got);
        fclose(f);
        total += h;
        files++;
    }
    xpost_glob_free(&g);

    return _hash(total, &files, sizeof files);
}

/* What this build would write, so that a reader compares an image
   against the build reading it rather than against a constant. */
static void _stamps(unsigned int *stamp, int host_state)
{
    unsigned int build = XPOST_VM_IMAGE_DIGEST_SEED;
    unsigned int id = XPOST_BUILD_ID;

    build = _hash_text(build, PACKAGE_VERSION);
    /* and what this build was made out of, so one build of a version is
       not read as another: the version moves when a release is cut, the
       sources move whenever anyone edits one. */
    build = _hash(build, &id, sizeof id);

    stamp[XPOST_VM_IMAGE_STAMP_VERSION] = XPOST_VM_IMAGE_VERSION;
    stamp[XPOST_VM_IMAGE_STAMP_ENDIAN] = XPOST_VM_IMAGE_ENDIAN;
    stamp[XPOST_VM_IMAGE_STAMP_OBJECT_SIZE] = (unsigned int)sizeof(Xpost_Object);
    stamp[XPOST_VM_IMAGE_STAMP_ENT_MAX] = XPOST_OBJECT_COMP_MAX_ENT;
    stamp[XPOST_VM_IMAGE_STAMP_POINTER_SIZE] = (unsigned int)sizeof(void *);
    stamp[XPOST_VM_IMAGE_STAMP_SIGNATURE_SIZE] = (unsigned int)sizeof(Xpost_Signature);
    stamp[XPOST_VM_IMAGE_STAMP_OPERATOR_SIZE] = (unsigned int)sizeof(Xpost_Operator);
    stamp[XPOST_VM_IMAGE_STAMP_CONTEXT_SIZE] = (unsigned int)sizeof(Xpost_Context);
    stamp[XPOST_VM_IMAGE_STAMP_BUILD] = build;
    stamp[XPOST_VM_IMAGE_STAMP_CONFIG] = _config;
    stamp[XPOST_VM_IMAGE_STAMP_DATA] = _data_hash();
    stamp[XPOST_VM_IMAGE_STAMP_BANKS] = XPOST_VM_IMAGE_BANKS;
    stamp[XPOST_VM_IMAGE_STAMP_CONTEXT_FIELDS] = (unsigned int)_CTX_COUNT;
    stamp[XPOST_VM_IMAGE_STAMP_ROOTS] = (unsigned int)_ROOT_COUNT;
    stamp[XPOST_VM_IMAGE_STAMP_TYPENAMES] = _TYPENAME_COUNT;
    stamp[XPOST_VM_IMAGE_STAMP_HOST_STATE] = host_state ? 1u : 0u;
    stamp[XPOST_VM_IMAGE_STAMP_OPERATORS] = 0; /* the caller's to fill in */
    /* How many values each bank carries. Every other count an image
       depends on is stamped -- banks, context fields, roots, type
       names, operators -- and this one was not, so a field added to or
       taken out of a bank changed the layout with nothing but the
       version to notice, by hand. */
    stamp[XPOST_VM_IMAGE_STAMP_BANK_FIELDS] = XPOST_VM_IMAGE_BANK_FIELDS;
}

/*
 *
 * The operator table.
 *
 */

/* The name one row of the operator table carries, as text. The row
   records it as an index into the name stack of global memory, which is
   what the row's own image holds; the text is what an image carries, so
   that a reader with another name stack can still say which operator a
   row is. */
static int _row_name(Xpost_Context *ctx, unsigned int index,
                     char *buf, size_t sz)
{
    Xpost_Object nm = { 0 };
    Xpost_Object str;
    char *s;

    nm.mark_.tag = nametype | XPOST_OBJECT_TAG_DATA_FLAG_BANK;
    nm.mark_.pad0 = 0;
    nm.mark_.padw = index;
    str = xpost_name_get_string(ctx, nm);
    if (xpost_object_get_type(str) != stringtype)
        return 0;
    if ((size_t)str.comp_.sz >= sz)
        return 0;
    s = xpost_string_get_pointer(ctx, str);
    if (!s)
        return 0;
    memcpy(buf, s, str.comp_.sz);
    buf[str.comp_.sz] = '\0';
    return 1;
}

/* The host functions one signature of the operator table carries. */
typedef struct
{
    Xpost_Op_Func fp;
    int (*checkstack)(Xpost_Context *ctx);
} _Host_Sig;

/* One row of the operator table as this process holds it: what it is
   called, how many operand shapes it states, and where its signatures
   begin in the run below. */
typedef struct
{
    char name[_NAME_MAX];
    int n;
    unsigned int first;
} _Host_Row;

/* The operator table this process built for itself, read out before an
   image displaces it and put back into the image's table once the two
   have been held to each other row for row. Every row of it comes from
   C, since nothing else has run yet; the rows an image carries beyond
   these are the ones the boot files wrapped around procedures, and they
   carry no host function to rebuild. */
typedef struct
{
    _Host_Row *row;
    _Host_Sig *sig;
    unsigned int rows;
} _Host_Table;

static void _free_table(_Host_Table *t)
{
    free(t->row);
    free(t->sig);
    t->row = NULL;
    t->sig = NULL;
    t->rows = 0;
}

static int _capture_operators(Xpost_Context *ctx, _Host_Table *t)
{
    Xpost_Operator *optab;
    unsigned int n = xpost_operator_count();
    unsigned int total = 0;
    unsigned int k;

    memset(t, 0, sizeof *t);
    if (n == 0)
    {
        XPOST_LOG_ERR("no operator table to rebuild an image's against");
        return 0;
    }
    t->row = calloc(n, sizeof *t->row);
    if (!t->row)
        return 0;

    optab = xpost_operator_table(ctx->gl);
    for (k = 0; k < n; k++)
    {
        if (optab[k].n < 0)
        {
            XPOST_LOG_ERR("operator %u states %d operand shapes", k,
                          optab[k].n);
            _free_table(t);
            return 0;
        }
        total += (unsigned int)optab[k].n;
    }
    t->sig = calloc(total ? total : 1, sizeof *t->sig);
    if (!t->sig)
    {
        _free_table(t);
        return 0;
    }

    total = 0;
    for (k = 0; k < n; k++)
    {
        Xpost_Signature *sig;
        int s;

        optab = xpost_operator_table(ctx->gl);
        if (!_row_name(ctx, optab[k].name, t->row[k].name,
                       sizeof t->row[k].name))
        {
            XPOST_LOG_ERR("cannot read the name of operator %u", k);
            _free_table(t);
            return 0;
        }
        t->row[k].n = optab[k].n;
        t->row[k].first = total;
        sig = (Xpost_Signature *)(void *)((unsigned char *)optab
                                          + optab[k].sigadr);
        for (s = 0; s < t->row[k].n; s++)
        {
            t->sig[total + (unsigned int)s].fp = sig[s].fp;
            t->sig[total + (unsigned int)s].checkstack = sig[s].checkstack;
        }
        total += (unsigned int)t->row[k].n;
    }

    t->rows = n;
    return 1;
}

/*
 *
 * Writing.
 *
 */

/* One value. Every number in an image is written this way, so the
   layout is a run of four-byte quantities with nothing between them:
   writing a structure whole would carry the padding the compiler left
   inside it, which is storage no one assigned and which would differ
   between two images of the same memory. */
/* Where an image is being written, and what has gone into it so far.
   Every byte of an image passes through this, and every byte is hashed
   on the way: the digest at the end of the file is over the whole of
   what came before it, so nothing an image carries is outside what the
   digest answers for. */
typedef struct
{
    FILE *f;
    unsigned int digest;
} _Writer;

static int _emit(_Writer *w, const void *p, size_t n)
{
    if (n == 0)
        return 1;
    w->digest = _hash(w->digest, p, n);
    return fwrite(p, n, 1, w->f) == 1;
}

static int _put(_Writer *w, unsigned int v)
{
    return _emit(w, &v, sizeof v);
}

static int _put_object(_Writer *w, Xpost_Object o)
{
    return _emit(w, &o, sizeof o);
}

/* An operator row: what it states and what it is called. The name is
   padded out to a whole number of values so that everything after it
   keeps the alignment every other part of an image has. */
static int _put_operators(_Writer *w, Xpost_Context *ctx, unsigned int count)
{
    unsigned int k;

    for (k = 0; k < count; k++)
    {
        char name[_NAME_MAX];
        Xpost_Operator *optab = xpost_operator_table(ctx->gl);
        unsigned int len;
        unsigned int pad;

        if (!_row_name(ctx, optab[k].name, name, sizeof name))
        {
            XPOST_LOG_ERR("cannot read the name of operator %u", k);
            return 0;
        }
        len = (unsigned int)strlen(name);
        pad = (4u - (len % 4u)) % 4u;
        if (!_put(w, (unsigned int)optab[k].n)) return 0;
        if (!_put(w, len)) return 0;
        if (len && !_emit(w, name, len)) return 0;
        if (pad)
        {
            static const char zero[4] = { 0, 0, 0, 0 };

            if (!_emit(w, zero, pad)) return 0;
        }
    }
    return 1;
}

/* The context's own share of what the boot settled. */
static int _put_context(_Writer *w, Xpost_Context *ctx)
{
    unsigned int i;

#define _PUT_CTX_FIELD(field) \
    if (!_put(w, (unsigned int)ctx->field)) return 0;
    XPOST_VM_IMAGE_CTX_FIELDS(_PUT_CTX_FIELD)
#undef _PUT_CTX_FIELD

#define _PUT_CTX_ROOT(field) \
    if (!_put_object(w, ctx->field)) return 0;
    XPOST_CONTEXT_OBJECT_ROOTS(_PUT_CTX_ROOT)
#undef _PUT_CTX_ROOT

    for (i = 0; i < _TYPENAME_COUNT; i++)
        if (!_put_object(w, ctx->typenames[i]))
            return 0;
    return 1;
}

/* A bank's bookkeeping, in the order the field enumeration gives. The
   signed members go out as the bytes they occupy: nothing here
   interprets them, and a reader takes them back the way they were
   stored. */
static int _put_bank_fields(_Writer *w, Xpost_Memory_File *mem)
{
    unsigned int field[XPOST_VM_IMAGE_BANK_FIELDS];
    unsigned int i;

    field[XPOST_VM_IMAGE_BANK_HIGH_WATER] = mem->high_water;
    field[XPOST_VM_IMAGE_BANK_START] = mem->start;
    field[XPOST_VM_IMAGE_BANK_NEXTENT] = mem->table.nextent;
    field[XPOST_VM_IMAGE_BANK_FREE_SUBSTACK] = mem->free_substack;
    field[XPOST_VM_IMAGE_BANK_FREE_SCAN] = mem->free_scan;
    field[XPOST_VM_IMAGE_BANK_THRESHOLD] = (unsigned int)mem->threshold;
    field[XPOST_VM_IMAGE_BANK_GC_ENT_BUDGET] = mem->gc_ent_budget;
    field[XPOST_VM_IMAGE_BANK_FILE_BIRTH_MAX] = mem->file_birth_max;
    field[XPOST_VM_IMAGE_BANK_GC_AUTO] = (unsigned int)mem->garbage_collect_auto;
    field[XPOST_VM_IMAGE_BANK_GC_PENDING] = (unsigned int)mem->garbage_collect_pending;
    field[XPOST_VM_IMAGE_BANK_ENT_RESERVE_OPEN] = (unsigned int)mem->ent_reserve_open;
    field[XPOST_VM_IMAGE_BANK_ENT_EXHAUSTED] = (unsigned int)mem->ent_exhausted;
    field[XPOST_VM_IMAGE_BANK_PUSH_REFUSED] = (unsigned int)mem->push_refused;

    for (i = 0; i < XPOST_VM_IMAGE_BANK_FIELDS; i++)
        if (!_put(w, field[i]))
            return 0;
    return 1;
}

/* The arena, with the host addresses in it taken out.

   An operator's signature keeps the C function implementing it and the
   one checking its operands, and both are this process's. Neither can
   be carried, so both are written as zeros and the reader puts back the
   ones its own process holds. A caller that asked for host state gets
   them as they stand, which is for looking at rather than for reading
   back.

   The copy is what makes that possible without disturbing the running
   interpreter: the arena the image is taken of stays exactly as it is. */
static int _put_arena(_Writer *w, Xpost_Context *ctx, Xpost_Memory_File *mem,
                      int is_global, int host_state)
{
    unsigned char *copy;
    unsigned int k;
    unsigned int nops;
    int ok;

    if (mem->high_water == 0)
        return 1;

    /* A build that describes its arena to a memory checker keeps the
       storage the file has not handed out closed, the padding between
       allocations among it, so that a read of any of it is reported
       against whoever read it. Taking an image is the one read of the
       extent whole, so the extent is opened for it -- as the file
       already opens it to grow itself -- and stays open afterwards, at
       the cost of what the description would have caught in the padding
       for the rest of the run. */
    XPOST_VG_REOPEN_RANGE(mem->base, 0, mem->high_water);

    if (host_state || !is_global)
        return _emit(w, mem->base, mem->high_water);

    copy = malloc(mem->high_water);
    if (!copy)
    {
        XPOST_LOG_ERR("cannot hold a copy of the arena to write");
        return 0;
    }
    memcpy(copy, mem->base, mem->high_water);

    nops = xpost_operator_count();
    for (k = 0; k < nops; k++)
    {
        Xpost_Operator *optab = xpost_operator_table(ctx->gl);
        /* the rows name what they point at by its offset within their own
           entity, and this walks a copy of the whole arena, so the two are
           added to reach the same bytes */
        unsigned int adr = xpost_memory_operator_table_adr(ctx->gl)
                           + optab[k].sigadr;
        int n = optab[k].n;
        int s;

        for (s = 0; s < n; s++)
        {
            unsigned int at = adr + (unsigned int)s * (unsigned int)sizeof(Xpost_Signature);

            if (at + sizeof(Xpost_Signature) > mem->high_water)
                break;
            memset(copy + at + offsetof(Xpost_Signature, fp), 0,
                   sizeof ((Xpost_Signature *)0)->fp);
            memset(copy + at + offsetof(Xpost_Signature, checkstack), 0,
                   sizeof ((Xpost_Signature *)0)->checkstack);
        }
    }

    ok = _emit(w, copy, mem->high_water);
    free(copy);
    return ok;
}

/* One bank whole: its name, its bookkeeping, the birth-stamp counters,
   the entity table and the arena.

   The arena goes out from its start to the high-water mark, which is
   more than the live entities: the padding an aligned allocation skips
   and the storage a reclaimed entity left behind are both inside that
   range and both are written. That is deliberate. The range is what an
   image would have to reproduce for an entity's recorded address to
   name the same bytes, and storage no one has written since the arena
   was cleared is exactly where a difference between two runs would
   otherwise go unseen. */
static int _put_bank(_Writer *w, Xpost_Context *ctx, unsigned int bank,
                     Xpost_Memory_File *mem, int host_state)
{
    char name[8];
    unsigned int ent;
    unsigned int i;

    memset(name, 0, sizeof name);
    strncpy(name, xpost_vm_image_bank_name(bank), sizeof name - 1);
    if (!_emit(w, name, sizeof name))
        return 0;

    if (!_put_bank_fields(w, mem))
        return 0;

    for (i = 0; i < XPOST_VM_IMAGE_FILE_BIRTHS; i++)
        if (!_put(w, mem->file_births[i]))
            return 0;

    for (ent = 0; ent < mem->table.nextent; ent++)
    {
        if (!_put(w, mem->table.tab[ent].adr)) return 0;
        if (!_put(w, mem->table.tab[ent].used)) return 0;
        if (!_put(w, mem->table.tab[ent].sz)) return 0;
        if (!_put(w, mem->table.tab[ent].mark)) return 0;
        if (!_put(w, mem->table.tab[ent].tag)) return 0;
        /* The link is written for the rows that describe storage, where
           it chains the released blocks of one size, and written as
           nothing for the rows that describe none, where it chains the
           rows waiting to be issued again. That second chain is derived
           on the way back in, from the rows themselves; writing the link
           for those rows would put a value in the image that a load does
           not restore, so a context read from an image would write back
           an image differing from the one it read. */
        if (!_put(w, (ent >= mem->start && mem->table.tab[ent].sz == 0)
                     ? 0u : mem->table.tab[ent].nextfree)) return 0;
    }

    return _put_arena(w, ctx, mem, bank == 0, host_state);
}

/* Empty the scratch stack, of what it holds and of what it held.

   An operator's operands wait there while the operator runs, and a
   composite under construction is rooted there so that an allocation
   cannot sweep it; nothing is on it between operators, and what is in it
   then is whatever the last one to use it left. That residue is a
   function of the run rather than of the language -- two contexts that
   arrive at the same language by different roads leave different things
   in it -- so an image would carry one road's leavings and a comparison
   of two images would report them.

   Cleared to the bottom and to the bytes, in every segment: the count is
   what a reader of the stack sees, and the bytes are what a reader of
   the image sees. */
static void _clear_scratch(Xpost_Context *ctx)
{
    unsigned int ent = ctx->hold;

    xpost_stack_clear(ctx->lo, ent);
    while (ent)
    {
        Xpost_Stack *seg = xpost_stack_at(ctx->lo, ent);
        unsigned int next = seg->nextseg;

        memset(seg->data, 0, sizeof seg->data);
        ent = next;
    }
}

/* Whether any entity of a bank stands for a block held outside virtual
   memory. Such a block is this process's, reached through a number this
   process issued, and an image carrying one would offer a reader a
   handle on nothing. The entity is marked as carrying one in the
   memory table, which is where this reads it. */
static int _holds_a_handle(Xpost_Memory_File *mem)
{
    unsigned int ent;

    for (ent = 0; ent < mem->table.nextent; ent++)
        if (mem->table.tab[ent].tag & XPOST_MEMORY_TABLE_TAG_HANDLE)
            return 1;
    return 0;
}

XPOST_TEST_VISIBLE int
xpost_vm_image_write(Xpost_Context *ctx, const char *path, int host_state)
{
    unsigned int stamp[XPOST_VM_IMAGE_STAMPS];
    _Writer writer;
    _Writer *w = &writer;
    Xpost_Memory_File *bank[XPOST_VM_IMAGE_BANKS];
    unsigned int i;
    int err = 0;

    if (!ctx || !path)
    {
        XPOST_LOG_ERR("no context or no path to write virtual memory to");
        return 0;
    }
    if (!ctx->gl || !ctx->lo)
    {
        XPOST_LOG_ERR("the context has no virtual memory to write");
        return 0;
    }

    bank[0] = ctx->gl;
    bank[1] = ctx->lo;

    for (i = 0; i < XPOST_VM_IMAGE_BANKS; i++)
        if (_holds_a_handle(bank[i]))
        {
            XPOST_LOG_ERR("the %s bank holds a block outside virtual memory, "
                          "which no other process can be handed",
                          xpost_vm_image_bank_name(i));
            return 0;
        }

    _clear_scratch(ctx);

    _stamps(stamp, host_state);
    stamp[XPOST_VM_IMAGE_STAMP_OPERATORS] = xpost_operator_count();
    if (stamp[XPOST_VM_IMAGE_STAMP_OPERATORS] == 0)
    {
        XPOST_LOG_ERR("the context has no operator table to write");
        return 0;
    }

    /* Through the one opener, like every other disk file the
       interpreter creates. The path is the caller's and not a running
       program's, so it is an interpreter-managed open rather than one
       the sandbox stands between. */
    writer.f = xpost_diskfile_fopen(path, "wb", 1, &err);
    writer.digest = XPOST_VM_IMAGE_DIGEST_SEED;
    if (!writer.f)
    {
        XPOST_LOG_ERR("%d cannot open %s to write virtual memory to",
                      err, path);
        return 0;
    }

    if (!_emit(w, XPOST_VM_IMAGE_MAGIC, XPOST_VM_IMAGE_MAGIC_LEN))
        goto refuse;
    for (i = 0; i < XPOST_VM_IMAGE_STAMPS; i++)
        if (!_put(w, stamp[i]))
            goto refuse;

    if (!_put_operators(w, ctx, stamp[XPOST_VM_IMAGE_STAMP_OPERATORS]))
        goto refuse;
    if (!_put_context(w, ctx))
        goto refuse;

    for (i = 0; i < XPOST_VM_IMAGE_BANKS; i++)
        if (!_put_bank(w, ctx, i, bank[i], host_state))
            goto refuse;

    /* The digest of everything above, and the last thing in the file. It
       is not hashed into itself, and nothing follows it, so a reader
       hashes the file up to its last four bytes and compares. */
    {
        unsigned int digest = writer.digest;

        if (fwrite(&digest, sizeof digest, 1, writer.f) != 1)
            goto refuse;
    }

    /* the image is only written where it reached storage: a short write
       discovered at the close is the same failure as one discovered
       above, and a caller told the write succeeded would go on to
       compare or load a truncated image */
    if (fclose(writer.f) != 0)
    {
        XPOST_LOG_ERR("cannot finish writing virtual memory to %s", path);
        return 0;
    }
    return 1;

  refuse:
    XPOST_LOG_ERR("cannot write virtual memory to %s", path);
    (void)fclose(writer.f);
    return 0;
}

/*
 *
 * Reading.
 *
 */

/* What has been read so far, and how much of the image is left. Every
   read goes through this, so an image that ends early is met at the
   read that runs off it rather than by whatever it left uninitialised. */
typedef struct
{
    unsigned char *at;
    size_t left;
} _Reader;

static int _take(_Reader *r, void *p, size_t n)
{
    if (r->left < n)
        return 0;
    memcpy(p, r->at, n);
    r->at += n;
    r->left -= n;
    return 1;
}

static int _take_u32(_Reader *r, unsigned int *v)
{
    return _take(r, v, sizeof *v);
}

/* Skip forward, for a part of an image whose bytes are read where they
   lie rather than copied out. */
static unsigned char *_take_run(_Reader *r, size_t n)
{
    unsigned char *p = r->at;

    if (r->left < n)
        return NULL;
    r->at += n;
    r->left -= n;
    return p;
}

/* The whole of an image, in memory. An image is read once and checked
   through before any of it reaches the context, so it is held whole
   rather than read in pieces. */
static unsigned char *_slurp(const char *path, size_t *len)
{
    unsigned char *buf;
    FILE *f;
    long n;
    size_t got;
    int err = 0;

    f = xpost_diskfile_fopen(path, "rb", 1, &err);
    if (!f)
        return NULL;
    /* An empty file is refused here rather than carried as a buffer of
       one byte holding nothing: an image answers for itself with a
       digest and names itself with a magic, so a file with room for
       neither is not a short image but no image at all, and the
       allocation below is then exactly the file's length rather than a
       length or a stand-in for one. */
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n)
    {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

/* One bank of an image, located rather than copied: everything but the
   arena and the entity table is small enough to take out, and those two
   are read where they lie. */
typedef struct
{
    unsigned int field[XPOST_VM_IMAGE_BANK_FIELDS];
    unsigned int births[XPOST_VM_IMAGE_FILE_BIRTHS];
    unsigned char *rows;
    unsigned char *arena;
} _Bank;

static int _read_bank(_Reader *r, unsigned int which, _Bank *b)
{
    char name[8];
    unsigned int i;

    if (!_take(r, name, sizeof name))
        return 0;
    if (strncmp(name, xpost_vm_image_bank_name(which), sizeof name) != 0)
    {
        XPOST_LOG_INFO("the image names its bank %u something other than %s",
                      which, xpost_vm_image_bank_name(which));
        return 0;
    }
    for (i = 0; i < XPOST_VM_IMAGE_BANK_FIELDS; i++)
        if (!_take_u32(r, &b->field[i]))
            return 0;
    for (i = 0; i < XPOST_VM_IMAGE_FILE_BIRTHS; i++)
        if (!_take_u32(r, &b->births[i]))
            return 0;

    b->rows = _take_run(r, (size_t)b->field[XPOST_VM_IMAGE_BANK_NEXTENT]
                           * XPOST_VM_IMAGE_ROW_FIELDS * sizeof(unsigned int));
    if (!b->rows)
        return 0;
    b->arena = _take_run(r, b->field[XPOST_VM_IMAGE_BANK_HIGH_WATER]);
    if (!b->arena)
        return 0;
    return 1;
}

/* Whether a bank's entity table describes storage the arena has. A row
   naming bytes past the high-water mark would be followed out of the
   arena by everything that resolves an entity, so an image saying so is
   refused rather than read. */
static int _bank_consistent(const _Bank *b, unsigned int which)
{
    unsigned int used = b->field[XPOST_VM_IMAGE_BANK_HIGH_WATER];
    unsigned int entities = b->field[XPOST_VM_IMAGE_BANK_NEXTENT];
    unsigned int ent;

    if (entities > XPOST_OBJECT_COMP_MAX_ENT)
    {
        XPOST_LOG_INFO("the %s bank of the image holds %u entities, more than "
                      "an object of this build can name",
                      xpost_vm_image_bank_name(which), entities);
        return 0;
    }
    for (ent = 0; ent < entities; ent++)
    {
        unsigned int adr;
        unsigned int sz;

        memcpy(&adr, b->rows + (ent * XPOST_VM_IMAGE_ROW_FIELDS
                                + XPOST_VM_IMAGE_ROW_ADR)
                               * sizeof(unsigned int), sizeof adr);
        memcpy(&sz, b->rows + (ent * XPOST_VM_IMAGE_ROW_FIELDS
                               + XPOST_VM_IMAGE_ROW_SZ)
                              * sizeof(unsigned int), sizeof sz);
        if (adr > used || sz > used - adr)
        {
            XPOST_LOG_INFO("entity %u of the %s bank of the image is %u bytes "
                          "at %u, past the %u the bank holds", ent,
                          xpost_vm_image_bank_name(which), sz, adr, used);
            return 0;
        }
    }
    return 1;
}

/* Room for what an image holds, made before any of it is copied in. The
   arena is grown by the file's own growth, so the base it hands out
   afterwards is the one it will keep using; the entity table is grown
   the way the allocator grows it, one slot short of its capacity. */
static int _make_room(Xpost_Memory_File *mem, const _Bank *b)
{
    unsigned int used = b->field[XPOST_VM_IMAGE_BANK_HIGH_WATER];
    unsigned int entities = b->field[XPOST_VM_IMAGE_BANK_NEXTENT];
    unsigned int max = mem->table.max;

    if (used > mem->high_water && !xpost_memory_file_grow(mem, used - mem->high_water))
    {
        XPOST_LOG_INFO("cannot grow virtual memory to the %u bytes an image "
                      "holds", used);
        return 0;
    }
    while (max <= entities)
        max *= 2;
    if (max != mem->table.max)
    {
        void *tmp = realloc(mem->table.tab, max * sizeof *mem->table.tab);

        if (!tmp)
        {
            XPOST_LOG_INFO("cannot grow the entity table to the %u entities an "
                          "image holds", entities);
            return 0;
        }
        mem->table.tab = tmp;
        mem->table.max = max;
    }
    return 1;
}

/* One bank, taken over by what the image holds. Everything this writes
   is either read out of the image or arrived at from it; what the
   memory file holds about the host -- where it is mapped, what it was
   opened on, the functions installed in it -- is left exactly as this
   process set it. */
static void _install_bank(Xpost_Memory_File *mem, const _Bank *b)
{
    unsigned int entities = b->field[XPOST_VM_IMAGE_BANK_NEXTENT];
    unsigned int used = b->field[XPOST_VM_IMAGE_BANK_HIGH_WATER];
    unsigned int ent;
    unsigned int i;

    XPOST_VG_REOPEN_RANGE(mem->base, 0, mem->max);

    for (ent = 0; ent < entities; ent++)
    {
        unsigned int v[XPOST_VM_IMAGE_ROW_FIELDS];

        memcpy(v, b->rows + (size_t)ent * XPOST_VM_IMAGE_ROW_FIELDS
                            * sizeof(unsigned int), sizeof v);
        mem->table.tab[ent].adr = v[XPOST_VM_IMAGE_ROW_ADR];
        mem->table.tab[ent].used = v[XPOST_VM_IMAGE_ROW_USED];
        mem->table.tab[ent].nextfree = v[XPOST_VM_IMAGE_ROW_NEXTFREE];
        mem->table.tab[ent].sz = v[XPOST_VM_IMAGE_ROW_SZ];
        mem->table.tab[ent].mark = v[XPOST_VM_IMAGE_ROW_MARK];
        mem->table.tab[ent].tag = v[XPOST_VM_IMAGE_ROW_TAG];
    }
    mem->table.nextent = entities;

    if (used)
        memcpy(mem->base, b->arena, used);
    mem->high_water = used;
    mem->start = b->field[XPOST_VM_IMAGE_BANK_START];

    /* The chain of rows that describe nothing is derived rather than
       carried in the image: every row on it has a size of zero, so it is
       read back off the rows themselves. A head written alongside them
       could disagree with them, and the rows are what an allocation acts
       on. Read after the collector's band is known, since the specials
       below it also carry no size and are not rows to hand out. */
    mem->table.freerow = 0;
    for (ent = entities; ent-- > mem->start; )
        if (mem->table.tab[ent].sz == 0)
        {
            mem->table.tab[ent].nextfree = mem->table.freerow;
            mem->table.freerow = ent;
        }
    mem->free_substack = b->field[XPOST_VM_IMAGE_BANK_FREE_SUBSTACK];
    mem->free_scan = b->field[XPOST_VM_IMAGE_BANK_FREE_SCAN];
    mem->threshold = (int)b->field[XPOST_VM_IMAGE_BANK_THRESHOLD];
    mem->gc_ent_budget = b->field[XPOST_VM_IMAGE_BANK_GC_ENT_BUDGET];
    mem->file_birth_max = b->field[XPOST_VM_IMAGE_BANK_FILE_BIRTH_MAX];
    mem->garbage_collect_auto = (int)b->field[XPOST_VM_IMAGE_BANK_GC_AUTO];
    mem->garbage_collect_pending = (int)b->field[XPOST_VM_IMAGE_BANK_GC_PENDING];
    mem->ent_reserve_open = (int)b->field[XPOST_VM_IMAGE_BANK_ENT_RESERVE_OPEN];
    mem->ent_exhausted = (int)b->field[XPOST_VM_IMAGE_BANK_ENT_EXHAUSTED];
    mem->push_refused = (int)b->field[XPOST_VM_IMAGE_BANK_PUSH_REFUSED];

    for (i = 0; i < XPOST_VM_IMAGE_FILE_BIRTHS; i++)
        mem->file_births[i] = b->births[i];
}

/* One row of an image's operator table as the image states it. */
typedef struct
{
    char name[_NAME_MAX];
    unsigned int n;
} _Image_Row;

/* The image's operator rows, and this build's held to them.

   The rows this build installs from C come first and in one order,
   because the same code installs them in the same order every time. So
   the image's first rows must name the same operators, one for one:
   an operator object carries the number of its row and nothing else, and
   a table whose rows had come out differently would raise nothing and
   run the wrong operator. Beyond those, an image's rows are what the
   boot files wrapped around procedures; each is required to carry the
   procedure that makes it one, so a row of the image that ought to have
   a C function behind it cannot pass as one that never had. */
static int _check_operators(const _Image_Row *image, unsigned int nimage,
                            const _Host_Table *t)
{
    unsigned int k;

    if (nimage < t->rows)
    {
        XPOST_LOG_INFO("the image holds %u operators and this build installs "
                      "%u from C", nimage, t->rows);
        return 0;
    }
    for (k = 0; k < t->rows; k++)
    {
        if (strcmp(image[k].name, t->row[k].name) != 0)
        {
            XPOST_LOG_INFO("the image calls operator %u %s and this build "
                          "calls it %s; an operator object carries the number "
                          "of its row, so the two tables are not one table",
                          k, image[k].name, t->row[k].name);
            return 0;
        }
        if (image[k].n != (unsigned int)t->row[k].n)
        {
            XPOST_LOG_INFO("the image has operator %s stating %u operand "
                          "shapes and this build has it stating %d",
                          t->row[k].name, image[k].n, t->row[k].n);
            return 0;
        }
    }
    return 1;
}

/* Where the operator table lies in a bank's arena as the image holds it,
   or nothing. Read out of the image rather than out of the context,
   because everything an image says is checked before any of it is
   installed: a table found wanting after the install is one there is no
   way back from. */
/* `room` takes the bytes the table's entity holds, which is what bounds
   the offsets its rows carry -- the rows name what they point at by its
   place within this entity, not within the arena. */
static const unsigned char *_image_optab(const _Bank *g, unsigned int rows,
                                         unsigned int *room)
{
    Xpost_Memory_File view;
    unsigned int used = g->field[XPOST_VM_IMAGE_BANK_HIGH_WATER];
    unsigned int entities = g->field[XPOST_VM_IMAGE_BANK_NEXTENT];
    unsigned int adr;

    /* A bank holding fewer entities than there are special ones holds no
       operator table, and the accessor below would read a row that is not
       there. */
    if (entities < XPOST_MEMORY_COLLECT_START_GLOBAL)
        return NULL;

    /* The bank as the memory file it is a picture of, so that where the
       table lies is asked the one way it is asked anywhere. The rows of
       an image are the rows of a table -- five addresses and counts in
       the order a row declares them -- and the arena is the arena. What
       the view has not got is everything about the host, which nothing
       here asks it for. */
    memset(&view, 0, sizeof view);
    view.fd = -1;
    view.base = g->arena;
    view.high_water = used;
    view.max = used;
    view.table.tab = (void *)g->rows;
    view.table.nextent = entities;
    view.table.max = entities;

    adr = xpost_memory_operator_table_adr(&view);
    if (adr > used || (size_t)rows * sizeof(Xpost_Operator) > used - adr)
        return NULL;
    if (room)
    {
        *room = xpost_memory_operator_table_size(&view);
        if (*room > used - adr)
            return NULL;   /* the entity claims more than the bank holds */
    }
    return g->arena + adr;
}

/* What each row of the image's operator table says about itself, held to
   what a rebuild will need of it: a row this build installs from C must
   state its signatures somewhere the image holds, and a row past those
   must carry the procedure that makes it one the boot files wrapped. A
   row of the second kind that ought to have been of the first would
   otherwise run with no function at all. */
static int _check_operator_rows(const _Bank *g, const _Host_Table *t,
                                unsigned int nimage)
{
    unsigned int room = 0;
    const unsigned char *rows = _image_optab(g, nimage, &room);
    unsigned int k;

    if (!rows)
    {
        XPOST_LOG_INFO("the image does not hold the %u operators it says it "
                       "does", nimage);
        return 0;
    }
    for (k = 0; k < nimage; k++)
    {
        Xpost_Operator op;

        memcpy(&op, rows + (size_t)k * sizeof op, sizeof op);
        if (k >= t->rows)
        {
            if (xpost_object_get_type(op.proc) != arraytype)
            {
                XPOST_LOG_INFO("operator %u of the image is past the ones this "
                               "build installs from C and carries no procedure "
                               "to run", k);
                return 0;
            }
            continue;
        }
        if (op.n != t->row[k].n)
        {
            XPOST_LOG_INFO("operator %s of the image states %d operand shapes "
                           "in its table and %d in its names",
                           t->row[k].name, op.n, t->row[k].n);
            return 0;
        }
        if (op.n == 0)
            continue;
        /* the offset is within the operator table's own entity, so what
           bounds it is that entity rather than the whole arena -- a run
           reaching past the entity is one the image does not hold, even
           where the arena happens to go on beyond it */
        if (op.sigadr == 0 || op.sigadr > room ||
            (size_t)op.n * sizeof(Xpost_Signature) > room - op.sigadr)
        {
            XPOST_LOG_INFO("operator %s of the image states %d operand shapes "
                           "at %u, which its operator table does not hold",
                           t->row[k].name, op.n, op.sigadr);
            return 0;
        }
    }
    return 1;
}

/* Put back the host functions the image was written without. The rows
   have already been held to this build's own, by name and by what each
   states, so the row a function goes into is the row it came out of and
   nothing here can refuse. */
static void _rebuild_operators(Xpost_Context *ctx, const _Host_Table *t)
{
    unsigned int k;

    for (k = 0; k < t->rows; k++)
    {
        Xpost_Operator *optab = xpost_operator_table(ctx->gl);
        Xpost_Signature *sig;
        int s;

        if (optab[k].n == 0)
            continue;
        sig = (Xpost_Signature *)(void *)((unsigned char *)optab
                                          + optab[k].sigadr);
        for (s = 0; s < optab[k].n; s++)
        {
            sig[s].fp = t->sig[t->row[k].first + (unsigned int)s].fp;
            sig[s].checkstack =
                t->sig[t->row[k].first + (unsigned int)s].checkstack;
        }
    }
}

XPOST_TEST_VISIBLE int
xpost_vm_image_load(Xpost_Context *ctx, const char *path)
{
    unsigned int mine[XPOST_VM_IMAGE_STAMPS];
    unsigned int stamp[XPOST_VM_IMAGE_STAMPS];
    unsigned char magic[XPOST_VM_IMAGE_MAGIC_LEN];
    unsigned char *bytes = NULL;
    _Host_Table host;
    _Image_Row *image = NULL;
    _Bank bank[XPOST_VM_IMAGE_BANKS];
    unsigned int ctxfield[_CTX_COUNT];
    Xpost_Object root[_ROOT_COUNT];
    Xpost_Object typename[_TYPENAME_COUNT];
    _Reader r;
    size_t len = 0;
    unsigned int i;
    int taken = 0;

    _in_use = 0;
    if (!ctx || !path || !ctx->gl || !ctx->lo)
        return 0;

    if (!_capture_operators(ctx, &host))
        return 0;

    bytes = _slurp(path, &len);
    if (!bytes)
    {
        XPOST_LOG_INFO("no image of virtual memory at %s", path);
        goto done;
    }
    r.at = bytes;
    r.left = len;

    /* What the file says of itself, before anything in it is read as
       anything. Every byte an image carries went into the digest at its
       end, so a file that does not answer for itself is put down here
       and nothing below is asked to make sense of it. */
    if (len < XPOST_VM_IMAGE_MAGIC_LEN + sizeof(unsigned int))
    {
        XPOST_LOG_INFO("%s is too short to be an image of virtual memory",
                       path);
        goto done;
    }
    {
        unsigned int said;
        unsigned int found;

        memcpy(&said, bytes + len - sizeof said, sizeof said);
        found = _hash(XPOST_VM_IMAGE_DIGEST_SEED, bytes,
                      len - sizeof said);
        if (said != found)
        {
            XPOST_LOG_INFO("%s answers for itself with %08x and its bytes "
                           "come to %08x; the image is not the one that was "
                           "written", path, said, found);
            goto done;
        }
        r.left = len - sizeof said;
    }

    if (!_take(&r, magic, sizeof magic) ||
        memcmp(magic, XPOST_VM_IMAGE_MAGIC, XPOST_VM_IMAGE_MAGIC_LEN) != 0)
    {
        XPOST_LOG_INFO("%s does not begin as an image of virtual memory", path);
        goto done;
    }
    for (i = 0; i < XPOST_VM_IMAGE_STAMPS; i++)
        if (!_take_u32(&r, &stamp[i]))
        {
            XPOST_LOG_INFO("%s ends in the middle of what it says about the "
                          "build that wrote it", path);
            goto done;
        }

    _stamps(mine, 0);
    mine[XPOST_VM_IMAGE_STAMP_OPERATORS] = stamp[XPOST_VM_IMAGE_STAMP_OPERATORS];
    for (i = 0; i < XPOST_VM_IMAGE_STAMPS; i++)
        if (stamp[i] != mine[i])
        {
            XPOST_LOG_INFO("%s was written with %s %u and this build reads %u; "
                          "the image is not this build's to read", path,
                          xpost_vm_image_stamp_name(i), stamp[i], mine[i]);
            goto done;
        }

    image = calloc(stamp[XPOST_VM_IMAGE_STAMP_OPERATORS] ?
                   stamp[XPOST_VM_IMAGE_STAMP_OPERATORS] : 1, sizeof *image);
    if (!image)
        goto done;
    for (i = 0; i < stamp[XPOST_VM_IMAGE_STAMP_OPERATORS]; i++)
    {
        unsigned int namelen;
        const unsigned char *name;

        if (!_take_u32(&r, &image[i].n) || !_take_u32(&r, &namelen))
            goto short_image;
        if (namelen >= _NAME_MAX)
        {
            XPOST_LOG_INFO("operator %u of %s is named in %u bytes", i, path,
                          namelen);
            goto done;
        }
        name = _take_run(&r, namelen + (4u - (namelen % 4u)) % 4u);
        if (!name)
            goto short_image;
        memcpy(image[i].name, name, namelen);
        image[i].name[namelen] = '\0';
    }

    if (!_check_operators(image, stamp[XPOST_VM_IMAGE_STAMP_OPERATORS], &host))
        goto done;

    for (i = 0; i < (unsigned int)_CTX_COUNT; i++)
        if (!_take_u32(&r, &ctxfield[i]))
            goto short_image;
    for (i = 0; i < (unsigned int)_ROOT_COUNT; i++)
        if (!_take(&r, &root[i], sizeof root[i]))
            goto short_image;
    for (i = 0; i < _TYPENAME_COUNT; i++)
        if (!_take(&r, &typename[i], sizeof typename[i]))
            goto short_image;

    for (i = 0; i < XPOST_VM_IMAGE_BANKS; i++)
        if (!_read_bank(&r, i, &bank[i]))
            goto short_image;
    if (r.left != 0)
    {
        XPOST_LOG_INFO("%s carries %lu bytes past what it describes", path,
                      (unsigned long)r.left);
        goto done;
    }
    for (i = 0; i < XPOST_VM_IMAGE_BANKS; i++)
        if (!_bank_consistent(&bank[i], i))
            goto done;

    if (!_check_operator_rows(&bank[0], &host,
                              stamp[XPOST_VM_IMAGE_STAMP_OPERATORS]))
        goto done;

    /* Everything the image says has now been read and checked, and
       nothing of the context has been touched. Room comes next, so that
       the one step that can still fail fails with the context holding
       what it always held; after it, nothing can refuse. */
    if (!_make_room(ctx->gl, &bank[0]) || !_make_room(ctx->lo, &bank[1]))
        goto done;

    _install_bank(ctx->gl, &bank[0]);
    _install_bank(ctx->lo, &bank[1]);
    xpost_operator_set_count(stamp[XPOST_VM_IMAGE_STAMP_OPERATORS]);
    _rebuild_operators(ctx, &host);

    i = 0;
#define _GET_CTX_FIELD(field) ctx->field = ctxfield[i++];
    XPOST_VM_IMAGE_CTX_FIELDS(_GET_CTX_FIELD)
#undef _GET_CTX_FIELD
    i = 0;
#define _GET_CTX_ROOT(field) ctx->field = root[i++];
    XPOST_CONTEXT_OBJECT_ROOTS(_GET_CTX_ROOT)
#undef _GET_CTX_ROOT
    for (i = 0; i < _TYPENAME_COUNT; i++)
        ctx->typenames[i] = typename[i];

    /* The cache of what a name resolves to against the dictionary stack
       answers for the dictionaries this context had a moment ago, and
       every one of them has just been replaced. It is thrown away rather
       than invalidated: the generation that would invalidate it came out
       of the image, and an entry left behind under that same generation
       would read as current. */
    free(ctx->namecache_gen);
    free(ctx->namecache_val);
    ctx->namecache_gen = NULL;
    ctx->namecache_val = NULL;
    ctx->namecache_size = 0;

    taken = 1;
    goto done;

  short_image:
    XPOST_LOG_INFO("%s ends before what it describes does", path);

  done:
    free(image);
    _free_table(&host);
    free(bytes);
    _in_use = taken;
    return taken;
}

/* ---- where an image lives when nothing names one -------------------
 *
 * An image is worth having only if a run gets one without being asked
 * to arrange it, so the two places one is looked for are settled here
 * rather than left to whoever starts the interpreter.
 *
 * The first is beside the boot files. An image there is part of an
 * installation: built once by whoever assembled it, out of exactly the
 * files sitting next to it, and read by every run on the machine. It is
 * never written by a run -- the directory belongs to the installation
 * and a run may not have it, nor should want it.
 *
 * The second is a cache belonging to the user, which is where a run
 * writes. That is a cache in the ordinary sense: losing it costs the
 * time the image would have saved and nothing else, so it goes where
 * the platform puts such things and is dropped as freely.
 *
 * ONE FILE PER LANGUAGE, and the name says which. Two builds whose
 * images could not be read by each other must not be offered the same
 * file: the object width decides what the bytes mean, and the
 * configuration decides which language was built. Both are in the name.
 * What is deliberately NOT in the name is this build's identity. It
 * would stop two builds of the same width and configuration from
 * taking turns at one file -- but the stamps already refuse an image
 * from another build, so taking turns costs a rebuild, which is what a
 * run with no image pays anyway. Keying by build instead would leave a
 * file behind for every build ever run, growing a directory nobody
 * looks at, to save a cost that is only ever paid once per switch.
 */

/* Make one directory, saying nothing of a directory that is already
   there: two runs starting together both find it missing and both
   make it, and the one that lost has what it wanted. */
static int _make_dir(const char *path)
{
    return (_xpost_mkdir(path) == 0 || errno == EEXIST);
}

/* The directory a user's caches go in, by the convention of the
   platform. Answers 0 where the environment does not say -- a run with
   no home has nowhere of its own to write, which is not a failure of
   anything and leaves it booting the long way. */
static int _cache_dir(char *buf, size_t len)
{
    const char *base;
    int n;

#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (!base || !base[0])
        return 0;
    n = snprintf(buf, len, "%s\\xpost", base);
#else
    base = getenv("XDG_CACHE_HOME");
    if (base && base[0])
    {
        n = snprintf(buf, len, "%s/xpost", base);
    }
    else
    {
        base = getenv("HOME");
        if (!base || !base[0])
            return 0;
        /* The parent is made too: a home that has never held a cache
           has no .cache for this to go in, and one missing directory
           is not a reason to boot the long way for ever after. */
        n = snprintf(buf, len, "%s/.cache", base);
        if (n < 0 || (size_t)n >= len)
            return 0;
        if (!_make_dir(buf))
            return 0;
        n = snprintf(buf, len, "%s/.cache/xpost", base);
    }
#endif
    if (n < 0 || (size_t)n >= len)
        return 0;
    return 1;
}

/* The name an image of this language goes under. */
static int _image_name(char *buf, size_t len)
{
    int n = snprintf(buf, len, "xpost-%u-%02x.vmimg",
                     (unsigned int)sizeof(Xpost_Object),
                     xpost_vm_image_config());

    return !(n < 0 || (size_t)n >= len);
}

int xpost_vm_image_default_path(char *buf, size_t len,
                                const char *datadir, int for_write)
{
    char dir[XPOST_PATH_MAX];
    char name[64];
    int n;

    if (!buf || len == 0 || !_image_name(name, sizeof(name)))
        return 0;

    /* Beside the boot files, for reading only, and only where one is
       actually there: the fall through to the cache is what a machine
       with no installed image wants, and asking the file system is the
       only way to tell the two apart. */
    if (!for_write && datadir && datadir[0])
    {
        FILE *f;
        int err;

        n = snprintf(buf, len, "%s/%s", datadir, name);
        /* Through the one opener every disk file the interpreter reads
           goes through, so that this asks the question the same way the
           read of the image will answer it. */
        if (n > 0 && (size_t)n < len
            && (f = xpost_diskfile_fopen(buf, "rb", 1, &err)))
        {
            fclose(f);
            return 1;
        }
    }

    if (!_cache_dir(dir, sizeof(dir)))
        return 0;
    /* Made on the way to reading as well as to writing. A first run
       reads nothing and writes, and the write wants the directory. */
    if (!_make_dir(dir))
        return 0;

    n = snprintf(buf, len, "%s/%s", dir, name);
    return !(n < 0 || (size_t)n >= len);
}
