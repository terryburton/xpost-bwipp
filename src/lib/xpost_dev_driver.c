/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_dev_driver.c
 * @brief Loads a device by name and hands back its class dictionary.
 *
 * The one place a device is named. Everything above works in methods.
 */

/** \file xpost_dev_driver.c
   registering a device class: the part of the driver contract that runs
   once per device rather than once per mark
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_stack.h"
#include "xpost_context.h"
#include "xpost_error.h"
#include "xpost_dict.h"
#include "xpost_string.h"
#include "xpost_array.h"
#include "xpost_name.h"
#include "xpost_operator.h"
#include "xpost_dev_driver.h"

/* Build a method's operator with the arity its kind and the device's
   colour space give it. Returns an invalidtype object if the shape is
   not one this contract knows. */
static Xpost_Object
xpost_dev_method_cons(Xpost_Context *ctx,
                      const Xpost_Dev_Method *m,
                      int ncomp)
{
    /* numeric operands before the device dictionary, and results */
    int n = 0, poly = 0;

    switch (m->kind)
    {
        case XPOST_DEV_M_CREATE:
            return xpost_operator_cons(ctx, m->opname, m->func, 3,
                                       integertype, integertype, dicttype);
        case XPOST_DEV_M_PUTPIX: n = ncomp + 2; break;
        case XPOST_DEV_M_GETPIX: n = 2; break;
        case XPOST_DEV_M_LINE:   n = ncomp + 4; break;
        case XPOST_DEV_M_RECT:   n = ncomp + 4; break;
        case XPOST_DEV_M_BLEND:  n = ncomp + 3; break;
        case XPOST_DEV_M_POLY:   n = ncomp; poly = 1; break;
        case XPOST_DEV_M_BAND:   n = 2; break;
        case XPOST_DEV_M_PAGE:   n = 0; break;
    }

    if (poly)
    {
        switch (n)
        {
            case 1: return xpost_operator_cons(ctx, m->opname, m->func, 3,
                        numbertype, arraytype, dicttype);
            case 3: return xpost_operator_cons(ctx, m->opname, m->func, 5,
                        numbertype, numbertype, numbertype, arraytype, dicttype);
            case 4: return xpost_operator_cons(ctx, m->opname, m->func, 6,
                        numbertype, numbertype, numbertype, numbertype,
                        arraytype, dicttype);
        }
        return invalid;
    }

    switch (n)
    {
        case 0: return xpost_operator_cons(ctx, m->opname, m->func, 1,
                    dicttype);
        case 2: return xpost_operator_cons(ctx, m->opname, m->func, 3,
                    numbertype, numbertype, dicttype);
        case 3: return xpost_operator_cons(ctx, m->opname, m->func, 4,
                    numbertype, numbertype, numbertype, dicttype);
        case 4: return xpost_operator_cons(ctx, m->opname, m->func, 5,
                    numbertype, numbertype, numbertype, numbertype, dicttype);
        case 5: return xpost_operator_cons(ctx, m->opname, m->func, 6,
                    numbertype, numbertype, numbertype, numbertype,
                    numbertype, dicttype);
        case 6: return xpost_operator_cons(ctx, m->opname, m->func, 7,
                    numbertype, numbertype, numbertype, numbertype,
                    numbertype, numbertype, dicttype);
        case 7: return xpost_operator_cons(ctx, m->opname, m->func, 8,
                    numbertype, numbertype, numbertype, numbertype,
                    numbertype, numbertype, numbertype, dicttype);
        case 8: return xpost_operator_cons(ctx, m->opname, m->func, 9,
                    numbertype, numbertype, numbertype, numbertype,
                    numbertype, numbertype, numbertype, numbertype, dicttype);
    }
    return invalid;
}

