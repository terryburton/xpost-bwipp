/*
 * Xpost DSC - a DSC PostScript parser
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_DSC_CTX_H
#define XPOST_DSC_CTX_H


#include <stdlib.h> /* for size_t on some OS */

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
# undef WIN32_LEAN_AND_MEAN
#endif

typedef struct Xpost_Dsc_Ctx Xpost_Dsc_Ctx;

struct Xpost_Dsc_Ctx
{
    const unsigned char *base;
    const unsigned char *cur_loc;
    size_t length;
    /* level 1 */
    /* 0 unset, 1 set, 2 atend (unset) */
    unsigned int HEADER_DOCUMENT_FONTS : 2;
    unsigned int HEADER_TITLE : 1;
    unsigned int HEADER_CREATOR : 1;
    unsigned int HEADER_CREATION_DATE : 1;
    unsigned int HEADER_FOR : 1;
    unsigned int HEADER_PAGES : 2;
    unsigned int HEADER_BOUNDING_BOX : 2;
    unsigned int BODY_PAGE : 1;
    /* level 2 */
    unsigned int HEADER_DOCUMENT_PAPER_SIZES : 1;
    unsigned int HEADER_DOCUMENT_NEEDED_FONTS : 1;
    unsigned int HEADER_DOCUMENT_SUPPLIED_FONTS : 1;
    /* level 3 */
    unsigned int HEADER_PAGE_ORDER : 2;

    unsigned int eof : 1;
};


#endif
