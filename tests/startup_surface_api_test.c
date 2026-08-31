/* The interpreter's surface as an embedding program finds it.
 *
 * startup_surface_test.ps states the same shape from a file run by the
 * command-line interpreter. The two do not arrive by the same road: the
 * command line runs one of the start procedures, which loads graphics
 * and locks the interpreter down before the file executes, while an
 * embedding caller gets a context from xpost_create and feeds it
 * fragments through xpost_run. Whether the shape survives that second
 * road is a separate question from whether it survives the first, and
 * the answer has to be the same either way -- a program embedding the
 * interpreter is no more entitled to reach the machinery than one run
 * from a file.
 *
 * The checks run inside the interpreter, where the dictionaries are, and
 * report a count this side reads back.
 */

#include <stdio.h>
#include <string.h>

#include "xpost.h"

#include "xpost_test.h"

XPOST_TEST_SINK(out, 4096)


/* run a fragment and report whether the interpreter printed "yes" */
static int answers_yes(Xpost_Context *ctx, const char *frag)
{
    char prog[2048];

    out_len = 0;
    out_buf[0] = '\0';
    snprintf(prog, sizeof prog, "%s { (yes) print }{ (no) print } ifelse", frag);
    if (xpost_run(ctx, XPOST_INPUT_STRING, prog, 0) != XPOST_RUN_COMPLETE)
        return 0;
    out_buf[out_len < sizeof out_buf ? out_len : sizeof out_buf - 1] = '\0';
    return strstr(out_buf, "yes") != NULL && strstr(out_buf, "no") == NULL;
}

int main(void)
{
    Xpost_Context *ctx;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        xpost_quit();
        return verdict();
    }
    xpost_job_snapshots_set(ctx, 0);
    xpost_stdout_handler_set(ctx, out_sink, NULL);

    /* --- the standard operators, and the dictionary holding them --- */
    check(answers_yes(ctx, "systemdict gcheck"),
          "systemdict is in global VM through the API");
    check(answers_yes(ctx, "systemdict wcheck not"),
          "systemdict is read-only through the API");
    check(answers_yes(ctx,
              "{ systemdict /api.intruder 1 put } stopped "
              "{ $error /errorname get /invalidaccess eq }{ false } ifelse"),
          "systemdict refuses an embedding program's definition");
    check(answers_yes(ctx,
              "/n 0 def [ /add /get /stroke /show /findfont /filter /run ] "
              "{ dup systemdict exch known not { /n n 1 add store } if "
              "load type /operatortype ne { /n n 1 add store } if } forall "
              "n 0 eq"),
          "the standard operators are operators in systemdict");

    /* --- the private operators --- */
    check(answers_yes(ctx, "1183615869 internaldict wcheck not"),
          "internaldict is read-only through the API");
    check(answers_yes(ctx, "1183615869 internaldict gcheck"),
          "internaldict is in global VM through the API");
    check(answers_yes(ctx,
              "{ 42 internaldict } stopped "
              "{ $error /errorname get /invalidaccess eq }{ false } ifelse"),
          "the accessor refuses the wrong password through the API");
    check(answers_yes(ctx,
              "/n 0 def 1183615869 internaldict { pop dup type /nametype eq "
              "{ dup systemdict exch known { /n n 1 add store } if } if pop } forall "
              "n 0 eq"),
          "no private operator is visible in systemdict through the API");

    /* --- the machinery --- */
    check(answers_yes(ctx, ".privatedict gcheck not"),
          "privatedict is in local VM through the API");
    check(answers_yes(ctx,
              "/n 0 def .privatedict { pop dup type /nametype eq "
              "{ dup systemdict exch known { /n n 1 add store } if "
              "dup userdict exch known { /n n 1 add store } if } if pop } forall "
              "n 0 eq"),
          "no machinery member is visible to an embedding program");
    check(answers_yes(ctx, "/.xpostsys where not"),
          "the helper namespace cannot be named through the API");
    check(answers_yes(ctx, "/.internaldict where not"),
          "internaldict cannot be named directly through the API");

    /* --- the standard dictionaries --- */
    check(answers_yes(ctx,
              "/n 0 def [ /userdict /errordict /$error /statusdict /FontDirectory ] "
              "{ systemdict exch get gcheck { /n n 1 add store } if } forall n 0 eq"),
          "the standard local dictionaries are local through the API");
    check(answers_yes(ctx,
              "/n 0 def [ /systemdict /globaldict /GlobalFontDirectory ] "
              "{ systemdict exch get gcheck not { /n n 1 add store } if } forall n 0 eq"),
          "the standard global dictionaries are global through the API");

    /* --- the wrapped procedures --- */
    check(answers_yes(ctx,
              "/n 0 def .privatedict /.wrappedprocs get "
              "{ pop gcheck not { /n n 1 add store } if } forall "
              "n 0 eq .privatedict /.wrappedprocs get length 0 gt and"),
          "every wrapped procedure is in global VM through the API");

    /* --- the machinery resists an embedding program taking names --- */
    check(answers_yes(ctx,
              /* the verdict is a boolean on the stack: a variable set
                 inside the save level would be reverted by the restore,
                 taking the answer with it */
              "/sv save def "
              "/get { (stolen) } def /put { (stolen) } def "
              "/exch { (stolen) } def /type { (stolen) } def "
              "/forall { (stolen) } def /known { (stolen) } def "
              "{ gsave newpath 0 0 moveto 10 10 lineto 2 setlinewidth stroke grestore } "
              "stopped not "
              "sv restore"),
          "the machinery works after an embedding program takes six names");

    /* The context has been running fragments, so userdict holds what they
       defined; what matters is that nothing of the interpreter's was
       there to begin with. A context that has run nothing answers that,
       and the interpreter holds one instance at a time, so this one goes
       first. */
    xpost_stdout_handler_set(ctx, NULL, NULL);
    xpost_destroy(ctx);

    {
        Xpost_Context *fresh;
        fresh = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                             XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                             XPOST_USE_SIZE, 100, 100);
        if (fresh)
        {
            xpost_job_snapshots_set(fresh, 0);
            xpost_stdout_handler_set(fresh, out_sink, NULL);
            check(answers_yes(fresh, "userdict length 0 eq"),
                  "a fresh context starts with an empty userdict");
            check(answers_yes(fresh, "globaldict length 0 eq"),
                  "a fresh context starts with an empty globaldict");
            xpost_stdout_handler_set(fresh, NULL, NULL);
            xpost_destroy(fresh);
        }
        else
        {
            check(0, "a context is created once the previous one is destroyed");
        }
    }

    xpost_quit();

    return verdict();
}
