/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_type.c
 * @brief Installs the type and access operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * type cvi cvr cvn cvs cvrs cvx cvlit xcheck
 * readonly executeonly noaccess rcheck wcheck
 *
 * An object carries its type and its access attributes in its tag, so both
 * halves of this group read and write the same few bits.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h> /* NULL strtod */
#include <stddef.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "xpost.h"
#include "xpost_compat.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_dict.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_type.h"
#include "xpost_op_token.h"

/* any  type  name
   return type of any as a nametype object */
static
int Atype(Xpost_Context *ctx,
          Xpost_Object o)
{
    const char *name = xpost_op_type_name(xpost_op_type_index(o));

    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvx(xpost_name_cons(ctx, name)));
    return 0;
}

int xpost_op_type_code(Xpost_Context *ctx, Xpost_Object name)
{
    Xpost_Object str = xpost_name_get_string(ctx, name);
    char buf[64];
    unsigned int len;
    unsigned int i;

    if (xpost_object_get_type(str) != stringtype)
        return -1;
    len = str.comp_.sz;
    if (len >= sizeof buf)
        return -1;
    memcpy(buf, xpost_string_get_pointer(ctx, str), len);
    buf[len] = '\0';

    if (strcmp(buf, "anytype") == 0)
        return anytype;
    if (strcmp(buf, "numbertype") == 0)
        return numbertype;
    if (strcmp(buf, "floattype") == 0)
        return floattype;
    if (strcmp(buf, "proctype") == 0)
        return proctype;
    for (i = 0; i < XPOST_OBJECT_NTYPES; i++)
        if (strcmp(buf, xpost_object_type_names[i]) == 0)
            return (int)i;
    return -1;
}

/* obj   cvlit  obj
   set executable attribute in obj to literal (quoted) */
static
int Acvlit(Xpost_Context *ctx,
           Xpost_Object o)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(o));
    return 0;
}

/* obj  cvx  obj
   set executable attribute in obj to executable */
static
int Acvx(Xpost_Context *ctx,
         Xpost_Object o)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvx(o));
    return 0;
}

/* obj  xcheck  bool
   test executable attribute in obj */
static
int Axcheck(Xpost_Context *ctx,
            Xpost_Object o)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(xpost_object_is_exe(o)));
    return 0;
}

/* The four accesses are rungs of one ladder, in increasing order of
   permissiveness: none, execute-only, read-only, unlimited (PLRM 3.3.2
   "Access"). readonly, executeonly and noaccess each reduce an object to
   the rung they name, and access moves only down that ladder, never up
   (PLRM 3.6.6). Asking for the rung an object already sits on is nothing
   to do, and it is answered unchanged.

   Asking for a rung it has already passed is a widening, and what that
   should do depends on the type. An array or a string carries its access
   on the object, so the request is about that one reference and can be
   answered on its own terms: it is refused, which is why PLRM 8.2 lists
   invalidaccess among the errors of these operators at all. A dictionary
   carries its access on its value, so every reference to it shares one
   access and no widening could be confined to the asker -- it would
   reopen the dictionary underneath every other holder, the one that
   sealed it included. Since the widening cannot happen either way, the
   dictionary is answered unchanged rather than refused. That is the
   reading the specification leaves open; it also keeps the request off
   virtual memory, since setting a dictionary's access writes its value,
   which a standing save level must back up first and can be refused room
   for.

   A file reaches none of this. It sits on no rung, and each of the three
   operators sends one to its own rule before consulting the ladder. */
static
Xpost_Object _reduce_access(Xpost_Context *ctx,
                            Xpost_Object o,
                            Xpost_Object_Tag_Access access)
{
    if (xpost_object_get_access(ctx, o) <= access)
        return o;
    return xpost_object_set_access(ctx, o, access);
}

/* Whether the rung asked for is above the one the object sits on, for an
   object whose access the operators refuse to widen. A dictionary is
   excluded for the reason above; a file, as above, never arrives. noaccess
   never asks this: no-access is the foot of the ladder, so nothing sits
   below it. */
static
int _widens_access(Xpost_Context *ctx,
                   Xpost_Object o,
                   Xpost_Object_Tag_Access access)
{
    if (xpost_object_get_type(o) == dicttype)
        return 0;
    return xpost_object_get_access(ctx, o) < access;
}

