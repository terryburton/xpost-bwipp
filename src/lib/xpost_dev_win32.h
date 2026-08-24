/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_win32.h
 * @brief This file provides the Xpost win32 device functions.
 *
 * This header provides the Xpost win32 device functions.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_DEV_WIN32_H
#define XPOST_DEV_WIN32_H

/**
 * @brief install operator loadWin32device in systemdict
 *
 * When run, creates a new operator
 *
 *       width height  newwin32device  device
 *
 * which when run creates a window and returns the device
 * instance dictionary
 */
int xpost_oper_init_win32_device_ops(Xpost_Context *ctx,
                                     Xpost_Object sd);

/**
 * @}
 */

#endif
