/*
 * Xpost - a PostScript Level-3 interpreter
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XPOST_OBJECT_H
#define XPOST_OBJECT_H

#include "xpost.h"
#include "xpost_private.h" /* XPOST_TEST_VISIBLE */

/**
 * @file xpost_object.h
 * @brief The file defines the basic object structure, typically 8-bytes.
 *
 * The types it represents, and what distinguishes one from another, are
 * PLRM 3.3: a simple object holds its value where a composite holds a
 * reference to storage shared by every object referring to it (3.3.1),
 * and the attributes each carries are 3.3.2. The representation below
 * is this interpreter's own; what it has to represent is that section's.
 *
 * @defgroup xpost_object Object structure
 *
 * @{
 */

/*
 *
 * Macros
 *
 */

/**
 * @def XPOST_OBJECT_TYPES
 * @brief X-macro for defining enum of typenames and
 *        associated string-table.
 */

#define XPOST_OBJECT_TYPES(_) \
    _(invalid)   /*0*/ \
    _(null)      /*1*/ \
    _(mark)      /*2*/ \
    _(integer)   /*3*/ \
    _(real)      /*4*/ \
    _(array)     /*5*/ \
    _(dict)      /*6*/ \
    _(file)      /*7*/ \
    _(operator)  /*8*/ \
    _(save)      /*9*/ \
    _(name)     /*10*/ \
    _(boolean)  /*11*/ \
    _(context)  /*12*/ \
    _(extended) /*13*/ \
    _(glob)     /*14*/ \
    _(magic)    /*15*/ \
    _(string)   /*16*/ \
    _(fontID)   /*17*/ \
/* #def XPOST_OBJECT_TYPES */

#define XPOST_OBJECT_AS_TYPE(_) \
    _ ## type ,

#define XPOST_OBJECT_AS_STR(_) \
    #_ ,

#define XPOST_OBJECT_AS_TYPE_STR(_) \
    #_ "type" ,

#define XPOST_OBJECT_DECLARE_SINGLETON(_) \
    XPOST_TEST_VISIBLE extern Xpost_Object _ ;

/* The inner braces are the member the union names first, which is the
   one an initialiser reaches through; see the union below. */
#define XPOST_OBJECT_DEFINE_SINGLETON(_) \
    Xpost_Object _ = \
    { \
        { XPOST_OBJECT_AS_TYPE(_) 0, 0 } \
    };

#define XPOST_OBJECT_SINGLETONS(_) \
    _(invalid) \
    _(null) \
    _(mark) \
/* #def XPOST_OBJECT_SINGLETONS */


/*
 *
 * Enums
 *
 */

/**
 * @enum Xpost_Object_Type
 * @brief A value to track the type of object,
 *        and select the correct union member for manipulation.
 *
 * The numbering is what the language is told: a type's position here
 * fixes the position of every type after it, and the name each position
 * carries is what the type operator answers with. magictype names no
 * member of the union and no object is built of it; the entry holds the
 * number in place.
 *
 * fontIDtype is PLRM Table 3.2's fontID: the type of the object a font
 * dictionary carries under FID (PLRM Table 5.2). It is a simple object
 * carrying an identity in .mark_.padw and nothing else; no operator
 * builds one, so a program can tell one apart but cannot make one.
 */
typedef enum
{
    XPOST_OBJECT_TYPES(XPOST_OBJECT_AS_TYPE)
    XPOST_OBJECT_NTYPES
} Xpost_Object_Type;

/**
 * @enum Xpost_Object_Tag_Data
 * @brief Bitmasks and bitshift-positions for the flags in the tag.
 */
