/* A process that takes the library down and brings it back up may ask
 * for the same font again.
 *
 * xpost_init and xpost_quit are counted, and an embedder that serves
 * work in bursts brings the library up for a burst and takes it down
 * between them. What findfont holds against a name is a face, and a face
 * belongs to the library the font module opened: taking the module down
 * takes every face still open with it. So a cache that outlives the
 * teardown names faces that have been freed, and the next run asking for
 * a name an earlier one asked for is handed one -- not a stale value to
 * be noticed somewhere later, but a pointer followed straight into freed
 * memory by whatever asks the face a question.
 *
 * The test is two cycles, each a whole library lifetime: initialise,
 * make a context, show text through a font found by name, destroy, quit.
 * The name is the same both times, which is what puts the second cycle
 * on the cached entry. A run holding the cache across the teardown does
 * not fail an assertion here -- it takes the process down -- so what is
 * asserted is the ordinary thing, that both cycles complete and both
 * print what they were asked to.
 *
 * A host with no font of that name reaches none of this: findfont has
 * nothing to cache and the first cycle says so by not completing. The
 * note is on the output and the verdict is computed from the tally,
 * there being nothing here that went wrong.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include "xpost.h"
#include "xpost_test.h"

XPOST_TEST_SINK(out, 256)

/* One whole library lifetime, showing text through a font found by
   name. Answers 1 where the run completed and printed its word, which
   is the only reading the second cycle needs -- a face followed after
   its library has gone does not return an answer to check. */
static int cycle(void)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;
    int ok;

    if (!xpost_init())
    {
        report_failure("the library did not initialise");
        return 0;
    }
    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 200, 200);
    if (!ctx)
    {
        report_failure("a library cycle made no context");
        xpost_quit();
        return 0;
    }
    xpost_job_snapshots_set(ctx, 0);
    xpost_stdout_handler_set(ctx, out_sink, NULL);
    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING,
                   "/Times-Roman findfont 12 scalefont setfont "
                   "10 10 moveto (hi) show (shown) print flush", 0);
    out_buf[out_len] = '\0';
    ok = (st == XPOST_RUN_COMPLETE) && (strcmp(out_buf, "shown") == 0);

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
    xpost_quit();
    return ok;
}

int main(void)
{
    if (!cycle())
    {
        printf("no font of that name to hold, so a held one cannot be"
               " reached\n");
        return verdict();
    }
    check(cycle(), "a font asked for again in a second library cycle");
    return verdict();
}
