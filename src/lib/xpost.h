/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013-2016 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_H
#define XPOST_H

/**
 * @def XPAPI
 * @brief Marks a name the shared library exports.
 *
 * What it expands to is the host's way of saying so: an export or an
 * import declaration where the library is a DLL, default visibility
 * where the compiler hides symbols by default, and nothing where neither
 * applies. A declaration in this header carries it; one that does not is
 * not part of the library's surface.
 */
#ifdef XPAPI
# undef XPAPI
#endif

#ifdef _WIN32
# ifdef XPOST_BUILD
#  ifdef DLL_EXPORT
#   define XPAPI __declspec(dllexport)
#  else
#   define XPAPI
#  endif
# else
#  define XPAPI __declspec(dllimport)
# endif
#else
# ifdef __GNUC__
#  if __GNUC__ >= 4
#   define XPAPI __attribute__ ((visibility("default")))
#  else
#   define XPAPI
#  endif
# else
#  define XPAPI
# endif
#endif

#include <stdlib.h> /* for size_t */

#ifdef __cplusplus
extern "C" {
#endif /* ifdef __cplusplus */


/**
 * @file xpost.h
 * @brief This file provides the Xpost API functions.
 *
 * This is the master "include" file which includes
 * all headers in the proper order needed to control
 * xpost features at the top level.
 * @defgroup xpost_library Library functions
 *
 * @{
 */

/**
 * @brief Initialize the xpost library.
 *
 * @return The new init count. Will be 0 if initialization failed.
 *
 * The first time this function is called, it will perform all the internal
 * initialization required for the library to function properly and increment
 * the initialization counter. Any subsequent call only increment this counter
 * and return its new value, so it's safe to call this function more than once.
 *
 * @see xpost_quit();
 */
XPAPI int xpost_init(void);

/**
 * @brief Quit the xpost library.
 *
 * @return The new init count.
 *
 * If xpost_init() was called more than once for the running application,
 * xpost_quit() will decrement the initialization counter and return its
 * new value, without doing anything else. When the counter reaches 0, all
 * of the internal elements will be shutdown and any memory used freed.
 *
 * @see xpost_init()
 */
XPAPI int xpost_quit(void);

/**
 * @brief Request a PostScript-level interrupt.
 *
 * Raises the interrupt error at the interpreter's next evaluation
 * step, as the language specifies for an external interrupt request
 * such as control-C. Only a flag is set, so this function is safe to
 * call from a signal handler.
 */
XPAPI void xpost_interrupt(void);

/**
 * @brief Retrieve the version of the library.
 *
 * @param[out] maj The major version.
 * @param[out] min The minor version.
 * @param[out] mic The micro version.
 *
 * This function stores the major, minor and micro version of the library
 * respectively in the buffers @p maj, @p min and @p mic. @p maj, @p min
 * and @p mic can be @c NULL.
 */
XPAPI void xpost_version_get(int *maj, int *min, int *mic);

/**
 * @brief Return the path of the shared library.
 *
 * @return The path of the shared library.
 *
 * This function returns the path of the shared library.
 */
XPAPI const char *xpost_lib_dir_get(void);

/**
 * @brief Return the path of the data directory, based on the path of the
 * shared library.
 *
 * @return The path of the data directory.
 *
 * This function returns the path of the data directory, based on the shared library. More precisely, it is xpost_lib_path_get()../share/xpost.
 */
XPAPI const char *xpost_data_dir_get(void);

/**
 * @typedef Xpost_Context
 * @brief The context abstract structure for a thread of execution of ps code.
 */
typedef struct _Xpost_Context Xpost_Context;

/**
 * @typedef Xpost_Showpage_Semantics
 * @brief Specify the behavior the interpreter should take when executing `showpage`.
 */
typedef enum {
    XPOST_SHOWPAGE_DEFAULT, /**< Pause for whoever is watching: print
                                 "----showpage----" to stdout and read
                                 and discard a line of text from stdin
                                 (ie. wait for return). Both are done
                                 only where the run has somebody to do
                                 them for -- stdin a terminal, and no
                                 output filename given -- so a run under
                                 a script neither writes the marker to
                                 the output it is sharing with the
                                 program nor takes a line of what stdin
                                 was carrying. The page is transmitted
                                 and erased either way. */
    XPOST_SHOWPAGE_NOPAUSE, /**< Bypasses this action but still
                                 performs a "flush" of the graphics
                                 device. */
    XPOST_SHOWPAGE_RETURN /**< Causes the interpreter to return
                               control to its caller; the suspended
                               context may be resumed by calling
                               xpost_run with the #XPOST_INPUT_RESUME
                               input type. */
} Xpost_Showpage_Semantics;

/**
 * @typedef Xpost_Output_Type
 * @brief Specify the interpretation of the outputptr parameter to xpost_create().
 */
typedef enum {
    XPOST_OUTPUT_DEFAULT, /**< Ignores outputptr. */
    XPOST_OUTPUT_FILENAME, /**< Treats outputptr as a char* to a
                                zero-terminated OS path string
                                (implemented in pgm and ppm devices). */
    XPOST_OUTPUT_BUFFERIN, /**< Treats outputptr as an unsigned char *
                                and renders directly into this memory.
                                Implemented by the raster device, which
                                keeps its page extent in front of the
                                page and so needs room for that as well;
                                every other device allocates a page of
                                its own and leaves this memory alone. The
                                memory stays the caller's throughout: it
                                is not given back with
                                xpost_output_buffer_release(), which has
                                nothing to give back for a page the
                                library did not allocate. */
    XPOST_OUTPUT_BUFFEROUT /**< Treats outputptr as an unsigned char **
                                and assigns a new buffer to the
                                unsigned char * which outputptr points
                                to. The buffer is given back with
                                xpost_output_buffer_release(). */
} Xpost_Output_Type;

/**
 * @typedef Xpost_Input_Type
 * @brief Specify the interpretation of the inputptr parameter to xpost_run().
 */
typedef enum {
    XPOST_INPUT_STRING, /**< Treats inputptr as a char * to an
                             zero-terminated ascii string, writes the
                             whole string into a temporary file and
                             falls through to the #XPOST_INPUT_FILEPTR
                             case. */
    XPOST_INPUT_FILENAME, /**< Treats inputptr as a char * to a
                              zero-terminated OS path string, and
                              pushes the path string itself,
                              scheduling a procedure to execute it. */
    XPOST_INPUT_FILEPTR, /**< Treats inputptr as a FILE *, creates a
                               postscript file object and pushes it on
                               the execution stack (scheduling it to
                               execute). */
    XPOST_INPUT_RESUME /**< Bypasses any execution scheduling. */
} Xpost_Input_Type;

/**
 * @typedef Xpost_Set_Size
 * @brief Whether a caller-supplied start page size is used, or ignored in
 *        favour of the default of US Letter (612x792 points).
 */
typedef enum {
    XPOST_IGNORE_SIZE,
    XPOST_USE_SIZE
} Xpost_Set_Size;

/**
 * @typedef Xpost_Output_Message
 * @brief Specify the kind of messages that the interpreter displays to output.
 */
typedef enum
{
    XPOST_OUTPUT_MESSAGE_QUIET, /**< Suppress interpreter messages. */
    XPOST_OUTPUT_MESSAGE_VERBOSE, /**< Display some interpreter messages. */
    XPOST_OUTPUT_MESSAGE_TRACING /**< Display all interpreter messages and fill xdump* file. */
} Xpost_Output_Message;

/**
 * @brief Create a newly allocated context.
 *
 * @param device The device to paint into, by name; the names this build
 *               has are what @c --help lists.
 * @param output_type How @p outputptr is to be read.
 * @param outputptr What @p output_type names: a file name, a buffer, or
 *                  nothing.
 * @param semantics What @c showpage does at the end of a page.
 * @param output_msg Which of the interpreter's own messages reach the
 *                   output.
 * @param set_size Whether @p width and @p height are used or ignored.
 * @param width The width of the context page.
 * @param height The height of the context page.
 *
 * @return The interpreter's context, or @c NULL on failure.
 *
 * This function creates a #Xpost_Context with the given parameters,
 * bringing up the one interpreter instance the process may hold and
 * returning the context that instance runs.
 *
 * The instance is single: an interpreter's multiple execution contexts
 * live in its own context table, so a second call to this function while
 * an instance is live returns @c NULL rather than replacing the live
 * instance under a context the caller still holds. Sequential use is
 * unrestricted -- once xpost_destroy() has ended an instance, this
 * function creates another.
 *
 * When not needed the context must be freed with xpost_destroy().
 *
 * @see xpost_destroy()
 */
XPAPI Xpost_Context *xpost_create(const char *device,
                                  Xpost_Output_Type output_type,
                                  const void *outputptr,
                                  Xpost_Showpage_Semantics semantics,
                                  Xpost_Output_Message output_msg,
                                  Xpost_Set_Size set_size,
                                  int width,
                                  int height);

/**
 * @brief Add extra definitions to userdict
 *
 * @param ctx The context to use.
 * @param cnt The number of elements in the @p defs array
 * @param defs An argv-style array of pointers to "key=value" strings.
 *
 * This function allows extra defined key/value pairs to be
 * added to userdict after a context is created using xpost_create,
 * presumably before calling xpost_run.
 *
 * Definitions may be used by the ps program. Control information for
 * a device is not supplied this way: a device is configured through
 * the keys of a setpagedevice request, or through the defaults an
 * embedder records with xpost_dev_png_options_set() and
 * xpost_dev_jpeg_options_set().
 */
XPAPI int xpost_add_definitions(Xpost_Context *ctx,
                                int cnt,
                                char *defs[]);

/**
 * @brief Run a context's programs without loading graphics.
 *
 * By default each xpost_run() job loads the graphics modules before
 * running. When enabled, xpost_run() selects the no-graphics start
 * procedures: the interpreter is still locked down (the language
 * relocates into systemdict and the private namespaces are sealed),
 * but the graphics modules are never loaded. Use for a program that
 * needs no graphics, or to exercise the no-graphics lockdown path.
 */
XPAPI void xpost_skip_graphics_set(Xpost_Context *ctx, int enable);

/**
 * @brief Build the language rather than read it out of an image.
 *
 * Where the environment names an image of virtual memory,
 * xpost_create() reads the language out of it instead of running the
 * boot files, and the context comes up with the language the image was
 * written with. That is decided before a caller has said anything about
 * what language it wants, so a caller that wants another one -- one
 * without graphics -- says so here, before creating the context.
 *
 * There is no way back: a process that has said this builds the
 * language for the rest of its life.
 */
XPAPI void xpost_vm_image_refuse(void);

/**
 * @brief Say which options that change the language are in force.
 *
 * Some options change the language a context comes up with rather than
 * what a run does with it: without graphics the boot files build a
 * smaller language, and with Display PostScript they build a larger one.
 * An image carries the language it was written with, so a run wanting
 * one of those must not read an image of another.
 *
 * The image is read as the context is created, before the context can be
 * asked what it wants, so this is said first -- the way
 * xpost_vm_image_refuse() is, and for the same reason. What it names is
 * written into the image and compared on the way back, so each
 * configuration keeps its own image rather than none of them having one.
 *
 * @param[in] mask a bitwise or of XPOST_VM_IMAGE_CONFIG_* values.
 */
XPAPI void xpost_vm_image_config_set(unsigned int mask);

/** @brief The boot files build a language without graphics. */
#define XPOST_VM_IMAGE_CONFIG_NO_GRAPHICS 1u
/** @brief The Display PostScript context operators are installed. */
#define XPOST_VM_IMAGE_CONFIG_DPS         2u

/**
 * @brief Choose where a retained page's marks are held.
 *
 * @param state "auto", "never" or "always"
 * @return 1, or 0 where the word is none of the three
 *
 * A page too large for the band budget is held as the marks that made it
 * rather than as its pixels, and those marks have to be kept somewhere.
 * By default they are weighed: while they come to less than the raster
 * that banding the page saves they stay in memory, and past that they go
 * into a scratch file, which is what holds what a page costs to a bound
 * rather than to its drawing.
 *
 * "never" keeps them in memory whatever they come to. It is the one
 * state in which what a page costs follows its drawing without limit,
 * and it is here because a caller may have nowhere to write, or may
 * prefer to spend memory than to touch a disk at all. Nothing else ever
 * selects it: a scratch directory that refuses a file does not demote a
 * run to it, because a caller that did not ask for unbounded memory must
 * not be given it quietly.
 *
 * "always" puts them in a file from the first mark, and a run that asks
 * for it where no scratch file can be made is refused when its device is
 * made.
 *
 * Asked before xpost_create, which reads it. A word that is none of the
 * three changes nothing.
 */
XPAPI int xpost_record_spill_set(const char *state);

/**
 * @brief The largest budget a band of a page may be given.
 *
 * The budget is divided by what one row of the raster costs to say how
 * deep a band is, and that division is done in the interpreter's own
 * integers, the narrower of which counts to this. It is the same number
 * at both object widths so that one invocation is taken the same way by
 * either build.
 */
#define XPOST_BAND_BYTES_MAX 2147483647L

/**
 * @brief Choose what one band of a page may cost.
 *
 * @param bytes the raster a device may hold at once, in bytes
 * @return 1, or 0 where the count is outside 1 to
 *         @ref XPOST_BAND_BYTES_MAX
 *
 * A device whose page may arrive a band at a time holds this many bytes
 * of raster at once and as many rows of the page as that buys. It is
 * also what a page is weighed against to decide whether it is held that
 * way at all: a page the budget covers arrives in one band, which is the
 * page, so it is painted directly and no mark is written down. So this
 * one number says both which pages are held as their marks and how much
 * of such a page is in memory at once, and lowering it holds a smaller
 * page in bands.
 *
 * It bounds the marks as well as the raster. A page held as its marks
 * puts them in a scratch file once they come to more than this, so what
 * such a page costs is a band of raster and a budget's worth of marks
 * whatever the drawing (see xpost_record_spill_set, which says where
 * those marks may go).
 *
 * A budget that cannot buy one row of the page in hand is refused when
 * the device that would hold it is made, rather than quietly taken as
 * one row.
 *
 * PLRM Appendix G declares the band-device setup operators obsolete and
 * bars them from a page description, so a program may not choose how its
 * marks are held. This is settled by whoever starts the run, and
 * currentsystemparams reports it as MaxBandBytes.
 *
 * Asked before xpost_create, which reads it. A count outside the range
 * changes nothing.
 */
XPAPI int xpost_band_bytes_set(long bytes);

/**
 * @brief Install the Display PostScript multiple-execution-context operators.
 *
 * @param enable Nonzero to install them, zero (the default) to leave them out.
 *
 * fork, join, yield, detach and currentcontext are Display PostScript
 * operators (PLRM 2nd ed 7.1), not standard base PostScript, and the
 * cooperative scheduler that gives them meaning is not yet driven. They are
 * installed only when a run asks for them; a default run leaves the names
 * undefined, so a program reaching for one gets undefined.
 *
 * Asked before xpost_create, which reads it. The xpost binary exposes it as
 * --enable-dps.
 */
XPAPI void xpost_dps_set(int enable);

/**
 * @brief The devices whose page may arrive a band at a time.
 *
 * Selecting one of these selects banding, and a colon after its name may
 * then say which of the two ways such a page is held. Every other device
 * holds the page whole and takes no such word.
 *
 * Written as a list a caller expands, because more than one thing is
 * made from it and none of them may be the one that is wrong: the
 * selection, which is settled before any boot file is read and so cannot
 * ask a device dictionary that does not exist yet, and the usage text,
 * which says which devices the mode words apply to. A sentence naming
 * them in prose beside the list is a second statement, and a device
 * added to the list does not reach it -- the help then describes a fleet
 * the program does not have, and nothing says so.
 *
 * Expand it by passing a macro taking one string:
 * @code
 * #define BAND_NAME(name) name,
 * static const char *const bands[] = { XPOST_BANDS_BY_DEFAULT(BAND_NAME) NULL };
 * #undef BAND_NAME
 * @endcode
 */
#define XPOST_BANDS_BY_DEFAULT(X) \
    X("pgm") X("ppm") X("pbm") X("tiff") X("png") X("jpeg")

/**
 * @brief Declare that this context serves no interactive user.
 *
 * A program named to xpost_run() as XPOST_INPUT_FILENAME is a job, and a
 * job ends where its program ends. When enabled, that is all a run does:
 * the run returns when the named program returns, whatever standard
 * input happens to be. Left disabled, a run over a named program offers
 * the interactive executive after the program when standard input is a
 * terminal, and returns as above when it is not.
 *
 * A program that wants a session after itself asks for one in the
 * language, with the executive operator, which is unaffected either way.
 */
XPAPI void xpost_batch_set(Xpost_Context *ctx, int enable);

/**
 * @brief Receives text output from the interpreter.
 *
 * @param user The pointer registered alongside the handler.
 * @param buf The bytes written by the program.
 * @param len The number of bytes.
 * @return The number of bytes accepted; a short count is an error.
 */
typedef size_t (*Xpost_Output_Fn)(void *user, const char *buf, size_t len);

/**
 * @brief Divert the program's standard-output text to a handler.
 *
 * Everything a program writes to its standard output -- print, = and
 * writes to the %stdout file -- is passed to @p fn instead of the
 * process's stdout. Device output (files, buffers) is not affected.
 * Pass NULL to restore the default.
 */
XPAPI void xpost_stdout_handler_set(Xpost_Context *ctx,
                                    Xpost_Output_Fn fn,
                                    void *user);

/**
 * @brief Divert the program's standard-error text to a handler.
 *
 * As xpost_stdout_handler_set(), for writes to the %stderr file.
 */
XPAPI void xpost_stderr_handler_set(Xpost_Context *ctx,
                                    Xpost_Output_Fn fn,
                                    void *user);

/**
 * @brief Enable or disable per-job VM isolation for a context.
 *
 * On by default and the isolation contract: each xpost_run() job is
 * encapsulated (PLRM 3.7.7). When a job's execution is over, the context
 * reverts to a fixed baseline -- both VM banks, and the interpreter state
 * outside VM -- so nothing one job did reaches a later job. The revert is
 * a whole-VM image restore: total (strings and stack contents revert with
 * the objects), infallible (a copy that allocates nothing), and leak-free
 * (the bank cursors return to the baseline, so serving many jobs does not
 * accumulate). The baseline is established once, by the first run (which
 * loads the language and any prelude the run installs). Disable this only
 * for a context whose jobs are meant to share state (a REPL, or a prelude
 * loaded across several runs before xpost_job_baseline_set()).
 */
XPAPI void xpost_job_snapshots_set(Xpost_Context *ctx, int enable);

/**
 * @brief Treat a run's input stream as a Control-D-framed job-server channel.
 *
 * Off by default. When on -- together with per-job isolation, which it needs
 * -- an embedder-supplied input stream (xpost_run() with XPOST_INPUT_STRING
 * or XPOST_INPUT_FILEPTR) may carry more than one job, framed by the
 * Control-D (0x04) end-of-file the serial job-server protocol uses (PLRM
 * 3.7.7). Each Control-D ends the job being read and the next begins,
 * reverting to the baseline in between, all within the one xpost_run() call:
 * the reader is held outside the virtual memory the boundary reverts, so a
 * job boundary can be taken in mid-run. A named file (XPOST_INPUT_FILENAME)
 * is one job whatever it holds, and a Control-D in a file a program opens or
 * in a nested read is an ordinary byte -- the protocol frames the outermost
 * channel, and is not part of the language.
 */
XPAPI void xpost_jobserver_set(Xpost_Context *ctx, int enable);

/**
 * @brief Set the current state as the baseline every later job reverts to.
 *
 * The job boundary reverts to a fixed baseline captured once, ordinarily
 * by the first run. An embedder that installs a server-level prelude over
 * several runs (with isolation disabled) calls this once the prelude is in
 * place, so that jobs revert to the post-prelude state rather than the
 * bare initial one. This is the persistent equivalent of exitserver /
 * `true password startjob`: it folds the current state into the baseline.
 * The operand and scratch stacks are cleared so the baseline carries the
 * empty operand stack a job begins from.
 */
XPAPI void xpost_job_baseline_set(Xpost_Context *ctx);

/**
 * @brief Revert the context to the baseline now, readying a fresh job.
 *
 * The host equivalent of a job delimiter (`false password startjob`, or a
 * job-server channel's end-of-job): it reverts the whole context to the
 * baseline -- both VM banks, strings, stacks, and the non-VM per-job state
 * -- without tearing the context down and rebuilding it, and without
 * accumulating anything. Use it to force the boundary around a piece of
 * untrusted input that is not itself a complete xpost_run() job. If no
 * baseline has been established yet, the current state becomes it. Returns
 * 1 on success, 0 if the revert could not complete (in which case the
 * context is no longer safe to reuse and must be destroyed).
 */
XPAPI int xpost_new_job(Xpost_Context *ctx);

/**
 * @brief Set the StartJobPassword for startjob and exitserver.
 *
 * PLRM C.3.1: the password guards the door a job uses to make its changes
 * persist (exitserver, `true password startjob`). It is empty by default,
 * which -- per the spec's factory default -- disables the check, so a
 * trusted prolog can persist definitions without ceremony. A host serving
 * untrusted jobs sets it non-empty here to lock the door: a program cannot
 * change it (setsystemparams is a no-op), so only a caller with this
 * function can. Passing NULL or "" reopens the door.
 */
XPAPI void xpost_startjob_password_set(Xpost_Context *ctx, const char *password);

/**
 * @brief Set the SystemParamsPassword (PLRM C.3.1).
 *
 * Decides which kind of unencapsulated job startjob starts. Presenting
 * this password starts a system administrator job, which may change an
 * implementation limit; presenting the start job password starts an
 * ordinary unencapsulated job, which may alter initial VM and may not.
 *
 * Empty is the factory default and collapses the distinction: every
 * startjob then starts an administrator job. A host serving jobs from
 * more than one submitter sets both this and the start job password.
 */
XPAPI void xpost_system_params_password_set(Xpost_Context *ctx,
                                           const char *password);

/**
 * @brief File-access sandbox: permit directory trees, then engage.
 *
 * Before the sandbox is engaged, a program's disk access is
 * unrestricted. xpost_path_permit_read() (xpost_path_permit_write())
 * grants reading (writing) of files within a directory tree;
 * xpost_path_control_engage() then denies every other disk open by the
 * running program. Engaging is process-wide and one-way -- it cannot be
 * reversed and the permit set is frozen -- so configure the permitted
 * directories first and engage before running untrusted input.
 * Resource-file loading is separately confined and is unaffected.
 *
 * The permit functions answer whether the directory is permitted
 * afterwards: 1 when it is, including when the permitted set already
 * covers it -- asking again for the same tree costs nothing and may be
 * done as often as is convenient -- and 0 when the set does not cover it
 * and cannot be extended to, because the directory does not resolve, the
 * sandbox is engaged, or the table (64 entries) is full. A refusal is
 * also reported on the error log.
 *
 * The sandbox belongs to the process, not to a context. Every context
 * the process creates is confined by the same latch and reaches the same
 * directories, so one created after the sandbox is engaged finds it
 * engaged and finds what was permitted before it existed. This confines
 * the process against the program it runs; it does not divide one job in
 * the process from another.
 *
 * This is defence in depth: it complements, and does not replace,
 * operating-system confinement of the host process.
 */
XPAPI int xpost_path_permit_read(const char *dir);

/**
 * @brief Permit writing within a directory tree.
 *
 * The counterpart to xpost_path_permit_read(), whose description above
 * covers both: when the permit set is frozen, what it answers, and why
 * asking twice for the same tree costs nothing.
 *
 * @param[in] dir The directory whose tree may be written.
 * @return 1 where the tree is permitted afterwards, 0 where it is not
 *         and cannot be.
 */
XPAPI int xpost_path_permit_write(const char *dir);

/**
 * @brief Permit writing one file rather than a tree.
 *
 * For a host that knows the single path its job may write and does not
 * want to open the directory around it. Answers as the tree permits do.
 *
 * @param[in] path The file that may be written.
 * @return 1 where the file is permitted afterwards, 0 where it is not
 *         and cannot be.
 */
XPAPI int xpost_path_permit_write_file(const char *path);

/**
 * @brief Engage the sandbox: deny every disk open not already permitted.
 *
 * Process-wide and one-way. What a host calls when it has permitted what
 * the job needs; xpost_lockdown() does this and takes the configuring
 * operators away as well.
 */
XPAPI void xpost_path_control_engage(void);

/**
 * @brief Engage the sandbox and retire the operators that configure it.
 *
 * What a host should call once it has permitted what the job needs and is
 * about to run input it does not trust. It engages the latch as
 * xpost_path_control_engage() does, and additionally removes
 * .permitfileread, .permitfilewrite, .lockdown and .resourcefileopen from
 * @p ctx's systemdict, so the program cannot name the operators that
 * configure its own confinement. Enforcement does not rest on their
 * absence -- the permit check is in C and applies whether or not a name
 * reaches it -- so this is the second line and not the first.
 *
 * The latch is the process's and the names are the context's, so a
 * context created afterwards finds the latch engaged and finds the names
 * still in its own systemdict; call this for each context that is to run
 * untrusted input.
 */
XPAPI void xpost_lockdown(Xpost_Context *ctx);

/**
 * @brief Append a directory to the resource search path.
 *
 * findresource searches these directories, in the order added, when a
 * resource is not already defined in virtual memory. Add directories
 * before running the program that resolves resources. Returns 1 on
 * success, 0 on failure.
 */
XPAPI int xpost_add_resource_dir(Xpost_Context *ctx, const char *dir);

/**
 * @brief Outcome of executing a program with xpost_run().
 *
 * A context that reports #XPOST_RUN_COMPLETE, #XPOST_RUN_YIELDED or
 * #XPOST_RUN_ERRORED remains usable for further runs; after
 * #XPOST_RUN_FAILED it must be destroyed. When a run reports
 * #XPOST_RUN_ERRORED, xpost_error_name_get() identifies the
 * PostScript error that ended it.
 */
typedef enum {
    XPOST_RUN_COMPLETE = 0, /**< the program ran to completion */
    XPOST_RUN_YIELDED,      /**< showpage returned control to the caller
                                 (#XPOST_SHOWPAGE_RETURN); pass
                                 #XPOST_INPUT_RESUME to continue */
    XPOST_RUN_ERRORED,      /**< an uncaught PostScript error ended the
                                 program; the context has been tidied
                                 and accepts further runs */
    XPOST_RUN_FAILED        /**< the run could not be scheduled or the
                                 interpreter is no longer coherent */
} Xpost_Run_Status;

/**
 * @brief Name of the PostScript error that ended the last run.
 *
 * Valid after xpost_run() returns #XPOST_RUN_ERRORED, until the next
 * run on the same context; the empty string otherwise. The name is the
 * standard error name, e.g. "typecheck" or "undefined".
 */
XPAPI const char *xpost_error_name_get(Xpost_Context *ctx);

/**
 * @brief Additional information for the error that ended the last run.
 *
 * The errorinfo detail recorded alongside the error, when the program
 * supplied one; the empty string otherwise.
 */
XPAPI const char *xpost_error_info_get(Xpost_Context *ctx);

/**
 * @brief Execute ps program.
 *
 * @param ctx The context to run.
 * @param input_type The input type to use.
 * @param inputptr The pointer passed to the interpreter.
 * @param size The size of the memory passed to the interpreter.
 * @return The run's outcome as an #Xpost_Run_Status.
 *
 * This function executes a ps program until quit, fall-through to quit,
 * #XPOST_SHOWPAGE_RETURN semantic, or error (default action: message,
 * purge and quit).
 *
 * Depending upon @p input_type, this function will package the input
 * into an appropriate postscript object and schedule it for execution
 * by marking it executable and pushing to the exec stack, or by
 * pushing to the operand stack and pushing to the exec stack a small
 * program to execute it.
 *
 * The parameter @p size is used when @p input_type is
 * #XPOST_INPUT_STRING. If @p inputptr is a nul terminated string, 0
 * can be passed and the default size will be the length of the
 * string. If @p inputptr is a piece of memory, then pass the size of
 * that memory.
 *
 * For a filename, push a proc to open and execute it.
 *
 * For a string, write to a temp file and fall-through to FILE * case.
 *
 * For a FILE *, mark executable and push to exec stack.
 *
 * As a special-case, if executing a FILE *, and that file is a
 * console or tty, it pushes a proc which launches the postscript
 * `executive` which offers PS> prompts.
 *
 * If an output device (such as a window) has been specified in the
 * call to xpost_create(), it is here in the startup code, where
 * the device is initialized. The device is specified in xpost_create,
 * not because it is needed at that point, but because it is considered
 * a constant for the context, whereas it is intended that a context
 * may be re-used by calling xpost_run upon it again, presumably with
 * differing arguments.
 *
 * @see #Xpost_Input_Type
 */
XPAPI Xpost_Run_Status xpost_run(Xpost_Context *ctx,
                    Xpost_Input_Type input_type,
                    const void *inputptr,
                    size_t size);

/**
 * @brief Destroy the given context.
 *
 * @param ctx The context to destroy.
 *
 * This function destroys the context @p ctx which has been created with
 * xpost_create(), and with it the interpreter instance holding it.
 * Nothing of the instance outlives the call, so xpost_create() may be
 * called again afterwards to obtain another.
 *
 * A @c NULL @p ctx, or a pointer that is not the context xpost_create()
 * returned for the live instance, is declined and nothing is destroyed.
 *
 * @see xpost_create()
 */
XPAPI void xpost_destroy(Xpost_Context *ctx);

/**
 * @brief Give back a page buffer a run handed over.
 *
 * @param buffer The address a #XPOST_OUTPUT_BUFFEROUT run was given as
 *        its outputptr, holding the buffer that run stored there.
 *
 * A run started with #XPOST_OUTPUT_BUFFEROUT stores its finished page
 * through the address the caller gave xpost_create(), and the page is
 * the caller's from that moment. It is not part of the interpreter's
 * memory: xpost_destroy() leaves it alone and nothing the interpreter
 * does afterwards reaches it, so a caller may destroy the context and
 * read the pixels after. This call is how the buffer is given back.
 *
 * Pass the address xpost_create() was given, holding the pointer the run
 * stored there. The buffer is released and the pointer set to null, so a
 * second call on the same variable does nothing: a null @p buffer, and a
 * @p buffer holding null, are both accepted and do nothing. Any address
 * holding the pointer will do -- what the call reads is the pointer and
 * what it clears is the variable it was handed -- but a caller that
 * releases through a copy is left holding an original that no longer
 * names memory. An address holding a pointer this library did not hand
 * over is as undefined as passing such a pointer to free().
 *
 * Release once the context is done with the buffer. Every page of a run
 * is painted into the same buffer and the context paints into it until
 * it is destroyed, so the buffer is given back after xpost_destroy(), or
 * at least after the last run that paints a page.
 *
 * A caller that never releases leaks the buffer. Neither xpost_destroy()
 * nor xpost_quit() gives it back, since neither can know whether the
 * caller is still reading it; this call is the only one that does.
 *
 * How the memory was obtained, and how it is given back, is the
 * library's business and not part of this contract -- which is why the
 * buffer is released here rather than by the caller's own free().
 *
 * @see xpost_create()
 */
XPAPI void xpost_output_buffer_release(unsigned char **buffer);

/**
 * @brief Set the default quality for compression of JPEG files.
 *
 * @param ctx The context to use.
 * @param quality The quality value, between 0 and 100.
 *
 * This function records @p quality as the default under the
 * /jpeg_quality key of the JPEG device class, so every JPEG device
 * made in this context compresses at that quality. A program's own
 * page-device request naming /jpeg_quality still overrides it.
 * @p quality must be between 0 and 100. On error, nothing is done.
 */
XPAPI void
xpost_dev_jpeg_options_set(Xpost_Context *ctx,
                           int quality);

/**
 * @brief Set the default compression and interlacing of PNG files.
 *
 * @param ctx The context to use.
 * @param compression_level The compression level, between 0 and 9.
 * @param interlaced Whether the PNG file is interlaced or not.
 *
 * This function records @p compression_level and @p interlaced as the
 * defaults under the /png_compression_level and /png_interlaced keys
 * of the two PNG device classes (plain and alpha), so every PNG
 * device made in this context writes with them. A program's own
 * page-device request naming either key still overrides it.
 * @p compression_level must be between 0 and 9. On error, nothing is
 * done.
 */
XPAPI void
xpost_dev_png_options_set(Xpost_Context *ctx,
                          int compression_level,
                          int interlaced);

/**
 * @}
 */


#ifdef __cplusplus
}
#endif /* ifdef __cplusplus */

#endif
