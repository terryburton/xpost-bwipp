/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>

#ifdef HAVE_ZLIB
# include <zlib.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_LIBJPEG
# include <jpeglib.h>
# include <setjmp.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif

#include "xpost.h"
#include "xpost_log.h"
#include "xpost_compat.h"
#include "xpost_memory.h"  /* a file entity lives in the (local) mfile */
#include "xpost_object.h"
#include "xpost_handle.h"  /* a file entity carries a handle on its stream */
#include "xpost_stack.h"  /* files are objects */
#include "xpost_context.h"

#include "xpost_error.h"  /* file functions may throw errors */
#include "xpost_file.h"  /* double-check prototypes */
#include "xpost_strbuf.h"  /* a rereadable file captures its source in one */
#include "xpost_string.h"  /* a procedure stream hands over its bytes in one */
#include "xpost_interpreter.h"  /* a procedure stream runs the procedure */

/* --- file-access sandbox -------------------------------------------------
   A process-wide, one-way latch. Before engaging, disk access is
   unrestricted; once engaged, an open by the running program is denied
   unless the path resolves within a permitted directory. This is defence
   in depth around the operating-system confinement of the host process,
   not a substitute for it. Resource-file loading is separately confined
   (see xpost_diskfile_fopen_beneath) and does not consult this.

   The latch and the permitted set are the process's. Every context in the
   process is confined by the same latch and reaches the same directories,
   which is also how a context created after the latch engaged reads its
   own start-up files: those are permitted by whichever context reached
   that directory first. So this confines the process against the program
   it runs; it does not divide one job in the process from another. */

#define XPOST_PATH_PERMIT_MAX 64

/* Where a program may reach, and where a permit names one file rather
   than a tree, that file's leaf beside it.

   The leaf is what lets an output file be permitted without permitting
   everything beside it: a run given -o /dev/null would otherwise have
   been handed the whole of /dev, because a directory was the only thing
   this table could say.

   Two arrays rather than an array of pairs, and deliberately: a static
   that can be made to name something says so with a star in its own
   declaration, which is what tests/check-library-lifetime.sh derives
   its population from and what tells a reader where the storage is
   owned. A pointer hidden one level down inside a struct type is
   invisible to both. */
static char *xpost_permit_read_dir[XPOST_PATH_PERMIT_MAX];
static char *xpost_permit_read_leaf[XPOST_PATH_PERMIT_MAX];
static int xpost_permit_read_cnt = 0;
static char *xpost_permit_write_dir[XPOST_PATH_PERMIT_MAX];
static char *xpost_permit_write_leaf[XPOST_PATH_PERMIT_MAX];
static int xpost_permit_write_cnt = 0;
static int xpost_path_control_engaged = 0;

/* Index of the permitted entry that contains the canonical path `full`, or
   -1 if none does. A permitted directory contains `full` when it is a prefix
   ending at a path separator (or the whole of `full`).

   A root that is itself a separator -- the filesystem root, or a drive's --
   already ends at one, and the byte after the prefix is the first of the
   name within it rather than the separator between them. Comparing the
   short prefix instead is what lets such a root contain anything: matched
   whole, "/" is a prefix of every absolute path and the byte after it is
   never a separator, so a root left as it stands would contain nothing at
   all and permit nothing. */
static const char *xpost_path_after_root(const char *full, const char *root);

static int
xpost_path_within_idx(const char *full, char *const *dirs,
                      char *const *leaves, int cnt)
{
    int i;

    for (i = 0; i < cnt; i++)
    {
        size_t rl = strlen(dirs[i]);

        while (rl > 1 && (dirs[i][rl - 1] == '/'
#ifdef _WIN32
                          || dirs[i][rl - 1] == '\\'
#endif
                         ))
            rl--;
        if (rl == 1 && (dirs[i][0] == '/'
#ifdef _WIN32
                        || dirs[i][0] == '\\'
#endif
                       ))
            rl = 0;

#ifdef _WIN32
        /* Windows paths are case-insensitive and GetFullPathName yields
           backslash separators (mirrors the beneath-root check) */
        if (_strnicmp(full, dirs[i], rl) != 0 ||
            (full[rl] != '\\' && full[rl] != '/' && full[rl] != '\0'))
            continue;
#else
        if (strncmp(full, dirs[i], rl) != 0 ||
            (full[rl] != '/' && full[rl] != '\0'))
            continue;
#endif
        /* An entry naming one file admits that file and nothing else
           beside it. Keep scanning rather than refusing here: a broader
           entry later in the table may cover this path legitimately, and
           which entry was written first is not a policy. */
        if (leaves[i] &&
            strcmp(xpost_path_after_root(full, dirs[i]), leaves[i]) != 0)
            continue;
        return i;
    }
    return -1;
}

/* Permit a directory tree, answering whether it is permitted afterwards.

   A directory the table already covers is answered yes without an entry:
   the check above is a containment scan, so a second entry inside the
   first decides nothing the first does not. That is what makes a permit
   idempotent, and what a process asking once per context for the same
   start-up directory relies on -- a table that spent an entry per ask
   would fill, and a full table permits nothing at all.

   Anything else needs an entry, which a frozen or a full table cannot
   give. Then the answer is no, and it is said aloud as well as returned:
   a caller that believes it has configured a sandbox and has not is
   worse off than one that is told. */
static int
xpost_path_permit_add(char **dirs, char **leaves, int *cnt,
                      const char *dir, const char *leaf)
{
    char *rp = xpost_realpath(dir);
    char *lf = NULL;

    if (!rp)
    {
        XPOST_LOG_ERR("cannot resolve the directory to permit: %s", dir);
        return 0;
    }
    /* Already covered, and the question is asked of the thing being
       permitted rather than of its directory: a file permit is covered
       by a tree that holds it, and a tree is not covered by a permit for
       one file inside it. */
    {
        char probe[XPOST_PATH_MAX];
        const char *ask = rp;

        if (leaf && snprintf(probe, sizeof probe, "%s/%s", rp, leaf)
                    < (int)sizeof probe)
            ask = probe;
        if (xpost_path_within_idx(ask, dirs, leaves, *cnt) >= 0)
        {
            free(rp);
            return 1;
        }
    }
    if (leaf && !(lf = strdup(leaf)))
    {
        free(rp);
        return 0;
    }
    if (xpost_path_control_engaged) /* the permit set is frozen once engaged */
    {
        XPOST_LOG_ERR("file access is engaged: %s cannot be permitted now", rp);
        free(rp);
        free(lf);
        return 0;
    }
    if (*cnt >= XPOST_PATH_PERMIT_MAX)
    {
        XPOST_LOG_ERR("no room beside the %d permitted directories for %s",
                      XPOST_PATH_PERMIT_MAX, rp);
        free(rp);
        free(lf);
        return 0;
    }
    dirs[*cnt] = rp;
    leaves[*cnt] = lf;
    ++*cnt;
    return 1;
}

int
xpost_path_permit_read(const char *dir)
{
    return xpost_path_permit_add(xpost_permit_read_dir,
                                 xpost_permit_read_leaf,
                                 &xpost_permit_read_cnt, dir, NULL);
}

int
xpost_path_permit_write(const char *dir)
{
    return xpost_path_permit_add(xpost_permit_write_dir,
                                 xpost_permit_write_leaf,
                                 &xpost_permit_write_cnt, dir, NULL);
}

/* Permit exactly one file for writing: the directory it sits in, with
   that file named, so nothing else beside it is granted.

   A path with no separator is a file in the current directory. A path
   whose leaf cannot be told from its directory is refused rather than
   widened, since answering the wider question is the mistake this
   exists to avoid. */
int
xpost_path_permit_write_file(const char *path)
{
    char buf[XPOST_PATH_MAX];
    char *sep;

    if (!path || !*path || strlen(path) >= sizeof buf)
        return 0;
    strcpy(buf, path);
    sep = strrchr(buf, '/');
#ifdef _WIN32
    {
        char *bs = strrchr(buf, '\\');

        if (bs && (!sep || bs > sep))
            sep = bs;
    }
#endif
    if (sep)
    {
        *sep = '\0';
        if (!buf[0])
            strcpy(buf, "/");
        if (!sep[1])
            return 0; /* names a directory, not a file in one */
        return xpost_path_permit_add(xpost_permit_write_dir,
                                     xpost_permit_write_leaf,
                                     &xpost_permit_write_cnt, buf, sep + 1);
    }
    return xpost_path_permit_add(xpost_permit_write_dir,
                                 xpost_permit_write_leaf,
                                 &xpost_permit_write_cnt, ".", buf);
}

void
xpost_path_control_engage(void)
{
    xpost_path_control_engaged = 1;
}

/* Resolve `path` to an absolute, symlink-free target in `buf`. An existing
   path resolves directly; for a not-yet-existent write target the parent is
   resolved and the leaf reattached, so a symlinked access directory (e.g.
   /tmp -> /private/tmp) still lands on its canonical form. Returns 1 on
   success, 0 when the path (or its parent, for a create) cannot be resolved. */
static int
xpost_path_canonical_target(const char *path, int write, char *buf, size_t buflen)
{
    char *canon = xpost_realpath(path);

    if (canon)
    {
        int ok = strlen(canon) < buflen;

        if (ok)
            strcpy(buf, canon);
        free(canon);
        return ok;
    }

    /* the path does not resolve: only a create (write) is meaningful */
    if (!write)
        return 0;
    {
        char tmp[XPOST_PATH_MAX];
        char *sep;
        char *cdir;
        const char *parent;
        const char *base;
        int ok;

        if (strlen(path) >= sizeof tmp)
            return 0;
        strcpy(tmp, path);
        sep = strrchr(tmp, '/');
#ifdef _WIN32
        /* accept either separator when splitting off the leaf */
        {
            char *bs = strrchr(tmp, '\\');

            if (bs && (!sep || bs > sep))
                sep = bs;
        }
#endif
        if (sep)
        {
            *sep = '\0';
            parent = tmp[0] ? tmp : "/";
            base = sep + 1;
        }
        else
        {
            parent = ".";
            base = tmp;
        }
        cdir = xpost_realpath(parent);
        if (!cdir)
            return 0;
        ok = snprintf(buf, buflen, "%s/%s", cdir, base) < (int)buflen;
        free(cdir);
        return ok;
    }
}

/* Is opening `path` (for writing when `write`) permitted? Kept for the
   filesystem-control operations (delete/rename/enumerate) that decide access
   from a name rather than an opened descriptor. */
static int
xpost_path_permitted(const char *path, int write)
{
    char full[XPOST_PATH_MAX];

    if (!xpost_path_canonical_target(path, write, full, sizeof full))
        return 0;
    return xpost_path_within_idx(full,
               write ? xpost_permit_write_dir : xpost_permit_read_dir,
               write ? xpost_permit_write_leaf : xpost_permit_read_leaf,
               write ? xpost_permit_write_cnt : xpost_permit_read_cnt) >= 0;
}

/* map an fopen/openat2 errno to a PostScript file error */
static int
xpost_fopen_errno(int e)
{
    switch (e)
    {
        case EACCES:
#ifdef EPERM
        case EPERM:
#endif
#ifdef ELOOP
        case ELOOP:
#endif
#ifdef EXDEV
        case EXDEV:
#endif
            return invalidfileaccess;
        case ENOENT:
#ifdef ENOTDIR
        case ENOTDIR:
#endif
            return undefinedfilename;
#ifdef EMFILE
        case EMFILE:      /* this process may open no more */
#endif
#ifdef ENFILE
        case ENFILE:      /* the system may open no more */
#endif
#if defined(EMFILE) || defined(ENFILE)
            /* "If the number of files opened by the current context
               exceeds an implementation limit, a limitcheck error
               occurs" (PLRM 8.2, file) */
            return limitcheck;
#endif
#ifdef ENAMETOOLONG
        case ENAMETOOLONG:
            /* the length a file system will take a name in is one of its
               limits, and limitcheck is "an implementation limit has
               been exceeded" (PLRM 8.2). It is what the confined opener
               beside this answers for the same condition, so a program
               is told the same thing about the same name whichever one
               it reached. */
            return limitcheck;
#endif
        default:
            /* "If an environment-dependent error is detected, an ioerror
               occurs" (PLRM 8.2, file) */
            return ioerror;
    }
}

/* The sole fopen call: every disk open the interpreter performs funnels
   here, whether or not the sandbox is engaged. */
static FILE *
xpost_raw_fopen(const char *path, const char *mode, int *err)
{
    char bmode[8];
    FILE *fp;

    /* PostScript files are binary byte streams; force binary mode so that
       Windows text translation -- CRLF rewriting and a 0x1A byte read as
       end-of-file -- cannot corrupt or truncate them. On POSIX 'b' is a
       no-op. */
    if (!strchr(mode, 'b'))
    {
        size_t n = strlen(mode);

        if (n + 1 < sizeof bmode)
        {
            memcpy(bmode, mode, n);
            bmode[n] = 'b';
            bmode[n + 1] = '\0';
            mode = bmode;
        }
    }
    fp = fopen(path, mode);

    if (!fp)
    {
        *err = xpost_fopen_errno(errno);
        return NULL;
    }
    *err = 0;
    return fp;
}

/* Advance past the separator(s) joining a permitted root to the path within
   it, given a canonical `full` known to sit inside `root`. */
static const char *
xpost_path_after_root(const char *full, const char *root)
{
    const char *rel = full + strlen(root);

    while (*rel == '/'
#ifdef _WIN32
           || *rel == '\\'
#endif
          )
        rel++;
    return rel;
}

/* Open a program-driven path under the engaged sandbox without a
   check-then-open race. The target's permitted root is identified, then the
   open is anchored there and resolved atomically beneath it by the kernel
   (openat2), so a path repointed after the check cannot escape.

   Where that primitive is unavailable the same decision is reached without
   the kernel's help, and the order the three steps are taken in is what
   makes it sound. What is opened is the canonical target the check
   approved, never the name the program supplied, so no name is resolved
   twice. A canonical target whose last component is itself a symbolic link
   is refused before anything is opened: that is the one target
   canonicalisation cannot resolve -- a link with nothing at the end of it
   canonicalises to the link's own place, which is inside the permitted
   tree, while opening it for writing would create the file it points at,
   which need not be. It is also exactly what the kernel refuses on the
   atomic path, so both routes accept the same targets. Then the opened
   descriptor's real location is checked, which catches a swap made between
   the check and the open; a platform that can report a descriptor's
   location but did not report this one refuses, since the reason it could
   not is the reason to be careful. */
static FILE *
xpost_confined_fopen(const char *path, const char *mode, int write, int *err)
{
    char full[XPOST_PATH_MAX];
    char *const *dirs = write ? xpost_permit_write_dir : xpost_permit_read_dir;
    char *const *leaves = write ? xpost_permit_write_leaf : xpost_permit_read_leaf;
    int cnt = write ? xpost_permit_write_cnt : xpost_permit_read_cnt;
    int idx;
    int access;
    int supported;
    const char *rel;
    FILE *fp;

    if (!xpost_path_canonical_target(path, write, full, sizeof full) ||
        (idx = xpost_path_within_idx(full, dirs, leaves, cnt)) < 0)
    {
        *err = invalidfileaccess;
        return NULL;
    }

    /* the portion of the canonical target beyond the permitted root is what
       is resolved beneath that root */
    rel = xpost_path_after_root(full, dirs[idx]);
    if (!*rel) /* the permitted directory itself, not a file within it */
    {
        *err = invalidfileaccess;
        return NULL;
    }

    access = 0;
    if (strchr(mode, '+')) access |= XPOST_OPEN_WRITE | XPOST_OPEN_RDWR;
    else if (write)        access |= XPOST_OPEN_WRITE;
    if (strchr(mode, 'w')) access |= XPOST_OPEN_CREATE | XPOST_OPEN_TRUNC;
    if (strchr(mode, 'a')) access |= XPOST_OPEN_CREATE | XPOST_OPEN_APPEND;

    fp = xpost_openat2_beneath(dirs[idx], rel, mode, access, &supported);
    if (supported)
    {
        if (!fp)
            *err = xpost_fopen_errno(errno);
        else
            *err = 0;
        return fp;
    }

    /* portable fallback */
    if (xpost_path_is_symlink(full))
    {
        *err = invalidfileaccess;
        return NULL;
    }
    fp = xpost_raw_fopen(full, mode, err);
    if (!fp)
        return NULL;
    {
        char idbuf[XPOST_PATH_MAX];
        int located = xpost_fd_realpath(fileno(fp), idbuf, sizeof idbuf);

        if (located == 0 ||
            (located > 0 && xpost_path_within_idx(idbuf, dirs, leaves, cnt) < 0))
        {
            fclose(fp);
            *err = invalidfileaccess;
            return NULL;
        }
    }
    *err = 0;
    return fp;
}

/* The single path-to-stream opener for disk-backed files: every disk file
   the interpreter opens is created here, so file-access policy has one
   enforcement point. internal marks a trusted interpreter-managed path
   (temporary scratch) rather than one derived from the running program;
   such opens bypass the sandbox. */
FILE *
xpost_diskfile_fopen(const char *path, const char *mode, int internal, int *err)
{
    if (!internal && xpost_path_control_engaged)
    {
        int write = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');

        return xpost_confined_fopen(path, mode, write, err);
    }

    return xpost_raw_fopen(path, mode, err);
}

/* deletefile and renamefile modify the filesystem at the target path(s)
   rather than opening a stream, so they do not pass through the opener
   above; route them through the same policy. Under the engaged sandbox
   each affected path must be write-permitted. */
int
xpost_diskfile_remove(const char *path, int *err)
{
    if (xpost_path_control_engaged)
    {
        char full[XPOST_PATH_MAX];
        int idx;
        const char *rel;
        int supported;
        int ret;

        if (!xpost_path_canonical_target(path, 1, full, sizeof full) ||
            (idx = xpost_path_within_idx(full, xpost_permit_write_dir,
                                         xpost_permit_write_leaf,
                                         xpost_permit_write_cnt)) < 0)
        {
            *err = invalidfileaccess;
            return -1;
        }
        rel = xpost_path_after_root(full, xpost_permit_write_dir[idx]);
        if (!*rel)
        {
            *err = invalidfileaccess;
            return -1;
        }
        /* delete relative to the parent resolved beneath the permitted root,
           so the name cannot be repointed after the check */
        ret = xpost_unlinkat_beneath(xpost_permit_write_dir[idx], rel,
                                     &supported);
        if (supported)
        {
            if (ret != 0)
            {
                *err = xpost_fopen_errno(errno);
                return -1;
            }
            *err = 0;
            return 0;
        }
        /* otherwise fall through: the name check above stands */
    }
    if (remove(path) != 0)
    {
        *err = xpost_fopen_errno(errno);
        return -1;
    }
    *err = 0;
    return 0;
}

int
xpost_diskfile_rename(const char *oldpath, const char *newpath, int *err)
{
    if (xpost_path_control_engaged)
    {
        char oldfull[XPOST_PATH_MAX];
        char newfull[XPOST_PATH_MAX];
        int oidx;
        int nidx;
        const char *orel;
        const char *nrel;
        int supported;
        int ret;

        if (!xpost_path_canonical_target(oldpath, 1, oldfull, sizeof oldfull) ||
            (oidx = xpost_path_within_idx(oldfull, xpost_permit_write_dir,
                                          xpost_permit_write_leaf,
                                          xpost_permit_write_cnt)) < 0 ||
            !xpost_path_canonical_target(newpath, 1, newfull, sizeof newfull) ||
            (nidx = xpost_path_within_idx(newfull, xpost_permit_write_dir,
                                          xpost_permit_write_leaf,
                                          xpost_permit_write_cnt)) < 0)
        {
            *err = invalidfileaccess;
            return -1;
        }
        orel = xpost_path_after_root(oldfull, xpost_permit_write_dir[oidx]);
        nrel = xpost_path_after_root(newfull, xpost_permit_write_dir[nidx]);
        if (!*orel || !*nrel)
        {
            *err = invalidfileaccess;
            return -1;
        }
        ret = xpost_renameat_beneath(xpost_permit_write_dir[oidx], orel,
                                     xpost_permit_write_dir[nidx], nrel,
                                     &supported);
        if (supported)
        {
            if (ret != 0)
            {
                *err = xpost_fopen_errno(errno);
                return -1;
            }
            *err = 0;
            return 0;
        }
        /* otherwise fall through: the name checks above stand */
    }
    if (rename(oldpath, newpath) != 0)
    {
        *err = xpost_fopen_errno(errno);
        return -1;
    }
    *err = 0;
    return 0;
}

/* May the running program see `path`? Directory enumeration is filtered to
   the files it could actually open, so a listing does not disclose names
   outside the permitted set. */
int
xpost_diskfile_readable(const char *path)
{
    return !xpost_path_control_engaged || xpost_path_permitted(path, 0);
}

/* Report on a named regular file for the string form of status. Returns 1 and
   fills the fields when the file exists and the path sandbox permits it, 0
   otherwise. bytes is the size; pages is an implementation-defined block count;
   referred and created are the access and modification times in seconds. */
int
xpost_diskfile_stat(const char *path, long long *pages, long long *bytes,
                    long long *referred, long long *created)
{
    struct stat st;

    if (xpost_path_control_engaged && !xpost_path_permitted(path, 0))
        return 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    /* The file system counts a size in its own width, which is not long
       everywhere: long is 32 bits on LLP64, so narrowing here would spend
       the quantity before whoever weighs it against the integer that must
       carry it ever sees it, and the check would be given a number that
       had already wrapped. Carry the full width out instead and let that
       check do the weighing. */
    *bytes = (long long)st.st_size;
    *pages = (long long)((st.st_size + 1023) / 1024);
    *referred = (long long)st.st_atime;
    *created = (long long)st.st_mtime;
    return 1;
}

/* Has the file-access sandbox been engaged? Environment access is refused
   once it has, since the environment is neither read nor written through
   the opener. */
int
xpost_path_control_is_engaged(void)
{
    return xpost_path_control_engaged;
}

/* Is s[0..len) a safe single path component ("leaf")? Externally-derived
   resource names are validated with this so they cannot express a path:
   rejected are separators of either platform, ':' (drive letter / NTFS
   stream), NUL and other control bytes, '.' and '..', a leading dot or
   space, a trailing dot or space (which Windows strips), and the reserved
   Windows device names. Bytes are otherwise restricted to [A-Za-z0-9._-].
   Returns 1 if safe, 0 otherwise. */
int
xpost_path_safe_leaf(const char *s, size_t len)
{
    static const char *const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    size_t i;
    size_t stem;
    size_t r;

    if (len == 0)
        return 0;
    if (s[0] == '.' || s[0] == ' ')                 /* leading dot ('.', '..',
                                                       hidden) or space */
        return 0;
    if (s[len - 1] == '.' || s[len - 1] == ' ')     /* trailing dot or space */
        return 0;
    for (i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7f)                  /* NUL and control bytes */
            return 0;
        if (c == '/' || c == '\\' || c == ':')      /* separators, drive/ADS */
            return 0;
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return 0;
    }
    /* reserved device name: the stem before the first '.', case-insensitive */
    for (stem = 0; stem < len && s[stem] != '.'; stem++)
        ;
    for (r = 0; r < sizeof reserved / sizeof reserved[0]; r++)
    {
        size_t rl = strlen(reserved[r]);
        size_t k;

        if (rl != stem)
            continue;
        for (k = 0; k < rl; k++)
        {
            unsigned char c = (unsigned char)s[k];

            if (c >= 'a' && c <= 'z')
                c = (unsigned char)(c - 32);
            if (c != (unsigned char)reserved[r][k])
                break;
        }
        if (k == rl)
            return 0;
    }
    return 1;
}

/* Open rel for reading beneath root, with the OS confining resolution to
   root (see xpost_open_beneath), mapping the failure to an error code.
   rel should already be composed of xpost_path_safe_leaf components. Under
   the engaged sandbox root must be a read-permitted directory: it is caller-
   supplied, so confinement beneath it is not itself a permit boundary. */
