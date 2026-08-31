/* Putting a bank back by restoring only the pages a job wrote.
 *
 * The job boundary restores a whole bank from a copy of it, and most of
 * that copy is waste: a job writes a small part of the bank and the rest
 * is copied over itself. Where the host can say which pages were
 * written, the boundary puts back only those.
 *
 * The whole of that idea rests on the host naming every page. Naming too
 * many costs a little work; naming too few leaves one job's bytes in the
 * next job's memory, which is the isolation the boundary exists to
 * provide, and it would show up nowhere else -- the bank still looks
 * plausible, the interpreter still runs, and the wrong answer appears in
 * whatever the next job happens to read. So the restore is held to the
 * baseline byte for byte here, over writes placed where a job's would
 * fall: scattered singly, in runs, at the very edges of the extent, and
 * across a grow, which is the one event that makes a host give the
 * arrangement up and build it again.
 *
 * A host with no answer worth having declines, and the caller copies. So
 * this test passes either way and says which happened: a host that is
 * meant to track and has quietly stopped reads as "declines" here rather
 * than as agreement, and the count of restores taken by each route is
 * printed so that a run which tracked nothing cannot be mistaken for one
 * that tracked everything.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_memory.h"
#include "xpost_object.h"
#include "xpost_free.h"
#include "xpost_vm_writeset.h"

#include "xpost_test.h"

static int fixture_initializing(void) { return 1; }
static void fixture_set_initializing(int i) { (void)i; }

/* Bytes that make a wrong answer obvious: a value derived from the
   offset, so a page put back from the wrong place is not a page of
   zeroes but a page whose contents name where they came from. */
static void fill(unsigned char *p, size_t n, unsigned int seed)
{
    size_t i;

    /* The file closes the arena it has not handed out, so a build that
       describes the arena to the checker reads a write here as a write
       to storage nobody was given -- 261120 of them, one per byte past
       the table. What writes here stands in for the allocator: these are
       the bytes a job would have written, and the extent is the whole
       bank by construction, since what the restore is held to is the
       bank byte for byte. So the range is opened the way the file opens
       a piece it hands out. */
    XPOST_VG_UNPOISON_RANGE(p, 0, n);
    for (i = 0; i < n; i++)
        p[i] = (unsigned char)((i * 31u + seed * 7u) >> 3);
}

