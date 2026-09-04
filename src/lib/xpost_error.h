/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2026 Terry Burton
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_error.h
 * @brief This file provides the Xpost error functions.
 *
 * This header provides the Xpost error functions.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_ERROR_H
#define XPOST_ERROR_H

#include "xpost.h"  /* XPAPI, for the array declared below */

/*
   X-Macro utilities
   For a commentary on these macros, see my answer to this SO question
http://stackoverflow.com/questions/6635851/real-world-use-of-x-macros/6636596#6636596
*/

#define AS_BARE(a) a ,
/* #define AS_STR(a) #a , /\* defined in ob.h *\/ */

/**
 * @brief Macro to generate error identifiers.
 * These error codes are (mostly) defined in the PLRM and can be returned by operator
 * functions and handled at the postscript level. If an operator (including a device
 * function) returns a value outside of this range, the error-name returned to postscript
 * will be /unknownerror.
 * In some circumstances, /unregistered is used also used for this purpose,
 * since it has no documented use in the PLRM.
 * This gives you /unknownerror as a (mostly) freely-available error code for any
 * device-specific testing or such. So `return -1;` in a device function will send this
 * code back to the ps error-handler.
 *
 * An operator function may fail to execute if the operator_exec function cannot match
 * the type signature against the operand stack. It will then return a /typecheck or
 * /stackunderflow error to postscript.
 *
 * contextswitch, ioblock and collectretry represent requests to the interpreter to
 * change the state of the execution-context. They cannot be caught by postscript
 * error code.
 *
 * yieldtocaller is used to implement the Showpage-Return semantic where xpost
 * returns from xpost_run() after rendering the buffer.
 *
 * collectretry says the operator has done nothing and wants running again once a
 * collection has been taken. It is answered where a host resource an unreachable
 * object still holds has run out -- the descriptor behind a file object nothing
 * names -- because the collector is asked by entities and bytes and knows nothing
 * of such a resource, so the reclaim that would give it back is never due. The
 * interpreter puts the operator and its operands back the way a blocked read is
 * put back, and its safe point takes the collection before the operator runs
 * again. One retry is offered per refusal, so an operator whose second attempt is
 * refused as well reports the refusal.
 */
#define ERRORS(_) \
    _(noerror)            /*0*/\
    _(unregistered)            \
    _(dictfull)                \
    _(dictstackoverflow)       \
    _(dictstackunderflow)      \
    _(execstackoverflow)  /*5*/\
    _(execstackunderflow)      \
    _(handleerror)             \
    _(interrupt)               \
    _(invalidaccess)           \
    _(invalidexit)       /*10*/\
    _(invalidfileaccess)       \
    _(invalidfont)             \
    _(invalidrestore)          \
    _(ioerror)                 \
    _(limitcheck)        /*15*/\
    _(nocurrentpoint)          \
    _(rangecheck)              \
    _(stackoverflow)           \
    _(stackunderflow)          \
    _(syntaxerror)       /*20*/\
    _(timeout)                 \
    _(typecheck)               \
    _(undefined)               \
    _(undefinedfilename)       \
    _(undefinedresult)   /*25*/\
    _(unmatchedmark)           \
    _(VMerror)                 \
    _(invalidcontext)          \
    _(contextswitch)           \
    _(ioblock)                 \
    _(collectretry)     /* 31*/\
    _(yieldtocaller)    /* 32*/\
    _(unknownerror)     /* 33 nb. unknownerror is the catch-all and must be last */ \
/* #enddef ERRORS */

/**
 * @brief Error codes for operator return.
 */
enum err { ERRORS(AS_BARE) };

/**
 * @brief Printable string representations of Error codes.
 */
/* Data crossing a library boundary needs the same decoration its
   functions do: a consumer on Windows reaches an array in a DLL through
   an import reference, and one declared without it has no symbol the
   linker can resolve. The header carrying that decoration is included
   above, so every translation unit reading this one sees the same
   declaration whatever else it includes. */
XPAPI extern const char *errorname[] /*= { ERRORS(AS_STR) }*/;
/* puts(errorname[(enum err)limitcheck]); */

/**
 * @}
 */

#endif