FILE *
xpost_diskfile_fopen_beneath(const char *root, const char *rel, int *err)
{
    FILE *fp;

    if (xpost_path_control_engaged && !xpost_path_permitted(root, 0))
    {
        *err = invalidfileaccess;
        return NULL;
    }

    fp = xpost_open_beneath(root, rel);

    if (!fp)
    {
        switch (errno)
        {
            case EACCES:
            case EPERM:
#ifdef ELOOP
            case ELOOP:
#endif
#ifdef EXDEV
            case EXDEV:
#endif
                *err = invalidfileaccess;
                break;
            case ENOENT:
            case ENOTDIR:
                *err = undefinedfilename;
                break;
#ifdef ENAMETOOLONG
            case ENAMETOOLONG:
                *err = limitcheck;
                break;
#endif
            default:
                *err = unregistered;
                break;
        }
        return NULL;
    }
    *err = 0;
    return fp;
}

#ifdef _WIN32
/*
 * FIXME: maybe use a WIN32 API for all this. See FIXME in xpost_op_file.c
 * Note:
 * this hack is needed as tmpfile in the Windows CRT opens
 * the temporary file in c:/ which needs administrator
 * privileges.
 */
static FILE *
f_tmpfile(void)
{
    char buf[XPOST_PATH_MAX];
    const char *name;
    const char *tmpdir;
    size_t l1;
    size_t l2;

    tmpdir = getenv("TEMP");
    if (!tmpdir)
        tmpdir = getenv("TMP");
    if (!tmpdir)
        return NULL;

    name = tmpnam(NULL);
    /* name points to a static buffer, so no need to check it */

    l1 = strlen(tmpdir);
    l2 = strlen(name);
    memset(buf, 0, l1 + l2 + 1);
    memcpy(buf, tmpdir, l1);
    memcpy(buf + l1, name, l2);

#ifdef DEBUG_FILE
    printf("fopen\n");
#endif
    {
        int err;
        return xpost_diskfile_fopen(buf, "w+bD", 1, &err);
    }
}
#else
# define f_tmpfile tmpfile
#endif

static int
disk_readch(Xpost_File *file)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;

    if (!df->file) /* the stream is closed: it holds no more data */
        return EOF;

    /*
     * FIXME: check if this work on Windows
     * indeed, on Windows, select() needs a socket, not a fd, and fileno() returns a fd
     * See http://stackoverflow.com/questions/6418232/how-to-use-select-to-read-input-from-keyboard-in-c/6419955#6419955
     * and https://msdn.microsoft.com/en-us/library/windows/desktop/ms682499%28v=vs.85%29.aspx
     * Maybe WaitForSingleObject will also be needed
     */

#ifdef HAVE_SYS_SELECT_H
    if (df->poll_before_read)
    {
        FILE *fp;
        fd_set reads, writes, excepts;
        int ret;
        struct timeval tv_timeout;
        fp = df->file;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_ZERO(&excepts);
        FD_SET(fileno(fp), &reads);
        tv_timeout.tv_sec = 0;
        tv_timeout.tv_usec = 0;

        ret = select(fileno(fp) + 1, &reads, &writes, &excepts, &tv_timeout);

        if (ret <= 0 || !FD_ISSET(fileno(fp), &reads))
        {
            /* byte not available, push retry, and request eval() to block this thread */
            errno=EINTR;
            return EOF;
        }
    }
#endif

    /* the interpreter is single-threaded, so the unlocked fast path is safe;
       unistd.h is present on mingw but does not declare getc_unlocked there */
#if defined(_WIN32)
    return _getc_nolock(df->file);
#elif defined(HAVE_UNISTD_H)
    return getc_unlocked(df->file);
#else
    return fgetc(df->file);
#endif
}

/* A stream that has already failed a write takes no more: the byte the C
   library accepts here would go into a buffer whose contents it has
   dropped, so a caller told the byte arrived would be told wrong. Only
   the flush a write happens to trigger reports the underlying failure,
   which is why the answer has to be read off the stream and not off this
   one call. */
static int
disk_writech(Xpost_File *file, int c)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;
    int ret;

    if (!df->file)
        return EOF;
    ret = fputc(c, df->file);
    if (ferror(df->file))
        return EOF;
    return ret;
}

static int
disk_writeblock(Xpost_File *file, const unsigned char *buf, int n)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;
    size_t wrote;

    if (!df->file)
        return EOF;
    wrote = fwrite(buf, 1, (size_t)n, df->file);
    if (ferror(df->file))
        return EOF;
    return (int)wrote;
}

static int
disk_close(Xpost_File *file)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;
    FILE *fp = df->file;
    int ret;

    if (!fp)
        return 0;
    if (fp == stdin || fp == stdout || fp == stderr) /* do NOT close standard files */
        return 0;
    /* what the stream lost earlier is still lost, whether or not the last
       flush of it happens to succeed */
    ret = ferror(fp) ? EOF : 0;
    if (fclose(df->file) != 0)
        ret = EOF;
    df->file = NULL;

    return ret;
}

/* flushfile: an output stream writes through what it has buffered, an
   input stream is read and discarded to end of data (PLRM 8.2). Reading a
   stream opened for output would set its error indicator, and flushing one
   opened for input would leave the characters the program asked to be rid
   of still in front of it, so the two halves are told apart by the
   direction the stream was opened in rather than attempted together. */
static int
disk_flush(Xpost_File *file)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;

    if (!df->file)
        return 0;
    if (df->input)
    {
        while (disk_readch(file) != EOF)
            /**/;
        return 0;
    }
    return fflush(df->file);
}

static void
disk_purge(Xpost_File *file)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;

    if (!df->file)
        return;
    xpost_fpurge(df->file);
}

static int
disk_unreadch(Xpost_File *file, int c)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;

    if (!df->file)
        return EOF;
    return ungetc(c, df->file);
}

/* the stdio call that counts a position in the file system's own width
   rather than in long's */
#ifdef _WIN32
# define XPOST_FTELL(f)      _ftelli64(f)
# define XPOST_FSEEK(f, o)   _fseeki64((f), (o), SEEK_SET)
#else
# define XPOST_FTELL(f)      ftello(f)
# define XPOST_FSEEK(f, o)   fseeko((f), (off_t)(o), SEEK_SET)
#endif

static long long
disk_tell(Xpost_File *file)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;

    if (!df->file)
        return -1;
    return (long long)XPOST_FTELL(df->file);
}

static int
disk_seek(Xpost_File *file, long long offset)
{
    Xpost_DiskFile *df = (Xpost_DiskFile*) file;

    if (!df->file)
        return -1;
    return XPOST_FSEEK(df->file, offset);
}

struct Xpost_File_Methods disk_methods =
{
    disk_readch,
    disk_writech,
    disk_close,
    disk_flush,
    disk_purge,
    disk_unreadch,
    disk_tell,
    disk_seek,
    disk_writeblock
};

/* A file that is a stream in its own right rather than a filter over one:
   nothing sits beneath it, nobody made it for anybody else, and it is
   born with no claims on it. Both such files start here. */
static void
_plain_file_init(Xpost_File *f, Xpost_File_Methods *methods)
{
    f->methods = methods;
    f->refs = 0;
    f->closed = 0;
    f->owned = 0;
    f->ent = 0;
    f->wraps = XPOST_FILE_WRAPS_NOTHING;
    f->job_stream = 0;
    f->eot = 0;
}

static Xpost_File *
xpost_diskfile_open(const FILE *fp, int input)
{
    Xpost_DiskFile *df = malloc(sizeof *df);
    struct stat st;

    if (!df)
        return NULL;
    _plain_file_init(&df->methods, &disk_methods);
    df->file = (FILE*)fp;
    df->input = input;
    /* reads from a regular file never block, so only poll fds that
       can stall (pipes, terminals, sockets) */
    df->poll_before_read = !(fstat(fileno(df->file), &st) == 0 &&
                             S_ISREG(st.st_mode));
    return (Xpost_File *)df;
}


/* the underlying stdio stream of a disk-backed file, or NULL */
FILE *xpost_file_stdio_stream_get(Xpost_File *f)
{
    if (f && f->methods == &disk_methods)
        return ((Xpost_DiskFile *)f)->file;
    return NULL;
}

static int
memory_readch(Xpost_File *f)
{
    Xpost_MemoryFile *mf = (Xpost_MemoryFile *)f;

    if (!mf->is_read)
        return EOF;
    if (mf->read_next == mf->read_limit)
        return EOF;

    return mf->contents[ mf->read_next++ ];
}

static int
memory_writech(Xpost_File *f, int c)
{
    Xpost_MemoryFile *mf = (Xpost_MemoryFile *)f;

    if (mf->is_read)
        return EOF;
    if (mf->write_next == mf->write_capacity){
        unsigned char *tmp;
        if (!mf->is_malloc)
            return EOF;
        tmp = realloc(mf->contents, mf->write_capacity * 1.4 + 12);
        if (!tmp)
            return EOF;
        mf->contents = tmp;
        mf->write_capacity = mf->write_capacity * 1.4 + 12;
    }

    mf->contents[ mf->write_next++ ] = c;
    return 0;
}

static int
memory_close(Xpost_File *f)
{
    Xpost_MemoryFile *mf = (Xpost_MemoryFile *)f;

    if (mf->is_malloc)
        free(mf->contents);
    /* the buffer is gone: a write must not grow one back */
    mf->is_malloc = 0;
    mf->contents = NULL;
    mf->read_next =
      mf->read_limit =
      mf->write_next =
      mf->write_capacity = 0;

    return 0;
}

/* flushfile (PLRM 8.2): a stream over memory holds nothing on its way out,
   and reaches the end of its data on the way in as soon as the read
   position is the limit. */
static int
memory_flush(Xpost_File *f)
{
    Xpost_MemoryFile *mf = (Xpost_MemoryFile *)f;

    if (mf->is_read)
        mf->read_next = mf->read_limit;
    return 0;
}

static void
memory_purge(Xpost_File *f)
{
    Xpost_MemoryFile *mf = (Xpost_MemoryFile *)f;

    if (mf->is_read)
        mf->read_next = mf->read_limit;
}

static int
memory_unreadch(Xpost_File *f, int c)
{
    Xpost_MemoryFile *mf = (Xpost_MemoryFile *)f;

    if (!mf->is_read)
        return EOF;
    if (mf->read_next == 0)
        return EOF;

    mf->contents[ --mf->read_next ] = c;
    return 0;
}

static long long
memory_tell(Xpost_File *f)
{
    Xpost_MemoryFile *mf = (Xpost_MemoryFile *)f;

    return (long long)mf->read_next;
}

static int
memory_seek(Xpost_File *f, long long pos)
{
    Xpost_MemoryFile *mf = (Xpost_MemoryFile *)f;

    if (pos < 0 || (unsigned long long)pos > (unsigned long long)mf->read_limit)
        return EOF;

    mf->read_next = (size_t)pos;
    return 0;
}

struct Xpost_File_Methods memory_methods =
{
    memory_readch,
    memory_writech,
    memory_close,
    memory_flush,
    memory_purge,
    memory_unreadch,
    memory_tell,
    memory_seek
};

static Xpost_File *
xpost_memoryfile_open_read(unsigned char *ptr, size_t limit)
{
    Xpost_MemoryFile *mf = malloc(sizeof *mf);

    if (!mf)
        return NULL;
    _plain_file_init(&mf->methods, &memory_methods);
    mf->contents = ptr;
    mf->is_read = 1;
    mf->is_malloc = 0;
    mf->read_next = 0;
    mf->read_limit = limit;
    return (Xpost_File *)mf;
}

/* What every decode filter is, whatever it decodes: a read-only stream
   over a source it does not own, with one byte of pushback and a latch
   for the end of the data. Every decode filter struct begins with this
   and adds only what its own coding needs, so the shared part is
   declared once and the machinery reaches it through one cast.
   Xpost_EncBase below says the same for the encode side. */
typedef struct Xpost_FilterBase
{
    Xpost_File methods;
    Xpost_File *source;
    int pushback;
    int eod;
} Xpost_FilterBase;

/* Those casts, and the ones every coding makes to its own struct, are a
   pointer to a struct read as a pointer to what it begins with. The
   method table has to be the first thing in the base for that to be the
   file, so say so where the base is defined rather than leave it to hold
   by luck. (A negative array size rather than _Static_assert: this
   builds as C99 with -pedantic-errors, which rejects the latter.) */
typedef char xpost_filter_base_begins_with_the_file[
    offsetof(Xpost_FilterBase, methods) == 0 ? 1 : -1];

/* ASCII85Decode filter: a read file decoding an ASCII base-85 stream
   from an underlying file. Whitespace between coded characters is
   ignored (a stream's layout carries no data), 'z' abbreviates four
   zero bytes, and the "~>" marker ends the data with the underlying
   file positioned just after it, so a program executing
   "currentfile /ASCII85Decode filter cvx exec" resumes cleanly at
   end of data. The source file is not owned: closing the filter
   leaves it open. */
typedef struct Xpost_FilterFile
{
    Xpost_FilterBase base;
    unsigned char out[4];
    int outn, outi;    /* decoded bytes pending */
} Xpost_FilterFile;

/* Peek past optional whitespace for a trailing "~>" and consume it, leaving the
   source just past the end-of-data marker so a fresh ASCII85Decode filter on the
   same stream stays in sync (the dvips image idiom abandons the filter after each
   readstring). A non-terminator byte is put back untouched. */
static void
a85_eat_eod(Xpost_FilterFile *ff)
{
    int nc;
    do { nc = xpost_file_getc(ff->base.source); } while (nc != EOF && isspace(nc));
    if (nc == '~')
    {
        do { nc = xpost_file_getc(ff->base.source); } while (nc != EOF && isspace(nc));
        if (nc != '>' && nc != EOF)
            xpost_file_ungetc(ff->base.source, nc);
        ff->base.eod = 1;
    }
    else if (nc != EOF)
        xpost_file_ungetc(ff->base.source, nc);
}

static int
a85_readch(Xpost_File *f)
{
    Xpost_FilterFile *ff = (Xpost_FilterFile *)f;
    unsigned int grp[5];
    int n, c;

    if (ff->base.pushback >= 0)
    {
        c = ff->base.pushback;
        ff->base.pushback = -1;
        return c;
    }
    if (ff->outi < ff->outn)
        return ff->out[ff->outi++];
    if (ff->base.eod)
        return EOF;

    /* gather the next coded group */
    n = 0;
    for (;;)
    {
        c = xpost_file_getc(ff->base.source);
        if (c == EOF)
        {
            ff->base.eod = 1;
            break;
        }
        if (isspace(c))
            continue;
        if (c == '~')
        {
            /* end of data: consume the closing '>' */
            do
            {
                c = xpost_file_getc(ff->base.source);
            } while (c != EOF && isspace(c));
            if (c != '>' && c != EOF)
                xpost_file_ungetc(ff->base.source, c);
            ff->base.eod = 1;
            break;
        }
        if (c == 'z' && n == 0)
        {
            ff->out[0] = ff->out[1] = ff->out[2] = ff->out[3] = 0;
            ff->outn = 4;
            ff->outi = 1;
            /* a "z" is a complete zero group; consume a trailing "~>" eagerly,
               exactly as the full five-character group below does */
            if (!ff->base.eod)
                a85_eat_eod(ff);
            return 0;
        }
        if (c < '!' || c > 'u')
        {
            XPOST_LOG_ERR("character %d in ASCII85Decode stream", c);
            ff->base.eod = 1;
            break;
        }
        grp[n++] = c - '!';
        if (n == 5)
            break;
    }

    /* A full five-character group breaks the gather loop before the closing
       "~>" is seen; the next read would consume it only when it starts a fresh
       group. The dvips image idiom abandons the filter after each readstring, so
       consume a trailing "~>" eagerly -- leaving the underlying file just past
       it -- rather than stranding it to be read as a token, which
       desynchronises every following scanline. */
    if (n == 5 && !ff->base.eod)
        a85_eat_eod(ff);

    if (n <= 1)   /* nothing, or a dangling single character */
        return EOF;

    {
        unsigned long long tuple = 0;
        unsigned int t32;
        int i, nbytes = n - 1;

        for (i = 0; i < 5; i++)
            tuple = tuple * 85 + (i < n ? grp[i] : 84);  /* pad with 'u' */
        /* A five-character group encodes a 32-bit value (PLRM 3.13.3), so a
           complete group whose base-85 value exceeds 2^32-1 -- the largest
           valid one is "s8W-!", and "uuuuu" comes to 4,437,053,124 -- is not
           a valid encoding. Assembled in 64 bits above so the excess is
           visible rather than wrapped to the low 32 bits, which would emit
           bytes the stream never encoded. End the stream at the bad group,
           as the filter already does for a character outside the alphabet.
           (A short final group cannot reach this: padding a valid one with
           'u' raises it by less than a base-85 place, never past the limit.) */
        if (n == 5 && tuple > 0xFFFFFFFFULL)
        {
            XPOST_LOG_ERR("group value exceeds 2^32-1 in ASCII85Decode stream");
            ff->base.eod = 1;
            return EOF;
        }
        t32 = (unsigned int)tuple;
        ff->out[0] = (t32 >> 24) & 0xff;
        ff->out[1] = (t32 >> 16) & 0xff;
        ff->out[2] = (t32 >> 8) & 0xff;
        ff->out[3] = t32 & 0xff;
        ff->outn = nbytes;
        ff->outi = 1;
        return ff->out[0];
    }
}

static int
a85_writech(Xpost_File *f, int c)
{
    (void)f;
    (void)c;
    return EOF;
}

static int
a85_close(Xpost_File *f)
{
    Xpost_FilterFile *ff = (Xpost_FilterFile *)f;

    /* the source stays open; just stop producing */
    ff->base.eod = 1;
    ff->outi = ff->outn = 0;
    ff->base.pushback = -1;
    return 0;
}

static void
a85_purge(Xpost_File *f)
{
    Xpost_FilterFile *ff = (Xpost_FilterFile *)f;

    ff->base.eod = 1;
    ff->outi = ff->outn = 0;
    ff->base.pushback = -1;
}

static int
a85_unreadch(Xpost_File *f, int c)
{
    Xpost_FilterFile *ff = (Xpost_FilterFile *)f;

    if (ff->base.pushback >= 0)
        return EOF;
    ff->base.pushback = c;
    return 0;
}

/* the draining and positioning methods shared by every decode filter
   (see filter_flush and filter_tell) */
static int filter_flush(Xpost_File *f);
static long long filter_tell(Xpost_File *f);
static int filter_seek(Xpost_File *f, long long offset);

struct Xpost_File_Methods a85_methods =
{
    a85_readch,
    a85_writech,
    a85_close,
    filter_flush,
    a85_purge,
    a85_unreadch,
    filter_tell,
    filter_seek
};

/* filetype objects use a slightly different interpretation
   of the access field.
   It uses two flags rather than a 2-bit number.
   XPOST_OBJECT_TAG_ACCESS_FLAG_WRITE designates a writable file
   XPOST_OBJECT_TAG_ACCESS_FLAG_READ designates a readable file
   */

/* construct a file object.
   set the tag,
   use the "doubleword" field as a "pointer" (ent),
   allocate a Xpost_File,
   install the Xpost_File,
   return object.
   caller must set access for a readable file,
   default is writable.
   eg.
    FILE *fp = xpost_diskfile_fopen(path, mode, 0, &err);
    Xpost_Object f = readonly(xpost_file_cons(fp, 1)).
 */

/* Tie a freshly allocated file entity to the stream it holds.

   Records the stream against the entity, which then carries the handle
   the file layer reaches it by. What the entity holds is a number, not
   the address of the struct: everything virtual memory holds names its
   storage by entity number, and a handle is likewise a number, so
   nothing in it depends on where the process put anything.

   Records the save depth at which the entity is born (as depth+1, zero
   meaning unstamped) in its low-level mark field: restore closes a file
   created since the corresponding save (PLRM 3.8.2), and the sweep needs
   the birth depth to tell such a file from an older one. The field is
   otherwise unused for files, which take no part in copy-on-write
   snapshots.

   And records the entity on the stream, so that whoever frees the struct
   can clear the entity that names it. The facts are written together
   because they are one fact -- this entity and this struct belong to
   each other -- and a stream that knew its depth but not its entity is
   how a freed struct came to be closed a second time. A refusal writes
   none of them: the entity is left holding no stream and outside the
   birth census, which is what it would be taken back to. */
static int
_file_bind_entity(Xpost_Memory_File *mem, unsigned int ent, Xpost_File *fp)
{
    unsigned int vs, depth = 0, mk;

    if (!xpost_handle_hold(mem, ent, XPOST_HANDLE_FILE,
                           XPOST_FILE_BLOCK_SIZE, fp))
    {
        /* the entity keeps the tag of a file and holds whatever the
           allocation left there, which is not to be read as a handle */
        (void)xpost_handle_drop(mem, ent);
        return 0;
    }
    if (xpost_memory_save_stack_ready(mem))
    {
        vs = xpost_memory_save_stack_ent(mem);
        depth = (unsigned int)xpost_stack_count(mem, vs);
    }
    if (depth > 254)
        depth = 254;
    mk = mem->table.tab[ent].mark;
    mk &= ~(unsigned int)XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK;
    mk |= (depth + 1) << XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET;
    mem->table.tab[ent].mark = mk;
    mem->file_births[depth + 1]++;
    if (depth + 1 > mem->file_birth_max)
        mem->file_birth_max = depth + 1;
    if (fp)
        fp->ent = ent;
    return 1;
}

/* Take a closed file out of the birth census, just after it closes.

   The census is what tells restore whether there is any point walking
   the local table at all: it holds a count per birth depth and the
   deepest depth still occupied. A file that has closed is not a file any
   restore can close, so it leaves the census then rather than when its
   entity is finally reclaimed -- otherwise the deepest occupied depth
   never falls again and every later restore walks the whole table to
   find nothing left to do.

   The stamp itself is cleared as the count is decremented, so a file
   leaves the census exactly once and the sweep skips it from then on.
   For a file entity that field carries only the birth depth: files take
   no part in copy-on-write snapshots, which is what it records for
   everything else. */
static void
_file_retire_stamp(Xpost_Memory_File *mem, unsigned int ent)
{
    unsigned int stamp;

    if (!xpost_ent_valid(mem, ent))
        return;
    stamp = (mem->table.tab[ent].mark
             & XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK)
            >> XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_OFFSET;
    if (stamp == 0)
        return;
    mem->table.tab[ent].mark &=
        ~(unsigned int)XPOST_MEMORY_TABLE_MARK_DATA_LOWLEVEL_MASK;
    if (mem->file_births[stamp] > 0)
        mem->file_births[stamp]--;
    while (mem->file_birth_max > 0
           && mem->file_births[mem->file_birth_max] == 0)
        mem->file_birth_max--;
}

/* Clear the entity that names this stream, just before the struct goes.
   Entity zero is the free list, never a file, so it stands for "no
   entity" on a stream that never had one.

   The number is remembered, not held: the entity it named can be
   reclaimed while this struct is still alive, and the free list hands
   the same number out again to whatever asks next. Being in range says
   only that some entity is there, not that it is still this stream's, so
   the tag is what decides. Without that, releasing a stream clears the
   handle of whichever object holds the number now -- a file that reports
   itself open and then answers nothing, or a string, or a link in the
   free list. */
static void
_file_forget_entity(Xpost_Memory_File *mem, Xpost_File *fp)
{
    unsigned int tag;

    if (!fp->ent || !xpost_ent_valid(mem, fp->ent))
        return;
    if (!xpost_memory_table_get_tag(mem, fp->ent, &tag) || tag != filetype)
        return;
    if (!xpost_handle_drop(mem, fp->ent))
        XPOST_LOG_ERR("cannot clear the handle of a released stream");
}

