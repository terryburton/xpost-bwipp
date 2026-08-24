/* In-stream job separation.
 *
 * A single input stream may carry more than one job. The end-of-transmission
 * control (^D, 0x04) separates them, as it does for a PostScript job server
 * (PLRM 3.7.7): reaching one ends the job being read and begins the next,
 * reading on from the same stream. The stream is one xpost_run call, so the
 * boundary between its jobs is taken in the middle of the run rather than at
 * its end -- the reader that holds the rest of the stream lives in C, outside
 * the virtual memory the boundary reverts, which is what lets the run go on.
 *
 * Three things are asserted, each the behaviour a server depends on:
 *   - what a job defines does not reach the job after it (isolation);
 *   - a job that calls exitserver folds its definitions into the baseline,
 *     so a later job in the same stream does see them (persistence);
 *   - a job that errors does not stop the stream: the job after it still runs
 *     (a poisoned job cannot take the server down).
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include "xpost.h"
#include "xpost_test.h"

static char outbuf[8192];
static size_t outlen;
static size_t out_sink(void *user, const char *buf, size_t len)
{
    (void)user;
    if (outlen + len < sizeof outbuf)
    {
        memcpy(outbuf + outlen, buf, len);
        outlen += len;
    }
    return len;
}

static void stream(Xpost_Showpage_Semantics semantics, const char *what)
{
    Xpost_Context *ctx;

    /* four ^D-separated jobs in one stream:
       1. defines /leaked in userdict (must NOT reach job 2)
       2. reports whether /leaked is known -- prints "clean" if isolated
       3. exitservers and defines /kept permanently
       4. reports whether /kept is known -- prints "kept" if it persisted
       and, between 2 and 3, a job that raises an error, to show the stream
       survives it: it is job 2 here that errors first, on purpose. */
    static const char prog[] =
        "/leaked 1 def\004"
        "userdict /leaked known {(LEAKED)}{(clean)} ifelse print flush\004"
        "nonexistent-name-raises-undefined\004"
        "serverdict begin () exitserver /kept 42 def\004"
        "userdict /kept known {(kept)}{(lost)} ifelse print flush";

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL, semantics,
                       XPOST_OUTPUT_MESSAGE_QUIET, XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("%s: xpost_create", what);
        return;
    }
    xpost_job_snapshots_set(ctx, 1);
    xpost_jobserver_set(ctx, 1);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    outlen = 0;
    (void) xpost_run(ctx, XPOST_INPUT_STRING, prog, sizeof prog - 1);
    outbuf[outlen < sizeof outbuf ? outlen : sizeof outbuf - 1] = '\0';

    if (strstr(outbuf, "LEAKED"))
        report_failure("%s: a definition crossed a ^D job boundary", what);
    if (!strstr(outbuf, "clean"))
        report_failure("%s: the job after the first did not run (stream stalled)", what);
    if (!strstr(outbuf, "kept"))
        report_failure("%s: exitserver state did not persist across ^D", what);

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* A job that compacts virtual memory (an immediate vmreclaim) must not
 * poison the allocator the next job in the stream inherits. Compaction
 * chains the rows it frees onto a derived free-row head; the job boundary
 * reverts the entity table beneath that head, so a boundary that did not
 * rebuild it would hand the next job an entity number outside the live
 * table -- a cross-job failure and a table write out of bounds. */
static void stream_reclaim(Xpost_Showpage_Semantics semantics, const char *what)
{
    Xpost_Context *ctx;
    static const char prog[] =
        "1 1 500 { pop 128 string pop } for 2 vmreclaim\004"
        "1 1 50 { pop 8 array pop } for (reclaim-ok)print flush";

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL, semantics,
                       XPOST_OUTPUT_MESSAGE_QUIET, XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("%s: xpost_create", what);
        return;
    }
    xpost_job_snapshots_set(ctx, 1);
    xpost_jobserver_set(ctx, 1);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    outlen = 0;
    (void) xpost_run(ctx, XPOST_INPUT_STRING, prog, sizeof prog - 1);
    outbuf[outlen < sizeof outbuf ? outlen : sizeof outbuf - 1] = '\0';

    if (!strstr(outbuf, "reclaim-ok"))
        report_failure("%s: a job compacting VM poisoned the next job's allocator",
                       what);

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* A file or filter a job opens and leaves open at the boundary must be
 * closed there. Its stream and, for a real file, its operating-system
 * descriptor live outside the arena the boundary reverts, so a boundary
 * that did not close it would leak both -- one per job that opened one --
 * until a long-running server ran out of descriptors. The leak, if it is
 * there, is a handle unreferenced after the run: the sanitizer build
 * reports it at exit, so this case needs no assertion of its own beyond
 * the next job still running. */
static void stream_file_leak(Xpost_Showpage_Semantics semantics, const char *what)
{
    Xpost_Context *ctx;
    static const char prog[] =
        "/f (0102) 0 dict /ASCIIHexDecode filter def\004"
        "(clean)print flush";

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL, semantics,
                       XPOST_OUTPUT_MESSAGE_QUIET, XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("%s: xpost_create", what);
        return;
    }
    xpost_job_snapshots_set(ctx, 1);
    xpost_jobserver_set(ctx, 1);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    outlen = 0;
    (void) xpost_run(ctx, XPOST_INPUT_STRING, prog, sizeof prog - 1);
    outbuf[outlen < sizeof outbuf ? outlen : sizeof outbuf - 1] = '\0';

    if (!strstr(outbuf, "clean"))
        report_failure("%s: the job after one that left a filter open did not run",
                       what);

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* A job that quits before reading its own Control-D leaves the delimiter
 * unread, with tokens still ahead of it. PLRM 3.7.7 step 4 has the server
 * flush the input to end-of-file at the boundary, so the delimiter is
 * consumed here and the next job begins at its own start rather than on this
 * job's tail. A server that did not flush would stall on the quit -- the next
 * job never running -- or read the tail as part of it. */
static void stream_quit(Xpost_Showpage_Semantics semantics, const char *what)
{
    Xpost_Context *ctx;
    static const char prog[] =
        "(first)print flush quit these tokens are past the quit\004"
        "(second)print flush";

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL, semantics,
                       XPOST_OUTPUT_MESSAGE_QUIET, XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("%s: xpost_create", what);
        return;
    }
    xpost_job_snapshots_set(ctx, 1);
    xpost_jobserver_set(ctx, 1);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    outlen = 0;
    (void) xpost_run(ctx, XPOST_INPUT_STRING, prog, sizeof prog - 1);
    outbuf[outlen < sizeof outbuf ? outlen : sizeof outbuf - 1] = '\0';

    if (!strstr(outbuf, "first"))
        report_failure("%s: the quitting job did not run", what);
    if (!strstr(outbuf, "second"))
        report_failure("%s: the stream did not resync past a job's quit", what);

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

int main(void)
{
    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    stream(XPOST_SHOWPAGE_NOPAUSE, "nopause");
    stream(XPOST_SHOWPAGE_RETURN, "returning");
    stream_reclaim(XPOST_SHOWPAGE_NOPAUSE, "reclaim-nopause");
    stream_reclaim(XPOST_SHOWPAGE_RETURN, "reclaim-returning");
    stream_file_leak(XPOST_SHOWPAGE_NOPAUSE, "fileleak-nopause");
    stream_file_leak(XPOST_SHOWPAGE_RETURN, "fileleak-returning");
    stream_quit(XPOST_SHOWPAGE_NOPAUSE, "quit-nopause");
    stream_quit(XPOST_SHOWPAGE_RETURN, "quit-returning");

    xpost_quit();
    return verdict();
}
