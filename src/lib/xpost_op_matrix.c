/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_op_matrix.c
 * @brief Installs the matrix operators.
 *
 * The implementations, and the one function that installs them.
 *
 * Installed into systemdict as:
 *
 * matrix initmatrix identmatrix defaultmatrix currentmatrix setmatrix
 * translate scale rotate concat concatmatrix transform dtransform
 * itransform idtransform invertmatrix
 *
 * A matrix is a six-element array a program may hold and alter, and the
 * current transformation is one of these kept in the graphics state.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define _USE_MATH_DEFINES /* needed for M_PI with Visual Studio */
#include <assert.h>
#include <math.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_matrix.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_string.h"
#include "xpost_dict.h"
#include "xpost_array.h"
#include "xpost_save.h"

//#include "xpost_interpreter.h"
#include "xpost_operator.h"
#include "xpost_op_matrix.h"

//#define RAD_PER_DEG (M_PI / 180.0)
/* full precision: a truncated literal skewed rotate off the axis angles */
#define RAD_PER_DEG (M_PI / 180.0)

/* The names the coordinate operators reach the current transformation
   through. Every one of them walks from the private dictionary to the
   graphics state to the matrix, and a name built from characters walks
   the name tree to get there -- twice for a name in the global bank,
   since the local one is searched first and misses. These operators run
   once per coordinate a program converts, so the names are resolved
   where the operators are installed and not where they are used. */
static Xpost_Object namedotgraphicsdict;
static Xpost_Object namecurrgstate;
static Xpost_Object namecurrmatrix;
static Xpost_Object namedevice;
static Xpost_Object namedefaultmatrix;

static
Xpost_Object _get_ctm(Xpost_Context *ctx)
{
    Xpost_Object gd;
    Xpost_Object gs;
    Xpost_Object psctm;

    gd = xpost_dict_get(ctx, ctx->privatedict, namedotgraphicsdict);
    gs = xpost_dict_get(ctx, gd, namecurrgstate);
    psctm = xpost_dict_get(ctx, gs, namecurrmatrix);

    return psctm;
}

static
int _psmat2xmat(Xpost_Context *ctx,
                 Xpost_Object psm,
                 Xpost_Matrix *m)
{
    Xpost_Object arr[6];
    int i;
    /* the matrix operand must be a six-element array; a shorter one would make
       the bulk read below run off the end of its storage (PLRM: rangecheck) */
    if (xpost_object_get_type(psm) != arraytype || psm.comp_.sz != 6)
        return rangecheck;
    /* every operator that reads a caller's matrix reads it through here,
       and the read bypasses xpost_array_get's own check: the six objects
       come out in one xpost_memory_get. A value an access withholds is not
       one an operator may take (PLRM 3.3.2); read-only still reads. */
    if (!xpost_object_is_readable(ctx, psm))
        return invalidaccess;
    if (!xpost_memory_get(xpost_context_select_memory(ctx, psm),
                          xpost_object_get_ent(psm), 0, sizeof arr, arr))
        return rangecheck;
    for (i = 0; i < 6; i++)
    {
        if (xpost_object_get_type(arr[i]) == integertype)
            arr[i] = xpost_real_cons((real)arr[i].int_.val);
    }
    m->xx = arr[0].real_.val;
    m->yx = arr[1].real_.val;
    m->xy = arr[2].real_.val;
    m->yy = arr[3].real_.val;
    m->xz = arr[4].real_.val;
    m->yz = arr[5].real_.val;
    return 0;

    /*
    Xpost_Object el;
    el = xpost_array_get(ctx, psm, 0);
    m->xx = xpost_object_get_type(el) == integertype ?  el.int_.val : el.real_.val;
    el = xpost_array_get(ctx, psm, 1);
    m->yx = xpost_object_get_type(el) == integertype ?  el.int_.val : el.real_.val;
    el = xpost_array_get(ctx, psm, 2);
    m->xy = xpost_object_get_type(el) == integertype ?  el.int_.val : el.real_.val;
    el = xpost_array_get(ctx, psm, 3);
    m->yy = xpost_object_get_type(el) == integertype ?  el.int_.val : el.real_.val;
    el = xpost_array_get(ctx, psm, 4);
    m->xz = xpost_object_get_type(el) == integertype ?  el.int_.val : el.real_.val;
    el = xpost_array_get(ctx, psm, 5);
    m->yz = xpost_object_get_type(el) == integertype ?  el.int_.val : el.real_.val;
    */
}

