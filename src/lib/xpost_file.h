/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file xpost_file.h
 * @brief This file provides the Xpost functions.
 *
 * This header provides the Xpost management functions.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

#ifndef XPOST_F_H
#define XPOST_F_H

#include "xpost_private.h" /* XPOST_TEST_VISIBLE */

/*
   a filetype object uses .mark_.padw to store the ent
   for the Xpost_File *

   The Xpost_File code for abstract use of files and file-like
   interfaces is a slight variation of an approach described
   by Tim Rentsch.

   Using simple inheritance (by composition), the Xpost_File*
   functions are virtualized through this vtable. With the 
   specified inlining this should result in minimal overhead
   of simple pointer indirection.

   String-backed MemoryFiles and morphism-interfaced FilteredFiles
   will implement the same virtual functions for their respective
   structures.
   */

typedef struct Xpost_File Xpost_File;

/* What a file is a filter over. A decode filter is a file over the source
   it reads; an encode filter is a file over the target it writes; a file
   that is a stream in its own right is over nothing. Which of the three
   is not a question about the coding, so it is not asked of the coding:
   the constructor a filter is born through states it, in the same call
   that names the filter's methods and hands it the stream, and the
   machinery that takes and gives up the claim on that stream reads the
   answer off the file. */
typedef enum
{
    XPOST_FILE_WRAPS_NOTHING = 0,
    XPOST_FILE_WRAPS_SOURCE,
    XPOST_FILE_WRAPS_TARGET
} Xpost_File_Wraps;

typedef struct Xpost_File_Methods
{
    int (*readch)(Xpost_File*);
    int (*writech)(Xpost_File*, int);
    int (*close)(Xpost_File*);
    int (*flush)(Xpost_File*);
    void (*purge)(Xpost_File*);
    int (*unreadch)(Xpost_File*, int);
    /* A position in a stream is counted in the width the stream itself
       counts in, which is not long everywhere -- long is 32 bits on
       LLP64. Narrowing here would spend a position past two gigabytes
       before whoever weighs it against the integer that must carry it
       ever saw it, and that check would be handed a number that had
       already wrapped, so it could not refuse what it exists to refuse. */
    long long (*tell)(Xpost_File*);
    int (*seek)(Xpost_File*, long long);
    /* A run of bytes lodged in one call, answering how many it took.
       A stream that leaves this unset takes runs a byte at a time
       through writech instead; only a stream whose backing can take a
       run wholesale carries one, since the point of it is to write the
       run without a call per byte. */
    integer (*writeblock)(Xpost_File*, const unsigned char *buf, integer n);
} Xpost_File_Methods;

/* A filter holds the stream it decodes from (or encodes to) as a plain
   pointer, so that stream must outlive it however the two are closed.
   refs counts the filters holding this stream; closed records that its
   own file object has been closed. A closed stream whose refs have not
   all been released stays allocated -- its methods then report end of
   data and refuse writes -- and the last filter to release it frees it.

   owned marks a stream the file machinery made for one filter's use and
   which no program object names: the filter above it is the only thing
   that can close it, so it does, along with itself.

   ent is the file entity carrying the handle on this struct. No program
   object naming a stream does not mean no ENTITY names it -- an owned
   stream still has one, and restore's close sweep walks entities rather
   than objects, so it reaches one. Whoever frees the struct must
   therefore clear the entity first, and can only do that if the struct
   says which entity that is.

   wraps says which stream, if any, this file holds beneath it, and so
   which of the two filter bases it begins with. */
struct Xpost_File
{
    Xpost_File_Methods *methods;
    int refs;
    int closed;
    int owned;
    unsigned int ent;
    Xpost_File_Wraps wraps;
    /* A job-server channel frames its jobs with a Control-D (0x04): reading
       one ends the job and the stream reads on for the next (PLRM 3.7.7).
       This is device-dependent channel framing, not a PostScript token, so
       it is honoured only on a stream the embedder marks as such a channel;
       a file a program opens, a filter, and every nested read leave it 0 and
       so pass a 0x04 through as an ordinary byte. */
    int job_stream;
    int eot; /* a 0x04 was read from a job_stream: this job ended at the
                delimiter and the stream has more after it */
};

