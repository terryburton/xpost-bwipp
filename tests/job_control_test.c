/*
 * The PostScript job-control operators, startjob and exitserver (PLRM
 * 3.7.7), driving the job boundary.
 *
 * A job is encapsulated by default: its boundary reverts it, so a name it
 * defines is gone by the next job. exitserver (and `true password
 * startjob`) end the encapsulated job and start an unencapsulated one, so
 * a name defined after it OUTLIVES the job -- the boundary folds the run's
 * state into the baseline instead of reverting. `false password startjob`
 * returns to encapsulated. The door is guarded by a password (empty by
 * default = open, PLRM C.3.1) the host sets; and startjob is neutralised
 * when bracketed in a program's own save/restore (save nesting deeper than
 * the job's own level), so a file that uses it can be run as part of
 * another job unchanged.
 *
 * The requested test is the persistence one: a procedure installed through
 * exitserver survives a job boundary. Its contrasts -- the same definition
 * without exitserver does not, false reverts, true persists, a wrong
 * password denies, and save-nesting neutralises -- are here beside it.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include "xpost.h"
#include "xpost_test.h"

static char out_buf[512];
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

static const char *run_job(Xpost_Context *ctx, const char *prog)
{
    Xpost_Run_Status st;
    int resumes = 0;

    out_len = 0;
    st = xpost_run(ctx, XPOST_INPUT_STRING, prog, 0);
    while (st == XPOST_RUN_YIELDED && resumes++ < 16)
        st = xpost_run(ctx, XPOST_INPUT_RESUME, "", 0);
    out_buf[out_len] = '\0';
    return out_buf;
}

/* run prog, then a probe, and require the probe's printed verdict to be want */
static void expect(Xpost_Context *ctx, const char *what,
                   const char *prog, const char *probe, const char *want)
{
    (void)run_job(ctx, prog);
    {
        const char *got = run_job(ctx, probe);
        if (strcmp(got, want) != 0)
            report_failure("%s: got '%s', want '%s'", what, got, want);
    }
}

int main(void)
{
    Xpost_Context *ctx;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }
    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL, XPOST_SHOWPAGE_NOPAUSE,
                       XPOST_OUTPUT_MESSAGE_QUIET, XPOST_USE_SIZE, 64, 64);
    if (!ctx)
    {
        report_failure("xpost_create");
        return verdict();
    }
    xpost_stdout_handler_set(ctx, out_sink, NULL);
    (void)run_job(ctx, "(warm)pop");  /* establish the baseline */

    /* THE requested test: a procedure installed through exitserver outlives
       the job boundary and is still callable, returning its value */
    expect(ctx, "exitserver persists a definition across a job",
           "serverdict begin () exitserver /survivor { 40 2 add } def",
           "/survivor where {pop survivor ==}{(GONE)print}ifelse flush",
           "42\n");

    /* the same definition WITHOUT exitserver is reverted */
    expect(ctx, "an encapsulated job's definition does not survive",
           "/survivor2 { 42 } def",
           "/survivor2 where {pop(KEPT)}{(GONE)}ifelse print flush",
           "GONE");

    /* false password startjob: the job stays encapsulated, its def reverts */
    expect(ctx, "false startjob leaves the job encapsulated",
           "false () startjob /fs { 1 } def",
           "/fs where {pop(KEPT)}{(GONE)}ifelse print flush",
           "GONE");

    /* true password startjob: the job is unencapsulated, its def persists */
    expect(ctx, "true startjob makes the job's definitions persist",
           "true () startjob /ts { 1 } def",
           "/ts where {pop(KEPT)}{(GONE)}ifelse print flush",
           "KEPT");

    /* startjob bracketed in the program's own save/restore is neutralised:
       the save nesting is deeper than the job level, so it does not start a
       job -- it pushes false and the definition after it still reverts */
    if (strcmp(run_job(ctx,
            "save true () startjob {(STARTED)}{(REFUSED)}ifelse print flush restore"),
            "REFUSED") != 0)
        report_failure("startjob inside save/restore was not neutralised: '%s'",
                       out_buf);
    expect(ctx, "a neutralised startjob does not persist what follows it",
           "save true () startjob pop /sj { 1 } def restore",
           "/sj where {pop(KEPT)}{(GONE)}ifelse print flush",
           "GONE");

    /* the password door, open by default: exitserver with the empty
       password succeeds (already shown above). Now lock it. */
    xpost_startjob_password_set(ctx, "s3cret");

    /* a wrong password: startjob pushes false and starts no job */
    if (strcmp(run_job(ctx,
            "true (wrong) startjob {(STARTED)}{(DENIED)}ifelse print flush"),
            "DENIED") != 0)
        report_failure("startjob with a wrong password was not denied: '%s'",
                       out_buf);
    /* and a def attempted through a denied exitserver does not persist */
    expect(ctx, "a wrong password denies persistence",
           "{ serverdict begin (wrong) exitserver /nope { 1 } def } stopped pop",
           "/nope where {pop(KEPT)}{(GONE)}ifelse print flush",
           "GONE");
    /* a wrong-password exitserver raises invalidaccess */
    if (strcmp(run_job(ctx,
            "{ serverdict begin (wrong) exitserver } stopped "
            "{ $error /errorname get == }{ (NOERROR)print } ifelse flush"),
            "/invalidaccess\n") != 0)
        report_failure("wrong-password exitserver did not raise invalidaccess: '%s'",
                       out_buf);

    /* the correct password re-opens the door */
    expect(ctx, "the correct password permits persistence",
           "serverdict begin (s3cret) exitserver /okpw { 1 } def",
           "/okpw where {pop(KEPT)}{(GONE)}ifelse print flush",
           "KEPT");

    xpost_startjob_password_set(ctx, "");  /* reopen for tidiness */

    /* exitserver removes serverdict from the dict stack (startjob resets it
       to the base depth) and announces on the standard output */
    {
        const char *msg = run_job(ctx,
            "serverdict begin () exitserver "
            "countdictstack 3 eq {(BASE)}{(DEEP)}ifelse print flush");
        if (!strstr(msg, "exitserver: permanent state may be changed"))
            report_failure("exitserver printed no announcement: '%s'", msg);
        if (!strstr(msg, "BASE"))
            report_failure("exitserver left serverdict on the dict stack: '%s'", msg);
    }

    /* PLRM 3.7.7 suppresses the announcement when binary is true in $error,
       so a job that asked for the quiet form is not sent a line it did not
       ask for. */
    {
        const char *msg = run_job(ctx,
            "$error /binary true put serverdict begin () exitserver "
            "(QUIET) print flush");
        if (strstr(msg, "permanent state may be changed"))
            report_failure("exitserver announced although binary is true: '%s'",
                           msg);
        if (!strstr(msg, "QUIET"))
            report_failure("exitserver did not run with binary true: '%s'", msg);
    }

    /* $error is a program's own dictionary, so the condition is asked of a
       dictionary that need not hold the key at all: exitserver still runs,
       and announces, when a job has taken it out. Last, because the job that
       removes it is unencapsulated and the removal stands. */
    {
        const char *msg = run_job(ctx,
            "$error /binary undef serverdict begin () exitserver "
            "(NOKEY) print flush");
        if (!strstr(msg, "NOKEY"))
            report_failure("exitserver failed with binary absent from $error:"
                           " '%s'", msg);
        if (!strstr(msg, "permanent state may be changed"))
            report_failure("exitserver did not announce with binary absent:"
                           " '%s'", msg);
    }

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
    xpost_quit();
    return verdict();
}