/* A file's access is not a rung of that ladder but a set of independent
   capabilities, settled when the file is opened by the access string it
   was opened with: a file opened for writing may be written and not read
   (PLRM 3.8.1), which no rung says. So the three operators name a set for
   a file rather than a rung, and each names the same set whatever it is
   applied to -- read-only is read and execute, execute-only is execute,
   no-access is nothing.

   Access still only ever reduces (PLRM 3.6.6), and for a set that means
   the set asked for must be one the file already grants throughout: a
   capability the file does not have is not one an operator may hand it.
   A request for one is a widening and is refused, so readonly and
   executeonly both refuse a file opened only for writing -- neither
   reading nor executing is a thing that file was ever able to do -- while
   noaccess asks for nothing and is always a reduction. */
static
Xpost_Object_Tag_Access _file_access_asked(Xpost_Object_Tag_Access rung)
{
    switch (rung)
    {
        case XPOST_OBJECT_TAG_ACCESS_READ_ONLY:
            return XPOST_OBJECT_TAG_ACCESS_FILE_READ
                 | XPOST_OBJECT_TAG_ACCESS_FILE_EXEC;
        case XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY:
            return XPOST_OBJECT_TAG_ACCESS_FILE_EXEC;
        default:
            return XPOST_OBJECT_TAG_ACCESS_NONE;
    }
}

static
int _reduce_file_access(Xpost_Context *ctx,
                        Xpost_Object *o,
                        Xpost_Object_Tag_Access rung)
{
    Xpost_Object_Tag_Access has = xpost_object_get_access(ctx, *o);
    Xpost_Object_Tag_Access asked = _file_access_asked(rung);

    if (asked & ~has)
        return invalidaccess;
    *o = xpost_object_set_access(ctx, *o, asked);
    return 0;
}

/* obj  executeonly  obj
   reduce access attribute for obj to execute-only */
static
int Aexecuteonly(Xpost_Context *ctx,
                 Xpost_Object o)
{
    word type = xpost_object_get_type(o);
    /* executeonly applies to arrays, strings and files, not dictionaries */
    if (type != arraytype && type != stringtype && type != filetype)
        return typecheck;
    if (type == filetype)
    {
        int ret = _reduce_file_access(ctx, &o,
                                      XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY);
        if (ret) return ret;
    }
    else
    {
        if (_widens_access(ctx, o, XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY))
            return invalidaccess;
        o = _reduce_access(ctx, o, XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY);
    }
    xpost_stack_push(ctx->lo, ctx->os, o);
    return 0;
}

/* obj  noaccess  obj
   reduce access attribute for obj to no-access */
static
int Anoaccess(Xpost_Context *ctx,
              Xpost_Object o)
{
    /* noaccess applies to composite objects and files */
    if (!xpost_object_is_composite(o) && xpost_object_get_type(o) != filetype)
        return typecheck;
    /* A dictionary carries its access on the value rather than on the
       object, so every reference to that dictionary loses access at once
       -- including the interpreter's own. A read-only dictionary is
       therefore not something a program may take the rest of the way to
       no-access: the language refuses exactly that step (PLRM). A dictionary
       that is still writable may be reduced, and one already at no-access
       may be reduced again to no effect; it is the read-only rung that is
       refused. An array, a string or a file carries the access on the
       object, so reducing one leaves every other reference to the same
       value alone, and needs no such refusal. */
    if (xpost_object_get_type(o) == dicttype &&
        xpost_object_get_access(ctx, o) == XPOST_OBJECT_TAG_ACCESS_READ_ONLY)
        return invalidaccess;
    if (xpost_object_get_type(o) == filetype)
    {
        int ret = _reduce_file_access(ctx, &o, XPOST_OBJECT_TAG_ACCESS_NONE);
        if (ret) return ret;
    }
    else
        o = _reduce_access(ctx, o, XPOST_OBJECT_TAG_ACCESS_NONE);
    /* a dictionary's access is part of its value, so reducing it writes
       virtual memory and can be declined room for the backup a standing
       save level needs */
    if (xpost_object_get_type(o) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, o);
    return 0;
}

/* obj  readonly  obj
   reduce access attribute for obj to read-only */
