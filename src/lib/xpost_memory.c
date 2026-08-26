/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_memory.c
 * @brief The arena every object lives in, and the table that indexes it.
 *
 * A memory file is one region that grows without moving, because an object
 * addresses it by offset rather than by pointer: a growth that copied the
 * region would leave every offset naming the wrong byte. What backs the
 * region -- a mapping, a reservation, or the host allocator -- is decided
 * here, at the head of the file, and is the one thing about it a caller
 * cannot choose.
 *
 * The table at the front of the region is how a byte is found: an entity is
 * a row, and a row is an address and a size. Everything above the layer is
 * an entity number.
 *
 * This is also where a job's snapshot of virtual memory is taken and put
 * back, which is the whole arena copied in one go rather than object by
 * object.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <ctype.h> /* isprint */
#include <errno.h>
#include <stdlib.h> /* free malloc realloc */
#include <stdio.h> /* remove puts */
#include <string.h> /* memset strerror */

#include <sys/stat.h> /* open */

#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h> /* mmap munmap mremap */
/* Two spellings for a mapping backed by nothing, and one flag that does
   not exist everywhere. MAP_ANON is the older name and the only one the
   BSD-derived systems declare by default; MAP_NORESERVE asks a host that
   accounts for commit up front not to, and where it is absent nothing is
   being asked for -- a reservation is made unreadable and unwritable, so
   there is nothing to account for until part of it is committed. */
# if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#  define MAP_ANONYMOUS MAP_ANON
# endif
# ifndef MAP_NORESERVE
#  define MAP_NORESERVE 0
# endif
#endif

/* THE THREE WAYS AN ARENA IS BACKED, and what decides between them.

   What an arena wants is to grow without moving: objects address it by
   offset, so a base that moves is only bearable because every pointer is
   derived afresh, and a grow that copies pays for the whole file each
   time it doubles.

     a mapping extended in place   mmap with mremap
     a reservation committed by
       the piece                   mmap without mremap, and Win64
     the host allocator            everything else

   The middle one is what a host without mremap does instead of copying
   between two mappings: the range an offset can address is reserved
   without being charged for, and more of it is committed as the file
   fills. The base never moves, no copy is made, and only what is in use
   is charged -- the same arrangement as the first, reached differently.
   It is also what makes storage returnable, since the file then owns
   pages rather than borrowing them from an allocator. */

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
# undef WIN32_LEAN_AND_MEAN
# include <io.h> /* _chsize close */
# define read(f, p, s) _read(f, p, s)
# define lseek(f, p, fl) _lseek(f, p, fl)
# define close(f) _close(f)
# define ftruncate(fd_, size_) _chsize((fd_), (size_))
#else
# include <unistd.h> /* close ftruncate getpagesize read sysconf write */
#endif


#include "xpost.h"
#include "xpost_log.h"
#include "xpost_compat.h"
#include "xpost_error.h"
#include "xpost_memory.h"
#include "xpost_free.h" /* the installed allocator's answer codes */
#include "xpost_object.h"



size_t xpost_memory_page_size;
size_t xpost_memory_return_grain;

/* Reserve and commit: taken on Win64, and on a POSIX host whose mapping
   cannot be extended.

   On Win64 it is taken in preference to a section, because a
   pagefile-backed section charges its whole nominal size against the
   system commit limit the moment it is created, where a reservation
   charges a page when the page is touched. A memory file grows
   geometrically and leaves most of the growth untouched, so sections
   would ask the system to commit gigabytes that are never written --
   and ask for the old and the new size at once, the contents having to
   be copied across.

   On POSIX it is taken in preference to the host allocator, which is
   what a host without mremap had before: a reservation grows without
   copying and without moving, and the file owns the pages under it
   rather than borrowing them, which is what lets storage be given
   back. */
#if defined(_WIN64) || (defined(HAVE_MMAP) && !defined(HAVE_MREMAP))
# define XPOST_MEMORY_RESERVED_VM 1
/* an object addresses the file through an unsigned int offset, so the
   file cannot exceed 4G and a reservation of that size always covers it */
# define XPOST_MEMORY_RESERVE ((size_t)0x100000000ULL)
/* capacity is committed in steps of this size, so a file that fills
   byte by byte does not make a system call per allocation */
# define XPOST_MEMORY_COMMIT_STEP ((size_t)0x100000)

/* The three steps, each named once so that the two hosts differ in what
   they call and in nothing else. A reservation is address space the
   process has claimed and is not charged for; committing part of it
   makes that part readable and writable, and a freshly committed page
   reads as zero, which is what the caller of a new memory file is
   promised. */