static
int _xmat2psmat(Xpost_Context *ctx,
                Xpost_Matrix *m,
                Xpost_Object psm)
{
    Xpost_Object arr[6];
    Xpost_Memory_File *mem;
    unsigned int ent;
    int ret;

    /* the bulk write below stores six objects at once; a destination shorter
       than six would overrun its storage in the arena (PLRM: rangecheck) */
    if (xpost_object_get_type(psm) != arraytype || psm.comp_.sz != 6)
        return rangecheck;
    /* every operator that fills a caller's matrix stores through here,
       and the store bypasses xpost_array_put's own check: the six
       objects go in one xpost_memory_put. Interpreter start-up builds
       matrices before any program runs. */
    if (!ctx->gl->interpreter_get_initializing())
        if (!xpost_object_is_writeable(ctx, psm))
            return invalidaccess;
    mem = xpost_context_select_memory(ctx, psm);
    ent = xpost_object_get_ent(psm);

    arr[0] = xpost_real_cons(m->xx);
    arr[1] = xpost_real_cons(m->yx);
    arr[2] = xpost_real_cons(m->xy);
    arr[3] = xpost_real_cons(m->yy);
    arr[4] = xpost_real_cons(m->xz);
    arr[5] = xpost_real_cons(m->yz);
    /* Back the array up for save/restore before the bulk write, the way
       xpost_array_put does per element. Without this a matrix modified inside a
       save -- the CTM, under scale/concat/rotate -- is not reverted by the
       matching restore, so a transform set inside a save leaks past it.
       The backup is an allocation the memory file can refuse, and a write
       that goes in over a refused backup is one restore has no record of:
       the refusal is the operator's answer, not something to write past. */
    ret = xpost_save_cow(mem, arraytype, psm.comp_.sz, ent);
    if (ret)
        return ret;
    if (!xpost_memory_put(mem, ent, 0, sizeof arr, arr))
        return rangecheck;
    return 0;
}

/* forward decl's */
static int _ident_matrix(Xpost_Context *ctx, Xpost_Object psmat);
static int _default_matrix(Xpost_Context *ctx, Xpost_Object psmat);
static int _set_matrix(Xpost_Context *ctx, Xpost_Object psmat);

/* -  matrix  matrix
   create identity matrix */
static
int _matrix(Xpost_Context *ctx)
{
    Xpost_Object psmat;
    psmat = xpost_object_cvlit(xpost_array_cons(ctx, 6));
    return _ident_matrix(ctx, psmat);
}

/* matrix  identmatrix  matrix
   fill matrix with identity transform */
static
int _ident_matrix(Xpost_Context *ctx,
                  Xpost_Object psmat)
{
    Xpost_Matrix mat;
    xpost_matrix_identity(&mat);
    { int ret = _xmat2psmat(ctx, &mat, psmat); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    return 0;
}

/* -  initmatrix  -
   set ctm to device default */
static
int _init_matrix(Xpost_Context *ctx)
{
    /* schedule the operators themselves: a name would resolve through the
       dict stack and could be captured by a user definition */
    xpost_stack_push(ctx->lo, ctx->es,
                     XPOST_OP(ctx, setmatrix));
    xpost_stack_push(ctx->lo, ctx->es,
                     XPOST_OP(ctx, defaultmatrix));
    xpost_stack_push(ctx->lo, ctx->es,
                     XPOST_OP(ctx, matrix));
    /*
    _matrix(ctx);
    _default_matrix(ctx, xpost_stack_pop(ctx->lo, ctx->os));
    _set_matrix(ctx, xpost_stack_pop(ctx->lo, ctx->os));
            */
    return 0;
}

/* matrix  defaultmatrix  matrix
   fill matrix with device default matrix */
static
int _default_matrix(Xpost_Context *ctx,
                    Xpost_Object psmat)
{
    Xpost_Object gd;
    Xpost_Object gs;
    Xpost_Object devdic;
    Xpost_Object defmat;

    gd = xpost_dict_get(ctx, ctx->privatedict, namedotgraphicsdict);
    if (xpost_object_get_type(gd) == invalidtype)
        return undefined;
    XPOST_LOG_INFO("loaded graphicsdict");

    gs = xpost_dict_get(ctx, gd, namecurrgstate);
    if (xpost_object_get_type(gs) == invalidtype)
        return undefined;
    XPOST_LOG_INFO("loaded gstate");

    devdic = xpost_dict_get(ctx, gs, namedevice);
    if (xpost_object_get_type(devdic) == invalidtype)
        return undefined;
    XPOST_LOG_INFO("loaded device");

    defmat = xpost_dict_get(ctx, devdic, namedefaultmatrix);
    if (xpost_object_get_type(defmat) == invalidtype)
        return undefined;
    XPOST_LOG_INFO("loaded defaultmatrix");

    xpost_stack_push(ctx->lo, ctx->os, defmat);
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    /* schedule the operator itself: a name would resolve through the
       dict stack and could be captured by a user definition */
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, copy));
    return 0;
}