static
int Areadonly(Xpost_Context *ctx,
              Xpost_Object o)
{
    /* readonly applies to composite objects and files */
    if (!xpost_object_is_composite(o) && xpost_object_get_type(o) != filetype)
        return typecheck;
    if (xpost_object_get_type(o) == filetype)
    {
        int ret = _reduce_file_access(ctx, &o,
                                      XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
        if (ret) return ret;
    }
    else
    {
        if (_widens_access(ctx, o, XPOST_OBJECT_TAG_ACCESS_READ_ONLY))
            return invalidaccess;
        o = _reduce_access(ctx, o, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
    }
    /* as noaccess above: a dictionary's seal is a write to its value */
    if (xpost_object_get_type(o) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, o);
    return 0;
}

/* rcheck and wcheck ask after an object's access, and only the
   composite types carry one: array, packed array, dictionary, file and
   string. Any other operand is a typecheck (PLRM 8.2) rather than a
   verdict about an access it does not have. */
static
int _carries_access(Xpost_Object o)
{
    switch (xpost_object_get_type(o))
    {
        case arraytype: /*@fallthrough@*/
        case dicttype: /*@fallthrough@*/
        case filetype: /*@fallthrough@*/
        case stringtype:
            return 1;
        default:
            return 0;
    }
}

/* obj  rcheck  bool
   test obj for read-access

   Each answers with the same reading of the access that decides whether
   an operator reading or writing the object is allowed to proceed, so
   the verdict a program is given is the one it will meet. A file is why
   that matters: its access is a set of capabilities rather than a rung,
   and a file opened for writing grants write and not read, which the
   ladder's ordering cannot express in either direction. */
static
int Archeck(Xpost_Context *ctx,
            Xpost_Object o)
{
    if (!_carries_access(o))
        return typecheck;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_object_is_readable(ctx, o)));
    return 0;
}

/* obj  wcheck  bool
   test obj for write-access */
static
int Awcheck(Xpost_Context *ctx,
            Xpost_Object o)
{
    if (!_carries_access(o))
        return typecheck;
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_bool_cons(xpost_object_is_writeable(ctx, o)));
    return 0;
}

/* number  cvi  int
   convert number to integer */
static
/* Largest and smallest values the integer type holds, as doubles, for range
   checks; the ternary folds to a constant. */
#define XPOST_INTEGER_HI_D ((sizeof(integer) >= 8) ? 9223372036854775807.0 : 2147483647.0)
#define XPOST_INTEGER_LO_D ((sizeof(integer) >= 8) ? -9223372036854775808.0 : -2147483648.0)

int Ncvi(Xpost_Context *ctx,
         Xpost_Object n)
{
    if (xpost_object_get_type(n) == realtype)
    {
        double v = (double)n.real_.val;
        /* cvi truncates toward zero; PLRM raises rangecheck when the real is
           too large to represent as an integer */
        /* a NaN fails every comparison, so the range guard below lets it
           through to (integer)NaN, which is undefined; it has no integer
           to truncate to. (NaN reaches cvi from real overflow: inf - inf.) */
        if (isnan(v))
            return undefinedresult;
        if (v >= XPOST_INTEGER_HI_D + 1.0 || v <= XPOST_INTEGER_LO_D - 1.0)
            return rangecheck;
        n = xpost_int_cons((integer)v);
    }
    xpost_stack_push(ctx->lo, ctx->os, n);
    return 0;
}

/* a numeral token's end: white space, a delimiter, or the string's */
static
int _num_token_end(char c)
{
    return c == '\0' || c == ' ' || c == '\t' || c == '\n'
        || c == '\r' || c == '\f'
        || c == '(' || c == ')' || c == '<' || c == '>'
        || c == '[' || c == ']' || c == '{' || c == '}'
        || c == '/' || c == '%';
}

/* helper function: read a PostScript numeral (decimal or radix form)
   from the string with the scanner's token semantics -- leading
   white space skipped, the token must be a complete numeral, and
   whatever follows its end is ignored; returns 0 on success or an
   error code.

   A radix numeral denotes the integer of the same twos-complement
   representation (PLRM 3.2), which is a value no real carries: the top
   of the integer's field is a negative integer and a positive real of
   that magnitude both, and a numeral read into a real arrives as the
   second. So it is answered in the integer's own field, in *ival with
   *isint set, and the other forms are answered in *out as before. */
