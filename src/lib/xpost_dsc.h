/*
 * Xpost DSC - a DSC PostScript parser
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_DSC_H
#define XPOST_DSC_H

#include <stddef.h> /* for ptrdiff_t */

#ifdef XPAPI
# undef XPAPI
#endif

#ifdef _WIN32
# ifdef XPOST_BUILD
#  ifdef DLL_EXPORT
#   define XPAPI __declspec(dllexport)
#  else
#   define XPAPI
#  endif
# else
#  define XPAPI __declspec(dllimport)
# endif
#else
# ifdef __GNUC__
#  if __GNUC__ >= 4
#   define XPAPI __attribute__ ((visibility("default")))
#  else
#   define XPAPI
#  endif
# else
#  define XPAPI
# endif
#endif

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */


/* File */

typedef struct Xpost_Dsc_File Xpost_Dsc_File;

XPAPI Xpost_Dsc_File *xpost_dsc_file_new_from_address(const unsigned char *base,
                                                      size_t length);

XPAPI Xpost_Dsc_File *xpost_dsc_file_new_from_file(const char *filename);

XPAPI const unsigned char *xpost_dsc_file_base_get(const Xpost_Dsc_File *file);

XPAPI size_t xpost_dsc_file_length_get(const Xpost_Dsc_File *file);

XPAPI void xpost_dsc_file_del(Xpost_Dsc_File *file);

/* DSC */

typedef enum
{
    XPOST_DSC_STATUS_ERROR,
    XPOST_DSC_STATUS_NO_DSC,
    XPOST_DSC_STATUS_SUCCESS
} Xpost_Dsc_Status;

typedef enum
{
    XPOST_DSC_JOB_NONE,
    XPOST_DSC_JOB_EPS,
    XPOST_DSC_JOB_QUERY,
    XPOST_DSC_JOB_EXIT_SERVER,
    XPOST_DSC_JOB_RESOURCE_ENCODING,
    XPOST_DSC_JOB_RESOURCE_FILE,
    XPOST_DSC_JOB_RESOURCE_FONT,
    XPOST_DSC_JOB_RESOURCE_FORM,
    XPOST_DSC_JOB_RESOURCE_PATTERN,
    XPOST_DSC_JOB_RESOURCE_PROCSET
} Xpost_Dsc_Job;

typedef enum
{
    XPOST_DSC_PAGE_ORDER_NONE,
    XPOST_DSC_PAGE_ORDER_ASCEND,
    XPOST_DSC_PAGE_ORDER_DESCEND,
    XPOST_DSC_PAGE_ORDER_SPECIAL
} Xpost_Dsc_Page_Order;

typedef struct
{
    int llx;
    int lly;
    int urx;
    int ury;
} Xpost_Dsc_Bounding_Box;

typedef struct
{
    char **array;
    int nbr;
} Xpost_Dsc_Str_Array;

typedef struct
{
    ptrdiff_t start; /* relative to base address */
    ptrdiff_t end; /* relative to base address */
} Xpost_Dsc_Section;

typedef struct
{
    Xpost_Dsc_Section section;
    char *fontname;
    char *printername;
} Xpost_Dsc_Font;

typedef struct
{
    Xpost_Dsc_Section section;
    char *label;
    int ordinal; /* -1 means '?' with DSC level 1 */
    Xpost_Dsc_Str_Array *fonts;
} Xpost_Dsc_Page;

typedef struct
{
    unsigned char ps_vmaj;
    unsigned char ps_vmin;
    Xpost_Dsc_Job job;
    unsigned char eps_vmaj;
    unsigned char eps_vmin;

    struct
    {
        /* level 1 */
        Xpost_Dsc_Str_Array document_fonts;
        char *title;
        char *creator;
        char *creation_date;
        char *for_whom;
        int pages;
        Xpost_Dsc_Bounding_Box bounding_box;
        /* level 2 */
        Xpost_Dsc_Str_Array document_paper_sizes;
        Xpost_Dsc_Str_Array document_needed_fonts;
        Xpost_Dsc_Str_Array document_supplied_fonts;
        /* level 3 */
        Xpost_Dsc_Page_Order page_order;
    } header;

    Xpost_Dsc_Section prolog;

    Xpost_Dsc_Font *fonts;
    Xpost_Dsc_Page *pages;
} Xpost_Dsc;

XPAPI Xpost_Dsc_Status xpost_dsc_parse(const Xpost_Dsc_File *file,
                                       Xpost_Dsc *dsc);

XPAPI void xpost_dsc_free(Xpost_Dsc *dsc);


#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */

#endif
