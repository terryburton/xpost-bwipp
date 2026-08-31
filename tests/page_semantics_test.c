/*
 * Embedding-contract test: the page operators observe the page
 * semantics the embedder chose.
 *
 * An embedder selects one of three behaviours at a page boundary
 * (Xpost_Showpage_Semantics): pause and announce the page on the
 * standard output, do neither, or return control to the caller. Both
 * showpage and copypage end a page -- copypage transmits it without
 * erasing it (PLRM 8.2) -- so both must take the behaviour that was
 * chosen. An operator that announces a page under the quiet semantics
 * writes into the program's own output stream, and one that reads the
 * line editor there consumes a line of the program's input.
 */

#include <stdio.h>
#include <string.h>
#include "xpost.h"

#include "xpost_test.h"

XPOST_TEST_SINK(out, 512)


/* run one program with the standard output captured; the captured text
   is left in out_buf, terminated */
static Xpost_Run_Status run_captured(Xpost_Context *ctx, const char *prog)
{
    Xpost_Run_Status st;

    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING, prog, 0);
    out_buf[out_len] = '\0';
    return st;
}

int main(void)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    /* --- the quiet semantics: neither operator announces a page --- */

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create nopause");
        return verdict();
    }
    xpost_job_snapshots_set(ctx, 0);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    st = run_captured(ctx, "(a) print showpage (b) print flush");
    check(st == XPOST_RUN_COMPLETE, "showpage completes under the quiet semantics");
    check(strcmp(out_buf, "ab") == 0,
          "showpage writes nothing of its own under the quiet semantics");

    st = run_captured(ctx, "(a) print copypage (b) print flush");
    check(st == XPOST_RUN_COMPLETE, "copypage completes under the quiet semantics");
    check(strcmp(out_buf, "ab") == 0,
          "copypage writes nothing of its own under the quiet semantics");

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);

    /* --- the returning semantics: both operators hand back control --- */

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_RETURN, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create return");
        return verdict();
    }
    xpost_job_snapshots_set(ctx, 0);

    st = xpost_run(ctx, XPOST_INPUT_STRING, "showpage /a 1 def", 0);
    check(st == XPOST_RUN_YIELDED, "showpage returns control to the caller");
    st = xpost_run(ctx, XPOST_INPUT_RESUME, "", 0);
    check(st == XPOST_RUN_COMPLETE, "the run resumes past showpage");

    st = xpost_run(ctx, XPOST_INPUT_STRING, "copypage /b 1 def", 0);
    check(st == XPOST_RUN_YIELDED, "copypage returns control to the caller");
    st = xpost_run(ctx, XPOST_INPUT_RESUME, "", 0);
    check(st == XPOST_RUN_COMPLETE, "the run resumes past copypage");

    st = xpost_run(ctx, XPOST_INPUT_STRING,
                   "a 1 eq b 1 eq and { (STATE-OK) print } if flush", 0);
    check(st == XPOST_RUN_COMPLETE, "the context survives both page boundaries");

    xpost_destroy(ctx);

    /* --- the page handlers under the returning semantics ---
       showpage hands control back in the middle of ending a page, so the
       count it advances and the BeginPage that opens the next one happen
       after the embedder resumes the run. An embedder that never resumed
       would leave the next page unopened, which is why the handlers are
       exercised here and not only through the interpreter's own suite. */

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_RETURN, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create return handlers");
        return verdict();
    }
    xpost_job_snapshots_set(ctx, 0);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    st = xpost_run(ctx, XPOST_INPUT_STRING,
                   "/bp -1 def /ep -1 def /er -1 def"
                   " << /OutputDevice /null"
                   "    /BeginPage { /bp exch store }"
                   "    /EndPage { /er exch store /ep exch store true }"
                   " >> setpagedevice", 0);
    check(st == XPOST_RUN_COMPLETE, "a device carrying handlers installs");

    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING, "bp 20 string cvs print flush", 0);
    out_buf[out_len] = '\0';
    check(st == XPOST_RUN_COMPLETE && strcmp(out_buf, "0") == 0,
          "setpagedevice opened the first page under the returning semantics");

    st = xpost_run(ctx, XPOST_INPUT_STRING, "showpage", 0);
    check(st == XPOST_RUN_YIELDED,
          "showpage still hands back control with handlers installed");

    st = xpost_run(ctx, XPOST_INPUT_RESUME, "", 0);
    check(st == XPOST_RUN_COMPLETE, "the run resumes past the handled showpage");

    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING,
                   "ep 20 string cvs print ( ) print er 20 string cvs print"
                   " ( ) print bp 20 string cvs print flush", 0);
    out_buf[out_len] = '\0';
    check(st == XPOST_RUN_COMPLETE && strcmp(out_buf, "0 0 1") == 0,
          "the page ended with reason 0 and the next opened, after the resume");

    /* a page the handler refuses is not transmitted, so the embedder is
       never handed one: the run completes instead of yielding */
    st = xpost_run(ctx, XPOST_INPUT_STRING,
                   "<< /EndPage { pop pop false } >> setpagedevice", 0);
    check(st == XPOST_RUN_COMPLETE, "the refusing handler installs");
    st = xpost_run(ctx, XPOST_INPUT_STRING, "showpage", 0);
    check(st == XPOST_RUN_COMPLETE,
          "a page EndPage refuses is not handed to the embedder");

    /* installing the refusing handler reactivated the device, so its count
       began again at zero; the refused page is the first counted against it */
    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING, "bp 20 string cvs print flush", 0);
    out_buf[out_len] = '\0';
    check(st == XPOST_RUN_COMPLETE && strcmp(out_buf, "1") == 0,
          "a page not transmitted is still counted and the next still opened");

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
    xpost_quit();

    return verdict();
}
