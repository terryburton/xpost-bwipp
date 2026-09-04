/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (C) 2013-2016, Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Xpost software product nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Whether the storage under reclaimed blocks actually leaves the
 * process.
 *
 * WHY THIS IS A C TEST AND NOT A POSTSCRIPT ONE. Nothing a program can
 * ask the interpreter reports what the process is charged for. The three
 * numbers a bank has all describe the bank: a cursor into the storage it
 * has taken, the storage it holds, and what its free lists hold. They
 * say what the bank is doing with its range and nothing about what the
 * process is charged for it, so a test written in PostScript could
 * asserted only that the figures moved as expected. What the change is
 * for is resident size, and the process's own resident size is a thing
 * only its host can read.
 *
 * WHAT IS ASSERTED, in the order it has to be established:
 *
 *   the collection puts a real amount on the free lists, so that there
 *     is something to hand back and a later figure of zero means the
 *     return did nothing rather than that there was nothing to do;
 *   closing the arena up reports something like that amount, since what
 *     it gathers above the cursor is what the lists were holding;
 *   the lists are then empty and the cursor has come back by about that
 *     much, because the blocks they named were absorbed rather than
 *     kept;
 *   the storage the bank holds does not change, since the range stays
 *     the file's however much of it is resident;
 *   resident size falls by something like the amount reported;
 *   and a block handed out afterwards reads and writes as any other.
 *
 * The last is what says the range is still the file's: a page dropped
 * this way stays addressable and is charged again when it is next
 * written, and an implementation that had instead unmapped the range
 * would fail here rather than somewhere far away.
 *
 * WHERE IT RUNS. Handing pages back needs a backing that can take them,
 * and reading resident size needs a host that reports it. Where either
 * is missing the return answers zero, which this reads as "not this
 * host" and reports as a skip rather than as a pass -- a pass would say
 * the pages went.
 *
 * AND THAT A PROGRAM REACHES IT. The above holds the mechanism and says
 * nothing about whether anything that ships calls it. So this also runs
 * as one half of a pair, under tests/run-vm-page-return-test.sh: the
 * same ordinary program in two processes, one ending with `1 vmreclaim`
 * and one not, each reporting what its process is left holding. Nothing
 * else in the interpreter hands the arena back, so the difference
 * between them is the operator having reached the return.
 *
 * It has to be a pair of processes rather than two figures from one.
 * What a program drops is freed twice over: once as it runs, which is
 * what a collection it asks for can hand back, and again by the rewind
 * that ends the job, which hands back nothing. A figure read after the
 * job has ended therefore says nothing on its own, and the two runs
 * have to differ in the one thing being asked about.
 *
 * WHAT IT DOES NOT ESTABLISH. That a collection running of its own
 * accord leaves the storage alone. That is what the operator is written
 * to do and it is a negative: showing it would mean provoking an
 * automatic collection and reading a figure that did not move, which a
 * host under load moves anyway.
 *
 * MODES.
 *   (none)  the mechanism, above
 *   ask     run the program, ending with `1 vmreclaim`, and report
 *           what the process is left holding
 *   noask   the same program without the request */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
# undef WIN32_LEAN_AND_MEAN
# include <psapi.h>
#endif
#ifdef __APPLE__
# include <mach/mach.h>
# include <mach/task_info.h>
#endif

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_string.h"
#include "xpost_free.h"
#include "xpost_garbage.h"

#include "xpost_test.h"

/* Blocks of fifteen pages each at the commonest page size, and enough of
   them that the figure is well clear of anything the boot or this test's
   own allocations move. Each is written through, so the pages are
   resident before anything is handed back: a page never touched is not
   charged for and could not be given up.

   Fifteen pages rather than more because a string counts its length in a
   field that stops at XPOST_OBJECT_COMP_MAX_SZ, which is 65535 at the
   narrower object width. The number of blocks is what carries the total
   instead. */
#define BLOCK_SZ 61440u
#define BLOCK_N 256

/* What the process is charged for, in bytes, or zero where the host does
   not say -- which the caller reads as "not this host" rather than as a
   figure. Each host names it differently: the second field of
   /proc/self/statm is a count of resident pages, a Windows process is
   asked for its working set, and macOS is asked for its footprint.

   The footprint rather than the resident size, on that host, because the
   two do not agree: a range given up by some of the calls there leaves
   the resident size where it was and the footprint is what falls. The
   footprint is also the figure the system itself acts on, so it is the
   one worth holding the change to. */
static size_t resident_bytes(void)
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;

    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return 0;
    return (size_t)pmc.WorkingSetSize;
#elif defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;

    if (task_info(mach_task_self(), TASK_VM_INFO,
                  (task_info_t)&info, &n) != KERN_SUCCESS)
        return 0;
    return (size_t)info.phys_footprint;
#elif defined(__linux__)
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long total = 0, resident = 0;
    int got;

    if (!f)
        return 0;
    got = fscanf(f, "%lu %lu", &total, &resident);
    fclose(f);
    if (got != 2)
        return 0;
    return (size_t)resident * xpost_memory_page_size;
#else
    return 0;
#endif
}

/* The two halves of the pair. One program, run to its end, in a process
   that reports what it is left holding. The strings are written through
   a byte per page: a page never touched is not charged for, and one that
   was never charged for cannot be given up, so an untouched arena would
   have both halves report the same figure and say nothing.

   What the figure means is decided by the script comparing the two; what
   this process answers is only whether it got that far. */