typedef enum
{
    XPOST_OBJECT_TAG_DATA_TYPE_MASK          = 0x001F, /**< mask to yield Xpost_Object_Type */
    XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET = 5,    /**< bitwise offset of the ACCESS field */
    XPOST_OBJECT_TAG_DATA_FLAG_LIT_OFFSET =
        XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET + 2, /* access is a 2-bit field, lit must make room */
    XPOST_OBJECT_TAG_DATA_FLAG_BANK_OFFSET,
    XPOST_OBJECT_TAG_DATA_EXTENDED_REAL_OFFSET,
    XPOST_OBJECT_TAG_DATA_FLAG_PACKED_OFFSET,
    /* Two bits a pair of flags used to hold, kept out of use rather than
       given away. What lies above the flags is the entity number's high
       end (XPOST_OBJECT_TAG_EXTRA_BITS_SIZE below), so a flag that stops
       being spent is a quadrupling of how many entities an object can
       name -- which is an architectural bound, stated in the design page
       and held by a test that fills to it, and not something a tidying
       of flags should decide by side effect. They are here for the next
       flag that wants one, or to be spent deliberately. */
    XPOST_OBJECT_TAG_DATA_RESERVED_0_OFFSET,
    XPOST_OBJECT_TAG_DATA_RESERVED_1_OFFSET,
    XPOST_OBJECT_TAG_DATA_EXTRA_BITS,
    XPOST_OBJECT_TAG_DATA_NBITS = XPOST_OBJECT_TAG_DATA_EXTRA_BITS,  /* this MUST be < 16, the size of the tag field */

    XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK =
        03 << XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET,
            /**< 2-bit mask for the ACCESS field */
    XPOST_OBJECT_TAG_DATA_FLAG_LIT =
        01 << XPOST_OBJECT_TAG_DATA_FLAG_LIT_OFFSET,
            /**< literal flag: 0=executable, 1=literal */
    XPOST_OBJECT_TAG_DATA_FLAG_BANK =
        01 << XPOST_OBJECT_TAG_DATA_FLAG_BANK_OFFSET,
            /**< select memory-file for composite-object data:
              0=local, 1=global */
    XPOST_OBJECT_TAG_DATA_EXTENDED_REAL =
        01 << XPOST_OBJECT_TAG_DATA_EXTENDED_REAL_OFFSET,
            /**< an extended object was a real; clear, it was an integer.
                 One bit rather than two: a number key is one or the
                 other, so a pair of flags spends a bit of tag space to
                 describe a state that cannot occur */
            /**< for _onerror to reset stack */
    XPOST_OBJECT_TAG_DATA_FLAG_PACKED =
        01 << XPOST_OBJECT_TAG_DATA_FLAG_PACKED_OFFSET,
            /**< the array was produced by the packing machinery
                 (setpacking true, or the packedarray operator). It is
                 read-only like any packed array, but -- unlike a plain
                 read-only array -- bind descends into and rewrites it,
                 and the type operator reports packedarraytype. */

    XPOST_OBJECT_TAG_DATA_FLAG_FILE_EXEC =
        01 << XPOST_OBJECT_TAG_DATA_EXTRA_BITS
            /**< filetype objects only: the file may be executed although
                 it may not be read, which is where executeonly leaves it.
                 A file that may be read may be executed, so the flag says
                 nothing until read access has gone and is only ever set
                 by the file access hooks in xpost_file.c.

                 It sits in the tag bits above the named flags, which the
                 tag lends to a composite object's entity number on a
                 narrow-word build. A file is not a composite -- it holds
                 its entity in .mark_.padw, and set_ent and get_ent refuse
                 an object that is not a string, array or dictionary -- so
                 nothing else ever writes these bits of a file's tag. */
} Xpost_Object_Tag_Data;

/**
 * @enum Xpost_Object_Tag_Access
 * @brief valid values for the ACCESS bitfield in the object's tag.
 *
 * An array, a string and a dictionary sit on a ladder of four accesses in
 * increasing order of restriction -- none, execute-only, read-only,
 * unlimited (PLRM 3.3.2 "Access") -- and each value below names a rung.
 *
 * A file does not sit on that ladder. Its read and its write capability
 * are settled independently by the access string it was opened with
 * (PLRM 3.8.1), so a file opened for writing can be written and not read,
 * which is no rung of the ladder at all. A file's access is therefore a
 * set of the three FILE_ capabilities below, and it is read and written
 * through xpost_object_get_access and xpost_object_set_access like any
 * other, by way of the hooks a file installs for them.
 */
