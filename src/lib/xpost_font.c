/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_font.c
 * @brief Faces, glyphs, and the cache that keeps a rendered glyph.
 *
 * Where the face library is reached. A build without one is a font system
 * with no faces rather than one with a hole in it: every operator still
 * exists and refuses with invalidfont where a face would be needed.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h> /* snprintf */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h> /* sqrt */

#ifdef HAVE_FREETYPE2

# ifdef HAVE_FONTCONFIG
#  include <fontconfig/fontconfig.h>
# endif

# include <ft2build.h>
# include FT_FREETYPE_H
# include FT_OUTLINE_H
# include FT_BBOX_H
# include FT_FONT_FORMATS_H

#endif

#include "xpost_log.h"
#include "xpost_object.h"
#include "xpost_font.h"
#include "xpost_main.h" /* to be called when the library goes down */

#ifdef HAVE_FREETYPE2

# ifdef HAVE_FONTCONFIG
static FcConfig *_xpost_font_fc_config = NULL;
# endif

static FT_Library _xpost_font_ft_library = NULL;

#endif

/* The glyph cache: rendered glyph rasters keyed by their full
   rendering inputs -- the face (or, for glyphs painted by procedure,
   the font), the glyph selector, and the exact fixed-point transform
   and base size in force -- so a hit replays the very bytes a fresh
   rasterization would produce. Entries link into a recency list and
   hash by key; the store answers cachestatus and honours the byte
   ceiling of setcachelimit, glyphs above it rendering uncached. */

#define GCACHE_BUCKETS 512

/* The largest font cache this implementation offers, and so the largest
   value either byte parameter takes: a cache size above it, or a
   per-glyph ceiling above it, is substituted by it with no error
   indication (PLRM 8.2, setcacheparams). The per-glyph ceiling needs a
   ceiling of its own because it is the only size gate an entry passes
   on its way into the store below -- what an entry costs is a rendered
   raster, and a ceiling of a whole word admits any of them. */
#define GCACHE_BYTES_MAX 67108864L

typedef struct Xpost_Glyph_Entry
{
    const void *k1;            /* face, or the procedure font's key */
    unsigned long long k2;     /* glyph index or character selector */
    long m[4];                 /* the 16.16 transform in force */
    long size;                 /* the base size the face serves */
    unsigned char *bits;
    int rows, width, pitch;
    char pixel_mode;
    int left, top;
    long advance_x, advance_y;
    size_t bytes;
    struct Xpost_Glyph_Entry *hnext;
    struct Xpost_Glyph_Entry *lprev, *lnext; /* recency list */
} Xpost_Glyph_Entry;

static Xpost_Glyph_Entry *gcache_hash[GCACHE_BUCKETS];
static Xpost_Glyph_Entry *gcache_head = NULL, *gcache_tail = NULL;
static long gcache_bsize = 0;     /* bytes held */
static long gcache_csize = 0;     /* glyphs held */
static long gcache_bmax = 1048576;
static long gcache_cmax = 4096;
static long gcache_mmax = 4096;
static long gcache_blimit = XPOST_FONT_ITEM_LIMIT_DEFAULT;

/* the transform and size in force per live face, recorded as they
   are installed: together with the glyph index they key the cache */
#define GCACHE_FACES 64
static struct { const void *face; long m[4]; long size; }
    gcache_state[GCACHE_FACES];
static int gcache_nstate = 0;
static int gcache_rr = 0;         /* next slot displaced when all are live */

static const Xpost_Glyph_Entry *gcache_serving = NULL;

/* --- the rendered-glyph cache ----------------------------------------
   Rendering a glyph costs the face, the transform and the hinting, and a
   page of text asks for the same few glyphs repeatedly. So a rendered
   glyph is kept, keyed by face, glyph, size and the exact fixed-point
   transform -- exact, because a glyph rendered at a hair's difference is a
   different glyph. */

static unsigned int
gcache_hashkey(const void *k1, unsigned long long k2, const long m[4], long size)
{
    unsigned long long h = (unsigned long long)(size_t)k1;

    h = h * 31 + k2;
    h = h * 31 + (unsigned long long)m[0];
    h = h * 31 + (unsigned long long)m[1];
    h = h * 31 + (unsigned long long)m[2];
    h = h * 31 + (unsigned long long)m[3];
    h = h * 31 + (unsigned long long)size;
    return (unsigned int)(h % GCACHE_BUCKETS);
}

#ifdef HAVE_FREETYPE2
/* recorded as the face route installs a transform and size, and read
   back when it renders: a procedural glyph's key carries its transform
   from the caller, so only faces pass through here */
static void
gcache_state_set(const void *face, const long *m, const long *size)
{
    int i, slot = -1;

    for (i = 0; i < gcache_nstate; i++)
        if (gcache_state[i].face == face)
            { slot = i; break; }
    if (slot < 0)
    {
        /* a slot a freed face gave back is taken before the table
           grows, so the table's population is the live faces and not
           every face there has ever been */
        for (i = 0; i < gcache_nstate; i++)
            if (!gcache_state[i].face)
                { slot = i; break; }
    }
    if (slot < 0)
    {
        if (gcache_nstate < GCACHE_FACES)
            slot = gcache_nstate++;
        else
        {
            /* every slot holds a live face: displace them in rotation,
               so no one face's state is the standing casualty. The
               displaced face keeps its cached rasters -- they are keyed
               by its address -- but declines the cache until its state
               is recorded again. */
            slot = gcache_rr;
            gcache_rr = (gcache_rr + 1) % GCACHE_FACES;
        }
    }
    if (gcache_state[slot].face != face)
    {
        gcache_state[slot].face = face;
        gcache_state[slot].m[0] = 65536; gcache_state[slot].m[1] = 0;
        gcache_state[slot].m[2] = 0;     gcache_state[slot].m[3] = 65536;
        gcache_state[slot].size = 0;
    }
    if (m)
        memcpy(gcache_state[slot].m, m, 4 * sizeof(long));
    if (size)
        gcache_state[slot].size = *size;
}

static int
gcache_state_get(const void *face, long m[4], long *size)
{
    int i;

    for (i = 0; i < gcache_nstate; i++)
        if (gcache_state[i].face == face)
        {
            memcpy(m, gcache_state[i].m, 4 * sizeof(long));
            *size = gcache_state[i].size;
            return 1;
        }
    m[0] = 65536; m[1] = 0; m[2] = 0; m[3] = 65536;
    *size = 0;
    return 0;
}
#endif

static Xpost_Glyph_Entry *
gcache_find(const void *k1, unsigned long long k2, const long m[4], long size)
{
    Xpost_Glyph_Entry *e;

    for (e = gcache_hash[gcache_hashkey(k1, k2, m, size)]; e; e = e->hnext)
        if (e->k1 == k1 && e->k2 == k2 && e->size == size
         && e->m[0] == m[0] && e->m[1] == m[1]
         && e->m[2] == m[2] && e->m[3] == m[3])
            return e;
    return NULL;
}

static void
gcache_bump(Xpost_Glyph_Entry *e)
{
    if (gcache_head == e)
        return;
    if (e->lprev) e->lprev->lnext = e->lnext;
    if (e->lnext) e->lnext->lprev = e->lprev;
    if (gcache_tail == e) gcache_tail = e->lprev;
    e->lprev = NULL;
    e->lnext = gcache_head;
    if (gcache_head) gcache_head->lprev = e;
    gcache_head = e;
    if (!gcache_tail) gcache_tail = e;
}

static void
gcache_drop(Xpost_Glyph_Entry *e)
{
    Xpost_Glyph_Entry **pp =
        &gcache_hash[gcache_hashkey(e->k1, e->k2, e->m, e->size)];

    while (*pp && *pp != e)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = e->hnext;
    if (e->lprev) e->lprev->lnext = e->lnext;
    if (e->lnext) e->lnext->lprev = e->lprev;
    if (gcache_head == e) gcache_head = e->lnext;
    if (gcache_tail == e) gcache_tail = e->lprev;
    gcache_bsize -= (long)e->bytes;
    gcache_csize--;
    if (gcache_serving == e)
        gcache_serving = NULL;
    free(e->bits);
    free(e);
}

static Xpost_Glyph_Entry *
gcache_insert(const void *k1, unsigned long long k2, const long m[4], long size,
              const unsigned char *bits, int rows, int width, int pitch,
              char pixel_mode, int left, int top,
              long advance_x, long advance_y)
{
    size_t bytes = (size_t)(pitch < 0 ? -pitch : pitch) * (size_t)rows;
    Xpost_Glyph_Entry *e;
    unsigned int b;

    if (bytes == 0 || (long)bytes > gcache_blimit)
        return NULL;
    e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->bits = malloc(bytes);
    if (!e->bits)
    {
        free(e);
        return NULL;
    }
    memcpy(e->bits, bits, bytes);
    e->k1 = k1; e->k2 = k2;
    memcpy(e->m, m, 4 * sizeof(long));
    e->size = size;
    e->rows = rows; e->width = width; e->pitch = pitch;
    e->pixel_mode = pixel_mode;
    e->left = left; e->top = top;
    e->advance_x = advance_x; e->advance_y = advance_y;
    e->bytes = bytes;
    b = gcache_hashkey(k1, k2, m, size);
    e->hnext = gcache_hash[b];
    gcache_hash[b] = e;
    e->lnext = gcache_head;
    if (gcache_head) gcache_head->lprev = e;
    gcache_head = e;
    if (!gcache_tail) gcache_tail = e;
    gcache_bsize += (long)bytes;
    gcache_csize++;
    while ((gcache_bsize > gcache_bmax || gcache_csize > gcache_cmax)
           && gcache_tail && gcache_tail != e)
        gcache_drop(gcache_tail);
    return e;
}

/* order two cache entries by the face and transform that make a
   combination -- the key, the base size, the transform -- so that entries
   of one combination sort together and the distinct ones can be counted in
   a single pass. The glyph a combination was cached for (k2) is not part
   of it. */
static int _combo_cmp(const void *pa, const void *pb)
{
    const Xpost_Glyph_Entry *a = *(const Xpost_Glyph_Entry * const *)pa;
    const Xpost_Glyph_Entry *b = *(const Xpost_Glyph_Entry * const *)pb;
    int i;

    if ((size_t)a->k1 != (size_t)b->k1)
        return (size_t)a->k1 < (size_t)b->k1 ? -1 : 1;
    if (a->size != b->size)
        return a->size < b->size ? -1 : 1;
    for (i = 0; i < 4; i++)
        if (a->m[i] != b->m[i])
            return a->m[i] < b->m[i] ? -1 : 1;
    return 0;
}

/* Every figure currentcachestatus and the cache parameters report,
   gathered in one pass. */
