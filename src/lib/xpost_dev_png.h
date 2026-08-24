/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_png.h
 * @brief This file provides the PNG device operator functions.
 *
 * This header provides the PNG device operator functions.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_DEV_PNG_H
#define XPOST_DEV_PNG_H

#ifdef HAVE_LIBPNG

/**
 * @brief install operator loadpngdevice in systemdict
 *
 * When run, creates a new operator
 *
 *       width height  newpngdevice  device
 *
 * which, when run, creates and returns the device
 * instance dictionary.
 */
int xpost_oper_init_png_device_ops(Xpost_Context *ctx,
                                   Xpost_Object sd);

#endif

/**
 * @}
 */

#endif
