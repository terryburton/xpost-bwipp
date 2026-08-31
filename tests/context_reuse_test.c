/*
 * Embedding-contract test: a process may create, use and destroy a
 * context repeatedly.
 *
 * An embedder that serves one job per context creates and destroys
 * contexts for as long as the process lives. Each context owns its
 * operator table and its two memory files, so nothing an earlier
 * context installed or allocated may limit a later one, and a context
 * that has been destroyed must hold nothing: a job server that gained
 * a context's worth of memory per job would grow without bound.
 *
 * The memory claim is measured as the peak resident size, which only
 * ever rises -- so memory returned and reused registers as no growth,
 * while memory retained registers on every cycle. Where the platform
 * does not report it the cycles still run and their results are still
 * checked; only the growth comparison is left out, which the test says
 * so on its output.
 *
 * A peak is a high-water mark of the address space the process has
 * touched, not of the memory it holds, and an allocator that satisfies a
 * request from fresh pages rather than from the ones just returned moves
 * that mark without anything having been retained. Such an allocator
 * moves it by a bounded amount, spent over the first cycles and tailing
 * off; retention moves it by a context's worth on every cycle for as
 * long as the process runs. The two are told apart by reading the growth
 * in units of what one context costs, which is measured here rather than
 * assumed: the peak before any context has been created against the peak
 * once one has been created, used and destroyed. What the growth is read
 * over is the second half of the run, since that is the half in which an
 * allocator's own movement has largely been spent while retention would
 * still be costing a unit a cycle. The allowance is a few units for that
 * half, which a run keeping every context would exceed twice over.
 */

#include <stdio.h>
#include <string.h>
#include "xpost.h"

#ifndef _WIN32
# include <sys/time.h>
# include <sys/resource.h>
# include <unistd.h>          /* sysconf, for the page size /proc counts in */
# ifdef __APPLE__
#  include <mach/mach.h>      /* task_info, which says what is held now */
# endif
#endif

#include "xpost_test.h"

#define CYCLES 24

/* The growth is read over the second half of the run. An allocator that
   moves the mark without anything having been retained spends most of
   that movement on the first cycles, so the half that is read is the
   half where it has largely stopped, while retention costs the same in
   either half. */
#define MEASURED_FROM (CYCLES / 2 - 1)

/* the growth allowed over that half, in units of what one context costs:
   enough that an allocator's own drift fits inside it, and half of the
   CYCLES - 1 - MEASURED_FROM units that retaining every context would
   come to */
#define GROWTH_UNITS 6

/* What this process holds now, in KiB, or 0 where the platform does not say.

   Not the high-water mark. ru_maxrss never falls, so it answers "how much
   did this process ever have", which is the same as "how much does it hold"
   only where the allocator gives back the address space it freed. Where it
   does not, the mark climbs by about a context a cycle with nothing retained
   at all, and this check reads that as exactly the leak it exists to catch:
   MEASURED, a host reporting 104 MB of growth over twelve cycles against an
   allowance of 91 MB, where the same test on two hosts whose allocators do
   reuse reported 0 and under 3 MB. What is held now is the question, so it
   is what is asked. */
static long peak_resident_kib(void)
{
#if defined(_WIN32)
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS)
        return 0;
    return (long)(info.resident_size / 1024);
#else
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0;

    if (!f)
        return 0;
    if (fscanf(f, "%*s %ld", &pages) != 1)
        pages = 0;
    fclose(f);
    return (long)((pages * (long)sysconf(_SC_PAGESIZE)) / 1024);
#endif
}

static char out_buf[256];
static size_t out_len = 0;

static size_t out_sink(void *user, const char *buf, size_t len)
{
    (void)user;
    if (out_len + len < sizeof out_buf)
    {
        memcpy(out_buf + out_len, buf, len);
        out_len += len;
    }
    return len;
}

int main(void)
{
    long base;
    long one = 0;
    long settled = 0;
    long grown = 0;
    int i;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    /* the process with the library up and no context in it: what a
       context costs is read against this */
    base = peak_resident_kib();

    for (i = 0; i < CYCLES; i++)
    {
        Xpost_Context *ctx;
        Xpost_Run_Status st;

        ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                           XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                           XPOST_USE_SIZE, 100, 100);
        if (!ctx)
        {
            report_failure("context %d of %d was not created", i + 1, CYCLES);
            break;
        }
        xpost_job_snapshots_set(ctx, 0);
        xpost_stdout_handler_set(ctx, out_sink, NULL);

        /* an operator installed into this context's own table, reached
           by name through this context's own dictionaries */
        out_len = 0;
        st = xpost_run(ctx, XPOST_INPUT_STRING, "2 3 add (ok) print flush", 0);
        out_buf[out_len] = '\0';
        check(st == XPOST_RUN_COMPLETE, "each context runs a program");
        check(strcmp(out_buf, "ok") == 0, "each context reaches its operators");

        xpost_stdout_handler_set(ctx, NULL, NULL);
        xpost_destroy(ctx);

        /* the first cycle carries one context's worth onto the peak, so
           the reading after it is what a context costs */
        if (i == 0)
            one = peak_resident_kib();
        if (i == MEASURED_FROM)
            settled = peak_resident_kib();
        grown = peak_resident_kib();
    }

    if (base > 0 && one > base)
    {
        long unit = one - base;

        printf("one context costs %ld KiB; the last %d cycles of %d "
               "added %ld KiB\n", unit, CYCLES - 1 - MEASURED_FROM, CYCLES,
               grown - settled);
        /* The numbers go in the complaint, not only in the line above it.
           What a harness keeps of a failing run is the lines that say they
           are failures, so a measurement printed beside the verdict is a
           measurement a reader of the log does not get -- and this verdict
           cannot be acted on without it. Retention of a unit a cycle and
           an allowance that came out near nothing read the same here and
           want opposite fixes. */
        if (grown - settled >= unit * GROWTH_UNITS)
            report_failure("a run of contexts keeps no context's worth per"
                           " cycle: one context costs %ld KiB, the last %d"
                           " of %d cycles added %ld KiB, and the allowance"
                           " is %d units of a context (%ld KiB)",
                           unit, CYCLES - 1 - MEASURED_FROM, CYCLES,
                           grown - settled, GROWTH_UNITS,
                           (long)(unit * GROWTH_UNITS));
    }
    else
        printf("NOTE: peak resident size unavailable; growth not compared\n");

    xpost_quit();

    return verdict();
}
