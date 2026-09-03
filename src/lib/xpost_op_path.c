/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_path.c
 * @brief Installs the path-construction operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Path construction is PLRM 4.4: the current path a program builds
 * segment by segment, and the clipping path built from one (4.4.2).
 * Each operator is defined in PLRM 8.2.
 *
 * Installed into systemdict as:
 *
 * newpath moveto rmoveto lineto rlineto curveto rcurveto arc arcn arct
 * arcto closepath currentpoint pathbbox flattenpath
 *
 * A path is built here and painted elsewhere. Curves are kept as curves
 * until something asks for the flattened form.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define _USE_MATH_DEFINES /* needed for M_PI with Visual Studio */
#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_private.h" /* XPOST_REFUSAL_IMPOSSIBLE */
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_dict.h"
#include "xpost_matrix.h"
#include "xpost_save.h"  /* the current path obeys save/restore */

#include "xpost_garbage.h"
#include "xpost_operator.h"
#include "xpost_op_dict.h"
#include "xpost_op_path.h"
#include "xpost_dev_generic.h"  /* the vector devices' content accumulator */

#undef y0
#undef y1

/*
   The current path is a packed byte string held at /currpath in the
   graphics state, avoiding a dictionary and an array allocation per
   path element:

       header (32 bytes):
           u32 used       total bytes in use, including this header
           u32 sp_start   offset of the current subpath's move element
           u32 last_elem  offset of the most recent element
           u32 cap        allocated capacity in bytes
           f32 bbox[4]    running minx miny maxx maxy over every point
                          appended (conservative: it may retain points
                          later overwritten by a move-onto-move merge,
                          and it spans curve control hulls)
       element:
           u8  cmd        0 move, 1 line, 2 curve, 3 close
           f32 coords     one point (move, line); three points (curve);
                          the subpath's start point repeated (close)

   Coordinates are stored in device space, already transformed by the
   CTM. The string object is an opaque handle: its sz field is a
   16-bit word, far too small for a large symbol's path, so the true
   extent lives in the header (entity allocations are not so limited)
   and sz holds a fixed nonzero sentinel. The sentinel keeps clear of
   the reserved sz==0 && ent!=0 encoding that luser-dr00g/xpost#40
   earmarks for 65536-byte strings, so a path can never be mistaken
   for one. PostScript code never takes the handle's length; C code
   reaches the bytes through the entity address alone.
   The allocation is oversized and doubled as needed; growth replaces
   /currpath, so a reference snapshotted before newpath stays intact.
   A move following a move overwrites it in place, so a subpath always
   begins with exactly one move element.

   The string's bytes are written directly, so unlike the rest of VM
   the path contents are not unwound by restore; the path is graphics
   state, reverted by grestore's copy, and no drawing sequence relies
   on restore rebuilding a partly constructed path.
 */

//#define RAD_PER_DEG (M_PI / 180.0)
/* full precision: a truncated literal skewed arc endpoints off the axes */
#define RAD_PER_DEG (M_PI / 180.0)

/*name objects*/
static Xpost_Object namegraphicsdict;
static Xpost_Object namecurrgstate;
static Xpost_Object namecurrpath;
static Xpost_Object nameclipregion;
static Xpost_Object nameflat;
static Xpost_Object namecurrmatrix;

/*opcodes*/
static unsigned int _currentpoint_opcode;
static unsigned int _moveto_opcode;
static unsigned int _rmoveto_cont_opcode;
static unsigned int _lineto_opcode;
static unsigned int _rlineto_cont_opcode;
static unsigned int _curveto_opcode;
static unsigned int _rcurveto_cont_opcode;
static unsigned int _arct_cont_opcode;
static unsigned int _arcto_cont_opcode;

/*matrices*/

#define NUM(x) (xpost_object_get_type(x)==realtype?x.real_.val:(real)x.int_.val)

/* The header holds four u32 fields (extent, flags, last-element offset,
   capacity) and the four bbox reals; the bbox slots are real-sized, so
   the header widens with the build's real type. */
#define PATH_BBOX_OFF 16
#define PATH_BBOX(i) (PATH_BBOX_OFF + (unsigned int)((i) * sizeof(real)))
/* The box a program declares with setbbox, held beside the box the path
   has actually reached. It is kept in device space, fixed as the operator
   is called: PLRM 8.2 says the box holds "for the lifetime of the current
   path", and a path outlives any number of changes to the CTM, so a box
   that followed the CTM would be a different box at every element. An
   unset box is the empty one, which no coordinate can be inside, so the
   two questions "is a box declared" and "is this point in it" are the
   same question. */
#define PATH_SBB_OFF (PATH_BBOX_OFF + (unsigned int)(4 * sizeof(real)))
#define PATH_SBB(i) (PATH_SBB_OFF + (unsigned int)((i) * sizeof(real)))
#define PATH_HDR (PATH_SBB_OFF + (unsigned int)(4 * sizeof(real)))
#define PATH_CMD_MOVE 0
#define PATH_CMD_LINE 1
#define PATH_CMD_CURVE 2
#define PATH_CMD_CLOSE 3

/* The command byte of an element. The path is held in a char string,
   whose signedness is the platform's, so the byte is read as the
   unsigned value it is: every command is a small non-negative number,
   and a byte that is not one of them must compare above the last rather
   than below the first on one platform and above it on another. */
#define PATH_CMD(p, o) ((unsigned char)(p)[(o)])

/* --- how a path is stored --------------------------------------------
   A path is a string, not an array of objects: a run of fixed-size
   elements read and written through these. That is what lets a path of
   tens of thousands of segments cost bytes rather than composite objects,
   and it is why nothing outside this file knows the layout. */

static unsigned int
_path_get_u32(const char *p, unsigned int off)
{
    unsigned int v;
    memcpy(&v, p + off, sizeof v);
    return v;
}

static void
_path_set_u32(char *p, unsigned int off, unsigned int v)
{
    memcpy(p + off, &v, sizeof v);
}

static real
_path_get_f32(const char *p, unsigned int off)
{
    real v;
    memcpy(&v, p + off, sizeof v);
    return v;
}

static void
_path_set_f32(char *p, unsigned int off, real v)
{
    memcpy(p + off, &v, sizeof v);
}

static unsigned int
_path_elem_size(int cmd)
{
    return cmd == PATH_CMD_CURVE ? 1 + 6 * sizeof(real) : 1 + 2 * sizeof(real);
}

static void
_path_get_coords(const char *p, unsigned int elem, real *co, int n)
{
    memcpy(co, p + elem + 1, n * sizeof(real));
}

/* Bytes readable through a path string's data pointer: the backing entity
   less the object's own offset. A path records its extent in its header
   rather than comp_.sz, so an operator handed a program-supplied string
   must bound the header against the allocation, not the object size. */
static unsigned int
_path_avail(Xpost_Context *ctx, Xpost_Object path)
{
    Xpost_Memory_File *mem = xpost_context_select_memory(ctx, path);
    unsigned int ent = xpost_object_get_ent(path);
    unsigned int entsz = mem->table.tab[ent].sz;
    unsigned int off = path.comp_.off;

    return off < entsz ? entsz - off : 0;
}

/* A path string's content extent, taken from offset 0 and bounded to the
   bytes the string itself holds. The current path lives in a
   program-reachable dictionary and can be replaced with a forged string,
   so a reader that trusts the packed layout takes the extent from here
   rather than from the header directly; the element chain then bounds
   each element against that extent as it walks. 0 when the string is too
   short for a header or declares more content than it has. */
static int
_path_extent(Xpost_Context *ctx, Xpost_Object path,
             char **pp, unsigned int *usedp)
{
    unsigned int avail, used;
    char *p;

    avail = _path_avail(ctx, path);
    if (avail < PATH_HDR)
        return 0;
    p = xpost_string_get_pointer(ctx, path);
    used = _path_get_u32(p, 0);
    if (used < PATH_HDR || used > avail)
        return 0;
    *pp = p;
    *usedp = used;
    return 1;
}

/* True when a path string's header and element chain lie within its own
   allocation: the extent fits the string, every element of the chain
   from PATH_HDR fits the extent, and the subpath start at offset 4 and
   the last-element offset at offset 8 each name an element the chain
   reaches. The readers take a command code and its coordinates from
   those two offsets, so an offset naming a coordinate byte reads a
   command the coordinate happens to spell and a width to match.

   The chain is walked from where the last walk over the same entity
   stopped rather than from the header, which rests on every writer of a
   path's elements leaving the boundaries below that point where they
   were. There are three, and they are all in this file.

   _path_append writes its element at the extent and raises the extent
   past it, so it only ever adds boundaries above where a walk reached.

   The move-onto-move merge in _path_append rewrites one element's
   coordinates in place, below the extent, and moves no boundary.

   _retagclose rewrites one command byte in place, below the extent,
   from PATH_CMD_LINE to PATH_CMD_CLOSE. That moves no boundary because
   _path_elem_size gives a move, a line and a close the same width, and
   the offset it writes at is one _path_ok has just accounted for, since
   it reads the path through _cpath. The equal widths are what the
   resumption rests on here: a close made narrower than a line -- which
   is the space there is to save, its coordinates being the subpath
   start repeated -- would move every boundary after it, and the walk
   would have to begin at the header again.

   Every other write of a path's bytes lands on an entity freshly
   allocated by _path_cons -- the growth in _path_append, .copypath and
   .newpathstr -- or on that entity's header, which the walk starts
   past. A fresh entity is a number no walk has a record of, because
   xpost_memory_table_alloc drops the record as it issues the number;
   without that the record would outlive the path it describes, a
   reclaimed number would come back carrying a conclusion about its
   previous tenant, and the offsets in a new path's header would be
   answered from that instead of from the elements actually there.

   Restore points the number at the storage kept from the save, whose
   extent is the one the path had then and so is never longer than the
   one a walk since has reached: the extent test below sends that back
   to the header, and where the two are equal the storage is what was
   walked. A program reaches the header and no further, because the
   string handle the graphics state hands out spans the header alone
   and every writer of a string -- put, putinterval, copy, cvs, the
   readstring family, token's pushback -- is bounded by the handle's
   own length.

   The two offsets carry the same way: one an earlier walk found naming
   an element is naming it still, and one below the resumed point that
   the earlier walk did not find sends this one back to the header. */
static int
_path_ok(Xpost_Context *ctx, Xpost_Object path)
{
    Xpost_Memory_File *mem;
    unsigned int used, last, sps, o, ent;
    char *p;
    int lastok = 0, spsok = 0;

    if (xpost_object_get_type(path) != stringtype)
        return 0;
    if (!_path_extent(ctx, path, &p, &used))
        return 0;
    sps = _path_get_u32(p, 4);
    last = _path_get_u32(p, 8);
    mem = xpost_context_select_memory(ctx, path);
    ent = xpost_object_get_ent(path);
    o = PATH_HDR;
    if (ent != 0 && ent == mem->path_walk.ent && used >= mem->path_walk.end)
    {
        /* an offset the earlier walk reached an element at is still one;
           any other offset below where it stopped is unaccounted for,
           and the whole chain is walked to account for it */
        spsok = sps >= PATH_HDR && sps == mem->path_walk.sps;
        lastok = last >= PATH_HDR && last == mem->path_walk.last;
        if ((sps >= mem->path_walk.end || spsok)
         && (last >= mem->path_walk.end || lastok))
            o = mem->path_walk.end;
        else
            spsok = lastok = 0;
    }
    for (; o < used; )
    {
        unsigned int esz;

        if (PATH_CMD(p, o) > PATH_CMD_CLOSE)
            return 0;
        esz = _path_elem_size(p[o]);
        if (esz > used - o)   /* the element must fit the declared content */
            return 0;
        if (o == sps)
            spsok = 1;
        if (o == last)
            lastok = 1;
        o += esz;
        if (mem->path_walk.steps < (unsigned int)INT_MAX)
            ++mem->path_walk.steps;
    }
    if (!(used == PATH_HDR || (spsok && lastok)))
        return 0;
    /* only an offset a walk reached an element at is recorded as one:
       an empty path is well formed whatever its two offsets say, and
       what they say there was established about nothing */
    mem->path_walk.ent = ent;
    mem->path_walk.end = used;
    mem->path_walk.sps = spsok ? sps : 0;
    mem->path_walk.last = lastok ? last : 0;
    return 1;
}

/* paths always live in local VM: the graphics state dictionary is
   local, and local objects may not be stored into global composites */
