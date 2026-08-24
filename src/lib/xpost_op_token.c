/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h> /* errno */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* strchr */

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_dict.h"
#include "xpost_file.h"
#include "xpost_name.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_array.h"
#include "xpost_op_dict.h"
#include "xpost_bytes.h"
#include "xpost_op_token.h"

/* large enough for the longest legal string literal (a string's
   length field is 16 bits), which base-85 z-runs can reach from a
   modest number of coded characters */
enum { NBUF = 65540 };

static
int puff(Xpost_Context *ctx,
         char *buf,
         int nbuf,
         Xpost_Object *src,
         int (*next)(Xpost_Context *ctx, Xpost_Object *src),
         void (*back)(Xpost_Context *ctx, int c, Xpost_Object *src));
static
int toke(Xpost_Context *ctx,
         Xpost_Object *src,
         int (*next)(Xpost_Context *ctx, Xpost_Object *src),
         void (*back)(Xpost_Context *ctx, int c, Xpost_Object *src),
         Xpost_Object *retval);

static
int ishash(int c)
{
    return c == '#';
}

static
int isdot(int c)
{
    return c == '.';
}

static
int ise(int c)
{
    return c == 'e' || c == 'E';
}

static
int issign(int c)
{
    return c == '+' || c == '-';
}

static
int isdel(int c)
{
    switch (c)
    {
        case '(': case ')': case '[': case ']':
        case '<': case '>': case '{': case '}':
        case '/': case '%':
            return 1;
    }
    return 0;
}

static
int isreg(int c)
{
    /* bytes in the binary-token range 128..159 delimit the token they
       follow, exactly as whitespace or a delimiter would (PLRM 3.14.2);
       bytes 160..255 are regular characters and may appear in names */
    return (c != EOF) && !(c >= 128 && c <= 159) &&
           (c >= 128 || (!isspace(c) && !isdel(c)));
}

//int isxdigit (int c) { return strchr("0123456789ABCDEFabcdef", c) != NULL; }

typedef
struct
{
    int (*pred)(int);
    int y;
    int n;
} test;

test fsm_dec[] = {
    /*  int pred(int), y,  n */
    /*       -------- --  -- */
    /* 0 */ { issign,  1,  1 },
    /* 1 */ { isdigit, 2, -1 },
    /* 2 */ { isdigit, 2, -1 } };
static
int accept_dec(int i)
{
    return i == 2;
}

test fsm_rad[] = {
    /* 0 */ { isdigit, 1, -1 },
    /* 1 */ { isdigit, 1,  2 },
    /* 2 */ { ishash,  3, -1 },
    /* 3 */ { isalnum, 4, -1 },
    /* 4 */ { isalnum, 4, -1 } };
static
int accept_rad(int i)
{
    return i == 4;
}

test fsm_real[] = {
    /* 0 */  { issign,  1,   1 },
    /* 1 */  { isdigit, 2,   4 },
    /* 2 */  { isdigit, 2,   3 },
    /* 3 */  { isdot,   6,   7 },
    /* 4 */  { isdot,   5,  -1 },
    /* 5 */  { isdigit, 6,  -1 },
    /* 6 */  { isdigit, 6,   7 },
    /* 7 */  { ise,     8,  -1 },
    /* 8 */  { issign,  9,   9 },
    /* 9 */  { isdigit, 10, -1 },
    /* 10 */ { isdigit, 10, -1 } };
static
int accept_real(int i)
{
    switch (i) { //case 2:
        case 6: case 10: return 1; default: return 0; }
    //return (i & 3) == 2;  // 2, 6 == 2|4, 10 == 2|8
}

static
int fsm_check(char *s,
              int ns,
              test *fsm,
              int (*accept)(int final))
{
    int sta = 0;
    char *sp = s;
    while (sta != -1 && *sp)
    {
        if (sp - s > ns) break;
        if (fsm[sta].pred(*sp))
        {
            sta = fsm[sta].y;
            ++sp;
        }
        else
        {
            sta = fsm[sta].n;
        }
    }
    return accept(sta);
}

/* A numeral is read into a field that spans the whole of the integer
   object's, and is judged against the integer's own range afterwards. The
   two are separate: the field belongs to the conversion functions and the
   range belongs to the object, and the range is the only one a program can
   observe. `long` is the width of an integer object on some platforms and
   half of it on others, so a numeral judged in a `long` would be a
   different numeral on each; `long long` is at least the integer's width
   everywhere, which is what this pins. */
typedef char xpost_scan_field_spans_the_integer[
    sizeof(long long) >= sizeof(integer)
    && sizeof(unsigned long long) >= sizeof(dword)
    && sizeof(dword) == sizeof(integer) ? 1 : -1];

/* the value a character carries as a digit of the given base, or -1 for
   a character that is not one of that base's digits */
static
int radix_digit(int c,
                unsigned long base)
{
    unsigned long d;

    if (c >= '0' && c <= '9')
        d = (unsigned long)(c - '0');
    else if (c >= 'A' && c <= 'Z')
        d = (unsigned long)(c - 'A') + 10;
    else if (c >= 'a' && c <= 'z')
        d = (unsigned long)(c - 'a') + 10;
    else
        return -1;
    return d < base ? (int)d : -1;
}

