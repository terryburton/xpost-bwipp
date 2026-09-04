/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * Copyright (c) 2013 Vincent Torri
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_FONT_H
#define XPOST_FONT_H

#ifndef XPOST_OBJECT_H
# error MUST #include "xpost_object.h" before this file
#endif

/**
 * @file xpost_font.h
 * @brief Font manipuation functions
 */

/**
 * @typedef Xpost_Font_Pixel_Mode
 * Describe the format of pixels in a given bitmap.
 *
 * corresponds directly to FreeType's FT_Pixel_Mode enum.
 * http://www.freetype.org/freetype2/docs/reference/ft2-basic_types.html#FT_Pixel_Mode
 */
typedef enum
{
    XPOST_FONT_PIXEL_MODE_NONE = 0, /**< Reserved */
    XPOST_FONT_PIXEL_MODE_MONO,     /**< Monochrome bitmap */
    XPOST_FONT_PIXEL_MODE_GRAY,     /**< 8-bit per pixel bitmap */
    XPOST_FONT_PIXEL_MODE_GRAY2,    /**< 2-bit per pixel bitmap */
    XPOST_FONT_PIXEL_MODE_GRAY4,    /**< 4-bit per pixel bitmap */
    XPOST_FONT_PIXEL_MODE_LCD,      /**< 8-bit per pixel bitmap (for LCD displays) */
    XPOST_FONT_PIXEL_MODE_LCD_V,    /**< 8-bit per pixel bitmap (for LCD displays) */
    XPOST_FONT_PIXEL_MODE_BGRA,     /**< 8-bit per pixel bitmap for colored fonts  with alpha channel */
    XPOST_FONT_PIXEL_MODE_MAX       /**< Reserved */

} Xpost_Font_Pixel_Mode;

/**
 * @brief Initialize the font module.
 *
 * @return 1 on success, 0 otherwise.
 *
 * This function initializes the font module. It is called by
 * xpost_init().
 *
 * @see xpost_font_quit()
 * @see xpost_init()
 */
int xpost_font_init(void);

/**
 * @brief Shut down the font module.
 *
 * This function shuts down the font module. It is called by
 * xpost_quit().
 *
 * @see xpost_font_init()
 * @see xpost_quit()
 */
void xpost_font_quit(void);

/**
 * @brief Return the font face from the given font name.
 *
 * @param[in] name The font name.
 * @return The font face.
 *
 * This function returns the font face of the font named @p name. On
 * error, it returns @c NULL.
 *
 * @see xpost_font_face_free()
 */
void *xpost_font_face_new_from_name(const char *name);

/**
 * @brief Return a font face from a font program held in memory.
 *
 * @param[in] data The font program bytes (TrueType/OpenType sfnt).
 * @param[in] len The number of bytes.
 * @return The font face, or @c NULL on error.
 *
 * The face reads the buffer where it lies rather than copying it, and
 * takes ownership of it: the buffer is freed with the face, or when the
 * font machinery goes down still holding it. The caller keeps ownership
 * only when this returns @c NULL.
 *
 * @see xpost_font_face_new_from_name()
 * @see xpost_font_face_free()
 */
void *xpost_font_face_new_from_memory(const unsigned char *data, size_t len);

/**
 * @brief Return bounding box from a font face.
 *
 */
void xpost_font_face_get_bbox(void *face, Xpost_Object *bboxarray, real em);

/**
 * @brief Free the given font.
 *
 * @param[in,out] face The font face.
 *
 * This function frees the memory stored by @p face.
 *
 * @see xpost_font_face_new_from_name()
 */
int xpost_font_face_units(void *face);
int xpost_font_face_is_truetype(void *face);
const char *xpost_font_face_last_file(void);

/**
 * @brief Whether the face most recently opened by name is a face other
 * than the one that name asked for.
 *
 * @return 1 where the face carries neither the requested name nor any
 * name equal to it, 0 otherwise.
 *
 * The platform's font configuration answers almost any name with some
 * face, so a name nothing supplies produces a face all the same --
 * which is what PLRM 8.2 asks of findfont, and what leaves a caller
 * unable to tell a face it asked for from one it was given instead.
 * Read straight after xpost_font_face_new_from_name(), as
 * xpost_font_face_last_file() is: it answers for the last open by
 * name, and the next one replaces the answer.
 */
int xpost_font_face_last_is_substitute(void);

/* whether the host carries a face of this name rather than a substitute
   for it; asked of the configuration, opening nothing */
