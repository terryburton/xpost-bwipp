/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_CONTEXT_H
#define XPOST_CONTEXT_H

/**
 * @file xpost_context.h
 * @brief This file provides the context functions.
 *
 * This header provides the Xpost context functions.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

/* The number of execution contexts (Display PostScript, PLRM 2nd ed 7.1)
   that can exist at once, which is the ceiling fork raises limitcheck at.
   PLRM leaves the number implementation-defined; the Display PostScript
   Client Library manual puts a real server's limit "on the order of 50 to
   100", so this sits at the top of that range with headroom rather than at
   a token handful. The table of this many Xpost_Context structures is
   allocated once with the interpreter (an empty slot is only the structure,
   ~3 KB; a context's stacks are allocated in shared VM when fork creates
   it), so the cost of the unused slots a single-context run leaves is a
   fixed, modest one. A power of two keeps the slot index a mask. */
#define MAXCONTEXT 128

/**
 * @brief valid values for Xpost_Context::vmmode
 */
enum { LOCAL, GLOBAL };

/**
 * @brief valid values for Xpost_Context::state
 */
enum { C_FREE, C_IDLE, C_RUN, C_WAIT, C_IOBLOCK, C_ZOMB };

/**
 * @def XPOST_OP_REFS
 * @brief every operator the interpreter itself reaches for, by name
 *
 * C reaches a standard operator by holding the operator object. Pushing
 * the operator's name instead defers the decision to the dictionary stack
 * as it stands when the name runs, so a program that defines that name --
 * which PLRM 3.3 entitles it to do -- takes over the inside of a standard
 * operator. Resolving a name at run time also costs a global name intern
 * and a linear walk of the operator table, and answers null for a name
 * that is not there, which the interpreter then schedules.
 *
 * This is the one statement of that set. Each entry gives the C spelling
 * of the reference and the operator's name in the language; the operator
 * prefix on some of them keeps a C keyword out of a member name. From it
 * are generated the member that holds each opcode, the marker written
 * before registration begins, and the check that every one was captured.
 * Capture happens inside xpost_operator_cons keyed by the name, so it
 * cannot be forgotten at a registration or attached to the wrong operator
 * -- both of which the marker and the check exist because of.
 *
 * Reach an entry through XPOST_OP (the operator object, to schedule it) or
 * XPOST_OP_CODE (its opcode, to recognise it); tests/check-op-references.sh
 * holds the tree to that.
 */
#define XPOST_OP_REFS(_) \
    /* recognised inline by the procedure walker */ \
    _(oppop,               "pop") \
    _(opexch,              "exch") \
    _(opdup,               "dup") \
    _(opindex,             "index") \
    _(oproll,              "roll") \
    _(opadd,               "add") \
    _(opsub,               "sub") \
    _(opmul,               "mul") \
    _(opeq,                "eq") \
    _(opne,                "ne") \
    _(oplt,                "lt") \
    _(ople,                "le") \
    _(opgt,                "gt") \
    _(opge,                "ge") \
    _(opif,                "if") \
    _(opifelse,            "ifelse") \
    _(opdef,               "def") \
    _(opget,               "get") \
    _(opput,               "put") \
    _(optype,              "type") \
    /* iteration: the operator that starts one and the continuation that \
       carries it, which is scheduled beneath each pass */ \
    _(opfor,               "for") \
    _(repeat,              "repeat") \
    _(loop,                "loop") \
    _(forall,              "forall") \
    _(filenameforall,      "filenameforall") \
    _(forcont,             "for.iterate") \
    _(repeatcont,          "repeat.iterate") \
    _(loopcont,            "loop.iterate") \
    _(arrayforallcont,     "forall.array.iterate") \
    _(stringforallcont,    "forall.string.iterate") \
    _(dictforallcont,      "forall.dict.iterate") \
    _(contfilenameforall,  "contfilenameforall") \
    /* scheduled by an operator implemented in C to finish its own work */ \
    _(cvx,                 "cvx") \
    _(load,                "load") \
    _(exec,                "exec") \
    _(token,               "token") \
    _(copy,                "copy") \
    _(stop,                "stop") \
    _(quit,                "quit") \
    _(matrix,              "matrix") \
    _(defaultmatrix,       "defaultmatrix") \
    _(setmatrix,           "setmatrix") \
    _(concat,              "concat") \
    _(concatmatrix,        "concatmatrix") \
    _(rotate,              "rotate") \
    _(transform,           "transform") \
    _(itransform,          "itransform") \
    _(moveto,              "moveto") \
    _(lineto,              "lineto") \
    /* closes the array a device method call is assembled into */ \
    _(rbracket,            "]") \
    /* the frame marker a wrapped operator leaves on the execution stack, \
       and the same marker on a call a failure passed a boundary to leave */ \
    _(wrapdone,            "wrap.done") \
    _(wrapsealed,          "wrap.sealed") \
    /* the boundary an operator calling back into a procedure of the \
       program's leaves on the execution stack */ \
    _(calloutdone,         "callout.done")