/* What a file's handle is recorded and asked for as: the base every
   stream begins with, whatever the subtype allocated behind it. A handle
   of this kind names a stream, and which stream it is the method table
   the struct opens with says -- so there is one question to ask of a
   file entity and one answer to check, and a block issued for anything
   else does not answer it. */
#define XPOST_FILE_BLOCK_SIZE (sizeof(Xpost_File))

/* Every file subtype begins with the base, so a subtype's address and its
   base's address are the same address and the cast between them is the
   whole conversion. That is what lets the method table hold one function
   type for every subtype: a method receives the base and casts back down
   to the struct the allocation actually is. */
typedef struct Xpost_DiskFile
{
    Xpost_File methods;
    FILE *file;
    int poll_before_read; /* select() before each read: only needed for
                             pipes/terminals/sockets, where a read may block;
                             regular files are always ready */
    int input;            /* opened for reading only. flushfile means two
                             different things by the direction (PLRM 8.2) and
                             a stdio stream does not say which it is, so the
                             opener, which knows the access string, says */
} Xpost_DiskFile;

typedef struct Xpost_MemoryFile
{
    Xpost_File methods;
    unsigned char *contents;
    int is_malloc;
    int is_read;
    size_t read_next;
    size_t read_limit;
    size_t write_next;
    size_t write_capacity;
} Xpost_MemoryFile;

/* A stream whose bytes are a procedure's to supply or to dispose of
   (PLRM 3.13.1). It is a stream and not a filter: the filter layered
   over it decodes or encodes as it always did, and reads or writes
   through the one pointer it holds either way, so no coding knows a
   procedure is down there.

   The procedure runs on the stacks of the run that is reading or
   writing, from inside the read or the write. That is what makes the
   supply incremental: a source is asked for the next buffer at the
   moment the filter runs out of the last one, so a program can compute
   its data as the filter consumes it, and neither the length of the
   data nor the time the procedure takes to produce it is bounded here.

   The procedure and the string it last returned are objects held in a C
   struct, which the collector does not walk. Both are named to it
   instead, through the file entity that holds this struct. */
typedef struct Xpost_ProcFile
{
    Xpost_File methods;
    Xpost_Context *ctx;
    Xpost_Object proc;
    Xpost_Object buf;  /* the string the procedure last gave back */
    unsigned int pos;  /* how much of buf has been handed over or filled */
    int pushback;      /* a byte handed over and given back, held apart
                          from buf: what gives it back may do so after
                          the procedure has been asked for the next
                          string, and buf is that string by then */
    int eod;           /* a source that answered with no bytes, or a
                          target already told the data had ended */
    int started;       /* a target that has been asked for its first
                          buffer: the empty first call is made once */
    int running;       /* the procedure is on the stack now */
    /* Strings a call answered with while a nested call was still holding
       bytes of an earlier one. A source procedure may read the very
       stream it supplies (PLRM 3.13.1 forbids nothing of the sort), and
       the nested read is served first, so the string the outer call
       answers with arrives while the buffer it would become still has
       bytes to hand over. It waits here and is taken up in turn, rather
       than replacing a buffer that is still being read. The depth of
       the nesting bounds how many can be waiting. */
    Xpost_Object *pending;
    int npending;
    int cpending;
} Xpost_ProcFile;


/* interface fgetc
   in preparation for more elaborate cross-platform non-blocking mechanisms
cf. http://stackoverflow.com/questions/20428616/how-to-handle-window-events-while-waiting-for-terminal-input
and http://stackoverflow.com/questions/25506324/how-to-do-pollstdin-or-selectstdin-when-stdin-is-a-windows-console
   */
/**
 * @brief Read a byte from an Xpost_File abstraction.
 */
static inline
int xpost_file_getc(Xpost_File *in)
{
    int c;
    /* Control-D on a job-server channel is that channel's end-of-file
       (PLRM 3.7.7): the job ends here and the stream is read on for the
       next. Once one is read the stream stays at end-of-file for the rest
       of the job -- the scanner reads past a lone end-of-file while it
       skips space, so a one-shot end here would be swallowed and the next
       job read into this one. The run's boundary clears eot to read the
       next job. Marked only on the outermost server stream, so a program's
       own files and nested reads pass the byte through untouched. */
    if (in->job_stream && in->eot)
        return EOF;
    c = in->methods->readch(in);
    if (in->job_stream && c == 0x04)
    {
        in->eot = 1;
        return EOF;
    }
    return c;
}

