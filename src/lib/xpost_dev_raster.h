/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_raster.h
 * @brief This file provides the Xpost raster output functions.
 *
 * This header provides the Xpost raster output functions.
 * The raster device is modelled after the BGR device,
 * but provides BGR BGRA ARGB or RGB buffers.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_DEV_RASTER_H
#define XPOST_DEV_RASTER_H

/**
 * @brief the pixel formats a "raster:FORMAT" selection may name
 *
 * A null-terminated roster, in the order a refusal lists them, and the
 * whole of what such a selection may carry. It is here so that the
 * check a selection is held to and the device that reads the name
 * afterwards work from one list.
 */
extern const char *const xpost_raster_formats[];

/**
 * @brief install operator loadrasterdevice in systemdict
 *
 * When run, creates a new operator
 *
 *       width height  newrasterdevice  device
 *
 * which, when run, creates and returns the device
 * instance dictionary.
 */
int xpost_oper_init_raster_device_ops(Xpost_Context *ctx,
                                      Xpost_Object sd);

/**
 * @}
 */

#endif
