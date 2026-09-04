/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_BYTES_H
#define XPOST_BYTES_H

/**
 * @file xpost_bytes.h
 * @brief fixed-order byte readers.
 *
 * The binary-token scanner and the encoded-number-string decoder read
 * 16- and 32-bit values whose byte order the DATA declares, independent
 * of the host: these four readers are that assembly, once. (Native-order
 * reads use memcpy directly, as IEEE reals in binary tokens do.)
 */

static inline unsigned int
xpost_bytes_be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16)
         | ((unsigned int)p[2] << 8)  |  (unsigned int)p[3];
}

static inline unsigned int
xpost_bytes_le32(const unsigned char *p)
{
    return ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[1] << 8)  |  (unsigned int)p[0];
}

static inline unsigned short
xpost_bytes_be16(const unsigned char *p)
{
    return (unsigned short)((p[0] << 8) | p[1]);
}

static inline unsigned short
xpost_bytes_le16(const unsigned char *p)
{
    return (unsigned short)((p[1] << 8) | p[0]);
}

#endif