void
xpost_font_cache_status(long *bsize, long *bmax, long *msize, long *mmax,
                        long *csize, long *cmax, long *blimit)
{
    const Xpost_Glyph_Entry *e, *f;
    long combos = 0;

    /* Distinct face-and-transform combinations held. Sorting the entries by
       combination and counting the runs finds them in n log n, where
       comparing every entry against all before it was n squared -- the cost
       a program could raise by asking for the cache's status with the cache
       full. The order is by pointer identity and integer fields, so the
       count is exact and does not depend on it; if the scratch array cannot
       be had, the older pairwise scan gives the same answer. */
    if (gcache_csize > 0)
    {
        const Xpost_Glyph_Entry **arr =
            malloc((size_t)gcache_csize * sizeof(*arr));
        if (arr)
        {
            long n = 0, i;

            for (e = gcache_head; e && n < gcache_csize; e = e->lnext)
                arr[n++] = e;
            qsort(arr, (size_t)n, sizeof(*arr), _combo_cmp);
            combos = n ? 1 : 0;
            for (i = 1; i < n; i++)
                if (_combo_cmp(&arr[i - 1], &arr[i]) != 0)
                    combos++;
            free(arr);
        }
        else
        {
            for (e = gcache_head; e; e = e->lnext)
            {
                for (f = gcache_head; f != e; f = f->lnext)
                    if (f->k1 == e->k1 && f->size == e->size
                     && f->m[0] == e->m[0] && f->m[1] == e->m[1]
                     && f->m[2] == e->m[2] && f->m[3] == e->m[3])
                        break;
                if (f == e)
                    combos++;
            }
        }
    }
    *bsize = gcache_bsize; *bmax = gcache_bmax;
    *msize = combos;       *mmax = gcache_mmax;
    *csize = gcache_csize; *cmax = gcache_cmax;
    *blimit = gcache_blimit;
}

/* The per-glyph ceiling a program states, held to the range the store
   offers: a value above it is that value, and one below it is no
   ceiling at all, which is a store this implementation can be -- the
   entry gate below admits nothing and every glyph renders from its
   description. Neither is an error (PLRM 8.2, setcachelimit raises
   stackunderflow and typecheck and nothing besides). */
static long
gcache_limit_in_range(long upper)
{
    if (upper < 0)
        return 0;
    return upper > GCACHE_BYTES_MAX ? GCACHE_BYTES_MAX : upper;
}

/* The ceiling on what the glyph cache may hold, clamped to what the
   counters can carry, and answered so the caller records the ceiling in
   force rather than the one it asked for. */
long
xpost_font_cache_setlimit(long blimit)
{
    gcache_blimit = gcache_limit_in_range(blimit);
    return gcache_blimit;
}

/* A cache size of zero is the request stating none: the operator's
   operands are delimited by a mark rather than counted, and the
   interpreter fills the absent ones in (data/font.ps), so what stands
   for a size the request did not carry is a size no request can state.
   PLRM 8.2 leaves the cache size unchanged where the request omits it.
   The per-glyph ceiling is always carried and is always taken. */
long
xpost_font_cache_setparams(long bmax, long lower, long upper)
{
    (void)lower;   /* the compression threshold: rasters stay flat */
    if (bmax > 0)
        gcache_bmax = bmax > GCACHE_BYTES_MAX ? GCACHE_BYTES_MAX : bmax;
    gcache_blimit = gcache_limit_in_range(upper);
    while ((gcache_bsize > gcache_bmax || gcache_csize > gcache_cmax)
           && gcache_tail)
        gcache_drop(gcache_tail);
    return gcache_blimit;
}

/* --- the stencil-run cache -------------------------------------------
   A second cache over the first, for driver-generated bitmap text: the
   same small masks paint over and over, and scanning their bits again per
   occurrence is where such jobs spend their time. Keyed by the content, so
   a collision answers no differently than a miss. */

int
xpost_mask_cache_lookup(const void *k1, unsigned long long k2,
                             const long m[4], long size,
                             unsigned char **bits, int *rows, int *width,
                             int *pitch, int *left, int *top,
                             long *advance_x, long *advance_y)
{
    Xpost_Glyph_Entry *e = gcache_find(k1, k2, m, size);

    if (!e)
        return 0;
    gcache_bump(e);
    *bits = e->bits;
    *rows = e->rows; *width = e->width; *pitch = e->pitch;
    *left = e->left; *top = e->top;
    *advance_x = e->advance_x; *advance_y = e->advance_y;
    return 1;
}

/* Puts a mask a driver generated into the same store the rendered
   glyphs use, as a grey bitmap. Answers whether it was taken, a mask
   too large for the cache being no error. */
int
xpost_mask_cache_insert(const void *k1, unsigned long long k2,
                             const long m[4], long size,
                             const unsigned char *bits, int rows, int width,
                             int pitch, int left, int top,
                             long advance_x, long advance_y)
{
    return gcache_insert(k1, k2, m, size, bits, rows, width, pitch,
                         8 /* gray */, left, top, advance_x, advance_y)
           != NULL;
}

static void
gcache_clear(void)
{
    while (gcache_tail)
        gcache_drop(gcache_tail);
    /* the transforms recorded per face go with the entries they key: a
       face is an address, and an address given back can be handed out
       again for a different face */
    memset(gcache_state, 0, sizeof(gcache_state));
    gcache_nstate = 0;
    gcache_rr = 0;
    gcache_serving = NULL;
}

void
xpost_mask_cache_clear(void)
{
    gcache_clear();
}

/* Drop every procedure/Type-3 glyph mask -- the entries keyed by k1==NULL,
   whose k2 carries a glyph selector built from the serial the font
   dictionary holds under .fontid.

   That serial is the program's, not the interpreter's. definefont stamps
   one only where there is none, and nothing stops a program writing its
   own there or taking one away (data/font.ps, .glyphkey). So two jobs can
   present the same serial over different glyph descriptions, and a mask
   left cached under it would answer the second job the first job's glyph.
   Flushing them at the job boundary is what stops a program carrying a
   glyph from one job into the next.

   FreeType-face glyphs (k1 == the face pointer, which the face's own
   release already drops) are left cached: nothing a program writes
   decides which face a pointer names. */
void
xpost_font_mask_cache_flush(void)
{
    Xpost_Glyph_Entry *e = gcache_head;

    while (e)
    {
        Xpost_Glyph_Entry *next = e->lnext;

        if (e->k1 == NULL)
            gcache_drop(e);
        e = next;
    }
}

#ifdef HAVE_FREETYPE2
/* A face built over a font program in memory reads the program where it
   lies, keeping the pointer it was given rather than a copy of the bytes,
   so the program has to outlive every glyph built from it. It is the
   face's to hold for exactly that long: this is where the two are kept
   together, so that releasing a face releases its program, and a program
   still held when the library goes down goes with it. */
typedef struct _Xpost_Font_Program
{
    struct _Xpost_Font_Program *next;
    void *face;
    unsigned char *bytes;
} Xpost_Font_Program;

static Xpost_Font_Program *program_head = NULL;

/* --- the font program a face was opened over -------------------------
   Kept for as long as the face might build another glyph, and given up
   with it. */

static int
program_keep(void *face, unsigned char *bytes)
{
    Xpost_Font_Program *p = malloc(sizeof *p);

    if (!p)
        return 0;
    p->face = face;
    p->bytes = bytes;
    p->next = program_head;
    program_head = p;
    return 1;
}

/* Unlink the program held for a face and hand it back, so that the caller
   can close the face before freeing what it was reading. */
static unsigned char *
program_take(void *face)
{
    Xpost_Font_Program **pp = &program_head;
    unsigned char *bytes = NULL;

    while (*pp)
    {
        Xpost_Font_Program *p = *pp;

        if (p->face != face)
        {
            pp = &p->next;
            continue;
        }
        *pp = p->next;
        if (bytes)
            free(bytes);
        bytes = p->bytes;
        free(p);
    }
    return bytes;
}

static void
program_clear(void)
{
    while (program_head)
    {
        Xpost_Font_Program *p = program_head;

        program_head = p->next;
        free(p->bytes);
        free(p);
    }
}

static void strike_clear(void);
#endif

/* Starts the font machinery: the rendering library, and the font
   configuration where the build has one. Called once, and answers
   whether faces can be opened at all. */
int
xpost_font_init(void)
{
#ifdef HAVE_FREETYPE2
    FT_Error err_ft;

    err_ft = FT_Init_FreeType(&_xpost_font_ft_library);
    if (err_ft)
        return 0;

# ifdef HAVE_FONTCONFIG
    {
        FcBool err_fc;

        err_fc = FcInit();
        if (!err_fc)
        {
            FT_Done_FreeType(_xpost_font_ft_library);
            return 0;
        }

        _xpost_font_fc_config = FcInitLoadConfigAndFonts();
        if (_xpost_font_fc_config == NULL)
            XPOST_LOG_ERR("cannot load Fc config and fonts");
    }
# endif
#endif

    /* coming up is what asks to be taken down, so a module that starts
       holding something cannot be left out of a list. The configurations
       reach it by one path rather than each carrying a copy: a copy per
       branch is one the next configuration is written without, and where
       both are compiled one of them is code that cannot run. */
    (void)xpost_at_quit(xpost_font_quit);

    return 1;
}

/* Gives up the font machinery in the order the pieces depend on each
   other: the caches first, since an entry names a face; then the
   library, which takes the faces still open with it; then the font
   programs those faces were reading. */
void
xpost_font_quit(void)
{
#ifdef HAVE_FREETYPE2
# ifdef HAVE_FONTCONFIG
    FcConfigDestroy(_xpost_font_fc_config);
    FcFini();
# endif
#endif

    gcache_clear();
#ifdef HAVE_FREETYPE2
    strike_clear();
    FT_Done_FreeType(_xpost_font_ft_library);
    /* the library takes the faces still open with it; their programs are
       this module's to give up, and nothing reads them once it has gone */
    program_clear();
#endif
}

#ifdef HAVE_FREETYPE2
/* Reading a face through the map it states.

   The rendering library selects a character map for a face only where
   the face states one over the standard code space; a face whose only
   map is the symbol map is left with none selected, and then every
   character code of it reaches no glyph at all. That map is an
   encoding all the same -- the font's own, its codes reaching their
   glyphs at the private-use points U+F020..U+F0FF -- so it is selected
   here, and such a face means by its codes what it says it means
   rather than nothing. */
static void
_xpost_font_face_map_select(FT_Face face)
{
    if (!face || face->charmap || face->num_charmaps <= 0)
        return;
    (void)FT_Select_Charmap(face, FT_ENCODING_MS_SYMBOL);
}
#endif

/* Whether the name most recently resolved landed on a face carrying
   some other name. The platform's configuration answers nearly every
   name with some face, so this is the only thing that tells a face
   that was asked for from one that was given instead. A build that
   resolves no names by itself substitutes nothing, and leaves it at
   the no. */
static int _xpost_font_last_substitute = 0;

#ifdef HAVE_FREETYPE2
# ifdef HAVE_FONTCONFIG

/* --- finding a face by name ------------------------------------------
   What the platform's font configuration answers, and how much of it to
   believe. A substitution is recorded rather than hidden, so the suite can
   hold a name to the face it actually lands on. */
/* case- and blank-insensitive name comparison, as fontconfig applies
   to family names */