int xpost_scanner_radix_number(const char *s,
                               const char **end,
                               integer *out)
{
    const unsigned long long limit = (unsigned long long)(dword)~(dword)0;
    unsigned long base = 0;
    unsigned long long num = 0;
    const char *p = s;
    int over = 0;
    int ndigits = 0;
    int d;

    *end = s;
    if (!(*p >= '0' && *p <= '9'))
        return -1;
    /* the base is the decimal 2 to 36 the syntax allows and nothing
       wider, so it is bounded below rather than by the field it is read
       into: once it is past 36 no further digit can bring it back */
    while (*p >= '0' && *p <= '9')
    {
        base = base > 36 ? 37 : base * 10 + (unsigned long)(*p - '0');
        p++;
    }
    if (*p != '#' || base < 2 || base > 36)
        return -1;
    p++;

    /* PLRM 3.2: a radix number is unsigned, so it is read unsigned and
       the whole of the integer's field is available to it. Every digit is
       read before the number is judged, so the end is past the whole of
       the numeral either way and a caller can tell a numeral too large
       from text that stops being a numeral partway */
    while ((d = radix_digit((unsigned char)*p, base)) >= 0)
    {
        if (over || num > (limit - (unsigned long long)d) / base)
            over = 1;
        else
            num = num * base + (unsigned long long)d;
        ++ndigits;
        p++;
    }
    if (ndigits == 0)
        return -1;
    *end = p;
    /* it becomes the integer of the same twos-complement representation,
       so it must fit the integer's own field; one that exceeds that field
       is a limitcheck, neither narrowed to fit nor promoted to a real the
       way an over-range decimal integer is */
    if (over)
        return limitcheck;
    *out = (integer)(dword)num;
    return 0;
}