/**
 * @def XPOST_OP_CODE
 * @brief the opcode of a referenced operator, for recognising one
 */
#define XPOST_OP_CODE(ctx, ref) ((ctx)->opcode_shortcuts.ref)

#define XPOST_OP_REF_MEMBER(ref, name) int ref;

/**
 * @def XPOST_CONTEXT_OBJECT_ROOTS
 * @brief every object the context holds on its own account
 *
 * The collector begins its walk from these, and walks them by expanding
 * this list rather than by naming them one at a time. An object the
 * context holds and the collector does not walk is taken by the first
 * collection that reaches it, so the declaration and the walk come from
 * one list: a field added here is marked, and a field marked here is
 * declared.
 *
 * A field belongs here when the context is what keeps the object
 * reachable. One holding a name, or an object something else already
 * keeps, need not be here and costs little to be here anyway.
 */
#define XPOST_CONTEXT_OBJECT_ROOTS(_) \
    /* the object being executed, named in an error report */ \
    _(currentobject) \
    /* the array a procedure is being run out of: the run resolves its \
       storage to a pointer, so it must outlive whatever else names it */ \
    _(executingarray) \
    /* the procedure the arc operators run, built while they are \
       installed, before there is a dictionary to keep it in */ \
    _(arcstartproc) \
    /* the page device the graphics state template names, and the \
       procedure that retires it */ \
    _(pagedevice) \
    _(pagedevice_destroy) \
    /* the interpreter's own machinery, local and global */ \
    _(privatedict) \
    _(globalprivatedict) \
    /* the file a run wrapped around its program */ \
    _(run_input_file) \
    /* the window a device draws into, and the handler it reports to */ \
    _(window_device) \
    _(event_handler) \
    /* the two font directories, which setglobal rebinds FontDirectory \
       between as the allocation mode calls for (PLRM 3.7.5) */ \
    _(localfontdir) \
    _(globalfontdir)

#define XPOST_CONTEXT_DECLARE_ROOT(f) Xpost_Object f;

/** @struct Xpost_Context
 * @brief The context structure for a thread of execution of ps code
 */
struct _Xpost_Context {

    /**< opcode of each operator the interpreter reaches for, captured as
         the operators are registered; see XPOST_OP_REFS */
    struct
    {
        XPOST_OP_REFS(XPOST_OP_REF_MEMBER)
    } opcode_shortcuts;
#undef XPOST_OP_REF_MEMBER


    /* Set when a registration could not place an operator in systemdict.
       Registration is several hundred calls spread over two dozen
       modules, each of which would otherwise have to carry the answer
       back by hand; this collects it in one place, which
       xpost_oplib_init_ops reads once when they have all run. An
       interpreter missing an operator is not one that can run a
       program. */
    int operator_install_refused;

    /* operands the dispatcher coerced from integer to real for the current
       operator; an error restores them to the originals the program pushed,
       as PLRM 3.11 requires. Empty for operators that coerce nothing. */
    int op_restore_n;
    unsigned char op_restore_idx[8];
    /* the objects the context holds on its own account; the collector
       walks exactly these, from the same list that declares them */
    XPOST_CONTEXT_OBJECT_ROOTS(XPOST_CONTEXT_DECLARE_ROOT)

    Xpost_Object op_restore_val[8];

