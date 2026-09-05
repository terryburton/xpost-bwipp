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
#  include <malloc.h>         /* malloc_trim, mallinfo2 */
#  if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33)
#   define HAVE_MALLINFO2 1
#  endif
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

/* What the malloc heap holds now: the bytes in it that are in use, and the
   number of blocks they are in. Zero where the platform does not say.

   Read beside the resident size because the two answer different questions
   and only the pair says where a growth is. Resident size is every page the
   process holds, whatever took them; the heap figures are what the
   allocator has handed out and not been given back. A growth in both is
   memory allocated and not freed, and the block count then says whether it
   is many small allocations or few large ones. A growth in the resident
   size while the heap figures stand still is not in the malloc heap at all,
   which sends the hunt to mapped storage instead -- the arenas, a
   reservation, a library's own mappings -- and away from a missing free.

   The block count is the sharper of the two. It is a count of live
   allocations rather than a quantity of memory, so it does not move when an
   allocator takes room for itself and it cannot be masked by one that
   reuses what it holds: a structure that keeps one more allocation per
   cycle shows as one more block per cycle, exactly, whatever the sizes
   involved. */
static void heap_census(long *kib, long *blocks)
{
    *kib = 0;
    *blocks = 0;
#if defined(__APPLE__)
    {
        malloc_statistics_t t;

        /* every zone, which is what the interpreter's allocations are
           spread over rather than any one of them */
        malloc_zone_statistics(NULL, &t);
        *kib = (long)(t.size_in_use / 1024);
        *blocks = (long)t.blocks_in_use;
    }
#elif defined(HAVE_MALLINFO2)
    {
        struct mallinfo2 mi = mallinfo2();

        /* glibc reports the bytes in use but not how many blocks they are
           in, so the count is left unanswered rather than guessed at */
        *kib = (long)(mi.uordblks / 1024);
    }
#endif
}

/* What the host charges this process for, in KiB, where it offers a figure
   separate from the resident size. Zero everywhere else.

   The resident size counts every page the process has resident, including
   ones that are file-backed, shared, or clean and reclaimable at any
   moment. A page of a mapped file that has been touched is resident and
   costs the process nothing it cannot be relieved of. Darwin reports a
   second figure that leaves those out -- what the host would charge if it
   had to -- and the two answer different questions.

   It is read and printed rather than judged on, because which of the two a
   check like this should hold to is a decision and not a measurement, and
   the measurement has to come first. A resident size that climbs while
   this stands still is pages the process holds and is not being charged
   for, which is not the retention this check exists to catch; the two
   climbing together is. Page size is what makes the distinction worth
   drawing here: where a host maps in a larger unit, the same touched file
   costs several times as much resident size and the same behaviour reads
   differently. */
static long footprint_kib(void)
{
#if defined(__APPLE__) && defined(TASK_VM_INFO)
    task_vm_info_data_t vm;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;

    if (task_info(mach_task_self(), TASK_VM_INFO,
                  (task_info_t)&vm, &count) != KERN_SUCCESS)
        return 0;
    return (long)(vm.phys_footprint / 1024);
#else
    return 0;
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
    long held_live[CYCLES]; /* and what it held with its context still up */
    long heap_at[CYCLES];   /* and what the malloc heap held, with how many */
    long heap_blk[CYCLES];  /* blocks it was in, read at the same moments */
    long foot_at[CYCLES];   /* and what the host charged, where it says */
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
        /* what the process holds with the context still up, against what
           it holds once the context has gone: a step that is taken here
           and given back below is a context costing what a context costs,
           and one that survives below is the context's own memory not
           coming back */
        held_live[i] = resident_kib();
        xpost_destroy(ctx);

        /* the first cycle carries one context's worth onto the reading,
           so what it reads after it is what a context costs */
        if (i == 0)
            one = resident_kib();
        if (i == MEASURED_FROM)
            settled = resident_kib();
        grown = resident_kib();
        read_at[i] = grown;
        heap_census(&heap_at[i], &heap_blk[i]);
        foot_at[i] = footprint_kib();
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
        /* Printed by a run that passes as well as one that fails, so that
           a host reporting nothing is told apart from a host reporting no
           growth. What it held after the first cycle is the control: zero
           there is a platform that does not answer, and every heap reading
           below is then empty rather than flat. */
        printf("the malloc heap held %ld KiB in %ld blocks after the first"
               " cycle and moved %ld KiB and %ld blocks over the measured"
               " ones\n", heap_at[0], heap_blk[0],
               heap_at[done - 1] - heap_at[MEASURED_FROM],
               heap_blk[done - 1] - heap_blk[MEASURED_FROM]);
        printf("the host charged %ld KiB after the first cycle and %ld more"
               " over the measured ones\n", foot_at[0],
               foot_at[done - 1] - foot_at[MEASURED_FROM]);
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
            printf("and what it held with its context still up:\n ");
            for (c = 0; c < done; c++)
                printf(" %ld", held_live[c] - base);
            printf("\n");
            printf("so each context gave back, KiB:\n ");
            for (c = 0; c < done; c++)
                printf(" %ld", held_live[c] - read_at[c]);
            printf("\n");
            /* Where the growth is, which the resident size alone cannot
               say. These two moving with the resident size is memory
               allocated and never freed; these standing still while it
               climbs is a growth outside the malloc heap. A host that does
               not report them reads as zeros throughout, which is not the
               same shape as either and cannot be mistaken for one. */
            printf("what the malloc heap held, KiB above the empty"
                   " process:\n ");
            for (c = 0; c < done; c++)
                printf(" %ld", heap_at[c] - heap_at[0]);
            printf("\n");
            printf("and in how many blocks, above the first cycle:\n ");
            for (c = 0; c < done; c++)
                printf(" %ld", heap_blk[c] - heap_blk[0]);
            printf("\n");
            printf("and what the host charged, KiB above the first cycle:\n ");
            for (c = 0; c < done; c++)
                printf(" %ld", foot_at[c] - foot_at[0]);
            printf("\n");
        }
    }
    else
        printf("NOTE: resident size unavailable; growth not compared\n");

    xpost_quit();

    return verdict();
}