static
int grok(Xpost_Context *ctx,
         char *s,
         int ns,
         Xpost_Object *src,
         int (*next)(Xpost_Context *ctx, Xpost_Object *src),
         void (*back)(Xpost_Context *ctx, int c, Xpost_Object *src),
         Xpost_Object *retval)
{
    Xpost_Object obj;

    if (ns == NBUF)
    {
        XPOST_LOG_ERR("buf maxxed");
        return limitcheck;
    }
    s[ns] = '\0';  //fsm_check & xpost_name_cons  terminate on \0

    { /* plain decimal integers dominate; scan them without the fsms */
        char *p = s;
        if (*p == '+' || *p == '-')
            p++;
        if (isdigit((unsigned char)*p))
        {
            do { p++; } while (isdigit((unsigned char)*p));
            if (p - s == ns)
            {
                long long num;
                errno = 0;
                num = strtoll(s, NULL, 10);
                if (errno == ERANGE || (long long)(integer)num != num)
                {
                    /* beyond the integer range: PLRM 3.3.2 makes it a real */
                    *retval = xpost_real_cons((real)strtod(s, NULL));
                    return 0;
                }
                *retval = xpost_int_cons(num);
                return 0;
            }
        }
    }

    /* a token that does not start with a digit, sign or dot cannot
       match any of the numeric forms */
    if (!isdigit((unsigned char)*s) && *s != '+' && *s != '-' && *s != '.')
        goto not_a_number;

    if (fsm_check(s, ns, fsm_dec, accept_dec))
    {
        long long num;
        errno = 0;
        num = strtoll(s, NULL, 10);
        if (errno == ERANGE || (long long)(integer)num != num)
        {
            /* beyond the integer range: PLRM 3.3.2 makes it a real */
            *retval = xpost_real_cons((real)strtod(s, NULL));
            return 0;
        }
        *retval = xpost_int_cons(num);
        return 0;
    }

    else if (fsm_check(s, ns, fsm_rad, accept_rad))
    {
        const char *rend;
        integer rnum = 0;
        int rret = xpost_scanner_radix_number(s, &rend, &rnum);

        /* PLRM 3.2 admits a base of 2 through 36 and digits ranging from
           0 to base-1. Text failing either is not a radix number, and a
           token that cannot be interpreted as a number is a name */
        if (rret < 0 || *rend != '\0')
            goto not_a_number;
        if (rret != 0)
        {
            XPOST_LOG_ERR("radixnumber exceeds integer width");
            return rret;
        }
        *retval = xpost_int_cons(rnum);
        return 0;
    }

    else if (fsm_check(s, ns, fsm_real, accept_real))
    {
        double num;
        num = strtod(s, NULL);
        if ((num == HUGE_VAL || num == -HUGE_VAL) && errno == ERANGE)
        {
            XPOST_LOG_ERR("real out of range");
            return limitcheck;
        }
        *retval = xpost_real_cons((real)num);
        return 0;
    }

    else
      not_a_number:
        switch(*s)
        {
            case '(':
            {
                int c, defer = 1;
                char *sp = s;
                while (defer && (c = next(ctx, src)) != EOF)
                {
                    switch(c)
                    {
                        case '(': ++defer; break;
                        case ')': --defer; break;
                        case '\r':
                        {
                            /* an end-of-line marker inside a string is
                               one newline character, whichever of the
                               three conventions wrote it */
                            int c2 = next(ctx, src);

                            if (c2 != '\n' && c2 != EOF)
                                back(ctx, c2, src);
                            c = '\n';
                            break;
                        }
                        case '\\':
                            switch(c = next(ctx, src))
                            {
                                case '\n': continue;
                                case '\r':
                                {
                                    /* an escaped end-of-line joins the
                                       lines: nothing is inserted, for
                                       CR alone or CR LF */
                                    int c2 = next(ctx, src);

                                    if (c2 != '\n' && c2 != EOF)
                                        back(ctx, c2, src);
                                    continue;
                                }
                                case 'a': c = '\a'; break;
                                case 'b': c = '\b'; break;
                                case 'f': c = '\f'; break;
                                case 'n': c = '\n'; break;
                                case 'r': c = '\r'; break;
                                case 't': c = '\t'; break;
                                case 'v': c = '\v'; break;
                                default:
                                    if (c >= '0' && c <= '7')
                                    {
                                        /* up to three octal digits, and
                                           only octal ones: 8 and 9 end the
                                           escape, and the character after
                                           the third digit is not part of it */
                                        int t = c - '0', n = 1;
                                        while (n < 3)
                                        {
                                            int d = next(ctx, src);
                                            if (d >= '0' && d <= '7')
                                            {
                                                t = t * 8 + (d - '0');
                                                ++n;
                                            }
                                            else
                                            {
                                                if (d != EOF) back(ctx, d, src);
                                                break;
                                            }
                                        }
                                        c = t & 0xff;
                                    }
                            }
                    }
                    if (!defer) break;
                    if (sp - s >= NBUF)
                    {
                        XPOST_LOG_ERR("string exceeds buf");
                        return limitcheck;
                    }
                    else *sp++ = c;
                }
                if (defer)
                {
                    /* the closing parenthesis never arrived: an
                       unterminated string literal is not a token */
                    XPOST_LOG_ERR("end of input inside a string literal");
                    return syntaxerror;
                }
                obj = xpost_string_cons(ctx, sp - s, s);
                if (xpost_object_get_type(obj) == nulltype)
                    return VMerror;
                *retval = xpost_object_cvlit(obj);
                return 0;
            }

            case '<':
            {
                int c;
                char d;
                const char *x = "0123456789ABCDEF";
                char *sp = s;
                c = next(ctx, src);
                if (c == '<')
                {
                    *retval = xpost_object_cvx(xpost_name_cons(ctx, "<<"));
                    return 0;
                }
                if (c == '~')
                {
                    /* <~ ... ~> ASCII base-85 string literal (PLRM
                       3.2.2): five coded characters carry four bytes,
                       'z' abbreviates four zeros, a short final group
                       drops its pad bytes, whitespace falls out */
                    unsigned int grp[5];
                    int n = 0;

                    for (c = next(ctx, src); c != EOF; c = next(ctx, src))
                    {
                        unsigned int tuple;
                        int k;

                        if (isspace(c))
                            continue;
                        if (c == '~')
                        {
                            c = next(ctx, src);
                            if (c != '>')
                            {
                                XPOST_LOG_ERR("malformed base-85 terminator");
                                return syntaxerror;
                            }
                            break;
                        }
                        if (c == 'z' && n == 0)
                        {
                            if (sp - s + 4 > NBUF)
                            {
                                XPOST_LOG_ERR("base-85 string exceeds buf");
                                return limitcheck;
                            }
                            *sp++ = 0; *sp++ = 0; *sp++ = 0; *sp++ = 0;
                            continue;
                        }
                        if (c < '!' || c > 'u')
                        {
                            XPOST_LOG_ERR("character %d in base-85 string", c);
                            return syntaxerror;
                        }
                        grp[n++] = c - '!';
                        if (n < 5)
                            continue;
                        tuple = 0;
                        for (k = 0; k < 5; k++)
                            tuple = tuple * 85 + grp[k];
                        if (sp - s + 4 > NBUF)
                        {
                            XPOST_LOG_ERR("base-85 string exceeds buf");
                            return limitcheck;
                        }
                        *sp++ = (tuple >> 24) & 0xff;
                        *sp++ = (tuple >> 16) & 0xff;
                        *sp++ = (tuple >> 8) & 0xff;
                        *sp++ = tuple & 0xff;
                        n = 0;
                    }
                    if (n == 1)
                    {
                        XPOST_LOG_ERR("dangling base-85 character");
                        return syntaxerror;
                    }
                    if (n > 1)
                    {
                        unsigned int tuple = 0;
                        int k, nbytes = n - 1;

                        for (k = 0; k < 5; k++)
                            tuple = tuple * 85 + (k < n ? grp[k] : 84);
                        if (sp - s + nbytes > NBUF)
                        {
                            XPOST_LOG_ERR("base-85 string exceeds buf");
                            return limitcheck;
                        }
                        /* a partial group carries one byte fewer than
                           it has characters, and the tuple it decodes
                           to is four bytes wide */
                        for (k = 0; k < nbytes && k < 4; k++)
                            *sp++ = (tuple >> (24 - 8 * k)) & 0xff;
                    }
                    if (c == EOF)
                    {
                        /* the ~> terminator never arrived */
                        XPOST_LOG_ERR("end of input inside a base-85 string literal");
                        return syntaxerror;
                    }
                    obj = xpost_string_cons(ctx, sp - s, s);
                    if (xpost_object_get_type(obj) == nulltype)
                        return VMerror;
                    *retval = xpost_object_cvlit(obj);
                    return 0;
                }
                for ( ; c != '>' && c != EOF; c = next(ctx, src))
                {
                    if (isspace(c))
                        continue;
                    if (isxdigit(c))
                        c = strchr(x, toupper(c)) - x;
                    else
                    {
                        XPOST_LOG_ERR("non-hex digit in hex string");
                        return syntaxerror;
                    }
                    d = c << 4; // hi nib
                    while (isspace(c = next(ctx, src)))
                        /**/;
                    if (isxdigit(c))
                        c = strchr(x, toupper(c)) - x;
                    else if (c == '>')
                    {
                        back(ctx, c, src); // pushback for next iter
                        c = 0;             // pretend it got a 0
                    }
                    else
                    {
                        XPOST_LOG_ERR("non-hex digit in hex string");
                        return syntaxerror;
                    }
                    d |= c;
                    if (sp - s >= NBUF)
                    {
                        XPOST_LOG_ERR("hexstring exceeds buf");
                        return limitcheck;
                    }
                    *sp++ = d;
                }
                if (c == EOF)
                {
                    /* the > terminator never arrived */
                    XPOST_LOG_ERR("end of input inside a hex string literal");
                    return syntaxerror;
                }
                obj = xpost_string_cons(ctx, sp - s, s);
                if (xpost_object_get_type(obj) == nulltype)
                    return VMerror;
                *retval = xpost_object_cvlit(obj);
                return 0;
            }

            case '>':
            {
                int c;
                if ((c = next(ctx, src)) == '>')
                {
                    *retval = xpost_object_cvx(xpost_name_cons(ctx, ">>"));
                    return 0;
                }
                else
                {
                    XPOST_LOG_ERR("bare angle bracket");
                    return syntaxerror;
                }
            }
            return unregistered; //not reached

            case '{':
            { // This is the one part that makes it a recursive-descent parser
                /* each level is a toke() frame carrying a 64 KB token buffer;
                   cap the nesting well within the C stack and raise
                   limitcheck beyond it. The counter unwinds on the single
                   exit below, so scans stay balanced. It lives in the
                   context, not process-global state: a scan never yields
                   today, but each context is its own interpreter. */
                enum { PROC_NEST_MAX = 100 };
                int ret;
                Xpost_Object tail;

                if (++ctx->scan_proc_depth > PROC_NEST_MAX)
                {
                    --ctx->scan_proc_depth;
                    XPOST_LOG_ERR("procedure nesting too deep");
                    return limitcheck;
                }
                tail = xpost_name_cons(ctx, "}");
                xpost_stack_push(ctx->lo, ctx->os, mark);
                ret = 0;
                while (1)
                {
                    Xpost_Object t;
                    ret = toke(ctx, src, next, back, &t);
                    if (ret)
                        break;
                    /* the source ended inside the procedure: the
                       scanner answers a null there and nowhere else,
                       a literal null in the text being a name */
                    if (xpost_object_get_type(t) == nulltype)
                    {
                        XPOST_LOG_ERR("end of input inside a procedure");
                        ret = syntaxerror;
                        break;
                    }
                    if ((xpost_object_get_type(t) == nametype) &&
                        (xpost_dict_compare_objects(ctx, t, tail) == 0))
                        break;
                    xpost_stack_push(ctx->lo, ctx->os, t);
                }
                if (ret == 0)
                {
                    ret = xpost_op_array_to_mark(ctx);  // ie. the /] operator
                    if (ret == 0)
                    {
                        Xpost_Object proc = xpost_stack_pop(ctx->lo, ctx->os);
                        /* in packing mode a procedure is built read-only, which
                           is how xpost represents a packed array; the packed
                           flag tells it apart from a plain read-only array so
                           bind and type treat it as the packed array it is */
                        if (ctx->packing)
                            proc = xpost_object_set_packed(
                                    xpost_object_set_access(ctx, proc,
                                        XPOST_OBJECT_TAG_ACCESS_READ_ONLY));
                        *retval = xpost_object_cvx(proc);
                    }
                }
                --ctx->scan_proc_depth;
                return ret;
            }

            case '/':
            {
                *s = next(ctx, src);
                if (ns && *s == '/')
                {
                    Xpost_Object ret;
                    ns = puff(ctx, s, NBUF, src, next, back);
                    if (ns == NBUF)
                    {
                        XPOST_LOG_ERR("immediate name exceeds buf");
                        return limitcheck;
                    }
                    s[ns] = '\0';
                    if (DEBUGLOAD)
                        printf("\ntoken: loading immediate name %s\n", s);
                    /* PLRM 3.12.2: the scanner substitutes the value the
                       name has on the dictionary stack, and where it
                       cannot find the name an undefined error occurs. A
                       lookup that fails pushes nothing, so reading a
                       value regardless takes the operand beneath instead
                       -- the token becomes whatever the program last left
                       on the stack, with nothing said. The same load in
                       the binary-token path below is read this way. */
                    {
                        int lret = xpost_op_any_load(ctx,
                                xpost_object_cvx(xpost_name_cons(ctx, s)));
                        if (lret)
                            return lret;
                    }
                    ret = xpost_stack_pop(ctx->lo, ctx->os);
                    if (DEBUGLOAD)
                        xpost_object_dump(ret);
                    *retval = ret;
                    return 0;
                }
                else
                {
                    if ((isspace)(*s))
                    {
                        ns = 0;
                    }
                    else if (isdel(*s))
                    {
                        back(ctx, *s, src);
                        ns = 0;
                    }
                    else
                    {
                        ns += puff(ctx, s + 1, NBUF - 1, src, next, back);
                    }
                }
                if (ns == NBUF)
                {
                    XPOST_LOG_ERR("name exceeds buf");
                    return limitcheck;
                }
                s[ns] = '\0';
                *retval = xpost_object_cvlit(xpost_name_cons(ctx, s));
                return 0;
            }
            default:
            {
                *retval = xpost_object_cvx(xpost_name_cons(ctx, s));
                return 0;
            }
        }
}

