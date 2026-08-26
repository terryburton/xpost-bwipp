/* That the scan conversion's own sort arranges a shape's passages
 * exactly as the order it documents, over every shape of input.
 *
 * The sort exists because a page of small shapes sorts thousands of
 * times and the comparison is the whole cost; what it must never do is
 * arrange anything differently from the order it replaced taking. Two
 * properties state that completely, because the order is total: the
 * output is ordered by (band, left edge, right edge, direction), and the
 * output holds exactly the passages the input held. A sort satisfying
 * both is indistinguishable to the insideness walk from any other
 * correct sort, which is what lets the pages stay byte-identical.
 *
 * The order is restated here rather than read from the library, so that
 * the library drifting from what this file says goes red instead of the
 * two moving together.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_object.h"
#include "xpost_span.h"

#include "xpost_test.h"

/* the documented order, stated independently of the implementation */
static int _spec_order(const struct band_span *a, const struct band_span *b)
{
    if (a->band != b->band) return a->band < b->band ? -1 : 1;
    if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
    if (a->hi != b->hi) return a->hi < b->hi ? -1 : 1;
    return a->dirn - b->dirn;
}

/* byte order, for the multiset comparison only: whether two arrays hold
   the same records is a question about bytes, not about the span order */
static int _byte_order(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(struct band_span));
}

/* a fixed-seed generator, so a failure names a case that can be re-run */
static unsigned long _state = 2463534242UL;
static unsigned long _next(void)
{
    _state ^= _state << 13;
    _state ^= _state >> 7;
    _state ^= _state << 17;
    return _state;
}

static void _check_case(const char *what, struct band_span *in, int n)
{
    struct band_span *out, *want;
    int i;

    out = malloc((size_t)(n ? n : 1) * sizeof *out);
    want = malloc((size_t)(n ? n : 1) * sizeof *want);
    if (!out || !want)
    {
        report_failure("no memory to run the %s case", what);
        free(out); free(want);
        return;
    }
    memcpy(out, in, (size_t)n * sizeof *out);
    xpost_span_sort(out, n);

    for (i = 0; i + 1 < n; i++)
        if (_spec_order(&out[i], &out[i + 1]) > 0)
        {
            report_failure("%s: records %d and %d are not in the order the "
                           "conversion documents (band %d [%g %g] %+d before "
                           "band %d [%g %g] %+d)",
                           what, i, i + 1,
                           out[i].band, out[i].lo, out[i].hi, out[i].dirn,
                           out[i + 1].band, out[i + 1].lo, out[i + 1].hi,
                           out[i + 1].dirn);
            break;
        }

    memcpy(want, in, (size_t)n * sizeof *want);
    qsort(want, (size_t)n, sizeof *want, _byte_order);
    qsort(out, (size_t)n, sizeof *out, _byte_order);
    if (memcmp(out, want, (size_t)n * sizeof *out) != 0)
        report_failure("%s: the sorted array does not hold the records the "
                       "input held -- something was lost, invented or "
                       "altered on the way through", what);

    free(out);
    free(want);
}

int main(void)
{
    struct band_span *a;
    int i, n;
    static const int sizes[] = { 0, 1, 2, 3, 4, 7, 8, 9, 63, 64, 65,
                                 1000, 1972, 4095 };

    a = malloc(4096 * sizeof *a);
    if (!a)
    {
        report_failure("no memory for the cases");
        return verdict();
    }

    /* every size that meets a merge-width boundary, filled with heavy
       duplication: few distinct bands and edges, so equal keys and
       equal whole records both occur */
    for (i = 0; i < (int)(sizeof sizes / sizeof *sizes); i++)
    {
        int k;
        char what[32];
        n = sizes[i];
        for (k = 0; k < n; k++)
        {
            a[k].band = (int)(_next() % 7u);
            a[k].dirn = (_next() & 1u) ? 1 : -1;
            a[k].lo = (double)(_next() % 5u);
            a[k].hi = a[k].lo + (double)(_next() % 3u);
        }
        sprintf(what, "random n=%d", n);
        _check_case(what, a, n);
    }

    /* already ordered, and exactly reversed */
    n = 1972;
    for (i = 0; i < n; i++)
    {
        a[i].band = i / 4;
        a[i].dirn = (i & 1) ? 1 : -1;
        a[i].lo = (double)(i % 4);
        a[i].hi = a[i].lo + 1;
    }
    _check_case("already ordered", a, n);
    for (i = 0; i < n / 2; i++)
    {
        struct band_span t = a[i];
        a[i] = a[n - 1 - i];
        a[n - 1 - i] = t;
    }
    _check_case("reversed", a, n);

    /* every record equal: nothing to order, everything to preserve */
    for (i = 0; i < n; i++)
    {
        a[i].band = 3; a[i].dirn = 1; a[i].lo = 1; a[i].hi = 2;
    }
    _check_case("all equal", a, n);

    free(a);
    return verdict();
}