Xpost_Object xpost_file_cons(Xpost_Memory_File *mem,
                             /*@NULL@*/ const FILE *fp,
                             int input)
{
    Xpost_Object f = { 0 };
    unsigned int ent;
    Xpost_File *df;

#ifdef DEBUG_FILE
    printf("xpost_file_cons %p\n", fp);
#endif
    f.tag = filetype /*| (XPOST_OBJECT_TAG_ACCESS_UNLIMITED << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET)*/;
    df = xpost_diskfile_open(fp, input);
    if (!df)
        return invalid;
    if (!xpost_memory_table_alloc(mem, XPOST_HANDLE_ENTITY_SIZE, filetype,
                                  &ent))
    {
        XPOST_LOG_ERR("cannot allocate file record");
        /* the stream is being abandoned before any program saw it */
        free(df);
        return invalid;
    }
    if (!_file_bind_entity(mem, ent, df))
    {
        XPOST_LOG_ERR("cannot hold the stream of a file record");
        free(df);
        return invalid;
    }
    f.mark_.padw = ent;
    return f;
}

/* A readable file over a private copy of a byte range, for the string form of
   the filter operator. The copy is owned by the memory file (is_malloc) and is
   released when the file is closed; the wrapping decode filter closes it (see
   xpost_file_object_close), since a filter otherwise leaves its source open. */
Xpost_Object xpost_file_cons_readstring(Xpost_Memory_File *mem,
                                        const unsigned char *ptr,
                                        unsigned int len)
{
    Xpost_Object f = { 0 };
    unsigned int ent;
    Xpost_File *mf;
    unsigned char *copy;

    f.tag = filetype;
    copy = malloc(len ? len : 1);
    if (!copy)
        return invalid;
    if (len)
        memcpy(copy, ptr, len);
    mf = xpost_memoryfile_open_read(copy, len);
    if (!mf)
    {
        free(copy);
        return invalid;
    }
    ((Xpost_MemoryFile *)mf)->is_malloc = 1;
    if (!xpost_memory_table_alloc(mem, XPOST_HANDLE_ENTITY_SIZE, filetype,
                                  &ent))
    {
        XPOST_LOG_ERR("cannot allocate file record");
        /* the stream is being abandoned before any program saw it */
        (void)xpost_file_close(mf);
        free(mf);
        return invalid;
    }
    if (!_file_bind_entity(mem, ent, mf))
    {
        XPOST_LOG_ERR("cannot hold the stream of a file record");
        (void)xpost_file_close(mf);
        free(mf);
        return invalid;
    }
    f.mark_.padw = ent;
    return f;
}

/* Record a failed callback so the operator that was reaching through
   this stream answers for it. A stream can only say end of data or
   refusal; the error itself is carried here. */
static int
_proc_failed(Xpost_ProcFile *pf, int err)
{
    if (err && !pf->ctx->callback_error)
        pf->ctx->callback_error = (unsigned int)err;
    return err;
}

/* Ask the procedure for its next string, with whatever the caller wants
   it called with already pushed. The answer is left on the operand
   stack. */
static int
_proc_call(Xpost_ProcFile *pf, int refuse_reentry)
{
    int ret;

    if (pf->methods.closed)
        return ioerror;
    /* A target procedure that writes to the stream it is the target of
       is asking it to dispose of bytes it is in the middle of being
       asked to dispose of. There is nothing to answer with: the string
       the waiting call must give back is the one this call would have to
       return first. PLRM 8.2 gives ioerror for an output error, and a
       stream that cannot be written because writing it is what is
       already happening is one. A source is under no such difficulty --
       the bytes a nested read takes are bytes the stream has -- and is
       allowed to nest. */
    if (refuse_reentry && pf->running)
        return ioerror;
    pf->running = 1;
    ret = xpost_interpreter_run_nested(pf->ctx, pf->proc);
    pf->running = 0;
    return ret;
}

/* Keep a string until the buffer in front of it has been read out. */
static int
_proc_queue(Xpost_ProcFile *pf, Xpost_Object s)
{
    if (pf->npending == pf->cpending)
    {
        int want = pf->cpending ? pf->cpending * 2 : 4;
        Xpost_Object *grown = realloc(pf->pending, (size_t)want * sizeof *grown);

        if (!grown)
            return VMerror;
        pf->pending = grown;
        pf->cpending = want;
    }
    pf->pending[pf->npending++] = s;
    return 0;
}

/* Take up the string that has waited longest. */
static void
_proc_dequeue(Xpost_ProcFile *pf)
{
    int i;

    pf->buf = pf->pending[0];
    pf->pos = 0;
    for (i = 1; i < pf->npending; i++)
        pf->pending[i - 1] = pf->pending[i];
    --pf->npending;
}

/* Take the string the procedure answered with. A procedure that
   answered with something else, or with nothing, has not met the
   contract, and the read or write that called it fails. */
static int
_proc_take_string(Xpost_ProcFile *pf, Xpost_Object *out)
{
    Xpost_Object s;

    if (xpost_stack_count(pf->ctx->lo, pf->ctx->os) < 1)
        return stackunderflow;
    s = xpost_stack_pop(pf->ctx->lo, pf->ctx->os);
    if (xpost_object_get_type(s) != stringtype)
        return typecheck;
    *out = s;
    return 0;
}

/* refill from the source procedure; 0 once buf holds bytes to hand out,
   and the source is at its end when it answers with none */
static int
_procsrc_refill(Xpost_ProcFile *pf)
{
    int ret;
    Xpost_Object s;

    ret = _proc_call(pf, 0);
    if (ret)
        return ret;
    ret = _proc_take_string(pf, &s);
    if (ret)
        return ret;
    if (!xpost_object_is_readable(pf->ctx, s))
        return invalidaccess;
    if (s.comp_.sz == 0)
    {
        /* the end of the data, once whatever is already waiting has
           been read out */
        pf->eod = 1;
        return 0;
    }
    /* the procedure may have read this stream while it was running, and
       been served from a buffer that still has bytes left: this string
       goes behind that one rather than over it */
    if (pf->pos < pf->buf.comp_.sz || pf->npending)
        return _proc_queue(pf, s);
    pf->buf = s;
    pf->pos = 0;
    return 0;
}

static int
procsrc_readch(Xpost_File *f)
{
    Xpost_ProcFile *pf = (Xpost_ProcFile *)f;
    integer c;

    /* Each source of bytes is tested on every pass. Asking the procedure
       for more data runs the program, and the program may read this same
       stream from inside that call, leaving a pushback byte, a
       part-read buffer or a queued string. Each of those precedes what
       a later call supplies. */
    for (;;)
    {
        if (pf->pushback >= 0)
        {
            c = pf->pushback;
            pf->pushback = -1;
            return (int)c;
        }
        if (pf->pos < pf->buf.comp_.sz)
        {
            /* the string is read one byte at a time rather than through
               a pointer taken once: the procedure that returned it runs
               the interpreter, and a collection between two of its calls
               may move the string it gave back */
            if (xpost_string_get(pf->ctx, pf->buf, (integer)pf->pos, &c) != 0)
                return EOF;
            pf->pos++;
            return c & 0xff;
        }
        /* whatever is waiting comes before anything the procedure has
           still to say, and before the end of the data: a string the
           source answered with is data it supplied */
        if (pf->npending)
        {
            _proc_dequeue(pf);
            continue;
        }
        if (pf->eod || pf->methods.closed)
            return EOF;
        if (_proc_failed(pf, _procsrc_refill(pf)) != 0)
            return EOF;
    }
}

static int
procsrc_writech(Xpost_File *f, int c)
{
    (void)f;
    (void)c;
    return EOF;
}

/* Hand the target procedure what has accumulated, and take the string
   it gives back to fill next. more says whether the filter has further
   data to write (PLRM 3.13.1): false is the last call, made as the
   filter closes, and its answer is dropped. */
static int
_proctgt_dispose(Xpost_ProcFile *pf, int more)
{
    Xpost_Object arg;
    Xpost_Object s;
    int ret;

    /* what the procedure is shown is the part of its own string the
       filter filled, which is that string or the front of it */
    if (pf->started)
    {
        arg = pf->buf;
        arg.comp_.sz = (word)pf->pos;
    }
    else
    {
        arg = xpost_string_cons(pf->ctx, 0, NULL);
        if (xpost_object_get_type(arg) != stringtype)
            return VMerror;
    }
    if (!xpost_stack_push(pf->ctx->lo, pf->ctx->os, arg))
        return stackoverflow;
    if (!xpost_stack_push(pf->ctx->lo, pf->ctx->os,
                          xpost_bool_cons(more ? 1 : 0)))
        return stackoverflow;
    ret = _proc_call(pf, 1);
    if (ret)
        return ret;
    ret = _proc_take_string(pf, &s);
    if (ret)
        return ret;
    if (!more)
    {
        /* the last answer is not written into, so nothing is asked of
           it: any string will do, and the filter merely drops it */
        pf->eod = 1;
        return 0;
    }
    if (!xpost_object_is_writeable(pf->ctx, s))
        return invalidaccess;
    if (s.comp_.sz == 0)
        return ioerror;
    pf->buf = s;
    pf->pos = 0;
    pf->started = 1;
    return 0;
}

static int
procsrc_close(Xpost_File *f)
{
    Xpost_ProcFile *pf = (Xpost_ProcFile *)f;

    pf->methods.closed = 1;
    pf->eod = 1;
    return 0;
}

static int
proctgt_writech(Xpost_File *f, int c)
{
    Xpost_ProcFile *pf = (Xpost_ProcFile *)f;

    if (pf->methods.closed || pf->eod)
        return EOF;
    if (!pf->started || pf->pos >= pf->buf.comp_.sz)
    {
        if (_proc_failed(pf, _proctgt_dispose(pf, 1)) != 0)
            return EOF;
    }
    if (xpost_string_put(pf->ctx, pf->buf, (integer)pf->pos, c & 0xff) != 0)
        return EOF;
    pf->pos++;
    return c & 0xff;
}

static int
proctgt_readch(Xpost_File *f)
{
    (void)f;
    return EOF;
}

/* The close is where the target procedure is told the data has ended,
   which is the one call it is promised beyond the ones its own buffer
   filling asks for. A target already at its end is not told twice. */
static int
proctgt_close(Xpost_File *f)
{
    Xpost_ProcFile *pf = (Xpost_ProcFile *)f;
    int ret = 0;

    if (!pf->methods.closed && !pf->eod)
        ret = _proc_failed(pf, _proctgt_dispose(pf, 0));
    pf->methods.closed = 1;
    pf->eod = 1;
    return ret ? EOF : 0;
}

/* Whatever the filter above has buffered reaches the procedure through
   the close; a flush of its own has nothing to push further, since the
   procedure is handed data as soon as its string is full. */
static int
proctgt_flush(Xpost_File *f)
{
    (void)f;
    return 0;
}

static void
proc_purge(Xpost_File *f)
{
    (void)f;
}

/* Give a byte back. A source is read through by codings that decide a
   byte is not theirs only after taking it, and the byte may be the last
   of the string the procedure gave, so it is held here rather than
   pushed back into that string: by the time it comes back the procedure
   may already have been asked for the next one. */
static int
procsrc_unreadch(Xpost_File *f, int c)
{
    Xpost_ProcFile *pf = (Xpost_ProcFile *)f;

    if (pf->pushback >= 0)
        return EOF;
    pf->pushback = c & 0xff;
    return c & 0xff;
}

static int
proc_unreadch(Xpost_File *f, int c)
{
    (void)f;
    (void)c;
    return EOF;
}

/* A procedure supplies or consumes a stream of bytes and answers no
   question about a position in it. */
static long long
proc_tell(Xpost_File *f)
{
    (void)f;
    return -1;
}

static int
proc_seek(Xpost_File *f, long long pos)
{
    (void)f;
    (void)pos;
    return EOF;
}

static struct Xpost_File_Methods procsrc_methods =
{
    procsrc_readch, procsrc_writech, procsrc_close, proctgt_flush,
    proc_purge, procsrc_unreadch, proc_tell, proc_seek
};

static struct Xpost_File_Methods proctgt_methods =
{
    proctgt_readch, proctgt_writech, proctgt_close, proctgt_flush,
    proc_purge, proc_unreadch, proc_tell, proc_seek
};

static Xpost_Object
_proc_stream_cons(Xpost_Context *ctx, Xpost_Object proc,
                  Xpost_File_Methods *methods)
{
    Xpost_Object f = { 0 };
    unsigned int ent;
    Xpost_File *mf;
    Xpost_ProcFile *pf;

    pf = malloc(sizeof *pf);
    if (!pf)
        return invalid;
    _plain_file_init(&pf->methods, methods);
    pf->ctx = ctx;
    pf->proc = proc;
    pf->buf = null;
    pf->pos = 0;
    pf->pushback = -1;
    pf->eod = 0;
    pf->started = 0;
    pf->running = 0;
    pf->pending = NULL;
    pf->npending = 0;
    pf->cpending = 0;
    mf = (Xpost_File *)pf;

    f.tag = filetype;
    if (!xpost_memory_table_alloc(ctx->lo, XPOST_HANDLE_ENTITY_SIZE, filetype,
                                  &ent))
    {
        XPOST_LOG_ERR("cannot allocate file record");
        free(pf);
        return invalid;
    }
    if (!_file_bind_entity(ctx->lo, ent, mf))
    {
        XPOST_LOG_ERR("cannot hold the stream of a file record");
        free(pf);
        return invalid;
    }
    f.mark_.padw = ent;
    return f;
}

Xpost_Object xpost_file_cons_procsource(Xpost_Context *ctx, Xpost_Object proc)
{
    return _proc_stream_cons(ctx, proc, &procsrc_methods);
}

Xpost_Object xpost_file_cons_proctarget(Xpost_Context *ctx, Xpost_Object proc)
{
    return _proc_stream_cons(ctx, proc, &proctgt_methods);
}

/* What the collector cannot see of a file: a procedure stream holds the
   procedure it calls and the string that procedure last gave back, and
   both are ordinary objects living in a C struct. Every other kind of
   file holds none, and says so with a count of zero. */
static Xpost_Object _file_object_of_entity(unsigned int ent);

static Xpost_ProcFile *_proc_stream_at(Xpost_Memory_File *mem, unsigned int ent)
{
    Xpost_File *f;

    /* A file entity holds the pointer to its stream. An entity of any
       other type holds bytes that are not one. Callers include the
       collector, which arrives by walking entities rather than by
       naming a file. */
    f = xpost_file_get_file_pointer(mem, _file_object_of_entity(ent));
    if (!f)
        return NULL;
    if (f->methods != &procsrc_methods && f->methods != &proctgt_methods)
        return NULL;
    return (Xpost_ProcFile *)f;
}

int xpost_file_held_count(Xpost_Memory_File *mem, unsigned int ent)
{
    Xpost_ProcFile *pf = _proc_stream_at(mem, ent);

    return pf ? 2 + pf->npending : 0;
}

Xpost_Object xpost_file_held_object(Xpost_Memory_File *mem, unsigned int ent,
                                    int i)
{
    Xpost_ProcFile *pf = _proc_stream_at(mem, ent);

    if (!pf || i < 0 || i >= 2 + pf->npending)
        return null;
    if (i == 0)
        return pf->proc;
    if (i == 1)
        return pf->buf;
    return pf->pending[i - 2];
}

/* ASCIIHexDecode filter: hexadecimal digit pairs to bytes, whitespace
   ignored, '>' ends the data (an odd final digit is padded with 0). */
typedef struct Xpost_HexFile
{
    Xpost_FilterBase base;
} Xpost_HexFile;

static int
_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int
hex_readch(Xpost_File *f)
{
    Xpost_HexFile *ff = (Xpost_HexFile *)f;
    int c, hi, lo;

    if (ff->base.pushback >= 0)
    {
        c = ff->base.pushback;
        ff->base.pushback = -1;
        return c;
    }
    if (ff->base.eod)
        return EOF;
    do
    {
        c = xpost_file_getc(ff->base.source);
    } while (c != EOF && isspace(c));
    if (c == EOF || c == '>')
    {
        ff->base.eod = 1;
        return EOF;
    }
    hi = _hexval(c);
    if (hi < 0)
    {
        XPOST_LOG_ERR("character %d in ASCIIHexDecode stream", c);
        ff->base.eod = 1;
        return EOF;
    }
    do
    {
        c = xpost_file_getc(ff->base.source);
    } while (c != EOF && isspace(c));
    if (c == EOF || c == '>')
    {
        ff->base.eod = 1;
        lo = 0;
    }
    else
    {
        lo = _hexval(c);
        if (lo < 0)
        {
            XPOST_LOG_ERR("character %d in ASCIIHexDecode stream", c);
            ff->base.eod = 1;
            lo = 0;
        }
        else if (!ff->base.eod)
        {
            /* the byte is complete; eagerly consume a trailing '>' so a
               following read of an abandoned filter is not stranded on it */
            do { c = xpost_file_getc(ff->base.source); } while (c != EOF && isspace(c));
            if (c == '>')
                ff->base.eod = 1;
            else if (c != EOF)
                xpost_file_ungetc(ff->base.source, c);
        }
    }
    return (hi << 4) | lo;
}

/* RunLengthDecode filter: a length byte 0..127 copies that many plus
   one literal bytes; 129..255 repeats the next byte 257 minus the
   length times; 128 ends the data. */
typedef struct Xpost_RleFile
{
    Xpost_FilterBase base;
    int litrun;   /* literal bytes still to copy */
    int reprun;   /* repetitions still to emit */
    int repbyte;
} Xpost_RleFile;

/* Read the next run header from the source: prime a literal or repeat run, or
   set eod on the 128 marker or EOF. Called eagerly as each run empties so the
   closing 128 -- and, through it, the underlying filter's own EOD -- is
   consumed while the source is positioned there, rather than stranded for a
   following read to trip on (the dvips image idiom abandons the filter after
   each readstring, so nothing else would consume it). */
static void
rle_prime(Xpost_RleFile *ff)
{
    int c;

    if (ff->base.eod)
        return;
    c = xpost_file_getc(ff->base.source);
    if (c == EOF || c == 128)
    {
        ff->base.eod = 1;
        return;
    }
    if (c < 128)
    {
        ff->litrun = c + 1;   /* this many literal bytes follow */
    }
    else
    {
        int rb = xpost_file_getc(ff->base.source);
        if (rb == EOF)
        {
            ff->base.eod = 1;
            return;
        }
        ff->repbyte = rb;
        ff->reprun = 257 - c;   /* this many copies */
    }
}

static int
rle_readch(Xpost_File *f)
{
    Xpost_RleFile *ff = (Xpost_RleFile *)f;
    int c;

    if (ff->base.pushback >= 0)
    {
        c = ff->base.pushback;
        ff->base.pushback = -1;
        return c;
    }
    for (;;)
    {
        if (ff->reprun > 0)
        {
            c = ff->repbyte;
            if (--ff->reprun == 0)
                rle_prime(ff);   /* eagerly consume a trailing 128 */
            return c;
        }
        if (ff->litrun > 0)
        {
            c = xpost_file_getc(ff->base.source);
            if (c == EOF)
            {
                ff->base.eod = 1;
                return EOF;
            }
            if (--ff->litrun == 0)
                rle_prime(ff);   /* eagerly consume a trailing 128 */
            return c;
        }
        if (ff->base.eod)
            return EOF;
        rle_prime(ff);   /* prime the first run */
    }
}

/* SubFileDecode filter: pass bytes through until the EOD string has been seen
   count times. For count > 0 the data delivered is everything up to and
   including the count-th occurrence, leaving the source just after it. A count
   of zero with a non-empty string consumes the first occurrence without
   delivering it; an empty string makes count a plain byte count. */
typedef struct Xpost_SubFile
{
    Xpost_FilterBase base;
    int count;
    unsigned char eodstr[64];
    int eodlen;
    unsigned char pend[64];   /* partially matched prefix to re-emit */
    int pendn, pendi;
} Xpost_SubFile;

static int
subfile_readch(Xpost_File *f)
{
    Xpost_SubFile *ff = (Xpost_SubFile *)f;
    int c;
    int matched;

    if (ff->base.pushback >= 0)
    {
        c = ff->base.pushback;
        ff->base.pushback = -1;
        return c;
    }
    if (ff->pendi < ff->pendn)
        return ff->pend[ff->pendi++];
    if (ff->base.eod)
        return EOF;

    if (ff->eodlen == 0)
    {
        /* byte count mode */
        if (ff->count <= 0)
        {
            ff->base.eod = 1;
            return EOF;
        }
        ff->count--;
        c = xpost_file_getc(ff->base.source);
        if (c == EOF)
            ff->base.eod = 1;
        return c;
    }

    /* match the EOD string; on a partial mismatch the consumed prefix
       replays from the pending buffer (the string may not contain a
       repeated prefix hazard longer than itself, so rescanning from
       the second byte is not needed for the delimiters in use) */
    matched = 0;
    for (;;)
    {
        c = xpost_file_getc(ff->base.source);
        if (c == EOF)
        {
            ff->base.eod = 1;
            if (matched)
            {
                memcpy(ff->pend, ff->eodstr, matched);
                ff->pendn = matched;
                ff->pendi = 1;
                return ff->pend[0];
            }
            return EOF;
        }
        if ((unsigned char)c == ff->eodstr[matched])
        {
            matched++;
            if (matched == ff->eodlen)
            {
                if (ff->count > 1)
                {
                    /* an intermediate occurrence: deliver it and keep going */
                    ff->count--;
                    memcpy(ff->pend, ff->eodstr, matched);
                    ff->pendn = matched;
                    ff->pendi = 1;
                    return ff->pend[0];
                }
                ff->base.eod = 1;
                if (ff->count == 1)
                {
                    /* the final occurrence: PLRM passes all data "up to and
                       including" the count-th EOD string, so deliver it before
                       reporting end-of-data (pend drains ahead of the eod flag).
                       The source is already positioned just past it. */
                    memcpy(ff->pend, ff->eodstr, matched);
                    ff->pendn = matched;
                    ff->pendi = 1;
                    return ff->pend[0];
                }
                /* count == 0: the first occurrence is consumed but not passed */
                return EOF;
            }
            continue;
        }
        if (matched)
        {
            /* replay the matched prefix, then this byte */
            memcpy(ff->pend, ff->eodstr, matched);
            ff->pend[matched] = (unsigned char)c;
            ff->pendn = matched + 1;
            ff->pendi = 1;
            return ff->pend[0];
        }
        return c;
    }
}

#ifdef HAVE_ZLIB
/* FlateDecode filter: a zlib stream inflated from the source, which
   is left positioned just after the compressed data. */
typedef struct Xpost_FlateFile
{
    Xpost_FilterBase base;
    z_stream strm;
    unsigned char in[1];
    unsigned char out[4096];
    int outn, outi;
    int look, haslook;   /* one byte read ahead to drive zlib past the trailer */
} Xpost_FlateFile;

/* Inflate the next chunk (up to the whole out buffer) from the source. When the
   buffer fills without reaching the stream's end, look one byte past it so that a
   trailing EOD -- and, beneath a nested filter, that filter's own terminator -- is
   consumed the moment the last data byte becomes available. A bounded reader stops
   the instant it has its byte count and never reads far enough to trigger the
   trailer otherwise; the peeked byte is delivered ahead of the following refill,
   which then drives zlib through the trailer while the reader is none the wiser. */
