/* Implicit resource loading: findresource, on a miss, loads a named
   instance from the resource search path, confined and leaf-validated.
   Builds a temporary resource tree on disk and drives findresource. */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE /* mkdtemp */
#endif

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* the Windows CRT mkdir takes no mode argument */
#ifdef _WIN32
# define test_mkdir(p) mkdir(p)
#else
# define test_mkdir(p) mkdir((p), 0700)
#endif

#include "xpost.h"

#include "xpost_test.h"

XPOST_TEST_SINK(out, 256)


int main(void)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;
    char root[] = "xpost_res_XXXXXX";  /* relative: a native binary need not share /tmp */
    char dir[512];
    char file[600];
    FILE *w;

    if (!mkdtemp(root))
    {
        report_failure("mkdtemp");
        return verdict();
    }
    snprintf(dir, sizeof dir, "%s/TestCategory", root);
    if (test_mkdir(dir) != 0)
    {
        report_failure("mkdir");
        return verdict();
    }
    snprintf(file, sizeof file, "%s/TestCategory/testinstance", root);
    w = fopen(file, "wb");
    if (!w)
    {
        report_failure("write instance");
        return verdict();
    }
    fputs("/testinstance (RESOURCE-OK) /TestCategory defineresource pop\n", w);
    fclose(w);

    /* a second category whose implementation and instance both live on
       disk, exercising category-on-demand loading (the layout used by a
       resource-tree distribution: Category/<name> and <name>/<instance>) */
    snprintf(dir, sizeof dir, "%s/Category", root);
    test_mkdir(dir);
    snprintf(file, sizeof file, "%s/Category/DiskCat", root);
    w = fopen(file, "wb");
    if (w)
    {
        fputs("/DiskCat /Generic /Category findresource dup length 2 add dict copy\n"
              "  dup /Category /DiskCat put /Category defineresource pop\n", w);
        fclose(w);
    }
    snprintf(dir, sizeof dir, "%s/DiskCat", root);
    test_mkdir(dir);
    snprintf(file, sizeof file, "%s/DiskCat/diskinst", root);
    w = fopen(file, "wb");
    if (w)
    {
        fputs("/diskinst (DISK-CAT-OK) /DiskCat defineresource pop\n", w);
        fclose(w);
    }

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }
    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_RETURN, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        return verdict();
    }
    xpost_job_snapshots_set(ctx, 0);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    /* point the search path at the temporary tree via the C API */
    check(xpost_add_resource_dir(ctx, root) == 1, "add resource dir");

    /* Create the category, in global VM like the shared instance table. It
       is seeded from the Generic category's implementation dictionary
       (PLRM 3.9.3), which is where the five procedures a category answers
       through come from: all five are Required, and a category supplying
       none has nothing to answer with. */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "currentglobal true setglobal "
        "/TestCategory /Generic /Category findresource dup length 2 add dict copy "
        "dup /Category /TestCategory put /Category defineresource pop "
        "setglobal", 0);
    check(st == XPOST_RUN_COMPLETE, "setup completes");

    /* a miss loads the instance from disk, then resolves it */
    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "/testinstance /TestCategory findresource print flush", 0);
    check(st == XPOST_RUN_COMPLETE, "findresource of a disk instance completes");
    out_buf[out_len] = '\0';
    check(strcmp(out_buf, "RESOURCE-OK") == 0, "loaded instance resolves");

    /* a resolved instance is cached: a second lookup does not error */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "/testinstance /TestCategory findresource pop", 0);
    check(st == XPOST_RUN_COMPLETE, "second lookup is served from VM");

    /* systemdict is read-only after loading, so a program cannot undef an
       operator from it. Lockdown removes .resourcefileopen through the
       privileged C path (_undef_sandbox_ops), not through this operator. */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "systemdict /.resourcefileopen undef", 0);
    check(st == XPOST_RUN_ERRORED, "undef of sealed systemdict is rejected");
    check(strcmp(xpost_error_name_get(ctx), "invalidaccess") == 0,
          "the rejection is invalidaccess");

    /* an unknown category is itself loaded on demand, then its instance --
       from disk */
    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "/diskinst /DiskCat findresource print flush", 0);
    check(st == XPOST_RUN_COMPLETE, "category-on-demand load completes");
    out_buf[out_len] = '\0';
    check(strcmp(out_buf, "DISK-CAT-OK") == 0, "on-demand category resolves");

    /* a non-leaf key cannot name a file: refused, reported undefinedresource */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "(../../nonexistent) /TestCategory findresource", 0);
    check(st == XPOST_RUN_ERRORED, "traversal key errors");
    check(strcmp(xpost_error_name_get(ctx), "undefinedresource") == 0,
          "traversal key is undefinedresource");

    /* an absent instance is reported, not fabricated */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "/nope /TestCategory findresource", 0);
    check(st == XPOST_RUN_ERRORED, "absent instance errors");
    check(strcmp(xpost_error_name_get(ctx), "undefinedresource") == 0,
          "absent instance is undefinedresource");

    /* a category that is neither in VM nor on disk errors */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "/x /NoSuchCategory findresource", 0);
    check(st == XPOST_RUN_ERRORED, "unknown category errors");
    check(strcmp(xpost_error_name_get(ctx), "undefinedresource") == 0,
          "unknown category is undefinedresource");

    xpost_destroy(ctx);
    xpost_quit();

    snprintf(file, sizeof file, "%s/TestCategory/testinstance", root); unlink(file);
    snprintf(file, sizeof file, "%s/Category/DiskCat", root); unlink(file);
    snprintf(file, sizeof file, "%s/DiskCat/diskinst", root); unlink(file);
    snprintf(dir, sizeof dir, "%s/TestCategory", root); rmdir(dir);
    snprintf(dir, sizeof dir, "%s/Category", root); rmdir(dir);
    snprintf(dir, sizeof dir, "%s/DiskCat", root); rmdir(dir);
    rmdir(root);

    return verdict();
}