/* read until a non-whitespace, non-comment char.
   "prime" the buffer.  */
static
int snip(Xpost_Context *ctx,
         char *buf,
         Xpost_Object *src,
         int (*next)(Xpost_Context *ctx, Xpost_Object *src))
{
    int c;
    do {
        c = next(ctx, src);
        if (c == '%')
        {
            do {
                c = next(ctx, src);
            } while(c != '\n' && c != '\f' && c != EOF);
        }
    } while(c != EOF && isspace(c));
    if (c == EOF) return 0;
    *buf = c;
    return 1; // true, and size of buffer
}

/* read in a token up to delimiter
   read into buf any regular characters,
   if we read one too many, put it back, unless whitespace. */
static
int puff(Xpost_Context *ctx,
         char *buf,
         int nbuf,
         Xpost_Object *src,
         int (*next)(Xpost_Context *ctx, Xpost_Object *src),
         void (*back)(Xpost_Context *ctx, int c, Xpost_Object *src))
{
    int c;
    char *s = buf;
    while (isreg(c = next(ctx, src)))
    {
        if (s - buf >= nbuf) return 0;
        *s++ = c;
    }
    if (c == '\r')
    {
        /* an end-of-line marker is one white-space character however
           written (PLRM 3.8): a CR delimiter claims its LF */
        int c2 = next(ctx, src);
        if (c2 != '\n' && c2 != EOF)
            back(ctx, c2, src);
    }
    else if (!isspace(c) && c != EOF)
        back(ctx, c, src);
    return s - buf;
}


/* Binary tokens (PLRM 3.14.2): a byte in 128..159 read where a token
   is expected introduces a fixed-format encoded object rather than
   ASCII syntax. Packaged program bodies are written almost entirely
   in these. Only the single-object token forms appear in such
   bodies; binary object sequences (128..131) and the composite forms
   are not accepted. */

/* The standard system name table (PLRM 3rd ed. Appendix F):
   binary name tokens carry an index into this table. Entries
   match the encoding emitted by Level 2 producers; unassigned
   indices are NULL. */
