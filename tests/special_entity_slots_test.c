/* That each special entity sits on the slot its enumerator names.
 *
 * The specials are reached by number, never searched for. Every accessor
 * in the memory header subscripts the table with an enumerator, and the
 * band the collector owns begins one past the last of them -- so a
 * special that landed elsewhere is an accessor handing back another
 * entity's storage and a collection whose domain opens on a root.
 * Nothing arranges the numbering except the order the constructors run
 * in, which is to say nothing arranges it at all.
 *
 * The constructors refuse a slot they did not get, so the shape of the
 * failure is a context that cannot be built. That is checked here too:
 * without it this file would pass by never reaching its assertions,
 * which is the way a test about initialisation usually goes quiet.
 *
 * WHY THIS IS SEPARATE FROM THE REST OF THE SUITE. The fault was found
 * once, by an entity allocated out of turn during initialisation. Three
 * tests went red and two of the three were reading the warning text the
 * constructor printed rather than the state -- silence the line, leave
 * the fault, and they pass. The third saw it, as a gap in a reachability
 * walk reported against an entity number. None of them said what was
 * wrong. This one fails alone and names the slot.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_interpreter.h"

#include "xpost_test.h"

/* The band the collector owns opens one past the last special the bank
   holds. Derived from the enumerators, so this is the arithmetic the
   header states rather than a number copied out of it. */
static void _check_band(Xpost_Memory_File *mem, const char *bank,
                        unsigned int want)
{
    if (mem->start != want)
        report_failure("the collector's band in %s virtual memory opens at "
                       "%u, and the specials end at %u. A band opening below "
                       "the last of them puts a root inside a collection; "
                       "above it leaves an entity nothing ever sweeps",
                       bank, mem->start, want);
}

/* Every slot below the band must be one the table actually handed out,
   or the accessors for them are subscripting rows nothing filled. */
static void _check_reached(Xpost_Memory_File *mem, const char *bank,
                           unsigned int last)
{
    if (mem->table.nextent <= last)
        report_failure("%s virtual memory reached entity %u, which does not "
                       "cover the specials up to %u: an accessor for one of "
                       "them subscripts a row the table never filled",
                       bank, mem->table.nextent, last);
}

int main(void)
{
    Xpost_Context *ctx;

    if (!xpost_init())
    {
        report_failure("the library would not start");
        return verdict();
    }

    ctx = xpost_create("pgm", XPOST_OUTPUT_FILENAME, "/dev/null",
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 612, 792);
    if (!ctx)
    {
        /* This is what a special landing on the wrong slot looks like from
           outside: the constructor refused it and no interpreter was
           built. Reported as the same finding, since the alternative --
           returning quietly because there was nothing to assert against --
           is a test that goes green exactly when the fault is present. */
        report_failure("no context could be built, which is what a special "
                       "entity landing on a slot other than its own is "
                       "refused as. Run the interpreter to see which slot");
        xpost_quit();
        return verdict();
    }

    _check_band(ctx->gl, "global", XPOST_MEMORY_COLLECT_START_GLOBAL);
    _check_band(ctx->lo, "local", XPOST_MEMORY_COLLECT_START_LOCAL);
    _check_reached(ctx->gl, "global",
                   XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME);
    _check_reached(ctx->lo, "local", XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME);

    /* The bogus name is the one special that is not allocated through the
       table allocator -- it is the first string pushed on the name stack --
       so its slot is the one no shared check covers. */
    {
        Xpost_Object o;
        unsigned int nstk = xpost_memory_name_stack_ent(ctx->gl);

        o = xpost_stack_bottomup_fetch(ctx->gl, nstk, 0);
        if (xpost_object_get_ent(o) != XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME)
            report_failure("the name no program can spell is entity %u and "
                           "its slot is %u. It is pushed on the name stack "
                           "rather than allocated with the others, so an "
                           "entity taken out of turn before it moves it "
                           "alone",
                           xpost_object_get_ent(o),
                           (unsigned int)XPOST_MEMORY_TABLE_SPECIAL_BOGUS_NAME);
    }

    xpost_destroy(ctx);
    xpost_quit();
    return verdict();
}
