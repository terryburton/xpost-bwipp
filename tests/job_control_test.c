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

XPOST_TEST_SINK(out, 512)


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


/* What tier the presented password reaches, as the interpreter now stands:
   REFUSED if startjob would not start a job, ORDINARY if it starts one that
   may not state a cache capacity, ADMIN if it starts one that may. The
   capacity stated is the one already in force, so asking the question does
   not change the answer for the next asking. */
static const char *tier_reached(Xpost_Context *ctx, const char *password)
{
    char prog[512];

    snprintf(prog, sizeof prog,
             "true (%s) startjob "
             "{ { mark 1048576 4 32768 setcacheparams } stopped "
             "    { (ORDINARY) }{ (ADMIN) } ifelse } "
             "{ (REFUSED) } ifelse print flush",
             password);
    return run_job(ctx, prog);
}

/* one row of the matrix below */
static void expect_tier(Xpost_Context *ctx, int jobserver,
                        const char *admin_pw, const char *job_pw,
                        const char *presented, const char *want)
{
    const char *got;

    xpost_system_params_password_set(ctx, admin_pw);
    xpost_startjob_password_set(ctx, job_pw);
    xpost_jobserver_set(ctx, jobserver);
    got = tier_reached(ctx, presented);
    if (strcmp(got, want) != 0)
        report_failure("%s, admin %s, job %s, presenting '%s': got %s, want %s",
                       jobserver ? "job stream" : "single run",
                       !admin_pw ? "unconfigured"
                                 : (admin_pw[0] ? "set" : "set empty"),
                       !job_pw ? "unconfigured"
                               : (job_pw[0] ? "set" : "set empty"),
                       presented, got, want);
    xpost_jobserver_set(ctx, 0);
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

    xpost_startjob_password_set(ctx, NULL);  /* unconfigured again */

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

    /* PLRM C.3.1: the two passwords start two kinds of unencapsulated job,
       and the difference is what a job may do to the jobs after it. An
       ordinary one may alter initial VM -- install a prolog, install a font
       -- and may not change an implementation limit; an administrator job
       may do both. A host taking jobs from several submitters sets both
       passwords, which is the only way to hand out the first without the
       second. */
    xpost_startjob_password_set(ctx, "jobpw");
    xpost_system_params_password_set(ctx, "adminpw");

    /* the start job password starts a job, and it is not an administrator:
       a capacity stated to setcacheparams is refused */
    if (strcmp(run_job(ctx,
            "true (jobpw) startjob {(STARTED)}{(DENIED)}ifelse print flush"),
            "STARTED") != 0)
        report_failure("the start job password did not start a job: '%s'",
                       out_buf);
    if (strcmp(run_job(ctx,
            "true (jobpw) startjob pop "
            "{ mark 262144 4 32768 setcacheparams } stopped "
            "{ $error /errorname get == }{ (ALLOWED) print } ifelse flush"),
            "/invalidaccess\n") != 0)
        report_failure("an ordinary unencapsulated job was allowed to change"
                       " the cache capacity: '%s'", out_buf);

    /* it may still alter initial VM, which is what it is for */
    expect(ctx, "an ordinary unencapsulated job may install a prolog",
           "true (jobpw) startjob pop /prologdef { 1 } def",
           "/prologdef where {pop(KEPT)}{(GONE)}ifelse print flush",
           "KEPT");

    /* the system parameter password starts an administrator job, which may */
    if (strcmp(run_job(ctx,
            "true (adminpw) startjob pop "
            "{ mark 262144 4 32768 setcacheparams (ALLOWED) print } stopped "
            "{ $error /errorname get == } if flush"),
            "ALLOWED") != 0)
        report_failure("an administrator job could not change the cache"
                       " capacity: '%s'", out_buf);

    /* a password that is neither starts nothing */
    if (strcmp(run_job(ctx,
            "true (neither) startjob {(STARTED)}{(DENIED)}ifelse print flush"),
            "DENIED") != 0)
        report_failure("a password matching neither started a job: '%s'",
                       out_buf);

    /* An empty system parameter password collapses the two tiers, which is
       the factory default C.3.1 describes: every startjob is then an
       administrator job. Restored last so the check leaves the interpreter
       as it found it. */
    xpost_system_params_password_set(ctx, "");
    xpost_startjob_password_set(ctx, "");
    if (strcmp(run_job(ctx,
            "true () startjob pop "
            "{ mark 262144 4 32768 setcacheparams (ALLOWED) print } stopped "
            "{ $error /errorname get == } if flush"),
            "ALLOWED") != 0)
        report_failure("with no system parameter password set, startjob did"
                       " not start an administrator job: '%s'", out_buf);


    /* Every combination of the two passwords against the two modes, because
       what an unset password means is the whole of the difference between
       them and nothing else states it.

       A single run of a single job has nobody to protect from the program
       it was started with, so with neither password set it admits at
       administrator level -- the factory default PLRM C.3.1 describes. A
       job stream is the opposite case: it serves jobs it did not choose,
       from submitters that do not trust each other, and admits only what it
       was configured to admit. An unset password there opens nothing.

       In both, setting either password is a configuration someone made, and
       a tier left unset beside it is closed rather than collapsed. */
    {
        static const struct {
            int jobserver;
            const char *admin_pw, *job_pw, *presented, *want;
        } matrix[] = {
            /* NULL is a password never configured; "" is one configured as
               the empty string, which a job presents by presenting nothing.
               They differ, and the difference is the whole of what a job
               stream admits. */

            /* --- a single run: an unconfigured password is an open door - */
            { 0, NULL,   NULL,    "",       "ADMIN"    },
            { 0, NULL,   NULL,    "junk",   "ADMIN"    },
            /* configured empty is a password, and it is matched by nothing */
            { 0, NULL,   "",      "",       "ORDINARY" },
            { 0, NULL,   "",      "junk",   "REFUSED"  },
            { 0, "",     NULL,    "",       "ADMIN"    },
            /* a start job password set alone admits, and admits no
               administrator: the tier beside it was left closed */
            { 0, NULL,   "jobpw", "jobpw",  "ORDINARY" },
            { 0, NULL,   "jobpw", "wrong",  "REFUSED"  },
            /* an administrator password alone: the door is still open,
               but reaching the tier takes the password */
            { 0, "adpw", NULL,    "adpw",   "ADMIN"    },
            { 0, "adpw", NULL,    "wrong",  "ORDINARY" },
            /* both configured: each password reaches its own tier */
            { 0, "adpw", "jobpw", "adpw",   "ADMIN"    },
            { 0, "adpw", "jobpw", "jobpw",  "ORDINARY" },
            { 0, "adpw", "jobpw", "wrong",  "REFUSED"  },

            /* --- a job stream: unconfigured admits nobody --------------- */
            { 1, NULL,   NULL,    "",       "REFUSED"  },
            { 1, NULL,   NULL,    "junk",   "REFUSED"  },
            /* but configured empty is configured, and admits */
            { 1, NULL,   "",      "",       "ORDINARY" },
            { 1, NULL,   "",      "junk",   "REFUSED"  },
            { 1, "",     NULL,    "",       "ADMIN"    },
            { 1, NULL,   "jobpw", "jobpw",  "ORDINARY" },
            { 1, NULL,   "jobpw", "wrong",  "REFUSED"  },
            { 1, "adpw", NULL,    "adpw",   "ADMIN"    },
            { 1, "adpw", NULL,    "wrong",  "REFUSED"  },
            { 1, "adpw", "jobpw", "adpw",   "ADMIN"    },
            { 1, "adpw", "jobpw", "jobpw",  "ORDINARY" },
            { 1, "adpw", "jobpw", "wrong",  "REFUSED"  }
        };
        size_t i;

        for (i = 0; i < sizeof matrix / sizeof *matrix; i++)
            expect_tier(ctx, matrix[i].jobserver, matrix[i].admin_pw,
                        matrix[i].job_pw, matrix[i].presented,
                        matrix[i].want);
    }

    /* left as it was found */
    xpost_system_params_password_set(ctx, NULL);
    xpost_startjob_password_set(ctx, NULL);
    xpost_jobserver_set(ctx, 0);

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
    xpost_quit();
    return verdict();
}