static Xpost_Object
_path_cons(Xpost_Context *ctx, unsigned int cap)
{
    Xpost_Object s = { 0 };
    unsigned int ent;
    char *p;

    if (!xpost_memory_table_alloc(ctx->lo, cap, stringtype, &ent))
        return invalid;
    /* stamp the ent with the current save level, as the standard
       composite constructors do, so the save/restore guard in
       _path_append can tell a path predating a save from one created
       inside it */
    xpost_save_stamp_birth(ctx->lo, ent);
    s.tag = stringtype |
        (XPOST_OBJECT_TAG_ACCESS_UNLIMITED << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    /* nonzero sentinel: the extent lives in the header, but sz must
       stay clear of the reserved sz==0 encoding (see the note above).
       The header size is always within the allocation, so a stray
       generic read of sz bytes cannot run past the buffer. */
    s.comp_.sz = PATH_HDR;
    s.comp_.off = 0;
    s = xpost_object_set_ent(s, ent);
    s = xpost_object_cvlit(s);
    p = xpost_string_get_pointer(ctx, s);
    _path_set_u32(p, 0, PATH_HDR);
    _path_set_u32(p, 4, 0);
    _path_set_u32(p, 8, 0);
    _path_set_u32(p, 12, cap);
    _path_set_f32(p, PATH_BBOX(0), FLT_MAX);
    _path_set_f32(p, PATH_BBOX(1), FLT_MAX);
    _path_set_f32(p, PATH_BBOX(2), -FLT_MAX);
    _path_set_f32(p, PATH_BBOX(3), -FLT_MAX);
    _path_set_f32(p, PATH_SBB(0), FLT_MAX);
    _path_set_f32(p, PATH_SBB(1), FLT_MAX);
    _path_set_f32(p, PATH_SBB(2), -FLT_MAX);
    _path_set_f32(p, PATH_SBB(3), -FLT_MAX);
    return s;
}

/* read a path's capacity from its header */
static unsigned int
_path_cap(Xpost_Context *ctx, Xpost_Object path)
{
    return _path_get_u32(xpost_string_get_pointer(ctx, path), 12);
}

/* append an element, growing the string (and re-seating /currpath in
   the graphics state) when full; *pathp is updated in place */
static int
_path_append(Xpost_Context *ctx, Xpost_Object gstate, Xpost_Object *pathp,
             int cmd, const real *co, int ncoords)
{
    char *p;
    unsigned int used, esz, pent;

    /* the current path is part of the graphics state, which save
       snapshots and restore rewinds. Back up the path storage on its
       first mutation after a save so restore reverts it: one save
       record per save level, not per element. A grow allocates a fresh
       ent whose /currpath slot is separately save-protected, so this
       only matters for in-place appends and the move-onto-move merge.
       save_save_ent may move the memory file, so *pathp's pointer is
       derived afterwards. */
    pent = xpost_object_get_ent(*pathp);
    if (xpost_save_cow(ctx->lo, stringtype, 0, pent))
        return VMerror;

    p = xpost_string_get_pointer(ctx, *pathp);
    used = _path_get_u32(p, 0);
    esz = 1 + ncoords * sizeof(real);
    /* the path lives in a program-reachable dictionary; reject a forged
       header before it drives an out-of-bounds append or a grow loop that
       never terminates (a zero capacity doubles to itself) */
    {
        unsigned int avail = _path_avail(ctx, *pathp);
        unsigned int cap;

        if (avail < PATH_HDR)
            return rangecheck;
        cap = _path_get_u32(p, 12);
        if (used < PATH_HDR || used > cap || cap > avail)
            return rangecheck;
    }

    /* merge a move into an immediately preceding move */
    if (cmd == PATH_CMD_MOVE && used > PATH_HDR)
    {
        unsigned int last = _path_get_u32(p, 8);
        if (last >= PATH_HDR && last <= used - esz && p[last] == PATH_CMD_MOVE)
        {
            memcpy(p + last + 1, co, ncoords * sizeof(real));
            return 0;
        }
    }

    if (used + esz > _path_get_u32(p, 12))
    {
        Xpost_Object ns;
        unsigned int newcap;
        char *np;
        int ret;

        /* The path extent lives in a 32-bit header. A curve whose
           flattening needs more than that -- a huge control polygon
           subdivided to the flatness tolerance can ask for it -- cannot
           be represented, and the capacity-doubling below would wrap past
           2^32 and allocate a buffer smaller than the data already in
           hand, which the copy then overruns. Refuse at the limit rather
           than overrun. */
        if (esz > 0xFFFFFFFFu - used)
            return limitcheck;
        newcap = _path_get_u32(p, 12);
        while (newcap < used + esz)
        {
            if (newcap > 0xFFFFFFFFu / 2)
                return limitcheck;
            newcap *= 2;
        }
        ns = _path_cons(ctx, newcap);
        if (xpost_object_get_type(ns) == invalidtype)
            return VMerror;
        np = xpost_string_get_pointer(ctx, ns);
        p = xpost_string_get_pointer(ctx, *pathp); /* re-derive: cons may move the file */
        memcpy(np, p, used);
        _path_set_u32(np, 12, newcap); /* the copied header overwrote cap */
        ret = xpost_dict_put(ctx, gstate, namecurrpath, ns);
        if (ret)
            return ret;
        *pathp = ns;
        p = xpost_string_get_pointer(ctx, ns);
    }

    /* Every element of every path arrives here, which is where a declared
       box is held against: PLRM 8.2 makes "any subsequent attempt to append
       a path element with a coordinate lying outside the bounding box" a
       rangecheck. Asked before anything is written, so a refused element
       leaves the path as it was. */
    {
        real sx0 = _path_get_f32(p, PATH_SBB(0));
        real sx1 = _path_get_f32(p, PATH_SBB(2));

        if (sx0 <= sx1)
        {
            real sy0 = _path_get_f32(p, PATH_SBB(1));
            real sy1 = _path_get_f32(p, PATH_SBB(3));
            int k;

            for (k = 0; k + 1 < ncoords; k += 2)
                if (co[k] < sx0 || co[k] > sx1
                 || co[k + 1] < sy0 || co[k + 1] > sy1)
                    return rangecheck;
        }
    }

    p[used] = (char)cmd;
    memcpy(p + used + 1, co, ncoords * sizeof(real));
    if (cmd == PATH_CMD_MOVE)
        _path_set_u32(p, 4, used);
    _path_set_u32(p, 8, used);
    _path_set_u32(p, 0, used + esz);
    {
        int k;
        for (k = 0; k + 1 < ncoords; k += 2)
        {
            if (co[k] < _path_get_f32(p, PATH_BBOX(0))) _path_set_f32(p, PATH_BBOX(0), co[k]);
            if (co[k + 1] < _path_get_f32(p, PATH_BBOX(1))) _path_set_f32(p, PATH_BBOX(1), co[k + 1]);
            if (co[k] > _path_get_f32(p, PATH_BBOX(2))) _path_set_f32(p, PATH_BBOX(2), co[k]);
            if (co[k + 1] > _path_get_f32(p, PATH_BBOX(3))) _path_set_f32(p, PATH_BBOX(3), co[k + 1]);
        }
    }
    return 0;
}

/* A context's currgstate is created once when its graphics state is set
   up and is only ever mutated in place (setgstate, grestore and
   .gstatecopy copy into it, never rebind it), so the resolved dictionary
   can be cached after the first lookup instead of searching the
   dictionary stack on every path operator.

   The cache is per context, not per interpreter. Under Display
   PostScript each forked context is given its OWN graphics state (fork
   copies the graphics state stack, PLRM 2nd ed 7.1), so there is a
   distinct currgstate for each, and a single cached dictionary would
   hand one context the graphics state -- and so the current path -- of
   another. The cache therefore records which context it belongs to and
   is used only for that one; a default single-context run keeps the same
   id throughout and so keeps the cache hot, while a context switch makes
   the next path operator re-resolve for whichever context now runs. The
   id is the context's unique identifier, which a later context never
   reuses, so a table slot reused by a new context does not read a stale
   entry. */
static Xpost_Object _gstate_cache;
static int _gstate_cached = 0;
static unsigned int _gstate_cache_id = 0;

/* --- building a path -------------------------------------------------
   The construction operators the language exposes. Each appends one
   element and leaves the current point where the specification says; the
   relative forms reach the absolute ones after asking where the pen is. */

static
Xpost_Object _gstate(Xpost_Context *ctx)
{
    Xpost_Object gd, gs;

    if (_gstate_cached && _gstate_cache_id == ctx->id)
        return _gstate_cache;
    gd = ctx->graphicsdict;
    if (xpost_object_get_type(gd) != dicttype)
        return invalid;
    gs = xpost_dict_get(ctx, gd, namecurrgstate);
    if (xpost_object_get_type(gs) == dicttype)
    {
        _gstate_cache = gs;
        _gstate_cached = 1;
        _gstate_cache_id = ctx->id;
    }
    return gs;
}

/* read the CTM's six coefficients, promoting integer entries,
   with arithmetic identical to the transform operator's */
static
int _path_ctm(Xpost_Context *ctx, Xpost_Object gstate, real *m)
{
    Xpost_Object psm;
    Xpost_Object arr[6];
    int i;

    psm = xpost_dict_get(ctx, gstate, namecurrmatrix);
    if (xpost_object_get_type(psm) != arraytype)
        return undefined;
    /* the bulk read takes six objects at once, so a shorter matrix in the
       graphics state would run off the end of its storage */
    if (!xpost_memory_get(xpost_context_select_memory(ctx, psm),
                          xpost_object_get_ent(psm), 0, sizeof arr, arr))
        return rangecheck;
    for (i = 0; i < 6; i++)
        m[i] = xpost_object_get_type(arr[i]) == integertype
             ? (real)arr[i].int_.val : arr[i].real_.val;
    return 0;
}

static
int _newpath(Xpost_Context *ctx)
{
    Xpost_Object gstate, path;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    path = _path_cons(ctx, 256);
    if (xpost_object_get_type(path) == invalidtype)
        return VMerror;
    return xpost_dict_put(ctx, gstate, namecurrpath, path);
}

/* return a fresh empty path string (for graphics-state templates) */
static
int _newpathstr(Xpost_Context *ctx)
{
    Xpost_Object path;

    path = _path_cons(ctx, 256);
    if (xpost_object_get_type(path) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, path);
    return 0;
}

/* The current path, reached the way the language reaches it and
   checked before it is handed back. A path is bytes, and everything
   below reads those bytes trusting the layout, so a current path a
   program has replaced with a string of its own is answered as invalid
   here rather than being decoded. */
static
Xpost_Object _cpath(Xpost_Context *ctx)
{
    Xpost_Object gd, gstate, path;

    /* graphicsdict /currgstate get /currpath get */
    gd = ctx->graphicsdict;
    if (xpost_object_get_type(gd) != dicttype)
        return invalid;
    gstate = xpost_dict_get(ctx, gd, namecurrgstate);
    if (xpost_object_get_type(gstate) == invalidtype)
        return invalid;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    /* the readers below trust the packed layout, so a forged current path
       must not reach them */
    if (xpost_object_get_type(path) == stringtype && !_path_ok(ctx, path))
        return invalid;
    return path;
}

int _currentpoint(Xpost_Context *ctx)
{
    Xpost_Object path;
    char *p;
    unsigned int used, last;
    real co[6];
    int cmd, n;

    path = _cpath(ctx);
    if (xpost_object_get_type(path) != stringtype)
        return nocurrentpoint;
    p = xpost_string_get_pointer(ctx, path);
    used = _path_get_u32(p, 0);
    if (used <= PATH_HDR)
        return nocurrentpoint;
    last = _path_get_u32(p, 8);
    cmd = p[last];
    n = cmd == PATH_CMD_CURVE ? 6 : 2;
    _path_get_coords(p, last, co, n);
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(co[n - 2]));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(co[n - 1]));
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, itransform));

    return 0;
}

/* -  .pathbox  llx lly urx ury
   The declared box in device space, as the header holds it -- the empty
   box where none is declared, which is what the header holds for one.
   Paired with .setpathbox so a user path can be given its own box and the
   one before it put back: PLRM 8.2 says the box a user path declares
   "applies only to elements of the user path itself". */
static
int _pathboxget(Xpost_Context *ctx)
{
    Xpost_Object path;
    char *p;
    unsigned int used;
    int i;

    path = _cpath(ctx);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;
    for (i = 0; i < 4; i++)
        xpost_stack_push(ctx->lo, ctx->os,
                         xpost_real_cons(_path_get_f32(p, PATH_SBB(i))));
    return 0;
}

/* llx lly urx ury  .setpathbox  -
   put a device-space box back, exactly as .pathbox gave it up. An empty
   box means no box, so this is also how one is cleared. */
static
int _pathboxput(Xpost_Context *ctx, Xpost_Object llx, Xpost_Object lly,
                Xpost_Object urx, Xpost_Object ury)
{
    Xpost_Object path;
    char *p;
    unsigned int used;

    path = _cpath(ctx);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;
    _path_set_f32(p, PATH_SBB(0), llx.real_.val);
    _path_set_f32(p, PATH_SBB(1), lly.real_.val);
    _path_set_f32(p, PATH_SBB(2), urx.real_.val);
    _path_set_f32(p, PATH_SBB(3), ury.real_.val);
    return 0;
}

/* llx lly urx ury  setbbox  -
   declare the box the current path is to stay inside (PLRM 8.2). The
   corners are given in user space and kept in device space, the box being
   fixed as this runs rather than following the CTM: it holds "for the
   lifetime of the current path", and a path outlives any number of changes
   to the CTM. Under a rotation the four transformed corners no longer make
   a rectangle, so what is kept is the box that contains them. */
static
int _setbbox(Xpost_Context *ctx, Xpost_Object llx, Xpost_Object lly,
             Xpost_Object urx, Xpost_Object ury)
{
    Xpost_Object gstate, path;
    real m[6], ux[4], uy[4], dx, dy;
    real x0, y0, x1, y1;
    char *p;
    unsigned int used;
    int i, ret;

    /* PLRM 8.2: "the upper-right coordinate values must be greater than or
       equal to the lower-left values, or a rangecheck error will occur" --
       asked in the space the program stated them in, which is the space it
       wrote them down in */
    if (urx.real_.val < llx.real_.val || ury.real_.val < lly.real_.val)
        return rangecheck;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    ret = _path_ctm(ctx, gstate, m);
    if (ret)
        return ret;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;

    ux[0] = llx.real_.val; uy[0] = lly.real_.val;
    ux[1] = urx.real_.val; uy[1] = lly.real_.val;
    ux[2] = urx.real_.val; uy[2] = ury.real_.val;
    ux[3] = llx.real_.val; uy[3] = ury.real_.val;
    x0 = y0 = FLT_MAX;
    x1 = y1 = -FLT_MAX;
    for (i = 0; i < 4; i++)
    {
        dx = m[0] * ux[i] + m[2] * uy[i] + m[4];
        dy = m[1] * ux[i] + m[3] * uy[i] + m[5];
        if (dx < x0) x0 = dx;
        if (dy < y0) y0 = dy;
        if (dx > x1) x1 = dx;
        if (dy > y1) y1 = dy;
    }
    _path_set_f32(p, PATH_SBB(0), x0);
    _path_set_f32(p, PATH_SBB(1), y0);
    _path_set_f32(p, PATH_SBB(2), x1);
    _path_set_f32(p, PATH_SBB(3), y1);
    return 0;
}

static
int _moveto(Xpost_Context *ctx, Xpost_Object x, Xpost_Object y)
{
    Xpost_Object gstate, path;
    real m[6];
    real co[2];
    int ret;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    ret = _path_ctm(ctx, gstate, m);
    if (ret)
        return ret;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    co[0] = m[0] * x.real_.val + m[2] * y.real_.val + m[4];
    co[1] = m[1] * x.real_.val + m[3] * y.real_.val + m[5];
    return _path_append(ctx, gstate, &path, PATH_CMD_MOVE, co, 2);
}

/* The relative constructors, each of which needs the current point
   before it can do anything. Rather than read the point here, the
   operator puts its own continuation and currentpoint on the execution
   stack and returns: currentpoint runs, leaves the point on the
   operand stack, and the continuation is then called with the
   displacement and the point together. Every _cont below is the second
   half of one of these, and takes the arguments in that order. */
static
int _rmoveto(Xpost_Context *ctx, Xpost_Object dx, Xpost_Object dy)
{
    xpost_stack_push(ctx->lo, ctx->os, dx);
    xpost_stack_push(ctx->lo, ctx->os, dy);
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_rmoveto_cont_opcode));
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_currentpoint_opcode));
    return 0;
}

static
int _rmoveto_cont(Xpost_Context *ctx,
                  Xpost_Object dx, Xpost_Object dy,
                  Xpost_Object x, Xpost_Object y)
{
    x.real_.val += dx.real_.val;
    y.real_.val += dy.real_.val;
    return _moveto(ctx, x, y);
}

static
int _lineto(Xpost_Context *ctx, Xpost_Object x, Xpost_Object y)
{
    Xpost_Object gstate, path;
    char *p;
    unsigned int used;
    real m[6];
    real co[2];
    int ret;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    ret = _path_ctm(ctx, gstate, m);
    if (ret)
        return ret;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;
    if (used <= PATH_HDR)
        return nocurrentpoint;
    co[0] = m[0] * x.real_.val + m[2] * y.real_.val + m[4];
    co[1] = m[1] * x.real_.val + m[3] * y.real_.val + m[5];
    return _path_append(ctx, gstate, &path, PATH_CMD_LINE, co, 2);
}

static
int _rlineto(Xpost_Context *ctx, Xpost_Object dx, Xpost_Object dy)
{
    xpost_stack_push(ctx->lo, ctx->os, dx);
    xpost_stack_push(ctx->lo, ctx->os, dy);
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_rlineto_cont_opcode));
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_currentpoint_opcode));
    return 0;
}

static
int _rlineto_cont(Xpost_Context *ctx,
                  Xpost_Object dx, Xpost_Object dy,
                  Xpost_Object x, Xpost_Object y)
{
    x.real_.val += dx.real_.val;
    y.real_.val += dy.real_.val;
    return _lineto(ctx, x, y);
}

