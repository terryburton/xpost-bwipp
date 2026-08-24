/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>

#include <xpost_dsc.h>

#define PRINT_STR_ARRAY(msg, memb) \
    do { \
        int i; \
        printf("%s : ", msg); \
        for (i = 0; i < dsc.header.memb.nbr; i++) \
            printf("%s ", dsc.header.memb.array[i]); \
        printf("\n"); \
    } while (0)

#define PRINT_INT_ARRAY(msg, memb) \
    do { \
        int i; \
        printf("%s : ", msg); \
        for (i = 0; i < dsc.header.memb.nbr; i++) \
            printf("%d ", dsc.header.memb.array[i]); \
        printf("\n"); \
    } while (0)

int main(int argc, char *argv[])
{
    Xpost_Dsc_File *file;
    Xpost_Dsc dsc;
    Xpost_Dsc_Status res;

    if (argc < 2)
    {
        printf("Usage: %s file.ps\n", argv[0]);
        return -1;
    }

    file = xpost_dsc_file_new_from_file(argv[1]);
    if (!file)
    {
        printf("can not create file from filename %s\n", argv[1]);
    }

    res = xpost_dsc_parse(file, &dsc);
    printf("result : %s\n", (res == XPOST_DSC_STATUS_SUCCESS)? "good" : "error");
    if (!res)
    {
        printf("exiting because of errors...");
        xpost_dsc_file_del(file);
        return -1;
    }

    printf("version : %d.%d", dsc.ps_vmaj, dsc.ps_vmin);
    switch (dsc.job)
    {
        case XPOST_DSC_JOB_EPS:
            printf(" EPS %d.%d\n", dsc.eps_vmaj, dsc.eps_vmin);
            break;
        case XPOST_DSC_JOB_QUERY:
            printf(" Query\n");
            break;
        case XPOST_DSC_JOB_EXIT_SERVER:
            printf(" ExitServer\n");
            break;
        case XPOST_DSC_JOB_RESOURCE_ENCODING:
            printf(" Resource (encoding)\n");
            break;
        case XPOST_DSC_JOB_RESOURCE_FILE:
            printf(" Resource (file)\n");
            break;
        case XPOST_DSC_JOB_RESOURCE_FONT:
            printf(" Resource (font)\n");
            break;
        case XPOST_DSC_JOB_RESOURCE_FORM:
            printf(" Resource (form)\n");
            break;
        case XPOST_DSC_JOB_RESOURCE_PATTERN:
            printf(" Resource (pattern)\n");
            break;
        case XPOST_DSC_JOB_RESOURCE_PROCSET:
            printf(" Resource (procset)\n");
            break;
        default:
            printf("\n");
            break;
    }
    PRINT_STR_ARRAY("document fonts", document_fonts);
    printf("title : %s\n", dsc.header.title ? dsc.header.title : "");
    printf("creator : %s\n", dsc.header.creator ? dsc.header.creator : "");
    printf("creation date : %s\n", dsc.header.creation_date ? dsc.header.creation_date : "");
    printf("for whom : %s\n", dsc.header.for_whom ? dsc.header.for_whom : "");
    printf("pages : %d\n", dsc.header.pages);
    printf("bounding box : %d %d %d %d\n",
           dsc.header.bounding_box.llx,
           dsc.header.bounding_box.lly,
           dsc.header.bounding_box.urx,
           dsc.header.bounding_box.ury);
    PRINT_STR_ARRAY("paper sizes", document_paper_sizes);
    printf("page order : ");
    switch (dsc.header.page_order)
    {
        case XPOST_DSC_PAGE_ORDER_ASCEND:
            printf("Ascend\n");
            break;
        case XPOST_DSC_PAGE_ORDER_DESCEND:
            printf("Descend\n");
            break;
        case XPOST_DSC_PAGE_ORDER_SPECIAL:
            printf("Special\n");
            break;
        default:
            printf("Unknown\n");
            break;
    }
    if (dsc.fonts)
    {
        int i;

        for (i = 0; i < dsc.header.document_fonts.nbr; i++)
        {
            printf("font #%d\n", i + 1);
            printf("  start: %td\n", dsc.fonts[i].section.start);
            printf("  end: %td\n", dsc.fonts[i].section.end);
            printf("  fontname: %s\n", dsc.fonts[i].fontname);
            printf("  printername: %s\n", dsc.fonts[i].printername ? dsc.fonts[i].printername : "");
        }
    }
    if (dsc.pages)
    {
        int i;

        for (i = 0; i < dsc.header.pages; i++)
        {
            printf("page #%d\n", i + 1);
            printf("  start: %td\n", dsc.pages[i].section.start);
            printf("  end: %td\n", dsc.pages[i].section.end);
            printf("  label: %s\n", dsc.pages[i].label);
            printf("  ordinal: %d\n", dsc.pages[i].ordinal);

        }
    }

    /* display prolog */
    /* { */
    /*     ptrdiff_t iter; */

    /*     /\* iter = dsc.prolog.start; *\/ */
    /*     printf("----- Begin Prolog -----\n"); */
    /*     for (iter = dsc.prolog.start; iter < dsc.prolog.end; iter++) */
    /*     { */
    /*         printf("%c", *(xpost_dsc_file_base_get(file) + iter)); */
    /*     } */
    /*     printf("\n"); */
    /*     printf("----- End Prolog -----\n"); */
    /* } */

    /* display font */
    /* { */
    /*     ptrdiff_t iter; */

    /*     /\* iter = dsc.prolog.start; *\/ */
    /*     printf("----- Begin Font 0 -----\n"); */
    /*     for (iter = dsc.fonts[0].section.start; iter < dsc.fonts[0].section.end; iter++) */
    /*     { */
    /*         printf("%c", *(xpost_dsc_file_base_get(file) + iter)); */
    /*     } */
    /*     printf("\n"); */
    /*     printf("----- End Font 0 -----\n"); */
    /* } */

    xpost_dsc_free(&dsc);
    xpost_dsc_file_del(file);

    return 0;
}