static void
flate_refill(Xpost_FlateFile *ff)
{
    int c, ret;

    ff->outi = 0;
    ff->outn = 0;
    if (ff->base.eod)
        return;

    ff->strm.next_out = ff->out;
    ff->strm.avail_out = sizeof(ff->out);
    while (ff->strm.avail_out > 0)
    {
        if (ff->strm.avail_in == 0)
        {
            c = xpost_file_getc(ff->base.source);
            if (c == EOF)
            {
                ff->base.eod = 1;
                break;
            }
            ff->in[0] = (unsigned char)c;
            ff->strm.next_in = ff->in;
            ff->strm.avail_in = 1;
        }
        ret = inflate(&ff->strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END)
        {
            ff->base.eod = 1;
            break;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR)
        {
            XPOST_LOG_ERR("FlateDecode error %d", ret);
            ff->base.eod = 1;
            break;
        }
    }
    ff->outn = (int)(sizeof(ff->out) - ff->strm.avail_out);

    if (ff->strm.avail_out == 0 && !ff->base.eod)
    {
        /* the byte the decompressor is given room for; room left over is
           what says it produced none, so it is read only where the room
           is gone */
        unsigned char sb;
        for (;;)
        {
            ff->strm.next_out = &sb;
            ff->strm.avail_out = 1;
            if (ff->strm.avail_in == 0)
            {
                c = xpost_file_getc(ff->base.source);
                if (c == EOF)
                {
                    ff->base.eod = 1;
                    break;
                }
                ff->in[0] = (unsigned char)c;
                ff->strm.next_in = ff->in;
                ff->strm.avail_in = 1;
            }
            ret = inflate(&ff->strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_END)
            {
                ff->base.eod = 1;
                if (ff->strm.avail_out == 0)   /* the final byte came with the end */
                {
                    /* cppcheck-suppress uninitvar */
                    ff->look = sb;
                    ff->haslook = 1;
                }
                break;
            }
            if (ret != Z_OK && ret != Z_BUF_ERROR)
            {
                XPOST_LOG_ERR("FlateDecode error %d", ret);
                ff->base.eod = 1;
                break;
            }
            if (ff->strm.avail_out == 0)   /* a byte past the buffer; stream continues */
            {
                ff->look = sb;
                ff->haslook = 1;
                break;
            }
            /* Z_OK/Z_BUF_ERROR with no output: needs more input, keep going */
        }
    }
}

static int
flate_readch(Xpost_File *f)
{
    Xpost_FlateFile *ff = (Xpost_FlateFile *)f;

    if (ff->base.pushback >= 0)
    {
        int c = ff->base.pushback;
        ff->base.pushback = -1;
        return c;
    }
    for (;;)
    {
        if (ff->outi < ff->outn)
            return ff->out[ff->outi++];
        if (ff->haslook)
        {
            /* Deliver the read-ahead byte, refilling behind it first so that if it
               was the stream's last byte the trailer is consumed now, not on a
               follow-up read the bounded reader never makes. */
            int b = ff->look;
            ff->haslook = 0;
            flate_refill(ff);
            return b;
        }
        if (ff->base.eod)
            return EOF;
        flate_refill(ff);
    }
}
#endif

#ifdef HAVE_LIBJPEG
/* DCTDecode filter: a JPEG stream decompressed from the source into
   interleaved samples (grey, RGB or CMYK as the stream declares).
   libjpeg pulls its input through a one-byte source manager so the
   underlying file is never read past what the decoder consumes,
   leaving it positioned at the end of the compressed data like the
   other decode filters. Decoder errors end the stream rather than
   the process: the default error handler exits, so a longjmp handler
   is installed around every libjpeg call. */
typedef struct Xpost_DctFile
{
    Xpost_FilterBase base;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    jmp_buf jmp;
    struct jpeg_source_mgr jsrc;
    JOCTET jbytes[2];
    int started;
    unsigned char *row;
    unsigned int rown, rowi;
} Xpost_DctFile;

static void
dct_error_exit(j_common_ptr cinfo)
{
    Xpost_DctFile *ff = (Xpost_DctFile *)cinfo->client_data;
    char msg[JMSG_LENGTH_MAX];

    (*cinfo->err->format_message)(cinfo, msg);
    XPOST_LOG_ERR("DCTDecode: %s", msg);
    longjmp(ff->jmp, 1);
}

static void
dct_output_message(j_common_ptr cinfo)
{
    char msg[JMSG_LENGTH_MAX];

    (*cinfo->err->format_message)(cinfo, msg);
    XPOST_LOG_ERR("DCTDecode: %s", msg);
}

static void
dct_init_source(j_decompress_ptr cinfo)
{
    (void)cinfo;
}

static boolean
dct_fill_input_buffer(j_decompress_ptr cinfo)
{
    Xpost_DctFile *ff = (Xpost_DctFile *)cinfo->client_data;
    int c = xpost_file_getc(ff->base.source);

    if (c == EOF)
    {
        /* A stream that stops early is closed off with the end-of-image
           marker the decoder is waiting for, so it finishes with the
           scanlines it has. The marker is both its bytes: handed the
           second one alone the decoder sees no marker and asks for input
           that will never come, for as long as it is asked to decode. */
        ff->jbytes[0] = 0xff;
        ff->jbytes[1] = 0xd9;
        ff->jsrc.next_input_byte = ff->jbytes;
        ff->jsrc.bytes_in_buffer = 2;
        return TRUE;
    }

    ff->jbytes[0] = (JOCTET)c;
    ff->jsrc.next_input_byte = ff->jbytes;
    ff->jsrc.bytes_in_buffer = 1;
    return TRUE;
}

static void
dct_skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
    Xpost_DctFile *ff = (Xpost_DctFile *)cinfo->client_data;

    if (num_bytes <= 0)
        return;
    if ((size_t)num_bytes <= ff->jsrc.bytes_in_buffer)
    {
        ff->jsrc.next_input_byte += num_bytes;
        ff->jsrc.bytes_in_buffer -= num_bytes;
        return;
    }
    num_bytes -= (long)ff->jsrc.bytes_in_buffer;
    ff->jsrc.bytes_in_buffer = 0;
    while (num_bytes-- > 0)
        if (xpost_file_getc(ff->base.source) == EOF)
            break;
}

static void
dct_term_source(j_decompress_ptr cinfo)
{
    (void)cinfo;
}

static int
dct_readch(Xpost_File *f)
{
    Xpost_DctFile *ff = (Xpost_DctFile *)f;
    int c;

    if (ff->base.pushback >= 0)
    {
        c = ff->base.pushback;
        ff->base.pushback = -1;
        return c;
    }
    if (ff->rowi < ff->rown)
        return ff->row[ff->rowi++];
    if (ff->base.eod)
        return EOF;

    if (setjmp(ff->jmp))
    {
        ff->base.eod = 1;
        return EOF;
    }
    if (!ff->started)
    {
        jpeg_read_header(&ff->cinfo, TRUE);
        jpeg_start_decompress(&ff->cinfo);
        ff->row = malloc((size_t)ff->cinfo.output_width
                         * ff->cinfo.output_components);
        if (!ff->row)
        {
            ff->base.eod = 1;
            return EOF;
        }
        ff->started = 1;
    }
    if (ff->cinfo.output_scanline < ff->cinfo.output_height)
    {
        JSAMPROW rowp = ff->row;

        if (jpeg_read_scanlines(&ff->cinfo, &rowp, 1) != 1)
        {
            ff->base.eod = 1;
            return EOF;
        }
        ff->rown = ff->cinfo.output_width * ff->cinfo.output_components;
        ff->rowi = 0;
        /* the caller may drain exactly the samples and never pull the
           EOF that follows: consume through the end-of-image marker as
           soon as the last scanline is out, so a program resumes
           reading the underlying file just past the compressed data */
        if (ff->cinfo.output_scanline >= ff->cinfo.output_height)
        {
            jpeg_finish_decompress(&ff->cinfo);
            ff->base.eod = 1;
        }
        if (ff->rown)
            return ff->row[ff->rowi++];
        return EOF;
    }
    jpeg_finish_decompress(&ff->cinfo);
    ff->base.eod = 1;
    return EOF;
}

static int
dct_close(Xpost_File *f)
{
    Xpost_DctFile *ff = (Xpost_DctFile *)f;

    jpeg_destroy_decompress(&ff->cinfo);
    free(ff->row);
    ff->row = NULL;
    ff->base.eod = 1;
    ff->base.pushback = -1;
    return 0;
}
#endif

static int
filter_writech(Xpost_File *f, int c)
{
    (void)f;
    (void)c;
    return EOF;
}

static int
filter_close(Xpost_File *f)
{
    Xpost_FilterBase *ff = (Xpost_FilterBase *)f;

    ff->eod = 1;
    ff->pushback = -1;
    return 0;
}

/* flushfile on an input file reads and discards to end of data (PLRM 8.2):
   programs drain a decode filter to position the underlying file just past
   an inline stream they skip. */
static int
filter_flush(Xpost_File *f)
{
    while (f->methods->readch(f) != EOF)
        ;
    return 0;
}

static void
filter_purge(Xpost_File *f)
{
    Xpost_FilterBase *ff = (Xpost_FilterBase *)f;

    ff->eod = 1;
    ff->pushback = -1;
}

static int
filter_unreadch(Xpost_File *f, int c)
{
    Xpost_FilterBase *ff = (Xpost_FilterBase *)f;

    if (ff->pushback >= 0)
        return EOF;
    ff->pushback = c;
    return 0;
}

/* A filter delivers a stream of decoded (or encoded) bytes with no
   relation to a position in anything underneath it, and cannot be
   repositioned; report it as unpositionable, which fileposition and
   setfileposition raise ioerror for (PLRM 8.2). */
static long long
filter_tell(Xpost_File *f)
{
    (void)f;
    return -1;
}

static int
filter_seek(Xpost_File *f, long long offset)
{
    (void)f;
    (void)offset;
    return -1;
}

struct Xpost_File_Methods hex_methods =
{
    hex_readch, filter_writech, filter_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};

struct Xpost_File_Methods rle_methods =
{
    rle_readch, filter_writech, filter_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};

struct Xpost_File_Methods subfile_methods =
{
    subfile_readch, filter_writech, filter_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};

#ifdef HAVE_ZLIB
static int
flate_close(Xpost_File *f)
{
    Xpost_FlateFile *ff = (Xpost_FlateFile *)f;

    inflateEnd(&ff->strm);
    ff->base.eod = 1;
    ff->base.pushback = -1;
    return 0;
}

struct Xpost_File_Methods flate_methods =
{
    flate_readch, filter_writech, flate_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};
#endif

#ifdef HAVE_LIBJPEG
struct Xpost_File_Methods dct_methods =
{
    dct_readch, filter_writech, dct_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};
#endif

static Xpost_Object _filter_object_cons(Xpost_Memory_File *mem, Xpost_File *ff,
                                        Xpost_File_Methods *methods,
                                        Xpost_File_Wraps wraps,
                                        Xpost_File *under);
static Xpost_Object _dec_cons(Xpost_Memory_File *mem, Xpost_FilterBase *ff,
                              Xpost_File_Methods *methods, Xpost_File *source);
static Xpost_File *_filter_underlying_stream(Xpost_File *f);

/* Predictor stage (PLRM Table 3.20). The compressed data of an LZW or
   Flate stream may have been differenced before compression, either
   horizontally (TIFF predictor 2) or by one of the PNG row filters
   (predictor 10 and above). Undoing that is a transformation of the
   decompressed bytes and nothing to do with either decompressor, so it
   layers over whichever one produced them.

   A PNG row carries a leading byte naming the filter used for it; a
   TIFF row does not. Both need the previous decoded row, so a row is
   decoded whole and handed out a byte at a time. */
typedef struct Xpost_PredFile
{
    Xpost_FilterBase base;
    int predictor;
    int colors;
    int bpc;
    int columns;
    int rowbytes;    /* bytes in one decoded row */
    int bpp;         /* bytes between a byte and its left neighbour */
    unsigned char *cur;
    unsigned char *prev;
    int have;        /* bytes decoded into cur */
    int pos;         /* next byte to hand out */
} Xpost_PredFile;

static int
_paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

/* fill cur with one decoded row; 0 at end of data */
static int
_pred_row(Xpost_PredFile *ff)
{
    int i;
    int ft = 0;

    if (ff->predictor >= 10)
    {
        ft = ff->base.source->methods->readch(ff->base.source);
        if (ft == EOF)
            return 0;
        if (ft < 0 || ft > 4)
            return -1;
    }
    for (i = 0; i < ff->rowbytes; i++)
    {
        int c = ff->base.source->methods->readch(ff->base.source);
        if (c == EOF)
        {
            if (i == 0)
                return 0;
            break;
        }
        ff->cur[i] = (unsigned char)c;
    }
    ff->have = i;
    if (ff->predictor == 2)
    {
        /* horizontal differencing, defined here for whole-byte samples */
        for (i = ff->bpp; i < ff->have; i++)
            ff->cur[i] = (unsigned char)(ff->cur[i] + ff->cur[i - ff->bpp]);
    }
    else
    {
        for (i = 0; i < ff->have; i++)
        {
            int a = (i >= ff->bpp) ? ff->cur[i - ff->bpp] : 0;
            int b = ff->prev[i];
            int c = (i >= ff->bpp) ? ff->prev[i - ff->bpp] : 0;
            int x = ff->cur[i];

            switch (ft)
            {
                case 0: break;
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) / 2; break;
                default: x += _paeth(a, b, c); break;
            }
            ff->cur[i] = (unsigned char)x;
        }
    }
    memcpy(ff->prev, ff->cur, (size_t)ff->have);
    ff->pos = 0;
    return 1;
}

static int
pred_readch(Xpost_File *f)
{
    Xpost_PredFile *ff = (Xpost_PredFile *)f;

    if (ff->base.pushback >= 0)
    {
        int c = ff->base.pushback;
        ff->base.pushback = -1;
        return c;
    }
    if (ff->base.eod)
        return EOF;
    while (ff->pos >= ff->have)
    {
        int r = _pred_row(ff);

        if (r <= 0)
        {
            ff->base.eod = 1;
            return EOF;
        }
    }
    return ff->cur[ff->pos++];
}

static int
pred_close(Xpost_File *f)
{
    Xpost_PredFile *ff = (Xpost_PredFile *)f;

    free(ff->cur);
    free(ff->prev);
    ff->cur = ff->prev = NULL;
    ff->base.eod = 1;
    return filter_close(f);
}

static struct Xpost_File_Methods pred_methods =
{
    pred_readch, filter_writech, pred_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};

Xpost_Object xpost_file_cons_filter_predictor(Xpost_Memory_File *mem,
                                              Xpost_Object src,
                                              int predictor, int colors,
                                              int bpc, int columns)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_PredFile *ff;
    Xpost_Object layered;
    int rowbits;

    if (!source)
        return invalid;
    if (colors < 1 || columns < 1 || columns > (1 << 20))
        return invalid;
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)
        return invalid;
    ff = calloc(1, sizeof *ff);
    if (!ff)
        return invalid;
    rowbits = colors * bpc * columns;
    ff->predictor = predictor;
    ff->colors = colors;
    ff->bpc = bpc;
    ff->columns = columns;
    ff->rowbytes = (rowbits + 7) / 8;
    ff->bpp = (colors * bpc + 7) / 8;
    if (ff->bpp < 1)
        ff->bpp = 1;
    ff->cur = calloc(1, (size_t)ff->rowbytes);
    ff->prev = calloc(1, (size_t)ff->rowbytes);
    if (!ff->cur || !ff->prev)
    {
        free(ff->cur);
        free(ff->prev);
        free(ff);
        return invalid;
    }
    layered = _dec_cons(mem, &ff->base, &pred_methods, source);
    /* the stage stands in front of the decompressor, which the program
       never sees and so can never close: it belongs to the stage now */
    if (xpost_object_get_type(layered) == filetype)
        xpost_file_hand_over(mem, src);
    return layered;
}

/* a most-significant-bit-first code reader shared by the LZW and CCITT
   decoders, the mirror of Xpost_BitEncBase below */
typedef struct
{
    Xpost_FilterBase base;
    unsigned int bitbuf;
    int bitcnt;
} Xpost_BitDecBase;

/* the next n bits of the source, high bit first, or -1 at its end */
static int
bitdec_get(Xpost_BitDecBase *ff, int n)
{
    int c;

    while (ff->bitcnt < n)
    {
        c = xpost_file_getc(ff->base.source);
        if (c == EOF)
            return -1;
        ff->bitbuf = ff->bitbuf << 8 | (unsigned int)c;
        ff->bitcnt += 8;
    }
    ff->bitcnt -= n;
    return (int)(ff->bitbuf >> ff->bitcnt) & ((1 << n) - 1);
}

/* LZWDecode filter: the variable-width LZW codes PostScript and PDF
   share -- 9 to 12 bits, packed high bit first, code 256 clearing
   the table and 257 ending the data, with the width growing one
   code early under the default EarlyChange. Table entries chain
   through prefix links; a code expands by walking the chain onto a
   stack and draining it byte by byte. */
typedef struct Xpost_LzwFile
{
    Xpost_BitDecBase base;
    int codewidth;
    int nextcode;
    int early;
    int prev;
    unsigned short prefix[4096];
    unsigned char suffix[4096];
    unsigned char stack[4096];
    int sp;
    int nextval, havenext;   /* one code read ahead so a trailing 257 is eaten */
} Xpost_LzwFile;

static int
lzw_nextcode(Xpost_LzwFile *ff)
{
    return bitdec_get(&ff->base, ff->codewidth);
}

/* The end-of-data code has just been read, so the source is byte-aligned past
   the LZW padding. Nudge any decoding filter beneath (hex, base-85) to swallow
   its own in-band terminator, leaving the underlying file past the whole encoded
   stream; a plain byte is put back untouched. */
static void
lzw_eat_eod(Xpost_LzwFile *ff)
{
    int c = xpost_file_getc(ff->base.base.source);
    if (c != EOF)
        xpost_file_ungetc(ff->base.base.source, c);
    ff->base.base.eod = 1;
}

static int
lzw_getcode(Xpost_LzwFile *ff)
{
    if (ff->havenext)
    {
        ff->havenext = 0;
        return ff->nextval;
    }
    return lzw_nextcode(ff);
}

static int
lzw_readch(Xpost_File *f)
{
    Xpost_LzwFile *ff = (Xpost_LzwFile *)f;
    int code, k;

    if (ff->base.base.pushback >= 0)
    {
        code = ff->base.base.pushback;
        ff->base.base.pushback = -1;
        return code;
    }
    if (ff->sp > 0)
        return ff->stack[--ff->sp];
    if (ff->base.base.eod)
        return EOF;

    for (;;)
    {
        code = lzw_getcode(ff);
        if (code < 0 || code == 257)
        {
            if (code == 257)
                lzw_eat_eod(ff);
            else
                ff->base.base.eod = 1;
            return EOF;
        }
        if (code == 256)
        {
            ff->codewidth = 9;
            ff->nextcode = 258;
            ff->prev = -1;
            continue;
        }
        break;
    }

    /* expand the code onto the stack, reversed so the root -- the
       string's first byte -- sits on top; the not-yet-defined code
       is the previous string plus its own first byte */
    if (ff->prev >= 0 && code == ff->nextcode)
    {
        int w = ff->prev;
        while (w >= 258)
        {
            ff->stack[ff->sp++] = ff->suffix[w];
            w = ff->prefix[w];
        }
        ff->stack[ff->sp++] = (unsigned char)w;
        /* first byte of the previous string repeats at the end */
        {
            unsigned char first = ff->stack[ff->sp - 1];
            int i;
            for (i = ff->sp; i > 0; i--)
                ff->stack[i] = ff->stack[i - 1];
            ff->stack[0] = first;
            ff->sp++;
        }
    }
    else if (code < 256 || (code >= 258 && code < ff->nextcode))
    {
        int w = code;
        while (w >= 258)
        {
            ff->stack[ff->sp++] = ff->suffix[w];
            w = ff->prefix[w];
        }
        ff->stack[ff->sp++] = (unsigned char)w;
    }
    else
    {
        XPOST_LOG_ERR("LZWDecode: code out of range");
        ff->base.base.eod = 1;
        return EOF;
    }

    if (ff->prev >= 0 && ff->nextcode < 4096)
    {
        ff->prefix[ff->nextcode] = (unsigned short)ff->prev;
        ff->suffix[ff->nextcode] = ff->stack[ff->sp - 1]; /* first byte of current */
        ff->nextcode++;
    }
    if (ff->nextcode + ff->early >= (1 << ff->codewidth)
     && ff->codewidth < 12)
        ff->codewidth++;
    ff->prev = code;

    /* Read the following code ahead of need. A bounded readstring stops the
       moment it has its byte count and never reads again, so a lazily-read 257
       would be stranded, desynchronising a fresh filter on the same stream. Peek
       it here, with the table and code width in the state this code left them:
       a trailing 257 is eaten now (source left byte-aligned past the padding); a
       real code is cached for the next call; a clear code is handled there too. */
    {
        int nxt = lzw_nextcode(ff);
        if (nxt == 257)
            lzw_eat_eod(ff);
        else if (nxt < 0)
            ff->base.base.eod = 1;
        else
        {
            ff->nextval = nxt;
            ff->havenext = 1;
        }
    }

    k = ff->stack[--ff->sp];
    return k;
}

struct Xpost_File_Methods lzw_methods =
{
    lzw_readch, filter_writech, filter_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};

Xpost_Object xpost_file_cons_filter_lzw(Xpost_Memory_File *mem, Xpost_Object src, int early)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_LzwFile *ff;

    if (!source)
        return invalid;
    ff = calloc(1, sizeof *ff);
    if (!ff)
        return invalid;
    ff->codewidth = 9;
    ff->nextcode = 258;
    ff->early = early;
    ff->prev = -1;
    return _dec_cons(mem, &ff->base.base, &lzw_methods, source);
}

/* CCITTFaxDecode filter: the Group 3 and Group 4 facsimile codings
   of ITU-T T.4 and T.6.  K selects the scheme -- zero is
   one-dimensional Modified Huffman, negative is the purely
   two-dimensional Group 4 coding, positive mixes the two, a tag bit
   behind each row's end-of-line marker naming its coding.  Rows
   decode into changing-element position lists; the two-dimensional
   modes place each element against the previous row's list. */

typedef struct
{
    short run;
    unsigned char len;
    unsigned short code;
} Xpost_Fax_Code;

static const Xpost_Fax_Code fax_white[] =
{
    {2,4,0x007}, {3,4,0x008}, {4,4,0x00b}, {5,4,0x00c}, {6,4,0x00e},
    {7,4,0x00f}, {10,5,0x007}, {11,5,0x008}, {128,5,0x012}, {8,5,0x013},
    {9,5,0x014}, {64,5,0x01b}, {13,6,0x003}, {1,6,0x007}, {12,6,0x008},
    {192,6,0x017}, {1664,6,0x018}, {16,6,0x02a}, {17,6,0x02b}, {14,6,0x034},
    {15,6,0x035}, {22,7,0x003}, {23,7,0x004}, {20,7,0x008}, {19,7,0x00c},
    {26,7,0x013}, {21,7,0x017}, {28,7,0x018}, {27,7,0x024}, {18,7,0x027},
    {24,7,0x028}, {25,7,0x02b}, {256,7,0x037}, {29,8,0x002}, {30,8,0x003},
    {45,8,0x004}, {46,8,0x005}, {47,8,0x00a}, {48,8,0x00b}, {33,8,0x012},
    {34,8,0x013}, {35,8,0x014}, {36,8,0x015}, {37,8,0x016}, {38,8,0x017},
    {31,8,0x01a}, {32,8,0x01b}, {53,8,0x024}, {54,8,0x025}, {39,8,0x028},
    {40,8,0x029}, {41,8,0x02a}, {42,8,0x02b}, {43,8,0x02c}, {44,8,0x02d},
    {61,8,0x032}, {62,8,0x033}, {63,8,0x034}, {0,8,0x035}, {320,8,0x036},
    {384,8,0x037}, {59,8,0x04a}, {60,8,0x04b}, {49,8,0x052}, {50,8,0x053},
    {51,8,0x054}, {52,8,0x055}, {55,8,0x058}, {56,8,0x059}, {57,8,0x05a},
    {58,8,0x05b}, {448,8,0x064}, {512,8,0x065}, {640,8,0x067}, {576,8,0x068},
    {1472,9,0x098}, {1536,9,0x099}, {1600,9,0x09a}, {1728,9,0x09b}, {704,9,0x0cc},
    {768,9,0x0cd}, {832,9,0x0d2}, {896,9,0x0d3}, {960,9,0x0d4}, {1024,9,0x0d5},
    {1088,9,0x0d6}, {1152,9,0x0d7}, {1216,9,0x0d8}, {1280,9,0x0d9}, {1344,9,0x0da},
    {1408,9,0x0db},
};