typedef enum
{
    XPOST_OBJECT_TAG_ACCESS_NONE,         /**< WRITE= no,  READ= no,  EXEC= no   */
    XPOST_OBJECT_TAG_ACCESS_EXECUTE_ONLY, /**< WRITE= no,  READ= no,  EXEC= yes  */
    XPOST_OBJECT_TAG_ACCESS_READ_ONLY,    /**< WRITE= no,  READ= yes, EXEC= yes */
    XPOST_OBJECT_TAG_ACCESS_UNLIMITED,    /**< WRITE= yes, READ= yes, EXEC= yes */

    /* these 3 are for filetype objects only, and combine: */

    XPOST_OBJECT_TAG_ACCESS_FILE_WRITE = 1 << 0, /**< file is writeable */
    XPOST_OBJECT_TAG_ACCESS_FILE_READ  = 1 << 1, /**< file is readable */
    XPOST_OBJECT_TAG_ACCESS_FILE_EXEC  = 1 << 2  /**< file is executable */
} Xpost_Object_Tag_Access;


/*
 *
 * Typedefs
 *
 */

#ifdef WANT_LARGE_OBJECT
typedef unsigned char byte;
typedef unsigned int word;      /* 2x small size */
# ifdef _WIN32
 typedef unsigned __int64 dword; /* 2x small size */
 typedef __int64 integer;        /* 2x small size */
# else
 typedef unsigned long long dword;    /* 2x small size */
 typedef long long integer;           /* 2x small size */
# endif
typedef double real;            /* 2x small size */
typedef dword addr;             /* 2x small size (via dword) */
#else
typedef unsigned char byte;  /* assumed 8-bit */
typedef unsigned short word; /* assumed 16-bit */
typedef unsigned int dword;  /* assumed 32-bit */
typedef int integer;         /* assumed 32-bit */
typedef float real;          /* assumed IEEE 754 32-bit floating-point */
typedef dword addr;
#endif

#define XPOST_OBJECT_TAG_EXTRA_BITS_SIZE  (sizeof(word)*8 - XPOST_OBJECT_TAG_DATA_EXTRA_BITS)
            /**< for extending the ent field for composite objects */

/*
 *
 * Structs
 *
 */

/**
 * @struct Xpost_Object_Mark
 * @brief A generic object: tag word, pad word, and a double-word.
 *
 * To avoid too many structures, many types use .mark_.padw
 * to hold an unsigned integer (eg. operatortype, nametype, filetype).
 * Of course, if a type needs to use pad0, that's a sign that
 * it needs its own struct.
 */
typedef struct
{
    word tag; /**< (marktype, filetype, operatortype or nametype) | flags */
    word pad0; /**< == 0 */
    dword padw; /**< payload: an unsigned integer,
                  0 in a marktype object,
                  used for an ent (memory table index)
                      which addresses the FILE * in a filetype object,
                  used for opcode in an operatortype object,
                  used for name index in a nametype object. */
} Xpost_Object_Mark;

/**
 * @struct Xpost_Object_Int
 * @brief The integertype object.
 */
typedef struct
{
    word tag; /**< integertype | flags */
    word pad; /**< == 0 */
    integer val; /**< payload integer value */
} Xpost_Object_Int;

/**
 * @struct Xpost_Object_Real
 * @brief The realtype object.
 */
typedef struct
{
    word tag; /**< realtype | flags */
    word pad; /**< == 0 */
    real val; /**< payload floating-point value */
} Xpost_Object_Real;

/**
 * @struct Xpost_Object_Extended
 * @brief A combined integer-real for use in dictionaries
 *        as number keys.
 */