    /* array-packing mode (setpacking/currentpacking): when set, the scanner
       builds { } procedures read-only. packing_hist records the mode at each
       save level so restore reverts it, as the parameter is save/restore-subject */
    int packing;
    unsigned char packing_hist[256];

    /* cache of name -> value resolutions against the dict stack,
       invalidated in bulk whenever any binding may have changed */
    unsigned int *namecache_gen;   /**< generation per (name index, bank) */
    Xpost_Object *namecache_val;   /**< cached resolution */
    unsigned int namecache_size;   /**< entries allocated */
    unsigned int namebind_gen;     /**< current binding generation */

    /** the name a wrapped call's saved-operand array is kept in
        privatedict under, interned on first use. A name object is an
        index into the name stack of the context that interned it and
        names something else, or nothing, in any other, so the one
        interned here belongs to this context and is kept with it. */
    Xpost_Object namewrapsave;

    Xpost_Object typenames[XPOST_OBJECT_NTYPES + 1]; /**< executable name per
                                                          type index, populated
                                                          on first use; the
                                                          index past the object
                                                          types names a packed
                                                          array (see
                                                          xpost_op_type.h) */

    /*@dependent@*/
    Xpost_Memory_File *gl; /**< global VM */
    /*@dependent@*/
    Xpost_Memory_File *lo; /**< local VM */

    unsigned int id; /**< cid for this context */

    unsigned int os, es, ds, hold; /**< stack addresses in local VM */
    /** The random number generator's state (PLRM 8.2 rand, srand, rrand).
        rrand reports it as an integer and srand takes one back, so it is
        the integer's own width: a state narrower than that would report a
        seed as something other than the seed it was given. */
    dword rand_next;
    unsigned int vmmode; /**< allocating in GLOBAL or LOCAL */
    /** the allocation mode at each save level, so restore reverts it, as
        the parameter is save/restore-subject (PLRM 8.2 restore) */
    unsigned char vmmode_hist[256];

    /** The VMThreshold user parameter (PLRM C.3.5): the count of bytes
        allocated between the collections this interpreter runs of its
        own accord. Setting it gives the count to both banks of virtual
        memory, which count it down as they allocate. Held per context,
        as a user parameter is (PLRM C.1.1), and recorded at each save
        level so restore reverts it (PLRM 8.2 restore). The count is the
        width of the integers a program hands it, so a count it can
        express is a count it reads back. */
    integer vmthreshold;
    integer vmthreshold_hist[256];

    /** Whether bind replaces a bound procedure that matches an IdiomSet
        template with the paired substitute (PLRM 3.12.1). The
        IdiomRecognition user parameter: held per context as a user
        parameter is (PLRM C.1.1), and recorded at each save level so
        restore reverts it (PLRM 8.2 restore). True at context start. */
    int idiomrecognition;
    unsigned char idiomrecognition_hist[256];

    /** Which banks a collection that runs of its own accord reclaims,
        at each save level, in the form xpost_garbage_auto_banks reports.
        That setting is the whole of the VMReclaim user parameter -- it
        is what currentuserparams reads and what vmreclaim writes -- so
        putting it back at the restore is what reverts the parameter. */
    unsigned char autobanks_hist[256];

    /** The two font directories, so setglobal can rebind the name
        FontDirectory to whichever the allocation mode calls for (PLRM).
        Both are null until the boot file has defined them. */
    unsigned int state;  /**< process state: running, blocked, iowait */
    unsigned int quit;  /**< if 1 cause mainloop() to return, if 0 keep looping */

    /** How many evaluations are nested inside operators of this
        context's run. A filter reading from a procedure data source
        runs that procedure from inside the read, and the procedure may
        read a filter of its own, so the depth is the program's to
        choose and is bounded rather than trusted. */
    unsigned int nest_depth;

    /** The error a procedure backing a stream failed with, waiting for
        the operator that was reaching through that stream to answer for
        it. Zero when there is none. */
    unsigned int callback_error;


