/*
 * Job-to-job VM isolation: the security boundary between successive jobs
 * run in one long-lived context (the worker/embedder deployment).
 *
 * PLRM 3.7.7 makes each job encapsulated: the server takes an outermost
 * save over BOTH local and global VM before the job and restores both
 * after it, and explicitly resets the stacks and the interpreter
 * parameters restore does not cover (steps 3 and 5). One job must not be
 * able to leave anything a later job can see.
 *
 * This is the adversarial battery. Every case runs a hostile job1 that
 * tries to poison the interpreter one way, then a job2 that asserts the
 * interpreter it starts from is pristine. It runs under snapshots-on (the
 * isolation contract) across the page semantics an embedder can pick,
 * including XPOST_SHOWPAGE_RETURN -- the raster-worker shape where a job
 * is not one call but a first call that yields at showpage and a later
 * call that finishes it, so the boundary has to bracket the whole
 * multi-call job, not each call.
 *
 * A vector proven clean here is one job2 cannot be poisoned through. The
 * battery is the record of which vectors the boundary closes.
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
#include "xpost_context.h"

#include "xpost_test.h"

static char out_buf[1024];
static size_t out_len;

static size_t out_sink(void *user, const char *buf, size_t len)
{
    (void)user;
    if (out_len + len < sizeof out_buf - 1)
    {
        memcpy(out_buf + out_len, buf, len);
        out_len += len;
    }
    return len;
}

/* Run one job to completion. Under the returning semantics a job yields
   at each page boundary and is resumed until it finishes; the guard
   fails rather than hangs if it never ends. */
static Xpost_Run_Status run_job(Xpost_Context *ctx, const char *prog)
{
    Xpost_Run_Status st;
    int resumes = 0;

    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING, prog, 0);
    while (st == XPOST_RUN_YIELDED && resumes++ < 16)
        st = xpost_run(ctx, XPOST_INPUT_RESUME, "", 0);
    out_buf[out_len] = '\0';
    return st;
}

static const char *sem_name(Xpost_Showpage_Semantics s)
{
    switch (s)
    {
        case XPOST_SHOWPAGE_DEFAULT: return "default";
        case XPOST_SHOWPAGE_NOPAUSE: return "nopause";
        case XPOST_SHOWPAGE_RETURN:  return "return";
        default: return "?";
    }
}

/* one poison/probe pair: run poison in job1, then run probe in job2 and
   require its printed verdict to be exactly want (the probe prints CLEAN
   when the interpreter is pristine, POISONED otherwise). */
static void vector(Xpost_Context *ctx, Xpost_Showpage_Semantics sem,
                   const char *what, const char *poison, const char *probe)
{
    Xpost_Run_Status st;

    st = run_job(ctx, poison);
    if (st != XPOST_RUN_COMPLETE && st != XPOST_RUN_ERRORED)
        report_failure("%s/%s: poison job did not finish (%d)",
                       sem_name(sem), what, (int)st);

    st = run_job(ctx, probe);
    if (st != XPOST_RUN_COMPLETE)
        report_failure("%s/%s: probe job did not complete (%d): '%s'",
                       sem_name(sem), what, (int)st, out_buf);
    else if (strcmp(out_buf, "CLEAN") != 0)
        report_failure("%s/%s: job2 not pristine: '%s'",
                       sem_name(sem), what, out_buf);
}

