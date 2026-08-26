/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_strbuf.c
 * @brief A growing byte buffer, outside virtual memory.
 *
 * For assembling text this process needs and the language does not: messages,
 * paths, the encoders' output.
 */

/** \file xpost_strbuf.c
   the growable byte buffer's one out-of-line member
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdarg.h>
#include <stdio.h>

#include "xpost_error.h"
#include "xpost_strbuf.h"

int
xpost_strbuf_appendf(Xpost_String_Buffer *b, const char *fmt, ...)
{
    va_list ap;
    int n, ret;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return VMerror;
    ret = xpost_strbuf_reserve(b, (size_t)n + 1);
    if (ret)
        return ret;
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}
