/* What a graphics load or a lockdown that stops leaves behind, and what
 * it says.
 *
 * The language is read into systemdict, which is opened for writing for
 * as long as the reading lasts and closed on a one-shot when it ends. A
 * page the device cannot provide stops the reading part way, and the two
 * things that follow from that are held here.
 *
 * The name. The report a caller gets is the whole of what it is told --
 * the run that knew the limit is over -- so it has to be the limit the
 * device reached and not a name raised by the interpreter on its way out
 * of the load. $error holds one error, the most recent, so any error
 * raised after the first displaces it.
 *
 * The window. It must be shut when the load ends, however the load
 * ended, and shut for good: a program runs against systemdict as the
 * load left it, and one that could reopen it could redefine the
 * language. Both halves are read off the context, because a run that
 * reaches a program is exactly the run this cannot arrange.
 *
 * The dictionary stack goes with the window. The language is read with
 * systemdict open and current, and the two are one act: a load that shut
 * the window and left the dictionary current would leave every later
 * definition aimed at a dictionary that has just been closed. So the
 * depth the load began at is the depth it must end at.
 *
 * The other end of a run is held to the same thing, and for a reason of
 * its own. A job that painted and never asked for a page has that page
 * ended for it as the job ends, which writes a file; a name that cannot
 * be opened stops the device's own method with the device dictionary
 * current and the refused operands on the stack, and the failure is not
 * reported because the job is over. So a run that meets one has to end
 * on the stacks it was given, or the next run begins on a dictionary
 * stack with a device on top of it and definitions of its own landing in
 * the device.
 *
 * Two devices, for the two ways the refusal is raised. A device whose
 * raster is one block of pixels outside virtual memory is refused by the
 * C that would allocate it, and that refusal reaches the interpreter's
 * own error handler. A device whose raster is virtual memory is refused
 * in PostScript, which never passes through that handler at all. A check
 * put to one of them says nothing about the other.
 *
 * Asked twice, because the load is attempted once per run and the first
 * attempt is the one that meets the device: an interpreter that answers
 * the second attempt with something of its own is the failure this is
 * looking for.
 *
 * The lockdown that follows the load is held to the same three things,
 * for the same reasons: it opens the same window on systemdict, it is
 * entered again by the next start procedure, and what it leaves is what
 * the run after it has.
 *
 * A lockdown has no device to refuse it, so its refusal is arranged out
 * of its own work. The promotion of the procedure-implemented standard
 * operators puts each one back into the dictionary it was found in, so a
 * dictionary that cannot be written refuses it -- and the refusal
 * arrives while the promotion's own working dictionary is open, which is
 * what makes the dictionary stack part of the question rather than a
 * depth that was never disturbed.
 *
 * Both ways a run reaches the lockdown are put to it. A run that loads
 * the graphics language reaches it after the load, and there the load
 * that finished must not be sent round again by the lockdown stopping:
 * the language cannot be read into a systemdict that is shut, so a
 * second reading would stop under a name of its own and that name would
 * be the one the caller is told. A run that loads no graphics reaches
 * the lockdown on its own, which is the lockdown with nothing else in
 * the picture.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_dict.h"
#include "xpost_name.h"

#include "xpost_test.h"

/* A side small enough to look like a page whose area is past every
   machine's memory, so no build allocates for it and the refusal is the
   same wherever this runs. */
#define STOPPING_SIDE 2000000000
#define ORDINARY_SIDE 200

XPOST_TEST_SINK(out, 4096)


static const char *collect_run(Xpost_Context *ctx, const char *prog,
                               Xpost_Run_Status *st)
{
    out_len = 0;
    *st = xpost_run(ctx, XPOST_INPUT_STRING, prog, 0);
    out_buf[out_len] = '\0';
    return out_buf;
}

/* systemdict is the bottom of the dictionary stack. */
static int systemdict_is_writeable(Xpost_Context *ctx)
{
    return xpost_object_is_writeable(ctx,
               xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0));
}

/* The window is shut, the one-shot that opens it is spent, and the
   dictionaries the step made current are closed again. */