int xpost_font_name_is_available(const char *name);

/* the host's faces as the configuration holds them: how many there are,
   and the name each calls itself by (NULL where a face carries none) */
int xpost_font_host_face_count(void);
const char *xpost_font_host_face_name(int i);

/**
 * @brief Copy the name the face carries for itself (nul-terminated).
 *
 * @param[in] face The font face.
 * @param[out] buf Where the name is written.
 * @param[in] len The room in @p buf.
 * @return 0 where the face states no name or the name does not fit,
 * 1 otherwise.
 *
 * The name the font program states for itself, and where it states
 * none, the family the platform knows it by. It is what a font
 * dictionary made over the face has to give as its /FontName (PLRM
 * Table 5.7) when the name asked for reached some other face.
 */
int xpost_font_face_name_get(void *face, char *buf, int len);
int xpost_font_face_is_type1(void *face);
int xpost_font_face_is_cff(void *face);
void xpost_font_face_free(void *face);

/**
 * @brief Take a further claim on the given face.
 *
 * @param[in,out] face The font face.
 *
 * A face may be named from several places at once -- a cache entry and
 * the font dictionaries made from it -- and each holder releases its
 * claim through xpost_font_face_free(); the face is given up when the
 * last claim is. The count is kept by the font library.
 *
 * @see xpost_font_face_free()
 */
void xpost_font_face_reference(void *face);

/**
 * @brief Return how many faces the font machinery currently holds open.
 *
 * A face is host state outside virtual memory, and how many are open
 * shows nowhere else: the library takes every face still open with it
 * when the font machinery goes down, so what a run held while it ran
 * leaves no trace in what it held at the end. This answers that number
 * during the run, for the callers that have to say a face was given
 * back rather than merely reclaimed at teardown.
 *
 * @return The number of faces open, counting both those opened from a
 *         file and those opened over a program in memory.
 *
 * @see xpost_font_face_new_from_name()
 * @see xpost_font_face_new_from_memory()
 * @see xpost_font_face_free()
 */
long xpost_font_faces_held(void);

/**
 * @brief Scale the given font.
 * @param[in] face The font face.
 * @param[in] scale The scale factor in point.
 *
 * This function scales the font @p face to size @p scale in point
 * unit.
 */
real xpost_font_face_scale(void *face, real scale);

/**
 * @brief Transform the given font.
 * @param[in] face The font face.
 * @param[in] mat The matrix values.
 *
 * This function applies a linear transformation matrix to the font,
 * which may effect any combination of scaling/rotation/skew.
 */
void xpost_font_face_transform(void *face, float *mat);

/**
 * @brief Return the glyph index of the given char  from the given
 * font.
 *
 * @param[in] face The font face.
 * @param[in] c The character.
 * @return The glyph index.
 *
 * This function returns the glygh index of the character @p c in
 * font @p face. If Freetype is not available, it returns -1,
 * otherwise the glyph index.
 *
 * see xpost_font_face_glyph_render()
 */
unsigned int xpost_font_face_glyph_index_get(void *face, char c);

/**
 * @brief Return the glyph index for a glyph name in the given font.
 *
 * @param[in] face The font face.
 * @param[in] name The glyph name (e.g. "zero").
 * @return The glyph index, or 0 when the face has no glyph by that
 * name (or carries no glyph names at all).
 *
 * @see xpost_font_face_glyph_index_get()
 */
unsigned int xpost_font_face_glyph_name_index_get(void *face, const char *name);

/**
 * @brief Return the glyph the face itself gives this name to.
 *
 * @param[in] face The font face.
 * @param[in] name The glyph name (e.g. "zero").
 * @return The glyph index, or 0 where the face gives no glyph that
 * name -- and 0 for every name where it names no glyphs at all.
 *
 * The strict question, where xpost_font_face_glyph_name_index_get()
 * asks the useful one: that call falls back to the standard name's
 * character code and the character map, so it answers for a name the
 * face never wrote down. That fallback is what lets a named encoding
 * select a glyph on a face carrying no names; it is also what makes
 * the other call unable to say whether the face has the name. This
 * one says.
 *
 * @see xpost_font_face_glyph_name_index_get()
 */
unsigned int xpost_font_face_own_name_index_get(void *face, const char *name);

/**
 * @brief The number of glyphs in the face, or 0 when the face carries
 * no glyph names to enumerate them by.
 */
unsigned int xpost_font_face_glyph_name_count(void *face);