static const char * const _bin_sysname[256] =
{
    "abs", "add", "aload", "anchorsearch",
    "and", "arc", "arcn", "arct",
    "arcto", "array", "ashow", "astore",
    "awidthshow", "begin", "bind", "bitshift",
    "ceiling", "charpath", "clear", "cleartomark",
    "clip", "clippath", "closepath", "concat",
    "concatmatrix", "copy", "count", "counttomark",
    "currentcmykcolor", "currentdash", "currentdict", "currentfile",
    "currentfont", "currentgray", "currentgstate", "currenthsbcolor",
    "currentlinecap", "currentlinejoin", "currentlinewidth", "currentmatrix",
    "currentpoint", "currentrgbcolor", "currentshared", "curveto",
    "cvi", "cvlit", "cvn", "cvr",
    "cvrs", "cvs", "cvx", "def",
    "defineusername", "dict", "div", "dtransform",
    "dup", "end", "eoclip", "eofill",
    "eoviewclip", "eq", "exch", "exec",
    "exit", "file", "fill", "findfont",
    "flattenpath", "floor", "flush", "flushfile",
    "for", "forall", "ge", "get",
    "getinterval", "grestore", "gsave", "gstate",
    "gt", "identmatrix", "idiv", "idtransform",
    "if", "ifelse", "image", "imagemask",
    "index", "ineofill", "infill", "initviewclip",
    "inueofill", "inufill", "invertmatrix", "itransform",
    "known", "le", "length", "lineto",
    "load", "loop", "lt", "makefont",
    "matrix", "maxlength", "mod", "moveto",
    "mul", "ne", "neg", "newpath",
    "not", "null", "or", "pathbbox",
    "pathforall", "pop", "print", "printobject",
    "put", "putinterval", "rcurveto", "read",
    "readhexstring", "readline", "readstring", "rectclip",
    "rectfill", "rectstroke", "rectviewclip", "repeat",
    "restore", "rlineto", "rmoveto", "roll",
    "rotate", "round", "save", "scale",
    "scalefont", "search", "selectfont", "setbbox",
    "setcachedevice", "setcachedevice2", "setcharwidth", "setcmykcolor",
    "setdash", "setfont", "setgray", "setgstate",
    "sethsbcolor", "setlinecap", "setlinejoin", "setlinewidth",
    "setmatrix", "setrgbcolor", "setshared", "shareddict",
    "show", "showpage", "stop", "stopped",
    "store", "string", "stringwidth", "stroke",
    "strokepath", "sub", "systemdict", "token",
    "transform", "translate", "truncate", "type",
    "uappend", "ucache", "ueofill", "ufill",
    "undef", "upath", "userdict", "ustroke",
    "viewclip", "viewclippath", "where", "widthshow",
    "write", "writehexstring", "writeobject", "writestring",
    "wtranslation", "xor", "xshow", "xyshow",
    "yshow", "FontDirectory", "SharedFontDirectory", "Courier",
    "Courier-Bold", "Courier-BoldOblique", "Courier-Oblique", "Helvetica",
    "Helvetica-Bold", "Helvetica-BoldOblique", "Helvetica-Oblique", "Symbol",
    "Times-Bold", "Times-BoldItalic", "Times-Italic", "Times-Roman",
    "execuserobject", "currentcolor", "currentcolorspace", "currentglobal",
    "execform", "filter", "findresource", "globaldict",
    "makepattern", "setcolor", "setcolorspace", "setglobal",
    "setpagedevice", "setpattern", NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL
};

/* read n payload bytes, failing on end of input */
static
int bt_read(Xpost_Context *ctx,
            Xpost_Object *src,
            int (*next)(Xpost_Context *ctx, Xpost_Object *src),
            unsigned char *p,
            int n)
{
    int i, c;

    for (i = 0; i < n; i++)
    {
        c = next(ctx, src);
        if (c == EOF)
            return 0;
        p[i] = (unsigned char)c;
    }
    return 1;
}

/* Number representation byte, shared by the fixed-point token (137)
   and homogeneous number arrays (149): the low bits select width and
   scale, bit 7 selects byte order.
     0..31    32-bit fixed point, scale 0..31
     32..47   16-bit fixed point, scale 0..15
     48       32-bit IEEE real (homogeneous arrays only)
     49       32-bit native real (homogeneous arrays only)
   A scale of zero yields an integer; any other scale divides the
   signed fixed value by 2^scale into a real. The fixed-point token
   defines only the fixed representations; a real representation
   there is a malformed token. */
static
int bt_rep_size(unsigned int rep)
{
    unsigned int r = rep & 127;

    if (r <= 31 || r == 48 || r == 49)
        return 4;
    if (r <= 47)
        return 2;
    return 0;
}

/* decode one number of a binary token or encoded number string
   (PLRM 3.14.4/3.14.5): rep selects representation and byte order,
   p holds the encoded bytes. Also used by .numstring2array. */
int xpost_scanner_rep_number(unsigned int rep, const unsigned char *p, Xpost_Object *retval)
{
    unsigned int r = rep & 127;
    int le = rep >= 128;

    if (r == 48 || r == 49)
    {
        unsigned int v;
        float f;

        if (r == 49)
            memcpy(&v, p, 4);      /* native order */
        else
            v = le ? xpost_bytes_le32(p) : xpost_bytes_be32(p);
        memcpy(&f, &v, 4);
        *retval = xpost_real_cons((real)f);
        return 0;
    }
    if (r <= 31)
    {
        unsigned int v = le ? xpost_bytes_le32(p) : xpost_bytes_be32(p);
        int i = (int)v;

        if (r == 0)
            *retval = xpost_int_cons(i);
        else
            *retval = xpost_real_cons((real)(i / (double)(1u << r)));
        return 0;
    }
    if (r <= 47)
    {
        unsigned short v = le ? xpost_bytes_le16(p) : xpost_bytes_be16(p);
        short i = (short)v;
        unsigned int scale = r - 32;

        if (scale == 0)
            *retval = xpost_int_cons(i);
        else
            *retval = xpost_real_cons((real)(i / (double)(1u << scale)));
        return 0;
    }
    return syntaxerror;
}

/* True when the span [base+value, base+value+len) lies within a buffer of
   buflen bytes. The sum is taken in 64 bits: base, value and len are each
   attacker-supplied 32-bit fields, and adding them in 32 bits lets a value
   near 2^32 wrap the sum below buflen and pass a check the span should fail,
   after which the read advances by the true (huge) offset off the end of the
   buffer. Every offset-bearing record arm below routes its bound through here
   so no one arm can reintroduce the 32-bit wrap. */