/* matrix  currentmatrix  matrix
   fill matrix with ctm */
static
int _current_matrix(Xpost_Context *ctx,
                    Xpost_Object psmat)
{
    Xpost_Object ctm;
    ctm = _get_ctm(ctx);
    xpost_stack_push(ctx->lo, ctx->os, ctm);
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    /* schedule the operator itself: a name would resolve through the
       dict stack and could be captured by a user definition */
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, copy));
    return 0;
}

/* matrix  setmatrix  -
   replace ctm by matrix */
static
int _set_matrix(Xpost_Context *ctx,
                Xpost_Object psmat)
{
    Xpost_Object ctm;

    /* a transformation matrix is six numbers */
    if (psmat.comp_.sz != 6)
        return rangecheck;

    ctm = _get_ctm(ctx);
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    xpost_stack_push(ctx->lo, ctx->os, ctm);
    /* schedule the operator itself: a name would resolve through the
       dict stack and could be captured by a user definition */
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, oppop));
    /* schedule the operator itself: a name would resolve through the
       dict stack and could be captured by a user definition */
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, copy));
    return 0;
}

/* tx ty  translate
   translate user space by (tx, ty) */
static
int _translate(Xpost_Context *ctx,
               Xpost_Object xt,
               Xpost_Object yt)
{
    Xpost_Matrix mat;
    Xpost_Object psmat;
    psmat = xpost_object_cvlit(xpost_array_cons(ctx, 6));
    xpost_matrix_translate(&mat, xt.real_.val, yt.real_.val);
    { int ret = _xmat2psmat(ctx, &mat, psmat); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    /* schedule the operator itself: a name would resolve through the
       dict stack and could be captured by a user definition */
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, concat));
    return 0;
}

/* tx ty matrix  translate  matrix
   define translation by (tx, ty) */
static
int _mat_translate(Xpost_Context *ctx,
                   Xpost_Object xt,
                   Xpost_Object yt,
                   Xpost_Object psmat)
{
    Xpost_Matrix mat;
    xpost_matrix_translate(&mat, xt.real_.val, yt.real_.val);
    { int ret = _xmat2psmat(ctx, &mat, psmat); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    return 0;
}

/* sx sy  scale  -
   scale user space by sx and sy */
static
int _scale(Xpost_Context *ctx,
           Xpost_Object xs,
           Xpost_Object ys)
{
    Xpost_Matrix mat;
    Xpost_Object psmat;
    psmat = xpost_object_cvlit(xpost_array_cons(ctx, 6));
    xpost_matrix_scale(&mat, xs.real_.val, ys.real_.val);
    { int ret = _xmat2psmat(ctx, &mat, psmat); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    /* schedule the operator itself: a name would resolve through the
       dict stack and could be captured by a user definition */
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, concat));
    return 0;
}

/* sx sy matrix  scale  -
   define scaling by sx and sy */