static void no_window_was_left(Xpost_Context *ctx, const char *whose,
                               const char *when, int entry_depth)
{
    if (systemdict_is_writeable(ctx))
        report_failure("%s: systemdict is still writeable %s", whose, when);
    if (ctx->sysdict_unlocked)
        report_failure("%s: the writeable window on systemdict is still "
                       "open %s", whose, when);
    if (!ctx->sysdict_load_done)
        report_failure("%s: the one-shot that opens systemdict is unspent "
                       "%s, so it can be opened again", whose, when);
    if (xpost_stack_count(ctx->lo, ctx->ds) != entry_depth)
        report_failure("%s: the dictionary stack is %d deep %s, not the %d "
                       "it began at", whose,
                       xpost_stack_count(ctx->lo, ctx->ds), when, entry_depth);
}

/* A page the device provides: the control that the device starts at all,
   and that a load which ran to its end leaves the same closed window. */
static void an_ordinary_page(const char *dev)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;
    const char *out;
    int entry_depth;

    ctx = xpost_create(dev, XPOST_OUTPUT_FILENAME, "/dev/null",
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, ORDINARY_SIDE, ORDINARY_SIDE);
    if (!ctx)
    {
        report_failure("%s: no context on a page it provides", dev);
        return;
    }
    entry_depth = xpost_stack_count(ctx->lo, ctx->ds);
    xpost_stdout_handler_set(ctx, out_sink, NULL);
    out = collect_run(ctx, "0 0 moveto 10 10 lineto stroke (drew) print flush",
                      &st);
    if (st != XPOST_RUN_COMPLETE)
        report_failure("%s: a page it provides did not run: %s", dev, out);
    else
        check(strstr(out, "drew") != NULL,
              "a run on a page the device provides reaches the program");
    no_window_was_left(ctx, dev, "after a load that ran to its end",
                       entry_depth);
    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* A page the device provides and cannot write.

   A job that painted and never asked for a page has that page ended for
   it as the job ends, which opens the file the page is written to. A
   name that cannot be opened stops the device's own method part way,
   and the method had made the device dictionary current to do its work:
   what the failure leaves is that dictionary still current and the
   operands the open was refused with still on the stack. The job is
   over and the failure is not reported to it -- the program is no longer
   there to be told -- so the state it left is the next job's to meet,
   and the next job is entitled to begin where this one began (PLRM
   3.7.7).

   The name below is a file in a directory that is not there, which no
   platform opens for writing and none creates anything in. It is not a
   device's own limit and not a page the device refused: the device
   provided the page and painted it, and only the writing failed. */
#define UNWRITABLE_NAME "xpost-no-such-directory/page.out"

static void a_page_it_cannot_write(const char *dev)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;
    const char *out;
    int entry_depth;
    int entry_count;

    ctx = xpost_create(dev, XPOST_OUTPUT_FILENAME, UNWRITABLE_NAME,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, ORDINARY_SIDE, ORDINARY_SIDE);
    if (!ctx)
    {
        report_failure("%s: no context on a page it cannot write", dev);
        return;
    }
    entry_depth = xpost_stack_count(ctx->lo, ctx->ds);
    entry_count = xpost_stack_count(ctx->lo, ctx->os);
    xpost_stdout_handler_set(ctx, out_sink, NULL);
    out = collect_run(ctx, "0 0 moveto 10 10 lineto stroke (drew) print flush",
                      &st);
    if (st != XPOST_RUN_COMPLETE)
        report_failure("%s: a page it cannot write did not run: %s", dev, out);
    else
        check(strstr(out, "drew") != NULL,
              "a run whose page cannot be written reaches the program");
    no_window_was_left(ctx, dev, "after a job whose page could not be written",
                       entry_depth);
    if (xpost_stack_count(ctx->lo, ctx->os) != entry_count)
        report_failure("%s: the operand stack holds %d after a job whose page "
                       "could not be written, not the %d it began at", dev,
                       xpost_stack_count(ctx->lo, ctx->os), entry_count);
    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* A page the device cannot provide. */