static int
_fc_name_eq(const char *a, const char *b)
{
    while (*a || *b)
    {
        while (*a == ' ') a++;
        while (*b == ' ') b++;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        if (*a) a++;
        if (*b) b++;
    }
    return 1;
}

/* does the matched font actually carry the requested name (as family
   or PostScript name)? A fuzzy fontconfig match always returns some
   face, so this distinguishes a real hit from a fallback. */
static int
_fc_match_is_exact(FcPattern *match, const char *name)
{
    char *s;
    int i;

    for (i = 0; FcPatternGetString(match, FC_FAMILY, i, (FcChar8 **)&s) == FcResultMatch; i++)
        if (_fc_name_eq(s, name))
            return 1;
    for (i = 0; FcPatternGetString(match, FC_POSTSCRIPT_NAME, i, (FcChar8 **)&s) == FcResultMatch; i++)
        if (_fc_name_eq(s, name))
            return 1;
    return 0;
}

/* PostScript style-variant suffixes translated to fontconfig style
   flags, so that e.g. Helvetica-Bold reaches the bold face of the
   family Helvetica is aliased to */
static const struct { const char *suffix; const char *flags; } _ps_style_suffix[] = {
    { "-BoldItalic",  ":bold:italic" },
    { "-BoldOblique", ":bold:italic" },
    { "-Bold",        ":bold" },
    { "-Italic",      ":italic" },
    { "-Oblique",     ":italic" },
    { "-Roman",       "" },
    { "-Regular",     "" },
};

/* Asks the platform's font configuration for the closest face to a
   name. It answers with something for almost any name, the
   substitution rules seeing to that, which is why the caller records
   what it landed on rather than assuming it got what it asked for.

   The request the match was made against is handed back where the
   caller asks for it: it is the request, not the name, that says what
   kind of font was wanted, and the caller needs that to tell a name
   the configuration answered from one it fell back on. */
static FcPattern *
_fc_match_name(const char *name, FcPattern **request)
{
    FcPattern *pattern;
    FcPattern *match;
    FcResult result;

    if (request)
        *request = NULL;

    pattern = FcNameParse((const FcChar8 *)name);
    if (!pattern)
        return NULL;

    if (!FcConfigSubstitute (_xpost_font_fc_config, pattern, FcMatchPattern))
    {
        FcPatternDestroy(pattern);
        return NULL;
    }

    FcDefaultSubstitute(pattern);
    match = FcFontMatch(_xpost_font_fc_config, pattern, &result);
    switch (result) {
        case FcResultMatch: break;
        case FcResultNoMatch: goto destroy_match;
        case FcResultTypeMismatch: break;
        case FcResultNoId: break;
        case FcResultOutOfMemory: goto destroy_match;
    }
    if (request)
        *request = pattern;
    else
        FcPatternDestroy(pattern);
    return match;

  destroy_match:
    FcPatternDestroy(pattern);
    if (match)
        FcPatternDestroy(match);
    return NULL;
}

/* The same, asking by the name a font calls itself.

   A PostScript font name is not a family name, and the configuration
   reads a bare name as a family: a document asking for a face by the
   name that face carries -- which is what /FontName holds and what
   findfont is given -- can be answered with some other family
   altogether. The configuration does index the PostScript name, so the
   question can simply be asked in those terms, and where it answers the
   answer is the face the document named.

   Asked only where reading the name as a family did not reach the name,
   so a document naming a family still reaches the family. */
static FcPattern *
_fc_match_psname(const char *name, FcPattern **request)
{
    FcPattern *pattern;
    FcPattern *match;
    FcResult result;

    if (request)
        *request = NULL;

    pattern = FcPatternCreate();
    if (!pattern)
        return NULL;
    if (!FcPatternAddString(pattern, FC_POSTSCRIPT_NAME,
                            (const FcChar8 *)name))
    {
        FcPatternDestroy(pattern);
        return NULL;
    }
    if (!FcConfigSubstitute(_xpost_font_fc_config, pattern, FcMatchPattern))
    {
        FcPatternDestroy(pattern);
        return NULL;
    }
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(_xpost_font_fc_config, pattern, &result);
    if (result == FcResultNoMatch || result == FcResultOutOfMemory)
    {
        FcPatternDestroy(pattern);
        if (match)
            FcPatternDestroy(match);
        return NULL;
    }
    if (request)
        *request = pattern;
    else
        FcPatternDestroy(pattern);
    return match;
}

/* --- a name the configuration answers, and a name it falls back on ---

   Resolving a name to a face of another family is not by itself a
   substitution. A configuration is told which families stand in for
   which -- the families a document names and the families a machine
   actually holds are not the same set, and mapping one onto the other
   is what it is for -- and a name it maps is a name the machine
   considers itself to have. What the program asked for is there; it is
   there under another family's file. The name it asked by is still the
   name of the font it got, and that is what the dictionary must say.

   A name the configuration has nothing at all for is different. There
   the request falls through to the generic family every request carries
   -- the kind of font wanted rather than any font in particular -- and
   what comes back is the machine's default face of that kind. That is
   the substitution PLRM 8.2 permits in place of the invalidfont error,
   and the program has been handed a font it did not ask for.

   The two are told apart by asking a second question: what does this
   machine answer for a request of this kind carrying no family at all?
   If that is the same face, the name contributed nothing to the answer
   and the face is the default one. If it is a different face, some
   family of the request reached it, which is the configuration
   answering rather than falling back. */

/* the generic families, which name a kind of font rather than a font */
static const char *_fc_generic_family[] = {
    "serif", "sans-serif", "sans serif", "monospace",
    "cursive", "fantasy", "system-ui", "math", "emoji"
};

static const char *
_fc_request_generic(FcPattern *request)
{
    char *s;
    int i;
    size_t g;

    for (i = 0;
         FcPatternGetString(request, FC_FAMILY, i, (FcChar8 **)&s) == FcResultMatch;
         i++)
        for (g = 0; g < sizeof _fc_generic_family / sizeof *_fc_generic_family; g++)
            if (_fc_name_eq(s, _fc_generic_family[g]))
                return _fc_generic_family[g];
    return NULL;
}

/* --- the kind of face a name asks for ---------------------------------

   A name the configuration has nothing for reaches the machine's
   default face, and that face is of whatever kind the machine defaults
   to -- one kind for every name it cannot place. A document setting
   text in a serif it has not got is then set in a sans, which is not a
   different cut of the type it asked for but a different kind of type
   altogether: the wrong letterforms at the wrong widths for the measure
   the document was written to.

   A PostScript font name usually says which kind it meant. The names
   are built out of words -- run together in capitals, or split by
   hyphens -- and among those words are the ones type has always used to
   say what a face is: the class outright (Serif, Sans, Mono), the words
   other languages and other centuries use for the same classes
   (Grotesk, Gothic, Antiqua), and the names of families whose class
   nobody disputes. Where a word of the name says the kind, the request
   is put again as that kind, and the machine answers with its default
   serif or monospace instead of its default anything.

   READ AS WORDS, never as substrings. Sansom is not a sans and Monotype
   is not a monospace; both would be if the name were searched for the
   letters rather than split into its words. The split is the one the
   names are written by: a hyphen or a space ends a word, and so does a
   capital following a lowercase (StoneSerif) or a capital followed by a
   lowercase at the end of a run of capitals (ITCGaramond). A word
   counts only where it is the whole of a word, compared without regard
   to case.

   WHAT IS NOT A SIGNAL, and both of these look like one:

   Roman. In these names it is the upright cut and not the class --
   Times-Roman, Palatino-Roman, NewCenturySchlbk-Roman all use it that
   way, which is why the style table above reads it as the regular
   style. Where it does belong to a family name another word of that
   name carries the class (TimesNewRoman has Times), so nothing is lost
   by refusing it.

   Book. It is a weight, as Light and Medium are: AvantGarde-Book is a
   sans and ITCGaramond-Book is a serif, and the word is the same in
   both. Bookman is a word in its own right and is in the table below;
   Book is not.

   A NAME THAT SAYS NOTHING keeps the face the configuration gave it.
   The machine's own default is what a request carrying no family is
   entitled to reach, and a name that names no kind gives this
   interpreter no ground to overrule the machine on it. */

static const struct { const char *word; const char *generic; } _fc_class_word[] = {
    /* the class named outright */
    { "serif",       "serif" },
    { "sans",        "sans-serif" },
    { "sansserif",   "sans-serif" },
    { "mono",        "monospace" },
    { "monospace",   "monospace" },
    { "monospaced",  "monospace" },

    /* the same classes under the words other typographic traditions
       name them by: a Grotesk is a sans, an Antiqua is a serif, and a
       Gothic in Latin type is a sans (News, Franklin, Trade, Century) */
    { "grotesk",     "sans-serif" },
    { "grotesque",   "sans-serif" },
    { "gothic",      "sans-serif" },
    { "antiqua",     "serif" },
    { "slab",        "serif" },

    /* a design that is fixed-pitch by what it is: a typewriter face and
       a teletype face have one width per character, and Courier is the
       monospace of the standard PostScript set, which every name built
       on it inherits */
    { "courier",     "monospace" },
    { "typewriter",  "monospace" },
    { "teletype",    "monospace" },

    /* families whose class is not in dispute, here because documents
       name them far more often than they name a class word, and mostly
       under names the configuration cannot place: HelveticaNeue and
       ArialMT and TimesNewRomanPSMT are all one word away from a family
       the machine holds, and none of them reaches it */
    { "helvetica",   "sans-serif" },
    { "arial",       "sans-serif" },
    { "univers",     "sans-serif" },
    { "futura",      "sans-serif" },
    { "times",       "serif" },
    { "garamond",    "serif" },
    { "bookman",     "serif" },
    { "palatino",    "serif" },
    { "minion",      "serif" },
    { "century",     "serif" },
    { "schoolbook",  "serif" },
    { "schlbk",      "serif" }
};

/* How firmly a word settles the kind, where the words of one name
   disagree. A name saying both mono and something else is asking for
   the fixed pitch: that constrains the metrics a line is set to and not
   only the shapes, so it is the more particular of the two claims. A
   name saying both sans and serif is asking for the sans -- sans serif
   is literally the serif denied, so the two words together are the one
   class and not two. CenturyGothic is the case that makes this matter:
   a serif word and a sans word in one name, and it is a sans. */
static int
_fc_class_rank(const char *generic)
{
    if (strcmp(generic, "monospace") == 0)
        return 3;
    if (strcmp(generic, "sans-serif") == 0)
        return 2;
    return 1;
}

/* The next word of a font name, lowercased, from *at. Answers 0 at the
   end of the name. A word longer than the buffer is answered as an
   empty one: nothing in the table above is anywhere near that long, so
   a word that does not fit cannot be one of them, and truncating it
   could only invent a match. */
