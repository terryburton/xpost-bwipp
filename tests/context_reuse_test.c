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
 * The memory claim is measured as what the process holds, read once a
 * cycle -- so memory returned and reused registers as no growth, while
 * memory retained registers on every cycle. Where the platform does not
 * report it the cycles still run and their results are still checked;
 * only the growth comparison is left out, which the test says so on its
 * output.
 *
 * Two things move that reading and only one of them is retention. The
 * other is the C allocator, which keeps what it has freed for as long as
 * it likes -- so before each reading it is asked to hand back what it is
 * holding and not using. What that leaves is what the process needs, and
 * a leak is still there afterwards because memory still allocated is not
 * the allocator's to give back. Without that step the reading is the
 * host's to decide: MEASURED, the same interpreter over the same cycles
 * reads no growth at all on one host, occasional regions on a second,
 * and a level step every cycle on a third -- the very shape retention
 * makes. Asked to release first, the second host's total falls from
 * about 7800 KiB to a few hundred.
 *
 * What is then read is not how far the reading rose but how often:
 * retention costs a context's worth on every cycle, where anything else
 * is spent early and stops. So the figure is the middle one of the
 * per-cycle steps over the second half of the run. A total cannot say
 * which happened -- the same total is three steps of 3800 or a unit a
 * cycle, and those want opposite fixes -- and a total is also read
 * against the size of a page, where pages four times larger cost four
 * times as much for the same steps while a context does not.
 *
 * The step allowed is half of what one context costs, which is measured
 * here rather than assumed: the reading before any context has been
 * created against the reading once one has been created, used and
 * destroyed. A run keeping every context reads a whole unit a cycle and
 * a run keeping half of one reads the allowance exactly.
 *
 * A run that exceeds it prints what it read on every cycle, since the
 * shape says where to look: steps level to the end are a cost paid per
 * context, and steps that shrink are something spending itself out.
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
#  include <malloc/malloc.h>  /* malloc_zone_pressure_relief */
# endif
# ifdef __GLIBC__
#  include <malloc.h>         /* malloc_trim */
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

/* The step allowed, as a fraction of what one context costs: a cycle
   that keeps half a context, held to through the measured half, is the
   most that reads as no retention. Retaining every context reads twice
   this. */
#define STEP_NUMERATOR   1
#define STEP_DENOMINATOR 2

/* Hand back what the C allocator is holding but not using, so that what is
   read next is what the process needs rather than what it has cached.

   Without this the reading is the allocator's to decide. It keeps freed
   pages at its own discretion, and how much it keeps is a property of the
   machine: MEASURED, one host gives every cycle's memory straight back and
   reads no growth at all, a second keeps it in occasional regions, and a
   third keeps every cycle's worth and reads a level step a cycle -- the
   very shape retention makes. Nothing about the interpreter differs across
   the three. Asked to release first, all three answer about the
   interpreter.

   Where a host offers no such call the reading is what it was, and the
   check is left reading the allocator on that host as it always did. */
static void release_allocator_caches(void)
{
#if defined(__APPLE__)
    malloc_zone_pressure_relief(NULL, 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}

/* What this process holds now, in KiB, or 0 where the platform does not say.

   Not the high-water mark. A high-water mark never falls, so it answers
   "how much has this process ever had", which is the same as "how much does
   it hold" only where the allocator reuses the address space it freed. Ask
   the first of those and an allocator that does not reuse reads exactly
   like the leak this check exists to catch. Where a host holds on to
   everything it has taken the two readings agree, so asking for what is
   held costs nothing there and is the right question everywhere else. */
static long resident_kib(void)
{
#if defined(_WIN32)
    release_allocator_caches();
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    release_allocator_caches();
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS)
        return 0;
    return (long)(info.resident_size / 1024);
#else
    FILE *f;
    long pages = 0;

    release_allocator_caches();
    f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    if (fscanf(f, "%*s %ld", &pages) != 1)
        pages = 0;
    fclose(f);
    return (long)((pages * (long)sysconf(_SC_PAGESIZE)) / 1024);
#endif
}

XPOST_TEST_SINK(out, 256)


int main(void)
{
    long base;
    long one = 0;
    long settled = 0;
    long grown = 0;
    long read_at[CYCLES];   /* what each cycle left held, for the shape */
    int done = 0;
    int i;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    /* the process with the library up and no context in it: what a
       context costs is read against this */
    base = resident_kib();

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

        /* the first cycle carries one context's worth onto the reading,
           so what it reads after it is what a context costs */
        if (i == 0)
            one = resident_kib();
        if (i == MEASURED_FROM)
            settled = resident_kib();
        grown = resident_kib();
        read_at[i] = grown;
        done = i + 1;
    }

    if (base > 0 && one > base)
    {
        long unit = one - base;
        long steps[CYCLES];
        long allowed = unit * STEP_NUMERATOR / STEP_DENOMINATOR;
        long middle = 0;
        int nsteps = 0;
        int c;

        /* the per-cycle steps over the measured half, in order, so the
           middle one can be taken */
        for (c = MEASURED_FROM + 1; c < done; c++)
            steps[nsteps++] = read_at[c] - read_at[c - 1];
        for (c = 1; c < nsteps; c++)   /* insertion sort: nsteps is CYCLES/2 */
        {
            long v = steps[c];
            int j = c - 1;

            while (j >= 0 && steps[j] > v)
            {
                steps[j + 1] = steps[j];
                j--;
            }
            steps[j + 1] = v;
        }
        if (nsteps > 0)
            middle = steps[nsteps / 2];

        printf("one context costs %ld KiB; over the last %d cycles of %d "
               "the middle step is %ld KiB and the total %ld\n", unit,
               CYCLES - 1 - MEASURED_FROM, CYCLES, middle, grown - settled);
        /* The numbers go in the complaint, not only in the line above it.
           What a harness keeps of a failing run is the lines that say they
           are failures, so a measurement printed beside the verdict is a
           measurement a reader of the log does not get -- and this verdict
           cannot be acted on without it. Retention of a unit a cycle and
           an allowance that came out near nothing read the same here and
           want opposite fixes. */
        if (middle > allowed)
        {
            report_failure("a run of contexts keeps no context's worth per"
                           " cycle: one context costs %ld KiB, the middle"
                           " step over the last %d of %d cycles is %ld KiB,"
                           " and the step allowed is half a context"
                           " (%ld KiB); the half came to %ld in all",
                           unit, CYCLES - 1 - MEASURED_FROM, CYCLES,
                           middle, allowed, grown - settled);
            printf("what each cycle left held, KiB above the empty"
                   " process:\n ");
            for (c = 0; c < done; c++)
                printf(" %ld", read_at[c] - base);
            printf("\n");
        }
    }
    else
        printf("NOTE: resident size unavailable; growth not compared\n");

    xpost_quit();

    return verdict();
}