static int
bt_span_within(unsigned int base, unsigned int value,
               unsigned long long len, unsigned int buflen)
{
    return (unsigned long long)base + value + len <= buflen;
}

/* Binary object sequence (PLRM 3.14.1): a self-delimiting block of
   encoded objects. The header carries the top-level count and total
   length; each object is an 8-byte record, with composite bodies
   (array element records, string and name text) at record-relative
   offsets in the remainder. The whole sequence scans to one
   executable array of the top-level objects, which the interpreter
   executes (unlike a brace procedure, which it defers). */
static
int bt_seq_object(Xpost_Context *ctx,
                  const unsigned char *buf,
                  unsigned int buflen,
                  unsigned int base,
                  unsigned int recoff,
                  int le,
                  int depth,
                  Xpost_Object *retval)
{
    const unsigned char *p;
    unsigned int type, xflag, length, value;
    Xpost_Object obj;

    if (depth > 32 || recoff + 8 > buflen)
        return syntaxerror;
    p = buf + recoff;
    xflag = p[0] & 0x80;
    type = p[0] & 0x7f;
    length = le ? xpost_bytes_le16(p + 2) : xpost_bytes_be16(p + 2);
    value = le
        ? xpost_bytes_le32(p + 4) : xpost_bytes_be32(p + 4);

    /* records that use the value as an offset (string and name text,
       array elements) are only defined for the low-order header
       forms; offsets are relative to the start of the object records */
    switch (type)
    {
        case 0:  /* null: no other content is valid */
            if (length != 0 || value != 0)
                return syntaxerror;
            obj = null;
            break;
        case 1:  /* integer */
            obj = xpost_int_cons((integer)(int)value);
            break;
        case 2:  /* real: the native-real formats travel in the
                    sequence's byte order all the same, as the
                    deployed writers have it */
            {
                float f;

                memcpy(&f, &value, 4);
                obj = xpost_real_cons((real)f);
            }
            break;
        case 3:  /* name: text at offset, or a user name table index */
        case 6:  /* as 3, but the value replaces the name at scan time */
            if (length == 0)
            {
                /* no operator populates the user name table */
                XPOST_LOG_ERR("user name index %d: no user name table", (int)value);
                return undefined;
            }
            else
            {
                char *nm;
                if (!le || length > 127
                 || !bt_span_within(base, value, length, buflen))
                    return syntaxerror;
                nm = malloc(length + 1);
                if (!nm)
                    return VMerror;
                memcpy(nm, buf + base + value, length);
                nm[length] = '\0';
                obj = xpost_name_cons(ctx, nm);
                free(nm);
            }
            if (xpost_object_get_type(obj) == invalidtype)
                return VMerror;
            if (type == 6)
            {
                int ret = xpost_op_any_load(ctx, xpost_object_cvx(obj));
                if (ret)
                    return ret;
                obj = xpost_stack_pop(ctx->lo, ctx->os);
                *retval = obj;
                return 0;
            }
            break;
        case 4:  /* boolean */
            if (value > 1)
                return syntaxerror;
            obj = xpost_bool_cons(value != 0);
            break;
        case 5:  /* string */
            if (length && (!le || !bt_span_within(base, value, length, buflen)))
                return syntaxerror;
            obj = xpost_string_cons(ctx, length, (char *)(buf + base + value));
            if (xpost_object_get_type(obj) == nulltype)
                return VMerror;
            break;
        case 9:  /* array: length records at the offset */
            {
                unsigned int i;
                int ret;

                if (length && (!le || !bt_span_within(base, value,
                                                      (unsigned long long)length * 8, buflen)))
                    return syntaxerror;
                obj = xpost_array_cons(ctx, length);
                if (xpost_object_get_type(obj) == nulltype)
                    return VMerror;
                for (i = 0; i < length; i++)
                {
                    Xpost_Object el;

                    ret = bt_seq_object(ctx, buf, buflen, base, base + value + i * 8, le, depth + 1, &el);
                    if (ret)
                        return ret;
                    ret = xpost_array_put(ctx, obj, i, el);
                    if (ret)
                        return ret;
                }
            }
            break;
        case 10: /* mark */
            if (length != 0 || value != 0)
                return syntaxerror;
            obj = mark;
            break;
        default:
            XPOST_LOG_ERR("unsupported type %u in binary object sequence", type);
            return syntaxerror;
    }
    /* the record's high bit is the object's literal/executable
       attribute and applies to every type; the immediately evaluated
       name is the exception -- its record's attribute has no
       influence on the resulting object */
    if (type == 6)
        *retval = xpost_object_cvlit(obj);
    else
        *retval = xflag ? xpost_object_cvx(obj) : xpost_object_cvlit(obj);
    return 0;
}