static
int _string_to_number(const char *t,
                      double *out,
                      integer *ival,
                      int *isint)
{
    char *end;
    double num;

    *isint = 0;
    *ival = 0;

    while (*t == ' ' || *t == '\t' || *t == '\n'
        || *t == '\r' || *t == '\f')
        t++;

    /* radix numeral, e.g. 16#ff, read by the scanner's own reader so that
       a numeral is the same numeral to token and to cvi. Text the reader
       refuses is not a radix numeral, and falls through to the decimal
       forms below, which end in the typecheck a name earns */
    {
        const char *rend;
        integer rnum = 0;
        int rret = xpost_scanner_radix_number(t, &rend, &rnum);

        if (rret >= 0)
        {
            /* the numeral ends where the scanner's token would end;
               anything else is a name */
            if (!_num_token_end(*rend))
                return typecheck;
            if (rret != 0)
                return rret;
            *isint = 1;
            *ival = rnum;
            *out = (double)*ival;
            return 0;
        }
    }

    errno = 0;
    num = strtod(t, &end);
    if (end == t || !_num_token_end(*end))
        return typecheck;
    if (errno == ERANGE)
        return limitcheck;
    /* strtod also accepts forms outside PostScript number syntax -- a C-style
       0x hexadecimal prefix and inf/nan; PostScript treats those as typecheck */
    {
        const char *p;
        for (p = t; p < end; p++)
            if (*p == 'x' || *p == 'X')
                return typecheck;
    }
    if (!(num == num) || num > 1e308 || num < -1e308)
        return typecheck;
    *out = num;
    return 0;
}

/* string  cvi  int
   convert string numeral to integer */
static
int Scvi(Xpost_Context *ctx,
         Xpost_Object s)
{
    double dbl;
    integer ival;
    int isint;
    int ret;
    char *t;

    /* the numeral is scanned out of the string's characters, so the string
       needs read access */
    if (!xpost_object_is_readable(ctx, s))
        return invalidaccess;

    t = xpost_string_allocate_cstring(ctx, s);
    ret = _string_to_number(t, &dbl, &ival, &isint);
    free(t);
    if (ret)
        return ret;
    /* a numeral already in the integer's field is the integer it denotes */
    if (isint)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ival));
        return 0;
    }
    /* a numeral that does not fit the integer type is rangecheck (PLRM cvi) */
    if (dbl >= XPOST_INTEGER_HI_D + 1.0 || dbl <= XPOST_INTEGER_LO_D - 1.0)
        return rangecheck;

    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons((integer)dbl));
    return 0;
}

/* string  cvn  name
   convert string to name */
static
int Scvn(Xpost_Context *ctx,
         Xpost_Object s)
{
    Xpost_Object name;

    /* the string's characters become the name, so it needs read access */
    if (!xpost_object_is_readable(ctx, s))
        return invalidaccess;

    /* the name is lexically the string, every byte of it: a nul is a
       name character, not a terminator */
    name = xpost_name_cons_n(ctx, xpost_string_get_pointer(ctx, s),
                             s.comp_.sz);
    if (xpost_object_get_type(name) == invalidtype)
        return VMerror;
    if (xpost_object_is_exe(s))
        name = xpost_object_cvx(name);
    else
        name = xpost_object_cvlit(name);
    xpost_stack_push(ctx->lo, ctx->os, name);
    return 0;
}

/* number  cvr  real
   convert number to real */
static
int Ncvr(Xpost_Context *ctx,
         Xpost_Object n)
{
    if (xpost_object_get_type(n) == integertype)
        n = xpost_real_cons((real)n.int_.val);
    xpost_stack_push(ctx->lo, ctx->os, n);
    return 0;
}

/* string  cvr  real
   convert string numeral to real */
static
int Scvr(Xpost_Context *ctx,
         Xpost_Object str)
{
    double num;
    integer ival;
    int isint;
    int ret;
    char *s;

    /* as cvi above: the numeral is read out of the string */
    if (!xpost_object_is_readable(ctx, str))
        return invalidaccess;

    s = xpost_string_allocate_cstring(ctx, str);
    ret = _string_to_number(s, &num, &ival, &isint);
    free(s);
    if (ret)
        return ret;

    /* the integer a radix numeral denotes, converted (PLRM cvr), rather
       than the magnitude its unsigned reading has */
    xpost_stack_push(ctx->lo, ctx->os,
                     xpost_real_cons(isint ? (real)ival : (real)num));
    return 0;
}

/* the digits of num under rad, written into s. num is unsigned: the
   caller has already decided what bit pattern is being rendered, and
   every value of it has digits. Answers the count written, or -1 for
   a string the digits do not fit. */
