/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_file.c
 * @brief Installs the file and filter operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * file filter closefile read write readstring writestring readline
 * readhexstring writehexstring bytesavailable flush flushfile resetfile
 * status run currentfile print echo deletefile renamefile filenameforall
 *
 * A filter is a file whose bytes pass through a decoder or an encoder on the
 * way, and it is a file object like any other to everything above it.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h> /* NULL */
#include <stddef.h>

#include <assert.h>
#include <ctype.h>
//#include <poll.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <winsock2.h> /* select */
# undef WIN32_LEAN_AND_MEAN
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_compat.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_dict.h"
#include "xpost_file.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_file.h"

/* The C form of a file name operand.

   A PostScript string is a counted byte sequence and may hold a NUL,
   while the file layer takes a path as a NUL-terminated C string. Were
   the name shortened there, what reached the file system would be the
   part of it before the NUL -- a name the program did not write, in the
   directory it did -- and the operator would report on that file as
   though it were the one asked for; the name the sandbox weighed would
   not be the name the program meant either. PLRM 3.8.2 has a file name
   unable to hold a null character, so a string holding one names nothing
   and is refused rather than shortened. Every other byte is a byte of
   the name: a file system that keeps a name as bytes keeps a newline or
   a byte above the ASCII range in one, and the name a program writes is
   the name it gets.

   Returns 0 and the allocated name, or an error code and nothing. */
static int
xpost_op_file_name(Xpost_Context *ctx, Xpost_Object s, char **out)
{
    char *p = xpost_string_allocate_cstring(ctx, s);

    if (!p)
        return VMerror;
    if (strlen(p) != (size_t)s.comp_.sz)
    {
        free(p);
        return undefinedfilename;
    }
    *out = p;
    return 0;
}

/* filename mode  file  file
   create file object for filename with access mode */
static
int xpost_op_string_mode_file (Xpost_Context *ctx,
                               Xpost_Object fn,
                               Xpost_Object mode)
{
    Xpost_Object f;
    char *cfn, *cmode;
    int ret;

    ret = xpost_op_file_name(ctx, fn, &cfn);
    if (ret)
        return ret;
    cmode = xpost_string_allocate_cstring(ctx, mode);

    ret = xpost_file_open(ctx->lo, cfn, cmode, &f);
    if (ret){
        free(cfn);
        free(cmode);
        return ret;
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
    free(cfn);
    free(cmode);
    return 0;
}

/* The defaults a filter asked for by name alone is built with.  A
   parameter dictionary overrides them elsewhere; here they stand for
   "the program said nothing about this". */
#define XPOST_FILTER_LZW_EARLY   1
#define XPOST_FILTER_FAX_COLUMNS 1728

static Xpost_Object _cons_lzw_default (Xpost_Memory_File *mem, Xpost_Object src)
{
    return xpost_file_cons_filter_lzw(mem, src, XPOST_FILTER_LZW_EARLY);
}

static Xpost_Object _cons_fax_default (Xpost_Memory_File *mem, Xpost_Object src)
{
    return xpost_file_cons_filter_ccitt(mem, src, 0, XPOST_FILTER_FAX_COLUMNS,
                                        0, 0, 0, 0, 1);
}

static Xpost_Object _cons_enc_rle_default (Xpost_Memory_File *mem, Xpost_Object tgt)
{
    return xpost_file_cons_filter_enc_rle(mem, tgt, 0);
}

static Xpost_Object _cons_enc_lzw_default (Xpost_Memory_File *mem, Xpost_Object tgt)
{
    return xpost_file_cons_filter_enc_lzw(mem, tgt, XPOST_FILTER_LZW_EARLY);
}

static Xpost_Object _cons_enc_fax_default (Xpost_Memory_File *mem, Xpost_Object tgt)
{
    return xpost_file_cons_filter_enc_ccitt(mem, tgt, 0, XPOST_FILTER_FAX_COLUMNS,
                                            0, 0, 0, 0, 1);
}

/* Every filter this interpreter answers to by name: which way it works,
   whether it can be built from the name alone, and what builds it.

   The name is looked up here before anything else is asked, and that
   order is what a program can see.  An unknown name is unknown whatever
   it was handed, so one mistake gets one answer; were the access of the
   data source checked first, the same unknown name would be answered
   invalidaccess when the source was a file open for writing and
   undefined when it was a string, and the difference would lie in
   something the program had not got wrong.  The access check therefore
   applies to a direction this table states, rather than to one inferred
   from a name ending in Encode.

   The list is also the only list -- the constructor is a column of it,
   so a filter cannot be added to the dispatch without a direction, or
   given a direction and left unbuildable. */
typedef Xpost_Object (*Xpost_Filter_Cons)(Xpost_Memory_File *mem, Xpost_Object f);

typedef struct
{
    const char       *name;
    int               encodes;      /* writes to its target, rather than reading */
    Xpost_Filter_Cons cons;         /* NULL: cannot be built from the name alone */
} Xpost_Filter_Spec;

static const Xpost_Filter_Spec _filter_specs[] =
{
    { "ASCII85Decode",        0, xpost_file_cons_filter_a85 },
    { "ASCIIHexDecode",       0, xpost_file_cons_filter_hex },
    { "RunLengthDecode",      0, xpost_file_cons_filter_rle },
    { "ReusableStreamDecode", 0, xpost_file_cons_filter_rsd },
    { "LZWDecode",            0, _cons_lzw_default },
    { "CCITTFaxDecode",       0, _cons_fax_default },
#ifdef HAVE_ZLIB
    { "FlateDecode",          0, xpost_file_cons_filter_flate },
#endif
#ifdef HAVE_LIBJPEG
    { "DCTDecode",            0, xpost_file_cons_filter_dct },
#endif
    /* told what ends the subfile it reads, and there is no default for
       that: a subfile of no stated extent is not a subfile */
    { "SubFileDecode",        0, NULL },
    { "ASCIIHexEncode",       1, xpost_file_cons_filter_enc_hex },
    { "ASCII85Encode",        1, xpost_file_cons_filter_enc_a85 },
    { "NullEncode",           1, xpost_file_cons_filter_enc_null },
    { "RunLengthEncode",      1, _cons_enc_rle_default },
    { "LZWEncode",            1, _cons_enc_lzw_default },
    { "CCITTFaxEncode",       1, _cons_enc_fax_default },
#ifdef HAVE_ZLIB
    { "FlateEncode",          1, xpost_file_cons_filter_enc_flate },
#endif
#ifdef HAVE_LIBJPEG
    /* told the geometry of the image it compresses, which nothing here
       could guess from the bytes.  Conditional for the same reason
       DCTDecode is: a build without the library does not have the filter,
       and saying its parameters are missing would claim it does */
    { "DCTEncode",            1, NULL }
#endif
};

static const Xpost_Filter_Spec *_filter_spec (const char *name)
{
    size_t i;

    for (i = 0; i < sizeof _filter_specs / sizeof *_filter_specs; i++)
        if (strcmp(_filter_specs[i].name, name) == 0)
            return &_filter_specs[i];
    return NULL;
}

/* file /FilterName  filter  file'
   layer a filter over a file, in the direction the filter works in */
static
int xpost_op_file_filter (Xpost_Context *ctx,
                          Xpost_Object F,
                          Xpost_Object name)
{
    const Xpost_Filter_Spec *spec;
    Xpost_Object namestr;
    char *cname;
    Xpost_Object f;

    namestr = xpost_name_get_string(ctx, name);
    cname = xpost_string_allocate_cstring(ctx, namestr);
    if (!cname)
        return VMerror;
    spec = _filter_spec(cname);
    /* the name first, and nothing about the source before it: a name this
       interpreter does not know is not known whatever it was handed */
    if (!spec)
    {
        /* an ordinary program error, reported by the error machinery like
           any other.  A diagnostic here would also go to a channel the
           program cannot see or silence, once per occurrence */
        free(cname);
        return undefined;
    }
    free(cname);
    /* a filter that cannot be built from its name alone was asked for
       without the parameters it needs, so the operands were the wrong
       shape -- which is what the program got wrong, and is not a
       statement about the source either */
    if (!spec->cons)
        return typecheck;
    if (spec->encodes)
    {
        if (!xpost_object_is_writeable(ctx, F))
            return invalidaccess;
    }
    else
    {
        if (!xpost_object_is_readable(ctx, F))
            return invalidaccess;
    }
    f = spec->cons(ctx->lo, F);
    if (xpost_object_get_type(f) == invalidtype)
        return ioerror;
    f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    f.tag |= ((spec->encodes ? XPOST_OBJECT_TAG_ACCESS_FILE_WRITE
                             : XPOST_OBJECT_TAG_ACCESS_FILE_READ)
              << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
    return 0;
}

static
int _dict_int (Xpost_Context *ctx, Xpost_Object dict, const char *key, int def)
{
    Xpost_Object v = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, key));

    if (xpost_object_get_type(v) == integertype)
        return v.int_.val;
    if (xpost_object_get_type(v) == booleantype)
        return v.int_.val != 0;
    return def;
}

