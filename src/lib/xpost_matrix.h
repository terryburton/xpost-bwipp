/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_matrix.h
 * @brief matrix functions
 *
 * The six numbers and what they do to a coordinate are PLRM 4.3.3. The
 * operators that a program reaches them through, and the current
 * transformation the graphics state holds, are 4.3.2 and PLRM 8.2.
 *
 * @{
 */

#ifndef XPOST_MATRIX_H
#define XPOST_MATRIX_H

#ifndef XPOST_OBJECT_H
# error MUST #include "xpost_object.h" before this file
#endif

/*
  [ xx xy xz ]
  | yx yy yz |
  [ 0  0  1  ]
*/

/**
 * @typedef Xpost_Matrix
 */
typedef struct
{
    real xx;
    real xy;
    real xz;

    real yx;
    real yy;
    real yz;
} Xpost_Matrix;

/**
 * @brief Return the identity matrix.
 *
 * @param[out] m The matrix.
 *
 * This function fills the buffer @p m with the identity matrix.
 */
void xpost_matrix_identity(Xpost_Matrix *m);

/**
 * @brief Return the translation matrix.
 *
 * @param[out] m The matrix.
 * @param[in] tx The translation in x.
 * @param[in] ty The translation in y.
 *
 * This function fills the buffer @p m with the translation matrix
 * with the translation (@p tx, @p ty).
 */
void xpost_matrix_translate(Xpost_Matrix *m, real tx, real ty);

/**
 * @brief Return the scale matrix.
 *
 * @param[out] m The matrix.
 * @param[in] sx The scale in x.
 * @param[in] sy The scale in y.
 *
 * This function fills the buffer @p m with the scale matrix
 * with the scale (@p sx, @p sy).
 */
void xpost_matrix_scale(Xpost_Matrix *m, real sx, real sy);

/**
 * @brief Return the rotation matrix.
 *
 * @param[out] m The matrix.
 * @param[in] rad The angle in rad.
 *
 * This function fills the buffer @p m with the rotation matrix
 * with the angle @p rad (in radian).
 */
void xpost_matrix_rotate(Xpost_Matrix *m, real rad);

/**
 * @brief Return the rotation matrix.
 *
 * @param[in] m1 The first matrix.
 * @param[in] m2 The second matrix.
 * @param[out] m The multiplication matrix.
 *
 * This function fills the buffer @p m with the multiplication of
 * @p m1 and @p m2.
 */
void xpost_matrix_mult(const Xpost_Matrix *m1, const Xpost_Matrix *m2, Xpost_Matrix *m);

/**
 * @}
 */

#endif