    /** The page device the graphics state template names, recorded by
        setpagedevice as it installs one, with the save depth it was
        installed at (depth + 1; zero when nothing is recorded) and the
        Destroy operator the instance carried then (null for a device
        whose Destroy is a PostScript procedure). A device holds its
        raster or its content accumulator outside virtual memory, so the
        collector has no claim on that memory and no reason to look:
        whoever takes the device out of the graphics state has to release
        it. setpagedevice does so for the device it replaces, and restore
        does so here, for the one it displaces (PLRM 6.1). The device is
        rooted in the collector, so the entity cannot be recycled while
        this names it. */
    unsigned int pagedevice_depth;

    /**< privatedict -- a LOCAL dictionary that holds the interpreter's local
         machinery (the device class dictionaries, the wrapped-operator anchor
         procedures, the graphics scratch and template). Rooted here so the
         collector keeps it and its contents, but never pushed on the dict
         stack, so a program can neither name nor enumerate its members. The
         C reaches the device classes through it; PostScript through a frozen
         reference. Set from init.ps by .setprivatedict. */

    /**< globalprivatedict -- the GLOBAL private namespace, .xpostsys. Rooted
         here for the same reason privatedict is, and reached from C the same
         way: the namespace drops its userdict anchor at lockdown, so without
         this record it is reachable only through the references frozen into
         procedure bodies, and not reachable from C at all.

         What it holds for C is state that outlives a save and belongs to no
         one context: the caches whose other half is a host resource a C
         static keeps. A slot in such a cache and the object it names must
         stay reachable together, and a per-context record cannot do that --
         contexts share these memory banks, and the context that filled the
         cache may end before one still reading it. Held here, the objects
         are reachable while any context that could reach the cache is.

         Global, so it may not hold local objects; local machinery belongs in
         privatedict. Set from init.ps by .setglobalprivatedict. */

    /**< executingarray -- the array a procedure is being run out of, while
         it is being run. Its storage is resolved to a pointer once and the
         elements read through it, so the array must not be reclaimed
         underneath the walk, and the walk holds it in C where the
         collector cannot see it. Recorded here for the length of the run
         and put back to what it was afterwards, so a procedure that runs
         another leaves the outer one rooted too. */

    /**< arcstartproc -- the procedure the arc operators run to reach an
         arc's starting point. It is built while those operators are
         installed, before there is any dictionary to keep it in, and the
         file that built it holds it in a variable of its own, which the
         collector does not walk. Rooted here instead. */
    const char *device_str;

    int quiet; /**< the -q/--quiet startup flag, retained so the shutdown
                    message can honour it without reading a PostScript name:
                    QUIET lives in the private .internaldict, out of a program's
                    reach, once init.ps has relocated it there. */

    int ignoreinvalidaccess; //briefly allow invalid access to put userdict in systemdict (per PLRM)

    int sysdict_unlocked; /**< systemdict is temporarily writeable while the
                            graphics language loads into it; the error handler
                            relocks it if a load faults */
    int sysdict_load_done; /**< the graphics language has been loaded into
                             systemdict; the one-shot unlock is spent */
    int device_made; /**< the device this run was started with has been
                        made. Read beside the flag above to decide
                        whether a job has to bring the context up first:
                        the language may stand without the device having
                        been made, which is where a context whose
                        language arrived whole begins */

    int es_over;              /**< the exec-stack ceiling has been reported;
                                   holds off a re-raise until depth recedes */
    int os_over;              /**< likewise for the operand stack */
    int ds_over;              /**< likewise for the dictionary stack */
    int onerr_run;            /**< consecutive errors handled without the run
                                   reaching `stop`; a runaway error cascade
                                   (an error raised from within the error
                                   machinery itself) drives this without bound */
    int scanner_defer; /**< the token just scanned is a brace procedure:
                            the interpreter pushes it as data rather than
                            executing it. A binary object sequence also
                            scans to an executable array but executes. */
    int scan_proc_depth; /**< the scanner's live brace-procedure nesting,
                              bounded against C-stack exhaustion; zero
                              between scans */

    size_t (*stdout_fn)(void *, const char *, size_t); /**< divert %stdout text */
    void *stdout_user;
    size_t (*stderr_fn)(void *, const char *, size_t); /**< divert %stderr text */
    void *stderr_user;

    char run_error_name[48];  /**< error that ended the last run ("" if none) */
    char run_error_info[128]; /**< errorinfo detail for the same ("" if none) */
    int run_uncaught;         /**< an error unwound past every stopped context */