static
int _curveto(Xpost_Context *ctx,
             Xpost_Object x1, Xpost_Object y1,
             Xpost_Object x2, Xpost_Object y2,
             Xpost_Object x3, Xpost_Object y3)
{
    Xpost_Object gstate, path;
    char *p;
    unsigned int used;
    real m[6];
    real co[6];
    int ret;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    ret = _path_ctm(ctx, gstate, m);
    if (ret)
        return ret;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;
    if (used <= PATH_HDR)
        return nocurrentpoint;
    co[0] = m[0] * x1.real_.val + m[2] * y1.real_.val + m[4];
    co[1] = m[1] * x1.real_.val + m[3] * y1.real_.val + m[5];
    co[2] = m[0] * x2.real_.val + m[2] * y2.real_.val + m[4];
    co[3] = m[1] * x2.real_.val + m[3] * y2.real_.val + m[5];
    co[4] = m[0] * x3.real_.val + m[2] * y3.real_.val + m[4];
    co[5] = m[1] * x3.real_.val + m[3] * y3.real_.val + m[5];
    return _path_append(ctx, gstate, &path, PATH_CMD_CURVE, co, 6);
}

static
int _rcurveto(Xpost_Context *ctx,
              Xpost_Object x1, Xpost_Object y1,
              Xpost_Object x2, Xpost_Object y2,
              Xpost_Object x3, Xpost_Object y3)
{
    xpost_stack_push(ctx->lo, ctx->os, x1);
    xpost_stack_push(ctx->lo, ctx->os, y1);
    xpost_stack_push(ctx->lo, ctx->os, x2);
    xpost_stack_push(ctx->lo, ctx->os, y2);
    xpost_stack_push(ctx->lo, ctx->os, x3);
    xpost_stack_push(ctx->lo, ctx->os, y3);
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_rcurveto_cont_opcode));
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_currentpoint_opcode));
    return 0;
}

static
int _rcurveto_cont(Xpost_Context *ctx,
                   Xpost_Object x1, Xpost_Object y1,
                   Xpost_Object x2, Xpost_Object y2,
                   Xpost_Object x3, Xpost_Object y3,
                   Xpost_Object x, Xpost_Object y)
{
    x1.real_.val += x.real_.val;
    y1.real_.val += y.real_.val;
    x2.real_.val += x.real_.val;
    y2.real_.val += y.real_.val;
    x3.real_.val += x.real_.val;
    y3.real_.val += y.real_.val;
    return _curveto(ctx, x1, y1, x2, y2, x3, y3);
}

/* --- asking a path about itself --------------------------------------
   What the path is, rather than what to add to it: its extent, whether it
   is a rectangle, whether it is empty. The rectangle question is asked
   often enough to be worth answering directly -- a rectangular clip is the
   common case, and knowing it is one is what lets the clip be a pair of
   numbers instead of a shape. */

/* walk a packed path accumulating the bounding box of every stored
   coordinate pair (curve controls and close repeats included, matching
   the behaviour of the dictionary-walking predecessors); a moveto that
   ends the path is disregarded (PLRM pathbbox: a trailing moveto marks
   only a pending current point, as after charpath advances it, and is
   not part of the box); when inv is non-NULL it is an affine matrix
   (PostScript [a b c d tx ty] layout) applied to each stored point
   before accumulation; returns 0 on a malformed path or when a curve
   is present and curves are not accepted, 2 on an empty path */
static
int _path_walk_bbox(Xpost_Context *ctx, Xpost_Object path,
                    int accept_curves, const real *inv,
                    real *minx, real *miny, real *maxx, real *maxy)
{
    char *p;
    unsigned int used, o;
    int any = 0;

    if (xpost_object_get_type(path) != stringtype)
        return 0;
    p = xpost_string_get_pointer(ctx, path);
    used = _path_get_u32(p, 0);
    for (o = PATH_HDR; o < used; o += _path_elem_size(p[o]))
    {
        int cmd = p[o];
        int n, k;
        real co[6];

        if (cmd < PATH_CMD_MOVE || cmd > PATH_CMD_CLOSE)
            return 0;
        if (!accept_curves && cmd == PATH_CMD_CURVE)
            return 0;
        if (cmd == PATH_CMD_MOVE && o + _path_elem_size(cmd) >= used && any)
            continue; /* trailing moveto: pending point, not box (but a
                         path holding only a moveto is that one point) */
        n = cmd == PATH_CMD_CURVE ? 6 : 2;
        _path_get_coords(p, o, co, n);
        for (k = 0; k + 1 < n; k += 2)
        {
            real x = co[k], y = co[k + 1];
            if (inv)
            {
                real x_ = inv[0] * x + inv[2] * y + inv[4];
                y = inv[1] * x + inv[3] * y + inv[5];
                x = x_;
            }
            if (!any)
            {
                *minx = *maxx = x;
                *miny = *maxy = y;
                any = 1;
            }
            else
            {
                if (x < *minx) *minx = x;
                if (x > *maxx) *maxx = x;
                if (y < *miny) *miny = y;
                if (y > *maxy) *maxy = y;
            }
        }
    }
    return any ? 1 : 2;
}

/* Test whether a path is a single closed axis-aligned rectangle and
   return its bounds: 1 when it is one, 0 when it is not, -1 when the
   string does not hold a path at all -- its extent or one of its
   elements reaching past the bytes it has. The walk stops at the second
   subpath, so a region of many rectangles costs the first few elements
   and not the whole chain; the bounds are applied as it goes rather
   than by a validating pass over the chain ahead of it. */
static
int _path_is_rect(Xpost_Context *ctx, Xpost_Object path,
                  real *minx, real *miny, real *maxx, real *maxy)
{
    char *p;
    unsigned int used, o, esz;
    real px[5], py[5];
    int npts = 0;
    int j;

    if (xpost_object_get_type(path) != stringtype)
        return 0;
    if (!_path_extent(ctx, path, &p, &used))
        return -1;
    for (o = PATH_HDR; o < used; o += esz)
    {
        int cmd = p[o];
        real co[2];

        esz = _path_elem_size(cmd);
        if (esz > used - o)
            return -1;
        if (cmd == PATH_CMD_CLOSE)
            continue; /* close repeats the start point */
        if (o == PATH_HDR)
        {
            if (cmd != PATH_CMD_MOVE)
                return 0;
        }
        else if (cmd != PATH_CMD_LINE)
            return 0; /* second subpath or curve */
        if (npts >= 5)
            return 0;
        _path_get_coords(p, o, co, 2);
        px[npts] = co[0];
        py[npts] = co[1];
        npts++;
    }
    /* an explicitly repeated start point is equivalent to closure */
    if (npts == 5)
    {
        if (px[4] != px[0] || py[4] != py[0])
            return 0;
        npts = 4;
    }
    if (npts != 4)
        return 0;
    /* every side, including the closing one, must be axis-parallel */
    for (j = 0; j < 4; j++)
    {
        int n = (j + 1) & 3;
        if (px[j] != px[n] && py[j] != py[n])
            return 0;
    }
    *minx = *maxx = px[0];
    *miny = *maxy = py[0];
    for (j = 1; j < 4; j++)
    {
        if (px[j] < *minx) *minx = px[j];
        if (px[j] > *maxx) *maxx = px[j];
        if (py[j] < *miny) *miny = py[j];
        if (py[j] > *maxy) *maxy = py[j];
    }
    return *maxx > *minx && *maxy > *miny;
}

/* --- handing a path to something that paints it ----------------------
   The points a fill needs, the arguments the compiled polygon filler
   takes, and the walks that put a path out to a vector writer as its own
   path operators. */

/* Allocate an array the caller fills through the pointer it is handed,
   without the per-put save checks of the ordinary constructor.

   Filled with null all the same, and not left as it was allocated. What
   a caller writes is what it turns out to need, which is not always
   every slot -- a run of points it decides to drop leaves the slots it
   would have taken untouched -- and an allocation here can be the one
   that makes a collection due, with an array filed a moment earlier
   still on a stack. The collector walks what an array's entity holds,
   so a slot never written is a tag it reads and an entity number it may
   follow. The fill is a run through the payload rather than a put
   apiece, so what the ordinary constructor costs here is the save
   check, not the writing. */
static Xpost_Object
_rawarray_cons(Xpost_Context *ctx, unsigned int sz, Xpost_Object **payload)
{
    Xpost_Memory_File *mem = ctx->lo;
    unsigned int ent;
    unsigned int i;
    Xpost_Object *slots;
    Xpost_Object o = { 0 };

    if (!xpost_memory_table_alloc(mem, sz * sizeof(Xpost_Object), arraytype, &ent))
        return invalid;
    /* stamp as saved at the current level, as the constructor would */
    xpost_save_stamp_birth(mem, ent);
    o.tag = arraytype |
        (XPOST_OBJECT_TAG_ACCESS_UNLIMITED << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    o.comp_.sz = sz;
    o.comp_.off = 0;
    o = xpost_object_set_ent(o, ent);
    o = xpost_object_cvlit(o);
    slots = xpost_ent_ptr(mem, ent);
    for (i = 0; i < sz; i++)
        slots[i] = null;
    *payload = slots;
    return o;
}

/* A path's fill vertices as a run of coordinates: two reals per point,
   a subpath's own first point repeated where it closes, and a break --
   XPOST_PATH_BREAK in both coordinates -- after each subpath. Subpaths
   of fewer than three points cannot enclose area and are dropped.
 *
 * These are the vertices .fillpolyargs below states as a PostScript
 * array, in the same order and by the same subpath rule, and the two
 * are read by the same scan conversion. What differs is what holds
 * them: an array's length is a bounded field, so a path of enough
 * points has no array form, and a run of coordinates has no such
 * bound. The clipping region is read this way, since a region cut from
 * another is one pixel-band rectangle per band it covers and a page
 * divided finely across the rows has more of those than one array
 * describes.
 *
 * The caller owns the returned buffer, and is given none when the path
 * has no points. 0 on success. */
int xpost_path_fill_points(Xpost_Context *ctx, Xpost_Object path,
                           real **out, int *nout)
{
    char *p;
    unsigned int used, o;
    unsigned int npts = 0, nsp = 0;
    unsigned int n, spn;
    real *buf;
    int cmd, nc, k;
    real co[6];

    *out = NULL;
    *nout = 0;
    /* the region lives in a program-reachable dictionary and can be
       replaced with a forged string, so the packed layout is checked
       before it is walked */
    if (!_path_ok(ctx, path))
        return unregistered;
    p = xpost_string_get_pointer(ctx, path);
    used = _path_get_u32(p, 0);

    for (o = PATH_HDR; o < used; o += _path_elem_size(p[o]))
    {
        cmd = p[o];
        if (cmd == PATH_CMD_MOVE)
            nsp++;
        if (cmd == PATH_CMD_CLOSE)
            npts += npts ? 1 : 0;
        else
            npts += cmd == PATH_CMD_CURVE ? 3 : 1;
    }
    if (npts == 0)
        return 0;

    buf = malloc((size_t)(npts + nsp) * 2 * sizeof *buf);
    if (!buf)
        return VMerror;

    /* n counts points and breaks written; spn is where the subpath in
       hand began, so n - spn is how many points it has so far and an
       area-less one rolls back to it */
    n = spn = 0;
    for (o = PATH_HDR; o < used; o += _path_elem_size(p[o]))
    {
        cmd = p[o];
        if (cmd == PATH_CMD_MOVE && n > spn)
        {
            if (n - spn >= 3)
            {
                buf[2 * n] = XPOST_PATH_BREAK;
                buf[2 * n + 1] = XPOST_PATH_BREAK;
                n++;
            }
            else
                n = spn;
            spn = n;
        }
        if (cmd == PATH_CMD_CLOSE)
        {
            if (n > spn)
            {
                buf[2 * n] = buf[2 * spn];
                buf[2 * n + 1] = buf[2 * spn + 1];
                n++;
            }
            continue;
        }
        nc = cmd == PATH_CMD_CURVE ? 6 : 2;
        _path_get_coords(p, o, co, nc);
        for (k = 0; k + 1 < nc; k += 2)
        {
            buf[2 * n] = co[k];
            buf[2 * n + 1] = co[k + 1];
            n++;
        }
    }
    if (n > spn)
    {
        if (n - spn >= 3)
        {
            buf[2 * n] = XPOST_PATH_BREAK;
            buf[2 * n + 1] = XPOST_PATH_BREAK;
            n++;
        }
        else
            n = spn;
    }

    *out = buf;
    *nout = (int)n;
    return 0;
}

/* build the polygon argument for the device FillPoly procedure: a flat
   array of [x y] point pairs with subpaths separated (and terminated)
   by null. All pairs are two-object views into one backing array, so
   the whole argument costs two allocations. A close element repeats
   the subpath's first point. Subpaths of fewer than three points
   cannot enclose area and are dropped. */
static
int _fillpolyargs(Xpost_Context *ctx)
{
    Xpost_Object path, backing, result;
    Xpost_Object *bk, *rs;
    char *p;
    unsigned int used, o;
    unsigned int npts = 0, nsp = 0;
    unsigned int bi, ri, spbi, spri;
    int cmd, n, k;
    real co[6];

    path = _cpath(ctx);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    p = xpost_string_get_pointer(ctx, path);
    used = _path_get_u32(p, 0);

    /* first pass: count every point and subpath; area-less subpaths
       are dropped in the second pass after their points are written,
       so the buffers are sized before the drop rule is applied */
    for (o = PATH_HDR; o < used; o += _path_elem_size(p[o]))
    {
        cmd = p[o];
        if (cmd == PATH_CMD_MOVE)
            nsp++;
        if (cmd == PATH_CMD_CLOSE)
            npts += npts ? 1 : 0;
        else
            npts += cmd == PATH_CMD_CURVE ? 3 : 1;
    }

    if (npts == 0)
    {
        result = xpost_object_cvlit(xpost_array_cons(ctx, 0));
        xpost_stack_push(ctx->lo, ctx->os, result);
        return 0;
    }
    /* the length one array is allowed to reach: a deliberate bound
       rather than the size field's own ceiling, which differs between
       the ordinary and the large-object build. A path outrunning it is
       still readable through xpost_path_fill_points above, which holds
       its vertices outside VM */
    if (2 * npts > 65535)
        return limitcheck;

    backing = _rawarray_cons(ctx, 2 * npts, &bk);
    if (xpost_object_get_type(backing) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->hold, backing);
    result = _rawarray_cons(ctx, npts + nsp, &rs);
    if (xpost_object_get_type(result) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->hold, result);
    /* allocations can move the memory file */
    p = xpost_string_get_pointer(ctx, path);
    {
        unsigned int adr;
        /* backing's entity was allocated in this same memory file two
           statements above and the allocation was checked; an entity
           number stays within the table once the table has reached it */
        XPOST_REFUSAL_IMPOSSIBLE(
            xpost_memory_table_get_addr(ctx->lo,
                                        xpost_object_get_ent(backing), &adr));
        bk = (Xpost_Object *)xpost_vm_ptr(ctx->lo, adr);
    }

    /* second pass: fill the backing coordinates and the pair views,
       rolling an area-less subpath back where it ended */
    bi = ri = spbi = spri = 0;
    for (o = PATH_HDR; o < used; o += _path_elem_size(p[o]))
    {
        cmd = p[o];
        if (cmd == PATH_CMD_MOVE && bi > spbi)
        {
            if (bi - spbi >= 6) rs[ri++] = null;
            else { bi = spbi; ri = spri; }
            spbi = bi;
            spri = ri;
        }
        if (cmd == PATH_CMD_CLOSE)
        {
            if (bi > spbi)
            {
                Xpost_Object v = backing;
                v.comp_.off = spbi;
                v.comp_.sz = 2;
                rs[ri++] = v;
                /* keep the backing fully initialised and count the
                   repeat toward the size rule */
                bk[bi] = bk[spbi];
                bk[bi + 1] = bk[spbi + 1];
                bi += 2;
            }
            continue;
        }
        n = cmd == PATH_CMD_CURVE ? 6 : 2;
        _path_get_coords(p, o, co, n);
        for (k = 0; k + 1 < n; k += 2)
        {
            Xpost_Object v = backing;
            bk[bi] = xpost_real_cons(co[k]);
            bk[bi + 1] = xpost_real_cons(co[k + 1]);
            v.comp_.off = bi;
            v.comp_.sz = 2;
            rs[ri++] = v;
            bi += 2;
        }
    }
    if (bi > spbi)
    {
        if (bi - spbi >= 6) rs[ri++] = null;
        else ri = spri;
    }

    result.comp_.sz = ri; /* dropped subpaths shrink the view */
    xpost_stack_push(ctx->lo, ctx->os, result);
    return 0;
}

/* How many times the question below has been answered no: the clip may
   cut what is about to be painted, so what reaches the device is the
   shape resolved against the region rather than the shape.
 *
 * It is counted because a caller keeping marks to paint again elsewhere
 * has to know whether they were resolved: a shape cut to a region is
 * resolved into whole pixel rows, and rows carried a fraction of a pixel
 * are not the rows the shape covers there. A count rather than a flag,
 * so that a caller reads it before and after the painting it is asking
 * about and no one has to clear it.
 */
static unsigned int _clipcuts;

/* -  .clipcuts  int
   How many paintings so far may have been cut by a clip region. */
static
int _clipcutsop(Xpost_Context *ctx)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)_clipcuts));
    return 0;
}

