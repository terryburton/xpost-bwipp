/* The device a context is started with: when it is made, and what a
 * graphics state without one answers.
 *
 * The language a context loads is the same for every run of the build.
 * The device is not: it is the one the caller asked for, at the page the
 * caller asked for, and it is made once in the life of the context.
 * Once, and not once per job -- a job is bracketed by a save level taken
 * before it and rewound after it, so a device made inside that bracket
 * is unmade with it, and the block of state it reached through a handle
 * is outside virtual memory where neither the rewind nor the collector
 * reaches. A context that rebuilt its device per job would work, print
 * the pages asked of it, and grow by one device's worth of memory every
 * time.
 *
 * So the device is identified from inside each job and required to be
 * the same one. The raster device is what is asked, because its instance
 * state is the state that lives outside virtual memory: it reaches it
 * through a handle this interpreter issued, and a second instance is
 * issued a second handle. A device whose state is virtual memory could
 * be rebuilt and rebuilt and nothing outside the memory file would show
 * it.
 *
 * Both snapshot settings are run, because the bracket is what the
 * setting selects and the bracket is what a device made in the wrong
 * place would be caught by.
 *
 * And the state itself: a graphics state that carries no device. The
 * language stands on its own -- it is loaded and locked down before the
 * run's device is made -- so this is a state the interpreter can be in,
 * and what a painting operator does there has to be an answer rather
 * than a crash. It is reached here the only way a program can reach it,
 * by taking the device out of the graphics state, and what is required
 * is that the operator raises a name the program can catch and that the
 * interpreter goes on afterwards.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include "xpost.h"

#include "xpost_test.h"

#define JOBS 3

/* Names the device serving this job. The raster device reaches its
   instance state through a handle, and the handle is what tells one
   instance from another; it is printed a byte at a time because it is
   carried in a string. */
static const char *const probe =
    "(handle=) print "
    "DEVICE /Private known "
    "{ DEVICE /Private get { 4 string cvs print (.) print } forall } "
    "{ (none) print } ifelse "
    "0 0 moveto 5 5 lineto stroke ( drew\n) print flush";

/* Takes the device out of the graphics state, paints, and reports what
   painting answered; then puts it back, so that a job after this one
   finds the context as it was. */
static const char *const probe_nodevice =
    "/xpdev graphicsdict /currgstate get /device get def "
    "graphicsdict /currgstate get /device undef "
    "mark { 0 0 moveto 5 5 lineto stroke } stopped "
    "{ (painted=) print $error /errorname get =print } "
    "{ (painted=nofault) print } ifelse cleartomark "
    "graphicsdict /currgstate get /device xpdev put "
    "( andback\n) print flush";

XPOST_TEST_SINK(out, 512)


static Xpost_Run_Status run_job(Xpost_Context *ctx, const char *prog)
{
    Xpost_Run_Status st;

    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING, prog, 0);
    out_buf[out_len] = '\0';
    return st;
}

static const char *setting_name(int snapshots)
{
    return snapshots ? "snapshots on" : "snapshots off";
}

/* the text between "handle=" and the space that ends it, copied out so
   that one job's answer can be held beside the next one's */
static int handle_of(const char *out, char *buf, size_t sz)
{
    const char *p = strstr(out, "handle=");
    size_t n = 0;

    if (!p)
        return 0;
    p += sizeof "handle=" - 1;
    while (*p && *p != ' ' && *p != '\n' && n + 1 < sz)
        buf[n++] = *p++;
    buf[n] = '\0';
    return n > 0;
}

static void one_device_per_context(int snapshots)
{
    Xpost_Context *ctx;
    char first[64];
    char this_one[64];
    int i;

    ctx = xpost_create("raster", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 32, 32);
    if (!ctx)
    {
        report_failure("%s: xpost_create", setting_name(snapshots));
        return;
    }
    xpost_job_snapshots_set(ctx, snapshots);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    first[0] = '\0';
    for (i = 0; i < JOBS; i++)
    {
        if (run_job(ctx, probe) != XPOST_RUN_COMPLETE)
        {
            report_failure("%s: job %d of %d did not complete: %s",
                           setting_name(snapshots), i + 1, JOBS, out_buf);
            break;
        }
        if (!strstr(out_buf, "drew"))
        {
            report_failure("%s: job %d of %d did not reach the graphics "
                           "operators: %s", setting_name(snapshots),
                           i + 1, JOBS, out_buf);
            break;
        }
        if (!handle_of(out_buf, this_one, sizeof this_one))
        {
            report_failure("%s: job %d of %d named no device: %s",
                           setting_name(snapshots), i + 1, JOBS, out_buf);
            break;
        }
        /* a device that names itself with nothing would report every job
           as served by the same one */
        if (strcmp(this_one, "none") == 0)
        {
            report_failure("%s: the device exposes no handle to tell one "
                           "instance from another, so nothing here holds",
                           setting_name(snapshots));
            break;
        }
        if (i == 0)
            strcpy(first, this_one);
        else if (strcmp(first, this_one) != 0)
            report_failure("%s: job %d of %d was served by device %s, and "
                           "the first job by device %s: the context built a "
                           "second device rather than serving the job with "
                           "the one it was started with",
                           setting_name(snapshots), i + 1, JOBS,
                           this_one, first);
    }
    check(first[0] != '\0',
          "a context names the device serving its jobs, so that the jobs "
          "after the first can be held to being served by the same one");

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

static void a_graphics_state_without_a_device(int snapshots)
{
    Xpost_Context *ctx;

    ctx = xpost_create("raster", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 32, 32);
    if (!ctx)
    {
        report_failure("%s: xpost_create for the deviceless state",
                       setting_name(snapshots));
        return;
    }
    xpost_job_snapshots_set(ctx, snapshots);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    if (run_job(ctx, probe_nodevice) != XPOST_RUN_COMPLETE)
        report_failure("%s: the run that painted without a device did not "
                       "complete: %s", setting_name(snapshots), out_buf);
    /* PLRM 8.2 gives undefined for a name that has no value, which is
       what the graphics state answers with when nothing has been
       installed under the one the painting machinery asks for. */
    else if (!strstr(out_buf, "painted=undefined"))
        report_failure("%s: painting with no device in the graphics state "
                       "must raise undefined, and answered: %s",
                       setting_name(snapshots), out_buf);
    else
        check(strstr(out_buf, "andback") != NULL,
              "a program that paints with no device in the graphics state "
              "is told undefined and goes on running");

    /* and the context goes on serving jobs afterwards */
    if (run_job(ctx, probe) != XPOST_RUN_COMPLETE || !strstr(out_buf, "drew"))
        report_failure("%s: the job after the deviceless one did not "
                       "paint: %s", setting_name(snapshots), out_buf);

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

int main(void)
{
    int snapshots;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    for (snapshots = 0; snapshots < 2; snapshots++)
    {
        one_device_per_context(snapshots);
        a_graphics_state_without_a_device(snapshots);
    }

    xpost_quit();

    return verdict();
}