/* file  .eexecdecode  file'
   the decryption layer under a Type 1 font program's eexec */
static
int xpost_op_file_eexecdecode (Xpost_Context *ctx,
                               Xpost_Object F)
{
    Xpost_Object f;

    if (!xpost_object_is_readable(ctx, F))
        return invalidaccess;
    f = xpost_file_cons_filter_eexec(ctx->lo, F);
    if (xpost_object_get_type(f) == invalidtype)
        return ioerror;
    f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    f.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_READ << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
    return 0;
}

/* file recordsize /RunLengthEncode  filter  file'
   records bound the runs; zero means no record structure */
static
int xpost_op_file_filter_int (Xpost_Context *ctx,
                              Xpost_Object F,
                              Xpost_Object rec,
                              Xpost_Object name)
{
    Xpost_Object namestr, f;
    char *cname;

    namestr = xpost_name_get_string(ctx, name);
    cname = xpost_string_allocate_cstring(ctx, namestr);
    if (!cname)
        return VMerror;
    if (strcmp(cname, "RunLengthEncode") != 0)
    {
        free(cname);
        return typecheck;
    }
    free(cname);
    if (!xpost_object_is_writeable(ctx, F))
        return invalidaccess;
    if (rec.int_.val < 0)
        return rangecheck;
    f = xpost_file_cons_filter_enc_rle(ctx->lo, F, rec.int_.val);
    if (xpost_object_get_type(f) == invalidtype)
        return ioerror;
    f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    f.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_WRITE << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
    return 0;
}

/* Layer the predictor stage over a decoding filter just built, taking
   its parameters from the filter's dictionary (PLRM Table 3.20). The
   filter object is read and replaced in place. */
static
int _layer_predictor(Xpost_Context *ctx, Xpost_Object dict, Xpost_Object *f)
{
    Xpost_Object p;
    int predictor, colors, bpc, columns;
    Xpost_Object layered;

    p = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "Predictor"));
    if (xpost_object_get_type(p) == invalidtype)
        return 0;
    if (xpost_object_get_type(p) != integertype)
        return typecheck;
    predictor = p.int_.val;
    if (predictor == 1)
        return 0;
    if (predictor != 2 && (predictor < 10 || predictor > 15))
        return rangecheck;

    colors = _dict_int(ctx, dict, "Colors", 1);
    bpc = _dict_int(ctx, dict, "BitsPerComponent", 8);
    columns = _dict_int(ctx, dict, "Columns", 1);
    if (colors < 1 || colors > 4 || columns < 1 || columns > (1 << 20))
        return rangecheck;
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)
        return rangecheck;
    /* the differencing the predictors undo is defined on whole bytes
       here, so a sample narrower than one is not screened out silently */
    if (bpc < 8)
        return rangecheck;

    layered = xpost_file_cons_filter_predictor(ctx->lo, *f, predictor,
                                               colors, bpc, columns);
    if (xpost_object_get_type(layered) == invalidtype)
        return ioerror;
    layered.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    layered.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_READ
                    << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    *f = layered;
    return 0;
}

/* file dict /FilterName  filter  file'
   the optional parameter dictionary form. LZWDecode reads its
   EarlyChange switch and CCITTFaxDecode its coding layout from the
   dictionary; the other decode filters take no parameter that
   changes their output here (DCTDecode reads its layout from the
   stream itself), so it is otherwise set aside */
static
int xpost_op_file_filter_dict (Xpost_Context *ctx,
                               Xpost_Object F,
                               Xpost_Object dict,
                               Xpost_Object name)
{
    Xpost_Object namestr;
    char *cname;

    namestr = xpost_name_get_string(ctx, name);
    cname = xpost_string_allocate_cstring(ctx, namestr);
    if (cname && (strcmp(cname, "CCITTFaxDecode") == 0
               || strcmp(cname, "CCITTFaxEncode") == 0))
    {
        Xpost_Object f;

        int enc = cname[8] == 'E';
        int access = enc ? XPOST_OBJECT_TAG_ACCESS_FILE_WRITE
                         : XPOST_OBJECT_TAG_ACCESS_FILE_READ;

        free(cname);
        if (enc ? !xpost_object_is_writeable(ctx, F)
                : !xpost_object_is_readable(ctx, F))
            return invalidaccess;
        f = (enc ? xpost_file_cons_filter_enc_ccitt
                 : xpost_file_cons_filter_ccitt)(ctx->lo, F,
                _dict_int(ctx, dict, "K", 0),
                _dict_int(ctx, dict, "Columns", 1728),
                _dict_int(ctx, dict, "Rows", 0),
                _dict_int(ctx, dict, "BlackIs1", 0),
                _dict_int(ctx, dict, "EncodedByteAlign", 0),
                _dict_int(ctx, dict, "EndOfLine", 0),
                _dict_int(ctx, dict, "EndOfBlock", 1));
        if (xpost_object_get_type(f) == invalidtype)
            return ioerror;
        f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        f.tag |= (unsigned int)access << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET;
        xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
        return 0;
    }
#ifdef HAVE_LIBJPEG
    if (cname && strcmp(cname, "DCTEncode") == 0)
    {
        Xpost_Object f, v;
        int hs[4] = { 1, 1, 1, 1 }, vs[4] = { 1, 1, 1, 1 };
        int cols = _dict_int(ctx, dict, "Columns", 0);
        int rows = _dict_int(ctx, dict, "Rows", 0);
        int colors = _dict_int(ctx, dict, "Colors", 0);
        int ct = _dict_int(ctx, dict, "ColorTransform", colors == 3);
        double qf = 1.0;
        int i;

        free(cname);
        if (!xpost_object_is_writeable(ctx, F))
            return invalidaccess;
        if (cols < 1 || rows < 1 || colors < 1 || colors > 4)
            return rangecheck;
        v = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "QFactor"));
        if (xpost_object_get_type(v) == realtype)
            qf = v.real_.val;
        else if (xpost_object_get_type(v) == integertype)
            qf = (double)v.int_.val;
        if (qf <= 0.0)
            return rangecheck;
        v = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "HSamples"));
        if (xpost_object_get_type(v) == arraytype
            && (integer)v.comp_.sz >= colors)
            for (i = 0; i < colors; i++)
            {
                Xpost_Object e = xpost_array_get(ctx, v, i);

                if (xpost_object_get_type(e) == integertype
                    && e.int_.val >= 1 && e.int_.val <= 4)
                    hs[i] = e.int_.val;
            }
        v = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "VSamples"));
        if (xpost_object_get_type(v) == arraytype
            && (integer)v.comp_.sz >= colors)
            for (i = 0; i < colors; i++)
            {
                Xpost_Object e = xpost_array_get(ctx, v, i);

                if (xpost_object_get_type(e) == integertype
                    && e.int_.val >= 1 && e.int_.val <= 4)
                    vs[i] = e.int_.val;
            }
        f = xpost_file_cons_filter_enc_dct(ctx->lo, F, cols, rows, colors,
                                           qf, ct, hs, vs);
        if (xpost_object_get_type(f) == invalidtype)
            return ioerror;
        f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        f.tag |= XPOST_OBJECT_TAG_ACCESS_FILE_WRITE << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET;
        xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
        return 0;
    }