static int
_ps_name_word(const char *name, size_t *at, char *buf, size_t bufsz)
{
    size_t i = *at;
    size_t n = 0;
    int over = 0;

    while (name[i] && !isalnum((unsigned char)name[i]))
        i++;
    if (!name[i])
    {
        *at = i;
        return 0;
    }
    if (isdigit((unsigned char)name[i]))
        while (isdigit((unsigned char)name[i]))
        {
            if (n + 1 < bufsz)
                buf[n++] = name[i];
            else
                over = 1;
            i++;
        }
    else
        while (isalpha((unsigned char)name[i]))
        {
            /* a capital ends the word before it where the letter before
               is lowercase (StoneSerif), and where it is itself the last
               capital of a run and a lowercase follows (ITCGaramond) */
            if (n > 0
                && isupper((unsigned char)name[i])
                && (islower((unsigned char)name[i - 1])
                    || (isupper((unsigned char)name[i - 1])
                        && islower((unsigned char)name[i + 1]))))
                break;
            if (n + 1 < bufsz)
                buf[n++] = (char)tolower((unsigned char)name[i]);
            else
                over = 1;
            i++;
        }
    buf[over ? 0 : n] = '\0';
    *at = i;
    return 1;
}

/* The generic family the words of a name ask for, or NULL where none of
   them names a kind. */
static const char *
_fc_name_family_class(const char *name)
{
    const char *best = NULL;
    int rank = 0;
    size_t at = 0;
    char part[32];

    while (_ps_name_word(name, &at, part, sizeof part))
    {
        size_t w;

        for (w = 0; w < sizeof _fc_class_word / sizeof *_fc_class_word; w++)
            if (strcmp(part, _fc_class_word[w].word) == 0)
            {
                int r = _fc_class_rank(_fc_class_word[w].generic);

                if (r > rank)
                {
                    rank = r;
                    best = _fc_class_word[w].generic;
                }
            }
    }
    return best;
}

/* The face this machine answers a request of that kind with, when no
   family of its own reaches one. The style the request carries goes
   with it, so a bold or an italic request is answered by the default
   face of that shape rather than by the plain one. */
static FcPattern *
_fc_generic_face(const char *generic, FcPattern *request)
{
    static const char *shape[] = { FC_WEIGHT, FC_SLANT, FC_WIDTH };
    FcPattern *plain;
    FcPattern *face;
    FcResult result;
    int i;

    plain = FcPatternCreate();
    if (!plain)
        return NULL;
    if (!FcPatternAddString(plain, FC_FAMILY, (const FcChar8 *)generic))
    {
        FcPatternDestroy(plain);
        return NULL;
    }
    for (i = 0; request && i < (int)(sizeof shape / sizeof *shape); i++)
    {
        int v;

        if (FcPatternGetInteger(request, shape[i], 0, &v) == FcResultMatch)
            (void)FcPatternAddInteger(plain, shape[i], v);
    }
    if (!FcConfigSubstitute(_xpost_font_fc_config, plain, FcMatchPattern))
    {
        FcPatternDestroy(plain);
        return NULL;
    }
    FcDefaultSubstitute(plain);
    face = FcFontMatch(_xpost_font_fc_config, plain, &result);
    FcPatternDestroy(plain);
    if (result == FcResultNoMatch || result == FcResultOutOfMemory)
    {
        if (face)
            FcPatternDestroy(face);
        return NULL;
    }
    return face;
}

/* Whether the match is the face this machine answers a request of that
   kind with when no family of its own reaches one. */
static int
_fc_match_is_the_default(FcPattern *request, FcPattern *match)
{
    const char *generic = _fc_request_generic(request);
    FcPattern *fallback;
    char *mfile;
    char *dfile;
    int midx = 0;
    int didx = 0;
    int same;

    if (!generic)
        return 0;
    if (FcPatternGetString(match, FC_FILE, 0, (FcChar8 **)&mfile) != FcResultMatch)
        return 0;
    (void)FcPatternGetInteger(match, FC_INDEX, 0, &midx);

    fallback = _fc_generic_face(generic, request);
    if (!fallback)
        return 0;

    same = FcPatternGetString(fallback, FC_FILE, 0, (FcChar8 **)&dfile)
               == FcResultMatch
        && strcmp(dfile, mfile) == 0;
    if (same)
    {
        (void)FcPatternGetInteger(fallback, FC_INDEX, 0, &didx);
        same = midx == didx;
    }
    FcPatternDestroy(fallback);
    return same;
}

# endif

/* The file a name resolves to, and which face within it -- a font file
   may carry several. The caller opens the file; nothing here holds it. */
static char *
_xpost_font_face_filename_and_index_get(const char *name, int *idx)
{
# ifdef HAVE_FONTCONFIG
    FcPattern *match;
    FcPattern *request;
    char *file;
    char *filename;
    FcResult result;

    match = _fc_match_name(name, &request);
    if (!match)
        return NULL;

    /* The name a face carries, asked for as such. A bare name is read as
       a family, so a document naming a face by its own name can be
       answered with another family's face; the configuration indexes
       that name, so where reading it as a family did not reach it, it is
       asked for directly. */
    if (!_fc_match_is_exact(match, name))
    {
        FcPattern *psrequest;
        FcPattern *psmatch = _fc_match_psname(name, &psrequest);

        if (psmatch)
        {
            if (_fc_match_is_exact(psmatch, name))
            {
                FcPatternDestroy(match);
                match = psmatch;
                if (request)
                    FcPatternDestroy(request);
                request = psrequest;
            }
            else
            {
                FcPatternDestroy(psmatch);
                if (psrequest)
                    FcPatternDestroy(psrequest);
            }
        }
    }

    /* a fallback match for a PostScript style-variant name loses the
       style (fontconfig reads the whole name as a family); requery as
       family plus style flags so the alias family's styled face wins */
    if (!_fc_match_is_exact(match, name))
    {
        size_t i;

        for (i = 0; i < sizeof(_ps_style_suffix)/sizeof(*_ps_style_suffix); i++)
        {
            const char *sfx = _ps_style_suffix[i].suffix;
            size_t nlen = strlen(name), slen = strlen(sfx);

            if (nlen > slen && strcmp(name + nlen - slen, sfx) == 0)
            {
                char *styled = malloc(nlen + strlen(_ps_style_suffix[i].flags) + 1);
                FcPattern *restyled;
                FcPattern *rerequest;

                if (!styled)
                    break;
                memcpy(styled, name, nlen - slen);
                strcpy(styled + nlen - slen, _ps_style_suffix[i].flags);
                restyled = _fc_match_name(styled, &rerequest);
                free(styled);
                if (restyled)
                {
                    FcPatternDestroy(match);
                    match = restyled;
                    if (request)
                        FcPatternDestroy(request);
                    request = rerequest;
                }
                break;
            }
        }
    }

    /* Whether the program was handed a font it did not ask for. Both
       questions are asked of the match that will actually be opened,
       the requery above having possibly replaced the first one, and of
       the name as it arrived: a restyled query is this module's way of
       reaching a face, not something the caller asked for.

       A face carrying the name is the name's own, and a face some
       family of the request reached is the name's too -- the
       configuration was told those families stand for one another. Only
       where neither holds has the name reached nothing and the machine
       answered with its default face of that kind. */
    _xpost_font_last_substitute = !_fc_match_is_exact(match, name)
                               && _fc_match_is_the_default(request, match);

    /* A substitute is still owed the kind of type that was asked for.
       The default face reached above is the machine's answer to a
       request carrying no family at all, so it is of one kind whatever
       was asked for; where the words of the name say which kind was
       meant, the question is put again as that kind and the machine
       answers with its default serif, sans or monospace instead. The
       style the request carries is carried into that second question,
       so a name whose italic was recovered above stays italic.

       Only a substitute reaches here. A name the configuration
       answered keeps the face it answered with, and a name saying
       nothing about its kind keeps the face the machine chose for it. */
    if (_xpost_font_last_substitute)
    {
        const char *generic = _fc_name_family_class(name);

        if (generic)
        {
            FcPattern *classed = _fc_generic_face(generic, request);

            if (classed)
            {
                FcPatternDestroy(match);
                match = classed;
            }
        }
    }

    result = FcPatternGetString(match, FC_FILE, 0, (FcChar8 **)&file);
    if (result != FcResultMatch)
        goto destroy_match;

    XPOST_LOG_INFO("Font %s found in file %s", name, file);

    result = FcPatternGetInteger(match, FC_INDEX, 0, idx);
    if (result != FcResultMatch)
        goto destroy_match;

    XPOST_LOG_INFO("Font %s has index %d", name, *idx);

    filename = strdup(file);

    if (request)
        FcPatternDestroy(request);
    FcPatternDestroy(match);

    return filename;

  destroy_match:
    if (request)
        FcPatternDestroy(request);
    FcPatternDestroy(match);
# endif

    return NULL;
}
#endif

static char *_xpost_font_last_file = NULL;

/* How many faces the module has open. A face is host state outside
   virtual memory and its bookkeeping shows nowhere else: the library
   takes every face still open with it when the font machinery goes
   down, so a run that ends says nothing about how many it was holding
   while it ran. This is that number while it runs. */
static long _xpost_font_faces = 0;

long
xpost_font_faces_held(void)
{
    return _xpost_font_faces;
}

/* the file behind the face most recently opened by name: the caller
   reads the program itself (a Type 42 dictionary publishes it as
   sfnts) */
const char *
xpost_font_face_last_file(void)
{
    return _xpost_font_last_file;
}

/* whether the face most recently opened by name is some face other
   than the one the name asked for: read where the file is, and for
   the same reason -- it answers for the last open by name and the
   next one replaces the answer */
int
xpost_font_face_last_is_substitute(void)
{
    return _xpost_font_last_substitute;
}

/* How many faces the configuration holds, and the name each calls
   itself by.

   The configuration is an index of the host's faces, built and cached by
   the library that owns it and already loaded here, so enumerating the
   faces a program could ask for is a question put to something that
   exists rather than a catalogue this interpreter has to keep. The set
   belongs to the configuration and is not this module's to free.

   Faces outnumber names: a name is carried by one face, but a face may
   carry none, and several faces of a family answer to one name in
   different sizes. The caller sees every face and makes what it needs of
   the names. */
int
xpost_font_host_face_count(void)
{
#if defined(HAVE_FREETYPE2) && defined(HAVE_FONTCONFIG)
    FcFontSet *set;

    if (!_xpost_font_fc_config)
        return 0;
    set = FcConfigGetFonts(_xpost_font_fc_config, FcSetSystem);
    return set ? set->nfont : 0;
#else
    return 0;
#endif
}

/* The name the i-th face calls itself by, or NULL where it has none.
   The string is the configuration's; a caller keeping it copies it. */
const char *
xpost_font_host_face_name(int i)
{
#if defined(HAVE_FREETYPE2) && defined(HAVE_FONTCONFIG)
    FcFontSet *set;
    FcChar8 *psname;

    if (!_xpost_font_fc_config || i < 0)
        return NULL;
    set = FcConfigGetFonts(_xpost_font_fc_config, FcSetSystem);
    if (!set || i >= set->nfont)
        return NULL;
    if (FcPatternGetString(set->fonts[i], FC_POSTSCRIPT_NAME, 0, &psname)
        != FcResultMatch)
        return NULL;
    return (const char *)psname;
#else
    (void)i;
    return NULL;
#endif
}