static void battery(Xpost_Showpage_Semantics sem)
{
    Xpost_Context *ctx;

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL, sem,
                       XPOST_OUTPUT_MESSAGE_QUIET, XPOST_USE_SIZE, 64, 64);
    if (!ctx)
    {
        report_failure("%s: xpost_create", sem_name(sem));
        return;
    }
    /* isolation is the snapshots-on contract */
    xpost_job_snapshots_set(ctx, 1);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    /* warm the language once so job1 is not the load */
    run_job(ctx, "(warm)pop");

    /* V1: redefine a standard operator through globaldict (global VM) */
    vector(ctx, sem, "global-op-redef",
           "true setglobal globaldict /add {pop pop 99} put false setglobal",
           "2 3 add 5 eq {(CLEAN)}{(POISONED)}ifelse print flush");

    /* V2: anchor a hostile name in globaldict (global VM) */
    vector(ctx, sem, "global-name-anchor",
           "true setglobal globaldict /XHOSTILE {42} put false setglobal",
           "/XHOSTILE where {pop(POISONED)}{(CLEAN)}ifelse print flush");

    /* V3: the soundness key -- a LOCAL-mode job writes a PRE-EXISTING
       global slot (globaldict); the write must log to the global bank */
    vector(ctx, sem, "soundness-key-local-writes-global",
           "globaldict /XSK 7 put",
           "globaldict /XSK known {(POISONED)}{(CLEAN)}ifelse print flush");

    /* V4: local userdict definition */
    vector(ctx, sem, "local-def",
           "/XLOCAL 1 def",
           "/XLOCAL where {pop(POISONED)}{(CLEAN)}ifelse print flush");

    /* V5: leftover operand stack */
    vector(ctx, sem, "operand-stack-leftover",
           "1 2 3 4 5",
           "count 0 eq {(CLEAN)}{(POISONED)}ifelse print flush");

    /* V6: leftover dict on the dict stack (name-lookup poisoning) */
    vector(ctx, sem, "dict-stack-leftover",
           "<< /add {pop pop 123} >> begin",
           "countdictstack 3 le {2 3 add 5 eq}{false}ifelse "
           "{(CLEAN)}{(POISONED)}ifelse print flush");

    /* V7: RNG seed carryover (not in VM) */
    vector(ctx, sem, "rng-seed",
           "42 srand",
           "rrand 42 eq {(POISONED)}{(CLEAN)}ifelse print flush");

    /* V8: array-packing mode carryover (not in VM) */
    vector(ctx, sem, "packing-mode",
           "true setpacking",
           "currentpacking {(POISONED)}{(CLEAN)}ifelse print flush");

    /* V9: unbalanced save left pending at job end */
    vector(ctx, sem, "unbalanced-save",
           "save pop save pop 1 2 3",
           "count 0 eq {2 3 add 5 eq}{false}ifelse {(CLEAN)}{(POISONED)}ifelse "
           "print flush");

    /* V10: job ends in an uncaught error (the boundary must still run) */
    vector(ctx, sem, "uncaught-error",
           "true setglobal globaldict /add {pop pop 7} put false setglobal "
           "1 0 div",
           "2 3 add 5 eq {(CLEAN)}{(POISONED)}ifelse print flush");

    /* V11: job leaves leftover gsave depth / dirtied graphics */
    vector(ctx, sem, "graphics-leftover",
           "gsave 3 3 3 setrgbcolor 10 10 moveto",
           "gsave grestore (CLEAN)print flush");

    /* V12: string remanence -- mutate the bytes of a reachable pre-existing
       string (the version string in systemdict) */
    vector(ctx, sem, "string-remanence",
           "{version dup length 0 gt {0 88 put}{pop} ifelse} stopped pop",
           "version 0 get 88 eq {(POISONED)}{(CLEAN)}ifelse print flush");

    /* V13: a job that tries to discover and re-restore the outermost VM
       state cannot reach it (the baseline is the server's, held in C, not
       a save object on any stack the job can read); aggressive save/restore
       plus a global poison still leaves job2 clean */
    vector(ctx, sem, "hostile-save-restore",
           "{ save save save restore restore restore } stopped pop "
           "true setglobal globaldict /add {pop pop 3} put false setglobal",
           "2 3 add 5 eq {(CLEAN)}{(POISONED)}ifelse print flush");

    /* V14: the store-order invariant -- a global composite may never come to
       reference a younger local object (invalidaccess at store time), so the
       revert can never meet a dangling cross-bank reference. The attempt is
       refused and job2 is clean either way. */
    vector(ctx, sem, "store-order-invariant",
           "{ true setglobal /GA 4 array def false setglobal "
           "  GA 0 (localstr) put } stopped pop",
           "/GA where {pop(POISONED)}{(CLEAN)}ifelse print flush");

    /* V15: the boundary does not hand back what the lockdown took away.
       The lockdown runs at boot, BEFORE the first job, and the boundary is
       an outermost restore over both banks -- so what it restores TO is the
       state the server took its snapshot at, and a snapshot taken earlier
       than the lockdown would put the machinery back in a program's reach
       for every job after the first. Job1 does nothing but exist; the
       question is entirely about what job2 can see. */
    vector(ctx, sem, "machinery-hidden-across-the-boundary",
           "(job) pop",
           "/graphicsdict where {pop(POISONED)}"
           "{systemdict /.privatedict known {(POISONED)}"
           "{systemdict /DEVICE known {(POISONED)}"
           "{1183615869 internaldict length 0 eq {(CLEAN)}{(POISONED)}ifelse}"
           "ifelse}ifelse}ifelse print flush");

    /* V16: and a job that shadows a machinery name in its own dictionary
       does not leave the name behind for the next one -- nor does the
       revert of that shadow uncover a real one underneath it. */
    vector(ctx, sem, "machinery-name-shadow",
           "/graphicsdict {42} def /DEVICE 7 def",
           "/graphicsdict where {pop(POISONED)}"
           "{/DEVICE where {pop(POISONED)}{(CLEAN)}ifelse}ifelse print flush");

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

    battery(XPOST_SHOWPAGE_NOPAUSE);
    battery(XPOST_SHOWPAGE_DEFAULT);
    battery(XPOST_SHOWPAGE_RETURN);

    xpost_quit();
    return verdict();
}
