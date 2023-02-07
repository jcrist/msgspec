#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <float.h>

#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "datetime.h"
#include "structmember.h"

#include "common.h"
#include "ryu.h"
#include "atof.h"

/* Hint to the compiler not to store `x` in a register since it is likely to
 * change. Results in much higher performance on GCC, with smaller benefits on
 * clang */
#if defined(__GNUC__)
    #define OPT_FORCE_RELOAD(x) __asm volatile("":"=m"(x)::);
#else
    #define OPT_FORCE_RELOAD(x)
#endif

#ifdef __GNUC__
#define ms_popcount(i) __builtin_popcountll(i)
#else
static int
ms_popcount(uint64_t i) {                            \
    i = i - ((i >> 1) & 0x5555555555555555);  // pairs
    i = (i & 0x3333333333333333) + ((i >> 2) & 0x3333333333333333);  // quads
    i = (i + (i >> 4)) & 0x0F0F0F0F0F0F0F0F;  // groups of 8
    return (uint64_t)(i * 0x0101010101010101) >> 56;  // sum bytes
}
#endif

#if PY_VERSION_HEX < 0x03090000
#define CALL_ONE_ARG(f, a) PyObject_CallFunctionObjArgs(f, a, NULL)
#define CALL_NO_ARGS(f) PyObject_CallFunctionObjArgs(f, NULL)
#define CALL_METHOD_ONE_ARG(o, n, a) PyObject_CallMethodObjArgs(o, n, a, NULL)
#define CALL_METHOD_NO_ARGS(o, n) PyObject_CallMethodObjArgs(o, n, NULL)
#define SET_SIZE(obj, size) (((PyVarObject *)obj)->ob_size = size)
#else
#define CALL_ONE_ARG(f, a) PyObject_CallOneArg(f, a)
#define CALL_NO_ARGS(f) PyObject_CallNoArgs(f)
#define CALL_METHOD_ONE_ARG(o, n, a) PyObject_CallMethodOneArg(o, n, a)
#define CALL_METHOD_NO_ARGS(o, n) PyObject_CallMethodNoArgs(o, n)
#define SET_SIZE(obj, size) Py_SET_SIZE(obj, size)
#endif

#define is_digit(c) (c >= '0' && c <= '9')

/* Easy access to NoneType object */
#define NONE_TYPE ((PyObject *)(Py_TYPE(Py_None)))

/* Fast shrink of bytes & bytearray objects. This doesn't do any memory
 * allocations, it just shrinks the size of the view presented to Python. Since
 * outputs of `encode` should be short lived (immediately written to a
 * socket/file then dropped), this shouldn't result in increased application
 * memory usage. */
# define FAST_BYTES_SHRINK(obj, size) \
    do { \
    SET_SIZE(obj, size); \
    PyBytes_AS_STRING(obj)[size] = '\0'; \
    } while (0);
# define FAST_BYTEARRAY_SHRINK(obj, size) \
    do { \
    SET_SIZE(obj, size); \
    PyByteArray_AS_STRING(obj)[size] = '\0'; \
    } while (0);

/* XXX: Optimized `PyUnicode_AsUTF8AndSize` for strs that we know have
 * a cached unicode representation. */
static inline const char *
unicode_str_and_size_nocheck(PyObject *str, Py_ssize_t *size) {
    if (MS_LIKELY(PyUnicode_IS_COMPACT_ASCII(str))) {
        *size = ((PyASCIIObject *)str)->length;
        return (char *)(((PyASCIIObject *)str) + 1);
    }
    *size = ((PyCompactUnicodeObject *)str)->utf8_length;
    return ((PyCompactUnicodeObject *)str)->utf8;
}

/* XXX: Optimized `PyUnicode_AsUTF8AndSize` */
static inline const char *
unicode_str_and_size(PyObject *str, Py_ssize_t *size) {
    const char *out = unicode_str_and_size_nocheck(str, size);
    if (MS_LIKELY(out != NULL)) return out;
    return PyUnicode_AsUTF8AndSize(str, size);
}

static MS_INLINE char *
ascii_get_buffer(PyObject *str) {
    return (char *)(((PyASCIIObject *)str) + 1);
}

/* Fill in view.buf & view.len from either a Unicode or buffer-compatible
 * object. */
static int
ms_get_buffer(PyObject *obj, Py_buffer *view) {
    if (MS_UNLIKELY(PyUnicode_CheckExact(obj))) {
        view->buf = (void *)unicode_str_and_size(obj, &(view->len));
        if (view->buf == NULL) return -1;
        return 0;
    }
    return PyObject_GetBuffer(obj, view, PyBUF_CONTIG_RO);
}

static void
ms_release_buffer(PyObject *obj, Py_buffer *view) {
    if (MS_LIKELY(!PyUnicode_CheckExact(obj))) {
        PyBuffer_Release(view);
    }
}

/* Hash algorithm borrowed from cpython 3.10's hashing algorithm for tuples.
 * See https://github.com/python/cpython/blob/4bcef2bb48b3fd82011a89c1c716421b789f1442/Objects/tupleobject.c#L386-L424
 */
#if SIZEOF_PY_UHASH_T > 4
#define MS_HASH_XXPRIME_1 ((Py_uhash_t)11400714785074694791ULL)
#define MS_HASH_XXPRIME_2 ((Py_uhash_t)14029467366897019727ULL)
#define MS_HASH_XXPRIME_5 ((Py_uhash_t)2870177450012600261ULL)
#define MS_HASH_XXROTATE(x) ((x << 31) | (x >> 33))  /* Rotate left 31 bits */
#else
#define MS_HASH_XXPRIME_1 ((Py_uhash_t)2654435761UL)
#define MS_HASH_XXPRIME_2 ((Py_uhash_t)2246822519UL)
#define MS_HASH_XXPRIME_5 ((Py_uhash_t)374761393UL)
#define MS_HASH_XXROTATE(x) ((x << 13) | (x >> 19))  /* Rotate left 13 bits */
#endif

/* Optimized version of PyLong_AsLongLongAndOverflow/PyLong_AsUnsignedLongLong.
 *
 * Returns True if sign * scale won't fit in an `int64` or a `uint64`.
 */
static inline bool
fast_long_extract_parts(PyObject *vv, bool *neg, uint64_t *scale) {
    uint64_t prev, x = 0;
    PyLongObject *v = (PyLongObject *)vv;
    Py_ssize_t i = Py_SIZE(v);
    bool negative = false;

    if (MS_LIKELY(i == 1)) {
        x = v->ob_digit[0];
    }
    else if (i != 0) {
        negative = i < 0;
        if (MS_UNLIKELY(negative)) { i = -i; }
        while (--i >= 0) {
            prev = x;
            x = (x << PyLong_SHIFT) + v->ob_digit[i];
            if ((x >> PyLong_SHIFT) != prev) {
                return true;
            }
        }
        if (negative && x > (1ull << 63)) {
            return true;
        }
    }

    *neg = negative;
    *scale = x;
    return false;
}

/* Access macro to the members which are floating "behind" the object */
#define MS_PyHeapType_GET_MEMBERS(etype) \
    ((PyMemberDef *)(((char *)(etype)) + Py_TYPE(etype)->tp_basicsize))

#define MS_GET_FIRST_SLOT(obj) \
    *((PyObject **)((char *)(obj) + sizeof(PyObject))) \

#define MS_SET_FIRST_SLOT(obj, val) \
    MS_GET_FIRST_SLOT(obj) = (val)


/*************************************************************************
 * Lookup Tables                                                         *
 *************************************************************************/

static const char hex_encode_table[] = "0123456789abcdef";

static const char base64_encode_table[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*************************************************************************
 * GC Utilities                                                          *
 *************************************************************************/

/* Mirrored from pycore_gc.h in cpython */
typedef struct {
    uintptr_t _gc_next;
    uintptr_t _gc_prev;
} MS_PyGC_Head;

#define MS_AS_GC(o) ((MS_PyGC_Head *)(o)-1)
#define MS_TYPE_IS_GC(t) (((PyTypeObject *)(t))->tp_flags & Py_TPFLAGS_HAVE_GC)
#define MS_OBJECT_IS_GC(obj) MS_TYPE_IS_GC(Py_TYPE(obj))
#define MS_IS_TRACKED(o) (MS_AS_GC(o)->_gc_next != 0)

/* Is this object something that is/could be GC tracked? True if
 * - the value supports GC
 * - the value isn't a tuple or the object is tracked (skip tracked checks for non-tuples)
 */
#define MS_OBJ_IS_GC(x) \
    (MS_TYPE_IS_GC(Py_TYPE(x)) && \
     (!PyTuple_CheckExact(x) || MS_IS_TRACKED(x)))

/*************************************************************************
 * Murmurhash2                                                           *
 *************************************************************************/

static inline uint32_t
unaligned_load(const unsigned char *p) {
    uint32_t out;
    memcpy(&out, p, sizeof(out));
    return out;
}

static inline uint32_t
murmur2(const char *p, Py_ssize_t len) {
    const unsigned char *buf = (unsigned char *)p;
    const size_t m = 0x5bd1e995;
    uint32_t hash = (uint32_t)len;

    while(len >= 4) {
        uint32_t k = unaligned_load(buf);
        k *= m;
        k ^= k >> 24;
        k *= m;
        hash *= m;
        hash ^= k;
        buf += 4;
        len -= 4;
    }

    switch(len) {
        case 3:
            hash ^= buf[2] << 16;
        case 2:
            hash ^= buf[1] << 8;
        case 1:
            hash ^= buf[0];
            hash *= m;
    };

    hash ^= hash >> 13;
    hash *= m;
    hash ^= hash >> 15;
    return hash;
}

/*************************************************************************
 * String Cache                                                          *
 *************************************************************************/

#ifndef STRING_CACHE_SIZE
#define STRING_CACHE_SIZE 512
#endif
#ifndef STRING_CACHE_MAX_STRING_LENGTH
#define STRING_CACHE_MAX_STRING_LENGTH 32
#endif

static PyObject *string_cache[STRING_CACHE_SIZE];

static void
string_cache_clear(void) {
    /* Traverse the string cache, deleting any string with a reference count of
     * only 1 */
    for (Py_ssize_t i = 0; i < STRING_CACHE_SIZE; i++) {
        PyObject *obj = string_cache[i];
        if (obj != NULL) {
            if (Py_REFCNT(obj) == 1) {
                Py_DECREF(obj);
                string_cache[i] = NULL;
            }
        }
    }
}

/*************************************************************************
 * Endian handling macros                                                *
 *************************************************************************/

#define _msgspec_store16(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)(x & 0xff); \
} while (0);

#define _msgspec_store32(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 24) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)((x >> 16) & 0xff); \
    ((uint8_t*)to)[2] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[3] = (uint8_t)(x & 0xff); \
} while (0);

#define _msgspec_store64(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 56) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)((x >> 48) & 0xff); \
    ((uint8_t*)to)[2] = (uint8_t)((x >> 40) & 0xff); \
    ((uint8_t*)to)[3] = (uint8_t)((x >> 32) & 0xff); \
    ((uint8_t*)to)[4] = (uint8_t)((x >> 24) & 0xff); \
    ((uint8_t*)to)[5] = (uint8_t)((x >> 16) & 0xff); \
    ((uint8_t*)to)[6] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[7] = (uint8_t)(x & 0xff); \
} while (0);

#define _msgspec_load16(cast, from) ((cast)( \
    (((uint16_t)((uint8_t*)from)[0]) << 8) | \
    (((uint16_t)((uint8_t*)from)[1])     ) ))

#define _msgspec_load32(cast, from) ((cast)( \
    (((uint32_t)((uint8_t*)from)[0]) << 24) | \
    (((uint32_t)((uint8_t*)from)[1]) << 16) | \
    (((uint32_t)((uint8_t*)from)[2]) <<  8) | \
    (((uint32_t)((uint8_t*)from)[3])      ) ))

#define _msgspec_load64(cast, from) ((cast)( \
    (((uint64_t)((uint8_t*)from)[0]) << 56) | \
    (((uint64_t)((uint8_t*)from)[1]) << 48) | \
    (((uint64_t)((uint8_t*)from)[2]) << 40) | \
    (((uint64_t)((uint8_t*)from)[3]) << 32) | \
    (((uint64_t)((uint8_t*)from)[4]) << 24) | \
    (((uint64_t)((uint8_t*)from)[5]) << 16) | \
    (((uint64_t)((uint8_t*)from)[6]) << 8)  | \
    (((uint64_t)((uint8_t*)from)[7])     )  ))

/*************************************************************************
 * Module level state                                                    *
 *************************************************************************/

/* State of the msgspec module */
typedef struct {
    PyObject *MsgspecError;
    PyObject *EncodeError;
    PyObject *DecodeError;
    PyObject *ValidationError;
    PyObject *StructType;
    PyTypeObject *EnumMetaType;
    PyObject *struct_lookup_cache;
    PyObject *str___weakref__;
    PyObject *str__value2member_map_;
    PyObject *str___msgspec_cache__;
    PyObject *str__value_;
    PyObject *str_type;
    PyObject *str_enc_hook;
    PyObject *str_dec_hook;
    PyObject *str_ext_hook;
    PyObject *str_utcoffset;
    PyObject *str___origin__;
    PyObject *str___args__;
    PyObject *str___metadata__;
    PyObject *str___total__;
    PyObject *str___required_keys__;
    PyObject *str__fields;
    PyObject *str__field_defaults;
    PyObject *str___dataclass_fields__;
    PyObject *str___post_init__;
    PyObject *str___supertype__;
    PyObject *str_int;
    PyObject *str_is_safe;
    PyObject *UUIDType;
    PyObject *uuid_safeuuid_unknown;
    PyObject *DecimalType;
    PyObject *typing_union;
    PyObject *typing_any;
    PyObject *typing_literal;
    PyObject *typing_classvar;
    PyObject *typing_generic_alias;
    PyObject *typing_annotated_alias;
    PyObject *concrete_types;
    PyObject *get_type_hints;
    PyObject *get_typeddict_hints;
    PyObject *get_dataclass_info;
    PyObject *rebuild;
#if PY_VERSION_HEX >= 0x030a00f0
    PyObject *types_uniontype;
#endif
    PyObject *astimezone;
    PyObject *re_compile;
    uint8_t gc_cycle;
} MsgspecState;

/* Forward declaration of the msgspec module definition. */
static struct PyModuleDef msgspecmodule;

/* Given a module object, get its per-module state. */
static MsgspecState *
msgspec_get_state(PyObject *module)
{
    return (MsgspecState *)PyModule_GetState(module);
}

/* Find the module instance imported in the currently running sub-interpreter
   and get its state. */
static MsgspecState *
msgspec_get_global_state(void)
{
    PyObject *module = PyState_FindModule(&msgspecmodule);
    return module == NULL ? NULL : msgspec_get_state(module);
}

static int
ms_err_truncated(void)
{
    PyErr_SetString(msgspec_get_global_state()->DecodeError, "Input data was truncated");
    return -1;
}

static PyObject *
ms_err_unreachable(void) {
    PyErr_SetString(PyExc_RuntimeError, "Supposedly unreachable branch hit, please file an issue on GitHub!");
    return NULL;
}

/*************************************************************************
 * Utilities                                                             *
 *************************************************************************/

static PyObject*
find_keyword(PyObject *kwnames, PyObject *const *kwstack, PyObject *key)
{
    Py_ssize_t i, nkwargs;

    nkwargs = PyTuple_GET_SIZE(kwnames);
    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* kwname == key will normally find a match in since keyword keys
           should be interned strings; if not retry below in a new loop. */
        if (kwname == key) {
            return kwstack[i];
        }
    }

    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);
        assert(PyUnicode_Check(kwname));
        if (_PyUnicode_EQ(kwname, key)) {
            return kwstack[i];
        }
    }
    return NULL;
}

static int
check_positional_nargs(Py_ssize_t nargs, Py_ssize_t min, Py_ssize_t max) {
    if (nargs > max) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra positional arguments provided"
        );
        return 0;
    }
    else if (nargs < min) {
        PyErr_Format(
            PyExc_TypeError,
            "Missing %zd required arguments",
            min - nargs
        );
        return 0;
    }
    return 1;
}

/* A utility incrementally building strings */
typedef struct strbuilder {
    char *sep;
    Py_ssize_t sep_size;
    char *buffer;
    Py_ssize_t size;  /* How many bytes have been written */
    Py_ssize_t capacity;  /* How many bytes can be written */
} strbuilder;

#define strbuilder_extend_literal(self, str) strbuilder_extend(self, str, sizeof(str) - 1)

static bool strbuilder_extend(strbuilder *self, const char *buf, Py_ssize_t nbytes) {
    bool is_first_write = self->size == 0;
    Py_ssize_t required = self->size + nbytes + self->sep_size;

    if (required > self->capacity) {
        self->capacity = required * 1.5;
        char *new_buf = PyMem_Realloc(self->buffer, self->capacity);
        if (new_buf == NULL) {
            PyMem_Free(self->buffer);
            self->buffer = NULL;
            return false;
        }
        self->buffer = new_buf;
    }
    if (self->sep_size && !is_first_write) {
        memcpy(self->buffer + self->size, self->sep, self->sep_size);
        self->size += self->sep_size;
    }
    memcpy(self->buffer + self->size, buf, nbytes);
    self->size += nbytes;
    return true;
}

static bool
strbuilder_extend_unicode(strbuilder *self, PyObject *obj) {
    Py_ssize_t size;
    const char* buf = unicode_str_and_size(obj, &size);
    if (buf == NULL) return false;
    return strbuilder_extend(self, buf, size);
}

static void
strbuilder_reset(strbuilder *self) {
    if (self->capacity != 0 && self->buffer != NULL) {
        PyMem_Free(self->buffer);
    }
    self->buffer = NULL;
    self->size = 0;
    self->capacity = 0;
}

static PyObject *
strbuilder_build(strbuilder *self) {
    PyObject *out = PyUnicode_FromStringAndSize(self->buffer, self->size);
    strbuilder_reset(self);
    return out;
}

/*************************************************************************
 * Lookup Tables for ints & strings                                      *
 *************************************************************************/

typedef struct Lookup {
    PyObject_VAR_HEAD
    PyObject *tag_field;  /* used for struct lookup table only */
    bool array_like;
    bool json_compatible;
} Lookup;

static PyTypeObject IntLookup_Type;
static PyTypeObject StrLookup_Type;

typedef struct IntLookup {
    Lookup common;
    bool compact;
} IntLookup;

typedef struct IntLookupEntry {
    int64_t key;
    PyObject *value;
} IntLookupEntry;

typedef struct IntLookupHashmap {
    IntLookup base;
    IntLookupEntry table[];
} IntLookupHashmap;

typedef struct IntLookupCompact {
    IntLookup base;
    int64_t offset;
    PyObject* table[];
} IntLookupCompact;

typedef struct StrLookupEntry {
    PyObject *key;
    PyObject *value;
} StrLookupEntry;

typedef struct StrLookup {
    Lookup common;
    StrLookupEntry table[];
} StrLookup;

#define Lookup_array_like(obj) ((Lookup *)(obj))->array_like
#define Lookup_json_compatible(obj) ((Lookup *)(obj))->json_compatible
#define Lookup_tag_field(obj) ((Lookup *)(obj))->tag_field
#define Lookup_IsStrLookup(obj) (Py_TYPE(obj) == &StrLookup_Type)
#define Lookup_IsIntLookup(obj) (Py_TYPE(obj) == &IntLookup_Type)

static IntLookupEntry *
_IntLookupHashmap_lookup(IntLookupHashmap *self, int64_t key) {
    IntLookupEntry *table = self->table;
    size_t mask = Py_SIZE(self) - 1;
    size_t i = key & mask;

    while (true) {
        IntLookupEntry *entry = &table[i];
        if (MS_LIKELY(entry->key == key)) return entry;
        if (entry->value == NULL) return entry;
        i = (i + 1) & mask;
    }
    /* Unreachable */
    return NULL;
}

static void
_IntLookupHashmap_Set(IntLookupHashmap *self, int64_t key, PyObject *value) {
    IntLookupEntry *entry = _IntLookupHashmap_lookup(self, key);
    Py_XDECREF(entry->value);
    Py_INCREF(value);
    entry->key = key;
    entry->value = value;
}

static PyObject *
IntLookup_New(PyObject *arg, PyObject *tag_field, bool array_like, bool json_compatible) {
    Py_ssize_t nitems;
    PyObject *item, *items = NULL;
    IntLookup *self = NULL;
    int64_t imin = LLONG_MAX, imax = LLONG_MIN;

    if (PyDict_CheckExact(arg)) {
        nitems = PyDict_GET_SIZE(arg);
    }
    else {
        items = PySequence_Tuple(arg);
        if (items == NULL) return NULL;
        nitems = PyTuple_GET_SIZE(items);
    }

    /* Must have at least one item */
    if (nitems == 0) {
        PyErr_Format(
            PyExc_TypeError,
            "Enum types must have at least one item, %R is invalid",
            arg
        );
        goto cleanup;
    }

    /* Find the min/max of items, and error if any item isn't an integer or is
     * out of range */
#define handle(key) \
    do { \
        int overflow = 0; \
        int64_t ival = PyLong_AsLongLongAndOverflow(key, &overflow); \
        if (overflow) { \
            PyErr_SetString( \
                PyExc_NotImplementedError, \
                "Integer values > (2**63 - 1) are not currently supported for " \
                "Enum/Literal/integer tags. If you need this feature, please " \
                "open an issue on GitHub." \
            ); \
            goto cleanup; \
        } \
        if (ival == -1 && PyErr_Occurred()) goto cleanup; \
        if (ival < imin) { \
            imin = ival; \
        } \
        if (ival > imax) { \
            imax = ival; \
        } \
    } while (false)
    if (PyDict_CheckExact(arg)) {
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(arg, &pos, &key, &val)) {
            handle(key);
        }
    }
    else {
        for (Py_ssize_t i = 0; i < nitems; i++) {
            handle(PyTuple_GET_ITEM(items, i));
        }
    }
#undef handle

    /* Calculate range without overflow */
    uint64_t range;
    if (imax > 0) {
        range = imax;
        range -= imin;
    }
    else {
        range = imax - imin;
    }

    if (range < 1.4 * nitems) {
        /* Use compact representation */
        size_t size = range + 1;

        /* XXX: In Python 3.11+ there's not an easy way to allocate an untyped
         * block of memory that is also tracked by the GC. To hack around this
         * we set `tp_itemsize = 1` for `IntLookup_Type`, and manually calculate
         * the size of trailing parts. It's gross, but it works. */
        size_t nextra = (
            sizeof(IntLookupCompact)
            + (size * sizeof(PyObject *))
            - sizeof(IntLookup)
        );
        IntLookupCompact *out = PyObject_GC_NewVar(
            IntLookupCompact, &IntLookup_Type, nextra
        );
        if (out == NULL) goto cleanup;
        /* XXX: overwrite `ob_size`, since we lied above */
        SET_SIZE(out, size);

        out->offset = imin;
        for (size_t i = 0; i < size; i++) {
            out->table[i] = NULL;
        }

#define setitem(key, val) \
    do { \
        int64_t ikey = PyLong_AsLongLong(key); \
        out->table[ikey - imin] = val; \
        Py_INCREF(val); \
    } while (false)

        if (PyDict_CheckExact(arg)) {
            PyObject *key, *val;
            Py_ssize_t pos = 0;
            while (PyDict_Next(arg, &pos, &key, &val)) {
                setitem(key, val);
            }
        }
        else {
            for (Py_ssize_t i = 0; i < nitems; i++) {
                item = PyTuple_GET_ITEM(items, i);
                setitem(item, item);
            }
        }

#undef setitem

        self = (IntLookup *)out;
        self->compact = true;
    }
    else {
        /* Use hashtable */
        size_t needed = nitems * 4 / 3;
        size_t size = 4;
        while (size < (size_t)needed) { size <<= 1; }

        /* XXX: This is hacky, see comment above allocating IntLookupCompact */
        size_t nextra = (
            sizeof(IntLookupHashmap)
            + (size * sizeof(IntLookupEntry))
            - sizeof(IntLookup)
        );
        IntLookupHashmap *out = PyObject_GC_NewVar(
            IntLookupHashmap, &IntLookup_Type, nextra
        );
        if (out == NULL) goto cleanup;
        /* XXX: overwrite `ob_size`, since we lied above */
        SET_SIZE(out, size);

        for (size_t i = 0; i < size; i++) {
            out->table[i].key = 0;
            out->table[i].value = NULL;
        }

        if (PyDict_CheckExact(arg)) {
            PyObject *key, *val;
            Py_ssize_t pos = 0;
            while (PyDict_Next(arg, &pos, &key, &val)) {
                int64_t ival = PyLong_AsLongLong(key);
                _IntLookupHashmap_Set(out, ival, val);
            }
        }
        else {
            for (Py_ssize_t i = 0; i < nitems; i++) {
                PyObject *val = PyTuple_GET_ITEM(items, i);
                int64_t ival = PyLong_AsLongLong(val);
                _IntLookupHashmap_Set(out, ival, val);
            }
        }
        self = (IntLookup *)out;
        self->compact = false;
    }

    /* Store extra metadata (struct lookup only) */
    Py_XINCREF(tag_field);
    self->common.tag_field = tag_field;
    self->common.array_like = array_like;
    self->common.json_compatible = json_compatible;

cleanup:
    Py_XDECREF(items);
    if (self != NULL) {
        PyObject_GC_Track(self);
    }
    return (PyObject *)self;
}

static int
IntLookup_traverse(IntLookup *self, visitproc visit, void *arg)
{
    if (self->compact) {
        IntLookupCompact *lk = (IntLookupCompact *)self;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            Py_VISIT(lk->table[i]);
        }
    }
    else {
        IntLookupHashmap *lk = (IntLookupHashmap *)self;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            Py_VISIT(lk->table[i].value);
        }
    }
    return 0;
}

static int
IntLookup_clear(IntLookup *self)
{
    if (self->compact) {
        IntLookupCompact *lk = (IntLookupCompact *)self;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            Py_CLEAR(lk->table[i]);
        }
    }
    else {
        IntLookupHashmap *lk = (IntLookupHashmap *)self;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            Py_CLEAR(lk->table[i].value);
        }
    }
    Py_CLEAR(self->common.tag_field);
    return 0;
}

static void
IntLookup_dealloc(IntLookup *self)
{
    PyObject_GC_UnTrack(self);
    IntLookup_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
IntLookup_GetInt64(IntLookup *self, int64_t key) {
    if (MS_LIKELY(self->compact)) {
        IntLookupCompact *lk = (IntLookupCompact *)self;
        Py_ssize_t index = key - lk->offset;
        if (index >= 0 && index < Py_SIZE(lk)) {
            return lk->table[index];
        }
        return NULL;
    }
    return _IntLookupHashmap_lookup((IntLookupHashmap *)self, key)->value;
}

static PyObject *
IntLookup_GetUInt64(IntLookup *self, uint64_t key) {
    if (key > LLONG_MAX) return NULL;
    return IntLookup_GetInt64(self, key);
}

static PyTypeObject IntLookup_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.IntLookup",
    .tp_basicsize = sizeof(IntLookup),
    .tp_itemsize = 1,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_dealloc = (destructor)IntLookup_dealloc,
    .tp_clear = (inquiry)IntLookup_clear,
    .tp_traverse = (traverseproc)IntLookup_traverse,
};

static StrLookupEntry *
_StrLookup_lookup(StrLookup *self, const char *key, Py_ssize_t size)
{
    StrLookupEntry *table = self->table;
    size_t hash = murmur2(key, size);
    size_t perturb = hash;
    size_t mask = Py_SIZE(self) - 1;
    size_t i = hash & mask;

    while (true) {
        StrLookupEntry *entry = &table[i];
        if (entry->value == NULL) return entry;
        Py_ssize_t entry_size;
        const char *entry_key = unicode_str_and_size_nocheck(entry->key, &entry_size);
        if (entry_size == size && memcmp(entry_key, key, size) == 0) return entry;
        /* Collision, perturb and try again */
        perturb >>= 5;
        i = mask & (i*5 + perturb + 1);
    }
    /* Unreachable */
    return NULL;
}

static int
StrLookup_Set(StrLookup *self, PyObject *key, PyObject *value) {
    Py_ssize_t key_size;
    const char *key_str = unicode_str_and_size(key, &key_size);
    if (key_str == NULL) return -1;

    StrLookupEntry *entry = _StrLookup_lookup(self, key_str, key_size);
    entry->key = key;
    Py_INCREF(key);
    entry->value = value;
    Py_INCREF(value);
    return 0;
}

static PyObject *
StrLookup_New(PyObject *arg, PyObject *tag_field, bool array_like, bool json_compatible) {
    Py_ssize_t nitems;
    PyObject *item, *items = NULL;
    StrLookup *self = NULL;

    if (PyDict_CheckExact(arg)) {
        nitems = PyDict_GET_SIZE(arg);
    }
    else {
        items = PySequence_Tuple(arg);
        if (items == NULL) return NULL;
        nitems = PyTuple_GET_SIZE(items);
    }

    /* Must have at least one item */
    if (nitems == 0) {
        PyErr_Format(
            PyExc_TypeError,
            "Enum types must have at least one item, %R is invalid",
            arg
        );
        goto cleanup;
    }

    size_t needed = nitems * 4 / 3;
    size_t size = 4;
    while (size < (size_t)needed) {
        size <<= 1;
    }
    self = PyObject_GC_NewVar(StrLookup, &StrLookup_Type, size);
    if (self == NULL) goto cleanup;
    /* Zero out memory */
    self->common.tag_field = NULL;
    for (size_t i = 0; i < size; i++) {
        self->table[i].key = NULL;
        self->table[i].value = NULL;
    }

    if (PyDict_CheckExact(arg)) {
        PyObject *key, *val;
        Py_ssize_t pos = 0;

        while (PyDict_Next(arg, &pos, &key, &val)) {
            if (!PyUnicode_CheckExact(key)) {
                PyErr_SetString(PyExc_RuntimeError, "Enum values must be strings");
                Py_CLEAR(self);
                goto cleanup;
            }
            if (StrLookup_Set(self, key, val) < 0) {
                Py_CLEAR(self);
                goto cleanup;
            }
        }
    }
    else {
        for (Py_ssize_t i = 0; i < nitems; i++) {
            item = PyTuple_GET_ITEM(items, i);
            if (!PyUnicode_CheckExact(item)) {
                PyErr_SetString(PyExc_RuntimeError, "Enum values must be strings");
                Py_CLEAR(self);
                goto cleanup;
            }
            if (StrLookup_Set(self, item, item) < 0) {
                Py_CLEAR(self);
                goto cleanup;
            }
        }
    }

    /* Store extra metadata (struct lookup only) */
    Py_XINCREF(tag_field);
    self->common.tag_field = tag_field;
    self->common.array_like = array_like;
    self->common.json_compatible = json_compatible;

cleanup:
    Py_XDECREF(items);
    if (self != NULL) {
        PyObject_GC_Track(self);
    }
    return (PyObject *)self;
}

static int
StrLookup_traverse(StrLookup *self, visitproc visit, void *arg)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        Py_VISIT(self->table[i].key);
        Py_VISIT(self->table[i].value);
    }
    return 0;
}

static int
StrLookup_clear(StrLookup *self)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        Py_CLEAR(self->table[i].key);
        Py_CLEAR(self->table[i].value);
    }
    Py_CLEAR(self->common.tag_field);
    return 0;
}

static void
StrLookup_dealloc(StrLookup *self)
{
    PyObject_GC_UnTrack(self);
    StrLookup_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
StrLookup_Get(StrLookup *self, const char *key, Py_ssize_t size) {
    StrLookupEntry *entry = _StrLookup_lookup(self, key, size);
    return entry->value;
}

static PyTypeObject StrLookup_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.StrLookup",
    .tp_basicsize = sizeof(StrLookup),
    .tp_itemsize = sizeof(StrLookupEntry),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_dealloc = (destructor) StrLookup_dealloc,
    .tp_clear = (inquiry)StrLookup_clear,
    .tp_traverse = (traverseproc) StrLookup_traverse,
};

/*************************************************************************
 * Raw                                                                   *
 *************************************************************************/

static PyTypeObject Raw_Type;

typedef struct Raw {
    PyObject_HEAD
    PyObject *base;
    char *buf;
    Py_ssize_t len;
    bool is_view;
} Raw;

static PyObject *
Raw_New(PyObject *msg) {
    Raw *out = (Raw *)Raw_Type.tp_alloc(&Raw_Type, 0);
    if (out == NULL) return NULL;
    if (PyBytes_CheckExact(msg)) {
        Py_INCREF(msg);
        out->base = msg;
        out->buf = PyBytes_AS_STRING(msg);
        out->len = PyBytes_GET_SIZE(msg);
        out->is_view = false;
    }
    else if (PyUnicode_CheckExact(msg)) {
        out->base = msg;
        out->buf = (char *)unicode_str_and_size(msg, &out->len);
        if (out->buf == NULL) return NULL;
        Py_INCREF(msg);
        out->is_view = false;
    }
    else {
        Py_buffer buffer;
        if (PyObject_GetBuffer(msg, &buffer, PyBUF_CONTIG_RO) < 0) {
            Py_DECREF(out);
            return NULL;
        }
        out->base = buffer.obj;
        out->buf = buffer.buf;
        out->len = buffer.len;
        out->is_view = true;
    }
    return (PyObject *)out;
}

PyDoc_STRVAR(Raw__doc__,
"Raw(msg="", /)\n"
"--\n"
"\n"
"A buffer containing an encoded message.\n"
"\n"
"Raw objects have two common uses:\n"
"\n"
"- During decoding. Fields annotated with the ``Raw`` type won't be decoded\n"
"  immediately, but will instead return a ``Raw`` object with a view into the\n"
"  original message where that field is encoded. This is useful for decoding\n"
"  fields whose type may only be inferred after decoding other fields.\n"
"- During encoding. Raw objects wrap pre-encoded messages. These can be added\n"
"  as components of larger messages without having to pay the cost of decoding\n"
"  and re-encoding them.\n"
"\n"
"Parameters\n"
"----------\n"
"msg: bytes, bytearray, memoryview, or str, optional\n"
"    A buffer containing an encoded message. One of bytes, bytearray, memoryview,\n"
"    str, or any object that implements the buffer protocol. If not present,\n"
"    defaults to an empty buffer."
);
static PyObject *
Raw_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    PyObject *msg;
    Py_ssize_t nargs, nkwargs;

    nargs = PyTuple_GET_SIZE(args);
    nkwargs = (kwargs == NULL) ? 0 : PyDict_GET_SIZE(kwargs);

    if (nkwargs != 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Raw takes no keyword arguments"
        );
        return NULL;
    }
    else if (nargs == 0) {
        msg = PyBytes_FromStringAndSize(NULL, 0);
        if (msg == NULL) return NULL;
        /* This looks weird, but is safe since the empty bytes object is an
         * immortal singleton */
        Py_DECREF(msg);
    }
    else if (nargs == 1) {
        msg = PyTuple_GET_ITEM(args, 0);
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "Raw expected at most 1 arguments, got %zd",
            nargs
        );
        return NULL;
    }
    return Raw_New(msg);
}

static void
Raw_dealloc(Raw *self)
{
    if (self->base != NULL) {
        if (!self->is_view) {
            Py_DECREF(self->base);
        }
        else {
            Py_buffer buffer;
            buffer.obj = self->base;
            buffer.len = self->len;
            buffer.buf = self->buf;
            PyBuffer_Release(&buffer);
        }
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
Raw_FromView(PyObject *buffer_obj, char *data, Py_ssize_t len) {
    Raw *out = (Raw *)Raw_Type.tp_alloc(&Raw_Type, 0);
    if (out == NULL) return NULL;

    Py_buffer buffer;
    if (PyObject_GetBuffer(buffer_obj, &buffer, PyBUF_CONTIG_RO) < 0) {
        Py_DECREF(out);
        return NULL;
    }
    out->base = buffer.obj;
    out->buf = data;
    out->len = len;
    out->is_view = true;
    return (PyObject *)out;
}

static PyObject *
Raw_richcompare(Raw *self, PyObject *other, int op) {
    if (Py_TYPE(other) != &Raw_Type) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    Raw *raw_other = (Raw *)other;
    bool equal = (
        self == raw_other || (
            (self->len == raw_other->len) &&
            (memcmp(self->buf, raw_other->buf, self->len) == 0)
        )
    );
    bool result = (op == Py_EQ) ? equal : !equal;
    if (result) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static int
Raw_buffer_getbuffer(Raw *self, Py_buffer *view, int flags)
{
    return PyBuffer_FillInfo(view, (PyObject *)self, self->buf, self->len, 1, flags);
}

static PyBufferProcs Raw_as_buffer = {
    .bf_getbuffer = (getbufferproc)Raw_buffer_getbuffer
};

static Py_ssize_t
Raw_length(Raw *self) {
    return self->len;
}

static PySequenceMethods Raw_as_sequence = {
    .sq_length = (lenfunc)Raw_length
};

static PyObject *
Raw_reduce(Raw *self, PyObject *unused)
{
    if (!self->is_view) {
        return Py_BuildValue("O(O)", &Raw_Type, self->base);
    }
    return Py_BuildValue("O(y#)", &Raw_Type, self->buf, self->len);
}

PyDoc_STRVAR(Raw_copy__doc__,
"copy(self)\n"
"--\n"
"\n"
"Copy a Raw object.\n"
"\n"
"If the raw message is backed by a memoryview into a larger buffer (as happens\n"
"during decoding), the message is copied and the reference to the larger buffer\n"
"released. This may be useful to reduce memory usage if a Raw object created\n"
"during decoding will be kept in memory for a while rather than immediately\n"
"decoded and dropped."
);
static PyObject *
Raw_copy(Raw *self, PyObject *unused)
{
    if (!self->is_view) {
        Py_INCREF(self);
        return (PyObject *)self;
    }
    PyObject *buf = PyBytes_FromStringAndSize(self->buf, self->len);
    if (buf == NULL) return NULL;
    return Raw_New(buf);
}

static PyMethodDef Raw_methods[] = {
    {"__reduce__", (PyCFunction)Raw_reduce, METH_NOARGS},
    {"copy", (PyCFunction)Raw_copy, METH_NOARGS, Raw_copy__doc__},
    {NULL, NULL},
};

static PyTypeObject Raw_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.Raw",
    .tp_doc = Raw__doc__,
    .tp_basicsize = sizeof(Raw),
    .tp_itemsize = sizeof(char),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Raw_new,
    .tp_dealloc = (destructor) Raw_dealloc,
    .tp_as_buffer = &Raw_as_buffer,
    .tp_as_sequence = &Raw_as_sequence,
    .tp_methods = Raw_methods,
    .tp_richcompare = (richcmpfunc) Raw_richcompare,
};

/*************************************************************************
 * Meta                                                                  *
 *************************************************************************/

static PyTypeObject Meta_Type;

typedef struct Meta {
    PyObject_HEAD
    PyObject *gt;
    PyObject *ge;
    PyObject *lt;
    PyObject *le;
    PyObject *multiple_of;
    PyObject *pattern;
    PyObject *regex;
    PyObject *min_length;
    PyObject *max_length;
    PyObject *tz;
    PyObject *title;
    PyObject *description;
    PyObject *examples;
    PyObject *extra_json_schema;
    PyObject *extra;
} Meta;

static bool
ensure_is_string(PyObject *val, const char *param) {
    if (PyUnicode_CheckExact(val)) return true;
    PyErr_Format(
        PyExc_TypeError,
        "`%s` must be a str, got %.200s",
        param, Py_TYPE(val)->tp_name
    );
    return false;
}

static bool
ensure_is_bool(PyObject *val, const char *param) {
    if (val == Py_True || val == Py_False) return true;
    PyErr_Format(
        PyExc_TypeError,
        "`%s` must be a bool, got %.200s",
        param, Py_TYPE(val)->tp_name
    );
    return false;
}

static bool
ensure_is_nonnegative_integer(PyObject *val, const char *param) {
    if (!PyLong_CheckExact(val)) {
        PyErr_Format(
            PyExc_TypeError,
            "`%s` must be an int, got %.200s",
            param, Py_TYPE(val)->tp_name
        );
        return false;
    }
    Py_ssize_t x = PyLong_AsSsize_t(val);
    if (x >= 0) return true;
    PyErr_Format(PyExc_ValueError, "`%s` must be >= 0, got %R", param, val);
    return false;
}

static bool
ensure_is_finite_numeric(PyObject *val, const char *param, bool positive) {
    double x;
    if (PyLong_CheckExact(val)) {
        x = PyLong_AsDouble(val);
    }
    else if (PyFloat_CheckExact(val)) {
        x = PyFloat_AS_DOUBLE(val);
        if (!isfinite(x)) {
            PyErr_Format(
                PyExc_ValueError,
                "`%s` must be finite, %R is not supported",
                param, val
            );
            return false;
        }
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "`%s` must be an int or float, got %.200s",
            param, Py_TYPE(val)->tp_name
        );
        return false;
    }
    if (positive && x <= 0) {
        PyErr_Format(PyExc_ValueError, "`%s` must be > 0", param);
        return false;
    }
    return true;
}

PyDoc_STRVAR(Meta__doc__,
"Meta(*, gt=None, ge=None, lt=None, le=None, multiple_of=None, pattern=None, "
"min_length=None, max_length=None, tz=None, title=None, description=None, "
"examples=None, extra_json_schema=None, extra=None)\n"
"--\n"
"\n"
"Extra metadata and constraints for a type or field.\n"
"\n"
"Parameters\n"
"----------\n"
"gt : int or float, optional\n"
"    The annotated value must be greater than ``gt``.\n"
"ge : int or float, optional\n"
"    The annotated value must be greater than or equal to ``ge``.\n"
"lt : int or float, optional\n"
"    The annotated value must be less than ``lt``.\n"
"le : int or float, optional\n"
"    The annotated value must be less than or equal to ``le``.\n"
"multiple_of : int or float, optional\n"
"    The annotated value must be a multiple of ``multiple_of``.\n"
"pattern : str, optional\n"
"    A regex pattern that the annotated value must match against. Note that\n"
"    the pattern is treated as **unanchored**, meaning the ``re.search``\n"
"    method is used when matching.\n"
"min_length: int, optional\n"
"    The annotated value must have a length greater than or equal to\n"
"    ``min_length``.\n"
"max_length: int, optional\n"
"    The annotated value must have a length less than or equal to\n"
"    ``max_length``.\n"
"tz: bool, optional\n"
"    Configures the timezone-requirements for annotated ``datetime``/``time``\n"
"    types. Set to ``True`` to require timezone-aware values, or ``False`` to\n"
"    require timezone-naive values. The default is ``None``, which accepts\n"
"    either timezone-aware or timezone-naive values.\n"
"title: str, optional\n"
"    The title to use for the annotated value when generating a json-schema.\n"
"description: str, optional\n"
"    The description to use for the annotated value when generating a\n"
"    json-schema.\n"
"examples: list, optional\n"
"    A list of examples to use for the annotated value when generating a\n"
"    json-schema.\n"
"extra_json_schema: dict, optional\n"
"    A dict of extra fields to set for the annotated value when generating\n"
"    a json-schema. This dict is recursively merged with the generated schema,\n"
"    with ``extra_json_schema`` overriding any conflicting autogenerated fields.\n"
"extra: dict, optional\n"
"    Any additional user-defined metadata.\n"
"\n"
"Examples\n"
"--------\n"
"Here we use ``Meta`` to add constraints on two different types. The first\n"
"defines a new type alias ``NonNegativeInt``, which is an integer that must be\n"
"``>= 0``. This type alias can be reused in multiple locations. The second uses\n"
"``Meta`` inline in a struct definition to restrict the ``name`` string field\n"
"to a maximum length of 32 characters.\n"
"\n"
">>> from typing import Annotated\n"
">>> from msgspec import Struct, Meta\n"
">>> NonNegativeInt = Annotated[int, Meta(ge=0)]\n"
">>> class User(Struct):\n"
"...     name: Annotated[str, Meta(max_length=32)]\n"
"...     age: NonNegativeInt\n"
"...\n"
">>> msgspec.json.decode(b'{\"name\": \"alice\", \"age\": 25}', type=User)\n"
"User(name='alice', age=25)\n"
);
static PyObject *
Meta_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    static char *kwlist[] = {
        "gt", "ge", "lt", "le", "multiple_of",
        "pattern", "min_length", "max_length", "tz",
        "title", "description", "examples", "extra_json_schema",
        "extra", NULL
    };
    PyObject *gt = NULL, *ge = NULL, *lt = NULL, *le = NULL, *multiple_of = NULL;
    PyObject *pattern = NULL, *min_length = NULL, *max_length = NULL, *tz = NULL;
    PyObject *title = NULL, *description = NULL, *examples = NULL;
    PyObject *extra_json_schema = NULL, *extra = NULL;
    PyObject *regex = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "|$OOOOOOOOOOOOOO:Meta.__new__", kwlist,
            &gt, &ge, &lt, &le, &multiple_of,
            &pattern, &min_length, &max_length, &tz,
            &title, &description, &examples, &extra_json_schema,
            &extra
        )
    )
        return NULL;

#define NONE_TO_NULL(x) do { if (x == Py_None) {x = NULL;} } while(0)
    NONE_TO_NULL(gt);
    NONE_TO_NULL(ge);
    NONE_TO_NULL(lt);
    NONE_TO_NULL(le);
    NONE_TO_NULL(multiple_of);
    NONE_TO_NULL(pattern);
    NONE_TO_NULL(min_length);
    NONE_TO_NULL(max_length);
    NONE_TO_NULL(tz);
    NONE_TO_NULL(title);
    NONE_TO_NULL(description);
    NONE_TO_NULL(examples);
    NONE_TO_NULL(extra_json_schema);
    NONE_TO_NULL(extra);
#undef NONE_TO_NULL

    /* Check parameter types/values */
    if (gt != NULL && !ensure_is_finite_numeric(gt, "gt", false)) return NULL;
    if (ge != NULL && !ensure_is_finite_numeric(ge, "ge", false)) return NULL;
    if (lt != NULL && !ensure_is_finite_numeric(lt, "lt", false)) return NULL;
    if (le != NULL && !ensure_is_finite_numeric(le, "le", false)) return NULL;
    if (multiple_of != NULL && !ensure_is_finite_numeric(multiple_of, "multiple_of", true)) return NULL;
    if (pattern != NULL && !ensure_is_string(pattern, "pattern")) return NULL;
    if (min_length != NULL && !ensure_is_nonnegative_integer(min_length, "min_length")) return NULL;
    if (max_length != NULL && !ensure_is_nonnegative_integer(max_length, "max_length")) return NULL;
    if (tz != NULL && !ensure_is_bool(tz, "tz")) return NULL;

    /* Check multiple constraint restrictions */
    if (gt != NULL && ge != NULL) {
        PyErr_SetString(PyExc_ValueError, "Cannot specify both `gt` and `ge`");
        return NULL;
    }
    if (lt != NULL && le != NULL) {
        PyErr_SetString(PyExc_ValueError, "Cannot specify both `lt` and `le`");
        return NULL;
    }
    bool numeric = (gt != NULL || ge != NULL || lt != NULL || le != NULL || multiple_of != NULL);
    bool other = (pattern != NULL || min_length != NULL || max_length != NULL || tz != NULL);
    if (numeric && other) {
        PyErr_SetString(
            PyExc_ValueError,
            "Cannot mix numeric constraints (gt, lt, ...) with non-numeric "
            "constraints (pattern, min_length, max_length, tz)"
        );
        return NULL;
    }

    /* Check types/values of extra metadata */
    if (title != NULL && !ensure_is_string(title, "title")) return NULL;
    if (description != NULL && !ensure_is_string(description, "description")) return NULL;
    if (examples != NULL && !PyList_CheckExact(examples)) {
        PyErr_Format(
            PyExc_TypeError,
            "`examples` must be a list, got %.200s",
            Py_TYPE(examples)->tp_name
        );
        return NULL;
    }
    if (extra_json_schema != NULL && !PyDict_CheckExact(extra_json_schema)) {
        PyErr_Format(
            PyExc_TypeError,
            "`extra_json_schema` must be a dict, got %.200s",
            Py_TYPE(extra_json_schema)->tp_name
        );
        return NULL;
    }
    if (extra != NULL && !PyDict_CheckExact(extra)) {
        PyErr_Format(
            PyExc_TypeError,
            "`extra` must be a dict, got %.200s",
            Py_TYPE(extra)->tp_name
        );
        return NULL;
    }

    /* regex compile pattern if provided */
    if (pattern != NULL) {
        MsgspecState *mod = msgspec_get_global_state();
        regex = CALL_ONE_ARG(mod->re_compile, pattern);
        if (regex == NULL) return NULL;
    }

    Meta *out = (Meta *)Meta_Type.tp_alloc(&Meta_Type, 0);
    if (out == NULL) return NULL;

#define SET_FIELD(x) do { Py_XINCREF(x); out->x = x; } while(0)
    SET_FIELD(gt);
    SET_FIELD(ge);
    SET_FIELD(lt);
    SET_FIELD(le);
    SET_FIELD(multiple_of);
    SET_FIELD(pattern);
    SET_FIELD(regex);
    SET_FIELD(min_length);
    SET_FIELD(max_length);
    SET_FIELD(tz);
    SET_FIELD(title);
    SET_FIELD(description);
    SET_FIELD(examples);
    SET_FIELD(extra_json_schema);
    SET_FIELD(extra);
#undef SET_FIELD
    return (PyObject *)out;
}

static int
Meta_traverse(Meta *self, visitproc visit, void *arg) {
    Py_VISIT(self->regex);
    Py_VISIT(self->examples);
    Py_VISIT(self->extra_json_schema);
    Py_VISIT(self->extra);
    return 0;
}

static void
Meta_clear(Meta *self) {
    Py_CLEAR(self->gt);
    Py_CLEAR(self->ge);
    Py_CLEAR(self->lt);
    Py_CLEAR(self->le);
    Py_CLEAR(self->multiple_of);
    Py_CLEAR(self->pattern);
    Py_CLEAR(self->regex);
    Py_CLEAR(self->min_length);
    Py_CLEAR(self->max_length);
    Py_CLEAR(self->tz);
    Py_CLEAR(self->title);
    Py_CLEAR(self->description);
    Py_CLEAR(self->examples);
    Py_CLEAR(self->extra_json_schema);
    Py_CLEAR(self->extra);
}

static void
Meta_dealloc(Meta *self) {
    PyObject_GC_UnTrack(self);
    Meta_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static bool
_meta_repr_part(strbuilder *builder, const char *prefix, Py_ssize_t prefix_size, PyObject *field, bool *first) {
    if (*first) {
        *first = false;
    }
    else {
        if (!strbuilder_extend_literal(builder, ", ")) return false;
    }
    if (!strbuilder_extend(builder, prefix, prefix_size)) return false;
    PyObject *repr = PyObject_Repr(field);
    if (repr == NULL) return false;
    bool ok = strbuilder_extend_unicode(builder, repr);
    Py_DECREF(repr);
    return ok;
}

static PyObject *
Meta_repr(Meta *self) {
    strbuilder builder = {0};
    bool first = true;
    if (!strbuilder_extend_literal(&builder, "msgspec.Meta(")) return NULL;
    /* sizeof(#field) is the length of the field name + 1 (null terminator). We
     * want the length of field name + 1 (for the `=`). */
#define DO_REPR(field) do { \
    if (self->field != NULL) { \
        if (!_meta_repr_part(&builder, #field "=", sizeof(#field), self->field, &first)) { \
            goto error; \
        } \
    } \
} while(0)
    DO_REPR(gt);
    DO_REPR(ge);
    DO_REPR(lt);
    DO_REPR(le);
    DO_REPR(multiple_of);
    DO_REPR(pattern);
    DO_REPR(min_length);
    DO_REPR(max_length);
    DO_REPR(tz);
    DO_REPR(title);
    DO_REPR(description);
    DO_REPR(examples);
    DO_REPR(extra_json_schema);
    DO_REPR(extra);
#undef DO_REPR
    if (!strbuilder_extend_literal(&builder, ")")) goto error;
    return strbuilder_build(&builder);
error:
    strbuilder_reset(&builder);
    return NULL;
}

static PyObject *
Meta_rich_repr(PyObject *py_self, PyObject *args) {
    Meta *self = (Meta *)py_self;
    PyObject *out = PyList_New(0);
    if (out == NULL) goto error;
#define DO_REPR(field) do { \
    if (self->field != NULL) { \
        PyObject *part = Py_BuildValue("(UO)", #field, self->field); \
        if (part == NULL || (PyList_Append(out, part) < 0)) goto error;\
    } } while(0)
    DO_REPR(gt);
    DO_REPR(ge);
    DO_REPR(lt);
    DO_REPR(le);
    DO_REPR(multiple_of);
    DO_REPR(pattern);
    DO_REPR(min_length);
    DO_REPR(max_length);
    DO_REPR(tz);
    DO_REPR(title);
    DO_REPR(description);
    DO_REPR(examples);
    DO_REPR(extra_json_schema);
    DO_REPR(extra);
#undef DO_REPR
    return out;
error:
    Py_XDECREF(out);
    return NULL;
}

static int
_meta_richcompare_part(PyObject *left, PyObject *right) {
    if ((left == NULL) != (right == NULL)) {
        return 0;
    }
    if (left != NULL) {
        return PyObject_RichCompareBool(left, right, Py_EQ);
    }
    else {
        return 1;
    }
}

static PyObject *
Meta_richcompare(Meta *self, PyObject *py_other, int op) {
    int equal = 1;
    PyObject *out;

    if (Py_TYPE(py_other) != &Meta_Type) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (!(op == Py_EQ || op == Py_NE)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    Meta *other = (Meta *)py_other;

    /* Only need to loop if self is not other` */
    if (MS_LIKELY(self != other)) {
#define DO_COMPARE(field) do { \
        equal = _meta_richcompare_part(self->field, other->field); \
        if (equal < 0) return NULL; \
        if (!equal) goto done; \
    } while (0)
        DO_COMPARE(gt);
        DO_COMPARE(ge);
        DO_COMPARE(lt);
        DO_COMPARE(le);
        DO_COMPARE(multiple_of);
        DO_COMPARE(pattern);
        DO_COMPARE(min_length);
        DO_COMPARE(max_length);
        DO_COMPARE(tz);
        DO_COMPARE(title);
        DO_COMPARE(description);
        DO_COMPARE(examples);
        DO_COMPARE(extra_json_schema);
        DO_COMPARE(extra);
    }
#undef DO_COMPARE
done:
    if (op == Py_EQ) {
        out = equal ? Py_True : Py_False;
    }
    else {
        out = (!equal) ? Py_True : Py_False;
    }
    Py_INCREF(out);
    return out;
}

static Py_hash_t
Meta_hash(Meta *self) {
    Py_ssize_t nfields = 0;
    Py_uhash_t acc = MS_HASH_XXPRIME_5;

#define DO_HASH(field) \
    if (self->field != NULL) { \
        Py_uhash_t lane = PyObject_Hash(self->field); \
        if (lane == (Py_uhash_t)-1) return -1; \
        acc += lane * MS_HASH_XXPRIME_2; \
        acc = MS_HASH_XXROTATE(acc); \
        acc *= MS_HASH_XXPRIME_1; \
        nfields += 1; \
    }
    DO_HASH(gt);
    DO_HASH(ge);
    DO_HASH(lt);
    DO_HASH(le);
    DO_HASH(multiple_of);
    DO_HASH(pattern);
    DO_HASH(min_length);
    DO_HASH(max_length);
    DO_HASH(tz);
    DO_HASH(title);
    DO_HASH(description);
    /* Leave out examples & description, since they could be unhashable */
#undef DO_HASH
    acc += nfields ^ (MS_HASH_XXPRIME_5 ^ 3527539UL);
    return (acc == (Py_uhash_t)-1) ?  1546275796 : acc;
}

static PyMethodDef Meta_methods[] = {
    {"__rich_repr__", Meta_rich_repr, METH_NOARGS, "rich repr"},
    {NULL, NULL},
};

static PyMemberDef Meta_members[] = {
    {"gt", T_OBJECT, offsetof(Meta, gt), READONLY, NULL},
    {"ge", T_OBJECT, offsetof(Meta, ge), READONLY, NULL},
    {"lt", T_OBJECT, offsetof(Meta, lt), READONLY, NULL},
    {"le", T_OBJECT, offsetof(Meta, le), READONLY, NULL},
    {"multiple_of", T_OBJECT, offsetof(Meta, multiple_of), READONLY, NULL},
    {"pattern", T_OBJECT, offsetof(Meta, pattern), READONLY, NULL},
    {"min_length", T_OBJECT, offsetof(Meta, min_length), READONLY, NULL},
    {"max_length", T_OBJECT, offsetof(Meta, max_length), READONLY, NULL},
    {"tz", T_OBJECT, offsetof(Meta, tz), READONLY, NULL},
    {"title", T_OBJECT, offsetof(Meta, title), READONLY, NULL},
    {"description", T_OBJECT, offsetof(Meta, description), READONLY, NULL},
    {"examples", T_OBJECT, offsetof(Meta, examples), READONLY, NULL},
    {"extra_json_schema", T_OBJECT, offsetof(Meta, extra_json_schema), READONLY, NULL},
    {"extra", T_OBJECT, offsetof(Meta, extra), READONLY, NULL},
    {NULL},
};

static PyTypeObject Meta_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.Meta",
    .tp_doc = Meta__doc__,
    .tp_basicsize = sizeof(Meta),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = Meta_new,
    .tp_traverse = (traverseproc) Meta_traverse,
    .tp_clear = (inquiry) Meta_clear,
    .tp_dealloc = (destructor) Meta_dealloc,
    .tp_methods = Meta_methods,
    .tp_members = Meta_members,
    .tp_repr = (reprfunc) Meta_repr,
    .tp_richcompare = (richcmpfunc) Meta_richcompare,
    .tp_hash = (hashfunc) Meta_hash
};

/*************************************************************************
 * nodefault singleton                                                   *
 *************************************************************************/

PyObject _NoDefault_Object;
#define NODEFAULT &_NoDefault_Object

static PyObject *
nodefault_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) || (kwargs && PyDict_GET_SIZE(kwargs))) {
        PyErr_SetString(PyExc_TypeError, "NoDefault takes no arguments");
        return NULL;
    }
    Py_INCREF(NODEFAULT);
    return NODEFAULT;
}

static PyObject *
nodefault_repr(PyObject *op)
{
    return PyUnicode_FromString("nodefault");
}

static PyObject *
nodefault_reduce(PyObject *op, PyObject *args)
{
    return PyUnicode_FromString("nodefault");
}

static PyMethodDef nodefault_methods[] = {
    {"__reduce__", nodefault_reduce, METH_NOARGS, NULL},
    {NULL, NULL}
};

PyTypeObject NoDefault_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.NoDefault",
    .tp_repr = nodefault_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = nodefault_methods,
    .tp_new = nodefault_new,
    .tp_dealloc = 0,
    .tp_itemsize = 0,
    .tp_basicsize = 0
};

PyObject _NoDefault_Object = {1, &NoDefault_Type};

/*************************************************************************
 * UNSET singleton                                                       *
 *************************************************************************/

PyObject _Unset_Object;
#define UNSET &_Unset_Object

PyDoc_STRVAR(Unset__doc__,
"Unset()\n"
"--\n"
"\n"
"A singleton indicating a value is unset."
);
static PyObject *
unset_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) || (kwargs && PyDict_GET_SIZE(kwargs))) {
        PyErr_SetString(PyExc_TypeError, "Unset takes no arguments");
        return NULL;
    }
    Py_INCREF(UNSET);
    return UNSET;
}

static PyObject *
unset_repr(PyObject *op)
{
    return PyUnicode_FromString("UNSET");
}

static PyObject *
unset_reduce(PyObject *op, PyObject *args)
{
    return PyUnicode_FromString("UNSET");
}

static PyMethodDef unset_methods[] = {
    {"__reduce__", unset_reduce, METH_NOARGS, NULL},
    {NULL, NULL}
};

PyTypeObject Unset_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.Unset",
    .tp_doc = Unset__doc__,
    .tp_repr = unset_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = unset_methods,
    .tp_new = unset_new,
    .tp_dealloc = 0,
    .tp_itemsize = 0,
    .tp_basicsize = 0
};

PyObject _Unset_Object = {1, &Unset_Type};

/*************************************************************************
 * Factory                                                               *
 *************************************************************************/

static PyTypeObject Factory_Type;

typedef struct {
    PyObject_HEAD
    PyObject *factory;
} Factory;

static PyObject *
Factory_New(PyObject *factory) {
    if (!PyCallable_Check(factory)) {
        PyErr_SetString(PyExc_TypeError, "default_factory must be callable");
        return NULL;
    }

    Factory *out = (Factory *)Factory_Type.tp_alloc(&Factory_Type, 0);
    if (out == NULL) return NULL;

    Py_INCREF(factory);
    out->factory = factory;
    return (PyObject *)out;
}

static PyObject *
Factory_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    Py_ssize_t nkwargs = (kwargs == NULL) ? 0 : PyDict_GET_SIZE(kwargs);
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    if (nkwargs != 0) {
        PyErr_SetString(PyExc_TypeError, "Factory takes no keyword arguments");
        return NULL;
    }
    else if (nargs != 1) {
        PyErr_Format(
            PyExc_TypeError,
            "Factory expected 1 argument, got %zd",
            nargs
        );
        return NULL;
    }
    else {
        return Factory_New(PyTuple_GET_ITEM(args, 0));
    }
}

static PyObject *
Factory_Call(PyObject *self) {
    PyObject *factory = ((Factory *)(self))->factory;
    /* Inline two common factory types */
    if (factory == (PyObject *)(&PyList_Type)) {
        return PyList_New(0);
    }
    else if (factory == (PyObject *)(&PyDict_Type)) {
        return PyDict_New();
    }
    return CALL_NO_ARGS(factory);
}

static PyObject *
Factory_repr(PyObject *op)
{
    return PyUnicode_FromString("<factory>");
}

static int
Factory_traverse(Factory *self, visitproc visit, void *arg)
{
    Py_VISIT(self->factory);
    return 0;
}

static int
Factory_clear(Factory *self)
{
    Py_CLEAR(self->factory);
    return 0;
}

static void
Factory_dealloc(Factory *self)
{
    PyObject_GC_UnTrack(self);
    Factory_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Factory_members[] = {
    {"factory", T_OBJECT_EX, offsetof(Factory, factory), READONLY, "The factory function"},
    {NULL},
};

static PyTypeObject Factory_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.Factory",
    .tp_basicsize = sizeof(Factory),
    .tp_itemsize = 0,
    .tp_new = Factory_new,
    .tp_repr = Factory_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)Factory_clear,
    .tp_traverse = (traverseproc)Factory_traverse,
    .tp_dealloc = (destructor) Factory_dealloc,
    .tp_members = Factory_members,
};

/*************************************************************************
 * Field                                                                 *
 *************************************************************************/

static PyTypeObject Field_Type;

typedef struct {
    PyObject_HEAD
    PyObject *default_value;
    PyObject *default_factory;
} Field;

PyDoc_STRVAR(Field__doc__,
"Configuration for a Struct field.\n"
"\n"
"Parameters\n"
"----------\n"
"default : Any, optional\n"
"    A default value to use for this field.\n"
"default_factory : callable, optional\n"
"    A zero-argument function called to generate a new default value\n"
"    per-instance, rather than using a constant value as in ``default``."
);
static PyObject *
Field_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    static char *kwlist[] = {"default", "default_factory", NULL};
    PyObject *default_value = UNSET, *default_factory = UNSET;

    if (
        !PyArg_ParseTupleAndKeywords(
            args, kwargs, "|$OO", kwlist,
            &default_value, &default_factory
        )
    ) {
        return NULL;
    }
    if (default_value != UNSET && default_factory != UNSET) {
        PyErr_SetString(
            PyExc_TypeError, "Cannot set both `default` and `default_factory`"
        );
        return NULL;
    }
    if (default_factory != UNSET) {
        if (!PyCallable_Check(default_factory)) {
            PyErr_SetString(PyExc_TypeError, "default_factory must be callable");
            return NULL;
        }
    }

    Field *self = (Field *)Field_Type.tp_alloc(&Field_Type, 0);
    if (self == NULL) return NULL;
    Py_INCREF(default_value);
    self->default_value = default_value;
    Py_INCREF(default_factory);
    self->default_factory = default_factory;
    return (PyObject *)self;
}

static int
Field_traverse(Field *self, visitproc visit, void *arg)
{
    Py_VISIT(self->default_value);
    Py_VISIT(self->default_factory);
    return 0;
}

static int
Field_clear(Field *self)
{
    Py_CLEAR(self->default_value);
    Py_CLEAR(self->default_factory);
    return 0;
}

static void
Field_dealloc(Field *self)
{
    PyObject_GC_UnTrack(self);
    Field_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Field_members[] = {
    {"default", T_OBJECT_EX, offsetof(Field, default_value), READONLY, "The default value, or UNSET if unset"},
    {"default_factory", T_OBJECT_EX, offsetof(Field, default_factory), READONLY, "The default_factory, or UNSET if unset"},
    {NULL},
};

static PyTypeObject Field_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.Field",
    .tp_doc = Field__doc__,
    .tp_basicsize = sizeof(Field),
    .tp_itemsize = 0,
    .tp_new = Field_new,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)Field_clear,
    .tp_traverse = (traverseproc)Field_traverse,
    .tp_dealloc = (destructor) Field_dealloc,
    .tp_members = Field_members,
};

/*************************************************************************
 * Struct, PathNode, and TypeNode Types                                  *
 *************************************************************************/

/* Types */
#define MS_TYPE_ANY                 (1ull << 0)
#define MS_TYPE_NONE                (1ull << 1)
#define MS_TYPE_BOOL                (1ull << 2)
#define MS_TYPE_INT                 (1ull << 3)
#define MS_TYPE_FLOAT               (1ull << 4)
#define MS_TYPE_STR                 (1ull << 5)
#define MS_TYPE_BYTES               (1ull << 6)
#define MS_TYPE_BYTEARRAY           (1ull << 7)
#define MS_TYPE_DATETIME            (1ull << 8)
#define MS_TYPE_DATE                (1ull << 9)
#define MS_TYPE_TIME                (1ull << 10)
#define MS_TYPE_UUID                (1ull << 11)
#define MS_TYPE_EXT                 (1ull << 12)
#define MS_TYPE_STRUCT              (1ull << 13)
#define MS_TYPE_STRUCT_ARRAY        (1ull << 14)
#define MS_TYPE_STRUCT_UNION        (1ull << 15)
#define MS_TYPE_STRUCT_ARRAY_UNION  (1ull << 16)
#define MS_TYPE_ENUM                (1ull << 17)
#define MS_TYPE_INTENUM             (1ull << 18)
#define MS_TYPE_CUSTOM              (1ull << 19)
#define MS_TYPE_CUSTOM_GENERIC      (1ull << 20)
#define MS_TYPE_DICT                ((1ull << 21) | (1ull << 22))
#define MS_TYPE_LIST                (1ull << 23)
#define MS_TYPE_SET                 (1ull << 24)
#define MS_TYPE_FROZENSET           (1ull << 25)
#define MS_TYPE_VARTUPLE            (1ull << 26)
#define MS_TYPE_FIXTUPLE            (1ull << 27)
#define MS_TYPE_INTLITERAL          (1ull << 28)
#define MS_TYPE_STRLITERAL          (1ull << 29)
#define MS_TYPE_TYPEDDICT           (1ull << 30)
#define MS_TYPE_DATACLASS           (1ull << 31)
#define MS_TYPE_NAMEDTUPLE          (1ull << 32)
#define MS_TYPE_DECIMAL             (1ull << 33)
/* Constraints */
#define MS_CONSTR_INT_MIN           (1ull << 42)
#define MS_CONSTR_INT_MAX           (1ull << 43)
#define MS_CONSTR_INT_MULTIPLE_OF   (1ull << 44)
#define MS_CONSTR_FLOAT_GT          (1ull << 45)
#define MS_CONSTR_FLOAT_GE          (1ull << 46)
#define MS_CONSTR_FLOAT_LT          (1ull << 47)
#define MS_CONSTR_FLOAT_LE          (1ull << 48)
#define MS_CONSTR_FLOAT_MULTIPLE_OF (1ull << 49)
#define MS_CONSTR_STR_REGEX         (1ull << 50)
#define MS_CONSTR_STR_MIN_LENGTH    (1ull << 51)
#define MS_CONSTR_STR_MAX_LENGTH    (1ull << 52)
#define MS_CONSTR_BYTES_MIN_LENGTH  (1ull << 53)
#define MS_CONSTR_BYTES_MAX_LENGTH  (1ull << 54)
#define MS_CONSTR_ARRAY_MIN_LENGTH  (1ull << 55)
#define MS_CONSTR_ARRAY_MAX_LENGTH  (1ull << 56)
#define MS_CONSTR_MAP_MIN_LENGTH    (1ull << 57)
#define MS_CONSTR_MAP_MAX_LENGTH    (1ull << 58)
#define MS_CONSTR_TZ_AWARE          (1ull << 59)
#define MS_CONSTR_TZ_NAIVE          (1ull << 60)
/* Extra flag bit, used by TypedDict/dataclass implementations */
#define MS_EXTRA_FLAG               (1ull << 63)

/* A TypeNode encodes information about all types at the same hierarchy in the
 * type tree. They can encode both single types (`int`) and unions of types
 * (`int | float | list[int]`). The encoding is optimized for the common case
 * of simple scalar types (or unions of these types) with no constraints -
 * these values only require a single uint64_t. More complicated types require
 * extra *details* (`TypeDetail` objects) stored in a variable length array.
 *
 * The encoding is *compressed* - only fields that are set are stored. To know
 * which fields are set, a bitmask of `types` is used, masking both the types
 * and constraints set on the node.
 *
 * The order these details are stored is consistent, allowing the offset of a
 * field to be computed using an efficient bitmask and popcount.
 *
 * The order is documented below:
 *
 * O | STRUCT | STRUCT_ARRAY | STRUCT_UNION | STRUCT_ARRAY_UNION | CUSTOM |
 * O | INTENUM | INTLITERAL |
 * O | ENUM | STRLITERAL |
 * O | TYPEDDICT | DATACLASS |
 * O | NAMEDTUPLE |
 * O | STR_REGEX |
 * T | DICT [key, value] |
 * T | LIST | SET | FROZENSET | VARTUPLE |
 * I | INT_MIN |
 * I | INT_MAX |
 * I | INT_MULTIPLE_OF |
 * F | FLOAT_GT | FLOAT_GE |
 * F | FLOAT_LT | FLOAT_LE |
 * F | FLOAT_MULTIPLE_OF |
 * S | STR_MIN_LENGTH |
 * S | STR_MAX_LENGTH |
 * S | BYTES_MIN_LENGTH |
 * S | BYTES_MAX_LENGTH |
 * S | ARRAY_MIN_LENGTH |
 * S | ARRAY_MAX_LENGTH |
 * S | MAP_MIN_LENGTH |
 * S | MAP_MAX_LENGTH |
 * T | FIXTUPLE [size, types ...] |
 * */

#define SLOT_00 ( \
    MS_TYPE_STRUCT | MS_TYPE_STRUCT_ARRAY | \
    MS_TYPE_STRUCT_UNION | MS_TYPE_STRUCT_ARRAY_UNION | \
    MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC \
)
#define SLOT_01 (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)
#define SLOT_02 (MS_TYPE_ENUM | MS_TYPE_STRLITERAL)
#define SLOT_03 (MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS)
#define SLOT_04 MS_TYPE_NAMEDTUPLE
#define SLOT_05 MS_CONSTR_STR_REGEX
#define SLOT_06 MS_TYPE_DICT
#define SLOT_07 (MS_TYPE_LIST | MS_TYPE_VARTUPLE | MS_TYPE_SET | MS_TYPE_FROZENSET)
#define SLOT_08 MS_CONSTR_INT_MIN
#define SLOT_09 MS_CONSTR_INT_MAX
#define SLOT_10 MS_CONSTR_INT_MULTIPLE_OF
#define SLOT_11 (MS_CONSTR_FLOAT_GE | MS_CONSTR_FLOAT_GT)
#define SLOT_12 (MS_CONSTR_FLOAT_LE | MS_CONSTR_FLOAT_LT)
#define SLOT_13 MS_CONSTR_FLOAT_MULTIPLE_OF
#define SLOT_14 MS_CONSTR_STR_MIN_LENGTH
#define SLOT_15 MS_CONSTR_STR_MAX_LENGTH
#define SLOT_16 MS_CONSTR_BYTES_MIN_LENGTH
#define SLOT_17 MS_CONSTR_BYTES_MAX_LENGTH
#define SLOT_18 MS_CONSTR_ARRAY_MIN_LENGTH
#define SLOT_19 MS_CONSTR_ARRAY_MAX_LENGTH
#define SLOT_20 MS_CONSTR_MAP_MIN_LENGTH
#define SLOT_21 MS_CONSTR_MAP_MAX_LENGTH

/* Common groups */
#define MS_INT_CONSTRS (SLOT_08 | SLOT_09 | SLOT_10)
#define MS_FLOAT_CONSTRS (SLOT_11 | SLOT_12 | SLOT_13)
#define MS_STR_CONSTRS (SLOT_05 | SLOT_14 | SLOT_15)
#define MS_BYTES_CONSTRS (SLOT_16 | SLOT_17)
#define MS_ARRAY_CONSTRS (SLOT_18 | SLOT_19)
#define MS_MAP_CONSTRS (SLOT_20 | SLOT_21)
#define MS_TIME_CONSTRS (MS_CONSTR_TZ_AWARE | MS_CONSTR_TZ_NAIVE)

typedef union TypeDetail {
    int64_t i64;
    double f64;
    Py_ssize_t py_ssize_t;
    void *pointer;
} TypeDetail;

typedef struct TypeNode {
    uint64_t types;
    TypeDetail details[];
} TypeNode;

/* A simple extension of TypeNode to allow for static allocation */
typedef struct {
    uint64_t types;
    TypeDetail details[1];
} TypeNodeSimple;

typedef struct {
    PyObject *key;
    TypeNode *type;
} TypedDictField;

typedef struct {
    PyObject_VAR_HEAD
    Py_ssize_t nrequired;
    bool json_compatible;
    TypedDictField fields[];
} TypedDictInfo;

typedef struct {
    PyObject *key;
    TypeNode *type;
} DataclassField;

typedef struct DataclassInfo {
    PyObject_VAR_HEAD
    bool json_compatible;
    bool traversing;
    bool has_post_init;
    PyObject *class;
    PyObject *defaults;
    DataclassField fields[];
} DataclassInfo;

typedef struct {
    PyObject_VAR_HEAD
    bool json_compatible;
    bool traversing;
    PyObject *class;
    PyObject *defaults;
    TypeNode *types[];
} NamedTupleInfo;

typedef struct {
    PyHeapTypeObject base;
    PyObject *struct_fields;
    PyObject *struct_defaults;
    Py_ssize_t *struct_offsets;
    PyObject *struct_encode_fields;
    TypeNode **struct_types;
    Py_ssize_t nkwonly;
    Py_ssize_t n_trailing_defaults;
    PyObject *struct_tag_field;  /* str or NULL */
    PyObject *struct_tag_value;  /* str or NULL */
    PyObject *struct_tag;        /* True, str, or NULL */
    PyObject *match_args;
    PyObject *rename;
    bool json_compatible;
    bool traversing;
    int8_t frozen;
    int8_t order;
    int8_t eq;
    int8_t array_like;
    int8_t gc;
    int8_t omit_defaults;
    int8_t forbid_unknown_fields;
} StructMetaObject;

static PyTypeObject TypedDictInfo_Type;
static PyTypeObject DataclassInfo_Type;
static PyTypeObject NamedTupleInfo_Type;
static PyTypeObject StructMetaType;
static PyTypeObject Ext_Type;
static TypeNode* TypeNode_Convert(PyObject *type, bool, bool *);
static int StructMeta_prep_types(PyObject*, bool, bool*);
static PyObject* TypedDictInfo_Convert(PyObject*, bool, bool*);
static PyObject* DataclassInfo_Convert(PyObject*, bool, bool*);
static PyObject* NamedTupleInfo_Convert(PyObject*, bool, bool*);

#define StructMeta_GET_FIELDS(s) (((StructMetaObject *)(s))->struct_fields)
#define StructMeta_GET_NFIELDS(s) (PyTuple_GET_SIZE((((StructMetaObject *)(s))->struct_fields)))
#define StructMeta_GET_DEFAULTS(s) (((StructMetaObject *)(s))->struct_defaults)
#define StructMeta_GET_OFFSETS(s) (((StructMetaObject *)(s))->struct_offsets)

#define OPT_UNSET -1
#define OPT_FALSE 0
#define OPT_TRUE 1
#define STRUCT_MERGE_OPTIONS(opt1, opt2) (((opt2) != OPT_UNSET) ? (opt2) : (opt1))

static MS_INLINE StructMetaObject *
TypeNode_get_struct(TypeNode *type) {
    /* Struct types are always first */
    return type->details[0].pointer;
}

static MS_INLINE Lookup *
TypeNode_get_struct_union(TypeNode *type) {
    /* Struct union types are always first */
    return type->details[0].pointer;
}

static MS_INLINE PyObject *
TypeNode_get_custom(TypeNode *type) {
    /* Custom types can't be mixed with anything */
    return type->details[0].pointer;
}

static MS_INLINE IntLookup *
TypeNode_get_int_enum_or_literal(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & SLOT_00);
    return type->details[i].pointer;
}

static MS_INLINE StrLookup *
TypeNode_get_str_enum_or_literal(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (SLOT_00 | SLOT_01));
    return type->details[i].pointer;
}

static MS_INLINE TypedDictInfo *
TypeNode_get_typeddict_info(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (SLOT_00 | SLOT_01 | SLOT_02));
    return type->details[i].pointer;
}

static MS_INLINE DataclassInfo *
TypeNode_get_dataclass_info(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (SLOT_00 | SLOT_01 | SLOT_02));
    return type->details[i].pointer;
}

static MS_INLINE NamedTupleInfo *
TypeNode_get_namedtuple_info(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03
        )
    );
    return type->details[i].pointer;
}

static MS_INLINE PyObject *
TypeNode_get_constr_str_regex(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04
        )
    );
    return type->details[i].pointer;
}

static MS_INLINE void
TypeNode_get_dict(TypeNode *type, TypeNode **key, TypeNode **val) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05
        )
    );
    *key = type->details[i].pointer;
    *val = type->details[i + 1].pointer;
}

static MS_INLINE TypeNode *
TypeNode_get_array(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06
        )
    );
    return type->details[i].pointer;
}

static MS_INLINE int64_t
TypeNode_get_constr_int_min(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07
        )
    );
    return type->details[i].i64;
}

static MS_INLINE int64_t
TypeNode_get_constr_int_max(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08
        )
    );
    return type->details[i].i64;
}

static MS_INLINE int64_t
TypeNode_get_constr_int_multiple_of(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09
        )
    );
    return type->details[i].i64;
}

static MS_INLINE double
TypeNode_get_constr_float_min(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10
        )
    );
    return type->details[i].f64;
}

static MS_INLINE double
TypeNode_get_constr_float_max(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11
        )
    );
    return type->details[i].f64;
}

static MS_INLINE double
TypeNode_get_constr_float_multiple_of(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12
        )
    );
    return type->details[i].f64;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_str_min_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_str_max_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_bytes_min_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_bytes_max_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_array_min_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_array_max_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17 | SLOT_18
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_map_min_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17 | SLOT_18 | SLOT_19
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_map_max_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17 | SLOT_18 | SLOT_19 | SLOT_20
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE void
TypeNode_get_fixtuple(TypeNode *type, Py_ssize_t *offset, Py_ssize_t *size) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17 | SLOT_18 | SLOT_19 | SLOT_20 | SLOT_21
        )
    );
    *size = type->details[i].py_ssize_t;
    *offset = i + 1;
}

static void
TypeNode_get_traverse_ranges(
    TypeNode *type, Py_ssize_t *n_objects, Py_ssize_t *n_typenode,
    Py_ssize_t *fixtuple_offset, Py_ssize_t *fixtuple_size
) {
    Py_ssize_t n_obj = 0, n_type = 0, ft_offset = 0, ft_size = 0;
    /* Custom types cannot share a union with anything except `None` */
    if (type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC)) {
        n_obj = 1;
    }
    else if (!(type->types & MS_TYPE_ANY)) {
        /* Number of pyobject details */
        n_obj = ms_popcount(
            type->types & (
                MS_TYPE_STRUCT | MS_TYPE_STRUCT_UNION |
                MS_TYPE_STRUCT_ARRAY | MS_TYPE_STRUCT_ARRAY_UNION |
                MS_TYPE_INTENUM | MS_TYPE_INTLITERAL |
                MS_TYPE_ENUM | MS_TYPE_STRLITERAL |
                MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS |
                MS_TYPE_NAMEDTUPLE
            )
        );
        /* Number of typenode details */
        n_type = ms_popcount(
            type->types & (
                MS_TYPE_DICT |
                MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_FROZENSET | MS_TYPE_VARTUPLE
            )
        );
        if (type->types & MS_TYPE_FIXTUPLE) {
            TypeNode_get_fixtuple(type, &ft_offset, &ft_size);
        }
    }
    *n_objects = n_obj;
    *n_typenode = n_type;
    *fixtuple_offset = ft_offset;
    *fixtuple_size = ft_size;
}

static void
TypeNode_Free(TypeNode *self) {
    if (self == NULL) return;
    Py_ssize_t n_obj, n_typenode, fixtuple_offset, fixtuple_size, i;
    TypeNode_get_traverse_ranges(self, &n_obj, &n_typenode, &fixtuple_offset, &fixtuple_size);

    for (i = 0; i < n_obj; i++) {
        PyObject *obj = (PyObject *)(self->details[i].pointer);
        Py_XDECREF(obj);
    }
    for (i = n_obj; i < (n_obj + n_typenode); i++) {
        TypeNode *node = (TypeNode *)(self->details[i].pointer);
        TypeNode_Free(node);
    }
    for (i = 0; i < fixtuple_size; i++) {
        TypeNode *node = (TypeNode *)(self->details[i + fixtuple_offset].pointer);
        TypeNode_Free(node);
    }
    PyMem_Free(self);
}

static int
TypeNode_traverse(TypeNode *self, visitproc visit, void *arg) {
    if (self == NULL) return 0;
    Py_ssize_t n_obj, n_typenode, fixtuple_offset, fixtuple_size, i;
    TypeNode_get_traverse_ranges(self, &n_obj, &n_typenode, &fixtuple_offset, &fixtuple_size);

    for (i = 0; i < n_obj; i++) {
        PyObject *obj = (PyObject *)(self->details[i].pointer);
        Py_VISIT(obj);
    }
    for (i = n_obj; i < (n_obj + n_typenode); i++) {
        int out;
        TypeNode *node = (TypeNode *)(self->details[i].pointer);
        if ((out = TypeNode_traverse(node, visit, arg)) != 0) return out;
    }
    for (i = 0; i < fixtuple_size; i++) {
        int out;
        TypeNode *node = (TypeNode *)(self->details[i + fixtuple_offset].pointer);
        if ((out = TypeNode_traverse(node, visit, arg)) != 0) return out;
    }
    return 0;
}

static PyObject *
typenode_simple_repr(TypeNode *self) {
    strbuilder builder = {" | ", 3};

    if (self->types & (MS_TYPE_ANY | MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC) || self->types == 0) {
        return PyUnicode_FromString("any");
    }
    if (self->types & MS_TYPE_BOOL) {
        if (!strbuilder_extend_literal(&builder, "bool")) return NULL;
    }
    if (self->types & (MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        if (!strbuilder_extend_literal(&builder, "int")) return NULL;
    }
    if (self->types & MS_TYPE_FLOAT) {
        if (!strbuilder_extend_literal(&builder, "float")) return NULL;
    }
    if (self->types & (MS_TYPE_STR | MS_TYPE_ENUM | MS_TYPE_STRLITERAL)) {
        if (!strbuilder_extend_literal(&builder, "str")) return NULL;
    }
    if (self->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY)) {
        if (!strbuilder_extend_literal(&builder, "bytes")) return NULL;
    }
    if (self->types & MS_TYPE_DATETIME) {
        if (!strbuilder_extend_literal(&builder, "datetime")) return NULL;
    }
    if (self->types & MS_TYPE_DATE) {
        if (!strbuilder_extend_literal(&builder, "date")) return NULL;
    }
    if (self->types & MS_TYPE_TIME) {
        if (!strbuilder_extend_literal(&builder, "time")) return NULL;
    }
    if (self->types & MS_TYPE_UUID) {
        if (!strbuilder_extend_literal(&builder, "uuid")) return NULL;
    }
    if (self->types & MS_TYPE_DECIMAL) {
        if (!strbuilder_extend_literal(&builder, "decimal")) return NULL;
    }
    if (self->types & MS_TYPE_EXT) {
        if (!strbuilder_extend_literal(&builder, "ext")) return NULL;
    }
    if (self->types & (
            MS_TYPE_STRUCT | MS_TYPE_STRUCT_UNION |
            MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS | MS_TYPE_DICT
        )
    ) {
        if (!strbuilder_extend_literal(&builder, "object")) return NULL;
    }
    if (
        self->types & (
            MS_TYPE_STRUCT_ARRAY | MS_TYPE_STRUCT_ARRAY_UNION |
            MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_FROZENSET |
            MS_TYPE_VARTUPLE | MS_TYPE_FIXTUPLE | MS_TYPE_NAMEDTUPLE
        )
    ) {
        if (!strbuilder_extend_literal(&builder, "array")) return NULL;
    }
    if (self->types & MS_TYPE_NONE) {
        if (!strbuilder_extend_literal(&builder, "null")) return NULL;
    }

    return strbuilder_build(&builder);
}

typedef struct {
    PyObject *gt;
    PyObject *ge;
    PyObject *lt;
    PyObject *le;
    PyObject *multiple_of;
    PyObject *regex;
    PyObject *min_length;
    PyObject *max_length;
    PyObject *tz;
} Constraints;

typedef struct {
    MsgspecState *mod;
    PyObject *context;
    uint64_t types;
    PyObject *struct_obj;
    PyObject *structs_set;
    PyObject *structs_lookup;
    PyObject *intenum_obj;
    PyObject *enum_obj;
    PyObject *custom_obj;
    PyObject *array_el_obj;
    PyObject *dict_key_obj;
    PyObject *dict_val_obj;
    PyObject *typeddict_obj;
    PyObject *dataclass_obj;
    PyObject *namedtuple_obj;
    PyObject *literals;
    PyObject *int_literal_values;
    PyObject *int_literal_lookup;
    PyObject *str_literal_values;
    PyObject *str_literal_lookup;
    /* Constraints */
    int64_t c_int_min;
    int64_t c_int_max;
    int64_t c_int_multiple_of;
    double c_float_min;
    double c_float_max;
    double c_float_multiple_of;
    PyObject *c_str_regex;
    Py_ssize_t c_str_min_length;
    Py_ssize_t c_str_max_length;
    Py_ssize_t c_bytes_min_length;
    Py_ssize_t c_bytes_max_length;
    Py_ssize_t c_array_min_length;
    Py_ssize_t c_array_max_length;
    Py_ssize_t c_map_min_length;
    Py_ssize_t c_map_max_length;
} TypeNodeCollectState;

static MS_INLINE bool
constraints_is_empty(Constraints *self) {
    return (
        self->gt == NULL &&
        self->ge == NULL &&
        self->lt == NULL &&
        self->le == NULL &&
        self->multiple_of == NULL &&
        self->regex == NULL &&
        self->min_length == NULL &&
        self->max_length == NULL &&
        self->tz == NULL
    );
}

static int
_set_constraint(PyObject *source, PyObject **target, const char *name, PyObject *type) {
    if (source == NULL) return 0;
    if (*target == NULL) {
        *target = source;
        return 0;
    }
    PyErr_Format(
        PyExc_TypeError,
        "Multiple `Meta` annotations setting `%s` found, "
        "type `%R` is invalid",
        name, type
    );
    return -1;
}

static int
constraints_update(Constraints *self, Meta *meta, PyObject *type) {
#define set_constraint(field) do { \
    if (_set_constraint(meta->field, &(self->field), #field, type) < 0) return -1; \
} while (0)
    set_constraint(gt);
    set_constraint(ge);
    set_constraint(lt);
    set_constraint(le);
    set_constraint(multiple_of);
    set_constraint(regex);
    set_constraint(min_length);
    set_constraint(max_length);
    set_constraint(tz);
    if (self->gt != NULL && self->ge != NULL) {
        PyErr_Format(
            PyExc_TypeError,
            "Cannot set both `gt` and `ge` on the same annotated type, "
            "type `%R` is invalid",
            type
        );
        return -1;
    }
    if (self->lt != NULL && self->le != NULL) {
        PyErr_Format(
            PyExc_TypeError,
            "Cannot set both `lt` and `le` on the same annotated type, "
            "type `%R` is invalid",
            type
        );
        return -1;
    }
    return 0;
#undef set_constraint
}

enum constraint_kind {
    CK_INT = 0,
    CK_FLOAT = 1,
    CK_STR = 2,
    CK_BYTES = 3,
    CK_TIME = 4,
    CK_ARRAY = 5,
    CK_MAP = 6,
    CK_OTHER = 7,
};

static int
err_invalid_constraint(const char *name, const char *kind, PyObject *obj) {
    PyErr_Format(
        PyExc_TypeError,
        "Can only set `%s` on a %s type - type `%R` is invalid",
        name, kind, obj
    );
    return -1;
}

static bool
_constr_as_i64(PyObject *obj, int64_t *target, int offset) {
    int overflow;
    int64_t x = PyLong_AsLongLongAndOverflow(obj, &overflow);
    if (overflow != 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "Integer bounds constraints (`ge`, `le`, ...) that don't fit in an "
            "int64 are currently not supported. If you need this feature, please "
            "open an issue on GitHub"
        );
        return false;
    }
    else if (x == -1 && PyErr_Occurred()) {
        return false;
    }
    /* Do offsets for lt/gt */
    if (offset == -1) {
        if (x == (-1LL << 63)) {
            PyErr_SetString(PyExc_ValueError, "lt <= -2**63 is not supported");
            return false;
        }
        x -= 1;
    }
    else if (offset == 1) {
        if (x == ((1ULL << 63) - 1)) {
            PyErr_SetString(PyExc_ValueError, "gt >= 2**63 - 1 is not supported");
            return false;
        }
        x += 1;
    }

    *target = x;
    return true;
}

static bool
_constr_as_f64(PyObject *obj, double *target, int offset) {
    /* Use PyFloat_AsDouble to also handle integers */
    double x = PyFloat_AsDouble(obj);
    /* Should never be hit, types already checked */
    if (x == -1.0 && PyErr_Occurred()) return false;
    if (offset == 1) {
        x = nextafter(x, DBL_MAX);
    }
    else if (offset == -1) {
        x = nextafter(x, -DBL_MAX);
    }
    *target = x;
    return true;
}

static bool
_constr_as_py_ssize_t(PyObject *obj, Py_ssize_t *target) {
    Py_ssize_t x = PyLong_AsSsize_t(obj);
    /* Should never be hit, types already checked */
    if (x == -1 && PyErr_Occurred()) return false;
    *target = x;
    return true;
}

static int
typenode_collect_constraints(
    TypeNodeCollectState *state,
    Constraints *constraints,
    enum constraint_kind kind,
    PyObject *obj
) {
    /* If no constraints, do nothing */
    if (constraints == NULL) return 0;
    if (constraints_is_empty(constraints)) return 0;

    /* Check that the constraints are valid for the corresponding type */
    if (kind != CK_INT && kind != CK_FLOAT) {
        if (constraints->gt != NULL) return err_invalid_constraint("gt", "numeric", obj);
        if (constraints->ge != NULL) return err_invalid_constraint("ge", "numeric", obj);
        if (constraints->lt != NULL) return err_invalid_constraint("lt", "numeric", obj);
        if (constraints->le != NULL) return err_invalid_constraint("le", "numeric", obj);
        if (constraints->multiple_of != NULL) return err_invalid_constraint("multiple_of", "numeric", obj);
    }
    if (kind != CK_STR) {
        if (constraints->regex != NULL) return err_invalid_constraint("pattern", "str", obj);
    }
    if (kind != CK_STR && kind != CK_BYTES && kind != CK_ARRAY && kind != CK_MAP) {
        if (constraints->min_length != NULL) return err_invalid_constraint("min_length", "str, bytes, or collection", obj);
        if (constraints->max_length != NULL) return err_invalid_constraint("max_length", "str, bytes, or collection", obj);
    }
    if (kind != CK_TIME) {
        if (constraints->tz != NULL) return err_invalid_constraint("tz", "datetime or time", obj);
    }

    /* Next attempt to fill in the state. */
    if (kind == CK_INT) {
        if (constraints->gt != NULL) {
            state->types |= MS_CONSTR_INT_MIN;
            if (!_constr_as_i64(constraints->gt, &(state->c_int_min), 1)) return -1;
        }
        else if (constraints->ge != NULL) {
            state->types |= MS_CONSTR_INT_MIN;
            if (!_constr_as_i64(constraints->ge, &(state->c_int_min), 0)) return -1;
        }
        if (constraints->lt != NULL) {
            state->types |= MS_CONSTR_INT_MAX;
            if (!_constr_as_i64(constraints->lt, &(state->c_int_max), -1)) return -1;
            state->c_int_min -= 1;
        }
        else if (constraints->le != NULL) {
            state->types |= MS_CONSTR_INT_MAX;
            if (!_constr_as_i64(constraints->le, &(state->c_int_max), 0)) return -1;
        }
        if (constraints->multiple_of != NULL) {
            state->types |= MS_CONSTR_INT_MULTIPLE_OF;
            if (!_constr_as_i64(constraints->multiple_of, &(state->c_int_multiple_of), 0)) return -1;
        }
    }
    else if (kind == CK_FLOAT) {
        if (constraints->gt != NULL) {
            state->types |= MS_CONSTR_FLOAT_GT;
            if (!_constr_as_f64(constraints->gt, &(state->c_float_min), 1)) return -1;
        }
        else if (constraints->ge != NULL) {
            state->types |= MS_CONSTR_FLOAT_GE;
            if (!_constr_as_f64(constraints->ge, &(state->c_float_min), 0)) return -1;
        }
        if (constraints->lt != NULL) {
            state->types |= MS_CONSTR_FLOAT_LT;
            if (!_constr_as_f64(constraints->lt, &(state->c_float_max), -1)) return -1;
        }
        else if (constraints->le != NULL) {
            state->types |= MS_CONSTR_FLOAT_LE;
            if (!_constr_as_f64(constraints->le, &(state->c_float_max), 0)) return -1;
        }
        if (constraints->multiple_of != NULL) {
            state->types |= MS_CONSTR_FLOAT_MULTIPLE_OF;
            if (!_constr_as_f64(constraints->multiple_of, &(state->c_float_multiple_of), 0)) return -1;
        }
    }
    else if (kind == CK_STR) {
        if (constraints->regex != NULL) {
            state->types |= MS_CONSTR_STR_REGEX;
            Py_INCREF(constraints->regex);
            state->c_str_regex = constraints->regex;
        }
        if (constraints->min_length != NULL) {
            state->types |= MS_CONSTR_STR_MIN_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->min_length, &(state->c_str_min_length))) return -1;
        }
        if (constraints->max_length != NULL) {
            state->types |= MS_CONSTR_STR_MAX_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->max_length, &(state->c_str_max_length))) return -1;
        }
    }
    else if (kind == CK_BYTES) {
        if (constraints->min_length != NULL) {
            state->types |= MS_CONSTR_BYTES_MIN_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->min_length, &(state->c_bytes_min_length))) return -1;
        }
        if (constraints->max_length != NULL) {
            state->types |= MS_CONSTR_BYTES_MAX_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->max_length, &(state->c_bytes_max_length))) return -1;
        }
    }
    else if (kind == CK_TIME) {
        if (constraints->tz != NULL) {
            if (constraints->tz == Py_True) {
                state->types |= MS_CONSTR_TZ_AWARE;
            }
            else {
                state->types |= MS_CONSTR_TZ_NAIVE;
            }
        }
    }
    else if (kind == CK_ARRAY) {
        if (constraints->min_length != NULL) {
            state->types |= MS_CONSTR_ARRAY_MIN_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->min_length, &(state->c_array_min_length))) return -1;
        }
        if (constraints->max_length != NULL) {
            state->types |= MS_CONSTR_ARRAY_MAX_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->max_length, &(state->c_array_max_length))) return -1;
        }
    }
    else if (kind == CK_MAP) {
        if (constraints->min_length != NULL) {
            state->types |= MS_CONSTR_MAP_MIN_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->min_length, &(state->c_map_min_length))) return -1;
        }
        if (constraints->max_length != NULL) {
            state->types |= MS_CONSTR_MAP_MAX_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->max_length, &(state->c_map_max_length))) return -1;
        }
    }
    return 0;
}

static int typenode_collect_type(TypeNodeCollectState*, PyObject*);

static TypeNode *
typenode_from_collect_state(TypeNodeCollectState *state, bool err_not_json, bool *json_compatible) {
    Py_ssize_t e_ind, n_extra = 0, fixtuple_size = 0;
    bool has_fixtuple = false;

    n_extra = ms_popcount(
        state->types & (
            MS_TYPE_STRUCT | MS_TYPE_STRUCT_ARRAY |
            MS_TYPE_STRUCT_UNION | MS_TYPE_STRUCT_ARRAY_UNION |
            MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC |
            MS_TYPE_INTENUM | MS_TYPE_INTLITERAL |
            MS_TYPE_ENUM | MS_TYPE_STRLITERAL |
            MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS |
            MS_TYPE_NAMEDTUPLE |
            MS_CONSTR_STR_REGEX |
            MS_TYPE_DICT |
            MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_FROZENSET | MS_TYPE_VARTUPLE |
            MS_CONSTR_INT_MIN |
            MS_CONSTR_INT_MAX |
            MS_CONSTR_INT_MULTIPLE_OF |
            MS_CONSTR_FLOAT_GT | MS_CONSTR_FLOAT_GE |
            MS_CONSTR_FLOAT_LT | MS_CONSTR_FLOAT_LE |
            MS_CONSTR_FLOAT_MULTIPLE_OF |
            MS_CONSTR_STR_MIN_LENGTH |
            MS_CONSTR_STR_MAX_LENGTH |
            MS_CONSTR_BYTES_MIN_LENGTH |
            MS_CONSTR_BYTES_MAX_LENGTH |
            MS_CONSTR_ARRAY_MIN_LENGTH |
            MS_CONSTR_ARRAY_MAX_LENGTH |
            MS_CONSTR_MAP_MIN_LENGTH |
            MS_CONSTR_MAP_MAX_LENGTH
        )
    );
    if (state->types & MS_TYPE_FIXTUPLE) {
        has_fixtuple = true;
        fixtuple_size = PyTuple_GET_SIZE(state->array_el_obj);
        n_extra += fixtuple_size + 1;
    }

    if (n_extra == 0) {
        TypeNode *out = (TypeNode *)PyMem_Malloc(sizeof(TypeNode));
        if (out == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        out->types = state->types;
        return out;
    }

    /* Use calloc so that `out->details` is initialized, easing cleanup on error */
    TypeNode *out = (TypeNode *)PyMem_Calloc(
        1, sizeof(TypeNode) + n_extra * sizeof(TypeDetail)
    );
    if (out == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    out->types = state->types;
    /* Populate `details` fields in order */
    e_ind = 0;
    if (state->custom_obj != NULL) {
        Py_INCREF(state->custom_obj);
        /* Add `Any` to the type node, so the individual decode functions can
         * check for `Any` alone, and only have to handle custom types in one
         * location  (e.g. `mpack_decode`). */
        out->types |= MS_TYPE_ANY;
        out->details[e_ind++].pointer = state->custom_obj;
    }
    if (state->struct_obj != NULL) {
        Py_INCREF(state->struct_obj);
        out->details[e_ind++].pointer = state->struct_obj;
    }
    if (state->structs_lookup != NULL) {
        Py_INCREF(state->structs_lookup);
        out->details[e_ind++].pointer = state->structs_lookup;
    }
    if (state->intenum_obj != NULL) {
        PyObject *lookup = PyObject_GetAttr(state->intenum_obj, state->mod->str___msgspec_cache__);
        if (lookup == NULL) {
            /* IntLookup isn't created yet, create and store on enum class */
            PyErr_Clear();
            PyObject *member_map = PyObject_GetAttr(state->intenum_obj, state->mod->str__value2member_map_);
            if (member_map == NULL) goto error;
            lookup = IntLookup_New(member_map, NULL, false, false);
            Py_DECREF(member_map);
            if (lookup == NULL) goto error;
            if (PyObject_SetAttr(state->intenum_obj, state->mod->str___msgspec_cache__, lookup) < 0) {
                Py_DECREF(lookup);
                goto error;
            }
        }
        else if (!Lookup_IsIntLookup(lookup)) {
            /* the lookup attribute has been overwritten, error */
            Py_DECREF(lookup);
            PyErr_Format(
                PyExc_RuntimeError,
                "%R.__msgspec_cache__ has been overwritten",
                state->intenum_obj
            );
            goto error;
        }
        out->details[e_ind++].pointer = lookup;
    }
    if (state->int_literal_lookup != NULL) {
        Py_INCREF(state->int_literal_lookup);
        out->details[e_ind++].pointer = state->int_literal_lookup;
    }
    if (state->enum_obj != NULL) {
        PyObject *lookup = PyObject_GetAttr(state->enum_obj, state->mod->str___msgspec_cache__);
        if (lookup == NULL) {
            /* StrLookup isn't created yet, create and store on enum class */
            PyErr_Clear();
            PyObject *member_map = PyObject_GetAttr(state->enum_obj, state->mod->str__value2member_map_);
            if (member_map == NULL) goto error;
            lookup = StrLookup_New(member_map, NULL, false, false);
            Py_DECREF(member_map);
            if (lookup == NULL) goto error;
            if (PyObject_SetAttr(state->enum_obj, state->mod->str___msgspec_cache__, lookup) < 0) {
                Py_DECREF(lookup);
                goto error;
            }
        }
        else if (Py_TYPE(lookup) != &StrLookup_Type) {
            /* the lookup attribute has been overwritten, error */
            Py_DECREF(lookup);
            PyErr_Format(
                PyExc_RuntimeError,
                "%R.__msgspec_cache__ has been overwritten",
                state->enum_obj
            );
            goto error;
        }
        out->details[e_ind++].pointer = lookup;
    }
    if (state->str_literal_lookup != NULL) {
        Py_INCREF(state->str_literal_lookup);
        out->details[e_ind++].pointer = state->str_literal_lookup;
    }
    if (state->typeddict_obj != NULL) {
        PyObject *info = TypedDictInfo_Convert(
            state->typeddict_obj, err_not_json, json_compatible
        );
        if (info == NULL) goto error;
        out->details[e_ind++].pointer = info;
    }
    if (state->dataclass_obj != NULL) {
        PyObject *info = DataclassInfo_Convert(
            state->dataclass_obj, err_not_json, json_compatible
        );
        if (info == NULL) goto error;
        out->details[e_ind++].pointer = info;
    }
    if (state->namedtuple_obj != NULL) {
        PyObject *info = NamedTupleInfo_Convert(
            state->namedtuple_obj, err_not_json, json_compatible
        );
        if (info == NULL) goto error;
        out->details[e_ind++].pointer = info;
    }
    if (state->types & MS_CONSTR_STR_REGEX) {
        Py_INCREF(state->c_str_regex);
        out->details[e_ind++].pointer = state->c_str_regex;
    }
    if (state->dict_key_obj != NULL) {
        TypeNode *temp = TypeNode_Convert(state->dict_key_obj, err_not_json, json_compatible);
        if (temp == NULL) goto error;
        out->details[e_ind++].pointer = temp;
        /* Check that JSON dict keys are strings */
        if (
            temp->types
            & ~(
                MS_TYPE_ANY |
                MS_TYPE_STR | MS_TYPE_ENUM | MS_TYPE_STRLITERAL | MS_STR_CONSTRS |
                MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL | MS_INT_CONSTRS |
                MS_TYPE_BYTES | MS_BYTES_CONSTRS |
                MS_TYPE_DATETIME | MS_TYPE_DATE | MS_TYPE_TIME | MS_TIME_CONSTRS |
                MS_TYPE_UUID | MS_TYPE_DECIMAL
            )
        ) {
            if (err_not_json) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Only dicts with str-like or int-like keys are supported "
                    "- type `%R` is not supported",
                    state->context
                );
                goto error;  /* temp already added to `out`, gets freed below */
            }
            if (json_compatible != NULL)
                *json_compatible = false;
        }
        temp = TypeNode_Convert(state->dict_val_obj, err_not_json, json_compatible);
        if (temp == NULL) goto error;
        out->details[e_ind++].pointer = temp;
    }
    if (state->array_el_obj != NULL) {
        if (has_fixtuple) {
            out->details[e_ind++].py_ssize_t = fixtuple_size;

            for (Py_ssize_t i = 0; i < fixtuple_size; i++) {
                TypeNode *temp = TypeNode_Convert(
                    PyTuple_GET_ITEM(state->array_el_obj, i),
                    err_not_json,
                    json_compatible
                );
                if (temp == NULL) goto error;
                out->details[e_ind++].pointer = temp;
            }
        }
        else {
            TypeNode *temp = TypeNode_Convert(
                state->array_el_obj, err_not_json, json_compatible
            );
            if (temp == NULL) goto error;
            out->details[e_ind++].pointer = temp;
        }
    }
    if (state->types & MS_CONSTR_INT_MIN) {
        out->details[e_ind++].i64 = state->c_int_min;
    }
    if (state->types & MS_CONSTR_INT_MAX) {
        out->details[e_ind++].i64 = state->c_int_max;
    }
    if (state->types & MS_CONSTR_INT_MULTIPLE_OF) {
        out->details[e_ind++].i64 = state->c_int_multiple_of;
    }
    if (state->types & (MS_CONSTR_FLOAT_GT | MS_CONSTR_FLOAT_GE)) {
        out->details[e_ind++].f64 = state->c_float_min;
    }
    if (state->types & (MS_CONSTR_FLOAT_LT | MS_CONSTR_FLOAT_LE)) {
        out->details[e_ind++].f64 = state->c_float_max;
    }
    if (state->types & MS_CONSTR_FLOAT_MULTIPLE_OF) {
        out->details[e_ind++].f64 = state->c_float_multiple_of;
    }
    if (state->types & MS_CONSTR_STR_MIN_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_str_min_length;
    }
    if (state->types & MS_CONSTR_STR_MAX_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_str_max_length;
    }
    if (state->types & MS_CONSTR_BYTES_MIN_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_bytes_min_length;
    }
    if (state->types & MS_CONSTR_BYTES_MAX_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_bytes_max_length;
    }
    if (state->types & MS_CONSTR_ARRAY_MIN_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_array_min_length;
    }
    if (state->types & MS_CONSTR_ARRAY_MAX_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_array_max_length;
    }
    if (state->types & MS_CONSTR_MAP_MIN_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_map_min_length;
    }
    if (state->types & MS_CONSTR_MAP_MAX_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_map_max_length;
    }
    return (TypeNode *)out;

error:
    TypeNode_Free((TypeNode *)out);
    return NULL;
}

static int
typenode_collect_err_unique(TypeNodeCollectState *state, const char *kind) {
    PyErr_Format(
        PyExc_TypeError,
        "Type unions may not contain more than one %s type - "
        "type `%R` is not supported",
        kind,
        state->context
    );
    return -1;
}

static int
typenode_collect_check_invariants(
    TypeNodeCollectState *state, bool err_not_json, bool *json_compatible
) {
    /* If a custom type is used, this node may only contain that type and `None */
    if (
        state->custom_obj != NULL &&
        state->types & ~(MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC | MS_TYPE_NONE)
    ) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions containing a custom type may not contain any "
            "additional types other than `None` - type `%R` is not supported",
            state->context
        );
        return -1;
    }

    /* Ensure at most one array-like type in the union */
    if (ms_popcount(
            state->types & (
                MS_TYPE_STRUCT_ARRAY | MS_TYPE_STRUCT_ARRAY_UNION |
                MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_FROZENSET |
                MS_TYPE_VARTUPLE | MS_TYPE_FIXTUPLE | MS_TYPE_NAMEDTUPLE
            )
        ) > 1
    ) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain more than one array-like type "
            "(`Struct(array_like=True)`, `list`, `set`, `frozenset`, `tuple`, "
            "`NamedTuple`) - type `%R` is not supported",
            state->context
        );
        return -1;
    }
    /* Ensure at most one dict-like type in the union */
    int ndictlike = ms_popcount(
        state->types & (
            MS_TYPE_STRUCT | MS_TYPE_STRUCT_UNION |
            MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS
        )
    );
    if (state->types & MS_TYPE_DICT) {
        ndictlike++;
    }
    if (ndictlike > 1) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain more than one dict-like type "
            "(`Struct`, `dict`, `TypedDict`, `dataclass`) - type `%R` "
            "is not supported",
            state->context
        );
        return -1;
    }

    /* If int & int literals are both present, drop literals */
    if (state->types & MS_TYPE_INT && state->int_literal_lookup) {
        state->types &= ~MS_TYPE_INTLITERAL;
        Py_CLEAR(state->int_literal_lookup);
    }

    /* If str & str literals are both present, drop literals */
    if (state->types & MS_TYPE_STR && state->str_literal_lookup) {
        state->types &= ~MS_TYPE_STRLITERAL;
        Py_CLEAR(state->str_literal_lookup);
    }

    /* Ensure int-like types don't conflict */
    if (ms_popcount(state->types & (MS_TYPE_INT | MS_TYPE_INTLITERAL | MS_TYPE_INTENUM)) > 1) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain more than one int-like type (`int`, "
            "`Enum`, `Literal[int values]`) - type `%R` is not supported",
            state->context
        );
        return -1;
    }

    /* Ensure str-like types don't conflict */
    if (ms_popcount(
            state->types & (
                MS_TYPE_STR | MS_TYPE_STRLITERAL | MS_TYPE_ENUM |
                MS_TYPE_BYTES | MS_TYPE_BYTEARRAY |
                MS_TYPE_DATETIME | MS_TYPE_DATE | MS_TYPE_TIME |
                MS_TYPE_UUID | MS_TYPE_DECIMAL
            )
        ) > 1
    ) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain more than one str-like type (`str`, "
            "`Enum`, `Literal[str values]`, `datetime`, `date`, `time`, `uuid`, "
            "`decimal`, `bytes`, `bytearray`) - type `%R` is not supported",
            state->context
        );
        return -1;
    }
    return 0;
}

static int
typenode_collect_enum(TypeNodeCollectState *state, PyObject *obj) {
    bool is_intenum;

    if (PyType_IsSubtype((PyTypeObject *)obj, &PyLong_Type)) {
        is_intenum = true;
    }
    else if (PyType_IsSubtype((PyTypeObject *)obj, &PyUnicode_Type)) {
        is_intenum = false;
    }
    else {
        PyObject *members = PyObject_GetAttr(obj, state->mod->str__value2member_map_);
        if (members == NULL) return -1;
        if (!PyDict_Check(members)) {
            Py_DECREF(members);
            PyErr_SetString(
                PyExc_RuntimeError, "Expected _value2member_map_ to be a dict"
            );
            return -1;
        }
        /* Traverse _value2member_map_ to determine key type */
        Py_ssize_t pos = 0;
        PyObject *key;
        bool all_ints = true;
        bool all_strs = true;
        while (PyDict_Next(members, &pos, &key, NULL)) {
            all_ints &= PyLong_CheckExact(key);
            all_strs &= PyUnicode_CheckExact(key);
        }
        Py_CLEAR(members);

        if (all_ints) {
            is_intenum = true;
        }
        else if (all_strs) {
            is_intenum = false;
        }
        else {
            PyErr_Format(
                PyExc_TypeError,
                "Enums must contain either all str or all int values - "
                "type `%R` is not supported",
                state->context
            );
            return -1;
        }
    }

    if (is_intenum) {
        if (state->intenum_obj != NULL) {
            return typenode_collect_err_unique(state, "int enum");
        }
        state->types |= MS_TYPE_INTENUM;
        Py_INCREF(obj);
        state->intenum_obj = obj;
    }
    else {
        if (state->enum_obj != NULL) {
            return typenode_collect_err_unique(state, "str enum");
        }
        state->types |= MS_TYPE_ENUM;
        Py_INCREF(obj);
        state->enum_obj = obj;
    }
    return 0;
}

static int
typenode_collect_dict(TypeNodeCollectState *state, PyObject *key, PyObject *val) {
    if (state->dict_key_obj != NULL) {
        return typenode_collect_err_unique(state, "dict");
    }
    state->types |= MS_TYPE_DICT;
    Py_INCREF(key);
    state->dict_key_obj = key;
    Py_INCREF(val);
    state->dict_val_obj = val;
    return 0;
}

static int
typenode_collect_array(TypeNodeCollectState *state, uint64_t type, PyObject *obj) {
    if (state->array_el_obj != NULL) {
        return typenode_collect_err_unique(
            state, "array-like (list, set, tuple)"
        );
    }
    state->types |= type;
    Py_INCREF(obj);
    state->array_el_obj = obj;
    return 0;
}

static int
typenode_collect_custom(TypeNodeCollectState *state, uint64_t type, PyObject *obj) {
    if (state->custom_obj != NULL) {
        return typenode_collect_err_unique(state, "custom");
    }
    state->types |= type;
    Py_INCREF(obj);
    state->custom_obj = obj;
    return 0;
}

static int
typenode_collect_struct(TypeNodeCollectState *state, PyObject *obj) {
    if (state->struct_obj == NULL && state->structs_set == NULL) {
        /* First struct found, store it directly */
        Py_INCREF(obj);
        state->struct_obj = obj;
    }
    else {
        if (state->structs_set == NULL) {
            /* Second struct found, create a set and move the existing struct there */
            state->structs_set = PyFrozenSet_New(NULL);
            if (state->structs_set == NULL) return -1;
            if (PySet_Add(state->structs_set, state->struct_obj) < 0) return -1;
            Py_CLEAR(state->struct_obj);
        }
        if (PySet_Add(state->structs_set, obj) < 0) return -1;
    }
    return 0;
}

static int
typenode_collect_typeddict(TypeNodeCollectState *state, PyObject *obj) {
    if (state->typeddict_obj != NULL) {
        return typenode_collect_err_unique(state, "TypedDict");
    }
    state->types |= MS_TYPE_TYPEDDICT;
    Py_INCREF(obj);
    state->typeddict_obj = obj;
    return 0;
}

static int
typenode_collect_dataclass(TypeNodeCollectState *state, PyObject *obj) {
    if (state->dataclass_obj != NULL) {
        return typenode_collect_err_unique(state, "dataclass");
    }
    state->types |= MS_TYPE_DATACLASS;
    Py_INCREF(obj);
    state->dataclass_obj = obj;
    return 0;
}

static int
typenode_collect_namedtuple(TypeNodeCollectState *state, PyObject *obj) {
    if (state->namedtuple_obj != NULL) {
        return typenode_collect_err_unique(state, "NamedTuple");
    }
    state->types |= MS_TYPE_NAMEDTUPLE;
    Py_INCREF(obj);
    state->namedtuple_obj = obj;
    return 0;
}

static int
typenode_collect_literal(TypeNodeCollectState *state, PyObject *literal) {
    PyObject *args = PyObject_GetAttr(literal, state->mod->str___args__);
    /* This should never happen, since we know this is a `Literal` object */
    if (args == NULL) return -1;

    Py_ssize_t size = PyTuple_GET_SIZE(args);
    if (size < 0) return -1;

    if (size == 0) {
        PyErr_Format(
            PyExc_TypeError,
            "Literal types must have at least one item, %R is invalid",
            literal
        );
        return -1;
    }

    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *obj = PyTuple_GET_ITEM(args, i);
        PyTypeObject *type = Py_TYPE(obj);

        if (obj == Py_None || obj == NONE_TYPE) {
            state->types |= MS_TYPE_NONE;
        }
        else if (type == &PyLong_Type) {
            if (state->int_literal_values == NULL) {
                state->types |= MS_TYPE_INTLITERAL;
                state->int_literal_values = PySet_New(NULL);
                if (state->int_literal_values == NULL) goto error;
            }
            if (PySet_Add(state->int_literal_values, obj) < 0) goto error;
        }
        else if (type == &PyUnicode_Type) {
            if (state->str_literal_values == NULL) {
                state->types |= MS_TYPE_STRLITERAL;
                state->str_literal_values = PySet_New(NULL);
                if (state->str_literal_values == NULL) goto error;
            }
            if (PySet_Add(state->str_literal_values, obj) < 0) goto error;
        }
        else {
            /* Check for nested Literal */
            PyObject *origin = PyObject_GetAttr(obj, state->mod->str___origin__);
            if (origin == NULL) {
                PyErr_Clear();
                goto invalid;
            }
            else if (origin != state->mod->typing_literal) {
                Py_DECREF(origin);
                goto invalid;
            }
            Py_DECREF(origin);
            if (typenode_collect_literal(state, obj) < 0) goto error;
        }
    }

    Py_DECREF(args);
    return 0;

invalid:
    PyErr_Format(
        PyExc_TypeError,
        "Literal may only contain None/integers/strings - %R is not supported",
        literal
    );

error:
    Py_DECREF(args);
    return -1;
}

static int
typenode_collect_convert_literals(TypeNodeCollectState *state) {
    if (state->literals == NULL) {
        /* Nothing to do */
        return 0;
    }

    Py_ssize_t n = PyList_GET_SIZE(state->literals);

    if (n == 1) {
        PyObject *literal = PyList_GET_ITEM(state->literals, 0);

        /* Check if cached, otherwise create and cache */
        PyObject *cached = PyObject_GetAttr(literal, state->mod->str___msgspec_cache__);
        if (cached != NULL) {
            /* Extract and store the lookups */
            if (PyTuple_CheckExact(cached) && PyTuple_GET_SIZE(cached) == 2) {
                PyObject *int_lookup = PyTuple_GET_ITEM(cached, 0);
                PyObject *str_lookup = PyTuple_GET_ITEM(cached, 1);
                if (
                    (
                        (int_lookup == Py_None || Lookup_IsIntLookup(int_lookup)) &&
                        (str_lookup == Py_None || Lookup_IsStrLookup(str_lookup))
                    )
                ) {
                    if (Lookup_IsIntLookup(int_lookup)) {
                        Py_INCREF(int_lookup);
                        state->types |= MS_TYPE_INTLITERAL;
                        state->int_literal_lookup = int_lookup;
                    }
                    if (Lookup_IsStrLookup(str_lookup)) {
                        Py_INCREF(str_lookup);
                        state->types |= MS_TYPE_STRLITERAL;
                        state->str_literal_lookup = str_lookup;
                    }
                    Py_DECREF(cached);
                    return 0;
                }
            }
            Py_DECREF(cached);
            PyErr_Format(
                PyExc_RuntimeError,
                "%R.__msgspec_cache__ has been overwritten",
                literal
            );
            return -1;
        }
        else {
            /* Clear the AttributError from the cache lookup */
            PyErr_Clear();

            /* Collect all values in the literal */
            if (typenode_collect_literal(state, literal) < 0) return -1;

            /* Convert values to lookup objects (if values exist for each type) */
            if (state->int_literal_values != NULL) {
                state->types |= MS_TYPE_INTLITERAL;
                state->int_literal_lookup = IntLookup_New(state->int_literal_values, NULL, false, false);
                if (state->int_literal_lookup == NULL) return -1;
            }
            if (state->str_literal_values != NULL) {
                state->types |= MS_TYPE_STRLITERAL;
                state->str_literal_lookup = StrLookup_New(state->str_literal_values, NULL, false, false);
                if (state->str_literal_lookup == NULL) return -1;
            }

            /* Cache the lookups as a tuple on the `Literal` object */
            PyObject *cached = PyTuple_Pack(
                2,
                state->int_literal_lookup == NULL ? Py_None : state->int_literal_lookup,
                state->str_literal_lookup == NULL ? Py_None : state->str_literal_lookup
            );
            if (cached == NULL) return -1;
            int out = PyObject_SetAttr(literal, state->mod->str___msgspec_cache__, cached);
            Py_DECREF(cached);
            return out;
        }
    }
    else {
        /* Collect all values in all literals */
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *literal = PyList_GET_ITEM(state->literals, i);
            if (typenode_collect_literal(state, literal) < 0) return -1;
        }
        /* Convert values to lookup objects (if values exist for each type) */
        if (state->int_literal_values != NULL) {
            state->types |= MS_TYPE_INTLITERAL;
            state->int_literal_lookup = IntLookup_New(state->int_literal_values, NULL, false, false);
            if (state->int_literal_lookup == NULL) return -1;
        }
        if (state->str_literal_values != NULL) {
            state->types |= MS_TYPE_STRLITERAL;
            state->str_literal_lookup = StrLookup_New(state->str_literal_values, NULL, false, false);
            if (state->str_literal_lookup == NULL) return -1;
        }
        return 0;
    }
}

static void
_lookup_raise_json_incompatible(PyObject *lookup) {
    if (Py_TYPE(lookup) == &StrLookup_Type) {
        StrLookup *lk = (StrLookup *)lookup;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            if (lk->table[i].value != NULL) {
                PyObject *struct_type = lk->table[i].value;
                if (StructMeta_prep_types(struct_type, true, NULL) < 0) return;
            }
        }
    }
    else {
        if (((IntLookup *)lookup)->compact) {
            IntLookupCompact *lk = (IntLookupCompact *)lookup;
            for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
                if (lk->table[i] != NULL) {
                    PyObject *struct_type = lk->table[i];
                    if (StructMeta_prep_types(struct_type, true, NULL) < 0) return;
                }
            }
        }
        else {
            IntLookupHashmap *lk = (IntLookupHashmap *)lookup;
            for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
                if (lk->table[i].value != NULL) {
                    PyObject *struct_type = lk->table[i].value;
                    if (StructMeta_prep_types(struct_type, true, NULL) < 0) return;
                }
            }
        }
    }
}


static int
typenode_collect_convert_structs(
    TypeNodeCollectState *state, bool err_not_json, bool *json_compatible
) {
    if (state->struct_obj == NULL && state->structs_set == NULL) {
        return 0;
    }
    else if (state->struct_obj != NULL) {
        /* Single struct */
        if (StructMeta_prep_types(state->struct_obj, err_not_json, json_compatible) < 0) {
            return -1;
        }
        if (((StructMetaObject *)state->struct_obj)->array_like == OPT_TRUE) {
            state->types |= MS_TYPE_STRUCT_ARRAY;
        }
        else {
            state->types |= MS_TYPE_STRUCT;
        }
        return 0;
    }

    /* Multiple structs.
     *
     * Try looking the structs_set up in the cache first, to avoid building a
     * new one below.
     */
    PyObject *lookup = PyDict_GetItem(
        state->mod->struct_lookup_cache, state->structs_set
    );
    if (lookup != NULL) {
        /* Lookup was in the cache, update the state and return */
        bool union_json_compatible = Lookup_json_compatible(lookup);
        if (!union_json_compatible) {
            if (json_compatible != NULL) {
                *json_compatible = union_json_compatible;
            }
            if (err_not_json) {
                /* Recurse to raise nice error message */
                _lookup_raise_json_incompatible(lookup);
                return -1;
            }
        }
        Py_INCREF(lookup);
        state->structs_lookup = lookup;

        if (Lookup_array_like(lookup)) {
            state->types |= MS_TYPE_STRUCT_ARRAY_UNION;
        }
        else {
            state->types |= MS_TYPE_STRUCT_UNION;
        }
        return 0;
    }

    /* Here we check a number of restrictions before building a lookup table
     * from struct tags to their matching classes.
     *
     * Validation checks:
     * - All structs in the set are tagged.
     * - All structs in the set have the same `array_like` status
     * - All structs in the set have the same `tag_field`
     * - All structs in the set have a unique `tag_value`
     *
     * If any of these checks fails, an appropriate error is returned.
     */
    PyObject *tag_mapping = NULL, *tag_field = NULL, *set_item = NULL;
    Py_ssize_t set_pos = 0;
    Py_hash_t set_hash;
    bool array_like = false;
    bool union_json_compatible = true;
    bool tags_are_strings = true;
    int status = -1;

    tag_mapping = PyDict_New();
    if (tag_mapping == NULL) goto cleanup;

    while (_PySet_NextEntry(state->structs_set, &set_pos, &set_item, &set_hash)) {
        StructMetaObject *struct_type = (StructMetaObject *)set_item;
        PyObject *item_tag_field = struct_type->struct_tag_field;
        PyObject *item_tag_value = struct_type->struct_tag_value;
        bool item_array_like = struct_type->array_like == OPT_TRUE;
        bool item_json_compatible = true;

        if (StructMeta_prep_types((PyObject *)struct_type, err_not_json, &item_json_compatible) < 0) {
            goto cleanup;
        }
        union_json_compatible &= item_json_compatible;

        if (item_tag_value == NULL) {
            PyErr_Format(
                PyExc_TypeError,
                "If a type union contains multiple Struct types, all Struct "
                "types must be tagged (via `tag` or `tag_field` kwarg) - type "
                "`%R` is not supported",
                state->context
            );
            goto cleanup;
        }
        if (tag_field == NULL) {
            array_like = struct_type->array_like == OPT_TRUE;
            tag_field = struct_type->struct_tag_field;
            tags_are_strings = PyUnicode_CheckExact(item_tag_value);
        }
        else {
            if (array_like != item_array_like) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Type unions may not contain Struct types with `array_like=True` "
                    "and `array_like=False` - type `%R` is not supported",
                    state->context
                );
                goto cleanup;
            }
            if (tags_are_strings != PyUnicode_CheckExact(item_tag_value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Type unions may not contain Struct types with both `int` "
                    "and `str` tags - type `%R` is not supported",
                    state->context
                );
                goto cleanup;
            }

            int compare = PyUnicode_Compare(item_tag_field, tag_field);
            if (compare == -1 && PyErr_Occurred()) goto cleanup;
            if (compare != 0) {
                PyErr_Format(
                    PyExc_TypeError,
                    "If a type union contains multiple Struct types, all Struct types "
                    "must have the same `tag_field` - type `%R` is not supported",
                    state->context
                );
                goto cleanup;
            }
        }
        if (PyDict_GetItem(tag_mapping, item_tag_value) != NULL) {
            PyErr_Format(
                PyExc_TypeError,
                "If a type union contains multiple Struct types, all Struct types "
                "must have unique `tag` values - type `%R` is not supported",
                state->context
            );
            goto cleanup;
        }
        if (PyDict_SetItem(tag_mapping, item_tag_value, (PyObject *)struct_type) < 0) {
            goto cleanup;
        }
    }
    if (json_compatible != NULL && !union_json_compatible) {
        *json_compatible = union_json_compatible;
    }
    /* Build a lookup from tag_value -> struct_type */
    if (tags_are_strings) {
        lookup = StrLookup_New(tag_mapping, tag_field, array_like, union_json_compatible);
    }
    else {
        lookup = IntLookup_New(tag_mapping, tag_field, array_like, union_json_compatible);
    }
    if (lookup == NULL) goto cleanup;

    state->structs_lookup = lookup;

    /* Check if the cache is full, if so clear the oldest item */
    if (PyDict_GET_SIZE(state->mod->struct_lookup_cache) == 64) {
        PyObject *key;
        Py_ssize_t pos = 0;
        if (PyDict_Next(state->mod->struct_lookup_cache, &pos, &key, NULL)) {
            if (PyDict_DelItem(state->mod->struct_lookup_cache, key) < 0) {
                goto cleanup;
            }
        }
    }

    /* Add the new lookup to the cache */
    if (PyDict_SetItem(state->mod->struct_lookup_cache, state->structs_set, lookup) < 0) {
        goto cleanup;
    }

    /* Update the `types` */
    if (array_like) {
        state->types |= MS_TYPE_STRUCT_ARRAY_UNION;
    }
    else {
        state->types |= MS_TYPE_STRUCT_UNION;
    }

    status = 0;

cleanup:
    Py_XDECREF(tag_mapping);
    return status;
}

static void
typenode_collect_clear_state(TypeNodeCollectState *state) {
    Py_CLEAR(state->struct_obj);
    Py_CLEAR(state->structs_set);
    Py_CLEAR(state->structs_lookup);
    Py_CLEAR(state->intenum_obj);
    Py_CLEAR(state->enum_obj);
    Py_CLEAR(state->custom_obj);
    Py_CLEAR(state->array_el_obj);
    Py_CLEAR(state->dict_key_obj);
    Py_CLEAR(state->dict_val_obj);
    Py_CLEAR(state->typeddict_obj);
    Py_CLEAR(state->dataclass_obj);
    Py_CLEAR(state->namedtuple_obj);
    Py_CLEAR(state->literals);
    Py_CLEAR(state->int_literal_values);
    Py_CLEAR(state->int_literal_lookup);
    Py_CLEAR(state->str_literal_values);
    Py_CLEAR(state->str_literal_lookup);
    Py_CLEAR(state->c_str_regex);
}

/* This decomposes an input type `obj`, stripping out any "wrapper" types
 * (Annotated/NewType). It returns the following components:
 *
 * - `t` (return value): the first "concrete" type found in the type tree.
 * - `origin`: `__origin__` on `t` (if present), with a few normalizations
 *   applied to work around differences in type spelling (List vs list) and
 *   python version.
 * - `args`: `__args__` on `t` (if present)
 * - `constraints`: Any constraints from `Meta` objects annotated on the type
 */
static PyObject *
typenode_origin_args_metadata(
    TypeNodeCollectState *state, PyObject *obj,
    PyObject **out_origin, PyObject **out_args, Constraints *constraints
) {
    PyObject *origin = NULL, *args = NULL;
    PyObject *t = obj;
    Py_INCREF(t);

    /* First strip out meta "wrapper" types (Annotated, NewType) */
    while (true) {
        if (Py_TYPE(t) == (PyTypeObject *)(state->mod->typing_annotated_alias)) {
            /* Handle Annotated */
            PyObject *origin = PyObject_GetAttr(t, state->mod->str___origin__);
            if (origin == NULL) {
                Py_CLEAR(t);
                return NULL;
            }

            PyObject *metadata = PyObject_GetAttr(t, state->mod->str___metadata__);
            if (metadata == NULL) {
                Py_DECREF(origin);
                Py_DECREF(t);
                return NULL;
            }

            for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(metadata); i++) {
                PyObject *annot = PyTuple_GET_ITEM(metadata, i);
                if (Py_TYPE(annot) == &Meta_Type) {
                    if (constraints_update(constraints, (Meta *)annot, obj) < 0) {
                        Py_DECREF(metadata);
                        Py_DECREF(origin);
                        Py_DECREF(t);
                        return NULL;
                    }
                }
            }
            Py_DECREF(metadata);
            Py_DECREF(t);
            t = origin;
        }
        else {
            /* Handle NewType */
            PyObject *supertype = PyObject_GetAttr(t, state->mod->str___supertype__);
            if (supertype != NULL) {
                Py_DECREF(t);
                t = supertype;
            }
            else {
                PyErr_Clear();
                break;
            }
        }
    }

    /* At this point `t` is a concrete type. Next check for generic types,
     * extracting `__origin__` and `__args__`. This lets us normalize how
     * we check for collection types later */
    if ((origin = PyDict_GetItem(state->mod->concrete_types, t)) != NULL) {
        Py_INCREF(origin);
    }
    #if PY_VERSION_HEX >= 0x030a00f0
    else if (Py_TYPE(t) == (PyTypeObject *)(state->mod->types_uniontype)) {
        args = PyObject_GetAttr(t, state->mod->str___args__);
        if (args == NULL) {
            Py_DECREF(t);
            return NULL;
        }
        origin = state->mod->typing_union;
        Py_INCREF(origin);
    }
    #endif
    else {
        origin = PyObject_GetAttr(t, state->mod->str___origin__);
        if (origin == NULL) {
            /* Not a generic */
            PyErr_Clear();
        }
        else {
            /* Lookup __origin__ in the mapping, in case it's a supported
             * abstract type */
            PyObject *temp = PyDict_GetItem(state->mod->concrete_types, origin);
            if (temp != NULL) {
                Py_DECREF(origin);
                Py_INCREF(temp);
                origin = temp;
            }
            args = PyObject_GetAttr(t, state->mod->str___args__);
            if (args == NULL) {
                /* Custom non-parametrized generics won't have __args__ set.
                 * Ignore __args__ error */
                PyErr_Clear();
            }
            else {
                if (!PyTuple_Check(args)) {
                    PyErr_SetString(PyExc_TypeError, "__args__ must be a tuple");
                    Py_DECREF(t);
                    Py_DECREF(origin);
                    Py_DECREF(args);
                    return NULL;
                }
            }
        }
    }

    *out_origin = origin;
    *out_args = args;
    return t;
}

static int
typenode_collect_type(TypeNodeCollectState *state, PyObject *obj) {
    int out = 0;
    PyObject *t = NULL, *origin = NULL, *args = NULL;
    Constraints constraints = {0};
    enum constraint_kind kind = CK_OTHER;

    /* If `Any` type already encountered, nothing to do */
    if (state->types & MS_TYPE_ANY) return 0;

    t = typenode_origin_args_metadata(state, obj, &origin, &args, &constraints);
    if (t == NULL) return -1;

    if (t == state->mod->typing_any) {
        /* Any takes precedence, drop all existing and update type flags */
        typenode_collect_clear_state(state);
        state->types = MS_TYPE_ANY;
    }
    else if (t == Py_None || t == NONE_TYPE) {
        state->types |= MS_TYPE_NONE;
    }
    else if (t == (PyObject *)(&PyBool_Type)) {
        state->types |= MS_TYPE_BOOL;
    }
    else if (t == (PyObject *)(&PyLong_Type)) {
        state->types |= MS_TYPE_INT;
        kind = CK_INT;
    }
    else if (t == (PyObject *)(&PyFloat_Type)) {
        state->types |= MS_TYPE_FLOAT;
        kind = CK_FLOAT;
    }
    else if (t == (PyObject *)(&PyUnicode_Type)) {
        state->types |= MS_TYPE_STR;
        kind = CK_STR;
    }
    else if (t == (PyObject *)(&PyBytes_Type)) {
        state->types |= MS_TYPE_BYTES;
        kind = CK_BYTES;
    }
    else if (t == (PyObject *)(&PyByteArray_Type)) {
        state->types |= MS_TYPE_BYTEARRAY;
        kind = CK_BYTES;
    }
    else if (t == (PyObject *)(PyDateTimeAPI->DateTimeType)) {
        state->types |= MS_TYPE_DATETIME;
        kind = CK_TIME;
    }
    else if (t == (PyObject *)(PyDateTimeAPI->TimeType)) {
        state->types |= MS_TYPE_TIME;
        kind = CK_TIME;
    }
    else if (t == (PyObject *)(PyDateTimeAPI->DateType)) {
        state->types |= MS_TYPE_DATE;
    }
    else if (t == state->mod->UUIDType) {
        state->types |= MS_TYPE_UUID;
    }
    else if (t == state->mod->DecimalType) {
        state->types |= MS_TYPE_DECIMAL;
    }
    else if (t == (PyObject *)(&Ext_Type)) {
        state->types |= MS_TYPE_EXT;
    }
    else if (t == (PyObject *)(&Raw_Type)) {
        /* Raw is marked with a typecode of 0, nothing to do */
    }
    else if (Py_TYPE(t) == &StructMetaType) {
        out = typenode_collect_struct(state, t);
    }
    else if (Py_TYPE(t) == state->mod->EnumMetaType) {
        out = typenode_collect_enum(state, t);
    }
    else if (origin == (PyObject*)(&PyDict_Type)) {
        kind = CK_MAP;
        if (args != NULL && PyTuple_GET_SIZE(args) != 2) goto invalid;
        out = typenode_collect_dict(
            state,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0),
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 1)
        );
    }
    else if (origin == (PyObject*)(&PyList_Type)) {
        kind = CK_ARRAY;
        if (args != NULL && PyTuple_GET_SIZE(args) != 1) goto invalid;
        out = typenode_collect_array(
            state,
            MS_TYPE_LIST,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0)
        );
    }
    else if (origin == (PyObject*)(&PySet_Type)) {
        kind = CK_ARRAY;
        if (args != NULL && PyTuple_GET_SIZE(args) != 1) goto invalid;
        out = typenode_collect_array(
            state,
            MS_TYPE_SET,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0)
        );
    }
    else if (origin == (PyObject*)(&PyFrozenSet_Type)) {
        kind = CK_ARRAY;
        if (args != NULL && PyTuple_GET_SIZE(args) != 1) goto invalid;
        out = typenode_collect_array(
            state,
            MS_TYPE_FROZENSET,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0)
        );
    }
    else if (origin == (PyObject*)(&PyTuple_Type)) {
        if (args == NULL) {
            kind = CK_ARRAY;
            out = typenode_collect_array(
                state, MS_TYPE_VARTUPLE, state->mod->typing_any
            );
        }
        else if (PyTuple_GET_SIZE(args) == 2 && PyTuple_GET_ITEM(args, 1) == Py_Ellipsis) {
            kind = CK_ARRAY;
            out = typenode_collect_array(
                state, MS_TYPE_VARTUPLE, PyTuple_GET_ITEM(args, 0)
            );
        }
        else if (
            PyTuple_GET_SIZE(args) == 1 &&
            PyTuple_CheckExact(PyTuple_GET_ITEM(args, 0)) &&
            PyTuple_GET_SIZE(PyTuple_GET_ITEM(args, 0)) == 0
        ) {
            /* XXX: this case handles a weird compatibility issue:
             * - Tuple[()].__args__ == ((),)
             * - tuple[()].__args__ == ()
             */
            out = typenode_collect_array(
                state, MS_TYPE_FIXTUPLE, PyTuple_GET_ITEM(args, 0)
            );
        }
        else {
            out = typenode_collect_array(state, MS_TYPE_FIXTUPLE, args);
        }
    }
    else if (origin == state->mod->typing_union) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); i++) {
            out = typenode_collect_type(state, PyTuple_GET_ITEM(args, i));
            if (out < 0) break;
        }
    }
    else if (origin == state->mod->typing_literal) {
        if (state->literals == NULL) {
            state->literals = PyList_New(0);
            if (state->literals == NULL) goto done;
        }
        out = PyList_Append(state->literals, t);
    }
    else if (PyType_Check(t)
        && PyType_IsSubtype((PyTypeObject *)t, &PyDict_Type)
        && PyObject_HasAttr(t, state->mod->str___total__)) {
        out = typenode_collect_typeddict(state, t);
    }
    else if (PyType_Check(t)
        && PyType_IsSubtype((PyTypeObject *)t, &PyTuple_Type)
        && PyObject_HasAttr(t, state->mod->str__fields)) {
        out = typenode_collect_namedtuple(state, t);
    }
    else if (PyType_Check(t)
        && PyObject_HasAttr(t, state->mod->str___dataclass_fields__)) {
        out = typenode_collect_dataclass(state, t);
    }
    else {
        if (origin != NULL) {
            if (!PyType_Check(origin)) goto invalid;
        }
        else {
            if (!PyType_Check(t)) goto invalid;
        }
        out = typenode_collect_custom(
            state,
            (origin != NULL) ? MS_TYPE_CUSTOM_GENERIC : MS_TYPE_CUSTOM,
            t
        );
    }

done:
    Py_XDECREF(t);
    Py_XDECREF(origin);
    Py_XDECREF(args);
    if (out == 0) {
        out = typenode_collect_constraints(state, &constraints, kind, obj);
    }
    return out;

invalid:
    PyErr_Format(PyExc_TypeError, "Type '%R' is not supported", t);
    out = -1;
    goto done;
}

static TypeNode *
TypeNode_Convert(PyObject *obj, bool err_not_json, bool *json_compatible) {
    TypeNode *out = NULL;
    TypeNodeCollectState state = {0};
    state.mod = msgspec_get_global_state();
    state.context = obj;

    /* Traverse `obj` to collect all type annotations at this level */
    if (typenode_collect_type(&state, obj) < 0) goto done;
    /* Handle structs in a second pass */
    if (typenode_collect_convert_structs(&state, err_not_json, json_compatible) < 0) goto done;
    /* Handle literals in a second pass */
    if (typenode_collect_convert_literals(&state) < 0) goto done;
    /* Check type invariants to ensure Union types are valid */
    if (typenode_collect_check_invariants(&state, err_not_json, json_compatible) < 0) goto done;
    /* Populate a new TypeNode, recursing into subtypes as needed */
    out = typenode_from_collect_state(&state, err_not_json, json_compatible);
done:
    typenode_collect_clear_state(&state);
    return out;
}

#define PATH_ELLIPSIS -1
#define PATH_STR -2
#define PATH_KEY -3

typedef struct PathNode {
    struct PathNode *parent;
    Py_ssize_t index;
    PyObject *object;
} PathNode;

/* reverse the parent pointers in the path linked list */
static PathNode *
pathnode_reverse(PathNode *path) {
    PathNode *current = path, *prev = NULL, *next = NULL;
    while (current != NULL) {
        next = current->parent;
        current->parent = prev;
        prev = current;
        current = next;
    }
    return prev;
}

static PyObject *
PathNode_ErrSuffix(PathNode *path) {
    strbuilder parts = {0};
    PathNode *path_orig;
    PyObject *out = NULL, *path_repr = NULL, *groups = NULL, *group = NULL;

    if (path == NULL) {
        return PyUnicode_FromString("");
    }

    /* Reverse the parent pointers for easier printing */
    path = pathnode_reverse(path);

    /* Cache the original path to reset the parent pointers later */
    path_orig = path;

    /* Start with the root element */
    if (!strbuilder_extend_literal(&parts, "`$")) goto cleanup;

    while (path != NULL) {
        if (path->object != NULL) {
            PyObject *name;
            if (path->index == PATH_STR) {
                name = path->object;
            }
            else {
                name = PyTuple_GET_ITEM(
                    ((StructMetaObject *)(path->object))->struct_encode_fields,
                    path->index
                );
            }
            if (!strbuilder_extend_literal(&parts, ".")) goto cleanup;
            if (!strbuilder_extend_unicode(&parts, name)) goto cleanup;
        }
        else if (path->index == PATH_ELLIPSIS) {
            if (!strbuilder_extend_literal(&parts, "[...]")) goto cleanup;
        }
        else if (path->index == PATH_KEY) {
            if (groups == NULL) {
                groups = PyList_New(0);
                if (groups == NULL) goto cleanup;
            }
            if (!strbuilder_extend_literal(&parts, "`")) goto cleanup;
            group = strbuilder_build(&parts);
            if (group == NULL) goto cleanup;
            if (PyList_Append(groups, group) < 0) goto cleanup;
            Py_CLEAR(group);
            strbuilder_extend_literal(&parts, "`key");
        }
        else {
            char buf[20];
            char *p = &buf[20];
            Py_ssize_t x = path->index;
            if (!strbuilder_extend_literal(&parts, "[")) goto cleanup;
            while (x >= 100) {
                const int64_t old = x;
                p -= 2;
                x /= 100;
                memcpy(p, DIGIT_TABLE + ((old - (x * 100)) << 1), 2);
            }
            if (x >= 10) {
                p -= 2;
                memcpy(p, DIGIT_TABLE + (x << 1), 2);
            }
            else {
                *--p = x + '0';
            }
            if (!strbuilder_extend(&parts, p, &buf[20] - p)) goto cleanup;
            if (!strbuilder_extend_literal(&parts, "]")) goto cleanup;
        }
        path = path->parent;
    }
    if (!strbuilder_extend_literal(&parts, "`")) goto cleanup;

    if (groups == NULL) {
        path_repr = strbuilder_build(&parts);
    }
    else {
        group = strbuilder_build(&parts);
        if (group == NULL) goto cleanup;
        if (PyList_Append(groups, group) < 0) goto cleanup;
        PyObject *sep = PyUnicode_FromString(" in ");
        if (sep == NULL) goto cleanup;
        if (PyList_Reverse(groups) < 0) goto cleanup;
        path_repr = PyUnicode_Join(sep, groups);
        Py_DECREF(sep);
    }

    out = PyUnicode_FromFormat(" - at %U", path_repr);

cleanup:
    Py_XDECREF(path_repr);
    Py_XDECREF(group);
    Py_XDECREF(groups);
    pathnode_reverse(path_orig);
    strbuilder_reset(&parts);
    return out;
}

#define ms_raise_validation_error(path, format, ...) \
    do { \
        MsgspecState *st = msgspec_get_global_state(); \
        PyObject *suffix = PathNode_ErrSuffix(path); \
        if (suffix != NULL) { \
            PyErr_Format(st->ValidationError, format, __VA_ARGS__, suffix); \
            Py_DECREF(suffix); \
        } \
    } while (0)

static MS_NOINLINE PyObject *
ms_validation_error(const char *got, TypeNode *type, PathNode *path) {
    PyObject *type_repr = typenode_simple_repr(type);
    if (type_repr != NULL) {
        ms_raise_validation_error(path, "Expected `%U`, got `%s`%U", type_repr, got);
        Py_DECREF(type_repr);
    }
    return NULL;
}

static PyObject *
ms_invalid_cstr_value(const char *cstr, Py_ssize_t size, PathNode *path) {
    PyObject *str = PyUnicode_DecodeUTF8(cstr, size, NULL);
    if (str == NULL) return NULL;
    ms_raise_validation_error(path, "Invalid value '%U'%U", str);
    Py_DECREF(str);
    return NULL;
}

static PyObject *
ms_invalid_cint_value(int64_t val, PathNode *path) {
    ms_raise_validation_error(path, "Invalid value %lld%U", val);
    return NULL;
}

static PyObject *
ms_invalid_cuint_value(uint64_t val, PathNode *path) {
    ms_raise_validation_error(path, "Invalid value %llu%U", val);
    return NULL;
}

static MS_NOINLINE PyObject *
ms_error_unknown_field(const char *key, Py_ssize_t key_size, PathNode *path) {
    PyObject *field = PyUnicode_FromStringAndSize(key, key_size);
    if (field == NULL) return NULL;
    ms_raise_validation_error(
        path, "Object contains unknown field `%U`%U", field
    );
    Py_DECREF(field);
    return NULL;
}

/* Same as ms_raise_validation_error, except doesn't require any format arguments. */
static PyObject *
ms_error_with_path(const char *msg, PathNode *path) {
    MsgspecState *st = msgspec_get_global_state();
    PyObject *suffix = PathNode_ErrSuffix(path);
    if (suffix != NULL) {
        PyErr_Format(st->ValidationError, msg, suffix);
        Py_DECREF(suffix);
    }
    return NULL;
}

static PyTypeObject StructMixinType;

/* To reduce overhead of repeatedly allocating & freeing messages (in e.g. a
 * server), we keep Struct objects below a certain size around in a freelist.
 * This freelist is cleared during major GC collections (as part of traversing
 * the msgspec module).
 *
 * Set STRUCT_FREELIST_MAX_SIZE to 0 to disable the freelist entirely.
 */
#ifndef STRUCT_FREELIST_MAX_SIZE
#define STRUCT_FREELIST_MAX_SIZE 10
#endif
#ifndef STRUCT_FREELIST_MAX_PER_SIZE
#define STRUCT_FREELIST_MAX_PER_SIZE 2000
#endif

#if STRUCT_FREELIST_MAX_SIZE > 0
static PyObject *struct_freelist[STRUCT_FREELIST_MAX_SIZE * 2];
static int struct_freelist_len[STRUCT_FREELIST_MAX_SIZE * 2];

static void
Struct_freelist_clear(void) {
    Py_ssize_t i;
    PyObject *obj;
    /* Non-gc freelist */
    for (i = 0; i < STRUCT_FREELIST_MAX_SIZE; i++) {
        while ((obj = struct_freelist[i]) != NULL) {
            struct_freelist[i] = MS_GET_FIRST_SLOT(obj);
            PyObject_Del(obj);
        }
        struct_freelist_len[i] = 0;
    }
    /* GC freelist */
    for (i = STRUCT_FREELIST_MAX_SIZE; i < STRUCT_FREELIST_MAX_SIZE * 2; i++) {
        while ((obj = struct_freelist[i]) != NULL) {
            struct_freelist[i] = MS_GET_FIRST_SLOT(obj);
            PyObject_GC_Del(obj);
        }
        struct_freelist_len[i] = 0;
    }
}
#endif

/* Note this always allocates an UNTRACKED object */
static PyObject *
Struct_alloc(PyTypeObject *type) {
    PyObject *obj;
    bool is_gc = MS_TYPE_IS_GC(type);

#if STRUCT_FREELIST_MAX_SIZE > 0
    Py_ssize_t size = (type->tp_basicsize - sizeof(PyObject)) / sizeof(void *);
    Py_ssize_t free_ind = (is_gc * STRUCT_FREELIST_MAX_SIZE) + size - 1;

    if (size > 0 &&
        size <= STRUCT_FREELIST_MAX_SIZE &&
        struct_freelist[free_ind] != NULL)
    {
        /* Pop object off freelist */
        struct_freelist_len[free_ind]--;
        obj = struct_freelist[free_ind];
        struct_freelist[free_ind] = MS_GET_FIRST_SLOT(obj);
        MS_SET_FIRST_SLOT(obj, NULL);

        /* Initialize the object. This is mirrored from within `PyObject_Init`,
        * as well as PyType_GenericAlloc */
        obj->ob_type = type;
        Py_INCREF(type);
        _Py_NewReference(obj);
        return obj;
    }
#else
    if (false) {
    }
#endif
    else {
        if (is_gc) {
            obj = PyObject_GC_New(PyObject, type);
        }
        else {
            obj = PyObject_New(PyObject, type);
        }
        if (obj == NULL) return NULL;
        /* Zero out slot fields */
        memset((char *)obj + sizeof(PyObject), '\0', type->tp_basicsize - sizeof(PyObject));
        return obj;
    }
}

/* Mirrored from cpython Objects/typeobject.c */
static void
clear_slots(PyTypeObject *type, PyObject *self)
{
    Py_ssize_t i, n;
    PyMemberDef *mp;

    n = Py_SIZE(type);
    mp = MS_PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
    for (i = 0; i < n; i++, mp++) {
        if (mp->type == T_OBJECT_EX && !(mp->flags & READONLY)) {
            char *addr = (char *)self + mp->offset;
            PyObject *obj = *(PyObject **)addr;
            if (obj != NULL) {
                *(PyObject **)addr = NULL;
                Py_DECREF(obj);
            }
        }
    }
}

static void
Struct_dealloc(PyObject *self) {
    PyTypeObject *type, *base;
    bool is_gc;

    type = Py_TYPE(self);

    is_gc = MS_TYPE_IS_GC(type);

    if (is_gc) {
        PyObject_GC_UnTrack(self);
    }

    Py_TRASHCAN_BEGIN(self, Struct_dealloc)

    /* Maybe call a finalizer */
    if (type->tp_finalize) {
        /* If resurrected, exit early */
        if (PyObject_CallFinalizerFromDealloc(self) < 0) goto done;
    }

    /* Maybe clear weakrefs */
    if (type->tp_weaklistoffset) {
        PyObject_ClearWeakRefs(self);
    }

    /* Clear all slots */
    base = type;
    while (base != NULL) {
        if (Py_SIZE(base)) {
            clear_slots(base, self);
        }
        base = base->tp_base;
    }

#if STRUCT_FREELIST_MAX_SIZE > 0
    Py_ssize_t size = (type->tp_basicsize - sizeof(PyObject)) / sizeof(void *);
    Py_ssize_t free_ind = (is_gc * STRUCT_FREELIST_MAX_SIZE) + size - 1;
    if (size > 0 &&
        size <= STRUCT_FREELIST_MAX_SIZE &&
        struct_freelist_len[free_ind] < STRUCT_FREELIST_MAX_PER_SIZE)
    {
        /* XXX: Python 3.11 requires GC types to have a valid ob_type field
         * when deleted, it uses this to determine the header size. This means
         * that objects shoved in the freelist need to still have an `ob_type`
         * field. However, we can't decref any GC types in
         * `Struct_freelist_clear`, since that's called during a GC traversal
         * and deleting GC objects in a traversal leads to segfaults. To work
         * around this, we set `ob_type` to a valid statically allocated type
         * just so `PyObject_GC_Del` still works. This means that we can still
         * clear the freelist in a traversal without ever needing to free a GC
         * type. The actual type is still decref'd here.
         *
         * All of this is very complicated and finicky, and indicates we may need
         * to reevaluate if the freelist is even worth it.
         * */

        /* Push object onto freelist */
        if (is_gc) {
            /* Reset GC state before pushing onto freelist */
            MS_AS_GC(self)->_gc_next = 0;
            MS_AS_GC(self)->_gc_prev = 0;
            /* A statically allocated GC type */
            self->ob_type = &IntLookup_Type;
        }
        else {
            /* A statically allocated non-GC type */
            self->ob_type = &StructMixinType;
        }
        struct_freelist_len[free_ind]++;
        MS_SET_FIRST_SLOT(self, struct_freelist[free_ind]);
        struct_freelist[free_ind] = self;
    }
#else
    if (false) {
    }
#endif
    else {
        type->tp_free(self);
    }
    /* Decref the object type immediately */
    Py_DECREF(type);
done:
    Py_TRASHCAN_END
}

static MS_INLINE Py_ssize_t
StructMeta_get_field_index(
    StructMetaObject *self, const char * key, Py_ssize_t key_size, Py_ssize_t *pos
) {
    const char *field;
    Py_ssize_t nfields, field_size, i, offset = *pos;
    nfields = PyTuple_GET_SIZE(self->struct_encode_fields);
    for (i = offset; i < nfields; i++) {
        field = unicode_str_and_size_nocheck(
            PyTuple_GET_ITEM(self->struct_encode_fields, i), &field_size
        );
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i < (nfields - 1) ? (i + 1) : 0;
            return i;
        }
    }
    for (i = 0; i < offset; i++) {
        field = unicode_str_and_size_nocheck(
            PyTuple_GET_ITEM(self->struct_encode_fields, i), &field_size
        );
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i + 1;
            return i;
        }
    }
    /* Not a field, check if it matches the tag field (if present) */
    if (MS_UNLIKELY(self->struct_tag_field != NULL)) {
        Py_ssize_t tag_field_size;
        const char *tag_field;
        tag_field = unicode_str_and_size_nocheck(self->struct_tag_field, &tag_field_size);
        if (key_size == tag_field_size && memcmp(key, tag_field, key_size) == 0) {
            return -2;
        }
    }
    return -1;
}

static int
dict_discard(PyObject *dict, PyObject *key) {
    int status = PyDict_Contains(dict, key);
    if (status < 0)
        return status;
    return (status == 1) ? PyDict_DelItem(dict, key) : 0;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames);

/* setattr for frozen=True types */
static int
Struct_setattro_frozen(PyObject *self, PyObject *key, PyObject *value) {
    PyErr_Format(
        PyExc_AttributeError, "immutable type: '%s'", Py_TYPE(self)->tp_name
    );
    return -1;
}

/* setattr for frozen=False, gc=True types */
static int
Struct_setattro_default(PyObject *self, PyObject *key, PyObject *value) {
    if (PyObject_GenericSetAttr(self, key, value) < 0)
        return -1;
    if (value != NULL && MS_OBJ_IS_GC(value) && !MS_IS_TRACKED(self))
        PyObject_GC_Track(self);
    return 0;
}

static PyObject*
rename_lower(PyObject *rename, PyObject *field) {
    return PyObject_CallMethod(field, "lower", NULL);
}

static PyObject*
rename_upper(PyObject *rename, PyObject *field) {
    return PyObject_CallMethod(field, "upper", NULL);
}

static PyObject*
rename_kebab(PyObject *rename, PyObject *field) {
    PyObject *underscore = NULL, *dash = NULL, *temp = NULL, *out = NULL;
    underscore = PyUnicode_FromStringAndSize("_", 1);
    if (underscore == NULL) goto error;
    dash = PyUnicode_FromStringAndSize("-", 1);
    if (dash == NULL) goto error;
    temp = PyObject_CallMethod(field, "strip", "s", "_");
    if (temp == NULL) goto error;
    out = PyUnicode_Replace(temp, underscore, dash, -1);
error:
    Py_XDECREF(underscore);
    Py_XDECREF(dash);
    Py_XDECREF(temp);
    return out;
}

static PyObject*
rename_camel_inner(PyObject *field, bool cap_first) {
    PyObject *parts = NULL, *out = NULL, *empty = NULL;
    PyObject *underscore = PyUnicode_FromStringAndSize("_", 1);
    if (underscore == NULL) return NULL;

    parts = PyUnicode_Split(field, underscore, -1);
    if (parts == NULL) goto cleanup;

    if (PyList_GET_SIZE(parts) == 1 && !cap_first) {
        Py_INCREF(field);
        out = field;
        goto cleanup;
    }

    bool first = true;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(parts); i++) {
        PyObject *part = PyList_GET_ITEM(parts, i);
        if (PyUnicode_GET_LENGTH(part) == 0) continue;
        if (!first || cap_first) {
            /* convert part to title case, inplace in the list */
            PyObject *part_title = PyObject_CallMethod(part, "title", NULL);
            if (part_title == NULL) goto cleanup;
            PyList_SET_ITEM(parts, i, part_title);
            Py_DECREF(part);
        }
        first = false;
    }
    empty = PyUnicode_FromString("");
    if (empty == NULL) goto cleanup;
    out = PyUnicode_Join(empty, parts);

cleanup:
    Py_XDECREF(empty);
    Py_XDECREF(underscore);
    Py_XDECREF(parts);
    return out;
}

static PyObject*
rename_camel(PyObject *rename, PyObject *field) {
    return rename_camel_inner(field, false);
}

static PyObject*
rename_pascal(PyObject *rename, PyObject *field) {
    return rename_camel_inner(field, true);
}

static PyObject*
rename_callable(PyObject *rename, PyObject *field) {
    PyObject *temp = CALL_ONE_ARG(rename, field);
    if (temp == NULL) return NULL;
    if (PyUnicode_CheckExact(temp)) return temp;
    if (temp == Py_None) {
        Py_DECREF(temp);
        Py_INCREF(field);
        return field;
    }
    PyErr_Format(
        PyExc_TypeError,
        "Expected calling `rename` to return a `str` or `None`, got `%.200s`",
        Py_TYPE(temp)->tp_name
    );
    Py_DECREF(temp);
    return NULL;
}

static PyObject*
rename_mapping(PyObject *rename, PyObject *field) {
    PyObject *temp = PyObject_GetItem(rename, field);
    if (temp == NULL) {
        PyErr_Clear();
        Py_INCREF(field);
        return field;
    }
    else if (temp == Py_None) {
        Py_DECREF(temp);
        Py_INCREF(field);
        return field;
    }
    else if (PyUnicode_CheckExact(temp)) {
        return temp;
    }
    PyErr_Format(
        PyExc_TypeError,
        "Expected `rename[field]` to return a `str` or `None`, got `%.200s`",
        Py_TYPE(temp)->tp_name
    );
    Py_DECREF(temp);
    return NULL;
}

typedef struct {
    /* Temporary state. All owned references. */
    PyObject *defaults_lk;
    PyObject *offsets_lk;
    PyObject *kwonly_fields;
    PyObject *slots;
    PyObject *namespace;
    /* Output values. All owned references. */
    PyObject *fields;
    PyObject *encode_fields;
    PyObject *defaults;
    PyObject *match_args;
    PyObject *tag;
    PyObject *tag_field;
    PyObject *tag_value;
    Py_ssize_t *offsets;
    Py_ssize_t nkwonly;
    Py_ssize_t n_trailing_defaults;
    /* Configuration values. All borrowed references. */
    PyObject *name;
    PyObject *temp_tag_field;
    PyObject *temp_tag;
    PyObject *rename;
    int omit_defaults;
    int forbid_unknown_fields;
    int frozen;
    int eq;
    int order;
    int array_like;
    int gc;
    int weakref;
    bool already_has_weakref;
} StructMetaInfo;

static int
structmeta_check_namespace(PyObject *namespace) {
    static const char *attrs[] = {"__init__", "__new__", "__slots__"};
    Py_ssize_t nattrs = 3;

    for (Py_ssize_t i = 0; i < nattrs; i++) {
        if (PyDict_GetItemString(namespace, attrs[i]) != NULL) {
            PyErr_Format(PyExc_TypeError, "Struct types cannot define %s", attrs[i]);
            return -1;
        }
    }
    return 0;
}

static PyObject *
structmeta_get_module_ns(StructMetaInfo *info) {
    PyObject *name = PyDict_GetItemString(info->namespace, "__module__");
    if (name == NULL) return NULL;
    PyObject *modules = PySys_GetObject("modules");
    if (modules == NULL) return NULL;
    PyObject *mod = PyDict_GetItem(modules, name);
    if (mod == NULL) return NULL;
    return PyObject_GetAttrString(mod, "__dict__");
}

static int
structmeta_collect_base(StructMetaInfo *info, PyObject *base) {
    if ((PyTypeObject *)base == &StructMixinType) return 0;

    if (!(PyType_Check(base) && (Py_TYPE(base) == &StructMetaType))) {
        PyErr_SetString(
            PyExc_TypeError,
            "All base classes must be subclasses of msgspec.Struct"
        );
        return -1;
    }

    if (((PyTypeObject *)base)->tp_weaklistoffset) {
        info->already_has_weakref = true;
    }

    StructMetaObject *st_type = (StructMetaObject *)base;

    /* Inherit config fields */
    if (st_type->struct_tag_field != NULL) {
        info->temp_tag_field = st_type->struct_tag_field;
    }
    if (st_type->struct_tag != NULL) {
        info->temp_tag = st_type->struct_tag;
    }
    if (st_type->rename != NULL) {
        info->rename = st_type->rename;
    }
    info->frozen = STRUCT_MERGE_OPTIONS(info->frozen, st_type->frozen);
    info->eq = STRUCT_MERGE_OPTIONS(info->eq, st_type->eq);
    info->order = STRUCT_MERGE_OPTIONS(info->order, st_type->order);
    info->array_like = STRUCT_MERGE_OPTIONS(info->array_like, st_type->array_like);
    info->gc = STRUCT_MERGE_OPTIONS(info->gc, st_type->gc);
    info->omit_defaults = STRUCT_MERGE_OPTIONS(info->omit_defaults, st_type->omit_defaults);
    info->forbid_unknown_fields = STRUCT_MERGE_OPTIONS(
        info->forbid_unknown_fields, st_type->forbid_unknown_fields
    );

    PyObject *fields = st_type->struct_fields;
    PyObject *defaults = st_type->struct_defaults;
    Py_ssize_t *offsets = st_type->struct_offsets;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    Py_ssize_t nkwonly = st_type->nkwonly;
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(defaults);
    Py_ssize_t defaults_offset = nfields - ndefaults;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        PyObject *default_val = NODEFAULT;
        if (i >= defaults_offset) {
            default_val = PyTuple_GET_ITEM(defaults, i - defaults_offset);
        }
        if (PyDict_SetItem(info->defaults_lk, field, default_val) < 0) return -1;

        /* Mark the field as kwonly or not */
        if (i >= (nfields - nkwonly)) {
            if (PySet_Add(info->kwonly_fields, field) < 0) return -1;
        }
        else {
            if (PySet_Discard(info->kwonly_fields, field) < 0) return -1;
        }

        PyObject *offset = PyLong_FromSsize_t(offsets[i]);
        if (offset == NULL) return -1;
        bool errored = PyDict_SetItem(info->offsets_lk, field, offset) < 0;
        Py_DECREF(offset);
        if (errored) return -1;
    }
    return 0;
}

static int
structmeta_process_default(StructMetaInfo *info, PyObject *field) {
    PyObject *obj = PyDict_GetItem(info->namespace, field);
    if (obj == NULL) {
        return PyDict_SetItem(info->defaults_lk, field, NODEFAULT);
    }

    PyObject* default_val = NULL;
    PyTypeObject *type = Py_TYPE(obj);

    if (type == &Field_Type) {
        Field *f = (Field *)obj;
        if (f->default_value != UNSET) {
            obj = f->default_value;
            type = Py_TYPE(obj);
        }
        else if (f->default_factory != UNSET) {
            default_val = Factory_New(f->default_factory);
            if (default_val == NULL) return -1;
            goto done;
        }
        else {
            if (PyDict_SetItem(info->defaults_lk, field, NODEFAULT) < 0) return -1;
            goto done;
        }
    }

    if (type == &PyDict_Type) {
        if (PyDict_GET_SIZE(obj) != 0) goto error_nonempty;
        default_val = Factory_New((PyObject*)(&PyDict_Type));
        if (default_val == NULL) return -1;
    }
    else if (type == &PyList_Type) {
        if (PyList_GET_SIZE(obj) != 0) goto error_nonempty;
        default_val = Factory_New((PyObject*)(&PyList_Type));
        if (default_val == NULL) return -1;
    }
    else if (type == &PySet_Type) {
        if (PySet_GET_SIZE(obj) != 0) goto error_nonempty;
        default_val = Factory_New((PyObject*)(&PySet_Type));
        if (default_val == NULL) return -1;
    }
    else if (type == &PyByteArray_Type) {
        if (PyByteArray_GET_SIZE(obj) != 0) goto error_nonempty;
        default_val = Factory_New((PyObject*)(&PyByteArray_Type));
        if (default_val == NULL) return -1;
    }
    else if (
        (Py_TYPE(type) == &StructMetaType) &&
        ((StructMetaObject *)type)->frozen != OPT_TRUE
    ) {
        goto error_mutable_struct;
    }
    else {
        Py_INCREF(obj);
        default_val = obj;
    }

done:
    if (dict_discard(info->namespace, field) < 0) {
        Py_DECREF(default_val);
        return -1;
    }
    int status = PyDict_SetItem(info->defaults_lk, field, default_val);
    Py_DECREF(default_val);
    return status;

error_nonempty:
    PyErr_Format(
        PyExc_TypeError,
        "Using a non-empty mutable collection (%R) as a default value is unsafe. "
        "Instead configure a `default_factory` for this field.",
        obj
    );
    return -1;

error_mutable_struct:
    PyErr_Format(
        PyExc_TypeError,
        "Using a mutable struct object (%R) as a default value is unsafe. "
        "Either configure a `default_factory` for this field, or set "
        "`frozen=True` on `%.200s`",
        obj, type->tp_name
    );
    return -1;
}

static int
structmeta_is_classvar(
    StructMetaInfo *info, MsgspecState *mod, PyObject *ann, PyObject **module_ns
) {
    PyTypeObject *ann_type = Py_TYPE(ann);
    if (ann_type == &PyUnicode_Type) {
        Py_ssize_t ann_len;
        const char *ann_buf = unicode_str_and_size(ann, &ann_len);
        if (ann_len < 8) return 0;
        if (memcmp(ann_buf, "ClassVar", 8) == 0) {
            if (ann_len != 8 && ann_buf[8] != '[') return 0;
            if (*module_ns == NULL) {
                *module_ns = structmeta_get_module_ns(info);
            }
            if (*module_ns == NULL) return 0;
            PyObject *temp = PyDict_GetItemString(*module_ns, "ClassVar");
            return temp == mod->typing_classvar;
        }
        if (ann_len < 15) return 0;
        if (memcmp(ann_buf, "typing.ClassVar", 15) == 0) {
            if (ann_len != 15 && ann_buf[15] != '[') return 0;
            if (*module_ns == NULL) {
                *module_ns = structmeta_get_module_ns(info);
            }
            if (*module_ns == NULL) return 0;
            PyObject *temp = PyDict_GetItemString(*module_ns, "typing");
            if (temp == NULL) return 0;
            temp = PyObject_GetAttrString(temp, "ClassVar");
            int status = temp == mod->typing_classvar;
            Py_DECREF(temp);
            return status;
        }
    }
    else {
        if (ann == mod->typing_classvar) {
            return 1;
        }
        else if ((PyObject *)ann_type == mod->typing_generic_alias) {
            PyObject *temp = PyObject_GetAttr(ann, mod->str___origin__);
            if (temp == NULL) return -1;
            int status = temp == mod->typing_classvar;
            Py_DECREF(temp);
            return status;
        }
        return 0;
    }
    return 0;
}

static int
structmeta_collect_fields(StructMetaInfo *info, MsgspecState *mod, bool kwonly) {
    PyObject *annotations = PyDict_GetItemString(
        info->namespace, "__annotations__"
    );
    if (annotations == NULL) return 0;

    if (!PyDict_Check(annotations)) {
        PyErr_SetString(PyExc_TypeError, "__annotations__ must be a dict");
        return -1;
    }

    PyObject *module_ns = NULL;
    PyObject *field, *value;
    Py_ssize_t i = 0;
    while (PyDict_Next(annotations, &i, &field, &value)) {
        if (!PyUnicode_CheckExact(field)) {
            PyErr_SetString(
                PyExc_TypeError, "__annotations__ keys must be strings"
            );
            goto error;
        }

        if (PyUnicode_Compare(field, mod->str___weakref__) == 0) {
            PyErr_SetString(
                PyExc_TypeError, "Cannot have a struct field named '__weakref__'"
            );
            goto error;
        }
        int status = structmeta_is_classvar(info, mod, value, &module_ns);
        if (status == 1) continue;
        if (status == -1) goto error;

        /* If the field is new, add it to slots */
        if (PyDict_GetItem(info->defaults_lk, field) == NULL) {
            if (PyList_Append(info->slots, field) < 0) goto error;
        }

        if (kwonly) {
            if (PySet_Add(info->kwonly_fields, field) < 0) goto error;
        }
        else {
            if (PySet_Discard(info->kwonly_fields, field) < 0) goto error;
        }

        if (structmeta_process_default(info, field) < 0) goto error;
    }
    return 0;
error:
    Py_XDECREF(module_ns);
    return -1;
}

static int
structmeta_construct_fields(StructMetaInfo *info, MsgspecState *mod) {
    Py_ssize_t nfields = PyDict_GET_SIZE(info->defaults_lk);
    Py_ssize_t nkwonly = PySet_GET_SIZE(info->kwonly_fields);
    Py_ssize_t field_index = 0;

    info->fields = PyTuple_New(nfields);
    if (info->fields == NULL) return -1;
    info->defaults = PyList_New(0);

    /* First pass - handle all non-kwonly fields. */
    PyObject *field, *default_val;
    Py_ssize_t pos = 0;
    while (PyDict_Next(info->defaults_lk, &pos, &field, &default_val)) {
        int kwonly = PySet_Contains(info->kwonly_fields, field);
        if (kwonly < 0) return -1;
        if (kwonly) continue;

        Py_INCREF(field);
        PyTuple_SET_ITEM(info->fields, field_index, field);

        if (default_val == NODEFAULT) {
            if (PyList_GET_SIZE(info->defaults)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Required field '%U' cannot follow optional fields. Either "
                    "reorder the struct fields, or set `kw_only=True` in the "
                    "struct definition.",
                    field
                );
                return -1;
            }
        }
        else {
            if (PyList_Append(info->defaults, default_val) < 0) return -1;
        }
        field_index++;
    }
    /* Next handle any kw_only fields */
    if (nkwonly) {
        PyObject *field, *default_val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(info->defaults_lk, &pos, &field, &default_val)) {
            int kwonly = PySet_Contains(info->kwonly_fields, field);
            if (kwonly < 0) return -1;
            if (!kwonly) continue;

            Py_INCREF(field);
            PyTuple_SET_ITEM(info->fields, field_index, field);
            if (PyList_GET_SIZE(info->defaults) || default_val != NODEFAULT) {
                if (PyList_Append(info->defaults, default_val) < 0) return -1;
            }
            field_index++;
        }
    }

    /* Convert defaults list to tuple */
    PyObject *temp_defaults = PyList_AsTuple(info->defaults);
    Py_DECREF(info->defaults);
    info->defaults = temp_defaults;
    if (info->defaults == NULL) return -1;

    /* Compute n_trailing_defaults */
    info->nkwonly = nkwonly;
    info->n_trailing_defaults = 0;
    for (Py_ssize_t i = PyTuple_GET_SIZE(info->defaults) - 1; i >= 0; i--) {
        if (PyTuple_GET_ITEM(info->defaults, i) == NODEFAULT) break;
        info->n_trailing_defaults++;
    }

    /* Construct __match_args__ */
    info->match_args = PyTuple_GetSlice(info->fields, 0, nfields - nkwonly);
    if (info->match_args == NULL) return -1;

    /* Construct __slots__ */
    if (info->weakref == OPT_TRUE && !info->already_has_weakref) {
        if (PyList_Append(info->slots, mod->str___weakref__) < 0) return -1;
    }
    else if (info->weakref == OPT_FALSE && info->already_has_weakref) {
        PyErr_SetString(
            PyExc_ValueError,
            "Cannot set `weakref=False` if base class already has `weakref=True`"
        );
        return -1;
    }

    if (PyList_Sort(info->slots) < 0) return -1;

    PyObject *slots = PyList_AsTuple(info->slots);
    if (slots == NULL) return -1;
    int out = PyDict_SetItemString(info->namespace, "__slots__", slots);
    Py_DECREF(slots);
    return out;
}


static int json_str_requires_escaping(PyObject *);

static int
structmeta_construct_encode_fields(StructMetaInfo *info)
{
    if (info->rename == NULL) {
        /* Nothing to do, use original field tuple */
        Py_INCREF(info->fields);
        info->encode_fields = info->fields;
        return 0;
    }

    PyObject* (*method)(PyObject *, PyObject *);
    if (PyUnicode_CheckExact(info->rename)) {
        if (PyUnicode_CompareWithASCIIString(info->rename, "lower") == 0) {
            method = &rename_lower;
        }
        else if (PyUnicode_CompareWithASCIIString(info->rename, "upper") == 0) {
            method = &rename_upper;
        }
        else if (PyUnicode_CompareWithASCIIString(info->rename, "camel") == 0) {
            method = &rename_camel;
        }
        else if (PyUnicode_CompareWithASCIIString(info->rename, "pascal") == 0) {
            method = &rename_pascal;
        }
        else if (PyUnicode_CompareWithASCIIString(info->rename, "kebab") == 0) {
            method = &rename_kebab;
        }
        else {
            PyErr_Format(PyExc_ValueError, "rename='%U' is unsupported", info->rename);
            return -1;
        }
    }
    else if (PyCallable_Check(info->rename)) {
        method = &rename_callable;
    }
    else if (PyMapping_Check(info->rename)) {
        method = &rename_mapping;
    }
    else {
        PyErr_SetString(PyExc_TypeError, "`rename` must be a str, callable, or mapping");
        return -1;
    }

    info->encode_fields = PyTuple_New(PyTuple_GET_SIZE(info->fields));
    if (info->encode_fields == NULL) return -1;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(info->fields); i++) {
        PyObject *temp = method(info->rename, PyTuple_GET_ITEM(info->fields, i));
        if (temp == NULL) return -1;
        PyTuple_SET_ITEM(info->encode_fields, i, temp);
    }

    /* Ensure that renamed fields don't collide */
    PyObject *fields_set = PySet_New(info->encode_fields);
    if (fields_set == NULL) return -1;
    bool unique = PySet_GET_SIZE(fields_set) == PyTuple_GET_SIZE(info->encode_fields);
    Py_DECREF(fields_set);
    if (!unique) {
        PyErr_SetString(
            PyExc_ValueError,
            "Multiple fields rename to the same name, field names "
            "must be unique"
        );
        return -1;
    }

    /* Ensure all renamed fields contain characters that don't require quoting
     * in JSON. This isn't strictly required, but usage of such characters is
     * extremely unlikely, and forbidding this allows us to optimize encoding */
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(info->encode_fields); i++) {
        PyObject *field = PyTuple_GET_ITEM(info->encode_fields, i);
        Py_ssize_t status = json_str_requires_escaping(field);
        if (status == -1) return -1;
        if (status == 1) {
            PyErr_Format(
                PyExc_ValueError,
                "Renamed field names must not contain '\\', '\"', or control characters "
                "('\\u0000' to '\\u001F') - '%U' is invalid",
                field
            );
            return -1;
        }
    }
    return 0;
}

static int
structmeta_construct_tag(StructMetaInfo *info, MsgspecState *mod) {
    if (info->temp_tag == Py_False) return 0;
    if (info->temp_tag == NULL && info->temp_tag_field == NULL) return 0;

    Py_XINCREF(info->temp_tag);
    info->tag = info->temp_tag;

    /* Determine the tag value */
    if (info->temp_tag == NULL || info->temp_tag == Py_True) {
        Py_INCREF(info->name);
        info->tag_value = info->name;
    }
    else {
        if (PyCallable_Check(info->temp_tag)) {
            info->tag_value = CALL_ONE_ARG(info->temp_tag, info->name);
            if (info->tag_value == NULL) return -1;
        }
        else {
            Py_INCREF(info->temp_tag);
            info->tag_value = info->temp_tag;
        }
        if (PyLong_CheckExact(info->tag_value)) {
            int64_t val = PyLong_AsLongLong(info->tag_value);
            if (val == -1 && PyErr_Occurred()) {
                PyErr_SetString(
                    PyExc_ValueError,
                    "Integer `tag` values must be within [-2**63, 2**63 - 1]"
                );
                return -1;
            }
        }
        else if (!PyUnicode_CheckExact(info->tag_value)) {
            PyErr_SetString(PyExc_TypeError, "`tag` must be a `str` or an `int`");
            return -1;
        }
    }

    /* Next determine the tag_field to use. */
    if (info->temp_tag_field == NULL) {
        info->tag_field = mod->str_type;
        Py_INCREF(info->tag_field);
    }
    else if (PyUnicode_CheckExact(info->temp_tag_field)) {
        info->tag_field = info->temp_tag_field;
        Py_INCREF(info->tag_field);
    }
    else {
        PyErr_SetString(PyExc_TypeError, "`tag_field` must be a `str`");
        return -1;
    }
    int contains = PySequence_Contains(info->encode_fields, info->tag_field);
    if (contains < 0) return -1;
    if (contains) {
        PyErr_Format(
            PyExc_ValueError,
            "`tag_field='%U' conflicts with an existing field of that name",
            info->tag_field
        );
        return -1;
    }
    return 0;
}

static int
structmeta_construct_offsets(StructMetaInfo *info, StructMetaObject *cls) {
    PyMemberDef *mp = MS_PyHeapType_GET_MEMBERS(cls);
    for (Py_ssize_t i = 0; i < Py_SIZE(cls); i++, mp++) {
        PyObject *offset = PyLong_FromSsize_t(mp->offset);
        if (offset == NULL) return -1;
        bool errored = PyDict_SetItemString(info->offsets_lk, mp->name, offset) < 0;
        Py_DECREF(offset);
        if (errored) return -1;
    }

    info->offsets = PyMem_New(Py_ssize_t, PyTuple_GET_SIZE(info->fields));
    if (info->offsets == NULL) return -1;

    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(info->fields); i++) {
        PyObject *field = PyTuple_GET_ITEM(info->fields, i);
        PyObject *offset = PyDict_GetItem(info->offsets_lk, field);
        if (offset == NULL) {
            PyErr_Format(PyExc_RuntimeError, "Failed to get offset for %R", field);
            return -1;
        }
        info->offsets[i] = PyLong_AsSsize_t(offset);
    }
    return 0;
}


static PyObject *
StructMeta_new_inner(
    PyTypeObject *type, PyObject *name, PyObject *bases, PyObject *namespace,
    PyObject *arg_tag_field, PyObject *arg_tag, PyObject *arg_rename,
    int arg_omit_defaults, int arg_forbid_unknown_fields,
    int arg_frozen, int arg_eq, int arg_order, bool arg_kw_only,
    int arg_array_like, int arg_gc, int arg_weakref
) {
    StructMetaObject *cls = NULL;
    MsgspecState *mod = msgspec_get_global_state();
    bool ok = false;

    if (structmeta_check_namespace(namespace) < 0) return NULL;

    StructMetaInfo info = {
        .defaults_lk = NULL,
        .offsets_lk = NULL,
        .kwonly_fields = NULL,
        .slots = NULL,
        .namespace = NULL,
        .fields = NULL,
        .encode_fields = NULL,
        .defaults = NULL,
        .match_args = NULL,
        .tag = NULL,
        .tag_field = NULL,
        .tag_value = NULL,
        .offsets = NULL,
        .nkwonly = 0,
        .n_trailing_defaults = 0,
        .name = name,
        .temp_tag_field = NULL,
        .temp_tag = NULL,
        .rename = NULL,
        .omit_defaults = -1,
        .forbid_unknown_fields = -1,
        .frozen = -1,
        .eq = -1,
        .order = -1,
        .array_like = -1,
        .gc = -1,
        .weakref = arg_weakref,
        .already_has_weakref = false,
    };

    info.defaults_lk = PyDict_New();
    if (info.defaults_lk == NULL) goto cleanup;
    info.offsets_lk = PyDict_New();
    if (info.offsets_lk == NULL) goto cleanup;
    info.kwonly_fields = PySet_New(NULL);
    if (info.kwonly_fields == NULL) goto cleanup;
    info.namespace = PyDict_Copy(namespace);
    if (info.namespace == NULL) goto cleanup;
    info.slots = PyList_New(0);
    if (info.slots == NULL) goto cleanup;

    /* Extract info from base classes in reverse MRO order */
    for (Py_ssize_t i = PyTuple_GET_SIZE(bases) - 1; i >= 0; i--) {
        PyObject *base = PyTuple_GET_ITEM(bases, i);
        if (structmeta_collect_base(&info, base) < 0) goto cleanup;
    }

    /* Process configuration options */
    if (arg_tag != NULL && arg_tag != Py_None) {
        info.temp_tag = arg_tag;
    }
    if (arg_tag_field != NULL && arg_tag_field != Py_None) {
        info.temp_tag_field = arg_tag_field;
    }
    if (arg_rename != NULL) {
        info.rename = arg_rename == Py_None ? NULL : arg_rename;
    }
    info.frozen = STRUCT_MERGE_OPTIONS(info.frozen, arg_frozen);
    info.eq = STRUCT_MERGE_OPTIONS(info.eq, arg_eq);
    info.order = STRUCT_MERGE_OPTIONS(info.order, arg_order);
    info.array_like = STRUCT_MERGE_OPTIONS(info.array_like, arg_array_like);
    info.gc = STRUCT_MERGE_OPTIONS(info.gc, arg_gc);
    info.omit_defaults = STRUCT_MERGE_OPTIONS(info.omit_defaults, arg_omit_defaults);
    info.forbid_unknown_fields = STRUCT_MERGE_OPTIONS(info.forbid_unknown_fields, arg_forbid_unknown_fields);

    if (info.eq == OPT_FALSE && info.order == OPT_TRUE) {
        PyErr_SetString(PyExc_ValueError, "eq must be true if order is true");
        goto cleanup;
    }

    /* Collect new fields and defaults */
    if (structmeta_collect_fields(&info, mod, arg_kw_only) < 0) goto cleanup;

    /* Construct fields and defaults */
    if (structmeta_construct_fields(&info, mod) < 0) goto cleanup;

    /* Construct encode_fields */
    if (structmeta_construct_encode_fields(&info) < 0) goto cleanup;

    /* Construct tag, tag_field, & tag_value */
    if (structmeta_construct_tag(&info, mod) < 0) goto cleanup;

    /* Construct type */
    PyObject *args = Py_BuildValue("(OOO)", name, bases, info.namespace);
    if (args == NULL) goto cleanup;
    cls = (StructMetaObject *) PyType_Type.tp_new(type, args, NULL);
    Py_CLEAR(args);
    if (cls == NULL) goto cleanup;

    /* Fill in type methods */
    ((PyTypeObject *)cls)->tp_vectorcall = (vectorcallfunc)Struct_vectorcall;
    ((PyTypeObject *)cls)->tp_dealloc = Struct_dealloc;
    if (info.gc == OPT_FALSE) {
        ((PyTypeObject *)cls)->tp_flags &= ~Py_TPFLAGS_HAVE_GC;
        ((PyTypeObject *)cls)->tp_free = &PyObject_Free;
    }
    else {
        ((PyTypeObject *)cls)->tp_flags |= Py_TPFLAGS_HAVE_GC;
        ((PyTypeObject *)cls)->tp_free = &PyObject_GC_Del;
    }
    if (info.frozen == OPT_TRUE) {
        ((PyTypeObject *)cls)->tp_setattro = &Struct_setattro_frozen;
    }
    else if (info.gc == OPT_FALSE) {
        ((PyTypeObject *)cls)->tp_setattro = &PyObject_GenericSetAttr;
    }
    else {
        ((PyTypeObject *)cls)->tp_setattro = &Struct_setattro_default;
    }

    /* Fill in struct offsets */
    if (structmeta_construct_offsets(&info, cls) < 0) goto cleanup;

    cls->nkwonly = info.nkwonly;
    cls->n_trailing_defaults = info.n_trailing_defaults;
    cls->struct_offsets = info.offsets;
    Py_INCREF(info.fields);
    cls->struct_fields = info.fields;
    Py_INCREF(info.defaults);
    cls->struct_defaults = info.defaults;
    Py_INCREF(info.encode_fields);
    cls->struct_encode_fields = info.encode_fields;
    Py_INCREF(info.match_args);
    cls->match_args = info.match_args;
    Py_XINCREF(info.tag);
    cls->struct_tag = info.tag;
    Py_XINCREF(info.tag_field);
    cls->struct_tag_field = info.tag_field;
    Py_XINCREF(info.tag_value);
    cls->struct_tag_value = info.tag_value;
    Py_XINCREF(info.rename);
    cls->rename = info.rename;
    cls->frozen = info.frozen;
    cls->eq = info.eq;
    cls->order = info.order;
    cls->array_like = info.array_like;
    cls->gc = info.gc;
    cls->omit_defaults = info.omit_defaults;
    cls->forbid_unknown_fields = info.forbid_unknown_fields;

    ok = true;

cleanup:
    /* Temporary structures */
    Py_XDECREF(info.defaults_lk);
    Py_XDECREF(info.offsets_lk);
    Py_XDECREF(info.kwonly_fields);
    Py_XDECREF(info.slots);
    Py_XDECREF(info.namespace);
    /* Constructed outputs */
    Py_XDECREF(info.fields);
    Py_XDECREF(info.encode_fields);
    Py_XDECREF(info.defaults);
    Py_XDECREF(info.match_args);
    Py_XDECREF(info.tag);
    Py_XDECREF(info.tag_field);
    Py_XDECREF(info.tag_value);
    if (!ok) {
        if (info.offsets != NULL) {
            PyMem_Free(info.offsets);
        }
        Py_XDECREF(cls);
        return NULL;
    }
    return (PyObject *) cls;
}

static PyObject *
StructMeta_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *name = NULL, *bases = NULL, *namespace = NULL;
    PyObject *arg_tag_field = NULL, *arg_tag = NULL, *arg_rename = NULL;
    int arg_omit_defaults = -1, arg_forbid_unknown_fields = -1;
    int arg_frozen = -1, arg_eq = -1, arg_order = -1;
    int arg_array_like = -1, arg_gc = -1, arg_weakref = -1;
    bool arg_kw_only = false;

    static char *kwlist[] = {
        "name", "bases", "dict",
        "tag_field", "tag", "rename",
        "omit_defaults", "forbid_unknown_fields",
        "frozen", "eq", "order", "kw_only",
        "array_like", "gc", "weakref",
        NULL
    };

    /* Parse arguments: (name, bases, dict) */
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "UO!O!|$OOOppppppppp:StructMeta.__new__", kwlist,
            &name, &PyTuple_Type, &bases, &PyDict_Type, &namespace,
            &arg_tag_field, &arg_tag, &arg_rename,
            &arg_omit_defaults, &arg_forbid_unknown_fields,
            &arg_frozen, &arg_eq, &arg_order, &arg_kw_only,
            &arg_array_like, &arg_gc, &arg_weakref
        )
    )
        return NULL;

    return StructMeta_new_inner(
        type, name, bases, namespace,
        arg_tag_field, arg_tag, arg_rename,
        arg_omit_defaults, arg_forbid_unknown_fields,
        arg_frozen, arg_eq, arg_order, arg_kw_only,
        arg_array_like, arg_gc, arg_weakref
    );
}


PyDoc_STRVAR(msgspec_defstruct__doc__,
"defstruct(name, fields, *, bases=(), module=None, namespace=None, "
"tag_field=None, tag=None, rename=None, omit_defaults=False, "
"forbid_unknown_fields=False, frozen=False, eq=True, order=False, "
"kw_only=False, array_like=False, gc=True, weakref=False)\n"
"--\n"
"\n"
"Dynamically define a new Struct class.\n"
"\n"
"Parameters\n"
"----------\n"
"name : str\n"
"    The name of the new Struct class.\n"
"fields : iterable\n"
"    An iterable of fields in the new class. Elements may be either ``name``,\n"
"    tuples of ``(name, type)``, or ``(name, type, default)``. Fields without\n"
"    a specified type will default to ``typing.Any``.\n"
"bases : tuple, optional\n"
"    A tuple of any Struct base classes to use when defining the new class.\n"
"module : str, optional\n"
"    The module name to use for the new class. If not provided, will be inferred\n"
"    from the caller's stack frame.\n"
"namespace : dict, optional\n"
"    If provided, will be used as the base namespace for the new class. This may\n"
"    be used to add additional methods to the class definition.\n"
"**kwargs :\n"
"    Additional Struct configuration options. See the ``Struct`` docs for more\n"
"    information.\n"
"\n"
"See Also\n"
"--------\n"
"Struct"
);
static PyObject *
msgspec_defstruct(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *name = NULL, *fields = NULL, *bases = NULL, *module = NULL, *namespace = NULL;
    PyObject *arg_tag_field = NULL, *arg_tag = NULL, *arg_rename = NULL;
    PyObject *new_bases = NULL, *annotations = NULL, *fields_fast = NULL, *out = NULL;
    int arg_omit_defaults = -1, arg_forbid_unknown_fields = -1;
    int arg_frozen = -1, arg_eq = -1, arg_order = -1;
    int arg_array_like = -1, arg_gc = -1, arg_weakref = -1;
    bool arg_kw_only = false;

    static char *kwlist[] = {
        "name", "fields", "bases", "module", "namespace",
        "tag_field", "tag", "rename",
        "omit_defaults", "forbid_unknown_fields",
        "frozen", "eq", "order", "kw_only",
        "array_like", "gc", "weakref",
        NULL
    };

    /* Parse arguments: (name, bases, dict) */
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "UO|$O!UO!OOOppppppppp:defstruct", kwlist,
            &name, &fields, &PyTuple_Type, &bases, &module, &PyDict_Type, &namespace,
            &arg_tag_field, &arg_tag, &arg_rename,
            &arg_omit_defaults, &arg_forbid_unknown_fields,
            &arg_frozen, &arg_eq, &arg_order, &arg_kw_only,
            &arg_array_like, &arg_gc, &arg_weakref)
    )
        return NULL;

    MsgspecState *mod = msgspec_get_global_state();

    namespace = (namespace == NULL) ? PyDict_New() : PyDict_Copy(namespace);
    if (namespace == NULL) return NULL;

    if (module != NULL) {
        if (PyDict_SetItemString(namespace, "__module__", module) < 0) goto cleanup;
    }

    if (bases == NULL) {
        new_bases = PyTuple_New(1);
        if (new_bases == NULL) goto cleanup;
        Py_INCREF(mod->StructType);
        PyTuple_SET_ITEM(new_bases, 0, mod->StructType);
        bases = new_bases;
    }

    annotations = PyDict_New();
    if (annotations == NULL) goto cleanup;

    fields_fast = PySequence_Fast(fields, "`fields` must be an iterable");
    if (fields_fast == NULL) goto cleanup;
    Py_ssize_t nfields = PySequence_Fast_GET_SIZE(fields_fast);

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *name = NULL, *type = NULL, *default_val = NULL;
        PyObject *field = PySequence_Fast_GET_ITEM(fields_fast, i);
        if (PyUnicode_Check(field)) {
            name = field;
            type = mod->typing_any;
        }
        else if (PyTuple_Check(field)) {
            Py_ssize_t len = PyTuple_GET_SIZE(field);
            if (len == 2) {
                name = PyTuple_GET_ITEM(field, 0);
                type = PyTuple_GET_ITEM(field, 1);
            }
            else if (len == 3) {
                name = PyTuple_GET_ITEM(field, 0);
                type = PyTuple_GET_ITEM(field, 1);
                default_val = PyTuple_GET_ITEM(field, 2);
            }
        }

        if (name == NULL || !PyUnicode_Check(name)) {
            PyErr_SetString(
                PyExc_TypeError,
                "items in `fields` must be one of `str`, `tuple[str, type]`, or `tuple[str, type, Any]`"
            );
            goto cleanup;
        }
        if (PyDict_SetItem(annotations, name, type) < 0) goto cleanup;
        if (default_val != NULL) {
            if (PyDict_SetItem(namespace, name, default_val) < 0) goto cleanup;
        }
    }
    if (PyDict_SetItemString(namespace, "__annotations__", annotations) < 0) goto cleanup;

    out = StructMeta_new_inner(
        &StructMetaType, name, bases, namespace,
        arg_tag_field, arg_tag, arg_rename,
        arg_omit_defaults, arg_forbid_unknown_fields,
        arg_frozen, arg_eq, arg_order, arg_kw_only,
        arg_array_like, arg_gc, arg_weakref
    );

cleanup:
    Py_XDECREF(namespace);
    Py_XDECREF(new_bases);
    Py_XDECREF(annotations);
    Py_XDECREF(fields_fast);
    return out;
}


static int
StructMeta_prep_types(PyObject *py_self, bool err_not_json, bool *json_compatible) {
    StructMetaObject *self = (StructMetaObject *)py_self;
    MsgspecState *st;
    TypeNode *type;
    TypeNode **struct_types = NULL;
    Py_ssize_t i, nfields;
    PyObject *obj, *field, *annotations = NULL;
    bool struct_is_json_compatible = true;

    /* Types are currently being prepped, recursive type */
    if (self->traversing) return 0;

    if (self->struct_types) {
        if (!self->json_compatible) {
            if (json_compatible != NULL) {
                *json_compatible = false;
            }
            if (!err_not_json) return 0;
            /* If we want to error, we need to recurse again here. This won't
             * modify any internal state, since it will error too early. */
        }
        else {
            return 0;
        }
    }

    if (MS_UNLIKELY(self->struct_fields == NULL)) {
        /* The struct isn't fully initialized! This most commonly happens if
         * the user tries to do anything inside a `__init_subclass__` method.
         * Error nicely rather than segfaulting. */
        PyErr_Format(
            PyExc_ValueError,
            "Type `%R` isn't fully defined, and can't be used in any "
            "`Decoder`/`decode` operations. This commonly happens when "
            "trying to use the struct type within an `__init_subclass__` "
            "method. If you believe what you're trying to do should work, "
            "please raise an issue on GitHub.",
            py_self
        );
        return -1;
    }

    /* Prevent recursion, clear on return */
    self->traversing = true;

    nfields = PyTuple_GET_SIZE(self->struct_fields);

    st = msgspec_get_global_state();
    annotations = CALL_ONE_ARG(st->get_type_hints, py_self);
    if (annotations == NULL) goto error;

    struct_types = PyMem_Calloc(nfields, sizeof(TypeNode*));
    if (struct_types == NULL)  {
        PyErr_NoMemory();
        goto error;
    }

    for (i = 0; i < nfields; i++) {
        bool field_is_json_compatible = true;
        field = PyTuple_GET_ITEM(self->struct_fields, i);
        obj = PyDict_GetItem(annotations, field);
        if (obj == NULL) goto error;
        type = TypeNode_Convert(obj, err_not_json, &field_is_json_compatible);
        if (type == NULL) goto error;
        struct_types[i] = type;
        struct_is_json_compatible &= field_is_json_compatible;
    }

    self->traversing = false;
    self->struct_types = struct_types;
    self->json_compatible = struct_is_json_compatible;
    if (!struct_is_json_compatible && json_compatible != NULL)
        *json_compatible = false;

    Py_DECREF(annotations);
    return 0;

error:
    self->traversing = false;
    Py_XDECREF(annotations);
    if (struct_types != NULL) {
        for (i = 0; i < nfields; i++) {
            TypeNode_Free(struct_types[i]);
        }
    }
    PyMem_Free(struct_types);
    return -1;
}

static int
StructMeta_traverse(StructMetaObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->struct_fields);
    Py_VISIT(self->struct_defaults);
    Py_VISIT(self->struct_encode_fields);
    Py_VISIT(self->struct_tag);  /* May be a function */
    Py_VISIT(self->rename);  /* May be a function */
    if (self->struct_types != NULL) {
        assert(self->struct_fields != NULL);
        Py_ssize_t nfields = PyTuple_GET_SIZE(self->struct_fields);
        for (Py_ssize_t i = 0; i < nfields; i++) {
            int out = TypeNode_traverse(self->struct_types[i], visit, arg);
            if (out != 0) return out;
        }
    }
    return PyType_Type.tp_traverse((PyObject *)self, visit, arg);
}

static int
StructMeta_clear(StructMetaObject *self)
{
    Py_ssize_t i, nfields;
    /* skip if clear already invoked */
    if (self->struct_fields == NULL) return 0;

    nfields = PyTuple_GET_SIZE(self->struct_fields);
    Py_CLEAR(self->struct_fields);
    Py_CLEAR(self->struct_defaults);
    Py_CLEAR(self->struct_encode_fields);
    Py_CLEAR(self->struct_tag_field);
    Py_CLEAR(self->struct_tag_value);
    Py_CLEAR(self->struct_tag);
    Py_CLEAR(self->rename);
    if (self->struct_offsets != NULL) {
        PyMem_Free(self->struct_offsets);
        self->struct_offsets = NULL;
    }
    if (self->struct_types != NULL) {
        for (i = 0; i < nfields; i++) {
            TypeNode_Free(self->struct_types[i]);
            self->struct_types[i] = NULL;
        }
        PyMem_Free(self->struct_types);
        self->struct_types = NULL;
    }
    return PyType_Type.tp_clear((PyObject *)self);
}

static void
StructMeta_dealloc(StructMetaObject *self)
{
    /* The GC invariants require dealloc immediately untrack to avoid double
     * deallocation. However, PyType_Type.tp_dealloc assumes the type is
     * currently tracked. Hence the unfortunate untrack/retrack below. */
    PyObject_GC_UnTrack(self);
    StructMeta_clear(self);
    PyObject_GC_Track(self);
    PyType_Type.tp_dealloc((PyObject *)self);
}

static PyObject*
StructMeta_frozen(StructMetaObject *self, void *closure)
{
    if (self->frozen == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_eq(StructMetaObject *self, void *closure)
{
    if (self->eq == OPT_FALSE) { Py_RETURN_FALSE; }
    else { Py_RETURN_TRUE; }
}

static PyObject*
StructMeta_order(StructMetaObject *self, void *closure)
{
    if (self->order == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_array_like(StructMetaObject *self, void *closure)
{
    if (self->array_like == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_gc(StructMetaObject *self, void *closure)
{
    if (self->gc == OPT_FALSE) { Py_RETURN_FALSE; }
    else { Py_RETURN_TRUE; }
}

static PyObject*
StructMeta_omit_defaults(StructMetaObject *self, void *closure)
{
    if (self->omit_defaults == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_forbid_unknown_fields(StructMetaObject *self, void *closure)
{
    if (self->forbid_unknown_fields == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_signature(StructMetaObject *self, void *closure)
{
    Py_ssize_t nfields, ndefaults, npos, nkwonly, i;
    MsgspecState *st;
    PyObject *res = NULL;
    PyObject *inspect = NULL;
    PyObject *parameter_cls = NULL;
    PyObject *parameter_empty = NULL;
    PyObject *kind_positional = NULL;
    PyObject *kind_kw_only = NULL;
    PyObject *signature_cls = NULL;
    PyObject *annotations = NULL;
    PyObject *parameters = NULL;
    PyObject *temp_args = NULL, *temp_kwargs = NULL;
    PyObject *field, *kind, *default_val, *parameter, *annotation;

    st = msgspec_get_global_state();

    nfields = PyTuple_GET_SIZE(self->struct_fields);
    ndefaults = PyTuple_GET_SIZE(self->struct_defaults);
    npos = nfields - ndefaults;
    nkwonly = self->nkwonly;

    inspect = PyImport_ImportModule("inspect");
    if (inspect == NULL) goto cleanup;
    parameter_cls = PyObject_GetAttrString(inspect, "Parameter");
    if (parameter_cls == NULL) goto cleanup;
    parameter_empty = PyObject_GetAttrString(parameter_cls, "empty");
    if (parameter_empty == NULL) goto cleanup;
    kind_positional = PyObject_GetAttrString(parameter_cls, "POSITIONAL_OR_KEYWORD");
    if (kind_positional == NULL) goto cleanup;
    kind_kw_only = PyObject_GetAttrString(parameter_cls, "KEYWORD_ONLY");
    if (kind_kw_only == NULL) goto cleanup;
    signature_cls = PyObject_GetAttrString(inspect, "Signature");
    if (signature_cls == NULL) goto cleanup;

    annotations = CALL_ONE_ARG(st->get_type_hints, (PyObject *)self);
    if (annotations == NULL) goto cleanup;

    parameters = PyList_New(nfields);
    if (parameters == NULL) return NULL;

    temp_args = PyTuple_New(0);
    if (temp_args == NULL) goto cleanup;
    temp_kwargs = PyDict_New();
    if (temp_kwargs == NULL) goto cleanup;

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(self->struct_fields, i);
        if (i < npos) {
            default_val = parameter_empty;
        } else {
            default_val = PyTuple_GET_ITEM(self->struct_defaults, i - npos);
            if (default_val == NODEFAULT) {
                default_val = parameter_empty;
            }
        }
        if (i < (nfields - nkwonly)) {
            kind = kind_positional;
        } else {
            kind = kind_kw_only;
        }
        annotation = PyDict_GetItem(annotations, field);
        if (annotation == NULL) {
            annotation = parameter_empty;
        }
        if (PyDict_SetItemString(temp_kwargs, "name", field) < 0) goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "kind", kind) < 0) goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "default", default_val) < 0) goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "annotation", annotation) < 0) goto cleanup;
        parameter = PyObject_Call(parameter_cls, temp_args, temp_kwargs);
        if (parameter == NULL) goto cleanup;
        PyList_SET_ITEM(parameters, i, parameter);
    }
    res = CALL_ONE_ARG(signature_cls, parameters);
cleanup:
    Py_XDECREF(inspect);
    Py_XDECREF(parameter_cls);
    Py_XDECREF(parameter_empty);
    Py_XDECREF(kind_positional);
    Py_XDECREF(kind_kw_only);
    Py_XDECREF(signature_cls);
    Py_XDECREF(annotations);
    Py_XDECREF(parameters);
    Py_XDECREF(temp_args);
    Py_XDECREF(temp_kwargs);
    return res;
}

static PyMemberDef StructMeta_members[] = {
    {"__struct_fields__", T_OBJECT_EX, offsetof(StructMetaObject, struct_fields), READONLY, "Struct fields"},
    {"__struct_defaults__", T_OBJECT_EX, offsetof(StructMetaObject, struct_defaults), READONLY, "Struct defaults"},
    {"__struct_encode_fields__", T_OBJECT_EX, offsetof(StructMetaObject, struct_encode_fields), READONLY, "Struct encoded field names"},
    {"__struct_tag_field__", T_OBJECT, offsetof(StructMetaObject, struct_tag_field), READONLY, "Struct tag field"},
    {"__struct_tag__", T_OBJECT, offsetof(StructMetaObject, struct_tag_value), READONLY, "Struct tag value"},
    {"__match_args__", T_OBJECT_EX, offsetof(StructMetaObject, match_args), READONLY, "Positional match args"},
    {NULL},
};

static PyGetSetDef StructMeta_getset[] = {
    {"__signature__", (getter) StructMeta_signature, NULL, NULL, NULL},
    {"frozen", (getter) StructMeta_frozen, NULL, NULL, NULL},
    {"eq", (getter) StructMeta_eq, NULL, NULL, NULL},
    {"order", (getter) StructMeta_order, NULL, NULL, NULL},
    {"array_like", (getter) StructMeta_array_like, NULL, NULL, NULL},
    {"gc", (getter) StructMeta_gc, NULL, NULL, NULL},
    {"omit_defaults", (getter) StructMeta_omit_defaults, NULL, NULL, NULL},
    {"forbid_unknown_fields", (getter) StructMeta_forbid_unknown_fields, NULL, NULL, NULL},
    {NULL},
};

static PyTypeObject StructMetaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.StructMeta",
    .tp_basicsize = sizeof(StructMetaObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_TYPE_SUBCLASS | Py_TPFLAGS_HAVE_GC | _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_new = StructMeta_new,
    .tp_dealloc = (destructor) StructMeta_dealloc,
    .tp_clear = (inquiry) StructMeta_clear,
    .tp_traverse = (traverseproc) StructMeta_traverse,
    .tp_members = StructMeta_members,
    .tp_getset = StructMeta_getset,
    .tp_call = PyVectorcall_Call,
    .tp_vectorcall_offset = offsetof(PyTypeObject, tp_vectorcall),
};


static PyObject *
get_default(PyObject *obj) {
    PyTypeObject *type = Py_TYPE(obj);
    if (type == &Factory_Type) {
        return Factory_Call(obj);
    }
    Py_INCREF(obj);
    return obj;
}

static MS_INLINE bool
is_default(PyObject *x, PyObject *d) {
    if (x == d) return true;
    if (Py_TYPE(d) == &Factory_Type) {
        PyTypeObject *factory = (PyTypeObject *)(((Factory *)d)->factory);
        if (Py_TYPE(x) != factory) return false;
        if (factory == &PyList_Type && PyList_GET_SIZE(x) == 0) return true;
        if (factory == &PyDict_Type && PyDict_GET_SIZE(x) == 0) return true;
        if (factory == &PySet_Type && PySet_GET_SIZE(x) == 0) return true;
    }
    return false;
}

/* Set field #index on obj. Steals a reference to val */
static inline void
Struct_set_index(PyObject *obj, Py_ssize_t index, PyObject *val) {
    StructMetaObject *cls;
    char *addr;
    PyObject *old;

    cls = (StructMetaObject *)Py_TYPE(obj);
    addr = (char *)obj + cls->struct_offsets[index];
    old = *(PyObject **)addr;
    Py_XDECREF(old);
    *(PyObject **)addr = val;
}

/* Get field #index or NULL on obj. Returns a borrowed reference */
static inline PyObject*
Struct_get_index_noerror(PyObject *obj, Py_ssize_t index) {
    StructMetaObject *cls = (StructMetaObject *)Py_TYPE(obj);
    char *addr = (char *)obj + cls->struct_offsets[index];
    return *(PyObject **)addr;
}

/* Get field #index on obj. Returns a borrowed reference */
static inline PyObject*
Struct_get_index(PyObject *obj, Py_ssize_t index) {
    PyObject *val = Struct_get_index_noerror(obj, index);
    if (val == NULL) {
        StructMetaObject *cls = (StructMetaObject *)Py_TYPE(obj);
        PyErr_Format(PyExc_AttributeError,
                     "Struct field %R is unset",
                     PyTuple_GET_ITEM(cls->struct_fields, index));
    }
    return val;
}

/* ASSUMPTION - obj is untracked and allocated via Struct_alloc */
static int
Struct_fill_in_defaults(StructMetaObject *st_type, PyObject *obj, PathNode *path) {
    Py_ssize_t nfields, ndefaults, i;
    bool is_gc, should_untrack;

    nfields = PyTuple_GET_SIZE(st_type->struct_encode_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    is_gc = MS_TYPE_IS_GC(st_type);
    should_untrack = is_gc;

    for (i = 0; i < nfields; i++) {
        PyObject *val = Struct_get_index_noerror(obj, i);
        if (val == NULL) {
            if (MS_UNLIKELY(i < (nfields - ndefaults))) goto missing_required;
            val = PyTuple_GET_ITEM(
                st_type->struct_defaults, i - (nfields - ndefaults)
            );
            if (MS_UNLIKELY(val == NODEFAULT)) goto missing_required;
            val = get_default(val);
            if (MS_UNLIKELY(val == NULL)) return -1;
            Struct_set_index(obj, i, val);
        }
        if (should_untrack) {
            should_untrack = !MS_OBJ_IS_GC(val);
        }
    }

    if (is_gc && !should_untrack)
        PyObject_GC_Track(obj);
    return 0;

missing_required:
    ms_raise_validation_error(
        path,
        "Object missing required field `%U`%U",
        PyTuple_GET_ITEM(st_type->struct_encode_fields, i)
    );
    return -1;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);

    StructMetaObject *st_type = (StructMetaObject *)cls;
    PyObject *fields = st_type->struct_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    PyObject *defaults = st_type->struct_defaults;
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(defaults);
    Py_ssize_t nkwonly = st_type->nkwonly;
    Py_ssize_t npos = nfields - ndefaults;

    if (MS_UNLIKELY(nargs > (nfields - nkwonly))) {
        PyErr_SetString(PyExc_TypeError, "Extra positional arguments provided");
        return NULL;
    }

    bool is_gc = MS_TYPE_IS_GC(cls);
    bool should_untrack = is_gc;

    PyObject *self = Struct_alloc(cls);
    if (self == NULL) return NULL;

    /* First, process all positional arguments */
    for (Py_ssize_t i = 0; i < nargs; i++) {
        PyObject *val = args[i];
        char *addr = (char *)self + st_type->struct_offsets[i];
        Py_INCREF(val);
        *(PyObject **)addr = val;
        if (should_untrack) {
            should_untrack = !MS_OBJ_IS_GC(val);
        }
    }

    /* Next, process all kwargs */
    for (Py_ssize_t i = 0; i < nkwargs; i++) {
        char *addr;
        PyObject *val;
        Py_ssize_t field_index;
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* Since keyword names are interned, first loop with pointer
         * comparisons only. */
        for (field_index = nargs; field_index < nfields; field_index++) {
            PyObject *field = PyTuple_GET_ITEM(fields, field_index);
            if (MS_LIKELY(kwname == field)) goto kw_found;
        }

        /* Fast path failed. It's more likely that this is an invalid kwarg
         * than that the kwname wasn't interned. Loop from 0 this time to also
         * check for parameters passed both as arg and kwarg */
        for (field_index = 0; field_index < nfields; field_index++) {
            PyObject *field = PyTuple_GET_ITEM(fields, field_index);
            if (_PyUnicode_EQ(kwname, field)) {
                if (MS_UNLIKELY(field_index < nargs)) {
                    PyErr_Format(
                        PyExc_TypeError,
                        "Argument '%U' given by name and position",
                        kwname
                    );
                    goto error;
                }
                goto kw_found;
            }
        }

        /* Unknown keyword */
        PyErr_Format(PyExc_TypeError, "Unexpected keyword argument '%U'", kwname);
        goto error;

kw_found:
        val = args[i + nargs];
        addr = (char *)self + st_type->struct_offsets[field_index];
        Py_INCREF(val);
        *(PyObject **)addr = val;
        if (should_untrack) {
            should_untrack = !MS_OBJ_IS_GC(val);
        }
    }

    /* Finally, fill in missing defaults */
    if (nargs + nkwargs < nfields) {
        for (Py_ssize_t field_index = nargs; field_index < nfields; field_index++) {
            char *addr = (char *)self + st_type->struct_offsets[field_index];
            if (MS_LIKELY(*(PyObject **)addr == NULL)) {
                if (MS_LIKELY(field_index >= npos)) {
                    PyObject *val = PyTuple_GET_ITEM(defaults, field_index - npos);
                    if (MS_LIKELY(val != NODEFAULT)) {
                        val = get_default(val);
                        if (MS_UNLIKELY(val == NULL)) goto error;
                        *(PyObject **)addr = val;
                        if (should_untrack) {
                            should_untrack = !MS_OBJ_IS_GC(val);
                        }
                        continue;
                    }
                }
                PyErr_Format(
                    PyExc_TypeError,
                    "Missing required argument '%U'",
                    PyTuple_GET_ITEM(fields, field_index)
                );
                goto error;
            }
        }
    }

    if (is_gc && !should_untrack)
        PyObject_GC_Track(self);
    return self;

error:
    Py_DECREF(self);
    return NULL;
}

static PyObject *
Struct_repr(PyObject *self) {
    int recursive;
    Py_ssize_t nfields, i;
    PyObject *parts = NULL, *empty = NULL, *out = NULL;
    PyObject *part, *fields, *field, *val;

    recursive = Py_ReprEnter(self);
    if (recursive != 0) {
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");  /* cpylint-ignore */
    }

    fields = StructMeta_GET_FIELDS(Py_TYPE(self));
    nfields = PyTuple_GET_SIZE(fields);
    if (nfields == 0) {
        out = PyUnicode_FromFormat("%s()", Py_TYPE(self)->tp_name);
        goto cleanup;
    }

    parts = PyList_New(nfields + 1);
    if (parts == NULL)
        goto cleanup;

    part = PyUnicode_FromFormat("%s(", Py_TYPE(self)->tp_name);
    if (part == NULL)
        goto cleanup;
    PyList_SET_ITEM(parts, 0, part);

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(fields, i);
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto cleanup;

        if (i == (nfields - 1)) {
            part = PyUnicode_FromFormat("%U=%R)", field, val);
        } else {
            part = PyUnicode_FromFormat("%U=%R, ", field, val);
        }
        if (part == NULL)
            goto cleanup;
        PyList_SET_ITEM(parts, i + 1, part);
    }
    empty = PyUnicode_FromString("");
    if (empty == NULL)
        goto cleanup;
    out = PyUnicode_Join(empty, parts);

cleanup:
    Py_XDECREF(parts);
    Py_XDECREF(empty);
    Py_ReprLeave(self);
    return out;
}

static Py_hash_t
Struct_hash(PyObject *self) {
    PyObject *val;
    Py_ssize_t i, nfields;
    Py_uhash_t acc = MS_HASH_XXPRIME_5;

    StructMetaObject *st_type = (StructMetaObject *)Py_TYPE(self);

    if (MS_UNLIKELY(st_type->eq == OPT_FALSE)) {
        /* If `__eq__` isn't implemented, then the default pointer-based
         * `__hash__` should be used */
        return PyBaseObject_Type.tp_hash(self);
    }

    if (MS_UNLIKELY(st_type->frozen != OPT_TRUE)) {
        /* If `__eq__` is implemented, only frozen types can be hashed */
        return PyObject_HashNotImplemented(self);
    }

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));

    for (i = 0; i < nfields; i++) {
        Py_uhash_t lane;
        val = Struct_get_index(self, i);
        if (val == NULL) return -1;
        lane = PyObject_Hash(val);
        if (lane == (Py_uhash_t)-1) return -1;
        acc += lane * MS_HASH_XXPRIME_2;
        acc = MS_HASH_XXROTATE(acc);
        acc *= MS_HASH_XXPRIME_1;
    }
    acc += nfields ^ (MS_HASH_XXPRIME_5 ^ 3527539UL);
    return (acc == (Py_uhash_t)-1) ?  1546275796 : acc;
}

static PyObject *
Struct_richcompare(PyObject *self, PyObject *other, int op) {
    if (Py_TYPE(self) != Py_TYPE(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    StructMetaObject *st_type = (StructMetaObject *)(Py_TYPE(self));

    if (op == Py_EQ || op == Py_NE) {
        if (MS_UNLIKELY(st_type->eq == OPT_FALSE)) {
            Py_RETURN_NOTIMPLEMENTED;
        }
    }
    else if (st_type->order != OPT_TRUE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    int equal = 1;
    PyObject *left = NULL, *right = NULL;

    /* Only need to loop if self is not other` */
    if (MS_LIKELY(self != other)) {
        Py_ssize_t nfields = StructMeta_GET_NFIELDS(st_type);
        for (Py_ssize_t i = 0; i < nfields; i++) {
            left = Struct_get_index(self, i);
            if (left == NULL) return NULL;

            right = Struct_get_index(other, i);
            if (right == NULL) return NULL;

            equal = PyObject_RichCompareBool(left, right, Py_EQ);

            if (equal < 0) return NULL;
            if (equal == 0) break;
        }
    }

    if (equal) {
        if (op == Py_EQ || op == Py_GE || op == Py_LE) {
            Py_RETURN_TRUE;
        }
        else if (op == Py_NE) {
            Py_RETURN_FALSE;
        }
        else if (left == NULL) {
            /* < or > on two 0-field or identical structs */
            Py_RETURN_FALSE;
        }
    }
    else if (op == Py_EQ) {
        Py_RETURN_FALSE;
    }
    else if (op == Py_NE) {
        Py_RETURN_TRUE;
    }
    /* Need to compare final element again to determine proper result */
    return PyObject_RichCompare(left, right, op);
}

static PyObject *
Struct_copy(PyObject *self, PyObject *args)
{
    Py_ssize_t i, nfields;
    PyObject *val, *res = NULL;

    res = Struct_alloc(Py_TYPE(self));
    if (res == NULL)
        return NULL;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto error;
        Py_INCREF(val);
        Struct_set_index(res, i, val);
    }
    /* If self is tracked, then copy is tracked */
    if (MS_OBJECT_IS_GC(self) && MS_IS_TRACKED(self))
        PyObject_GC_Track(res);
    return res;
error:
    Py_DECREF(res);
    return NULL;
}

PyDoc_STRVAR(struct_replace__doc__,
"replace(struct, / **changes)\n"
"--\n"
"\n"
"Create a new struct instance of the same type as ``struct``, replacing fields\n"
"with values from ``**changes``.\n"
"\n"
"Parameters\n"
"----------\n"
"struct: Struct\n"
"    The original struct instance.\n"
"**changes:\n"
"    Fields and values that should be replaced in the new struct instance.\n"
"\n"
"Returns\n"
"-------\n"
"new_struct: Struct\n"
"   A new struct instance of the same type as ``struct``.\n"
"\n"
"Examples\n"
"--------\n"
">>> class Point(msgspec.Struct):\n"
"...     x: int\n"
"...     y: int\n"
">>> obj = Point(x=1, y=2)\n"
">>> msgspec.structs.replace(obj, x=3)\n"
"Point(x=3, y=2)\n"
"\n"
"See Also\n"
"--------\n"
"dataclasses.replace"
);
static PyObject*
struct_replace(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);

    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    PyObject *obj = args[0];
    if (Py_TYPE(Py_TYPE(obj)) != &StructMetaType) {
        PyErr_SetString(PyExc_TypeError, "`struct` must be a `msgspec.Struct`");
        return NULL;
    }

    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(obj);
    PyObject *fields = struct_type->struct_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    bool is_gc = MS_TYPE_IS_GC(struct_type);
    bool should_untrack = is_gc;

    PyObject *out = Struct_alloc((PyTypeObject *)struct_type);
    if (out == NULL) return NULL;

    for (Py_ssize_t i = 0; i < nkwargs; i++) {
        PyObject *val;
        Py_ssize_t field_index;
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* Since keyword names are interned, first loop with pointer
         * comparisons only. */
        for (field_index = 0; field_index < nfields; field_index++) {
            PyObject *field = PyTuple_GET_ITEM(fields, field_index);
            if (MS_LIKELY(kwname == field)) goto kw_found;
        }
        for (field_index = 0; field_index < nfields; field_index++) {
            PyObject *field = PyTuple_GET_ITEM(fields, field_index);
            if (_PyUnicode_EQ(kwname, field)) goto kw_found;
        }

        /* Unknown keyword */
        PyErr_Format(
            PyExc_TypeError, "`%.200s` has no field '%U'",
            ((PyTypeObject *)struct_type)->tp_name, kwname
        );
        goto error;

    kw_found:
        val = args[i + 1];
        Py_INCREF(val);
        Struct_set_index(out, field_index, val);
        if (should_untrack) {
            should_untrack = !MS_OBJ_IS_GC(val);
        }
    }

    for (Py_ssize_t i = 0; i < nfields; i++) {
        if (Struct_get_index_noerror(out, i) == NULL) {
            PyObject *val = Struct_get_index(obj, i);
            if (val == NULL) goto error;
            if (should_untrack) {
                should_untrack = !MS_OBJ_IS_GC(val);
            }
            Py_INCREF(val);
            Struct_set_index(out, i, val);
        }
    }

    if (is_gc && !should_untrack) {
        PyObject_GC_Track(out);
    }
    return out;

error:
    Py_DECREF(out);
    return NULL;
}

PyDoc_STRVAR(struct_asdict__doc__,
"asdict(struct)\n"
"--\n"
"\n"
"Convert a struct to a dict.\n"
"\n"
"Parameters\n"
"----------\n"
"struct: Struct\n"
"    The struct instance.\n"
"\n"
"Returns\n"
"-------\n"
"dict\n"
"\n"
"Examples\n"
"--------\n"
">>> class Point(msgspec.Struct):\n"
"...     x: int\n"
"...     y: int\n"
">>> obj = Point(x=1, y=2)\n"
">>> msgspec.structs.asdict(obj)\n"
"{'x': 1, 'y': 2}\n"
"\n"
"See Also\n"
"--------\n"
"msgspec.structs.astuple\n"
"msgspec.to_builtins"
);
static PyObject*
struct_asdict(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    PyObject *obj = args[0];
    if (Py_TYPE(Py_TYPE(obj)) != &StructMetaType) {
        PyErr_SetString(PyExc_TypeError, "`struct` must be a `msgspec.Struct`");
        return NULL;
    }

    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(obj);
    PyObject *fields = struct_type->struct_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);

    PyObject *out = PyDict_New();
    if (out == NULL) return NULL;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *key = PyTuple_GET_ITEM(fields, i);
        PyObject *val = Struct_get_index(obj, i);
        if (val == NULL) goto error;
        if (PyDict_SetItem(out, key, val) < 0) goto error;
    }
    return out;
error:
    Py_DECREF(out);
    return NULL;
}

PyDoc_STRVAR(struct_astuple__doc__,
"astuple(struct)\n"
"--\n"
"\n"
"Convert a struct to a tuple.\n"
"\n"
"Parameters\n"
"----------\n"
"struct: Struct\n"
"    The struct instance.\n"
"\n"
"Returns\n"
"-------\n"
"tuple\n"
"\n"
"Examples\n"
"--------\n"
">>> class Point(msgspec.Struct):\n"
"...     x: int\n"
"...     y: int\n"
">>> obj = Point(x=1, y=2)\n"
">>> msgspec.structs.astuple(obj)\n"
"(1, 2)\n"
"\n"
"See Also\n"
"--------\n"
"msgspec.structs.asdict\n"
"msgspec.to_builtins"
);
static PyObject*
struct_astuple(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    PyObject *obj = args[0];
    if (Py_TYPE(Py_TYPE(obj)) != &StructMetaType) {
        PyErr_SetString(PyExc_TypeError, "`struct` must be a `msgspec.Struct`");
        return NULL;
    }

    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(obj);
    Py_ssize_t nfields = PyTuple_GET_SIZE(struct_type->struct_fields);

    PyObject *out = PyTuple_New(nfields);
    if (out == NULL) return NULL;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *val = Struct_get_index(obj, i);
        if (val == NULL) goto error;
        Py_INCREF(val);
        PyTuple_SET_ITEM(out, i, val);
    }
    return out;
error:
    Py_DECREF(out);
    return NULL;
}

static PyObject *
Struct_reduce(PyObject *self, PyObject *args)
{
    PyObject *values = NULL, *out = NULL;
    StructMetaObject *st_type = (StructMetaObject *)(Py_TYPE(self));
    Py_ssize_t nfields = PyTuple_GET_SIZE(st_type->struct_fields);

    if (st_type->nkwonly) {
        MsgspecState *mod = msgspec_get_global_state();
        values = PyDict_New();
        if (values == NULL) return NULL;
        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *field = PyTuple_GET_ITEM(st_type->struct_fields, i);
            PyObject *val = Struct_get_index(self, i);
            if (val == NULL) goto cleanup;
            if (PyDict_SetItem(values, field, val) < 0) goto cleanup;
        }
        out = Py_BuildValue("O(OO)", mod->rebuild, Py_TYPE(self), values);
    }
    else {
        values = PyTuple_New(nfields);
        if (values == NULL) return NULL;
        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *val = Struct_get_index(self, i);
            if (val == NULL) goto cleanup;
            Py_INCREF(val);
            PyTuple_SET_ITEM(values, i, val);
        }
        out = PyTuple_Pack(2, Py_TYPE(self), values);
    }
cleanup:
    Py_DECREF(values);
    return out;
}

static PyObject *
Struct_rich_repr(PyObject *self, PyObject *args) {
    PyObject *fields = StructMeta_GET_FIELDS(Py_TYPE(self));
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);

    PyObject *out = PyTuple_New(nfields);
    if (out == NULL) goto error;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        PyObject *val = Struct_get_index(self, i);
        if (val == NULL) goto error;
        PyObject *part = PyTuple_Pack(2, field, val);
        if (part == NULL) goto error;
        PyTuple_SET_ITEM(out, i, part);
    }
    return out;
error:
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
StructMixin_fields(PyObject *self, void *closure) {
    PyObject *out = ((StructMetaObject *)Py_TYPE(self))->struct_fields;
    Py_INCREF(out);
    return out;
}

static PyObject *
StructMixin_encode_fields(PyObject *self, void *closure) {
    PyObject *out = ((StructMetaObject *)Py_TYPE(self))->struct_encode_fields;
    Py_INCREF(out);
    return out;
}

static PyObject *
StructMixin_defaults(PyObject *self, void *closure) {
    PyObject *out = ((StructMetaObject *)Py_TYPE(self))->struct_defaults;
    Py_INCREF(out);
    return out;
}

static PyMethodDef Struct_methods[] = {
    {"__copy__", Struct_copy, METH_NOARGS, "copy a struct"},
    {"__reduce__", Struct_reduce, METH_NOARGS, "reduce a struct"},
    {"__rich_repr__", Struct_rich_repr, METH_NOARGS, "rich repr"},
    {NULL, NULL},
};

static PyGetSetDef StructMixin_getset[] = {
    {"__struct_fields__", (getter) StructMixin_fields, NULL, "Struct fields", NULL},
    {"__struct_encode_fields__", (getter) StructMixin_encode_fields, NULL, "Struct encoded field names", NULL},
    {"__struct_defaults__", (getter) StructMixin_defaults, NULL, "Struct defaults", NULL},
    {NULL},
};

static PyTypeObject StructMixinType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core._StructMixin",
    .tp_basicsize = 0,
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_setattro = Struct_setattro_default,
    .tp_repr = Struct_repr,
    .tp_richcompare = Struct_richcompare,
    .tp_hash = Struct_hash,
    .tp_methods = Struct_methods,
    .tp_getset = StructMixin_getset,
};

PyDoc_STRVAR(Struct__doc__,
"A base class for defining efficient serializable objects.\n"
"\n"
"Fields are defined using type annotations. Fields may optionally have\n"
"default values, which result in keyword parameters to the constructor.\n"
"\n"
"Structs automatically define ``__init__``, ``__eq__``, ``__repr__``, and\n"
"``__copy__`` methods. Additional methods can be defined on the class as\n"
"needed. Note that ``__init__``/``__new__`` cannot be overridden, but other\n"
"methods can. A tuple of the field names is available on the class via the\n"
"``__struct_fields__`` attribute if needed.\n"
"\n"
"Additional class options can be enabled by passing keywords to the class\n"
"definition (see example below).\n"
"\n"
"Configuration\n"
"-------------\n"
"frozen: bool, default False\n"
"   Whether instances of this type are pseudo-immutable. If true, attribute\n"
"   assignment is disabled and a corresponding ``__hash__`` is defined.\n"
"order: bool, default False\n"
"   If True, ``__lt__``, `__le__``, ``__gt__``, and ``__ge__`` methods\n"
"   will be generated for this type.\n"
"eq: bool, default True\n"
"   If True (the default), an ``__eq__`` method will be generated for this\n"
"   type. Set to False to compare based on instance identity alone.\n"
"kw_only: bool, default False\n"
"   If True, all fields will be treated as keyword-only arguments in the\n"
"   generated ``__init__`` method. Default is False.\n"
"omit_defaults: bool, default False\n"
"   Whether fields should be omitted from encoding if the corresponding value\n"
"   is the default for that field. Enabling this may reduce message size, and\n"
"   often also improve encoding & decoding performance.\n"
"forbid_unknown_fields: bool, default False\n"
"   If True, an error is raised if an unknown field is encountered while\n"
"   decoding structs of this type. If False (the default), no error is raised\n"
"   and the unknown field is skipped.\n"
"tag: str, int, bool, callable, or None, default None\n"
"   Used along with ``tag_field`` for configuring tagged union support. If\n"
"   either are non-None, then the struct is considered \"tagged\". In this case,\n"
"   an extra field (the ``tag_field``) and value (the ``tag``) are added to the\n"
"   encoded message, which can be used to differentiate message types during\n"
"   decoding.\n"
"\n"
"   Set ``tag=True`` to enable the default tagged configuration (``tag_field``\n"
"   is ``\"type\"``, ``tag`` is the class name). Alternatively, you can provide\n"
"   a string (or less commonly int) value directly to be used as the tag\n"
"   (e.g. ``tag=\"my-tag-value\"``).``tag`` can also be passed a callable that\n"
"   takes the class name and returns a valid tag value (e.g. ``tag=str.lower``).\n"
"   See the docs for more information.\n"
"tag_field: str or None, default None\n"
"   The field name to use for tagged union support. If ``tag`` is non-None,\n"
"   then this defaults to ``\"type\"``. See the ``tag`` docs above for more\n"
"   information.\n"
"rename: str, mapping, callable, or None, default None\n"
"   Controls renaming the field names used when encoding/decoding the struct.\n"
"   May be one of ``\"lower\"``, ``\"upper\"``, ``\"camel\"``, ``\"pascal\"``, or\n"
"   ``\"kebab\"`` to rename in lowercase, UPPERCASE, camelCase, PascalCase,\n"
"   or kebab-case respectively. May also be a mapping from field names to the\n"
"   renamed names (missing fields are not renamed). Alternatively, may be a\n"
"   callable that takes the field name and returns a new name or ``None`` to\n"
"   not rename that field. Default is ``None`` for no field renaming.\n"
"array_like: bool, default False\n"
"   If True, this struct type will be treated as an array-like type during\n"
"   encoding/decoding, rather than a dict-like type (the default). This may\n"
"   improve performance, at the cost of a more inscrutable message encoding.\n"
"gc: bool, default True\n"
"   Whether garbage collection is enabled for this type. Disabling this *may*\n"
"   help reduce GC pressure, but will prevent reference cycles composed of only\n"
"   ``gc=False`` from being collected. It is the user's responsibility to ensure\n"
"   that reference cycles don't occur when setting ``gc=False``.\n"
"weakref: bool, default False\n"
"   Whether instances of this type support weak references. Defaults to False.\n"
"\n"
"Examples\n"
"--------\n"
"Here we define a new `Struct` type for describing a dog. It has three fields;\n"
"two required and one optional.\n"
"\n"
">>> class Dog(Struct):\n"
"...     name: str\n"
"...     breed: str\n"
"...     is_good_boy: bool = True\n"
"...\n"
">>> Dog('snickers', breed='corgi')\n"
"Dog(name='snickers', breed='corgi', is_good_boy=True)\n"
"\n"
"Additional struct options can be set as part of the class definition. Here\n"
"we define a new `Struct` type for a frozen `Point` object.\n"
"\n"
">>> class Point(Struct, frozen=True):\n"
"...     x: float\n"
"...     y: float\n"
"...\n"
">>> {Point(1.5, 2.0): 1}  # frozen structs are hashable\n"
"{Point(1.5, 2.0): 1}"
);

static PyObject *
TypedDictInfo_Convert(PyObject *obj, bool err_not_json, bool *json_compatible) {
    PyObject *annotations = NULL, *required = NULL;
    TypedDictInfo *info = NULL;
    MsgspecState *mod = msgspec_get_global_state();
    bool cache_set = false;

    /* Check if cached */
    PyObject *cached = PyObject_GetAttr(obj, mod->str___msgspec_cache__);
    if (cached != NULL) {
        if (Py_TYPE(cached) != &TypedDictInfo_Type) {
            Py_DECREF(cached);
            PyErr_Format(
                PyExc_RuntimeError,
                "%R.__msgspec_cache__ has been overwritten",
                obj
            );
            return NULL;
        }

        /* -1 indicates the TypedDictInfo is still being built (due to a
         * recursive type definition). Just return immediately. */
        if (((TypedDictInfo *)cached)->nrequired == -1) return cached;

        if (!((TypedDictInfo *)cached)->json_compatible) {
            if (json_compatible != NULL) {
                *json_compatible = false;
            }
            if (!err_not_json) return cached;
            /* XXX: If we want to error, we need to recurse again here. This won't
            * modify any internal state, since it will error too early. */
            Py_DECREF(cached);
        }
        else {
            return cached;
        }
    }

    /* Clear the getattr error, if any */
    PyErr_Clear();

    /* Not cached, extract fields from TypedDict object */
    annotations = CALL_ONE_ARG(mod->get_typeddict_hints, obj);
    if (annotations == NULL) return NULL;

    required = PyObject_GetAttr(obj, mod->str___required_keys__);
    /* Python 3.8 doesn't have __required_keys__. In that case we treat the
     * dict as fully optional or fully required. */
    if (required == NULL) {
        PyErr_Clear();

        bool is_total;
        PyObject *total = PyObject_GetAttr(obj, mod->str___total__);
        if (total != NULL) {
            is_total = PyObject_IsTrue(total);
            Py_DECREF(total);
        }
        else {
            is_total = true;
            PyErr_Clear();
        }
        required = PyFrozenSet_New(is_total ? annotations : NULL);
        if (required == NULL) goto error;
    }

    if (cached != NULL) {
        /* If cached is not NULL, this means that a TypedDictInfo already exists for
         * this TypedDict, but it's not JSON compatible and we're recursing through
         * again to raise a nice TypeError. In this case we:
         * - Temporarily mark the TypedDictInfo object as being recursively traversed
         *   to guard against recursion errors. This is done by setting nrequired = -1
         * - Call `TypeNode_Convert` on every field type, bubbling up the error if
         *   raised, otherwise immediately free the result.
         * - Reset `nrequired` to its original value before raising the error.
         */
        Py_ssize_t nrequired_temp = ((TypedDictInfo *)cached)->nrequired;
        ((TypedDictInfo *)cached)->nrequired = -1;
        Py_ssize_t pos = 0;
        PyObject *val;
        while (PyDict_Next(annotations, &pos, NULL, &val)) {
            bool item_is_json_compatible = true;
            TypeNode *type = TypeNode_Convert(val, err_not_json, &item_is_json_compatible);
            if (type == NULL) break;
            TypeNode_Free(type);
        }
        ((TypedDictInfo *)cached)->nrequired = nrequired_temp;
        goto error;
    }

    /* Allocate and zero-out a new TypedDictInfo object */
    Py_ssize_t nfields = PyDict_GET_SIZE(annotations);
    info = PyObject_GC_NewVar(TypedDictInfo, &TypedDictInfo_Type, nfields);
    if (info == NULL) goto error;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        info->fields[i].key = NULL;
        info->fields[i].type = NULL;
    }
    /* Initialize nrequired to -1 as a flag in case of a recursive TypedDict
    * definition. */
    info->nrequired = -1;

    /* If not already cached, then cache on TypedDict object _before_
    * traversing fields. This is to ensure self-referential TypedDicts work. */
    if (PyObject_SetAttr(obj, mod->str___msgspec_cache__, (PyObject *)info) < 0) {
        goto error;
    }
    cache_set = true;

    /* Traverse fields and initialize TypedDictInfo */
    Py_ssize_t pos = 0, i = 0;
    PyObject *key, *val;
    bool dict_is_json_compatible = true;
    while (PyDict_Next(annotations, &pos, &key, &val)) {
        bool item_is_json_compatible = true;
        TypeNode *type = TypeNode_Convert(val, err_not_json, &item_is_json_compatible);
        if (type == NULL) goto error;
        Py_INCREF(key);
        info->fields[i].key = key;
        info->fields[i].type = type;
        dict_is_json_compatible &= item_is_json_compatible;
        int contains = PySet_Contains(required, key);
        if (contains == -1) goto error;
        if (contains) { type->types |= MS_EXTRA_FLAG; }
        i++;
    }
    info->nrequired = PySet_GET_SIZE(required);
    info->json_compatible = dict_is_json_compatible;
    if (!dict_is_json_compatible && json_compatible != NULL) {
        *json_compatible = false;
    }
    Py_XDECREF(annotations);
    Py_XDECREF(required);
    PyObject_GC_Track(info);
    return (PyObject *)info;

error:
    if (cache_set) {
        /* An error occurred after the cache was created and set on the
         * TypedDict. We need to delete the attribute. Fetch and restore the
         * original exception to avoid DelAttr silently clearing it on rare
         * occasions. */
        PyObject *err_type, *err_value, *err_tb;
        PyErr_Fetch(&err_type, &err_value, &err_tb);
        PyObject_DelAttr(obj, mod->str___msgspec_cache__);
        PyErr_Restore(err_type, err_value, err_tb);
    }
    Py_XDECREF((PyObject *)info);
    Py_XDECREF(annotations);
    Py_XDECREF(required);
    return NULL;
}

static MS_INLINE PyObject *
TypedDictInfo_lookup_key(
    TypedDictInfo *self, const char * key, Py_ssize_t key_size,
    TypeNode **type, Py_ssize_t *pos
) {
    const char *field;
    Py_ssize_t nfields, field_size, i, offset = *pos;
    nfields = Py_SIZE(self);
    for (i = offset; i < nfields; i++) {
        field = unicode_str_and_size_nocheck(self->fields[i].key, &field_size);
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i < (nfields - 1) ? (i + 1) : 0;
            *type = self->fields[i].type;
            return self->fields[i].key;
        }
    }
    for (i = 0; i < offset; i++) {
        field = unicode_str_and_size_nocheck(self->fields[i].key, &field_size);
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i + 1;
            *type = self->fields[i].type;
            return self->fields[i].key;
        }
    }
    return NULL;
}

static void
TypedDictInfo_error_missing(TypedDictInfo *self, PyObject *dict, PathNode *path) {
    Py_ssize_t nfields = Py_SIZE(self);
    for (Py_ssize_t i = 0; i < nfields; i++) {
        if (self->fields[i].type->types & MS_EXTRA_FLAG) {
            PyObject *field = self->fields[i].key;
            int contains = PyDict_Contains(dict, field);
            if (contains < 0) return;
            if (contains == 0) {
                ms_raise_validation_error(
                    path,
                    "Object missing required field `%U`%U",
                    field
                );
                return;
            }
        }
    }
}

static int
TypedDictInfo_traverse(TypedDictInfo *self, visitproc visit, void *arg)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        TypedDictField *field = &(self->fields[i]);
        if (field->key != NULL) {
            int out = TypeNode_traverse(field->type, visit, arg);
            if (out != 0) return out;
        }
    }
    return 0;
}

static int
TypedDictInfo_clear(TypedDictInfo *self)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        Py_CLEAR(self->fields[i].key);
        TypeNode_Free(self->fields[i].type);
        self->fields[i].type = NULL;
    }
    return 0;
}

static void
TypedDictInfo_dealloc(TypedDictInfo *self)
{
    PyObject_GC_UnTrack(self);
    TypedDictInfo_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject TypedDictInfo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.TypedDictInfo",
    .tp_basicsize = sizeof(TypedDictInfo),
    .tp_itemsize = sizeof(TypedDictField),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)TypedDictInfo_clear,
    .tp_traverse = (traverseproc)TypedDictInfo_traverse,
    .tp_dealloc = (destructor)TypedDictInfo_dealloc,
};

static PyObject *
DataclassInfo_Convert(PyObject *obj, bool err_not_json, bool *json_compatible) {
    PyObject *fields = NULL, *field_defaults = NULL;
    DataclassInfo *info = NULL;
    MsgspecState *mod = msgspec_get_global_state();
    bool cache_set = false;

    /* Check if cached */
    PyObject *cached = PyObject_GetAttr(obj, mod->str___msgspec_cache__);
    if (cached != NULL) {
        if (Py_TYPE(cached) != &DataclassInfo_Type) {
            Py_DECREF(cached);
            PyErr_Format(
                PyExc_RuntimeError,
                "%R.__msgspec_cache__ has been overwritten",
                obj
            );
            return NULL;
        }

        /* True if DataclassInfo is still being built (due to a
         * recursive type definition). Just return immediately. */
        if (((DataclassInfo *)cached)->traversing) return cached;

        if (!((DataclassInfo *)cached)->json_compatible) {
            if (json_compatible != NULL) {
                *json_compatible = false;
            }
            if (!err_not_json) return cached;
            /* XXX: If we want to error, we need to recurse again here. This won't
            * modify any internal state, since it will error too early. */
            Py_DECREF(cached);
        }
        else {
            return cached;
        }
    }

    /* Clear the getattr error, if any */
    PyErr_Clear();

    /* Not cached, extract fields from Dataclass object */
    PyObject *temp = CALL_ONE_ARG(mod->get_dataclass_info, obj);
    if (temp == NULL) return NULL;
    fields = PyTuple_GET_ITEM(temp, 0);
    Py_INCREF(fields);
    field_defaults = PyTuple_GET_ITEM(temp, 1);
    Py_INCREF(field_defaults);
    bool has_post_init = PyObject_IsTrue(PyTuple_GET_ITEM(temp, 2));
    Py_DECREF(temp);

    if (cached != NULL) {
        /* If cached is not NULL, this means that a DataclassInfo already exists for
         * this Dataclass, but it's not JSON compatible and we're recursing through
         * again to raise a nice TypeError. In this case we:
         * - Temporarily mark the DataclassInfo object as being recursively traversed
         *   to guard against recursion errors.
         * - Call `TypeNode_Convert` on every field type, bubbling up the error if
         *   raised, otherwise immediately free the result.
         */
        ((DataclassInfo *)cached)->traversing = true;
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(fields); i++) {
            TypeNode *temp = TypeNode_Convert(
                PyTuple_GET_ITEM(PyTuple_GET_ITEM(fields, i), 1),
                err_not_json,
                NULL
            );
            if (temp == NULL) goto found_invalid;
            TypeNode_Free(temp);
        }
    found_invalid:
        ((DataclassInfo *)cached)->traversing = false;
        goto error;
    }

    /* Allocate and zero-out a new DataclassInfo object */
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    info = PyObject_GC_NewVar(DataclassInfo, &DataclassInfo_Type, nfields);
    if (info == NULL) goto error;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        info->fields[i].key = NULL;
        info->fields[i].type = NULL;
    }
    Py_INCREF(field_defaults);
    info->defaults = field_defaults;
    Py_INCREF(obj);
    info->class = obj;
    info->has_post_init = has_post_init;
    info->traversing = true;

    /* If not already cached, then cache on Dataclass object _before_
    * traversing fields. This is to ensure self-referential Dataclasses work. */
    if (PyObject_SetAttr(obj, mod->str___msgspec_cache__, (PyObject *)info) < 0) {
        goto error;
    }
    cache_set = true;

    /* Traverse fields and initialize DataclassInfo */
    bool dict_is_json_compatible = true;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        bool item_is_json_compatible = true;
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        TypeNode *type = TypeNode_Convert(
            PyTuple_GET_ITEM(field, 1), err_not_json, &item_is_json_compatible
        );
        if (type == NULL) goto error;
        /* If field has a default factory, set extra flag bit */
        if (PyObject_IsTrue(PyTuple_GET_ITEM(field, 2))) {
            type->types |= MS_EXTRA_FLAG;
        }
        info->fields[i].type = type;
        info->fields[i].key = PyTuple_GET_ITEM(field, 0);
        Py_INCREF(info->fields[i].key);
        dict_is_json_compatible &= item_is_json_compatible;
    }

    info->traversing = false;
    info->json_compatible = dict_is_json_compatible;
    if (!dict_is_json_compatible && json_compatible != NULL) {
        *json_compatible = false;
    }
    Py_DECREF(fields);
    Py_DECREF(field_defaults);
    PyObject_GC_Track(info);
    return (PyObject *)info;

error:
    if (cache_set) {
        /* An error occurred after the cache was created and set on the
         * Dataclass. We need to delete the attribute. Fetch and restore the
         * original exception to avoid DelAttr silently clearing it on rare
         * occasions. */
        PyObject *err_type, *err_value, *err_tb;
        PyErr_Fetch(&err_type, &err_value, &err_tb);
        PyObject_DelAttr(obj, mod->str___msgspec_cache__);
        PyErr_Restore(err_type, err_value, err_tb);
    }
    Py_XDECREF((PyObject *)info);
    Py_XDECREF(fields);
    Py_XDECREF(field_defaults);
    return NULL;
}

static MS_INLINE PyObject *
DataclassInfo_lookup_key(
    DataclassInfo *self, const char * key, Py_ssize_t key_size,
    TypeNode **type, Py_ssize_t *pos
) {
    const char *field;
    Py_ssize_t nfields, field_size, i, offset = *pos;
    nfields = Py_SIZE(self);
    for (i = offset; i < nfields; i++) {
        field = unicode_str_and_size_nocheck(self->fields[i].key, &field_size);
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i < (nfields - 1) ? (i + 1) : 0;
            *type = self->fields[i].type;
            return self->fields[i].key;
        }
    }
    for (i = 0; i < offset; i++) {
        field = unicode_str_and_size_nocheck(self->fields[i].key, &field_size);
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i + 1;
            *type = self->fields[i].type;
            return self->fields[i].key;
        }
    }
    return NULL;
}

static int
DataclassInfo_post_decode(DataclassInfo *self, PyObject *obj, PathNode *path) {
    Py_ssize_t nfields = Py_SIZE(self);
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(self->defaults);

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *name = self->fields[i].key;
        if (!PyObject_HasAttr(obj, name)) {
            if (i < (nfields - ndefaults)) {
                ms_raise_validation_error(
                    path, "Object missing required field `%U`%U", name
                );
                return -1;
            }
            else {
                PyObject *default_value = PyTuple_GET_ITEM(
                    self->defaults, i - (nfields - ndefaults)
                );
                bool is_factory = self->fields[i].type->types & MS_EXTRA_FLAG;
                if (is_factory) {
                    default_value = CALL_NO_ARGS(default_value);
                    if (default_value == NULL) return -1;
                }
                int status = PyObject_SetAttr(obj, name, default_value);
                if (is_factory) {
                    Py_DECREF(default_value);
                }
                if (status < 0) return -1;
            }
        }
    }
    if (self->has_post_init) {
        MsgspecState *mod = msgspec_get_global_state();
        PyObject *res = CALL_METHOD_NO_ARGS(obj, mod->str___post_init__);
        if (res == NULL) return -1;
        Py_DECREF(res);
    }
    return 0;
}

static int
DataclassInfo_traverse(DataclassInfo *self, visitproc visit, void *arg)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        DataclassField *field = &(self->fields[i]);
        if (field->key != NULL) {
            int out = TypeNode_traverse(field->type, visit, arg);
            if (out != 0) return out;
        }
    }
    Py_VISIT(self->defaults);
    Py_VISIT(self->class);
    return 0;
}

static int
DataclassInfo_clear(DataclassInfo *self)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        Py_CLEAR(self->fields[i].key);
        TypeNode_Free(self->fields[i].type);
        self->fields[i].type = NULL;
    }
    Py_CLEAR(self->defaults);
    Py_CLEAR(self->class);
    return 0;
}

static void
DataclassInfo_dealloc(DataclassInfo *self)
{
    PyObject_GC_UnTrack(self);
    DataclassInfo_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject DataclassInfo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.DataclassInfo",
    .tp_basicsize = sizeof(DataclassInfo),
    .tp_itemsize = sizeof(DataclassField),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)DataclassInfo_clear,
    .tp_traverse = (traverseproc)DataclassInfo_traverse,
    .tp_dealloc = (destructor)DataclassInfo_dealloc,
};

static PyObject *
NamedTupleInfo_Convert(PyObject *obj, bool err_not_json, bool *json_compatible) {
    MsgspecState *mod = msgspec_get_global_state();
    NamedTupleInfo *info = NULL;
    PyObject *annotations = NULL, *fields = NULL, *defaults = NULL, *defaults_list = NULL;
    bool cache_set = false, succeeded = false;

    /* Check if cached */
    PyObject *cached = PyObject_GetAttr(obj, mod->str___msgspec_cache__);
    if (cached != NULL) {
        if (Py_TYPE(cached) != &NamedTupleInfo_Type) {
            Py_DECREF(cached);
            PyErr_Format(
                PyExc_RuntimeError,
                "%R.__msgspec_cache__ has been overwritten",
                obj
            );
            return NULL;
        }

        /* True if NamedTupleInfo is still being built (due to a
         * recursive type definition). Just return immediately. */
        if (((NamedTupleInfo *)cached)->traversing) return cached;

        if (!((NamedTupleInfo *)cached)->json_compatible) {
            if (json_compatible != NULL) {
                *json_compatible = false;
            }
            if (!err_not_json) return cached;
            /* XXX: If we want to error, we need to recurse again here. This won't
            * modify any internal state, since it will error too early. */
            Py_DECREF(cached);
        }
        else {
            return cached;
        }
    }

    /* Clear the getattr error, if any */
    PyErr_Clear();

    /* Not cached, extract fields from NamedTuple object */
    annotations = CALL_ONE_ARG(mod->get_type_hints, obj);
    if (annotations == NULL) goto cleanup;
    fields = PyObject_GetAttr(obj, mod->str__fields);
    if (fields == NULL) goto cleanup;
    defaults = PyObject_GetAttr(obj, mod->str__field_defaults);
    if (defaults == NULL) goto cleanup;

    if (cached != NULL) {
        /* If cached is not NULL, this means that a NamedTupleInfo already exists for
         * this NamedTuple, but it's not JSON compatible and we're recursing through
         * again to raise a nice TypeError. In this case we:
         * - Temporarily mark the NamedTupleInfo object as being recursively traversed
         *   to guard against recursion errors.
         * - Call `TypeNode_Convert` on every field type, bubbling up the error if
         *   raised, otherwise immediately free the result.
         */
        ((NamedTupleInfo *)cached)->traversing = true;
        Py_ssize_t pos = 0;
        PyObject *val;
        while (PyDict_Next(annotations, &pos, NULL, &val)) {
            bool item_is_json_compatible = true;
            TypeNode *type = TypeNode_Convert(val, err_not_json, &item_is_json_compatible);
            if (type == NULL) break;
            TypeNode_Free(type);
        }
        ((NamedTupleInfo *)cached)->traversing = false;
        goto cleanup;
    }

    /* Allocate and zero-out a new NamedTupleInfo object */
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    info = PyObject_GC_NewVar(NamedTupleInfo, &NamedTupleInfo_Type, nfields);
    if (info == NULL) goto cleanup;
    info->class = NULL;
    info->defaults = NULL;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        info->types[i] = NULL;
    }
    /* Mark this info object as still being traversed to guard */
    info->traversing = true;

    /* If not already cached, then cache on NamedTuple object _before_
    * traversing fields. This is to ensure self-referential NamedTuple work. */
    if (PyObject_SetAttr(obj, mod->str___msgspec_cache__, (PyObject *)info) < 0) {
        goto cleanup;
    }
    cache_set = true;

    /* Traverse fields and initialize NamedTupleInfo */
    bool tuple_is_json_compatible = true;
    defaults_list = PyList_New(0);
    if (defaults_list == NULL) goto cleanup;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        /* Get the field type, defaulting to Any */
        PyObject *type_obj = PyDict_GetItem(annotations, field);
        if (type_obj == NULL) {
            type_obj = mod->typing_any;
        }
        /* Convert the type to a TypeNode */
        bool item_is_json_compatible = true;
        TypeNode *type = TypeNode_Convert(type_obj, err_not_json, &item_is_json_compatible);
        tuple_is_json_compatible &= item_is_json_compatible;
        if (type == NULL) goto cleanup;
        info->types[i] = type;
        /* Get the field default (if any), and append it to the list */
        PyObject *default_obj = PyDict_GetItem(defaults, field);
        if (default_obj != NULL) {
            if (PyList_Append(defaults_list, default_obj) < 0) goto cleanup;
        }
    }
    info->traversing = false;
    Py_INCREF(obj);
    info->class = obj;
    info->defaults = PyList_AsTuple(defaults_list);
    if (info->defaults == NULL) goto cleanup;
    info->json_compatible = tuple_is_json_compatible;
    if (!tuple_is_json_compatible && json_compatible != NULL) {
        *json_compatible = false;
    }
    PyObject_GC_Track(info);

    succeeded = true;

cleanup:
    if (!succeeded) {
        Py_CLEAR(info);
        if (cache_set) {
            /* An error occurred after the cache was created and set on the
            * NamedTuple. We need to delete the attribute. Fetch and restore
            * the original exception to avoid DelAttr silently clearing it
            * on rare occasions. */
            PyObject *err_type, *err_value, *err_tb;
            PyErr_Fetch(&err_type, &err_value, &err_tb);
            PyObject_DelAttr(obj, mod->str___msgspec_cache__);
            PyErr_Restore(err_type, err_value, err_tb);
        }
    }
    Py_XDECREF(annotations);
    Py_XDECREF(fields);
    Py_XDECREF(defaults);
    Py_XDECREF(defaults_list);
    return (PyObject *)info;
}

static int
NamedTupleInfo_traverse(NamedTupleInfo *self, visitproc visit, void *arg)
{
    Py_VISIT(self->class);
    Py_VISIT(self->defaults);
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        int out = TypeNode_traverse(self->types[i], visit, arg);
        if (out != 0) return out;
    }
    return 0;
}

static int
NamedTupleInfo_clear(NamedTupleInfo *self)
{
    Py_CLEAR(self->class);
    Py_CLEAR(self->defaults);
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        TypeNode_Free(self->types[i]);
        self->types[i] = NULL;
    }
    return 0;
}

static void
NamedTupleInfo_dealloc(NamedTupleInfo *self)
{
    PyObject_GC_UnTrack(self);
    NamedTupleInfo_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject NamedTupleInfo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core.NamedTupleInfo",
    .tp_basicsize = sizeof(NamedTupleInfo),
    .tp_itemsize = sizeof(TypeNode *),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)NamedTupleInfo_clear,
    .tp_traverse = (traverseproc)NamedTupleInfo_traverse,
    .tp_dealloc = (destructor)NamedTupleInfo_dealloc,
};


/*************************************************************************
 * Ext                                                                   *
 *************************************************************************/

typedef struct Ext {
    PyObject_HEAD
    long code;
    PyObject *data;
} Ext;

static PyObject *
Ext_New(long code, PyObject *data) {
    Ext *out = (Ext *)Ext_Type.tp_alloc(&Ext_Type, 0);
    if (out == NULL)
        return NULL;

    out->code = code;
    Py_INCREF(data);
    out->data = data;
    return (PyObject *)out;
}

PyDoc_STRVAR(Ext__doc__,
"Ext(code, data)\n"
"--\n"
"\n"
"A record representing a MessagePack Extension Type.\n"
"\n"
"Parameters\n"
"----------\n"
"code : int\n"
"    The integer type code for this extension. Must be between -128 and 127.\n"
"data : bytes, bytearray, or memoryview\n"
"    The byte buffer for this extension. One of bytes, bytearray, memoryview,\n"
"    or any object that implements the buffer protocol."
);
static PyObject *
Ext_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    PyObject *pycode, *data;
    long code;
    Py_ssize_t nargs, nkwargs;

    nargs = PyTuple_GET_SIZE(args);
    nkwargs = (kwargs == NULL) ? 0 : PyDict_GET_SIZE(kwargs);

    if (nkwargs != 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Ext takes no keyword arguments"
        );
        return NULL;
    }
    else if (nargs != 2) {
        PyErr_Format(
            PyExc_TypeError,
            "Ext expected 2 arguments, got %zd",
            nargs
        );
        return NULL;
    }

    pycode = PyTuple_GET_ITEM(args, 0);
    data = PyTuple_GET_ITEM(args, 1);

    if (PyLong_CheckExact(pycode)) {
        code = PyLong_AsLong(pycode);
        if ((code == -1 && PyErr_Occurred()) || code > 127 || code < -128) {
            PyErr_SetString(
                PyExc_ValueError,
                "code must be an int between -128 and 127"
            );
            return NULL;
        }
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "code must be an int, got %.200s",
            Py_TYPE(pycode)->tp_name
        );
        return NULL;
    }
    if (!(PyBytes_CheckExact(data) || PyByteArray_CheckExact(data) || PyObject_CheckBuffer(data))) {
        PyErr_Format(
            PyExc_TypeError,
            "data must be a bytes, bytearray, or buffer-like object, got %.200s",
            Py_TYPE(data)->tp_name
        );
        return NULL;
    }
    return Ext_New(code, data);
}

static void
Ext_dealloc(Ext *self)
{
    Py_XDECREF(self->data);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Ext_members[] = {
    {"code", T_INT, offsetof(Ext, code), READONLY, "The extension type code"},
    {"data", T_OBJECT_EX, offsetof(Ext, data), READONLY, "The extension data payload"},
    {NULL},
};

static PyObject *
Ext_reduce(PyObject *self, PyObject *unused)
{
    return Py_BuildValue("O(bO)", Py_TYPE(self), ((Ext*)self)->code, ((Ext*)self)->data);
}

static PyObject *
Ext_richcompare(PyObject *self, PyObject *other, int op) {
    int status;
    PyObject *out;
    Ext *ex_self, *ex_other;

    if (Py_TYPE(other) != &Ext_Type) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    ex_self = (Ext *)self;
    ex_other = (Ext *)other;

    status = ex_self->code == ex_other->code;
    if (!status) {
        out = (op == Py_EQ) ? Py_False : Py_True;
    }
    else {
        status = PyObject_RichCompareBool(ex_self->data, ex_other->data, op);
        if (status == -1) return NULL;
        out = status ? Py_True : Py_False;
    }
    Py_INCREF(out);
    return out;
}

static PyMethodDef Ext_methods[] = {
    {"__reduce__", Ext_reduce, METH_NOARGS, "reduce an Ext"},
    {NULL, NULL},
};

static PyTypeObject Ext_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.msgpack.Ext",
    .tp_doc = Ext__doc__,
    .tp_basicsize = sizeof(Ext),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Ext_new,
    .tp_dealloc = (destructor) Ext_dealloc,
    .tp_richcompare = Ext_richcompare,
    .tp_members = Ext_members,
    .tp_methods = Ext_methods
};


/*************************************************************************
 * Shared Encoder structs/methods                                        *
 *************************************************************************/

typedef struct EncoderState {
    PyObject *enc_hook;     /* `enc_hook` callback */
    Py_ssize_t write_buffer_size;  /* Configured internal buffer size */

    PyObject *output_buffer;    /* bytes or bytearray storing the output */
    char *output_buffer_raw;    /* raw pointer to output_buffer internal buffer */
    Py_ssize_t output_len;      /* Length of output_buffer */
    Py_ssize_t max_output_len;  /* Allocation size of output_buffer */
    char* (*resize_buffer)(PyObject**, Py_ssize_t);  /* callback for resizing buffer */

    MsgspecState *mod;   /* module reference */
} EncoderState;

typedef struct Encoder {
    PyObject_HEAD
    EncoderState state;
} Encoder;

static char*
ms_resize_bytes(PyObject** output_buffer, Py_ssize_t size)
{
    int status = _PyBytes_Resize(output_buffer, size);
    if (status < 0) return NULL;
    return PyBytes_AS_STRING(*output_buffer);
}

static char*
ms_resize_bytearray(PyObject** output_buffer, Py_ssize_t size)
{
    int status = PyByteArray_Resize(*output_buffer, size);
    if (status < 0) return NULL;
    return PyByteArray_AS_STRING(*output_buffer);
}

static MS_NOINLINE int
ms_resize(EncoderState *self, Py_ssize_t size)
{
    self->max_output_len = Py_MAX(8, 1.5 * size);
    char *new_buf = self->resize_buffer(&self->output_buffer, self->max_output_len);
    if (new_buf == NULL) return -1;
    self->output_buffer_raw = new_buf;
    return 0;
}

static MS_INLINE int
ms_ensure_space(EncoderState *self, Py_ssize_t size) {
    Py_ssize_t required = self->output_len + size;
    if (required > self->max_output_len) {
        return ms_resize(self, required);
    }
    return 0;
}

static MS_INLINE int
ms_write(EncoderState *self, const char *s, Py_ssize_t n)
{
    Py_ssize_t required = self->output_len + n;
    if (MS_UNLIKELY(required > self->max_output_len)) {
        if (ms_resize(self, required) < 0) return -1;
    }
    memcpy(self->output_buffer_raw + self->output_len, s, n);
    self->output_len += n;
    return 0;
}

static int
Encoder_init(Encoder *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"enc_hook", "write_buffer_size", NULL};
    Py_ssize_t write_buffer_size = 512;
    PyObject *enc_hook = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$On", kwlist,
                                     &enc_hook, &write_buffer_size)) {
        return -1;
    }

    if (enc_hook == Py_None) {
        enc_hook = NULL;
    }
    if (enc_hook != NULL) {
        if (!PyCallable_Check(enc_hook)) {
            PyErr_SetString(PyExc_TypeError, "enc_hook must be callable");
            return -1;
        }
        Py_INCREF(enc_hook);
    }

    self->state.mod = msgspec_get_global_state();

    self->state.enc_hook = enc_hook;
    self->state.write_buffer_size = Py_MAX(write_buffer_size, 32);
    self->state.max_output_len = self->state.write_buffer_size;
    self->state.output_len = 0;
    self->state.output_buffer = NULL;
    self->state.resize_buffer = &ms_resize_bytes;
    return 0;
}

static int
Encoder_clear(Encoder *self)
{
    Py_CLEAR(self->state.output_buffer);
    Py_CLEAR(self->state.enc_hook);
    return 0;
}

static void
Encoder_dealloc(Encoder *self)
{
    PyObject_GC_UnTrack(self);
    Encoder_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Encoder_traverse(Encoder *self, visitproc visit, void *arg)
{
    Py_VISIT(self->state.enc_hook);
    return 0;
}

static PyObject*
Encoder_sizeof(Encoder *self)
{
    Py_ssize_t res;

    res = sizeof(Encoder);
    if (self->state.output_buffer != NULL) {
        res += self->state.max_output_len;
    }
    return PyLong_FromSsize_t(res);
}

PyDoc_STRVAR(Encoder_encode_into__doc__,
"encode_into(self, obj, buffer, offset=0, /)\n"
"--\n"
"\n"
"Serialize an object into an existing bytearray buffer.\n"
"\n"
"Upon success, the buffer will be truncated to the end of the serialized\n"
"message. Note that the underlying memory buffer *won't* be truncated,\n"
"allowing for efficiently appending additional bytes later.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"buffer : bytearray\n"
"    The buffer to serialize into.\n"
"offset : int, optional\n"
"    The offset into the buffer to start writing at. Defaults to 0. Set to -1\n"
"    to start writing at the end of the buffer.\n"
"\n"
"Returns\n"
"-------\n"
"None"
);
static PyObject*
encoder_encode_into_common(
    EncoderState *state,
    PyObject *const *args,
    Py_ssize_t nargs,
    int(*encode)(EncoderState*, PyObject*)
)
{
    int status;
    PyObject *obj, *old_buf, *buf;
    Py_ssize_t buf_size, offset = 0;

    if (!check_positional_nargs(nargs, 2, 3)) {
        return NULL;
    }
    obj = args[0];
    buf = args[1];
    if (!PyByteArray_CheckExact(buf)) {
        PyErr_SetString(PyExc_TypeError, "buffer must be a `bytearray`");
        return NULL;
    }
    buf_size = PyByteArray_GET_SIZE(buf);

    if (nargs == 3) {
        offset = PyLong_AsSsize_t(args[2]);
        if (offset == -1) {
            if (PyErr_Occurred()) return NULL;
            offset = buf_size;
        }
        if (offset < 0) {
            PyErr_SetString(PyExc_ValueError, "offset must be >= -1");
            return NULL;
        }
        if (offset > buf_size) {
            offset = buf_size;
        }
    }

    /* Setup buffer */
    old_buf = state->output_buffer;
    state->output_buffer = buf;
    state->output_buffer_raw = PyByteArray_AS_STRING(buf);
    state->resize_buffer = &ms_resize_bytearray;
    state->output_len = offset;
    state->max_output_len = buf_size;

    status = encode(state, obj);

    /* Reset buffer */
    state->output_buffer = old_buf;
    state->resize_buffer = &ms_resize_bytes;
    if (old_buf != NULL) {
        state->output_buffer_raw = PyBytes_AS_STRING(old_buf);
    }

    if (status == 0) {
        FAST_BYTEARRAY_SHRINK(buf, state->output_len);
        Py_RETURN_NONE;
    }
    return NULL;
}

PyDoc_STRVAR(Encoder_encode__doc__,
"encode(self, obj)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object.\n"
);
static PyObject*
encoder_encode_common(
    EncoderState *state,
    PyObject *const *args,
    Py_ssize_t nargs,
    int(*encode)(EncoderState*, PyObject*)
)
{
    int status;
    PyObject *res = NULL;

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }

    /* reset buffer */
    state->output_len = 0;
    if (state->output_buffer == NULL) {
        state->max_output_len = state->write_buffer_size;
        state->output_buffer = PyBytes_FromStringAndSize(NULL, state->max_output_len);
        if (state->output_buffer == NULL) return NULL;
        state->output_buffer_raw = PyBytes_AS_STRING(state->output_buffer);
    }

    status = encode(state, args[0]);

    if (status == 0) {
        if (state->max_output_len > state->write_buffer_size) {
            /* Buffer was resized, trim to length */
            res = state->output_buffer;
            state->output_buffer = NULL;
            FAST_BYTES_SHRINK(res, state->output_len);
        }
        else {
            /* Only constant buffer used, copy to output */
            res = PyBytes_FromStringAndSize(
                PyBytes_AS_STRING(state->output_buffer),
                state->output_len
            );
        }
    } else {
        /* Error in encode, drop buffer if necessary */
        if (state->max_output_len > state->write_buffer_size) {
            Py_DECREF(state->output_buffer);
            state->output_buffer = NULL;
        }
    }
    return res;
}

static PyObject*
encode_common(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwnames,
    int(*encode)(EncoderState*, PyObject*)
)
{
    int status;
    PyObject *enc_hook = NULL, *res = NULL;
    EncoderState state;

    state.mod = msgspec_get_global_state();

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if ((enc_hook = find_keyword(kwnames, args + nargs, state.mod->str_enc_hook)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    if (enc_hook == Py_None) {
        enc_hook = NULL;
    }
    if (enc_hook != NULL) {
        if (!PyCallable_Check(enc_hook)) {
            PyErr_SetString(PyExc_TypeError, "enc_hook must be callable");
            return NULL;
        }
    }
    state.enc_hook = enc_hook;

    /* use a smaller buffer size here to reduce chance of over allocating for one-off calls */
    state.write_buffer_size = 32;
    state.max_output_len = state.write_buffer_size;
    state.output_len = 0;
    state.output_buffer = PyBytes_FromStringAndSize(NULL, state.max_output_len);
    if (state.output_buffer == NULL) return NULL;
    state.output_buffer_raw = PyBytes_AS_STRING(state.output_buffer);
    state.resize_buffer = &ms_resize_bytes;

    status = encode(&state, args[0]);

    if (status == 0) {
        /* Trim output to length */
        res = state.output_buffer;
        FAST_BYTES_SHRINK(res, state.output_len);
    } else {
        /* Error in encode, drop buffer */
        Py_CLEAR(state.output_buffer);
    }
    return res;
}

static PyMemberDef Encoder_members[] = {
    {"enc_hook", T_OBJECT, offsetof(Encoder, state.enc_hook), READONLY, "The encoder enc_hook"},
    {"write_buffer_size", T_PYSSIZET, offsetof(Encoder, state.write_buffer_size),
        READONLY, "The encoder write buffer size"},
    {NULL},
};

/*************************************************************************
 * Shared Decoding Utilities                                             *
 *************************************************************************/

static MS_NOINLINE PyObject *
ms_decode_str_enum_or_literal(const char *name, Py_ssize_t size, TypeNode *type, PathNode *path) {
    StrLookup *lookup = TypeNode_get_str_enum_or_literal(type);
    PyObject *out = StrLookup_Get(lookup, name, size);
    if (out == NULL) {
        PyObject *val = PyUnicode_DecodeUTF8(name, size, NULL);
        if (val == NULL) return NULL;
        ms_raise_validation_error(path, "Invalid enum value '%U'%U", val);
        Py_DECREF(val);
        return NULL;
    }
    Py_INCREF(out);
    return out;
}

static MS_NOINLINE PyObject *
ms_decode_int_enum_or_literal_int64(int64_t val, TypeNode *type, PathNode *path) {
    IntLookup *lookup = TypeNode_get_int_enum_or_literal(type);
    PyObject *out = IntLookup_GetInt64(lookup, val);
    if (out == NULL) {
        ms_raise_validation_error(path, "Invalid enum value %lld%U", val);
        return NULL;
    }
    Py_INCREF(out);
    return out;
}

static MS_NOINLINE PyObject *
ms_decode_int_enum_or_literal_uint64(uint64_t val, TypeNode *type, PathNode *path) {
    IntLookup *lookup = TypeNode_get_int_enum_or_literal(type);
    PyObject *out = IntLookup_GetUInt64(lookup, val);
    if (out == NULL) {
        ms_raise_validation_error(path, "Invalid enum value %llu%U", val);
        return NULL;
    }
    Py_INCREF(out);
    return out;
}

static MS_NOINLINE PyObject *
ms_decode_int_enum_or_literal_pyint(PyObject *obj, TypeNode *type, PathNode *path) {
    uint64_t x;
    bool neg, overflow;
    PyObject *out = NULL;
    IntLookup *lookup = TypeNode_get_int_enum_or_literal(type);
    overflow = fast_long_extract_parts(obj, &neg, &x);
    if (!overflow) {
        if (neg) {
            out = IntLookup_GetInt64(lookup, -(int64_t)x);
        }
        else {
            out = IntLookup_GetUInt64(lookup, x);
        }
    }
    if (out == NULL) {
        ms_raise_validation_error(path, "Invalid enum value %R%U", obj);
        return NULL;
    }
    Py_INCREF(out);
    return out;
}

static MS_NOINLINE PyObject *
ms_decode_custom(PyObject *obj, PyObject *dec_hook, TypeNode* type, PathNode *path) {
    PyObject *custom_cls = NULL, *custom_obj, *out = NULL;
    int status;
    bool generic = type->types & MS_TYPE_CUSTOM_GENERIC;

    if (obj == NULL) return NULL;

    if (obj == Py_None && type->types & MS_TYPE_NONE) return obj;

    custom_obj = TypeNode_get_custom(type);

    if (dec_hook != NULL) {
        out = PyObject_CallFunctionObjArgs(dec_hook, custom_obj, obj, NULL);
        Py_DECREF(obj);
        if (out == NULL)
            return NULL;
    }
    else {
        out = obj;
    }

    /* Generic classes must be checked based on __origin__ */
    if (generic) {
        MsgspecState *st = msgspec_get_global_state();
        custom_cls = PyObject_GetAttr(custom_obj, st->str___origin__);
        if (custom_cls == NULL) {
            Py_DECREF(out);
            return NULL;
        }
    }
    else {
        custom_cls = custom_obj;
    }

    /* Check that the decoded value matches the expected type */
    status = PyObject_IsInstance(out, custom_cls);
    if (status == 0) {
        ms_raise_validation_error(
            path,
            "Expected `%s`, got `%s`%U",
            ((PyTypeObject *)custom_cls)->tp_name,
            Py_TYPE(out)->tp_name
        );
        Py_CLEAR(out);
    }
    else if (status == -1) {
        Py_CLEAR(out);
    }

    if (generic) {
        Py_DECREF(custom_cls);
    }
    return out;
}

static MS_NOINLINE PyObject *
_err_int_constraint(const char *msg, int64_t c, PathNode *path) {
    ms_raise_validation_error(path, msg, c);
    return NULL;
}

static MS_NOINLINE PyObject *
ms_decode_constr_int(int64_t x, TypeNode *type, PathNode *path) {
    if (type->types & MS_CONSTR_INT_MIN) {
        int64_t c = TypeNode_get_constr_int_min(type);
        bool ok = x >= c;
        if (MS_UNLIKELY(!ok)) {
            return _err_int_constraint("Expected `int` >= %lld%U", c, path);
        }
    }
    if (type->types & MS_CONSTR_INT_MAX) {
        int64_t c = TypeNode_get_constr_int_max(type);
        bool ok = x <= c;
        if (MS_UNLIKELY(!ok)) {
            return _err_int_constraint("Expected `int` <= %lld%U", c, path);
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_INT_MULTIPLE_OF)) {
        int64_t c = TypeNode_get_constr_int_multiple_of(type);
        bool ok = (x % c) == 0;
        if (MS_UNLIKELY(!ok)) {
            return _err_int_constraint(
                "Expected `int` that's a multiple of %lld%U", c, path
            );
        }
    }
    return PyLong_FromLongLong(x);
}

static MS_INLINE PyObject *
ms_decode_int(int64_t x, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_INT_CONSTRS)) {
        return ms_decode_constr_int(x, type, path);
    }
    return PyLong_FromLongLong(x);
}

static MS_NOINLINE PyObject *
ms_decode_constr_uint(uint64_t x, TypeNode *type, PathNode *path) {
    if (type->types & MS_CONSTR_INT_MAX) {
        int64_t c = TypeNode_get_constr_int_max(type);
        return _err_int_constraint("Expected `int` <= %lld%U", c, path);
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_INT_MULTIPLE_OF)) {
        int64_t c = TypeNode_get_constr_int_multiple_of(type);
        bool ok = (x % c) == 0;
        if (MS_UNLIKELY(!ok)) {
            return _err_int_constraint(
                "Expected `int` that's a multiple of %lld%U", c, path
            );
        }
    }
    return PyLong_FromUnsignedLongLong(x);
}

static MS_INLINE PyObject *
ms_decode_uint(uint64_t x, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_INT_CONSTRS)) {
        if (MS_LIKELY(x <= LLONG_MAX)) {
            return ms_decode_int(x, type, path);
        }
        return ms_decode_constr_uint(x, type, path);
    }
    return PyLong_FromUnsignedLongLong(x);
}

static MS_NOINLINE bool
ms_passes_int_constraints(uint64_t ux, bool neg, TypeNode *type, PathNode *path) {
    if (type->types & MS_CONSTR_INT_MIN) {
        int64_t c = TypeNode_get_constr_int_min(type);
        bool ok = (
            neg ? ((-(int64_t)ux) >= c) :
            ((c < 0) || (ux >= (uint64_t)c))
        );
        if (MS_UNLIKELY(!ok)) {
            _err_int_constraint("Expected `int` >= %lld%U", c, path);
            return false;
        }
    }
    if (type->types & MS_CONSTR_INT_MAX) {
        int64_t c = TypeNode_get_constr_int_max(type);
        bool ok = (
            neg ? ((-(int64_t)ux) <= c) :
            ((c >= 0) && (ux <= (uint64_t)c))
        );
        if (MS_UNLIKELY(!ok)) {
            _err_int_constraint("Expected `int` <= %lld%U", c, path);
            return false;
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_INT_MULTIPLE_OF)) {
        int64_t c = TypeNode_get_constr_int_multiple_of(type);
        bool ok = (ux % c) == 0;
        if (MS_UNLIKELY(!ok)) {
            _err_int_constraint(
                "Expected `int` that's a multiple of %lld%U", c, path
            );
            return false;
        }
    }
    return true;
}

static MS_INLINE PyObject *
ms_decode_pyint(PyObject *obj, TypeNode *type, PathNode *path) {
    uint64_t ux;
    bool neg, overflow;
    overflow = fast_long_extract_parts(obj, &neg, &ux);
    if (MS_UNLIKELY(overflow)) {
        return ms_error_with_path("Integer value out of range%U", path);
    }
    if (MS_UNLIKELY(type->types & MS_INT_CONSTRS)) {
        if (!ms_passes_int_constraints(ux, neg, type, path)) return NULL;
    }
    Py_INCREF(obj);
    return obj;
}

static MS_NOINLINE PyObject *
_err_float_constraint(
    const char *msg, int offset, double c, PathNode *path
) {
    if (offset == 1) {
        c = nextafter(c, DBL_MAX);
    }
    else if (offset == -1) {
        c = nextafter(c, -DBL_MAX);
    }
    PyObject *py_c = PyFloat_FromDouble(c);
    if (py_c != NULL) {
        ms_raise_validation_error(path, "Expected `float` %s %R%U", msg, py_c);
        Py_DECREF(py_c);
    }
    return NULL;
}

static MS_INLINE bool
ms_passes_float_constraints_inline(double x, TypeNode *type, PathNode *path) {
    if (type->types & (MS_CONSTR_FLOAT_GE | MS_CONSTR_FLOAT_GT)) {
        double c = TypeNode_get_constr_float_min(type);
        bool ok = x >= c;
        if (MS_UNLIKELY(!ok)) {
            bool eq = type->types & MS_CONSTR_FLOAT_GE;
            _err_float_constraint(
                eq ? ">=" : ">",
                eq ? 0 : -1,
                c,
                path
            );
            return false;
        }
    }
    if (type->types & (MS_CONSTR_FLOAT_LE | MS_CONSTR_FLOAT_LT)) {
        double c = TypeNode_get_constr_float_max(type);
        bool ok = x <= c;
        if (MS_UNLIKELY(!ok)) {
            bool eq = type->types & MS_CONSTR_FLOAT_LE;
            _err_float_constraint(
                eq ? "<=" : "<",
                eq ? 0 : 1,
                c,
                path
            );
            return false;
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_FLOAT_MULTIPLE_OF)) {
        double c = TypeNode_get_constr_float_multiple_of(type);
        bool ok = x == 0 || fmod(x, c) == 0.0;
        if (MS_UNLIKELY(!ok)) {
            _err_float_constraint(
                "that's a multiple of", 0, c, path
            );
            return false;
        }
    }
    return true;
}

static MS_NOINLINE PyObject *
ms_decode_constr_float(double x, TypeNode *type, PathNode *path) {
    if (!ms_passes_float_constraints_inline(x, type, path)) return NULL;
    return PyFloat_FromDouble(x);
}

static MS_INLINE PyObject *
ms_decode_float(double x, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_FLOAT_CONSTRS)) {
        return ms_decode_constr_float(x, type, path);
    }
    return PyFloat_FromDouble(x);
}

static MS_NOINLINE PyObject *
ms_decode_constr_pyfloat(PyObject *obj, TypeNode *type, PathNode *path) {
    double x = PyFloat_AS_DOUBLE(obj);
    if (!ms_passes_float_constraints_inline(x, type, path)) return NULL;
    Py_INCREF(obj);
    return obj;
}

static MS_INLINE PyObject *
ms_decode_pyfloat(PyObject *obj, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_FLOAT_CONSTRS)) {
        return ms_decode_constr_pyfloat(obj, type, path);
    }
    Py_INCREF(obj);
    return obj;
}

static MS_NOINLINE bool
_err_py_ssize_t_constraint(const char *msg, Py_ssize_t c, PathNode *path) {
    ms_raise_validation_error(path, msg, c);
    return false;
}

static MS_NOINLINE PyObject *
_ms_check_str_constraints(PyObject *obj, TypeNode *type, PathNode *path) {
    if (obj == NULL) return NULL;

    Py_ssize_t len = PyUnicode_GET_LENGTH(obj);

    if (type->types & MS_CONSTR_STR_MIN_LENGTH) {
        Py_ssize_t c = TypeNode_get_constr_str_min_length(type);
        if (len < c) {
            _err_py_ssize_t_constraint(
                "Expected `str` of length >= %zd%U", c, path
            );
            goto error;
        }
    }
    if (type->types & MS_CONSTR_STR_MAX_LENGTH) {
        Py_ssize_t c = TypeNode_get_constr_str_max_length(type);
        if (len > c) {
            _err_py_ssize_t_constraint(
                "Expected `str` of length <= %zd%U", c, path
            );
            goto error;
        }
    }
    if (type->types & MS_CONSTR_STR_REGEX) {
        PyObject *regex = TypeNode_get_constr_str_regex(type);
        PyObject *res = PyObject_CallMethod(regex, "search", "O", obj);
        if (res == NULL) goto error;
        bool ok = (res != Py_None);
        Py_DECREF(res);
        if (!ok) {
            PyObject *pattern = PyObject_GetAttrString(regex, "pattern");
            if (pattern == NULL) goto error;
            ms_raise_validation_error(
                path, "Expected `str` matching regex %R%U", pattern
            );
            Py_DECREF(pattern);
            goto error;
        }
    }
    return obj;

error:
    Py_DECREF(obj);
    return NULL;
}

static MS_INLINE PyObject *
ms_check_str_constraints(PyObject *obj, TypeNode *type, PathNode *path) {
    if (MS_LIKELY(!(type->types & MS_STR_CONSTRS))) return obj;
    return _ms_check_str_constraints(obj, type, path);
}

static bool
ms_passes_bytes_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_CONSTR_BYTES_MIN_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_bytes_min_length(type);
        if (size < c) {
            return _err_py_ssize_t_constraint(
                "Expected `bytes` of length >= %zd%U", c, path
            );
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_BYTES_MAX_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_bytes_max_length(type);
        if (size > c) {
            return _err_py_ssize_t_constraint(
                "Expected `bytes` of length <= %zd%U", c, path
            );
        }
    }
    return true;
}

static MS_NOINLINE bool
_ms_passes_array_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_CONSTR_ARRAY_MIN_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_array_min_length(type);
        if (size < c) {
            return _err_py_ssize_t_constraint(
                "Expected `array` of length >= %zd%U", c, path
            );
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_ARRAY_MAX_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_array_max_length(type);
        if (size > c) {
            return _err_py_ssize_t_constraint(
                "Expected `array` of length <= %zd%U", c, path
            );
        }
    }
    return true;
}

static MS_INLINE bool
ms_passes_array_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_ARRAY_CONSTRS)) {
        return _ms_passes_array_constraints(size, type, path);
    }
    return true;
}

static MS_NOINLINE bool
_ms_passes_map_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_CONSTR_MAP_MIN_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_map_min_length(type);
        if (size < c) {
            return _err_py_ssize_t_constraint(
                "Expected `object` of length >= %zd%U", c, path
            );
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_MAP_MAX_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_map_max_length(type);
        if (size > c) {
           return _err_py_ssize_t_constraint(
                "Expected `object` of length <= %zd%U", c, path
            );
        }
    }
    return true;
}

static MS_INLINE bool
ms_passes_map_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_MAP_CONSTRS)) {
        return _ms_passes_map_constraints(size, type, path);
    }
    return true;
}

static bool
ms_passes_tz_constraint(
    PyObject *tz, TypeNode *type, PathNode *path
) {
    char *err, *type_str;
    if (tz == Py_None) {
        if (type->types & MS_CONSTR_TZ_AWARE) {
            err = "Expected `%s` with a timezone component%U";
            goto error;
        }
    }
    else if (type->types & MS_CONSTR_TZ_NAIVE) {
        err = "Expected `%s` with no timezone component%U";
        goto error;
    }
    return true;

error:
    if (type->types & MS_TYPE_TIME) {
        type_str = "time";
    }
    else {
        type_str = "datetime";
    }

    ms_raise_validation_error(path, err, type_str);
    return false;
}

static int
ms_encode_err_type_unsupported(PyTypeObject *type) {
    PyErr_Format(
        PyExc_TypeError,
        "Encoding objects of type %.200s is unsupported",
        type->tp_name
    );
    return -1;
}

/*************************************************************************
 * Datetime utilities                                                    *
 *************************************************************************/

#define MS_HAS_TZINFO(o)  (((_PyDateTime_BaseTZInfo *)(o))->hastzinfo)
#if PY_VERSION_HEX < 0x030a00f0
#define MS_DATE_GET_TZINFO(o)      (MS_HAS_TZINFO(o) ? \
    ((PyDateTime_DateTime *)(o))->tzinfo : Py_None)
#define MS_TIME_GET_TZINFO(o)      (MS_HAS_TZINFO(o) ? \
    ((PyDateTime_Time *)(o))->tzinfo : Py_None)
#else
#define MS_DATE_GET_TZINFO(o) PyDateTime_DATE_GET_TZINFO(o)
#define MS_TIME_GET_TZINFO(o) PyDateTime_TIME_GET_TZINFO(o)
#endif

static bool
is_leap_year(int year)
{
    unsigned int y = (unsigned int)year;
    return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

static int
days_in_month(int year, int month) {
    static const uint8_t ndays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year))
        return 29;
    else
        return ndays[month - 1];
}

static inline int
divmod(int x, int y, int *r) {
    int quo = x / y;
    *r = x - quo * y;
    if (*r < 0) {
        --quo;
        *r += y;
    }
    return quo;
}

/* Convert a *valid* datetime with a tz offset (in minutes) to UTC time.
 * Returns -1 on error, but no error indicator set */
static int
datetime_apply_tz_offset(
    int *year, int *month, int *day, int *hour,
    int *minute, int tz_offset
) {
    *minute -= tz_offset;
    if (*minute < 0 || *minute >= 60) {
        *hour += divmod(*minute, 60, minute);
    }
    if (*hour < 0 || *hour >= 24) {
        *day += divmod(*hour, 24, hour);
    }
    /* days can only be off by +/- day */
    if (*day == 0) {
        --*month;
        if (*month > 0)
            *day = days_in_month(*year, *month);
        else {
            --*year;
            *month = 12;
            *day = 31;
        }
    }
    else if (*day == days_in_month(*year, *month) + 1) {
        ++*month;
        *day = 1;
        if (*month > 12) {
            *month = 1;
            ++*year;
        }
    }
    if (1 <= *year && *year <= 9999)
        return 0;
    return -1;
}

/* Convert a *valid* time with a tz offset (in minutes) to UTC time. */
static void
time_apply_tz_offset(
    int *hour, int *minute, int tz_offset
) {
    *minute -= tz_offset;
    if (*minute < 0 || *minute >= 60) {
        *hour += divmod(*minute, 60, minute);
    }
    if (*hour < 0 || *hour >= 24) {
        divmod(*hour, 24, hour);
    }
}

/* Days since 0001-01-01, the min value for python's datetime objects */
static int
days_since_min_datetime(int year, int month, int day)
{
    int out = day;
    static const int _days_before_month[] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    out += _days_before_month[month - 1];
    if (month > 2 && is_leap_year(year)) out++;

    year--; /* makes math easier */
    out += year*365 + year/4 - year/100 + year/400;

    return out;
}

static void
datetime_to_epoch(PyObject *obj, int64_t *seconds, int32_t *nanoseconds) {
    int64_t d = days_since_min_datetime(
        PyDateTime_GET_YEAR(obj),
        PyDateTime_GET_MONTH(obj),
        PyDateTime_GET_DAY(obj)
    ) - 719163;  /* days_since_min_datetime(1970, 1, 1) */
    int64_t s = (
        PyDateTime_DATE_GET_HOUR(obj) * 3600
        + PyDateTime_DATE_GET_MINUTE(obj) * 60
        + PyDateTime_DATE_GET_SECOND(obj)
    );
    int64_t us = PyDateTime_DATE_GET_MICROSECOND(obj);

    *seconds = 86400 * d + s;
    *nanoseconds = us * 1000;
}

/* Python datetimes bounded between (inclusive)
 * [0001-01-01T00:00:00.000000, 9999-12-31T23:59:59.999999] UTC */
#define MS_EPOCH_SECS_MAX 253402300800
#define MS_EPOCH_SECS_MIN -62135596800
#define MS_DAYS_PER_400Y (365*400 + 97)
#define MS_DAYS_PER_100Y (365*100 + 24)
#define MS_DAYS_PER_4Y   (365*4 + 1)

/* Epoch -> datetime conversion borrowed and modified from the implementation
 * in musl, found at
 * http://git.musl-libc.org/cgit/musl/tree/src/time/__secs_to_tm.c. musl is
 * copyright Rich Felker et. al, and is licensed under the standard MIT
 * license.  */
static PyObject *
datetime_from_epoch(
    int64_t epoch_secs, uint32_t epoch_nanos, TypeNode *type, PathNode *path
) {
    int64_t days, secs, years;
    int months, remdays, remsecs, remyears;
    int qc_cycles, c_cycles, q_cycles;
    /* Start in Mar not Jan, so leap day is on end */
    static const char days_in_month[] = {31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 31, 29};

    /* Offset to 2000-03-01, a mod 400 year, immediately after feb 29 */
    secs = epoch_secs - (946684800LL + 86400 * (31 + 29));
    days = secs / 86400;
    remsecs = secs % 86400;
    if (remsecs < 0) {
        remsecs += 86400;
        days--;
    }

    qc_cycles = days / MS_DAYS_PER_400Y;
    remdays = days % MS_DAYS_PER_400Y;
    if (remdays < 0) {
        remdays += MS_DAYS_PER_400Y;
        qc_cycles--;
    }

    c_cycles = remdays / MS_DAYS_PER_100Y;
    if (c_cycles == 4) c_cycles--;
    remdays -= c_cycles * MS_DAYS_PER_100Y;

    q_cycles = remdays / MS_DAYS_PER_4Y;
    if (q_cycles == 25) q_cycles--;
    remdays -= q_cycles * MS_DAYS_PER_4Y;

    remyears = remdays / 365;
    if (remyears == 4) remyears--;
    remdays -= remyears * 365;

    years = remyears + 4*q_cycles + 100*c_cycles + 400LL*qc_cycles;

    for (months = 0; days_in_month[months] <= remdays; months++)
        remdays -= days_in_month[months];

    if (months >= 10) {
        months -= 12;
        years++;
    }

    if (!ms_passes_tz_constraint(PyDateTime_TimeZone_UTC, type, path)) return NULL;
    return PyDateTimeAPI->DateTime_FromDateAndTime(
        years + 2000,
        months + 3,
        remdays + 1,
        remsecs / 3600,
        remsecs / 60 % 60,
        remsecs % 60,
        epoch_nanos / 1000,
        PyDateTime_TimeZone_UTC,
        PyDateTimeAPI->DateTimeType
    );
}

static inline char *
ms_write_fixint(char *p, uint32_t x, int width) {
    p += width;
    for (int i = 0; i < width; i++) {
        *--p = (x % 10) + '0';
        x = x / 10;
    }
    return p + width;
}

static inline const char *
ms_read_fixint(const char *buf, int width, int *out) {
    int x = 0;
    for (int i = 0; i < width; i++) {
        char c = *buf++;
        if (!is_digit(c)) return NULL;
        x = x * 10 + (c - '0');
    }
    *out = x;
    return buf;
}

/* Requires 10 bytes of scratch space */
static void
ms_encode_date(PyObject *obj, char *out)
{
    uint32_t year = PyDateTime_GET_YEAR(obj);
    uint8_t month = PyDateTime_GET_MONTH(obj);
    uint8_t day = PyDateTime_GET_DAY(obj);

    out = ms_write_fixint(out, year, 4);
    *out++ = '-';
    out = ms_write_fixint(out, month, 2);
    *out++ = '-';
    out = ms_write_fixint(out, day, 2);
}

/* Requires 21 bytes of scratch space */
static int
ms_encode_time_parts(
    MsgspecState *mod, PyObject *obj,
    uint8_t hour, uint8_t minute, uint8_t second, uint32_t microsecond,
    PyObject *tzinfo, char *out, int out_offset
) {
    char *p = out + out_offset;
    p = ms_write_fixint(p, hour, 2);
    *p++ = ':';
    p = ms_write_fixint(p, minute, 2);
    *p++ = ':';
    p = ms_write_fixint(p, second, 2);
    if (microsecond) {
        *p++ = '.';
        p = ms_write_fixint(p, microsecond, 6);
    }

    if (tzinfo != Py_None) {
        int32_t offset_days = 0, offset_secs = 0;

        if (tzinfo != PyDateTime_TimeZone_UTC) {
            PyObject *offset = CALL_METHOD_ONE_ARG(tzinfo, mod->str_utcoffset, Py_None);
            if (offset == NULL) return -1;
            if (PyDelta_Check(offset)) {
                offset_days = PyDateTime_DELTA_GET_DAYS(offset);
                offset_secs = PyDateTime_DELTA_GET_SECONDS(offset);
            }
            else if (offset != Py_None) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "tzinfo.utcoffset returned a non-timedelta object"
                );
                Py_DECREF(offset);
                return -1;
            }
            Py_DECREF(offset);
        }
        if (MS_LIKELY(offset_secs == 0)) {
            *p++ = 'Z';
        }
        else {
            char sign = '+';
            if (offset_days == -1) {
                sign = '-';
                offset_secs = 86400 - offset_secs;
            }
            uint8_t offset_hour = offset_secs / 3600;
            uint8_t offset_min = (offset_secs / 60) % 60;
            /* If the offset isn't an even number of minutes, RFC 3339
            * indicates that the offset should be rounded to the nearest
            * possible hour:min pair */
            bool round_up = (offset_secs - (offset_hour * 3600 + offset_min * 60)) > 30;
            if (MS_UNLIKELY(round_up)) {
                offset_min++;
                if (offset_min == 60) {
                    offset_min = 0;
                    offset_hour++;
                    if (offset_hour == 24) {
                        offset_hour = 0;
                    }
                }
            }
            if (offset_hour == 0 && offset_min == 0) {
                *p++ = 'Z';
            }
            else {
                *p++ = sign;
                p = ms_write_fixint(p, offset_hour, 2);
                *p++ = ':';
                p = ms_write_fixint(p, offset_min, 2);
            }
        }
    }
    return p - out;
}

/* Requires 21 bytes of scratch space max.
 *
 * Returns +nbytes if successful, -1 on failure */
static int
ms_encode_time(MsgspecState *mod, PyObject *obj, char *out)
{
    uint8_t hour = PyDateTime_TIME_GET_HOUR(obj);
    uint8_t minute = PyDateTime_TIME_GET_MINUTE(obj);
    uint8_t second = PyDateTime_TIME_GET_SECOND(obj);
    uint32_t microsecond = PyDateTime_TIME_GET_MICROSECOND(obj);
    PyObject *tzinfo = MS_TIME_GET_TZINFO(obj);
    return ms_encode_time_parts(
        mod, obj, hour, minute, second, microsecond, tzinfo, out, 0
    );
}

/* Requires 32 bytes of scratch space max.
 *
 * Returns +nbytes if successful, -1 on failure */
static int
ms_encode_datetime(MsgspecState *mod, PyObject *obj, char *out)
{
    uint8_t hour = PyDateTime_DATE_GET_HOUR(obj);
    uint8_t minute = PyDateTime_DATE_GET_MINUTE(obj);
    uint8_t second = PyDateTime_DATE_GET_SECOND(obj);
    uint32_t microsecond = PyDateTime_DATE_GET_MICROSECOND(obj);
    PyObject *tzinfo = MS_DATE_GET_TZINFO(obj);
    ms_encode_date(obj, out);
    out[10] = 'T';
    return ms_encode_time_parts(
        mod, obj, hour, minute, second, microsecond, tzinfo, out, 11
    );
}

static PyObject *
ms_decode_date(const char *buf, Py_ssize_t size, PathNode *path) {
    int year, month, day;

    /* A valid date is 10 characters in length */
    if (size != 10) goto invalid;

    /* Parse date */
    if ((buf = ms_read_fixint(buf, 4, &year)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &month)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &day)) == NULL) goto invalid;

    /* Ensure all numbers are valid */
    if (year == 0) goto invalid;
    if (month == 0 || month > 12) goto invalid;
    if (day == 0 || day > days_in_month(year, month)) goto invalid;

    return PyDateTimeAPI->Date_FromDate(year, month, day, PyDateTimeAPI->DateType);

invalid:
    return ms_error_with_path("Invalid RFC3339 encoded date%U", path);
}

static PyObject *
ms_decode_time(const char *buf, Py_ssize_t size, TypeNode *type, PathNode *path) {
    int hour, minute, second, microsecond = 0, offset = 0;
    const char *buf_end = buf + size;
    bool round_up_micros = false;
    PyObject *tz = Py_None;
    char c;

    /* A valid time is at least 8 characters in length */
    if (size < 8) goto invalid;

    /* Parse time */
    if ((buf = ms_read_fixint(buf, 2, &hour)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &minute)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &second)) == NULL) goto invalid;

    /* Remaining reads require bounds check */
#define next_or_null() (buf == buf_end) ? '\0' : *buf++
    c = next_or_null();

    if (c == '.') {
        int ndigits = 0;
        while (ndigits < 6) {
            c = next_or_null();
            if (!is_digit(c)) goto end_decimal;
            ndigits++;
            microsecond = microsecond * 10 + (c - '0');
        }
        c = next_or_null();
        if (is_digit(c)) {
            /* This timestamp has higher precision than microseconds; parse
            * the next digit to support rounding, then skip all remaining
            * digits */
            if ((c - '0') >= 5) {
                round_up_micros = true;
            }
            while (true) {
                c = next_or_null();
                if (!is_digit(c)) break;
            }
        }
end_decimal:
        /* Error if no digits after decimal */
        if (ndigits == 0) goto invalid;
        int pow10[6] = {100000, 10000, 1000, 100, 10, 1};
        /* Scale microseconds appropriately */
        microsecond *= pow10[ndigits - 1];
    }
#undef next_or_null

    /* Parse timezone */
    if (c == 'Z' || c == 'z') {
        tz = PyDateTime_TimeZone_UTC;

        /* Check for trailing characters */
        if (buf != buf_end) goto invalid;
    }
    else if (c != '\0') {
        int offset_hour, offset_min;
        if (c == '-') {
            offset = -1;
        }
        else if (c == '+') {
            offset = 1;
        }
        else {
            goto invalid;
        }

        /* Explicit offset requires exactly 5 bytes left */
        if (buf_end - buf != 5) goto invalid;

        if ((buf = ms_read_fixint(buf, 2, &offset_hour)) == NULL) goto invalid;
        if (*buf++ != ':') goto invalid;
        if ((buf = ms_read_fixint(buf, 2, &offset_min)) == NULL) goto invalid;
        if (offset_hour > 23 || offset_min > 59) goto invalid;
        offset *= (offset_hour * 60 + offset_min);
        tz = PyDateTime_TimeZone_UTC;
    }

    /* Ensure all numbers are valid */
    if (hour > 23) goto invalid;
    if (minute > 59) goto invalid;
    if (second > 59) goto invalid;

    if (MS_UNLIKELY(round_up_micros)) {
        microsecond++;
        if (microsecond == 1000000) {
            microsecond = 0;
            second++;
            if (second == 60) {
                second = 0;
                offset--;
            }
        }
    }

    if (offset) time_apply_tz_offset(&hour, &minute, offset);

    if (!ms_passes_tz_constraint(tz, type, path)) return NULL;
    return PyDateTimeAPI->Time_FromTime(
        hour, minute, second, microsecond, tz, PyDateTimeAPI->TimeType
    );

invalid:
    return ms_error_with_path("Invalid RFC3339 encoded time%U", path);
}

static PyObject *
ms_decode_datetime(const char *buf, Py_ssize_t size, TypeNode *type, PathNode *path) {
    int year, month, day, hour, minute, second, microsecond = 0, offset = 0;
    const char *buf_end = buf + size;
    bool round_up_micros = false;
    PyObject *tz = Py_None;
    char c;

    /* A valid datetime is at least 19 characters in length */
    if (size < 19) goto invalid;

    /* Parse date */
    if ((buf = ms_read_fixint(buf, 4, &year)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &month)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &day)) == NULL) goto invalid;

    /* Date/time separator can be T or t */
    c = *buf++;
    if (!(c == 'T' || c == 't')) goto invalid;

    /* Parse time */
    if ((buf = ms_read_fixint(buf, 2, &hour)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &minute)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &second)) == NULL) goto invalid;

    /* Remaining reads require bounds check */
#define next_or_null() (buf == buf_end) ? '\0' : *buf++
    c = next_or_null();

    /* Parse decimal if present.
     *
     * Python datetime's only supports microsecond precision, but RFC3339
     * doesn't specify a decimal precision limit. To work around this we
     * support infinite decimal digits, but round to the closest microsecond.
     * This means that nanosecond timestamps won't properly roundtrip through
     * datetime objects, but there's not much we can do about that. Other
     * systems commonly accept 3 or 6 digits, support for/usage of nanosecond
     * precision is rare. */
    if (c == '.') {
        int ndigits = 0;
        while (ndigits < 6) {
            c = next_or_null();
            if (!is_digit(c)) goto end_decimal;
            ndigits++;
            microsecond = microsecond * 10 + (c - '0');
        }
        c = next_or_null();
        if (is_digit(c)) {
            /* This timestamp has higher precision than microseconds; parse
            * the next digit to support rounding, then skip all remaining
            * digits */
            if ((c - '0') >= 5) {
                round_up_micros = true;
            }
            while (true) {
                c = next_or_null();
                if (!is_digit(c)) break;
            }
        }
end_decimal:
        /* Error if no digits after decimal */
        if (ndigits == 0) goto invalid;
        int pow10[6] = {100000, 10000, 1000, 100, 10, 1};
        /* Scale microseconds appropriately */
        microsecond *= pow10[ndigits - 1];
    }
#undef next_or_null

    /* Parse timezone */
    if (c == 'Z' || c == 'z') {
        tz = PyDateTime_TimeZone_UTC;

        /* Check for trailing characters */
        if (buf != buf_end) goto invalid;
    }
    else if (c != '\0') {
        int offset_hour, offset_min;
        if (c == '-') {
            offset = -1;
        }
        else if (c == '+') {
            offset = 1;
        }
        else {
            goto invalid;
        }

        /* Explicit offset requires exactly 5 bytes left */
        if (buf_end - buf != 5) goto invalid;

        if ((buf = ms_read_fixint(buf, 2, &offset_hour)) == NULL) goto invalid;
        if (*buf++ != ':') goto invalid;
        if ((buf = ms_read_fixint(buf, 2, &offset_min)) == NULL) goto invalid;
        if (offset_hour > 23 || offset_min > 59) goto invalid;
        offset *= (offset_hour * 60 + offset_min);
        tz = PyDateTime_TimeZone_UTC;
    }

    /* Ensure all numbers are valid */
    if (year == 0) goto invalid;
    if (month == 0 || month > 12) goto invalid;
    if (day == 0 || day > days_in_month(year, month)) goto invalid;
    if (hour > 23) goto invalid;
    if (minute > 59) goto invalid;
    if (second > 59) goto invalid;

    if (MS_UNLIKELY(round_up_micros)) {
        microsecond++;
        if (microsecond == 1000000) {
            microsecond = 0;
            second++;
            if (second == 60) {
                second = 0;
                offset--;
            }
        }
    }

    if (offset) {
        if (datetime_apply_tz_offset(&year, &month, &day, &hour, &minute, offset) < 0) {
            goto invalid;
        }
    }

    if (!ms_passes_tz_constraint(tz, type, path)) return NULL;
    return PyDateTimeAPI->DateTime_FromDateAndTime(
        year, month, day, hour, minute, second, microsecond, tz,
        PyDateTimeAPI->DateTimeType
    );

invalid:
    return ms_error_with_path("Invalid RFC3339 encoded datetime%U", path);
}

/*************************************************************************
 * Base64 Encoder                                                        *
 *************************************************************************/

static Py_ssize_t
ms_encode_base64_size(MsgspecState *mod, Py_ssize_t input_size) {
    if (input_size >= (1LL << 32)) {
        PyErr_SetString(
            mod->EncodeError,
            "Can't encode bytes-like objects longer than 2**32 - 1"
        );
        return -1;
    }
    /* ceil(4/3 * input_size) */
    return 4 * ((input_size + 2) / 3);
}

static void
ms_encode_base64(const char *input, Py_ssize_t input_size, char *out) {
    int nbits = 0;
    unsigned int charbuf = 0;
    for (; input_size > 0; input_size--, input++) {
        charbuf = (charbuf << 8) | (unsigned char)(*input);
        nbits += 8;
        while (nbits >= 6) {
            unsigned char ind = (charbuf >> (nbits - 6)) & 0x3f;
            nbits -= 6;
            *out++ = base64_encode_table[ind];
        }
    }
    if (nbits == 2) {
        *out++ = base64_encode_table[(charbuf & 3) << 4];
        *out++ = '=';
        *out++ = '=';
    }
    else if (nbits == 4) {
        *out++ = base64_encode_table[(charbuf & 0xf) << 2];
        *out++ = '=';
    }
}

/*************************************************************************
 * UUID Utilities                                                        *
 *************************************************************************/

static int
ms_encode_uuid(MsgspecState *mod, PyObject *obj, char *out) {
    int status = -1;
    PyObject *int128 = PyObject_GetAttr(obj, mod->str_int);
    if (int128 == NULL) return -1;
    if (MS_UNLIKELY(!PyLong_CheckExact(int128))) {
        PyErr_SetString(PyExc_TypeError, "uuid.int must be an int");
        return -1;
    }
    unsigned char scratch[16];
    unsigned char *buf = scratch;

    if (_PyLong_AsByteArray((PyLongObject *)int128, scratch, 16, 0, 0) < 0) {
        goto cleanup;
    }

    for (int i = 0; i < 4; i++) {
        unsigned char c = *buf++;
        *out++ = hex_encode_table[c >> 4];
        *out++ = hex_encode_table[c & 0xF];
    }
    *out++ = '-';
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 2; i++) {
            unsigned char c = *buf++;
            *out++ = hex_encode_table[c >> 4];
            *out++ = hex_encode_table[c & 0xF];
        }
        *out++ = '-';
    }
    for (int i = 0; i < 6; i++) {
        unsigned char c = *buf++;
        *out++ = hex_encode_table[c >> 4];
        *out++ = hex_encode_table[c & 0xF];
    }
    status = 0;
cleanup:
    Py_DECREF(int128);
    return status;
}


static PyObject *
ms_decode_uuid(const char *buf, Py_ssize_t size, PathNode *path) {
    PyObject *out = NULL;
    unsigned char scratch[16];
    unsigned char *decoded = scratch;
    int segments[] = {4, 2, 2, 2, 6};

    /* A valid uuid is 36 characters in length */
    if (size != 36) goto invalid;

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < segments[i]; j++) {
            char hi = *buf++;
            if (hi >= '0' && hi <= '9') { hi -= '0'; }
            else if (hi >= 'a' && hi <= 'f') { hi = hi - 'a' + 10; }
            else if (hi >= 'A' && hi <= 'F') { hi = hi - 'A' + 10; }
            else { goto invalid; }

            char lo = *buf++;
            if (lo >= '0' && lo <= '9') { lo -= '0'; }
            else if (lo >= 'a' && lo <= 'f') { lo = lo - 'a' + 10; }
            else if (lo >= 'A' && lo <= 'F') { lo = lo - 'A' + 10; }
            else { goto invalid; }

            *decoded++ = ((unsigned char)hi << 4) + (unsigned char)lo;
        }
        if (i < 4 && *buf++ != '-') goto invalid;
    }
    PyObject *int128 = _PyLong_FromByteArray(scratch, 16, 0, 0);
    if (int128 == NULL) return NULL;

    MsgspecState *mod = msgspec_get_global_state();
    PyTypeObject *uuid_type = (PyTypeObject *)(mod->UUIDType);
    out = uuid_type->tp_alloc(uuid_type, 0);
    if (out == NULL) goto error;
    /* UUID objects are immutable, use GenericSetAttr instead of SetAttr */
    if (PyObject_GenericSetAttr(out, mod->str_int, int128) < 0) goto error;
    if (PyObject_GenericSetAttr(out, mod->str_is_safe, mod->uuid_safeuuid_unknown) < 0) goto error;

    Py_DECREF(int128);
    return out;

error:
    Py_DECREF(int128);
    Py_XDECREF(out);
    return NULL;

invalid:
    return ms_error_with_path("Invalid UUID%U", path);
}

static PyObject *
ms_decode_decimal_pyobj(MsgspecState *mod, PyObject *str, PathNode *path) {
    PyObject *out = CALL_ONE_ARG(mod->DecimalType, str);
    if (out == NULL) {
        ms_error_with_path("Invalid decimal string%U", path);
    }
    return out;
}

static PyObject *
ms_decode_decimal(const char *view, Py_ssize_t size, bool is_ascii, PathNode *path) {
    PyObject *str;

    if (MS_LIKELY(is_ascii)) {
        str = PyUnicode_New(size, 127);
        if (str == NULL) return NULL;
        memcpy(ascii_get_buffer(str), view, size);
    }
    else {
        str = PyUnicode_DecodeUTF8(view, size, NULL);
        if (str == NULL) return NULL;
    }
    PyObject *out = ms_decode_decimal_pyobj(
        msgspec_get_global_state(), str, path
    );
    Py_DECREF(str);
    return out;
}

/*************************************************************************
 * MessagePack Encoder                                                   *
 *************************************************************************/

PyDoc_STRVAR(Encoder__doc__,
"Encoder(*, enc_hook=None, write_buffer_size=512)\n"
"--\n"
"\n"
"A MessagePack encoder.\n"
"\n"
"Parameters\n"
"----------\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"write_buffer_size : int, optional\n"
"    The size of the internal static write buffer."
);

enum mpack_code {
    MP_NIL = '\xc0',
    MP_FALSE = '\xc2',
    MP_TRUE = '\xc3',
    MP_FLOAT32 = '\xca',
    MP_FLOAT64 = '\xcb',
    MP_UINT8 = '\xcc',
    MP_UINT16 = '\xcd',
    MP_UINT32 = '\xce',
    MP_UINT64 = '\xcf',
    MP_INT8 = '\xd0',
    MP_INT16 = '\xd1',
    MP_INT32 = '\xd2',
    MP_INT64 = '\xd3',
    MP_FIXSTR = '\xa0',
    MP_STR8 = '\xd9',
    MP_STR16 = '\xda',
    MP_STR32 = '\xdb',
    MP_BIN8 = '\xc4',
    MP_BIN16 = '\xc5',
    MP_BIN32 = '\xc6',
    MP_FIXARRAY = '\x90',
    MP_ARRAY16 = '\xdc',
    MP_ARRAY32 = '\xdd',
    MP_FIXMAP = '\x80',
    MP_MAP16 = '\xde',
    MP_MAP32 = '\xdf',
    MP_FIXEXT1 = '\xd4',
    MP_FIXEXT2 = '\xd5',
    MP_FIXEXT4 = '\xd6',
    MP_FIXEXT8 = '\xd7',
    MP_FIXEXT16 = '\xd8',
    MP_EXT8 = '\xc7',
    MP_EXT16 = '\xc8',
    MP_EXT32 = '\xc9',
};

static int mpack_encode_inline(EncoderState *self, PyObject *obj);
static int mpack_encode(EncoderState *self, PyObject *obj);

static int
mpack_encode_none(EncoderState *self)
{
    const char op = MP_NIL;
    return ms_write(self, &op, 1);
}

static int
mpack_encode_bool(EncoderState *self, PyObject *obj)
{
    const char op = (obj == Py_True) ? MP_TRUE : MP_FALSE;
    return ms_write(self, &op, 1);
}

static MS_NOINLINE int
mpack_encode_long(EncoderState *self, PyObject *obj)
{
    bool overflow, neg;
    uint64_t ux;
    overflow = fast_long_extract_parts(obj, &neg, &ux);
    if (MS_UNLIKELY(overflow)) {
        PyErr_SetString(
            PyExc_OverflowError,
            "can't serialize ints < -2**63 or > 2**64 - 1"
        );
        return -1;
    }
    if (MS_UNLIKELY(neg)) {
        int64_t x = -ux;
        if(x < -(1LL<<5)) {
            if(x < -(1LL<<15)) {
                if(x < -(1LL<<31)) {
                    char buf[9];
                    buf[0] = MP_INT64;
                    _msgspec_store64(&buf[1], x);
                    return ms_write(self, buf, 9);
                } else {
                    char buf[5];
                    buf[0] = MP_INT32;
                    _msgspec_store32(&buf[1], (int32_t)x);
                    return ms_write(self, buf, 5);
                }
            } else {
                if(x < -(1<<7)) {
                    char buf[3];
                    buf[0] = MP_INT16;
                    _msgspec_store16(&buf[1], (int16_t)x);
                    return ms_write(self, buf, 3);
                } else {
                    char buf[2] = {MP_INT8, (x & 0xff)};
                    return ms_write(self, buf, 2);
                }
            }
        }
        else {
            char buf[1] = {(x & 0xff)};
            return ms_write(self, buf, 1);
        }
    }
    else {
        if (ux < (1<<7)) {
            char buf[1] = {(ux & 0xff)};
            return ms_write(self, buf, 1);
        } else {
            if(ux < (1<<16)) {
                if(ux < (1<<8)) {
                    char buf[2] = {MP_UINT8, (ux & 0xff)};
                    return ms_write(self, buf, 2);
                } else {
                    char buf[3];
                    buf[0] = MP_UINT16;
                    _msgspec_store16(&buf[1], (uint16_t)ux);
                    return ms_write(self, buf, 3);
                }
            } else {
                if(ux < (1LL<<32)) {
                    char buf[5];
                    buf[0] = MP_UINT32;
                    _msgspec_store32(&buf[1], (uint32_t)ux);
                    return ms_write(self, buf, 5);
                } else {
                    char buf[9];
                    buf[0] = MP_UINT64;
                    _msgspec_store64(&buf[1], ux);
                    return ms_write(self, buf, 9);
                }
            }
        }
    }
}

static MS_NOINLINE int
mpack_encode_float(EncoderState *self, PyObject *obj)
{
    char buf[9];
    double x = PyFloat_AS_DOUBLE(obj);
    uint64_t ux = 0;
    memcpy(&ux, &x, sizeof(double));
    buf[0] = MP_FLOAT64;
    _msgspec_store64(&buf[1], ux);
    return ms_write(self, buf, 9);
}

static MS_NOINLINE int
mpack_encode_cstr(EncoderState *self, const char *buf, Py_ssize_t len)
{
    if (buf == NULL) {
        return -1;
    }
    if (len < 32) {
        char header[1] = {MP_FIXSTR | (uint8_t)len};
        if (ms_write(self, header, 1) < 0)
            return -1;
    } else if (len < (1 << 8)) {
        char header[2] = {MP_STR8, (uint8_t)len};
        if (ms_write(self, header, 2) < 0)
            return -1;
    } else if (len < (1 << 16)) {
        char header[3];
        header[0] = MP_STR16;
        _msgspec_store16(&header[1], (uint16_t)len);
        if (ms_write(self, header, 3) < 0)
            return -1;
    } else if (len < (1LL << 32)) {
        char header[5];
        header[0] = MP_STR32;
        _msgspec_store32(&header[1], (uint32_t)len);
        if (ms_write(self, header, 5) < 0)
            return -1;
    } else {
        PyErr_SetString(
            self->mod->EncodeError,
            "Can't encode strings longer than 2**32 - 1"
        );
        return -1;
    }
    return len > 0 ? ms_write(self, buf, len) : 0;
}

static MS_INLINE int
mpack_encode_str(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) return -1;
    return mpack_encode_cstr(self, buf, len);
}

static int
mpack_encode_bin(EncoderState *self, const char* buf, Py_ssize_t len) {
    if (buf == NULL) {
        return -1;
    }
    if (len < (1 << 8)) {
        char header[2] = {MP_BIN8, (uint8_t)len};
        if (ms_write(self, header, 2) < 0)
            return -1;
    } else if (len < (1 << 16)) {
        char header[3];
        header[0] = MP_BIN16;
        _msgspec_store16(&header[1], (uint16_t)len);
        if (ms_write(self, header, 3) < 0)
            return -1;
    } else if (len < (1LL << 32)) {
        char header[5];
        header[0] = MP_BIN32;
        _msgspec_store32(&header[1], (uint32_t)len);
        if (ms_write(self, header, 5) < 0)
            return -1;
    } else {
        PyErr_SetString(
            self->mod->EncodeError,
            "Can't encode bytes-like objects longer than 2**32 - 1"
        );
        return -1;
    }
    return len > 0 ? ms_write(self, buf, len) : 0;
}

static int
mpack_encode_bytes(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyBytes_GET_SIZE(obj);
    const char* buf = PyBytes_AS_STRING(obj);
    return mpack_encode_bin(self, buf, len);
}

static int
mpack_encode_bytearray(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyByteArray_GET_SIZE(obj);
    const char* buf = PyByteArray_AS_STRING(obj);
    return mpack_encode_bin(self, buf, len);
}

static int
mpack_encode_memoryview(EncoderState *self, PyObject *obj)
{
    int out;
    Py_buffer buffer;
    if (PyObject_GetBuffer(obj, &buffer, PyBUF_CONTIG_RO) < 0) return -1;
    out = mpack_encode_bin(self, buffer.buf, buffer.len);
    PyBuffer_Release(&buffer);
    return out;
}

static int
mpack_encode_raw(EncoderState *self, PyObject *obj)
{
    Raw *raw = (Raw *)obj;
    if (ms_ensure_space(self, raw->len) < 0) return -1;
    memcpy(self->output_buffer_raw + self->output_len, raw->buf, raw->len);
    self->output_len += raw->len;
    return 0;
}

static int
mpack_encode_array_header(EncoderState *self, Py_ssize_t len, const char* typname)
{
    if (len < 16) {
        char header[1] = {MP_FIXARRAY | len};
        if (ms_write(self, header, 1) < 0)
            return -1;
    } else if (len < (1 << 16)) {
        char header[3];
        header[0] = MP_ARRAY16;
        _msgspec_store16(&header[1], (uint16_t)len);
        if (ms_write(self, header, 3) < 0)
            return -1;
    } else if (len < (1LL << 32)) {
        char header[5];
        header[0] = MP_ARRAY32;
        _msgspec_store32(&header[1], (uint32_t)len);
        if (ms_write(self, header, 5) < 0)
            return -1;
    } else {
        PyErr_Format(
            self->mod->EncodeError,
            "Can't encode %s longer than 2**32 - 1",
            typname
        );
        return -1;
    }
    return 0;
}

static MS_NOINLINE int
mpack_encode_list(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = 0;

    len = PyList_GET_SIZE(obj);
    if (mpack_encode_array_header(self, len, "list") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    for (i = 0; i < len; i++) {
        if (mpack_encode_inline(self, PyList_GET_ITEM(obj, i)) < 0) {
            status = -1;
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_set(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len, ppos = 0;
    Py_hash_t hash;
    PyObject *item;
    int status = 0;

    len = PySet_GET_SIZE(obj);
    if (mpack_encode_array_header(self, len, "set") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    while (_PySet_NextEntry(obj, &ppos, &item, &hash)) {
        if (mpack_encode_inline(self, item) < 0) {
            status = -1;
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_tuple(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = 0;

    len = PyTuple_GET_SIZE(obj);
    if (mpack_encode_array_header(self, len, "tuples") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    for (i = 0; i < len; i++) {
        if (mpack_encode_inline(self, PyTuple_GET_ITEM(obj, i)) < 0) {
            status = -1;
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_map_header(EncoderState *self, Py_ssize_t len, const char* typname)
{
    if (len < 16) {
        char header[1] = {MP_FIXMAP | len};
        if (ms_write(self, header, 1) < 0)
            return -1;
    } else if (len < (1 << 16)) {
        char header[3];
        header[0] = MP_MAP16;
        _msgspec_store16(&header[1], (uint16_t)len);
        if (ms_write(self, header, 3) < 0)
            return -1;
    } else if (len < (1LL << 32)) {
        char header[5];
        header[0] = MP_MAP32;
        _msgspec_store32(&header[1], (uint32_t)len);
        if (ms_write(self, header, 5) < 0)
            return -1;
    } else {
        PyErr_Format(
            self->mod->EncodeError,
            "Can't encode %s longer than 2**32 - 1",
            typname
        );
        return -1;
    }
    return 0;
}

static MS_NOINLINE int
mpack_encode_dict(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val;
    Py_ssize_t len, pos = 0;
    int status = -1;

    len = PyDict_GET_SIZE(obj);
    if (mpack_encode_map_header(self, len, "dicts") < 0) return -1;
    if (len == 0) return 0;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    while (PyDict_Next(obj, &pos, &key, &val)) {
        if (mpack_encode_inline(self, key) < 0) goto error;
        if (mpack_encode_inline(self, val) < 0) goto error;
    }
    status = 0;
error:
    Py_LeaveRecursiveCall();
    return status;
}

/* This method encodes an object as a map, with fields taken from `__dict__`,
 * followed by all `__slots__` in the class hierarchy. Any unset slots are
 * ignored, and `__weakref__` is not included. */
static int
mpack_encode_object(EncoderState *self, PyObject *obj)
{
    int status = -1;
    Py_ssize_t size = 0, max_size;

    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;

    /* Calculate the maximum number of fields that could be part of this object.
     * This is roughly equal to:
     *     max_size = size = len(getattr(obj, '__dict__', {}))
     *     max_size += sum(len(getattr(c, '__slots__', ())) for c in type(obj).mro())
     */
    PyObject *dict = PyObject_GenericGetDict(obj, NULL);
    if (MS_UNLIKELY(dict == NULL)) {
        PyErr_Clear();
        max_size = 0;
    }
    else {
        max_size = PyDict_GET_SIZE(dict);
    }

    PyTypeObject *type = Py_TYPE(obj);
    while (type != NULL) {
        max_size += Py_SIZE(type);
        type = type->tp_base;
    }
    /* Cache header offset in case we need to adjust the header after writing */
    Py_ssize_t header_offset = self->output_len;
    if (mpack_encode_map_header(self, max_size, "objects") < 0) goto cleanup;

    /* First encode everything in `__dict__` */
    if (dict != NULL) {
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(dict, &pos, &key, &val)) {
            if (MS_LIKELY(PyUnicode_CheckExact(key))) {
                Py_ssize_t key_len;
                const char* key_buf = unicode_str_and_size(key, &key_len);
                if (MS_UNLIKELY(key_buf == NULL)) goto cleanup;
                if (MS_UNLIKELY(*key_buf == '_')) continue;
                if (MS_UNLIKELY(mpack_encode_cstr(self, key_buf, key_len) < 0)) goto cleanup;
                if (MS_UNLIKELY(mpack_encode(self, val) < 0)) goto cleanup;
                size++;
            }
        }
    }
    /* Then encode everything in slots */
    type = Py_TYPE(obj);
    while (type != NULL) {
        Py_ssize_t n = Py_SIZE(type);
        if (n) {
            PyMemberDef *mp = MS_PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
            for (Py_ssize_t i = 0; i < n; i++, mp++) {
                if (MS_LIKELY(mp->type == T_OBJECT_EX && !(mp->flags & READONLY))) {
                    char *addr = (char *)obj + mp->offset;
                    PyObject *val = *(PyObject **)addr;
                    if (MS_UNLIKELY(val == NULL)) continue;
                    if (MS_UNLIKELY(*mp->name == '_')) continue;
                    if (MS_UNLIKELY(mpack_encode_cstr(self, mp->name, strlen(mp->name)) < 0)) goto cleanup;
                    if (MS_UNLIKELY(mpack_encode(self, val) < 0)) goto cleanup;
                    size++;
                }
            }
        }
        type = type->tp_base;
    }
    if (MS_UNLIKELY(size != max_size)) {
        /* Some fields were NULL, need to adjust header. We write the header
         * using the width type of `max_size`, but the value of `size`. */
        char *header_loc = self->output_buffer_raw + header_offset;
        if (max_size < 16) {
            *header_loc = MP_FIXMAP | size;
        } else if (max_size < (1 << 16)) {
            *header_loc++ = MP_MAP16;
            _msgspec_store16(header_loc, (uint16_t)size);
        } else {
            *header_loc++ = MP_MAP32;
            _msgspec_store32(header_loc, (uint32_t)size);
        }
    }
    status = 0;
cleanup:
    Py_XDECREF(dict);
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_struct(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val, *fields, *tag_field, *tag_value;
    Py_ssize_t i, nfields, len;
    int tagged, status = -1;
    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(obj);

    tag_field = struct_type->struct_tag_field;
    tag_value = struct_type->struct_tag_value;
    tagged = tag_value != NULL;
    fields = struct_type->struct_encode_fields;
    nfields = PyTuple_GET_SIZE(fields);
    len = nfields + tagged;

    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;

    if (struct_type->array_like == OPT_TRUE) {
        if (mpack_encode_array_header(self, len, "structs") < 0) goto cleanup;
        if (tagged) {
            if (mpack_encode(self, tag_value) < 0) goto cleanup;
        }
        for (i = 0; i < nfields; i++) {
            val = Struct_get_index(obj, i);
            if (val == NULL || mpack_encode(self, val) < 0) {
                goto cleanup;
            }
        }
    }
    else {
        Py_ssize_t header_offset = self->output_len;
        if (mpack_encode_map_header(self, len, "structs") < 0) goto cleanup;

        if (tagged) {
            if (mpack_encode_str(self, tag_field) < 0) goto cleanup;
            if (mpack_encode(self, tag_value) < 0) goto cleanup;
        }

        if (struct_type->omit_defaults == OPT_TRUE) {
            Py_ssize_t nunchecked = nfields - PyTuple_GET_SIZE(struct_type->struct_defaults);
            Py_ssize_t actual_len = len;

            for (i = 0; i < nunchecked; i++) {
                key = PyTuple_GET_ITEM(fields, i);
                val = Struct_get_index(obj, i);
                if (val == NULL || mpack_encode_str(self, key) < 0 || mpack_encode(self, val) < 0) {
                    goto cleanup;
                }
            }
            for (i = nunchecked; i < nfields; i++) {
                key = PyTuple_GET_ITEM(fields, i);
                val = Struct_get_index(obj, i);
                if (val == NULL) goto cleanup;
                PyObject *default_val = PyTuple_GET_ITEM(
                    struct_type->struct_defaults, i - nunchecked
                );
                if (!is_default(val, default_val)) {
                    if (mpack_encode_str(self, key) < 0 || mpack_encode(self, val) < 0) goto cleanup;
                }
                else {
                    actual_len--;
                }
            }
            if (actual_len != len) {
                /* Fixup the header length after we know how many fields were
                 * actually written */
                char *header_loc = self->output_buffer_raw + header_offset;
                if (len < 16) {
                    *header_loc = MP_FIXMAP | actual_len;
                } else if (len < (1 << 16)) {
                    *header_loc++ = MP_MAP16;
                    _msgspec_store16(header_loc, (uint16_t)actual_len);
                } else {
                    *header_loc++ = MP_MAP32;
                    _msgspec_store32(header_loc, (uint32_t)actual_len);
                }
            }
        }
        else {
            for (i = 0; i < nfields; i++) {
                key = PyTuple_GET_ITEM(fields, i);
                val = Struct_get_index(obj, i);
                if (val == NULL || mpack_encode_str(self, key) < 0 || mpack_encode(self, val) < 0) {
                    goto cleanup;
                }
            }
        }
    }

    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_ext(EncoderState *self, PyObject *obj)
{
    Ext *ex = (Ext *)obj;
    Py_ssize_t len;
    int status = -1, header_len = 2;
    char header[6];
    const char* data;
    Py_buffer buffer;
    buffer.buf = NULL;

    if (PyBytes_CheckExact(ex->data)) {
        len = PyBytes_GET_SIZE(ex->data);
        data = PyBytes_AS_STRING(ex->data);
    }
    else if (PyByteArray_CheckExact(ex->data)) {
        len = PyByteArray_GET_SIZE(ex->data);
        data = PyByteArray_AS_STRING(ex->data);
    }
    else {
        if (PyObject_GetBuffer(ex->data, &buffer, PyBUF_CONTIG_RO) < 0)
            return -1;
        len = buffer.len;
        data = buffer.buf;
    }
    if (len == 1) {
        header[0] = MP_FIXEXT1;
        header[1] = ex->code;
    }
    else if (len == 2) {
        header[0] = MP_FIXEXT2;
        header[1] = ex->code;
    }
    else if (len == 4) {
        header[0] = MP_FIXEXT4;
        header[1] = ex->code;
    }
    else if (len == 8) {
        header[0] = MP_FIXEXT8;
        header[1] = ex->code;
    }
    else if (len == 16) {
        header[0] = MP_FIXEXT16;
        header[1] = ex->code;
    }
    else if (len < (1<<8)) {
        header[0] = MP_EXT8;
        header[1] = len;
        header[2] = ex->code;
        header_len = 3;
    }
    else if (len < (1<<16)) {
        header[0] = MP_EXT16;
        _msgspec_store16(&header[1], (uint16_t)len);
        header[3] = ex->code;
        header_len = 4;
    }
    else if (len < (1LL<<32)) {
        header[0] = MP_EXT32;
        _msgspec_store32(&header[1], (uint32_t)len);
        header[5] = ex->code;
        header_len = 6;
    }
    else {
        PyErr_SetString(
            self->mod->EncodeError,
            "Can't encode Ext objects with data longer than 2**32 - 1"
        );
        goto done;
    }
    if (ms_write(self, header, header_len) < 0)
        goto done;
    status = len > 0 ? ms_write(self, data, len) : 0;
done:
    if (buffer.buf != NULL)
        PyBuffer_Release(&buffer);
    return status;
}

static int
mpack_encode_enum(EncoderState *self, PyObject *obj)
{
    if (PyLong_Check(obj))
        return mpack_encode_long(self, obj);
    if (PyUnicode_Check(obj))
        return mpack_encode_str(self, obj);

    int status;
    PyObject *value = PyObject_GetAttr(obj, self->mod->str__value_);
    if (value == NULL) return -1;
    if (PyLong_CheckExact(value)) {
        status = mpack_encode_long(self, value);
    }
    else if (PyUnicode_CheckExact(value)) {
        status = mpack_encode_str(self, value);
    }
    else {
        PyErr_SetString(
            self->mod->EncodeError,
            "Only enums with int or str values are supported"
        );
        status = -1;
    }
    Py_DECREF(value);
    return status;
}

static int
mpack_encode_uuid(EncoderState *self, PyObject *obj)
{
    char buf[36];
    if (ms_encode_uuid(self->mod, obj, buf) < 0) return -1;
    return mpack_encode_cstr(self, buf, 36);
}

static int
mpack_encode_decimal(EncoderState *self, PyObject *obj)
{
    PyObject *str = PyObject_Str(obj);
    if (str == NULL) return -1;
    int out = mpack_encode_str(self, str);
    Py_DECREF(str);
    return out;
}

static int
mpack_encode_date(EncoderState *self, PyObject *obj)
{
    char buf[10];
    ms_encode_date(obj, buf);
    return mpack_encode_cstr(self, buf, 10);
}

static int
mpack_encode_time(EncoderState *self, PyObject *obj)
{
    char buf[21];
    int size = ms_encode_time(self->mod, obj, buf);
    if (size < 0) return -1;
    return mpack_encode_cstr(self, buf, size);
}

static int
mpack_encode_datetime(EncoderState *self, PyObject *obj)
{
    int64_t seconds;
    int32_t nanoseconds;
    PyObject *tzinfo = MS_DATE_GET_TZINFO(obj);

    if (tzinfo == Py_None) {
        char buf[32];
        int size = ms_encode_datetime(self->mod, obj, buf);
        if (size < 0) return -1;
        return mpack_encode_cstr(self, buf, size);
    }

    if (tzinfo == PyDateTime_TimeZone_UTC) {
        datetime_to_epoch(obj, &seconds, &nanoseconds);
    }
    else {
        PyObject *temp = PyObject_CallFunctionObjArgs(
            self->mod->astimezone,
            obj, PyDateTime_TimeZone_UTC, NULL
        );
        if (temp == NULL) return -1;
        datetime_to_epoch(temp, &seconds, &nanoseconds);
        Py_DECREF(temp);
    }

    if ((seconds >> 34) == 0) {
        uint64_t data64 = ((uint64_t)nanoseconds << 34) | (uint64_t)seconds;
        if ((data64 & 0xffffffff00000000L) == 0) {
            /* timestamp 32 */
            char buf[6];
            buf[0] = MP_FIXEXT4;
            buf[1] = -1;
            uint32_t data32 = (uint32_t)data64;
            _msgspec_store32(&buf[2], data32);
            if (ms_write(self, buf, 6) < 0) return -1;
        } else {
            /* timestamp 64 */
            char buf[10];
            buf[0] = MP_FIXEXT8;
            buf[1] = -1;
            _msgspec_store64(&buf[2], data64);
            if (ms_write(self, buf, 10) < 0) return -1;
        }
    } else {
        /* timestamp 96 */
        char buf[15];
        buf[0] = MP_EXT8;
        buf[1] = 12;
        buf[2] = -1;
        _msgspec_store32(&buf[3], nanoseconds);
        _msgspec_store64(&buf[7], seconds);
        if (ms_write(self, buf, 15) < 0) return -1;
    }
    return 0;
}

static MS_NOINLINE int
mpack_encode_uncommon(EncoderState *self, PyTypeObject *type, PyObject *obj)
{
    if (obj == Py_None) {
        return mpack_encode_none(self);
    }
    else if (type == &PyBool_Type) {
        return mpack_encode_bool(self, obj);
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        return mpack_encode_struct(self, obj);
    }
    else if (type == &PyBytes_Type) {
        return mpack_encode_bytes(self, obj);
    }
    else if (type == &PyByteArray_Type) {
        return mpack_encode_bytearray(self, obj);
    }
    else if (type == &PyMemoryView_Type) {
        return mpack_encode_memoryview(self, obj);
    }
    else if (PyTuple_Check(obj)) {
        return mpack_encode_tuple(self, obj);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return mpack_encode_datetime(self, obj);
    }
    else if (type == PyDateTimeAPI->DateType) {
        return mpack_encode_date(self, obj);
    }
    else if (type == PyDateTimeAPI->TimeType) {
        return mpack_encode_time(self, obj);
    }
    else if (type == &Ext_Type) {
        return mpack_encode_ext(self, obj);
    }
    else if (type == &Raw_Type) {
        return mpack_encode_raw(self, obj);
    }
    else if (Py_TYPE(type) == self->mod->EnumMetaType) {
        return mpack_encode_enum(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->UUIDType)) {
        return mpack_encode_uuid(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->DecimalType)) {
        return mpack_encode_decimal(self, obj);
    }
    else if (PyAnySet_Check(obj)) {
        return mpack_encode_set(self, obj);
    }
    else if (PyDict_Contains(type->tp_dict, self->mod->str___dataclass_fields__)) {
        return mpack_encode_object(self, obj);
    }

    if (self->enc_hook != NULL) {
        int status = -1;
        PyObject *temp;
        temp = CALL_ONE_ARG(self->enc_hook, obj);
        if (temp == NULL) return -1;
        if (!Py_EnterRecursiveCall(" while serializing an object")) {
            status = mpack_encode(self, temp);
            Py_LeaveRecursiveCall();
        }
        Py_DECREF(temp);
        return status;
    }
    return ms_encode_err_type_unsupported(type);
}

static MS_INLINE int
mpack_encode_inline(EncoderState *self, PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);

    if (type == &PyUnicode_Type) {
        return mpack_encode_str(self, obj);
    }
    else if (type == &PyLong_Type) {
        return mpack_encode_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return mpack_encode_float(self, obj);
    }
    else if (PyList_Check(obj)) {
        return mpack_encode_list(self, obj);
    }
    else if (PyDict_Check(obj)) {
        return mpack_encode_dict(self, obj);
    }
    else {
        return mpack_encode_uncommon(self, type, obj);
    }
}

static int
mpack_encode(EncoderState *self, PyObject *obj) {
    return mpack_encode_inline(self, obj);
}

static PyObject*
Encoder_encode_into(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_into_common(&(self->state), args, nargs, &mpack_encode);
}

static PyObject*
Encoder_encode(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_common(&(self->state), args, nargs, &mpack_encode);
}

static struct PyMethodDef Encoder_methods[] = {
    {
        "encode", (PyCFunction) Encoder_encode, METH_FASTCALL,
        Encoder_encode__doc__,
    },
    {
        "encode_into", (PyCFunction) Encoder_encode_into, METH_FASTCALL,
        Encoder_encode_into__doc__,
    },
    {
        "__sizeof__", (PyCFunction) Encoder_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static PyTypeObject Encoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.msgpack.Encoder",
    .tp_doc = Encoder__doc__,
    .tp_basicsize = sizeof(Encoder),
    .tp_dealloc = (destructor)Encoder_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Encoder_traverse,
    .tp_clear = (inquiry)Encoder_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Encoder_init,
    .tp_methods = Encoder_methods,
    .tp_members = Encoder_members,
};

PyDoc_STRVAR(msgspec_msgpack_encode__doc__,
"msgpack_encode(obj, *, enc_hook=None)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object.\n"
"\n"
"See Also\n"
"--------\n"
"Encoder.encode"
);
static PyObject*
msgspec_msgpack_encode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    return encode_common(args, nargs, kwnames, &mpack_encode);
}

/*************************************************************************
 * JSON Encoder                                                          *
 *************************************************************************/

PyDoc_STRVAR(JSONEncoder__doc__,
"Encoder(*, enc_hook=None, write_buffer_size=512)\n"
"--\n"
"\n"
"A JSON encoder.\n"
"\n"
"Parameters\n"
"----------\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"write_buffer_size : int, optional\n"
"    The size of the internal static write buffer."
);

static int json_encode_inline(EncoderState*, PyObject*);
static int json_encode(EncoderState*, PyObject*);

static MS_INLINE int
json_encode_none(EncoderState *self)
{
    const char *buf = "null";
    return ms_write(self, buf, 4);
}

static MS_INLINE int
json_encode_true(EncoderState *self)
{
    const char *buf = "true";
    return ms_write(self, buf, 4);
}

static MS_INLINE int
json_encode_false(EncoderState *self)
{
    const char *buf = "false";
    return ms_write(self, buf, 5);
}

static MS_NOINLINE int
json_encode_long(EncoderState *self, PyObject *obj) {
    char buf[20];
    char *p = &buf[20];
    uint64_t x;
    bool neg, overflow;
    overflow = fast_long_extract_parts(obj, &neg, &x);
    if (MS_UNLIKELY(overflow)) {
        PyErr_SetString(
            PyExc_OverflowError,
            "can't serialize ints < -2**63 or > 2**64 - 1"
        );
        return -1;
    }
    while (x >= 100) {
        uint64_t const old = x;
        p -= 2;
        x /= 100;
        memcpy(p, DIGIT_TABLE + ((old - (x * 100)) << 1), 2);
    }
    if (x >= 10) {
        p -= 2;
        memcpy(p, DIGIT_TABLE + (x << 1), 2);
    }
    else {
        *--p = x + '0';
    }
    if (neg) {
        *--p = '-';
    }
    return ms_write(self, p, &buf[20] - p);
}

static int
json_encode_long_as_str(EncoderState *self, PyObject *obj) {
    if (ms_write(self, "\"", 1) < 0) return -1;
    if (json_encode_long(self, obj) < 0) return -1;
    return ms_write(self, "\"", 1);
}

static MS_NOINLINE int
json_encode_float(EncoderState *self, PyObject *obj) {
    char buf[24];
    double x = PyFloat_AS_DOUBLE(obj);
    int n = format_double(x, buf);
    return ms_write(self, buf, n);
}

static MS_INLINE int
json_encode_cstr(EncoderState *self, const char *str, Py_ssize_t size) {
    if (ms_ensure_space(self, size + 2) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p++ = '"';
    memcpy(p, str, size);
    *(p + size) = '"';
    self->output_len += size + 2;
    return 0;
}

static inline int
json_encode_str_nocheck(EncoderState *self, PyObject *obj) {
    Py_ssize_t len;
    const char* buf = unicode_str_and_size_nocheck(obj, &len);
    return json_encode_cstr(self, buf, len);
}

/* A table of escape characters to use for each byte (0 if no escape needed) */
static const char escape_table[256] = {
    'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'b', 't', 'n', 'u', 'f', 'r', 'u', 'u',
    'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u',
    0, 0, '"', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '\\', 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static int
json_str_requires_escaping(PyObject *obj) {
    Py_ssize_t i, len;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) return -1;
    for (i = 0; i < len; i++) {
        char escape = escape_table[(uint8_t)buf[i]];
        if (escape != 0) {
            return 1;
        }
    }
    return 0;
}

/* GCC generates better code if the uncommon path in the str encoding loop is
 * pulled out into a separate function. Clang generates the same code either
 * way. */
static MS_NOINLINE int
json_write_str_fragment(
    EncoderState *self, const char *buf, Py_ssize_t start, Py_ssize_t i, char c, char escape
) {
    if (MS_LIKELY(start < i)) {
        if (MS_UNLIKELY(ms_write(self, buf + start, i - start) < 0)) return -1;
    }

    /* Write the escaped character */
    char escaped[6] = {'\\', escape, '0', '0'};
    if (MS_UNLIKELY(escape == 'u')) {
        escaped[4] = hex_encode_table[c >> 4];
        escaped[5] = hex_encode_table[c & 0xF];
        if (MS_UNLIKELY(ms_write(self, escaped, 6) < 0)) return -1;
    }
    else {
        if (MS_UNLIKELY(ms_write(self, escaped, 2) < 0)) return -1;
    }
    return i + 1;
}

static MS_NOINLINE int
json_encode_str(EncoderState *self, PyObject *obj) {
    Py_ssize_t i, len, start = 0;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) return -1;

    if (ms_write(self, "\"", 1) < 0) return -1;

    for (i = 0; i < len; i++) {
        /* Scan through until a character needs to be escaped */
        char c = buf[i];
        char escape = escape_table[(uint8_t)c];
        if (MS_UNLIKELY(escape != 0)) {
            if (MS_UNLIKELY((start = json_write_str_fragment(self, buf, start, i, c, escape)) < 0)) {
                return -1;
            }
        }
    }
    /* Write the last unescaped fragment (if any) */
    if (start != len) {
        if (ms_write(self, buf + start, i - start) < 0) return -1;
    }

    return ms_write(self, "\"", 1);
}

static int
json_encode_bin(EncoderState *self, const char* buf, Py_ssize_t len) {
    /* Preallocate the buffer (ceil(4/3 * len) + 2) */
    Py_ssize_t encoded_len = ms_encode_base64_size(self->mod, len);
    if (encoded_len < 0) return -1;
    if (ms_ensure_space(self, encoded_len + 2) < 0) return -1;

    /* Write to the buffer directly */
    char *out = self->output_buffer_raw + self->output_len;

    *out++ = '"';
    ms_encode_base64(buf, len, out);
    out += encoded_len;
    *out++ = '"';
    self->output_len += encoded_len + 2;
    return 0;
}

static int
json_encode_bytes(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyBytes_GET_SIZE(obj);
    const char* buf = PyBytes_AS_STRING(obj);
    return json_encode_bin(self, buf, len);
}

static int
json_encode_bytearray(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyByteArray_GET_SIZE(obj);
    const char* buf = PyByteArray_AS_STRING(obj);
    return json_encode_bin(self, buf, len);
}

static int
json_encode_memoryview(EncoderState *self, PyObject *obj)
{
    int out;
    Py_buffer buffer;
    if (PyObject_GetBuffer(obj, &buffer, PyBUF_CONTIG_RO) < 0) return -1;
    out = json_encode_bin(self, buffer.buf, buffer.len);
    PyBuffer_Release(&buffer);
    return out;
}

static int
json_encode_raw(EncoderState *self, PyObject *obj)
{
    Raw *raw = (Raw *)obj;
    if (ms_ensure_space(self, raw->len) < 0) return -1;
    memcpy(self->output_buffer_raw + self->output_len, raw->buf, raw->len);
    self->output_len += raw->len;
    return 0;
}

static int
json_encode_enum(EncoderState *self, PyObject *obj, bool is_key)
{
    if (PyLong_Check(obj)) {
        if (MS_UNLIKELY(is_key)) {
            return json_encode_long_as_str(self, obj);
        }
        return json_encode_long(self, obj);
    }
    if (PyUnicode_Check(obj)) {
        return json_encode_str(self, obj);
    }

    int status;
    PyObject *value = PyObject_GetAttr(obj, self->mod->str__value_);
    if (value == NULL) return -1;
    if (PyLong_CheckExact(value)) {
        if (MS_UNLIKELY(is_key)) {
            status = json_encode_long_as_str(self, value);
        }
        else {
            status = json_encode_long(self, value);
        }
    }
    else if (PyUnicode_CheckExact(value)) {
        status = json_encode_str(self, value);
    }
    else {
        PyErr_SetString(
            self->mod->EncodeError,
            "Only enums with int or str values are supported"
        );
        status = -1;
    }
    Py_DECREF(value);
    return status;
}

static int
json_encode_uuid(EncoderState *self, PyObject *obj)
{
    char buf[38];
    buf[0] = '"';
    buf[37] = '"';
    if (ms_encode_uuid(self->mod, obj, buf + 1) < 0) return -1;
    return ms_write(self, buf, 38);
}

static int
json_encode_decimal(EncoderState *self, PyObject *obj)
{
    PyObject *str = PyObject_Str(obj);
    if (str == NULL) return -1;
    int out = json_encode_str(self, str);
    Py_DECREF(str);
    return out;
}

static int
json_encode_date(EncoderState *self, PyObject *obj)
{
    char buf[12];
    buf[0] = '"';
    buf[11] = '"';
    ms_encode_date(obj, buf + 1);
    return ms_write(self, buf, 12);
}

static int
json_encode_time(EncoderState *self, PyObject *obj)
{
    char buf[23];
    buf[0] = '"';
    int size = ms_encode_time(self->mod, obj, buf + 1);
    if (size < 0) return -1;
    buf[size + 1] = '"';
    return ms_write(self, buf, size + 2);
}

static int
json_encode_datetime(EncoderState *self, PyObject *obj)
{
    char buf[34];
    buf[0] = '"';
    int size = ms_encode_datetime(self->mod, obj, buf + 1);
    if (size < 0) return -1;
    buf[size + 1] = '"';
    return ms_write(self, buf, size + 2);
}

static MS_INLINE int
json_encode_sequence(EncoderState *self, Py_ssize_t size, PyObject **arr)
{
    int status = -1;

    if (size == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    for (Py_ssize_t i = 0; i < size; i++) {
        if (json_encode_inline(self, *(arr + i)) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static MS_NOINLINE int
json_encode_list(EncoderState *self, PyObject *obj)
{
    return json_encode_sequence(
        self, PyList_GET_SIZE(obj), ((PyListObject *)obj)->ob_item
    );
}

static MS_NOINLINE int
json_encode_tuple(EncoderState *self, PyObject *obj)
{
    return json_encode_sequence(
        self, PyTuple_GET_SIZE(obj), ((PyTupleObject *)obj)->ob_item
    );
}

static int
json_encode_set(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len, ppos = 0;
    Py_hash_t hash;
    PyObject *item;
    int status = -1;

    len = PySet_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    while (_PySet_NextEntry(obj, &ppos, &item, &hash)) {
        if (json_encode_inline(self, item) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static MS_NOINLINE int
json_encode_dict_key(EncoderState *self, PyObject *obj) {
    PyTypeObject *type = Py_TYPE(obj);

    /* PyUnicode_Type is handled inline in json_encode_dict */
    if (type == &PyLong_Type) {
        return json_encode_long_as_str(self, obj);
    }
    else if (Py_TYPE(type) == self->mod->EnumMetaType) {
        return json_encode_enum(self, obj, true);
    }
    else if (type == (PyTypeObject *)(self->mod->UUIDType)) {
        return json_encode_uuid(self, obj);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return json_encode_datetime(self, obj);
    }
    else if (type == PyDateTimeAPI->DateType) {
        return json_encode_date(self, obj);
    }
    else if (type == PyDateTimeAPI->TimeType) {
        return json_encode_time(self, obj);
    }
    else if (type == &PyBytes_Type) {
        return json_encode_bytes(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->DecimalType)) {
        return json_encode_decimal(self, obj);
    }
    else {
        PyErr_SetString(
            PyExc_TypeError,
            "Only dicts with str-like or int-like keys are supported"
        );
        return -1;
    }
}

static MS_NOINLINE int
json_encode_dict(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val;
    Py_ssize_t len, pos = 0;
    int status = -1;

    len = PyDict_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "{}", 2);
    if (ms_write(self, "{", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    while (PyDict_Next(obj, &pos, &key, &val)) {
        if (MS_LIKELY(PyUnicode_CheckExact(key))) {
            if (json_encode_str(self, key) < 0) goto cleanup;
        }
        else {
            if (json_encode_dict_key(self, key) < 0) goto cleanup;
        }
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (json_encode_inline(self, val) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with } */
    *(self->output_buffer_raw + self->output_len - 1) = '}';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}


/* This method encodes an object as a map, with fields taken from `__dict__`,
 * followed by all `__slots__` in the class hierarchy. Any unset slots are
 * ignored, and `__weakref__` is not included. */
static int
json_encode_object(EncoderState *self, PyObject *obj)
{
    int status = -1;
    if (ms_write(self, "{", 1) < 0) return -1;
    Py_ssize_t start_offset = self->output_len;

    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    /* First encode everything in `__dict__` */
    PyObject *dict = PyObject_GenericGetDict(obj, NULL);
    if (MS_UNLIKELY(dict == NULL)) {
        PyErr_Clear();
    }
    else {
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(dict, &pos, &key, &val)) {
            if (MS_LIKELY(PyUnicode_CheckExact(key))) {
                Py_ssize_t key_len;
                const char* key_buf = unicode_str_and_size(key, &key_len);
                if (MS_UNLIKELY(key_buf == NULL)) goto cleanup;
                if (MS_UNLIKELY(*key_buf == '_')) continue;
                if (MS_UNLIKELY(json_encode_cstr(self, key_buf, key_len) < 0)) goto cleanup;
                if (MS_UNLIKELY(ms_write(self, ":", 1) < 0)) goto cleanup;
                if (MS_UNLIKELY(json_encode(self, val) < 0)) goto cleanup;
                if (MS_UNLIKELY(ms_write(self, ",", 1) < 0)) goto cleanup;
            }
        }
    }
    /* Then encode everything in slots */
    PyTypeObject *type = Py_TYPE(obj);
    while (type != NULL) {
        Py_ssize_t n = Py_SIZE(type);
        if (n) {
            PyMemberDef *mp = MS_PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
            for (Py_ssize_t i = 0; i < n; i++, mp++) {
                if (MS_LIKELY(mp->type == T_OBJECT_EX && !(mp->flags & READONLY))) {
                    char *addr = (char *)obj + mp->offset;
                    PyObject *val = *(PyObject **)addr;
                    if (MS_UNLIKELY(val == NULL)) continue;
                    if (MS_UNLIKELY(*mp->name == '_')) continue;
                    if (MS_UNLIKELY(json_encode_cstr(self, mp->name, strlen(mp->name)) < 0)) goto cleanup;
                    if (MS_UNLIKELY(ms_write(self, ":", 1) < 0)) goto cleanup;
                    if (MS_UNLIKELY(json_encode(self, val) < 0)) goto cleanup;
                    if (MS_UNLIKELY(ms_write(self, ",", 1) < 0)) goto cleanup;
                }
            }
        }
        type = type->tp_base;
    }
    /* If any fields written, overwrite trailing comma with }, otherwise append } */
    if (MS_LIKELY(self->output_len != start_offset)) {
        *(self->output_buffer_raw + self->output_len - 1) = '}';
        status = 0;
    }
    else {
        status = ms_write(self, "}", 1);
    }
cleanup:
    Py_XDECREF(dict);
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_struct_tag(EncoderState *self, PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);

    if (type == &PyUnicode_Type) {
        return json_encode_str(self, obj);
    }
    else {
        return json_encode_long(self, obj);
    }
}

static int
json_encode_struct_default(
    EncoderState *self, StructMetaObject *struct_type, PyObject *obj
) {
    PyObject *key, *val, *fields, *tag_field, *tag_value;
    Py_ssize_t i, nfields;
    int status = -1;
    tag_field = struct_type->struct_tag_field;
    tag_value = struct_type->struct_tag_value;
    fields = struct_type->struct_encode_fields;
    nfields = PyTuple_GET_SIZE(fields);

    if (nfields == 0 && tag_value == NULL) return ms_write(self, "{}", 2);
    if (ms_write(self, "{", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    if (tag_value != NULL) {
        if (json_encode_str(self, tag_field) < 0) goto cleanup;
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (json_encode_struct_tag(self, tag_value) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    for (i = 0; i < nfields; i++) {
        key = PyTuple_GET_ITEM(fields, i);
        val = Struct_get_index(obj, i);
        if (val == NULL) goto cleanup;
        if (json_encode_str_nocheck(self, key) < 0) goto cleanup;
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (json_encode(self, val) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with } */
    *(self->output_buffer_raw + self->output_len - 1) = '}';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_struct_omit_defaults(
    EncoderState *self, StructMetaObject *struct_type, PyObject *obj
) {
    PyObject *key, *val, *fields, *defaults, *tag_field, *tag_value;
    Py_ssize_t i, nfields, nunchecked;
    int status = -1;
    tag_field = struct_type->struct_tag_field;
    tag_value = struct_type->struct_tag_value;
    fields = struct_type->struct_encode_fields;
    defaults = struct_type->struct_defaults;
    nfields = PyTuple_GET_SIZE(fields);
    nunchecked = nfields - PyTuple_GET_SIZE(defaults);

    if (ms_write(self, "{", 1) < 0) return -1;
    Py_ssize_t start_len = self->output_len;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    if (tag_value != NULL) {
        if (json_encode_str(self, tag_field) < 0) goto cleanup;
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (json_encode_struct_tag(self, tag_value) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }

    for (i = 0; i < nunchecked; i++) {
        key = PyTuple_GET_ITEM(fields, i);
        val = Struct_get_index(obj, i);
        if (val == NULL) goto cleanup;
        if (json_encode_str_nocheck(self, key) < 0) goto cleanup;
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (json_encode(self, val) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    for (i = nunchecked; i < nfields; i++) {
        key = PyTuple_GET_ITEM(fields, i);
        val = Struct_get_index(obj, i);
        if (val == NULL) goto cleanup;
        PyObject *default_val = PyTuple_GET_ITEM(defaults, i - nunchecked);
        if (!is_default(val, default_val)) {
            if (json_encode_str_nocheck(self, key) < 0) goto cleanup;
            if (ms_write(self, ":", 1) < 0) goto cleanup;
            if (json_encode(self, val) < 0) goto cleanup;
            if (ms_write(self, ",", 1) < 0) goto cleanup;
        }
    }
    if (MS_UNLIKELY(start_len == self->output_len)) {
        /* Empty struct, append "}" */
        if (ms_write(self, "}", 1) < 0) goto cleanup;
    }
    else {
        /* Overwrite trailing comma with } */
        *(self->output_buffer_raw + self->output_len - 1) = '}';
    }
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_struct_array_like(
    EncoderState *self, StructMetaObject *struct_type, PyObject *obj
) {
    int status = -1;
    PyObject *tag_value = struct_type->struct_tag_value;
    PyObject *fields = struct_type->struct_encode_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);

    if (nfields == 0 && tag_value == NULL) return ms_write(self, "[]", 2);
    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    if (tag_value != NULL) {
        if (json_encode_struct_tag(self, tag_value) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *val = Struct_get_index(obj, i);
        if (val == NULL) goto cleanup;
        if (json_encode(self, val) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_struct(EncoderState *self, PyObject *obj)
{
    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(obj);

    if (struct_type->array_like == OPT_TRUE) {
        return json_encode_struct_array_like(self, struct_type, obj);
    }
    else if (struct_type->omit_defaults == OPT_TRUE) {
        return json_encode_struct_omit_defaults(self, struct_type, obj);
    }
    else {
        return json_encode_struct_default(self, struct_type, obj);
    }
}

static MS_NOINLINE int
json_encode_uncommon(EncoderState *self, PyTypeObject *type, PyObject *obj) {
    if (obj == Py_None) {
        return json_encode_none(self);
    }
    else if (obj == Py_True) {
        return json_encode_true(self);
    }
    else if (obj == Py_False) {
        return json_encode_false(self);
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        return json_encode_struct(self, obj);
    }
    else if (PyTuple_Check(obj)) {
        return json_encode_tuple(self, obj);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return json_encode_datetime(self, obj);
    }
    else if (type == PyDateTimeAPI->DateType) {
        return json_encode_date(self, obj);
    }
    else if (type == PyDateTimeAPI->TimeType) {
        return json_encode_time(self, obj);
    }
    else if (type == &PyBytes_Type) {
        return json_encode_bytes(self, obj);
    }
    else if (type == &PyByteArray_Type) {
        return json_encode_bytearray(self, obj);
    }
    else if (type == &PyMemoryView_Type) {
        return json_encode_memoryview(self, obj);
    }
    else if (type == &Raw_Type) {
        return json_encode_raw(self, obj);
    }
    else if (Py_TYPE(type) == self->mod->EnumMetaType) {
        return json_encode_enum(self, obj, false);
    }
    else if (type == (PyTypeObject *)(self->mod->UUIDType)) {
        return json_encode_uuid(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->DecimalType)) {
        return json_encode_decimal(self, obj);
    }
    else if (PyAnySet_Check(obj)) {
        return json_encode_set(self, obj);
    }
    else if (PyDict_Contains(type->tp_dict, self->mod->str___dataclass_fields__)) {
        return json_encode_object(self, obj);
    }

    if (self->enc_hook != NULL) {
        int status = -1;
        PyObject *temp;
        temp = CALL_ONE_ARG(self->enc_hook, obj);
        if (temp == NULL) return -1;
        if (!Py_EnterRecursiveCall(" while serializing an object")) {
            status = json_encode(self, temp);
            Py_LeaveRecursiveCall();
        }
        Py_DECREF(temp);
        return status;
    }
    return ms_encode_err_type_unsupported(type);
}

static MS_INLINE int
json_encode_inline(EncoderState *self, PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);

    if (type == &PyUnicode_Type) {
        return json_encode_str(self, obj);
    }
    else if (type == &PyLong_Type) {
        return json_encode_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return json_encode_float(self, obj);
    }
    else if (PyList_Check(obj)) {
        return json_encode_list(self, obj);
    }
    else if (PyDict_Check(obj)) {
        return json_encode_dict(self, obj);
    }
    else {
        return json_encode_uncommon(self, type, obj);
    }
}

static int
json_encode(EncoderState *self, PyObject *obj)
{
    return json_encode_inline(self, obj);
}

static PyObject*
JSONEncoder_encode_into(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_into_common(&(self->state), args, nargs, &json_encode);
}

static PyObject*
JSONEncoder_encode(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_common(&(self->state), args, nargs, &json_encode);
}

static struct PyMethodDef JSONEncoder_methods[] = {
    {
        "encode", (PyCFunction) JSONEncoder_encode, METH_FASTCALL,
        Encoder_encode__doc__,
    },
    {
        "encode_into", (PyCFunction) JSONEncoder_encode_into, METH_FASTCALL,
        Encoder_encode_into__doc__,
    },
    {
        "__sizeof__", (PyCFunction) Encoder_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static PyTypeObject JSONEncoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.json.Encoder",
    .tp_doc = JSONEncoder__doc__,
    .tp_basicsize = sizeof(Encoder),
    .tp_dealloc = (destructor)Encoder_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Encoder_traverse,
    .tp_clear = (inquiry)Encoder_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Encoder_init,
    .tp_methods = JSONEncoder_methods,
    .tp_members = Encoder_members,
};

PyDoc_STRVAR(msgspec_json_encode__doc__,
"json_encode(obj, *, enc_hook=None)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object.\n"
"\n"
"See Also\n"
"--------\n"
"Encoder.encode"
);
static PyObject*
msgspec_json_encode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    return encode_common(args, nargs, kwnames, &json_encode);
}

/*************************************************************************
 * MessagePack Decoder                                                   *
 *************************************************************************/

typedef struct DecoderState {
    /* Configuration */
    TypeNode *type;
    PyObject *dec_hook;
    PyObject *ext_hook;

    /* Per-message attributes */
    PyObject *buffer_obj;
    char *input_start;
    char *input_pos;
    char *input_end;
} DecoderState;

typedef struct Decoder {
    PyObject_HEAD
    PyObject *orig_type;
    DecoderState state;
} Decoder;

PyDoc_STRVAR(Decoder__doc__,
"Decoder(type='Any', *, dec_hook=None, ext_hook=None)\n"
"--\n"
"\n"
"A MessagePack decoder.\n"
"\n"
"Parameters\n"
"----------\n"
"type : type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default MessagePack types.\n"
"dec_hook : callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic MessagePack types. This hook should transform ``obj`` into\n"
"    type ``type``, or raise a ``TypeError`` if unsupported.\n"
"ext_hook : callable, optional\n"
"    An optional callback for decoding MessagePack extensions. Should have the\n"
"    signature ``ext_hook(code: int, data: memoryview) -> Any``. If provided,\n"
"    this will be called to deserialize all extension types found in the\n"
"    message. Note that ``data`` is a memoryview into the larger message\n"
"    buffer - any references created to the underlying buffer without copying\n"
"    the data out will cause the full message buffer to persist in memory.\n"
"    If not provided, extension types will decode as ``msgspec.Ext`` objects."
);
static int
Decoder_init(Decoder *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"type", "dec_hook", "ext_hook", NULL};
    MsgspecState *st = msgspec_get_global_state();
    PyObject *type = st->typing_any;
    PyObject *ext_hook = NULL;
    PyObject *dec_hook = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|O$OO", kwlist, &type, &dec_hook, &ext_hook
        )) {
        return -1;
    }

    /* Handle dec_hook */
    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    if (dec_hook != NULL) {
        if (!PyCallable_Check(dec_hook)) {
            PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
            return -1;
        }
        Py_INCREF(dec_hook);
    }
    self->state.dec_hook = dec_hook;

    /* Handle ext_hook */
    if (ext_hook == Py_None) {
        ext_hook = NULL;
    }
    if (ext_hook != NULL) {
        if (!PyCallable_Check(ext_hook)) {
            PyErr_SetString(PyExc_TypeError, "ext_hook must be callable");
            return -1;
        }
        Py_INCREF(ext_hook);
    }
    self->state.ext_hook = ext_hook;

    /* Handle type */
    self->state.type = TypeNode_Convert(type, false, NULL);
    if (self->state.type == NULL) {
        return -1;
    }
    Py_INCREF(type);
    self->orig_type = type;
    return 0;
}

static int
Decoder_traverse(Decoder *self, visitproc visit, void *arg)
{
    int out = TypeNode_traverse(self->state.type, visit, arg);
    if (out != 0) return out;
    Py_VISIT(self->orig_type);
    Py_VISIT(self->state.dec_hook);
    Py_VISIT(self->state.ext_hook);
    return 0;
}

static void
Decoder_dealloc(Decoder *self)
{
    PyObject_GC_UnTrack(self);
    TypeNode_Free(self->state.type);
    Py_XDECREF(self->orig_type);
    Py_XDECREF(self->state.dec_hook);
    Py_XDECREF(self->state.ext_hook);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
Decoder_repr(Decoder *self) {
    int recursive;
    PyObject *typstr, *out = NULL;

    recursive = Py_ReprEnter((PyObject *)self);
    if (recursive != 0) {
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");  /* cpylint-ignore */
    }
    typstr = PyObject_Repr(self->orig_type);
    if (typstr != NULL) {
        out = PyUnicode_FromFormat("msgspec.msgpack.Decoder(%U)", typstr);
    }
    Py_XDECREF(typstr);
    Py_ReprLeave((PyObject *)self);
    return out;
}

static MS_INLINE int
mpack_read1(DecoderState *self, char *s)
{
    if (MS_UNLIKELY(self->input_pos == self->input_end)) {
        return ms_err_truncated();
    }
    *s = *self->input_pos++;
    return 0;
}

static MS_INLINE int
mpack_read(DecoderState *self, char **s, Py_ssize_t n)
{
    if (MS_LIKELY(n <= self->input_end - self->input_pos)) {
        *s = self->input_pos;
        self->input_pos += n;
        return 0;
    }
    return ms_err_truncated();
}

static MS_INLINE bool
mpack_has_trailing_characters(DecoderState *self)
{
    if (self->input_pos != self->input_end) {
        PyErr_Format(
            msgspec_get_global_state()->DecodeError,
            "MessagePack data is malformed: trailing characters (byte %zd)",
            (Py_ssize_t)(self->input_pos - self->input_start)
        );
        return true;
    }
    return false;
}

static MS_INLINE Py_ssize_t
mpack_decode_size1(DecoderState *self) {
    char s = 0;
    if (mpack_read1(self, &s) < 0) return -1;
    return (Py_ssize_t)((unsigned char)s);
}

static MS_INLINE Py_ssize_t
mpack_decode_size2(DecoderState *self) {
    char *s = NULL;
    if (mpack_read(self, &s, 2) < 0) return -1;
    return (Py_ssize_t)(_msgspec_load16(uint16_t, s));
}

static MS_INLINE Py_ssize_t
mpack_decode_size4(DecoderState *self) {
    char *s = NULL;
    if (mpack_read(self, &s, 4) < 0) return -1;
    return (Py_ssize_t)(_msgspec_load32(uint32_t, s));
}

static PyObject *
mpack_error_expected(char op, char *expected, PathNode *path) {
    char *got;
    if (('\x00' <= op && op <= '\x7f') || ('\xe0' <= op && op <= '\xff')) {
        got = "int";
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        got = "str";
    }
    else if ('\x90' <= op && op <= '\x9f') {
        got = "array";
    }
    else if ('\x80' <= op && op <= '\x8f') {
        got = "object";
    }
    else {
        switch ((enum mpack_code)op) {
            case MP_NIL:
                got = "null";
                break;
            case MP_TRUE:
            case MP_FALSE:
                got = "bool";
                break;
            case MP_UINT8:
            case MP_UINT16:
            case MP_UINT32:
            case MP_UINT64:
            case MP_INT8:
            case MP_INT16:
            case MP_INT32:
            case MP_INT64:
                got = "int";
                break;
            case MP_FLOAT32:
            case MP_FLOAT64:
                got = "float";
                break;
            case MP_STR8:
            case MP_STR16:
            case MP_STR32:
                got = "str";
                break;
            case MP_BIN8:
            case MP_BIN16:
            case MP_BIN32:
                got = "bytes";
                break;
            case MP_ARRAY16:
            case MP_ARRAY32:
                got = "array";
                break;
            case MP_MAP16:
            case MP_MAP32:
                got = "object";
                break;
            case MP_FIXEXT1:
            case MP_FIXEXT2:
            case MP_FIXEXT4:
            case MP_FIXEXT8:
            case MP_FIXEXT16:
            case MP_EXT8:
            case MP_EXT16:
            case MP_EXT32:
                got = "ext";
                break;
            default:
                got = "unknown";
                break;
        }
    }
    ms_raise_validation_error(path, "Expected `%s`, got `%s`%U", expected, got);
    return NULL;
}

static MS_INLINE Py_ssize_t
mpack_decode_cstr(DecoderState *self, char ** out, PathNode *path) {
    char op = 0;
    Py_ssize_t size;
    if (mpack_read1(self, &op) < 0) return -1;

    if ('\xa0' <= op && op <= '\xbf') {
        size = op & 0x1f;
    }
    else if (op == MP_STR8) {
        size = mpack_decode_size1(self);
    }
    else if (op == MP_STR16) {
        size = mpack_decode_size2(self);
    }
    else if (op == MP_STR32) {
        size = mpack_decode_size4(self);
    }
    else {
        mpack_error_expected(op, "str", path);
        return -1;
    }

    if (mpack_read(self, out, size) < 0) return -1;
    return size;
}

/* Decode an integer. If the value fits in an int64_t, it will be stored in
 * `out`, otherwise it will be stored in `uout`. A return value of -1 indicates
 * an error. */
static int
mpack_decode_cint(DecoderState *self, int64_t *out, uint64_t *uout, PathNode *path) {
    char op = 0;
    char *s = NULL;

    if (mpack_read1(self, &op) < 0) return -1;

    if (('\x00' <= op && op <= '\x7f') || ('\xe0' <= op && op <= '\xff')) {
        *out = *((int8_t *)(&op));
    }
    else if (op == MP_UINT8) {
        if (MS_UNLIKELY(mpack_read(self, &s, 1) < 0)) return -1;
        *out = *(uint8_t *)s;
    }
    else if (op == MP_UINT16) {
        if (MS_UNLIKELY(mpack_read(self, &s, 2) < 0)) return -1;
        *out = _msgspec_load16(uint16_t, s);
    }
    else if (op == MP_UINT32) {
        if (MS_UNLIKELY(mpack_read(self, &s, 4) < 0)) return -1;
        *out = _msgspec_load32(uint32_t, s);
    }
    else if (op == MP_UINT64) {
        if (MS_UNLIKELY(mpack_read(self, &s, 8) < 0)) return -1;
        uint64_t ux = _msgspec_load64(uint64_t, s);
        if (ux > LLONG_MAX) {
            *uout = ux;
        }
        else {
            *out = ux;
        }
    }
    else if (op == MP_INT8) {
        if (MS_UNLIKELY(mpack_read(self, &s, 1) < 0)) return -1;
        *out = *(int8_t *)s;
    }
    else if (op == MP_INT16) {
        if (MS_UNLIKELY(mpack_read(self, &s, 2) < 0)) return -1;
        *out = _msgspec_load16(int16_t, s);
    }
    else if (op == MP_INT32) {
        if (MS_UNLIKELY(mpack_read(self, &s, 4) < 0)) return -1;
        *out = _msgspec_load32(int32_t, s);
    }
    else if (op == MP_INT64) {
        if (MS_UNLIKELY(mpack_read(self, &s, 8) < 0)) return -1;
        *out = _msgspec_load64(int64_t, s);
    }
    else {
        mpack_error_expected(op, "int", path);
        return -1;
    }
    return 0;
}


static PyObject *
mpack_decode_datetime(
    DecoderState *self, const char *data_buf, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    uint64_t data64;
    uint32_t nanoseconds;
    int64_t seconds;
    char *err_msg;

    switch (size) {
        case 4:
            seconds = _msgspec_load32(uint32_t, data_buf);
            nanoseconds = 0;
            break;
        case 8:
            data64 = _msgspec_load64(uint64_t, data_buf);
            seconds = data64 & 0x00000003ffffffffL;
            nanoseconds = data64 >> 34;
            break;
        case 12:
            nanoseconds = _msgspec_load32(uint32_t, data_buf);
            seconds = _msgspec_load64(uint64_t, data_buf + 4);
            break;
        default:
            err_msg = "Invalid MessagePack timestamp%U";
            goto invalid;
    }

    if (nanoseconds > 999999999) {
        err_msg = "Invalid MessagePack timestamp: nanoseconds out of range%U";
        goto invalid;
    }
    /* Error on out-of-bounds datetimes. This leaves ample space in an int, so
     * no need to check for overflow later. */
    if (seconds < MS_EPOCH_SECS_MIN || seconds > MS_EPOCH_SECS_MAX) {
        err_msg = "Timestamp is out of range%U";
        goto invalid;
    }
    return datetime_from_epoch(seconds, nanoseconds, type, path);

invalid:
    return ms_error_with_path(err_msg, path);
}

static int mpack_skip(DecoderState *self);

static int
mpack_skip_array(DecoderState *self, Py_ssize_t size) {
    int status = -1;
    Py_ssize_t i;
    if (size < 0) return -1;
    if (size == 0) return 0;

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    for (i = 0; i < size; i++) {
        if (mpack_skip(self) < 0) goto done;
    }
    status = 0;
done:
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_skip_map(DecoderState *self, Py_ssize_t size) {
    return mpack_skip_array(self, size * 2);
}

static int
mpack_skip_ext(DecoderState *self, Py_ssize_t size) {
    char *s;
    if (size < 0) return -1;
    return mpack_read(self, &s, size + 1);
}

static int
mpack_skip(DecoderState *self) {
    char *s = NULL;
    char op = 0;
    Py_ssize_t size;

    if (mpack_read1(self, &op) < 0) return -1;

    if (('\x00' <= op && op <= '\x7f') || ('\xe0' <= op && op <= '\xff')) {
        return 0;
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        return mpack_read(self, &s, op & 0x1f);
    }
    else if ('\x90' <= op && op <= '\x9f') {
        return mpack_skip_array(self, op & 0x0f);
    }
    else if ('\x80' <= op && op <= '\x8f') {
        return mpack_skip_map(self, op & 0x0f);
    }
    switch ((enum mpack_code)op) {
        case MP_NIL:
        case MP_TRUE:
        case MP_FALSE:
            return 0;
        case MP_UINT8:
        case MP_INT8:
            return mpack_read1(self, &op);
        case MP_UINT16:
        case MP_INT16:
            return mpack_read(self, &s, 2);
        case MP_UINT32:
        case MP_INT32:
        case MP_FLOAT32:
            return mpack_read(self, &s, 4);
        case MP_UINT64:
        case MP_INT64:
        case MP_FLOAT64:
            return mpack_read(self, &s, 8);
        case MP_STR8:
        case MP_BIN8:
            if ((size = mpack_decode_size1(self)) < 0) return -1;
            return mpack_read(self, &s, size);
        case MP_STR16:
        case MP_BIN16:
            if ((size = mpack_decode_size2(self)) < 0) return -1;
            return mpack_read(self, &s, size);
        case MP_STR32:
        case MP_BIN32:
            if ((size = mpack_decode_size4(self)) < 0) return -1;
            return mpack_read(self, &s, size);
        case MP_ARRAY16:
            return mpack_skip_array(self, mpack_decode_size2(self));
        case MP_ARRAY32:
            return mpack_skip_array(self, mpack_decode_size4(self));
        case MP_MAP16:
            return mpack_skip_map(self, mpack_decode_size2(self));
        case MP_MAP32:
            return mpack_skip_map(self, mpack_decode_size4(self));
        case MP_FIXEXT1:
            return mpack_skip_ext(self, 1);
        case MP_FIXEXT2:
            return mpack_skip_ext(self, 2);
        case MP_FIXEXT4:
            return mpack_skip_ext(self, 4);
        case MP_FIXEXT8:
            return mpack_skip_ext(self, 8);
        case MP_FIXEXT16:
            return mpack_skip_ext(self, 16);
        case MP_EXT8:
            return mpack_skip_ext(self, mpack_decode_size1(self));
        case MP_EXT16:
            return mpack_skip_ext(self, mpack_decode_size2(self));
        case MP_EXT32:
            return mpack_skip_ext(self, mpack_decode_size4(self));
        default:
            PyErr_Format(
                msgspec_get_global_state()->DecodeError,
                "MessagePack data is malformed: invalid opcode '\\x%02x' (byte %zd)",
                (unsigned char)op,
                (Py_ssize_t)(self->input_pos - self->input_start - 1)
            );
            return -1;
    }
}

static PyObject * mpack_decode(
    DecoderState *self, TypeNode *type, PathNode *path, bool is_key
);

static PyObject *
mpack_decode_int(DecoderState *self, int64_t x, TypeNode *type, PathNode *path) {
    if (type->types & (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        return ms_decode_int_enum_or_literal_int64(x, type, path);
    }
    else if (type->types & (MS_TYPE_ANY | MS_TYPE_INT)) {
        return ms_decode_int(x, type, path);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return ms_decode_float(x, type, path);
    }
    return ms_validation_error("int", type, path);
}

static PyObject *
mpack_decode_uint(DecoderState *self, uint64_t x, TypeNode *type, PathNode *path) {
    if (type->types & (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        return ms_decode_int_enum_or_literal_uint64(x, type, path);
    }
    else if (type->types & (MS_TYPE_ANY | MS_TYPE_INT)) {
        return ms_decode_uint(x, type, path);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return ms_decode_float(x, type, path);
    }
    return ms_validation_error("int", type, path);
}

static PyObject *
mpack_decode_none(DecoderState *self, TypeNode *type, PathNode *path) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_NONE)) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    return ms_validation_error("None", type, path);
}

static PyObject *
mpack_decode_bool(DecoderState *self, PyObject *val, TypeNode *type, PathNode *path) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(val);
        return val;
    }
    return ms_validation_error("bool", type, path);
}

static PyObject *
mpack_decode_float(DecoderState *self, double val, TypeNode *type, PathNode *path) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)) {
        return ms_decode_float(val, type, path);
    }
    return ms_validation_error("float", type, path);
}

static PyObject *
mpack_decode_str(DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_LIKELY(
            type->types & (
                MS_TYPE_ANY | MS_TYPE_STR | MS_TYPE_ENUM | MS_TYPE_STRLITERAL |
                MS_TYPE_DATETIME | MS_TYPE_DATE | MS_TYPE_TIME |
                MS_TYPE_UUID | MS_TYPE_DECIMAL
            )
        )
    ) {
        char *s = NULL;
        if (MS_UNLIKELY(mpack_read(self, &s, size) < 0)) return NULL;
        if (MS_UNLIKELY(type->types & (MS_TYPE_ENUM | MS_TYPE_STRLITERAL))) {
            return ms_decode_str_enum_or_literal(s, size, type, path);
        }
        if (MS_UNLIKELY(type->types & MS_TYPE_DATETIME)) {
            return ms_decode_datetime(s, size, type, path);
        }
        if (MS_UNLIKELY(type->types & MS_TYPE_DATE)) {
            return ms_decode_date(s, size, path);
        }
        if (MS_UNLIKELY(type->types & MS_TYPE_TIME)) {
            return ms_decode_time(s, size, type, path);
        }
        if (MS_UNLIKELY(type->types & MS_TYPE_UUID)) {
            return ms_decode_uuid(s, size, path);
        }
        if (MS_UNLIKELY(type->types & MS_TYPE_DECIMAL)) {
            return ms_decode_decimal(s, size, false, path);
        }
        return ms_check_str_constraints(
            PyUnicode_DecodeUTF8(s, size, NULL), type, path
        );
    }
    return ms_validation_error("str", type, path);
}

static PyObject *
mpack_decode_bin(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    if (MS_UNLIKELY(size < 0)) return NULL;

    if (MS_UNLIKELY(!ms_passes_bytes_constraints(size, type, path))) return NULL;

    char *s = NULL;
    if (MS_UNLIKELY(mpack_read(self, &s, size) < 0)) return NULL;

    if (type->types & (MS_TYPE_ANY | MS_TYPE_BYTES)) {
        return PyBytes_FromStringAndSize(s, size);
    }
    else if (type->types & MS_TYPE_BYTEARRAY) {
        return PyByteArray_FromStringAndSize(s, size);
    }
    return ms_validation_error("bytes", type, path);
}

static PyObject *
mpack_decode_list(
    DecoderState *self, Py_ssize_t size, TypeNode *el_type, PathNode *path
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = PyList_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL; /* cpylint-ignore */
    }
    for (i = 0; i < size; i++) {
        PathNode el_path = {path, i};
        item = mpack_decode(self, el_type, &el_path, false);
        if (MS_UNLIKELY(item == NULL)) {
            Py_CLEAR(res);
            break;
        }
        PyList_SET_ITEM(res, i, item);
    }
    Py_LeaveRecursiveCall();
    return res;
}

static PyObject *
mpack_decode_set(
    DecoderState *self, bool mutable, Py_ssize_t size, TypeNode *el_type, PathNode *path
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = mutable ? PySet_New(NULL) : PyFrozenSet_New(NULL);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL; /* cpylint-ignore */
    }
    for (i = 0; i < size; i++) {
        PathNode el_path = {path, i};
        item = mpack_decode(self, el_type, &el_path, true);
        if (MS_UNLIKELY(item == NULL || PySet_Add(res, item) < 0)) {
            Py_XDECREF(item);
            Py_CLEAR(res);
            break;
        }
        Py_DECREF(item);
    }
    Py_LeaveRecursiveCall();
    return res;
}

static PyObject *
mpack_decode_vartuple(
    DecoderState *self, Py_ssize_t size, TypeNode *el_type, PathNode *path, bool is_key
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = PyTuple_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL; /* cpylint-ignore */
    }
    for (i = 0; i < size; i++) {
        PathNode el_path = {path, i};
        item = mpack_decode(self, el_type, &el_path, is_key);
        if (MS_UNLIKELY(item == NULL)) {
            Py_CLEAR(res);
            break;
        }
        PyTuple_SET_ITEM(res, i, item);
    }
    Py_LeaveRecursiveCall();
    return res;
}

static PyObject *
mpack_decode_fixtuple(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path, bool is_key
) {
    PyObject *res, *item;
    Py_ssize_t i, fixtuple_size, offset;

    TypeNode_get_fixtuple(type, &offset, &fixtuple_size);

    if (size != fixtuple_size) {
        /* tuple is the incorrect size, raise and return */
        ms_raise_validation_error(
            path,
            "Expected `array` of length %zd, got %zd%U",
            fixtuple_size,
            size
        );
        return NULL;
    }

    res = PyTuple_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL; /* cpylint-ignore */
    }

    for (i = 0; i < fixtuple_size; i++) {
        PathNode el_path = {path, i};
        item = mpack_decode(self, type->details[offset + i].pointer, &el_path, is_key);
        if (MS_UNLIKELY(item == NULL)) {
            Py_CLEAR(res);
            break;
        }
        PyTuple_SET_ITEM(res, i, item);
    }
    Py_LeaveRecursiveCall();
    return res;
}


static PyObject *
mpack_decode_namedtuple(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path, bool is_key
) {
    NamedTupleInfo *info = TypeNode_get_namedtuple_info(type);
    Py_ssize_t nfields = Py_SIZE(info);
    Py_ssize_t ndefaults = info->defaults == NULL ? 0 : PyTuple_GET_SIZE(info->defaults);
    Py_ssize_t nrequired = nfields - ndefaults;

    if (size < nrequired || nfields < size) {
        /* tuple is the incorrect size, raise and return */
        if (ndefaults == 0) {
            ms_raise_validation_error(
                path,
                "Expected `array` of length %zd, got %zd%U",
                nfields,
                size
            );
        }
        else {
            ms_raise_validation_error(
                path,
                "Expected `array` of length %zd to %zd, got %zd%U",
                nrequired,
                nfields,
                size
            );
        }
        return NULL;
    }
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyTypeObject *nt_type = (PyTypeObject *)(info->class);
    PyObject *res = nt_type->tp_alloc(nt_type, nfields);
    if (res == NULL) goto error;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyTuple_SET_ITEM(res, i, NULL);
    }

    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode el_path = {path, i};
        PyObject *item = mpack_decode(self, info->types[i], &el_path, is_key);
        if (MS_UNLIKELY(item == NULL)) goto error;
        PyTuple_SET_ITEM(res, i, item);
    }
    for (Py_ssize_t i = size; i < nfields; i++) {
        PyObject *item = PyTuple_GET_ITEM(info->defaults, i - nrequired);
        Py_INCREF(item);
        PyTuple_SET_ITEM(res, i, item);
    }
    Py_LeaveRecursiveCall();
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_CLEAR(res);
    return NULL;
}

static int
mpack_ensure_tag_matches(
    DecoderState *self, PathNode *path, PyObject *expected_tag
) {
    if (PyUnicode_CheckExact(expected_tag)) {
        char *tag = NULL;
        Py_ssize_t tag_size;
        tag_size = mpack_decode_cstr(self, &tag, path);
        if (tag_size < 0) return -1;

        /* Check that tag matches expected tag value */
        Py_ssize_t expected_size;
        const char *expected_str = unicode_str_and_size_nocheck(
            expected_tag, &expected_size
        );
        if (tag_size != expected_size || memcmp(tag, expected_str, expected_size) != 0) {
            /* Tag doesn't match the expected value, error nicely */
            ms_invalid_cstr_value(tag, tag_size, path);
            return -1;
        }
    }
    else {
        int64_t tag = 0;
        uint64_t utag = 0;
        if (mpack_decode_cint(self, &tag, &utag, path) < 0) return -1;
        int64_t expected = PyLong_AsLongLong(expected_tag);
        /* Tags must be int64s, if utag != 0 then we know the tags don't match.
         * We parse the full uint64 value only to validate the message and
         * raise a nice error */
        if (utag != 0) {
            ms_invalid_cuint_value(utag, path);
            return -1;
        }
        if (tag != expected) {
            ms_invalid_cint_value(tag, path);
            return -1;
        }
    }
    return 0;
}

static StructMetaObject *
mpack_decode_tag_and_lookup_type(
    DecoderState *self, Lookup *lookup, PathNode *path
) {
    StructMetaObject *out = NULL;
    if (Lookup_IsStrLookup(lookup)) {
        Py_ssize_t tag_size;
        char *tag = NULL;
        tag_size = mpack_decode_cstr(self, &tag, path);
        if (tag_size < 0) return NULL;
        out = (StructMetaObject *)StrLookup_Get((StrLookup *)lookup, tag, tag_size);
        if (out == NULL) {
            ms_invalid_cstr_value(tag, tag_size, path);
        }
    }
    else {
        int64_t tag = 0;
        uint64_t utag = 0;
        if (mpack_decode_cint(self, &tag, &utag, path) < 0) return NULL;
        if (utag == 0) {
            out = (StructMetaObject *)IntLookup_GetInt64((IntLookup *)lookup, tag);
            if (out == NULL) {
                ms_invalid_cint_value(tag, path);
            }
        }
        else {
            out = (StructMetaObject *)IntLookup_GetUInt64((IntLookup *)lookup, utag);
            if (out == NULL) {
                ms_invalid_cuint_value(utag, path);
            }
        }
    }
    return out;
}

static PyObject *
mpack_decode_struct_array_inner(
    DecoderState *self, Py_ssize_t size, bool tag_already_read,
    StructMetaObject *st_type, PathNode *path, bool is_key
) {
    Py_ssize_t i, nfields, ndefaults, nrequired, npos;
    PyObject *res, *val = NULL;
    bool is_gc, should_untrack;
    bool tagged = st_type->struct_tag_value != NULL;
    PathNode item_path = {path, 0};

    nfields = PyTuple_GET_SIZE(st_type->struct_encode_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    nrequired = tagged + nfields - st_type->n_trailing_defaults;
    npos = nfields - ndefaults;

    if (size < nrequired) {
        ms_raise_validation_error(
            path,
            "Expected `array` of at least length %zd, got %zd%U",
            nrequired,
            size
        );
        return NULL;
    }

    if (tagged) {
        if (!tag_already_read) {
            if (mpack_ensure_tag_matches(self, &item_path, st_type->struct_tag_value) < 0) {
                return NULL;
            }
        }
        size--;
        item_path.index++;
    }

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    res = Struct_alloc((PyTypeObject *)(st_type));
    if (res == NULL) goto error;

    is_gc = MS_TYPE_IS_GC(st_type);
    should_untrack = is_gc;

    for (i = 0; i < nfields; i++) {
        if (size > 0) {
            val = mpack_decode(self, st_type->struct_types[i], &item_path, is_key);
            if (MS_UNLIKELY(val == NULL)) goto error;
            size--;
            item_path.index++;
        }
        else {
            val = get_default(
                PyTuple_GET_ITEM(st_type->struct_defaults, i - npos)
            );
            if (val == NULL)
                goto error;
        }
        Struct_set_index(res, i, val);
        if (should_untrack) {
            should_untrack = !MS_OBJ_IS_GC(val);
        }
    }
    if (MS_UNLIKELY(size > 0)) {
        if (MS_UNLIKELY(st_type->forbid_unknown_fields == OPT_TRUE)) {
            ms_raise_validation_error(
                path,
                "Expected `array` of at most length %zd, got %zd%U",
                nfields,
                nfields + size
            );
            goto error;
        }
        else {
            /* Ignore all trailing fields */
            while (size > 0) {
                if (mpack_skip(self) < 0)
                    goto error;
                size--;
            }
        }
    }
    Py_LeaveRecursiveCall();
    if (is_gc && !should_untrack)
        PyObject_GC_Track(res);
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(res);
    return NULL;
}

static PyObject *
mpack_decode_struct_array(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path, bool is_key
) {
    StructMetaObject *st_type = TypeNode_get_struct(type);
    return mpack_decode_struct_array_inner(self, size, false, st_type, path, is_key);
}

static PyObject *
mpack_decode_struct_array_union(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path, bool is_key
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    if (size == 0) {
        return ms_error_with_path(
            "Expected `array` of at least length 1, got 0%U", path
        );
    }

    /* Decode and lookup tag */
    PathNode tag_path = {path, 0};
    StructMetaObject *struct_type = mpack_decode_tag_and_lookup_type(self, lookup, &tag_path);
    if (struct_type == NULL) return NULL;

    /* Finish decoding the rest of the struct */
    return mpack_decode_struct_array_inner(self, size, true, struct_type, path, is_key);
}

static PyObject *
mpack_decode_array(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path, bool is_key
) {
    if (MS_UNLIKELY(!ms_passes_array_constraints(size, type, path))) return NULL;

    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        if (is_key) {
            return mpack_decode_vartuple(self, size, &type_any, path, is_key);
        }
        else {
            return mpack_decode_list(self, size, &type_any, path);
        }
    }
    else if (type->types & MS_TYPE_LIST) {
        return mpack_decode_list(self, size, TypeNode_get_array(type), path);
    }
    else if (type->types & (MS_TYPE_SET | MS_TYPE_FROZENSET)) {
        return mpack_decode_set(
            self, type->types & MS_TYPE_SET, size, TypeNode_get_array(type), path
        );
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return mpack_decode_vartuple(self, size, TypeNode_get_array(type), path, is_key);
    }
    else if (type->types & MS_TYPE_FIXTUPLE) {
        return mpack_decode_fixtuple(self, size, type, path, is_key);
    }
    else if (type->types & MS_TYPE_NAMEDTUPLE) {
        return mpack_decode_namedtuple(self, size, type, path, is_key);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY) {
        return mpack_decode_struct_array(self, size, type, path, is_key);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY_UNION) {
        return mpack_decode_struct_array_union(self, size, type, path, is_key);
    }
    return ms_validation_error("array", type, path);
}

/* Specialized mpack_decode for dict keys, handling caching of short string keys */
static PyObject *
mpack_decode_key(DecoderState *self, TypeNode *type, PathNode *path) {
    char op;

    if (MS_UNLIKELY(self->input_pos == self->input_end)) {
        ms_err_truncated();
        return NULL;
    }
    /* Peek at the next op */
    op = *self->input_pos;

    if (MS_LIKELY(
            '\xa0' <= op && op <= '\xbf' &&
            type->types & (MS_TYPE_STR | MS_TYPE_ANY)
        )
    ) {
        /* A short (<= 31 byte) unicode str */
        self->input_pos++; /* consume op */
        Py_ssize_t size = op & 0x1f;

        /* Don't cache the empty string */
        if (MS_UNLIKELY(size == 0)) return PyUnicode_New(0, 127);

        /* Read in the string buffer */
        char *str;
        if (MS_UNLIKELY(mpack_read(self, &str, size) < 0)) return NULL;

        /* Attempt a cache lookup. We don't know if it's ascii yet, but
         * checking if it's ascii is more expensive than just doing a lookup,
         * and most dict key strings are ascii */
        uint32_t hash = murmur2(str, size);
        uint32_t index = hash % STRING_CACHE_SIZE;
        PyObject *existing = string_cache[index];

        if (MS_LIKELY(existing != NULL)) {
            Py_ssize_t e_size = ((PyASCIIObject *)existing)->length;
            char *e_str = ascii_get_buffer(existing);
            if (MS_LIKELY(size == e_size && memcmp(str, e_str, size) == 0)) {
                Py_INCREF(existing);
                return existing;
            }
        }

        /* Cache miss, create a new string */
        PyObject *new = PyUnicode_DecodeUTF8(str, size, NULL);
        if (new == NULL) return NULL;

        /* If ascii, add it to the cache */
        if (PyUnicode_IS_COMPACT_ASCII(new)) {
            Py_XDECREF(existing);
            Py_INCREF(new);
            string_cache[index] = new;
        }
        return new;
    }
    /* Fallback to standard decode */
    return mpack_decode(self, type, path, true);
}

static PyObject *
mpack_decode_dict(
    DecoderState *self, Py_ssize_t size, TypeNode *key_type,
    TypeNode *val_type, PathNode *path
) {
    Py_ssize_t i;
    PyObject *res, *key = NULL, *val = NULL;
    PathNode key_path = {path, PATH_KEY, NULL};
    PathNode val_path = {path, PATH_ELLIPSIS, NULL};

    res = PyDict_New();
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL; /* cpylint-ignore */
    }
    for (i = 0; i < size; i++) {
        key = mpack_decode_key(self, key_type, &key_path);
        if (MS_UNLIKELY(key == NULL))
            goto error;
        val = mpack_decode(self, val_type, &val_path, false);
        if (MS_UNLIKELY(val == NULL))
            goto error;
        if (MS_UNLIKELY(PyDict_SetItem(res, key, val) < 0))
            goto error;
        Py_CLEAR(key);
        Py_CLEAR(val);
    }
    Py_LeaveRecursiveCall();
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(key);
    Py_XDECREF(val);
    Py_DECREF(res);
    return NULL;
}

static PyObject *
mpack_decode_typeddict(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyObject *res = PyDict_New();
    if (res == NULL) goto error;

    TypedDictInfo *info = TypeNode_get_typeddict_info(type);
    Py_ssize_t nrequired = 0, pos = 0;
    for (Py_ssize_t i = 0; i < size; i++) {
        char *key;
        PathNode key_path = {path, PATH_KEY, NULL};
        Py_ssize_t key_size = mpack_decode_cstr(self, &key, &key_path);
        if (MS_UNLIKELY(key_size < 0)) goto error;

        TypeNode *field_type;
        PyObject *field = TypedDictInfo_lookup_key(info, key, key_size, &field_type, &pos);
        if (field != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = mpack_decode(self, field_type, &field_path, false);
            if (val == NULL) goto error;
            /* We want to keep a count of required fields we've decoded. Since
             * duplicates can occur, we stash the current dict size, then only
             * increment if the dict size has changed _and_ the field is
             * required. */
            Py_ssize_t cur_size = PyDict_GET_SIZE(res);
            int status = PyDict_SetItem(res, field, val);
            /* Always decref value, no need to decref key since it's a borrowed
             * reference. */
            Py_DECREF(val);
            if (status < 0) goto error;
            if ((PyDict_GET_SIZE(res) != cur_size) && (field_type->types & MS_EXTRA_FLAG)) {
                nrequired++;
            }
        }
        else {
            /* Unknown field, skip */
            if (mpack_skip(self) < 0) goto error;
        }
    }
    if (nrequired < info->nrequired) {
        /* A required field is missing, determine which one and raise */
        TypedDictInfo_error_missing(info, res, path);
        goto error;
    }
    Py_LeaveRecursiveCall();
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(res);
    return NULL;
}

static PyObject *
mpack_decode_dataclass(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    DataclassInfo *info = TypeNode_get_dataclass_info(type);

    PyTypeObject *dataclass_type = (PyTypeObject *)(info->class);
    PyObject *out = dataclass_type->tp_alloc(dataclass_type, 0);
    if (out == NULL) goto error;

    Py_ssize_t pos = 0;
    for (Py_ssize_t i = 0; i < size; i++) {
        char *key;
        PathNode key_path = {path, PATH_KEY, NULL};
        Py_ssize_t key_size = mpack_decode_cstr(self, &key, &key_path);
        if (MS_UNLIKELY(key_size < 0)) goto error;

        TypeNode *field_type;
        PyObject *field = DataclassInfo_lookup_key(info, key, key_size, &field_type, &pos);
        if (field != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = mpack_decode(self, field_type, &field_path, false);
            if (val == NULL) goto error;
            int status = PyObject_SetAttr(out, field, val);
            Py_DECREF(val);
            if (status < 0) goto error;
        }
        else {
            /* Unknown field, skip */
            if (mpack_skip(self) < 0) goto error;
        }
    }
    if (DataclassInfo_post_decode(info, out, path) < 0) goto error;

    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
mpack_decode_struct_map(
    DecoderState *self, Py_ssize_t size,
    StructMetaObject *st_type, PathNode *path, bool is_key
) {
    Py_ssize_t i, key_size, field_index, pos = 0;
    char *key = NULL;
    PyObject *res, *val = NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    res = Struct_alloc((PyTypeObject *)(st_type));
    if (res == NULL) goto error;

    for (i = 0; i < size; i++) {
        PathNode key_path = {path, PATH_KEY, NULL};
        key_size = mpack_decode_cstr(self, &key, &key_path);
        if (MS_UNLIKELY(key_size < 0)) goto error;

        field_index = StructMeta_get_field_index(st_type, key, key_size, &pos);
        if (field_index < 0) {
            if (MS_UNLIKELY(field_index == -2)) {
                PathNode tag_path = {path, PATH_STR, st_type->struct_tag_field};
                if (mpack_ensure_tag_matches(self, &tag_path, st_type->struct_tag_value) < 0) {
                    goto error;
                }
            }
            else {
                /* Unknown field */
                if (MS_UNLIKELY(st_type->forbid_unknown_fields == OPT_TRUE)) {
                    ms_error_unknown_field(key, key_size, path);
                    goto error;
                }
                else {
                    if (mpack_skip(self) < 0) goto error;
                }
            }
        }
        else {
            PathNode field_path = {path, field_index, (PyObject *)st_type};
            val = mpack_decode(
                self, st_type->struct_types[field_index], &field_path, is_key
            );
            if (val == NULL) goto error;
            Struct_set_index(res, field_index, val);
        }
    }

    if (Struct_fill_in_defaults(st_type, res, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(res);
    return NULL;
}

static PyObject *
mpack_decode_struct_union(
    DecoderState *self, Py_ssize_t size, TypeNode *type,
    PathNode *path, bool is_key
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    PathNode key_path = {path, PATH_KEY, NULL};
    Py_ssize_t tag_field_size;
    const char *tag_field = unicode_str_and_size_nocheck(
        Lookup_tag_field(lookup), &tag_field_size
    );

    /* Cache the current input position in case we need to reset it once the
     * tag is found */
    char *orig_input_pos = self->input_pos;

    for (Py_ssize_t i = 0; i < size; i++) {
        Py_ssize_t key_size;
        char *key = NULL;

        key_size = mpack_decode_cstr(self, &key, &key_path);
        if (key_size < 0) return NULL;

        if (key_size == tag_field_size && memcmp(key, tag_field, key_size) == 0) {
            /* Decode and lookup tag */
            PathNode tag_path = {path, PATH_STR, Lookup_tag_field(lookup)};
            StructMetaObject *struct_type = mpack_decode_tag_and_lookup_type(self, lookup, &tag_path);
            if (struct_type == NULL) return NULL;
            if (i == 0) {
                /* Common case, tag is first field. No need to reset, just mark
                 * that the first field has been read. */
                size--;
            }
            else {
                self->input_pos = orig_input_pos;
            }
            return mpack_decode_struct_map(self, size, struct_type, path, is_key);
        }
        else {
            /* Not the tag field, skip the value and try again */
            if (mpack_skip(self) < 0) return NULL;
        }
    }

    ms_raise_validation_error(
        path,
        "Object missing required field `%U`%U",
        Lookup_tag_field(lookup)
    );
    return NULL;
}

static PyObject *
mpack_decode_map(
    DecoderState *self, Py_ssize_t size, TypeNode *type,
    PathNode *path, bool is_key
) {
    if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        return mpack_decode_struct_map(self, size, struct_type, path, is_key);
    }
    else if (type->types & MS_TYPE_TYPEDDICT) {
        return mpack_decode_typeddict(self, size, type, path);
    }
    else if (type->types & MS_TYPE_DATACLASS) {
        return mpack_decode_dataclass(self, size, type, path);
    }
    else if (type->types & (MS_TYPE_DICT | MS_TYPE_ANY)) {
        if (MS_UNLIKELY(!ms_passes_map_constraints(size, type, path))) {
            return NULL;
        }
        TypeNode *key, *val;
        TypeNode type_any = {MS_TYPE_ANY};
        if (type->types & MS_TYPE_ANY) {
            key = &type_any;
            val = &type_any;
        }
        else {
            TypeNode_get_dict(type, &key, &val);
        }
        return mpack_decode_dict(self, size, key, val, path);
    }
    else if (type->types & MS_TYPE_STRUCT_UNION) {
        return mpack_decode_struct_union(self, size, type, path, is_key);
    }
    return ms_validation_error("object", type, path);
}

static PyObject *
mpack_decode_ext(
    DecoderState *self, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    Py_buffer *buffer;
    char c_code = 0, *data_buf = NULL;
    long code;
    PyObject *data, *pycode = NULL, *view = NULL, *out = NULL;

    if (size < 0) return NULL;
    if (mpack_read1(self, &c_code) < 0) return NULL;
    code = *((int8_t *)(&c_code));
    if (mpack_read(self, &data_buf, size) < 0) return NULL;

    if (type->types & MS_TYPE_DATETIME && code == -1) {
        return mpack_decode_datetime(self, data_buf, size, type, path);
    }
    else if (type->types & MS_TYPE_EXT) {
        data = PyBytes_FromStringAndSize(data_buf, size);
        if (data == NULL) return NULL;
        return Ext_New(code, data);
    }
    else if (!(type->types & MS_TYPE_ANY)) {
        return ms_validation_error("ext", type, path);
    }

    /* Decode Any.
     * - datetime if code == -1
     * - call ext_hook if available
     * - otherwise return Ext object
     * */
    if (code == -1) {
        return mpack_decode_datetime(self, data_buf, size, type, path);
    }
    else if (self->ext_hook == NULL) {
        data = PyBytes_FromStringAndSize(data_buf, size);
        if (data == NULL) return NULL;
        return Ext_New(code, data);
    }
    else {
        pycode = PyLong_FromLong(code);
        if (pycode == NULL) goto done;
    }
    view = PyMemoryView_GetContiguous(self->buffer_obj, PyBUF_READ, 'C');
    if (view == NULL) goto done;
    buffer = PyMemoryView_GET_BUFFER(view);
    buffer->buf = data_buf;
    buffer->len = size;

    out = PyObject_CallFunctionObjArgs(self->ext_hook, pycode, view, NULL);
done:
    Py_XDECREF(pycode);
    Py_XDECREF(view);
    return out;
}

static MS_NOINLINE PyObject *
mpack_decode_raw(DecoderState *self) {
    char *start = self->input_pos;
    if (mpack_skip(self) < 0) return NULL;
    Py_ssize_t size = self->input_pos - start;
    return Raw_FromView(self->buffer_obj, start, size);
}

static MS_INLINE PyObject *
mpack_decode_nocustom(
    DecoderState *self, TypeNode *type, PathNode *path, bool is_key
) {
    char op = 0;
    char *s = NULL;

    if (mpack_read1(self, &op) < 0) {
        return NULL;
    }

    if (('\x00' <= op && op <= '\x7f') || ('\xe0' <= op && op <= '\xff')) {
        return mpack_decode_int(self, *((int8_t *)(&op)), type, path);
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        return mpack_decode_str(self, op & 0x1f, type, path);
    }
    else if ('\x90' <= op && op <= '\x9f') {
        return mpack_decode_array(self, op & 0x0f, type, path, is_key);
    }
    else if ('\x80' <= op && op <= '\x8f') {
        return mpack_decode_map(self, op & 0x0f, type, path, is_key);
    }
    switch ((enum mpack_code)op) {
        case MP_NIL:
            return mpack_decode_none(self, type, path);
        case MP_TRUE:
            return mpack_decode_bool(self, Py_True, type, path);
        case MP_FALSE:
            return mpack_decode_bool(self, Py_False, type, path);
        case MP_UINT8:
            if (MS_UNLIKELY(mpack_read(self, &s, 1) < 0)) return NULL;
            return mpack_decode_uint(self, *(uint8_t *)s, type, path);
        case MP_UINT16:
            if (MS_UNLIKELY(mpack_read(self, &s, 2) < 0)) return NULL;
            return mpack_decode_uint(self, _msgspec_load16(uint16_t, s), type, path);
        case MP_UINT32:
            if (MS_UNLIKELY(mpack_read(self, &s, 4) < 0)) return NULL;
            return mpack_decode_uint(self, _msgspec_load32(uint32_t, s), type, path);
        case MP_UINT64:
            if (MS_UNLIKELY(mpack_read(self, &s, 8) < 0)) return NULL;
            return mpack_decode_uint(self, _msgspec_load64(uint64_t, s), type, path);
        case MP_INT8:
            if (MS_UNLIKELY(mpack_read(self, &s, 1) < 0)) return NULL;
            return mpack_decode_int(self, *(int8_t *)s, type, path);
        case MP_INT16:
            if (MS_UNLIKELY(mpack_read(self, &s, 2) < 0)) return NULL;
            return mpack_decode_int(self, _msgspec_load16(int16_t, s), type, path);
        case MP_INT32:
            if (MS_UNLIKELY(mpack_read(self, &s, 4) < 0)) return NULL;
            return mpack_decode_int(self, _msgspec_load32(int32_t, s), type, path);
        case MP_INT64:
            if (MS_UNLIKELY(mpack_read(self, &s, 8) < 0)) return NULL;
            return mpack_decode_int(self, _msgspec_load64(int64_t, s), type, path);
        case MP_FLOAT32: {
            float f = 0;
            uint32_t uf;
            if (mpack_read(self, &s, 4) < 0) return NULL;
            uf = _msgspec_load32(uint32_t, s);
            memcpy(&f, &uf, 4);
            return mpack_decode_float(self, f, type, path);
        }
        case MP_FLOAT64: {
            double f = 0;
            uint64_t uf;
            if (mpack_read(self, &s, 8) < 0) return NULL;
            uf = _msgspec_load64(uint64_t, s);
            memcpy(&f, &uf, 8);
            return mpack_decode_float(self, f, type, path);
        }
        case MP_STR8: {
            Py_ssize_t size = mpack_decode_size1(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_str(self, size, type, path);
        }
        case MP_STR16: {
            Py_ssize_t size = mpack_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_str(self, size, type, path);
        }
        case MP_STR32: {
            Py_ssize_t size = mpack_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_str(self, size, type, path);
        }
        case MP_BIN8:
            return mpack_decode_bin(self, mpack_decode_size1(self), type, path);
        case MP_BIN16:
            return mpack_decode_bin(self, mpack_decode_size2(self), type, path);
        case MP_BIN32:
            return mpack_decode_bin(self, mpack_decode_size4(self), type, path);
        case MP_ARRAY16: {
            Py_ssize_t size = mpack_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_array(self, size, type, path, is_key);
        }
        case MP_ARRAY32: {
            Py_ssize_t size = mpack_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_array(self, size, type, path, is_key);
        }
        case MP_MAP16: {
            Py_ssize_t size = mpack_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_map(self, size, type, path, is_key);
        }
        case MP_MAP32: {
            Py_ssize_t size = mpack_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mpack_decode_map(self, size, type, path, is_key);
        }
        case MP_FIXEXT1:
            return mpack_decode_ext(self, 1, type, path);
        case MP_FIXEXT2:
            return mpack_decode_ext(self, 2, type, path);
        case MP_FIXEXT4:
            return mpack_decode_ext(self, 4, type, path);
        case MP_FIXEXT8:
            return mpack_decode_ext(self, 8, type, path);
        case MP_FIXEXT16:
            return mpack_decode_ext(self, 16, type, path);
        case MP_EXT8:
            return mpack_decode_ext(self, mpack_decode_size1(self), type, path);
        case MP_EXT16:
            return mpack_decode_ext(self, mpack_decode_size2(self), type, path);
        case MP_EXT32:
            return mpack_decode_ext(self, mpack_decode_size4(self), type, path);
        default:
            PyErr_Format(
                msgspec_get_global_state()->DecodeError,
                "MessagePack data is malformed: invalid opcode '\\x%02x' (byte %zd)",
                (unsigned char)op,
                (Py_ssize_t)(self->input_pos - self->input_start - 1)
            );
            return NULL;
    }
}

static PyObject *
mpack_decode(
    DecoderState *self, TypeNode *type, PathNode *path, bool is_key
) {
    if (MS_UNLIKELY(type->types == 0)) {
        return mpack_decode_raw(self);
    }
    PyObject *obj = mpack_decode_nocustom(self, type, path, is_key);
    if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
        return ms_decode_custom(obj, self->dec_hook, type, path);
    }
    return obj;
}

PyDoc_STRVAR(Decoder_decode__doc__,
"decode(self, buf)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like\n"
"    The message to decode.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object.\n"
);
static PyObject*
Decoder_decode(Decoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *res = NULL;
    Py_buffer buffer;
    buffer.buf = NULL;

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }

    if (PyObject_GetBuffer(args[0], &buffer, PyBUF_CONTIG_RO) >= 0) {
        self->state.buffer_obj = args[0];
        self->state.input_start = buffer.buf;
        self->state.input_pos = buffer.buf;
        self->state.input_end = self->state.input_pos + buffer.len;

        res = mpack_decode(&(self->state), self->state.type, NULL, false);

        if (res != NULL && mpack_has_trailing_characters(&self->state)) {
            Py_CLEAR(res);
        }

        PyBuffer_Release(&buffer);
        self->state.buffer_obj = NULL;
        self->state.input_start = NULL;
        self->state.input_pos = NULL;
        self->state.input_end = NULL;
    }
    return res;
}

static struct PyMethodDef Decoder_methods[] = {
    {
        "decode", (PyCFunction) Decoder_decode, METH_FASTCALL,
        Decoder_decode__doc__,
    },
    {NULL, NULL}                /* sentinel */
};

static PyMemberDef Decoder_members[] = {
    {"type", T_OBJECT_EX, offsetof(Decoder, orig_type), READONLY, "The Decoder type"},
    {"dec_hook", T_OBJECT, offsetof(Decoder, state.dec_hook), READONLY, "The Decoder dec_hook"},
    {"ext_hook", T_OBJECT, offsetof(Decoder, state.ext_hook), READONLY, "The Decoder ext_hook"},
    {NULL},
};

static PyTypeObject Decoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.msgpack.Decoder",
    .tp_doc = Decoder__doc__,
    .tp_basicsize = sizeof(Decoder),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Decoder_init,
    .tp_traverse = (traverseproc)Decoder_traverse,
    .tp_dealloc = (destructor)Decoder_dealloc,
    .tp_repr = (reprfunc)Decoder_repr,
    .tp_methods = Decoder_methods,
    .tp_members = Decoder_members,
};


PyDoc_STRVAR(msgspec_msgpack_decode__doc__,
"msgpack_decode(buf, *, type='Any', dec_hook=None, ext_hook=None)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like\n"
"    The message to decode.\n"
"type : type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default MessagePack types.\n"
"dec_hook : callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic MessagePack types. This hook should transform ``obj`` into\n"
"    type ``type``, or raise a ``TypeError`` if unsupported.\n"
"ext_hook : callable, optional\n"
"    An optional callback for decoding MessagePack extensions. Should have the\n"
"    signature ``ext_hook(code: int, data: memoryview) -> Any``. If provided,\n"
"    this will be called to deserialize all extension types found in the\n"
"    message. Note that ``data`` is a memoryview into the larger message\n"
"    buffer - any references created to the underlying buffer without copying\n"
"    the data out will cause the full message buffer to persist in memory.\n"
"    If not provided, extension types will decode as ``msgspec.Ext`` objects.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object.\n"
"\n"
"See Also\n"
"--------\n"
"Decoder.decode"
);
static PyObject*
msgspec_msgpack_decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *res = NULL, *buf = NULL, *type = NULL, *dec_hook = NULL, *ext_hook = NULL;
    DecoderState state;
    Py_buffer buffer;
    MsgspecState *st = msgspec_get_global_state();

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    buf = args[0];
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if ((type = find_keyword(kwnames, args + nargs, st->str_type)) != NULL) nkwargs--;
        if ((dec_hook = find_keyword(kwnames, args + nargs, st->str_dec_hook)) != NULL) nkwargs--;
        if ((ext_hook = find_keyword(kwnames, args + nargs, st->str_ext_hook)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    /* Handle dec_hook */
    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    if (dec_hook != NULL) {
        if (!PyCallable_Check(dec_hook)) {
            PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
            return NULL;
        }
    }
    state.dec_hook = dec_hook;

    /* Handle ext_hook */
    if (ext_hook == Py_None) {
        ext_hook = NULL;
    }
    if (ext_hook != NULL) {
        if (!PyCallable_Check(ext_hook)) {
            PyErr_SetString(PyExc_TypeError, "ext_hook must be callable");
            return NULL;
        }
    }
    state.ext_hook = ext_hook;

    /* Allocated Any & Struct type nodes (simple, common cases) on the stack,
     * everything else on the heap */
    state.type = NULL;
    if (type == NULL || type == st->typing_any) {
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        if (StructMeta_prep_types(type, false, NULL) < 0) return NULL;
    }
    else {
        state.type = TypeNode_Convert(type, false, NULL);
        if (state.type == NULL) return NULL;
    }

    buffer.buf = NULL;
    if (PyObject_GetBuffer(buf, &buffer, PyBUF_CONTIG_RO) >= 0) {
        state.buffer_obj = buf;
        state.input_start = buffer.buf;
        state.input_pos = buffer.buf;
        state.input_end = state.input_pos + buffer.len;
        if (state.type != NULL) {
            res = mpack_decode(&state, state.type, NULL, false);
        }
        else if (type == NULL || type == st->typing_any) {
            TypeNode type_any = {MS_TYPE_ANY};
            res = mpack_decode(&state, &type_any, NULL, false);
        }
        else {
            bool array_like = ((StructMetaObject *)type)->array_like == OPT_TRUE;
            TypeNodeSimple type_obj;
            type_obj.types = array_like ? MS_TYPE_STRUCT_ARRAY : MS_TYPE_STRUCT;
            type_obj.details[0].pointer = type;
            res = mpack_decode(&state, (TypeNode*)(&type_obj), NULL, false);
        }
        PyBuffer_Release(&buffer);
        if (res != NULL && mpack_has_trailing_characters(&state)) {
            Py_CLEAR(res);
        }
    }

    if (state.type != NULL) {
        TypeNode_Free(state.type);
    }
    return res;
}

/*************************************************************************
 * JSON Decoder                                                          *
 *************************************************************************/

typedef struct JSONDecoderState {
    /* Configuration */
    TypeNode *type;
    PyObject *dec_hook;

    /* Temporary scratch space */
    unsigned char *scratch;
    Py_ssize_t scratch_capacity;
    Py_ssize_t scratch_len;

    /* Per-message attributes */
    PyObject *buffer_obj;
    unsigned char *input_start;
    unsigned char *input_pos;
    unsigned char *input_end;
} JSONDecoderState;

typedef struct JSONDecoder {
    PyObject_HEAD
    PyObject *orig_type;
    JSONDecoderState state;
} JSONDecoder;

PyDoc_STRVAR(JSONDecoder__doc__,
"Decoder(type='Any', *, dec_hook=None)\n"
"--\n"
"\n"
"A JSON decoder.\n"
"\n"
"Parameters\n"
"----------\n"
"type : type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default JSON types.\n"
"dec_hook : callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic JSON types. This hook should transform ``obj`` into type\n"
"    ``type``, or raise a ``TypeError`` if unsupported."
);
static int
JSONDecoder_init(JSONDecoder *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"type", "dec_hook", NULL};
    MsgspecState *st = msgspec_get_global_state();
    PyObject *type = st->typing_any;
    PyObject *dec_hook = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O$O", kwlist, &type, &dec_hook)) {
        return -1;
    }

    /* Handle dec_hook */
    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    if (dec_hook != NULL) {
        if (!PyCallable_Check(dec_hook)) {
            PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
            return -1;
        }
        Py_INCREF(dec_hook);
    }
    self->state.dec_hook = dec_hook;

    /* Handle type */
    self->state.type = TypeNode_Convert(type, true, NULL);
    if (self->state.type == NULL) return -1;
    Py_INCREF(type);
    self->orig_type = type;

    /* Init scratch space */
    self->state.scratch = NULL;
    self->state.scratch_capacity = 0;
    self->state.scratch_len = 0;

    return 0;
}

static int
JSONDecoder_traverse(JSONDecoder *self, visitproc visit, void *arg)
{
    int out = TypeNode_traverse(self->state.type, visit, arg);
    if (out != 0) return out;
    Py_VISIT(self->orig_type);
    Py_VISIT(self->state.dec_hook);
    return 0;
}

static void
JSONDecoder_dealloc(JSONDecoder *self)
{
    PyObject_GC_UnTrack(self);
    TypeNode_Free(self->state.type);
    Py_XDECREF(self->orig_type);
    Py_XDECREF(self->state.dec_hook);
    PyMem_Free(self->state.scratch);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
JSONDecoder_repr(JSONDecoder *self) {
    int recursive;
    PyObject *typstr, *out = NULL;

    recursive = Py_ReprEnter((PyObject *)self);
    if (recursive != 0) {
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");  /* cpylint-ignore */
    }
    typstr = PyObject_Repr(self->orig_type);
    if (typstr != NULL) {
        out = PyUnicode_FromFormat("msgspec.json.Decoder(%U)", typstr);
    }
    Py_XDECREF(typstr);
    Py_ReprLeave((PyObject *)self);
    return out;
}

static MS_INLINE bool
json_read1(JSONDecoderState *self, unsigned char *c)
{
    if (MS_UNLIKELY(self->input_pos == self->input_end)) {
        ms_err_truncated();
        return false;
    }
    *c = *self->input_pos;
    self->input_pos += 1;
    return true;
}

static MS_INLINE char
json_peek_or_null(JSONDecoderState *self) {
    if (MS_UNLIKELY(self->input_pos == self->input_end)) return '\0';
    return *self->input_pos;
}

static MS_INLINE bool
json_peek_skip_ws(JSONDecoderState *self, unsigned char *s)
{
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) {
            ms_err_truncated();
            return false;
        }
        unsigned char c = *self->input_pos;
        if (MS_LIKELY(c != ' ' && c != '\n' && c != '\r' && c != '\t')) {
            *s = c;
            return true;
        }
        self->input_pos++;
    }
}

static MS_INLINE bool
json_remaining(JSONDecoderState *self, ptrdiff_t remaining)
{
    return self->input_end - self->input_pos >= remaining;
}

static PyObject *
json_err_invalid(JSONDecoderState *self, const char *msg)
{
    PyErr_Format(
        msgspec_get_global_state()->DecodeError,
        "JSON is malformed: %s (byte %zd)",
        msg,
        (Py_ssize_t)(self->input_pos - self->input_start)
    );
    return NULL;
}

static MS_INLINE bool
json_has_trailing_characters(JSONDecoderState *self)
{
    while (self->input_pos != self->input_end) {
        unsigned char c = *self->input_pos++;
        if (MS_UNLIKELY(!(c == ' ' || c == '\n' || c == '\t' || c == '\r'))) {
            json_err_invalid(self, "trailing characters");
            return true;
        }
    }
    return false;
}

static int json_skip(JSONDecoderState *self);

static PyObject * json_decode(
    JSONDecoderState *self, TypeNode *type, PathNode *path
);

static PyObject *
json_decode_none(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 'n' */
    if (MS_UNLIKELY(!json_remaining(self, 3))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'u' || c2 != 'l' || c3 != 'l')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_NONE)) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    return ms_validation_error("null", type, path);
}

static PyObject *
json_decode_true(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 't' */
    if (MS_UNLIKELY(!json_remaining(self, 3))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'r' || c2 != 'u' || c3 != 'e')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    return ms_validation_error("bool", type, path);
}

static PyObject *
json_decode_false(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 'f' */
    if (MS_UNLIKELY(!json_remaining(self, 4))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    unsigned char c4 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'a' || c2 != 'l' || c3 != 's' || c4 != 'e')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    return ms_validation_error("bool", type, path);
}

#define JS_SCRATCH_MAX_SIZE 1024

static int
json_scratch_resize(JSONDecoderState *state, Py_ssize_t size) {
    unsigned char *temp = PyMem_Realloc(state->scratch, size);
    if (MS_UNLIKELY(temp == NULL)) {
        PyErr_NoMemory();
        return -1;
    }
    state->scratch = temp;
    state->scratch_capacity = size;
    return 0;
}

static MS_NOINLINE int
json_scratch_expand(JSONDecoderState *state, Py_ssize_t required) {
    size_t new_size = Py_MAX(8, 1.5 * required);
    return json_scratch_resize(state, new_size);
}

static int
json_scratch_reset(JSONDecoderState *state) {
    state->scratch_len = 0;
    if (state->scratch_capacity > JS_SCRATCH_MAX_SIZE) {
        if (json_scratch_resize(state, JS_SCRATCH_MAX_SIZE) < 0) return -1;
    }
    return 0;
}

static int
json_scratch_extend(JSONDecoderState *state, const void *buf, Py_ssize_t size) {
    Py_ssize_t required = state->scratch_len + size;
    if (MS_UNLIKELY(required >= state->scratch_capacity)) {
        if (MS_UNLIKELY(json_scratch_expand(state, required) < 0)) return -1;
    }
    memcpy(state->scratch + state->scratch_len, buf, size);
    state->scratch_len += size;
    return 0;
}

/* -1: '\', '"', and forbidden characters
 * 0: ascii
 * 1: non-ascii */
static const int8_t char_types[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, -1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
};

/* Is char `"`, `\`, or nonascii? */
static MS_INLINE bool char_is_special_or_nonascii(unsigned char c) {
    return char_types[c] != 0;
}

/* Is char `"` or `\`? */
static MS_INLINE bool char_is_special(unsigned char c) {
    return char_types[c] < 0;
}

static int
json_read_codepoint(JSONDecoderState *self, unsigned int *out) {
    unsigned char c;
    unsigned int cp = 0;
    if (!json_remaining(self, 4)) return ms_err_truncated();
    for (int i = 0; i < 4; i++) {
        c = *self->input_pos++;
        if (c >= '0' && c <= '9') {
            c -= '0';
        }
        else if (c >= 'a' && c <= 'f') {
            c = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F') {
            c = c - 'A' + 10;
        }
        else {
            json_err_invalid(self, "invalid character in unicode escape");
            return -1;
        }
        cp = (cp << 4) + c;
    }
    *out = cp;
    return 0;
}

static MS_NOINLINE int
json_handle_unicode_escape(JSONDecoderState *self) {
    unsigned int cp;
    if (json_read_codepoint(self, &cp) < 0) return -1;

    if (0xDC00 <= cp && cp <= 0xDFFF) {
        json_err_invalid(self, "invalid utf-16 surrogate pair");
        return -1;
    }
    else if (0xD800 <= cp && cp <= 0xDBFF) {
        /* utf-16 pair, parse 2nd pair */
        unsigned int cp2;
        if (!json_remaining(self, 6)) return ms_err_truncated();
        if (self->input_pos[0] != '\\' || self->input_pos[1] != 'u') {
            json_err_invalid(self, "unexpected end of escaped utf-16 surrogate pair");
            return -1;
        }
        self->input_pos += 2;
        if (json_read_codepoint(self, &cp2) < 0) return -1;
        if (cp2 < 0xDC00 || cp2 > 0xDFFF) {
            json_err_invalid(self, "invalid utf-16 surrogate pair");
            return -1;
        }
        cp = 0x10000 + (((cp - 0xD800) << 10) | (cp2 - 0xDC00));
    }

    /* Encode the codepoint as utf-8 */
    unsigned char *p = self->scratch + self->scratch_len;
    if (cp < 0x80) {
        *p++ = cp;
        self->scratch_len += 1;
    } else if (cp < 0x800) {
        *p++ = 0xC0 | (cp >> 6);
        *p++ = 0x80 | (cp & 0x3F);
        self->scratch_len += 2;
    } else if (cp < 0x10000) {
        *p++ = 0xE0 | (cp >> 12);
        *p++ = 0x80 | ((cp >> 6) & 0x3F);
        *p++ = 0x80 | (cp & 0x3F);
        self->scratch_len += 3;
    } else {
        *p++ = 0xF0 | (cp >> 18);
        *p++ = 0x80 | ((cp >> 12) & 0x3F);
        *p++ = 0x80 | ((cp >> 6) & 0x3F);
        *p++ = 0x80 | (cp & 0x3F);
        self->scratch_len += 4;
    }
    return 0;
}

/* These macros are used to manually unroll some ascii scanning loops below */
#define repeat8(x)  { x(0) x(1) x(2) x(3) x(4) x(5) x(6) x(7) }

#define parse_ascii_pre(i) \
    if (MS_UNLIKELY(char_is_special_or_nonascii(self->input_pos[i]))) goto parse_ascii_##i;

#define parse_ascii_post(i) \
    parse_ascii_##i: \
    self->input_pos += i; \
    goto parse_ascii_end;

#define parse_unicode_pre(i) \
    if (MS_UNLIKELY(char_is_special(self->input_pos[i]))) goto parse_unicode_##i;

#define parse_unicode_post(i) \
    parse_unicode_##i: \
    self->input_pos += i; \
    goto parse_unicode_end;

static MS_NOINLINE Py_ssize_t
json_decode_string_view_copy(
    JSONDecoderState *self, char **out, bool *is_ascii, unsigned char *start
) {
    unsigned char c;
    self->scratch_len = 0;

top:
    OPT_FORCE_RELOAD(*self->input_pos);

    c = *self->input_pos;
    if (c == '\\') {
        /* Write the current block to scratch */
        Py_ssize_t block_size = self->input_pos - start;
        /* An escape string requires at most 4 bytes to decode */
        Py_ssize_t required = self->scratch_len + block_size + 4;
        if (MS_UNLIKELY(required >= self->scratch_capacity)) {
            if (MS_UNLIKELY(json_scratch_expand(self, required) < 0)) return -1;
        }
        memcpy(self->scratch + self->scratch_len, start, block_size);
        self->scratch_len += block_size;

        self->input_pos++;
        if (!json_read1(self, &c)) return -1;

        switch (c) {
            case 'n': {
                *(self->scratch + self->scratch_len) = '\n';
                self->scratch_len++;
                break;
            }
            case '"': {
                *(self->scratch + self->scratch_len) = '"';
                self->scratch_len++;
                break;
            }
            case 't': {
                *(self->scratch + self->scratch_len) = '\t';
                self->scratch_len++;
                break;
            }
            case 'r': {
                *(self->scratch + self->scratch_len) = '\r';
                self->scratch_len++;
                break;
            }
            case '\\': {
                *(self->scratch + self->scratch_len) = '\\';
                self->scratch_len++;
                break;
            }
            case '/': {
                *(self->scratch + self->scratch_len) = '/';
                self->scratch_len++;
                break;
            }
            case 'b': {
                *(self->scratch + self->scratch_len) = '\b';
                self->scratch_len++;
                break;
            }
            case 'f': {
                *(self->scratch + self->scratch_len) = '\f';
                self->scratch_len++;
                break;
            }
            case 'u': {
                *is_ascii = false;
                if (json_handle_unicode_escape(self) < 0) return -1;
                break;
            }
            default:
                json_err_invalid(self, "invalid escape character in string");
                return -1;
        }

        start = self->input_pos;
    }
    else if (c == '"') {
        if (json_scratch_extend(self, start, self->input_pos - start) < 0) return -1;
        self->input_pos++;
        *out = (char *)(self->scratch);
        return self->scratch_len;
    }
    else {
        json_err_invalid(self, "invalid character");
        return -1;
    }

    /* Loop until `"`, `\`, or a non-ascii character */
    while (self->input_end - self->input_pos >= 8) {
        repeat8(parse_ascii_pre);
        self->input_pos += 8;
        continue;
        repeat8(parse_ascii_post);
    }
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
        if (MS_UNLIKELY(char_is_special_or_nonascii(*self->input_pos))) break;
        self->input_pos++;
    }

parse_ascii_end:
    OPT_FORCE_RELOAD(*self->input_pos);

    if (MS_UNLIKELY(*self->input_pos & 0x80)) {
        *is_ascii = false;
        /* Loop until `"` or `\` */
        while (self->input_end - self->input_pos >= 8) {
            repeat8(parse_unicode_pre);
            self->input_pos += 8;
            continue;
            repeat8(parse_unicode_post);
        }
        while (true) {
            if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
            if (MS_UNLIKELY(char_is_special(*self->input_pos))) break;
            self->input_pos++;
        }
    }
parse_unicode_end:
    goto top;
}

static Py_ssize_t
json_decode_string_view(JSONDecoderState *self, char **out, bool *is_ascii) {
    self->input_pos++; /* Skip '"' */
    unsigned char *start = self->input_pos;

    /* Loop until `"`, `\`, or a non-ascii character */
    while (self->input_end - self->input_pos >= 8) {
        repeat8(parse_ascii_pre);
        self->input_pos += 8;
        continue;
        repeat8(parse_ascii_post);
    }
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
        if (MS_UNLIKELY(char_is_special_or_nonascii(*self->input_pos))) break;
        self->input_pos++;
    }

parse_ascii_end:
    OPT_FORCE_RELOAD(*self->input_pos);

    if (MS_LIKELY(*self->input_pos == '"')) {
        Py_ssize_t size = self->input_pos - start;
        self->input_pos++;
        *out = (char *)start;
        return size;
    }

    if (MS_UNLIKELY(*self->input_pos & 0x80)) {
        *is_ascii = false;
        /* Loop until `"` or `\` */
        while (self->input_end - self->input_pos >= 8) {
            repeat8(parse_unicode_pre);
            self->input_pos += 8;
            continue;
            repeat8(parse_unicode_post);
        }
        while (true) {
            if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
            if (MS_UNLIKELY(char_is_special(*self->input_pos))) break;
            self->input_pos++;
        }
    }

parse_unicode_end:
    OPT_FORCE_RELOAD(*self->input_pos);

    if (MS_LIKELY(*self->input_pos == '"')) {
        Py_ssize_t size = self->input_pos - start;
        self->input_pos++;
        *out = (char *)start;
        return size;
    }

    return json_decode_string_view_copy(self, out, is_ascii, start);
}

static int
json_skip_string(JSONDecoderState *self) {
    self->input_pos++; /* Skip '"' */

parse_unicode:
    /* Loop until `"` or `\` */
    while (self->input_end - self->input_pos >= 8) {
        repeat8(parse_unicode_pre);
        self->input_pos += 8;
        continue;
        repeat8(parse_unicode_post);
    }
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
        if (MS_UNLIKELY(char_is_special(*self->input_pos))) break;
        self->input_pos++;
    }

parse_unicode_end:
    OPT_FORCE_RELOAD(*self->input_pos);

    if (MS_LIKELY(*self->input_pos == '"')) {
        self->input_pos++;
        return 0;
    }
    else if (*self->input_pos == '\\') {
        self->input_pos++;
        if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();

        switch (*self->input_pos) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                self->input_pos++;
                break;
            case 'u': {
                self->input_pos++;
                unsigned int cp;
                if (json_read_codepoint(self, &cp) < 0) return -1;

                if (0xDC00 <= cp && cp <= 0xDFFF) {
                    json_err_invalid(self, "invalid utf-16 surrogate pair");
                    return -1;
                }
                else if (0xD800 <= cp && cp <= 0xDBFF) {
                    /* utf-16 pair, parse 2nd pair */
                    unsigned int cp2;
                    if (!json_remaining(self, 6)) return ms_err_truncated();
                    if (self->input_pos[0] != '\\' || self->input_pos[1] != 'u') {
                        json_err_invalid(self, "unexpected end of hex escape");
                        return -1;
                    }
                    self->input_pos += 2;
                    if (json_read_codepoint(self, &cp2) < 0) return -1;
                    if (cp2 < 0xDC00 || cp2 > 0xDFFF) {
                        json_err_invalid(self, "invalid utf-16 surrogate pair");
                        return -1;
                    }
                    cp = 0x10000 + (((cp - 0xD800) << 10) | (cp2 - 0xDC00));
                }
                break;
            }
            default: {
                json_err_invalid(self, "invalid escaped character");
                return -1;
            }
        }
        goto parse_unicode;
    }
    else {
        json_err_invalid(self, "invalid character");
        return -1;
    }
}


#undef repeat8
#undef parse_ascii_pre
#undef parse_ascii_post
#undef parse_unicode_pre
#undef parse_unicode_post

/* A table of the corresponding base64 value for each character, or -1 if an
 * invalid character in the base64 alphabet (note the padding char '=' is
 * handled elsewhere, so is marked as invalid here as well) */
static const uint8_t base64_decode_table[] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63,
    52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-1,-1,-1,
    -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
    -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
    41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
};

static PyObject *
json_decode_binary(
    const char *buffer, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    PyObject *out = NULL;
    char *bin_buffer;
    Py_ssize_t bin_size, i;

    if (size % 4 != 0) goto invalid;

    int npad = 0;
    if (size > 0 && buffer[size - 1] == '=') npad++;
    if (size > 1 && buffer[size - 2] == '=') npad++;

    bin_size = (size / 4) * 3 - npad;
    if (!ms_passes_bytes_constraints(bin_size, type, path)) return NULL;

    if (type->types & MS_TYPE_BYTES) {
        out = PyBytes_FromStringAndSize(NULL, bin_size);
        if (out == NULL) return NULL;
        bin_buffer = PyBytes_AS_STRING(out);
    }
    else {
        out = PyByteArray_FromStringAndSize(NULL, bin_size);
        if (out == NULL) return NULL;
        bin_buffer = PyByteArray_AS_STRING(out);
    }

    int quad = 0;
    uint8_t left_c = 0;
    for (i = 0; i < size - npad; i++) {
        uint8_t c = base64_decode_table[(uint8_t)(buffer[i])];
        if (c >= 64) goto invalid;

        switch (quad) {
            case 0:
                quad = 1;
                left_c = c;
                break;
            case 1:
                quad = 2;
                *bin_buffer++ = (left_c << 2) | (c >> 4);
                left_c = c & 0x0f;
                break;
            case 2:
                quad = 3;
                *bin_buffer++ = (left_c << 4) | (c >> 2);
                left_c = c & 0x03;
                break;
            case 3:
                quad = 0;
                *bin_buffer++ = (left_c << 6) | c;
                left_c = 0;
                break;
        }
    }
    return out;

invalid:
    Py_XDECREF(out);
    return ms_error_with_path("Invalid base64 encoded string%U", path);
}

static bool
json_decode_int_from_str_inner(
    const char *p, Py_ssize_t size, bool err_invalid,
    TypeNode *type, PathNode *path, PyObject **out
) {
    /* This function signature has gotten kinda weird due to being shared
     * between `json_decode` and `from_builtins`. Read the comments below for
     * more info */
    uint64_t mantissa = 0;
    bool is_negative = false;
    const char *end = p + size;

    if (size == 0) goto invalid;

    char c = *p;
    if (c == '-') {
        p++;
        is_negative = true;
        if (p == end) goto invalid;
        c = *p;
    }

    if (MS_UNLIKELY(c == '0')) {
        /* Value is either 0 or invalid */
        p++;
        if (p == end) goto done;
        goto invalid;
    }

    /* We can read the first 19 digits safely into a uint64 without checking
     * for overflow. */
    size_t remaining = end - p;
    const char *safe_end = p + Py_MIN(19, remaining);
    while (p < safe_end) {
        c = *p;
        if (!is_digit(c)) goto end_digits;
        p++;
        mantissa = mantissa * 10 + (uint64_t)(c - '0');
    }
    if (MS_UNLIKELY(remaining > 19)) {
        /* Reading a 20th digit may or may not cause overflow. Any additional
         * digits definitely will. Read the 20th digit (and check for a 21st),
         * erroring upon overflow. */
        c = *p;
        if (MS_UNLIKELY(is_digit(c))) {
            p++;
            uint64_t mantissa2 = mantissa * 10 + (uint64_t)(c - '0');
            bool out_of_range = (
                (mantissa2 < mantissa) ||
                ((mantissa2 - (uint64_t)(c - '0')) / 10) != mantissa ||
                (p != end)
            );
            if (out_of_range) goto out_of_range;
            mantissa = mantissa2;
        }
    }

end_digits:
    /* There must be at least one digit */
    if (MS_UNLIKELY(mantissa == 0)) goto invalid;

    /* Check for trailing characters */
    if (p != end) goto invalid;

done:
    if (MS_UNLIKELY(is_negative)) {
        if (MS_UNLIKELY(mantissa > 1ull << 63)) {
            goto out_of_range;
        }
        if (MS_LIKELY(type->types & MS_TYPE_INT)) {
            *out = ms_decode_int(-1 * (int64_t)mantissa, type, path);
            return true;
        }
        *out = ms_decode_int_enum_or_literal_int64(-1 * (int64_t)mantissa, type, path);
        return true;
    }
    if (MS_LIKELY(type->types & MS_TYPE_INT)) {
        *out = ms_decode_uint(mantissa, type, path);
        return true;
    }
    *out = ms_decode_int_enum_or_literal_uint64(mantissa, type, path);
    return true;

out_of_range:
    *out = NULL;
    ms_error_with_path("Integer value out of range%U", path);
    return true;

invalid:
    /* An `invalid` error occurs when the string is not a valid integer. When
     * parsing a union of types from a string (in `from_builtins` with
     * `str_values=True`) we want to avoid raising an `invalid` error here, so
     * other types in the union can be tried. If the string is a valid integer,
     * but fails for other reasons (out of range, constraint issues, ...) then
     * an error is still raised in this function.
     */
    if (err_invalid) {
        *out = NULL;
        ms_error_with_path("Invalid integer string%U", path);
        return true;
    }
    /* `false` indicates no return value and no error raised */
    return false;
}

static PyObject *
json_decode_int_from_str(
    const char *p, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    PyObject *out;
    json_decode_int_from_str_inner(p, size, true, type, path, &out);
    return out;
}

static PyObject *
json_decode_string(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    if (
        MS_LIKELY(
            type->types & (
                MS_TYPE_ANY | MS_TYPE_STR | MS_TYPE_ENUM | MS_TYPE_STRLITERAL |
                MS_TYPE_BYTES | MS_TYPE_BYTEARRAY |
                MS_TYPE_DATETIME | MS_TYPE_DATE | MS_TYPE_TIME |
                MS_TYPE_UUID | MS_TYPE_DECIMAL
            )
        )
    ) {
        char *view = NULL;
        bool is_ascii = true;
        Py_ssize_t size = json_decode_string_view(self, &view, &is_ascii);
        if (size < 0) return NULL;
        if (MS_LIKELY(type->types & (MS_TYPE_STR | MS_TYPE_ANY))) {
            PyObject *out;
            if (MS_LIKELY(is_ascii)) {
                out = PyUnicode_New(size, 127);
                memcpy(ascii_get_buffer(out), view, size);
            }
            else {
                out = PyUnicode_DecodeUTF8(view, size, NULL);
            }
            return ms_check_str_constraints(out, type, path);
        }
        else if (MS_UNLIKELY(type->types & MS_TYPE_DATETIME)) {
            return ms_decode_datetime(view, size, type, path);
        }
        else if (MS_UNLIKELY(type->types & MS_TYPE_DATE)) {
            return ms_decode_date(view, size, path);
        }
        else if (MS_UNLIKELY(type->types & MS_TYPE_TIME)) {
            return ms_decode_time(view, size, type, path);
        }
        else if (MS_UNLIKELY(type->types & MS_TYPE_UUID)) {
            return ms_decode_uuid(view, size, path);
        }
        else if (MS_UNLIKELY(type->types & MS_TYPE_DECIMAL)) {
            return ms_decode_decimal(view, size, is_ascii, path);
        }
        else if (MS_UNLIKELY(type->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY))) {
            return json_decode_binary(view, size, type, path);
        }
        else {
            return ms_decode_str_enum_or_literal(view, size, type, path);
        }
    }
    return ms_validation_error("str", type, path);
}

static PyObject *
json_decode_dict_key_fallback(
    const char *view, Py_ssize_t size, bool is_ascii, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_STR | MS_TYPE_ANY)) {
        PyObject *out;
        if (is_ascii) {
            out = PyUnicode_New(size, 127);
            if (MS_UNLIKELY(out == NULL)) return NULL;
            memcpy(ascii_get_buffer(out), view, size);
        }
        else {
            out = PyUnicode_DecodeUTF8(view, size, NULL);
        }
        return ms_check_str_constraints(out, type, path);
    }
    if (type->types & (MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        return json_decode_int_from_str(view, size, type, path);
    }
    else if (type->types & (MS_TYPE_ENUM | MS_TYPE_STRLITERAL)) {
        return ms_decode_str_enum_or_literal(view, size, type, path);
    }
    else if (type->types & MS_TYPE_UUID) {
        return ms_decode_uuid(view, size, path);
    }
    else if (type->types & MS_TYPE_DATETIME) {
        return ms_decode_datetime(view, size, type, path);
    }
    else if (type->types & MS_TYPE_DATE) {
        return ms_decode_date(view, size, path);
    }
    else if (type->types & MS_TYPE_TIME) {
        return ms_decode_time(view, size, type, path);
    }
    else if (type->types & MS_TYPE_DECIMAL) {
        return ms_decode_decimal(view, size, is_ascii, path);
    }
    else if (type->types & MS_TYPE_BYTES) {
        return json_decode_binary(view, size, type, path);
    }
    else {
        return ms_err_unreachable();
    }
}

static PyObject *
json_decode_dict_key(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    bool is_ascii = true;
    char *view = NULL;
    Py_ssize_t size;
    bool is_str = type->types == MS_TYPE_ANY || type->types == MS_TYPE_STR;

    size = json_decode_string_view(self, &view, &is_ascii);
    if (size < 0) return NULL;

    bool cacheable = is_str && is_ascii && size > 0 && size <= STRING_CACHE_MAX_STRING_LENGTH;
    if (MS_UNLIKELY(!cacheable)) {
        return json_decode_dict_key_fallback(view, size, is_ascii, type, path);
    }

    uint32_t hash = murmur2(view, size);
    uint32_t index = hash % STRING_CACHE_SIZE;
    PyObject *existing = string_cache[index];

    if (MS_LIKELY(existing != NULL)) {
        Py_ssize_t e_size = ((PyASCIIObject *)existing)->length;
        char *e_str = ascii_get_buffer(existing);
        if (MS_LIKELY(size == e_size && memcmp(view, e_str, size) == 0)) {
            Py_INCREF(existing);
            return existing;
        }
    }

    /* Create a new ASCII str object */
    PyObject *new = PyUnicode_New(size, 127);
    if (MS_UNLIKELY(new == NULL)) return NULL;
    memcpy(ascii_get_buffer(new), view, size);

    /* Swap out the str in the cache */
    Py_XDECREF(existing);
    Py_INCREF(new);
    string_cache[index] = new;
    return new;
}

static PyObject *
json_decode_list(JSONDecoderState *self, TypeNode *type, TypeNode *el_type, PathNode *path) {
    PyObject *out, *item = NULL;
    unsigned char c;
    bool first = true;
    PathNode el_path = {path, 0, NULL};

    self->input_pos++; /* Skip '[' */

    out = PyList_New(0);
    if (out == NULL) return NULL;
    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Parse item */
        item = json_decode(self, el_type, &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Append item to list */
        if (PyList_Append(out, item) < 0) goto error;
        Py_CLEAR(item);
    }

    if (MS_UNLIKELY(!ms_passes_array_constraints(PyList_GET_SIZE(out), type, path))) {
        goto error;
    }

    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    Py_XDECREF(item);
    return NULL;
}

static PyObject *
json_decode_set(
    JSONDecoderState *self, TypeNode *type, TypeNode *el_type, PathNode *path
) {
    PyObject *out, *item = NULL;
    unsigned char c;
    bool first = true;
    PathNode el_path = {path, 0, NULL};

    self->input_pos++; /* Skip '[' */

    out = (type->types & MS_TYPE_SET) ?  PySet_New(NULL) : PyFrozenSet_New(NULL);
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Parse item */
        item = json_decode(self, el_type, &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Append item to set */
        if (PySet_Add(out, item) < 0) goto error;
        Py_CLEAR(item);
    }

    if (MS_UNLIKELY(!ms_passes_array_constraints(PySet_GET_SIZE(out), type, path))) {
        goto error;
    }

    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    Py_XDECREF(item);
    return NULL;
}

static PyObject *
json_decode_vartuple(JSONDecoderState *self, TypeNode *type, TypeNode *el_type, PathNode *path) {
    PyObject *list, *item, *out = NULL;
    Py_ssize_t size, i;

    list = json_decode_list(self, type, el_type, path);
    if (list == NULL) return NULL;

    size = PyList_GET_SIZE(list);
    out = PyTuple_New(size);
    if (out != NULL) {
        for (i = 0; i < size; i++) {
            item = PyList_GET_ITEM(list, i);
            PyTuple_SET_ITEM(out, i, item);
            PyList_SET_ITEM(list, i, NULL);  /* Drop reference in old list */
        }
    }
    Py_DECREF(list);
    return out;
}

static PyObject *
json_decode_fixtuple(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    PyObject *out, *item;
    unsigned char c;
    bool first = true;
    PathNode el_path = {path, 0, NULL};
    Py_ssize_t i = 0, offset, fixtuple_size;

    TypeNode_get_fixtuple(type, &offset, &fixtuple_size);

    self->input_pos++; /* Skip '[' */

    out = PyTuple_New(fixtuple_size);
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }

    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            if (MS_UNLIKELY(i < fixtuple_size)) goto size_error;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Check we don't have too many elements */
        if (MS_UNLIKELY(i >= fixtuple_size)) goto size_error;

        /* Parse item */
        item = json_decode(self, type->details[offset + i].pointer, &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Add item to tuple */
        PyTuple_SET_ITEM(out, i, item);
        i++;
    }
    Py_LeaveRecursiveCall();
    return out;

size_error:
    ms_raise_validation_error(
        path,
        "Expected `array` of length %zd%U",
        fixtuple_size
    );
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_namedtuple(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    unsigned char c;
    bool first = true;
    Py_ssize_t nfields, ndefaults, nrequired;
    NamedTupleInfo *info = TypeNode_get_namedtuple_info(type);

    nfields = Py_SIZE(info);
    ndefaults = info->defaults == NULL ? 0 : PyTuple_GET_SIZE(info->defaults);
    nrequired = nfields - ndefaults;

    self->input_pos++; /* Skip '[' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyTypeObject *nt_type = (PyTypeObject *)(info->class);
    PyObject *out = nt_type->tp_alloc(nt_type, nfields);
    if (out == NULL) goto error;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyTuple_SET_ITEM(out, i, NULL);
    }

    Py_ssize_t i = 0;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            if (MS_UNLIKELY(i < nrequired)) goto size_error;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Check we don't have too many elements */
        if (MS_UNLIKELY(i >= nfields)) goto size_error;

        /* Parse item */
        PathNode el_path = {path, i, NULL};
        PyObject *item = json_decode(self, info->types[i], &el_path);
        if (item == NULL) goto error;

        /* Add item to tuple */
        PyTuple_SET_ITEM(out, i, item);
        i++;
    }
    Py_LeaveRecursiveCall();

    /* Fill in defaults */
    for (; i < nfields; i++) {
        PyObject *item = PyTuple_GET_ITEM(info->defaults, i - nrequired);
        Py_INCREF(item);
        PyTuple_SET_ITEM(out, i, item);
    }

    return out;

size_error:
    if (ndefaults == 0) {
        ms_raise_validation_error(
            path,
            "Expected `array` of length %zd%U",
            nfields
        );
    }
    else {
        ms_raise_validation_error(
            path,
            "Expected `array` of length %zd to %zd%U",
            nrequired,
            nfields
        );
    }
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_struct_array_inner(
    JSONDecoderState *self, StructMetaObject *st_type, PathNode *path,
    Py_ssize_t starting_index
) {
    Py_ssize_t nfields, ndefaults, nrequired, npos, i = 0;
    PyObject *out, *item = NULL;
    unsigned char c;
    bool is_gc, should_untrack;
    bool first = starting_index == 0;
    PathNode item_path = {path, starting_index};

    out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) return NULL;

    nfields = PyTuple_GET_SIZE(st_type->struct_encode_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    nrequired = nfields - st_type->n_trailing_defaults;
    npos = nfields - ndefaults;
    is_gc = MS_TYPE_IS_GC(st_type);
    should_untrack = is_gc;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        if (MS_LIKELY(i < nfields)) {
            /* Parse item */
            item = json_decode(self, st_type->struct_types[i], &item_path);
            if (MS_UNLIKELY(item == NULL)) goto error;
            Struct_set_index(out, i, item);
            if (should_untrack) {
                should_untrack = !MS_OBJ_IS_GC(item);
            }
            i++;
            item_path.index++;
        }
        else {
            if (MS_UNLIKELY(st_type->forbid_unknown_fields == OPT_TRUE)) {
                ms_raise_validation_error(
                    path,
                    "Expected `array` of at most length %zd",
                    nfields
                );
                goto error;
            }
            else {
                /* Skip trailing fields */
                if (json_skip(self) < 0) goto error;
            }
        }
    }

    /* Check for missing required fields */
    if (i < nrequired) {
        ms_raise_validation_error(
            path,
            "Expected `array` of at least length %zd, got %zd%U",
            nrequired + starting_index,
            i + starting_index
        );
        goto error;
    }
    /* Fill in missing fields with defaults */
    for (; i < nfields; i++) {
        item = get_default(
            PyTuple_GET_ITEM(st_type->struct_defaults, i - npos)
        );
        if (item == NULL) goto error;
        Struct_set_index(out, i, item);
        if (should_untrack) {
            should_untrack = !MS_OBJ_IS_GC(item);
        }
    }
    Py_LeaveRecursiveCall();
    if (is_gc && !should_untrack)
        PyObject_GC_Track(out);
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

/* Decode an integer. If the value fits in an int64_t, it will be stored in
 * `out`, otherwise it will be stored in `uout`. A return value of -1 indicates
 * an error. */
static int
json_decode_cint(JSONDecoderState *self, int64_t *out, uint64_t *uout, PathNode *path) {
    uint64_t mantissa = 0;
    bool is_negative = false;
    unsigned char c;
    unsigned char *orig_input_pos = self->input_pos;

    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
        is_negative = true;
    }

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        c = json_peek_or_null(self);
        if (MS_UNLIKELY(is_digit(c))) {
            json_err_invalid(self, "invalid number");
            return -1;
        }
    }
    else {
        /* Parse the integer part of the number.
         *
         * We can read the first 19 digits safely into a uint64 without
         * checking for overflow. Removing overflow checks from the loop gives
         * a measurable performance boost. */
        size_t remaining = self->input_end - self->input_pos;
        size_t n_safe = Py_MIN(19, remaining);
        while (n_safe) {
            c = *self->input_pos;
            if (!is_digit(c)) goto end_integer;
            self->input_pos++;
            n_safe--;
            mantissa = mantissa * 10 + (uint64_t)(c - '0');
        }
        if (MS_UNLIKELY(remaining > 19)) {
            /* Reading a 20th digit may or may not cause overflow. Any
             * additional digits definitely will. Read the 20th digit (and
             * check for a 21st), taking the slow path upon overflow. */
            c = *self->input_pos;
            if (MS_UNLIKELY(is_digit(c))) {
                self->input_pos++;
                uint64_t mantissa2 = mantissa * 10 + (uint64_t)(c - '0');
                bool overflowed = (mantissa2 < mantissa) || ((mantissa2 - (uint64_t)(c - '0')) / 10) != mantissa;
                if (MS_UNLIKELY(overflowed || is_digit(json_peek_or_null(self)))) {
                    goto error_not_int;
                }
                mantissa = mantissa2;
                c = json_peek_or_null(self);
            }
        }

end_integer:
        /* There must be at least one digit */
        if (MS_UNLIKELY(mantissa == 0)) goto error_not_int;
    }

    if (c == '.' || c == 'e' || c == 'E') goto error_not_int;

    if (is_negative) {
        if (mantissa > 1ull << 63) goto error_not_int;
        *out = -1 * (int64_t)mantissa;
    }
    else {
        if (mantissa > LLONG_MAX) {
            *uout = mantissa;
        }
        else {
            *out = mantissa;
        }
    }
    return 0;

error_not_int:
    /* Use skip to catch malformed JSON */
    self->input_pos = orig_input_pos;
    if (json_skip(self) < 0) return -1;

    ms_error_with_path("Expected `int`%U", path);
    return -1;
}

static Py_ssize_t
json_decode_cstr(JSONDecoderState *self, char **out, PathNode *path) {
    unsigned char c;
    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;
    if (c != '"') {
        /* Use skip to catch malformed JSON */
        if (json_skip(self) < 0) return -1;
        /* JSON is valid but the wrong type */
        ms_error_with_path("Expected `str`%U", path);
        return -1;
    }
    bool is_ascii = true;
    return json_decode_string_view(self, out, &is_ascii);
}

static int
json_ensure_array_nonempty(
    JSONDecoderState *self, StructMetaObject *st_type, PathNode *path
) {
    unsigned char c;
    /* Check for an early end to the array */
    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;
    if (c == ']') {
        Py_ssize_t expected_size;
        if (st_type == NULL) {
            /* If we don't know the type, the most we know is that the minimum
             * size is 1 */
            expected_size = 1;
        }
        else {
            /* n_fields - n_optional_fields + 1 tag */
            expected_size = PyTuple_GET_SIZE(st_type->struct_encode_fields)
                            - PyTuple_GET_SIZE(st_type->struct_defaults)
                            + 1;
        }
        ms_raise_validation_error(
            path,
            "Expected `array` of at least length %zd, got 0%U",
            expected_size
        );
        return -1;
    }
    return 0;
}

static int
json_ensure_tag_matches(
    JSONDecoderState *self, PathNode *path, PyObject *expected_tag
) {
    if (PyUnicode_CheckExact(expected_tag)) {
        char *tag = NULL;
        Py_ssize_t tag_size;
        tag_size = json_decode_cstr(self, &tag, path);
        if (tag_size < 0) return -1;

        /* Check that tag matches expected tag value */
        Py_ssize_t expected_size;
        const char *expected_str = unicode_str_and_size_nocheck(
            expected_tag, &expected_size
        );
        if (tag_size != expected_size || memcmp(tag, expected_str, expected_size) != 0) {
            /* Tag doesn't match the expected value, error nicely */
            ms_invalid_cstr_value(tag, tag_size, path);
            return -1;
        }
    }
    else {
        int64_t tag = 0;
        uint64_t utag = 0;
        if (json_decode_cint(self, &tag, &utag, path) < 0) return -1;
        int64_t expected = PyLong_AsLongLong(expected_tag);
        /* Tags must be int64s, if utag != 0 then we know the tags don't match.
         * We parse the full uint64 value only to validate the message and
         * raise a nice error */
        if (utag != 0) {
            ms_invalid_cuint_value(utag, path);
            return -1;
        }
        if (tag != expected) {
            ms_invalid_cint_value(tag, path);
            return -1;
        }
    }
    return 0;
}

static StructMetaObject *
json_decode_tag_and_lookup_type(
    JSONDecoderState *self, Lookup *lookup, PathNode *path
) {
    StructMetaObject *out = NULL;
    if (Lookup_IsStrLookup(lookup)) {
        Py_ssize_t tag_size;
        char *tag = NULL;
        tag_size = json_decode_cstr(self, &tag, path);
        if (tag_size < 0) return NULL;
        out = (StructMetaObject *)StrLookup_Get((StrLookup *)lookup, tag, tag_size);
        if (out == NULL) {
            ms_invalid_cstr_value(tag, tag_size, path);
        }
    }
    else {
        int64_t tag = 0;
        uint64_t utag = 0;
        if (json_decode_cint(self, &tag, &utag, path) < 0) return NULL;
        if (utag == 0) {
            out = (StructMetaObject *)IntLookup_GetInt64((IntLookup *)lookup, tag);
            if (out == NULL) {
                ms_invalid_cint_value(tag, path);
            }
        }
        else {
            /* tags can't be uint64 values, we only decode to give a nice error */
            ms_invalid_cuint_value(utag, path);
        }
    }
    return out;
}

static PyObject *
json_decode_struct_array(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    Py_ssize_t starting_index = 0;
    StructMetaObject *st_type = TypeNode_get_struct(type);

    self->input_pos++; /* Skip '[' */

    /* If this is a tagged struct, first read and validate the tag */
    if (st_type->struct_tag_value != NULL) {
        PathNode tag_path = {path, 0};
        if (json_ensure_array_nonempty(self, st_type, path) < 0) return NULL;
        if (json_ensure_tag_matches(self, &tag_path, st_type->struct_tag_value) < 0) return NULL;
        starting_index = 1;
    }

    /* Decode the rest of the struct */
    return json_decode_struct_array_inner(self, st_type, path, starting_index);
}

static PyObject *
json_decode_struct_array_union(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    PathNode tag_path = {path, 0};
    Lookup *lookup = TypeNode_get_struct_union(type);

    self->input_pos++; /* Skip '[' */
    /* Decode & lookup struct type from tag */
    if (json_ensure_array_nonempty(self, NULL, path) < 0) return NULL;
    StructMetaObject *struct_type = json_decode_tag_and_lookup_type(self, lookup, &tag_path);
    if (struct_type == NULL) return NULL;

    /* Finish decoding the rest of the struct */
    return json_decode_struct_array_inner(self, struct_type, path, 1);
}

static PyObject *
json_decode_array(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        return json_decode_list(self, type, &type_any, path);
    }
    else if (type->types & MS_TYPE_LIST) {
        return json_decode_list(self, type, TypeNode_get_array(type), path);
    }
    else if (type->types & (MS_TYPE_SET | MS_TYPE_FROZENSET)) {
        return json_decode_set(self, type, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return json_decode_vartuple(self, type, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_FIXTUPLE) {
        return json_decode_fixtuple(self, type, path);
    }
    else if (type->types & MS_TYPE_NAMEDTUPLE) {
        return json_decode_namedtuple(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY) {
        return json_decode_struct_array(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY_UNION) {
        return json_decode_struct_array_union(self, type, path);
    }
    return ms_validation_error("array", type, path);
}

static PyObject *
json_decode_dict(
    JSONDecoderState *self, TypeNode *type, TypeNode *key_type, TypeNode *val_type, PathNode *path
) {
    PyObject *out, *key = NULL, *val = NULL;
    unsigned char c;
    bool first = true;
    PathNode key_path = {path, PATH_KEY, NULL};
    PathNode val_path = {path, PATH_ELLIPSIS, NULL};

    self->input_pos++; /* Skip '{' */

    out = PyDict_New();
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            key = json_decode_dict_key(self, key_type, &key_path);
            if (key == NULL) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        val = json_decode(self, val_type, &val_path);
        if (val == NULL) goto error;

        /* Add item to dict */
        if (MS_UNLIKELY(PyDict_SetItem(out, key, val) < 0))
            goto error;
        Py_CLEAR(key);
        Py_CLEAR(val);
    }

    if (MS_UNLIKELY(!ms_passes_map_constraints(PyDict_GET_SIZE(out), type, path))) goto error;

    Py_LeaveRecursiveCall();
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(key);
    Py_XDECREF(val);
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_typeddict(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    PyObject *out;
    unsigned char c;
    char *key = NULL;
    bool first = true;
    Py_ssize_t key_size, nrequired = 0, pos = 0;
    TypedDictInfo *info = TypeNode_get_typeddict_info(type);

    self->input_pos++; /* Skip '{' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    out = PyDict_New();
    if (out == NULL) goto error;

    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            bool is_ascii = true;
            key_size = json_decode_string_view(self, &key, &is_ascii);
            if (key_size < 0) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        TypeNode *field_type;
        PyObject *field = TypedDictInfo_lookup_key(info, key, key_size, &field_type, &pos);

        if (field != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = json_decode(self, field_type, &field_path);
            if (val == NULL) goto error;
            /* We want to keep a count of required fields we've decoded. Since
             * duplicates can occur, we stash the current dict size, then only
             * increment if the dict size has changed _and_ the field is
             * required. */
            Py_ssize_t cur_size = PyDict_GET_SIZE(out);
            int status = PyDict_SetItem(out, field, val);
            /* Always decref value, no need to decref key since it's a borrowed
             * reference. */
            Py_DECREF(val);
            if (status < 0) goto error;
            if ((PyDict_GET_SIZE(out) != cur_size) && (field_type->types & MS_EXTRA_FLAG)) {
                nrequired++;
            }
        }
        else {
            /* Skip unknown fields */
            if (json_skip(self) < 0) goto error;
        }
    }
    if (nrequired < info->nrequired) {
        /* A required field is missing, determine which one and raise */
        TypedDictInfo_error_missing(info, out, path);
        goto error;
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_dataclass(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    PyObject *out;
    unsigned char c;
    char *key = NULL;
    bool first = true;
    Py_ssize_t key_size, pos = 0;
    DataclassInfo *info = TypeNode_get_dataclass_info(type);

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyTypeObject *dataclass_type = (PyTypeObject *)(info->class);
    out = dataclass_type->tp_alloc(dataclass_type, 0);
    if (out == NULL) goto error;

    self->input_pos++; /* Skip '{' */

    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            bool is_ascii = true;
            key_size = json_decode_string_view(self, &key, &is_ascii);
            if (key_size < 0) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        TypeNode *field_type;
        PyObject *field = DataclassInfo_lookup_key(info, key, key_size, &field_type, &pos);

        if (field != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = json_decode(self, field_type, &field_path);
            if (val == NULL) goto error;
            int status = PyObject_SetAttr(out, field, val);
            Py_DECREF(val);
            if (status < 0) goto error;
        }
        else {
            /* Skip unknown fields */
            if (json_skip(self) < 0) goto error;
        }
    }
    if (DataclassInfo_post_decode(info, out, path) < 0) goto error;

    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
json_decode_struct_map_inner(
    JSONDecoderState *self, StructMetaObject *st_type, PathNode *path,
    Py_ssize_t starting_index
) {
    PyObject *out, *val = NULL;
    Py_ssize_t key_size, field_index, pos = 0;
    unsigned char c;
    char *key = NULL;
    bool first = starting_index == 0;
    PathNode field_path = {path, 0, (PyObject *)st_type};

    out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            bool is_ascii = true;
            key_size = json_decode_string_view(self, &key, &is_ascii);
            if (key_size < 0) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        field_index = StructMeta_get_field_index(st_type, key, key_size, &pos);
        if (MS_LIKELY(field_index >= 0)) {
            field_path.index = field_index;
            TypeNode *type = st_type->struct_types[field_index];
            val = json_decode(self, type, &field_path);
            if (val == NULL) goto error;
            Py_INCREF(val);
            Struct_set_index(out, field_index, val);
        }
        else if (MS_UNLIKELY(field_index == -2)) {
            /* Decode and check that the tag value matches the expected value */
            PathNode tag_path = {path, PATH_STR, st_type->struct_tag_field};
            if (json_ensure_tag_matches(self, &tag_path, st_type->struct_tag_value) < 0) {
                goto error;
            }
        }
        else {
            /* Unknown field */
            if (MS_UNLIKELY(st_type->forbid_unknown_fields == OPT_TRUE)) {
                ms_error_unknown_field(key, key_size, path);
                goto error;
            }
            else {
                if (json_skip(self) < 0) goto error;
            }
        }
    }
    if (Struct_fill_in_defaults(st_type, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_struct_map(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    StructMetaObject *st_type = TypeNode_get_struct(type);

    self->input_pos++; /* Skip '{' */

    return json_decode_struct_map_inner(self, st_type, path, 0);
}

static PyObject *
json_decode_struct_union(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    PathNode tag_path = {path, PATH_STR, Lookup_tag_field(lookup)};
    Py_ssize_t tag_field_size;
    const char *tag_field = unicode_str_and_size_nocheck(
        Lookup_tag_field(lookup), &tag_field_size
    );

    self->input_pos++; /* Skip '{' */

    /* Cache the current input position in case we need to reset it once the
     * tag is found */
    unsigned char *orig_input_pos = self->input_pos;

    for (Py_ssize_t i = 0; ; i++) {
        unsigned char c;

        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && (i != 0)) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;
        }
        else if (i != 0) {
            return json_err_invalid(self, "expected ',' or '}'");
        }

        /* Parse a string key */
        Py_ssize_t key_size;
        char *key = NULL;
        if (c == '"') {
            bool is_ascii = true;
            key_size = json_decode_string_view(self, &key, &is_ascii);
            if (key_size < 0) return NULL;
        }
        else if (c == '}') {
            return json_err_invalid(self, "trailing comma in object");
        }
        else {
            return json_err_invalid(self, "object keys must be strings");
        }

        /* Check if key matches tag_field */
        bool tag_found = false;
        if (key_size == tag_field_size && memcmp(key, tag_field, key_size) == 0) {
            tag_found = true;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;
        if (c != ':') {
            return json_err_invalid(self, "expected ':'");
        }
        self->input_pos++;

        /* Parse value */
        if (tag_found) {
            /* Decode & lookup struct type from tag */
            StructMetaObject *st_type = json_decode_tag_and_lookup_type(self, lookup, &tag_path);
            if (st_type == NULL) return NULL;
            if (i != 0) {
                /* tag wasn't first field, reset decoder position */
                self->input_pos = orig_input_pos;
            }
            return json_decode_struct_map_inner(self, st_type, path, i == 0 ? 1 : 0);
        }
        else {
            if (json_skip(self) < 0) return NULL;
        }
    }

    ms_raise_validation_error(
        path,
        "Object missing required field `%U`%U",
        Lookup_tag_field(lookup)
    );
    return NULL;
}

static PyObject *
json_decode_object(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        return json_decode_dict(self, type, &type_any, &type_any, path);
    }
    else if (type->types & MS_TYPE_DICT) {
        TypeNode *key, *val;
        TypeNode_get_dict(type, &key, &val);
        return json_decode_dict(self, type, key, val, path);
    }
    else if (type->types & MS_TYPE_TYPEDDICT) {
        return json_decode_typeddict(self, type, path);
    }
    else if (type->types & MS_TYPE_DATACLASS) {
        return json_decode_dataclass(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        return json_decode_struct_map(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_UNION) {
        return json_decode_struct_union(self, type, path);
    }
    return ms_validation_error("object", type, path);
}

static MS_NOINLINE PyObject *
json_decode_extended_float(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    uint32_t nd = 0;
    int32_t dp = 0;

    ms_hpd dec;
    dec.num_digits = 0;
    dec.decimal_point = 0;
    dec.negative = false;
    dec.truncated = false;

    /* We know there is at least one byte available when this function is
     * called */
    char c = *self->input_pos;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
        dec.negative = true;
    }

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        /* This _can't_ happen, since it would have been caught in the fast routine first:
        c = json_peek_or_null(self);
        if (MS_UNLIKELY(is_digit(c))) return json_err_invalid(self, "invalid number");
        */
    }
    else {
        /* Parse the integer part of the number. */
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            if (MS_LIKELY(nd < MS_HPD_MAX_DIGITS)) {
                dec.digits[nd++] = (uint8_t)(c - '0');
            }
            else if (c != '0') {
                dec.truncated = true;
            }
            dp++;
        }
        /* This _can't_ happen, since it would have been caught in the fast routine first */
        /*if (MS_UNLIKELY(nd == 0)) return json_err_invalid(self, "invalid character");*/
    }

    c = json_peek_or_null(self);
    if (c == '.') {
        self->input_pos++;
        /* Parse remaining digits until invalid/unknown character */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            if (c == '0') {
                if (nd == 0) {
                    /* Track leading zeros implicitly */
                    dp--;
                }
                else if (nd < MS_HPD_MAX_DIGITS) {
                    dec.digits[nd++] = (uint8_t)(c - '0');
                }
            }
            else if ('1' <= c && c <= '9') {
                if (nd < MS_HPD_MAX_DIGITS) {
                    dec.digits[nd++] = (uint8_t)(c - '0');
                }
                else {
                    dec.truncated = true;
                }
            }
        }
        /* Error if no digits after decimal */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) return json_err_invalid(self, "invalid number");

        c = json_peek_or_null(self);
    }
    if (c == 'e' || c == 'E') {
        self->input_pos++;

        int64_t exp_sign = 1, exp_part = 0;

        c = json_peek_or_null(self);
        /* Parse exponent sign (if any) */
        if (c == '+') {
            self->input_pos++;
        }
        else if (c == '-') {
            self->input_pos++;
            exp_sign = -1;
        }

        /* Parse exponent digits */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            if (MS_LIKELY(exp_part < 922337203685477580)) {  /* won't overflow int64_t */
                exp_part = (int64_t)(exp_part * 10) + (c - '0');
            }
        }

        /* Error if no digits in exponent */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) return json_err_invalid(self, "invalid number");

        dp += exp_sign * exp_part;
    }

    dec.num_digits = nd;
    if (dp < -MS_HPD_DP_RANGE) {
        dec.decimal_point = -(MS_HPD_DP_RANGE + 1);
    }
    else if (dp > MS_HPD_DP_RANGE) {
        dec.decimal_point = (MS_HPD_DP_RANGE + 1);
    }
    else {
        dec.decimal_point = dp;
    }
    ms_hpd_trim(&dec);
    double res = ms_hpd_to_double(&dec);
    if (Py_IS_INFINITY(res)) {
        return ms_error_with_path("Number out of range%U", path);
    }
    return ms_decode_float(res, type, path);
}

static PyObject *
json_maybe_decode_number(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    uint64_t mantissa = 0;
    int32_t exponent = 0;
    bool is_negative = false;
    bool is_float = false;

    unsigned char *initial_pos = self->input_pos;

    /* We know there is at least one byte available when this function is
     * called */
    char c = *self->input_pos;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
        is_negative = true;
    }

    unsigned char *first_digit_pos = self->input_pos;

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        c = json_peek_or_null(self);
        if (MS_UNLIKELY(is_digit(c))) return json_err_invalid(self, "invalid number");
    }
    else {
        /* Parse the integer part of the number.
         *
         * We can read the first 19 digits safely into a uint64 without
         * checking for overflow. Removing overflow checks from the loop gives
         * a measurable performance boost. */
        size_t remaining = self->input_end - self->input_pos;
        size_t n_safe = Py_MIN(19, remaining);
        while (n_safe) {
            c = *self->input_pos;
            if (!is_digit(c)) goto end_integer;
            self->input_pos++;
            n_safe--;
            mantissa = mantissa * 10 + (uint64_t)(c - '0');
        }
        if (MS_UNLIKELY(remaining > 19)) {
            /* Reading a 20th digit may or may not cause overflow. Any
             * additional digits definitely will. Read the 20th digit (and
             * check for a 21st), taking the slow path upon overflow. */
            c = *self->input_pos;
            if (MS_UNLIKELY(is_digit(c))) {
                self->input_pos++;
                uint64_t mantissa2 = mantissa * 10 + (uint64_t)(c - '0');
                bool overflowed = (mantissa2 < mantissa) || ((mantissa2 - (uint64_t)(c - '0')) / 10) != mantissa;
                if (MS_UNLIKELY(overflowed || is_digit(json_peek_or_null(self)))) {
                    goto fallback_extended;
                }
                mantissa = mantissa2;
                c = json_peek_or_null(self);
            }
        }

end_integer:
        /* There must be at least one digit */
        if (MS_UNLIKELY(mantissa == 0)) return json_err_invalid(self, "invalid character");
    }

    if (c == '.') {
        self->input_pos++;
        is_float = true;
        /* Parse remaining digits until invalid/unknown character */
        unsigned char *first_dec_digit = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            mantissa = mantissa * 10 + (uint64_t)(c - '0');
        }
        /* Error if no digits after decimal */
        if (MS_UNLIKELY(first_dec_digit == self->input_pos)) return json_err_invalid(self, "invalid number");
        exponent = first_dec_digit - self->input_pos;

        c = json_peek_or_null(self);

        /* This is may be an overestimate of significant digits, only fix if it matters */
        uint32_t ndigits = self->input_pos - first_digit_pos;
        if (MS_UNLIKELY(ndigits > 19)) {
            /* Fix ndigits estimate by trimming leading zeros/decimal */
            const uint8_t* p = first_digit_pos;
            while (*p == '0' || *p == '.') p++;
            ndigits -= (p - first_digit_pos);
            if (ndigits > 19) {
                goto fallback_extended;
            }
        }
    }

    if (c == 'e' || c == 'E') {
        int32_t exp_sign = 1, exp_part = 0;
        self->input_pos++;
        is_float = true;

        c = json_peek_or_null(self);
        /* Parse exponent sign (if any) */
        if (c == '+') {
            self->input_pos++;
        }
        else if (c == '-') {
            self->input_pos++;
            exp_sign = -1;
        }

        /* Parse exponent digits */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            c = *self->input_pos++;
            if (MS_LIKELY(exp_part < 10000)) {
                exp_part = (int32_t)(exp_part * 10) + (uint32_t)(c - '0');
            }
        }

        /* Error if no digits in exponent */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) return json_err_invalid(self, "invalid number");

        exponent += exp_sign * exp_part;
    }

    if (MS_UNLIKELY(is_negative && mantissa > 1ull << 63)) {
        is_float = true;
    }

    if (!is_float) {
        if (MS_LIKELY(type->types & (MS_TYPE_ANY | MS_TYPE_INT))) {
            if (is_negative) {
                return ms_decode_int(-1 * (int64_t)mantissa, type, path);
            }
            return ms_decode_uint(mantissa, type, path);
        }
        else if (MS_UNLIKELY(type->types & (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL))) {
            if (is_negative) {
                return ms_decode_int_enum_or_literal_int64(-1 * (int64_t)mantissa, type, path);
            }
            return ms_decode_int_enum_or_literal_uint64(mantissa, type, path);
        }
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)) {
        double val;
        if (MS_UNLIKELY(!reconstruct_double(mantissa, exponent, is_negative, &val))) {
            goto fallback_extended;
        }
        return ms_decode_float(val, type, path);
    }
    if (!is_float) {
        return ms_validation_error("int", type, path);
    }
    return ms_validation_error("float", type, path);

fallback_extended:
    self->input_pos = initial_pos;
    if (MS_UNLIKELY(!(type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)))) {
        return ms_validation_error("float", type, path);
    }
    return json_decode_extended_float(self, type, path);
}

static MS_NOINLINE PyObject *
json_decode_raw(JSONDecoderState *self) {
    unsigned char c;
    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;
    const unsigned char *start = self->input_pos;
    if (json_skip(self) < 0) return NULL;
    Py_ssize_t size = self->input_pos - start;
    return Raw_FromView(self->buffer_obj, (char *)start, size);
}

static MS_INLINE PyObject *
json_decode_nocustom(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    unsigned char c;

    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;

    switch (c) {
        case 'n': return json_decode_none(self, type, path);
        case 't': return json_decode_true(self, type, path);
        case 'f': return json_decode_false(self, type, path);
        case '[': return json_decode_array(self, type, path);
        case '{': return json_decode_object(self, type, path);
        case '"': return json_decode_string(self, type, path);
        default: return json_maybe_decode_number(self, type, path);
    }
}

static PyObject *
json_decode(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    if (MS_UNLIKELY(type->types == 0)) {
        return json_decode_raw(self);
    }
    PyObject *obj = json_decode_nocustom(self, type, path);
    if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
        return ms_decode_custom(obj, self->dec_hook, type, path);
    }
    return obj;
}

static int
json_skip_ident(JSONDecoderState *self, const char *ident, size_t len) {
    self->input_pos++;  /* Already checked first char */
    if (MS_UNLIKELY(!json_remaining(self, len))) return ms_err_truncated();
    if (memcmp(self->input_pos, ident, len) != 0) {
        json_err_invalid(self, "invalid character");
        return -1;
    }
    self->input_pos += len;
    return 0;
}

static int
json_skip_array(JSONDecoderState *self) {
    unsigned char c;
    bool first = true;
    int out = -1;

    self->input_pos++; /* Skip '[' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c == ']') {
            self->input_pos++;
            out = 0;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        }
        else if (first) {
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            break;
        }
        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            break;
        }

        if (json_skip(self) < 0) break;
    }
    Py_LeaveRecursiveCall();
    return out;
}

static int
json_skip_object(JSONDecoderState *self) {
    unsigned char c;
    bool first = true;
    int out = -1;

    self->input_pos++; /* Skip '{' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c == '}') {
            self->input_pos++;
            out = 0;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        }
        else if (first) {
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            break;
        }

        /* Skip key */
        if (c == '"') {
            if (json_skip_string(self) < 0) break;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            break;
        }
        else {
            json_err_invalid(self, "expected '\"'");
            break;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            break;
        }
        self->input_pos++;

        /* Skip value */
        if (json_skip(self) < 0) break;
    }
    Py_LeaveRecursiveCall();
    return out;
}

static int
json_maybe_skip_number(JSONDecoderState *self) {
    /* We know there is at least one byte available when this function is
     * called */
    char c = *self->input_pos;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
    }

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        c = json_peek_or_null(self);
        if (MS_UNLIKELY(is_digit(c))) {
            json_err_invalid(self, "invalid number");
            return -1;
        }
    }
    else {
        /* Skip the integer part of the number. */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* There must be at least one digit */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid character");
            return -1;
        }
    }

    c = json_peek_or_null(self);
    if (c == '.') {
        self->input_pos++;
        /* Skip remaining digits until invalid/unknown character */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* Error if no digits after decimal */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid number");
            return -1;
        }

        c = json_peek_or_null(self);
    }
    if (c == 'e' || c == 'E') {
        self->input_pos++;

        /* Parse exponent sign (if any) */
        c = json_peek_or_null(self);
        if (c == '+' || c == '-') {
            self->input_pos++;
        }

        /* Parse exponent digits */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* Error if no digits in exponent */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid number");
            return -1;
        }
    }
    return 0;
}

static int
json_skip(JSONDecoderState *self)
{
    unsigned char c;

    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;

    switch (c) {
        case 'n': return json_skip_ident(self, "ull", 3);
        case 't': return json_skip_ident(self, "rue", 3);
        case 'f': return json_skip_ident(self, "alse", 4);
        case '"': return json_skip_string(self);
        case '[': return json_skip_array(self);
        case '{': return json_skip_object(self);
        default: return json_maybe_skip_number(self);
    }
}

static int
json_format(
    JSONDecoderState *, EncoderState *,
    Py_ssize_t indent, Py_ssize_t cur_indent
);

static int
json_write_indent(EncoderState *self, Py_ssize_t indent, Py_ssize_t cur_indent) {
    if (indent <= 0) return 0;
    if (ms_ensure_space(self, cur_indent + 1) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p++ = '\n';
    for (Py_ssize_t i = 0; i < cur_indent; i++) {
        *p++ = ' ';
    }
    self->output_len += cur_indent + 1;
    return 0;
}

static int
json_format_array(
    JSONDecoderState *dec, EncoderState *enc,
    Py_ssize_t indent, Py_ssize_t cur_indent
) {
    unsigned char c;
    bool first = true;
    int out = -1;
    Py_ssize_t el_indent = cur_indent + indent;

    dec->input_pos++; /* Skip '[' */
    if (ms_write(enc, "[", 1) < 0) return -1;

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(dec, &c))) break;
        if (c == ']') {
            dec->input_pos++;
            if (!first) {
                if (MS_UNLIKELY(json_write_indent(enc, indent, cur_indent) < 0)) break;
            }
            out = ms_write(enc, "]", 1);
            break;
        }
        else if (c == ',' && !first) {
            dec->input_pos++;
            if (indent == 0) {
                if (MS_UNLIKELY(ms_write(enc, ", ", 2) < 0)) break;
            } else {
                if (MS_UNLIKELY(ms_write(enc, ",", 1) < 0)) break;
            }
            if (MS_UNLIKELY(!json_peek_skip_ws(dec, &c))) break;
        }
        else if (first) {
            first = false;
        }
        else {
            json_err_invalid(dec, "expected ',' or ']'");
            break;
        }
        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(dec, "trailing comma in array");
            break;
        }

        if (json_write_indent(enc, indent, el_indent) < 0) break;
        if (json_format(dec, enc, indent, el_indent) < 0) break;
    }
    Py_LeaveRecursiveCall();
    return out;
}

static int
json_format_object(
    JSONDecoderState *dec, EncoderState *enc,
    Py_ssize_t indent, Py_ssize_t cur_indent
) {
    unsigned char c;
    bool first = true;
    int out = -1;
    Py_ssize_t el_indent = cur_indent + indent;

    dec->input_pos++; /* Skip '{' */
    if (ms_write(enc, "{", 1) < 0) return -1;

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(dec, &c))) break;
        if (c == '}') {
            dec->input_pos++;
            if (!first) {
                if (MS_UNLIKELY(json_write_indent(enc, indent, cur_indent) < 0)) break;
            }
            out = ms_write(enc, "}", 1);
            break;
        }
        else if (c == ',' && !first) {
            dec->input_pos++;
            if (indent == 0) {
                if (MS_UNLIKELY(ms_write(enc, ", ", 2) < 0)) break;
            } else {
                if (MS_UNLIKELY(ms_write(enc, ",", 1) < 0)) break;
            }
            if (MS_UNLIKELY(!json_peek_skip_ws(dec, &c))) break;
        }
        else if (first) {
            first = false;
        }
        else {
            json_err_invalid(dec, "expected ',' or '}'");
            break;
        }

        if (c == '"') {
            if (json_write_indent(enc, indent, el_indent) < 0) break;
            if (json_format(dec, enc, indent, el_indent) < 0) break;
        }
        else if (c == '}') {
            json_err_invalid(dec, "trailing comma in object");
            break;
        }
        else {
            json_err_invalid(dec, "expected '\"'");
            break;
        }

        if (MS_UNLIKELY(!json_peek_skip_ws(dec, &c))) break;
        if (c != ':') {
            json_err_invalid(dec, "expected ':'");
            break;
        }
        dec->input_pos++;
        if (indent >= 0) {
            if (ms_write(enc, ": ", 2) < 0) break;
        }
        else {
            if (ms_write(enc, ":", 1) < 0) break;
        }

        if (json_format(dec, enc, indent, el_indent) < 0) break;
    }
    Py_LeaveRecursiveCall();
    return out;
}

static int
json_format(
    JSONDecoderState *dec, EncoderState *enc,
    Py_ssize_t indent, Py_ssize_t cur_indent
) {
    unsigned char c;
    if (MS_UNLIKELY(!json_peek_skip_ws(dec, &c))) return -1;

    if (c == '[') {
        return json_format_array(dec, enc, indent, cur_indent);
    }
    else if (c == '{') {
        return json_format_object(dec, enc, indent, cur_indent);
    }
    else {
        unsigned char *start = dec->input_pos;
        if (json_skip(dec) < 0) return -1;
        unsigned char *end = dec->input_pos;
        return ms_write(enc, (char *)start, end - start);
    }
}

PyDoc_STRVAR(msgspec_json_format__doc__,
"json_format(buf, *, indent=2)\n"
"--\n"
"\n"
"Format an existing JSON message, usually to be more human readable.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like or str\n"
"    The JSON message to format.\n"
"indent : int, optional\n"
"    How many spaces to indent for a single indentation level. Defaults to 2.\n"
"    Set to 0 to format the message as a single line, with spaces added between\n"
"    items for readability. Set to a negative number to strip all unnecessary\n"
"    whitespace, minimizing the message size.\n"
"\n"
"Returns\n"
"-------\n"
"output : bytes or str\n"
"    The formatted JSON message. Returns a str if input is a str, bytes otherwise."
);
static PyObject*
msgspec_json_format(PyObject *self, PyObject *args, PyObject *kwargs)
{
    int status;
    Py_buffer buffer;
    PyObject *out = NULL, *buf = NULL;
    static char *kwlist[] = {"buf", "indent", NULL};
    Py_ssize_t indent = 2;

    /* Parse arguments */
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$n", kwlist, &buf, &indent))
        return NULL;
    if (indent < 0) {
        indent = -1;
    }

    buffer.buf = NULL;
    if (ms_get_buffer(buf, &buffer) >= 0) {
        JSONDecoderState dec;
        EncoderState enc;

        /* Init decoder */
        dec.dec_hook = NULL;
        dec.type = NULL;
        dec.scratch = NULL;
        dec.scratch_capacity = 0;
        dec.scratch_len = 0;
        dec.buffer_obj = buf;
        dec.input_start = buffer.buf;
        dec.input_pos = buffer.buf;
        dec.input_end = dec.input_pos + buffer.len;

        /* Init encoder */
        enc.mod = msgspec_get_global_state();
        enc.enc_hook = NULL;
        if (indent >= 0) {
            /* Assume pretty-printing will take at least as much space as the
             * input. This is true unless there's existing whitespace. */
            enc.write_buffer_size = buffer.len;
        }
        else {
            enc.write_buffer_size = 512;
        }
        enc.output_len = 0;
        enc.max_output_len = enc.write_buffer_size;
        enc.output_buffer = PyBytes_FromStringAndSize(NULL, enc.max_output_len);
        if (enc.output_buffer != NULL) {
            enc.output_buffer_raw = PyBytes_AS_STRING(enc.output_buffer);
            enc.resize_buffer = &ms_resize_bytes;

            status = json_format(&dec, &enc, indent, 0);

            if (status == 0 && json_has_trailing_characters(&dec)) {
                status = -1;
            }

            if (status == 0) {
                if (PyUnicode_CheckExact(buf)) {
                    /* str input, str output */
                    out = PyUnicode_FromStringAndSize(
                        enc.output_buffer_raw,
                        enc.output_len
                    );
                    Py_CLEAR(enc.output_buffer);
                }
                else {
                    /* Trim output to length */
                    out = enc.output_buffer;
                    FAST_BYTES_SHRINK(out, enc.output_len);
                }
            } else {
                /* Error, drop buffer */
                Py_CLEAR(enc.output_buffer);
            }
        }

        ms_release_buffer(buf, &buffer);
    }

    return out;
}


PyDoc_STRVAR(JSONDecoder_decode__doc__,
"decode(self, buf)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like or str\n"
"    The message to decode.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object.\n"
);
static PyObject*
JSONDecoder_decode(JSONDecoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    PyObject *res = NULL;
    Py_buffer buffer;
    buffer.buf = NULL;

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }

    if (ms_get_buffer(args[0], &buffer) >= 0) {

        self->state.buffer_obj = args[0];
        self->state.input_start = buffer.buf;
        self->state.input_pos = buffer.buf;
        self->state.input_end = self->state.input_pos + buffer.len;

        res = json_decode(&(self->state), self->state.type, NULL);

        if (res != NULL && json_has_trailing_characters(&self->state)) {
            Py_CLEAR(res);
        }

        ms_release_buffer(args[0], &buffer);

        self->state.buffer_obj = NULL;
        self->state.input_start = NULL;
        self->state.input_pos = NULL;
        self->state.input_end = NULL;
        json_scratch_reset(&(self->state));
    }

    return res;
}

static struct PyMethodDef JSONDecoder_methods[] = {
    {
        "decode", (PyCFunction) JSONDecoder_decode, METH_FASTCALL,
        JSONDecoder_decode__doc__,
    },
    {NULL, NULL}                /* sentinel */
};

static PyMemberDef JSONDecoder_members[] = {
    {"type", T_OBJECT_EX, offsetof(JSONDecoder, orig_type), READONLY, "The Decoder type"},
    {"dec_hook", T_OBJECT, offsetof(JSONDecoder, state.dec_hook), READONLY, "The Decoder dec_hook"},
    {NULL},
};

static PyTypeObject JSONDecoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.json.Decoder",
    .tp_doc = JSONDecoder__doc__,
    .tp_basicsize = sizeof(JSONDecoder),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)JSONDecoder_init,
    .tp_traverse = (traverseproc)JSONDecoder_traverse,
    .tp_dealloc = (destructor)JSONDecoder_dealloc,
    .tp_repr = (reprfunc)JSONDecoder_repr,
    .tp_methods = JSONDecoder_methods,
    .tp_members = JSONDecoder_members,
};

PyDoc_STRVAR(msgspec_json_decode__doc__,
"json_decode(buf, *, type='Any', dec_hook=None)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like or str\n"
"    The message to decode.\n"
"type : type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default JSON types.\n"
"dec_hook : callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic JSON types. This hook should transform ``obj`` into type\n"
"    ``type``, or raise a ``TypeError`` if unsupported.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object.\n"
"\n"
"See Also\n"
"--------\n"
"Decoder.decode"
);
static PyObject*
msgspec_json_decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *res = NULL, *buf = NULL, *type = NULL, *dec_hook = NULL;
    JSONDecoderState state;
    Py_buffer buffer;
    MsgspecState *st = msgspec_get_global_state();

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    buf = args[0];
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if ((type = find_keyword(kwnames, args + nargs, st->str_type)) != NULL) nkwargs--;
        if ((dec_hook = find_keyword(kwnames, args + nargs, st->str_dec_hook)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    /* Handle dec_hook */
    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    if (dec_hook != NULL) {
        if (!PyCallable_Check(dec_hook)) {
            PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
            return NULL;
        }
    }
    state.dec_hook = dec_hook;

    /* Init scratch space */
    state.scratch = NULL;
    state.scratch_capacity = 0;
    state.scratch_len = 0;

    /* Allocated Any & Struct type nodes (simple, common cases) on the stack,
     * everything else on the heap */
    state.type = NULL;
    if (type == NULL || type == st->typing_any) {
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        if (StructMeta_prep_types(type, true, NULL) < 0) return NULL;
    }
    else {
        state.type = TypeNode_Convert(type, true, NULL);
        if (state.type == NULL) return NULL;
    }

    buffer.buf = NULL;
    if (ms_get_buffer(buf, &buffer) >= 0) {
        state.buffer_obj = buf;
        state.input_start = buffer.buf;
        state.input_pos = buffer.buf;
        state.input_end = state.input_pos + buffer.len;

        if (state.type != NULL) {
            res = json_decode(&state, state.type, NULL);
        }
        else if (type == NULL || type == st->typing_any) {
            TypeNode type_any = {MS_TYPE_ANY};
            res = json_decode(&state, &type_any, NULL);
        }
        else {
            bool array_like = ((StructMetaObject *)type)->array_like == OPT_TRUE;
            TypeNodeSimple type_obj;
            type_obj.types = array_like ? MS_TYPE_STRUCT_ARRAY : MS_TYPE_STRUCT;
            type_obj.details[0].pointer = type;
            res = json_decode(&state, (TypeNode*)(&type_obj), NULL);
        }

        if (res != NULL && json_has_trailing_characters(&state)) {
            Py_CLEAR(res);
        }

        ms_release_buffer(buf, &buffer);
    }

    PyMem_Free(state.scratch);

    if (state.type != NULL) {
        TypeNode_Free(state.type);
    }

    return res;
}

/*************************************************************************
 * to_builtins                                                           *
 *************************************************************************/

#define MS_BUILTIN_BYTES      (1ull << 0)
#define MS_BUILTIN_BYTEARRAY  (1ull << 1)
#define MS_BUILTIN_MEMORYVIEW (1ull << 2)
#define MS_BUILTIN_DATETIME   (1ull << 3)
#define MS_BUILTIN_DATE       (1ull << 4)
#define MS_BUILTIN_TIME       (1ull << 5)
#define MS_BUILTIN_UUID       (1ull << 6)
#define MS_BUILTIN_DECIMAL    (1ull << 7)

typedef struct {
    MsgspecState *mod;
    PyObject *enc_hook;
    bool str_keys;
    uint32_t builtin_types;
} ToBuiltinsState;

static PyObject * to_builtins(ToBuiltinsState *, PyObject *, bool);

static PyObject *
to_builtins_enum(ToBuiltinsState *self, PyObject *obj)
{
    PyObject *value = PyObject_GetAttr(obj, self->mod->str__value_);
    if (value == NULL) return NULL;
    if (PyLong_CheckExact(value) || PyUnicode_CheckExact(value)) {
        return value;
    }
    Py_DECREF(value);
    PyErr_SetString(
        self->mod->EncodeError,
        "Only enums with int or str values are supported"
    );
    return NULL;
}

static PyObject *
to_builtins_binary(ToBuiltinsState *self, const char *buf, Py_ssize_t size) {
    Py_ssize_t output_size = ms_encode_base64_size(self->mod, size);
    if (output_size < 0) return NULL;
    PyObject *out = PyUnicode_New(output_size, 127);
    if (out == NULL) return NULL;
    ms_encode_base64(buf, size, ascii_get_buffer(out));
    return out;
}

static PyObject *
to_builtins_datetime(ToBuiltinsState *self, PyObject *obj) {
    char buf[32];
    int size = ms_encode_datetime(self->mod, obj, buf);
    if (size < 0) return NULL;
    PyObject *out = PyUnicode_New(size, 127);
    memcpy(ascii_get_buffer(out), buf, size);
    return out;
}

static PyObject *
to_builtins_date(ToBuiltinsState *self, PyObject *obj) {
    PyObject *out = PyUnicode_New(10, 127);
    if (out == NULL) return NULL;
    ms_encode_date(obj, ascii_get_buffer(out));
    return out;
}

static PyObject *
to_builtins_time(ToBuiltinsState *self, PyObject *obj) {
    char buf[21];
    int size = ms_encode_time(self->mod, obj, buf);
    if (size < 0) return NULL;
    PyObject *out = PyUnicode_New(size, 127);
    memcpy(ascii_get_buffer(out), buf, size);
    return out;
}

static PyObject *
to_builtins_uuid(ToBuiltinsState *self, PyObject *obj) {
    PyObject *out = PyUnicode_New(36, 127);
    if (out == NULL) return NULL;
    if (ms_encode_uuid(self->mod, obj, ascii_get_buffer(out)) < 0) {
        Py_CLEAR(out);
    }
    return out;
}

static PyObject *
to_builtins_decimal(ToBuiltinsState *self, PyObject *obj) {
    return PyObject_Str(obj);
}

static PyObject *
to_builtins_list(ToBuiltinsState *self, PyObject *obj) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    Py_ssize_t size = PyList_GET_SIZE(obj);
    PyObject *out = PyList_New(size);
    if (out == NULL) goto cleanup;
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = PyList_GET_ITEM(obj, i);
        PyObject *new = to_builtins(self, item, false);
        if (new == NULL) {
            Py_CLEAR(out);
            goto cleanup;
        }
        PyList_SET_ITEM(out, i, new);
    }

cleanup:
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
to_builtins_tuple(ToBuiltinsState *self, PyObject *obj, bool is_key) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    Py_ssize_t size = PyTuple_GET_SIZE(obj);
    PyObject *out = PyTuple_New(size);
    if (out == NULL) goto cleanup;
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = PyTuple_GET_ITEM(obj, i);
        PyObject *new = to_builtins(self, item, is_key);
        if (new == NULL) {
            Py_CLEAR(out);
            goto cleanup;
        }
        PyTuple_SET_ITEM(out, i, new);
    }
cleanup:
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
to_builtins_set(ToBuiltinsState *self, PyObject *obj, bool is_key) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    Py_ssize_t size = PySet_GET_SIZE(obj);
    Py_hash_t hash;
    PyObject *item, *out;

    out = is_key ? PyTuple_New(size) : PyList_New(size);
    if (out == NULL) goto cleanup;

    Py_ssize_t i = 0, pos = 0;
    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        PyObject *new = to_builtins(self, item, is_key);
        if (new == NULL) {
            Py_CLEAR(out);
            goto cleanup;
        }
        if (is_key) {
            PyTuple_SET_ITEM(out, i, new);
        }
        else {
            PyList_SET_ITEM(out, i, new);
        }
        i++;
    }
cleanup:
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
to_builtins_dict(ToBuiltinsState *self, PyObject *obj) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    PyObject *new_key = NULL, *new_val = NULL, *key, *val;
    bool ok = false;
    PyObject *out = PyDict_New();
    if (out == NULL) goto cleanup;

    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key, &val)) {
        new_key = to_builtins(self, key, true);
        if (new_key == NULL) goto cleanup;
        if (self->str_keys) {
            if (PyLong_CheckExact(new_key)) {
                PyObject *temp = PyObject_Str(new_key);
                if (temp == NULL) goto cleanup;
                Py_DECREF(new_key);
                new_key = temp;
            }
            else if (!PyUnicode_CheckExact(new_key)) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "Only dicts with `str` or `int` keys are supported"
                );
                goto cleanup;
            }
        }
        new_val = to_builtins(self, val, false);
        if (new_val == NULL) goto cleanup;
        if (PyDict_SetItem(out, new_key, new_val) < 0) goto cleanup;
        Py_CLEAR(new_key);
        Py_CLEAR(new_val);
    }
    ok = true;

cleanup:
    Py_LeaveRecursiveCall();
    if (!ok) {
        Py_CLEAR(out);
        Py_XDECREF(new_key);
        Py_XDECREF(new_val);
    }
    return out;
}

static PyObject *
to_builtins_struct(ToBuiltinsState *self, PyObject *obj, bool is_key) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    bool ok = false;
    PyObject *out = NULL;
    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(obj);
    PyObject *tag_field = struct_type->struct_tag_field;
    PyObject *tag_value = struct_type->struct_tag_value;
    PyObject *fields = struct_type->struct_encode_fields;
    PyObject *defaults = struct_type->struct_defaults;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    Py_ssize_t npos = nfields - PyTuple_GET_SIZE(defaults);
    bool omit_defaults = struct_type->omit_defaults == OPT_TRUE;

    if (struct_type->array_like == OPT_TRUE) {
        Py_ssize_t tagged = (tag_value != NULL);
        Py_ssize_t size = nfields + tagged;
        if (is_key) {
            out = PyTuple_New(size);
        }
        else {
            out = PyList_New(size);
        }
        if (out == NULL) goto cleanup;

        if (tagged) {
            Py_INCREF(tag_value);
            if (is_key) {
                PyTuple_SET_ITEM(out, 0, tag_value);
            }
            else {
                PyList_SET_ITEM(out, 0, tag_value);
            }
        }
        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *val = Struct_get_index(obj, i);
            if (val == NULL) goto cleanup;
            PyObject *val2 = to_builtins(self, val, is_key);
            if (val2 == NULL) goto cleanup;
            Py_INCREF(val2);
            if (is_key) {
                PyTuple_SET_ITEM(out, i + tagged, val2);
            }
            else {
                PyList_SET_ITEM(out, i + tagged, val2);
            }
        }
    }
    else {
        out = PyDict_New();
        if (out == NULL) goto cleanup;
        if (tag_value != NULL) {
            if (PyDict_SetItem(out, tag_field, tag_value) < 0) goto cleanup;
        }
        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *key = PyTuple_GET_ITEM(fields, i);
            PyObject *val = Struct_get_index(obj, i);
            if (val == NULL) goto cleanup;
            if (
                !omit_defaults ||
                i < npos ||
                !is_default(val, PyTuple_GET_ITEM(defaults, i - npos))
            ) {
                PyObject *val2 = to_builtins(self, val, false);
                if (val2 == NULL) goto cleanup;
                int status = PyDict_SetItem(out, key, val2);
                Py_DECREF(val2);
                if (status < 0) goto cleanup;
            }
        }
    }
    ok = true;

cleanup:
    Py_LeaveRecursiveCall();
    if (!ok) {
        Py_CLEAR(out);
    }
    return out;
}

static PyObject*
to_builtins_object(ToBuiltinsState *self, PyObject *obj) {
    bool ok = false;
    PyObject *dict = NULL, *out = NULL;

    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    out = PyDict_New();
    if (out == NULL) goto cleanup;

    /* First encode everything in `__dict__` */
    dict = PyObject_GenericGetDict(obj, NULL);
    if (MS_UNLIKELY(dict == NULL)) {
        PyErr_Clear();
    }
    else {
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(dict, &pos, &key, &val)) {
            if (MS_LIKELY(PyUnicode_CheckExact(key))) {
                Py_ssize_t key_len;
                const char* key_buf = unicode_str_and_size(key, &key_len);
                if (MS_UNLIKELY(key_buf == NULL)) goto cleanup;
                if (MS_UNLIKELY(*key_buf == '_')) continue;

                PyObject *val2 = to_builtins(self, val, false);
                if (val2 == NULL) goto cleanup;
                int status = PyDict_SetItem(out, key, val2);
                Py_DECREF(val2);
                if (status < 0) goto cleanup;
            }
        }
    }
    /* Then encode everything in slots */
    PyTypeObject *type = Py_TYPE(obj);
    while (type != NULL) {
        Py_ssize_t n = Py_SIZE(type);
        if (n) {
            PyMemberDef *mp = MS_PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
            for (Py_ssize_t i = 0; i < n; i++, mp++) {
                if (MS_LIKELY(mp->type == T_OBJECT_EX && !(mp->flags & READONLY))) {
                    char *addr = (char *)obj + mp->offset;
                    PyObject *val = *(PyObject **)addr;
                    if (MS_UNLIKELY(val == NULL)) continue;
                    if (MS_UNLIKELY(*mp->name == '_')) continue;

                    PyObject *key = PyUnicode_InternFromString(mp->name);
                    if (key == NULL) goto cleanup;

                    int status = -1;
                    PyObject *val2 = to_builtins(self, val, false);
                    if (val2 != NULL) {
                        status = PyDict_SetItem(out, key, val2);
                        Py_DECREF(val2);
                    }
                    Py_DECREF(key);
                    if (status < 0) goto cleanup;
                }
            }
        }
        type = type->tp_base;
    }
    ok = true;

cleanup:
    Py_XDECREF(dict);
    Py_LeaveRecursiveCall();
    if (!ok) {
        Py_CLEAR(out);
    }
    return out;
}

static PyObject *
to_builtins(ToBuiltinsState *self, PyObject *obj, bool is_key) {
    PyTypeObject *type = Py_TYPE(obj);

    if (
        obj == Py_None ||
        type == &PyBool_Type ||
        type == &PyLong_Type ||
        type == &PyFloat_Type ||
        type == &PyUnicode_Type
    ) {
        goto builtin;
    }
    else if (type == &PyBytes_Type) {
        if (self->builtin_types & MS_BUILTIN_BYTES) goto builtin;
        return to_builtins_binary(
            self, PyBytes_AS_STRING(obj), PyBytes_GET_SIZE(obj)
        );
    }
    else if (type == &PyByteArray_Type) {
        if (self->builtin_types & MS_BUILTIN_BYTEARRAY) goto builtin;
        return to_builtins_binary(
            self, PyByteArray_AS_STRING(obj), PyByteArray_GET_SIZE(obj)
        );
    }
    else if (type == &PyMemoryView_Type) {
        if (self->builtin_types & MS_BUILTIN_MEMORYVIEW) goto builtin;
        PyObject *out;
        Py_buffer buffer;
        if (PyObject_GetBuffer(obj, &buffer, PyBUF_CONTIG_RO) < 0) return NULL;
        out = to_builtins_binary(self, buffer.buf, buffer.len);
        PyBuffer_Release(&buffer);
        return out;
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        if (self->builtin_types & MS_BUILTIN_DATETIME) goto builtin;
        return to_builtins_datetime(self, obj);
    }
    else if (type == PyDateTimeAPI->DateType) {
        if (self->builtin_types & MS_BUILTIN_DATE) goto builtin;
        return to_builtins_date(self, obj);
    }
    else if (type == PyDateTimeAPI->TimeType) {
        if (self->builtin_types & MS_BUILTIN_TIME) goto builtin;
        return to_builtins_time(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->UUIDType)) {
        if (self->builtin_types & MS_BUILTIN_UUID) goto builtin;
        return to_builtins_uuid(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->DecimalType)) {
        if (self->builtin_types & MS_BUILTIN_DECIMAL) goto builtin;
        return to_builtins_decimal(self, obj);
    }
    else if (PyList_Check(obj)) {
        return to_builtins_list(self, obj);
    }
    else if (PyTuple_Check(obj)) {
        return to_builtins_tuple(self, obj, is_key);
    }
    else if (PyDict_Check(obj)) {
        return to_builtins_dict(self, obj);
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        return to_builtins_struct(self, obj, is_key);
    }
    else if (Py_TYPE(type) == self->mod->EnumMetaType) {
        return to_builtins_enum(self, obj);
    }
    else if (PyAnySet_Check(obj)) {
        return to_builtins_set(self, obj, is_key);
    }
    else if (PyDict_Contains(type->tp_dict, self->mod->str___dataclass_fields__)) {
        return to_builtins_object(self, obj);
    }
    else if (self->enc_hook != NULL) {
        PyObject *out = NULL;
        PyObject *temp;
        temp = CALL_ONE_ARG(self->enc_hook, obj);
        if (temp == NULL) return NULL;
        if (!Py_EnterRecursiveCall(" while serializing an object")) {
            out = to_builtins(self, temp, is_key);
            Py_LeaveRecursiveCall();
        }
        Py_DECREF(temp);
        return out;
    }
    else {
        ms_encode_err_type_unsupported(type);
        return NULL;
    }

builtin:
    Py_INCREF(obj);
    return obj;
}

static int
ms_process_builtin_types(MsgspecState *mod, PyObject *builtin_types, uint32_t *mask) {
    if (builtin_types != NULL && builtin_types != Py_None) {
        PyObject *seq = PySequence_Fast(
            builtin_types, "builtin_types must be an iterable of types"
        );
        if (seq == NULL) return -1;
        Py_ssize_t size = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject *type = PySequence_Fast_GET_ITEM(seq, i);
            if (type == (PyObject *)(&PyBytes_Type)) {
                *mask |= MS_BUILTIN_BYTES;
            }
            else if (type == (PyObject *)(&PyByteArray_Type)) {
                *mask |= MS_BUILTIN_BYTEARRAY;
            }
            else if (type == (PyObject *)(&PyMemoryView_Type)) {
                *mask |= MS_BUILTIN_MEMORYVIEW;
            }
            else if (type == (PyObject *)(PyDateTimeAPI->DateTimeType)) {
                *mask |= MS_BUILTIN_DATETIME;
            }
            else if (type == (PyObject *)(PyDateTimeAPI->DateType)) {
                *mask |= MS_BUILTIN_DATE;
            }
            else if (type == (PyObject *)(PyDateTimeAPI->TimeType)) {
                *mask |= MS_BUILTIN_TIME;
            }
            else if (type == mod->UUIDType) {
                *mask |= MS_BUILTIN_UUID;
            }
            else if (type == mod->DecimalType) {
                *mask |= MS_BUILTIN_DECIMAL;
            }
            else {
                PyErr_Format(PyExc_TypeError, "Cannot treat %R as a builtin type", type);
                Py_DECREF(seq);
                return -1;
            }
        }
        Py_DECREF(seq);
    }
    return 0;
}


PyDoc_STRVAR(msgspec_to_builtins__doc__,
"to_builtins(obj, *, str_keys=False, builtin_types=None, enc_hook=None)\n"
"--\n"
"\n"
"Convert a complex object to one composed only of simpler builtin types\n"
"commonly supported by Python serialization libraries.\n"
"\n"
"This is mainly useful for adding msgspec support for other protocols.\n"
"\n"
"Parameters\n"
"----------\n"
"obj: Any\n"
"    The object to convert.\n"
"builtin_types: Iterable[type], optional\n"
"    An iterable of types to treat as additional builtin types. These types will\n"
"    be passed through ``to_builtins`` unchanged. Currently only supports\n"
"    `bytes`, `bytearray`, `memoryview`, `datetime.datetime`, `datetime.time`,\n"
"    `datetime.date`, `uuid.UUID`, and `decimal.Decimal`.\n"
"str_keys: bool, optional\n"
"    Whether to convert all object keys to strings. Default is False.\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported msgspec types. Takes the\n"
"    unsupported object and should return a supported object, or raise a TypeError.\n"
"\n"
"Returns\n"
"-------\n"
"Any\n"
"    The converted object.\n"
"\n"
"Examples\n"
"--------\n"
">>> import msgspec\n"
">>> class Example(msgspec.Struct):\n"
"...     x: set[int]\n"
"...     y: bytes\n"
">>> msg = Example({1, 2, 3}, b'\\x01\\x02')\n"
"\n"
"Convert the message to a simpler set of builtin types. Note that by default\n"
"all bytes-like objects are base64-encoded and converted to strings.\n"
"\n"
">>> msgspec.to_builtins(msg)\n"
"{'x': [1, 2, 3], 'y': 'AQI='}\n"
"\n"
"If the downstream code supports binary objects natively, you can\n"
"disable conversion by passing in the types to ``builtin_types``.\n"
"\n"
">>> msgspec.to_builtins(msg, builtin_types=(bytes, bytearray, memoryview))\n"
"{'x': [1, 2, 3], 'y': b'\\x01\\x02'}\n"
"\n"
"See Also\n"
"--------\n"
"msgspec.from_builtins\n"
"msgspec.structs.asdict\n"
"msgspec.structs.astuple"
);
static PyObject*
msgspec_to_builtins(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *obj = NULL, *builtin_types = NULL, *enc_hook = NULL;
    int str_keys = false;
    ToBuiltinsState state;

    static char *kwlist[] = {"obj", "builtin_types", "str_keys", "enc_hook", NULL};

    /* Parse arguments */
    if (!PyArg_ParseTupleAndKeywords(
        args, kwargs, "O|$OpO", kwlist, &obj, &builtin_types, &str_keys, &enc_hook
    )) {
        return NULL;
    }

    state.mod = msgspec_get_global_state();
    state.str_keys = str_keys;
    state.builtin_types = 0;

    if (enc_hook == Py_None) {
        enc_hook = NULL;
    }
    else if (enc_hook != NULL && !PyCallable_Check(enc_hook)) {
        PyErr_SetString(PyExc_TypeError, "enc_hook must be callable");
        return NULL;
    }
    state.enc_hook = enc_hook;
    if (ms_process_builtin_types(state.mod, builtin_types, &(state.builtin_types)) < 0) return NULL;

    return to_builtins(&state, obj, false);
}

/*************************************************************************
 * from_builtins                                                           *
 *************************************************************************/

typedef struct FromBuiltinsState {
    MsgspecState *mod;
    PyObject *dec_hook;
    uint32_t builtin_types;
    bool str_keys;
    PyObject* (*from_builtins_str)(
        struct FromBuiltinsState*, PyObject*, bool, TypeNode*, PathNode*
    );
} FromBuiltinsState;

static PyObject * from_builtins(FromBuiltinsState *, PyObject *, TypeNode *, PathNode *);

static PyObject *
from_builtins_int(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_INT)) {
        return ms_decode_pyint(obj, type, path);
    }
    else if (type->types & (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        return ms_decode_int_enum_or_literal_pyint(obj, type, path);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return ms_decode_float(PyLong_AsDouble(obj), type, path);
    }
    return ms_validation_error("int", type, path);
}

static PyObject *
from_builtins_float(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)) {
        return ms_decode_pyfloat(obj, type, path);
    }
    return ms_validation_error("float", type, path);
}

static PyObject *
from_builtins_bool(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error("bool", type, path);
}

static PyObject *
from_builtins_none(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_NONE)) {
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error("null", type, path);
}

static PyObject *
from_builtins_str_uncommon(
    FromBuiltinsState *self, PyObject *obj, const char *view, Py_ssize_t size,
    bool is_key, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_ENUM | MS_TYPE_STRLITERAL)) {
        return ms_decode_str_enum_or_literal(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_DATETIME)
        && !(self->builtin_types & MS_BUILTIN_DATETIME)
    ) {
        return ms_decode_datetime(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_DATE)
        && !(self->builtin_types & MS_BUILTIN_DATE)
    ) {
        return ms_decode_date(view, size, path);
    }
    else if (
        (type->types & MS_TYPE_TIME)
        && !(self->builtin_types & MS_BUILTIN_TIME)
    ) {
        return ms_decode_time(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_UUID)
        && !(self->builtin_types & MS_BUILTIN_UUID)
    ) {
        return ms_decode_uuid(view, size, path);
    }
    else if (
        (type->types & MS_TYPE_DECIMAL)
        && !(self->builtin_types & MS_BUILTIN_DECIMAL)
    ) {
        return ms_decode_decimal_pyobj(self->mod, obj, path);
    }
    else if (
        (type->types & MS_TYPE_BYTES)
        && !(self->builtin_types & MS_BUILTIN_BYTES)
    ) {
        return json_decode_binary(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_BYTEARRAY)
        && !(self->builtin_types & MS_BUILTIN_BYTEARRAY)
    ) {
        return json_decode_binary(view, size, type, path);
    }
    else if (
        is_key && self->str_keys && (type->types & (MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL))
    ) {
        return json_decode_int_from_str(view, size, type, path);
    }
    return ms_validation_error("str", type, path);
}

static PyObject *
from_builtins_str_strict(
    FromBuiltinsState *self, PyObject *obj,
    bool is_key, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_STR)) {
        Py_INCREF(obj);
        return ms_check_str_constraints(obj, type, path);
    }
    Py_ssize_t size;
    const char* view = unicode_str_and_size(obj, &size);
    if (view == NULL) return NULL;
    return from_builtins_str_uncommon(self, obj, view, size, is_key, type, path);
}

static PyObject *
from_builtins_str_lax(
    FromBuiltinsState *self, PyObject *obj,
    bool is_key, TypeNode *type, PathNode *path
) {
    Py_ssize_t size;
    const char* view = unicode_str_and_size(obj, &size);
    if (view == NULL) return NULL;

    if (type->types & (MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        PyObject *out = NULL;
        if (json_decode_int_from_str_inner(view, size, false, type, path, &out)) {
            return out;
        }
    }

    if (type->types & MS_TYPE_FLOAT) {
        PyObject *out = PyFloat_FromString(obj);
        if (out != NULL) {
            return ms_decode_pyfloat(out, type, path);
        }
        PyErr_Clear();
    }

    if (type->types & MS_TYPE_BOOL) {
        if (size == 1) {
            if (*view == '0') {
                Py_RETURN_FALSE;
            }
            else if (*view == '1') {
                Py_RETURN_TRUE;
            }
        }
        else if (size == 4) {
            if (
                (view[0] == 't' || view[0] == 'T') &&
                (view[1] == 'r' || view[1] == 'R') &&
                (view[2] == 'u' || view[2] == 'U') &&
                (view[3] == 'e' || view[3] == 'E')
            ) {
                Py_RETURN_TRUE;
            }
        }
        else if (size == 5) {
            if (
                (view[0] == 'f' || view[0] == 'F') &&
                (view[1] == 'a' || view[1] == 'A') &&
                (view[2] == 'l' || view[2] == 'L') &&
                (view[3] == 's' || view[3] == 'S') &&
                (view[4] == 'e' || view[4] == 'E')
            ) {
                Py_RETURN_FALSE;
            }
        }
    }

    if (type->types & MS_TYPE_NONE) {
        if (size == 4 &&
            (view[0] == 'n' || view[0] == 'N') &&
            (view[1] == 'u' || view[1] == 'U') &&
            (view[2] == 'l' || view[2] == 'L') &&
            (view[3] == 'l' || view[3] == 'L')
        ) {
            Py_RETURN_NONE;
        }
    }

    if (type->types & (MS_TYPE_ANY | MS_TYPE_STR)) {
        Py_INCREF(obj);
        return ms_check_str_constraints(obj, type, path);
    }
    return from_builtins_str_uncommon(self, obj, view, size, false, type, path);
}


static PyObject *
from_builtins_bytes(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY)) {
        if (!ms_passes_bytes_constraints(PyBytes_GET_SIZE(obj), type, path)) {
            return NULL;
        }
        if (type->types & MS_TYPE_BYTES) {
            Py_INCREF(obj);
            return obj;
        }
        return PyByteArray_FromObject(obj);
    }
    return ms_validation_error("bytes", type, path);
}

static PyObject *
from_builtins_bytearray(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY)) {
        if (!ms_passes_bytes_constraints(PyByteArray_GET_SIZE(obj), type, path)) {
            return NULL;
        }
        if (type->types & MS_TYPE_BYTEARRAY) {
            Py_INCREF(obj);
            return obj;
        }
        return PyBytes_FromObject(obj);
    }
    return ms_validation_error("bytes", type, path);
}

static PyObject *
from_builtins_datetime(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_DATETIME) {
        PyObject *tz = MS_DATE_GET_TZINFO(obj);
        if (!ms_passes_tz_constraint(tz, type, path)) return NULL;
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error("datetime", type, path);
}

static PyObject *
from_builtins_time(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_TIME) {
        PyObject *tz = MS_TIME_GET_TZINFO(obj);
        if (!ms_passes_tz_constraint(tz, type, path)) return NULL;
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error("time", type, path);
}

static PyObject *
from_builtins_immutable(
    FromBuiltinsState *self, uint64_t mask, const char *expected,
    PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & mask) {
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error(expected, type, path);
}

static PyObject *
from_builtins_list(
    FromBuiltinsState *self, PyObject **items, Py_ssize_t size,
    TypeNode *item_type, PathNode *path
) {
    PyObject *out = PyList_New(size);
    if (out == NULL) return NULL;
    if (size == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode item_path = {path, i};
        PyObject *val = from_builtins(self, items[i], item_type, &item_path);
        if (val == NULL) {
            Py_CLEAR(out);
            break;
        }
        PyList_SET_ITEM(out, i, val);
    }
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
from_builtins_set(
    FromBuiltinsState *self, PyObject **items, Py_ssize_t size,
    bool mutable, TypeNode *item_type, PathNode *path
) {
    PyObject *out = mutable ? PySet_New(NULL) : PyFrozenSet_New(NULL);
    if (out == NULL) return NULL;
    if (size == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode item_path = {path, i};
        PyObject *val = from_builtins(self, items[i], item_type, &item_path);
        if (MS_UNLIKELY(val == NULL || PySet_Add(out, val) < 0)) {
            Py_XDECREF(val);
            Py_CLEAR(out);
            break;
        }
        Py_DECREF(val);
    }
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
from_builtins_vartuple(
    FromBuiltinsState *self, PyObject **items, Py_ssize_t size,
    TypeNode *item_type, PathNode *path
) {
    PyObject *out = PyTuple_New(size);
    if (out == NULL) return NULL;
    if (size == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode item_path = {path, i};
        PyObject *val = from_builtins(self, items[i], item_type, &item_path);
        if (val == NULL) {
            Py_CLEAR(out);
            break;
        }
        PyTuple_SET_ITEM(out, i, val);
    }
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
from_builtins_fixtuple(
    FromBuiltinsState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    Py_ssize_t fixtuple_size, offset;
    TypeNode_get_fixtuple(type, &offset, &fixtuple_size);

    if (size != fixtuple_size) {
        /* tuple is the incorrect size, raise and return */
        ms_raise_validation_error(
            path,
            "Expected `array` of length %zd, got %zd%U",
            fixtuple_size,
            size
        );
        return NULL;
    }

    PyObject *out = PyTuple_New(size);
    if (out == NULL) return NULL;
    if (size == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }

    for (Py_ssize_t i = 0; i < fixtuple_size; i++) {
        PathNode item_path = {path, i};
        PyObject *val = from_builtins(
            self, items[i], type->details[offset + i].pointer, &item_path
        );
        if (MS_UNLIKELY(val == NULL)) {
            Py_CLEAR(out);
            break;
        }
        PyTuple_SET_ITEM(out, i, val);
    }
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
from_builtins_namedtuple(
    FromBuiltinsState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    NamedTupleInfo *info = TypeNode_get_namedtuple_info(type);
    Py_ssize_t nfields = Py_SIZE(info);
    Py_ssize_t ndefaults = info->defaults == NULL ? 0 : PyTuple_GET_SIZE(info->defaults);
    Py_ssize_t nrequired = nfields - ndefaults;

    if (size < nrequired || nfields < size) {
        /* tuple is the incorrect size, raise and return */
        if (ndefaults == 0) {
            ms_raise_validation_error(
                path,
                "Expected `array` of length %zd, got %zd%U",
                nfields,
                size
            );
        }
        else {
            ms_raise_validation_error(
                path,
                "Expected `array` of length %zd to %zd, got %zd%U",
                nrequired,
                nfields,
                size
            );
        }
        return NULL;
    }
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyTypeObject *nt_type = (PyTypeObject *)(info->class);
    PyObject *out = nt_type->tp_alloc(nt_type, nfields);
    if (out == NULL) goto error;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyTuple_SET_ITEM(out, i, NULL);
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode item_path = {path, i};
        PyObject *item = from_builtins(self, items[i], info->types[i], &item_path);
        if (MS_UNLIKELY(item == NULL)) goto error;
        PyTuple_SET_ITEM(out, i, item);
    }
    for (Py_ssize_t i = size; i < nfields; i++) {
        PyObject *item = PyTuple_GET_ITEM(info->defaults, i - nrequired);
        Py_INCREF(item);
        PyTuple_SET_ITEM(out, i, item);
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static bool
from_builtins_tag_matches(
    FromBuiltinsState *self, PyObject *tag, PyObject *expected_tag, PathNode *path
) {
    if (PyUnicode_CheckExact(expected_tag)) {
        if (!PyUnicode_CheckExact(tag)) goto wrong_type;
    }
    else if (!PyLong_CheckExact(tag)) {
        goto wrong_type;
    }
    int status = PyObject_RichCompareBool(tag, expected_tag, Py_EQ);
    if (status == 1) return true;
    if (status == 0) {
        ms_raise_validation_error(path, "Invalid value %R%U", tag);
    }
    return false;
wrong_type:
    ms_raise_validation_error(
        path,
        "Expected `%s`, got `%s`%U",
        (PyUnicode_CheckExact(expected_tag) ? "str" : "int"),
        Py_TYPE(tag)->tp_name
    );
    return false;
}

static StructMetaObject *
from_builtins_lookup_tag(
    FromBuiltinsState *self, Lookup *lookup, PyObject *tag, PathNode *path
) {
    StructMetaObject *out = NULL;
    if (Lookup_IsStrLookup(lookup)) {
        if (!PyUnicode_CheckExact(tag)) goto wrong_type;
        Py_ssize_t size;
        const char *buf = unicode_str_and_size(tag, &size);
        if (buf == NULL) return NULL;
        out = (StructMetaObject *)StrLookup_Get((StrLookup *)lookup, buf, size);
    }
    else {
        if (!PyLong_CheckExact(tag)) goto wrong_type;
        uint64_t ux;
        bool neg, overflow;
        overflow = fast_long_extract_parts(tag, &neg, &ux);
        if (overflow) goto invalid_value;
        if (neg) {
            out = (StructMetaObject *)IntLookup_GetInt64((IntLookup *)lookup, -(int64_t)ux);
        }
        else {
            out = (StructMetaObject *)IntLookup_GetUInt64((IntLookup *)lookup, ux);
        }
    }
    if (out != NULL) return out;
invalid_value:
    ms_raise_validation_error(path, "Invalid value %R%U", tag);
    return NULL;
wrong_type:
    ms_raise_validation_error(
        path,
        "Expected `%s`, got `%s`%U",
        (Lookup_IsStrLookup(lookup) ? "str" : "int"),
        Py_TYPE(tag)->tp_name
    );
    return NULL;
}

static PyObject *
from_builtins_struct_array_inner(
    FromBuiltinsState *self, PyObject **items, Py_ssize_t size,
    bool tag_already_read, StructMetaObject *st_type, PathNode *path
) {
    PathNode item_path = {path, 0};
    bool tagged = st_type->struct_tag_value != NULL;
    Py_ssize_t nfields = PyTuple_GET_SIZE(st_type->struct_encode_fields);
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    Py_ssize_t nrequired = tagged + nfields - st_type->n_trailing_defaults;
    Py_ssize_t npos = nfields - ndefaults;

    if (size < nrequired) {
        ms_raise_validation_error(
            path,
            "Expected `array` of at least length %zd, got %zd%U",
            nrequired,
            size
        );
        return NULL;
    }

    if (tagged) {
        if (!tag_already_read) {
            if (
                !from_builtins_tag_matches(
                    self, items[item_path.index], st_type->struct_tag_value, &item_path
                )
            ) {
                return NULL;
            }
        }
        size--;
        item_path.index++;
    }

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyObject *out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) goto error;

    bool is_gc = MS_TYPE_IS_GC(st_type);
    bool should_untrack = is_gc;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *val;
        if (size > 0) {
            val = from_builtins(
                self, items[item_path.index], st_type->struct_types[i], &item_path
            );
            if (MS_UNLIKELY(val == NULL)) goto error;
            size--;
            item_path.index++;
        }
        else {
            val = get_default(
                PyTuple_GET_ITEM(st_type->struct_defaults, i - npos)
            );
            if (val == NULL) goto error;
        }
        Struct_set_index(out, i, val);
        if (should_untrack) {
            should_untrack = !MS_OBJ_IS_GC(val);
        }
    }
    if (MS_UNLIKELY(size > 0)) {
        if (MS_UNLIKELY(st_type->forbid_unknown_fields == OPT_TRUE)) {
            ms_raise_validation_error(
                path,
                "Expected `array` of at most length %zd, got %zd%U",
                nfields,
                nfields + size
            );
            goto error;
        }
    }
    Py_LeaveRecursiveCall();
    if (is_gc && !should_untrack)
        PyObject_GC_Track(out);
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
from_builtins_struct_array(
    FromBuiltinsState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    return from_builtins_struct_array_inner(
        self, items, size, false, TypeNode_get_struct(type), path
    );
}

static PyObject *
from_builtins_struct_array_union(
    FromBuiltinsState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    if (size == 0) {
        return ms_error_with_path(
            "Expected `array` of at least length 1, got 0%U", path
        );
    }

    PathNode tag_path = {path, 0};
    StructMetaObject *struct_type = from_builtins_lookup_tag(
        self, lookup, items[0], &tag_path
    );
    if (struct_type == NULL) return NULL;
    return from_builtins_struct_array_inner(self, items, size, true, struct_type, path);
}

static PyObject *
from_builtins_array(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    PyObject **items = PySequence_Fast_ITEMS(obj);
    Py_ssize_t size = PySequence_Fast_GET_SIZE(obj);

    if (!ms_passes_array_constraints(size, type, path)) return NULL;

    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        return from_builtins_list(self, items, size, &type_any, path);
    }
    else if (type->types & MS_TYPE_LIST) {
        return from_builtins_list(self, items, size, TypeNode_get_array(type), path);
    }
    else if (type->types & (MS_TYPE_SET | MS_TYPE_FROZENSET)) {
        return from_builtins_set(
            self, items, size,
            (type->types & MS_TYPE_SET), TypeNode_get_array(type), path
        );
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return from_builtins_vartuple(self, items, size, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_FIXTUPLE) {
        return from_builtins_fixtuple(self, items, size, type, path);
    }
    else if (type->types & MS_TYPE_NAMEDTUPLE) {
        return from_builtins_namedtuple(self, items, size, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY) {
        return from_builtins_struct_array(self, items, size, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY_UNION) {
        return from_builtins_struct_array_union(self, items, size, type, path);
    }
    return ms_validation_error("array", type, path);
}

static PyObject *
from_builtins_dict(
    FromBuiltinsState *self, PyObject *obj,
    TypeNode *key_type, TypeNode *val_type, PathNode *path
) {
    PathNode key_path = {path, PATH_KEY, NULL};
    PathNode val_path = {path, PATH_ELLIPSIS, NULL};

    PyObject *out = PyDict_New();
    if (out == NULL) return NULL;
    if (PyDict_GET_SIZE(obj) == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    PyObject *key_obj = NULL, *val_obj = NULL;
    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key_obj, &val_obj)) {
        PyObject *key;
        if (PyUnicode_CheckExact(key_obj)) {
            key = from_builtins_str_strict(self, key_obj, true, key_type, &key_path);
        }
        else {
            key = from_builtins(self, key_obj, key_type, &key_path);
        }
        if (MS_UNLIKELY(key == NULL)) goto error;
        PyObject *val = from_builtins(self, val_obj, val_type, &val_path);
        if (MS_UNLIKELY(val == NULL)) {
            Py_DECREF(key);
            goto error;
        }
        int status = PyDict_SetItem(out, key, val);
        Py_DECREF(key);
        Py_DECREF(val);
        if (status < 0) goto error;
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static bool
from_builtins_is_str_key(PyObject *key, PathNode *path) {
    if (PyUnicode_CheckExact(key)) return true;
    PathNode key_path = {path, PATH_KEY, NULL};
    ms_error_with_path("Expected `str`%U", &key_path);
    return false;
}


static PyObject *
from_builtins_struct(
    FromBuiltinsState *self, PyObject *obj, StructMetaObject *struct_type, PathNode *path
) {
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyObject *out = Struct_alloc((PyTypeObject *)(struct_type));
    if (out == NULL) goto error;

    Py_ssize_t pos = 0, pos_obj = 0;
    PyObject *key_obj, *val_obj;
    while (PyDict_Next(obj, &pos_obj, &key_obj, &val_obj)) {
        if (!from_builtins_is_str_key(key_obj, path)) goto error;

        Py_ssize_t key_size;
        const char *key = unicode_str_and_size(key_obj, &key_size);
        if (key == NULL) goto error;

        Py_ssize_t field_index = StructMeta_get_field_index(struct_type, key, key_size, &pos);
        if (field_index < 0) {
            if (MS_UNLIKELY(field_index == -2)) {
                PathNode tag_path = {path, PATH_STR, struct_type->struct_tag_field};
                if (
                    !from_builtins_tag_matches(
                        self, val_obj, struct_type->struct_tag_value, &tag_path
                    )
                ) {
                    goto error;
                }
            }
            else {
                /* Unknown field */
                if (MS_UNLIKELY(struct_type->forbid_unknown_fields == OPT_TRUE)) {
                    ms_error_unknown_field(key, key_size, path);
                    goto error;
                }
            }
        }
        else {
            PathNode field_path = {path, field_index, (PyObject *)struct_type};
            PyObject *val = from_builtins(
                self, val_obj, struct_type->struct_types[field_index], &field_path
            );
            if (val == NULL) goto error;
            Struct_set_index(out, field_index, val);
        }
    }

    if (Struct_fill_in_defaults(struct_type, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
from_builtins_struct_union(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    PyObject *tag_field = Lookup_tag_field(lookup);

    PyObject *key_obj, *val_obj;
    Py_ssize_t pos_obj = 0;
    while (PyDict_Next(obj, &pos_obj, &key_obj, &val_obj)) {
        if (!from_builtins_is_str_key(key_obj, path)) return NULL;
        if (_PyUnicode_EQ(key_obj, tag_field)) {
            /* Decode and lookup tag */
            PathNode tag_path = {path, PATH_STR, tag_field};
            StructMetaObject *struct_type = from_builtins_lookup_tag(
                self, lookup, val_obj, &tag_path
            );
            if (struct_type == NULL) return NULL;
            return from_builtins_struct(self, obj, struct_type, path);
        }
    }

    ms_raise_validation_error(
        path,
        "Object missing required field `%U`%U",
        tag_field
    );
    return NULL;
}

static PyObject *
from_builtins_typeddict(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyObject *out = PyDict_New();
    if (out == NULL) goto error;

    TypedDictInfo *info = TypeNode_get_typeddict_info(type);
    Py_ssize_t nrequired = 0, pos = 0, pos_obj = 0;
    PyObject *key_obj, *val_obj;
    while (PyDict_Next(obj, &pos_obj, &key_obj, &val_obj)) {
        if (!from_builtins_is_str_key(key_obj, path)) goto error;

        Py_ssize_t key_size;
        const char *key = unicode_str_and_size(key_obj, &key_size);
        if (key == NULL) goto error;

        TypeNode *field_type;
        PyObject *field = TypedDictInfo_lookup_key(
            info, key, key_size, &field_type, &pos
        );
        if (field != NULL) {
            if (field_type->types & MS_EXTRA_FLAG) nrequired++;
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = from_builtins(self, val_obj, field_type, &field_path);
            if (val == NULL) goto error;
            int status = PyDict_SetItem(out, field, val);
            Py_DECREF(val);
            if (status < 0) goto error;
        }
    }
    if (nrequired < info->nrequired) {
        /* A required field is missing, determine which one and raise */
        TypedDictInfo_error_missing(info, out, path);
        goto error;
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
from_builtins_dataclass(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    DataclassInfo *info = TypeNode_get_dataclass_info(type);

    PyTypeObject *dataclass_type = (PyTypeObject *)(info->class);
    PyObject *out = dataclass_type->tp_alloc(dataclass_type, 0);
    if (out == NULL) goto error;

    Py_ssize_t pos = 0, pos_obj = 0;
    PyObject *key_obj = NULL, *val_obj = NULL;
    while (PyDict_Next(obj, &pos_obj, &key_obj, &val_obj)) {
        if (!from_builtins_is_str_key(key_obj, path)) goto error;
        Py_ssize_t key_size;
        const char *key = unicode_str_and_size(key_obj, &key_size);
        if (MS_UNLIKELY(key == NULL)) goto error;

        TypeNode *field_type;
        PyObject *field = DataclassInfo_lookup_key(
            info, key, key_size, &field_type, &pos
        );
        if (field != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = from_builtins(self, val_obj, field_type, &field_path);
            if (val == NULL) goto error;
            int status = PyObject_SetAttr(out, field, val);
            Py_DECREF(val);
            if (status < 0) goto error;
        }
    }
    if (DataclassInfo_post_decode(info, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
from_builtins_object(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_DICT | MS_TYPE_ANY)) {
        Py_ssize_t size = PyDict_GET_SIZE(obj);
        if (!ms_passes_map_constraints(size, type, path)) return NULL;
        TypeNode *key_type, *val_type;
        TypeNode type_any = {MS_TYPE_ANY};
        if (type->types & MS_TYPE_ANY) {
            key_type = &type_any;
            val_type = &type_any;
        }
        else {
            TypeNode_get_dict(type, &key_type, &val_type);
        }
        return from_builtins_dict(self, obj, key_type, val_type, path);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        return from_builtins_struct(self, obj, struct_type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_UNION) {
        return from_builtins_struct_union(self, obj, type, path);
    }
    else if (type->types & MS_TYPE_TYPEDDICT) {
        return from_builtins_typeddict(self, obj, type, path);
    }
    else if (type->types & MS_TYPE_DATACLASS) {
        return from_builtins_dataclass(self, obj, type, path);
    }
    return ms_validation_error("object", type, path);
}

static PyObject *
from_builtins(
    FromBuiltinsState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    PyObject *out = NULL;
    PyTypeObject *pytype = Py_TYPE(obj);
    if (obj == Py_None) {
        out = from_builtins_none(self, obj, type, path);
    }
    else if (pytype == &PyLong_Type) {
        out = from_builtins_int(self, obj, type, path);
    }
    else if (pytype == &PyFloat_Type) {
        out = from_builtins_float(self, obj, type, path);
    }
    else if (pytype == &PyBool_Type) {
        out = from_builtins_bool(self, obj, type, path);
    }
    else if (pytype == &PyUnicode_Type) {
        out = self->from_builtins_str(self, obj, false, type, path);
    }
    else if (pytype == &PyBytes_Type) {
        out = from_builtins_bytes(self, obj, type, path);
    }
    else if (pytype == &PyByteArray_Type) {
        out = from_builtins_bytearray(self, obj, type, path);
    }
    else if (pytype == PyDateTimeAPI->DateTimeType) {
        out = from_builtins_datetime(self, obj, type, path);
    }
    else if (pytype == PyDateTimeAPI->TimeType) {
        out = from_builtins_time(self, obj, type, path);
    }
    else if (pytype == PyDateTimeAPI->DateType) {
        out = from_builtins_immutable(self, MS_TYPE_DATE, "date", obj, type, path);
    }
    else if (pytype == (PyTypeObject *)self->mod->UUIDType) {
        out = from_builtins_immutable(self, MS_TYPE_UUID, "uuid", obj, type, path);
    }
    else if (pytype == (PyTypeObject *)self->mod->DecimalType) {
        out = from_builtins_immutable(self, MS_TYPE_DECIMAL, "decimal", obj, type, path);
    }
    else if (pytype == &PyList_Type || pytype == &PyTuple_Type) {
        out = from_builtins_array(self, obj, type, path);
    }
    else if (pytype == &PyDict_Type) {
        out = from_builtins_object(self, obj, type, path);
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "from_builtins doesn't support objects of type '%.200s'",
            pytype->tp_name
        );
        return NULL;
    }

    if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
        return ms_decode_custom(out, self->dec_hook, type, path);
    }
    return out;
}

PyDoc_STRVAR(msgspec_from_builtins__doc__,
"from_builtins(obj, type, *, str_keys=False, str_values=False, builtin_types=None, dec_hook=None)\n"
"--\n"
"\n"
"Construct a complex object from one composed only of simpler builtin types\n"
"commonly supported by Python serialization libraries.\n"
"\n"
"This is mainly useful for adding msgspec support for other protocols.\n"
"\n"
"Parameters\n"
"----------\n"
"obj: Any\n"
"    The object to convert.\n"
"type: Type\n"
"    A Python type (in type annotation form) to convert the object to.\n"
"builtin_types: Iterable[type], optional\n"
"    An iterable of types to treat as additional builtin types. Passing a type\n"
"    here indicates that the wrapped protocol natively supports that type,\n"
"    disabling any coercion to that type provided by `from_builtins`. For\n"
"    example, passing in ``builtin_types=(datetime,)`` disables the default\n"
"    ``str`` to ``datetime`` conversion; the wrapped protocol must provide\n"
"    a ``datetime`` object directly. Currently only supports `bytes`,\n"
"    `bytearray`, `datetime.datetime`, `datetime.time`, `datetime.date`,\n"
"    `uuid.UUID`, and `decimal.Decimal`.\n"
"str_keys: bool, optional\n"
"    Whether the wrapped protocol only supports string keys. Setting to True\n"
"    enables a wider set of coercion rules from string to non-string types for\n"
"    dict keys. Default is False.\n"
"str_values: bool, optional\n"
"    Whether the wrapped protocol only supports string values. Setting to True\n"
"    enables a wider set of coercion rules from string to non-string types for\n"
"    all values. Implies ``str_keys=True``. Default is False.\n"
"dec_hook: callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic MessagePack types. This hook should transform ``obj`` into\n"
"    type ``type``, or raise a ``TypeError`` if unsupported.\n"
"\n"
"Returns\n"
"-------\n"
"Any\n"
"    The converted object of the specified ``type``.\n"
"\n"
"Examples\n"
"--------\n"
">>> import msgspec\n"
">>> class Example(msgspec.Struct):\n"
"...     x: set[int]\n"
"...     y: bytes\n"
">>> msg = {'x': [1, 2, 3], 'y': 'AQI='}\n"
"\n"
"Construct the message from a simpler set of builtin types.\n"
"\n"
">>> msgspec.from_builtins(msg, Example)\n"
"Example({1, 2, 3}, b'\\x01\\x02')\n"
"\n"
"See Also\n"
"--------\n"
"to_builtins"
);
static PyObject*
msgspec_from_builtins(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *obj = NULL, *pytype = NULL, *builtin_types = NULL, *dec_hook = NULL;
    int str_keys = false, str_values = false;
    FromBuiltinsState state;

    static char *kwlist[] = {
        "obj", "type", "builtin_types", "str_keys", "str_values", "dec_hook", NULL
    };

    /* Parse arguments */
    if (!PyArg_ParseTupleAndKeywords(
        args, kwargs, "OO|$OppO", kwlist,
        &obj, &pytype, &builtin_types, &str_keys, &str_values, &dec_hook
    )) {
        return NULL;
    }

    state.mod = msgspec_get_global_state();
    state.builtin_types = 0;
    state.str_keys = str_keys;
    if (str_values) {
        state.from_builtins_str = &(from_builtins_str_lax);
    }
    else {
        state.from_builtins_str = &(from_builtins_str_strict);
    }

    if (dec_hook == Py_None) {
        dec_hook = NULL;
    }
    else if (dec_hook != NULL && !PyCallable_Check(dec_hook)) {
        PyErr_SetString(PyExc_TypeError, "dec_hook must be callable");
        return NULL;
    }
    state.dec_hook = dec_hook;
    if (ms_process_builtin_types(state.mod, builtin_types, &(state.builtin_types)) < 0) return NULL;

    TypeNode *type = TypeNode_Convert(pytype, str_keys, NULL);
    if (type == NULL) return NULL;

    PyObject *out = from_builtins(&state, obj, type, NULL);
    TypeNode_Free(type);
    return out;
}


/*************************************************************************
 * Module Setup                                                          *
 *************************************************************************/

static struct PyMethodDef msgspec_methods[] = {
    {
        "replace", (PyCFunction) struct_replace, METH_FASTCALL | METH_KEYWORDS,
        struct_replace__doc__,
    },
    {
        "asdict", (PyCFunction) struct_asdict, METH_FASTCALL, struct_asdict__doc__,
    },
    {
        "astuple", (PyCFunction) struct_astuple, METH_FASTCALL, struct_astuple__doc__,
    },
    {
        "defstruct", (PyCFunction) msgspec_defstruct, METH_VARARGS | METH_KEYWORDS,
        msgspec_defstruct__doc__,
    },
    {
        "msgpack_encode", (PyCFunction) msgspec_msgpack_encode, METH_FASTCALL | METH_KEYWORDS,
        msgspec_msgpack_encode__doc__,
    },
    {
        "msgpack_decode", (PyCFunction) msgspec_msgpack_decode, METH_FASTCALL | METH_KEYWORDS,
        msgspec_msgpack_decode__doc__,
    },
    {
        "json_encode", (PyCFunction) msgspec_json_encode, METH_FASTCALL | METH_KEYWORDS,
        msgspec_json_encode__doc__,
    },
    {
        "json_decode", (PyCFunction) msgspec_json_decode, METH_FASTCALL | METH_KEYWORDS,
        msgspec_json_decode__doc__,
    },
    {
        "json_format", (PyCFunction) msgspec_json_format, METH_VARARGS | METH_KEYWORDS,
        msgspec_json_format__doc__,
    },
    {
        "to_builtins", (PyCFunction) msgspec_to_builtins, METH_VARARGS | METH_KEYWORDS,
        msgspec_to_builtins__doc__,
    },
    {
        "from_builtins", (PyCFunction) msgspec_from_builtins, METH_VARARGS | METH_KEYWORDS,
        msgspec_from_builtins__doc__,
    },
    {NULL, NULL} /* sentinel */
};

static int
msgspec_clear(PyObject *m)
{
    MsgspecState *st = msgspec_get_state(m);
    Py_CLEAR(st->MsgspecError);
    Py_CLEAR(st->EncodeError);
    Py_CLEAR(st->DecodeError);
    Py_CLEAR(st->StructType);
    Py_CLEAR(st->EnumMetaType);
    Py_CLEAR(st->struct_lookup_cache);
    Py_CLEAR(st->str___weakref__);
    Py_CLEAR(st->str__value2member_map_);
    Py_CLEAR(st->str___msgspec_cache__);
    Py_CLEAR(st->str__value_);
    Py_CLEAR(st->str_type);
    Py_CLEAR(st->str_enc_hook);
    Py_CLEAR(st->str_dec_hook);
    Py_CLEAR(st->str_ext_hook);
    Py_CLEAR(st->str_utcoffset);
    Py_CLEAR(st->str___origin__);
    Py_CLEAR(st->str___args__);
    Py_CLEAR(st->str___metadata__);
    Py_CLEAR(st->str___total__);
    Py_CLEAR(st->str___required_keys__);
    Py_CLEAR(st->str__fields);
    Py_CLEAR(st->str__field_defaults);
    Py_CLEAR(st->str___dataclass_fields__);
    Py_CLEAR(st->str___post_init__);
    Py_CLEAR(st->str___supertype__);
    Py_CLEAR(st->str_int);
    Py_CLEAR(st->str_is_safe);
    Py_CLEAR(st->UUIDType);
    Py_CLEAR(st->uuid_safeuuid_unknown);
    Py_CLEAR(st->DecimalType);
    Py_CLEAR(st->typing_union);
    Py_CLEAR(st->typing_any);
    Py_CLEAR(st->typing_literal);
    Py_CLEAR(st->typing_classvar);
    Py_CLEAR(st->typing_generic_alias);
    Py_CLEAR(st->typing_annotated_alias);
    Py_CLEAR(st->concrete_types);
    Py_CLEAR(st->get_type_hints);
    Py_CLEAR(st->get_typeddict_hints);
    Py_CLEAR(st->get_dataclass_info);
    Py_CLEAR(st->rebuild);
#if PY_VERSION_HEX >= 0x030a00f0
    Py_CLEAR(st->types_uniontype);
#endif
    Py_CLEAR(st->astimezone);
    Py_CLEAR(st->re_compile);
    return 0;
}

static void
msgspec_free(PyObject *m)
{
    msgspec_clear(m);
}

static int
msgspec_traverse(PyObject *m, visitproc visit, void *arg)
{
    MsgspecState *st = msgspec_get_state(m);

#if STRUCT_FREELIST_MAX_SIZE > 0
    /* Since module objects tend to persist throughout a program's execution,
     * this should only be called during major GC collections (i.e. rarely).
     *
     * We want to clear the freelist periodically to free up old pages and
     * reduce fragementation. But we don't want to do it too often, or the
     * freelist will rarely be used. Hence clearing the freelist here. This may
     * change in future releases.
     */
    Struct_freelist_clear();
#endif
    /* Clear the string cache every 10 major GC passes.
     *
     * The string cache can help improve performance in 2 different situations:
     *
     * - Calling untyped `json.decode` on a large message, where many keys are
     *   repeated within the same message.
     * - Calling untyped `json.decode` in a hot loop on many messages that
     *   share the same structure.
     *
     * In both cases, the string cache helps because common keys are more
     * likely to remain in cache. We do want to periodically clear the cache so
     * the allocator can free up old pages and reduce fragmentation, but we
     * want to do so as infrequently as possible. I've arbitrarily picked 10
     * major GC passes here as a heuristic.
     *
     * We clear the cache less frequently than the struct freelist, since:
     *
     * - Allocating a struct is much cheaper than validating and allocating a
     *   new string object.
     * - The struct freelist may contain many more objects, and consume a
     *   larger amount of memory.
     *
     * With the current configuration, the string cache may consume up to 20
     * KiB at a time, but that's with 100% of slots filled (unlikely due to
     * collisions). 50% filled is more likely, so 12 KiB max is a reasonable
     * estimate.
     */
    st->gc_cycle++;
    if (st->gc_cycle == 10) {
        st->gc_cycle = 0;
        string_cache_clear();
    }

    Py_VISIT(st->MsgspecError);
    Py_VISIT(st->EncodeError);
    Py_VISIT(st->DecodeError);
    Py_VISIT(st->StructType);
    Py_VISIT(st->EnumMetaType);
    Py_VISIT(st->struct_lookup_cache);
    Py_VISIT(st->typing_union);
    Py_VISIT(st->typing_any);
    Py_VISIT(st->typing_literal);
    Py_VISIT(st->typing_classvar);
    Py_VISIT(st->typing_generic_alias);
    Py_VISIT(st->typing_annotated_alias);
    Py_VISIT(st->concrete_types);
    Py_VISIT(st->get_type_hints);
    Py_VISIT(st->get_typeddict_hints);
    Py_VISIT(st->get_dataclass_info);
    Py_VISIT(st->rebuild);
#if PY_VERSION_HEX >= 0x030a00f0
    Py_VISIT(st->types_uniontype);
#endif
    Py_VISIT(st->astimezone);
    Py_VISIT(st->re_compile);
    return 0;
}

static struct PyModuleDef msgspecmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "msgspec._core",
    .m_size = sizeof(MsgspecState),
    .m_methods = msgspec_methods,
    .m_traverse = msgspec_traverse,
    .m_clear = msgspec_clear,
    .m_free =(freefunc)msgspec_free
};

PyMODINIT_FUNC
PyInit__core(void)
{
    PyObject *m, *temp_module, *temp_obj;
    MsgspecState *st;

    PyDateTime_IMPORT;

    m = PyState_FindModule(&msgspecmodule);
    if (m) {
        Py_INCREF(m);
        return m;
    }

    StructMetaType.tp_base = &PyType_Type;
    if (PyType_Ready(&NoDefault_Type) < 0)
        return NULL;
    if (PyType_Ready(&Unset_Type) < 0)
        return NULL;
    if (PyType_Ready(&Factory_Type) < 0)
        return NULL;
    if (PyType_Ready(&Field_Type) < 0)
        return NULL;
    if (PyType_Ready(&IntLookup_Type) < 0)
        return NULL;
    if (PyType_Ready(&StrLookup_Type) < 0)
        return NULL;
    if (PyType_Ready(&TypedDictInfo_Type) < 0)
        return NULL;
    if (PyType_Ready(&DataclassInfo_Type) < 0)
        return NULL;
    if (PyType_Ready(&NamedTupleInfo_Type) < 0)
        return NULL;
    if (PyType_Ready(&Meta_Type) < 0)
        return NULL;
    if (PyType_Ready(&StructMetaType) < 0)
        return NULL;
    if (PyType_Ready(&StructMixinType) < 0)
        return NULL;
    if (PyType_Ready(&Encoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&Decoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&Ext_Type) < 0)
        return NULL;
    if (PyType_Ready(&Raw_Type) < 0)
        return NULL;
    if (PyType_Ready(&JSONEncoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&JSONDecoder_Type) < 0)
        return NULL;

    /* Create the module */
    m = PyModule_Create(&msgspecmodule);
    if (m == NULL)
        return NULL;

    /* Add types */
    Py_INCREF(&Factory_Type);
    if (PyModule_AddObject(m, "Factory", (PyObject *)&Factory_Type) < 0)
        return NULL;
    if (PyModule_AddObject(m, "Field", (PyObject *)&Field_Type) < 0)
        return NULL;
    Py_INCREF(&Meta_Type);
    if (PyModule_AddObject(m, "Meta", (PyObject *)&Meta_Type) < 0)
        return NULL;
    Py_INCREF(&Ext_Type);
    if (PyModule_AddObject(m, "Ext", (PyObject *)&Ext_Type) < 0)
        return NULL;
    Py_INCREF(&Raw_Type);
    if (PyModule_AddObject(m, "Raw", (PyObject *)&Raw_Type) < 0)
        return NULL;
    Py_INCREF(&Encoder_Type);
    if (PyModule_AddObject(m, "MsgpackEncoder", (PyObject *)&Encoder_Type) < 0)
        return NULL;
    Py_INCREF(&Decoder_Type);
    if (PyModule_AddObject(m, "MsgpackDecoder", (PyObject *)&Decoder_Type) < 0)
        return NULL;
    Py_INCREF(&JSONEncoder_Type);
    if (PyModule_AddObject(m, "JSONEncoder", (PyObject *)&JSONEncoder_Type) < 0)
        return NULL;
    Py_INCREF(&JSONDecoder_Type);
    if (PyModule_AddObject(m, "JSONDecoder", (PyObject *)&JSONDecoder_Type) < 0)
        return NULL;

    st = msgspec_get_state(m);

    /* Initialize GC counter */
    st->gc_cycle = 0;

    /* Add nodefault singleton */
    Py_INCREF(NODEFAULT);
    if (PyModule_AddObject(m, "nodefault", NODEFAULT) < 0)
        return NULL;

    /* Add UNSET singleton */
    Py_INCREF(UNSET);
    if (PyModule_AddObject(m, "UNSET", UNSET) < 0)
        return NULL;

    /* Initialize the Struct Type */
    st->StructType = PyObject_CallFunction(
        (PyObject *)&StructMetaType, "s(O){ssss}", "Struct", &StructMixinType,
        "__module__", "msgspec", "__doc__", Struct__doc__
    );
    if (st->StructType == NULL)
        return NULL;
    Py_INCREF(st->StructType);
    if (PyModule_AddObject(m, "Struct", st->StructType) < 0)
        return NULL;

    /* Initialize the exceptions. */
    st->MsgspecError = PyErr_NewExceptionWithDoc(
        "msgspec.MsgspecError",
        "Base class for all Msgspec exceptions",
        NULL, NULL
    );
    if (st->MsgspecError == NULL)
        return NULL;
    st->EncodeError = PyErr_NewExceptionWithDoc(
        "msgspec.EncodeError",
        "An error occurred while encoding an object",
        st->MsgspecError, NULL
    );
    if (st->EncodeError == NULL)
        return NULL;
    st->DecodeError = PyErr_NewExceptionWithDoc(
        "msgspec.DecodeError",
        "An error occurred while decoding an object",
        st->MsgspecError, NULL
    );
    if (st->DecodeError == NULL)
        return NULL;
    st->ValidationError = PyErr_NewExceptionWithDoc(
        "msgspec.ValidationError",
        "The message didn't match the expected schema",
        st->DecodeError, NULL
    );
    if (st->ValidationError == NULL)
        return NULL;

    Py_INCREF(st->MsgspecError);
    if (PyModule_AddObject(m, "MsgspecError", st->MsgspecError) < 0)
        return NULL;
    Py_INCREF(st->EncodeError);
    if (PyModule_AddObject(m, "EncodeError", st->EncodeError) < 0)
        return NULL;
    Py_INCREF(st->DecodeError);
    if (PyModule_AddObject(m, "DecodeError", st->DecodeError) < 0)
        return NULL;
    Py_INCREF(st->ValidationError);
    if (PyModule_AddObject(m, "ValidationError", st->ValidationError) < 0)
        return NULL;

    /* Initialize the struct_lookup_cache */
    st->struct_lookup_cache = PyDict_New();
    if (st->struct_lookup_cache == NULL) return NULL;
    Py_INCREF(st->struct_lookup_cache);
    if (PyModule_AddObject(m, "_struct_lookup_cache", st->struct_lookup_cache) < 0)
        return NULL;

#define SET_REF(attr, name) \
    do { \
    st->attr = PyObject_GetAttrString(temp_module, name); \
    if (st->attr == NULL) return NULL; \
    } while (0)

    /* Get all imports from the typing module */
    temp_module = PyImport_ImportModule("typing");
    if (temp_module == NULL) return NULL;
    SET_REF(typing_union, "Union");
    SET_REF(typing_any, "Any");
    SET_REF(typing_literal, "Literal");
    SET_REF(typing_classvar, "ClassVar");
    SET_REF(typing_generic_alias, "_GenericAlias");
    Py_DECREF(temp_module);

    temp_module = PyImport_ImportModule("msgspec._utils");
    if (temp_module == NULL) return NULL;
    SET_REF(concrete_types, "_CONCRETE_TYPES");
    SET_REF(get_type_hints, "get_type_hints");
    SET_REF(get_typeddict_hints, "get_typeddict_hints");
    SET_REF(get_dataclass_info, "get_dataclass_info");
    SET_REF(typing_annotated_alias, "_AnnotatedAlias");
    SET_REF(rebuild, "rebuild");
    Py_DECREF(temp_module);

#if PY_VERSION_HEX >= 0x030a00f0
    temp_module = PyImport_ImportModule("types");
    if (temp_module == NULL) return NULL;
    SET_REF(types_uniontype, "UnionType");
    Py_DECREF(temp_module);
#endif

    /* Get the EnumMeta type */
    temp_module = PyImport_ImportModule("enum");
    if (temp_module == NULL)
        return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "EnumMeta");
    Py_DECREF(temp_module);
    if (temp_obj == NULL)
        return NULL;
    if (!PyType_Check(temp_obj)) {
        Py_DECREF(temp_obj);
        PyErr_SetString(PyExc_TypeError, "enum.EnumMeta should be a type");
        return NULL;
    }
    st->EnumMetaType = (PyTypeObject *)temp_obj;

    /* Get the datetime.datetime.astimezone method */
    temp_module = PyImport_ImportModule("datetime");
    if (temp_module == NULL) return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "datetime");
    Py_DECREF(temp_module);
    if (temp_obj == NULL) return NULL;
    st->astimezone = PyObject_GetAttrString(temp_obj, "astimezone");
    Py_DECREF(temp_obj);
    if (st->astimezone == NULL) return NULL;

    /* uuid module imports */
    temp_module = PyImport_ImportModule("uuid");
    if (temp_module == NULL) return NULL;
    st->UUIDType = PyObject_GetAttrString(temp_module, "UUID");
    if (st->UUIDType == NULL) return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "SafeUUID");
    if (temp_obj == NULL) return NULL;
    st->uuid_safeuuid_unknown = PyObject_GetAttrString(temp_obj, "unknown");
    Py_DECREF(temp_obj);
    if (st->uuid_safeuuid_unknown == NULL) return NULL;

    /* decimal module imports */
    temp_module = PyImport_ImportModule("decimal");
    if (temp_module == NULL) return NULL;
    st->DecimalType = PyObject_GetAttrString(temp_module, "Decimal");
    if (st->DecimalType == NULL) return NULL;

    /* Get the re.compile function */
    temp_module = PyImport_ImportModule("re");
    if (temp_module == NULL) return NULL;
    st->re_compile = PyObject_GetAttrString(temp_module, "compile");
    Py_DECREF(temp_module);
    if (st->re_compile == NULL) return NULL;

    /* Initialize cached constant strings */
#define CACHED_STRING(attr, str) \
    if ((st->attr = PyUnicode_InternFromString(str)) == NULL) return NULL
    CACHED_STRING(str___weakref__, "__weakref__");
    CACHED_STRING(str__value2member_map_, "_value2member_map_");
    CACHED_STRING(str___msgspec_cache__, "__msgspec_cache__");
    CACHED_STRING(str__value_, "_value_");
    CACHED_STRING(str_type, "type");
    CACHED_STRING(str_enc_hook, "enc_hook");
    CACHED_STRING(str_dec_hook, "dec_hook");
    CACHED_STRING(str_ext_hook, "ext_hook");
    CACHED_STRING(str_utcoffset, "utcoffset");
    CACHED_STRING(str___origin__, "__origin__");
    CACHED_STRING(str___args__, "__args__");
    CACHED_STRING(str___metadata__, "__metadata__");
    CACHED_STRING(str___total__, "__total__");
    CACHED_STRING(str___required_keys__, "__required_keys__");
    CACHED_STRING(str__fields, "_fields");
    CACHED_STRING(str__field_defaults, "_field_defaults");
    CACHED_STRING(str___dataclass_fields__, "__dataclass_fields__");
    CACHED_STRING(str___post_init__, "__post_init__");
    CACHED_STRING(str___supertype__, "__supertype__");
    CACHED_STRING(str_int, "int");
    CACHED_STRING(str_is_safe, "is_safe");

    return m;
}