/**
 * @brief Whether the face keeps its character codes in the symbol map.
 *
 * @param[in] face The font face.
 * @return 1 where the map the face is read through is the symbol map,
 * 0 otherwise.
 *
 * A face read through that map states an encoding of its own: its
 * codes are the font's, not the standard set's, and they reach their
 * glyphs at the private-use points U+F020..U+F0FF rather than at the
 * points the standard names stand for. So none of the standard names
 * reaches a glyph on such a face, and the standard encoding is not a
 * statement about it -- which is what a caller building the /Encoding
 * of a base font (PLRM 5.2 and Table 5.7) has to know.
 */
int xpost_font_face_is_symbol_encoded(void *face);

/**
 * @brief Copy the name a face that names no glyphs gives a character
 * code (nul-terminated).
 *
 * @param[in] face The font face.
 * @param[in] code The character code, 0..255.
 * @param[out] buf Where the name is written.
 * @param[in] len The room in @p buf.
 * @return 0 where the code reaches no glyph or the name does not fit,
 * 1 otherwise.
 *
 * A glyph on such a face has no name in the font program, and a base
 * font has to name every code it encodes. The name is therefore made
 * from the point the face's own character map reaches the glyph at,
 * written the way xpost_font_face_glyph_name_index_get() reads such a
 * name back -- so a name this answers selects the glyph the code
 * selects, which is the property /Encoding and /CharStrings are read
 * for.
 */
int xpost_font_face_code_glyph_name(void *face, unsigned int code,
                                    char *buf, int len);

/**
 * @brief Copy the name of the given glyph into buf (nul-terminated).
 * Returns 0 on a nameless glyph or a face without glyph names.
 */
int xpost_font_face_glyph_name_get(void *face, unsigned int gid, char *buf, int len);

/**
 * @brief Walk the standard glyph names, answering the glyph this face
 * reaches for each through its character map.
 *
 * The names are the standard-encoding ones, which is what a face
 * carrying no glyph names of its own can still be asked for by name:
 * the name gives a code point and the character map gives the glyph.
 * This is the enumeration behind the glyph complement published for
 * such a face, and it is the same resolution
 * xpost_font_face_glyph_name_index_get() performs for one name.
 *
 * @param[in] face The font face.
 * @param[in] i The position in the standard names, counted from zero.
 * @param[out] name The name at that position, owned by the font layer.
 * @param[out] gid The glyph the character map gives, or 0 where the
 * face has none for that name.
 * @return 0 once @p i has passed the last standard name, 1 otherwise.
 */
int xpost_font_face_std_name_at(void *face, unsigned int i,
                                const char **name, unsigned int *gid);

/**
 * @typedef Xpost_Font_Outline_Sink
 * Callbacks receiving a glyph outline decomposed into path segments.
 *
 * Coordinates are in pixels (26.6 fixed point divided out), y-up,
 * relative to the pen position. Quadratic segments are converted so
 * only cubic curves are delivered. Each callback returns 0 to
 * continue, non-zero to abort the decomposition.
 */
typedef struct
{
    int (*moveto)(void *user, double x, double y);
    int (*lineto)(void *user, double x, double y);
    int (*curveto)(void *user, double x1, double y1, double x2, double y2, double x3, double y3);
    int (*closepath)(void *user);
    void *user;
} Xpost_Font_Outline_Sink;

/**
 * @brief Decompose a glyph's outline into path segments.
 *
 * @param[in] face The font face.
 * @param[in] glyph_index The glyph index.
 * @param[in] sink The segment callbacks.
 * @param[out] advance_x The horizontal advance (16.16 fixed point,
 * unhinted linear width through the face transform).
 * @param[out] advance_y The vertical advance (16.16 fixed point).
 * @return 1 on success, 0 otherwise (e.g. a bitmap-only glyph).
 *
 * The glyph is loaded without rendering; the face's size and
 * transform apply to the outline and the advance exactly as they do
 * to the rendered bitmap.
 */
int xpost_font_face_glyph_outline(void *face, unsigned int glyph_index, const Xpost_Font_Outline_Sink *sink, long *advance_x, long *advance_y);

/**
 * @brief Walk a glyph's outline in its own design units.
 *
 * @param[in] face The face.
 * @param[in] glyph_index The glyph.
 * @param[in] sink Where the outline is walked to.
 * @param[in] units The em count the caller keeps glyph space in.
 * @return 1 on success, 0 otherwise.
 *
 * As xpost_font_face_glyph_outline(), but the outline is taken
 * unscaled and untransformed and rescaled to @p units per em, so the
 * same glyph yields the same description at every size it is drawn.
 */