/* Whether the host genuinely carries a face of this name, as against a
   default standing in for one it has not got. The resource operators
   answer for an instance they can obtain, and a substitute is not that
   instance: a program asking whether a font is available is entitled to
   no rather than to the name of something else.

   Asked of the configuration alone -- the file a name resolves to is
   found and given back again, and no face is opened, so this costs a
   lookup and not a load. */
int
xpost_font_name_is_available(const char *name)
{
#if defined(HAVE_FREETYPE2) && defined(HAVE_FONTCONFIG)
    int idx = 0;
    char *filename;

    if (!name)
        return 0;
    _xpost_font_last_substitute = 0;
    filename = _xpost_font_face_filename_and_index_get(name, &idx);
    if (!filename)
        return 0;
    free(filename);
    return !_xpost_font_last_substitute;
#else
    /* a build without the configuration has no host faces to answer for */
    (void)name;
    return 0;
#endif
}

/* Opens the face a name resolves to through the platform's
   configuration, and records the file it came from so that a caller
   can publish the program itself. */
void *
xpost_font_face_new_from_name(const char *name)
{
#ifdef HAVE_FREETYPE2
    FT_Face face;
    FT_Error err;
    char *filename;
    int idx;

    /* nothing is a substitute until the resolution says so, so a
       resolution that never reaches the question leaves a no behind
       rather than the last one's answer */
    _xpost_font_last_substitute = 0;
    filename = _xpost_font_face_filename_and_index_get(name, &idx);
    free(_xpost_font_last_file);
    _xpost_font_last_file = NULL;
    if (!filename)
        return NULL;

    err = FT_New_Face(_xpost_font_ft_library, filename, idx, &face) ;
    if (err == FT_Err_Unknown_File_Format)
    {
        XPOST_LOG_INFO("Font format unsupported");
        free(filename);
        return NULL;
    }
    else if (err)
    {
        XPOST_LOG_INFO("Font file %s can not be opened or read or is broken", filename);
        free(filename);
        return NULL;
    }

    _xpost_font_face_map_select(face);
    _xpost_font_last_file = filename;
    _xpost_font_faces++;

    return face;
#else
    (void)name;
#endif

    return NULL;
}

/* Opens a face over a font program the caller already holds -- an
   embedded font, or one a program built. The bytes are kept for as
   long as the face is, the rendering library reading them as it goes
   rather than copying them. */
void *
xpost_font_face_new_from_memory(const unsigned char *data, size_t len)
{
#ifdef HAVE_FREETYPE2
    FT_Face face;
    FT_Error err;

    err = FT_New_Memory_Face(_xpost_font_ft_library, data, (FT_Long)len, 0, &face);
    if (err)
    {
        XPOST_LOG_INFO("Font program can not be opened or read or is broken (error : %d)", err);
        return NULL;
    }

    _xpost_font_face_map_select(face);

    /* the face reads the program where it lies, so the program becomes
       the face's from here. A face that cannot be given its program is
       refused rather than left reading bytes nothing will free. */
    if (!program_keep(face, (unsigned char *)data))
    {
        FT_Done_Face(face);
        return NULL;
    }
    _xpost_font_faces++;

    return face;
#else
    (void)data;
    (void)len;
#endif

    return NULL;
}

/* The face's design bounding box, in the units the face declares. */
void
xpost_font_face_get_bbox(void *face, Xpost_Object *bboxarray, real em){
#ifdef HAVE_FREETYPE2
    FT_Face f = face;
    real s = 1.0;

    /* FontBBox belongs to the glyph coordinate system, whose scale is a
       convention of the font type (1000 units per em for Type 1, one
       unit for Type 42): the face's design units are normalized to the
       em the caller names */
    if (f->units_per_EM > 0)
        s = em / f->units_per_EM;
    bboxarray[0] = xpost_real_cons(f->bbox.xMin * s);
    bboxarray[1] = xpost_real_cons(f->bbox.yMin * s);
    bboxarray[2] = xpost_real_cons(f->bbox.xMax * s);
    bboxarray[3] = xpost_real_cons(f->bbox.yMax * s);
#else
    (void)face;
    (void)bboxarray;
    (void)em;
#endif
}

/* How many design units the face puts in an em, or zero where it
   declares none. */
int
xpost_font_face_units(void *face)
{
#ifdef HAVE_FREETYPE2
    FT_Face f = face;

    return f->units_per_EM > 0 ? f->units_per_EM : 0;
#else
    (void)face;
    return 0;
#endif
}

/* What kind of font program the face was opened over. The three are
   asked separately because each answers a different question about
   what else can be got out of the face. */
int
xpost_font_face_is_truetype(void *face)
{
#ifdef HAVE_FREETYPE2
    const char *fmt = FT_Get_Font_Format((FT_Face)face);

    return fmt && strcmp(fmt, "TrueType") == 0;
#else
    (void)face;
    return 0;
#endif
}

int
xpost_font_face_is_type1(void *face)
{
#ifdef HAVE_FREETYPE2
    const char *fmt = FT_Get_Font_Format((FT_Face)face);

    return fmt && strcmp(fmt, "Type 1") == 0;
#else
    (void)face;
    return 0;
#endif
}

int
xpost_font_face_is_cff(void *face)
{
#ifdef HAVE_FREETYPE2
    const char *fmt = FT_Get_Font_Format((FT_Face)face);

    return fmt && strcmp(fmt, "CFF") == 0;
#else
    (void)face;
    return 0;
#endif
}

void
xpost_font_face_reference(void *face)
{
#ifdef HAVE_FREETYPE2
    if (!face)
        return;

    /* the count is the library's: each holder releases through
       xpost_font_face_free, and the face goes when the last one has */
    FT_Reference_Face((FT_Face)face);
    _xpost_font_faces++;
#else
    (void)face;
#endif
}

void
xpost_font_face_free(void *face)
{
#ifdef HAVE_FREETYPE2
    Xpost_Glyph_Entry *e, *next;
    unsigned char *bytes;
    int i;

    if (!face)
        return;

    /* the address may be reissued: no stale rasters may answer it */
    for (e = gcache_head; e; e = next)
    {
        next = e->lnext;
        if (e->k1 == face)
            gcache_drop(e);
    }
    for (i = 0; i < gcache_nstate; i++)
        if (gcache_state[i].face == face)
            gcache_state[i].face = NULL;
    /* trailing slots given back shrink the population, so the count
       bounds the live faces rather than every face there has ever been */
    while (gcache_nstate > 0 && !gcache_state[gcache_nstate - 1].face)
        gcache_nstate--;

    /* the face reads its program until it is closed, so the program is
       taken off the list while the face is still a face, and given up
       once the face has gone */
    bytes = program_take(face);
    FT_Done_Face(face);
    free(bytes);
    _xpost_font_faces--;
#else
    (void)face;
#endif
}

/* Sets the size glyphs are rendered at and answers the size actually
   taken, which a face with fixed strikes may round. */
real
xpost_font_face_scale(void *face, real scale)
{
#ifdef HAVE_FREETYPE2
    FT_Face f = face;

    /* Request the scale directly (16.16 pixels per font unit) rather
       than a nominal character size: a nominal request quantizes the
       em to 1/64 pixel, which at the sub-pixel em sizes produced by a
       small user-space size under a modest CTM is a metrics error of
       whole percents.

       FreeType's size machinery is only dependable within a moderate
       band of pixel sizes: a sub-pixel or enormous em makes the
       request fail or degrade silently. Since every glyph is loaded
       unhinted, geometry is linear in the scale, so an extreme
       request is served at a clamped, well-conditioned size instead
       and the caller folds the residual ratio into the face
       transform. Returns the scale actually installed; fixed-size
       faces keep the nominal path. */
    if (f->units_per_EM > 0)
    {
        FT_Size_RequestRec req;
        real base = scale;

        if (!(base >= 8.0))     /* the negated test also catches a NaN scale */
            base = 8.0;
        else if (base > 2048.0)
            base = 2048.0;

        req.type = FT_SIZE_REQUEST_TYPE_SCALES;
        req.width = req.height =
            (FT_Long)(base * 64.0 * 65536.0 / f->units_per_EM + 0.5);
        req.horiResolution = 0;
        req.vertResolution = 0;
        if (FT_Request_Size(f, &req) == 0
         || FT_Set_Char_Size(f, 0, (FT_F26Dot6)(base * 64 + 0.5),
                             72, 72) == 0)
        {
            long sz = (long)(base * 64.0 + 0.5);

            gcache_state_set(face, NULL, &sz);
            return base;
        }
    }
    if (f->num_fixed_sizes > 0)
    {
        /* a fixed-size face serves its nearest strike; the residual
           ratio to the requested size rides in the face transform,
           and the strike's raster and advance are scaled by it when
           the glyph is served */
        int i, best = 0;
        double want = scale, bestd = -1.0;

        for (i = 0; i < f->num_fixed_sizes; i++)
        {
            double got = f->available_sizes[i].y_ppem / 64.0;
            double d = got > want ? got - want : want - got;

            if (bestd < 0.0 || d < bestd)
            {
                bestd = d;
                best = i;
            }
        }
        if (FT_Select_Size(f, best) == 0)
        {
            long sz = (long)f->available_sizes[best].y_ppem;

            gcache_state_set(face, NULL, &sz);
            return (real)(f->available_sizes[best].y_ppem / 64.0);
        }
    }
    FT_Set_Char_Size(f, 0, (FT_F26Dot6)(scale * 64 + 0.5), 72, 72);
    {
        long sz = (long)(scale * 64.0 + 0.5);

        gcache_state_set(face, NULL, &sz);
    }
#else
    (void)face;
#endif
    return scale;
}

/* Sets the transform glyphs are rendered under, and records it against
   the face so the cache can key on it. Kept as the fixed-point value
   the library was handed rather than the float it came from: two
   transforms that round to the same fixed point render the same glyph,
   and must key the same. */
void
xpost_font_face_transform(void *face, float *mat)
{
#ifdef HAVE_FREETYPE2
    FT_Matrix matrix;
    /* A NaN or out-of-range element would make the cast to the 16.16
       fixed-point FT_Fixed undefined; hold each to the range that fits and
       send a non-finite one to zero -- a NaN passes neither comparison. */
#define XPOST_FT_FIX(v) \
    ((FT_Fixed)(((v) >= -32000.0f && (v) <= 32000.0f ? (v) : 0.0f) * 0x10000L))
    matrix.xx = XPOST_FT_FIX(mat[0]);
    matrix.xy = XPOST_FT_FIX(mat[1]);
    matrix.yx = XPOST_FT_FIX(mat[2]);
    matrix.yy = XPOST_FT_FIX(mat[3]);
#undef XPOST_FT_FIX
    FT_Set_Transform((FT_Face)face, &matrix, 0);
    {
        long m[4];

        m[0] = (long)matrix.xx; m[1] = (long)matrix.xy;
        m[2] = (long)matrix.yx; m[3] = (long)matrix.yy;
        gcache_state_set(face, m, NULL);
    }
#else
    (void)face;
    (void)mat;
#endif
}