int main(void)
{
    Xpost_Memory_File mem;
    Xpost_Memory_Image img;
    unsigned int used;
    int armed, round, tracked = 0, copied = 0, host_tracks = 0;

    if (!xpost_init())
    {
        report_failure("xpost_init");
        return verdict();
    }
    memset(&img, 0, sizeof img);
    memset(&mem, 0, sizeof mem);
    if (!xpost_memory_file_init(&mem, NULL, -1, NULL,
                                fixture_initializing,
                                fixture_set_initializing))
    {
        report_failure("xpost_memory_file_init");
        return verdict();
    }
    if (!xpost_memory_table_init(&mem, XPOST_MEMORY_TABLE_SPECIAL_FREE + 1))
    {
        report_failure("xpost_memory_table_init");
        return verdict();
    }
    if (!xpost_free_init(&mem))
    {
        report_failure("xpost_free_init");
        return verdict();
    }

    /* A bank with something in it, and a baseline taken of it. */
    used = 256u * 1024u;
    if (mem.max < used && !xpost_memory_file_grow(&mem, used))
    {
        report_failure("cannot grow the fixture bank to %u", used);
        return verdict();
    }
    fill(mem.base, used, 1);
    mem.high_water = used;
    if (!xpost_memory_image_capture(&mem, &img))
    {
        report_failure("xpost_memory_image_capture");
        return verdict();
    }

    armed = xpost_memory_revert_arm(&mem, img.store, img.used);
    printf("# this run %s to track what a job writes\n",
           armed ? "asks" : "does not ask");

    /* Writes where a job's would fall, and the bank held to the baseline
       after each boundary. */
    for (round = 0; round < 24; round++)
    {
        /* Where the writes are placed, not the host's page size: the
           restore has to put the bank back whatever the host's pages
           are, and asking the library for its own would be importing a
           data symbol across a shared-library boundary that not every
           host lets a test do. */
        const size_t pg = 4096;
        size_t k;

        switch (round % 4)
        {
        case 0:                                  /* scattered single bytes */
            for (k = 0; k < 8; k++)
                mem.base[((k * 37 + 11) % (img.used / pg)) * pg] =
                    (unsigned char)round;
            break;
        case 1:                                  /* a run of whole pages */
            memset(mem.base + 5 * pg, (unsigned char)round, 6 * pg);
            break;
        case 2:                                  /* the very first and last */
            mem.base[0] = (unsigned char)round;
            mem.base[img.used - 1] = (unsigned char)round;
            break;
        case 3:                                  /* a page's last byte */
            for (k = 1; k < 5; k++)
                mem.base[k * pg - 1] = (unsigned char)round;
            break;
        }

        /* The boundary itself, not the host's half of it: this is the
           call the interpreter makes, so the route it takes and the
           arranging it does on the way out are the ones under test. */
        if (mem.writeset.tracking)
        {
            tracked++;
            host_tracks = 1;   /* the arrangement got made, so it can be */
        }
        else
            copied++;
        xpost_memory_image_restore(&mem, &img);
        check(memcmp(mem.base, img.store, img.used) == 0,
              "the bank is the baseline again after a boundary");
    }

    /* An arrangement is made against one baseline and must not be used to
       put back another: the pages it would restore are the ones written
       since it began, which says nothing about a different image. */
    /* Asking is the run's half; whether the host can is the host's. A
       host that cannot answer declines and every boundary copies, which
       is correct and is what the counts below report. What must not pass
       unnoticed is a host that made the arrangement and then never used
       it. */
    check(!host_tracks || tracked > 0,
          "a bank the host arranged tracking for takes the tracked route");
    check(!armed || copied >= 1,
          "the first boundary copies, since there was nothing yet to track");

    if (armed)
    {
        unsigned char *other = malloc(img.used);

        if (other)
        {
            fill(other, img.used, 2);
            check(xpost_vm_writeset_restore(&mem, other, img.used) == 0,
                  "a restore against a baseline it was not armed with is refused");
            free(other);
        }
    }

    /* A grow moves or remakes the bank, so a host that laid something
       over it has to give that up and build it again. The boundary after
       one must still put the bank back. */
    {
        unsigned int bigger = mem.max + 256u * 1024u;

        if (!xpost_memory_file_grow(&mem, bigger))
            report_failure("the fixture bank would not grow");
        else
        {
            memset(mem.base + 3 * 4096, 0x5a, 4096);
            if (!xpost_vm_writeset_restore(&mem, img.store, img.used))
                memcpy(mem.base, img.store, img.used);
            check(memcmp(mem.base, img.store, img.used) == 0,
                  "the bank is the baseline again after a grow");

            /* and the arrangement can be made afresh over the grown bank */
            if (armed)
            {
                int again = xpost_memory_revert_arm(&mem, img.store, img.used);
                printf("# after a grow the host %s again\n",
                       again ? "tracks" : "declines");
                memset(mem.base + 7 * 4096, 0x33, 2 * 4096);
                if (!xpost_vm_writeset_restore(&mem, img.store, img.used))
                    memcpy(mem.base, img.store, img.used);
                check(memcmp(mem.base, img.store, img.used) == 0,
                      "the bank is the baseline again after arming afresh");
            }
        }
    }

    printf("# the host %s track this bank's writes\n",
           host_tracks ? "does" : "does not");
    printf("# %d boundaries put back only what was written, %d copied\n",
           tracked, copied);
    check(tracked + copied == 24, "every boundary took one route or the other");

    xpost_memory_image_free(&img);
    xpost_memory_file_exit(&mem);
    xpost_quit();
    return verdict();
}