    /** The matched paths of each filenameforall now running. A set of
        matched paths is a host allocation and not virtual memory, so the
        object the enumeration leaves on the execution stack carries the
        number its paths are held under here rather than their address.
        Enumerations nest, so there is a slot for each one running; a slot
        is given back as its enumeration ends, however it ends, and any
        still held when the context goes are released with it. */
    void **globs;
    unsigned int globs_size;


    unsigned int es_run_base; /**< exec-stack depth at xpost_run entry;
                                    a completed run is truncated back to
                                    this depth so its scheduling frames
                                    cannot accumulate across jobs */
    int skip_graphics; /**< run the interpreter lockdown (.finalize) without
                            loading graphics; xpost_run selects the no-graphics
                            start procedures so a program that needs no graphics
                            never pays to load them, and the no-graphics lockdown
                            path is exercised */

    int batch; /**< the host has said this invocation is not a session with a
                    user: a run over a named program ends where the program
                    ends, and control is never offered to the interactive
                    executive. Left clear, a run over a named program offers
                    the executive when standard input is a terminal and ends
                    where the program ends when it is not */

    int job_snapshots; /**< take VM snapshots around each xpost_run job
                            (restored on the quit path); disable for a
                            persistent context serving many runs that
                            deliberately share state (the definitions of one
                            job are meant to reach the next). With it on --
                            the default and the isolation contract -- each
                            job is encapsulated per PLRM 3.7.7: the boundary
                            reverts both VM banks and resets the interpreter
                            state that lives outside VM, so no job can leave
                            anything a later job sees. */

    /** The job-encapsulation boundary (PLRM 3.7.7). The two images are the
        fixed post-prelude baseline of both VM banks, captured once when the
        language (and any server-level prelude) has loaded; the scalars are
        the baseline of the per-job interpreter state that does not live in
        virtual memory. Each job reverts to this baseline in C, after the
        job's execution is over and outside any code the job can reach: the
        two banks by whole-VM image restore -- total (every byte of both
        arenas, so strings and stack contents revert with the objects),
        infallible (a copy back into storage the file already owns), and
        leak-free (the arena cursors reset to the baseline rather than
        accumulating a save level per job) -- and the scalars by
        assignment. A job under XPOST_SHOWPAGE_RETURN spans several xpost_run
        calls (it yields at each showpage), so the boundary reverts when the
        job completes, not when a call returns. job_baseline holds the two
        images; job_boundary_failed says a revert could not run and the
        context must be destroyed rather than reused. */
    struct Xpost_Memory_Image *job_baseline_lo;
    struct Xpost_Memory_Image *job_baseline_gl;
    dword job_rand_next;
    unsigned int job_vmmode;
    int job_packing;
    unsigned int job_baseline_ds; /**< dict-stack depth at baseline capture,
                                       the depth startjob resets the stack to */
    int job_boundary_failed;

    /** Job encapsulation (PLRM 3.7.7 startjob/exitserver). A run is
        encapsulated by default: at its end the boundary reverts it to the
        baseline. A run that executes `true password startjob` or exitserver
        becomes unencapsulated -- at its end the boundary folds its state
        into the baseline instead, so its definitions persist into the jobs
        that follow. `false password startjob` returns to encapsulated,
        folding the unencapsulated work done so far into the baseline first
        (the prolog/script idiom). Reset to encapsulated at the start of
        every run. */
    int job_encapsulated;

    /** Treat a run's embedder-supplied input stream as a Control-D-framed
        job-server channel (PLRM 3.7.7): a Control-D read from it ends the
        job being read and the next begins, all within one xpost_run. Off by
        default; set by xpost_jobserver_set(). Needs job_snapshots, which
        encapsulates the jobs the delimiter separates. */
    int jobserver;

    /** The StartJobPassword (PLRM C.3.1). Empty (the factory default and
        the initial value here) disables the check, so a trusted prolog can
        use exitserver out of the box; a host serving untrusted jobs sets it
        non-empty with xpost_startjob_password_set() to lock the door, since
        a program cannot set it (setsystemparams is a no-op). */
    char startjob_password[128];