/* The name the face states for itself: the PostScript name a font
   program carries, and where it carries none, the family the platform
   knows the file by. */
int
xpost_font_face_name_get(void *face, char *buf, int len)
{
#ifdef HAVE_FREETYPE2
    const char *n = FT_Get_Postscript_Name((FT_Face)face);

    if (!n || !*n)
        n = ((FT_Face)face)->family_name;
    if (!n || !*n)
        return 0;
    if ((int)strlen(n) >= len)
        return 0;
    strcpy(buf, n);
    return 1;
#else
    (void)face;
    (void)buf;
    (void)len;
    return 0;
#endif
}

/* Whether the map the face is read through is the symbol map. */
int
xpost_font_face_is_symbol_encoded(void *face)
{
#ifdef HAVE_FREETYPE2
    FT_Face f = (FT_Face)face;

    return f && f->charmap && f->charmap->encoding == FT_ENCODING_MS_SYMBOL;
#else
    (void)face;
    return 0;
#endif
}

#ifdef HAVE_FREETYPE2
/* The point in the face's own character map that a character code
   reaches its glyph at. A face read through the symbol map keeps its
   codes in the private-use area, so a code reaches its glyph there;
   every other map is asked for the code itself. */
static FT_ULong
_xpost_font_face_code_point(FT_Face face, unsigned int code)
{
    if (face && face->charmap
     && face->charmap->encoding == FT_ENCODING_MS_SYMBOL
     && code <= 0xFF)
    {
        FT_ULong pua = 0xF000UL + code;

        if (FT_Get_Char_Index(face, pua))
            return pua;
    }
    return (FT_ULong)code;
}
#endif

/* The glyph a character code selects through the face's own encoding. */
unsigned int
xpost_font_face_glyph_index_get(void *face, char c)
{
#ifdef HAVE_FREETYPE2
    /* the character code is a byte value: keep 128-255 out of the
       sign extension */
    unsigned int code = (unsigned char)c;

    return FT_Get_Char_Index(face,
                             _xpost_font_face_code_point((FT_Face)face, code));
#else
    (void)face;
    (void)c;
    return 0;
#endif
}

/* The name a face that names no glyphs gives a character code: the
   point its own character map reaches the glyph at, spelled the way
   the resolution below reads such a name back. */
int
xpost_font_face_code_glyph_name(void *face, unsigned int code,
                                char *buf, int len)
{
#ifdef HAVE_FREETYPE2
    FT_ULong cp;
    char name[16];

    if (code > 0xFF)
        return 0;
    cp = _xpost_font_face_code_point((FT_Face)face, code);
    if (!FT_Get_Char_Index((FT_Face)face, cp))
        return 0;
    snprintf(name, sizeof name, "u%04lX", (unsigned long)cp);
    if ((int)strlen(name) >= len)
        return 0;
    strcpy(buf, name);
    return 1;
#else
    (void)face;
    (void)code;
    (void)buf;
    (void)len;
    return 0;
#endif
}

#ifdef HAVE_FREETYPE2
/* Adobe glyph name -> Unicode for the names the standard encodings hold
   (PLRM Appendix E.6 and E.7). Over U+0020..U+007E and U+00A0..U+00FF
   the ISOLatin1Encoding position is the code point, and the rest are
   named below where they part company. Lets a named /Encoding select a
   glyph on a face whose post table stores no names, by resolving the
   name to Unicode and consulting the character map, and is the
   enumeration such a face's glyph complement is published from. */
static const struct { const char *name; unsigned short cp; } _xpost_glyph_unicode[] = {
    { "space", 0x0020 },
    { "exclam", 0x0021 },
    { "quotedbl", 0x0022 },
    { "numbersign", 0x0023 },
    { "dollar", 0x0024 },
    { "percent", 0x0025 },
    { "ampersand", 0x0026 },
    { "quoteright", 0x0027 },
    { "parenleft", 0x0028 },
    { "parenright", 0x0029 },
    { "asterisk", 0x002A },
    { "plus", 0x002B },
    { "comma", 0x002C },
    { "minus", 0x002D },
    { "period", 0x002E },
    { "slash", 0x002F },
    { "zero", 0x0030 },
    { "one", 0x0031 },
    { "two", 0x0032 },
    { "three", 0x0033 },
    { "four", 0x0034 },
    { "five", 0x0035 },
    { "six", 0x0036 },
    { "seven", 0x0037 },
    { "eight", 0x0038 },
    { "nine", 0x0039 },
    { "colon", 0x003A },
    { "semicolon", 0x003B },
    { "less", 0x003C },
    { "equal", 0x003D },
    { "greater", 0x003E },
    { "question", 0x003F },
    { "at", 0x0040 },
    { "A", 0x0041 },
    { "B", 0x0042 },
    { "C", 0x0043 },
    { "D", 0x0044 },
    { "E", 0x0045 },
    { "F", 0x0046 },
    { "G", 0x0047 },
    { "H", 0x0048 },
    { "I", 0x0049 },
    { "J", 0x004A },
    { "K", 0x004B },
    { "L", 0x004C },
    { "M", 0x004D },
    { "N", 0x004E },
    { "O", 0x004F },
    { "P", 0x0050 },
    { "Q", 0x0051 },
    { "R", 0x0052 },
    { "S", 0x0053 },
    { "T", 0x0054 },
    { "U", 0x0055 },
    { "V", 0x0056 },
    { "W", 0x0057 },
    { "X", 0x0058 },
    { "Y", 0x0059 },
    { "Z", 0x005A },
    { "bracketleft", 0x005B },
    { "backslash", 0x005C },
    { "bracketright", 0x005D },
    { "asciicircum", 0x005E },
    { "underscore", 0x005F },
    { "quoteleft", 0x0060 },
    { "a", 0x0061 },
    { "b", 0x0062 },
    { "c", 0x0063 },
    { "d", 0x0064 },
    { "e", 0x0065 },
    { "f", 0x0066 },
    { "g", 0x0067 },
    { "h", 0x0068 },
    { "i", 0x0069 },
    { "j", 0x006A },
    { "k", 0x006B },
    { "l", 0x006C },
    { "m", 0x006D },
    { "n", 0x006E },
    { "o", 0x006F },
    { "p", 0x0070 },
    { "q", 0x0071 },
    { "r", 0x0072 },
    { "s", 0x0073 },
    { "t", 0x0074 },
    { "u", 0x0075 },
    { "v", 0x0076 },
    { "w", 0x0077 },
    { "x", 0x0078 },
    { "y", 0x0079 },
    { "z", 0x007A },
    { "braceleft", 0x007B },
    { "bar", 0x007C },
    { "braceright", 0x007D },
    { "asciitilde", 0x007E },
    { "exclamdown", 0x00A1 },
    { "cent", 0x00A2 },
    { "sterling", 0x00A3 },
    { "currency", 0x00A4 },
    { "yen", 0x00A5 },
    { "brokenbar", 0x00A6 },
    { "section", 0x00A7 },
    { "dieresis", 0x00A8 },
    { "copyright", 0x00A9 },
    { "ordfeminine", 0x00AA },
    { "guillemotleft", 0x00AB },
    { "logicalnot", 0x00AC },
    /* the hyphen a line of text is set with. The Latin-1 position the
       rest of this range follows holds the one a line break leaves
       behind, which a face need carry no glyph for at all and which the
       standard encoding does not put this name at: it names the
       character at position 45, where the two encodings differ only in
       calling it hyphen and minus. */
    { "hyphen", 0x002D },
    { "registered", 0x00AE },
    { "macron", 0x00AF },
    { "degree", 0x00B0 },
    { "plusminus", 0x00B1 },
    { "twosuperior", 0x00B2 },
    { "threesuperior", 0x00B3 },
    { "acute", 0x00B4 },
    { "mu", 0x00B5 },
    { "paragraph", 0x00B6 },
    { "periodcentered", 0x00B7 },
    { "cedilla", 0x00B8 },
    { "onesuperior", 0x00B9 },
    { "ordmasculine", 0x00BA },
    { "guillemotright", 0x00BB },
    { "onequarter", 0x00BC },
    { "onehalf", 0x00BD },
    { "threequarters", 0x00BE },
    { "questiondown", 0x00BF },
    { "Agrave", 0x00C0 },
    { "Aacute", 0x00C1 },
    { "Acircumflex", 0x00C2 },
    { "Atilde", 0x00C3 },
    { "Adieresis", 0x00C4 },
    { "Aring", 0x00C5 },
    { "AE", 0x00C6 },
    { "Ccedilla", 0x00C7 },
    { "Egrave", 0x00C8 },
    { "Eacute", 0x00C9 },
    { "Ecircumflex", 0x00CA },
    { "Edieresis", 0x00CB },
    { "Igrave", 0x00CC },
    { "Iacute", 0x00CD },
    { "Icircumflex", 0x00CE },
    { "Idieresis", 0x00CF },
    { "Eth", 0x00D0 },
    { "Ntilde", 0x00D1 },
    { "Ograve", 0x00D2 },
    { "Oacute", 0x00D3 },
    { "Ocircumflex", 0x00D4 },
    { "Otilde", 0x00D5 },
    { "Odieresis", 0x00D6 },
    { "multiply", 0x00D7 },
    { "Oslash", 0x00D8 },
    { "Ugrave", 0x00D9 },
    { "Uacute", 0x00DA },
    { "Ucircumflex", 0x00DB },
    { "Udieresis", 0x00DC },
    { "Yacute", 0x00DD },
    { "Thorn", 0x00DE },
    { "germandbls", 0x00DF },
    { "agrave", 0x00E0 },
    { "aacute", 0x00E1 },
    { "acircumflex", 0x00E2 },
    { "atilde", 0x00E3 },
    { "adieresis", 0x00E4 },
    { "aring", 0x00E5 },
    { "ae", 0x00E6 },
    { "ccedilla", 0x00E7 },
    { "egrave", 0x00E8 },
    { "eacute", 0x00E9 },
    { "ecircumflex", 0x00EA },
    { "edieresis", 0x00EB },
    { "igrave", 0x00EC },
    { "iacute", 0x00ED },
    { "icircumflex", 0x00EE },
    { "idieresis", 0x00EF },
    { "eth", 0x00F0 },
    { "ntilde", 0x00F1 },
    { "ograve", 0x00F2 },
    { "oacute", 0x00F3 },
    { "ocircumflex", 0x00F4 },
    { "otilde", 0x00F5 },
    { "odieresis", 0x00F6 },
    { "divide", 0x00F7 },
    { "oslash", 0x00F8 },
    { "ugrave", 0x00F9 },
    { "uacute", 0x00FA },
    { "ucircumflex", 0x00FB },
    { "udieresis", 0x00FC },
    { "yacute", 0x00FD },
    { "thorn", 0x00FE },
    { "ydieresis", 0x00FF },
    /* The rest of StandardEncoding (PLRM Appendix E.6). These are the
       names whose character is not where the Latin-1 rule above puts
       it: the encoding position and the code point part company above
       U+00FF, at the ligatures and the letters no Latin-1 position
       holds, and at the accents, which are the spacing forms rather
       than the ASCII characters that resemble them -- the circumflex
       and the tilde of an accent are not asciicircum and asciitilde,
       which keep their own names and their own positions. */
    { "quotesingle", 0x0027 },
    { "grave", 0x0060 },
    { "dotlessi", 0x0131 },
    { "Lslash", 0x0141 },
    { "lslash", 0x0142 },
    { "OE", 0x0152 },
    { "oe", 0x0153 },
    { "florin", 0x0192 },
    { "circumflex", 0x02C6 },
    { "caron", 0x02C7 },
    { "breve", 0x02D8 },
    { "dotaccent", 0x02D9 },
    { "ring", 0x02DA },
    { "ogonek", 0x02DB },
    { "tilde", 0x02DC },
    { "hungarumlaut", 0x02DD },
    { "endash", 0x2013 },
    { "emdash", 0x2014 },
    { "quotesinglbase", 0x201A },
    { "quotedblleft", 0x201C },
    { "quotedblright", 0x201D },
    { "quotedblbase", 0x201E },
    { "dagger", 0x2020 },
    { "daggerdbl", 0x2021 },
    { "bullet", 0x2022 },
    { "ellipsis", 0x2026 },
    { "perthousand", 0x2030 },
    { "guilsinglleft", 0x2039 },
    { "guilsinglright", 0x203A },
    { "fraction", 0x2044 },
    { "fi", 0xFB01 },
    { "fl", 0xFB02 },
};

