/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2015-2016 Michael Joshua Ryan
 * Copyright (c) 2015 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_PRIVATE_H
#define XPOST_PRIVATE_H

#include "xpost.h" /* XPAPI */

/* Marks a symbol that the unit tests reach past the public API to use.
   It carries the same linkage marks as the API, so a test links to it
   the same way: on a platform whose shared libraries name what they
   import, a reference the header did not mark is resolved by a runtime
   relocation of limited reach instead, and whether it reaches is a
   matter of where the two images happen to land. This says nothing
   about the function's contract -- for that, see the must-check mark
   defined below. */
#ifdef XPOST_TEST_VISIBLE
# undef XPOST_TEST_VISIBLE
#endif

#define XPOST_TEST_VISIBLE XPAPI

/* Marks a function whose return value carries a refusal the caller must
   act on. Discarding it turns the refusal into a silent no-op. */
#if defined(__GNUC__) || defined(__clang__)
# define XPOST_MUST_CHECK __attribute__((warn_unused_result))
#else
# define XPOST_MUST_CHECK
#endif

/* Marks a function that stays out of line at every call site. Link-time
   optimization inlines a small cross-unit function wherever it is
   called, and the growth that costs the unit is taken out of the budget
   the interpreter's own hot accessors are inlined from; a function
   called from every device method is over that line. */
#if defined(__GNUC__) || defined(__clang__)
# define XPOST_NOINLINE __attribute__((noinline))
#else
# define XPOST_NOINLINE
#endif

/* Marks a function that takes a printf format string and the arguments
   it names, so that the compiler checks the two against each other. The
   numbers are the positions of the format parameter and of the first
   argument it consumes, counting the whole parameter list from one: a
   function that carries a destination ahead of the format numbers them
   2 and 3, not 1 and 2. The pair names parameters rather than variable
   arguments, so a mark that names the wrong ones checks the wrong
   parameter and says so with the same confidence as a right one.

   A compiler that does not know the attribute drops the checking, not
   the declaration. */
#if defined(__GNUC__) || defined(__clang__)
# define XPOST_PRINTF(fmt_arg, first_arg) \
    __attribute__((format(printf, fmt_arg, first_arg)))
#else
# define XPOST_PRINTF(fmt_arg, first_arg)
#endif

/* Consumes the answer of an XPOST_MUST_CHECK function at a site where
   the refusal it reports cannot arise -- an index the line above just
   read from, a resource the caller has already established. Naming the
   claim is the point: it is a statement that can be read and checked,
   where a bare cast would say nothing, and it greps. Every use carries
   a comment saying why the refusal cannot happen there. */
#define XPOST_REFUSAL_IMPOSSIBLE(call) do { if (call) { } } while (0)

/**
 * @brief Initialize the log module.
 *
 * @return 1 on success, 0 otherwise.
 *
 * This function initializes the log module. Currently, it only gets
 * the value of the environment variable XPOST_LOG_LEVEL if it
 * exists and create a file stream for dumping errors in the
 * interpreter. It is called by xpost_init().
 *
 * @see xpost_log_quit()
 * @see xpost_init()
 */
int xpost_log_init(void);

/**
 * @brief Shut down the log module.
 *
 * This function shuts down the log module. It is called by
 * xpost_quit().
 *
 * @see xpost_log_init()
 * @see xpost_quit()
 */
void xpost_log_quit(void);

#endif
