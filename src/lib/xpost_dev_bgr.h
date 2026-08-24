/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_bgr.h
 * @brief This file provides the BGR device operator functions.
 *
 * This header provides the BGR device operator functions.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_DEV_BGR_H
#define XPOST_DEV_BGR_H

/**
 * @brief install operator loadbgrdevice in systemdict
 *
 * When run, creates a new operator
 *
 *       width height  newbgrdevice  device
 *
 * which, when run, creates and returns the device
 * instance dictionary.
 */
int xpost_oper_init_bgr_device_ops(Xpost_Context *ctx,
                                   Xpost_Object sd);

/**
 * @}
 */

#endif