/* The character a glyph name stands for: the uniXXXX and uXXXX forms
   read as the hexadecimal they spell, and the names the table above
   carries looked up in it. */
static long
_xpost_glyph_name_to_unicode(const char *name)
{
    size_t i, n;
    char *end;
    long v;

    /* uniXXXX: exactly four hexadecimal digits */
    if (strncmp(name, "uni", 3) == 0 && strlen(name + 3) == 4)
    {
        v = strtol(name + 3, &end, 16);
        if (*end == '\0' && v >= 0)
            return v;
    }
    /* uXXXX .. uXXXXXX: four to six hexadecimal digits */
    if (name[0] == 'u' && name[1] != 'n')
    {
        n = strlen(name + 1);
        if (n >= 4 && n <= 6 && isxdigit((unsigned char)name[1]))
        {
            v = strtol(name + 1, &end, 16);
            if (*end == '\0' && v >= 0 && v <= 0x10FFFF)
                return v;
        }
    }
    for (i = 0; i < sizeof _xpost_glyph_unicode / sizeof _xpost_glyph_unicode[0]; i++)
        if (strcmp(name, _xpost_glyph_unicode[i].name) == 0)
            return _xpost_glyph_unicode[i].cp;
    return -1;
}
#endif /* HAVE_FREETYPE2 */

unsigned int
xpost_font_face_glyph_name_count(void *face)
{
#ifdef HAVE_FREETYPE2
    if (!FT_HAS_GLYPH_NAMES((FT_Face)face))
        return 0;
    return (unsigned int)((FT_Face)face)->num_glyphs;
#else
    (void)face;
    return 0;
#endif
}

/* The name the face gives a glyph, where it names its glyphs at all. */
int
xpost_font_face_glyph_name_get(void *face, unsigned int gid, char *buf, int len)
{
#ifdef HAVE_FREETYPE2
    if (!FT_HAS_GLYPH_NAMES((FT_Face)face))
        return 0;
    if (FT_Get_Glyph_Name((FT_Face)face, gid, buf, (FT_UInt)len) != 0)
        return 0;
    return buf[0] != '\0';
#else
    (void)face;
    (void)gid;
    (void)buf;
    (void)len;
    return 0;
#endif
}

/* The name at a position in the standard ordering, for a face that
   names its glyphs by that ordering rather than carrying names of its
   own. */
int
xpost_font_face_std_name_at(void *face, unsigned int i,
                            const char **name, unsigned int *gid)
{
#ifdef HAVE_FREETYPE2
    if (i >= sizeof _xpost_glyph_unicode / sizeof _xpost_glyph_unicode[0])
        return 0;
    *name = _xpost_glyph_unicode[i].name;
    *gid = FT_Get_Char_Index((FT_Face)face,
                             (FT_ULong)_xpost_glyph_unicode[i].cp);
    return 1;
#else
    (void)face;
    (void)i;
    (void)name;
    (void)gid;
    return 0;
#endif
}

/* The glyph the face itself gives this name to, and nothing else: a
   face that never wrote the name down answers nothing, whatever its
   character map would reach for the character of that name. Whether a
   face has a name of its own is what an encoding recovered from the
   face turns on, and the resolution below cannot be asked it. */
unsigned int
xpost_font_face_own_name_index_get(void *face, const char *name)
{
#ifdef HAVE_FREETYPE2
    if (!FT_HAS_GLYPH_NAMES((FT_Face)face))
        return 0;
    return FT_Get_Name_Index((FT_Face)face, (FT_String *)name);
#else
    (void)face;
    (void)name;
    return 0;
#endif
}

/* The glyph a name selects, which is how a font that re-encodes by
   name reaches one. */
unsigned int
xpost_font_face_glyph_name_index_get(void *face, const char *name)
{
#ifdef HAVE_FREETYPE2
    unsigned int gi;
    long uni;

    gi = xpost_font_face_own_name_index_get(face, name);
    if (gi)
        return gi;
    /* no post name for this glyph: resolve the Adobe name to Unicode and
       take it through the character map */
    uni = _xpost_glyph_name_to_unicode(name);
    if (uni >= 0)
        return FT_Get_Char_Index((FT_Face)face, (FT_ULong)uni);
    return 0;
#else
    (void)face;
    (void)name;
    return 0;
#endif
}

#ifdef HAVE_FREETYPE2
/* FT_Outline_Decompose adapter: track the current point, divide the
   26.6 fixed-point coordinates out to pixels, and raise quadratic
   segments to the equivalent cubics so the sink sees one curve form.
   The decomposition starts each contour with a moveto and leaves it
   implicitly closed, so a closepath is synthesized before the next
   contour and after the last. */
struct _outline_walk
{
    const Xpost_Font_Outline_Sink *sink;
    double x, y;
    int open;
};

static int
_outline_moveto(const FT_Vector *to, void *user)
{
    struct _outline_walk *w = user;

    if (w->open)
    {
        int ret = w->sink->closepath(w->sink->user);
        if (ret)
            return ret;
    }
    w->open = 1;
    w->x = to->x / 64.0;
    w->y = to->y / 64.0;
    return w->sink->moveto(w->sink->user, w->x, w->y);
}

static int
_outline_lineto(const FT_Vector *to, void *user)
{
    struct _outline_walk *w = user;

    w->x = to->x / 64.0;
    w->y = to->y / 64.0;
    return w->sink->lineto(w->sink->user, w->x, w->y);
}

static int
_outline_conicto(const FT_Vector *control, const FT_Vector *to, void *user)
{
    struct _outline_walk *w = user;
    double cx = control->x / 64.0;
    double cy = control->y / 64.0;
    double ex = to->x / 64.0;
    double ey = to->y / 64.0;
    /* a quadratic's control point pulls each cubic control 2/3 of the
       way from the respective endpoint */
    double c1x = w->x + (cx - w->x) * (2.0 / 3.0);
    double c1y = w->y + (cy - w->y) * (2.0 / 3.0);
    double c2x = ex + (cx - ex) * (2.0 / 3.0);
    double c2y = ey + (cy - ey) * (2.0 / 3.0);

    w->x = ex;
    w->y = ey;
    return w->sink->curveto(w->sink->user, c1x, c1y, c2x, c2y, ex, ey);
}

static int
_outline_cubicto(const FT_Vector *control1, const FT_Vector *control2, const FT_Vector *to, void *user)
{
    struct _outline_walk *w = user;

    w->x = to->x / 64.0;
    w->y = to->y / 64.0;
    return w->sink->curveto(w->sink->user,
                            control1->x / 64.0, control1->y / 64.0,
                            control2->x / 64.0, control2->y / 64.0,
                            w->x, w->y);
}
#endif


#ifdef HAVE_FREETYPE2
/* The hinter rounds slot->advance to whole pixels and the rounding
   accumulates as horizontal drift across a string. Derive the pen
   advance from the unhinted linear width instead, applied through the
   face's current transform (identity when none is set), and report it
   in 16.16 pixels: squeezing through the slot's 26.6 resolution costs
   up to 1/64 pixel per glyph, a whole percent of a sub-pixel em.
   Bitmap-only glyphs carry no linear width; those widen the slot
   advance. */
static void
_glyph_linear_advance(FT_Face face, long *advance_x, long *advance_y)
{
    FT_GlyphSlot slot = face->glyph;
    FT_Fixed lin = slot->linearHoriAdvance;   /* 16.16 pixels */
    FT_Matrix m;

    if (lin == 0)
    {
        /* a strike's advance rides here (26.6), already carrying the
           face transform: FreeType exempts only the raster itself */
        *advance_x = slot->advance.x << 10;   /* 26.6 -> 16.16 */
        *advance_y = slot->advance.y << 10;
        return;
    }
    FT_Get_Transform(face, &m, NULL);
    *advance_x = FT_MulFix(m.xx, lin);
    *advance_y = FT_MulFix(m.yx, lin);
}
#endif

/* Walks a glyph's outline into the caller's sink, as moves, lines and
   cubics -- a quadratic being raised to a cubic on the way -- with
   each contour closed. Loaded unhinted, because an outline is asked
   for to be transformed further and hinting is a decision about
   pixels. */
int
xpost_font_face_glyph_outline(void *face, unsigned int glyph_index, const Xpost_Font_Outline_Sink *sink, long *advance_x, long *advance_y)
{
#ifdef HAVE_FREETYPE2
    FT_GlyphSlot slot;
    FT_Outline *outline;
    FT_Error err;
    struct _outline_walk w;

    err = FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING);
    if (err)
    {
        XPOST_LOG_INFO("Can not load glyph (error : %d)", err);
        return 0;
    }
    slot = ((FT_Face)face)->glyph;
    _glyph_linear_advance((FT_Face)face, advance_x, advance_y);
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
    {
        XPOST_LOG_INFO("glyph has no outline");
        return 0;
    }
    outline = &slot->outline;

    w.sink = sink;
    w.x = 0;
    w.y = 0;
    w.open = 0;
    {
        FT_Outline_Funcs funcs;

        funcs.move_to = _outline_moveto;
        funcs.line_to = _outline_lineto;
        funcs.conic_to = _outline_conicto;
        funcs.cubic_to = _outline_cubicto;
        funcs.shift = 0;
        funcs.delta = 0;
        err = FT_Outline_Decompose(outline, &funcs, &w);
        if (err)
            return 0;
    }
    if (w.open && sink->closepath(sink->user))
        return 0;
    return 1;
#else
    (void)face;
    (void)glyph_index;
    (void)sink;
    (void)advance_x;
    (void)advance_y;
    return 0;
#endif
}