/* The trivial-accept question, answered without recording it. The count
   above is the record of a painting that may have been cut, so it is
   moved by the operator below and not by this: a caller that asks in
   order to decide whether to paint at all has cut nothing by asking,
   and one that hands the painting back to the general route leaves that
   route to ask again and to count once, as it does for every fill. */
static
int _cliptrivial_accept(Xpost_Context *ctx, int *acceptp)
{
    Xpost_Object gstate, path, clipregion;
    real cminx, cminy, cmaxx, cmaxy;
    real pminx, pminy, pmaxx, pmaxy;
    int accept = 0;
    int rect;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    clipregion = xpost_dict_get(ctx, gstate, nameclipregion);

    rect = _path_is_rect(ctx, clipregion, &cminx, &cminy, &cmaxx, &cmaxy);
    if (rect < 0)
        return rangecheck;
    if (xpost_object_get_type(path) == stringtype && rect)
    {
        /* the header maintains a conservative bounding box (curve
           control hulls contain their curves); an empty path is
           accepted, there being nothing to clip */
        char *p;
        unsigned int used;

        if (!_path_extent(ctx, path, &p, &used))
            return rangecheck;
        pminx = _path_get_f32(p, PATH_BBOX(0));
        pminy = _path_get_f32(p, PATH_BBOX(1));
        pmaxx = _path_get_f32(p, PATH_BBOX(2));
        pmaxy = _path_get_f32(p, PATH_BBOX(3));
        accept = used <= PATH_HDR ||
                 (pminx >= cminx && pmaxx <= cmaxx &&
                  pminy >= cminy && pmaxy <= cmaxy);
    }

    *acceptp = accept;
    return 0;
}

/* clip trivial-accept test.
   Push true when the clip region is an axis-aligned rectangle and the
   current path lies entirely inside it: clipping the path against such
   a region passes every point through unchanged, so the caller can skip
   the polygon-clipping machinery. Push false in every uncertain case. */
static
int _cliptrivial(Xpost_Context *ctx)
{
    int accept;
    int ret;

    ret = _cliptrivial_accept(ctx, &accept);
    if (ret)
        return ret;
    if (!accept)
        _clipcuts++;
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(accept));
    return 0;
}

/* Whether a segment meets a closed axis-aligned box, exactly. The box
   and the segment are both convex, so they miss only if a line
   separates them, and only four lines can be that separator: the box's
   own two axes, and the segment's own. The first two are the extent
   tests, the third is the sign of the cross product at each corner --
   corners on both sides of the segment's line, or one on it, and the
   two meet. */
static
int _seg_meets_box(real x0, real y0, real x1, real y1,
                   real bx0, real by0, real bx1, real by1)
{
    real bx[4], by[4];
    real dx, dy;
    int above = 0, below = 0;
    int k;

    if ((x0 < bx0 && x1 < bx0) || (x0 > bx1 && x1 > bx1))
        return 0;
    if ((y0 < by0 && y1 < by0) || (y0 > by1 && y1 > by1))
        return 0;
    dx = x1 - x0;
    dy = y1 - y0;
    bx[0] = bx0; by[0] = by0;
    bx[1] = bx1; by[1] = by0;
    bx[2] = bx1; by[2] = by1;
    bx[3] = bx0; by[3] = by1;
    for (k = 0; k < 4; k++)
    {
        real s = dx * (by[k] - y0) - dy * (bx[k] - x0);

        if (s > 0) above = 1;
        else if (s < 0) below = 1;
        else return 1;      /* a corner on the line, within both extents */
        if (above && below)
            return 1;
    }
    return 0;
}

/* The winding a segment contributes about a point, by the crossings of
   the ray running right from it. A segment whose ends straddle the
   point's row crosses that ray on one side or the other, and which side
   is the sign of the cross product. The point is never on a segment
   where this is used, so no crossing is ambiguous. */
static
int _seg_winds(real x0, real y0, real x1, real y1, real px, real py)
{
    if (y0 <= py)
    {
        if (y1 > py && (x1 - x0) * (py - y0) - (px - x0) * (y1 - y0) > 0)
            return 1;
    }
    else
    {
        if (y1 <= py && (x1 - x0) * (py - y0) - (px - x0) * (y1 - y0) < 0)
            return -1;
    }
    return 0;
}

/* Whether an axis-aligned rectangle subpath begins at an offset, and if
   one does whether it reaches into a box: 1 when it does, 0 when it is
   clear of the box, -1 when the subpath is not such a rectangle. The
   offset the next subpath begins at comes back in *next when the answer
   is not -1.

   A rectangle is four corners -- a move and three lines -- with every
   side, the closing one included, parallel to an axis, and then the
   next subpath's move or the end of the path, with at most a close
   between. A close repeats the start point and carries no corner of its
   own, and it does not end a subpath: anything after one but a move
   belongs to the same subpath, which is then no rectangle. The four
   corner elements are all of one width, so where the extent holds four
   of them it holds all four, and they are read at offsets arrived at by
   arithmetic rather than by walking.

   The regions this is asked about are mostly built of these: a region
   cut from another arrives as one pixel-band rectangle per band it
   covers, hundreds of them on a page, and a rectangle answers about a
   box in four comparisons where a walk of its four sides answers in
   four separating-axis tests and four winding steps. */
static
int _rect_subpath_meets(const char *p, unsigned int used, unsigned int o,
                        real bx0, real by0, real bx1, real by1,
                        unsigned int *next)
{
    const unsigned int esz = 1 + 2 * (unsigned int)sizeof(real);
    real px[4], py[4];
    real rx0, ry0, rx1, ry1;
    unsigned int q;
    int j;

    if (used - o < 4 * esz)
        return -1;
    if (PATH_CMD(p, o) != PATH_CMD_MOVE
            || PATH_CMD(p, o + esz) != PATH_CMD_LINE
            || PATH_CMD(p, o + 2 * esz) != PATH_CMD_LINE
            || PATH_CMD(p, o + 3 * esz) != PATH_CMD_LINE)
        return -1;
    for (j = 0; j < 4; j++)
    {
        real co[2];

        _path_get_coords(p, o + (unsigned int)j * esz, co, 2);
        px[j] = co[0];
        py[j] = co[1];
    }
    for (j = 0; j < 4; j++)
    {
        int k = (j + 1) & 3;

        if (px[j] != px[k] && py[j] != py[k])
            return -1;
    }
    q = o + 4 * esz;
    if (q < used && PATH_CMD(p, q) == PATH_CMD_CLOSE)
    {
        if (esz > used - q)
            return -1;
        q += esz;
    }
    /* a subpath runs to the next move and no closer: a close in the
       middle of one repeats its start point and leaves the subpath
       open, and what follows belongs to it. Four corners with nothing
       after them but the next subpath is a rectangle here; four with
       anything else after them is part of a larger figure. */
    if (q < used && PATH_CMD(p, q) != PATH_CMD_MOVE)
        return -1;
    rx0 = rx1 = px[0];
    ry0 = ry1 = py[0];
    for (j = 1; j < 4; j++)
    {
        if (px[j] < rx0) rx0 = px[j];
        if (px[j] > rx1) rx1 = px[j];
        if (py[j] < ry0) ry0 = py[j];
        if (py[j] > ry1) ry1 = py[j];
    }
    *next = q;
    return rx0 <= bx1 && rx1 >= bx0 && ry0 <= by1 && ry1 >= by0;
}

/* Whether an axis-aligned box meets the nonzero interior of a path:
   1 when it does, 0 when it does not, -1 when this cannot say -- a
   curve, or an element the extent does not hold.

   Two questions in one walk. If any segment of the path meets the box
   the two overlap and the answer is yes at once. If none does, the box
   lies wholly inside one face of the path and every point of it winds
   alike, so one corner answers for the box: a nonzero winding puts the
   box inside the path, a zero winding outside it.

   A rectangle subpath is settled without either. Its interior is its
   own box, so a rectangle the box reaches into is an answer of yes; and
   one clear of the box holds no point of the box, so the corner the
   winding is taken at lies outside it and it winds about that corner
   not at all. Neither answer needs its sides walked.

   Each remaining subpath closes as a fill closes it, back to the point
   it was opened at, whether or not a close element says so, and a close
   element is that point again rather than the coordinates it carries. A
   curve is refused rather than approximated: the control hull contains
   the curve but winds differently from it, and a walk that read the
   hull's segments would answer about a shape the path is not. */
static
int _box_meets_path(const char *p, unsigned int used,
                    real bx0, real by0, real bx1, real by1)
{
    unsigned int o = PATH_HDR;
    int wind = 0;

    while (o < used)
    {
        real cx = 0, cy = 0, sx = 0, sy = 0;
        unsigned int next;
        int started = 0;
        int rect;

        rect = _rect_subpath_meets(p, used, o, bx0, by0, bx1, by1, &next);
        if (rect >= 0)
        {
            if (rect)
                return 1;
            o = next;
            continue;
        }
        while (o < used)
        {
            int cmd = PATH_CMD(p, o);
            unsigned int esz;
            real co[2];

            if (cmd > PATH_CMD_CLOSE)
                return -1;
            esz = _path_elem_size(cmd);
            if (esz > used - o)
                return -1;
            if (cmd == PATH_CMD_CURVE)
                return -1;
            if (cmd == PATH_CMD_MOVE && started)
                break;
            if (cmd == PATH_CMD_CLOSE)
            {
                /* a close is the subpath's start point again, whatever
                   its own coordinates say, which is how the walk that
                   hands a path to a fill reads one */
                if (!started)
                {
                    o += esz;
                    continue;
                }
                co[0] = sx;
                co[1] = sy;
            }
            else
                _path_get_coords(p, o, co, 2);
            if (!started)
            {
                sx = cx = co[0];
                sy = cy = co[1];
                started = 1;
            }
            else
            {
                if (_seg_meets_box(cx, cy, co[0], co[1], bx0, by0, bx1, by1))
                    return 1;
                wind += _seg_winds(cx, cy, co[0], co[1], bx0, by0);
                cx = co[0];
                cy = co[1];
            }
            o += esz;
        }
        if (!started)   /* an element the walk could not advance past */
            return -1;
        if (cx != sx || cy != sy)
        {
            if (_seg_meets_box(cx, cy, sx, sy, bx0, by0, bx1, by1))
                return 1;
            wind += _seg_winds(cx, cy, sx, sy, bx0, by0);
        }
    }
    return wind != 0;
}

/* clip trivial-reject test.
   Push true when a fill of the current path can mark no pixel at all
   under the clip region in force, so that the paint may be dropped
   where it stands. Push false in every uncertain case, so what is not
   rejected here travels the route it would have travelled anyway: this
   rejects, it does not decide, and a wrong yes is a blank on someone's
   page where a wrong no is only a fill written out that the consumer
   then clips away.

   The question is asked about pixels and not about geometry, because
   pixels are what a painting marks. PLRM 7.5.1 has the region a
   painting covers scan-converted to whole pixels, and the region a clip
   describes likewise, and the painting marks the pixels the two share.
   So each operand is taken out to the pixels it reaches -- the columns
   from the floor of a left edge up to but not including the ceiling of
   a right one, and the rows likewise -- and two paints that share no
   such pixel cannot mark together. Two shapes that meet along an edge
   at a pixel boundary reach no common pixel and are rejected here; two
   that fall short of each other inside one pixel do share it, and are
   not.

   The path's box comes from its header, which bounds the stored points
   -- curve controls included, so a curve's box contains the curve --
   and errs large, which can only make this answer no.

   Where the boxes do share pixels the region's own shape is asked, and
   this is where a region that is not its own box answers for itself:
   the hole of an even-odd annulus, or the notch of an L, is inside the
   box and outside the region, and a fill there marks nothing. The box
   handed to that question is the pixels the fill reaches grown by one
   more all round, which is what makes a geometric question about the
   region answer a question about pixels: a pixel the fill reaches and
   the region's scan conversion paints has the region's own outline or
   interior within a pixel of it.

   An empty path and an empty region are both left alone, their header
   boxes being the sentinel a path is created with, which describes
   nothing. */
static
int _clipmisses(Xpost_Context *ctx)
{
    Xpost_Object gstate, path, clipregion;
    char *p;
    unsigned int used;
    real sminx, sminy, smaxx, smaxy;
    real cminx, cminy, cmaxx, cmaxy;
    int miss = 0;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    clipregion = xpost_dict_get(ctx, gstate, nameclipregion);
    if (xpost_object_get_type(path) == stringtype
            && xpost_object_get_type(clipregion) == stringtype)
    {
        if (!_path_extent(ctx, path, &p, &used))
            return rangecheck;
        if (used > PATH_HDR)
        {
            sminx = _path_get_f32(p, PATH_BBOX(0));
            sminy = _path_get_f32(p, PATH_BBOX(1));
            smaxx = _path_get_f32(p, PATH_BBOX(2));
            smaxy = _path_get_f32(p, PATH_BBOX(3));
            if (!_path_extent(ctx, clipregion, &p, &used))
                return rangecheck;
            if (used > PATH_HDR)
            {
                cminx = _path_get_f32(p, PATH_BBOX(0));
                cminy = _path_get_f32(p, PATH_BBOX(1));
                cmaxx = _path_get_f32(p, PATH_BBOX(2));
                cmaxy = _path_get_f32(p, PATH_BBOX(3));
                /* a box whose low edge is above its high one is the
                   sentinel, not a box, and answers nothing */
                if (sminx <= smaxx && sminy <= smaxy
                        && cminx <= cmaxx && cminy <= cmaxy)
                {
                    /* the pixels each box reaches, as the half-open
                       real intervals those whole pixels span */
                    real sx0 = (real)floor(sminx), sx1 = (real)ceil(smaxx);
                    real sy0 = (real)floor(sminy), sy1 = (real)ceil(smaxy);
                    real cx0 = (real)floor(cminx), cx1 = (real)ceil(cmaxx);
                    real cy0 = (real)floor(cminy), cy1 = (real)ceil(cmaxy);

                    if (sx1 <= cx0 || sx0 >= cx1
                            || sy1 <= cy0 || sy0 >= cy1)
                        miss = 1;
                    else if (_box_meets_path(p, used, sx0 - 1, sy0 - 1,
                                             sx1 + 1, sy1 + 1) == 0)
                        miss = 1;
                }
            }
        }
    }

    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(miss));
    return 0;
}