static unsigned char *_xpost_memory_reserve(size_t len)
{
# ifdef _WIN32
    return (unsigned char *)VirtualAlloc(NULL, len, MEM_RESERVE, PAGE_READWRITE);
# else
    void *p = mmap(NULL, len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    return (p == MAP_FAILED) ? NULL : (unsigned char *)p;
# endif
}

static int _xpost_memory_commit(unsigned char *base, size_t len)
{
# ifdef _WIN32
    return VirtualAlloc(base, len, MEM_COMMIT, PAGE_READWRITE) != NULL;
# else
    return mprotect(base, len, PROT_READ | PROT_WRITE) == 0;
# endif
}

static void _xpost_memory_unreserve(unsigned char *base, size_t len)
{
# ifdef _WIN32
    (void)len;
    VirtualFree((void *)base, 0, MEM_RELEASE);
# else
    munmap((void *)base, len);
# endif
}

/* what the last of those said went wrong, as a number the log can carry */
static unsigned long _xpost_memory_reserve_error(void)
{
# ifdef _WIN32
    return (unsigned long)GetLastError();
# else
    return (unsigned long)errno;
# endif
}
#endif

/*
   initialize the global extern page_size variable
 */
int
xpost_memory_init(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;

    GetSystemInfo(&si);

    /* Two grains, and they differ here: address space is handed out in
       allocation-granularity units, storage is given up a page at a
       time. */
    xpost_memory_page_size = (size_t)si.dwAllocationGranularity;
    xpost_memory_return_grain = (size_t)si.dwPageSize;
    return 1;
#elif defined HAVE_SYSCONF_PAGESIZE
    xpost_memory_page_size = (size_t)sysconf(_SC_PAGESIZE);
    xpost_memory_return_grain = xpost_memory_page_size;
    return 1;
#elif defined HAVE_SYSCONF_PAGE_SIZE
    xpost_memory_page_size = (size_t)sysconf(_SC_PAGE_SIZE);
    xpost_memory_return_grain = xpost_memory_page_size;
    return 1;
#elif defined HAVE_GETPAGESIZE
    xpost_memory_page_size = (size_t)getpagesize();
    xpost_memory_return_grain = xpost_memory_page_size;
    return 1;
#else
    XPOST_LOG_ERR("Could not find a way to retrieve the page size");
    return 0;
#endif
}

/*
   initialize the memory file structure,
   possibly using filename or file descriptor.
   install pointers to interpreter functions (so gc can discover contexts given only a memory file)
 */
XPOST_TEST_VISIBLE int
xpost_memory_file_init(Xpost_Memory_File *mem,
                       const char *fname,
                       int fd,
                       struct _Xpost_Context *(*xpost_interpreter_cid_get_context)(unsigned int cid),
                       int (*xpost_interpreter_get_initializing)(void),
                       void (*xpost_interpreter_set_initializing)(int))
{
    struct stat buf;
    size_t sz = xpost_memory_page_size;
#ifdef _WIN32
    HANDLE h;
    HANDLE fm;
#endif

    if (!mem)
    {
        XPOST_LOG_ERR("%d mem pointer is NULL", VMerror);
        return 0;
    }
    XPOST_LOG_INFO("init memory file%s%s",
                   fname ? " for " : "", fname ? fname : "");

    mem->interpreter_cid_get_context = xpost_interpreter_cid_get_context;
    mem->interpreter_get_initializing = xpost_interpreter_get_initializing;
    mem->interpreter_set_initializing = xpost_interpreter_set_initializing;

    mem->free_scan = 0;
    mem->stack_walk = 0;
    mem->push_refused = 0;
    mem->path_walk.ent = 0;
    mem->path_walk.end = 0;
    mem->path_walk.sps = 0;
    mem->path_walk.last = 0;
    mem->path_walk.steps = 0;

    if(fname)
    {
        strncpy(mem->fname, fname, sizeof(mem->fname));
        mem->fname[sizeof(mem->fname) - 1] = '\0';
    }
    else
        mem->fname[0] = '\0';

    mem->fd = fd;
    if (fd != -1)
    {
        if (fstat(fd, &buf) == 0)
        {
            sz = buf.st_size;
            if (sz < xpost_memory_page_size)
            {
                sz = xpost_memory_page_size;
#if defined (HAVE_MMAP) || defined (_WIN32)
                if (fd != -1)
                {
                    if (ftruncate(fd, sz) == -1)
                        XPOST_LOG_ERR("ftruncate(%d, %llu) returned -1 (error: %s)",
                                      fd, (unsigned long long)sz,
                                      strerror(errno));
                }
#endif
            }
        }
    }


#ifdef XPOST_MEMORY_RESERVED_VM
    if (fd == -1)
    {
        mem->base = _xpost_memory_reserve(XPOST_MEMORY_RESERVE);
        if (mem->base && !_xpost_memory_commit(mem->base, sz))
        {
            _xpost_memory_unreserve(mem->base, XPOST_MEMORY_RESERVE);
            mem->base = NULL;
        }
        if (!mem->base)
        {
            XPOST_LOG_ERR("%d failed to reserve memory-file data (%lu)",
                          VMerror, _xpost_memory_reserve_error());
            return 0;
        }
        mem->high_water = 0;
        mem->max = sz;
        /* a freshly committed page reads as zero, which is what the
           caller of a new memory file is promised */
        return 1;
    }
#endif

#ifdef _WIN32
    if (fd == -1)
        h = INVALID_HANDLE_VALUE;
    else
    {
        h = (HANDLE)_get_osfhandle(fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            XPOST_LOG_ERR("Invalid handle");
            close(fd);
            return 0;
        }
    }

# ifdef _WIN64
    fm = CreateFileMapping(h, NULL, PAGE_READWRITE,
                           (DWORD)(sz >> 32), (DWORD)(sz & 0x00000000ffffffffULL), NULL);
# else
    fm = CreateFileMapping(h, NULL, PAGE_READWRITE, 0, sz & 0xffffffff, NULL);
# endif
    if (!fm)
    {
        XPOST_LOG_ERR("CreateFileMapping failed (%ld)", GetLastError());
        if (fd != -1) close(fd);
        return 0;
    }

    mem->base = (unsigned char *)MapViewOfFile(fm, FILE_MAP_ALL_ACCESS, 0, 0, sz);
    CloseHandle(fm);
    if (!mem->base)
    {
#elif defined (HAVE_MREMAP)
    /* Reached only where the mapping can be extended in place. A file
       backed by a descriptor and a host without mremap have no way to
       grow this without copying between two mappings, and take the
       allocator below instead -- the reservation above is for the
       anonymous case, which is the only one anything makes. */
    mem->base = (unsigned char *)mmap(NULL,
                                      sz,
                                      PROT_READ | PROT_WRITE,
                                      (fd == -1 ? MAP_PRIVATE   : MAP_SHARED) |
                                      (fd == -1 ? MAP_ANONYMOUS : 0),
                                      fd, 0);
    if (mem->base == MAP_FAILED)
    { /* . */
#else
    mem->base = malloc(sz);
    if (mem->base == NULL)
    { /* .. */
#endif
        XPOST_LOG_ERR("%d failed to allocate memory-file data", VMerror);
        return 0;
    } /* . .. */
    mem->high_water = 0;
    mem->max = sz;
#ifndef HAVE_MMAP
    /* read file into malloc'd memory */
    if (fd != -1)
    {
        if (read(fd, mem->base, sz) == -1)
            XPOST_LOG_ERR("%d failed to read memory file (error: %s)",
                          VMerror, strerror(errno));
    }
#endif
    if (fd == -1)
        memset(mem->base, 0, mem->max);

    /* nothing has been handed out yet, so the whole extent is closed */
    XPOST_VG_POISON_RANGE(mem->base, 0, mem->max);

    return 1;
}

/* Give the whole pages inside a range of the arena back to the system.
   Answers the bytes that went, which is zero where none could.

   WHICH BACKINGS CAN TAKE IT. Only storage this process holds on its own
   account -- an anonymous mapping, or a reservation it committed -- whose
   pages are charged to it again when the range is next written. A mapping
   of a file is not this process's to give back: the bytes are the file's,
   and dropping the pages only means reading them in again. Where the
   arena came from the host allocator the file does not own the pages
   under it and has no way to name them.

   WHICH CALL. What is wanted is a call that gives the storage up and
   leaves the range writable, so that nothing has to be asked for before
   the range is used again: an allocator that had to ask would be asking
   on the path every allocation takes. Every host has one, and they
   behave alike -- the storage goes, the range stays addressable, and it
   reads as zero until it is written again.

     Linux          madvise MADV_DONTNEED
     other POSIX    the range re-mapped in place, MAP_FIXED
     Windows        DiscardVirtualMemory, from Windows 8.1

   The middle one is plain POSIX and serves Linux too, at about three
   times the cost of the call above it (9.7us against 3.1us for a block
   and a write); Linux keeps the cheaper one and everything else takes
   the portable route, which is why no host here needs a mechanism of its
   own. macOS has a third way, MADV_FREE_REUSABLE, which is cheaper still
   but has to be paired with MADV_FREE_REUSE where the range is next
   used -- a call on the allocator's path, to save time on this one --
   so it is not taken.

   Windows is also able to decommit the range outright, which is cheaper
   per call, but a decommitted range faults on the next write and would
   put a system call on every handout to prevent it. The discard is taken
   instead: it costs about ten times as much, and it costs it here, where
   a program has asked for a collection, rather than in the allocator. */
unsigned int
xpost_memory_file_release_range(Xpost_Memory_File *mem,
                                unsigned int adr,
                                unsigned int len)
{
#if defined(HAVE_MMAP) || defined(XPOST_MEMORY_RESERVED_VM)
    size_t ps = xpost_memory_return_grain;
    size_t from;
    size_t to;

    if (!mem || !mem->base || mem->fd != -1 || ps == 0)
        return 0;
    /* the range is the file's to speak for */
    if ((size_t)adr + len > mem->max)
        return 0;

    from = (((size_t)adr + ps - 1) / ps) * ps;
    to = (((size_t)adr + len) / ps) * ps;
    if (to <= from)
        return 0;

    /* a range of bytes rather than anything with a shape: what the calls
       below are told is where the storage is and how much of it there is */
    {
        void *bytes = xpost_vm_ptr(mem, (unsigned int)from);

# if defined(_WIN32)
        /* Named through the module rather than linked to, so that a
           build of this runs on the Windows that predate the call: where
           it is absent nothing is handed back, which is the same answer
           this gives for a backing that cannot take it. */
        typedef DWORD (WINAPI *Xpost_Discard_Func)(PVOID, SIZE_T);
        Xpost_Discard_Func discard = (Xpost_Discard_Func)(void *)
            GetProcAddress(GetModuleHandleA("kernel32.dll"),
                           "DiscardVirtualMemory");
        DWORD ret;

        if (!discard)
            return 0;
        ret = discard(bytes, to - from);
        if (ret != ERROR_SUCCESS)
        {
            XPOST_LOG_ERR("cannot hand back %lu bytes at %lu (error %lu)",
                          (unsigned long)(to - from), (unsigned long)from,
                          (unsigned long)ret);
            return 0;
        }
# else
        /* The portable route: the range put back as fresh anonymous
           pages, in place. The old ones go and the mapping is left
           exactly as it was, so this neither disturbs the reservation
           around it nor leaves the kernel holding more mappings than it
           did -- measured over three rounds of returning and rewriting
           every block, the count does not move.

           Where a host has a cheaper call of its own it is used instead,
           and XPOST_RETURN_REMAPS asks for this one anyway. That is what
           runs the route the other systems take on a host that would
           otherwise never reach it; it takes nothing away, both routes
           hand back the same bytes and leave the range writable, so it
           chooses between two correct answers as XPOST_GROW_MOVES does. */
        int remap = 1;

#  if defined(__linux__) && defined(MADV_DONTNEED)
        remap = getenv("XPOST_RETURN_REMAPS") != NULL;
        if (!remap && madvise(bytes, to - from, MADV_DONTNEED) != 0)
        {
            XPOST_LOG_ERR("cannot hand back %lu bytes at %lu (error: %s)",
                          (unsigned long)(to - from), (unsigned long)from,
                          strerror(errno));
            return 0;
        }
#  endif
        if (remap
            && mmap(bytes, to - from, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
        {
            XPOST_LOG_ERR("cannot hand back %lu bytes at %lu (error: %s)",
                          (unsigned long)(to - from), (unsigned long)from,
                          strerror(errno));
            return 0;
        }
# endif
        /* The storage has gone, so nothing may be read from the range
           until it is taken again. Said here rather than left to the
           call, because the two routes leave a checker's view of the
           range differing: a re-mapping is a system call it follows, and
           advice to drop the pages is not something it sees at all. */
        XPOST_VG_POISON_RANGE(mem->base, (unsigned int)from,
                              (unsigned int)(to - from));
    }
    return (unsigned int)(to - from);
#else
    (void)mem;
    (void)adr;
    (void)len;
    return 0;
#endif
}

/*
   Close, deallocate, and destroy memory file structure
 */
XPOST_TEST_VISIBLE int
xpost_memory_file_exit(Xpost_Memory_File *mem)
{
    if (!mem)
    {
        XPOST_LOG_ERR("%d mem pointer is NULL", VMerror);
        return 0;
    }

    if (mem->base == NULL)
    {
        XPOST_LOG_ERR("%d mem->base is NULL, mem not initialized ?", VMerror);
        return 0;
    }
    XPOST_LOG_INFO("exit memory file %s", mem->fname);

    /* The operator table is this file's and outside the arena, so it does
       not go with the mapping the way everything else in here does. The
       local bank never has one and frees a null. */
    free(mem->optab);
    mem->optab = NULL;
    mem->optab_max = 0;

#if defined(XPOST_MEMORY_RESERVED_VM)
    if (mem->fd == -1)
        _xpost_memory_unreserve(mem->base, XPOST_MEMORY_RESERVE);
    else
# ifdef _WIN32
        UnmapViewOfFile(mem->base);
# else
        munmap((void *)mem->base, mem->max);
# endif
#elif defined(_WIN32)
    UnmapViewOfFile(mem->base);
#elif defined (HAVE_MREMAP)
    munmap((void *)mem->base, mem->max);
#else
    if (mem->fd != -1)
    {
        /* the arena is written back from its start, so a descriptor that
           will not seek there has no offset the write could go to: it
           would land wherever the descriptor happened to be, and the file
           the next run reads back as its memory would be shifted */
        if (lseek(mem->fd, 0, SEEK_SET) == (off_t)-1)
            XPOST_LOG_ERR("%d unable to rewind memory file (error: %s)",
                          VMerror, strerror(errno));
        else if (write(mem->fd, mem->base, mem->high_water) == -1)
            XPOST_LOG_ERR("%d unable to write memory file (error: %s)",
                          VMerror, strerror(errno));
    }
    free(mem->base);
#endif
    mem->base = NULL;
    mem->high_water = 0;
    mem->max = 0;

    if (mem->fd != -1)
    {
        close(mem->fd);
        mem->fd = -1;
    }
    if (mem->fname[0] != '\0')
    {
        struct stat sb;
        if (stat(mem->fname, &sb) == 0)
            remove(mem->fname);
        mem->fname[0] = '\0';
    }

    /* the table indexing the arena is held outside it */
    free(mem->table.tab);
    mem->table.tab = NULL;
    mem->table.max = 0;
    mem->table.nextent = 0;

    return 1;
}

/* grow memory file by sz bytes, rounded up to the nearest system page size.
   return 1 on success, 0 on failure.
 */
XPOST_TEST_VISIBLE int
xpost_memory_file_grow(Xpost_Memory_File *mem,
                       size_t sz)
{
#ifdef _WIN32
    HANDLE h;
    HANDLE fm;
#endif
    void *tmp;
    int ret = 1;

    if (!mem)
    {
        XPOST_LOG_ERR("%d mem pointer is NULL", VMerror);
        return 0;
    }

    if (mem->base == NULL)
    {
        XPOST_LOG_ERR("%d mem->base is NULL", VMerror);
        return 0;
    }

    /* every route below moves or rewrites the whole extent -- copying
       it forward, zeroing the part above the high-water mark -- so the
       file's own bookkeeping reaches storage the arena has closed.
       Open all of it here; the state is laid over the new extent once
       it is in place. */
    XPOST_VG_REOPEN_RANGE(mem->base, 0, mem->max);

    if (sz < xpost_memory_page_size)
        sz = xpost_memory_page_size;
    else
        sz = (sz / xpost_memory_page_size + 1) * xpost_memory_page_size;

#ifdef XPOST_MEMORY_RESERVED_VM
    if (mem->fd == -1)
    {
        /* the range is already reserved and the base does not move, so
           capacity is added by committing a further step of it: the
           over-allocation a copying grow needs to keep its cost down
           would only be commit charge for bytes the file never writes */
        size_t want = (size_t)mem->high_water + sz;

        if (want > 0xffffffffu)
        {
            XPOST_LOG_ERR("%d memory file full: cannot grow beyond addressable size", VMerror);
            return 0;
        }
        want = (want / XPOST_MEMORY_COMMIT_STEP + 1) * XPOST_MEMORY_COMMIT_STEP;
        if (want > 0xffffffffu)
            want = 0xffffffffu;
        if (want <= mem->max)
            return 1;

        XPOST_LOG_INFO("commit memory file%s%s (old: %lu  new: %lu)",
                       mem->fname[0] ? " for " : "", mem->fname[0] ? mem->fname : "",
                       (unsigned long)mem->max, (unsigned long)want);

        if (!_xpost_memory_commit(mem->base, want))
        {
            XPOST_LOG_ERR("%d unable to commit memory (%lu)", VMerror,
                          _xpost_memory_reserve_error());
            return 0;
        }
        mem->max = want;
        return 1;
    }
#endif

    {
        /* objects address the file through unsigned int offsets, which
           caps a memory file at 4G: clamp the geometric growth to that
           limit and fail once a request itself no longer fits, so the
           caller raises VMerror instead of wrapping the size.

           The arithmetic is done in a type wide enough to hold the cap
           whatever a pointer is. size_t is that wide only where a
           pointer is: where one is four bytes, every size_t is below the
           cap by construction, so the comparison answers no to every
           request and the growth it exists to refuse wraps instead --
           the file then reports a size it does not have and an address
           taken in it lands outside the allocation.

           The cap is the largest size a narrow platform can express at
           all, so a request that reaches it is passed to the allocator
           to refuse rather than being refused twice here. */
        unsigned long long req = sz;
        unsigned long long want = req + (unsigned long long)(mem->max * 1.5);

        if (want > 0xffffffffull)
        {
            if ((unsigned long long)mem->high_water + req > 0xffffffffull)
            {
                XPOST_LOG_ERR("%d memory file full: cannot grow beyond addressable size", VMerror);
                return 0;
            }
            want = 0xffffffffull;
        }
        sz = (size_t)want;
    }

    XPOST_LOG_INFO("grow memory file%s%s (old: %u  new: %llu)",
                   mem->fname[0] ? " for " : "", mem->fname[0] ? mem->fname : "",
                   mem->max, (unsigned long long)sz);

#ifdef _WIN32
    if (mem->fd != -1)
    {
        if (ftruncate(mem->fd, sz) == -1)
        {
            XPOST_LOG_ERR("ftruncate(%d, %d) returned -1", mem->fd, sz);
            XPOST_LOG_ERR("strerror: %s", strerror(errno));
        }
    }

    if (mem->fd == -1)
        h = INVALID_HANDLE_VALUE;
    else
    {
        h = (HANDLE)_get_osfhandle(mem->fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            XPOST_LOG_ERR("Invalid handle");
            close(mem->fd);
            return 0;
        }
    }

#ifdef _WIN64
    fm = CreateFileMapping(h, NULL, PAGE_READWRITE,
                           (DWORD)(sz >> 32), (DWORD)(sz & 0x00000000ffffffffULL), NULL);
#else
    fm = CreateFileMapping(h, NULL, PAGE_READWRITE, 0, sz & 0xffffffff, NULL);
#endif
    if (!fm)
    {
        XPOST_LOG_ERR("CreateFileMapping failed (%ld)", GetLastError());
        if (mem->fd != -1) close(mem->fd);
        return 0;
    }

    tmp = MapViewOfFile(fm, FILE_MAP_ALL_ACCESS, 0, 0, sz);
    CloseHandle(fm);
    if (tmp)
    {
        memcpy(tmp, mem->base, mem->high_water);
        UnmapViewOfFile(mem->base);
    }
    else
    { /* hanging error case */
#elif defined (HAVE_MREMAP)
    if (mem->fd != -1)
    {
        if (ftruncate(mem->fd, sz) == -1)
            XPOST_LOG_ERR("ftruncate(%d, %llu) returned -1 (error: %s)",
                          mem->fd, (unsigned long long)sz, strerror(errno));
    }
    /* Extending the mapping in place is what makes this backing worth
       having: the storage that is added is never written by the grow, so
       the extent a geometric growth leaves spare costs nothing until the
       file reaches it. That also means a pointer taken into the arena
       before a grow usually still works, and a stale one is found only
       by chance. Under XPOST_GROW_MOVES the mapping is made afresh
       elsewhere and the old one released instead, so any pointer held
       across an allocation addresses an unmapped page; that is what
       tests/run-reloc-stress-test.sh runs the interpreter under. Both
       routes grow the file and leave the caller with what it asked for. */
    if (getenv("XPOST_GROW_MOVES"))
    {
        tmp = mmap(NULL, sz,
                   PROT_READ | PROT_WRITE,
                   (mem->fd == -1 ? MAP_PRIVATE | MAP_ANONYMOUS : MAP_SHARED),
                   mem->fd, 0);
        if (tmp != MAP_FAILED)
        {
            memcpy(tmp, mem->base, mem->high_water);
            munmap((void *)mem->base, mem->max);
        }
    }
    else
        tmp = mremap(mem->base, mem->max, sz, MREMAP_MAYMOVE);
    if (tmp == MAP_FAILED)
    { /* hanging error case */
#else
    if (getenv("XPOST_GROW_MOVES"))
    {
        /* Force every grow to relocate, so that a pointer taken into the
           old buffer is a use-after-free a sanitizer reports rather than
           a read of stale bytes that usually looks right.

           Unlike the collector's quarantine, this is not compiled behind
           WANT_DEBUG_HOOKS. It takes nothing away: both branches grow
           the file, the interpreter computes the same answers either way
           -- the same run reports the same virtual memory under the
           variable and without it -- and neither leaves the caller with
           less than it asked for. What it changes is which correct way
           the buffer is grown, which makes a defect elsewhere visible
           without being one. And tests/run-reloc-stress-test.sh runs
           under it in the ordinary suite: compiled out of the build
           everyone builds, the tree's standing stress for stale virtual
           memory pointers would stop running while still reporting
           success. */
        tmp = malloc(sz);
        if (tmp != NULL)
        {
            memcpy(tmp, mem->base, mem->high_water);
            free(mem->base);
        }
    }
    else
    tmp = realloc(mem->base, sz);
    /* A block from the host allocator carries what it last held, past
       what was copied into it. A memory file reads as zero above its
       high-water mark from the moment it is made, and it goes on
       reading that way through a grow: the storage above the mark is
       what the file hands out next, and an allocation that came back
       carrying the last tenant's bytes would put them in the middle of
       what the file holds. Both ways of growing the file are covered by
       the one clearing. */
    if (tmp != NULL)
        memset((unsigned char *)tmp + mem->high_water, 0, sz - mem->high_water);
    if (tmp == NULL)
    { /* hanging error case */
#endif
        /* common error case closes the three possible hanging error cases */
        XPOST_LOG_ERR("%d unable to grow memory", VMerror);
        /* leave the existing mapping in place: publishing the failed base
           (MAP_FAILED or NULL) would turn a recoverable VMerror into a wild
           dereference on the very next memory access */
        return 0;
    }
    mem->base = (unsigned char *)tmp;
    mem->max = sz;

    /* The host allocator hands back a block accessible in its whole
       extent, so what the arena knows about it is laid over it again:
       everything the file has handed out stays open, everything above
       the high-water mark is closed. A reclaimed entity below the mark
       reopens here -- the tag a freed entity carries is the same zero a
       live raw allocation carries, so the two cannot be told apart from
       the table, and reopening one costs a redzone while closing the
       other would report the file's own bookkeeping as an error. */
    XPOST_VG_POISON_RANGE(mem->base, mem->high_water, sz - mem->high_water);
#ifdef XPOST_VALGRIND_ARENA
    xpost_free_repoison(mem);
#endif

    return ret;
}


/* The whole-VM image: capture is a copy of the value store and the entity
   table plus the file's free-list and file-birth bookkeeping; restore puts
   all of it back and moves the cursors to where they stood, which reverts
   every object at once (strings and stack bytes with the rest, since the
   store is where they live) and drops everything born since the capture by
   cursor rather than by freeing. This is the revert the job-encapsulation
   boundary performs (PLRM 3.7.7): total over the bank and, being a copy
   back into storage the file already owns, allocating nothing and unable
   to fail. */

void xpost_memory_image_free(Xpost_Memory_Image *img)
{
    if (!img)
        return;
    free(img->store);
    free(img->tab);
    img->store = NULL;
    img->tab = NULL;
    img->valid = 0;
}

int xpost_memory_image_capture(Xpost_Memory_File *mem, Xpost_Memory_Image *img)
{
    size_t esz = sizeof *mem->table.tab;
    size_t tabbytes;

    xpost_memory_image_free(img);

    img->used = mem->high_water;
    img->nextent = mem->table.nextent;
    img->max = mem->max;
    tabbytes = (size_t)img->nextent * esz;

    img->store = malloc(img->used ? img->used : 1);
    if (!img->store)
    {
        XPOST_LOG_ERR("%d cannot allocate VM image store", VMerror);
        return 0;
    }
    img->tab = malloc(tabbytes ? tabbytes : 1);
    if (!img->tab)
    {
        XPOST_LOG_ERR("%d cannot allocate VM image table", VMerror);
        free(img->store);
        img->store = NULL;
        return 0;
    }
    /* The copy reads the whole live extent in one go, reclaimed entities
       and all, so it reaches storage the arena has closed -- the same
       thing a grow does, and answered the same way: open the extent for
       the copy, then lay the description over it again. Closing them
       again from the free lists is what keeps a use-after-free inside
       the arena reportable across a job boundary; reopening alone would
       give that up from the first boundary onwards. */
    XPOST_VG_REOPEN_RANGE(mem->base, 0, img->used);
    memcpy(img->store, mem->base, img->used);
    memcpy(img->tab, mem->table.tab, tabbytes);
#ifdef XPOST_VALGRIND_ARENA
    xpost_free_repoison(mem);
#endif

    img->start = mem->start;
    img->free_substack = mem->free_substack;
    img->free_scan = mem->free_scan;
    img->gc_ent_budget = mem->gc_ent_budget;
    img->garbage_collect_auto = mem->garbage_collect_auto;
    img->threshold = mem->threshold;
    img->threshold_bytes = mem->threshold_bytes;
    img->file_birth_max = mem->file_birth_max;
    memcpy(img->file_births, mem->file_births, sizeof img->file_births);

    img->valid = 1;
    return 1;
}

void xpost_memory_image_restore(Xpost_Memory_File *mem, const Xpost_Memory_Image *img)
{
    size_t esz = sizeof *mem->table.tab;
    unsigned int ent;

    if (!img || !img->valid)
        return;

    /* the file only ever grows, so its store and table are at least as
       large as the image; the copies land in place with no allocation */
    /* As in the capture, and for the same reason: the write lands over
       whatever the job left, including the entities the job's own
       collector reclaimed. The description is laid over the arena again
       at the end of this function, once the table it is read from is the
       baseline's rather than the finished job's. */
    XPOST_VG_REOPEN_RANGE(mem->base, 0, img->used);
    memcpy(mem->base, img->store, img->used);
    memcpy(mem->table.tab, img->tab, (size_t)img->nextent * esz);

    mem->high_water = img->used;
    mem->table.nextent = img->nextent;
    mem->start = img->start;
    mem->free_substack = img->free_substack;
    mem->free_scan = img->free_scan;
    mem->gc_ent_budget = img->gc_ent_budget;
    mem->garbage_collect_auto = img->garbage_collect_auto;
    mem->threshold = img->threshold;
    mem->threshold_bytes = img->threshold_bytes;
    mem->file_birth_max = img->file_birth_max;
    memcpy(mem->file_births, img->file_births, sizeof mem->file_births);

    /* The chain of zero-size rows that describe nothing is derived, not
       carried in the image -- a stored head could disagree with the rows
       the allocator acts on -- so it is rebuilt by scanning the restored
       table, the way the whole-VM image loader does (xpost_vm_image.c). A
       job that compacted the arena (an immediate vmreclaim) chains rows
       here; leaving the head across the revert hands the next job an entity
       number the revert just un-counted, which is a cross-job failure and a
       table write outside the live band. */
    mem->table.freerow = 0;
    for (ent = mem->table.nextent; ent-- > mem->start; )
        if (mem->table.tab[ent].sz == 0)
        {
            mem->table.tab[ent].nextfree = mem->table.freerow;
            mem->table.freerow = ent;
        }

    /* Requests and a cache the revert leaves stale: a collection or a
       compaction the reverted-away job asked for, and the path-walk cache,
       which is keyed by an entity number the revert may now give different
       contents. Clear all three so the next job decides afresh. */
    mem->garbage_collect_pending = 0;
    mem->compact_pending = 0;
    mem->path_walk.ent = 0;

    /* Hand back what the job grew. The revert pulled the live cursor down to
       the baseline, so the pages the job committed above it are no longer
       anyone's: release them to the system, so the worker's footprint tracks
       the job it is running rather than the largest it ever ran, and bring
       the arena's size down with the cursor, so globalvmstatus reports the
       arena a job begins from and a job that fills the free space fills the
       baseline rather than a peak an earlier job left. This is the page
       return vmreclaim's compaction uses (xpost_memory_file_release_range);
       the revert has already put the free lists back to the baseline's, so
       the run above the cursor is handed back whole. A job whose peak fit
       under the baseline pays nothing; one that grew the arena pays in
       re-committing the room it needs the next time. */
    if (mem->max > mem->high_water)
        (void)xpost_memory_file_release_range(mem, mem->high_water,
                                              mem->max - mem->high_water);
    if (mem->max > img->max)
        mem->max = img->max;

    /* What the arena knows about itself, laid over the restored contents:
       everything above the live cursor is closed, and the entities the
       baseline's own free lists chain are closed again. Read from the
       restored table, so what closes is what the next job will find
       reclaimed rather than what the job just discarded had. */
    XPOST_VG_POISON_RANGE(mem->base, mem->high_water,
                          mem->max - mem->high_water);
#ifdef XPOST_VALGRIND_ARENA
    xpost_free_repoison(mem);
#endif
}


/*
   allocate data linearly from the memory file
   */
XPOST_TEST_VISIBLE int
xpost_memory_file_alloc(Xpost_Memory_File *mem,
                        unsigned int sz,
                        unsigned int *retaddr)
{
    unsigned long long adr;
    unsigned long long end;

    if (!mem)
    {
        XPOST_LOG_ERR("%d mem pointer is NULL", VMerror);
        return 0;
    }

    if (mem->base == NULL)
    {
        XPOST_LOG_ERR("%d mem->base is NULL, mem not initialized ?", VMerror);
        return 0;
    }

    /* 8-align every allocation so structs stored in the file -- dict
       headers, name-tree nodes, operator signatures, objects -- are read
       and written at their natural alignment (the file base is already
       aligned). The bytes skipped before an aligned address are padding
       within the file; entities are located by their recorded address, not
       by walking the file, so the gap is harmless.

       The aligned address and the byte one past the allocation are both
       arrived at in a type wide enough to hold every address the file
       has, whatever a pointer is on the platform. An address in the file
       is an unsigned int, so where a size_t is no wider than one, a sum
       of two of them wraps: an allocation reaching past the end of the
       address range compares as one well inside it, and the address
       handed back is a small number that names storage already in use --
       or, taken as an offset from the file's base, memory in front of the
       file altogether. */
    adr = ((unsigned long long)mem->high_water + 7u) & ~(unsigned long long)7u;
    /* The mark advances by the aligned extent, not the requested one, so
       that an entity's footprint in the file is the same whether it was
       laid down here or packed by the rearrangement, which accounts in
       aligned extents throughout. The mark is then one figure for one
       population of entities, however the arena came to hold them, and
       the padding above an odd-sized allocation is charged to the
       allocation rather than surfacing later as a phantom cost of the
       first rearrangement to pass over it. */
    end = (adr + sz + 7u) & ~(unsigned long long)7u;

    /* the file's addresses are unsigned ints and cannot name a byte past
       this one, so an allocation ending beyond it is one no amount of
       growth can hold */
    if (end > 0xffffffffull)
    {
        XPOST_LOG_ERR("%d memory file full: cannot allocate beyond addressable size", VMerror);
        return 0;
    }

    if (sz)
    {
        if (end >= mem->max)
        {
            if (!xpost_memory_file_grow(mem, (size_t)(end - mem->high_water)))
            {
                XPOST_LOG_ERR("%d unable to allocate memory", VMerror);
                return 0;
            }
        }

        /* opened before it is written: the zeroing below is the first
           access the file makes to storage it has just handed out. The
           alignment padding above the allocation is inside the extent
           the mark advances over, so it is cleared with the allocation
           -- every byte below the mark is one the file put there -- and
           closed again: the padding is not handed out, so nothing may
           read it afterwards. */
        XPOST_VG_UNPOISON_RANGE(mem->base, (unsigned int)adr,
                                (unsigned int)(end - adr));
        memset(xpost_vm_ptr(mem, (unsigned int)adr), 0, (size_t)(end - adr));
        if ((unsigned long long)sz < end - adr)
            XPOST_VG_POISON_RANGE(mem->base, (unsigned int)adr + sz,
                                  (unsigned int)(end - adr) - sz);
    }

    /* The bytes skipped to reach an aligned address are inside what the
       file holds and nothing ever writes them: no entity is located
       there and no reader has a reason to look. What that leaves is
       storage in the middle of the file whose contents are whatever the
       file's own storage arrived carrying -- zero where it was mapped,
       and whatever the host allocator last had in it where it was not.
       So the gaps are cleared as they are skipped, and every byte below
       the high-water mark is one the file put there. Opened to be
       written and closed again: the padding is not handed out, so
       nothing may read it afterwards either. */
    if (adr > mem->high_water)
    {
        unsigned int gap = (unsigned int)adr - mem->high_water;

        /* An allocation of nothing is not grown for, so the alignment it
           steps over is only the file's to clear as far as the file
           reaches. */
        if (mem->high_water + gap > mem->max)
            gap = mem->max - mem->high_water;

        XPOST_VG_UNPOISON_RANGE(mem->base, mem->high_water, gap);
        memset(xpost_vm_ptr(mem, mem->high_water), 0, gap);
        XPOST_VG_POISON_RANGE(mem->base, mem->high_water, gap);
    }

    mem->high_water = (unsigned int)end;
    *retaddr = (unsigned int)adr;
    return 1;
}

void
xpost_memory_file_dump(const Xpost_Memory_File *mem)
{
    if (!mem)
    {
        XPOST_LOG_ERR("%d mem pointer is NULL", VMerror);
        return;
    }

    XPOST_LOG_DUMP("{mfile: base = %p, "
            "used = 0x%x (%u), "
            "max = 0x%x (%u), "
            "start = %d}\n",
            mem->base,
            mem->high_water, mem->high_water,
            mem->max, mem->max,
            mem->start);

    /* The header above is the whole of what this reports. A
       hex-and-character dump of every byte the file holds would follow
       it, and is not made: a memory file runs to megabytes and the
       header is what a reader of a log wants. */

    XPOST_LOG_DUMP("\n");
}


/*
 * allocate and initialize a memory table data structure
 */
XPOST_TEST_VISIBLE int
xpost_memory_table_init(Xpost_Memory_File *mem, unsigned int nspecials)
{
    unsigned int i, ent;

    mem->table.tab = malloc( (mem->table.max = 1000) * sizeof(*mem->table.tab));
    if (!mem->table.tab)
    {
        XPOST_LOG_ERR("%d unable to initialize memory table", VMerror);
        return 0;
    }
    mem->table.nextent = 0;
    mem->table.freerow = 0;
    mem->ent_reserve_open = 0;
    mem->ent_exhausted = 0;

    /* the specials take the foot of the table, in the order the
       enumerators name them */
    for (i = 0; i < nspecials; i++)
    {
        if (!xpost_memory_table_alloc(mem, 0, 0, &ent))
        {
            XPOST_LOG_ERR("%d unable to reserve the special entity slots",
                          VMerror);
            return 0;
        }
        if (ent != i)
        {
            XPOST_LOG_ERR("%d slot %u was taken before the specials were "
                          "reserved", VMerror, ent);
            return 0;
        }
    }
    return 1;
}


/* install free-list function into memory file */
int
xpost_memory_register_free_list_alloc_function(
    Xpost_Memory_File *mem,
    int (*free_list_alloc)(struct Xpost_Memory_File *mem, unsigned int sz, unsigned int tag, unsigned int *entity))
{
    mem->free_list_alloc = free_list_alloc;
    mem->free_list_alloc_is_installed = 1;
    return 1;
}

/* install garbage-collect function into memory file */
int
xpost_memory_register_garbage_collect_function(
    Xpost_Memory_File *mem,
    int (*garbage_collect)(struct Xpost_Memory_File *mem, int dosweep, int markall))
{
    mem->garbage_collect = garbage_collect;
    mem->garbage_collect_is_installed = 1;
    /* automatic collection reclaims this bank unless a program says
       otherwise through vmreclaim */
    mem->garbage_collect_auto = 1;
    return 1;
}

/*
   allocate sz bytes as an 'ent' in the memory table
   */
static int
_xpost_memory_table_alloc_new(Xpost_Memory_File *mem,
                              unsigned int sz,
                              unsigned int tag,
                              unsigned int *entity)
{
    unsigned int ent;
    unsigned int adr;
    unsigned int last;

    if (!mem)
    {
        XPOST_LOG_ERR("%d mem pointer is NULL", VMerror);
        return 0;
    }

    /* The end of the range belongs to the machinery that reports
       reaching it, and is out of reach until the interpreter opens it
       (see XPOST_MEMORY_TABLE_ENT_RESERVE). It shuts again of its own
       accord here, where a slot outside it is being handed out: that is
       the run allocating with room to spare, which is the whole of what
       the reserve was waiting for. */
    /* A row released by a rearrangement of the arena describes nothing
       and is on no block free list, so nothing else can offer it. Taken
       before the cursor is touched, since the whole reason it is here is
       that the cursor cannot come back down. */
    if (mem->table.freerow)
    {
        ent = mem->table.freerow;
        mem->table.freerow = mem->table.tab[ent].nextfree;
        if (!xpost_memory_file_alloc(mem, sz, &adr))
        {
            /* put it back rather than lose the number to a failed
               request: the row is still one nothing else can offer */
            mem->table.tab[ent].nextfree = mem->table.freerow;
            mem->table.freerow = ent;
            XPOST_LOG_ERR("%d unable to allocate entity data storage", VMerror);
            return 0;
        }
        mem->table.tab[ent].adr = adr;
        mem->table.tab[ent].sz = sz;
        mem->table.tab[ent].tag = tag;
        mem->table.tab[ent].mark = 0;
        mem->table.tab[ent].nextfree = 0;
        *entity = ent;
        return 1;
    }

    last = XPOST_OBJECT_COMP_MAX_ENT - XPOST_MEMORY_TABLE_ENT_RESERVE;

    ent = mem->table.nextent;
    if (ent <= last)
        mem->ent_reserve_open = 0;
    else if (mem->ent_reserve_open)
        last = XPOST_OBJECT_COMP_MAX_ENT;

    if (ent > last)
    {
        /* an ent number beyond the object field width would be silently
           truncated when stored in an object, aliasing another entity.
           The width is an implementation limit and the memory behind it
           is not spent, so this is limitcheck and not VMerror; the flag
           carries that distinction to where the error is raised. */
        mem->ent_exhausted = 1;
        XPOST_LOG_ERR("%d entity numbers exhausted (%u of a possible %u)",
                limitcheck, ent, XPOST_OBJECT_COMP_MAX_ENT);
        return 0;
    }
    if (!xpost_memory_file_alloc(mem, sz, &adr))
    {
        XPOST_LOG_ERR("%d unable to allocate entity data storage", VMerror);
        return 0;
    }

    /* The slot is filled before it is counted. Everything that walks the
       table walks the slots below the count, reading each one's fields,
       so a slot the count reaches holds what those readers read: the
       storage the table is kept in is not cleared when it grows, and a
       slot counted before it was written holds whatever the allocator
       handed back.

       Counting it after also keeps the grow below on the one path that
       needs it. The table is grown when the count reaches its capacity,
       and a request that gave up between the two left the count raised
       and the grow unreached, so the next slot to be handed out was one
       past the end of the table. */
    mem->table.tab[ent].adr = adr;
    mem->table.tab[ent].sz = sz;
    mem->table.tab[ent].tag = tag;
    mem->table.tab[ent].mark = 0;
    ++mem->table.nextent;

    if (mem->table.nextent == mem->table.max)
    {
        void *tmp = realloc(mem->table.tab, (mem->table.max*=2) * sizeof(*mem->table.tab));
        if (!tmp)
        {
            XPOST_LOG_ERR("%d unable to grow memory table", VMerror);
            mem->table.max/=2;
            return 0;
        }
        mem->table.tab = tmp;
    }

    *entity = ent;
    return 1;
}

/* Hand back the row of an entity whose storage has gone.

   A rearrangement of the arena slides the live entities down over the
   free blocks. A block it slid over no longer has bytes, so its row
   describes nothing and belongs on no block free list -- and a row on no
   list is a number that can never be issued again, which would let a job
   that fragments and compacts repeatedly spend the whole entity range
   without spending memory.

   The row is cleared here rather than by the caller, so that "a released
   row describes nothing" is true by construction at the one place a row
   is released, and not a thing each caller is trusted to have done. A
   row that still holds storage is refused: that one belongs on a block
   free list, where the bytes are kept for reuse as well as the number,
   and accepting it here would drop them. */
int xpost_memory_table_release_row(Xpost_Memory_File *mem, unsigned int ent)
{
    if (!mem || !mem->table.tab)
        return 0;
    if (ent == 0 || ent >= mem->table.nextent)
        return 0;
    if (mem->table.tab[ent].sz != 0)
        return 0;

    mem->table.tab[ent].adr = 0;
    mem->table.tab[ent].used = 0;
    mem->table.tab[ent].tag = 0;
    mem->table.tab[ent].mark = 0;
    mem->table.tab[ent].nextfree = mem->table.freerow;
    mem->table.freerow = ent;
    return 1;
}

/* The part of an entity's block that its occupant does not cover.

   A block comes off the free list whole, so one handed to a smaller
   request keeps the rest of whatever was there before -- and what was
   there before may be a structure this process filled with its own
   addresses. Nothing reads those bytes through the entity, whose length
   is what it was allocated for; everything that reads virtual memory
   whole does, and reads them as part of what the memory holds.

   Cleared as the block is handed out, which is the moment the previous
   tenant stops being the answer to what is in it. Nothing where the
   block is fresh: an allocation the file has just grown into is exactly
   as long as it was asked for. */
static void _clear_slack(Xpost_Memory_File *mem, unsigned int ent)
{
    unsigned int used = mem->table.tab[ent].used;
    unsigned int sz = mem->table.tab[ent].sz;

    if (sz > used)
        memset(xpost_vm_ptr(mem, mem->table.tab[ent].adr + used), 0,
               sz - used);
}

/* An entity number is about to name contents of somebody else's making.
   Anything holding a conclusion about what it named before is dropped
   here.

   This is the one place a number is ever handed out -- from the free
   list and on a fresh slot alike -- which is why the conclusions are
   dropped here rather than where a number is released. Numbers are
   released from more than one place: xpost_free.c releases one at a
   time, and the collector's sweep splices whole runs of them onto the
   free list itself, so a release is a place to be forgotten. A release
   the conclusion outlives is not the danger in any case. The storage
   still holds the bytes that were examined -- the free list writes no
   word of it -- and nothing can offer a released entity for
   examination: what let it be reclaimed was that nothing referred to
   it. What makes a conclusion wrong is somebody else
   writing the storage, and that begins here. */
static void
_ent_issued(Xpost_Memory_File *mem, unsigned int ent)
{
    if (mem->path_walk.ent == ent)
        mem->path_walk.ent = 0;
}

/*
   allocate sz bytes in the memory table, using free-list if installed,
   possibly calling garbage collector, if installed
   */
XPOST_TEST_VISIBLE int
xpost_memory_table_alloc(Xpost_Memory_File *mem,
                         unsigned int sz,
                         unsigned int tag,
                         unsigned int *entity)
{
    int ret;

    /* the flag describes this request, so it says nothing about any
       earlier one */
    mem->ent_exhausted = 0;

    if (mem->free_list_alloc_is_installed)
    {
        /* Entity slots are a budget of their own, independent of the
           byte threshold: a table grown this large is worth a collection
           whatever the bytes behind it come to, and where the object
           field spans no more than the table, allocation fails outright
           once the numbers run out.

           What paces the requests is a count of allocations, spent on
           every one of them -- from the free list as well as on a fresh
           slot -- so that a job whose garbage is reclaimed is asked at
           the same rate as one whose is not, and its table stops
           growing. A pace read off the next-slot cursor instead would
           not: that cursor only ever rises, so each request would set
           the next one further out and a job allocating steadily would
           be offered a fixed number of collections however much each
           one reclaimed. */
        if (mem->garbage_collect_is_installed &&
            !mem->interpreter_get_initializing() &&
            mem->table.nextent > XPOST_MEMORY_TABLE_PRESSURE)
        {
            if (mem->gc_ent_budget == 0)
            {
                mem->garbage_collect_pending = 1;
                mem->gc_ent_budget = XPOST_MEMORY_TABLE_GC_BUDGET;
            }
            else
                --mem->gc_ent_budget;
        }

        ret = mem->free_list_alloc(mem, sz, tag, entity);
        if (ret == 1)
        {
            _ent_issued(mem, *entity);
            mem->table.tab[*entity].used = sz;
            /* the link means nothing once the entity is live, so a live
               row carries the one value rather than whatever the entity
               was linked to when it was last free */
            mem->table.tab[*entity].nextfree = 0;
            _clear_slack(mem, *entity);
            return 1;
        }
        else if (ret == XPOST_FREE_WANT_COLLECTION)
        {
            /* collection is due, but running it here would sweep any
               object the current operator holds only in C variables
               (invisible to the root set). Record the request; the
               interpreter collects at its safe point between operator
               executions, where the stacks are the complete roots. */
            if (mem->garbage_collect_is_installed &&
                    !mem->interpreter_get_initializing())
                mem->garbage_collect_pending = 1;
        }
    }
    ret = _xpost_memory_table_alloc_new(mem, sz, tag, entity);
    if (!ret)
        return 0; /* *entity is not valid on failure */
    /* a fresh slot is a number never handed out before, so there is
       nothing held about it; told all the same, so that the rule is
       "every number is announced where it is issued" and not a rule
       with a case in it */
    _ent_issued(mem, *entity);
    mem->table.tab[*entity].used = sz;
    mem->table.tab[*entity].nextfree = 0;
    return ret;
}

int
xpost_memory_table_alloc_special(Xpost_Memory_File *mem,
                                 unsigned int sz,
                                 unsigned int tag,
                                 unsigned int want,
                                 unsigned int *entity)
{
    unsigned int adr;

    if (want >= mem->table.nextent)
    {
        XPOST_LOG_ERR("%d slot %u was never reserved: the reservation runs "
                      "before any constructor and this one is outside it",
                      VMerror, want);
        return 0;
    }
    if (mem->table.tab[want].adr != 0 || mem->table.tab[want].sz != 0)
    {
        XPOST_LOG_ERR("%d slot %u was built twice", VMerror, want);
        return 0;
    }

    /* The row is already there; what it needs is storage. Taken straight
       off the file, which leaves nothing undescribed: this row is the
       description, and giving the special a row of its own is what the
       reservation is for. */
    if (sz)
    {
        if (!xpost_memory_file_alloc(mem, sz, &adr))
            return 0;
        mem->table.tab[want].adr = adr;
        mem->table.tab[want].sz = sz;
        mem->table.tab[want].used = sz;
    }
    mem->table.tab[want].tag = tag;
    *entity = want;
    return 1;
}



#define CHECK_VALID_ENT(ent,mem,ret) \
    if (!xpost_ent_valid(mem, ent)) \
    { \
        XPOST_LOG_ERR("%d entity not found %u", VMerror, ent); \
        return ret; \
    }

/* get the address of an allocation from the memory table */
int
xpost_memory_table_get_addr(Xpost_Memory_File *mem,
                            unsigned int ent,
                            unsigned int *retaddr)
{
    CHECK_VALID_ENT(ent,mem,0)
    *retaddr = mem->table.tab[ent].adr;
    return 1;
}

/* change the address of an allocation in the memory table */
int xpost_memory_table_set_addr(Xpost_Memory_File *mem,
                                unsigned int ent,
                                unsigned int setaddr)
{
    CHECK_VALID_ENT(ent,mem,0)
    mem->table.tab[ent].adr = setaddr;
    return 1;
}


/* get the size of an allocation from the memory table */
int
xpost_memory_table_get_size(Xpost_Memory_File *mem,
                            unsigned int ent,
                            unsigned int *sz)
{
    CHECK_VALID_ENT(ent,mem,0)
    *sz = mem->table.tab[ent].sz;
    return 1;
}

/* set the size of an allocation in the memory table */
int
xpost_memory_table_set_size(Xpost_Memory_File *mem,
                            unsigned int ent,
                            unsigned int size)
{
    CHECK_VALID_ENT(ent,mem,0)
    mem->table.tab[ent].sz = size;
    return 1;
}

/* get the mark field of an allocation from the memory table */
int
xpost_memory_table_get_mark(Xpost_Memory_File *mem,
                            unsigned int ent,
                            unsigned int *retmark)
{
    CHECK_VALID_ENT(ent,mem,0)
    *retmark = mem->table.tab[ent].mark;
    return 1;
}


/* change the mark field of an allocation in the memory table */
int
xpost_memory_table_set_mark(Xpost_Memory_File *mem,
                            unsigned int ent,
                            unsigned int setmark)
{
    CHECK_VALID_ENT(ent,mem,0)
    mem->table.tab[ent].mark = setmark;
    return 1;
}


/* get the tag field of an allocation from the memory table */
int
xpost_memory_table_get_tag(Xpost_Memory_File *mem,
                           unsigned int ent,
                           unsigned int *tag)
{
    CHECK_VALID_ENT(ent,mem,0)
    *tag = mem->table.tab[ent].tag;
    return 1;
}

/* change the tag field of an allocation in the memory table */
int
xpost_memory_table_set_tag(Xpost_Memory_File *mem,
                           unsigned int ent,
                           unsigned int tag)
{
    CHECK_VALID_ENT(ent,mem,0)
    mem->table.tab[ent].tag = tag;
    return 1;
}


/* get sz bytes at offset*sz from a memory allocation */
XPOST_TEST_VISIBLE int
xpost_memory_get(Xpost_Memory_File *mem,
                 unsigned int ent,
                 unsigned int offset,
                 unsigned int sz,
                 void *dest)
{
    CHECK_VALID_ENT(ent,mem,0)

    /* offset is an index added to the composite's base; compute the bound in
       64 bits so offset*sz cannot wrap a 32-bit unsigned past the check */
    if ((unsigned long long)offset * sz + sz > mem->table.tab[ent].sz)
    {
        XPOST_LOG_ERR("%d out of bounds memory %u * %u > %u", rangecheck,
                offset, sz, mem->table.tab[ent].sz);
        return 0;
    }

    memcpy(dest, (unsigned char *)xpost_ent_ptr(mem, ent) + offset * sz, sz);
    return 1;
}

/* put sz bytes at offset*sz in a memory allocation */
XPOST_TEST_VISIBLE int
xpost_memory_put(Xpost_Memory_File *mem,
                 unsigned int ent,
                 unsigned int offset,
                 unsigned int sz,
                 const void *src)
{
    CHECK_VALID_ENT(ent,mem,0)

    if ((unsigned long long)offset * sz + sz > mem->table.tab[ent].sz)
    {
        XPOST_LOG_ERR("%d out of bounds memory %u * %u > %u", rangecheck,
                offset, sz, mem->table.tab[ent].sz);
        return 0;
    }

    memcpy((unsigned char *)xpost_ent_ptr(mem, ent) + offset * sz, src, sz);
    return 1;
}


void
xpost_memory_table_dump_ent(Xpost_Memory_File *mem,
                            unsigned int ent)
{
    unsigned int u;
    unsigned int i = ent;
    unsigned int e = ent;
    CHECK_VALID_ENT(ent,mem,)
    XPOST_LOG_DUMP("ent %d (%d): "
            "adr %u 0x%04x, "
            "sz [%u], "
            "mark %s rfct %d llev %d tlev %d\n",
            e, i,
            mem->table.tab[i].adr, mem->table.tab[i].adr,
            mem->table.tab[i].sz,
            mem->table.tab[i].mark
                & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK ? "#" : "_",
            (mem->table.tab[i].mark
                & XPOST_MEMORY_TABLE_MARK_DATA_REFCOUNT_MASK)
                >> XPOST_MEMORY_TABLE_MARK_DATA_REFCOUNT_OFFSET,
            (mem->table.tab[i].mark
                & XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK)
                >> XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET,
            (mem->table.tab[i].mark
                & XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_MASK)
                >> XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_OFFSET);
        for (u = 0; u < mem->table.tab[i].sz; u++)
        {
            XPOST_LOG_DUMP(" %02x%c",
                    mem->base[ mem->table.tab[i].adr + u ],
                    isprint(mem->base[ mem->table.tab[i].adr + u]) ?
                        mem->base[ mem->table.tab[i].adr + u ] :
                        ' ');
        }
}

void
xpost_memory_table_dump(const Xpost_Memory_File *mem)
{
    unsigned int i;
    unsigned int e = 0;

    XPOST_LOG_DUMP("nextent: %u\n", mem->table.nextent);
    for (i = 0; i < mem->table.nextent; i++, e++)
    {
        unsigned int u;
        XPOST_LOG_DUMP("ent %d (%d): "
                "adr %u 0x%04x, "
                "sz [%u], "
                "mark %s rfct %d llev %d tlev %d\n",
                e, i,
                mem->table.tab[i].adr, mem->table.tab[i].adr,
                mem->table.tab[i].sz,
                mem->table.tab[i].mark
                    & XPOST_MEMORY_TABLE_MARK_DATA_MARK_MASK ? "#" : "_",
                (mem->table.tab[i].mark
                    & XPOST_MEMORY_TABLE_MARK_DATA_REFCOUNT_MASK)
                    >> XPOST_MEMORY_TABLE_MARK_DATA_REFCOUNT_OFFSET,
                (mem->table.tab[i].mark
                    & XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK)
                    >> XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET,
                (mem->table.tab[i].mark
                    & XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_MASK)
                    >> XPOST_MEMORY_TABLE_MARK_DATA_TOPLEVEL_OFFSET);
        for (u = 0; u < mem->table.tab[i].sz; u++)
        {
            XPOST_LOG_DUMP(" %02x%c",
                    mem->base[ mem->table.tab[i].adr + u ],
                    isprint(mem->base[ mem->table.tab[i].adr + u]) ?
                        mem->base[ mem->table.tab[i].adr + u ] :
                        ' ');
        }
        XPOST_LOG_DUMP("\n");
    }
}
