/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_jpeg.h
 * @brief This file provides the JPEG device operator functions.
 *
 * This header provides the JPEG device operator functions.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_DEV_JPEG_H
#define XPOST_DEV_JPEG_H

/**
 * @brief install operator loadjpegdevice in systemdict
 *
 * When run, creates a new operator
 *
 *       width height  newjpegdevice  device
 *
 * which, when run, creates and returns the device
 * instance dictionary.
 */
int xpost_oper_init_jpeg_device_ops(Xpost_Context *ctx,
                                    Xpost_Object sd);

/**
 * @}
 */

#endif