static inline
int xpost_file_putc(Xpost_File *out, int c)
{
    return out->methods->writech(out, c);
}

static inline
int xpost_file_close(Xpost_File *f)
{
    return f->methods->close(f);
}

static inline
int xpost_file_flush(Xpost_File *f)
{
    return f->methods->flush(f);
}

static inline
void xpost_file_purge(Xpost_File *f)
{
    f->methods->purge(f);
}

static inline
int xpost_file_ungetc(Xpost_File *in, int c)
{
    return in->methods->unreadch(in, c);
}

static inline
long long xpost_file_tell(Xpost_File *f)
{
    return f->methods->tell(f);
}

static inline
int xpost_file_seek(Xpost_File *f, long long offset)
{
    return f->methods->seek(f, offset);
}


/**
 * @brief Construct a file object given a FILE* and the direction it was
 * opened in (non-zero for a stream that is only read).
 */
Xpost_Object xpost_file_cons(Xpost_Memory_File *mem, /*@NULL@*/ const FILE *fp,
                             int input);

/**
 * @brief Construct a readable file object over a private copy of a
 * pointer and size.
 */
Xpost_Object xpost_file_cons_readstring(Xpost_Memory_File *mem, const unsigned char *ptr, unsigned int len);

/**
 * @brief Construct a readable stream whose bytes a procedure supplies.
 *
 * The procedure is called whenever the stream runs out of bytes to hand
 * over, and answers with a string holding the next of them; a string of
 * no bytes ends the data (PLRM 3.13.1). It is called from inside the
 * read, so a filter layered over this reads the data as the program
 * computes it, however much of it there turns out to be.
 */
Xpost_Object xpost_file_cons_procsource(Xpost_Context *ctx, Xpost_Object proc);

/**
 * @brief Construct a writeable stream whose bytes a procedure disposes
 * of.
 *
 * The procedure is called with a string and a boolean, and answers with
 * the string the stream is to fill next (PLRM 3.13.1): with an empty
 * string and true to be asked for the first buffer, with the filled
 * buffer and true whenever it is full, and with whatever is left and
 * false as the stream closes.
 */
Xpost_Object xpost_file_cons_proctarget(Xpost_Context *ctx, Xpost_Object proc);

/**
 * @brief How many objects a file holds outside virtual memory.
 *
 * A procedure stream holds the procedure it calls, the string that
 * procedure last answered with, and any strings waiting behind that one,
 * all in a C struct the collector does not walk; every other kind of
 * file holds no object at all.
 */
int xpost_file_held_count(Xpost_Memory_File *mem, unsigned int ent);

/**
 * @brief One of the objects a file holds outside virtual memory.
 */
Xpost_Object xpost_file_held_object(Xpost_Memory_File *mem, unsigned int ent,
                                    int i);

/**
 * @brief Hand a synthesised stream to the filter that will wrap it.
 *
 * The file machinery makes a stream for one filter's use -- an in-memory
 * file over a copy of a string, a decoding filter that a predictor stage
 * is layered over -- and no program object names it. Marking it here is
 * what makes the wrapping filter close and free it when it closes;
 * without that the stream, and everything it holds, is unreachable and
 * unreleasable.
 */
void xpost_file_hand_over(Xpost_Memory_File *mem, Xpost_Object f);

/**
 * @brief Construct an ASCII85Decode filter file over a source file object.
 *
 * The source file is not owned: closing the filter leaves it open,
 * positioned just after the "~>" end-of-data marker once the filter
 * has been read to end of file.
 */
Xpost_Object xpost_file_cons_filter_a85(Xpost_Memory_File *mem, Xpost_Object src);

/**
 * @brief The remaining decode filter constructors: hexadecimal,
 * run-length, byte-range/delimited subfiles, and (with zlib) flate.
 * All follow the ASCII85Decode contract: read filters over an
 * unowned source.
 */
