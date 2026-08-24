/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_xcb.h
 * @brief This file provides the Xpost xcb device functions.
 *
 * This header provides the Xpost xcb device functions.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_DEV_X_H
#define XPOST_DEV_X_H

/**
 * @brief install operator loadxcbdevice in systemdict
 *
 * When run, creates a new operator
 *
 *       width height  newxcbdevice  device
 *
 * which, when run, creates an X window and returns the device
 * instance dictionary.
 */
int xpost_oper_init_xcb_device_ops(Xpost_Context *ctx,
                                   Xpost_Object sd);

/**
 * @}
 */

#endif