#endif
    if (cname && (strcmp(cname, "LZWDecode") == 0
               || strcmp(cname, "LZWEncode") == 0))
    {
        Xpost_Object ec, f;
        int early = 1;
        int enc = cname[3] == 'E';
        int access = enc ? XPOST_OBJECT_TAG_ACCESS_FILE_WRITE
                         : XPOST_OBJECT_TAG_ACCESS_FILE_READ;

        free(cname);
        if (enc ? !xpost_object_is_writeable(ctx, F)
                : !xpost_object_is_readable(ctx, F))
            return invalidaccess;
        ec = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "EarlyChange"));
        if (xpost_object_get_type(ec) == integertype)
        {
            /* EarlyChange is 0 or 1 (PLRM 3.13.3). Any other value -- a
               negative one, or one large enough to overflow the sum -- would
               defeat the code-table reset test it feeds and let the code
               counter run past the fixed table the encoder writes into. */
            if (ec.int_.val != 0 && ec.int_.val != 1)
                return rangecheck;
            early = ec.int_.val;
        }
        f = (enc ? xpost_file_cons_filter_enc_lzw
                 : xpost_file_cons_filter_lzw)(ctx->lo, F, early);
        if (xpost_object_get_type(f) == invalidtype)
            return ioerror;
        f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        f.tag |= (unsigned int)access << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET;
        if (!enc)
        {
            int ret = _layer_predictor(ctx, dict, &f);

            if (ret)
                return ret;
        }
        xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
        return 0;
    }
    if (cname && strcmp(cname, "ReusableStreamDecode") == 0)
    {
        /* the dictionary may name a decode chain to run the source
           through before buffering: layer each in order */
        Xpost_Object flt = xpost_dict_get(ctx, dict,
            xpost_name_cons(ctx, "Filter"));
        Xpost_Object cur = F;
        Xpost_Object first = xpost_object_cvlit(null);
        int ret;

        free(cname);
        if (xpost_object_get_type(flt) == nametype)
        {
            ret = xpost_op_file_filter(ctx, cur, flt);
            if (ret)
                return ret;
            cur = xpost_stack_pop(ctx->lo, ctx->os);
            first = cur;
        }
        else if (xpost_object_get_type(flt) == arraytype)
        {
            unsigned int i;

            for (i = 0; i < flt.comp_.sz; i++)
            {
                Xpost_Object fn = xpost_array_get(ctx, flt, i);

                if (xpost_object_get_type(fn) != nametype)
                    return typecheck;
                ret = xpost_op_file_filter(ctx, cur, fn);
                if (ret)
                    return ret;
                cur = xpost_stack_pop(ctx->lo, ctx->os);
                if (i == 0)
                    first = cur;
            }
        }
        ret = xpost_op_file_filter(ctx, cur,
            xpost_name_cons(ctx, "ReusableStreamDecode"));
        if (ret)
            return ret;
        /* the reusable stream buffered everything its chain yields,
           but an inner stage can reach its own end before the stage
           against the file has consumed its end-of-data marker:
           drain that stage so the file resumes past the encoding */
        if (xpost_object_get_type(first) == filetype)
        {
            /* re-derive the pointer each read: a filter's read can
               grow the memory file and move it */
            for (;;)
            {
                Xpost_File *dr = xpost_file_get_file_pointer(ctx->lo, first);

                if (!dr || xpost_file_getc(dr) == EOF)
                    break;
            }
        }
        return 0;
    }
    /* SubFileDecode takes its two parameters positionally as well, but the
       dictionary is the general way to give a filter its parameters
       (PLRM 3.13.3), and this filter reaches its end of data by them. */
    if (cname && (strcmp(cname, "SubFileDecode") == 0))
    {
        Xpost_Object f;
        Xpost_Object eod;
        char *ceod = NULL;
        int eodlen = 0;

        Xpost_Object cnt;

        free(cname);
        if (!xpost_object_is_readable(ctx, F))
            return invalidaccess;
        /* both parameters are required of the dictionary form (PLRM
           3.13.3) */
        eod = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "EODString"));
        cnt = xpost_dict_get(ctx, dict, xpost_name_cons(ctx, "EODCount"));
        if ((xpost_object_get_type(eod) == invalidtype)
            || (xpost_object_get_type(cnt) == invalidtype))
            return undefined;
        if ((xpost_object_get_type(eod) != stringtype)
            || (xpost_object_get_type(cnt) != integertype))
            return typecheck;
        if (!xpost_object_is_readable(ctx, eod))
            return invalidaccess;
        if (cnt.int_.val < 0)
            return rangecheck;
        ceod = xpost_string_get_pointer(ctx, eod);
        eodlen = eod.comp_.sz;
        f = xpost_file_cons_filter_subfile(ctx->lo, F,
                cnt.int_.val, ceod, eodlen);
        if (xpost_object_get_type(f) == invalidtype)
            return ioerror;
        f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        f.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_READ
                  << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
        xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
        return 0;
    }
    /* the two compressing decoders may have been given a predictor;
       everything else takes its parameters and needs no stage */
    if (cname && strcmp(cname, "FlateDecode") == 0)
    {
        Xpost_Object f;
        int ret;

        free(cname);
        ret = xpost_op_file_filter(ctx, F, name);
        if (ret)
            return ret;
        f = xpost_stack_pop(ctx->lo, ctx->os);
        ret = _layer_predictor(ctx, dict, &f);
        if (ret)
            return ret;
        xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
        return 0;
    }
    free(cname);
    return xpost_op_file_filter(ctx, F, name);
}

/* file count string /SubFileDecode  filter  file'
   pass bytes through until the delimiter string has occurred count
   times (an empty string makes count a plain byte count) */
static
int xpost_op_file_filter_subfile (Xpost_Context *ctx,
                                  Xpost_Object F,
                                  Xpost_Object count,
                                  Xpost_Object eod,
                                  Xpost_Object name)
{
    Xpost_Object namestr;
    char *cname;
    int match;
    Xpost_Object f;
    char eodbuf[64];

    if (!xpost_object_is_readable(ctx, F))
        return invalidaccess;
    namestr = xpost_name_get_string(ctx, name);
    cname = xpost_string_allocate_cstring(ctx, namestr);
    if (!cname)
        return VMerror;
    match = strcmp(cname, "SubFileDecode") == 0;
    free(cname);
    if (!match)
        return undefined;
    if (eod.comp_.sz > sizeof(eodbuf))
        return rangecheck;
    memcpy(eodbuf, xpost_string_get_pointer(ctx, eod), eod.comp_.sz);

    f = xpost_file_cons_filter_subfile(ctx->lo, F, count.int_.val, eodbuf, (int)eod.comp_.sz);
    if (xpost_object_get_type(f) == invalidtype)
        return ioerror;
    f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    f.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_READ << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
    return 0;
}

/* file  closefile  -
   close file object */
static
int xpost_op_file_closefile (Xpost_Context *ctx,
                             Xpost_Object f)
{
    int ret;
    ret = xpost_file_object_close(ctx->lo, f);
    if (ret)
        return ret;
    return 0;
}

/* file  read  int true
               false
   read a byte from file */
static
int xpost_op_file_read(Xpost_Context *ctx,
                       Xpost_Object f)
{
    Xpost_Object b;
    if (!xpost_object_is_readable(ctx,f))
        return invalidaccess;
    b = xpost_file_read_byte(ctx->lo, f);
    if (xpost_object_get_type(b) == invalidtype)
    {
        /* a closed file reads as end-of-data rather than erroring */
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    if (b.int_.val != EOF)
    {
        xpost_stack_push(ctx->lo, ctx->os, b);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    }
    else
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    }
    return 0;
}

/* pass the bytes to a registered handler when the file is the
   process's standard output or error stream; returns 1 when the
   write was diverted, -1 when the handler refused it, 0 when the
   write should proceed normally */
static
int _divert_output(Xpost_Context *ctx, Xpost_File *f,
                   const char *buf, size_t len)
{
    FILE *stream = xpost_file_stdio_stream_get(f);
    if (stream == stdout && ctx->stdout_fn)
        return ctx->stdout_fn(ctx->stdout_user, buf, len) == len ? 1 : -1;
    if (stream == stderr && ctx->stderr_fn)
        return ctx->stderr_fn(ctx->stderr_user, buf, len) == len ? 1 : -1;
    return 0;
}

/* file int  write  -
   write a byte to file */
static
int xpost_op_file_write (Xpost_Context *ctx,
                         Xpost_Object f,
                         Xpost_Object i)
{
    int ret;
    if (!xpost_object_is_writeable(ctx, f))
        return invalidaccess;
    {
        char c = (char)i.int_.val;
        int d = _divert_output(ctx,
                xpost_file_get_file_pointer(ctx->lo, f), &c, 1);
        if (d < 0) return ioerror;
        if (d) return 0;
    }
    ret = xpost_file_write_byte(ctx->lo, f, i);
    if (ret)
        return ret;
    return 0;
}

const char *hex = "0123456789" "ABCDEF" "abcdef";

static
int read_hex_digit( Xpost_File *f, int *p )
{
    int eof = 0;
    do
        if ((*p = xpost_file_getc(f)) == EOF)
            ++eof;
    while ( !eof && !strchr(hex, *p) );
    return eof;
}

/* file string  readhexstring  substring true
                               substring false
   read hex-encoded data from file into string */
static
int xpost_op_file_readhexstring (Xpost_Context *ctx,
                                 Xpost_Object F,
                                 Xpost_Object S)
{
    word n;
    int c[2];
    int eof = 0;
    Xpost_File *f;
    char *s;
    if (!xpost_file_get_status(ctx->lo, F))
    {
        /* a closed file reads as end-of-data rather than erroring */
        S.comp_.sz = 0;
        xpost_stack_push(ctx->lo, ctx->os, S);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    if (!xpost_object_is_readable(ctx,F))
        return invalidaccess;
    /* the decoded bytes are stored into string, so the string must permit
       a write: a read-only or no-access destination is refused rather
       than overwritten (PLRM 8.2) */
    if (!xpost_object_is_writeable(ctx, S))
        return invalidaccess;
    f = xpost_file_get_file_pointer(ctx->lo, F);
    s = xpost_string_get_pointer(ctx, S);

    for (n = 0; n < S.comp_.sz; n++)
    {
        eof = read_hex_digit(f, &c[0]);
        XPOST_LOG_INFO("read %c", c[0]);
        if (!eof) eof = read_hex_digit(f, &c[1]);
        if (eof) break;
        XPOST_LOG_INFO("read %c", c[1]);
        s[n] = ((strchr(hex, toupper(c[0])) - hex) << 4)
             + (strchr(hex, toupper(c[1])) - hex);
    }
    fflush(stdout);
    S.comp_.sz = n;
    xpost_stack_push(ctx->lo, ctx->os, S);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(!eof));
    return 0;
}

