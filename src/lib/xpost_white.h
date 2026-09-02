/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_WHITE_H
#define XPOST_WHITE_H

/**
 * @file xpost_white.h
 * @brief the language's white-space set, once.
 *
 * PLRM 3.2.2 Table 3.1 names six characters and no others: null, tab,
 * line feed, form feed, carriage return and space. Every place that
 * separates one syntactic construct from the next reads that set -- the
 * scanner, the numeral reader cvi and cvr use on a string, and the two
 * decoding filters whose entries (PLRM 3.13.3, ASCIIHexDecode and
 * ASCII85Decode) spell the same six out in words.
 *
 * The set is here rather than at each of them because a place that
 * disagrees with the others is worse than all of them being wrong
 * together: a null would end a token for one reader and sit inside it
 * for the next, and the same bytes would then be two different programs
 * depending on which read them.
 *
 * A vertical tab is not in the set, so it is a regular character and may
 * appear in a name (PLRM 3.2.2, "Names"). EOF is not a character and is
 * not white space; a caller reading a stream tests for it separately.
 */

static inline int
xpost_white_space(int c)
{
    switch (c)
    {
        case 0x00:  /* null */
        case 0x09:  /* tab */
        case 0x0a:  /* line feed */
        case 0x0c:  /* form feed */
        case 0x0d:  /* carriage return */
        case 0x20:  /* space */
            return 1;
    }
    return 0;
}

#endif
