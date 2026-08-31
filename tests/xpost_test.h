/* Included by the C tests: report a failure, and answer the verdict.
 *
 * A C test reports through its exit status, so a failure it printed
 * counts for nothing unless it reaches that status. Two things have to
 * hold for it to. The failure has to be recorded at the moment it is
 * printed, and the status has to be computed from the record. A test
 * that arranges either of those for itself is a test that can be written
 * not to -- and what that costs is not a wrong answer but no answer: the
 * complaint is on screen, in a log nobody reads because the test passed.
 *
 * So neither half is left to the test. Printing a failure and recording
 * one are the same act here, because there is nothing that does one
 * without the other; the status is read off the same tally through the
 * one expression a test's main returns; and the tally lives inside the
 * function that keeps it, so nothing outside can set it, clear it or
 * read around it.
 *
 * That the tally is one object shared by every report is what makes the
 * count meaningful: a test says how many things went wrong, not merely
 * that something did, and a run whose output is read rather than whose
 * status is taken sees the same number.
 *
 * tests/check-test-quality.sh holds every C test to reaching its status
 * this way rather than restating it. tests/verdict.sh is the same rule
 * for the runs whose verdict is text on their output rather than a
 * status.
 */

#ifndef XPOST_TEST_H
#define XPOST_TEST_H

#include <stdarg.h>
#include <stdio.h>

/* Format checking for the report below, where the compiler offers it.
   A compiler that does not know the attribute drops the checking, not
   the declaration. */
#if defined(__GNUC__)
# define XPOST_TEST_PRINTF(fmt_arg, first_arg) \
    __attribute__((format(printf, fmt_arg, first_arg)))
#else
# define XPOST_TEST_PRINTF(fmt_arg, first_arg)
#endif

/* The tally, and the only thing that reaches it: called with 1 it
   records a failure, called with 0 it answers how many there have been.
   Keeping the count inside the function that keeps it leaves a test
   nothing to set, clear or read around. */
static int xpost_test_tally(int failed)
{
    static int failures = 0;

    if (failed)
        failures++;
    return failures;
}

/* Report a failure and record it. The message is the test's own; the
   prefix is the spelling every wrapper and guard in the suite reads a
   failure by, so it is not the test's to choose either. */
static void report_failure(const char *fmt, ...) XPOST_TEST_PRINTF(1, 2);

static void report_failure(const char *fmt, ...)
{
    va_list ap;

    fputs("FAIL: ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    (void)xpost_test_tally(1);
}

/* Hold a condition, and say so where it does not hold. */
#define check(cond, what) \
    do { if (!(cond)) report_failure("%s", (what)); } while (0)

/* The status a test's main returns, and the line reporting it. Both are
   read off the tally, so a run that reported a failure cannot answer
   zero however it reached here -- including from a setup path that gave
   up long before the end. */
static int verdict(void)
{
    int failures = xpost_test_tally(0);

    if (failures)
    {
        printf("FAILURES: %d\n", failures);
        return 1;
    }
    printf("SUCCESS\n");
    return 0;
}

/* The sink a test installs to keep what a program printed, declared with
   the buffer it fills and the length it fills it to.

   A test that reads a program's output needs three things that have to
   agree: somewhere to put it, how much is there, and a handler that
   guards the first against the second. Written out per test they are
   three chances to disagree -- and a handler guarding a bound the test
   does not read is a test that passes on a truncated answer without
   saying so. Declared together they cannot disagree, and the size, which
   is the one part that is properly the test's own business, is what the
   test says.

   Everything printed past the buffer is dropped, and the sink keeps the
   room to terminate what it did take, so what the test reads is always a
   string. A test needing more than this -- a sink that records what the
   interpreter held at each byte, or one that counts writes rather than
   keeping them -- writes its own; this is for the ones that only want
   the text back.

   The name is the test's too, since a test that keeps what went to the
   output apart from what went to the error stream needs two of these and
   has to say which is which. */
#define XPOST_TEST_SINK(name, size)                                     \
    static char name##_buf[size];                                       \
    static size_t name##_len;                                           \
    static size_t name##_sink(void *user, const char *buf, size_t len)  \
    {                                                                   \
        (void)user;                                                     \
        if (name##_len + len < sizeof name##_buf)                       \
        {                                                               \
            memcpy(name##_buf + name##_len, buf, len);                  \
            name##_len += len;                                          \
        }                                                               \
        name##_buf[name##_len] = '\0';                                  \
        return len;                                                     \
    }

#endif
