/*
 * The key a wrapped call's saved-operand array is kept under, in every
 * context a process brings up.
 *
 * A PostScript-implemented operator copies the operands its call runs
 * on into an array held in the interpreter's private dictionary under
 * .wrapsave. The key is the language's: a program reads the array by
 * that name, and the test that holds the copy to its bounds --
 * wrapped_save -- reaches it by that name to set the mark the clear
 * works from.
 *
 * A name is an index into the name stack of the context it was interned
 * in, and means nothing in another context: the same index there names
 * whatever that context's own boot happened to intern at it, or nothing
 * at all. So the name has to be interned in the context whose array it
 * names, once per context rather than once per process.
 *
 * That is what this measures, and it takes a process with more than one
 * context to measure it: the first context of a process is right either
 * way. Contexts are created and destroyed in turn, and each has the
 * entry dropped and then makes one wrapped call, which rebuilds it. Two
 * things are required of what comes back. It is under .wrapsave, which
 * is what says the rebuild used the name of the context it ran in. And
 * it is the one entry the call added, which is what says the rebuild did
 * not go somewhere else and leave privatedict a key the language does
 * not name -- a key that, on a later intern, comes to alias whatever
 * real name lands at its index.
 *
 * The drop is made from here rather than by the probe because
 * privatedict is sealed: what the machinery reaches by name there is
 * what the interpreter put there, so no program removes an entry, and
 * this stands where the interpreter does.
 *
 * The wrapped call is .gscratch, a wrapped operator the language always
 * has: its procedure takes no operands and yields the local scratch
 * dictionary, so the call is a call and nothing else. A call copies the
 * operands standing when it is made and copies nothing where there are
 * none, so the probe leaves one standing for it to copy.
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
#include "xpost_dict.h"
#include "xpost_name.h"

#include "xpost_test.h"

/* Three rather than two: the second context is where a name held over
   from the first is first read, and the third is where one held over
   from the second would be. */
#define CONTEXTS 3

/* Drop the entry, make one wrapped call to rebuild it, and report the
   two things required of what came back. */
/* one wrapped call and nothing else: .gscratch's procedure takes no
   operands and yields the local scratch dictionary */
static const char *const probe = "0 .gscratch pop pop";

XPOST_TEST_SINK(out, 256)


int main(void)
{
    int i;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    for (i = 0; i < CONTEXTS; i++)
    {
        Xpost_Context *ctx;
        Xpost_Run_Status st;
        Xpost_Object key;
        unsigned n0;

        ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                           XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                           XPOST_USE_SIZE, 100, 100);
        if (!ctx)
        {
            report_failure("context %d of %d was not created", i + 1, CONTEXTS);
            break;
        }
        xpost_job_snapshots_set(ctx, 0);
        xpost_stdout_handler_set(ctx, out_sink, NULL);

        /* one run to settle the context: the graphics modules load with
           the first page device a run asks for, and what they define in
           privatedict would otherwise be counted as the rebuild below */
        out_len = 0;
        if (xpost_run(ctx, XPOST_INPUT_STRING, probe, 0) != XPOST_RUN_COMPLETE)
        {
            report_failure("context %d of %d did not settle", i + 1, CONTEXTS);
            xpost_stdout_handler_set(ctx, NULL, NULL);
            xpost_destroy(ctx);
            continue;
        }

        /* drop the entry this context's boot made, so that the call
           below is the one that rebuilds it, and take the count the
           rebuild is measured against with the entry gone */
        key = xpost_name_cons(ctx, ".wrapsave");
        (void)xpost_dict_undef(ctx, ctx->privatedict, key);
        n0 = xpost_dict_length_memory(ctx->lo, ctx->privatedict);

        out_len = 0;
        st = xpost_run(ctx, XPOST_INPUT_STRING, probe, 0);
        out_buf[out_len < sizeof out_buf ? out_len : sizeof out_buf - 1] = '\0';

        if (st != XPOST_RUN_COMPLETE)
            report_failure("context %d of %d did not run the probe to "
                           "completion", i + 1, CONTEXTS);
        else
        {
            Xpost_Object arr = xpost_dict_get(ctx, ctx->privatedict, key);
            unsigned n1 = xpost_dict_length_memory(ctx->lo, ctx->privatedict);

            if (xpost_object_get_type(arr) != arraytype)
                report_failure("context %d of %d rebuilt the saved-operand "
                               "array somewhere other than .wrapsave",
                               i + 1, CONTEXTS);
            else if (n1 != n0 + 1)
                report_failure("context %d of %d took privatedict from %u "
                               "entries to %u, where a rebuild under "
                               ".wrapsave and nowhere else takes it to %u",
                               i + 1, CONTEXTS, n0, n1, n0 + 1);
        }

        xpost_stdout_handler_set(ctx, NULL, NULL);
        xpost_destroy(ctx);
    }

    xpost_quit();

    return verdict();
}
