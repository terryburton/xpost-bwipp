/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h> /* malloc */
#include <stddef.h>

#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"  /* dicts live in the memory file, accessed via memory table */
#include "xpost_object.h"  /* dict is an object, containing objects */
#include "xpost_stack.h"  /* may need to count the save stack */
#include "xpost_free.h"  /* dicts are allocated from the free list */

#include "xpost_save.h"  /* dicts obey save/restore */
#include "xpost_context.h"
#include "xpost_error.h"  /* dict functions may throw errors */
#include "xpost_string.h"  /* may need string functions (convert to name) */
#include "xpost_name.h"  /* may need name functions (create name) */
#include "xpost_file.h"
#include "xpost_dict.h"  /* double-check prototypes */



/*
typedef struct {
    word tag;
    word sz;
    word nused;
    word pad;
} dichead;

typedef struct
{
    unsigned int hash;
    object key;
    objecy value;
} dicrec;
*/

/* strict-aliasing compatible poking of double */
typedef union
{
    unsigned long long bits;
    double             number;
} Xpost_Ieee_Double_As_Int;

Xpost_Object_Tag_Access xpost_dict_get_access(Xpost_Context *ctx, Xpost_Object d)
{
    Xpost_Memory_File *mem;
    dichead *dp;
    mem = xpost_context_select_memory(ctx, d);
    dp = xpost_dict_head(mem, xpost_object_get_ent(d));
    return (Xpost_Object_Tag_Access)((dp->tag & XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK) >>
                                     XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
}

Xpost_Object xpost_dict_set_access(Xpost_Context *ctx, Xpost_Object d, Xpost_Object_Tag_Access access)
{
    Xpost_Memory_File *mem;
    dichead *dp;
    mem = xpost_context_select_memory(ctx, d);
    /* A dictionary's access attribute is a property of its value rather
       than of the object naming it (PLRM 3.3.2), and it is kept where
       the value is: in the dictionary's head, inside the entity a save
       level copies and a restore puts back. Writing it is therefore a
       write to that entity, and every write to an entity takes the
       backup first -- the copy a level holds stands for the entity as it
       was when the level was taken, which it is only if nothing reaches
       the entity ahead of it. A copy taken after this write would hold
       the access set here, and the restore that ends the level would
       hand back an access the dictionary never had at the save.

       Refused, the access is left alone and the refusal is answered
       with a null, so that the change is not made unrevertable: the
       caller decides what a dictionary it could not reduce means to it. */
    if (xpost_save_cow(mem, dicttype, 0, xpost_object_get_ent(d)))
        return null;
    /* the backup allocates, so the head is found after it */
    dp = xpost_dict_head(mem, xpost_object_get_ent(d));
    dp->tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    dp->tag |= access << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET;
    d.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    d.tag |= access << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET;
    return d;
}

/* Compare two objects for "equality".
   return 0 if "equal"
          +value if L > R
          -value if L < R
 */
int xpost_dict_compare_objects(Xpost_Context *ctx,
           Xpost_Object L,
           Xpost_Object R)
{
    int cmp;

    /* a pair of the same simple type orders without reading vm, and
       does so in the one place the fused relational operators share
       (see xpost_dict.h) */
    if (xpost_dict_compare_simple(L, R, &cmp))
        return cmp;

    /* fold nearly-comparable types to comparable */
    if (xpost_object_get_type(L) != xpost_object_get_type(R))
    {
        if (xpost_object_get_type(L) == integertype && xpost_object_get_type(R) == realtype)
        {
            L = xpost_real_cons((real)L.int_.val);
            goto cont;
        }
        if (xpost_object_get_type(R) == integertype && xpost_object_get_type(L) == realtype)
        {
            R = xpost_real_cons((real)R.int_.val);
            goto cont;
        }
        if (xpost_object_get_type(L) == nametype && xpost_object_get_type(R) == stringtype)
        {
            L = xpost_name_get_string(ctx, L);
            goto cont;
        }
        if (xpost_object_get_type(R) == nametype && xpost_object_get_type(L) == stringtype)
        {
            R = xpost_name_get_string(ctx, R);
            goto cont;
        }
        return xpost_object_get_type(L) - xpost_object_get_type(R);
    }

cont:
    switch (xpost_object_get_type(L))
    {
        default:
            XPOST_LOG_ERR("unhandled type (%s) in xpost_dict_compare_objects",
                    xpost_object_type_names[xpost_object_get_type(L)]);
            return -1;

        case marktype: return 0;
        case nulltype: return 0;
        case invalidtype: return 0;

        /* booleans, integers and names are settled above; the folds
           reach here as a real or a string */

        /* numbers compare exactly: this function also backs
           the relational operators */
        case realtype: return L.real_.val < R.real_.val ? -1 :
                              L.real_.val > R.real_.val ? 1 : 0;
        case extendedtype:
        {
            double l,r;
            l = xpost_dict_convert_extended_to_double(L);
            r = xpost_dict_convert_extended_to_double(R);
            return l < r ? -1 : l > r ? 1 : 0;
        }

        case operatortype:  return L.mark_.padw < R.mark_.padw ? -1 :
                                   L.mark_.padw > R.mark_.padw ? 1 : 0;

        /* two context identifiers name the same context when they carry
           the same id, so they compare by it -- the same field the hash
           reads, so a context used as a dictionary key hashes and
           compares consistently. PLRM 2nd ed 7.1 gives a context an
           identifier that means the same in every context; making the
           handle equatable is what lets a program tell one from another. */
        case contexttype:   return L.mark_.padw < R.mark_.padw ? -1 :
                                   L.mark_.padw > R.mark_.padw ? 1 : 0;

        case dicttype: /*@fallthrough@*/ /*return !( xpost_object_get_ent(L) == xpost_object_get_ent(R) ); */
        case arraytype: return !( L.comp_.sz == R.comp_.sz
                                && (L.tag&XPOST_OBJECT_TAG_DATA_FLAG_BANK) == (R.tag&XPOST_OBJECT_TAG_DATA_FLAG_BANK)
                                && xpost_object_get_ent(L) == xpost_object_get_ent(R)
                                && L.comp_.off == R.comp_.off ); /* 0 if all eq */

        case stringtype:
        {
            /* strings compare lexicographically, element by element;
               where one is a prefix of the other the shorter is less
               (PLRM, gt/ge/lt/le). Comparing by length alone is wrong:
               (abc) is less than (d), not greater. */
            unsigned int ln = L.comp_.sz, rn = R.comp_.sz;
            unsigned int n = ln < rn ? ln : rn;
            int c = memcmp(xpost_string_get_pointer(ctx, L),
                           xpost_string_get_pointer(ctx, R), n);
            return c != 0 ? c : (int)ln - (int)rn;
        }
        /* equal underlying streams compare equal (0), like every case above */
        case filetype: return xpost_file_get_file_pointer(ctx->lo, L) != xpost_file_get_file_pointer(ctx->lo, R);
    }
}

/* fold a payload as wide as this build's dword into the unsigned int
   the hash is mixed in, so that no part of it is dropped where a dword
   is the wider of the two */
static inline
unsigned int fold_payload(dword v)
{
    unsigned long long w = (unsigned long long)v;
    return (unsigned int)w ^ (unsigned int)(w >> 32);
}

/* more like scrambled eggs */
static
unsigned int hash(Xpost_Object k)
{
    unsigned int h;
    unsigned int t = (unsigned int)(xpost_object_get_type(k)
                | (k.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)); /* ignore
                     the other flags: a key equal to another under
                     xpost_dict_compare_objects must hash with it */

    /* Each key is hashed over the fields its own type occupies. Reading
       the composite fields of every key would read, for a key that is
       not a composite, whatever those fields happen to overlay -- which
       in one build is part of the payload and in another is the padding
       above it, so a hash written for one layout collapses in the
       other. */
    if (xpost_object_is_composite(k))
        h = (t << 1)
            + (k.comp_.sz << 3)
            + ((unsigned int)xpost_object_get_ent(k) << 7)
            + (k.comp_.off << 5);
    else if (xpost_object_get_type(k) == extendedtype)
        /* A number key holds the sign, exponent and fraction of the
           double it was made from. The fraction is added where it lies:
           consecutive integers differ only in its upper bits, which a
           shift toward the top of the mix would push out of it. */
        h = (t << 1)
            + ((unsigned int)k.extended_.sign_exp << 3)
            + fold_payload(k.extended_.fraction);
    else
        /* every other key is identified by the single unsigned payload
           beside its tag: a name's index, a file's entity, an
           operator's opcode, a boolean's value */
        h = (t << 1)
            + (fold_payload(k.mark_.padw) << 3);

    /* mix bits so the modulo by the table size spreads keys across
       all slots for any size */
    h *= 2654435761u; /* Knuth multiplicative hash (golden ratio) */
    h ^= h >> 16;
#ifdef DEBUGDIC
    printf("\nhash(");
    xpost_object_dump(k);
    printf(")=%u", h);
#endif
    return h;
}

/*
   Allocate a dictionary in the specified memory file.

   allocate an entity with xpost_memory_table_alloc,
   set the save level in the mark,
   extract the "pointer" from the entity,
   Initialize a dichead in memory,
   just after the head, clear a table of pairs. */
Xpost_Object xpost_dict_cons_memory (Xpost_Memory_File *mem,
               unsigned int sz)
{
    Xpost_Object d = { 0 };
    dichead *dp;
    dicrec *tp;
    unsigned int i;
    unsigned int ent;
    unsigned int hashnull;

    dword reqsz = sz; /* capacity asked for, as maxlength reports it */
    dword tabsz;      /* internal size, over-allocated from the above */

    if (sz < 8) sz = 8;
    tabsz = (dword)ceil((double)sz * 1.25);

    /* both sizes are held in header fields a word wide, and the
       over-allocation above carries a capacity that fits one to a size
       that does not. Hold the dictionary to what the fields describe: a
       size stored wider than its field reads back as a far smaller
       dictionary sitting in a far larger allocation, whose entries then
       rehash themselves into a table smaller than the one they came
       from. */
    if (tabsz > XPOST_OBJECT_COMP_MAX_SZ)
        tabsz = XPOST_OBJECT_COMP_MAX_SZ;
    if (reqsz > XPOST_OBJECT_COMP_MAX_SZ)
        reqsz = XPOST_OBJECT_COMP_MAX_SZ;
    sz = (unsigned int)tabsz;

    assert(mem->base);
    d.tag = dicttype | (XPOST_OBJECT_TAG_ACCESS_UNLIMITED << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    d.comp_.sz = sz;
    d.comp_.off = 0;
    /* The table byte-size is passed to an allocator whose size parameter is
       32 bits wide. On the wide build the size field above admits a capacity
       whose table would exceed that: DICTABSZ(sz) then wraps, a huge
       dictionary is backed by a small allocation, and the table-clearing loop
       below writes past it. Size the table in 64 bits here, at the one point
       it is sized, and refuse what the allocator cannot represent -- a
       dictionary that will not fit is a VMerror, not a buffer overrun. */
    if ((unsigned long long)sizeof(dichead)
        + (2ULL * sz + 1) * sizeof(dicrec) > 0xFFFFFFFFULL)
    {
        XPOST_LOG_ERR("dictionary table size overflows the allocator");
        return null;
    }
    if (!xpost_memory_table_alloc(mem, sizeof(dichead) + DICTABSZ(sz), dicttype, &ent))
    {
        XPOST_LOG_ERR("cannot allocate dictionary");
        return null;
    }
    d = xpost_object_set_ent(d, ent);

    xpost_save_stamp_birth(mem, ent);

    dp = xpost_ent_ptr(mem, ent); /* clear header */
    dp->tag = d.tag;
    dp->sz = sz;
    dp->nused = 0;
    dp->pad = (word)reqsz; /* the capacity maxlength reports */

    tp = xpost_dict_table_of(dp); /* clear table */
    hashnull = hash(null);
    for (i=0; i < DICTABN(sz); i++){
        tp[i].hash = hashnull;
        tp[i].key = null; /* remember our null object is not all-zero! */
        tp[i].value = null;
    }
#ifdef DEBUGDIC
    printf("xpost_dict_cons_memory : "); xpost_dict_dump_memory (mem, d);
#endif
    return d;
}

/*
   Allocate a dictionary in the currently active memory.

   select the memory file according to vmmode,
   call xpost_dict_cons_memory ,
   set the BANK flag. */
Xpost_Object xpost_dict_cons (Xpost_Context *ctx,
               unsigned int sz)
{
    Xpost_Object d = xpost_dict_cons_memory (ctx->vmmode==GLOBAL? ctx->gl: ctx->lo, sz);
    if (xpost_object_get_type(d) != nulltype)
    {
        if (ctx->vmmode == GLOBAL)
            d.tag |= XPOST_OBJECT_TAG_DATA_FLAG_BANK;
        xpost_stack_push(ctx->lo, ctx->hold, d); /* stash a reference on the hold stack in case of gc in caller */
    }
    return d;
}

/* get the nused field from the dichead */
unsigned int xpost_dict_length_memory (Xpost_Memory_File *mem,
                   Xpost_Object d)
{
    dichead *dp;
    dp = xpost_dict_head(mem, xpost_object_get_ent(d));
    return dp->nused;
}

/* get the sz field from the dichead: the internal hash-table size,
   used to decide when the dict is full and how large to grow it */
unsigned int xpost_dict_max_length_memory (Xpost_Memory_File *mem,
                      Xpost_Object d)
{
    dichead *dp;
    dp = xpost_dict_head(mem, xpost_object_get_ent(d));
    return dp->sz;
}

/* the capacity maxlength reports: the size the dict was created with
   while it holds no more than that, and the size of its table once it
   holds more. The internal size above is over-allocated (min 8, x1.25),
   so reporting it from the start would over-report a dict that has room
   it was never asked for */
unsigned int xpost_dict_capacity_memory (Xpost_Memory_File *mem,
                      Xpost_Object d)
{
    dichead *dp;
    dp = xpost_dict_head(mem, xpost_object_get_ent(d));
    return dp->pad;
}

/*
   grow a dictionary to a larger size. Returns 0 or the error to raise.

   allocate a new dictionary,
   copy over all non-null key/value pairs,
   swap adrs in the two table slots. */
static
int dicgrow(Xpost_Context *ctx,
             Xpost_Object d)
{
    Xpost_Memory_File *mem;
    unsigned int sz;
    unsigned int newsz;
    dichead *dp;
    dicrec *tp;
    Xpost_Object n;
    unsigned int i;
    unsigned int dent;
    int ret;

    xpost_stack_push(ctx->lo, ctx->hold, d);
    mem = xpost_context_select_memory(ctx, d);
#ifdef DEBUGDIC
    printf("DI growing dict\n");
    xpost_dict_dump_memory (mem, d);
#endif
    n = xpost_dict_cons_memory (mem, newsz = 2 * xpost_dict_max_length_memory (mem, d));
    if (xpost_object_get_type(n) == nulltype){
        XPOST_LOG_ERR("cannot grow dict");
        return VMerror;
    }
    /* a dictionary already at the widest size its header field carries
       is answered by a dictionary of that same size: the entries would
       rehash into a table no larger than the one they came from, and the
       last of them would ask to grow again. That size is the largest
       dictionary this build has. */
    if (xpost_dict_max_length_memory(mem, n)
            <= xpost_dict_max_length_memory(mem, d))
    {
        XPOST_LOG_ERR("cannot grow dict past the size the header carries");
        return limitcheck;
    }
    if (mem == ctx->gl)
        n.tag |= XPOST_OBJECT_TAG_DATA_FLAG_BANK;

    dent = xpost_object_get_ent(d);
    dp = xpost_ent_ptr(mem, dent);
    sz = DICTABN(dp->sz);
    for (i = 0; i < sz; i++)
    {
        /* xpost_dict_put_memory below allocates, which can relocate the
           memory file. xpost_ent_ptr reads the file's base as it stands
           at the call, so the header is re-derived per iteration and the
           record table with it. */
        dp = xpost_ent_ptr(mem, dent);
        tp = xpost_dict_table_of(dp);
        if (xpost_object_get_type(tp[i].key) != nulltype)
        {
            /* an entry that does not reach the larger dictionary is an
               entry the growth would drop */
            ret = xpost_dict_put_memory(ctx, mem, n, tp[i].key, tp[i].value);
            if (ret)
            {
                XPOST_LOG_ERR("cannot rehash a dict entry into the larger dict");
                return ret;
            }
        }
    }
#ifdef DEBUGDIC
    printf("n: ");
    xpost_dict_dump_memory (mem, n);
#endif

    {   /* exchange entities */
        unsigned int nent;

        nent = xpost_object_get_ent(n);

        xpost_ent_swap(mem, dent, nent);

    }
    return 0;
}

/* is it full? (y/n) */
int xpost_dict_is_full_memory (Xpost_Memory_File *mem,
             Xpost_Object d)
{
    return xpost_dict_length_memory (mem, d) == xpost_dict_max_length_memory (mem, d);
}

/* print a dump of the dictionary data */
void xpost_dict_dump_memory (Xpost_Memory_File *mem,
             Xpost_Object d)
{
    dichead *dp;
    dicrec *tp;
    unsigned int sz;
    unsigned int i;

    dp = xpost_dict_head(mem, xpost_object_get_ent(d));
    tp = xpost_dict_table_of(dp);
    sz = DICTABN(dp->sz);

    printf("\n");
    for (i = 0; i < sz; i++)
    {
        printf("%u:", i);
        if (xpost_object_get_type(tp[i].key) != nulltype)
        {
            xpost_object_dump(tp[i].key);
        }
    }
}

/* construct an extendedtype object
   from a double value */
/*n.b. Caller Must set EXTENDEDINT or EXTENDEDREAL flag */
/*     in order to xpost_dict_convert_extended_to_number() later. */
static
Xpost_Object consextended (double d)
{
    Xpost_Ieee_Double_As_Int r;
    Xpost_Object o = { 0 };

    r.number = d;
    o.extended_.tag = extendedtype;
    o.extended_.sign_exp = (r.bits >> 52) & 0xFFF;
    o.extended_.fraction = (r.bits >> 20) & 0xFFFFFFFF;
    return o;
}

/* adapter:
   double <- extendedtype object */
double xpost_dict_convert_extended_to_double (Xpost_Object e)
{
    Xpost_Ieee_Double_As_Int r;
    r.bits = ((unsigned long long)e.extended_.sign_exp << 52)
             | ((unsigned long long)e.extended_.fraction << 20);
    return r.number;
}

/* convert an extendedtype object to integertype or realtype
   depending upon flag */
Xpost_Object xpost_dict_convert_extended_to_number (Xpost_Object e)
{
    Xpost_Object o = { 0 };
    double d = xpost_dict_convert_extended_to_double(e);

    if (e.tag & XPOST_OBJECT_TAG_DATA_EXTENDED_INT)
    {
        o = xpost_int_cons((integer)d);
    }
    else if (e.tag & XPOST_OBJECT_TAG_DATA_EXTENDED_REAL)
    {
        o = xpost_real_cons((real)d);
    }
    else
    {
        XPOST_LOG_ERR("invalid extended number object");
        return null;
    }
    return o;
}

/* make key the proper type for hashing.

   lookup_only asks for the key of a read that stores nothing: a string is
   resolved to the name it already interns to, or to the invalid object if
   it is not yet a name -- which cannot be a key of any dictionary, since
   every key was interned when it was stored. A read that interned its
   string key would leave a permanent name behind for a key that matched
   nothing, so an untrusted program probing with unbounded distinct strings
   would grow the name table without end. A store (lookup_only false) still
   interns, because its key is about to live in the dictionary. */
static
Xpost_Object clean_key (Xpost_Context *ctx,
                        Xpost_Object k,
                        int lookup_only)
{
    switch(xpost_object_get_type(k))
    {
        default: break;
        case stringtype:
        {
            char *s = xpost_string_allocate_cstring(ctx, k);
            k = lookup_only
                ? xpost_name_find_n(ctx, s, (unsigned int)strlen(s))
                : xpost_name_cons(ctx, s);
            free(s);
            break;
        }
        case integertype:
            k = consextended(k.int_.val);
            k.tag |= XPOST_OBJECT_TAG_DATA_EXTENDED_INT;
            break;
        case realtype:
            k = consextended(k.real_.val);
            k.tag |= XPOST_OBJECT_TAG_DATA_EXTENDED_REAL;
            break;
    }
    return k;
}

/* keys are overwhelmingly names: equal iff same bank and name index.
   decided inline to spare a function call per probe */
static inline int
_keys_equal(Xpost_Context *ctx, Xpost_Object a, Xpost_Object b)
{
    if (xpost_object_get_type(a) == nametype &&
        xpost_object_get_type(b) == nametype)
        return ((a.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK) ==
                (b.tag & XPOST_OBJECT_TAG_DATA_FLAG_BANK)) &&
               a.mark_.padw == b.mark_.padw;
    return xpost_dict_compare_objects(ctx, a, b) == 0;
}

/* repeated loop body from the lookup function */
#define RETURN_TAB_I_IF_EQ_K_OR_NULL    \
    if (xpost_object_get_type(tp[i].key) == nulltype \
        || (hashval == tp[i].hash \
            && _keys_equal(ctx, tp[i].key, k))) \
        return tp + i

static dicrec invalidrec[] = {{ 0, {{0}}, {{0}}}};

/* perform a hash-assisted lookup.
   returns a pointer to the desired pair (if found)), or a null-pair. */
/*@dependent@*/ /*@null@*/
static
dicrec *diclookup(Xpost_Context *ctx,
        /*@dependent@*/ Xpost_Memory_File *mem,
        Xpost_Object d,
        Xpost_Object k,
        int lookup_only)
{
    dichead *dp;
    dicrec *tp;
    unsigned int sz;
    unsigned int hashval;
    unsigned int h;
    unsigned int i;

    k = clean_key(ctx, k, lookup_only);
    if (xpost_object_get_type(k) == invalidtype)
        /* a read whose string key is not an interned name matches nothing;
           report the miss as an empty slot would, without a name to blame */
        return lookup_only ? NULL : invalidrec;

    dp = (dichead *)xpost_ent_ptr_checked(mem, xpost_object_get_ent(d));
    if (!dp)
        return invalidrec;
    tp = xpost_dict_table_of(dp);
    sz = DICTABN(dp->sz);

    hashval = hash(k);
    h = hashval % sz;
    i = h;
#ifdef DEBUGDIC
    printf("diclookup(");
    xpost_object_dump(k);
    printf(");");
    printf("%%%u=%u",sz, h);
#endif

    RETURN_TAB_I_IF_EQ_K_OR_NULL;
    for (++i; i < sz; i++)
    {
        RETURN_TAB_I_IF_EQ_K_OR_NULL;
    }
    for (i = 0; i < h; i++)
    {
        RETURN_TAB_I_IF_EQ_K_OR_NULL;
    }
    return NULL; /* i == h : dict is overfull: no null entry */
}

/* see if lookup returns a non-null pair. */
int xpost_dict_known_key(Xpost_Context *ctx,
                         /*@dependent@*/ Xpost_Memory_File *mem,
                         Xpost_Object d,
                         Xpost_Object k)
{
    dicrec *r;

    r = diclookup(ctx, mem, d, k, 1);
    if (r == NULL) return 0;
    if (r == invalidrec) return 0;
    return xpost_object_get_type(r->key) != nulltype;
}

/*
   Get value from dict+key with specified memory file
   (dict must be valid for this memory file)

   call diclookup,
   return the value if the key is non-null
   or invalid if key is null (interpret as "undefined"). */
Xpost_Object xpost_dict_get_memory (Xpost_Context *ctx,
        /*@dependent@*/ Xpost_Memory_File *mem,
        Xpost_Object d,
        Xpost_Object k)
{
    dicrec *r;

    r = diclookup(ctx, mem, d, k, 1);
    if (r == invalidrec){
        XPOST_LOG_ERR("warning: invalid key\n");
        return invalid;
    }
    if (r == NULL || xpost_object_get_type(r->key) == nulltype)
    {
        return invalid;
    }
    return r->value;
}

/*
   Get value from dict+key.

   select the memory file according to BANK field,
   call xpost_dict_get_memory . */
Xpost_Object xpost_dict_get(Xpost_Context *ctx,
        Xpost_Object d,
        Xpost_Object k)
{
    return xpost_dict_get_memory (ctx, xpost_context_select_memory(ctx, d), d, k);
}

/*
   Get value from dict with a name key.

   names are already canonical dict keys, so the key normalisation and
   generality of the full lookup are unnecessary. */
Xpost_Object xpost_dict_get_name(Xpost_Context *ctx,
        Xpost_Object d,
        Xpost_Object k)
{
    Xpost_Memory_File *mem = xpost_context_select_memory(ctx, d);
    unsigned int ent = xpost_object_get_ent(d);
    dichead *dp;
    dicrec *tp;
    unsigned int sz;
    unsigned int hashval;
    unsigned int h;
    unsigned int i;

    dp = xpost_ent_ptr_checked(mem, ent);
    if (!dp)
        return invalid;
    tp = xpost_dict_table_of(dp);
    sz = DICTABN(dp->sz);

    hashval = hash(k);
    h = hashval % sz;

    for (i = h; i < sz; i++)
    {
        if (xpost_object_get_type(tp[i].key) == nulltype)
            return invalid;
        if (hashval == tp[i].hash && _keys_equal(ctx, tp[i].key, k))
            return tp[i].value;
    }
    for (i = 0; i < h; i++)
    {
        if (xpost_object_get_type(tp[i].key) == nulltype)
            return invalid;
        if (hashval == tp[i].hash && _keys_equal(ctx, tp[i].key, k))
            return tp[i].value;
    }
    return invalid;
}

/* Bring the capacity up to what the dictionary has.

   The table is over-allocated from the capacity the dictionary was
   asked for, so it takes entries past that capacity before it grows. A
   dictionary holding more than it was asked for has the capacity of its
   table, which is at least its length (PLRM 8.2 maxlength) and, since
   the table only ever grows, is never less than the capacity reported
   before it. */
static
void note_capacity(dichead *dp)
{
    if (dp->nused > dp->pad)
        dp->pad = dp->sz;
}

/*
   Put key+value in dict with specified memory file.
   (dict must be valid for this memory file)

   save data if not save at this level,
   lookup the key,
   if key is null, check if the dict is full,
       increase nused,
       set key,
       update value. */
int xpost_dict_put_memory(Xpost_Context *ctx,
        Xpost_Memory_File *mem,
        Xpost_Object d,
        Xpost_Object k,
        Xpost_Object v)
{
    dicrec *r;
    dichead *dp;
    int ret;

    /* a key may be any object except null (PLRM 3.3.5): null is what an
       empty slot holds, so a null key names nothing */
    if (xpost_object_get_type(k) == nulltype)
        return typecheck;

    if (!ctx->gl->interpreter_get_initializing())
        if (!xpost_object_is_writeable(ctx, d))
            return invalidaccess;

    /* canonicalise the key first: converting a new string key to a name
       allocates, which can collect or move the memory file; every
       pointer derived before that point would be stale */
    k = clean_key(ctx, k, 0);
    if (xpost_object_get_type(k) == invalidtype)
        return VMerror;

    ret = xpost_save_cow(mem, dicttype, 0, xpost_object_get_ent(d));
    if (ret)
        return ret;

    r = diclookup(ctx, mem, d, k, 0);

    if (r == invalidrec){
        XPOST_LOG_ERR("warning: invalid key\n");
        return VMerror;
    }
    if (r == NULL)
    {
        /* dict overfull:  grow dict! */
        ret = dicgrow(ctx, d);
        if (ret)
            return ret;

        r = diclookup(ctx, mem, d, k, 0);
        if (r == NULL)
            return VMerror;
    }
    else if (xpost_object_get_type(r->key) == invalidtype)
    {
        XPOST_LOG_ERR("warning: invalidtype key in dict\n");
        r->key = null;
    }
    else if (xpost_object_get_type(r->key) == nulltype)
    {
        if (xpost_dict_is_full_memory (mem, d))
        {
            /* dict full:  grow dict! */
            ret = dicgrow(ctx, d);
            if (ret)
                return ret;

            r = diclookup(ctx, mem, d, k, 0);

            if (r == NULL)
                return VMerror;
        }

        dp = xpost_dict_head(mem, xpost_object_get_ent(d));
        ++ dp->nused;
        note_capacity(dp);
        r->key = k; /* canonicalised above */
        r->hash = hash(k);
    }
    r->value = v;
    return 0;
}

/*
   Put key+value in dict.

   select the memory file according to BANK field,
   call xpost_dict_put_memory. */
int xpost_dict_put(Xpost_Context *ctx,
        Xpost_Object d,
        Xpost_Object k,
        Xpost_Object v)
{
    Xpost_Memory_File *mem = xpost_context_select_memory(ctx, d);
    if (!ctx->ignoreinvalidaccess)
    {
        if ( mem == ctx->gl
                && xpost_object_is_composite(k)
                && mem != xpost_context_select_memory(ctx, k))
        {
            XPOST_LOG_ERR("local key into global dict");
            return invalidaccess;
        }
        if ( mem == ctx->gl
                && xpost_object_is_composite(v)
                && mem != xpost_context_select_memory(ctx, v))
        {
            xpost_object_dump(v);
            XPOST_LOG_ERR("local value into global dict");
            return invalidaccess;
        }
    }
    xpost_stack_push(ctx->lo, ctx->hold, d);
    xpost_stack_push(ctx->lo, ctx->hold, k);
    xpost_stack_push(ctx->lo, ctx->hold, v);

    ++ctx->namebind_gen; /* a binding may change: invalidate name cache */

    return xpost_dict_put_memory(ctx, xpost_context_select_memory(ctx, d), d, k, v);
}

/* undefine key from dict.

   after emptying the slot, re-slot the entries that follow it in the
   probe cluster so no entry is left unreachable beyond the new hole
   (Knuth TAOCP vol.3, 6.4 Algorithm R). */
int xpost_dict_undef_memory(Xpost_Context *ctx,
        Xpost_Memory_File *mem,
        Xpost_Object d,
        Xpost_Object k)
{
    dicrec *e;
    dichead *dp;
    dicrec *tp;
    unsigned int sz;
    unsigned int hashnull;
    unsigned int i;
    unsigned int j;
    int ret;

    ++ctx->namebind_gen;

    ret = xpost_save_cow(mem, dicttype, 0, xpost_object_get_ent(d));
    if (ret)
        return ret;

    k = clean_key(ctx, k, 1); /* may allocate: derive pointers after */
    if (xpost_object_get_type(k) == invalidtype)
        /* a string key that is not an interned name is in no dictionary,
           so there is nothing to undefine -- the same answer as a key that
           is a name but absent, below */
        return undefined;

    dp = xpost_dict_head(mem, xpost_object_get_ent(d));
    tp = xpost_dict_table_of(dp);

    e = diclookup(ctx, mem, d, k, 1); /*find slot for key */
    if (e == NULL || e == invalidrec || xpost_object_get_type(e->key) == nulltype)
    {
        return undefined;
    }

    sz = DICTABN(dp->sz);
    hashnull = hash(null);

    j = e - tp; /* empty the slot */
    tp[j].key = null;
    tp[j].hash = hashnull;
    tp[j].value = null;
    --dp->nused;

    i = j;
    while (1) /* re-slot the remainder of the cluster */
    {
        unsigned int home;

        i = (i + 1) % sz;
        if (xpost_object_get_type(tp[i].key) == nulltype)
            break;
        /* move entry i into the hole at j unless its home slot lies
           cyclically within (j, i], in which case it is still reachable */
        home = tp[i].hash % sz;
        if (i > j ? (home <= j || home > i) : (home <= j && home > i))
        {
            tp[j] = tp[i];
            tp[i].key = null;
            tp[i].hash = hashnull;
            tp[i].value = null;
            j = i;
        }
    }

    return 0;
}

/* undefine key from banked dict */
int xpost_dict_undef(Xpost_Context *ctx,
        Xpost_Object d,
        Xpost_Object k)
{
    return xpost_dict_undef_memory(ctx, xpost_context_select_memory(ctx, d), d, k);
}