    int (*xpost_interpreter_cid_init)(unsigned int *cid);
    Xpost_Memory_File *(*xpost_interpreter_alloc_local_memory)(void);
    Xpost_Memory_File *(*xpost_interpreter_alloc_global_memory)(void);
    XPOST_MUST_CHECK int (*garbage_collect_function)(Xpost_Memory_File *mem,
                                                     int dosweep,
                                                     int markall);
};

int xpost_context_init_ctxlist(Xpost_Memory_File *mem);
int xpost_context_append_ctxlist(Xpost_Memory_File *mem, unsigned cid);

/**
 * @brief initialize the context structure
 */
int xpost_context_init(Xpost_Context *ctx,
                       int (*xpost_interpreter_cid_init)(unsigned int *cid),
                       Xpost_Context *(*xpost_interpreter_cid_get_context)(unsigned int cid),
                       int (*xpost_interpreter_get_initializing)(void),
                       void (*xpost_interpreter_set_initializing)(int),
                       Xpost_Memory_File *(*xpost_interpreter_alloc_local_memory)(void),
                       Xpost_Memory_File *(*xpost_interpreter_alloc_global_memory)(void),
                       int (*garbage_collect_function)(Xpost_Memory_File *mem, int dosweep, int markall));

/**
 * @brief destroy the context structure, and all components
 */
void xpost_context_exit(Xpost_Context *ctx);

/**
 * @brief what this run settled under the given name
 *
 * The interpreter's dictionaries hold the language, which is the same for
 * every run of this build. What is not -- where this run found its boot
 * files, which directories a resource search covers, whether there is a
 * user at the other end of standard input, where a page goes -- is
 * settled by the host on every launch and kept apart from the language,
 * in .hostdict inside the private global namespace this context roots.
 * This is how C reads one.
 *
 * Answers a null for a setting the host had nothing to say about, which
 * is what the interpreter writes for one, and for a name that is no
 * setting at all: a caller reads a setting it knows the interpreter
 * records, and tests/host_settings.golden is what holds it to the set
 * that exists.
 */
Xpost_Object xpost_context_host_setting(Xpost_Context *ctx, const char *name);

/**
 * @brief hold the matched paths of a filenameforall, and report the
 *        number they are held under.
 *
 * The number is what the enumeration's object carries; it is never zero,
 * so an object that carries none is told from one that does. Returns 0
 * where the number could not be issued.
 */
int xpost_context_glob_hold(Xpost_Context *ctx,
                            void *glob,
                            unsigned int *id);

/**
 * @brief the matched paths held under @p id, or NULL where none are.
 */
void *xpost_context_glob_held(Xpost_Context *ctx, unsigned int id);

/**
 * @brief release the matched paths held under @p id, and the number with
 *        them. Does nothing where none are held under it.
 */
void xpost_context_glob_release(Xpost_Context *ctx, unsigned int id);

/**
 * @brief utility function for extracting from the context
 *        the mfile relevant to an object
 */
/*@dependent@*/
XPOST_TEST_VISIBLE Xpost_Memory_File *xpost_context_select_memory(Xpost_Context *ctx, Xpost_Object o);

/**
 * @brief print a dump of the context structure data to stdout
 */
void xpost_context_dump(Xpost_Context *ctx);

/**
 * @brief install a function to be called by eval()
 */
int xpost_context_install_event_handler(Xpost_Context *ctx,
                                        Xpost_Object operator,
                                        Xpost_Object device);

/**
 * @brief fork new process with shared global and shared local vm (lightweight process)
 *
 * The memory files are shared because a context's name tables, operator
 * table and systemdict are built by the interpreter above this module: a
 * fork given memory files of its own would declare those entities
 * present and have none.
 */
unsigned int xpost_context_fork3(Xpost_Context *ctx,
                                 int (*xpost_interpreter_cid_init)(unsigned int *cid),
                                 Xpost_Context *(*xpost_interpreter_cid_get_context)(unsigned int cid),
                                 Xpost_Memory_File *(*xpost_interpreter_alloc_local_memory)(void),
                                 Xpost_Memory_File *(*xpost_interpreter_alloc_global_memory)(void),
                                 int (*garbage_collect_function)(Xpost_Memory_File *mem, int dosweep, int markall));

/**
 * @}
 */

#endif