static
int conv_rad(dword num,
             unsigned int rad,
             char *s,
             int n)
{
    const char *vec = "0123456789" "ABCDEFGHIJKLM" "NOPQRSTUVWXYZ";
    char t[sizeof(dword) * 8]; /* radix two writes one digit per bit */
    int len = 0, i;

    do { t[len++] = vec[num % rad]; num /= rad; } while (num);
    if (len > n) return -1;
    for (i = 0; i < len; i++)
        s[i] = t[len - 1 - i];
    return len;
}

static
int AScvs (Xpost_Context *ctx,
           Xpost_Object any,
           Xpost_Object str);

/* number radix string  cvrs  string
   convert number to a radix representation in string

   Radix ten is cvs by definition, so a real keeps its real syntax
   there. Every other radix renders the value truncated toward zero
   and unsigned at the integer's own width, a negative number
   arriving as its two's-complement bit pattern; a real beyond what
   the integer holds is refused. */
static
int NRScvrs(Xpost_Context *ctx,
            Xpost_Object num,
            Xpost_Object rad,
            Xpost_Object str)
{
    int r, n;
    /* the digits are written into the string, so it needs write access */
    if (!xpost_object_is_writeable(ctx, str))
        return invalidaccess;
    r = rad.int_.val;
    if (r < 2 || r > 36)
        return rangecheck;
    if (r == 10)
        return AScvs(ctx, num, str);
    if (xpost_object_get_type(num) == realtype)
    {
        double v = (double)num.real_.val;
        if (v >= XPOST_INTEGER_HI_D + 1.0 || v <= XPOST_INTEGER_LO_D - 1.0)
            return rangecheck;
        num = xpost_int_cons((integer)v);
    }
    n = conv_rad((dword)num.int_.val, (unsigned int)r,
                 xpost_string_get_pointer(ctx, str), str.comp_.sz);
    if (n == -1)
        return rangecheck;
    /* the written length is compared against the string's own in the
       wider signed type: a length the writer declined to produce is
       shorter than any string, not longer than every one */
    if (n < (integer)str.comp_.sz)
        str.comp_.sz = n;
    xpost_stack_push(ctx->lo, ctx->os, str);
    return 0;
}

/* helper function: fill string with real decimal representation */
static
int conv_real (real num,
               char *s,
               int n)
{
    char buf[40];
    double d = (double)num;
    int prec, e, len;

    if (n == 0) return -1;
    if (isinf(num))
    {
        if (n < 3) return -1;
        memcpy(s, "inf", 3);
        return 3;
    }
    if (isnan(num))
    {
        if (n < 3) return -1;
        memcpy(s, "nan", 3);
        return 3;
    }
    if (d == 0.0)
    {
        if (n < 3) return -1;
        memcpy(s, "0.0", 3);
        return 3;
    }

    /* the shortest decimal that reads back to the same value. Find
       the fewest significant
       digits whose round trip is exact, take the decimal exponent from
       that same scientific rendering (not from log10, which rounds the
       wrong way at the powers of ten), then present it thus: fixed
       notation while the magnitude sits between
       1e-4 and 1e6, scientific outside that band. A fixed value that
       comes out whole gains a ".0" so it scans back as a real. */
    for (prec = 1; prec < 17; prec++)
    {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, d);
        if ((real)strtod(buf, NULL) == num)
            break;
    }
    {
        char *ep = strchr(buf, 'e');
        e = ep ? atoi(ep + 1) : 0;
    }

    if (e >= -4 && e < 6)
    {
        int dec = prec - 1 - e;
        if (dec < 0) dec = 0;
        snprintf(buf, sizeof buf, "%.*f", dec, d);
        if (dec == 0)
        {
            len = (int)strlen(buf);
            if (len + 2 < (int)sizeof buf)
            { buf[len] = '.'; buf[len + 1] = '0'; buf[len + 2] = 0; }
        }
    }
    else
    {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, d);
    }

    len = (int)strlen(buf);
    if (len > n) return -1;
    memcpy(s, buf, len);
    return len;
}/* any string  cvs  string
   convert any object to string representation */