static
int bt_sequence(Xpost_Context *ctx,
                unsigned int t,
                Xpost_Object *src,
                int (*next)(Xpost_Context *ctx, Xpost_Object *src),
                Xpost_Object *retval)
{
    int le = (t == 129) || (t == 131);
    unsigned char hdr[8];
    unsigned int count, length, hdrlen;
    unsigned char *buf;
    Xpost_Object arr;
    unsigned int i;
    int ret;

    if (!bt_read(ctx, src, next, hdr + 1, 3))
        return syntaxerror;
    if (hdr[1] != 0)
    {
        count = hdr[1];
        length = le ? xpost_bytes_le16(hdr + 2) : xpost_bytes_be16(hdr + 2);
        hdrlen = 4;
    }
    else
    {
        /* extended header: 2-byte count and 4-byte length follow */
        if (!bt_read(ctx, src, next, hdr + 4, 4))
            return syntaxerror;
        count = le ? (unsigned int)((hdr[3] << 8) | hdr[2])
                   : (unsigned int)((hdr[2] << 8) | hdr[3]);
        length = le
            ? ((unsigned int)hdr[7] << 24) | ((unsigned int)hdr[6] << 16) | ((unsigned int)hdr[5] << 8) | hdr[4]
            : ((unsigned int)hdr[4] << 24) | ((unsigned int)hdr[5] << 16) | ((unsigned int)hdr[6] << 8) | hdr[7];
        hdrlen = 8;
    }
    if (length < hdrlen + count * 8u || length > 1u << 20)
        return syntaxerror;

    /* buffer the whole sequence; record offsets address into it */
    buf = malloc(length);
    if (!buf)
        return VMerror;
    memset(buf, 0, hdrlen);
    if (!bt_read(ctx, src, next, buf + hdrlen, (int)(length - hdrlen)))
    {
        free(buf);
        return syntaxerror;
    }

    arr = xpost_array_cons(ctx, count);
    if (xpost_object_get_type(arr) == nulltype)
    {
        free(buf);
        return VMerror;
    }
    for (i = 0; i < count; i++)
    {
        Xpost_Object el;

        ret = bt_seq_object(ctx, buf, length, hdrlen, hdrlen + i * 8, le, 0, &el);
        if (ret)
        {
            free(buf);
            return ret;
        }
        ret = xpost_array_put(ctx, arr, i, el);
        if (ret)
        {
            free(buf);
            return ret;
        }
    }
    free(buf);
    /* the sequence executes; only brace procedures defer */
    ctx->scanner_defer = 0;
    *retval = xpost_object_cvx(arr);
    return 0;
}

static
int binary_token(Xpost_Context *ctx,
                 unsigned int t,
                 Xpost_Object *src,
                 int (*next)(Xpost_Context *ctx, Xpost_Object *src),
                 Xpost_Object *retval)
{
    /* the payload buffer holds whatever width the token declares, and
       carries a value for every byte of it before any read */
    unsigned char p[4] = { 0, 0, 0, 0 };

    switch (t)
    {
        case 128: case 129: case 130: case 131:  /* binary object sequence */
            return bt_sequence(ctx, t, src, next, retval);
        case 137:  /* fixed-point number: representation byte, then value */
            {
                unsigned char q[4];
                int sz;

                if (!bt_read(ctx, src, next, p, 1))
                    return syntaxerror;
                if ((p[0] & 127) > 47)
                    return syntaxerror;
                sz = bt_rep_size(p[0]);
                if (sz == 0)
                    return syntaxerror;
                if (!bt_read(ctx, src, next, q, sz))
                    return syntaxerror;
                return xpost_scanner_rep_number(p[0], q, retval);
            }
        case 147: case 148:  /* user name table: nothing populates it */
            if (!bt_read(ctx, src, next, p, 1))
                return syntaxerror;
            XPOST_LOG_ERR("user name index %d: no user name table", p[0]);
            return undefined;
        case 149:  /* homogeneous number array */
            {
                unsigned int rep, count, i;
                int sz;
                unsigned char *buf;
                Xpost_Object arr;

                if (!bt_read(ctx, src, next, p, 3))
                    return syntaxerror;
                rep = p[0];
                count = rep >= 128 ? (unsigned int)((p[2] << 8) | p[1])
                                   : (unsigned int)((p[1] << 8) | p[2]);
                sz = bt_rep_size(rep);
                if (sz == 0)
                    return syntaxerror;
                buf = malloc((size_t)count * sz + 1);
                if (!buf)
                    return VMerror;
                if (!bt_read(ctx, src, next, buf, (int)(count * sz)))
                {
                    free(buf);
                    return syntaxerror;
                }
                arr = xpost_array_cons(ctx, count);
                if (xpost_object_get_type(arr) == nulltype)
                {
                    free(buf);
                    return VMerror;
                }
                for (i = 0; i < count; i++)
                {
                    Xpost_Object el;
                    int ret = xpost_scanner_rep_number(rep, buf + (size_t)i * sz, &el);
                    if (ret)
                    {
                        free(buf);
                        return ret;
                    }
                    ret = xpost_array_put(ctx, arr, i, el);
                    if (ret)
                    {
                        free(buf);
                        return ret;
                    }
                }
                free(buf);
                *retval = xpost_object_cvlit(arr);
            }
            return 0;
        case 132: case 133:  /* 32-bit integer, high/low order */
            if (!bt_read(ctx, src, next, p, 4))
                return syntaxerror;
            {
                unsigned int v = t == 132
                    ? ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3]
                    : ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16) | ((unsigned int)p[1] << 8) | p[0];
                *retval = xpost_int_cons((integer)(int)v);
            }
            return 0;
        case 134: case 135:  /* 16-bit integer, high/low order */
            if (!bt_read(ctx, src, next, p, 2))
                return syntaxerror;
            {
                unsigned short v = t == 134
                    ? (unsigned short)((p[0] << 8) | p[1])
                    : (unsigned short)((p[1] << 8) | p[0]);
                *retval = xpost_int_cons((short)v);
            }
            return 0;
        case 136:  /* 8-bit integer */
            if (!bt_read(ctx, src, next, p, 1))
                return syntaxerror;
            *retval = xpost_int_cons((signed char)p[0]);
            return 0;
        case 138: case 139: case 140:  /* 32-bit real: IEEE high/low, native */
            if (!bt_read(ctx, src, next, p, 4))
                return syntaxerror;
            {
                unsigned int v;
                float r;
                if (t == 139)
                    v = ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16) | ((unsigned int)p[1] << 8) | p[0];
                else if (t == 138)
                    v = ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
                else
                    memcpy(&v, p, 4);
                memcpy(&r, &v, 4);
                *retval = xpost_real_cons((real)r);
            }
            return 0;
        case 141:  /* boolean */
            if (!bt_read(ctx, src, next, p, 1))
                return syntaxerror;
            if (p[0] > 1)
                return syntaxerror;
            *retval = xpost_bool_cons(p[0] != 0);
            return 0;
        case 142: case 143: case 144:  /* string: 1-byte, 2-byte high/low length */
            {
                unsigned int len;
                unsigned char *buf;
                Xpost_Object obj;

                if (t == 142)
                {
                    if (!bt_read(ctx, src, next, p, 1))
                        return syntaxerror;
                    len = p[0];
                }
                else
                {
                    if (!bt_read(ctx, src, next, p, 2))
                        return syntaxerror;
                    len = t == 143 ? (unsigned int)((p[0] << 8) | p[1])
                                   : (unsigned int)((p[1] << 8) | p[0]);
                }
                buf = malloc(len ? len : 1);
                if (!buf)
                    return VMerror;
                if (!bt_read(ctx, src, next, buf, (int)len))
                {
                    free(buf);
                    return syntaxerror;
                }
                obj = xpost_string_cons(ctx, len, (char *)buf);
                free(buf);
                if (xpost_object_get_type(obj) == nulltype)
                    return VMerror;
                *retval = xpost_object_cvlit(obj);
            }
            return 0;
        case 145: case 146:  /* literal/executable system name */
            if (!bt_read(ctx, src, next, p, 1))
                return syntaxerror;
            if (_bin_sysname[p[0]] == NULL)
            {
                XPOST_LOG_ERR("system name index %d unassigned", p[0]);
                return undefined;
            }
            {
                Xpost_Object n = xpost_name_cons(ctx, (char *)_bin_sysname[p[0]]);
                if (xpost_object_get_type(n) == invalidtype)
                    return VMerror;
                *retval = t == 145 ? xpost_object_cvlit(n) : xpost_object_cvx(n);
            }
            return 0;
        default:
            XPOST_LOG_ERR("unsupported binary token type %u", t);
            return syntaxerror;
    }
}

