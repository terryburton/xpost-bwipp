/* What registration does with an operator that was never made.
 *
 * Registering an operator is two steps: xpost_operator_cons makes it, and
 * INSTALL puts it in systemdict under its own name. INSTALL reads that
 * name out of the operator table, indexing it by the number the operator
 * object carries.
 *
 * An operator object is not what cons answers when it has to refuse, and
 * it has five ways to: a name it could not intern, an operator table with
 * no room, and three allocations -- the signature block, the signature
 * table, the type block -- any of which the memory file can decline. One
 * of those answers invalid and the other four null, and neither carries
 * an operator number. What they carry is zero, because both are file
 * scope objects whose unwritten members are zero-initialised, so INSTALL
 * reads entry zero of the table. Entry zero is the first operator ever
 * registered, which is pop.
 *
 * So a refused registration does not leave its own operator out. It
 * defines pop -- an operator the interpreter reaches for and every
 * program uses -- as the thing cons refused to make. The store itself
 * succeeds, because pop is already in systemdict and replacing an entry
 * allocates nothing, so the record INSTALL keeps of a store that would
 * not go in stays empty and the interpreter comes up. What it comes up
 * with is a systemdict whose pop is not an operator, and the first
 * program to reach for it fails somewhere else entirely.
 *
 * The registration under test is the one save, restore, setglobal,
 * currentglobal and gcheck arrive by, run a second time over a context
 * that already holds them, with global VM declining every allocation. It
 * is run for its refusals, not its registrations: what the operators are
 * is beside the point, only that cons cannot make them.
 *
 * systemdict is made writable here because it is writable while the
 * operators are registered, and sealed once they are all in. A test over
 * a sealed one would be a test of the seal: the store would be refused
 * for its access rather than reaching pop at all, and would answer the
 * same before and after the defect it is looking for.
 *
 * Both the refusal and a sound registration are asked for. A registration
 * that always refused would satisfy the first alone; the second is what
 * says the same call still installs its operators, and leaves pop alone,
 * when nothing declines.
 *
 * The refusal is induced by putting the memory file at the far end of the
 * address range its offsets are unsigned ints of, which is where its
 * growth is declined. Nothing is written on that path, so the two fields
 * are the whole of the change and putting them back is the whole of
 * undoing it.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "xpost.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include <string.h>

#include "xpost_dict.h"
#include "xpost_name.h"
#include "xpost_operator.h"
#include "xpost_op_save.h"
#include "xpost_error.h"

#include "xpost_test.h"

static unsigned int held_used;
static unsigned int held_max;
static unsigned int held_cursor;

/* Decline every allocation in mem until released.
 *
 * Two things are moved, because an operator's signatures are not taken
 * off the memory file at all any more: the operator table is host
 * storage, and a run of signatures is cut from it. So the file's own
 * exhaustion no longer reaches the installer, and what is driven instead
 * is the table's own refusal -- its cursor is put where the next run
 * cannot be addressed, which is the one refusal the table can give that
 * does not depend on the host allocator failing on demand.
 *
 * The file is still filled, so that anything else the install path asks
 * of the arena is refused as it was. */
static void refuse_allocation(Xpost_Memory_File *mem)
{
    unsigned char *cursor = mem->optab + MAXOPS * sizeof(Xpost_Operator);

    held_used = mem->high_water;
    held_max = mem->max;
    memcpy(&held_cursor, cursor, sizeof held_cursor);
    {
        unsigned int full = 0xfffffff8u;
        memcpy(cursor, &full, sizeof full);
    }
    mem->high_water = 0xfffffff8u;
    mem->max = 0xfffffff8u;
}

static void allow_allocation(Xpost_Memory_File *mem)
{
    unsigned char *cursor = mem->optab + MAXOPS * sizeof(Xpost_Operator);

    mem->high_water = held_used;
    mem->max = held_max;
    memcpy(cursor, &held_cursor, sizeof held_cursor);
}

static void register_ops(int refused)
{
    Xpost_Context *ctx;
    Xpost_Object sd;
    Xpost_Object popped;
    Xpost_Object_Tag_Access was;

    ctx = xpost_create("null", XPOST_OUTPUT_DEFAULT, NULL,
                       XPOST_SHOWPAGE_NOPAUSE, XPOST_OUTPUT_MESSAGE_QUIET,
                       XPOST_USE_SIZE, 100, 100);
    if (!ctx)
    {
        report_failure("xpost_create");
        return;
    }

    /* systemdict is the bottom of the dictionary stack */
    sd = xpost_stack_bottomup_fetch(ctx->lo, ctx->ds, 0);
    if (xpost_object_get_type(sd) != dicttype)
    {
        report_failure("systemdict is not a dictionary");
        xpost_destroy(ctx);
        return;
    }

    popped = xpost_dict_get(ctx, sd, xpost_name_cons(ctx, "pop"));
    if (xpost_object_get_type(popped) != operatortype)
    {
        report_failure("pop is not an operator before registering");
        xpost_destroy(ctx);
        return;
    }

    /* as systemdict stands while the operators are registered */
    was = xpost_object_get_access(ctx, sd);
    xpost_object_set_access(ctx, sd, XPOST_OBJECT_TAG_ACCESS_UNLIMITED);

    if (refused)
        refuse_allocation(ctx->gl);
    xpost_oper_init_save_ops(ctx, sd);
    if (refused)
        allow_allocation(ctx->gl);

    xpost_object_set_access(ctx, sd, was);

    popped = xpost_dict_get(ctx, sd, xpost_name_cons(ctx, "pop"));
    if (xpost_object_get_type(popped) != operatortype)
        report_failure("pop is no longer an operator after a registration "
                       "that %s", refused ? "was refused" : "succeeded");

    if (refused)
    {
        if (!ctx->operator_install_refused)
            report_failure("a registration that could not make its operator "
                           "was not recorded as refused");
    }
    else
    {
        if (ctx->operator_install_refused)
            report_failure("a sound registration was recorded as refused");
    }

    xpost_destroy(ctx);
}

int main(void)
{
    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }

    register_ops(0);
    register_ops(1);

    xpost_quit();

    return verdict();
}