Xpost_Object xpost_file_cons_filter_hex(Xpost_Memory_File *mem, Xpost_Object src);
Xpost_Object xpost_file_cons_filter_rle(Xpost_Memory_File *mem, Xpost_Object src);
Xpost_Object xpost_file_cons_filter_subfile(Xpost_Memory_File *mem, Xpost_Object src, int count, const char *eod, int eodlen);
Xpost_Object xpost_file_cons_filter_flate(Xpost_Memory_File *mem, Xpost_Object src);
Xpost_Object xpost_file_cons_filter_dct(Xpost_Memory_File *mem, Xpost_Object src);
Xpost_Object xpost_file_cons_filter_rsd(Xpost_Memory_File *mem, Xpost_Object src);
Xpost_Object xpost_file_cons_filter_lzw(Xpost_Memory_File *mem, Xpost_Object src, int early);

/**
 * @brief undo the differencing an LZW or Flate stream was compressed with.
 *
 * Layers over the decompressing filter: predictor 2 is horizontal
 * differencing, 10 and above the PNG row filters (PLRM Table 3.20).
 */
Xpost_Object xpost_file_cons_filter_predictor(Xpost_Memory_File *mem,
                                              Xpost_Object src,
                                              int predictor, int colors,
                                              int bpc, int columns);
Xpost_Object xpost_file_cons_filter_ccitt(Xpost_Memory_File *mem, Xpost_Object src, int k, int columns, int rows, int blackis1, int byteal, int eol, int eob);
Xpost_Object xpost_file_cons_filter_enc_null(Xpost_Memory_File *mem, Xpost_Object tgt);
Xpost_Object xpost_file_cons_filter_enc_hex(Xpost_Memory_File *mem, Xpost_Object tgt);
Xpost_Object xpost_file_cons_filter_enc_a85(Xpost_Memory_File *mem, Xpost_Object tgt);
Xpost_Object xpost_file_cons_filter_enc_rle(Xpost_Memory_File *mem, Xpost_Object tgt, int recsize);
Xpost_Object xpost_file_cons_filter_enc_flate(Xpost_Memory_File *mem, Xpost_Object tgt);
Xpost_Object xpost_file_cons_filter_enc_lzw(Xpost_Memory_File *mem, Xpost_Object tgt, int early);
Xpost_Object xpost_file_cons_filter_enc_ccitt(Xpost_Memory_File *mem, Xpost_Object tgt, int k, int columns, int rows, int blackis1, int byteal, int eol, int eob);
Xpost_Object xpost_file_cons_filter_enc_dct(Xpost_Memory_File *mem, Xpost_Object tgt, int columns, int rows, int colors, double qfactor, int colortransform, const int *hsamp, const int *vsamp);
Xpost_Object xpost_file_cons_filter_eexec(Xpost_Memory_File *mem, Xpost_Object src);

/**
 * @brief The single path-to-stream opener for disk-backed files.
 *
 * Every disk file the interpreter opens passes through here, so
 * file-access policy has one enforcement point. Returns an open stream,
 * or NULL with *err set to a suitable error code. @p internal marks a
 * trusted interpreter-managed path (temporary scratch) rather than one
 * derived from the running program.
 */
FILE *xpost_diskfile_fopen(const char *path, const char *mode, int internal, int *err);

/**
 * @brief Delete @p path, subject to the file-access sandbox.
 *
 * A filesystem-control operation rather than a stream open. Under the
 * engaged sandbox @p path must be write-permitted. Returns 0 on success,
 * -1 with *err set otherwise.
 */
int xpost_diskfile_remove(const char *path, int *err);

/**
 * @brief Rename @p oldpath to @p newpath, subject to the sandbox.
 *
 * Under the engaged sandbox both paths must be write-permitted. Returns 0
 * on success, -1 with *err set otherwise.
 */
int xpost_diskfile_rename(const char *oldpath, const char *newpath, int *err);

/**
 * @brief May the running program see @p path (to open or enumerate it)?
 *
 * True when the sandbox is not engaged or @p path is read-permitted. Used
 * to filter directory enumeration to the visible files.
 */
int xpost_diskfile_readable(const char *path);

/**
 * @brief Has the file-access sandbox been engaged?
 */
int xpost_path_control_is_engaged(void);