/* file string  writehexstring  -
   write string to file in hex-encoding */
static
int xpost_op_file_writehexstring (Xpost_Context *ctx,
                                  Xpost_Object F,
                                  Xpost_Object S)
{
    word n;
    Xpost_File *f;
    char *s;
    if (!xpost_file_get_status(ctx->lo, F))
        return ioerror;
    if (!xpost_object_is_writeable(ctx, F))
        return invalidaccess;
    f = xpost_file_get_file_pointer(ctx->lo, F);
    s = xpost_string_get_pointer(ctx, S);

    for (n = 0; n < S.comp_.sz; n++)
    {
        char h[2];
        int d;
        /* char is signed on most platforms; hex is indexed by value */
        unsigned char b = (unsigned char)s[n];
        h[0] = hex[b / 16];
        h[1] = hex[b % 16];
        d = _divert_output(ctx, f, h, 2);
        if (d < 0) return ioerror;
        if (d) continue;
        if (xpost_file_putc(f, h[0]) == EOF)
            return ioerror;
        if (xpost_file_putc(f, h[1]) == EOF)
            return ioerror;
    }
    return 0;
}

/* file string  readstring  substring true
                            substring false
   read from file into string */
static
int xpost_op_file_readstring (Xpost_Context *ctx,
                              Xpost_Object F,
                              Xpost_Object S)
{
    integer n;
    Xpost_File *f;
    char *s;
    if (!xpost_object_is_readable(ctx,F))
        return invalidaccess;
    /* a zero-length string could hold nothing, so asking to fill one is an
       error rather than a transfer of no bytes (PLRM 8.2) */
    if (S.comp_.sz == 0)
        return rangecheck;
    if (!xpost_file_get_status(ctx->lo, F))
    {
        /* a closed file reads as end-of-data rather than erroring */
        S.comp_.sz = 0;
        xpost_stack_push(ctx->lo, ctx->os, S);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    /* the transfer stores into string, so the string must permit a write:
       a read-only or no-access destination is refused rather than
       overwritten (PLRM 8.2) */
    if (!xpost_object_is_writeable(ctx, S))
        return invalidaccess;
    f = xpost_file_get_file_pointer(ctx->lo, F);
    s = xpost_string_get_pointer(ctx, S);
    n = xpost_file_read(s, 1, S.comp_.sz, f);
    /* the count read is compared against the string's own length in the
       wider signed type: a read that answered short answers short */
    if (n == (integer)S.comp_.sz)
    {
        xpost_stack_push(ctx->lo, ctx->os, S);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    }
    else
    {
        S.comp_.sz = n;
        xpost_stack_push(ctx->lo, ctx->os, S);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
    }
    return 0;
}

/* file string  writestring  -
   write string to file */
static
int xpost_op_file_writestring (Xpost_Context *ctx,
                               Xpost_Object F,
                               Xpost_Object S)
{
    Xpost_File *f;
    char *s;
    if (!xpost_file_get_status(ctx->lo, F))
        return ioerror;
    if (!xpost_object_is_writeable(ctx, F))
        return invalidaccess;
    f = xpost_file_get_file_pointer(ctx->lo, F);
    s = xpost_string_get_pointer(ctx, S);
    {
        int d = _divert_output(ctx, f, s, S.comp_.sz);
        if (d < 0) return ioerror;
        if (d) return 0;
    }
    /* the count written is compared against the string's own length in
       the wider signed type: a write that answered short answers short */
    if (xpost_file_write(s, 1, S.comp_.sz, f) != (integer)S.comp_.sz)
        return ioerror;
    return 0;
}

/* file string  readline  substring true
                          substring false
   read a line of text from file */
static
int xpost_op_file_readline (Xpost_Context *ctx,
                            Xpost_Object F,
                            Xpost_Object S)
{
    Xpost_File *f;
    char *s;
    word n;
    int c = ' ';
    if (!xpost_file_get_status(ctx->lo, F))
    {
        /* a closed file reads as end-of-data rather than erroring */
        S.comp_.sz = 0;
        xpost_stack_push(ctx->lo, ctx->os, S);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    if (!xpost_object_is_readable(ctx,F))
        return invalidaccess;
    /* the line is stored into string, so the string must permit a write:
       a read-only or no-access destination is refused rather than
       overwritten (PLRM 8.2) */
    if (!xpost_object_is_writeable(ctx, S))
        return invalidaccess;
    f = xpost_file_get_file_pointer(ctx->lo, F);
    s = xpost_string_get_pointer(ctx, S);
    for (n = 0; n < S.comp_.sz; n++)
    {
        c = xpost_file_getc(f);
        if (c == EOF || c == '\n')
            break;
        if (c == '\r')
        {
            /* CR, LF and CRLF each end the line (PLRM 3.8): consume
               the whole marker, return the line without it */
            int c2 = xpost_file_getc(f);
            if (c2 != '\n' && c2 != EOF)
                xpost_file_ungetc(f, c2);
            break;
        }
        s[n] = c;
    }
    if (n == S.comp_.sz)
    {
        /* The string is exactly full. A line that ends here has been read,
           not overrun, so look at the next character: a terminator (whole,
           for CRLF) or end of file completes the line, and only a further
           character of text means the line does not fit (PLRM 8.2). */
        c = xpost_file_getc(f);
        if (c == '\r')
        {
            int c2 = xpost_file_getc(f);
            if (c2 != '\n' && c2 != EOF)
                xpost_file_ungetc(f, c2);
        }
        else if (c != '\n' && c != EOF)
        {
            xpost_file_ungetc(f, c);
            return rangecheck;
        }
    }
    S.comp_.sz = n;
    xpost_stack_push(ctx->lo, ctx->os, S);
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(c != EOF));
    return 0;
}

/* file  bytesavailable  int
   return number of bytes available to read or -1 if not known.
   The count is a count of what a read would deliver, so the access
   attribute that governs reading governs asking for it too (PLRM
   3.8.2). */
static
int xpost_op_file_bytesavailable (Xpost_Context *ctx,
                                  Xpost_Object F)
{
    int bytes;
    int ret;
    if (!xpost_object_is_readable(ctx, F))
        return invalidaccess;
    ret = xpost_file_get_bytes_available(ctx->lo, F, &bytes);
    if (ret)
        return ret;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(bytes));
    return 0;
}

/* -  flush  -
   flush all output buffers */
static
int xpost_op_flush (Xpost_Context *ctx)
{
    int ret;
    (void)ctx;
    ret = fflush(NULL);
    if (ret != 0)
        return ioerror;
    return 0;
}

/* file  flushfile  -
   An output file writes through whatever it has buffered; an input file
   is read and discarded to end of data (PLRM 8.2). Each stream carries
   the direction it was opened in and its flush method does the half that
   belongs to it, so a filter's own way of reaching the end of its data is
   what runs when the file is one. A file that grants neither access is
   left alone. */
static
int xpost_op_file_flushfile (Xpost_Context *ctx,
                             Xpost_Object F)
{
    Xpost_File *f;

    if (!xpost_file_get_status(ctx->lo, F)) return 0;
    if (!xpost_object_is_readable(ctx, F) && !xpost_object_is_writeable(ctx, F))
        return 0;
    f = xpost_file_get_file_pointer(ctx->lo, F);
    if (xpost_file_flush(f) != 0)
        return ioerror;
    return 0;
}

/* file  resetfile  -
   discard the file's buffered characters (PLRM). A reusable, filter or
   in-memory stream rewinds to the start of its held data; a disk file
   flushes its standard-I/O buffer, which is a no-op where the platform
   offers no way to purge one. */
static
int xpost_op_file_resetfile (Xpost_Context *ctx,
                             Xpost_Object F)
{
    Xpost_File *f;
    if (!xpost_file_get_status(ctx->lo, F)) return 0;
    f = xpost_file_get_file_pointer(ctx->lo, F);
    xpost_file_purge(f);
    return 0;
}

/* file  status  bool
   return bool indicating whether file object is active or closed */
static
int xpost_op_file_status (Xpost_Context *ctx,
                          Xpost_Object F)
{
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(xpost_file_get_status(ctx->lo, F)));
    return 0;
}

/* A quantity the file system reports, as the integer object that carries
   it. The file system counts in its own width and the object counts in
   the integer's, and the two are not the same width everywhere: a count
   past what the integer holds has no integer to be reported as, and the
   integer it would be narrowed to is a different count -- a length of
   three thousand million bytes narrows to a negative length. So it is
   refused rather than reported. */
static
int _as_integer(long long v, integer *out)
{
    *out = (integer)v;
    return (long long)*out == v;
}

