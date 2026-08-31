/*
 * Xpost DSC - a DSC PostScript parser
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dsc.h
 * @brief Declares the document-structuring-convention reader.
 *
 * The DSC comments a PostScript file carries about itself -- its pages, its
 * bounding box, what it needs -- read without executing the program.
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

/**
 * @brief The box a \%\%BoundingBox comment gives, in default user space.
 *
 * The corners a comment states, lower-left and upper-right, kept as the
 * integers the convention writes them as rather than converted: a reader
 * comparing what it found against what the file says is comparing the
 * same numbers.
 */
typedef struct
{
    int llx; /**< Lower-left x. */
    int lly; /**< Lower-left y. */
    int urx; /**< Upper-right x. */
    int ury; /**< Upper-right y. */
} Xpost_Dsc_Bounding_Box;

/**
 * @brief The strings one comment listed, and how many there are.
 *
 * Several of the conventions name a list -- the fonts a document uses,
 * the paper sizes it asks for -- and a list continued over \%\%+ lines is
 * gathered here as one. The strings and the array holding them belong to
 * the parse and go with xpost_dsc_free().
 */
typedef struct
{
    char **array; /**< The strings, in the order the file gave them. */
    int nbr;      /**< How many strings there are. */
} Xpost_Dsc_Str_Array;

/**
 * @brief Where a part of the document sits in the file it was read from.
 *
 * Offsets rather than pointers, and relative to the base address the
 * file was read at, so a caller that keeps the section past the mapping
 * still knows what it named. The end is one past the last byte.
 */
typedef struct
{
    ptrdiff_t start; /**< First byte, relative to the base address. */
    ptrdiff_t end;   /**< One past the last byte, likewise. */
} Xpost_Dsc_Section;

/**
 * @brief A font the document supplies, and where its program sits.
 *
 * A supplied font is one the file carries rather than expects the
 * printer to have, so the section is the program itself -- what a
 * consumer extracting or skipping fonts needs to find them by.
 */
typedef struct
{
    Xpost_Dsc_Section section; /**< Where the font's program sits. */
    char *fontname;            /**< The name the document calls it by. */
    char *printername;         /**< The name the printer knows, where the
                                    file gives one. */
} Xpost_Dsc_Font;

/**
 * @brief One page: where it sits, what it calls itself, what it needs.
 *
 * A consumer printing a range of pages, or one page of many, finds each
 * page's own bytes here without reading the pages before it -- which is
 * what the conventions exist to make possible.
 */
typedef struct
{
    Xpost_Dsc_Section section; /**< Where this page's bytes sit. */
    char *label;               /**< What the document calls the page,
                                    which need not be a number. */
    int ordinal;               /**< Its position in the document, or -1
                                    where the file wrote '?' -- which a
                                    level 1 document is allowed to. */
    Xpost_Dsc_Str_Array *fonts; /**< The fonts this page uses, where the
                                     document says per page. */
} Xpost_Dsc_Page;

/**
 * @brief What a parse found: the document's own account of itself.
 *
 * The conventions are comments, so nothing here is authority over what
 * the program does -- it is what the document says about itself, which
 * is what a consumer arranges its work by. What was absent is left as
 * the zero the parse started from, and the strings and arrays belong to
 * the parse: xpost_dsc_free() gives them back.
 *
 * The header is grouped by the level of the conventions that define each
 * member, since a document conforming to an earlier level says nothing
 * about the later ones.
 */
typedef struct
{
    unsigned char ps_vmaj; /**< Major version of the conventions the
                                document claims. */
    unsigned char ps_vmin; /**< Minor version of the same. */
    Xpost_Dsc_Job job;     /**< Whether the file is a document or an
                                encapsulated one. */
    unsigned char eps_vmaj; /**< Major version claimed for the
                                 encapsulated conventions, where the file
                                 is one. */
    unsigned char eps_vmin; /**< Minor version of the same. */

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

    Xpost_Dsc_Section prolog; /**< Where the prolog sits: what every page
                                   is to be run against. */

    Xpost_Dsc_Font *fonts; /**< The supplied fonts, as many as the header
                                said. */
    Xpost_Dsc_Page *pages; /**< The pages, as many as the header said. */
} Xpost_Dsc;

XPAPI Xpost_Dsc_Status xpost_dsc_parse(const Xpost_Dsc_File *file,
                                       Xpost_Dsc *dsc);

XPAPI void xpost_dsc_free(Xpost_Dsc *dsc);


#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */

#endif