static const Xpost_Fax_Code fax_black[] =
{
    {3,2,0x002}, {2,2,0x003}, {1,3,0x002}, {4,3,0x003}, {6,4,0x002},
    {5,4,0x003}, {7,5,0x003}, {9,6,0x004}, {8,6,0x005}, {10,7,0x004},
    {11,7,0x005}, {12,7,0x007}, {13,8,0x004}, {14,8,0x007}, {15,9,0x018},
    {18,10,0x008}, {64,10,0x00f}, {16,10,0x017}, {17,10,0x018}, {0,10,0x037},
    {24,11,0x017}, {25,11,0x018}, {23,11,0x028}, {22,11,0x037}, {19,11,0x067},
    {20,11,0x068}, {21,11,0x06c}, {52,12,0x024}, {55,12,0x027}, {56,12,0x028},
    {59,12,0x02b}, {60,12,0x02c}, {320,12,0x033}, {384,12,0x034}, {448,12,0x035},
    {53,12,0x037}, {54,12,0x038}, {50,12,0x052}, {51,12,0x053}, {44,12,0x054},
    {45,12,0x055}, {46,12,0x056}, {47,12,0x057}, {57,12,0x058}, {58,12,0x059},
    {61,12,0x05a}, {256,12,0x05b}, {48,12,0x064}, {49,12,0x065}, {62,12,0x066},
    {63,12,0x067}, {30,12,0x068}, {31,12,0x069}, {32,12,0x06a}, {33,12,0x06b},
    {40,12,0x06c}, {41,12,0x06d}, {128,12,0x0c8}, {192,12,0x0c9}, {26,12,0x0ca},
    {27,12,0x0cb}, {28,12,0x0cc}, {29,12,0x0cd}, {34,12,0x0d2}, {35,12,0x0d3},
    {36,12,0x0d4}, {37,12,0x0d5}, {38,12,0x0d6}, {39,12,0x0d7}, {42,12,0x0da},
    {43,12,0x0db}, {640,13,0x04a}, {704,13,0x04b}, {768,13,0x04c}, {832,13,0x04d},
    {1280,13,0x052}, {1344,13,0x053}, {1408,13,0x054}, {1472,13,0x055}, {1536,13,0x05a},
    {1600,13,0x05b}, {1664,13,0x064}, {1728,13,0x065}, {512,13,0x06c}, {576,13,0x06d},
    {896,13,0x072}, {960,13,0x073}, {1024,13,0x074}, {1088,13,0x075}, {1152,13,0x076},
    {1216,13,0x077},
};

static const Xpost_Fax_Code fax_ext[] =
{
    {1792,11,0x008}, {1856,11,0x00c}, {1920,11,0x00d}, {1984,12,0x012}, {2048,12,0x013},
    {2112,12,0x014}, {2176,12,0x015}, {2240,12,0x016}, {2304,12,0x017}, {2368,12,0x01c},
    {2432,12,0x01d}, {2496,12,0x01e}, {2560,12,0x01f},
};

/* The two run-length tables hold the same number of codes, so a caller
   that has chosen one of them by colour needs no second choice for its
   extent. Said here rather than left to hold by luck: a code added to
   one table alone would leave the walks below reading the wrong number
   of entries for the other, and nothing would say so. (A negative array
   size rather than _Static_assert: this builds as C99 with
   -pedantic-errors, which rejects the latter.) */
typedef char xpost_fax_run_tables_are_the_same_length[
    sizeof(fax_black) == sizeof(fax_white) ? 1 : -1];

typedef struct Xpost_FaxFile
{
    Xpost_BitDecBase base;
    int k;
    int columns;
    int rows;
    int blackis1;
    int byteal;
    int eol;
    int eob;
    int *ref, *cur;     /* changing-element positions, padded */
    int refcnt;
    unsigned char *row;
    int rowbytes;
    int rowpos;
    int rowsdone;
    int preeol;         /* end-of-line codes taken before the row decoder ran */
} Xpost_FaxFile;

static int
fax_bit(Xpost_FaxFile *ff)
{
    return bitdec_get(&ff->base, 1);
}

enum { FAX_RUN_EOL = -2, FAX_RUN_ERR = -1 };

/* one complete run length of the given colour: makeup codes add to
   a following terminating code.  An end-of-line code stands in
   either table so fill bits and markers surface here. */
static int
fax_runlength(Xpost_FaxFile *ff, int color)
{
    int total = 0;

    for (;;)
    {
        const Xpost_Fax_Code *tab = color ? fax_black : fax_white;
        int n = (int)(sizeof(fax_white)/sizeof(*fax_white));
        int next = (int)(sizeof(fax_ext)/sizeof(*fax_ext));
        unsigned int code = 0;
        int len = 0, run = FAX_RUN_ERR, i, b;

        while (len < 14 && run == FAX_RUN_ERR)
        {
            b = fax_bit(ff);
            if (b < 0)
                return FAX_RUN_ERR;
            code = code << 1 | (unsigned int)b;
            len++;
            if (len == 12 && code == 1)
                return total ? FAX_RUN_ERR : FAX_RUN_EOL;
            for (i = 0; i < n; i++)
                if (tab[i].len == len && tab[i].code == code)
                {
                    run = tab[i].run;
                    break;
                }
            if (run == FAX_RUN_ERR)
                for (i = 0; i < next; i++)
                    if (fax_ext[i].len == len && fax_ext[i].code == code)
                    {
                        run = fax_ext[i].run;
                        break;
                    }
        }
        if (run == FAX_RUN_ERR)
            return FAX_RUN_ERR;
        total += run;
        if (run < 64)
            return total;
    }
}

enum
{
    FAX_P, FAX_H, FAX_V0,
    FAX_VR1, FAX_VR2, FAX_VR3,
    FAX_VL1, FAX_VL2, FAX_VL3,
    FAX_EOL, FAX_ERR
};

static int
fax_mode(Xpost_FaxFile *ff)
{
    int b, z;

    if ((b = fax_bit(ff)) < 0) return FAX_ERR;
    if (b) return FAX_V0;
    if ((b = fax_bit(ff)) < 0) return FAX_ERR;
    if (b)
    {
        if ((b = fax_bit(ff)) < 0) return FAX_ERR;
        return b ? FAX_VR1 : FAX_VL1;
    }
    if ((b = fax_bit(ff)) < 0) return FAX_ERR;
    if (b) return FAX_H;
    if ((b = fax_bit(ff)) < 0) return FAX_ERR;
    if (b) return FAX_P;
    if ((b = fax_bit(ff)) < 0) return FAX_ERR;
    if (b)
    {
        if ((b = fax_bit(ff)) < 0) return FAX_ERR;
        return b ? FAX_VR2 : FAX_VL2;
    }
    if ((b = fax_bit(ff)) < 0) return FAX_ERR;
    if (b)
    {
        if ((b = fax_bit(ff)) < 0) return FAX_ERR;
        return b ? FAX_VR3 : FAX_VL3;
    }
    /* six zeros so far: only an end-of-line marker continues this way */
    for (z = 6; z < 64; z++)
    {
        if ((b = fax_bit(ff)) < 0) return FAX_ERR;
        if (b)
            return z >= 11 ? FAX_EOL : FAX_ERR;
    }
    return FAX_ERR;
}

/* consume fill bits and one end-of-line marker; -1 on anything else */
static int
fax_eateol(Xpost_FaxFile *ff)
{
    int b, z = 0;

    while (z < 64)
    {
        b = fax_bit(ff);
        if (b < 0)
            return -1;
        if (b)
            return z >= 11 ? 0 : -1;
        z++;
    }
    return -1;
}

/* how many end-of-line codes in a row close the data: two for a Group 4
   stream, six -- the Return To Control -- for a Group 3 one.  One place,
   because the row decoders have to recognise the same marker the finish
   consumes; when they disagreed, the decoder read the marker as six
   separators and went on to decode whatever followed it. */
static int
fax_blocklen(const Xpost_FaxFile *ff)
{
    return ff->k < 0 ? 2 : 6;
}

/* the trailing block marker; the source then stands at the next byte.
   Absent or malformed trailers are left alone. */
static void
fax_finish(Xpost_FaxFile *ff)
{
    int need = fax_blocklen(ff);
    int got = 0, c;

    if (ff->eob)
    {
        while (got < need && fax_eateol(ff) == 0)
        {
            got++;
            if (ff->k > 0 && got < need)
                (void)fax_bit(ff); /* tag bit trails each marker */
        }
    }
    ff->base.bitcnt = 0;
    /* as the data ends, an encoding filter beneath swallows its own
       in-band terminator producing the peeked byte; a plain byte is
       put back untouched */
    c = xpost_file_getc(ff->base.base.source);
    if (c != EOF)
        xpost_file_ungetc(ff->base.base.source, c);
    ff->base.base.eod = 1;
}

static int
fax_1d_row(Xpost_FaxFile *ff)
{
    int pos = 0, color = 0, ci = 0, run, guard = 0;
    int eols = ff->preeol;

    while (pos < ff->columns)
    {
        if (++guard > 2 * ff->columns + 64)
            return -1;
        /* a row holds at most one changing element per pixel; keep ci within
           the changing-element buffer, as the two-dimensional decoder does */
        if (ci > ff->columns)
            return -1;
        run = fax_runlength(ff, color);
        if (run == FAX_RUN_EOL)
        {
            if (pos == 0 && ci == 0)
            {
                /* one marker separates rows; a block's worth of them in
                   succession is the end of the data, and stopping there
                   is what leaves the bytes after it for the next reader */
                if (ff->eob && ++eols >= fax_blocklen(ff))
                    return -2;
                continue;   /* marker before the row */
            }
            return -1;
        }
        if (run < 0)
            return ci == 0 && pos == 0 ? -2 : -1; /* clean EOF at a row edge */
        pos += run;
        if (pos > ff->columns)
            pos = ff->columns;
        ff->cur[ci++] = pos;
        color ^= 1;
    }
    ff->cur[ci] = ff->cur[ci + 1] = ff->columns;
    ff->refcnt = ci;
    return 0;
}

static int
fax_2d_row(Xpost_FaxFile *ff)
{
    int a0 = -1, color = 0, ci = 0, ri = 0, eols = ff->preeol;
    int b1, b2, a1, r1, r2, start, mode;

    while (a0 < ff->columns)
    {
        if (ci > ff->columns)
            return -1;
        /* b1: first reference change right of a0 toward the colour
           opposite the current one; changes alternate to-black,
           to-white from an even origin.  The left vertical modes
           move a0 backward, so the walk goes down before up */
        while (ri > 0 && (ri >= ff->refcnt || ff->ref[ri - 1] > a0))
            ri--;
        while (ri < ff->refcnt && ff->ref[ri] <= a0)
            ri++;
        if ((ri ^ color) & 1)
            ri++;
        b1 = ri < ff->refcnt ? ff->ref[ri] : ff->columns;
        b2 = ri + 1 < ff->refcnt ? ff->ref[ri + 1] : ff->columns;

        mode = fax_mode(ff);
        switch (mode)
        {
        case FAX_P:
            a0 = b2;
            break;
        case FAX_H:
            start = a0 < 0 ? 0 : a0;
            r1 = fax_runlength(ff, color);
            r2 = r1 < 0 ? r1 : fax_runlength(ff, !color);
            if (r1 < 0 || r2 < 0)
                return -1;
            if (start + r1 > ff->columns) r1 = ff->columns - start;
            if (start + r1 + r2 > ff->columns) r2 = ff->columns - start - r1;
            ff->cur[ci++] = start + r1;
            ff->cur[ci++] = start + r1 + r2;
            a0 = start + r1 + r2;
            break;
        case FAX_V0: case FAX_VR1: case FAX_VR2: case FAX_VR3:
        case FAX_VL1: case FAX_VL2: case FAX_VL3:
            a1 = b1;
            if (mode >= FAX_VR1 && mode <= FAX_VR3)
                a1 += mode - FAX_VR1 + 1;
            else if (mode >= FAX_VL1)
                a1 -= mode - FAX_VL1 + 1;
            if (a1 < 0 || a1 > ff->columns)
                return -1;
            ff->cur[ci++] = a1;
            a0 = a1;
            color ^= 1;
            break;
        case FAX_EOL:
            if (a0 < 0 && ci == 0)
            {
                /* the same count the finish consumes: a Group 4 stream is
                   closed by a pair and a Group 3 one by six, and reaching
                   it here is what stops the data being read past */
                if (ff->eob && ++eols >= fax_blocklen(ff))
                    return -2;
                if (!ff->eob && ff->k < 0)  /* first of the closing pair */
                    return -2;
                continue;
            }
            return -1;
        default:
            return a0 < 0 && ci == 0 ? -2 : -1;
        }
    }
    ff->cur[ci] = ff->cur[ci + 1] = ff->columns;
    ff->refcnt = ci;
    return 0;
}

static int
fax_decoderow(Xpost_FaxFile *ff)
{
    int ret, twod, b, *tmp;
    int i, ci, pos;

    if (ff->rows > 0 && ff->rowsdone >= ff->rows)
    {
        fax_finish(ff);
        return EOF;
    }
    if (ff->byteal && ff->base.bitcnt < 8)
        ff->base.bitcnt = 0;
    /* the mixed coding types each row by a tag bit behind an
       end-of-line marker, so for positive K the marker is
       structural, whatever EndOfLine says of the plain codings */
    /* a marker read here is the first of any block marker that follows, so
       the row decoder is told about it: counting the block from zero would
       leave it one short and read on past the end of the data */
    ff->preeol = 0;
    if (ff->k >= 0 && (ff->eol || ff->k > 0))
    {
        if (fax_eateol(ff) < 0)
        {
            if (ff->k > 0)
                XPOST_LOG_ERR("CCITTFaxDecode: no end-of-line marker "
                              "before row %d", ff->rowsdone);
            ff->base.base.eod = 1;
            return EOF;
        }
        ff->preeol = 1;
    }
    if (ff->k < 0)
        twod = 1;
    else if (ff->k == 0)
        twod = 0;
    else
    {
        /* a tag bit rides behind each end-of-line marker */
        b = fax_bit(ff);
        if (b < 0)
        {
            ff->base.base.eod = 1;
            return EOF;
        }
        twod = !b;
    }

    ret = twod ? fax_2d_row(ff) : fax_1d_row(ff);
    if (ret == -2)  /* the stream closed at a row boundary */
    {
        /* the row decoder consumed the whole block marker, so nothing is
           read here: another end-of-line read speculatively would take
           bits off whatever follows the data and lose them */
        ff->base.bitcnt = 0;
        b = xpost_file_getc(ff->base.base.source);
        if (b != EOF)
            xpost_file_ungetc(ff->base.base.source, b);
        ff->base.base.eod = 1;
        return EOF;
    }
    if (ret < 0)
    {
        XPOST_LOG_ERR("CCITTFaxDecode: damaged row %d", ff->rowsdone);
        ff->base.base.eod = 1;
        return EOF;
    }

    /* render the changing elements: runs alternate from white */
    memset(ff->row, ff->blackis1 ? 0x00 : 0xff, (size_t)ff->rowbytes);
    ci = ff->refcnt;
    for (i = 0; i < ci; i += 2)
    {
        int to = i + 1 < ci ? ff->cur[i + 1] : ff->columns;
        for (pos = ff->cur[i]; pos < to; pos++)
        {
            if (ff->blackis1)
                ff->row[pos >> 3] |= (unsigned char)(0x80 >> (pos & 7));
            else
                ff->row[pos >> 3] &= (unsigned char)~(0x80 >> (pos & 7));
        }
    }

    tmp = ff->ref; ff->ref = ff->cur; ff->cur = tmp;
    ff->rowpos = 0;
    ff->rowsdone++;
    return 0;
}

static int
fax_readch(Xpost_File *f)
{
    Xpost_FaxFile *ff = (Xpost_FaxFile *)f;
    int c;

    if (ff->base.base.pushback >= 0)
    {
        c = ff->base.base.pushback;
        ff->base.base.pushback = -1;
        return c;
    }
    if (ff->rowpos >= ff->rowbytes)
    {
        if (ff->base.base.eod)
            return EOF;
        if (fax_decoderow(ff) == EOF)
            return EOF;
    }
    return ff->row[ff->rowpos++];
}

static int
fax_close(Xpost_File *f)
{
    Xpost_FaxFile *ff = (Xpost_FaxFile *)f;

    free(ff->ref);
    free(ff->cur);
    free(ff->row);
    ff->ref = ff->cur = NULL;
    ff->row = NULL;
    ff->base.base.eod = 1;
    ff->base.base.pushback = -1;
    return 0;
}

struct Xpost_File_Methods fax_methods =
{
    fax_readch, filter_writech, fax_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};

Xpost_Object xpost_file_cons_filter_ccitt(Xpost_Memory_File *mem,
                                          Xpost_Object src,
                                          int k, int columns, int rows,
                                          int blackis1, int byteal,
                                          int eol, int eob)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_FaxFile *ff;

    if (!source)
        return invalid;
    if (columns < 1 || columns > (1 << 20))
        return invalid;
    ff = calloc(1, sizeof *ff);
    if (!ff)
        return invalid;
    ff->k = k;
    ff->columns = columns;
    ff->rows = rows;
    ff->blackis1 = blackis1;
    ff->byteal = byteal;
    ff->eol = eol;
    ff->eob = eob;
    ff->rowbytes = (columns + 7) / 8;
    ff->ref = calloc((size_t)columns + 4, sizeof(int));
    ff->cur = calloc((size_t)columns + 4, sizeof(int));
    ff->row = malloc((size_t)ff->rowbytes);
    if (!ff->ref || !ff->cur || !ff->row)
    {
        free(ff->ref); free(ff->cur); free(ff->row); free(ff);
        return invalid;
    }
    /* the imaginary all-white line above the first row */
    ff->refcnt = 0;
    ff->rowpos = ff->rowbytes;
    return _dec_cons(mem, &ff->base.base, &fax_methods, source);
}

/* Encoding filters: write-side counterparts of the decode filters.
   Bytes pushed at the filter come out encoded on the target file;
   closing the filter writes the coding's end-of-data marker and
   leaves the target open.

   Every coding says the same three things about being closed: a closed
   filter takes no more bytes, the end-of-data is written once and only on
   the first close, and whatever the coding holds is given up there too.
   None of that varies with the coding, so none of it is written by one:
   the base below decides it, and a coding supplies only how a byte is
   encoded, what its end-of-data is, and what it has to release.

   The close answers 0 for a coding that reached its target, and EOF for
   one whose closing bytes -- the end of the encoded data, the marker
   after it, or both -- the target would not take. That answer reaches
   closefile, because a stream that stops short of its own terminator is
   not one a program can be told it wrote (PLRM 3.13.1). The close still
   runs to the end and releases whatever the coding holds, so the file is
   closed either way. */

typedef struct Xpost_EncBase Xpost_EncBase;

typedef struct
{
    /* one byte onto the target, encoded: the byte, or EOF if the target
       would not take it */
    int (*encode)(Xpost_EncBase *, int);
    /* the coding's end-of-data, written once; EOF if it did not arrive */
    int (*finish)(Xpost_EncBase *);
    /* whatever the coding holds beyond its own struct */
    void (*release)(Xpost_EncBase *);
} Xpost_Enc_Coding;

struct Xpost_EncBase
{
    Xpost_File methods;
    Xpost_File *target;
    int closed;
    const Xpost_Enc_Coding *coding;
};

static int
enc_readch(Xpost_File *f)
{
    (void)f;
    return EOF;
}

/* A coding with no end-of-data marker has nothing that can be refused it,
   and one that holds nothing beyond its own struct has nothing to give
   up. Both say so, rather than leaving a hole for the base to test for. */
static int
enc_finish_none(Xpost_EncBase *ff)
{
    (void)ff;
    return 0;
}

static void
enc_release_none(Xpost_EncBase *ff)
{
    (void)ff;
}

static int
enc_writech(Xpost_File *f, int c)
{
    Xpost_EncBase *ff = (Xpost_EncBase *)f;

    if (ff->closed)
        return EOF;
    return ff->coding->encode(ff, c & 0xff);
}

static int
enc_close(Xpost_File *f)
{
    Xpost_EncBase *ff = (Xpost_EncBase *)f;
    int ret = 0;

    if (!ff->closed)
    {
        ff->closed = 1;
        if (ff->coding->finish(ff) == EOF)
            ret = EOF;
        ff->coding->release(ff);
    }
    return ret;
}

static int
enc_flush(Xpost_File *f)
{
    Xpost_EncBase *ff = (Xpost_EncBase *)f;

    return xpost_file_flush(ff->target);
}

/* Discard what the target has buffered and go on encoding: resetfile
   drops characters that have not been written, and leaves the file open
   (PLRM 8.2). Whatever part of a coding group the encoder itself is
   holding stays with it, since the base has no reach into the coding. */
static void
enc_purge(Xpost_File *f)
{
    Xpost_EncBase *ff = (Xpost_EncBase *)f;

    xpost_file_purge(ff->target);
}

static int
enc_unreadch(Xpost_File *f, int c)
{
    (void)f;
    (void)c;
    return EOF;
}

/* the one method table of the encode family: every coding answers with
   these, and differs only in the coding hooks below */
static struct Xpost_File_Methods enc_methods =
{
    enc_readch, enc_writech, enc_close, enc_flush,
    enc_purge, enc_unreadch, filter_tell, filter_seek
};

/* NullEncode: bytes pass through untouched, and there is no end-of-data
   marker to lose */
static int
nullenc_encode(Xpost_EncBase *ff, int c)
{
    if (xpost_file_putc(ff->target, c) == EOF)
        return EOF;
    return c;
}

static const Xpost_Enc_Coding nullenc_coding =
{
    nullenc_encode, enc_finish_none, enc_release_none
};

/* ASCIIHexEncode: two hex digits per byte, a newline every
   thirty-two bytes, '>' at the end */
typedef struct
{
    Xpost_EncBase base;
    int col;
} Xpost_HexEncFile;

static int
hexenc_encode(Xpost_EncBase *base, int c)
{
    Xpost_HexEncFile *ff = (Xpost_HexEncFile *)base;
    static const char digit[] = "0123456789ABCDEF";

    if (xpost_file_putc(ff->base.target, digit[(c >> 4) & 15]) == EOF)
        return EOF;
    if (xpost_file_putc(ff->base.target, digit[c & 15]) == EOF)
        return EOF;
    if (++ff->col == 32)
    {
        ff->col = 0;
        if (xpost_file_putc(ff->base.target, '\n') == EOF)
            return EOF;
    }
    return c;
}

static int
hexenc_finish(Xpost_EncBase *ff)
{
    return xpost_file_putc(ff->target, '>') == EOF ? EOF : 0;
}

static const Xpost_Enc_Coding hexenc_coding =
{
    hexenc_encode, hexenc_finish, enc_release_none
};

/* ASCII85Encode: four bytes become five base-85 digits, an all-zero
   group abbreviates to 'z', the trailing partial group of n bytes
   to its first n+1 digits, and "~>" closes the stream */
typedef struct
{
    Xpost_EncBase base;
    int col;
    unsigned int tuple;
    int n;
} Xpost_A85EncFile;

static int
a85enc_putch(Xpost_A85EncFile *ff, int c)
{
    if (xpost_file_putc(ff->base.target, c) == EOF)
        return EOF;
    /* PLRM 3.13.3: ASCII85Encode "inserts a newline in the encoded
       output at least once every 80 characters", which this satisfies */
    if (++ff->col == 64)
    {
        ff->col = 0;
        if (xpost_file_putc(ff->base.target, '\n') == EOF)
            return EOF;
    }
    return c;
}