/* string  status  pages bytes referred created true | false
   report on a named file: its block count, size, and access and modification
   times if it exists and the sandbox permits it, otherwise false.

   PLRM 8.2 has bytes be the file's length and has larger times mean later
   ones, so each of the four is refused rather than narrowed: a narrowed
   length is not the length, and narrowed times do not keep their order. */
static
int xpost_op_string_status (Xpost_Context *ctx,
                            Xpost_Object S)
{
    char *sbuf;
    long long pages, bytes, referred, created;
    integer ipages, ibytes, ireferred, icreated;
    int exists;
    int ret;

    ret = xpost_op_file_name(ctx, S, &sbuf);
    if (ret == VMerror)
        return VMerror;
    if (ret)
    {
        /* PLRM 8.2 has status answer whether there is a file by the name
           given, and a name no file can hold is a name no file has */
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    exists = xpost_diskfile_stat(sbuf, &pages, &bytes, &referred, &created);
    free(sbuf);
    if (exists)
    {
        if (!_as_integer(pages, &ipages)
         || !_as_integer(bytes, &ibytes)
         || !_as_integer(referred, &ireferred)
         || !_as_integer(created, &icreated))
            return limitcheck;
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ipages));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ibytes));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ireferred));
        xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(icreated));
    }
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(exists));
    return 0;
}

/* -  currentfile  file
   return topmost file from the exec stack. The result carries the
   literal attribute (PLRM): programs stash it under a name and a
   later lookup must push the stashed file, not resume executing it. */
static
int xpost_op_currentfile (Xpost_Context *ctx)
{
    Xpost_Object o;
    /* topmost file on the exec stack, found in a single top-down pass (the
       former topdown_fetch-per-index loop was O(depth^2)) */
    if (xpost_stack_topdown_find_type(ctx->lo, ctx->es, filetype, &o) >= 0)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(o));
        return 0;
    }
    o = xpost_file_cons(ctx->lo, NULL, 1);
    if (xpost_object_get_type(o) == invalidtype)
        return VMerror;
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(o));
    return 0;
}

/* string  deletefile  -
   delete named file from filesystem */
static
int xpost_op_string_deletefile (Xpost_Context *ctx,
                                Xpost_Object S)
{
    char *sbuf;
    int ret;

    ret = xpost_op_file_name(ctx, S, &sbuf);
    if (ret)
        return ret;
    if (xpost_diskfile_remove(sbuf, &ret) != 0)
    {
        free(sbuf);
        return ret;
    }
    free(sbuf);
    return 0;
}

/* old new  renamefile  -
   rename old file to new in filesystem */
static
int xpost_op_string_renamefile (Xpost_Context *ctx,
                                Xpost_Object Old,
                                Xpost_Object New)
{
    char *oldbuf, *newbuf;
    int ret;

    /* both names are read before either is acted on, so a rename one of
       them refuses moves nothing */
    ret = xpost_op_file_name(ctx, Old, &oldbuf);
    if (ret)
        return ret;
    ret = xpost_op_file_name(ctx, New, &newbuf);
    if (ret)
    {
        free(oldbuf);
        return ret;
    }

    if (xpost_diskfile_rename(oldbuf, newbuf, &ret) != 0)
    {
        free(oldbuf);
        free(newbuf);
        return ret;
    }
    free(oldbuf);
    free(newbuf);
    return 0;
}

//#ifndef _WIN32

/* The object the matched paths of an enumeration ride the execution stack
   as. It carries the number they are held under and nothing else, so a
   copy of it is a name for them and not a way to reach them. */
static
Xpost_Object xpost_glob_cons(unsigned int id)
{
    Xpost_Object o;

    o.glob_.tag = globtype;
    o.glob_.pad = 0;
    o.glob_.id = id;
    return o;
}

/* internal continuation operator for filenameforall.

   The cursor into the matched paths rides the execution stack beside the
   glob as an integer of its own, in the shape the loop-continuation
   operators already use for their state. How many paths there are is a
   property of the directory, not of the build, so no field of the object
   is wide enough to index them and the count is carried in a type that
   reaches the whole array. */
static
int xpost_op_contfilenameforall (Xpost_Context *ctx,
                                 Xpost_Object oglob,
                                 Xpost_Object ocursor,
                                 Xpost_Object Proc,
                                 Xpost_Object Scr)
{
    glob_t *globbuf;
    size_t cursor;
    char *str;
    char *src;
    int len;
    Xpost_Object interval;

    globbuf = xpost_context_glob_held(ctx, (unsigned int)oglob.glob_.id);
    /* the enumeration whose paths these were has ended and given them
       back, so the object names none */
    if (!globbuf)
        return undefined;
    cursor = (size_t)ocursor.int_.val;
    /* skip entries the engaged sandbox would not let the program open, so
       a listing cannot disclose names outside the permitted set */
    while (cursor < globbuf->gl_pathc
           && !xpost_diskfile_readable(globbuf->gl_pathv[cursor]))
        ++cursor;
    if (cursor < globbuf->gl_pathc)
    {
        src = globbuf->gl_pathv[cursor];
        ++cursor;
        xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, contfilenameforall));
        xpost_stack_push(ctx->lo, ctx->es, Scr);
        xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, cvx));
        xpost_stack_push(ctx->lo, ctx->es, xpost_object_cvlit(Proc));
        xpost_stack_push(ctx->lo, ctx->es, xpost_int_cons((integer)cursor));
        xpost_stack_push(ctx->lo, ctx->es, oglob);

        /* after the pushes: a push may grow the memory file and move its
           base, and the scratch string lives in it */
        str = xpost_string_get_pointer(ctx, Scr);
        len = strlen(src);
        /* the name's length is compared against the scratch string's own
           in the wider signed type, so a length is short of the string
           only by being short of it */
        if (len > (integer)Scr.comp_.sz)
            return rangecheck;
        memcpy(str, src, len);
        interval = xpost_object_get_interval(Scr, 0, len);
        if (xpost_object_get_type(interval) == invalidtype)
            return rangecheck;
        xpost_stack_push(ctx->lo, ctx->os, interval);
        xpost_stack_push(ctx->lo, ctx->es, Proc);

    }
    else
    {
        /* iteration is complete and the reference and its cursor have
           already been popped: drop the sentinel the loop frame keeps
           beneath the continuation, then release the matched paths and
           their container */
        (void)xpost_stack_pop(ctx->lo, ctx->es);
        xpost_context_glob_release(ctx, (unsigned int)oglob.glob_.id);
    }
    return 0;
}

/* template proc scratch  filenameforall  -
   execute proc for all filenames matching template using scratch string */
static
int xpost_op_filenameforall (Xpost_Context *ctx,
                             Xpost_Object Tmp,
                             Xpost_Object Proc,
                             Xpost_Object Scr)
{
    char *tmpbuf;
    glob_t *globbuf;
    Xpost_Object oglob = { 0 };
    unsigned int id;
    int ret;

    ret = xpost_op_file_name(ctx, Tmp, &tmpbuf);
    if (ret == VMerror)
        return VMerror;
    if (ret)
        /* no file name holds a NUL, so a template holding one matches
           none of them: an empty enumeration, as for any other template
           nothing matches */
        return 0;
    globbuf = malloc(sizeof *globbuf);
    if (!globbuf){
        free(tmpbuf);
        return unregistered;
    }
    ret = xpost_glob(tmpbuf, globbuf);
    if (ret != 0)
    {
        free(tmpbuf);
        free(globbuf);
        /* the enumeration covers the files whose names match the template
           (PLRM): a template none matches enumerates nothing, which is not a
           failure of the file system. Only exhausted memory is reported. */
#ifdef GLOB_NOSPACE
        if (ret == GLOB_NOSPACE)
            return VMerror;
#endif
        return 0;
    }

    if (!xpost_context_glob_hold(ctx, globbuf, &id))
    {
        xpost_glob_free(globbuf);
        free(globbuf);
        free(tmpbuf);
        return VMerror;
    }
    oglob = xpost_glob_cons(id);

    /* loop frame: the sentinel loop operator (which exit searches for)
       stays beneath the per-iteration continuation until iteration
       completes or exit finds it */
    if (!xpost_stack_push(ctx->lo, ctx->es,
                          XPOST_OP(ctx, filenameforall)))
    {
        xpost_context_glob_release(ctx, id);
        free(tmpbuf);
        return execstackoverflow;
    }

    ret = xpost_op_contfilenameforall(ctx, oglob, xpost_int_cons(0),
                                      Proc, xpost_object_cvlit(Scr));
    free(tmpbuf);
    return ret;
}

//#endif

/* file int  setfileposition  -
   set position of read/write head for file */
static
int xpost_op_setfileposition (Xpost_Context *ctx,
                              Xpost_Object F,
                              Xpost_Object pos)
{
    int ret;

    if (!xpost_file_get_status(ctx->lo, F))
        return ioerror;
    ret = xpost_file_seek(xpost_file_get_file_pointer(ctx->lo, F), pos.int_.val);
    if (ret != 0)
        return ioerror;
    return 0;
}

