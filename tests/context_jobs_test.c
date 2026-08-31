/*
 * Embedding contract: a context serves job after job, under either
 * snapshot setting.
 *
 * An embedder that keeps one context and feeds it programs is the case
 * neither snapshot setting was ever run against. Every test in this
 * suite that reuses a context turns snapshots off first, and every test
 * that leaves them on runs a single job, so the two settings have only
 * ever been exercised where they agree. What a second job sees is the
 * whole of the difference between them, and it is what nothing looked
 * at.
 *
 * The two settings promise different things and both promises are held
 * here. With snapshots on, the job boundary reverts the whole context to
 * a fixed baseline -- the state the first run (here a warm-up) left, the
 * loaded language and graphics -- so what one job wrote is not what the
 * next one starts from: every job sees a fresh namespace. With them off
 * there is no boundary, so a job starts from what the last one left: the
 * definitions carry over. An embedder picks one or the other, and each is
 * worth nothing unless the jobs after the first behave as it says.
 *
 * The baseline is established by the first run, so a warm-up runs before
 * the jobs measured here: it loads the language and graphics the jobs
 * revert to, and every job after it -- under every page semantics,
 * including the returning one -- reverts to it.
 *
 * Three things are required of both settings, because they are not
 * promises of either but of the context: every job runs to completion,
 * every job reaches the graphics operators, and the save stacks are
 * where they started once each job is done. The graphics check is the
 * one that catches the bracket swallowing the language load -- the
 * language loads once into a context and leaves state outside virtual
 * memory behind it, so a bracket taken over the load rewinds half of it
 * and the next job finds neither a loaded language nor a context able
 * to load one again. The stack check is the one that catches a bracket
 * taken and not given back.
 *
 * The jobs run under each of the three page semantics, which select
 * different paths out of a run, and once more under the returning
 * semantics with a page boundary in the job -- the shape where a job is
 * not one call but a first call that yields and a later one that
 * finishes it. The boundary reverts to the baseline when the job
 * completes, not when a call returns, so the returning shape is
 * isolated to the same degree as the others.
 *
 * Last, the caches of interned names the context keeps beside its
 * object roots. A name object is an index into a name stack that lives
 * in virtual memory, so a name a job interned is dropped by the revert
 * while a field caching it -- being no part of the arena -- keeps the
 * index; the next job reading that index back is answered whatever the
 * stack holds there now. The fields are poisoned here rather than
 * driven to it through a program, so the check has a state to detect
 * before the job runs rather than an absence to report.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_name.h"
#include "xpost_context.h"

#include "xpost_test.h"

#define JOBS 4

/* Says whether it has run in this namespace before, then leaves the
   mark that says so, then draws -- so one job's output reports both
   what it inherited and whether the graphics operators were there. */
static const char *const probe =
    "/xpjobseen where { pop (CARRIED) print }{ (FRESH) print } ifelse "
    "/xpjobseen true def "
    "0 0 moveto 5 5 lineto stroke (+drew) print flush";

/* The same, ending at a page boundary: under the returning semantics
   the job hands control back there and is finished by a later call. */
static const char *const probe_page =
    "/xpjobseen where { pop (CARRIED) print }{ (FRESH) print } ifelse "
    "/xpjobseen true def "
    "0 0 moveto 5 5 lineto stroke showpage (+drew) print flush";

XPOST_TEST_SINK(out, 512)


static int global_save_depth(Xpost_Context *ctx)
{
    return xpost_stack_count(ctx->gl, xpost_memory_save_stack_ent(ctx->gl));
}

static int local_save_depth(Xpost_Context *ctx)
{
    return xpost_stack_count(ctx->lo, xpost_memory_save_stack_ent(ctx->lo));
}

/* Run one job to its end. A job under the returning semantics is not one
   call: it yields at each page boundary and is resumed until it
   finishes. The guard is there so a job that yields without end fails
   the test rather than hanging it. */
static Xpost_Run_Status run_job(Xpost_Context *ctx, const char *prog)
{
    Xpost_Run_Status st;
    int resumes = 0;

    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING, prog, 0);
    while (st == XPOST_RUN_YIELDED && resumes++ < 8)
        st = xpost_run(ctx, XPOST_INPUT_RESUME, "", 0);
    out_buf[out_len] = '\0';
    return st;
}

static const char *setting_name(int snapshots)
{
    return snapshots ? "snapshots on" : "snapshots off";
}