typedef struct
{
    word tag; /**< extendedtype |
                XPOST_OBJECT_TAG_DATA_EXTENDED_REAL when the key was a
                real, clear when it was an integer | other flags */
    word sign_exp; /**< sign and exponent from a double */
    dword fraction; /**< truncated fraction from a double */
} Xpost_Object_Extended;

/**
 * @struct Xpost_Object_Comp
 * @brief The composite object structure, used for strings, arrays, dicts.
 */
typedef struct
{
    word tag; /**< (stringtype, arraytype, or dicttype) | flags */
    word sz; /**< number of bytes in string,
                   number of objects in array,
                   number of key-value pairs in dict */
    word ent; /**< entity. Absolute index into Xpost_Memory_Table */
    word off; /**< byte offset in string,
                    object offset in array,
                    zero in dict: a dictionary's hash table is longer
                    than this field counts, so nothing indexes it here */
} Xpost_Object_Comp;
/* The widest entity number an object can carry: the ent field plus the
   tag's spare bits, clamped so a wide-word build -- whose ent field
   already spans the table -- neither overflows the shift nor borrows
   from the tag. */
#define XPOST_OBJECT_COMP_ENT_BITS \
    (sizeof(word)*8 + XPOST_OBJECT_TAG_EXTRA_BITS_SIZE > 31 \
        ? 31 : sizeof(word)*8 + XPOST_OBJECT_TAG_EXTRA_BITS_SIZE)
#define XPOST_OBJECT_COMP_MAX_ENT ((1u << XPOST_OBJECT_COMP_ENT_BITS) - 1)

/* The widest element count a composite can carry: the sz field's own
   width. The narrow build's 65,535 is the architectural limit PLRM
   Appendix B.1 documents; the wide build carries composites to
   memory. */
#define XPOST_OBJECT_COMP_MAX_SZ ((dword)(word)~(word)0)

/**
 * @struct Xpost_Object_Save
 * @brief The savetype object, for both user and on the save stack.
 */
typedef struct
{
    word tag; /**< savetype */
    word lev; /**< save-level, index into Save stack */
    dword stk; /**< address of Saverec stack */
} Xpost_Object_Save;

/**
 * @struct Xpost_Object_Saverec
 * @brief The saverec type overlays an object so that it can be stacked.
 *
 * The saverec type is not available as a (Postscript) user type.
 *  saverec's occupy the "current save stack" referred to by the
 * stk field of a save object.
 */
typedef struct
{
    word tag; /**< arraytype or dicttype */
    word pad; /**< == 0 */
    word src; /**< entity number of source, the allocation being used */
    word cpy; /**< entity number of copy, the copy to revert to in restore */
} Xpost_Object_Saverec;

/**
 * @struct Xpost_Object_Glob
 * @brief The globtype object exists only for passing between
 *        iterations of filenameforall.
 *
 * The globtype object is not available as a (Postscript) user type.
 * It has no use outside the filenameforall looping construct.
 * It names the matched paths and nothing else: how many there are is a
 * property of the directory, so the enumeration's cursor rides the
 * execution stack beside it as an integer rather than in a field here.
 *
 * The paths are a host allocation rather than virtual memory, and what
 * is carried here is the number the context holds them under, not their
 * address. An object outliving the enumeration that made it names
 * nothing, and the operator reading it reports rather than follows it.
 */
typedef struct
{
    word tag; /**< globtype */
    word pad; /**< == 0 */
    dword id; /**< the number the matched paths are held under in the
                    context; see xpost_context_glob_held */
} Xpost_Object_Glob;

/*
 *
 * Union
 *
 */

