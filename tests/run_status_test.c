/*
 * Embedding-contract test: xpost_run outcomes and error identity.
 *
 * A persistent context serves several programs in sequence; the test
 * checks that completion, showpage-yield, uncaught errors and post-error
 * reuse all report correctly, and that the error getters identify the
 * PostScript error by name.
 */

#include <stdio.h>
#include <string.h>
#include "xpost.h"

#include "xpost_test.h"

XPOST_TEST_SINK(out, 256)
XPOST_TEST_SINK(err, 256)



int main(void)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;

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

    /* completion */
    st = xpost_run(ctx, XPOST_INPUT_STRING, "/x 1 2 add def", 0);
    check(st == XPOST_RUN_COMPLETE, "simple program completes");
    check(xpost_error_name_get(ctx)[0] == '\0', "no error name after success");

    /* yield at showpage, then resume to completion */
    st = xpost_run(ctx, XPOST_INPUT_STRING, "showpage /y 2 def", 0);
    check(st == XPOST_RUN_YIELDED, "showpage yields");
    st = xpost_run(ctx, XPOST_INPUT_RESUME, "", 0);
    check(st == XPOST_RUN_COMPLETE, "resume runs to completion");

    /* uncaught error is reported and identified */
    st = xpost_run(ctx, XPOST_INPUT_STRING, "1 0 div", 0);
    check(st == XPOST_RUN_ERRORED, "uncaught error reports ERRORED");
    check(strcmp(xpost_error_name_get(ctx), "undefinedresult") == 0,
          "error name identifies the error");

    /* program-raised error with errorinfo, $error-and-stop style */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "$error /errorname /sometestfailure put "
        "$error /errorinfo (extra detail) put "
        "$error /newerror true put stop", 0);
    check(st == XPOST_RUN_ERRORED, "program-raised stop reports ERRORED");
    check(strcmp(xpost_error_name_get(ctx), "sometestfailure") == 0,
          "program-raised error name is reported");
    check(strcmp(xpost_error_info_get(ctx), "extra detail") == 0,
          "errorinfo detail is reported");

    /* an error caught by the program is not an errored run */
    st = xpost_run(ctx, XPOST_INPUT_STRING, "{ 1 0 div } stopped pop", 0);
    check(st == XPOST_RUN_COMPLETE, "caught error still completes");
    check(xpost_error_name_get(ctx)[0] == '\0',
          "no error name when the program caught it");

    /* text-output handlers capture print and %stderr writes */
    xpost_stdout_handler_set(ctx, out_sink, NULL);
    xpost_stderr_handler_set(ctx, err_sink, NULL);
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "(to-out) print (%stderr) (w) file (to-err) writestring", 0);
    check(st == XPOST_RUN_COMPLETE, "handler run completes");
    out_buf[out_len] = '\0';
    err_buf[err_len] = '\0';
    check(strcmp(out_buf, "to-out") == 0, "stdout handler received print");
    check(strcmp(err_buf, "to-err") == 0, "stderr handler received writestring");

    /* clearing the handlers restores the default routing */
    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_stderr_handler_set(ctx, NULL, NULL);
    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING, "(direct) print flush", 0);
    check(st == XPOST_RUN_COMPLETE, "post-handler run completes");
    check(out_len == 0, "cleared handler no longer receives output");

    /* the context stays healthy across all of the above */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "x 3 eq { (STATE-OK) print } if flush", 0);
    check(st == XPOST_RUN_COMPLETE, "context reusable after errors");

    /* Regression (PLRM 3.7.2): the local dictionaries systemdict holds --
       errordict, $error, statusdict, serverdict, FontDirectory -- are the
       sanctioned exception to the "no global->local reference" rule. A local
       garbage collection reaches them only through global systemdict, so it
       must mark them explicitly; otherwise repeated 1 vmreclaim on this
       persistent, snapshot-free context sweeps and recycles them, corrupting
       the error machinery so a later error is silently swallowed -- stopped
       reports false where it should report true. Drive several error-and-
       reclaim cycles, then confirm a fresh error inside a bound procedure is
       still caught. */
    out_len = 0;
    xpost_stdout_handler_set(ctx, out_sink, NULL);
    st = xpost_run(ctx, XPOST_INPUT_STRING,
        "3 { { zzq_undef_a } stopped pop 1 vmreclaim } repeat "
        "{ zzq_undef_b } stopped { (CAUGHT) }{ (MISSED) } ifelse print flush", 0);
    xpost_stdout_handler_set(ctx, NULL, NULL);
    out_buf[out_len < sizeof out_buf ? out_len : sizeof out_buf - 1] = '\0';
    check(st == XPOST_RUN_COMPLETE, "error-and-reclaim cycles complete");
    check(strcmp(out_buf, "CAUGHT") == 0,
          "an error still raises after repeated local GC (systemdict's local dicts survive)");

    xpost_destroy(ctx);
    xpost_quit();

    return verdict();
}