/* The ink extent of a glyph's outline in 26.6 glyph space (y-up around
   the pen), without rasterizing. An empty outline (a space) reports a
   degenerate box; a glyph with no outline at all (a bitmap strike)
   reports failure so the caller can fall back to rendering. The advance
   comes from the same load, in 16.16 as the rendering path reports it. */
int
xpost_font_face_glyph_extents(void *face, unsigned int glyph_index,
                              long *xmin, long *ymin, long *xmax, long *ymax,
                              long *advance_x, long *advance_y)
{
#ifdef HAVE_FREETYPE2
    FT_Error err;
    FT_GlyphSlot slot;
    FT_BBox box;

    err = FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING);
    if (err)
    {
        XPOST_LOG_INFO("Can not load glyph (error : %d)", err);
        return 0;
    }
    slot = ((FT_Face)face)->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
        return 0;
    FT_Outline_Get_BBox(&slot->outline, &box);
    *xmin = box.xMin;
    *ymin = box.yMin;
    *xmax = box.xMax;
    *ymax = box.yMax;
    _glyph_linear_advance((FT_Face)face, advance_x, advance_y);
    return 1;
#else
    (void)face;
    (void)glyph_index;
    (void)xmin;
    (void)ymin;
    (void)xmax;
    (void)ymax;
    (void)advance_x;
    (void)advance_y;
    return 0;
#endif
}

/* The glyph's own left sidebearing -- the position of its left
   sidebearing point in glyph space (PLRM 5.4), which is where the face
   draws it relative to the origin -- under the face's current
   transform, in 16.16 y-up, the form and convention the advances arrive
   in.

   The slot's metrics are scaled but never transformed: FreeType puts
   the transform through the outline and the advance alone. So the
   bearing goes through it here, the way the linear advance does, and a
   rotated or skewed font asks the same question of both. */
int
xpost_font_face_glyph_sidebearing(void *face, unsigned int glyph_index,
                                  long *sbx, long *sby)
{
#ifdef HAVE_FREETYPE2
    FT_Face f = face;
    FT_Matrix m;
    FT_Fixed lsb;

    if (FT_Load_Glyph(f, glyph_index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING))
    {
        XPOST_LOG_INFO("Can not load glyph (error : %d)", glyph_index);
        return 0;
    }
    /* 26.6 -> 16.16, by multiplication: the bearing of a glyph that
       reaches left of its origin is negative, and shifting one of those
       is not arithmetic C defines */
    lsb = (FT_Fixed)f->glyph->metrics.horiBearingX * 1024;
    FT_Get_Transform(f, &m, NULL);
    *sbx = FT_MulFix(m.xx, lsb);
    *sby = FT_MulFix(m.yx, lsb);
    return 1;
#else
    (void)face;
    (void)glyph_index;
    (void)sbx;
    (void)sby;
    return 0;
#endif
}

#ifdef HAVE_FREETYPE2
/* a fixed-size face's strike raster, resampled by the residual ratio
   the face transform carries (FreeType applies that transform to
   scalable formats only); serves the current glyph when the cache
   declines it */
static struct
{
    unsigned char *bits;
    int rows, width, pitch;
    char pixel_mode;
    int left, top;
    int valid;
} _strike_scaled;

/* Give the resampled strike back. It is one raster, replaced as each
   glyph needs one, so what is held at any moment is the last glyph
   scaled -- and at the teardown that is a raster this module allocated
   and nothing else will free. */
static void strike_clear(void)
{
    free(_strike_scaled.bits);
    memset(&_strike_scaled, 0, sizeof(_strike_scaled));
}

/* scale the loaded strike bitmap by the face transform's column norms
   (a rotation is not applied: a strike raster has no orientation to
   give). Fills _strike_scaled and returns 1 when scaling was needed
   and possible. */
static int
_strike_resample(FT_Face f, const long m[4], long ax, long ay)
{
    FT_Bitmap *b = &f->glyph->bitmap;
    double sx = sqrt((double)m[0] * m[0] + (double)m[2] * m[2]) / 65536.0;
    double sy = sqrt((double)m[1] * m[1] + (double)m[3] * m[3]) / 65536.0;
    int dw, dh, dpitch, i, j;
    unsigned char *bits;

    (void)ax;
    (void)ay;
    if (sx > 0.996 && sx < 1.004 && sy > 0.996 && sy < 1.004)
        return 0;
    if (b->pixel_mode != FT_PIXEL_MODE_MONO
     && b->pixel_mode != FT_PIXEL_MODE_GRAY)
        return 0;
    if (sx <= 0.0 || sy <= 0.0 || b->width == 0 || b->rows == 0)
        return 0;

    dw = (int)(b->width * sx + 0.5);
    dh = (int)(b->rows * sy + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > 4096 || dh > 4096)
        return 0;
    dpitch = b->pixel_mode == FT_PIXEL_MODE_MONO ? (dw + 7) / 8 : dw;
    bits = calloc((size_t)dpitch, (size_t)dh);
    if (!bits)
        return 0;

    for (i = 0; i < dh; i++)
    {
        int si = (int)(i / sy);
        const unsigned char *srow;

        if (si >= (int)b->rows) si = (int)b->rows - 1;
        srow = b->buffer + si * b->pitch;
        for (j = 0; j < dw; j++)
        {
            int sj = (int)(j / sx);
            unsigned int pix;

            if (sj >= (int)b->width) sj = (int)b->width - 1;
            if (b->pixel_mode == FT_PIXEL_MODE_MONO)
            {
                pix = (srow[sj / 8] >> (7 - (sj % 8))) & 1;
                if (pix)
                    bits[i * dpitch + j / 8] |= (unsigned char)(0x80 >> (j % 8));
            }
            else
                bits[i * dpitch + j] = srow[sj];
        }
    }

    free(_strike_scaled.bits);
    _strike_scaled.bits = bits;
    _strike_scaled.rows = dh;
    _strike_scaled.width = dw;
    _strike_scaled.pitch = dpitch;
    _strike_scaled.pixel_mode = (char)b->pixel_mode;
    _strike_scaled.left = (int)(f->glyph->bitmap_left * sx
                                + (f->glyph->bitmap_left < 0 ? -0.5 : 0.5));
    _strike_scaled.top = (int)(f->glyph->bitmap_top * sy
                               + (f->glyph->bitmap_top < 0 ? -0.5 : 0.5));
    _strike_scaled.valid = 1;
    return 1;
}
#endif

/* Renders a glyph, or finds it already rendered. What comes back is
   not the bitmap but the fact that one is now being served; the call
   below hands it over. Splitting the two is what lets a cache hit cost
   a lookup. */
int
xpost_font_face_glyph_render(void *face, unsigned int glyph_index)
{
#ifdef HAVE_FREETYPE2
    FT_Error err;
    long m[4], size;
    int keyed;

    _strike_scaled.valid = 0;

    /* a cached raster stands in for the rasterization whole: the key
       carries every input the rasterizer would see, so the replay is
       the same bytes. A face with no recorded transform and size has no
       such key, so it declines the cache -- served fresh from the slot
       below -- rather than being keyed on state nobody installed */
    keyed = gcache_state_get(face, m, &size);
    gcache_serving = keyed ? gcache_find(face, glyph_index, m, size) : NULL;
    if (gcache_serving)
    {
        gcache_bump((Xpost_Glyph_Entry *)gcache_serving);
        return 1;
    }

    err = FT_Load_Glyph(face, glyph_index, FT_LOAD_FORCE_AUTOHINT);
    if (!err)
    {
        if (((FT_Face)face)->glyph->format != FT_GLYPH_FORMAT_BITMAP)
        {
            err = FT_Render_Glyph(((FT_Face)face)->glyph, FT_RENDER_MODE_NORMAL);
            if (err)
            {
                XPOST_LOG_INFO("Can not render  non bitmap glyph (error : %d)", err);
                return 0;
            }
        }
        {
            FT_GlyphSlot slot = ((FT_Face)face)->glyph;
            long ax, ay;

            _glyph_linear_advance((FT_Face)face, &ax, &ay);
            if (!FT_IS_SCALABLE((FT_Face)face) && keyed
             && _strike_resample((FT_Face)face, m, ax, ay))
                gcache_serving = gcache_insert(face, glyph_index, m, size,
                                               _strike_scaled.bits,
                                               _strike_scaled.rows,
                                               _strike_scaled.width,
                                               _strike_scaled.pitch,
                                               _strike_scaled.pixel_mode,
                                               _strike_scaled.left,
                                               _strike_scaled.top,
                                               ax, ay);
            else if (keyed)
                gcache_serving = gcache_insert(face, glyph_index, m, size,
                                               slot->bitmap.buffer,
                                               (int)slot->bitmap.rows,
                                               (int)slot->bitmap.width,
                                               slot->bitmap.pitch,
                                               (char)slot->bitmap.pixel_mode,
                                               slot->bitmap_left,
                                               slot->bitmap_top,
                                               ax, ay);
        }
        return 1;
    }
    else
    {
        XPOST_LOG_INFO("Can not load glyph (error : %d)", err);
        return 0;
    }
#else
    (void)face;
    (void)glyph_index;
    return 0;
#endif
}

/* The glyph the call above rendered or found: its bits, its shape, and
   where it sits against the origin. */
void
xpost_font_face_glyph_buffer_get(void *face, unsigned char **buffer, int *rows, int *width, int *pitch, char *pixel_mode, int *left, int *top, long *advance_x, long *advance_y)
{
#ifdef HAVE_FREETYPE2
    if (gcache_serving)
    {
        *buffer = gcache_serving->bits;
        *rows = gcache_serving->rows;
        *width = gcache_serving->width;
        *pitch = gcache_serving->pitch;
        *pixel_mode = gcache_serving->pixel_mode;
        *left = gcache_serving->left;
        *top = gcache_serving->top;
        *advance_x = gcache_serving->advance_x;
        *advance_y = gcache_serving->advance_y;
        return;
    }
    if (_strike_scaled.valid)
    {
        *buffer = _strike_scaled.bits;
        *rows = _strike_scaled.rows;
        *width = _strike_scaled.width;
        *pitch = _strike_scaled.pitch;
        *pixel_mode = _strike_scaled.pixel_mode;
        *left = _strike_scaled.left;
        *top = _strike_scaled.top;
        _glyph_linear_advance((FT_Face)face, advance_x, advance_y);
        return;
    }
    *buffer = ((FT_Face)face)->glyph->bitmap.buffer;
    *rows = ((FT_Face)face)->glyph->bitmap.rows;
    *width = ((FT_Face)face)->glyph->bitmap.width;
    *pitch = ((FT_Face)face)->glyph->bitmap.pitch;
    *pixel_mode = ((FT_Face)face)->glyph->bitmap.pixel_mode;
    *left = ((FT_Face)face)->glyph->bitmap_left;
    *top = ((FT_Face)face)->glyph->bitmap_top;
    _glyph_linear_advance((FT_Face)face, advance_x, advance_y);
#else
    (void)face;
    (void)buffer;
    (void)rows;
    (void)width;
    (void)pitch;
    (void)pixel_mode;
    (void)left;
    (void)top;
    (void)advance_x;
    (void)advance_y;
#endif
}