/* file  fileposition  int
   return position of read/write head for file */
static
int xpost_op_fileposition (Xpost_Context *ctx,
                           Xpost_Object F)
{
    long long pos;
    integer ipos;
    if (!xpost_file_get_status(ctx->lo, F))
        return ioerror;
    pos = xpost_file_tell(xpost_file_get_file_pointer(ctx->lo, F));
    if (pos == -1)
        return ioerror;
    /* PLRM 8.2 has the result be a nonnegative count of bytes from the
       start of the file, which a position past the integer's width has no
       way to be: narrowed, it comes back as a position further back */
    if (!_as_integer(pos, &ipos))
        return limitcheck;
    xpost_stack_push(ctx->lo, ctx->os, xpost_int_cons(ipos));
    return 0;
}

/* string  print  -
   write string to stdout */
static
int xpost_op_string_print (Xpost_Context *ctx,
                           Xpost_Object S)
{
    size_t ret;
    char *s;

    /* the string's characters are read out, so it needs read access */
    if (!xpost_object_is_readable(ctx, S))
        return invalidaccess;
    s = xpost_string_get_pointer(ctx, S);
    if (ctx->stdout_fn)
    {
        if (ctx->stdout_fn(ctx->stdout_user, s, S.comp_.sz) != S.comp_.sz)
            return ioerror;
        return 0;
    }
    ret = fwrite(s, 1, S.comp_.sz, stdout);
    if (ret != S.comp_.sz)
        return ioerror;
    return 0;
}


/* Binary object sequences (PLRM 3.14.6), the writing half of the
   scanner's reader: a header names the format, one top-level record
   carries the operand and its tag, subsidiary records and text
   follow, every offset relative to the records' start. The object
   format parameter chooses the number representation; disabled (the
   default) the writers refuse with undefined. */

/* The parameter lives in the interpreter's private namespace, so a
   program reaches it only through the two operators and cannot write a
   value the range check below never saw. The put is recorded against
   the save in force like any dictionary write, which is what makes the
   parameter subject to save and restore; a context whose namespace is
   not built yet has no parameter to read, which reads as disabled. */
static
int _objfmt_get(Xpost_Context *ctx)
{
    Xpost_Object v;

    if (xpost_object_get_type(ctx->privatedict) != dicttype)
        return 0;
    v = xpost_dict_get(ctx, ctx->privatedict,
        xpost_name_cons(ctx, ".objectformat"));

    return xpost_object_get_type(v) == integertype ? v.int_.val : 0;
}

static
int xpost_op_int_setobjectformat(Xpost_Context *ctx,
                                 Xpost_Object n)
{
    if (n.int_.val < 0 || n.int_.val > 4)
        return rangecheck;
    if (xpost_object_get_type(ctx->privatedict) != dicttype)
        return undefined;
    return xpost_dict_put(ctx, ctx->privatedict,
        xpost_name_cons(ctx, ".objectformat"), n);
}

static
int xpost_op_currentobjectformat(Xpost_Context *ctx)
{
    if (!xpost_stack_push(ctx->lo, ctx->os,
                          xpost_int_cons(_objfmt_get(ctx))))
        return stackoverflow;
    return 0;
}

static
/* A binary object sequence for interchange is small; this ceiling on its
   measured size is far above any real one. It bounds two things a hostile
   program could otherwise make unbounded: the total size, so it cannot wrap
   the width it is computed in and leave the buffer allocated too small for
   what is then written into it; and the walk that measures it, which shared
   references -- a graph rather than a tree -- could otherwise make
   exponential, since the walk does not remember a node it has already
   counted. The check rides at the head of the walk, so a subtree that has
   already carried the count past the ceiling ends it there. */
#define BOS_SEQ_MAX ((size_t)1 << 27)

int _bos_measure(Xpost_Context *ctx,
                 Xpost_Object o,
                 int depth,
                 size_t *recs,
                 size_t *data)
{
    unsigned int i;
    int ret;

    if (depth > 32)
        return limitcheck;
    if (*recs + *data > BOS_SEQ_MAX)
        return limitcheck;
    switch (xpost_object_get_type(o))
    {
        case nulltype:
        case integertype:
        case realtype:
        case booleantype:
        case marktype:
            *recs += 8;
            return 0;
        case nametype:
        {
            Xpost_Object str = xpost_name_get_string(ctx, o);

            if (str.comp_.sz > 127)
                return limitcheck;
            *recs += 8;
            *data += str.comp_.sz;
            return 0;
        }
        case stringtype:
            *recs += 8;
            *data += o.comp_.sz;
            return 0;
        case arraytype:
            *recs += 8;
            for (i = 0; i < o.comp_.sz; i++)
            {
                ret = _bos_measure(ctx, xpost_array_get(ctx, o, i),
                                   depth + 1, recs, data);
                if (ret)
                    return ret;
            }
            return 0;
        default:
            return typecheck;
    }
}

typedef struct
{
    Xpost_Context *ctx;
    unsigned char *buf;
    unsigned int base;      /* records' start: the header length */
    unsigned int nextrec;   /* free record space, relative to base */
    unsigned int nextdata;  /* free text space, relative to base */
    int le;
} Bos;

static void
_bos_put16(const Bos *b, unsigned char *p, unsigned int v)
{
    if (b->le) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
    else       { p[0] = (v >> 8) & 0xff; p[1] = v & 0xff; }
}

static void
_bos_put32(const Bos *b, unsigned char *p, unsigned int v)
{
    if (b->le)
    {
        p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
        p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
    }
    else
    {
        p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff;
        p[2] = (v >> 8) & 0xff; p[3] = v & 0xff;
    }
}

static
int _bos_emit(Bos *b,
              Xpost_Object o,
              int tag,
              unsigned int recoff)
{
    unsigned char *p = b->buf + b->base + recoff;
    unsigned char x = xpost_object_is_exe(o) ? 0x80 : 0;
    unsigned int i;
    int ret;

    p[1] = (unsigned char)tag;
    switch (xpost_object_get_type(o))
    {
        case nulltype:
            p[0] = 0;
            break;
        case marktype:
            p[0] = 10;
            break;
        case integertype:
            p[0] = 1;
            _bos_put32(b, p + 4, (unsigned int)o.int_.val);
            break;
        case booleantype:
            p[0] = 4;
            _bos_put32(b, p + 4, o.int_.val ? 1 : 0);
            break;
        case realtype:
            /* the native-real formats travel in the sequence's byte
               order all the same: the deployed writers agree on that
               reading of native, and interchange follows them */
            p[0] = 2;
            {
                float f = (float)o.real_.val;
                unsigned int v;

                memcpy(&v, &f, 4);
                _bos_put32(b, p + 4, v);
            }
            break;
        case nametype:
        {
            Xpost_Object str = xpost_name_get_string(b->ctx, o);

            p[0] = 3 | x;
            _bos_put16(b, p + 2, str.comp_.sz);
            _bos_put32(b, p + 4, b->nextdata);
            memcpy(b->buf + b->base + b->nextdata,
                   xpost_string_get_pointer(b->ctx, str), str.comp_.sz);
            b->nextdata += str.comp_.sz;
            break;
        }
        case stringtype:
            p[0] = 5 | x;
            _bos_put16(b, p + 2, o.comp_.sz);
            _bos_put32(b, p + 4, b->nextdata);
            if (o.comp_.sz)
                memcpy(b->buf + b->base + b->nextdata,
                       xpost_string_get_pointer(b->ctx, o), o.comp_.sz);
            b->nextdata += o.comp_.sz;
            break;
        case arraytype:
        {
            unsigned int block = b->nextrec;

            p[0] = 9 | x;
            _bos_put16(b, p + 2, o.comp_.sz);
            _bos_put32(b, p + 4, block);
            b->nextrec += o.comp_.sz * 8;
            for (i = 0; i < o.comp_.sz; i++)
            {
                ret = _bos_emit(b, xpost_array_get(b->ctx, o, i),
                                0, block + i * 8);
                if (ret)
                    return ret;
            }
            break;
        }
        default:
            return typecheck;
    }
    return 0;
}

/* build the sequence for one top-level object; the caller frees */
static
int _bos_build(Xpost_Context *ctx,
               Xpost_Object o,
               int tag,
               unsigned char **out,
               unsigned int *outlen)
{
    Bos b;
    size_t recs = 0, data = 0, hdrlen, total;
    int fmt = _objfmt_get(ctx);
    int ret;

    if (fmt < 1 || fmt > 4)
        return undefined;
    if (tag < 0 || tag > 255)
        return rangecheck;
    ret = _bos_measure(ctx, o, 0, &recs, &data);
    if (ret)
        return ret;
    hdrlen = 4 + recs + data <= 65535 ? 4 : 8;
    total = hdrlen + recs + data;
    b.ctx = ctx;
    b.buf = calloc(total, 1);
    if (!b.buf)
        return VMerror;
    b.base = (unsigned int)hdrlen;
    b.nextrec = 8;
    b.nextdata = (unsigned int)recs;
    b.le = fmt == 2 || fmt == 4;
    b.buf[0] = (unsigned char)(127 + fmt);
    if (hdrlen == 4)
    {
        b.buf[1] = 1;
        _bos_put16(&b, b.buf + 2, (unsigned int)total);
    }
    else
    {
        b.buf[1] = 0;
        _bos_put16(&b, b.buf + 2, 1);
        _bos_put32(&b, b.buf + 4, (unsigned int)total);
    }
    ret = _bos_emit(&b, o, tag, 0);
    if (ret)
    {
        free(b.buf);
        return ret;
    }
    *out = b.buf;
    *outlen = total;
    return 0;
}