static
int toke(Xpost_Context *ctx,
         Xpost_Object *src,
         int (*next)(Xpost_Context *ctx, Xpost_Object *src),
         void (*back)(Xpost_Context *ctx, int c, Xpost_Object *src),
         Xpost_Object *retval)
{
    char buf[NBUF]; /* grok() NUL-terminates at the token length */
    int sta;  // status, and size
    Xpost_Object o;
    int ret;

    if (!src)
    {
        XPOST_LOG_ERR("src is NULL");
        return unregistered;
    }

    ctx->scanner_defer = 1;
    sta = snip(ctx, buf, src, next);
    if (!sta)
    {
        *retval = null;
        return 0;
    }
    if ((unsigned char)buf[0] >= 128 && (unsigned char)buf[0] <= 159)
        return binary_token(ctx, (unsigned char)buf[0], src, next, retval);
    if (!isdel(*buf))
        sta += puff(ctx, buf + 1, NBUF - 1, src, next, back);
    ret = grok(ctx, buf, sta, src, next, back, &o);
    if (ret)
        return ret;
    *retval = o;
    return 0;
}


/* file  token  token true
   false
   read token from file */
static
int Fnext(Xpost_Context *ctx,
          Xpost_Object *F)
{
    return xpost_file_getc(xpost_file_get_file_pointer(ctx->lo, *F));
}
static
void Fback(Xpost_Context *ctx,
           int c,
           Xpost_Object *F)
{
    (void)xpost_file_ungetc(xpost_file_get_file_pointer(ctx->lo, *F), c);
}
static
int Ftoken(Xpost_Context *ctx,
           Xpost_Object F)
{
    Xpost_Object t;
    int ret;

    /* scanning a token consumes characters of the file, so the access
       attribute the file object carries governs it as it governs read
       (PLRM 3.8.2). The attribute belongs to the object rather than to
       the stream, so it is asked before the stream's state is. */
    if (!xpost_object_is_readable(ctx, F))
        return invalidaccess;

    xpost_stack_push(ctx->lo, ctx->hold, F);

    if (!xpost_file_get_status(ctx->lo, F))
        return ioerror;
    ret = toke(ctx, &F, Fnext, Fback, &t);
    if (ret)
        return ret;
    if (xpost_object_get_type(t) != nulltype)
    {
        xpost_stack_push(ctx->lo, ctx->os, t);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    }
    else
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    }
    return 0;
}

/* string  token  substring token true
   false
   read token from string */
static
int Snext(Xpost_Context *ctx,
          Xpost_Object *S)
{
    int ret;
    if (S->comp_.sz == 0) return EOF;
    /* the byte must not sign-extend into EOF */
    ret = (unsigned char)xpost_string_get_pointer(ctx, *S)[0];
    ++S->comp_.off;
    --S->comp_.sz;
    return ret;
}
static
void Sback(Xpost_Context *ctx,
           int c,
           Xpost_Object *S)
{
    --S->comp_.off;
    ++S->comp_.sz;
    xpost_string_get_pointer(ctx, *S)[0] = c;
}
/* The scan itself, with no access rule of its own.
   Two callers reach a string this way and they arrive asking different
   questions. The token operator is reading a string on the program's
   behalf, so it needs read access. The interpreter stepping a string it
   is executing needs execute access, which PLRM 3.3.2 grants an
   execute-only object -- one that "may still be executed by the
   PostScript interpreter" -- and withholds from a no-access one. Each
   asks its own rule before calling this, so the scan is shared and the
   two rules cannot come to stand in for one another. */
int xpost_token_string_scan(Xpost_Context *ctx,
                            Xpost_Object S)
{
    Xpost_Object t;
    int ret;

    xpost_stack_push(ctx->lo, ctx->hold, S);

    ret = toke(ctx, &S, Snext, Sback, &t);
    if (ret)
        return ret;
    if (xpost_object_get_type(t) != nulltype)
    {
        xpost_stack_push(ctx->lo, ctx->os, S);
        xpost_stack_push(ctx->lo, ctx->os, t);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    }
    else
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    }
    return 0;
}

/* string  token  post any true
                  false
   read a token from a string, which is reading the string */
static
int Stoken(Xpost_Context *ctx,
           Xpost_Object S)
{
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    return xpost_token_string_scan(ctx, S);
}

int xpost_oper_init_token_ops(Xpost_Context *ctx,
                              Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    op = xpost_operator_cons(ctx, "token", (Xpost_Op_Func)Ftoken, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "token", (Xpost_Op_Func)Stoken, 1, stringtype);
    INSTALL;
    return 0;
}
