/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Thorsten Behrens
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_error.c
 * @brief Raising an error, and the record a handler reads.
 *
 * PLRM 3.11.1 has $error hold what was executing when the error was raised,
 * which is how a program learns which of its calls went wrong. Raising one
 * unwinds to the nearest stopped context.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "xpost_object.h"
#include "xpost_error.h"

const char *errorname[] = { ERRORS(XPOST_OBJECT_AS_STR) };