/* obj tag  printobject  -
   write obj's binary object sequence to the standard output */
static
int xpost_op_any_printobject(Xpost_Context *ctx,
                             Xpost_Object o,
                             Xpost_Object tag)
{
    unsigned char *buf;
    unsigned int len;
    int ret = _bos_build(ctx, o, tag.int_.val, &buf, &len);

    if (ret)
        return ret;
    if (ctx->stdout_fn)
    {
        if (ctx->stdout_fn(ctx->stdout_user, (char *)buf, len) != len)
            ret = ioerror;
    }
    else if (fwrite(buf, 1, len, stdout) != len)
        ret = ioerror;
    free(buf);
    return ret;
}

/* file obj tag  writeobject  -
   write obj's binary object sequence to file */
static
int xpost_op_any_writeobject(Xpost_Context *ctx,
                             Xpost_Object F,
                             Xpost_Object o,
                             Xpost_Object tag)
{
    Xpost_File *f;
    unsigned char *buf;
    unsigned int len;
    int ret;

    if (!xpost_file_get_status(ctx->lo, F))
        return ioerror;
    if (!xpost_object_is_writeable(ctx, F))
        return invalidaccess;
    ret = _bos_build(ctx, o, tag.int_.val, &buf, &len);
    if (ret)
        return ret;
    f = xpost_file_get_file_pointer(ctx->lo, F);
    {
        int d = _divert_output(ctx, f, (char *)buf, len);

        if (d < 0)
            ret = ioerror;
        else if (!d && xpost_file_write((char *)buf, 1, (integer)len, f) != (integer)len)
            ret = ioerror;
    }
    free(buf);
    return ret;
}

/* bool  echo  -
   enable/disable terminal echoing of input characters */
static
int xpost_op_bool_echo (Xpost_Context *ctx,
                        Xpost_Object b)
{
    (void)ctx;
    if (b.int_.val)
        echoon(stdin);
    else
        echooff(stdin);
    return 0;
}

/* string  .permitfileread  -
   permit reading files within the directory tree. A tree the permitted set
   already covers is permitted already and this does nothing; anything the
   set cannot be extended to hold -- because the sandbox is engaged, or the
   directory does not resolve, or there is no room -- raises
   invalidfileaccess, so a prolog that could not get the sandbox it asked
   for fails rather than proceeding as though it had. */
static
int xpost_op_string_permitfileread (Xpost_Context *ctx,
                                    Xpost_Object dir)
{
    char *d;
    int permitted;
    int ret = xpost_op_file_name(ctx, dir, &d);

    if (ret == VMerror)
        return VMerror;
    if (ret)
        /* a directory a name cannot express is one the permitted set
           cannot be extended to hold */
        return invalidfileaccess;
    permitted = xpost_path_permit_read(d);
    free(d);
    return permitted ? 0 : invalidfileaccess;
}

/* string  .permitfilewrite  -
   permit writing files within the directory tree, as .permitfileread */
static
int xpost_op_string_permitfilewrite (Xpost_Context *ctx,
                                     Xpost_Object dir)
{
    char *d;
    int permitted;
    int ret = xpost_op_file_name(ctx, dir, &d);

    if (ret == VMerror)
        return VMerror;
    if (ret)
        return invalidfileaccess;
    permitted = xpost_path_permit_write(d);
    free(d);
    return permitted ? 0 : invalidfileaccess;
}

/* Remove the sandbox-control and raw resource-open operators from systemdict
   so a program cannot name them after lockdown. .resourcefileopen stays bound
   (and executeonly) inside the resource machinery, so findresource is
   unaffected; the enforcement is the C-level permit check regardless. */
static void
_undef_sandbox_ops (Xpost_Context *ctx)
{
    static const char *const names[] = {
        ".permitfileread", ".permitfilewrite", ".lockdown", ".resourcefileopen"
    };
    Xpost_Object sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        (void)xpost_dict_undef(ctx, sd, xpost_name_cons(ctx, names[i]));
}

/* Engage the file-access sandbox and retire, from this context, the
   operators that configure it. Both halves belong together, and a caller
   that engages the latch alone leaves the control operators nameable: the
   enforcement is the C-level permit check either way, so what is lost is
   the second line rather than the first, but it is lost silently and the
   only sign of it is a program finding an operator that the design says
   is gone by then. */
void xpost_lockdown (Xpost_Context *ctx)
{
    xpost_path_control_engage();
    _undef_sandbox_ops(ctx);
}

/* -  .lockdown  -
   engage the file-access sandbox: subsequent program-driven opens are
   confined to the permitted directories. One-way -- a trusted prolog
   permits what it needs and locks down before running untrusted input. */
static
int xpost_op_lockdown (Xpost_Context *ctx)
{
    xpost_lockdown(ctx);
    return 0;
}

/* The string forms of filter: a private copy of the string becomes a readable
   file, and the file-source machinery runs over it. No program object names
   that file, so it is handed to the filter built over it, which closes and
   releases it along with itself. */
static
Xpost_Object _string_source(Xpost_Context *ctx, Xpost_Object S)
{
    Xpost_Object F;

    F = xpost_file_cons_readstring(ctx->lo,
            (const unsigned char *)xpost_string_get_pointer(ctx, S), S.comp_.sz);
    if (xpost_object_get_type(F) == filetype)
    {
        F.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        F.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_READ
                  << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
        xpost_file_hand_over(ctx->lo, F);
    }
    return F;
}

/* The procedure forms of filter: the procedure becomes a stream, which
   the filter machinery then runs over exactly as it runs over a file.
   Which kind of stream is the filter's to say, not the procedure's -- a
   procedure standing where an encode filter's data goes is a target and
   one standing where a decode filter's data comes from is a source --
   and the filter's name is what says so. As with the string forms, no
   program object names the stream, so it is handed to the filter built
   over it, which closes and releases it along with itself. */
static int _filter_name_encodes(Xpost_Context *ctx, Xpost_Object name)
{
    Xpost_Object namestr;
    char *cname;
    size_t len;
    int enc;

    namestr = xpost_name_get_string(ctx, name);
    cname = xpost_string_allocate_cstring(ctx, namestr);
    if (!cname)
        return 0;
    len = strlen(cname);
    enc = len > 6 && strcmp(cname + len - 6, "Encode") == 0;
    free(cname);
    return enc;
}

