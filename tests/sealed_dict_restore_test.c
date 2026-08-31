/* What a rewind does to a dictionary that was sealed before it.
 *
 * A dictionary's access attribute is a property of its value rather than
 * of the object naming it (PLRM 3.3.2), so every reference to a sealed
 * dictionary is sealed at once; xpost keeps it where the value is, in
 * the dictionary's head, inside the entity a save level copies. A
 * restore resets the values of composite objects to their state at the
 * time of the save (PLRM 3.7.3), and access is part of that state.
 *
 * Which makes when the copy is taken the whole question. A save level
 * copies an entity at the first write it sees, and that copy stands for
 * the entity as it was when the level was taken -- true only if nothing
 * writes the entity without taking the copy first. The access field is
 * written by the access operators, so those are among the writes that
 * have to take it, and a copy taken after one of them holds the access
 * the dictionary had at that moment rather than the access it had at the
 * save.
 *
 * The three dictionaries this is held over are the ones the interpreter
 * seals when the language finishes loading: systemdict, and the two
 * namespaces the machinery is kept private in. Sealing them is what
 * stops a program redefining the language, and the seal is applied once,
 * before any program runs. A save bracket is the whole of an ordinary
 * run, so a rewind that handed any of them back writable would hand a
 * program the language to redefine -- and the program would not have had
 * to do anything unusual to get it, only to run to the end.
 *
 * So the runs below are ordinary. Switching the allocation mode is the
 * one that reaches systemdict, because the name of the font directory is
 * rebound there as the mode changes and the rebinding opens systemdict
 * for as long as it takes; a program bracketing that with its own save
 * and restore, or leaving an error to be reported in the middle of it,
 * are the same run with the bracket and the error handler added.
 *
 * Held at the end of the run rather than inside it, because the bracket
 * that matters is the one the run itself takes over the job: the last
 * thing a run does is rewind to the level it started from.
 *
 * The last part puts the mechanism itself rather than the consequence: a
 * level taken over a dictionary, its access changed under that level,
 * the level rewound, and the access asked for. That one names no
 * particular dictionary, so what it holds is the rule and not the three
 * cases.
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
#include "xpost_save.h"

#include "xpost_test.h"

XPOST_TEST_SINK(out, 4096)


/* systemdict is the bottom of the dictionary stack, and the global
   private namespace is reached from the context, which is the only way
   left once the language has dropped its userdict anchor. */
static Xpost_Object sealed_dict(Xpost_Context *ctx, int which)
{
    if (which == 0)
        return xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    return ctx->globalprivatedict;
}

static const char *sealed_name(int which)
{
    return which == 0 ? "systemdict" : "the global private namespace";
}

/* The local private namespace has dropped its anchor too and is reached
   by the number the language answers it to, so it is asked in PostScript
   -- in a run of its own, after the run under test has rewound. */
static void internaldict_is_not_writable(Xpost_Context *ctx, const char *when)
{
    out_len = 0;
    (void) xpost_run(ctx, XPOST_INPUT_STRING,
                     "1183615869 internaldict wcheck "
                     "{ (writable) print }{ (sealed) print } ifelse "
                     "flush quit", 0);
    out_buf[out_len] = '\0';
    if (strcmp(out_buf, "sealed") != 0)
        report_failure("the local private namespace answered \"%s\" %s",
                       out_buf, when);
}

/* None of the three may be writable, whatever the run did. */
static void none_are_writable(Xpost_Context *ctx, const char *when)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        Xpost_Object d = sealed_dict(ctx, i);

        if (xpost_object_get_type(d) != dicttype)
        {
            report_failure("%s is not a dictionary %s", sealed_name(i), when);
            continue;
        }
        if (xpost_object_is_writeable(ctx, d))
            report_failure("%s is writable %s", sealed_name(i), when);
    }
    internaldict_is_not_writable(ctx, when);
}

static void a_run_leaves_the_seals_on(const char *prog, const char *what)
{
    Xpost_Context *ctx;

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        return;
    }
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    out_len = 0;
    (void) xpost_run(ctx, XPOST_INPUT_STRING, prog, 0);

    none_are_writable(ctx, what);

    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* The seals are on before any program runs: a run that found them off
   would say nothing about what the rewind did to them. */
static void the_seals_are_on_to_begin_with(void)
{
    Xpost_Context *ctx;

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        return;
    }
    xpost_stdout_handler_set(ctx, out_sink, NULL);
    out_len = 0;
    /* the language loads on the first run, so the seals are asked after
       one that does nothing */
    (void) xpost_run(ctx, XPOST_INPUT_STRING, "quit", 0);
    none_are_writable(ctx, "before any program has run");
    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);
}

/* The rule itself, put to a dictionary of the test's own: what a rewind
   gives back is the access the dictionary had when the level was taken.
   Asked in both directions -- a dictionary sealed under the level comes
   back open, one sealed before it stays sealed -- because a rewind that
   did nothing at all would pass the first half of that on its own. */
static void a_rewind_gives_back_the_access_at_the_level(void)
{
    Xpost_Context *ctx;
    Xpost_Object d;

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        return;
    }
    xpost_stdout_handler_set(ctx, out_sink, NULL);
    out_len = 0;
    (void) xpost_run(ctx, XPOST_INPUT_STRING, "quit", 0);

    /* sealed under the level: the level was taken over a writable
       dictionary, so that is what it gives back */
    d = xpost_dict_cons(ctx, 4);
    if (xpost_object_get_type(d) != dicttype)
    {
        report_failure("cannot make a dictionary to hold the rule over");
        goto done;
    }
    if (xpost_object_get_type(xpost_save_create_snapshot_object(ctx->lo))
        != savetype)
    {
        report_failure("cannot take a save level to hold the rule over");
        goto done;
    }
    (void) xpost_object_set_access(ctx, d, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
    check(!xpost_object_is_writeable(ctx, d),
          "a dictionary sealed under a save level is sealed while it stands");
    xpost_save_restore_snapshot(ctx->lo);
    check(xpost_object_is_writeable(ctx, d),
          "a rewind gives back the access the dictionary had at the level");

    /* sealed before the level: nothing under the level changed it, so
       the rewind has nothing to give back */
    (void) xpost_object_set_access(ctx, d, XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
    if (xpost_object_get_type(xpost_save_create_snapshot_object(ctx->lo))
        != savetype)
    {
        report_failure("cannot take a second save level");
        goto done;
    }
    xpost_save_restore_snapshot(ctx->lo);
    check(!xpost_object_is_writeable(ctx, d),
          "a rewind leaves a seal applied before the level alone");

done:
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

    the_seals_are_on_to_begin_with();

    a_run_leaves_the_seals_on("quit",
                              "after a run that did nothing");
    /* the allocation mode carries the font directory's name with it, and
       rebinding that name is a write to systemdict */
    a_run_leaves_the_seals_on("true setglobal false setglobal quit",
                              "after a run that switched allocation mode");
    a_run_leaves_the_seals_on("save true setglobal false setglobal restore quit",
                              "after a run that bracketed the switch in a save");
    /* the mode is switched back while an error is being reported, which
       is the one place the rebinding is reached with a program's own
       save level standing over it */
    a_run_leaves_the_seals_on("save true setglobal /nosuchname load restore quit",
                              "after a run that raised an error in global mode");
    a_run_leaves_the_seals_on("countdictstack array dictstack pop "
                              "true setglobal false setglobal quit",
                              "after a run that read the dictionary stack");

    a_rewind_gives_back_the_access_at_the_level();

    xpost_quit();

    return verdict();
}