static
int _mat_scale(Xpost_Context *ctx,
               Xpost_Object xs,
               Xpost_Object ys,
               Xpost_Object psmat)
{
    Xpost_Matrix mat;
    xpost_matrix_scale(&mat, xs.real_.val, ys.real_.val);
    { int ret = _xmat2psmat(ctx, &mat, psmat); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    return 0;
}

/* angle  rotate  -
   rotate user space by angle degrees */
static
int _rotate(Xpost_Context *ctx,
            Xpost_Object angle)
{
    Xpost_Matrix mat;
    Xpost_Object psmat;
    psmat = xpost_object_cvlit(xpost_array_cons(ctx, 6));
    xpost_matrix_rotate(&mat, angle.real_.val * RAD_PER_DEG);
    { int ret = _xmat2psmat(ctx, &mat, psmat); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    /* schedule the operator itself: a name would resolve through the
       dict stack and could be captured by a user definition */
    xpost_stack_push(ctx->lo, ctx->es, XPOST_OP(ctx, concat));
    return 0;
}

/* angle matrix  rotate  matrix
   define rotation by angle degrees */
static
int _mat_rotate(Xpost_Context *ctx,
                Xpost_Object angle,
                Xpost_Object psmat)
{
    Xpost_Matrix mat;
    xpost_matrix_rotate(&mat, (real)(RAD_PER_DEG * angle.real_.val));
    { int ret = _xmat2psmat(ctx, &mat, psmat); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat);
    return 0;
}

/* matrix  concat  -
   replace CTM by matrix X CTM */
static
int _concat(Xpost_Context *ctx,
            Xpost_Object psmat)
{
    Xpost_Matrix mat;
    Xpost_Object psctm;
    Xpost_Matrix ctm;
    Xpost_Matrix result;

    { int ret = _psmat2xmat(ctx, psmat, &mat); if (ret) return ret; }
    //fetch CTM from graphics state
    psctm = _get_ctm(ctx);
    //xpost_matrix_mult
    { int ret = _psmat2xmat(ctx, psctm, &ctm); if (ret) return ret; }
    xpost_matrix_mult(&ctm, &mat, &result);
    //replace CTM
    { int ret = _xmat2psmat(ctx, &result, psctm); if (ret) return ret; }
    return 0;
}

/* matrix1 matrix2 matrix3  concatmatrix  matrix3
   fill matrix3 with matrix1 X matrix2 */
static
int _concat_matrix(Xpost_Context *ctx,
                   Xpost_Object psmat1,
                   Xpost_Object psmat2,
                   Xpost_Object psmat3)
{
    Xpost_Matrix mat1, mat2, mat3;
    { int ret = _psmat2xmat(ctx, psmat1, &mat1); if (ret) return ret; }
    { int ret = _psmat2xmat(ctx, psmat2, &mat2); if (ret) return ret; }
    xpost_matrix_mult(&mat2, &mat1, &mat3);
    { int ret = _xmat2psmat(ctx, &mat3, psmat3); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat3);
    return 0;
}

/* x y matrix  transform  x' y'
   transform (x,y) by matrix */
static
int _mat_transform(Xpost_Context *ctx,
                   Xpost_Object x,
                   Xpost_Object y,
                   Xpost_Object psmat)
{
    Xpost_Matrix mat;
    real xres, yres;
    { int ret = _psmat2xmat(ctx, psmat, &mat); if (ret) return ret; }
    xres = mat.xx * x.real_.val + mat.xy * y.real_.val + mat.xz;
    yres = mat.yx * x.real_.val + mat.yy * y.real_.val + mat.yz;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(xres));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(yres));
    return 0;
}

/* x y  transform  x' y'
   transform (x,y) by CTM */
static
int _transform(Xpost_Context *ctx,
               Xpost_Object x,
               Xpost_Object y)
{
    Xpost_Object psctm;
    psctm = _get_ctm(ctx);
    return _mat_transform(ctx, x, y, psctm);
}

/* dx dy matrix  dtransform  dx' dy'
   transform distance (dx,dy) by matrix */