XPOST_MUST_CHECK int
xpost_dev_class_publish(Xpost_Context *ctx,
                        const char *name,
                        Xpost_Object classdic)
{
    int ret;

    ret = xpost_dict_put_internal(ctx, ctx->privatedict,
                                  xpost_name_cons(ctx, name), classdic);
    if (ret)
        return ret;
    /* A class states what a device of its kind IS -- the methods the
       machinery runs on it and the entries it answers with -- and it is
       finished by the time it is published. The lockdown closes the
       classes it can see; a driver brought in on first use arrives after
       the lockdown has run, and there is no later sweep to reach it. So
       the closing happens here, where every class passes whether it was
       built before the lockdown or after it. A class left open is a
       table a program takes out of the private dictionary, puts its own
       procedure into, and has the machinery run at showpage. */
    /* The same setting that holds the lockdown's classes open holds
       these open, and for the same reason: a run instrumenting the
       devices reaches into whichever class it was given, and a fleet
       half closed and half open would answer differently by device. */
    if (!getenv("XPOST_UNSEALED_DEVICES"))
        (void) xpost_object_set_access(ctx, classdic,
                                       XPOST_OBJECT_TAG_ACCESS_READ_ONLY);
    return 0;
}

int
xpost_dev_class_install(Xpost_Context *ctx,
                        Xpost_Object classdic,
                        int ncomp,
                        int raster_is_compiled,
                        const Xpost_Dev_Method *methods,
                        int nmethods)
{
    static const char *mandatory[] = XPOST_DEV_MANDATORY_SLOTS;
    static const char *raster[] = XPOST_DEV_RASTER_SLOTS;
    int i, j, suite = 0;

    /* Whether this table states the device's suite, which is the table
       that brings Create: a class is complete once one has been
       installed, and a later table adds a method to a class that
       already is -- so what a later one holds is not the whole of what
       the device offers, and the completeness below is not its
       question. */
    for (i = 0; i < nmethods; i++)
        if (!strcmp(methods[i].slot, "Create"))
            suite = 1;

    for (i = 0; i < nmethods; i++)
    {
        Xpost_Object op = xpost_dev_method_cons(ctx, &methods[i], ncomp);
        int ret;

        if (xpost_object_get_type(op) == invalidtype)
        {
            XPOST_LOG_ERR("device method %s has no shape in the driver contract",
                          methods[i].slot);
            return unregistered;
        }
        ret = xpost_dict_put(ctx, classdic,
                             xpost_name_cons(ctx, methods[i].slot), op);
        if (ret)
            return ret;
    }

    for (i = 0; i < (int)(sizeof(mandatory) / sizeof(*mandatory)); i++)
    {
        Xpost_Object v = xpost_dict_get(ctx, classdic,
                                        xpost_name_cons(ctx, mandatory[i]));

        if (xpost_object_get_type(v) == invalidtype ||
            xpost_object_get_type(v) == nulltype)
        {
            XPOST_LOG_ERR("device class has no %s", mandatory[i]);
            return unregistered;
        }
    }

    /* A device with a raster of its own brings every slot that would
       read the base class's. What it brings is what its table names:
       the value the class holds does not say, because a base class
       fills a slot with a compiled operator as readily as with a body
       of its own, and an inherited operator is an operator. */
    if (raster_is_compiled && suite)
        for (i = 0; i < (int)(sizeof(raster) / sizeof(*raster)); i++)
        {
            Xpost_Object v;

            for (j = 0; j < nmethods; j++)
                if (!strcmp(methods[j].slot, raster[i]))
                    break;
            if (j == nmethods)
            {
                XPOST_LOG_ERR("device keeps its own raster but inherits %s,"
                              " which reads the base class's", raster[i]);
                return unregistered;
            }

            v = xpost_dict_get(ctx, classdic,
                               xpost_name_cons(ctx, raster[i]));
            if (xpost_object_get_type(v) != operatortype)
            {
                XPOST_LOG_ERR("device keeps its own raster but inherits %s,"
                              " which reads the base class's", raster[i]);
                return unregistered;
            }
        }

    return 0;
}