static void jobs_in_one_context(const char *what,
                                Xpost_Showpage_Semantics semantics,
                                int snapshots,
                                const char *prog)
{
    Xpost_Context *ctx;
    int gdepth;
    int ldepth;
    int carried = 0;
    int i;

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       semantics, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("%s, %s: xpost_create", what, setting_name(snapshots));
        return;
    }
    xpost_job_snapshots_set(ctx, snapshots);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    /* the first run establishes the baseline the jobs revert to: warm the
       language and graphics here, so the measured jobs are all reverted to
       it rather than the first of them establishing it and escaping the
       measurement. It defines no name the probe looks for. */
    (void)run_job(ctx, "0 0 moveto 5 5 lineto stroke");

    gdepth = global_save_depth(ctx);
    ldepth = local_save_depth(ctx);

    for (i = 0; i < JOBS; i++)
    {
        Xpost_Run_Status st = run_job(ctx, prog);

        if (st != XPOST_RUN_COMPLETE)
            report_failure("%s, %s: job %d of %d did not complete (%d): %s",
                           what, setting_name(snapshots), i + 1, JOBS,
                           (int)st, out_buf);

        /* the graphics the first job loaded have to still be there for
           the ones after it: a job whose language was rewound out from
           under it never reaches its own program */
        if (!strstr(out_buf, "+drew"))
            report_failure("%s, %s: job %d of %d did not reach the graphics "
                           "operators: %s",
                           what, setting_name(snapshots), i + 1, JOBS, out_buf);

        if (strstr(out_buf, "CARRIED"))
            carried++;

        if (global_save_depth(ctx) != gdepth)
            report_failure("%s, %s: job %d left the global save stack at %d, "
                           "not %d",
                           what, setting_name(snapshots), i + 1,
                           global_save_depth(ctx), gdepth);
        if (local_save_depth(ctx) != ldepth)
            report_failure("%s, %s: job %d left the local save stack at %d, "
                           "not %d",
                           what, setting_name(snapshots), i + 1,
                           local_save_depth(ctx), ldepth);
    }

    /* What each setting is chosen for. With the boundary, no job inherits
       the one before it -- under every page semantics, the returning one
       included, since the boundary reverts to the baseline when the job
       completes rather than when a call returns. Without it, every job
       after the first inherits the last. Counted over the whole run rather
       than asserted per job, so the report says how far the setting held
       rather than only that it broke. */
    if (snapshots)
        check(carried == 0,
              "with snapshots on, no job inherits the namespace of the one "
              "before it");
    else
        check(carried == JOBS - 1,
              "with snapshots off, every job after the first inherits the "
              "namespace of the one before it");

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* The interned-name caches the context holds outside the arena, which the
   boundary must put back with the banks they index into: a name object
   carries an index into a name stack that lives in virtual memory, and a
   name a job interns is dropped by the revert while the field caching it
   is not. Poisoned after the baseline is captured, so what the job
   boundary has to undo is there to be seen; a poison equal to the
   baseline would make the check pass without the boundary doing
   anything, so that it differs is asserted first. */
static void interned_name_caches_revert(void)
{
    Xpost_Context *ctx;
    Xpost_Object base_wrapsave;
    Xpost_Object base_typename;
    Xpost_Object poison;
    const unsigned int slot = nulltype;

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_DEFAULT, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("interned name caches: xpost_create");
        return;
    }
    xpost_job_snapshots_set(ctx, 1);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    /* the first run establishes the baseline the caches are put back to */
    (void)run_job(ctx, "0 0 moveto 5 5 lineto stroke");

    base_wrapsave = ctx->namewrapsave;
    base_typename = ctx->typenames[slot];
    poison = xpost_object_cvx(xpost_name_cons(ctx, "-job-boundary-poison-"));

    if (xpost_object_get_type(poison) != nametype)
    {
        report_failure("interned name caches: the poison name would not intern");
    }
    else if (poison.mark_.padw == base_wrapsave.mark_.padw ||
             poison.mark_.padw == base_typename.mark_.padw)
    {
        report_failure("interned name caches: the poison is the baseline, so "
                       "the check could pass without the boundary running");
    }
    else
    {
        ctx->namewrapsave = poison;
        ctx->typenames[slot] = poison;

        (void)run_job(ctx, "1 1 add pop");

        check(ctx->namewrapsave.mark_.padw == base_wrapsave.mark_.padw &&
              xpost_object_get_type(ctx->namewrapsave)
                  == xpost_object_get_type(base_wrapsave),
              "the job boundary puts the wrapped-call name back to the "
              "baseline");
        check(ctx->typenames[slot].mark_.padw == base_typename.mark_.padw &&
              xpost_object_get_type(ctx->typenames[slot])
                  == xpost_object_get_type(base_typename),
              "the job boundary puts the type-name cache back to the "
              "baseline");
    }

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
        jobs_in_one_context("announcing semantics", XPOST_SHOWPAGE_DEFAULT,
                            snapshots, probe);
        jobs_in_one_context("quiet semantics", XPOST_SHOWPAGE_NOPAUSE,
                            snapshots, probe);
        jobs_in_one_context("quiet semantics, page boundary in the job",
                            XPOST_SHOWPAGE_NOPAUSE, snapshots, probe_page);
        jobs_in_one_context("returning semantics", XPOST_SHOWPAGE_RETURN,
                            snapshots, probe);
        jobs_in_one_context("returning semantics, page boundary in the job",
                            XPOST_SHOWPAGE_RETURN, snapshots, probe_page);
    }

    interned_name_caches_revert();

    xpost_quit();

    return verdict();
}