/* -  .pathisrect  x0 y0 x1 y1 true
                   false
   when the current path is a single axis-aligned rectangle, its
   device-space bounds; the clip machinery intersects two rectangles
   directly rather than through the span pipeline */
static
int _pathisrect(Xpost_Context *ctx)
{
    Xpost_Object gstate, path;
    real minx, miny, maxx, maxy;
    int rect;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    rect = _path_is_rect(ctx, path, &minx, &miny, &maxx, &maxy);
    if (rect < 0)
        return rangecheck;
    if (rect)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(minx));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(miny));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(maxx));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(maxy));
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    }
    else
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
}

/* -  .cliprect  x0 y0 x1 y1 true
                 false
   when the clip region is a single axis-aligned rectangle, its
   device-space bounds; painting machinery that writes the raster
   directly clamps to them */
static
int _cliprect(Xpost_Context *ctx)
{
    Xpost_Object gstate, clipregion;
    real cminx, cminy, cmaxx, cmaxy;
    int rect;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    clipregion = xpost_dict_get(ctx, gstate, nameclipregion);
    rect = _path_is_rect(ctx, clipregion, &cminx, &cminy, &cmaxx, &cmaxy);
    if (rect < 0)
        return rangecheck;
    if (rect)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(cminx));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(cminy));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(cmaxx));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(cmaxy));
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    }
    else
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    return 0;
}

/* Whether the subpath beginning at a move is an axis-aligned rectangle,
   and if so the corner and extent it spans.

   Both formats say a rectangle in one operator -- PDF's re, and SVG's
   own rect element -- where a subpath says it in five: a move, three
   lines and a close. What draws thousands of them is anything built out
   of bars, and a barcode label is the case that prompted this:
   MEASURED at twenty thousand rectangular subpaths on one page, better
   than sixty thousand line segments between them.

   The four corners have to come round in one of the two orders a
   rectangle can be walked, and the close is required: three lines that
   happen to be axis-aligned but are left open enclose nothing until
   the fill closes them, and a fill is not the only thing a path is
   walked for. */
static int _subpath_is_rect(const char *p, unsigned int o, unsigned int used,
                            real *x, real *y, real *w, real *h)
{
    real c[5][2];
    unsigned int q = o;
    int i;

    for (i = 0; i < 5; i++)
    {
        int cmd;
        unsigned int esz;

        if (q >= used)
            return 0;
        cmd = p[q];
        if (i == 0 ? cmd != PATH_CMD_MOVE
                   : i < 4 ? cmd != PATH_CMD_LINE
                           : cmd != PATH_CMD_CLOSE)
            return 0;
        esz = _path_elem_size(cmd);
        if (esz > used - q)
            return 0;
        _path_get_coords(p, q, c[i], 2);
        q += esz;
    }
    /* Compared at the precision the numbers are written to, not as the
       floats they arrive as. A corner meant to sit above another comes
       through a matrix and misses it by a fraction the writer would not
       print: MEASURED, comparing the floats exactly recognised 783 of
       nineteen thousand rectangles on a page built out of bars. What is
       being asked is whether the rectangle written would be the subpath
       written, and that is a question about the output. */
#define SAMENUM(a, b) (((a) - (b)) < 5e-5 && ((b) - (a)) < 5e-5)
    /* the close repeats the start, which is what makes it a closed
       four-cornered figure and not three lines that end elsewhere */
    if (!SAMENUM(c[4][0], c[0][0]) || !SAMENUM(c[4][1], c[0][1]))
        return 0;
    /* one of the two ways round: the sides alternate between holding x
       and holding y */
    if (SAMENUM(c[0][0], c[1][0]) && SAMENUM(c[1][1], c[2][1]) &&
        SAMENUM(c[2][0], c[3][0]) && SAMENUM(c[3][1], c[0][1]))
        ;
    else if (SAMENUM(c[0][1], c[1][1]) && SAMENUM(c[1][0], c[2][0]) &&
             SAMENUM(c[2][1], c[3][1]) && SAMENUM(c[3][0], c[0][0]))
        ;
    else
        return 0;
#undef SAMENUM
    *x = c[0][0] < c[2][0] ? c[0][0] : c[2][0];
    *y = c[0][1] < c[2][1] ? c[0][1] : c[2][1];
    *w = c[0][0] < c[2][0] ? c[2][0] - c[0][0] : c[0][0] - c[2][0];
    *h = c[0][1] < c[2][1] ? c[2][1] - c[0][1] : c[0][1] - c[2][1];
    return 1;
}

/* Emit the current path into a vector device's content accumulator with
   curves preserved -- the FillPath hot loop, walking the path string
   directly so arbitrarily many subpaths cost no operand stack. The
   syntax is the device's: PDF path operators or SVG path commands. */
static
int _fillpath_emit(Xpost_Context *ctx,
                   const double *comp,
                   Xpost_Object devdic, int svg)
{
    Xpost_Object gstate, path;
    char *p;
    unsigned int used, o, esz;
    char tmp[192];
    int n, i, ret;

    Xpost_Object eo;
    int evenodd;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    /* eofill raises the flag around its call: the consumer applies
       the even-odd rule instead of nonzero winding */
    eo = xpost_dict_get(ctx, gstate, xpost_name_cons(ctx, ".eorule"));
    evenodd = xpost_object_get_type(eo) == booleantype && eo.int_.val;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;
    if (used <= PATH_HDR)
        return 0;

    /* the PDF device emits its fill colour itself, before the walk */
    if (svg)
    {
        n = 0;
        memcpy(tmp + n, "<path fill=\"rgb(", 16); n += 16;
        n += xpost_dev_pdf_fmt_num(tmp + n, comp[0] * 100); tmp[n++] = '%'; tmp[n++] = ',';
        n += xpost_dev_pdf_fmt_num(tmp + n, comp[1] * 100); tmp[n++] = '%'; tmp[n++] = ',';
        n += xpost_dev_pdf_fmt_num(tmp + n, comp[2] * 100); tmp[n++] = '%';
        if (evenodd)
            { memcpy(tmp + n, ")\" fill-rule=\"evenodd\" d=\"", 26); n += 26; }
        else
            { memcpy(tmp + n, ")\" fill-rule=\"nonzero\" d=\"", 26); n += 26; }
        ret = xpost_dev_pdf_append(ctx, devdic, tmp, n);
        if (ret)
            return ret;
    }

    for (o = PATH_HDR; o < used; o += esz)
    {
        int cmd = p[o];
        int nco = cmd == PATH_CMD_CURVE ? 6 : 2;
        real co[6];
        real rx, ry, rw, rh;

        if (cmd < PATH_CMD_MOVE || cmd > PATH_CMD_CLOSE)
            return unregistered;
        esz = _path_elem_size(cmd);
        if (esz > used - o)
            return rangecheck;
        if (cmd == PATH_CMD_MOVE && !svg &&
            _subpath_is_rect(p, o, used, &rx, &ry, &rw, &rh))
        {
            n = 0;
            n += xpost_dev_pdf_fmt_num(tmp + n, rx); tmp[n++] = ' ';
            n += xpost_dev_pdf_fmt_num(tmp + n, ry); tmp[n++] = ' ';
            n += xpost_dev_pdf_fmt_num(tmp + n, rw); tmp[n++] = ' ';
            n += xpost_dev_pdf_fmt_num(tmp + n, rh); tmp[n++] = ' ';
            tmp[n++] = 'r'; tmp[n++] = 'e'; tmp[n++] = '\n';
            ret = xpost_dev_pdf_append(ctx, devdic, tmp, n);
            if (ret)
                return ret;
            /* the move, its three lines and the close are all said */
            esz = _path_elem_size(PATH_CMD_MOVE) * 4
                  + _path_elem_size(PATH_CMD_CLOSE);
            continue;
        }
        _path_get_coords(p, o, co, nco);
        n = 0;
        if (cmd == PATH_CMD_CLOSE)
        {
            /* the stored coordinates repeat the subpath start */
            if (svg)
                tmp[n++] = 'Z';
            else
                { tmp[n++] = 'h'; tmp[n++] = '\n'; }
        }
        else if (svg)
        {
            tmp[n++] = cmd == PATH_CMD_MOVE ? 'M'
                     : cmd == PATH_CMD_LINE ? 'L' : 'C';
            for (i = 0; i < nco; i++)
            {
                if (i) tmp[n++] = ' ';
                n += xpost_dev_pdf_fmt_num(tmp + n, co[i]);
            }
        }
        else
        {
            for (i = 0; i < nco; i++)
            {
                n += xpost_dev_pdf_fmt_num(tmp + n, co[i]);
                tmp[n++] = ' ';
            }
            tmp[n++] = cmd == PATH_CMD_MOVE ? 'm'
                     : cmd == PATH_CMD_LINE ? 'l' : 'c';
            tmp[n++] = '\n';
        }
        if (n)
        {
            ret = xpost_dev_pdf_append(ctx, devdic, tmp, n);
            if (ret)
                return ret;
        }
    }

    ret = xpost_dev_pdf_append(ctx, devdic,
                               svg ? "\"/>\n" : evenodd ? "f*\n" : "f\n",
                               svg ? 4 : evenodd ? 3 : 2);
    if (ret)
        return ret;
    return 0;
}

/* Fill the current path into a vector device's stream, in the form
   that device writes: the pair differ in what they emit and in whether
   the colour travels with the path, which is why the shared code takes
   the components rather than reading them. */
static
int _pdffillpath(Xpost_Context *ctx,
                 Xpost_Object devdic)
{
    return _fillpath_emit(ctx, NULL, devdic, 0);
}

static
int _svgfillpath(Xpost_Context *ctx,
                 Xpost_Object r, Xpost_Object g, Xpost_Object b,
                 Xpost_Object devdic)
{
    double comp[3];
    comp[0] = xpost_object_number(r); comp[1] = xpost_object_number(g); comp[2] = xpost_object_number(b);
    return _fillpath_emit(ctx, comp, devdic, 1);
}

static
int _closepath(Xpost_Context *ctx)
{
    Xpost_Object gstate, path;
    char *p;
    unsigned int used, last, sps;
    real co[2];

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;
    if (used <= PATH_HDR)
        return 0;
    last = _path_get_u32(p, 8);
    sps = _path_get_u32(p, 4);
    /* offset 8 names the element whose command code decides whether the
       subpath is already closed, and offset 4 the one whose first point
       the close repeats: each is applied to the path pointer, so each
       has to name content wide enough for what is read there */
    if (last >= used || used - last < _path_elem_size(p[last]))
        return rangecheck;
    if (sps >= used || used - sps < _path_elem_size(PATH_CMD_MOVE))
        return rangecheck;
    if (p[last] == PATH_CMD_CLOSE)
        return 0;
    _path_get_coords(p, sps, co, 2);
    return _path_append(ctx, gstate, &path, PATH_CMD_CLOSE, co, 2);
}

/* va vb vc  .tripoly  polygon true
   va vb vc  .tripoly  false

   The polygon argument the device polygon fill takes for a triangle,
   from three mesh vertices, where the clip region cannot cut it. A
   vertex is the array a mesh reads one into -- its coordinates in user
   space and then its colour -- and only the coordinates are read here.

   This is the flat fill of a smooth shading and nothing else. A shading
   resolves to flat-coloured triangles and paints one per subdivided
   cell, so three points become a fill tens of thousands of times over
   for one gradient, and what lies between the points and the device is
   the same every time: a path holding three points and a close, the
   question of whether the clip can cut it, and the polygon its points
   read back as. None of it outlives the fill. So the polygon is built
   from the points, and the path in the middle -- allocated, written,
   read once and thrown away -- is not built at all.

   What it answers is what that path would have answered. The points
   cross the transform the constructors apply, in the width a path
   stores a coordinate in, so each is the coordinate the path would have
   held. The polygon is the one the reader builds from four points and
   one subpath: three points, the subpath's first repeated where it
   closes, and the separator. The clip question is the one the
   trivial-accept test asks, over the box those points span, which is
   the box the path's header would have carried -- the close repeats a
   point already in it and cannot widen it.

   False rather than a cut polygon: a triangle the region can cut goes
   back to the general route, which cuts it, asks the trivial-accept
   question itself, and counts it.

   A polygon built leaves the current path empty, as a fill leaves it.
   A triangle answered false leaves it alone: the general route it goes
   to opens a path of its own. */
static
int _tripoly(Xpost_Context *ctx,
             Xpost_Object va, Xpost_Object vb, Xpost_Object vc)
{
    Xpost_Object gstate, clipregion, backing, result;
    Xpost_Object vert[3];
    Xpost_Object *bk, *rs;
    real m[6];
    real uv[6];
    real pt[6];
    real cminx, cminy, cmaxx, cmaxy;
    real bminx, bminy, bmaxx, bmaxy;
    int rect, i, ret;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    ret = _path_ctm(ctx, gstate, m);
    if (ret)
        return ret;

    /* a vertex carries its x and y first and its colour after them; a
       shorter one is not a vertex */
    vert[0] = va; vert[1] = vb; vert[2] = vc;
    for (i = 0; i < 3; i++)
    {
        Xpost_Object c;

        if (vert[i].comp_.sz < 2)
            return rangecheck;
        c = xpost_array_get(ctx, vert[i], 0);
        if (xpost_object_get_type(c) != realtype &&
            xpost_object_get_type(c) != integertype)
            return typecheck;
        uv[2 * i] = NUM(c);
        c = xpost_array_get(ctx, vert[i], 1);
        if (xpost_object_get_type(c) != realtype &&
            xpost_object_get_type(c) != integertype)
            return typecheck;
        uv[2 * i + 1] = NUM(c);
    }

    for (i = 0; i < 3; i++)
    {
        pt[2 * i]     = m[0] * uv[2 * i] + m[2] * uv[2 * i + 1] + m[4];
        pt[2 * i + 1] = m[1] * uv[2 * i] + m[3] * uv[2 * i + 1] + m[5];
    }

    clipregion = xpost_dict_get(ctx, gstate, nameclipregion);
    rect = _path_is_rect(ctx, clipregion, &cminx, &cminy, &cmaxx, &cmaxy);
    if (rect < 0)
        return rangecheck;
    if (rect)
    {
        bminx = bmaxx = pt[0];
        bminy = bmaxy = pt[1];
        for (i = 2; i < 6; i += 2)
        {
            if (pt[i] < bminx) bminx = pt[i];
            if (pt[i] > bmaxx) bmaxx = pt[i];
            if (pt[i + 1] < bminy) bminy = pt[i + 1];
            if (pt[i + 1] > bmaxy) bmaxy = pt[i + 1];
        }
        rect = bminx >= cminx && bmaxx <= cmaxx &&
               bminy >= cminy && bmaxy <= cmaxy;
    }
    if (!rect)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }

    /* four points -- the three and the close's repeat of the first --
       viewed in pairs out of one backing array, and the subpath
       separator after them */
    backing = _rawarray_cons(ctx, 8, &bk);
    if (xpost_object_get_type(backing) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->hold, backing);
    result = _rawarray_cons(ctx, 5, &rs);
    if (xpost_object_get_type(result) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->hold, result);
    /* the allocation above can move the memory file */
    {
        unsigned int adr;
        /* backing's entity was allocated in this same memory file two
           statements above and the allocation was checked; an entity
           number stays within the table once the table has reached it */
        XPOST_REFUSAL_IMPOSSIBLE(
            xpost_memory_table_get_addr(ctx->lo,
                                        xpost_object_get_ent(backing), &adr));
        bk = (Xpost_Object *)xpost_vm_ptr(ctx->lo, adr);
    }

    for (i = 0; i < 6; i++)
        bk[i] = xpost_real_cons(pt[i]);
    bk[6] = bk[0];
    bk[7] = bk[1];
    for (i = 0; i < 4; i++)
    {
        Xpost_Object v = backing;
        /* the close repeats the subpath's first point, and is that
           point's pair rather than the copy written above it */
        v.comp_.off = i == 3 ? 0 : (unsigned int)(2 * i);
        v.comp_.sz = 2;
        rs[i] = v;
    }
    rs[4] = null;

    xpost_stack_push(ctx->lo, ctx->os, result);
    /* the polygon stays on the operand stack while the empty path is
       allocated, so the collector reaching over that allocation sees it */
    ret = _newpath(ctx);
    if (ret)
        return ret;
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
}