/**
 * @union Xpost_Object
 * @brief The top-level object union.
 *
 * The tag word overlays the tag words in each subtype, so it can
 * be used to determine an object's type (using the xpost_object_get_type()
 * function which masks-off any flags in the tag).
 *
 * mark_ is named first, and which member is named first decides how
 * much of an object a brace initialiser reaches. A union is not an
 * aggregate: `= { 0 }` gives the member named first a value and says
 * nothing about the storage past that member (C99 6.2.5, 6.7.8), so a
 * first member narrower than the object leaves the rest of it holding
 * whatever the storage held before. Such an object is written to
 * virtual memory whole and read back as the language's, and what it
 * carries there is a value the language never put in it. A member
 * spanning the object, named first, puts every field within the
 * initialiser's reach: the fields after the one the initialiser gives a
 * value to are the remainder of an aggregate, which is cleared. The
 * assertions below hold mark_ to spanning an object and to there being
 * no padding anywhere in one, so its three fields are every byte there
 * is; tests/check-object-brace-init.sh holds the order.
 */
typedef union
{
    Xpost_Object_Mark mark_;

    word tag;

    Xpost_Object_Int int_;
    Xpost_Object_Real real_;
    Xpost_Object_Extended extended_;
    Xpost_Object_Comp comp_;
    Xpost_Object_Save save_;
    Xpost_Object_Saverec saverec_;
    Xpost_Object_Glob glob_;
} Xpost_Object;

/*
 * An object is the unit virtual memory is allocated and measured in, so
 * every member of the union is as wide as the union itself: one member
 * wider than the rest widens every object in the interpreter, every
 * array of objects, and every stack of them. A host pointer is what
 * does that on a build whose fields are narrower than one, so a member
 * given a pointer to carry fails the build here instead.
 */
#define XPOST_OBJECT_MEMBER_FILLS_UNION(member) \
    typedef char xpost_object_ ## member ## fills_union \
        [1 - 2*!(sizeof(((Xpost_Object *)0)->member) == sizeof(Xpost_Object))];

XPOST_OBJECT_MEMBER_FILLS_UNION(mark_)
XPOST_OBJECT_MEMBER_FILLS_UNION(int_)
XPOST_OBJECT_MEMBER_FILLS_UNION(real_)
XPOST_OBJECT_MEMBER_FILLS_UNION(extended_)
XPOST_OBJECT_MEMBER_FILLS_UNION(comp_)
XPOST_OBJECT_MEMBER_FILLS_UNION(save_)
XPOST_OBJECT_MEMBER_FILLS_UNION(saverec_)
XPOST_OBJECT_MEMBER_FILLS_UNION(glob_)

/* and the union is the two fields the tag word shares its place with,
   and no padding beyond them */
typedef char xpost_object_is_tag_pad_and_payload
    [1 - 2*!(sizeof(Xpost_Object) == sizeof(word)*2 + sizeof(dword))];


/*
 *
 * Variables
 *
 */

/**
 * @def XPOST_OBJECT_SINGLETONS
 * @brief Certain simple objects exist as global template variables
 *        rather than do-nothing constructors.
 */
XPOST_OBJECT_SINGLETONS(XPOST_OBJECT_DECLARE_SINGLETON)

/**
 * @var char *xpost_object_type_names[]
 * @brief A table of strings keyed to the types enum.
 */
extern
const char *xpost_object_type_names[]
    /*= { XPOST_OBJECT_TYPES(XPOST_OBJECT_AS_TYPE_STR) "invalid"}*/ ;

/*
 *
 * Functions
 *
 */


/*
   Constructors for simple types.

   These objects contain their own values and are not tied
   to a specific context.
 */

/**
 * @brief Construct a booleantype object with the given value.
 *
 * @param[in] b A boolean value.
 * @return A new object.
 *
 * This function constructs a booleantype object with value @p b.
 * It sets the type to booleantype, sets unlimited access, sets the
 * pad to 0, sets the value to @p b. It returns the object as literal.
 */
XPOST_TEST_VISIBLE Xpost_Object xpost_bool_cons(int b);

/**
 * @brief Construct an integertype object with the given value.
 *
 * @param[in] i An integer value, typically defined as int32_t.
 * @return A new object.
 *
 * This function constructs an integertype object with value @p i.
 * It sets the type to integertype, sest unlimited access, sets the
 * pad to 0, set the value to @p i. It returns the object as literal.
 */
XPOST_TEST_VISIBLE Xpost_Object xpost_int_cons(integer i);