static int
a85enc_group(Xpost_A85EncFile *ff, int nbytes)
{
    char digits[5];
    unsigned int tuple = ff->tuple;
    int i;

    if (nbytes == 4 && tuple == 0)
        return a85enc_putch(ff, 'z');
    for (i = 4; i >= 0; i--)
    {
        digits[i] = (char)('!' + tuple % 85);
        tuple /= 85;
    }
    for (i = 0; i <= nbytes; i++)
        if (a85enc_putch(ff, digits[i]) == EOF)
            return EOF;
    return 0;
}

static int
a85enc_encode(Xpost_EncBase *base, int c)
{
    Xpost_A85EncFile *ff = (Xpost_A85EncFile *)base;
    int ret = c;

    ff->tuple = ff->tuple << 8 | (unsigned int)c;
    /* the group ends with the fourth byte whether or not the target took
       it, so what is left over is always a partial one */
    if (++ff->n == 4)
    {
        if (a85enc_group(ff, 4) == EOF)
            ret = EOF;
        ff->tuple = 0;
        ff->n = 0;
    }
    return ret;
}

static int
a85enc_finish(Xpost_EncBase *base)
{
    Xpost_A85EncFile *ff = (Xpost_A85EncFile *)base;
    int ret = 0;

    if (ff->n)
    {
        ff->tuple <<= 8 * (4 - ff->n);
        if (a85enc_group(ff, ff->n) == EOF)
            ret = EOF;
    }
    if (xpost_file_putc(ff->base.target, '~') == EOF)
        ret = EOF;
    if (xpost_file_putc(ff->base.target, '>') == EOF)
        ret = EOF;
    return ret;
}

static const Xpost_Enc_Coding a85enc_coding =
{
    a85enc_encode, a85enc_finish, enc_release_none
};

/* RunLengthEncode: runs of three or more repeated bytes become a
   (257-count, byte) pair, other bytes gather into literal blocks of
   up to 128, and byte 128 ends the data */
typedef struct
{
    Xpost_EncBase base;
    unsigned char buf[128];
    int n;          /* literal bytes gathered */
    int runcnt;     /* repeats of runch gathered */
    int runch;
    int recsize;    /* runs never span record boundaries */
    int reccnt;
} Xpost_RleEncFile;

/* Each of these gives up what it has gathered before it writes it: the
   block leaves the encoder whether or not the target takes it, so what
   stays behind is always less than a full one. */
static int
rleenc_flushlit(Xpost_RleEncFile *ff)
{
    int i, n = ff->n;

    if (n == 0)
        return 0;
    ff->n = 0;
    if (xpost_file_putc(ff->base.target, n - 1) == EOF)
        return EOF;
    for (i = 0; i < n; i++)
        if (xpost_file_putc(ff->base.target, ff->buf[i]) == EOF)
            return EOF;
    return 0;
}

static int
rleenc_flushrun(Xpost_RleEncFile *ff)
{
    int cnt = ff->runcnt;

    if (cnt == 0)
        return 0;
    ff->runcnt = 0;
    if (cnt >= 3)
    {
        if (xpost_file_putc(ff->base.target, 257 - cnt) == EOF)
            return EOF;
        if (xpost_file_putc(ff->base.target, ff->runch) == EOF)
            return EOF;
        return 0;
    }
    while (cnt)
    {
        if (ff->n == 128 && rleenc_flushlit(ff) == EOF)
            return EOF;
        ff->buf[ff->n++] = (unsigned char)ff->runch;
        cnt--;
    }
    return 0;
}

static int
rleenc_encode(Xpost_EncBase *base, int c)
{
    Xpost_RleEncFile *ff = (Xpost_RleEncFile *)base;

    if (ff->recsize > 0 && ff->reccnt == ff->recsize)
    {
        if (ff->runcnt >= 3)
        {
            if (rleenc_flushlit(ff) == EOF || rleenc_flushrun(ff) == EOF)
                return EOF;
        }
        else if (rleenc_flushrun(ff) == EOF || rleenc_flushlit(ff) == EOF)
            return EOF;
        ff->reccnt = 0;
    }
    ff->reccnt++;
    if (ff->runcnt && c == ff->runch)
    {
        if (++ff->runcnt == 128)
        {
            if (rleenc_flushlit(ff) == EOF || rleenc_flushrun(ff) == EOF)
                return EOF;
        }
        return c;
    }
    if (ff->runcnt >= 3)
    {
        if (rleenc_flushlit(ff) == EOF || rleenc_flushrun(ff) == EOF)
            return EOF;
    }
    else if (rleenc_flushrun(ff) == EOF)   /* short run joins the literals */
        return EOF;
    ff->runch = c;
    ff->runcnt = 1;
    return c;
}

static int
rleenc_finish(Xpost_EncBase *base)
{
    Xpost_RleEncFile *ff = (Xpost_RleEncFile *)base;
    int ret = 0;

    if (ff->runcnt >= 3)
    {
        if (rleenc_flushlit(ff) == EOF)
            ret = EOF;
        if (rleenc_flushrun(ff) == EOF)
            ret = EOF;
    }
    else
    {
        if (rleenc_flushrun(ff) == EOF)
            ret = EOF;
        if (rleenc_flushlit(ff) == EOF)
            ret = EOF;
    }
    if (xpost_file_putc(ff->base.target, 128) == EOF)
        ret = EOF;
    return ret;
}

static const Xpost_Enc_Coding rleenc_coding =
{
    rleenc_encode, rleenc_finish, enc_release_none
};

#ifdef HAVE_ZLIB
/* FlateEncode: the zlib compressor */
typedef struct
{
    Xpost_EncBase base;
    z_stream strm;
    unsigned char out[4096];
} Xpost_FlateEncFile;

static int
flateenc_drain(Xpost_FlateEncFile *ff)
{
    int i, n = (int)(sizeof(ff->out) - ff->strm.avail_out);

    for (i = 0; i < n; i++)
        if (xpost_file_putc(ff->base.target, ff->out[i]) == EOF)
            return EOF;
    ff->strm.next_out = ff->out;
    ff->strm.avail_out = sizeof(ff->out);
    return 0;
}

static int
flateenc_encode(Xpost_EncBase *base, int c)
{
    Xpost_FlateEncFile *ff = (Xpost_FlateEncFile *)base;
    unsigned char b = (unsigned char)c;

    ff->strm.next_in = &b;
    ff->strm.avail_in = 1;
    while (ff->strm.avail_in)
    {
        if (deflate(&ff->strm, Z_NO_FLUSH) != Z_OK)
            return EOF;
        if (ff->strm.avail_out == 0 && flateenc_drain(ff) == EOF)
            return EOF;
    }
    return c;
}

static int
flateenc_finish(Xpost_EncBase *base)
{
    Xpost_FlateEncFile *ff = (Xpost_FlateEncFile *)base;
    int zret;
    int ret = 0;

    ff->strm.avail_in = 0;
    do
    {
        zret = deflate(&ff->strm, Z_FINISH);
        if (flateenc_drain(ff) == EOF)
        {
            ret = EOF;
            break;
        }
    } while (zret == Z_OK);
    if (zret != Z_STREAM_END)
        ret = EOF;
    return ret;
}

static void
flateenc_release(Xpost_EncBase *base)
{
    Xpost_FlateEncFile *ff = (Xpost_FlateEncFile *)base;

    deflateEnd(&ff->strm);
}

static const Xpost_Enc_Coding flateenc_coding =
{
    flateenc_encode, flateenc_finish, flateenc_release
};
#endif

#ifdef HAVE_LIBJPEG
/* DCTEncode filter: interleaved samples compress to a JPEG stream on
   the target. The parameter dictionary fixes the layout -- Columns,
   Rows and Colors have no in-stream default -- and QFactor scales
   the standard quantization tables, which is libjpeg's linear
   quality scaling directly. Input short of the declared height is
   completed with zero rows at close so the stream always ends
   well-formed; encoder errors end the stream rather than the
   process, through the same longjmp handler as the decoder. */
typedef struct
{
    Xpost_EncBase base;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    jmp_buf jmp;
    struct jpeg_destination_mgr jdst;
    JOCTET out[4096];
    unsigned char *row;
    unsigned int rowbytes, rowi;
    unsigned int rows, wrote;
    int started;
    int failed;
} Xpost_DctEncFile;

static void
dctenc_error_exit(j_common_ptr cinfo)
{
    Xpost_DctEncFile *ff = (Xpost_DctEncFile *)cinfo->client_data;
    char msg[JMSG_LENGTH_MAX];

    (*cinfo->err->format_message)(cinfo, msg);
    XPOST_LOG_ERR("DCTEncode: %s", msg);
    longjmp(ff->jmp, 1);
}

/* The destination manager reports a target that would not take the
   compressed bytes. It leaves the compressor the same way an error
   inside it would, but the compressor never raised one, so it has no
   message to format. */
static void
dctenc_target_refused(j_common_ptr cinfo)
{
    Xpost_DctEncFile *ff = (Xpost_DctEncFile *)cinfo->client_data;

    XPOST_LOG_ERR("DCTEncode: the data target would not take the stream");
    longjmp(ff->jmp, 1);
}

static void
dctenc_output_message(j_common_ptr cinfo)
{
    char msg[JMSG_LENGTH_MAX];

    (*cinfo->err->format_message)(cinfo, msg);
    XPOST_LOG_ERR("DCTEncode: %s", msg);
}

static void
dctenc_init_destination(j_compress_ptr cinfo)
{
    Xpost_DctEncFile *ff = (Xpost_DctEncFile *)cinfo->client_data;

    ff->jdst.next_output_byte = ff->out;
    ff->jdst.free_in_buffer = sizeof(ff->out);
}

static boolean
dctenc_empty_output_buffer(j_compress_ptr cinfo)
{
    Xpost_DctEncFile *ff = (Xpost_DctEncFile *)cinfo->client_data;
    size_t i;

    for (i = 0; i < sizeof(ff->out); i++)
        if (xpost_file_putc(ff->base.target, ff->out[i]) == EOF)
            dctenc_target_refused((j_common_ptr)cinfo);
    ff->jdst.next_output_byte = ff->out;
    ff->jdst.free_in_buffer = sizeof(ff->out);
    return TRUE;
}

static void
dctenc_term_destination(j_compress_ptr cinfo)
{
    Xpost_DctEncFile *ff = (Xpost_DctEncFile *)cinfo->client_data;
    size_t i, n = sizeof(ff->out) - ff->jdst.free_in_buffer;

    for (i = 0; i < n; i++)
        if (xpost_file_putc(ff->base.target, ff->out[i]) == EOF)
            dctenc_target_refused((j_common_ptr)cinfo);
}

static int
dctenc_encode(Xpost_EncBase *base, int c)
{
    Xpost_DctEncFile *ff = (Xpost_DctEncFile *)base;

    if (ff->failed)
        return EOF;
    if (setjmp(ff->jmp))
    {
        ff->failed = 1;
        return EOF;
    }
    if (!ff->started)
    {
        jpeg_start_compress(&ff->cinfo, TRUE);
        ff->started = 1;
    }
    ff->row[ff->rowi++] = (unsigned char)c;
    if (ff->rowi == ff->rowbytes)
    {
        JSAMPROW rp = ff->row;

        if (ff->wrote < ff->rows)
        {
            jpeg_write_scanlines(&ff->cinfo, &rp, 1);
            ff->wrote++;
        }
        ff->rowi = 0;
    }
    return c & 0xff;
}

static int
dctenc_finish(Xpost_EncBase *base)
{
    Xpost_DctEncFile *ff = (Xpost_DctEncFile *)base;

    if (ff->failed)
        return EOF;
    if (setjmp(ff->jmp))
        return EOF;
    if (!ff->started)
    {
        jpeg_start_compress(&ff->cinfo, TRUE);
        ff->started = 1;
    }
    /* input short of the declared height is completed with zero rows, so
       the stream always ends well-formed */
    if (ff->rowi || ff->wrote < ff->rows)
    {
        JSAMPROW rp = ff->row;

        memset(ff->row + ff->rowi, 0, ff->rowbytes - ff->rowi);
        while (ff->wrote < ff->rows)
        {
            jpeg_write_scanlines(&ff->cinfo, &rp, 1);
            ff->wrote++;
            memset(ff->row, 0, ff->rowbytes);
        }
    }
    jpeg_finish_compress(&ff->cinfo);
    return 0;
}

static void
dctenc_release(Xpost_EncBase *base)
{
    Xpost_DctEncFile *ff = (Xpost_DctEncFile *)base;

    jpeg_destroy_compress(&ff->cinfo);
    free(ff->row);
    ff->row = NULL;
}

static const Xpost_Enc_Coding dctenc_coding =
{
    dctenc_encode, dctenc_finish, dctenc_release
};
#endif

/* a most-significant-bit-first code writer shared by the LZW and
   CCITT encoders */
typedef struct
{
    Xpost_EncBase base;
    unsigned int bitbuf;
    int bitcnt;
} Xpost_BitEncBase;

/* the whole bytes the buffer holds, high byte first; a target that
   refuses one leaves the rest with the buffer */
static int
bitenc_bytes(Xpost_BitEncBase *ff)
{
    while (ff->bitcnt >= 8)
    {
        ff->bitcnt -= 8;
        if (xpost_file_putc(ff->base.target,
                            (int)(ff->bitbuf >> ff->bitcnt) & 0xff) == EOF)
            return EOF;
    }
    return 0;
}

static int
bitenc_put(Xpost_BitEncBase *ff, unsigned int code, int len)
{
    ff->bitbuf = ff->bitbuf << len | code;
    ff->bitcnt += len;
    return bitenc_bytes(ff);
}

/* out to a byte boundary: the whole bytes still buffered, then the bits
   short of one, at the top of a final byte and zero-filled below. The
   buffer is emptied before the last byte is offered, so what stays
   behind is short of a byte whatever the target does with it. */
static int
bitenc_pad(Xpost_BitEncBase *ff)
{
    int cnt;

    if (bitenc_bytes(ff) == EOF)
        return EOF;
    cnt = ff->bitcnt;
    ff->bitcnt = 0;
    if (cnt &&
        xpost_file_putc(ff->base.target,
                        (int)(ff->bitbuf << (8 - cnt)) & 0xff) == EOF)
        return EOF;
    return 0;
}

/* LZWEncode: the mirror of the decoder.  Strings grow through a
   child list per table entry; the code width follows the size the
   decoder's table will have reached when it reads each code, one
   entry behind this one */
typedef struct
{
    Xpost_BitEncBase base;
    int codewidth;
    int nextcode;
    int early;
    int prefix;
    short child[4096];      /* first extension of each string */
    short sibling[4096];    /* next extension sharing a prefix */
    unsigned char suffix[4096];
} Xpost_LzwEncFile;

static void
lzwenc_reset(Xpost_LzwEncFile *ff)
{
    int i;

    for (i = 0; i < 4096; i++)
        ff->child[i] = ff->sibling[i] = -1;
    ff->codewidth = 9;
    ff->nextcode = 258;
    ff->prefix = -1;
}

static int
lzwenc_emit(Xpost_LzwEncFile *ff, int code)
{
    return bitenc_put(&ff->base, (unsigned int)code,
                      ff->codewidth);
}

static int
lzwenc_encode(Xpost_EncBase *base, int c)
{
    Xpost_LzwEncFile *ff = (Xpost_LzwEncFile *)base;
    int i;

    if (ff->prefix < 0)
    {
        ff->prefix = c;
        return c;
    }
    for (i = ff->child[ff->prefix]; i >= 0; i = ff->sibling[i])
        if (ff->suffix[i] == c)
        {
            ff->prefix = i;
            return c;
        }
    if (lzwenc_emit(ff, ff->prefix) == EOF)
        return EOF;
    /* Add the new entry only while the table has room, as the decoder does
       at the matching site. The reset below keeps the counter below this for
       any valid EarlyChange, so this is the hard floor the fixed table
       cannot be written past whatever EarlyChange was handed in. */
    if (ff->nextcode < 4096)
    {
        ff->suffix[ff->nextcode] = (unsigned char)c;
        ff->sibling[ff->nextcode] = ff->child[ff->prefix];
        ff->child[ff->prefix] = (short)ff->nextcode;
        ff->nextcode++;
    }
    /* the decoder adds this entry only after the next code arrives,
       so its width grows one code later than a naive mirror would */
    if (ff->nextcode + ff->early > (1 << ff->codewidth)
     && ff->codewidth < 12)
        ff->codewidth++;
    if (ff->nextcode + ff->early > 4095)
    {
        if (lzwenc_emit(ff, c) == EOF)
            return EOF;
        if (lzwenc_emit(ff, 256) == EOF)
            return EOF;
        lzwenc_reset(ff);
        return c;
    }
    ff->prefix = c;
    return c;
}

static int
lzwenc_finish(Xpost_EncBase *base)
{
    Xpost_LzwEncFile *ff = (Xpost_LzwEncFile *)base;
    int ret = 0;

    if (ff->prefix >= 0 && lzwenc_emit(ff, ff->prefix) == EOF)
        ret = EOF;
    if (lzwenc_emit(ff, 257) == EOF)
        ret = EOF;
    if (bitenc_pad(&ff->base) == EOF)
        ret = EOF;
    return ret;
}

static const Xpost_Enc_Coding lzwenc_coding =
{
    lzwenc_encode, lzwenc_finish, enc_release_none
};

/* CCITTFaxEncode: rows buffer until complete, become changing-element
   lists, and leave through the coding schemes the decoder reads.
   Positive K codes at least every Kth row one-dimensionally; without
   end-of-line markers the mixed rows carry no tag bits, a form no
   decoder reads back */
typedef struct
{
    Xpost_BitEncBase base;
    int k;
    int columns;
    int rows;
    int blackis1;
    int byteal;
    int eol;
    int eob;
    int *ref, *cur;
    int refcnt;
    int curcnt;
    unsigned char *row;
    int rowbytes;
    int rowpos;
    int rowsdone;
} Xpost_FaxEncFile;

static int
faxenc_code(Xpost_FaxEncFile *ff, const Xpost_Fax_Code *tab, int n, int run)
{
    int i;

    for (i = 0; i < n; i++)
        if (tab[i].run == run)
            return bitenc_put(&ff->base, tab[i].code, tab[i].len);
    return EOF;
}

static int
faxenc_run(Xpost_FaxEncFile *ff, int run, int color)
{
    const Xpost_Fax_Code *tab = color ? fax_black : fax_white;
    int n = (int)(sizeof(fax_white)/sizeof(*fax_white));
    int next = (int)(sizeof(fax_ext)/sizeof(*fax_ext));

    while (run >= 2624)
    {
        if (faxenc_code(ff, fax_ext, next, 2560) == EOF)
            return EOF;
        run -= 2560;
    }
    if (run >= 64)
    {
        int makeup = run & ~63;

        if (makeup > 1728
            ? faxenc_code(ff, fax_ext, next, makeup) == EOF
            : faxenc_code(ff, tab, n, makeup) == EOF)
            return EOF;
        run -= makeup;
    }
    return faxenc_code(ff, tab, n, run);
}

static int
faxenc_eol(Xpost_FaxEncFile *ff)
{
    return bitenc_put(&ff->base, 1, 12);
}

/* changing-element positions of the buffered row */
static void
faxenc_elements(Xpost_FaxEncFile *ff)
{
    int x, color = 0, ci = 0;

    for (x = 0; x < ff->columns; x++)
    {
        int bit = (ff->row[x >> 3] >> (7 - (x & 7))) & 1;
        int black = ff->blackis1 ? bit : !bit;

        if (black != color)
        {
            ff->cur[ci++] = x;
            color = black;
        }
    }
    ff->cur[ci] = ff->cur[ci + 1] = ff->columns;
    ff->curcnt = ci;
}

static int
faxenc_1d(Xpost_FaxEncFile *ff)
{
    int pos = 0, color = 0, i = 0;

    while (pos < ff->columns)
    {
        int next = i < ff->curcnt ? ff->cur[i] : ff->columns;

        if (faxenc_run(ff, next - pos, color) == EOF)
            return EOF;
        pos = next;
        color ^= 1;
        i++;
    }
    return 0;
}

static int
faxenc_2d(Xpost_FaxEncFile *ff)
{
    int a0 = -1, color = 0, ai = 0, ri = 0, guard = 0;

    while (a0 < ff->columns)
    {
        int b1, b2, a1, a2;

        if (++guard > 2 * ff->columns + 64)
            return EOF;

        while (ri > 0 && (ri >= ff->refcnt || ff->ref[ri - 1] > a0))
            ri--;
        while (ri < ff->refcnt && ff->ref[ri] <= a0)
            ri++;
        if ((ri ^ color) & 1)
            ri++;
        b1 = ri < ff->refcnt ? ff->ref[ri] : ff->columns;
        b2 = ri + 1 < ff->refcnt ? ff->ref[ri + 1] : ff->columns;
        while (ai < ff->curcnt && ff->cur[ai] <= a0)
            ai++;
        a1 = ai < ff->curcnt ? ff->cur[ai] : ff->columns;
        a2 = ai + 1 < ff->curcnt ? ff->cur[ai + 1] : ff->columns;

        if (b2 < a1)
        {
            /* pass: the reference run ends before the next change */
            if (bitenc_put(&ff->base, 1, 4) == EOF)
                return EOF;
            a0 = b2;
        }
        else if (a1 - b1 >= -3 && a1 - b1 <= 3)
        {
            /* vertical: 1, 011/010, 000011/000010, 0000011/0000010 */
            static const struct { unsigned int code; int len; }
            vcode[7] = { {2,7}, {2,6}, {2,3}, {1,1}, {3,3}, {3,6}, {3,7} };
            int d = a1 - b1;

            if (bitenc_put(&ff->base,
                           vcode[d + 3].code, vcode[d + 3].len) == EOF)
                return EOF;
            a0 = a1;
            color ^= 1;
        }
        else
        {
            /* horizontal: 001 and the two runs from a0 */
            int start = a0 < 0 ? 0 : a0;

            if (bitenc_put(&ff->base, 1, 3) == EOF)
                return EOF;
            if (faxenc_run(ff, a1 - start, color) == EOF)
                return EOF;
            if (faxenc_run(ff, a2 - a1, !color) == EOF)
                return EOF;
            a0 = a2;
        }
    }
    return 0;
}

static int
faxenc_row(Xpost_FaxEncFile *ff)
{
    int twod, ret, *tmp;

    if (ff->byteal && bitenc_pad(&ff->base) == EOF)
        return EOF;
    if (ff->k < 0)
        twod = 1;
    else if (ff->k == 0)
        twod = 0;
    else
        twod = (ff->rowsdone % ff->k) != 0;
    if (ff->eol && ff->k >= 0)
    {
        if (faxenc_eol(ff) == EOF)
            return EOF;
        if (ff->k > 0 &&
            bitenc_put(&ff->base, !twod, 1) == EOF)
            return EOF;
    }
    faxenc_elements(ff);
    ret = twod ? faxenc_2d(ff) : faxenc_1d(ff);
    if (ret == EOF)
        return EOF;
    tmp = ff->ref; ff->ref = ff->cur; ff->cur = tmp;
    ff->refcnt = ff->curcnt;
    ff->rowsdone++;
    return 0;
}

static int
faxenc_encode(Xpost_EncBase *base, int c)
{
    Xpost_FaxEncFile *ff = (Xpost_FaxEncFile *)base;
    int ret = c;

    ff->row[ff->rowpos++] = (unsigned char)c;
    /* the row buffer holds one row: the byte that completes it starts the
       next, whatever became of the one just filled */
    if (ff->rowpos == ff->rowbytes)
    {
        if (faxenc_row(ff) == EOF)
            ret = EOF;
        ff->rowpos = 0;
    }
    return ret;
}

