/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define _USE_MATH_DEFINES /* needed for M_PI with Visual Studio */
#include <math.h>

#include "xpost_object.h"
#include "xpost_matrix.h"

void xpost_matrix_identity(Xpost_Matrix *m)
{
    m->xx = 1.0;
    m->xy = 0.0;
    m->xz = 0.0;

    m->yx = 0.0;
    m->yy = 1.0;
    m->yz = 0.0;
}

void xpost_matrix_translate(Xpost_Matrix *m, real tx, real ty)
{
    m->xx = 1.0;
    m->xy = 0.0;
    m->xz = tx;

    m->yx = 0.0;
    m->yy = 1.0;
    m->yz = ty;
}

void xpost_matrix_scale(Xpost_Matrix *m, real sx, real sy)
{
    m->xx = sx;
    m->xy = 0.0;
    m->xz = 0.0;

    m->yx = 0.0;
    m->yy = sy;
    m->yz = 0.0;
}

void xpost_matrix_rotate(Xpost_Matrix *m, real rad)
{
    real c;
    real s;

    /* the rotation lands in every subsequent coordinate: it must be
       trigonometrically exact, not approximated */
    c = (real)cos(rad);
    s = (real)sin(rad);

    m->xx = c;
    m->xy = -s;
    m->xz = 0.0;

    m->yx = s;
    m->yy = c;
    m->yz = 0.0;
}

void xpost_matrix_mult(const Xpost_Matrix *m1, const Xpost_Matrix *m2, Xpost_Matrix *m)
{
    m->xx = m1->xx * m2->xx + m1->xy * m2->yx;
    m->xy = m1->xx * m2->xy + m1->xy * m2->yy;
    m->xz = m1->xx * m2->xz + m1->xy * m2->yz + m1->xz;

    m->yx = m1->yx * m2->xx + m1->yy * m2->yx;
    m->yy = m1->yx * m2->xy + m1->yy * m2->yy;
    m->yz = m1->yx * m2->xz + m1->yy * m2->yz + m1->yz;
}