/* va vb vc devcols DEVICE  .trishade  bool

   Paint one mesh triangle with its colour varying across it, and answer
   whether it was painted.

   The three vertices are the arrays a mesh reads, and only their
   coordinates are read here: the colours reaching the device are
   `devcols`, three groups of components already converted for it, so
   that the conversion happens three times for a triangle rather than
   once for every piece the old subdivision would have cut it into.

   False where it did not paint -- a clip that can cut the triangle, a
   device that cannot be painted from C, a colour space whose components
   this does not count -- and the caller subdivides it as before. The
   graphics state and the current path are untouched either way, so the
   caller's fallback finds what it would have found.

   The clip question is the one .tripoly asks, and for the same reason:
   what is painted here goes to the device whole, with no region beside
   it, so it may only be painted where the region cannot cut it. */
static
int _trishade(Xpost_Context *ctx,
              Xpost_Object va, Xpost_Object vb, Xpost_Object vc,
              Xpost_Object devcols, Xpost_Object devdic)
{
    Xpost_Object gstate, clipregion;
    Xpost_Object vert[3];
    real m[6];
    real uv[6];
    double pt[6];
    double col[12];
    real cminx, cminy, cmaxx, cmaxy;
    double bminx, bminy, bmaxx, bmaxy;
    int ncomp, rect, i, ret, painted;

    if (devcols.comp_.sz < 3 || devcols.comp_.sz % 3 != 0)
        return rangecheck;
    ncomp = (int)devcols.comp_.sz / 3;
    if (ncomp > 4)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    ret = _path_ctm(ctx, gstate, m);
    if (ret)
        return ret;

    vert[0] = va; vert[1] = vb; vert[2] = vc;
    for (i = 0; i < 3; i++)
    {
        Xpost_Object c;

        if (vert[i].comp_.sz < 2)
            return rangecheck;
        c = xpost_array_get(ctx, vert[i], 0);
        if (xpost_object_get_type(c) != realtype &&
            xpost_object_get_type(c) != integertype)
            return typecheck;
        uv[2 * i] = NUM(c);
        c = xpost_array_get(ctx, vert[i], 1);
        if (xpost_object_get_type(c) != realtype &&
            xpost_object_get_type(c) != integertype)
            return typecheck;
        uv[2 * i + 1] = NUM(c);
    }
    for (i = 0; i < 3 * ncomp; i++)
    {
        Xpost_Object c = xpost_array_get(ctx, devcols, i);

        if (xpost_object_get_type(c) != realtype &&
            xpost_object_get_type(c) != integertype)
            return typecheck;
        col[i] = (double)NUM(c);
    }

    for (i = 0; i < 3; i++)
    {
        pt[2 * i]     = (double)(m[0] * uv[2 * i] + m[2] * uv[2 * i + 1] + m[4]);
        pt[2 * i + 1] = (double)(m[1] * uv[2 * i] + m[3] * uv[2 * i + 1] + m[5]);
    }

    clipregion = xpost_dict_get(ctx, gstate, nameclipregion);
    rect = _path_is_rect(ctx, clipregion, &cminx, &cminy, &cmaxx, &cmaxy);
    if (rect < 0)
        return rangecheck;
    if (rect)
    {
        bminx = bmaxx = pt[0];
        bminy = bmaxy = pt[1];
        for (i = 2; i < 6; i += 2)
        {
            if (pt[i] < bminx) bminx = pt[i];
            if (pt[i] > bmaxx) bmaxx = pt[i];
            if (pt[i + 1] < bminy) bminy = pt[i + 1];
            if (pt[i + 1] > bmaxy) bmaxy = pt[i + 1];
        }
        rect = bminx >= (double)cminx && bmaxx <= (double)cmaxx &&
               bminy >= (double)cminy && bmaxy <= (double)cmaxy;
    }
    if (!rect)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }

    ret = xpost_dev_gouraud_paint(ctx, devdic, pt, col, ncomp, &painted);
    if (ret)
        return ret;
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(painted));
    return 0;
}

/* -  .pathempty  bool
   report whether the current path has no elements */
static
int _pathempty(Xpost_Context *ctx)
{
    Xpost_Object path;
    int empty = 1;

    path = _cpath(ctx);
    if (xpost_object_get_type(path) == stringtype)
    {
        char *p = xpost_string_get_pointer(ctx, path);
        empty = _path_get_u32(p, 0) <= PATH_HDR;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(empty));
    return 0;
}

/* -  .pathwalksteps  int
   The number of path elements the layout checks have walked in local
   memory, saturating rather than wrapping. What reading a path costs is
   this number and not the path's length, so it is the measure of whether
   the cost of building a path tracks the elements it gains or that
   number multiplied by the operators that read it along the way. */
static
int _pathwalksteps(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons((int)ctx->lo->path_walk.steps)))
        return stackoverflow;
    return 0;
}

/* x y  .devmoveto  -
   append a move element in device coordinates, bypassing the CTM */
static
int _devmoveto(Xpost_Context *ctx, Xpost_Object x, Xpost_Object y)
{
    Xpost_Object gstate, path;
    real co[2];

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    co[0] = x.real_.val;
    co[1] = y.real_.val;
    return _path_append(ctx, gstate, &path, PATH_CMD_MOVE, co, 2);
}

/* x y  .devlineto  -
   append a line element in device coordinates, bypassing the CTM */
static
int _devlineto(Xpost_Context *ctx, Xpost_Object x, Xpost_Object y)
{
    Xpost_Object gstate, path;
    char *p;
    unsigned int used;
    real co[2];

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;
    if (used <= PATH_HDR)
        return nocurrentpoint;
    co[0] = x.real_.val;
    co[1] = y.real_.val;
    return _path_append(ctx, gstate, &path, PATH_CMD_LINE, co, 2);
}

/* x1 y1 x2 y2 x3 y3  .devcurveto  -
   append a curve element in device coordinates, bypassing the CTM */
static
int _devcurveto(Xpost_Context *ctx,
                Xpost_Object x1, Xpost_Object y1,
                Xpost_Object x2, Xpost_Object y2,
                Xpost_Object x3, Xpost_Object y3)
{
    Xpost_Object gstate, path;
    char *p;
    unsigned int used;
    real co[6];

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;
    if (used <= PATH_HDR)
        return nocurrentpoint;
    co[0] = x1.real_.val;
    co[1] = y1.real_.val;
    co[2] = x2.real_.val;
    co[3] = y2.real_.val;
    co[4] = x3.real_.val;
    co[5] = y3.real_.val;
    return _path_append(ctx, gstate, &path, PATH_CMD_CURVE, co, 6);
}

/* path  .copypath  path'
   value copy of a packed path (for gsave) */
static
int _copypath(Xpost_Context *ctx, Xpost_Object path)
{
    Xpost_Object np;
    char *p, *q;
    unsigned int used;

    if (xpost_object_get_type(path) != stringtype)
        return typecheck;
    if (_path_avail(ctx, path) < PATH_HDR)
        return rangecheck;
    p = xpost_string_get_pointer(ctx, path);
    used = _path_get_u32(p, 0);
    /* offset 0 is the byte extent copied and offset 12 the capacity the
       destination is sized to; both must fit the source allocation so the
       copy neither reads past the string nor writes past the new path */
    if (used < PATH_HDR || used > _path_avail(ctx, path)
            || used > _path_get_u32(p, 12))
        return rangecheck;
    np = _path_cons(ctx, _path_cap(ctx, path));
    if (xpost_object_get_type(np) == invalidtype)
        return VMerror;
    q = xpost_string_get_pointer(ctx, np);
    p = xpost_string_get_pointer(ctx, path); /* re-derive after cons */
    {
        unsigned int cap = _path_get_u32(q, 12);
        memcpy(q, p, used);
        _path_set_u32(q, 12, cap); /* keep the copy's own capacity */
    }
    xpost_stack_push(ctx->lo, ctx->os, np);
    return 0;
}

/* -  .retagclose  -
   convert a final line element whose endpoint the caller has verified
   equals the subpath start into a close element, in place */
static
int _retagclose(Xpost_Context *ctx)
{
    Xpost_Object path;
    char *p;
    unsigned int used, last;

    path = _cpath(ctx);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    /* an in-place mutation of the current path: back it up for restore
       as _path_append does (see the note there) */
    {
        unsigned int pent = xpost_object_get_ent(path);
        if (xpost_save_cow(ctx->lo, stringtype, 0, pent))
            return VMerror;
    }
    p = xpost_string_get_pointer(ctx, path);
    used = _path_get_u32(p, 0);
    if (used <= PATH_HDR)
        return 0;
    last = _path_get_u32(p, 8);
    if (p[last] == PATH_CMD_LINE)
        p[last] = PATH_CMD_CLOSE;
    return 0;
}

/* str off  .pathnext  coords... cmd nextoff true
   str off  .pathnext  false
   step a packed path enumeration: pushes the element at byte offset
   off (its point for move and line, three points for curve, nothing
   for close), the command code, and the offset of the following
   element, or false when off is past the end */
static
int _pathnext(Xpost_Context *ctx, Xpost_Object str, Xpost_Object off)
{
    char *p;
    unsigned int used, o;
    int cmd, n, k;
    real co[6];

    if (xpost_object_get_type(str) != stringtype)
        return typecheck;
    if (_path_avail(ctx, str) < PATH_HDR)
        return rangecheck;
    p = xpost_string_get_pointer(ctx, str);
    used = _path_get_u32(p, 0);
    if (used > _path_avail(ctx, str))
        return rangecheck;
    /* an offset before the first element names no element, and one
       below the header names the first */
    if (off.int_.val < 0)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    o = (unsigned int)off.int_.val < PATH_HDR
        ? PATH_HDR : (unsigned int)off.int_.val;
    if (o >= used)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    cmd = p[o];
    if (cmd < PATH_CMD_MOVE || cmd > PATH_CMD_CLOSE)
        return unregistered;
    if (cmd != PATH_CMD_CLOSE)
    {
        n = cmd == PATH_CMD_CURVE ? 6 : 2;
        if (o + _path_elem_size(cmd) > used)
            return rangecheck;
        _path_get_coords(p, o, co, n);
        for (k = 0; k < n; k++)
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(co[k]));
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(cmd));
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(o + _path_elem_size(cmd)));
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
}

/* -  pathbbox  llx lly urx ury
   bounding box of the current path in user space (PLRM): the stored
   device-space points are mapped back through the inverse CTM before
   accumulation; an empty path raises nocurrentpoint */
static
int _pathbbox(Xpost_Context *ctx)
{
    Xpost_Object path;
    Xpost_Object gd, gs, psmat;
    real m[6], inv[6], det;
    const real *invp = NULL;
    real minx = 0, miny = 0, maxx = 0, maxy = 0;
    int i, ret;

    /* fetch the CTM and build its inverse; on any irregularity fall
       back to the raw device-space box rather than erroring */
    gd = ctx->graphicsdict;
    gs = xpost_dict_get(ctx, gd, xpost_name_cons(ctx, "currgstate"));
    psmat = xpost_dict_get(ctx, gs, xpost_name_cons(ctx, "currmatrix"));
    if (xpost_object_get_type(psmat) == arraytype && psmat.comp_.sz == 6)
    {
        for (i = 0; i < 6; i++)
        {
            Xpost_Object e = xpost_array_get(ctx, psmat, i);
            m[i] = xpost_object_get_type(e) == realtype ? e.real_.val
                 : (real)e.int_.val;
        }
        det = m[0] * m[3] - m[1] * m[2];
        if (det != 0)
        {
            inv[0] = m[3] / det;
            inv[1] = -m[1] / det;
            inv[2] = -m[2] / det;
            inv[3] = m[0] / det;
            inv[4] = (m[2] * m[5] - m[3] * m[4]) / det;
            inv[5] = (m[1] * m[4] - m[0] * m[5]) / det;
            invp = inv;
        }
    }

    path = _cpath(ctx);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;

    /* A declared box answers instead of the path. PLRM 8.2: after setbbox,
       "subsequent invocations of pathbbox will return a result derived from
       the bounding box rather than from the actual path" -- and it answers
       even for a path that has reached nothing, the box being what the
       program said the path would occupy. */
    {
        char *bp;
        unsigned int bused;

        if (_path_extent(ctx, path, &bp, &bused))
        {
            real sx0 = _path_get_f32(bp, PATH_SBB(0));
            real sx1 = _path_get_f32(bp, PATH_SBB(2));

            if (sx0 <= sx1)
            {
                real sy0 = _path_get_f32(bp, PATH_SBB(1));
                real sy1 = _path_get_f32(bp, PATH_SBB(3));
                real cx[4], cy[4], ox, oy;
                int k;

                cx[0] = sx0; cy[0] = sy0;
                cx[1] = sx1; cy[1] = sy0;
                cx[2] = sx1; cy[2] = sy1;
                cx[3] = sx0; cy[3] = sy1;
                minx = miny = FLT_MAX;
                maxx = maxy = -FLT_MAX;
                for (k = 0; k < 4; k++)
                {
                    if (invp)
                    {
                        ox = invp[0] * cx[k] + invp[2] * cy[k] + invp[4];
                        oy = invp[1] * cx[k] + invp[3] * cy[k] + invp[5];
                    }
                    else
                    {
                        ox = cx[k]; oy = cy[k];
                    }
                    if (ox < minx) minx = ox;
                    if (oy < miny) miny = oy;
                    if (ox > maxx) maxx = ox;
                    if (oy > maxy) maxy = oy;
                }
                xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(minx));
                xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(miny));
                xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(maxx));
                xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(maxy));
                return 0;
            }
        }
    }

    ret = _path_walk_bbox(ctx, path, 1, invp, &minx, &miny, &maxx, &maxy);
    if (ret == 2)
        return nocurrentpoint; /* an empty path has no bounding box */
    if (ret == 1)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(minx));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(miny));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(maxx));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(maxy));
    }
    else
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(0));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(0));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(0));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(0));
    }
    return 0;
}

/* --- arcs ------------------------------------------------------------
   An arc is not a path element: the language has arc operators and the
   path has lines and curves, so an arc is approximated by Bezier segments
   here, at a division fine enough that the difference is below the
   device's own resolution. */


static
void _transform(Xpost_Matrix mat, real x, real y, real *xres, real *yres)
{
    *xres = mat.xx * x + mat.xy * y + mat.xz;
    *yres = mat.yx * x + mat.yy * y + mat.yz;
}

static
Xpost_Object _arc_start_proc;

/* Push one circular-arc segment's cubic Bezier onto the operand stack in
   user-space coordinates, forward: the four points run from the angle1
   end to the angle2 end (negative da sweeps clockwise). Push order is
   x1 y1 x2 y2 x3 y3, ready for the curveto the caller schedules; the
   control offsets follow the standard circular approximation. */