static int
faxenc_finish(Xpost_EncBase *base)
{
    Xpost_FaxEncFile *ff = (Xpost_FaxEncFile *)base;
    int ret = 0;

    if (ff->eob)
    {
        int i, n = ff->k < 0 ? 2 : 6;

        /* the block marker starts a fresh byte when rows do */
        if (ff->byteal && bitenc_pad(&ff->base) == EOF)
            ret = EOF;
        for (i = 0; i < n; i++)
        {
            if (faxenc_eol(ff) == EOF)
                ret = EOF;
            if (ff->k > 0 &&
                bitenc_put(&ff->base, 1, 1) == EOF)
                ret = EOF;
        }
    }
    if (bitenc_pad(&ff->base) == EOF)
        ret = EOF;
    return ret;
}

static void
faxenc_release(Xpost_EncBase *base)
{
    Xpost_FaxEncFile *ff = (Xpost_FaxEncFile *)base;

    free(ff->ref);
    free(ff->cur);
    free(ff->row);
    ff->ref = ff->cur = NULL;
    ff->row = NULL;
}

static const Xpost_Enc_Coding faxenc_coding =
{
    faxenc_encode, faxenc_finish, faxenc_release
};

/* An encode filter: the target it writes and the latch that says its
   end-of-data has been written, whatever the coding above them. */
static Xpost_Object
_enc_cons(Xpost_Memory_File *mem, Xpost_Object tgt, size_t size,
          const Xpost_Enc_Coding *coding, Xpost_EncBase **out)
{
    Xpost_File *target = xpost_file_get_file_pointer(mem, tgt);
    Xpost_EncBase *ff;
    Xpost_Object f = { 0 };

    *out = NULL;
    if (!target)
        return invalid;
    ff = calloc(1, size);
    if (!ff)
        return invalid;
    ff->target = target;
    ff->coding = coding;
    f = _filter_object_cons(mem, &ff->methods, &enc_methods,
                            XPOST_FILE_WRAPS_TARGET, target);
    /* the base is handed back only once it is registered: a refusal has
       already given the struct up, and a coding that went on writing
       into it would be writing into freed memory */
    if (xpost_object_get_type(f) == filetype)
        *out = ff;
    return f;
}

/* Give up a filter whose coding could not be built.

   The object exists by the time a coding fails: the entity holds the
   struct and the filter's claim is on the stream beneath, and the object
   naming all three is about to be discarded. Closing is what gives them
   back -- the claim, the struct, and the entity's pointer to it -- and
   leaves an entity holding no stream, which is a thing the collector
   knows how to reclaim. The coding is marked finished first, so that a
   filter which never encoded a byte writes no end-of-data marker to the
   target on its way out. */
static Xpost_Object
_enc_cons_abandon(Xpost_Memory_File *mem, Xpost_Object f, Xpost_EncBase *base)
{
    base->closed = 1;
    (void)xpost_file_object_close(mem, f);
    return invalid;
}

Xpost_Object xpost_file_cons_filter_enc_null(Xpost_Memory_File *mem, Xpost_Object tgt)
{
    Xpost_EncBase *ff;

    return _enc_cons(mem, tgt, sizeof *ff, &nullenc_coding, &ff);
}

Xpost_Object xpost_file_cons_filter_enc_hex(Xpost_Memory_File *mem, Xpost_Object tgt)
{
    Xpost_EncBase *ff;

    return _enc_cons(mem, tgt, sizeof(Xpost_HexEncFile), &hexenc_coding, &ff);
}

Xpost_Object xpost_file_cons_filter_enc_a85(Xpost_Memory_File *mem, Xpost_Object tgt)
{
    Xpost_EncBase *ff;

    return _enc_cons(mem, tgt, sizeof(Xpost_A85EncFile), &a85enc_coding, &ff);
}

Xpost_Object xpost_file_cons_filter_enc_rle(Xpost_Memory_File *mem, Xpost_Object tgt, int recsize)
{
    Xpost_EncBase *base;
    Xpost_Object f = _enc_cons(mem, tgt, sizeof(Xpost_RleEncFile),
                               &rleenc_coding, &base);
    Xpost_RleEncFile *ff = (Xpost_RleEncFile *)base;

    if (ff)
        ff->recsize = recsize;
    return f;
}

#ifdef HAVE_ZLIB
Xpost_Object xpost_file_cons_filter_enc_flate(Xpost_Memory_File *mem, Xpost_Object tgt)
{
    Xpost_EncBase *base;
    Xpost_Object f = _enc_cons(mem, tgt, sizeof(Xpost_FlateEncFile),
                               &flateenc_coding, &base);
    Xpost_FlateEncFile *ff = (Xpost_FlateEncFile *)base;

    if (!ff)
        return f;
    if (deflateInit(&ff->strm, Z_DEFAULT_COMPRESSION) != Z_OK)
        return _enc_cons_abandon(mem, f, &ff->base);
    ff->strm.next_out = ff->out;
    ff->strm.avail_out = sizeof(ff->out);
    return f;
}
#endif

Xpost_Object xpost_file_cons_filter_enc_lzw(Xpost_Memory_File *mem, Xpost_Object tgt, int early)
{
    Xpost_EncBase *base;
    Xpost_Object f = _enc_cons(mem, tgt, sizeof(Xpost_LzwEncFile),
                               &lzwenc_coding, &base);
    Xpost_LzwEncFile *ff = (Xpost_LzwEncFile *)base;

    if (!ff)
        return f;
    ff->early = early;
    lzwenc_reset(ff);
    bitenc_put(&ff->base, 256, 9);   /* opening clear */
    return f;
}

Xpost_Object xpost_file_cons_filter_enc_ccitt(Xpost_Memory_File *mem,
                                              Xpost_Object tgt,
                                              int k, int columns, int rows,
                                              int blackis1, int byteal,
                                              int eol, int eob)
{
    Xpost_EncBase *base;
    Xpost_Object f = { 0 };
    Xpost_FaxEncFile *ff;

    if (columns < 1 || columns > (1 << 20))
        return invalid;
    f = _enc_cons(mem, tgt, sizeof(Xpost_FaxEncFile), &faxenc_coding, &base);
    ff = (Xpost_FaxEncFile *)base;
    if (!ff)
        return f;
    ff->k = k;
    ff->columns = columns;
    ff->rows = rows;
    ff->blackis1 = blackis1;
    ff->byteal = byteal;
    ff->eol = eol;
    ff->eob = eob;
    ff->rowbytes = (columns + 7) / 8;
    ff->ref = calloc((size_t)columns + 4, sizeof(int));
    ff->cur = calloc((size_t)columns + 4, sizeof(int));
    ff->row = malloc((size_t)ff->rowbytes);
    if (!ff->ref || !ff->cur || !ff->row)
    {
        /* whichever of the three arrived is the coding's to give up */
        faxenc_release(&ff->base.base);
        return _enc_cons_abandon(mem, f, &ff->base.base);
    }
    return f;
}

#ifdef HAVE_LIBJPEG
Xpost_Object xpost_file_cons_filter_enc_dct(Xpost_Memory_File *mem,
                                            Xpost_Object tgt,
                                            int columns,
                                            int rows,
                                            int colors,
                                            double qfactor,
                                            int colortransform,
                                            const int *hsamp,
                                            const int *vsamp)
{
    Xpost_EncBase *base;
    Xpost_Object f = { 0 };
    Xpost_DctEncFile *ff;
    int i;

    if (columns < 1 || rows < 1 || colors < 1 || colors > 4
        || qfactor <= 0.0)
        return invalid;
    f = _enc_cons(mem, tgt, sizeof(Xpost_DctEncFile),
                  &dctenc_coding, &base);
    ff = (Xpost_DctEncFile *)base;
    if (!ff)
        return f;
    ff->cinfo.err = jpeg_std_error(&ff->jerr);
    ff->jerr.error_exit = dctenc_error_exit;
    ff->jerr.output_message = dctenc_output_message;
    ff->cinfo.client_data = ff;
    if (setjmp(ff->jmp))
        return _enc_cons_abandon(mem, f, &ff->base);
    jpeg_create_compress(&ff->cinfo);
    ff->jdst.init_destination = dctenc_init_destination;
    ff->jdst.empty_output_buffer = dctenc_empty_output_buffer;
    ff->jdst.term_destination = dctenc_term_destination;
    ff->cinfo.dest = &ff->jdst;
    ff->cinfo.image_width = (JDIMENSION)columns;
    ff->cinfo.image_height = (JDIMENSION)rows;
    ff->cinfo.input_components = colors;
    ff->cinfo.in_color_space = colors == 1 ? JCS_GRAYSCALE
                             : colors == 3 ? JCS_RGB
                             : colors == 4 ? JCS_CMYK
                             : JCS_UNKNOWN;
    jpeg_set_defaults(&ff->cinfo);
    /* three components transform to YCbCr unless the dictionary said
       otherwise; four stay untransformed unless it asked for YCCK.
       The APP14 marker carries the choice to the decoder */
    if (colors == 3 && !colortransform)
        jpeg_set_colorspace(&ff->cinfo, JCS_RGB);
    if (colors == 4 && colortransform)
        jpeg_set_colorspace(&ff->cinfo, JCS_YCCK);
    jpeg_set_linear_quality(&ff->cinfo,
                            (int)(qfactor * 100.0 + 0.5), TRUE);
    for (i = 0; i < colors; i++)
    {
        ff->cinfo.comp_info[i].h_samp_factor = hsamp[i];
        ff->cinfo.comp_info[i].v_samp_factor = vsamp[i];
    }
    ff->rowbytes = (unsigned int)columns * (unsigned int)colors;
    ff->rows = (unsigned int)rows;
    ff->row = malloc((size_t)ff->rowbytes);
    if (!ff->row)
    {
        /* the compressor is built by now and is the coding's to give up */
        jpeg_destroy_compress(&ff->cinfo);
        return _enc_cons_abandon(mem, f, &ff->base);
    }
    return f;
}
#endif

/* eexec decryption: the outer encryption layer of Type 1 font
   programs, R initialized to 55665, each plain byte the cipher byte
   xored with the high half of R before R absorbs the cipher byte.
   The first four plain bytes are salt and are discarded; whether the
   ciphertext is raw bytes or hexadecimal is decided the way the
   format specifies, by whether the first four bytes all read as
   hexadecimal characters. */
typedef struct Xpost_EexecFile
{
    Xpost_FilterBase base;
    unsigned short r;
    int mode;           /* -1 undecided, 0 binary, 1 hex */
    int skip;
    unsigned char head[4];
    int headn, headi;
} Xpost_EexecFile;

static int
eexec_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int
eexec_srcbyte(Xpost_EexecFile *ff)
{
    if (ff->headi < ff->headn)
        return ff->head[ff->headi++];
    return xpost_file_getc(ff->base.source);
}

static int
eexec_cipherbyte(Xpost_EexecFile *ff)
{
    int c, hi, lo;

    if (ff->mode < 0)
    {
        int i, allhex = 1;

        /* white space rides between the eexec token and the
           ciphertext; the four test bytes follow it */
        do
            c = xpost_file_getc(ff->base.source);
        while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (c == EOF)
            return EOF;
        ff->head[0] = (unsigned char)c;
        if (eexec_hexval(c) < 0)
            allhex = 0;
        for (i = 1; i < 4; i++)
        {
            c = xpost_file_getc(ff->base.source);
            if (c == EOF)
                return EOF;
            ff->head[i] = (unsigned char)c;
            if (eexec_hexval(c) < 0 && c != ' ' && c != '\t'
             && c != '\r' && c != '\n')
                allhex = 0;
        }
        ff->headn = 4;
        ff->headi = 0;
        ff->mode = allhex;
    }
    if (ff->mode == 0)
        return eexec_srcbyte(ff);
    do
        c = eexec_srcbyte(ff);
    while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    hi = eexec_hexval(c);
    if (hi < 0)
        return EOF;
    do
        c = eexec_srcbyte(ff);
    while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    lo = eexec_hexval(c);
    if (lo < 0)
        return EOF;
    return hi << 4 | lo;
}

static int
eexec_readch(Xpost_File *f)
{
    Xpost_EexecFile *ff = (Xpost_EexecFile *)f;
    int c, p;

    if (ff->base.pushback >= 0)
    {
        c = ff->base.pushback;
        ff->base.pushback = -1;
        return c;
    }
    if (ff->base.eod)
        return EOF;
    for (;;)
    {
        c = eexec_cipherbyte(ff);
        if (c == EOF)
        {
            ff->base.eod = 1;
            return EOF;
        }
        p = c ^ (ff->r >> 8);
        ff->r = (unsigned short)((unsigned int)(c + ff->r) * 52845u + 22719u);
        if (ff->skip)
        {
            ff->skip--;
            continue;
        }
        return p;
    }
}

struct Xpost_File_Methods eexec_methods =
{
    eexec_readch, filter_writech, filter_close, filter_flush,
    filter_purge, filter_unreadch, filter_tell, filter_seek
};

Xpost_Object xpost_file_cons_filter_eexec(Xpost_Memory_File *mem, Xpost_Object src)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_EexecFile *ff;

    if (!source)
        return invalid;
    ff = calloc(1, sizeof *ff);
    if (!ff)
        return invalid;
    ff->r = 55665;
    ff->mode = -1;
    ff->skip = 4;
    return _dec_cons(mem, &ff->base, &eexec_methods, source);
}

/* ReusableStreamDecode filter: the entire source is drained into a
   buffer at construction -- leaving the underlying file positioned
   just past the encoded data, exactly as reading it would -- and the
   buffer then serves reads any number of times: setfileposition
   repositions it and resetfile rewinds it to the beginning, so one
   inline stream can feed several consumers (the masked-image idiom:
   paint the same data under different masks). */
typedef struct Xpost_RsdFile
{
    /* the source is consumed whole at construction and kept only so that
       closing the stream releases a source the machinery owns, as it does
       for every other filter */
    Xpost_FilterBase base;
    unsigned char *data;
    size_t len, pos;
} Xpost_RsdFile;

static int
rsd_readch(Xpost_File *f)
{
    Xpost_RsdFile *ff = (Xpost_RsdFile *)f;

    if (ff->pos >= ff->len)
        return EOF;
    return ff->data[ff->pos++];
}

static int
rsd_unreadch(Xpost_File *f, int c)
{
    Xpost_RsdFile *ff = (Xpost_RsdFile *)f;

    if (ff->pos == 0)
        return EOF;
    ff->pos--;
    (void)c;
    return 0;
}

static int
rsd_close(Xpost_File *f)
{
    /* a reusable stream survives closing: the position rewinds and
       the data stays, so a program run off the stream -- which the
       interpreter closes at its end -- can run again */
    Xpost_RsdFile *ff = (Xpost_RsdFile *)f;

    ff->pos = 0;
    return 0;
}

static void
rsd_purge(Xpost_File *f)
{
    /* resetfile rewinds a reusable stream for its next consumer */
    Xpost_RsdFile *ff = (Xpost_RsdFile *)f;

    ff->pos = 0;
}

static long long
rsd_tell(Xpost_File *f)
{
    Xpost_RsdFile *ff = (Xpost_RsdFile *)f;

    return (long long)ff->pos;
}

static int
rsd_seek(Xpost_File *f, long long offset)
{
    Xpost_RsdFile *ff = (Xpost_RsdFile *)f;

    if (offset < 0 || (unsigned long long)offset > (unsigned long long)ff->len)
        return -1;
    ff->pos = (size_t)offset;
    return 0;
}

/* flushfile leaves an input file at the end of its data (PLRM 8.2), which
   a reusable stream is already holding whole: there is nothing to read
   through to reach it. */
static int
rsd_flush(Xpost_File *f)
{
    Xpost_RsdFile *ff = (Xpost_RsdFile *)f;

    ff->pos = ff->len;
    return 0;
}

struct Xpost_File_Methods rsd_methods =
{
    rsd_readch, filter_writech, rsd_close, rsd_flush,
    rsd_purge, rsd_unreadch, rsd_tell, rsd_seek
};

/* Wrap a malloc'd filter struct in a filetype object.
 *
 * Every filter of either family is born here, and states in this one call
 * the methods its coding answers with and the stream it is a filter over.
 * Recording the claim it lays on that stream is therefore not a thing a
 * filter can be written without: there is no other way to make one, and
 * the two entry points below ask for the stream by name.
 *
 * The struct belongs to this call: on any refusal it is given up here and
 * the caller is handed the invalid object, since no object naming it
 * comes back and nothing else knows it is there. That includes a refusal
 * after the entity exists, which leaves an entity tagged as a file whose
 * payload was never written -- it is retired from the birth census so
 * that no restore reads a stream out of it.
 */
static void
_filter_cons_abandon(Xpost_File *ff)
{
    /* torn down as far as a close tears a filter down: the coding gives
       up whatever it is already holding, a reusable stream gives up the
       copy of its source, and the struct goes. What a close does beyond
       that -- releasing the stream beneath, clearing the entity's
       pointer -- has nothing to undo here, since the claim was never
       laid and the entity holds nothing. An encode coding is marked
       finished first: a filter that never wrote a byte has no
       end-of-data marker to leave in somebody's file on its way out. */
    if (ff->methods == &enc_methods)
        ((Xpost_EncBase *)ff)->closed = 1;
    (void)xpost_file_close(ff);
    if (ff->methods == &rsd_methods)
        free(((Xpost_RsdFile *)ff)->data);
    free(ff);
}

static Xpost_Object
_filter_object_cons(Xpost_Memory_File *mem, Xpost_File *ff,
                    Xpost_File_Methods *methods,
                    Xpost_File_Wraps wraps, Xpost_File *under)
{
    Xpost_Object f = { 0 };
    unsigned int ent;

    if (!ff)
        return invalid;
    ff->methods = methods;
    ff->refs = 0;
    ff->closed = 0;
    ff->owned = 0;
    ff->ent = 0;
    ff->wraps = wraps;
    ff->job_stream = 0;
    ff->eot = 0;
    f.tag = filetype;
    if (!xpost_memory_table_alloc(mem, XPOST_HANDLE_ENTITY_SIZE, filetype,
                                  &ent))
    {
        XPOST_LOG_ERR("cannot allocate file record");
        _filter_cons_abandon(ff);
        return invalid;
    }
    if (!_file_bind_entity(mem, ent, ff))
    {
        XPOST_LOG_ERR("cannot hold the stream of a file record");
        _filter_cons_abandon(ff);
        return invalid;
    }
    f.mark_.padw = ent;
    if (under)
        under->refs++;
    return f;
}

/* A decode filter: the source it reads, one byte of pushback and the
   end-of-data latch, whatever the coding above them.

   The struct becomes this call's, and a constructor that has reached
   here has nothing left to release: an object made holds the struct, and
   a refusal tears it down as far as a close does. So the struct arrives
   as the base it begins with -- the same address, the coding's own name
   for it dropped -- and the constructor's last act is to answer with
   what this returns. */
static Xpost_Object
_dec_cons(Xpost_Memory_File *mem, Xpost_FilterBase *ff,
          Xpost_File_Methods *methods, Xpost_File *source)
{
    if (!ff)
        return invalid;
    ff->source = source;
    ff->pushback = -1;
    ff->eod = 0;
    return _filter_object_cons(mem, &ff->methods, methods,
                               XPOST_FILE_WRAPS_SOURCE, source);
}

Xpost_Object xpost_file_cons_filter_hex(Xpost_Memory_File *mem, Xpost_Object src)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_HexFile *ff;

    if (!source)
        return invalid;
    ff = malloc(sizeof *ff);
    if (!ff)
        return invalid;
    return _dec_cons(mem, &ff->base, &hex_methods, source);
}

Xpost_Object xpost_file_cons_filter_rle(Xpost_Memory_File *mem, Xpost_Object src)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_RleFile *ff;

    if (!source)
        return invalid;
    ff = malloc(sizeof *ff);
    if (!ff)
        return invalid;
    ff->litrun = 0;
    ff->reprun = 0;
    ff->repbyte = 0;
    return _dec_cons(mem, &ff->base, &rle_methods, source);
}

Xpost_Object xpost_file_cons_filter_subfile(Xpost_Memory_File *mem, Xpost_Object src,
                                            int count, const char *eod, int eodlen)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_SubFile *ff;

    if (!source || eodlen < 0 || eodlen > 64)
        return invalid;
    ff = malloc(sizeof *ff);
    if (!ff)
        return invalid;
    ff->count = count;
    /* an empty end-of-data string arrives as no pointer at all,
       and copying from one is undefined at any length */
    if (eodlen > 0)
        memcpy(ff->eodstr, eod, eodlen);
    ff->eodlen = eodlen;
    ff->pendn = ff->pendi = 0;
    return _dec_cons(mem, &ff->base, &subfile_methods, source);
}

#ifdef HAVE_ZLIB
Xpost_Object xpost_file_cons_filter_flate(Xpost_Memory_File *mem, Xpost_Object src)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_FlateFile *ff;

    if (!source)
        return invalid;
    ff = malloc(sizeof *ff);
    if (!ff)
        return invalid;
    memset(&ff->strm, 0, sizeof ff->strm);
    if (inflateInit(&ff->strm) != Z_OK)
    {
        free(ff);
        return invalid;
    }
    ff->outn = ff->outi = 0;
    ff->haslook = 0;
    return _dec_cons(mem, &ff->base, &flate_methods, source);
}
#endif

Xpost_Object xpost_file_cons_filter_rsd(Xpost_Memory_File *mem, Xpost_Object src)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_RsdFile *ff;
    Xpost_String_Buffer data;
    int c;

    if (!source)
        return invalid;
    if (xpost_strbuf_init(&data, 4096))
        return invalid;
    while ((c = xpost_file_getc(source)) != EOF)
    {
        char b = (char)c;

        if (xpost_strbuf_append(&data, &b, 1))
        {
            xpost_strbuf_free(&data);
            return invalid;
        }
    }
    ff = malloc(sizeof *ff);
    if (!ff)
    {
        xpost_strbuf_free(&data);
        return invalid;
    }
    ff->data = (unsigned char *)data.s;
    ff->len = data.len;
    ff->pos = 0;
    return _dec_cons(mem, &ff->base, &rsd_methods, source);
}

#ifdef HAVE_LIBJPEG
Xpost_Object xpost_file_cons_filter_dct(Xpost_Memory_File *mem, Xpost_Object src)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_DctFile *ff;

    if (!source)
        return invalid;
    ff = calloc(1, sizeof *ff);
    if (!ff)
        return invalid;
    ff->cinfo.err = jpeg_std_error(&ff->jerr);
    ff->jerr.error_exit = dct_error_exit;
    ff->jerr.output_message = dct_output_message;
    ff->cinfo.client_data = ff;
    if (setjmp(ff->jmp))
    {
        free(ff);
        return invalid;
    }
    jpeg_create_decompress(&ff->cinfo);
    ff->jsrc.init_source = dct_init_source;
    ff->jsrc.fill_input_buffer = dct_fill_input_buffer;
    ff->jsrc.skip_input_data = dct_skip_input_data;
    ff->jsrc.resync_to_restart = jpeg_resync_to_restart;
    ff->jsrc.term_source = dct_term_source;
    ff->jsrc.next_input_byte = NULL;
    ff->jsrc.bytes_in_buffer = 0;
    ff->cinfo.src = &ff->jsrc;
    return _dec_cons(mem, &ff->base, &dct_methods, source);
}
#endif

/* construct an ASCII85Decode filter file over a source file object */
Xpost_Object xpost_file_cons_filter_a85(Xpost_Memory_File *mem,
                                        Xpost_Object src)
{
    Xpost_File *source = xpost_file_get_file_pointer(mem, src);
    Xpost_FilterFile *ff;

    if (!source)
        return invalid;
    ff = malloc(sizeof *ff);
    if (!ff)
        return invalid;
    ff->outn = ff->outi = 0;
    return _dec_cons(mem, &ff->base, &a85_methods, source);
}