static
int _mat_dtransform(Xpost_Context *ctx,
                    Xpost_Object x,
                    Xpost_Object y,
                    Xpost_Object psmat)
{
    Xpost_Matrix mat;
    real xres, yres;
    { int ret = _psmat2xmat(ctx, psmat, &mat); if (ret) return ret; }
    xres = mat.xx * x.real_.val + mat.xy * y.real_.val;
    yres = mat.yx * x.real_.val + mat.yy * y.real_.val;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(xres));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(yres));
    return 0;
}

/* dx dy  dtransform  dx' dy'
   transform (dx,dy) by CTM */
static
int _dtransform(Xpost_Context *ctx,
                Xpost_Object x,
                Xpost_Object y)
{
    Xpost_Object psctm;
    psctm = _get_ctm(ctx);
    return _mat_dtransform(ctx, x, y, psctm);
}

/* x' y' matrix  itransform  x y
   perform inverse transformation of (x',y') by matrix */
static
int _mat_itransform(Xpost_Context *ctx,
                    Xpost_Object x,
                    Xpost_Object y,
                    Xpost_Object psmat)
{
    Xpost_Matrix mat;
    real xres, yres;
    real disc;
    real invdet;
    { int ret = _psmat2xmat(ctx, psmat, &mat); if (ret) return ret; }
    disc = mat.xx * mat.yy - mat.yx * mat.xy;
    if (disc == 0)
        return undefinedresult;
    invdet = 1 / disc;
    xres = (mat.yy * x.real_.val - mat.xy * y.real_.val +
            mat.xy * mat.yz - mat.yy * mat.xz) * invdet;
    yres = (-mat.yx * x.real_.val + mat.xx * y.real_.val +
            mat.yx * mat.xz - mat.xx * mat.yz) * invdet;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(xres));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(yres));
    return 0;
}

/* x' y'  itransform  x y
   perform inverse transformation of (x',y') by CTM */
static
int _itransform(Xpost_Context *ctx,
                Xpost_Object x,
                Xpost_Object y)
{
    Xpost_Object psctm;
    psctm = _get_ctm(ctx);
    return _mat_itransform(ctx, x, y, psctm);
}

/* dx' dy' matrix  idtransform  dx dy
   perform inverse transform of distance
   (dx',dy') by matrix */
static
int _mat_idtransform(Xpost_Context *ctx,
                     Xpost_Object x,
                     Xpost_Object y,
                     Xpost_Object psmat)
{
    Xpost_Matrix mat;
    real xres, yres;
    real disc;
    real invdet;
    { int ret = _psmat2xmat(ctx, psmat, &mat); if (ret) return ret; }
    disc = mat.xx * mat.yy - mat.yx * mat.xy;
    if (disc == 0)
        return undefinedresult;
    invdet = 1 / disc;
    xres = (mat.yy * x.real_.val - mat.xy * y.real_.val) * invdet;
    yres = ( -mat.yx * x.real_.val + mat.xx * y.real_.val) * invdet;
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(xres));
    xpost_stack_push(ctx->lo, ctx->os, xpost_real_cons(yres));
    return 0;
}

/* dx' dy'  idtransform  dx dy
   perform inverse transformation of distance
   (dx',dy') by CTM */
static
int _idtransform(Xpost_Context *ctx,
                 Xpost_Object x,
                 Xpost_Object y)
{
    Xpost_Object psctm;
    psctm = _get_ctm(ctx);
    return _mat_idtransform(ctx, x, y, psctm);
}

/* matrix1 matrix2  invertmatrix  matrix2
   fill matrix2 with inverse of matrix1 */
static
int _invert_matrix(Xpost_Context *ctx,
                   Xpost_Object psmat1,
                   Xpost_Object psmat2)
{
    Xpost_Matrix mat1, mat2;
    real disc;
    real invdet;
    { int ret = _psmat2xmat(ctx, psmat1, &mat1); if (ret) return ret; }
    disc = mat1.xx * mat1.yy - mat1.yx * mat1.xy;
    if (disc == 0)
        return undefinedresult;
    invdet = 1 / disc;
    mat2.xx = mat1.yy * invdet;
    mat2.yx = -mat1.yx * invdet;
    mat2.xy = -mat1.xy * invdet;
    mat2.yy = mat1.xx * invdet;
    mat2.xz = (mat1.xy * mat1.yz - mat1.yy * mat1.xz) * invdet;
    mat2.yz = (mat1.yx * mat1.xz - mat1.xx * mat1.yz) * invdet;
    { int ret = _xmat2psmat(ctx, &mat2, psmat2); if (ret) return ret; }
    xpost_stack_push(ctx->lo, ctx->os, psmat2);
    return 0;
}