static
int AScvs (Xpost_Context *ctx,
           Xpost_Object any,
           Xpost_Object str)
{
    char nostringval[] = "--nostringval--";
    char strue[] = "true";
    char sfalse[] = "false";
    int n;

    if (!xpost_object_is_writeable(ctx, str))
        return invalidaccess;
    /* a string source is copied out below, so it needs read access. The
       other types are rendered from the object itself and read no value:
       a composite that has none of its own representation here answers
       --nostringval-- whatever its access. */
    if (xpost_object_get_type(any) == stringtype &&
        !xpost_object_is_readable(ctx, any))
        return invalidaccess;
    switch(xpost_object_get_type(any))
    {
        default:
            if (str.comp_.sz < sizeof(nostringval)-1)
                return rangecheck;
            memcpy(xpost_string_get_pointer(ctx, str), nostringval, sizeof(nostringval)-1);
            str.comp_.sz = sizeof(nostringval)-1;
            break;

        case booleantype:
        {
            if (any.int_.val)
            {
                if (str.comp_.sz < sizeof(strue)-1)
                    return rangecheck;
                memcpy(xpost_string_get_pointer(ctx, str), strue, sizeof(strue)-1);
                str.comp_.sz = sizeof(strue)-1;
            }
            else
            {
                if (str.comp_.sz < sizeof(sfalse)-1)
                    return rangecheck;
                memcpy(xpost_string_get_pointer(ctx, str), sfalse, sizeof(sfalse)-1);
                str.comp_.sz = sizeof(sfalse)-1;
            }
        }
        break;
        case integertype:
        {
            /* write digits from the integer itself: a detour through
               real drops bits past the float mantissa, and negating
               the most negative integer does not exist */
            char *s = xpost_string_get_pointer(ctx, str);
            char t[24];
            dword u;
            int neg = any.int_.val < 0;
            int len = 0;

            u = neg ? -(dword)any.int_.val : (dword)any.int_.val;
            do { t[len++] = (char)(0x30 + u % 10); u /= 10; } while (u);
            n = neg + len;
            if (n > (integer)str.comp_.sz)
                return rangecheck;
            {
                int i = 0;
                if (neg) s[i++] = 0x2d;
                while (len) s[i++] = t[--len];
            }
            if (n < (integer)str.comp_.sz) str.comp_.sz = n;
            break;
        }
        case realtype:
            n = conv_real(any.real_.val, xpost_string_get_pointer(ctx, str), str.comp_.sz);
            if (n == -1)
                return rangecheck;
            if (n < (integer)str.comp_.sz) str.comp_.sz = n;
            break;

        case operatortype:
        {
            Xpost_Operator *optab;
            Xpost_Operator op;
            Xpost_Object_Mark nm;
            optab = xpost_operator_table(ctx->gl);
            op = optab[any.mark_.padw];
            nm.tag = nametype | XPOST_OBJECT_TAG_DATA_FLAG_BANK;
            nm.pad0 = 0;
            nm.padw = op.name;
            any.mark_ = nm;
        }
        /*@fallthrough@*/
        case nametype:
            any = xpost_name_get_string(ctx, any);
            /*@fallthrough@*/
        case stringtype:
            if (any.comp_.sz > str.comp_.sz)
                return rangecheck;
            if (any.comp_.sz < str.comp_.sz) str.comp_.sz = any.comp_.sz;
            memcpy(xpost_string_get_pointer(ctx, str), xpost_string_get_pointer(ctx, any), any.comp_.sz);
            break;
    }

    xpost_stack_push(ctx->lo, ctx->os, str);
    return 0;
}

int xpost_oper_init_type_ops(Xpost_Context *ctx,
                             Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "type", (Xpost_Op_Func)Atype, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cvlit", (Xpost_Op_Func)Acvlit, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cvx", (Xpost_Op_Func)Acvx, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "xcheck", (Xpost_Op_Func)Axcheck, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "executeonly", (Xpost_Op_Func)Aexecuteonly, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "noaccess", (Xpost_Op_Func)Anoaccess, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "readonly", (Xpost_Op_Func)Areadonly, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "rcheck", (Xpost_Op_Func)Archeck, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "wcheck", (Xpost_Op_Func)Awcheck, 1, anytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cvi", (Xpost_Op_Func)Ncvi, 1, numbertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cvi", (Xpost_Op_Func)Scvi, 1, stringtype);
    INSTALL;
    /* cvn converts a string, and only a string */
    op = xpost_operator_cons(ctx, "cvn", (Xpost_Op_Func)Scvn, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cvr", (Xpost_Op_Func)Ncvr, 1, numbertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cvr", (Xpost_Op_Func)Scvr, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cvrs", (Xpost_Op_Func)NRScvrs, 3, numbertype, integertype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "cvs", (Xpost_Op_Func)AScvs, 2, anytype, stringtype);
    INSTALL;

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
     */
    return 0;
}