static
void _arcbezseg(Xpost_Context *ctx,
                double cx, double cy, double r,
                double a1, double a2)
{
    Xpost_Matrix mat1, mat2, mat3;
    real da_2, sin_a, cos_a;
    real x1, y1, x2, y2, x3, y3;

    xpost_matrix_scale(&mat1, (real)r, (real)r);
    xpost_matrix_translate(&mat2, (real)cx, (real)cy);
    xpost_matrix_mult(&mat2, &mat1, &mat3);
    xpost_matrix_rotate(&mat2, (real)(((a1 + a2) / 2.0) * RAD_PER_DEG));
    xpost_matrix_mult(&mat3, &mat2, &mat1);

    da_2 = (real)(((a2 - a1) / 2.0) * RAD_PER_DEG);
    sin_a = (real)sin(da_2);
    cos_a = (real)cos(da_2);
    x1 = (real)((4 - cos_a) / 3.0);
    y1 = (1 - x1*cos_a) / sin_a;
    x2 = x1;
    x3 = cos_a;
    y2 = y1;
    y1 = -y1;
    y3 = sin_a;
    _transform(mat1, x1, y1, &x1, &y1);
    _transform(mat1, x2, y2, &x2, &y2);
    _transform(mat1, x3, y3, &x3, &y3);
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x1));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(y1));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x2));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(y2));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(x3));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(y3));
}

/* Append a circular arc counterclockwise (dir=1) or clockwise (dir=-1)
   from a1 to a2, split at the quadrant boundaries, emitted forward:
   the first point (at a1) reaches
   the path through moveto-or-lineto and each segment through curveto,
   leaving the current point at a2 as the language requires. The
   scheduled operators transform the user-space coordinates through the
   CTM as they run; the exec stack runs last-pushed first, so segments
   are pushed in reverse. */
static
int _arc_append(Xpost_Context *ctx,
                double cx, double cy, double rr,
                double a1, double a2, int dir)
{
    double segs[66][2];
    double cur, b;
    int n = 0, i;
    real sx, sy;

    if (dir > 0)
        while (a2 < a1) a2 += 360;
    else
        while (a2 > a1) a2 -= 360;
    /* a runaway sweep would otherwise fill the operand stack a segment
       at a time; sixteen revolutions is beyond any drawing's use */
    if (fabs(a2 - a1) > 5760)
        a2 = a1 + dir * 5760;

    /* snap an endpoint that lands a hair off a quadrant boundary onto it: the
       tangent-point arithmetic behind arct/arcto leaves a right-angle corner's
       start/end angle a shade off 90n, which would otherwise flip a floor() and
       emit a full quadrant plus a sub-ulp sliver instead of one segment. The
       tolerance is set from single-precision reals (typedef float real): one ulp
       of a degree value near 90 is 90*2^-23 ~= 7.6e-6, so an angle within 1e-5
       (just over one ulp) of 90n is a right angle blurred by float rounding, not
       a distinct span. Anything a full ulp or more away is left to split. */
    {
        double q1 = floor(a1 / 90.0 + 0.5) * 90.0;
        double q2 = floor(a2 / 90.0 + 0.5) * 90.0;
        if (fabs(a1 - q1) < 1e-5) a1 = q1;
        if (fabs(a2 - q2) < 1e-5) a2 = q2;
    }
    cur = a1;
    b = dir > 0 ? (floor(a1 / 90) + 1) * 90 : (ceil(a1 / 90) - 1) * 90;
    /* tolerate a hair of floating-point slop at a quadrant boundary: a span
       that ends exactly on 90n (a right-angle corner, common from arct) must
       be one segment, not a full quadrant plus a sub-microdegree sliver */
    while (n < 65 && (dir > 0 ? b < a2 - 1e-6 : b > a2 + 1e-6))
    {
        segs[n][0] = cur; segs[n][1] = b; n++;
        cur = b;
        b += dir * 90;
    }
    if (fabs(a2 - cur) > 1e-6)
    {
        segs[n][0] = cur; segs[n][1] = a2; n++;
    }

    for (i = n; i-- > 0; )
        _arcbezseg(ctx, cx, cy, rr, segs[i][0], segs[i][1]);
    sx = (real)(cx + rr * cos(a1 * RAD_PER_DEG));
    sy = (real)(cy + rr * sin(a1 * RAD_PER_DEG));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(sx));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(sy));

    for (i = 0; i < n; i++)
        xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_curveto_opcode));
    /* The context's own, not the file static. Every context that starts
       installs the operators again and rebuilds the static, so the static
       names whichever context initialised last -- while each context roots
       the one IT built. Running the static therefore ran an array that the
       running context did not root, and that nothing roots at all once the
       context which built it goes away. They are the same object in a run
       with one context, which is why this held up. */
    xpost_stack_push(ctx->lo, ctx->es,
                     xpost_object_get_type(ctx->arcstartproc) == arraytype
                     ? ctx->arcstartproc : _arc_start_proc);
    return 0;
}

static
int _arc(Xpost_Context *ctx,
         Xpost_Object x, Xpost_Object y, Xpost_Object r,
         Xpost_Object angle1, Xpost_Object angle2)
{
    return _arc_append(ctx, x.real_.val, y.real_.val, r.real_.val,
                       angle1.real_.val, angle2.real_.val, 1);
}

static
int _arcn(Xpost_Context *ctx,
          Xpost_Object x, Xpost_Object y, Xpost_Object r,
          Xpost_Object angle1, Xpost_Object angle2)
{
    return _arc_append(ctx, x.real_.val, y.real_.val, r.real_.val,
                       angle1.real_.val, angle2.real_.val, -1);
}

/* Shared arct/arcto continuation, entered with the five operands and
   the current point, delivered in user space by the scheduled
   currentpoint. The arc has radius r and is tangent to the ray from
   the current point toward (x1,y1) and to the ray from (x1,y1) toward
   (x2,y2): its centre sits on the corner's angle bisector at
   r/sin(half-angle) and the tangent points at r*cos/sin(half-angle)
   out along each ray, with the sweep direction given by the sign of
   the rays' cross product. The straight segment from the current
   point to the first tangent point falls out of _arc_append, whose
   start proc linetos on a non-empty path. When report is set (arcto)
   the four tangent coordinates are pushed before the arc's operands:
   the scheduled arc consumes its own from above, leaving them as the
   operator's results. */
static
int _arct_common(Xpost_Context *ctx,
                 double x1, double y1, double x2, double y2, double r,
                 double x0, double y0, int report)
{
    double dx0, dy0, dx2, dy2, sql0, sql2, cross;
    double ux0, uy0, ux2, uy2, sinhalf, coshalf, tanlen, bx, by, blen;
    double cx, cy, t1x, t1y, t2x, t2y, a1, a2;

    if (r < 0)
        return rangecheck;
    dx0 = x0 - x1; dy0 = y0 - y1;
    dx2 = x2 - x1; dy2 = y2 - y1;
    sql0 = dx0 * dx0 + dy0 * dy0;
    sql2 = dx2 * dx2 + dy2 * dy2;
    if (sql0 == 0.0 || sql2 == 0.0)
        return undefinedresult; /* a tangent ray has no direction */
    cross = dx0 * dy2 - dy0 * dx2;
    if (cross == 0.0 || r == 0.0)
    {
        /* collinear rays or a radius of zero: both tangent points
           collapse onto the corner, the arc is empty and only the
           straight segment survives */
        if (report)
        {
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)x1));
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)y1));
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)x1));
            xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)y1));
        }
        return _lineto(ctx, xpost_real_cons((real)x1), xpost_real_cons((real)y1));
    }
    ux0 = dx0 / sqrt(sql0); uy0 = dy0 / sqrt(sql0);
    ux2 = dx2 / sqrt(sql2); uy2 = dy2 / sqrt(sql2);
    /* half-angle identities on the rays' dot product; the clamps keep
       rounding from pushing the square roots' arguments negative */
    {
        double cosq = ux0 * ux2 + uy0 * uy2;
        if (cosq > 1.0) cosq = 1.0;
        if (cosq < -1.0) cosq = -1.0;
        sinhalf = sqrt((1.0 - cosq) / 2.0); /* nonzero: not collinear */
        coshalf = sqrt((1.0 + cosq) / 2.0);
    }
    tanlen = r * coshalf / sinhalf;
    /* the unnormalised bisector u0+u2 has length 2*cos(half-angle) */
    bx = ux0 + ux2; by = uy0 + uy2;
    blen = sqrt(bx * bx + by * by);
    cx = x1 + bx / blen * (r / sinhalf);
    cy = y1 + by / blen * (r / sinhalf);
    t1x = x1 + ux0 * tanlen; t1y = y1 + uy0 * tanlen;
    t2x = x1 + ux2 * tanlen; t2y = y1 + uy2 * tanlen;
    a1 = atan2(t1y - cy, t1x - cx) / RAD_PER_DEG;
    a2 = atan2(t2y - cy, t2x - cx) / RAD_PER_DEG;
    if (report)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)t1x));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)t1y));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)t2x));
        xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons((real)t2y));
    }
    return _arc_append(ctx, cx, cy, r, a1, a2, cross < 0.0 ? 1 : -1);
}

static
int _arct(Xpost_Context *ctx,
          Xpost_Object x1, Xpost_Object y1,
          Xpost_Object x2, Xpost_Object y2,
          Xpost_Object r)
{
    xpost_stack_push(ctx->lo, ctx->os, x1);
    xpost_stack_push(ctx->lo, ctx->os, y1);
    xpost_stack_push(ctx->lo, ctx->os, x2);
    xpost_stack_push(ctx->lo, ctx->os, y2);
    xpost_stack_push(ctx->lo, ctx->os, r);
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_arct_cont_opcode));
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_currentpoint_opcode));
    return 0;
}

static
int _arct_cont(Xpost_Context *ctx,
               Xpost_Object x1, Xpost_Object y1,
               Xpost_Object x2, Xpost_Object y2,
               Xpost_Object r,
               Xpost_Object x0, Xpost_Object y0)
{
    return _arct_common(ctx, x1.real_.val, y1.real_.val,
                        x2.real_.val, y2.real_.val, r.real_.val,
                        x0.real_.val, y0.real_.val, 0);
}

static
int _arcto(Xpost_Context *ctx,
           Xpost_Object x1, Xpost_Object y1,
           Xpost_Object x2, Xpost_Object y2,
           Xpost_Object r)
{
    xpost_stack_push(ctx->lo, ctx->os, x1);
    xpost_stack_push(ctx->lo, ctx->os, y1);
    xpost_stack_push(ctx->lo, ctx->os, x2);
    xpost_stack_push(ctx->lo, ctx->os, y2);
    xpost_stack_push(ctx->lo, ctx->os, r);
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_arcto_cont_opcode));
    xpost_stack_push(ctx->lo, ctx->es, xpost_operator_cons_opcode(_currentpoint_opcode));
    return 0;
}

static
int _arcto_cont(Xpost_Context *ctx,
                Xpost_Object x1, Xpost_Object y1,
                Xpost_Object x2, Xpost_Object y2,
                Xpost_Object r,
                Xpost_Object x0, Xpost_Object y0)
{
    return _arct_common(ctx, x1.real_.val, y1.real_.val,
                        x2.real_.val, y2.real_.val, r.real_.val,
                        x0.real_.val, y0.real_.val, 1);
}

#define NUM(x) (xpost_object_get_type(x)==realtype?x.real_.val:(real)x.int_.val)
/* destination for flattening: appends must be able to re-seat
   /currpath in the graphics state when the path string grows */
typedef struct
{
    Xpost_Object gstate;
    Xpost_Object path;
    long segments; /* line segments emitted so far, to bound a flatten of a
                      curve whose extent dwarfs the device: the depth cap
                      bounds the stack, this bounds the work */
} _flatten_dst;

/* The most line segments one flatten may emit before it is refused. A real
   curve flattens to a few hundred, and the most intricate path a fill ever
   hands over runs to some tens of thousands; a curve whose control points
   started enormous asks instead for astronomically many tiny off-device
   segments, which the depth cap alone still lets pile up -- and which the
   fill clips away against the page as soon as they are made, so the whole
   pile is built only to be discarded. The narrow build already refuses
   such a path when the flattened points outrun a composite's length; this
   is the same refusal for the wide build, whose composites are far longer,
   set well above anything a genuine path needs and far below the half a
   gigabyte and seconds the unbounded pile cost. */
#define FLATTEN_MAX_SEGMENTS 131072L

/* The deepest _chopcurve will subdivide. A curve converges to the
   flatness tolerance in a handful of levels; far more than this means the
   sub-curve has shrunk to where floating-point rounding keeps its
   flatness measure oscillating around the tolerance and it never comes
   under -- which a curve whose control points started enormous does, and
   used to recurse the C stack away. At the cap, take the sub-curve as a
   line: it is smaller than the tolerance in every case that matters. */
#define CHOPCURVE_MAX_DEPTH 32

/* Subdivides a curve until each piece is flat enough to be a line, and
   emits the lines. Depth is capped rather than trusted to the flatness
   measure, for the reason above. */
static
int _chopcurve(Xpost_Context *ctx, _flatten_dst *dst,
               real x0, real y0,
               real x1, real y1,
               real x2, real y2,
               real x3, real y3,
               real tol,
               int depth)
{
    real x01, y01, x12, y12, x23, y23,
         x012, y012, x123, y123,
         x0123, y0123;
    real ux, uy, vx, vy, u2, v2, lim2;

    /* A non-finite control point never converges: the subdivision test
       below compares a measure that stays infinite (or NaN) against the
       tolerance, so it is never met and the recursion runs until the C
       stack is gone. Such a coordinate reached the path through an
       ordinary curveto of an out-of-range value (e.g. an overflowing
       multiply that produced an infinity); refuse to flatten it rather
       than subdivide it forever. */
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)
     || !isfinite(x2) || !isfinite(y2) || !isfinite(x3) || !isfinite(y3))
        return limitcheck;

    /* How far the line from P0 to P3 can stray from the curve it stands
       in for. Subtracting the line from the curve leaves

           B(t) - L(t) = (1-t)^2 t u + (1-t) t^2 v,
           u = 3 P1 - 2 P0 - P3,   v = 3 P2 - P0 - 2 P3,

       and since (1-t)t never exceeds 1/4 while the remaining bracket is a
       weighted average of u and v, no point of the curve lies further than
       max(|u|,|v|)/4 from the point of the line at the same parameter.
       PLRM 8.2 (setflat) makes flatness precisely that quantity -- "the
       maximum allowable distance of any point of the approximation from
       the corresponding point on the true curve, measured in output device
       pixels" -- so a piece admitted here meets the tolerance along its
       whole length, and not merely where it was sampled.

       Both control points enter, and each of them against the chord.
       Measuring instead at one point of the curve cannot bound the rest of
       it, and is blind besides to an entire family of curves: the gap
       between the curve's midpoint and the chord's is |u+v|/8, which
       vanishes identically whenever v = -u, that is for every curve whose
       control points sit symmetrically about the middle of its chord --
       curves that leave the chord as far as any.

       Compared as squares, to keep a square root out of the innermost step
       of every flatten, and each of the two held against the limit in its
       own right rather than through a larger-of: an intermediate that
       overflowed to an infinity or to a NaN then fails its own comparison
       whichever of the two it is, and the piece is subdivided. That is the
       safe way to be wrong -- the halves carry smaller coordinates, and
       the depth cap bounds the descent regardless. */
    ux = (real)(3.0 * x1 - 2.0 * x0 - x3);
    uy = (real)(3.0 * y1 - 2.0 * y0 - y3);
    vx = (real)(3.0 * x2 - x0 - 2.0 * x3);
    vy = (real)(3.0 * y2 - y0 - 2.0 * y3);
    u2 = ux * ux + uy * uy;
    v2 = vx * vx + vy * vy;
    lim2 = (real)(4.0 * tol);
    lim2 = lim2 * lim2;

    if (depth >= CHOPCURVE_MAX_DEPTH || (u2 < lim2 && v2 < lim2))
    {
        real co[2];
        if (++dst->segments > FLATTEN_MAX_SEGMENTS)
            return limitcheck;
        co[0] = x3;
        co[1] = y3;
        return _path_append(ctx, dst->gstate, &dst->path, PATH_CMD_LINE, co, 2);
    }
    else
    {
        int ret;

#define MEDIAN(x, y, xA, yA, xB, yB) \
    x = (real)(((xA)+(xB))/2.0); \
    y = (real)(((yA)+(yB))/2.0);

        MEDIAN(x01, y01, x0, y0, x1, y1)
        MEDIAN(x12, y12, x1, y1, x2, y2)
        MEDIAN(x23, y23, x2, y2, x3, y3)
        MEDIAN(x012, y012, x01, y01, x12, y12)
        MEDIAN(x123, y123, x12, y12, x23, y23)
        MEDIAN(x0123, y0123, x012, y012, x123, y123)

        ret = _chopcurve(ctx, dst, x0, y0, x01, y01, x012, y012, x0123, y0123,
                         tol, depth + 1);
        if (ret)
            return ret;
        return _chopcurve(ctx, dst, x0123, y0123, x123, y123, x23, y23, x3, y3,
                          tol, depth + 1);
    }
}