static
Xpost_Object _proc_stream(Xpost_Context *ctx, Xpost_Object P, Xpost_Object name)
{
    Xpost_Object F;
    int enc = _filter_name_encodes(ctx, name);

    F = enc ? xpost_file_cons_proctarget(ctx, P)
            : xpost_file_cons_procsource(ctx, P);
    if (xpost_object_get_type(F) == filetype)
    {
        F.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        F.tag |= ((enc ? XPOST_OBJECT_TAG_ACCESS_FILE_WRITE
                       : XPOST_OBJECT_TAG_ACCESS_FILE_READ)
                  << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
        xpost_file_hand_over(ctx->lo, F);
    }
    return F;
}

static
int xpost_op_proc_filter (Xpost_Context *ctx,
                          Xpost_Object P,
                          Xpost_Object name)
{
    Xpost_Object F = _proc_stream(ctx, P, name);
    if (xpost_object_get_type(F) != filetype) return VMerror;
    return xpost_op_file_filter(ctx, F, name);
}

static
int xpost_op_proc_filter_dict (Xpost_Context *ctx,
                               Xpost_Object P,
                               Xpost_Object dict,
                               Xpost_Object name)
{
    Xpost_Object F = _proc_stream(ctx, P, name);
    if (xpost_object_get_type(F) != filetype) return VMerror;
    return xpost_op_file_filter_dict(ctx, F, dict, name);
}

static
int xpost_op_proc_filter_int (Xpost_Context *ctx,
                              Xpost_Object P,
                              Xpost_Object rec,
                              Xpost_Object name)
{
    Xpost_Object F = _proc_stream(ctx, P, name);
    if (xpost_object_get_type(F) != filetype) return VMerror;
    return xpost_op_file_filter_int(ctx, F, rec, name);
}

static
int xpost_op_proc_filter_subfile (Xpost_Context *ctx,
                                  Xpost_Object P,
                                  Xpost_Object count,
                                  Xpost_Object eod,
                                  Xpost_Object name)
{
    Xpost_Object F = _proc_stream(ctx, P, name);
    if (xpost_object_get_type(F) != filetype) return VMerror;
    return xpost_op_file_filter_subfile(ctx, F, count, eod, name);
}

static
int xpost_op_string_filter (Xpost_Context *ctx,
                            Xpost_Object S,
                            Xpost_Object name)
{
    Xpost_Object F = _string_source(ctx, S);
    if (xpost_object_get_type(F) != filetype) return VMerror;
    return xpost_op_file_filter(ctx, F, name);
}

static
int xpost_op_string_filter_dict (Xpost_Context *ctx,
                                 Xpost_Object S,
                                 Xpost_Object dict,
                                 Xpost_Object name)
{
    Xpost_Object F = _string_source(ctx, S);
    if (xpost_object_get_type(F) != filetype) return VMerror;
    return xpost_op_file_filter_dict(ctx, F, dict, name);
}

static
int xpost_op_string_filter_subfile (Xpost_Context *ctx,
                                    Xpost_Object S,
                                    Xpost_Object count,
                                    Xpost_Object eod,
                                    Xpost_Object name)
{
    Xpost_Object F = _string_source(ctx, S);
    if (xpost_object_get_type(F) != filetype) return VMerror;
    return xpost_op_file_filter_subfile(ctx, F, count, eod, name);
}

/* dir category name  .resourcefileopen  file true | false
   Open the resource instance <dir>/<category>/<name> for reading, confined
   beneath dir, with category and name validated as single path components.
   Returns the file and true, or just false when the instance is absent,
   refused, or the names are not valid components. */
static
int xpost_op_resourcefileopen (Xpost_Context *ctx,
                               Xpost_Object dir,
                               Xpost_Object cat,
                               Xpost_Object nam)
{
    char *cdir;
    char *ccat;
    char *cnam;
    char rel[XPOST_PATH_MAX];
    Xpost_Object f;
    FILE *fp;
    int err;
    int n;
    int ret;

    /* validate against the raw bytes and length (rejects embedded NUL)
       before composing any path */
    if (!xpost_path_safe_leaf(xpost_string_get_pointer(ctx, cat), cat.comp_.sz) ||
        !xpost_path_safe_leaf(xpost_string_get_pointer(ctx, nam), nam.comp_.sz))
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }

    /* the directory is validated as a name in its own right: the two
       components above cannot express a path, and neither may the
       directory they are composed beneath express a shorter one */
    ret = xpost_op_file_name(ctx, dir, &cdir);
    if (ret == VMerror)
        return VMerror;
    if (ret)
    {
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }
    ccat = xpost_string_allocate_cstring(ctx, cat);
    cnam = xpost_string_allocate_cstring(ctx, nam);
    if (!ccat || !cnam)
    {
        free(cdir);
        free(ccat);
        free(cnam);
        return VMerror;
    }

    n = snprintf(rel, sizeof rel, "%s/%s", ccat, cnam);
    free(ccat);
    free(cnam);
    if (n < 0 || n >= (int)sizeof rel)
    {
        free(cdir);
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }

    fp = xpost_diskfile_fopen_beneath(cdir, rel, &err);
    free(cdir);
    if (!fp)
    {
        /* absent or refused: the caller tries the next directory */
        xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(0));
        return 0;
    }

    f = xpost_file_cons(ctx->lo, fp, 1);
    if (xpost_object_get_type(f) == invalidtype)
    {
        fclose(fp);
        return VMerror;
    }
    f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    f.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_READ << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    xpost_stack_push(ctx->lo, ctx->os, xpost_object_cvlit(f));
    xpost_stack_push(ctx->lo, ctx->os, xpost_bool_cons(1));
    return 0;
}

int xpost_oper_init_file_ops (Xpost_Context *ctx,
                              Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);


    op = xpost_operator_cons(ctx, "file", (Xpost_Op_Func)xpost_op_string_mode_file, 2, stringtype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".permitfileread", (Xpost_Op_Func)xpost_op_string_permitfileread, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".permitfilewrite", (Xpost_Op_Func)xpost_op_string_permitfilewrite, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".lockdown", (Xpost_Op_Func)xpost_op_lockdown, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, ".resourcefileopen", (Xpost_Op_Func)xpost_op_resourcefileopen, 3, stringtype, stringtype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_file_filter, 2, filetype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_file_filter_dict, 3,
            filetype, dicttype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_file_filter_subfile, 4,
            filetype, integertype, stringtype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_file_filter_int, 3,
            filetype, integertype, nametype);
    INSTALL;
    /* longest pattern first: the subfile form's EOD-string operand would
       otherwise let the two-operand form match a four-operand call */
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_string_filter_subfile, 4,
            stringtype, integertype, stringtype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_string_filter_dict, 3,
            stringtype, dicttype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_proc_filter_subfile, 4,
            proctype, integertype, stringtype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_proc_filter_dict, 3,
            proctype, dicttype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_proc_filter_int, 3,
            proctype, integertype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_proc_filter, 2,
            proctype, nametype);
    INSTALL;
    /* last of all: this pattern is the tail of the subfile forms of every
       other kind of source -- their EOD string and the filter name -- so
       a four-operand call would match it before reaching the form it was
       written as, whatever that form's source is */
    op = xpost_operator_cons(ctx, "filter", (Xpost_Op_Func)xpost_op_string_filter, 2,
            stringtype, nametype);
    INSTALL;
    op = xpost_operator_cons(ctx, ".eexecdecode", (Xpost_Op_Func)xpost_op_file_eexecdecode, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "closefile", (Xpost_Op_Func)xpost_op_file_closefile, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "read", (Xpost_Op_Func)xpost_op_file_read, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "write", (Xpost_Op_Func)xpost_op_file_write, 2, filetype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "readhexstring", (Xpost_Op_Func)xpost_op_file_readhexstring, 2, filetype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "writehexstring", (Xpost_Op_Func)xpost_op_file_writehexstring, 2, filetype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "readstring", (Xpost_Op_Func)xpost_op_file_readstring, 2, filetype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "writestring", (Xpost_Op_Func)xpost_op_file_writestring, 2, filetype, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "readline", (Xpost_Op_Func)xpost_op_file_readline, 2, filetype, stringtype);
    INSTALL;
    /* token: see optok.c */
    op = xpost_operator_cons(ctx, "bytesavailable", (Xpost_Op_Func)xpost_op_file_bytesavailable, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "flush", (Xpost_Op_Func)xpost_op_flush, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "flushfile", (Xpost_Op_Func)xpost_op_file_flushfile, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "resetfile", (Xpost_Op_Func)xpost_op_file_resetfile, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "status", (Xpost_Op_Func)xpost_op_file_status, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "status", (Xpost_Op_Func)xpost_op_string_status, 1, stringtype);
    INSTALL;
    /* string status */
    /* run: see init.ps */
    op = xpost_operator_cons(ctx, "currentfile", (Xpost_Op_Func)xpost_op_currentfile, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "deletefile", (Xpost_Op_Func)xpost_op_string_deletefile, 1, stringtype);
    INSTALL;
    op = xpost_operator_cons(ctx, "renamefile", (Xpost_Op_Func)xpost_op_string_renamefile, 2, stringtype, stringtype);
    INSTALL;
//#ifndef _WIN32
    op = xpost_operator_cons(ctx, "contfilenameforall", (Xpost_Op_Func)xpost_op_contfilenameforall, 4, globtype, integertype, proctype, stringtype);
    op = xpost_operator_cons(ctx, "filenameforall", (Xpost_Op_Func)xpost_op_filenameforall, 3, stringtype, proctype, stringtype);
    INSTALL;
//#endif
    op = xpost_operator_cons(ctx, "setfileposition", (Xpost_Op_Func)xpost_op_setfileposition, 2, filetype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "fileposition", (Xpost_Op_Func)xpost_op_fileposition, 1, filetype);
    INSTALL;
    op = xpost_operator_cons(ctx, "print", (Xpost_Op_Func)xpost_op_string_print, 1, stringtype);
    INSTALL;
    /* =: see init.ps
     * ==: see init.ps
     * stack: see init.ps
     * pstack: see init.ps */
    op = xpost_operator_cons(ctx, "printobject", (Xpost_Op_Func)xpost_op_any_printobject, 2, anytype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "writeobject", (Xpost_Op_Func)xpost_op_any_writeobject, 3, filetype, anytype, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "setobjectformat", (Xpost_Op_Func)xpost_op_int_setobjectformat, 1, integertype);
    INSTALL;
    op = xpost_operator_cons(ctx, "currentobjectformat", (Xpost_Op_Func)xpost_op_currentobjectformat, 0);
    INSTALL;
    op = xpost_operator_cons(ctx, "echo", (Xpost_Op_Func)xpost_op_bool_echo, 1, booleantype);
    INSTALL;

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
    xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "mark"), mark); */
    return 0;
}
