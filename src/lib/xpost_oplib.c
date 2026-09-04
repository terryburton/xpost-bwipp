/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_oplib.c
 * @brief Asks each operator module in turn to install what it owns.
 *
 * The one place the modules are listed, and so the one place that decides
 * the order the operator table comes out in.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif


#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_name.h"
#include "xpost_array.h"
#include "xpost_dict.h"

#include "xpost_operator.h"
#include "xpost_oplib.h"

#include "xpost_op_stack.h"
#include "xpost_op_string.h"
#include "xpost_op_array.h"
#include "xpost_op_dict.h"
#include "xpost_op_boolean.h"
#include "xpost_op_control.h"
#include "xpost_op_type.h"
#include "xpost_op_token.h"
#include "xpost_op_math.h"
#include "xpost_op_file.h"
#include "xpost_op_save.h"
#include "xpost_op_misc.h"
#include "xpost_op_packedarray.h"
#include "xpost_op_param.h"
#include "xpost_op_matrix.h"
#include "xpost_op_path.h"
#include "xpost_op_font.h"
#include "xpost_op_context.h"
#include "xpost_dev_generic.h"
#ifdef _WIN32
# include "xpost_dev_win32.h"
#endif
#ifdef HAVE_XCB
# include "xpost_dev_xcb.h"
#endif
#include "xpost_dev_bgr.h"
#include "xpost_dev_raster.h"
#include "xpost_dev_record.h"
#ifdef HAVE_LIBPNG
# include "xpost_dev_png.h"
#endif
#ifdef HAVE_LIBJPEG
# include "xpost_dev_jpeg.h"
#endif

/* no-op operator useful as a break target.
   put 'breakhere' in the postscript program,
   run interpreter under gdb,
   gdb> b xpost_op_breakhere
   gdb> run
   will break in the breakhere function (of course),
   which you can follow back to the main loop (gdb> next),
   just as it's about to read the next token.
*/
int xpost_op_breakhere(Xpost_Context *ctx)
{
    (void)ctx;
    return 0;
}

/* create systemdict and call
   all initop?* functions, installing all operators */
int xpost_oplib_init_ops(Xpost_Context *ctx)
{
    Xpost_Object op;
    Xpost_Object n;
    Xpost_Object sd;
    Xpost_Memory_Table *tab;
    unsigned ent;
    Xpost_Operator *optab;

    /* Mark every reference-table entry uncaptured before any module
       registers. Zero cannot serve as the marker: it is a valid opcode --
       the first operator registered holds it -- so an entry left zero
       would be indistinguishable from a genuine capture of that operator.
       An unset entry must also never match a real opcode where the
       procedure walker recognises one, which -1 satisfies. */
#define XPOST_OP_REF_UNSET(ref, refname) XPOST_OP_CODE(ctx, ref) = -1;
    XPOST_OP_REFS(XPOST_OP_REF_UNSET)
#undef XPOST_OP_REF_UNSET

    sd = xpost_dict_cons (ctx, SDSIZE);
    if (xpost_object_get_type(sd) == nulltype)
    {
        XPOST_LOG_ERR("cannot allocate systemdict");
        return 0;
    }
    if (xpost_dict_put(ctx, sd, xpost_name_cons(ctx, "systemdict"), sd))
    {
        XPOST_LOG_ERR("cannot name systemdict in itself");
        return 0;
    }
    xpost_stack_push(ctx->lo, ctx->ds, sd); // push systemdict on dictstack
    ent = xpost_object_get_ent(sd);
    tab = &ctx->gl->table;
    tab->tab[ent].sz = 0; // make systemdict immune to collection

#ifdef DEBUGOP
    xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
    puts("");
#endif

    if (xpost_oper_init_stack_ops(ctx, sd))

        return 0;

//#ifdef DEBUGOP
//#endif

    op = xpost_operator_cons(ctx, "breakhere", (Xpost_Op_Func)xpost_op_breakhere, 0);
    INSTALL;

    if (xpost_oper_init_string_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_array_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_dict_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_bool_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_control_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_type_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_token_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_math_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_file_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_save_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_misc_ops(ctx, sd))

        return 0;

    if (xpost_oper_init_packedarray_ops(ctx, sd))

        return 0;
    if (xpost_oper_init_param_ops(ctx, sd))
        return 0;
    if (xpost_oper_init_matrix_ops(ctx, sd))
        return 0;
    if (xpost_oper_init_path_ops(ctx, sd))
        return 0;
    if (xpost_oper_init_font_ops(ctx, sd))
        return 0;
    if (xpost_oper_init_generic_device_ops(ctx, sd))
        return 0;
#ifdef _WIN32
    if (xpost_oper_init_win32_device_ops(ctx, sd))
        return 0;
#endif
#ifdef HAVE_XCB
    if (xpost_oper_init_xcb_device_ops(ctx, sd))
        return 0;
#endif
    if (xpost_oper_init_bgr_device_ops(ctx, sd))
        return 0;
    if (xpost_oper_init_raster_device_ops(ctx, sd))
        return 0;
    if (xpost_oper_init_record_device_ops(ctx, sd))
        return 0;
#ifdef HAVE_LIBPNG
    if (xpost_oper_init_png_device_ops(ctx, sd))
        return 0;
#endif
#ifdef HAVE_LIBJPEG
    if (xpost_oper_init_jpeg_device_ops(ctx, sd))
        return 0;
#endif
    if (xpost_oper_init_context_ops(ctx, sd))
        return 0;

    /* Every module has registered by here. A registration that could
       not place its operator in systemdict left the interpreter without
       it, and there is no recovering from that. */
    if (ctx->operator_install_refused)
    {
        XPOST_LOG_ERR("an operator could not be installed in systemdict");
        return 0;
    }

    /* An entry still holding the uncaptured marker names an operator no
       module registered, so nothing the interpreter schedules through it
       would be an operator at all. */
    {
        int uncaptured = 0;
#define XPOST_OP_REF_CHECK(ref, refname) \
        if (XPOST_OP_CODE(ctx, ref) == -1) \
        { \
            XPOST_LOG_ERR("no operator named %s to reach for", refname); \
            uncaptured = 1; \
        }
        XPOST_OP_REFS(XPOST_OP_REF_CHECK)
#undef XPOST_OP_REF_CHECK
        if (uncaptured)
            return 0;
    }

#ifdef DEBUGOP
    printf("final sd:\n");
    xpost_stack_dump(ctx->lo, ctx->ds);
    xpost_dict_dump_memory (ctx->gl, sd); fflush(NULL);
#endif

    return 1;
}