static void a_page_it_cannot_provide(const char *dev)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;
    const char *out;
    int attempt;
    int entry_depth;

    ctx = xpost_create(dev, XPOST_OUTPUT_FILENAME, "/dev/null",
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, STOPPING_SIDE, STOPPING_SIDE);
    if (!ctx)
    {
        report_failure("%s: no context on a page it cannot provide", dev);
        return;
    }
    entry_depth = xpost_stack_count(ctx->lo, ctx->ds);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    for (attempt = 1; attempt <= 2; attempt++)
    {
        out = collect_run(ctx, "showpage", &st);

        if (st == XPOST_RUN_COMPLETE)
            report_failure("%s: attempt %d provided a page it cannot hold",
                           dev, attempt);
        if (strstr(out, "unable to load graphics") == NULL)
            report_failure("%s: attempt %d said nothing about the load it "
                           "could not finish: %s", dev, attempt, out);
        /* PLRM 8.2 gives limitcheck for a limit of the implementation and
           VMerror for virtual memory exhausted; which of the two this page
           reaches is the platform's answer, and either is the device's own
           limit. Any other name is one the interpreter raised after it. */
        else if (strstr(out, "limitcheck") == NULL &&
                 strstr(out, "VMerror") == NULL)
            report_failure("%s: attempt %d names no limit the device "
                           "reached: %s", dev, attempt, out);

        no_window_was_left(ctx, dev, "after a load that stopped",
                                entry_depth);
    }

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* The interpreter's own flags, which it keeps in its private namespace.
   The namespace loses its userdict anchor when the lockdown finishes, so
   this reaches it only in a run whose lockdown stopped -- which is every
   run below that asks. */
static Xpost_Object interpreter_flags(Xpost_Context *ctx)
{
    Xpost_Object sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    Xpost_Object ud = xpost_dict_get(ctx, sd, xpost_name_cons(ctx, "userdict"));

    if (xpost_object_get_type(ud) != dicttype)
        return null;
    return xpost_dict_get(ctx, ud, xpost_name_cons(ctx, ".internaldict"));
}

static Xpost_Object flag(Xpost_Context *ctx, const char *name)
{
    Xpost_Object flags = interpreter_flags(ctx);

    if (xpost_object_get_type(flags) != dicttype)
    {
        report_failure("the interpreter's flags are out of reach");
        return null;
    }
    return xpost_dict_get(ctx, flags, xpost_name_cons(ctx, name));
}

static int flag_is_set(Xpost_Context *ctx, const char *name)
{
    return xpost_object_get_type(flag(ctx, name)) != invalidtype;
}

static int flag_names(Xpost_Context *ctx, const char *name, const char *what)
{
    Xpost_Object held = flag(ctx, name);

    return xpost_object_get_type(held) == nametype &&
           xpost_dict_compare_objects(ctx, held,
                                      xpost_name_cons(ctx, what)) == 0;
}

/* The dictionary the language itself never defines into, and which
   stands above systemdict in the name lookup: a standard operator
   planted there is the one the promotion finds, and nothing that runs
   before the lockdown is disturbed by what it holds. */
static Xpost_Object the_home_above(Xpost_Context *ctx)
{
    Xpost_Object sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    Xpost_Object gd = xpost_dict_get(ctx, sd, xpost_name_cons(ctx, "globaldict"));

    if (xpost_object_get_type(gd) != dicttype)
        report_failure("globaldict is not a dictionary");
    return gd;
}

static void plant(Xpost_Context *ctx, Xpost_Object home, const char *op,
                  Xpost_Object value)
{
    if (xpost_object_get_type(home) != dicttype)
        return;
    if (xpost_dict_put(ctx, home, xpost_name_cons(ctx, op), value))
        report_failure("cannot plant %s where the promotion will find it", op);
}

/* A home the promotion cannot write the promoted operator back into. The
   refusal is raised by the C the write goes through, so the
   interpreter's own error handler sees it. */