/**
 * @brief Construct a realtype object with the given value.
 *
 * @param[in] r A real value, typically defined as float.
 * @return A new object.
 *
 * This function constructs a realtype object with value @p r.
 * It sets the type to realtype, sets unlimited access,
 * sets the pad to 0, sets the value to @p r. It returns the object as
 * literal.
 */
XPOST_TEST_VISIBLE Xpost_Object xpost_real_cons(real r);

/**
 * @brief Construct a fontIDtype object with the given identity.
 *
 * @param[in] id The identity the object carries.
 * @return A new object.
 *
 * This function constructs the object a font dictionary carries under
 * FID (PLRM Table 5.2). Two of them are the same font's when they carry
 * the same identity, which is the whole of what the value is for: it
 * names the dictionary the font machinery stamped and is read by
 * nothing else. It returns the object as literal.
 */
XPOST_TEST_VISIBLE Xpost_Object xpost_fontid_cons(dword id);


/*
   Type and Tag Manipulation

   These functions manipulate the information in the Xpost_Object's
   tag field, which contains the type and various flags and bitfields.
 */

/**
 * @brief Return the object's type, it. the tag with flags masked-off.
 *
 * @param[in] obj The object.
 * @return The type of the object as an #Xpost_Object_Type.
 *
 * This function returns the type of the object @p obj, that is the tag
 * with flags masked-off : obj.tag & #XPOST_OBJECT_TAG_DATA_TYPEMASK
 */
static inline Xpost_Object_Type xpost_object_get_type(Xpost_Object obj)
{
    return (Xpost_Object_Type)(obj.tag & XPOST_OBJECT_TAG_DATA_TYPE_MASK);
}

/**
 * @brief Determine whether the object is composite or not (ie. simple).
 *
 * @param[in] obj The object.
 * @return 1 if the object is composite, 0 otherwise.
 *
 * This function returns 1 if the object @p obj is one of the composite
 * types (arraytype, stringtype, or dicttype), 0 otherwise.
 */
/**
 * @brief the numeric value of an integer- or real-type object, as a
 * double. The int-to-real promotion every consumer of a numbertype
 * operand performs; the caller has already type-checked the object.
 */
static inline double xpost_object_number(Xpost_Object obj)
{
    return xpost_object_get_type(obj) == realtype
        ? (double)obj.real_.val : (double)obj.int_.val;
}

static inline int xpost_object_is_composite(Xpost_Object obj)
{
    switch (xpost_object_get_type(obj))
    {
        case stringtype: /*@fallthrough@*/
        case arraytype: /*@fallthrough@*/
        case dicttype:
            return 1;
        default: break;
    }
    return 0;
}

/**
 * @brief Determine whether the object's value belongs to a VM bank.
 *
 * @param[in] obj The object.
 * @return 1 if the value belongs to one bank or the other, 0 otherwise.
 *
 * The three composite types answer here, their values being entities in
 * one memory file or the other, and so does the save object: it "is
 * composite and logically belongs to the local VM, regardless of the
 * current VM allocation mode" (PLRM 8.2, save), and so reads as local
 * whatever mode it was made under.
 *
 * This is the question PLRM 3.7.2 asks of a value being stored into a
 * composite object, where storing one that lives in local VM into one
 * that lives in global VM is an invalidaccess: a restore may take the
 * local value away and leave the global object naming nothing. A save
 * object is exactly such a value, so it is asked about alongside the
 * other three.
 */
static inline int xpost_object_is_banked(Xpost_Object obj)
{
    return xpost_object_is_composite(obj)
        || xpost_object_get_type(obj) == savetype;
}

/**
 * @brief Yield the ent number (memory table index)
 *        for a composite object.
 * @return ent number or -1 if not composite.
 *
 * filetype objects bypass these functions and use the dword field .mark_.padw
 */