static void _report_run(Xpost_Context *ctx, int ask)
{
    static const char body[] =
        "0 1 255 { pop 61440 string 0 4096 61439 { 1 index exch 120 put }"
        " for pop } for";
    char prog[sizeof body + 32];

    sprintf(prog, "%s%s\n", body, ask ? " 1 vmreclaim" : "");
    if (xpost_run(ctx, XPOST_INPUT_STRING, prog, 0) == XPOST_RUN_ERRORED)
        report_failure("the program %s the request did not run",
                       ask ? "with" : "without");
    else
        printf("rss=%lu\n", (unsigned long)resident_bytes());
}

int main(int argc, char **argv)
{
    Xpost_Context *ctx;
    Xpost_Memory_File *mem;
    char *text;
    unsigned int free_before, free_after;
    unsigned int used_before, used_after;
    size_t max_before, max_after;
    size_t rss_held, rss_after;
    unsigned int given;
    unsigned int fresh = 0;
    int i;

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
    mem = ctx->lo;

    if (argc == 2 && (strcmp(argv[1], "ask") == 0
                      || strcmp(argv[1], "noask") == 0))
    {
        _report_run(ctx, strcmp(argv[1], "ask") == 0);

        xpost_destroy(ctx);
        xpost_quit();
        return verdict();
    }

    text = malloc(BLOCK_SZ);
    if (!text)
    {
        report_failure("could not make the bytes the blocks are filled from");
        xpost_destroy(ctx);
        xpost_quit();
        return verdict();
    }
    memset(text, 'x', BLOCK_SZ);

    /* Blocks nothing refers to once the hold stack the constructor
       stashed them on is cleared, which is what the interpreter clears
       between operator executions. */
    for (i = 0; i < BLOCK_N; i++)
    {
        if (xpost_object_get_type(xpost_string_cons(ctx, BLOCK_SZ, text))
                != stringtype)
        {
            report_failure("could not allocate block %d", i);
            free(text);
            xpost_destroy(ctx);
            xpost_quit();
            return verdict();
        }
    }
    xpost_stack_clear(ctx->lo, ctx->hold);

    if (xpost_garbage_collect(mem, 1, 1) < 0)
        report_failure("the collection that fills the list failed");

    free_before = xpost_free_bytes(mem);
    used_before = mem->high_water;
    max_before = mem->max;
    rss_held = resident_bytes();

    check(free_before > (unsigned int)BLOCK_SZ * BLOCK_N / 2u,
          "a collection over the dropped blocks leaves a real amount free");

    if (!xpost_free_compact(mem, &given))
        given = 0;

    if (given == 0 || rss_held == 0)
    {
        /* Not a pass: nothing was handed back, and saying so is the
           whole of what this can report on such a host. */
        printf("SKIP: this host %s\n",
               given == 0 ? "keeps the storage the arena gathered"
                          : "does not report a resident set");
        free(text);
        xpost_destroy(ctx);
        xpost_quit();
        return verdict();
    }

    /* Something like the free amount: what gathers above the cursor is
       what the lists were holding, so a pass that reached only a
       fraction of the blocks would fail. */
    check(given > free_before / 2u,
          "closing the arena up gathers something like what the lists hold");

    free_after = xpost_free_bytes(mem);
    used_after = mem->high_water;
    max_after = mem->max;

    /* The blocks were absorbed rather than kept, so the lists that named
       them are empty and the cursor has come back over their storage. */
    check(free_after < free_before / 2u,
          "the lists no longer hold what was absorbed");
    check(used_before - used_after > given / 2u,
          "the cursor comes back by something like that much");
    /* The range stays the file's however little of it is resident, which
       is what lets the storage be taken again without asking. */
    check(max_after == max_before,
          "the storage the bank holds does not change");

    rss_after = resident_bytes();

    /* An arena the file borrowed from the host allocator is not this
       process's to give back: the pass gathers the storage above the
       cursor there as it does anywhere, and the host goes on holding it.
       Whether that is this host is asked of the file rather than derived
       from the build, so the rule stays in the one place that states it,
       and it is asked only where the figure did not move -- a host that
       does hand storage back is still held to having handed it back. The
       range asked about is the dead run the pass just left above the
       cursor, so nothing anything would read is given up. */
    if (rss_after + given / 2u >= rss_held
        && xpost_memory_file_release_range(mem, used_after,
                                           max_after - used_after) == 0)
    {
        printf("SKIP: this host keeps the storage the arena gathered\n");
        free(text);
        xpost_destroy(ctx);
        xpost_quit();
        return verdict();
    }

    check(rss_after + given / 2u < rss_held,
          "the process is charged for something like that much less");

    /* and the range is still the file's: addressable, and charged again
       as it is written */
    if (!xpost_memory_table_alloc(mem, BLOCK_SZ, 0, &fresh))
        report_failure("a block cannot be allocated after the pages went back");
    else
    {
        char *readback = malloc(BLOCK_SZ);

        if (!readback)
            report_failure("could not make the buffer to read a block back");
        else
        {
            check(xpost_memory_put(mem, fresh, 0, BLOCK_SZ, text) == 1,
                  "a block whose pages went back is written as any other");
            memset(readback, 0, BLOCK_SZ);
            check(xpost_memory_get(mem, fresh, 0, BLOCK_SZ, readback) == 1,
                  "and read back");
            check(memcmp(readback, text, BLOCK_SZ) == 0,
                  "and holds what was written into it");
            free(readback);
        }
    }

    free(text);
    xpost_destroy(ctx);
    xpost_quit();

    return verdict();
}