int xpost_oper_init_matrix_ops(Xpost_Context *ctx,
                               Xpost_Object sd)
{
    Xpost_Operator *optab;
    Xpost_Object n,op;

    assert(ctx->gl->base);

    if (xpost_object_get_type((namedotgraphicsdict =
            xpost_name_cons(ctx, ".graphicsdict"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namecurrgstate =
            xpost_name_cons(ctx, "currgstate"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namecurrmatrix =
            xpost_name_cons(ctx, "currmatrix"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedevice =
            xpost_name_cons(ctx, "device"))) == invalidtype)
        return VMerror;
    if (xpost_object_get_type((namedefaultmatrix =
            xpost_name_cons(ctx, "defaultmatrix"))) == invalidtype)
        return VMerror;

    op = xpost_operator_cons(ctx, "matrix", (Xpost_Op_Func)_matrix, 0);
    INSTALL;

    op = xpost_operator_cons(ctx, "initmatrix", (Xpost_Op_Func)_init_matrix, 0);
    INSTALL;

    op = xpost_operator_cons(ctx, "identmatrix", (Xpost_Op_Func)_ident_matrix, 1,
                             arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "defaultmatrix", (Xpost_Op_Func)_default_matrix, 1,
                             arraytype);
    INSTALL;

    op = xpost_operator_cons(ctx, "currentmatrix", (Xpost_Op_Func)_current_matrix, 1,
                             arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "setmatrix", (Xpost_Op_Func)_set_matrix, 1,
                             arraytype);
    INSTALL;

    op = xpost_operator_cons(ctx, "translate", (Xpost_Op_Func)_translate, 2,
                             floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "translate", (Xpost_Op_Func)_mat_translate, 3,
                             floattype, floattype, arraytype);
    INSTALL;

    op = xpost_operator_cons(ctx, "scale", (Xpost_Op_Func)_scale, 2,
            floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "scale", (Xpost_Op_Func)_mat_scale, 3,
                             floattype, floattype, arraytype);
    INSTALL;

    op = xpost_operator_cons(ctx, "rotate", (Xpost_Op_Func)_rotate, 1,
                             floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "rotate", (Xpost_Op_Func)_mat_rotate, 2,
                             floattype, arraytype);
    INSTALL;

    op = xpost_operator_cons(ctx, "concat", (Xpost_Op_Func)_concat, 1,
            arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "concatmatrix", (Xpost_Op_Func)_concat_matrix, 3,
                             arraytype, arraytype, arraytype);
    INSTALL;

    op = xpost_operator_cons(ctx, "transform", (Xpost_Op_Func)_transform, 2,
                             floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "transform", (Xpost_Op_Func)_mat_transform, 3,
                             floattype, floattype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "dtransform", (Xpost_Op_Func)_dtransform, 2,
                             floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "dtransform", (Xpost_Op_Func)_mat_dtransform, 3,
                             floattype, floattype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "itransform", (Xpost_Op_Func)_itransform, 2,
                             floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "itransform", (Xpost_Op_Func)_mat_itransform, 3,
                             floattype, floattype, arraytype);
    INSTALL;
    op = xpost_operator_cons(ctx, "idtransform", (Xpost_Op_Func)_idtransform, 2,
                             floattype, floattype);
    INSTALL;
    op = xpost_operator_cons(ctx, "idtransform", (Xpost_Op_Func)_mat_idtransform, 3,
                             floattype, floattype, arraytype);
    INSTALL;

    op = xpost_operator_cons(ctx, "invertmatrix", (Xpost_Op_Func)_invert_matrix, 2,
                             arraytype, arraytype);
    INSTALL;

    /* xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
    xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "mark"), mark); */

    return 0;
}