int xpost_font_face_glyph_outline_units(void *face, unsigned int glyph_index, const Xpost_Font_Outline_Sink *sink, int units);

/**
 * @brief A glyph's advance in the font program's own design units,
 * rescaled to @p units per em. Independent of the size being drawn.
 * @return 1 on success, 0 otherwise.
 */
int xpost_font_face_glyph_advance_units(void *face, unsigned int glyph_index, int units, double *advance);

/**
 * @brief render the given glyph of the given face.
 * font.
 *
 * @param[in] face The font face.
 * @param[in] glyph_index The glyph index.
 * @return 1 on success, 0 otherwise.
 *
 * This function renders in an internal buffer the glyph
 * @p glyph_index of font @p face in an internal buffer. It returns 1
 * on success, 0 otherwise.
 *
 * @see xpost_font_face_glyph_index_get()
 */
int xpost_font_face_glyph_render(void *face, unsigned int glyph_index);

/**
 * @brief Report a glyph outline's ink extent and advance without
 * rasterizing, in 26.6 glyph space (y-up around the pen).
 *
 * @return 1 on success; 0 when the glyph cannot load or has no
 * outline (a bitmap strike), in which case render instead.
 */
int xpost_font_face_glyph_extents(void *face, unsigned int glyph_index,
                                  long *xmin, long *ymin, long *xmax, long *ymax,
                                  long *advance_x, long *advance_y);

/**
 * @brief Report a glyph's own left sidebearing under the face's current
 * transform, in 16.16 (y-up around the pen).
 *
 * @return 1 on success, 0 when the glyph cannot be loaded.
 *
 * The left sidebearing point is where the face draws the glyph relative
 * to its origin (PLRM 5.4); a font dictionary's /Metrics entry may name
 * another, and the difference is how far the glyph moves.
 */
int xpost_font_face_glyph_sidebearing(void *face, unsigned int glyph_index,
                                      long *sbx, long *sby);

/* the glyph cache behind the rendering pair: status and limits for
   the cache operators, and keyed raster entry points for glyphs
   painted by procedure rather than by face */
void xpost_font_cache_status(long *bsize, long *bmax, long *msize,
                             long *mmax, long *csize, long *cmax,
                             long *blimit);
/* The per-glyph ceiling a context starts with, and so what the MaxFontItem
   user parameter reads before anything sets it (PLRM C.3.2). Declared here
   because the store and the context that owns the parameter must start from
   one number rather than from two that happen to agree. */
#define XPOST_FONT_ITEM_LIMIT_DEFAULT 32768L
/* Both setters answer the per-glyph ceiling now in force, which is the one
   asked for held to the range the store offers. The parameter is the
   context's (PLRM 8.2 setcachelimit: it is maintained separately for each
   context and is subject to save and restore), and a context that recorded
   the value it asked for rather than the value it got would read back a
   number the store is not going by. */
long xpost_font_cache_setlimit(long blimit);
long xpost_font_cache_setparams(long bmax, long lower, long upper);
/* Drop the procedure/Type-3 masks (keyed by k1==NULL, on a virtual-memory
   serial the job boundary reverts) so none carries across a job. */
void xpost_font_mask_cache_flush(void);
/* A cache of coverage masks, keyed by a caller's identity (k1, k2) and
   the transform they were rendered under. It is not font machinery: a
   glyph is one kind of mask and its face and character code are one way
   of naming one. It lives here because fonts were its first caller. */
int xpost_mask_cache_lookup(const void *k1, unsigned long long k2,
                                 const long m[4], long size,
                                 unsigned char **bits, int *rows, int *width,
                                 int *pitch, int *left, int *top,
                                 long *advance_x, long *advance_y);
int xpost_mask_cache_insert(const void *k1, unsigned long long k2,
                                 const long m[4], long size,
                                 const unsigned char *bits, int rows,
                                 int width, int pitch, int left, int top,
                                 long advance_x, long advance_y);

/* Empty it. For a caller whose way of naming a mask has run its range
   and started over: nothing held can be told apart from what the
   reissued names will mean, so nothing is held. */
void xpost_mask_cache_clear(void);

void xpost_font_face_glyph_buffer_get(void *face, unsigned char **buffer, int *rows, int *width, int *pitch, char *pixel_mode, int *left, int *top, long *advance_x, long *advance_y);


#endif