static inline int xpost_object_get_ent(Xpost_Object obj)
{
    if (!xpost_object_is_composite(obj))
        return -1;
    /* a word of unsigned int width (or wider) holds the entity whole;
       the tag lends bits only to a narrower ent field, and the shift
       counts stay below the operand width either way */
    if (sizeof(word) >= sizeof(unsigned int))
        return (int)obj.comp_.ent;
    return (unsigned int)obj.comp_.ent +
        ((obj.comp_.tag >> XPOST_OBJECT_TAG_DATA_EXTRA_BITS)
         << ((8*sizeof(word)) % (8*sizeof(unsigned int))));
}

/**
 * @brief set the ent number in the object.
 */
XPOST_TEST_VISIBLE Xpost_Object xpost_object_set_ent(Xpost_Object obj,
                                                     unsigned int ent);

/**
 * @brief adjust the size and offset fields in the object
*/
Xpost_Object xpost_object_get_interval(Xpost_Object a,
                                       integer s,
                                       integer n);

/**
 * @brief Determine whether the object is executable or not.
 *
 * @param[in] obj The object.
 * @return 1 if the object is executable, 0 otherwise.
 *
 * This function returns 1 if the object @p obj is executable, 0
 * otherwise.
 *
 * Masks the #XPOST_OBJECT_TAG_DATA_FLAG_LIT with the tag and performs
 * a logical NOT. Ie. executable means NOT having the
 * #XPOST_OBJECT_TAG_DATA_FLAG_LIT flag set.
 */
static inline int xpost_object_is_exe(Xpost_Object obj)
{
    return !(obj.tag & XPOST_OBJECT_TAG_DATA_FLAG_LIT);
}

/**
 * @brief Determine whether the object is literal or not.
 *
 * @param[in] obj The object.
 * @return 1 if the object is literal, 0 otherwise.
 *
 * This function returns 1 if the object @p obj is literal, 0
 * otherwise.
 *
 * Masks the #XPOST_OBJECT_TAG_DATA_FLAG_LIT with the tag and performs
 * a double-NOT to normalize the value to the range [0..1].
 */
static inline int xpost_object_is_lit(Xpost_Object obj)
{
    return !!(obj.tag & XPOST_OBJECT_TAG_DATA_FLAG_LIT);
}

/**
 * @brief Determine whether the array was produced by array packing.
 *
 * @param[in] obj The object.
 * @return 1 if the object carries the packed flag, 0 otherwise.
 *
 * A packed array is stored as a read-only array but tagged with
 * #XPOST_OBJECT_TAG_DATA_FLAG_PACKED so it can be distinguished from an
 * ordinary read-only array: bind rewrites a packed array (and descends
 * into nested ones) and the type operator reports packedarraytype.
 */
int xpost_object_is_packed(Xpost_Object obj);

/**
 * @brief Mark the array as produced by array packing.
 *
 * @param[in] obj The object.
 * @return The object with #XPOST_OBJECT_TAG_DATA_FLAG_PACKED set.
 */
Xpost_Object xpost_object_set_packed(Xpost_Object obj);

/**
 * @brief install specialized access getter functions
 *
 * Dict and file objects share the same access across all duplicates
 * of the object. These functions allow file and dict objects to
 * override the normal access field retrieval method.
 */
void xpost_object_install_dict_get_access(Xpost_Object_Tag_Access (*access_func)(Xpost_Context *, Xpost_Object));
void xpost_object_install_file_get_access(Xpost_Object_Tag_Access (*access_func)(Xpost_Context *, Xpost_Object));

/**
 * @brief Yield the access-field from the object's tag.
 *
 * @param[in] obj The object.
 * @return The access-field from the object's tag.
 *
 * This function returns the access-field from the tag of @p obj
 * a value from #Xpost_Object_Tag_Access.
 *
 * Mask the #XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_MASK with the tag, and shift
 * the result down by #XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET to return just
 * the (2-) bit field.
 *
 * A general description of the access flag behavior is at
 * https://groups.google.com/d/topic/comp.lang.postscript/ENxhFBqwgq4/discussion
 */