/* pinch-off a tmpfile containing one line from file.

   The temporary file is opened before the text is known to be acceptable,
   so a refusal has one open and is the only thing that can close it: the
   caller is handed a stream or an error code, never both. */
/*@null@*/
static
int lineedit(FILE *in, FILE **out)
{
    FILE *fp;
    int c;
    int ret;

    c = fgetc(in);
    if (c == EOF)
    {
        return undefinedfilename;
    }
#ifdef DEBUG_FILE
    printf("tmpfile (fdopen)\n");
#endif
    fp = f_tmpfile();
    if (fp == NULL)
    {
        XPOST_LOG_ERR("tmpfile() returned NULL");
        return ioerror;
    }
    while (c != EOF && c != '\n')
    {
        if (fputc(c, fp) == EOF)
        {
            ret = ioerror;
            goto give_up;
        }
        c = fgetc(in);
    }
    fseek(fp, 0, SEEK_SET);
    *out = fp;

    return 0;

give_up:
    fclose(fp);
    return ret;
}

enum { MAXNEST = 20 };

/* pinch-off a tmpfile containing one "statement" from file.

   The temporary file is opened before the text is known to be acceptable,
   so a refusal has one open and is the only thing that can close it: the
   caller is handed a stream or an error code, never both. */
/*@null@*/
static
int statementedit(FILE *in, FILE **out)
{
    FILE *fp;
    int c;
    int ret;
    char nest[MAXNEST + 1] = {0}; /* any of {(< waiting for matching >)};
                                     one past MAXNEST holds the level that
                                     tips over the limit until it is rejected */
    int defer = -1; /* defer is a flag (-1 == false)
                       and an index into nest[] */

    c = fgetc(in);
    if (c == EOF)
    {
        return undefinedfilename;
    }
#ifdef DEBUG_FILE
    printf("tmpfile (fdopen)\n");
#endif
    fp = f_tmpfile();
    if (fp == NULL)
    {
        XPOST_LOG_ERR("tmpfile() returned NULL");
        return ioerror;
    }
    do
    {
        if (defer > -1)
        {
            if (defer >= MAXNEST)
            {
                ret = syntaxerror;
                goto give_up;
            }
            switch(nest[defer])
            { /* what's the innermost nest? */
                case '{': /* within a proc, can end proc or begin proc, string, hex */
                    switch (c)
                    {
                        case '}': --defer; break;
                        case '{':
                        case '(':
                        case '<': nest[++defer] = c; break;
                    }
                    break;
                case '(': /* within a string, can begin or end nested paren */
                    switch (c)
                    {
                        case ')': --defer; break;
                        case '(': nest[++defer] = c; break;
                        case '\\': if (fputc(c, fp) == EOF)
                            {
                                ret = ioerror;
                                goto give_up;
                            }
                            c = fgetc(in);
                            if (c == EOF) goto done;
                            goto next;
                    }
                    break;
                case '<': /* hexstrings don't nest, can only end it */
                    if (c == '>') --defer;
                    break;
            }
        }
        else
            switch (c)
            { /* undefined, can begin any structure */
                case '{':
                case '(':
                case '<': nest[++defer] = c; break;
                case '\\': if (fputc(c, fp) == EOF)
                    {
                        ret = ioerror;
                        goto give_up;
                    }
                    c = fgetc(in); break;
            }
        if (c == '\n')
        {
            if (defer == -1) goto done;
            { /* sub-prompt */
                int i;
                for (i = 0; i <= defer; i++)
                    putchar(nest[i]);
                fputs(".:", stdout);
                fflush(NULL);
            }
        }
next:
        if (fputc(c, fp) == EOF)
        {
            ret = ioerror;
            goto give_up;
        }
        c = fgetc(in);
    } while(c != EOF);
done:
    fseek(fp, 0, SEEK_SET);
    *out = fp;
    return 0;

give_up:
    fclose(fp);
    return ret;
}

/* Give a stream this process opened to a file object carrying the
   access the mode granted. The object holds the stream from here on;
   a stream no object could be made for is closed rather than left
   open, since nothing else names it. */
static
int _file_adopt_stream(Xpost_Memory_File *mem,
                       FILE *fp,
                       unsigned int access,
                       Xpost_Object *retval)
{
    Xpost_Object f = { 0 };

    f = xpost_file_cons(mem, fp,
                        access == XPOST_OBJECT_TAG_ACCESS_FILE_READ);
    if (xpost_object_get_type(f) != filetype)
    {
        fclose(fp);
        return VMerror;
    }
    f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
    f.tag |= access << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET;
    *retval = f;
    return 0;
}

/* Open a file object,
   check for "special" filenames,
   fallback to fopen.

   A stream this function opened has nothing else naming it until the
   object exists, so it goes through the adopter above, which closes it
   when no object can be made for it. The three standard streams belong
   to the process and are never this function's to close, so they go to
   the constructor directly.

   The leak the analyser reports here is the one path that cannot run.
   It walks into the constructor, takes the branch that reports failure,
   and then does not carry that failure into the object the constructor
   returns -- a struct return it declines to model -- so it comes back
   out and takes the passing side of the check as well. On that pairing
   nothing closes the stream. Either half alone is handled: a failing
   constructor sends the stream to fclose above, and a succeeding one
   has already stored it in the file record. The class is held at zero
   across the tree, so it is turned off for this function alone, whose
   only allocations are the streams it hands to the adopter. Only gcc
   is shown the directive: the class is gcc's, and clang parses the
   group name, does not know it, and warns about the pragma itself. */
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif
int xpost_file_open(Xpost_Memory_File *mem,
                    char *fn,
                    char *mode,
                    Xpost_Object *retval)
{
    Xpost_Object f = { 0 };
    FILE *fp;
    int ret;

    f.tag = filetype;

    if (strcmp(fn, "%stdin") == 0)
    {
        if (strcmp(mode, "r") != 0)
        {
            return invalidfileaccess;
        }
        f = xpost_file_cons(mem, stdin, 1);
        if (xpost_object_get_type(f) != filetype)
            return VMerror;
        f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        f.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_READ << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    }
    else if (strcmp(fn, "%stdout") == 0)
    {
        if (strcmp(mode, "w") != 0)
        {
            return invalidfileaccess;
        }
        f = xpost_file_cons(mem, stdout, 0);
        if (xpost_object_get_type(f) != filetype)
            return VMerror;
        f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        f.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_WRITE << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    }
    else if (strcmp(fn, "%stderr") == 0)
    {
        if (strcmp(mode, "w") != 0)
        {
            return invalidfileaccess;
        }
        f = xpost_file_cons(mem, stderr, 0);
        if (xpost_object_get_type(f) != filetype)
            return VMerror;
        f.tag &= ~XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK;
        f.tag |= (XPOST_OBJECT_TAG_ACCESS_FILE_WRITE << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    }
    else if (strcmp(fn, "%lineedit") == 0)
    {
        ret = lineedit(stdin, &fp);
        if (ret)
        {
            return ret;
        }
        ret = _file_adopt_stream(mem, fp,
                                 XPOST_OBJECT_TAG_ACCESS_FILE_READ, &f);
        if (ret)
            return ret;
    }
    else if (strcmp(fn, "%statementedit") == 0)
    {
        ret = statementedit(stdin, &fp);
        if (ret)
        {
            return ret;
        }
        ret = _file_adopt_stream(mem, fp,
                                 XPOST_OBJECT_TAG_ACCESS_FILE_READ, &f);
        if (ret)
            return ret;
    }
    else
    {
        unsigned int access;

        /* An access string is r, w or a, optionally followed by + (PLRM
           3.8.1, Table 3.5): r reads an existing file, w creates or
           truncates, a creates or appends, and + adds the other direction.
           The access attribute the file object carries follows from the
           string, and both are settled before the file is opened, so an
           access string outside the table neither creates nor truncates
           anything and is reported as such. */
        if ((mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')
            || (mode[1] && (mode[1] != '+' || mode[2])))
            return invalidfileaccess;
        if (mode[1] == '+')
            access = XPOST_OBJECT_TAG_ACCESS_FILE_READ
                   | XPOST_OBJECT_TAG_ACCESS_FILE_WRITE;
        else if (mode[0] == 'r')
            access = XPOST_OBJECT_TAG_ACCESS_FILE_READ;
        else
            access = XPOST_OBJECT_TAG_ACCESS_FILE_WRITE;

#ifdef DEBUG_FILE
        printf("fopen\n");
#endif
        {
            /* A file is a sequence of bytes (PLRM 3.8), so it is opened
               as one. Where the C library distinguishes a text stream it
               rewrites line endings on the way through and reads a
               particular byte as the end of the data, which would make
               the bytes a program wrote differ from the bytes it reads
               back. The letter has no effect where there is no
               distinction. */
            char fmode[4];
            size_t mi = 0;

            while (mode[mi] && mi < sizeof fmode - 2)
            {
                fmode[mi] = mode[mi];
                mi++;
            }
            fmode[mi++] = 'b';
            fmode[mi] = '\0';
            fp = xpost_diskfile_fopen(fn, fmode, 0, &ret);
        }
        if (fp == NULL)
            return ret;
        ret = _file_adopt_stream(mem, fp, access, &f);
        if (ret)
            return ret;
    }

    f.tag |= XPOST_OBJECT_TAG_DATA_FLAG_LIT;
    *retval = f;

    return 0;
}
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC diagnostic pop
#endif

/* adapter:
           stream <- filetype object

   The whole file layer reaches its streams through here, and every one of
   those reads is followed by a call through the method table the stream
   begins with. What comes back is therefore not data but a jump target,
   and the only thing standing behind it is the entity the object names
   and the handle that entity carries.

   A file object can outlive that entity: restore releases the files
   opened since the save it undoes, and the collector reclaims the ones
   nothing reaches, either of which puts the number back on the free list
   to be handed out again. The entity then holds the free list's own
   link word, or whatever the next allocation stored there. Being in
   range says only that some entity is there; the tag is what says it is
   still a stream, and the handle is resolved against the entity it was
   read from, so a number that was a handle somewhere else names nothing
   here. Ask, and answer "no stream" when it is not, which is the answer
   every caller already handles -- status reports the file closed,
   closefile has nothing left to do, and a read or a write reports an
   ioerror. */
Xpost_File *xpost_file_get_file_pointer(Xpost_Memory_File *mem,
                                        Xpost_Object f)
{
    unsigned int tag;

    if (!xpost_memory_table_get_tag(mem, f.mark_.padw, &tag) || tag != filetype)
        return NULL;
    return (Xpost_File *)xpost_handle_block_at(mem, f.mark_.padw,
                                               XPOST_HANDLE_FILE,
                                               XPOST_FILE_BLOCK_SIZE);
}

/* make sure the FILE* is not null */
int xpost_file_get_status(Xpost_Memory_File *mem,
                          Xpost_Object f)
{
    return xpost_file_get_file_pointer(mem, f) != NULL;
}

/* A file's access is a set of capabilities, not a rung on the ladder an
   array or a dictionary sits on: the access string it was opened with
   settles reading and writing independently (PLRM 3.8.1), so a file
   opened for writing may be written and not read, which no rung says.
   The set belongs to the object rather than to the stream (PLRM 3.3.2),
   so it is kept in the object's tag and a copy taken before a reduction
   keeps the capabilities it was copied with.

   Reading and writing live in the tag's access field, which is where
   every file constructor puts them. Executing has nowhere to go in a
   two-bit field, and it is only ever worth a bit of its own once read
   access has gone -- a file that may be read may be executed, since
   running a file is reading it a token at a time -- so it is carried
   above the field, in a tag bit a file has no other use for, and only
   for the one access that needs it. */
Xpost_Object_Tag_Access xpost_file_get_access(Xpost_Context *ctx,
                                              Xpost_Object f)
{
    Xpost_Object_Tag_Access access;

    (void)ctx;
    access = (Xpost_Object_Tag_Access)
        ((f.tag & XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK)
         >> XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    if ((access & XPOST_OBJECT_TAG_ACCESS_FILE_READ)
        || (f.tag & XPOST_OBJECT_TAG_DATA_FLAG_FILE_EXEC))
        access |= XPOST_OBJECT_TAG_ACCESS_FILE_EXEC;
    return access;
}

Xpost_Object xpost_file_set_access(Xpost_Context *ctx,
                                   Xpost_Object f,
                                   Xpost_Object_Tag_Access access)
{
    (void)ctx;
    f.tag &= ~(word)(XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK
                     | XPOST_OBJECT_TAG_DATA_FLAG_FILE_EXEC);
    f.tag |= (word)((access & (XPOST_OBJECT_TAG_ACCESS_FILE_READ
                               | XPOST_OBJECT_TAG_ACCESS_FILE_WRITE))
                    << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET);
    if ((access & XPOST_OBJECT_TAG_ACCESS_FILE_EXEC)
        && !(access & XPOST_OBJECT_TAG_ACCESS_FILE_READ))
        f.tag |= (word)XPOST_OBJECT_TAG_DATA_FLAG_FILE_EXEC;
    return f;
}

//FIXME assumes DiskFile subtype
/* call fstat. */
int xpost_file_get_bytes_available(Xpost_Memory_File *mem,
                                   Xpost_Object f,
                                   int *retval)
{
    int ret;
    FILE *fp;
    struct stat sb;
    long sz, pos;
    Xpost_File *file;

    file = xpost_file_get_file_pointer(mem, f);
    if (!file) return ioerror;
    /* only a seekable disk file has a determinable byte count; for a memory
       file or a filter it cannot be determined, so report -1 (PLRM) rather
       than reading a FILE* field the other file types do not have */
    if (file->methods != &disk_methods)
    {
        *retval = -1;
        return 0;
    }
    fp = ((Xpost_DiskFile*)file)->file;
    if (!fp) return ioerror;
    ret = fstat(fileno(fp), &sb);
    if (ret != 0)
    {
        XPOST_LOG_ERR("fstat did not return 0");
        return ioerror;
    }
    if (sb.st_size > LONG_MAX)
        return rangecheck;
    sz = (long)sb.st_size;

    pos = ftell(fp);
    if ((sz - pos) > INT_MAX)
        return rangecheck;

    *retval = (int)(sz - pos);

    return 0;
}

/* If f is a filter, the stream beneath it: a decode filter's source or an
   encode filter's target; otherwise NULL. Which it is was stated when the
   filter was constructed, so the answer is read off the file rather than
   off a list of the filters this build happens to have. */
static Xpost_File *_filter_underlying_stream(Xpost_File *f)
{
    switch (f->wraps)
    {
        case XPOST_FILE_WRAPS_SOURCE:
            return ((Xpost_FilterBase *)f)->source;
        case XPOST_FILE_WRAPS_TARGET:
            return ((Xpost_EncBase *)f)->target;
        case XPOST_FILE_WRAPS_NOTHING:
            break;
    }
    return NULL;
}

/* Mark a stream the machinery made for the filter about to wrap it. */
void xpost_file_hand_over(Xpost_Memory_File *mem, Xpost_Object f)
{
    Xpost_File *fp = xpost_file_get_file_pointer(mem, f);

    if (fp)
        fp->owned = 1;
}

/* Give up what a stream holds beside the struct itself, so that the
   struct can go.

   Two paths free a stream struct -- closing the object that names it,
   and releasing the stream a filter above it was the last to hold --
   and a kind of stream that owns memory has to be answered on both. It
   says so here once rather than at each of them, and clears what it
   gave up, so arriving twice costs nothing. */
static void _file_release_owned(Xpost_File *f)
{
    /* a stream backed by a procedure holds the strings that call
       answered with while a nested one was still being read */
    if (f->methods == &procsrc_methods || f->methods == &proctgt_methods)
    {
        Xpost_ProcFile *pf = (Xpost_ProcFile *)f;

        free(pf->pending);
        pf->pending = NULL;
        pf->npending = 0;
        pf->cpending = 0;
    }
    /* a reusable stream holds its source's bytes in a buffer of its own */
    if (f->methods == &rsd_methods)
    {
        Xpost_RsdFile *rf = (Xpost_RsdFile *)f;

        free(rf->data);
        rf->data = NULL;
        rf->len = 0;
        rf->pos = 0;
    }
}

/* Give up this filter's claim on the stream beneath it, and free that stream
   once nothing holds it any longer. A stream the machinery made for this
   filter alone has no other route to a close, so it is closed here -- and
   since such a stream can itself be a filter over another, the release runs
   on down the chain from it. */
static void _release_underlying(Xpost_Memory_File *mem, Xpost_File *f)
{
    Xpost_File *under = _filter_underlying_stream(f);

    if (!under)
        return;
    if (under->owned && !under->closed)
    {
        (void)xpost_file_close(under);
        under->closed = 1;
        _release_underlying(mem, under);
    }
    if (under->refs > 0)
        --under->refs;
    if (under->closed && under->refs == 0)
    {
        /* an owned stream has an entity of its own even though no program
           object names it -- the string forms of filter build one, and
           hand it to the filter -- and restore's close sweep walks
           entities. Clear it before the struct goes, or the sweep reaches
           this stream again through a pointer to freed memory and
           dispatches a close on it. */
        _file_forget_entity(mem, under);
        _file_release_owned(under);
        free(under);
    }
}

/* close the file,
   NULL the FILE*. */
int xpost_file_object_close(Xpost_Memory_File *mem,
                            Xpost_Object f)
{
    Xpost_File *fp;
    int ret;
    int lost;

    fp = xpost_file_get_file_pointer(mem, f);
    if (fp)
    {
#ifdef DEBUG_FILE
        printf("fclose");
#endif

        /* the stream may have had bytes still to place -- an encoding
           filter's end-of-data marker, a buffer the system had not taken
           yet. The teardown below runs either way, since the file is
           closed whether or not those bytes arrived (PLRM 3.8), and the
           loss is reported once it has. */
        lost = xpost_file_close(fp) != 0;
        /* what the stream holds goes when the stream does */
        _file_release_owned(fp);
        fp->closed = 1;
        _release_underlying(mem, fp);
        /* the close method released the stream's own resources; the object's
           only pointer to the backing struct is cleared next, and nothing
           else knows the struct is there, so free it here or it leaks. A
           filter still reading from (or writing to) this stream holds the
           struct alive instead, and frees it when it closes. */
        if (fp->refs == 0)
            free(fp);
        else
            /* The struct outlives the object, but the entity below does
               not: it is cleared just after this and can then be handed
               out again to whatever asks. Forget the number now, or
               releasing this struct later writes through whatever holds
               it by then. */
            fp->ent = 0;
        ret = xpost_handle_drop(mem, f.mark_.padw);
        if (!ret)
        {
            XPOST_LOG_ERR("cannot clear the file's handle in VM");
            return VMerror;
        }
        _file_retire_stamp(mem, f.mark_.padw);
        if (lost)
            return ioerror;
    }
    return 0;
}

/* Build the file object naming an entity. Two callers below reach a stream
   by the number of the entity holding it rather than through an object the
   program named, and everything they call takes the object. */
static Xpost_Object
_file_object_of_entity(unsigned int ent)
{
    Xpost_Object o = { 0 };

    o.mark_.tag = filetype;
    o.mark_.pad0 = 0;
    o.mark_.padw = ent;
    return o;
}

/* The entity of the stream this one wraps, or zero where it wraps nothing
   or the stream beneath it never had an entity of its own.

   A filter holds the stream beneath it as a plain pointer, and that stream
   is named by no object the collector can reach -- the string forms of
   filter build one and hand it straight to the filter above. This is the
   route to it, so that marking a filter can go on to mark what it reads
   through. */
unsigned int xpost_file_underlying_entity(Xpost_Memory_File *mem,
                                          unsigned int ent)
{
    Xpost_File *fp = xpost_file_get_file_pointer(mem, _file_object_of_entity(ent));
    Xpost_File *under;

    if (!fp)
        return 0;
    under = _filter_underlying_stream(fp);
    if (!under)
        return 0;
    return under->ent;
}

/* Give up the stream an entity holds, as closing a file object does.

   Two callers arrive here with an entity and no object naming it: the
   collector, once nothing reaches the entity any longer, and the
   interpreter's teardown, for whatever a job was still holding when it
   ended. A file is an entity inside virtual memory and a struct outside
   it, and reclaiming the entity alone strands the struct -- with the
   stream, the coding state and the buffers it holds -- beyond any further
   reach, so both callers release the struct here first.

   A stream another still reads through survives this: the close leaves the
   struct to the filter above, which holds a reference on it and frees it
   with its own teardown. Releasing an entity twice is therefore harmless
   as well, since the first pass clears the pointer the entity held and a
   stream that is not there is nothing to give up. */
void xpost_file_release_entity(Xpost_Memory_File *mem, unsigned int ent)
{
    Xpost_Object o = _file_object_of_entity(ent);

    if (!xpost_file_get_file_pointer(mem, o))
        return;
    (void)xpost_file_object_close(mem, o);
}

/* the interpreter has read a stream it was executing to its end. A
   reusable stream stays open there and merely rewinds (PLRM 3.13), so a
   program run off one can be positioned and run again; every other
   stream closes. */
int xpost_file_object_close_at_eod(Xpost_Memory_File *mem,
                                   Xpost_Object f)
{
    Xpost_File *fp = xpost_file_get_file_pointer(mem, f);

    /* A job-server stream that ended at a Control-D has not ended: it framed
       one job and reads on for the next (PLRM 3.7.7). It is not closed here
       -- the run's boundary closes it when the whole stream reaches its true
       end, where eot is clear because the end was real end-of-file and not a
       delimiter. */
    if (fp && fp->job_stream && fp->eot)
        return 0;

    if (fp && (fp->methods == &rsd_methods))
        return xpost_file_close(fp) ? ioerror : 0;

    return xpost_file_object_close(mem, f);
}

// returned value is count of complete size-sized chunks read.
// function may have read up to size-1 additional bytes.
int xpost_file_read(char *buf, int size, int count, Xpost_File *fp)
{
    int c, i, j, k = 0;

    for (i = 0; i < count; ++i)
    {
        for (j = 0; j < size; ++j)
        {
            c = xpost_file_getc(fp);
            if (c == EOF) return i;
            buf[k++] = c;
        }
    }

    return i;
}

int xpost_file_write(const char *buf, int size, int count, Xpost_File *fp)
{
    int i, j, k = 0;

    /* a stream that takes a run wholesale is handed the whole run;
       every other stream takes it a byte at a time */
    if (fp->methods->writeblock)
    {
        int wrote = fp->methods->writeblock(fp, (const unsigned char *)buf,
                                            size * count);
        if (wrote == EOF)
            return 0;
        return wrote / size;
    }

    for (i = 0; i < count; ++i)
        for (j = 0; j < size; ++j)
            if (xpost_file_putc(fp, buf[k++]) == EOF) return i;

    return i;
}

/* if the file is valid,
   read a byte. */
Xpost_Object xpost_file_read_byte(Xpost_Memory_File *mem,
                                  Xpost_Object f)
{
    Xpost_File *fp;
    int c;

    /* one lookup answers both questions: whether the object still names a
       stream, and which stream to read */
    fp = xpost_file_get_file_pointer(mem, f);
    if (!fp)
    {
        return invalid;
    }
retry:
    errno=0;
    c = xpost_file_getc(fp);
    if (c == EOF && errno==EINTR)
        goto retry;

    return xpost_int_cons(c);
}

/* if the file is valid,
   write a byte. */
int xpost_file_write_byte(Xpost_Memory_File *mem,
                          Xpost_Object f,
                          Xpost_Object b)
{
    Xpost_File *fp;

    /* one lookup answers both questions: whether the object still names a
       stream, and which stream to write */
    fp = xpost_file_get_file_pointer(mem, f);
    if (!fp)
    {
        return ioerror;
    }
    if (xpost_file_putc(fp, b.int_.val) == EOF)
    {
        return ioerror;
    }
    return 0;
}