static void a_home_that_refuses(Xpost_Context *ctx, const char *op)
{
    Xpost_Object sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    Xpost_Object home = the_home_above(ctx);
    Xpost_Object proc = xpost_dict_get(ctx, sd, xpost_name_cons(ctx, op));

    if (xpost_object_get_type(proc) != arraytype)
    {
        report_failure("%s is not a procedure before the promotion", op);
        return;
    }
    plant(ctx, home, op, proc);
    xpost_object_set_access(ctx, home, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
}

/* An operator that states its operands and holds no procedure to
   promote, which is the post-condition the lockdown checks once the
   promotion has run. That refusal is raised in PostScript and never
   passes through the interpreter's own error handler, so the window on
   systemdict is the lockdown's own to shut. */
static void a_home_that_holds_no_procedure(Xpost_Context *ctx, const char *op)
{
    plant(ctx, the_home_above(ctx), op, xpost_int_cons(0));
}

/* A lockdown that cannot finish. Twice, and the second attempt is the
   one under test: it must answer with what stopped the first rather than
   with anything of its own. */
static void a_lockdown_that_stops(int graphics, int in_postscript)
{
    Xpost_Context *ctx;
    Xpost_Run_Status st;
    const char *out;
    const char *how = graphics ? "after the graphics language"
                              : "with no graphics language";
    /* PLRM 8.2 gives invalidaccess for an attempt to write where access
       forbids it, and undefined for a name with no value; which of the
       two arrives is which refusal was arranged. */
    const char *name = in_postscript ? "undefined" : "invalidaccess";
    int attempt;
    int entry_depth;

    ctx = xpost_create("null", XPOST_OUTPUT_FILENAME, "/dev/null",
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, ORDINARY_SIDE, ORDINARY_SIDE);
    if (!ctx)
    {
        report_failure("%s: no context", how);
        return;
    }
    if (!graphics)
        xpost_skip_graphics_set(ctx, 1);
    if (in_postscript)
        a_home_that_holds_no_procedure(ctx, "rectfill");
    else
        a_home_that_refuses(ctx, "rectfill");
    entry_depth = xpost_stack_count(ctx->lo, ctx->ds);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    for (attempt = 1; attempt <= 2; attempt++)
    {
        out = collect_run(ctx, "showpage", &st);

        if (st == XPOST_RUN_COMPLETE)
            report_failure("%s: attempt %d ran a program on an interpreter "
                           "it could not lock down", how, attempt);
        /* the two start procedures say it differently; either way the
           caller is told the run never reached its program */
        if (strstr(out, graphics ? "unable to load graphics"
                                 : "unable to lock down interpreter") == NULL)
            report_failure("%s: attempt %d said nothing about the start-up it "
                           "could not finish: %s", how, attempt, out);
        /* The name the lockdown stopped under is the one it keeps and
           the one it raises again. */
        if (!flag_names(ctx, "LOCKDOWN_STOPPED", name))
            report_failure("%s: after attempt %d the lockdown does not hold "
                           "%s, the name it stopped under", how, attempt, name);
        /* A start-up ordering complaint on a later attempt is the
           interpreter finding its own repeat out of order, which is a
           fault of the repeat and not of what stopped the lockdown. The
           attempt that met the arranged refusal may make it: where the
           refusal was arranged out of the lockdown's own post-condition,
           that complaint is the refusal. */
        if (attempt > 1 || !in_postscript)
        {
            if (strstr(out, "start-up ordering broken") != NULL)
                report_failure("%s: attempt %d complained about the order its "
                               "own steps ran in: %s", how, attempt, out);
        }

        /* A load that finished says so, and the lockdown stopping does
           not take that back: the record is what a later attempt reads
           to know there is no language left to read in. */
        if (graphics && !flag_is_set(ctx, "GRAPHICS_LOADED"))
            report_failure("%s: after attempt %d the load that finished is "
                           "not recorded as done, so the lockdown stopping "
                           "sends it round again", how, attempt);

        no_window_was_left(ctx, how, "after a lockdown that stopped",
                           entry_depth);
    }

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

int main(void)
{
    /* raster keeps its pixels outside virtual memory and is refused by
       the C that allocates them; pgm keeps its raster in virtual memory
       and is refused in PostScript, without the interpreter's own error
       handler seeing it. Both are built into every configuration. */
    static const char *const devices[] = { "raster", "pgm" };
    size_t i;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    for (i = 0; i < sizeof devices / sizeof devices[0]; i++)
    {
        an_ordinary_page(devices[i]);
        a_page_it_cannot_write(devices[i]);
        a_page_it_cannot_provide(devices[i]);
    }

    a_lockdown_that_stops(1, 0);
    a_lockdown_that_stops(0, 0);
    a_lockdown_that_stops(1, 1);
    a_lockdown_that_stops(0, 1);

    xpost_quit();

    return verdict();
}