Xpost_Object_Tag_Access xpost_object_get_access(Xpost_Context *ctx, Xpost_Object obj);

/**
 * @brief install specialized access setter functions
 */
void xpost_object_install_dict_set_access(Xpost_Object (*set_access_func)(Xpost_Context *, Xpost_Object, Xpost_Object_Tag_Access));
void xpost_object_install_file_set_access(Xpost_Object (*set_access_func)(Xpost_Context *, Xpost_Object, Xpost_Object_Tag_Access));

/**
 * @brief Return object with access-field set to access.
 *
 * @param[in] obj The object.
 * @param[in] access New access-field value.
 * @return The modified object.
 *
 * This function sets the access-field in @p obj to @p access.
 * It returns the modified object by clearing the access-field with
 * an inverse mask. OR-in the new access field, shifted up by
 * #XPOST_OBJECT_TAG_DATA_FLAG_ACCESS_OFFSET.
 *
 * A dictionary carries its access on its value rather than on the
 * object, so setting it reaches virtual memory and can be refused
 * there; a refusal is answered with null and leaves the access as it
 * was. Every other type carries it on the object and cannot refuse.
 */
Xpost_Object xpost_object_set_access(Xpost_Context *ctx,
                                     Xpost_Object obj,
                                     Xpost_Object_Tag_Access access);


/**
 * @brief Determine whether the object is readable or not.
 *
 * @param[in] obj The object.
 * @return 1 if the object is readable, 0 otherwise.
 *
 * This function checks the access permissions of @p obj,
 * specially for filetypes. Regular objects have read access if the
 * value is greater than executeonly.
 *
 * Filetype objects use the access field as 2 independent flags.
 * A file is readable if the FILE_READ flag is set.
 */
int xpost_object_is_readable(Xpost_Context *ctx, Xpost_Object obj);

/**
 * @brief Determine whether the object is writable or not.
 *
 * @param[in] obj The object.
 * @return 1 if the object is writeable, 0 otherwise.
 *
 * This function checks the access permissions of @p obj,
 * specially for filetypes. Regular objects have write access if
 * the value is equal to unlimited.
 *
 * Filetype objects use the access field as 2 independent flags.
 * A file is writeable if the FILE_WRITE flag is set.
 */
int xpost_object_is_writeable(Xpost_Context *ctx, Xpost_Object obj);


/**
 * @brief Convert object to executable.
 *
 * @param[in] obj The object.
 * @return A new object with executable attribute set to executable.
 *
 * The name 'cvx' is borrowed from the Postscript language.
 * cvx is the name of the Postscript operator which performs
 * this function.
 */
static inline Xpost_Object xpost_object_cvx(Xpost_Object obj)
{
    obj.tag &= ~ XPOST_OBJECT_TAG_DATA_FLAG_LIT;

    return obj;
}

/**
 * @brief Convert object to literal.
 *
 * @param[in] obj The object.
 * @return A new object with executable attribute set to literal.
 *
 * The name 'cvlit' is borrowed from the Postscript language.
 * cvlit is the name of the Postscript operator which performs
 * this function.
 */
static inline Xpost_Object xpost_object_cvlit(Xpost_Object obj)
{
    obj.tag |= XPOST_OBJECT_TAG_DATA_FLAG_LIT;

    return obj;
}


/*
   Debugging dump.

   This function is used in the backup error handler.
 */

/**
 * @brief print a dump of the object contents to stdout
 *
 * @param[in] obj The object to dump.
 *
 * This function can print the raw object's contents,
 * discriminated by type. It can print the values of
 * simple object, but not composites where it can only
 * print the memory-table index (aka 'ent') and offset.
 *
 * This function is used in the backup error handler
 * which is used for errors in initialization or when the
 * installed error handler fails. Since it is part of a
 * larger information dump, there should also be a dump
 * of the memory-file and memory-tables where the ent
 * may be located.
 */
void xpost_object_dump(Xpost_Object obj);

/**
 * @}
 */

#endif