/**
 * @brief Validate that s[0..len) is a safe single path component.
 *
 * Rejects path separators, ':' , NUL and control bytes, '.' and '..', a
 * leading dot or space, a trailing dot or space, and reserved device
 * names, so an externally-derived name cannot express a path. Returns 1
 * if safe, 0 otherwise.
 */
int xpost_path_safe_leaf(const char *s, size_t len);

/**
 * @brief Open @p rel for reading beneath directory @p root.
 *
 * The operating system confines resolution to @p root (no escape via ".."
 * or a symlink). @p rel should already be composed of safe leaves. Returns
 * an open stream, or NULL with *err set.
 */
FILE *xpost_diskfile_fopen_beneath(const char *root, const char *rel, int *err);

/**
 * @brief Open and construct a file object given filename and mode.
 */
XPOST_TEST_VISIBLE int xpost_file_open(Xpost_Memory_File *mem, char *fn, char *mode, Xpost_Object *retval);

/**
 * @brief Return the FILE* from the file object.
 */
Xpost_File *xpost_file_get_file_pointer(Xpost_Memory_File *mem, Xpost_Object f);

/**
 * @brief Get the status of the file object.
 */
int xpost_file_get_status(Xpost_Memory_File *mem, Xpost_Object f);

/**
 * @brief Yield the capabilities a file object's access grants.
 *
 * Installed as the filetype hook for xpost_object_get_access. A file's
 * access is a set of the FILE_ capabilities rather than a rung of the
 * ladder the other types sit on, since its read and its write are
 * settled independently by the access string (PLRM 3.8.1).
 */
Xpost_Object_Tag_Access xpost_file_get_access(Xpost_Context *ctx, Xpost_Object f);

/**
 * @brief Give a file object the capabilities named.
 *
 * Installed as the filetype hook for xpost_object_set_access. The
 * capabilities belong to the object (PLRM 3.3.2), so they go in its tag
 * and another object over the same stream keeps its own.
 */
Xpost_Object xpost_file_set_access(Xpost_Context *ctx, Xpost_Object f,
                                   Xpost_Object_Tag_Access access);

/**
 * @brief Return number of bytes available to read.
 */
int xpost_file_get_bytes_available(Xpost_Memory_File *mem, Xpost_Object f, int *retval);

/**
 * @brief Close the file and deallocate the descriptor in VM.
 */
int xpost_file_object_close(Xpost_Memory_File *mem, Xpost_Object f);
int xpost_file_object_close_at_eod(Xpost_Memory_File *mem, Xpost_Object f);

/**
 * @brief Return the entity of the stream a file wraps, or zero for none.
 */
unsigned int xpost_file_underlying_entity(Xpost_Memory_File *mem, unsigned int ent);

/**
 * @brief Release the stream an entity holds, naming it by entity alone.
 */
void xpost_file_release_entity(Xpost_Memory_File *mem, unsigned int ent);

integer xpost_file_read(char *buf, integer size, integer count, Xpost_File *fp);
integer xpost_file_write(const char *buf, integer size, integer count, Xpost_File *fp);
FILE *xpost_file_stdio_stream_get(Xpost_File *fp);

/**
 * @brief Read a byte from file object.
 */
Xpost_Object xpost_file_read_byte(Xpost_Memory_File *mem, Xpost_Object f);

/**
 * @brief Tell the stream layer how to ask whether another context could run.
 *
 * A read that would wait answers "not yet" instead when something else
 * could be running; with nothing else to run it waits, so a single
 * context sleeps rather than spinning.
 */
void xpost_file_other_runnable_set(int (*fn)(void));

/**
 * @brief Let this stream answer that a read would wait, for one read.
 *
 * Armed only for a plain host stream. A filter's source is never armed:
 * a filter handed an unexpected end-of-file would take its data for
 * finished.
 */
void xpost_file_ioblock_arm(Xpost_File *f);

/**
 * @brief Take the offer back, answering whether the read would have waited.
 */
int xpost_file_ioblock_disarm(void);

/**
 * @brief Write a byte to a file object.
 */
int xpost_file_write_byte(Xpost_Memory_File *mem, Xpost_Object f, Xpost_Object b);

/**
 * @}
 */


int xpost_diskfile_stat(const char *path, long long *pages, long long *bytes,
                        long long *referred, long long *created);

#endif