static
int _flattenpath (Xpost_Context *ctx)
{
    Xpost_Object gstate, flat;
    Xpost_Object path;
    _flatten_dst dst;
    char *p;
    unsigned int used, o, esz;
    real cp[2] = { 0, 0 };
    real tol;
    int curved = 0;
    int ret;

    gstate = _gstate(ctx);
    if (xpost_object_get_type(gstate) == invalidtype)
        return undefined;
    flat = xpost_dict_get(ctx, gstate, nameflat);
    /* the flatness value bounds the error in device pixels; subdivide
       well inside it so a curve's polygonization classifies the same
       boundary pixels as a renderer that meets the bound exactly.

       setflat clamps flatness to [0.2, 100], but it is not the only
       writer: the value is read here straight from the graphics-state
       dictionary, which a program can reach and set to zero (or NaN)
       by another path. A non-positive tolerance makes the subdivision
       test below never true, so _chopcurve recurses until the C stack
       is gone. Enforce the same floor at the point the value is used,
       so the reader is safe whatever the dictionary holds. */
    {
        real f = NUM(flat);
        if (!(f >= 0.2)) f = 0.2;          /* also catches NaN */
        else if (f > 100.0) f = 100.0;
        tol = f * 0.25;
    }

    path = xpost_dict_get(ctx, gstate, namecurrpath);
    if (xpost_object_get_type(path) != stringtype)
        return unregistered;
    if (!_path_extent(ctx, path, &p, &used))
        return rangecheck;

    /* a path without curves is already flat: leave it untouched
       rather than rebuild an identical copy */
    for (o = PATH_HDR; o < used; o += esz)
    {
        if (PATH_CMD(p, o) > PATH_CMD_CLOSE)
            return unregistered;
        esz = _path_elem_size(p[o]);
        if (esz > used - o)
            return rangecheck;
        if (p[o] == PATH_CMD_CURVE)
        {
            curved = 1;
            break;
        }
    }
    if (!curved)
        return 0;

    xpost_stack_push(ctx->lo, ctx->hold, path);
    ret = _newpath(ctx);
    if (ret)
        return ret;
    /* _newpath allocates, so the source string may have moved -- refresh the
       pointer before the loop reads it, as the loop already does after each
       append below */
    p = xpost_string_get_pointer(ctx, path);
    dst.gstate = gstate;
    dst.path = xpost_dict_get(ctx, gstate, namecurrpath);
    dst.segments = 0;

    for (o = PATH_HDR; o < used; o += esz)
    {
        int cmd = p[o];
        real co[6];

        if (cmd < PATH_CMD_MOVE || cmd > PATH_CMD_CLOSE)
            return unregistered;
        esz = _path_elem_size(cmd);
        if (esz > used - o)
            return rangecheck;
        _path_get_coords(p, o, co, cmd == PATH_CMD_CURVE ? 6 : 2);
        if (cmd == PATH_CMD_CURVE)
        {
            ret = _chopcurve(ctx, &dst,
                             cp[0], cp[1],
                             co[0], co[1], co[2], co[3], co[4], co[5],
                             tol, 0);
            if (ret)
                return ret;
            cp[0] = co[4];
            cp[1] = co[5];
        }
        else
        {
            ret = _path_append(ctx, dst.gstate, &dst.path, cmd, co, 2);
            if (ret)
                return ret;
            cp[0] = co[0];
            cp[1] = co[1];
        }
        /* appends allocate: the source string may have moved */
        p = xpost_string_get_pointer(ctx, path);
    }

    return 0;
}


/* -  .newformserial  int
   The serial a form's held drawing is keyed by. The counter only ever
   moves forward, so no two forms of a run are named alike and a drawing
   cached against a serial cannot be taken for a later form that happened
   to be given the same number.

   A counter kept in virtual memory could not promise that. The number is
   stamped into the form dictionary, which is the program's and may be in
   either bank, while the counter would be in one of them: a restore past
   the execform that raised it winds the counter back, a dictionary the
   restore does not reach carries the old number forward, and the next
   form is issued the number that dictionary is still holding. The two
   then name one entry and whichever was captured first is painted for
   both. Nothing reports that; it is a wrong shape on a page. The counter
   is therefore the interpreter's own, as the glyph masks' is
   (.newfontserial), and the bank the form dictionary lives in stops
   mattering.

   It lives here because the cache it keys lives in graphicsdict, which
   is the dictionary this file reaches. The cache itself stays in virtual
   memory and is wound back by a restore; that costs a re-capture and
   never a wrong drawing, because the serial it was filed under is not
   issued again. */
static int _form_serial_next = 1;

static int _newformserial(Xpost_Context *ctx)
{
    if (_form_serial_next <= 0)
    {
        /* the counter has run its range. What is held cannot be told
           apart from what the reissued numbers will name, so the cache
           has to be given up -- which is done by the caller, in the
           dictionary the cache lives in, since nothing here can reach
           it. Restarting at a number below the one last handed out is
           what tells the caller that. */
        _form_serial_next = 1;
    }
    if (!xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(_form_serial_next)))
        return stackoverflow;
    _form_serial_next++;
    return 0;
}


/* Installs the path operators and the opcodes the continuations above
   are pushed by. An opcode is taken here rather than looked up later
   because the continuation is put on the execution stack from inside
   an operator, where a name lookup could fail and there would be
   nowhere to report it. */
int xpost_oper_init_path_ops(Xpost_Context *ctx,
                             Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op,pathempty_op;
    int ret;

    assert(ctx->gl->base);

    _gstate_cached = 0;
    _gstate_cache_id = 0;

    if (xpost_object_get_type((namegraphicsdict = xpost_name_cons(ctx, ".graphicsdict"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namecurrgstate = xpost_name_cons(ctx, "currgstate"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namecurrpath = xpost_name_cons(ctx, "currpath"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameclipregion = xpost_name_cons(ctx, "clipregion"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((nameflat = xpost_name_cons(ctx, "flat"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namecurrmatrix = xpost_name_cons(ctx, "currmatrix"))) == invalidtype)
        return VMerror;


    op = xpost_operator_cons(ctx, "newpath", (Xpost_Op_Func)_newpath, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "currentpoint", (Xpost_Op_Func)_currentpoint, 0);
    _currentpoint_opcode = op.mark_.padw;
    INSTALL;

    op = xpost_operator_cons(ctx, ".pathbox", (Xpost_Op_Func)_pathboxget, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".setpathbox", (Xpost_Op_Func)_pathboxput, 4,
                             floattype, floattype, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "setbbox", (Xpost_Op_Func)_setbbox, 4,
                             floattype, floattype, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "moveto", (Xpost_Op_Func)_moveto, 2, floattype, floattype);
    _moveto_opcode = op.mark_.padw;
    INSTALL;

    op = xpost_operator_cons(ctx, "rmoveto", (Xpost_Op_Func)_rmoveto, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "rmoveto_cont", (Xpost_Op_Func)_rmoveto_cont, 4,
                             floattype, floattype, floattype, floattype);
    _rmoveto_cont_opcode = op.mark_.padw;

    op = xpost_operator_cons(ctx, "lineto", (Xpost_Op_Func)_lineto, 2, floattype, floattype);
    _lineto_opcode = op.mark_.padw;
    INSTALL;

    op = xpost_operator_cons(ctx, "rlineto", (Xpost_Op_Func)_rlineto, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "rlineto_cont", (Xpost_Op_Func)_rlineto_cont, 4,
                             floattype, floattype, floattype, floattype);
    _rlineto_cont_opcode = op.mark_.padw;

    op = xpost_operator_cons(ctx, "curveto", (Xpost_Op_Func)_curveto, 6,
                             floattype, floattype, floattype, floattype, floattype, floattype);
    _curveto_opcode = op.mark_.padw;
    INSTALL;

    op = xpost_operator_cons(ctx, "rcurveto", (Xpost_Op_Func)_rcurveto, 6,
                             floattype, floattype, floattype, floattype, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "rcurveto_cont", (Xpost_Op_Func)_rcurveto_cont, 8,
                             floattype, floattype, floattype, floattype, floattype, floattype, floattype, floattype);
    _rcurveto_cont_opcode = op.mark_.padw;

    op = xpost_operator_cons(ctx, "closepath", (Xpost_Op_Func)_closepath, 0);
    INSTALL;

    op = xpost_operator_cons(ctx, ".cliprect", (Xpost_Op_Func)_cliprect, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".pathisrect", (Xpost_Op_Func)_pathisrect, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".clipcuts", (Xpost_Op_Func)_clipcutsop, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".cliptrivial", (Xpost_Op_Func)_cliptrivial, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".clipmisses", (Xpost_Op_Func)_clipmisses, 0);
    INSTALL;

    op = xpost_operator_cons(ctx, ".fillpolyargs", (Xpost_Op_Func)_fillpolyargs, 0);
    INSTALL;

    op = xpost_operator_cons(ctx, ".tripoly", (Xpost_Op_Func)_tripoly, 3,
                             arraytype, arraytype, arraytype);
    INSTALL;

    op = xpost_operator_cons(ctx, ".trishade", (Xpost_Op_Func)_trishade, 5,
                             arraytype, arraytype, arraytype, arraytype,
                             dicttype);
    INSTALL;

    op = xpost_operator_cons(ctx, ".newpathstr", (Xpost_Op_Func)_newpathstr, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".newformserial", (Xpost_Op_Func)_newformserial, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".pathempty", (Xpost_Op_Func)_pathempty, 0);
    INSTALL;
    pathempty_op = op;   /* baked into _arc_start_proc below, so the arc
                            machinery reaches it after it relocates off systemdict */
    op = xpost_operator_cons(ctx, ".pathwalksteps", (Xpost_Op_Func)_pathwalksteps, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".devmoveto", (Xpost_Op_Func)_devmoveto, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".devlineto", (Xpost_Op_Func)_devlineto, 2, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".devcurveto", (Xpost_Op_Func)_devcurveto, 6,
                             floattype, floattype, floattype, floattype, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".retagclose", (Xpost_Op_Func)_retagclose, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".copypath", (Xpost_Op_Func)_copypath, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".pathnext", (Xpost_Op_Func)_pathnext, 2, stringtype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "pathbbox", (Xpost_Op_Func)_pathbbox, 0);
    INSTALL;

    op = xpost_operator_cons(ctx, "arc", (Xpost_Op_Func)_arc, 5,
                             floattype, floattype, floattype, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "arcn", (Xpost_Op_Func)_arcn, 5,
                             floattype, floattype, floattype, floattype, floattype);
    INSTALL;

    op = xpost_operator_cons(ctx, "arct", (Xpost_Op_Func)_arct, 5,
                             floattype, floattype, floattype, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "arct_cont", (Xpost_Op_Func)_arct_cont, 7,
                             floattype, floattype, floattype, floattype, floattype, floattype, floattype);
    _arct_cont_opcode = op.mark_.padw;

    op = xpost_operator_cons(ctx, "arcto", (Xpost_Op_Func)_arcto, 5,
                             floattype, floattype, floattype, floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "arcto_cont", (Xpost_Op_Func)_arcto_cont, 7,
                             floattype, floattype, floattype, floattype, floattype, floattype, floattype);
    _arcto_cont_opcode = op.mark_.padw;

    op = xpost_operator_cons(ctx, "flattenpath", (Xpost_Op_Func)_flattenpath, 0);
    INSTALL;

    op = xpost_operator_cons(ctx, ".pdffillpath", (Xpost_Op_Func)_pdffillpath, 1,
            dicttype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".svgfillpath", (Xpost_Op_Func)_svgfillpath, 4,
            numbertype, numbertype, numbertype, dicttype);
    INSTALL;

    /* The procedure that reaches an arc's starting point holds the three
       operators it runs. A name would resolve through the dictionary stack
       when the arc ran, so a program's moveto, lineto or ifelse would take
       over the inside of arc, arcn, arct and arcto. */
    _arc_start_proc = xpost_array_cons(ctx, 4);
    ret = xpost_array_put(ctx, _arc_start_proc, 0, pathempty_op);
    if (ret)
        return ret;
    {
        Xpost_Object true_clause = xpost_object_cvx(xpost_array_cons(ctx, 1));
        ret = xpost_array_put(ctx, true_clause, 0,
                              XPOST_OP(ctx, moveto));
        if (ret)
            return ret;
        ret = xpost_array_put(ctx, _arc_start_proc, 1, true_clause);
        if (ret)
            return ret;
    }
    {
        Xpost_Object false_clause = xpost_object_cvx(xpost_array_cons(ctx, 1));
        ret = xpost_array_put(ctx, false_clause, 0,
                              XPOST_OP(ctx, lineto));
        if (ret)
            return ret;
        ret = xpost_array_put(ctx, _arc_start_proc, 2, false_clause);
        if (ret)
            return ret;
    }
    ret = xpost_array_put(ctx, _arc_start_proc, 3,
                          XPOST_OP(ctx, opifelse));
    if (ret)
        return ret;

    /* Read-only from here on. It is an array the machinery executes,
       living in global virtual memory, which is the property the lockdown
       sweep refuses everywhere it can see -- and it cannot see this one:
       no dictionary holds it, so no walk from a namespace reaches it. It
       is finished at this point and nothing writes it again, so it is
       sealed where it is made rather than left to a sweep that cannot
       arrive. Execute-only, like every other body the machinery runs:
       the sweep closes those to reading too, and a body reached through
       a variable rather than a dictionary is no different for being
       out of the sweep's way. */
    _arc_start_proc = xpost_object_set_access(ctx, _arc_start_proc,
                                              XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY);

    /* The procedure is held in a variable of this file, which the
       collector does not walk. It is made while the operators are being
       installed, before there is any dictionary to keep it in, so the
       variable is rooted instead: the collector marks whatever it holds
       from then on. */
    ctx->arcstartproc = _arc_start_proc;
    return 0;
}
