#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include "Python.h"
#include "datetime.h"
#include "structmember.h"

#include "ryu.h"

#ifdef __GNUC__
#define MS_LIKELY(pred) __builtin_expect(!!(pred), 1)
#define MS_UNLIKELY(pred) __builtin_expect(!!(pred), 0)
#else
#define MS_LIKELY(pred) (pred)
#define MS_UNLIKELY(pred) (pred)
#endif

#ifdef __GNUC__
#define MS_INLINE __attribute__((always_inline)) inline
#define MS_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define MS_INLINE __forceinline inline
#define MS_NOINLINE __declspec(noinline)
#else
#define MS_INLINE inline
#define MS_NOINLINE
#endif

#ifdef __GNUC__
#define ms_popcount(i) __builtin_popcount(i)
#else
static int
ms_popcount(uint32_t i) {
     i = i - ((i >> 1) & 0x55555555);        // add pairs of bits
     i = (i & 0x33333333) + ((i >> 2) & 0x33333333);  // quads
     i = (i + (i >> 4)) & 0x0F0F0F0F;        // groups of 8
     return (i * 0x01010101) >> 24;          // horizontal sum of bytes
}
#endif

#if PY_VERSION_HEX < 0x03090000
#define IS_TRACKED _PyObject_GC_IS_TRACKED
#define CALL_ONE_ARG(fn, arg) PyObject_CallFunctionObjArgs((fn), (arg), NULL)
#define SET_SIZE(obj, size) (((PyVarObject *)obj)->ob_size = size)
#else
#define IS_TRACKED  PyObject_GC_IsTracked
#define CALL_ONE_ARG(fn, arg) PyObject_CallOneArg((fn), (arg))
#define SET_SIZE(obj, size) Py_SET_SIZE(obj, size)
#endif

/* Easy access to NoneType object */
#define NONE_TYPE ((PyObject *)(Py_TYPE(Py_None)))

/* Is this object something that is/could be GC tracked? True if
 * - the value supports GC
 * - the value isn't a tuple or the object is tracked (skip tracked checks for non-tuples)
 */
#define OBJ_IS_GC(x) \
    (PyType_IS_GC(Py_TYPE(x)) && \
     (!PyTuple_CheckExact(x) || IS_TRACKED(x)))

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

/* XXX: Optimized `PyUnicode_AsUTF8AndSize`, fastpath for ascii strings. */
static inline const char *
unicode_str_and_size(PyObject *str, Py_ssize_t *size) {
    if (PyUnicode_IS_COMPACT_ASCII(str)) {
        *size = ((PyASCIIObject *)str)->length;
        return (char *)(((PyASCIIObject *)str) + 1);
    }
    return PyUnicode_AsUTF8AndSize(str, size);
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
    PyObject *EncodingError;
    PyObject *DecodingError;
    PyObject *StructType;
    PyTypeObject *EnumType;
    PyObject *str__name_;
    PyObject *str__value2member_map_;
    PyObject *str_name;
    PyObject *str_type;
    PyObject *str_enc_hook;
    PyObject *str_dec_hook;
    PyObject *str_ext_hook;
    PyObject *str_tzinfo;
    PyObject *str___origin__;
    PyObject *str___args__;
    PyObject *typing_list;
    PyObject *typing_set;
    PyObject *typing_tuple;
    PyObject *typing_dict;
    PyObject *typing_union;
    PyObject *typing_any;
    PyObject *get_type_hints;
    PyObject *timestamp;
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
    return msgspec_get_state(PyState_FindModule(&msgspecmodule));
}

static int
ms_err_truncated(void)
{
    PyErr_SetString(msgspec_get_global_state()->DecodingError, "input data was truncated");
    return -1;
}

/*************************************************************************
 * Parsing utilities                                                     *
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

/*************************************************************************
 * Struct and TypeNode Types                                             *
 *************************************************************************/

#define MS_TYPE_ANY                 (1u << 0)
#define MS_TYPE_NONE                (1u << 1)
#define MS_TYPE_BOOL                (1u << 2)
#define MS_TYPE_INT                 (1u << 3)
#define MS_TYPE_FLOAT               (1u << 4)
#define MS_TYPE_STR                 (1u << 5)
#define MS_TYPE_BYTES               (1u << 6)
#define MS_TYPE_BYTEARRAY           (1u << 7)
#define MS_TYPE_DATETIME            (1u << 8)
#define MS_TYPE_EXT                 (1u << 9)
#define MS_TYPE_STRUCT              (1u << 10)
#define MS_TYPE_ENUM                (1u << 11)
#define MS_TYPE_INTENUM             (1u << 12)
#define MS_TYPE_CUSTOM              (1u << 13)
#define MS_TYPE_CUSTOM_GENERIC      (1u << 14)
#define MS_TYPE_DICT                (1u << 15)
#define MS_TYPE_LIST                (1u << 16)
#define MS_TYPE_SET                 (1u << 17)
#define MS_TYPE_VARTUPLE            (1u << 18)
#define MS_TYPE_FIXTUPLE            (1u << 19)

typedef struct TypeNode {
    uint32_t types;
} TypeNode;

typedef struct TypeNodeExtra {
    TypeNode type;
    Py_ssize_t fixtuple_size;
    void* extra[];
} TypeNodeExtra;

typedef struct {
    PyHeapTypeObject base;
    PyObject *struct_fields;
    PyObject *struct_defaults;
    Py_ssize_t *struct_offsets;
    TypeNode **struct_types;
    char immutable;
    char asarray;
} StructMetaObject;

static PyTypeObject StructMetaType;
static PyTypeObject Ext_Type;
static int StructMeta_prep_types(PyObject *self);

#define StructMeta_GET_FIELDS(s) (((StructMetaObject *)(s))->struct_fields);
#define StructMeta_GET_NFIELDS(s) (PyTuple_GET_SIZE((((StructMetaObject *)(s))->struct_fields)));
#define StructMeta_GET_DEFAULTS(s) (((StructMetaObject *)(s))->struct_defaults);
#define StructMeta_GET_OFFSETS(s) (((StructMetaObject *)(s))->struct_offsets);

#define OPT_UNSET -1
#define OPT_FALSE 0
#define OPT_TRUE 1
#define STRUCT_MERGE_OPTIONS(opt1, opt2) (((opt2) != OPT_UNSET) ? (opt2) : (opt1))

static Py_ssize_t
TypeNode_get_size(TypeNode *type, Py_ssize_t *n_typenode) {
    Py_ssize_t n_obj = 0, n_type = 0;
    /* Custom types cannot share a union with anything except `None` */
    if (type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC)) {
        n_obj = 1;
    }
    else if (!(type->types & MS_TYPE_ANY)) {
        n_obj = ms_popcount(type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM | MS_TYPE_ENUM));

        if (type->types & MS_TYPE_DICT) n_type += 2;
        /* Only one array generic is allowed in a union */
        if (type->types & (MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_VARTUPLE)) n_type++;
        if (type->types & MS_TYPE_FIXTUPLE) n_type += ((TypeNodeExtra *)type)->fixtuple_size;
    }
    *n_typenode = n_type;
    return n_obj;
}

static MS_INLINE StructMetaObject *
TypeNode_get_struct(TypeNode *type) {
    /* Struct types are always first */
    return ((TypeNodeExtra *)type)->extra[0];
}

static MS_INLINE PyObject *
TypeNode_get_custom(TypeNode *type) {
    /* Custom types can't be mixed with anything */
    return ((TypeNodeExtra *)type)->extra[0];
}

static MS_INLINE PyObject *
TypeNode_get_intenum(TypeNode *type) {
    Py_ssize_t i = (type->types & MS_TYPE_STRUCT) != 0;
    return ((TypeNodeExtra *)type)->extra[i];
}

static MS_INLINE PyObject *
TypeNode_get_enum(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM));
    return ((TypeNodeExtra *)type)->extra[i];
}

static MS_INLINE void
TypeNode_get_dict(TypeNode *type, TypeNode **key, TypeNode **val) {
    Py_ssize_t i = ms_popcount(type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM | MS_TYPE_ENUM));
    *key = ((TypeNodeExtra *)type)->extra[i];
    *val = ((TypeNodeExtra *)type)->extra[i + 1];
}

static MS_INLINE Py_ssize_t
TypeNode_get_array_offset(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (MS_TYPE_STRUCT | MS_TYPE_INTENUM | MS_TYPE_ENUM));
    if (type->types & MS_TYPE_DICT) i += 2;
    return i;
}

static MS_INLINE TypeNode *
TypeNode_get_array(TypeNode *type) {
    return ((TypeNodeExtra *)type)->extra[TypeNode_get_array_offset(type)];
}

static void
TypeNode_Free(TypeNode *self) {
    if (self == NULL) return;
    Py_ssize_t n_obj, n_typenode, i;
    n_obj = TypeNode_get_size(self, &n_typenode);
    TypeNodeExtra *tex = (TypeNodeExtra *)self;

    for (i = 0; i < n_obj; i++) {
        PyObject *obj = (PyObject *)(tex->extra[i]);
        Py_XDECREF(obj);
    }
    for (i = n_obj; i < (n_obj + n_typenode); i++) {
        TypeNode *node = (TypeNode *)(tex->extra[i]);
        TypeNode_Free(node);
    }
    PyMem_Free(self);
}

static int
TypeNode_traverse(TypeNode *self, visitproc visit, void *arg) {
    if (self == NULL) return 0;
    Py_ssize_t n_obj, n_typenode, i;
    n_obj = TypeNode_get_size(self, &n_typenode);
    TypeNodeExtra *tex = (TypeNodeExtra *)self;

    for (i = 0; i < n_obj; i++) {
        PyObject *obj = (PyObject *)(tex->extra[i]);
        Py_VISIT(obj);
    }
    for (i = n_obj; i < (n_obj + n_typenode); i++) {
        int out;
        TypeNode *node = (TypeNode *)(tex->extra[i]);
        if ((out = TypeNode_traverse(node, visit, arg)) != 0) return out;
    }
    return 0;
}

static PyObject * TypeNode_Repr(TypeNode *self);

static PyObject *
typenode_repr_array(TypeNodeExtra *tex, const char* format, Py_ssize_t *extra_ind) {
    PyObject *inner = TypeNode_Repr(tex->extra[(*extra_ind)++]);
    if (inner == NULL) return NULL;
    PyObject *out = PyUnicode_FromFormat(format, inner);
    Py_DECREF(inner);
    return out;
}

static PyObject *
typenode_repr_dict(TypeNodeExtra *tex, Py_ssize_t *extra_ind) {
    PyObject *key = NULL, *val = NULL, *out = NULL;
    key = TypeNode_Repr(tex->extra[(*extra_ind)++]);
    if (key == NULL) goto cleanup;
    val = TypeNode_Repr(tex->extra[(*extra_ind)++]);
    if (val == NULL) goto cleanup;
    out = PyUnicode_FromFormat("Dict[%U, %U]", key, val);
cleanup:
    Py_XDECREF(key);
    Py_XDECREF(val);
    return out;
}

static PyObject *
typenode_repr_fixtuple(TypeNodeExtra *tex, Py_ssize_t *extra_ind) {
    PyObject *out = NULL, *parts = NULL, *part = NULL, *comma = NULL, *empty = NULL;
    Py_ssize_t i;

    parts = PyList_New((2 * tex->fixtuple_size) + 1);
    if (parts == NULL) goto cleanup;

    part = PyUnicode_FromString("Tuple[");
    if (part == NULL) goto cleanup;
    PyList_SET_ITEM(parts, 0, part);
    part = NULL;

    comma = PyUnicode_FromString(", ");
    if (comma == NULL) goto cleanup;

    for (i = 0; i < tex->fixtuple_size; i++) {
        part = TypeNode_Repr(tex->extra[(*extra_ind)++]);
        if (part == NULL) goto cleanup;
        PyList_SET_ITEM(parts, (2 * i) + 1, part);
        part = NULL;
        if (i < (tex->fixtuple_size - 1)) {
            PyList_SET_ITEM(parts, (2 * i) + 2, comma);
            Py_INCREF(comma);
        }
    }

    part = PyUnicode_FromString("]");
    if (part == NULL) goto cleanup;
    PyList_SET_ITEM(parts, 2 * tex->fixtuple_size, part);
    part = NULL;

    empty = PyUnicode_FromString("");
    if (empty == NULL) goto cleanup;
    out = PyUnicode_Join(empty, parts);

cleanup:
    Py_XDECREF(parts);
    Py_XDECREF(part);
    Py_XDECREF(comma);
    Py_XDECREF(empty);
    return out;
}

static PyObject *
typenode_repr_single(uint32_t *types, Py_ssize_t *extra_ind, TypeNode *self) {
    if (*types & MS_TYPE_ANY) {
        *types ^= MS_TYPE_ANY;
        return PyUnicode_FromString("Any");
    }
    else if (*types & MS_TYPE_NONE) {
        *types ^= MS_TYPE_NONE;
        return PyUnicode_FromString("None");
    }
    else if (*types & MS_TYPE_BOOL) {
        *types ^= MS_TYPE_BOOL;
        return PyUnicode_FromString("bool");
    }
    else if (*types & MS_TYPE_INT) {
        *types ^= MS_TYPE_INT;
        return PyUnicode_FromString("int");
    }
    else if (*types & MS_TYPE_FLOAT) {
        *types ^= MS_TYPE_FLOAT;
        return PyUnicode_FromString("float");
    }
    else if (*types & MS_TYPE_STR) {
        *types ^= MS_TYPE_STR;
        return PyUnicode_FromString("str");
    }
    else if (*types & MS_TYPE_BYTES) {
        *types ^= MS_TYPE_BYTES;
        return PyUnicode_FromString("bytes");
    }
    else if (*types & MS_TYPE_BYTEARRAY) {
        *types ^= MS_TYPE_BYTEARRAY;
        return PyUnicode_FromString("bytearray");
    }
    else if (*types & MS_TYPE_DATETIME) {
        *types ^= MS_TYPE_DATETIME;
        return PyUnicode_FromString("datetime");
    }
    else if (*types & MS_TYPE_EXT) {
        *types ^= MS_TYPE_EXT;
        return PyUnicode_FromString("Ext");
    }

    TypeNodeExtra *tex = (TypeNodeExtra *)self;

    if (*types & MS_TYPE_STRUCT) {
        *types ^= MS_TYPE_STRUCT;
        return PyUnicode_FromString(((PyTypeObject *)(tex->extra[(*extra_ind)++]))->tp_name);
    }
    else if (*types & MS_TYPE_ENUM) {
        *types ^= MS_TYPE_ENUM;
        return PyUnicode_FromString(((PyTypeObject *)(tex->extra[(*extra_ind)++]))->tp_name);
    }
    else if (*types & MS_TYPE_INTENUM) {
        *types ^= MS_TYPE_INTENUM;
        return PyUnicode_FromString(((PyTypeObject *)(tex->extra[(*extra_ind)++]))->tp_name);
    }
    else if (*types & MS_TYPE_CUSTOM) {
        *types ^= MS_TYPE_CUSTOM;
        return PyUnicode_FromString(((PyTypeObject *)(tex->extra[(*extra_ind)++]))->tp_name);
    }
    else if (*types & MS_TYPE_CUSTOM_GENERIC) {
        *types ^= MS_TYPE_CUSTOM_GENERIC;
        PyObject *obj = (PyObject *)(tex->extra[(*extra_ind)++]);
        return PyObject_Repr(obj);
    }
    else if (*types & MS_TYPE_DICT) {
        *types ^= MS_TYPE_DICT;
        return typenode_repr_dict(tex, extra_ind);
    }
    else if (*types & MS_TYPE_LIST) {
        *types ^= MS_TYPE_LIST;
        return typenode_repr_array(tex, "List[%U]", extra_ind);
    }
    else if (*types & MS_TYPE_SET) {
        *types ^= MS_TYPE_SET;
        return typenode_repr_array(tex, "Set[%U]", extra_ind);
    }
    else if (*types & MS_TYPE_VARTUPLE) {
        *types ^= MS_TYPE_VARTUPLE;
        return typenode_repr_array(tex, "Tuple[%U, ...]", extra_ind);
    }
    else if (*types & MS_TYPE_FIXTUPLE) {
        *types ^= MS_TYPE_FIXTUPLE;
        return typenode_repr_fixtuple(tex, extra_ind);
    }

    /* Should never get here */
    PyErr_SetString(PyExc_RuntimeError, "Unexpected failure in TypeNode repr");
    return NULL;
}

static PyObject *
TypeNode_Repr(TypeNode *self) {
    PyObject *out = NULL, *part = NULL, *parts = NULL, *comma = NULL, *empty = NULL;
    uint32_t types = self->types;
    int pop = ms_popcount(types);
    Py_ssize_t i, extra_ind = 0;

    if (pop == 1) {
        return typenode_repr_single(&types, &extra_ind, self);
    }
    else if (pop == 2 && (types & MS_TYPE_NONE)) {
        types ^= MS_TYPE_NONE;
        part = typenode_repr_single(&types, &extra_ind, self);
        if (part == NULL) return NULL;
        out = PyUnicode_FromFormat("Optional[%U]", part);
        Py_DECREF(part);
        return out;
    }

    parts = PyList_New(2 * pop + 1);
    if (parts == NULL) return NULL;

    part = PyUnicode_FromString("Union[");
    if (part == NULL) goto cleanup;
    PyList_SET_ITEM(parts, 0, part);
    part = NULL;

    comma = PyUnicode_FromString(", ");
    if (comma == NULL) goto cleanup;

    for (i = 0; i < pop; i++) {
        part = typenode_repr_single(&types, &extra_ind, self);
        if (part == NULL) goto cleanup;
        PyList_SET_ITEM(parts, (2 * i) + 1, part);
        part = NULL;
        if (i < (pop - 1)) {
            PyList_SET_ITEM(parts, (2 * i) + 2, comma);
            Py_INCREF(comma);
        }
    }

    part = PyUnicode_FromString("]");
    if (part == NULL) goto cleanup;
    PyList_SET_ITEM(parts, 2 * pop, part);
    part = NULL;

    empty = PyUnicode_FromString("");
    if (empty == NULL) goto cleanup;
    out = PyUnicode_Join(empty, parts);

cleanup:
    Py_XDECREF(parts);
    Py_XDECREF(part);
    Py_XDECREF(comma);
    Py_XDECREF(empty);
    return out;
}

typedef struct {
    PyObject *context;
    uint32_t types;
    PyObject *struct_obj;
    PyObject *intenum_obj;
    PyObject *enum_obj;
    PyObject *custom_obj;
    PyObject *array_el_obj;
    PyObject *dict_key_obj;
    PyObject *dict_val_obj;
} TypeNodeCollectState;

static TypeNode * TypeNode_Convert(PyObject *type);

static TypeNode *
typenode_from_collect_state(TypeNodeCollectState *state) {
    Py_ssize_t e_ind, n_extra = 0, fixtuple_size = 0;
    bool has_fixtuple = false;

    if (state->struct_obj != NULL) n_extra++;
    if (state->intenum_obj != NULL) n_extra++;
    if (state->enum_obj != NULL) n_extra++;
    if (state->custom_obj != NULL) n_extra++;
    if (state->dict_key_obj != NULL) n_extra += 2;
    if (state->array_el_obj != NULL) {
        if (PyTuple_Check(state->array_el_obj)) {
            has_fixtuple = true;
            fixtuple_size = PyTuple_Size(state->array_el_obj);
            n_extra += fixtuple_size;
        }
        else {
            n_extra++;
        }
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

    /* Use calloc so that `out->extra` is initialized, easing cleanup on error */
    TypeNodeExtra *out = (TypeNodeExtra *)PyMem_Calloc(
        1, sizeof(TypeNodeExtra) + n_extra * sizeof(void *)
    );
    if (out == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    out->type.types = state->types;
    out->fixtuple_size = fixtuple_size;

    /* Populate `extra` fields in order */
    e_ind = 0;
    if (state->struct_obj != NULL) {
        if (StructMeta_prep_types(state->struct_obj) < 0) goto error;
        Py_INCREF(state->struct_obj);
        out->extra[e_ind++] = state->struct_obj;
    }
    if (state->intenum_obj != NULL) {
        Py_INCREF(state->intenum_obj);
        out->extra[e_ind++] = state->intenum_obj;
    }
    if (state->enum_obj != NULL) {
        Py_INCREF(state->enum_obj);
        out->extra[e_ind++] = state->enum_obj;
    }
    if (state->custom_obj != NULL) {
        Py_INCREF(state->custom_obj);
        out->extra[e_ind++] = state->custom_obj;
    }
    if (state->dict_key_obj != NULL) {
        TypeNode *temp = TypeNode_Convert(state->dict_key_obj);
        if (temp == NULL) goto error;
        out->extra[e_ind++] = temp;
        temp = TypeNode_Convert(state->dict_val_obj);
        if (temp == NULL) goto error;
        out->extra[e_ind++] = temp;
    }
    if (state->array_el_obj != NULL) {
        if (has_fixtuple) {
            for (Py_ssize_t i = 0; i < fixtuple_size; i++) {
                TypeNode *temp = TypeNode_Convert(PyTuple_GET_ITEM(state->array_el_obj, i));
                if (temp == NULL) goto error;
                out->extra[e_ind++] = temp;
            }
        }
        else {
            TypeNode *temp = TypeNode_Convert(state->array_el_obj);
            if (temp == NULL) goto error;
            out->extra[e_ind++] = temp;
        }
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
typenode_collect_check_invariants(TypeNodeCollectState *state) {
    /* Ensure at least one type is set */
    if (state->types == 0) {
        PyErr_Format(PyExc_RuntimeError, "No types found, this is likely a bug!");
    }

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

    /* Ensure structs don't conflict with dict/array types */
    if (state->struct_obj) {
        bool asarray = (((StructMetaObject *)(state->struct_obj))->asarray == OPT_TRUE);
        if (asarray && state->array_el_obj) {
            PyErr_Format(
                PyExc_TypeError,
                "Type unions containing a Struct type with `asarray=True` may "
                "not contain other array-like types - type `%R` is not supported",
                state->context
            );
            return -1;
        }
        else if (!asarray && state->dict_key_obj) {
            PyErr_Format(
                PyExc_TypeError,
                "Type unions may not contain both a Struct type and a dict type "
                "- type `%R` is not supported",
                state->context
            );
            return -1;
        }
    }

    /* Ensure IntEnum doesn't conflict with int */
    if (state->intenum_obj && state->types & MS_TYPE_INT) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain both int and an IntEnum "
            "- type `%R` is not supported",
            state->context
        );
        return -1;
    }

    /* Ensure Enum doesn't conflict with str */
    if (state->enum_obj && state->types & MS_TYPE_STR) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain both str and an Enum "
            "- type `%R` is not supported",
            state->context
        );
        return -1;
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
typenode_collect_array(TypeNodeCollectState *state, uint32_t type, PyObject *obj) {
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
typenode_collect_custom(TypeNodeCollectState *state, uint32_t type, PyObject *obj) {
    if (state->custom_obj != NULL) {
        return typenode_collect_err_unique(state, "custom");
    }
    state->types |= type;
    Py_INCREF(obj);
    state->custom_obj = obj;
    return 0;
}

static void
typenode_collect_clear_state(TypeNodeCollectState *state) {
    Py_CLEAR(state->struct_obj);
    Py_CLEAR(state->struct_obj);
    Py_CLEAR(state->intenum_obj);
    Py_CLEAR(state->enum_obj);
    Py_CLEAR(state->custom_obj);
    Py_CLEAR(state->array_el_obj);
    Py_CLEAR(state->dict_key_obj);
    Py_CLEAR(state->dict_val_obj);
}

static int
typenode_collect_type(TypeNodeCollectState *state, PyObject *obj) {
    int out = -1;
    PyObject *origin = NULL, *args = NULL;
    MsgspecState *st = msgspec_get_global_state();

    /* If `Any` type already encountered, nothing to do */
    if (state->types & MS_TYPE_ANY) return 0;
    if (obj == st->typing_any) {
        /* Any takes precedence, drop all existing and update type flags */
        typenode_collect_clear_state(state);
        state->types = MS_TYPE_ANY;
        return 0;
    }

    /* Handle Scalar types */
    if (obj == Py_None || obj == NONE_TYPE) {
        state->types |= MS_TYPE_NONE;
        return 0;
    }
    else if (obj == (PyObject *)(&PyBool_Type)) {
        state->types |= MS_TYPE_BOOL;
        return 0;
    }
    else if (obj == (PyObject *)(&PyLong_Type)) {
        state->types |= MS_TYPE_INT;
        return 0;
    }
    else if (obj == (PyObject *)(&PyFloat_Type)) {
        state->types |= MS_TYPE_FLOAT;
        return 0;
    }
    else if (obj == (PyObject *)(&PyUnicode_Type)) {
        state->types |= MS_TYPE_STR;
        return 0;
    }
    else if (obj == (PyObject *)(&PyBytes_Type)) {
        state->types |= MS_TYPE_BYTES;
        return 0;
    }
    else if (obj == (PyObject *)(&PyByteArray_Type)) {
        state->types |= MS_TYPE_BYTEARRAY;
        return 0;
    }
    else if (obj == (PyObject *)(PyDateTimeAPI->DateTimeType)) {
        state->types |= MS_TYPE_DATETIME;
        return 0;
    }
    else if (obj == (PyObject *)(&Ext_Type)) {
        state->types |= MS_TYPE_EXT;
        return 0;
    }

    /* Struct types */
    if (Py_TYPE(obj) == &StructMetaType) {
        /* May only have one Struct type in a union */
        if (state->struct_obj != NULL) {
            return typenode_collect_err_unique(state, "Struct");
        }
        state->types |= MS_TYPE_STRUCT;
        Py_INCREF(obj);
        state->struct_obj = obj;
        return 0;
    }

    /* Enum types */
    if (PyType_Check(obj) && PyType_IsSubtype((PyTypeObject *)obj, st->EnumType)) {
        if (PyType_IsSubtype((PyTypeObject *)obj, &PyLong_Type)) {
            /* IntEnum */
            if (state->intenum_obj != NULL) {
                return typenode_collect_err_unique(state, "IntEnum");
            }
            state->types |= MS_TYPE_INTENUM;
            Py_INCREF(obj);
            state->intenum_obj = obj;
            return 0;
        }
        else {
            /* Enum */
            if (state->enum_obj != NULL) {
                return typenode_collect_err_unique(state, "Enum");
            }
            state->types |= MS_TYPE_ENUM;
            Py_INCREF(obj);
            state->enum_obj = obj;
            return 0;
        }
    }

    /* Generic collections can be spelled a few different ways, so the below
     * logic is a bit split up as we discover what type of thing `obj` is. */
    if (obj == (PyObject*)(&PyDict_Type) || obj == st->typing_dict) {
        return typenode_collect_dict(state, st->typing_any, st->typing_any);
    }
    else if (obj == (PyObject*)(&PyList_Type) || obj == st->typing_list) {
        return typenode_collect_array(state, MS_TYPE_LIST, st->typing_any);
    }
    else if (obj == (PyObject*)(&PySet_Type) || obj == st->typing_set) {
        return typenode_collect_array(state, MS_TYPE_SET, st->typing_any);
    }
    else if (obj == (PyObject*)(&PyTuple_Type) || obj == st->typing_tuple) {
        return typenode_collect_array(state, MS_TYPE_VARTUPLE, st->typing_any);
    }

    /* Attempt to extract __origin__/__args__ from the obj as a typing object */
    if ((origin = PyObject_GetAttr(obj, st->str___origin__)) == NULL ||
            (args = PyObject_GetAttr(obj, st->str___args__)) == NULL) {
        /* Not a parametrized generic, must be a custom type */
        PyErr_Clear();
        if (!PyType_Check(origin != NULL ? origin : obj)) goto invalid;
        out = typenode_collect_custom(
            state,
            origin != NULL ? MS_TYPE_CUSTOM_GENERIC : MS_TYPE_CUSTOM,
            obj
        );
        goto done;
    }

    if (origin == (PyObject*)(&PyDict_Type)) {
        if (PyTuple_Size(args) != 2) goto invalid;
        out = typenode_collect_dict(
            state, PyTuple_GET_ITEM(args, 0), PyTuple_GET_ITEM(args, 1)
        );
        goto done;
    }
    else if (origin == (PyObject*)(&PyList_Type)) {
        if (PyTuple_Size(args) != 1) goto invalid;
        out = typenode_collect_array(
            state, MS_TYPE_LIST, PyTuple_GET_ITEM(args, 0)
        );
        goto done;
    }
    else if (origin == (PyObject*)(&PySet_Type)) {
        if (PyTuple_Size(args) != 1) goto invalid;
        out = typenode_collect_array(
            state, MS_TYPE_SET, PyTuple_GET_ITEM(args, 0)
        );
        goto done;
    }
    else if (origin == (PyObject*)(&PyTuple_Type)) {
        if (PyTuple_Size(args) == 2 && PyTuple_GET_ITEM(args, 1) == Py_Ellipsis) {
            out = typenode_collect_array(
                state, MS_TYPE_VARTUPLE, PyTuple_GET_ITEM(args, 0)
            );
        }
        else {
            out = typenode_collect_array(state, MS_TYPE_FIXTUPLE, args);
        }
        goto done;
    }
    else if (origin == st->typing_union) {
        for (Py_ssize_t i = 0; i < PyTuple_Size(args); i++) {
            out = typenode_collect_type(state, PyTuple_GET_ITEM(args, i));
            if (out < 0) break;
        }
        goto done;
    }
    else {
        if (!PyType_Check(origin)) goto invalid;
        /* A parametrized type, but not one we natively support. */
        out = typenode_collect_custom(state, MS_TYPE_CUSTOM_GENERIC, obj);
        goto done;
    }

invalid:
    PyErr_Format(PyExc_TypeError, "Type '%R' is not supported", obj);

done:
    Py_XDECREF(origin);
    Py_XDECREF(args);
    return out;
}

static TypeNode *
TypeNode_Convert(PyObject *obj) {
    TypeNode *out = NULL;
    TypeNodeCollectState state = {0};
    state.context = obj;

    /* Traverse `obj` to collect all type annotations at this level */
    if (typenode_collect_type(&state, obj) < 0) goto done;
    /* Check type invariants to ensure Union types are valid */
    if (typenode_collect_check_invariants(&state) < 0) goto done;
    /* Populate a new TypeNode, recursing into subtypes as needed */
    out = typenode_from_collect_state(&state);
done:
    typenode_collect_clear_state(&state);
    return out;
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
static PyObject *struct_freelist[STRUCT_FREELIST_MAX_SIZE];
static int struct_freelist_len[STRUCT_FREELIST_MAX_SIZE];

static void
Struct_freelist_clear(void) {
    Py_ssize_t i;
    PyObject *obj;
    for (i = 0; i < STRUCT_FREELIST_MAX_SIZE; i++) {
        while ((obj = struct_freelist[i]) != NULL) {
            struct_freelist[i] = (PyObject *)obj->ob_type;
            PyObject_GC_Del(obj);
        }
        struct_freelist_len[i] = 0;
    }
}

static PyObject *
Struct_alloc(PyTypeObject *type) {
    Py_ssize_t size;
    PyObject *obj;

    size = (type->tp_basicsize - sizeof(PyObject)) / sizeof(void *);

    if (size > 0 &&
        size <= STRUCT_FREELIST_MAX_SIZE &&
        struct_freelist[size - 1] != NULL)
    {
        /* Pop object off freelist */
        obj = struct_freelist[size - 1];
        struct_freelist[size - 1] = (PyObject *)obj->ob_type;
        struct_freelist_len[size - 1]--;
        /* Initialize the object. This is mirrored from within `PyObject_Init`,
         * as well as PyType_GenericAlloc */
        obj->ob_type = type;
        Py_INCREF(type);
        _Py_NewReference(obj);
        PyObject_GC_Track(obj);
    }
    else {
        obj = PyType_GenericAlloc(type, 0);
    }
    return obj;
}

/* Mirrored from cpython Objects/typeobject.c */
static void
clear_slots(PyTypeObject *type, PyObject *self)
{
    Py_ssize_t i, n;
    PyMemberDef *mp;

    n = Py_SIZE(type);
    mp = PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
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
    Py_ssize_t size;
    PyTypeObject *type, *base;

    type = Py_TYPE(self);

    PyObject_GC_UnTrack(self);

    size = (type->tp_basicsize - sizeof(PyObject)) / sizeof(void *);

    Py_TRASHCAN_BEGIN(self, Struct_dealloc)
    base = type;
    while (base != NULL) {
        if (Py_SIZE(base))
            clear_slots(base, self);
        base = base->tp_base;
    }
    Py_TRASHCAN_END

    if (size > 0 &&
        size <= STRUCT_FREELIST_MAX_SIZE &&
        struct_freelist_len[size - 1] < STRUCT_FREELIST_MAX_PER_SIZE)
    {
        /* Push object onto freelist */
        self->ob_type = (PyTypeObject *)(struct_freelist[size - 1]);
        struct_freelist_len[size - 1]++;
        struct_freelist[size - 1] = self;
    }
    else {
        type->tp_free(self);
    }
    Py_DECREF(type);
}

#else

static inline PyObject *
Struct_alloc(PyTypeObject *type) {
    return type->tp_alloc(type, 0);
}

#endif /* Struct freelist */

static Py_ssize_t
StructMeta_get_field_index(
    StructMetaObject *self, const char * key, Py_ssize_t key_size, Py_ssize_t *pos
) {
    const char *field;
    Py_ssize_t nfields, field_size, i, ind, offset = *pos;
    nfields = PyTuple_GET_SIZE(self->struct_fields);
    for (i = 0; i < nfields; i++) {
        ind = (i + offset) % nfields;
        field = unicode_str_and_size(
            PyTuple_GET_ITEM(self->struct_fields, ind), &field_size
        );
        if (field == NULL) return -1;
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = (ind + 1) % nfields;
            return ind;
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

static PyObject *
StructMeta_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    StructMetaObject *cls = NULL;
    PyObject *name = NULL, *bases = NULL, *orig_dict = NULL;
    PyObject *arg_fields = NULL, *kwarg_fields = NULL, *new_dict = NULL, *new_args = NULL;
    PyObject *fields = NULL, *defaults = NULL, *offsets_lk = NULL, *offset = NULL, *slots = NULL, *slots_list = NULL;
    PyObject *base, *base_fields, *base_defaults, *annotations;
    PyObject *default_val, *field;
    Py_ssize_t nfields, ndefaults, i, j, k;
    Py_ssize_t *offsets = NULL, *base_offsets;
    int arg_immutable = -1, arg_asarray = -1, immutable = -1, asarray = -1;

    static char *kwlist[] = {"name", "bases", "dict", "immutable", "asarray", NULL};

    /* Parse arguments: (name, bases, dict) */
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "UO!O!|$pp:StructMeta.__new__", kwlist,
            &name, &PyTuple_Type, &bases, &PyDict_Type, &orig_dict,
            &arg_immutable, &arg_asarray))
        return NULL;

    if (PyDict_GetItemString(orig_dict, "__init__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __init__");
        return NULL;
    }
    if (PyDict_GetItemString(orig_dict, "__new__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __new__");
        return NULL;
    }
    if (PyDict_GetItemString(orig_dict, "__slots__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __slots__");
        return NULL;
    }

    arg_fields = PyDict_New();
    if (arg_fields == NULL)
        goto error;
    kwarg_fields = PyDict_New();
    if (kwarg_fields == NULL)
        goto error;
    offsets_lk = PyDict_New();
    if (offsets_lk == NULL)
        goto error;

    for (i = PyTuple_GET_SIZE(bases) - 1; i >= 0; i--) {
        base = PyTuple_GET_ITEM(bases, i);
        if ((PyTypeObject *)base == &StructMixinType) {
            continue;
        }

        if (!(PyType_Check(base) && (Py_TYPE(base) == &StructMetaType))) {
            PyErr_SetString(
                PyExc_TypeError,
                "All base classes must be subclasses of msgspec.Struct"
            );
            goto error;
        }
        immutable = STRUCT_MERGE_OPTIONS(immutable, ((StructMetaObject *)base)->immutable);
        asarray = STRUCT_MERGE_OPTIONS(asarray, ((StructMetaObject *)base)->asarray);
        base_fields = StructMeta_GET_FIELDS(base);
        base_defaults = StructMeta_GET_DEFAULTS(base);
        base_offsets = StructMeta_GET_OFFSETS(base);
        nfields = PyTuple_GET_SIZE(base_fields);
        ndefaults = PyTuple_GET_SIZE(base_defaults);
        for (j = 0; j < nfields; j++) {
            field = PyTuple_GET_ITEM(base_fields, j);
            if (j < (nfields - ndefaults)) {
                if (PyDict_SetItem(arg_fields, field, Py_None) < 0)
                    goto error;
                if (dict_discard(kwarg_fields, field) < 0)
                    goto error;
            }
            else {
                default_val = PyTuple_GET_ITEM(base_defaults, (j + ndefaults - nfields));
                if (PyDict_SetItem(kwarg_fields, field, default_val) < 0)
                    goto error;
                if (dict_discard(arg_fields, field) < 0)
                    goto error;
            }
            offset = PyLong_FromSsize_t(base_offsets[j]);
            if (offset == NULL)
                goto error;
            if (PyDict_SetItem(offsets_lk, field, offset) < 0)
                goto error;
            Py_DECREF(offset);
        }
    }
    immutable = STRUCT_MERGE_OPTIONS(immutable, arg_immutable);
    asarray = STRUCT_MERGE_OPTIONS(asarray, arg_asarray);

    new_dict = PyDict_Copy(orig_dict);
    if (new_dict == NULL)
        goto error;
    slots_list = PyList_New(0);
    if (slots_list == NULL)
        goto error;

    annotations = PyDict_GetItemString(orig_dict, "__annotations__");
    if (annotations != NULL) {
        if (!PyDict_Check(annotations)) {
            PyErr_SetString(PyExc_TypeError, "__annotations__ must be a dict");
            goto error;
        }

        i = 0;
        while (PyDict_Next(annotations, &i, &field, NULL)) {
            if (!PyUnicode_CheckExact(field)) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "__annotations__ keys must be strings"
                );
                goto error;
            }

            /* If the field is new, add it to slots */
            if (PyDict_GetItem(arg_fields, field) == NULL && PyDict_GetItem(kwarg_fields, field) == NULL) {
                if (PyList_Append(slots_list, field) < 0)
                    goto error;
            }

            default_val = PyDict_GetItem(new_dict, field);
            if (default_val != NULL) {
                if (dict_discard(arg_fields, field) < 0)
                    goto error;
                if (PyDict_SetItem(kwarg_fields, field, default_val) < 0)
                    goto error;
                if (dict_discard(new_dict, field) < 0)
                    goto error;
            }
            else {
                if (dict_discard(kwarg_fields, field) < 0)
                    goto error;
                if (PyDict_SetItem(arg_fields, field, Py_None) < 0)
                    goto error;
            }
        }
    }

    fields = PyTuple_New(PyDict_Size(arg_fields) + PyDict_Size(kwarg_fields));
    if (fields == NULL)
        goto error;
    defaults = PyTuple_New(PyDict_Size(kwarg_fields));
    if (defaults == NULL)
        goto error;

    i = 0;
    j = 0;
    while (PyDict_Next(arg_fields, &i, &field, NULL)) {
        Py_INCREF(field);
        PyTuple_SET_ITEM(fields, j, field);
        j++;
    }
    i = 0;
    k = 0;
    while (PyDict_Next(kwarg_fields, &i, &field, &default_val)) {
        Py_INCREF(field);
        PyTuple_SET_ITEM(fields, j, field);
        Py_INCREF(default_val);
        PyTuple_SET_ITEM(defaults, k, default_val);
        j++;
        k++;
    }
    Py_CLEAR(arg_fields);
    Py_CLEAR(kwarg_fields);

    if (PyList_Sort(slots_list) < 0)
        goto error;

    slots = PyList_AsTuple(slots_list);
    if (slots == NULL)
        goto error;
    Py_CLEAR(slots_list);

    if (PyDict_SetItemString(new_dict, "__slots__", slots) < 0)
        goto error;
    Py_CLEAR(slots);

    new_args = Py_BuildValue("(OOO)", name, bases, new_dict);
    if (new_args == NULL)
        goto error;

    cls = (StructMetaObject *) PyType_Type.tp_new(type, new_args, NULL);
    if (cls == NULL)
        goto error;
    ((PyTypeObject *)cls)->tp_vectorcall = (vectorcallfunc)Struct_vectorcall;
#if STRUCT_FREELIST_MAX_SIZE > 0
    ((PyTypeObject *)cls)->tp_dealloc = Struct_dealloc;
#endif
    Py_CLEAR(new_args);

    PyMemberDef *mp = PyHeapType_GET_MEMBERS(cls);
    for (i = 0; i < Py_SIZE(cls); i++, mp++) {
        offset = PyLong_FromSsize_t(mp->offset);
        if (offset == NULL)
            goto error;
        if (PyDict_SetItemString(offsets_lk, mp->name, offset) < 0)
            goto error;
    }
    offsets = PyMem_New(Py_ssize_t, PyTuple_GET_SIZE(fields));
    if (offsets == NULL)
        goto error;
    for (i = 0; i < PyTuple_GET_SIZE(fields); i++) {
        field = PyTuple_GET_ITEM(fields, i);
        offset = PyDict_GetItem(offsets_lk, field);
        if (offset == NULL) {
            PyErr_Format(PyExc_RuntimeError, "Failed to get offset for %R", field);
            goto error;
        }
        offsets[i] = PyLong_AsSsize_t(offset);
    }
    Py_CLEAR(offsets_lk);

    cls->struct_fields = fields;
    cls->struct_defaults = defaults;
    cls->struct_offsets = offsets;
    cls->immutable = immutable;
    cls->asarray = asarray;
    return (PyObject *) cls;
error:
    Py_XDECREF(arg_fields);
    Py_XDECREF(kwarg_fields);
    Py_XDECREF(fields);
    Py_XDECREF(defaults);
    Py_XDECREF(new_dict);
    Py_XDECREF(slots_list);
    Py_XDECREF(new_args);
    Py_XDECREF(offsets_lk);
    Py_XDECREF(offset);
    if (offsets != NULL)
        PyMem_Free(offsets);
    return NULL;
}

static int
StructMeta_prep_types(PyObject *py_self) {
    StructMetaObject *self = (StructMetaObject *)py_self;
    MsgspecState *st;
    TypeNode *type;
    Py_ssize_t i, nfields;
    PyObject *obj, *field, *annotations = NULL;

    if (self->struct_types != NULL) return 0;

    nfields = PyTuple_GET_SIZE(self->struct_fields);

    st = msgspec_get_global_state();
    annotations = CALL_ONE_ARG(st->get_type_hints, py_self);
    if (annotations == NULL) goto error;

    self->struct_types = PyMem_Calloc(nfields, sizeof(TypeNode*));
    if (self->struct_types == NULL)  {
        PyErr_NoMemory();
        return -1;
    }

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(self->struct_fields, i);
        obj = PyDict_GetItem(annotations, field);
        if (obj == NULL) goto error;
        type = TypeNode_Convert(obj);
        if (type == NULL) goto error;
        self->struct_types[i] = type;
    }

    Py_DECREF(annotations);
    return 0;

error:
    Py_XDECREF(annotations);
    if (self->struct_types != NULL) {
        for (i = 0; i < nfields; i++) {
            TypeNode_Free(self->struct_types[i]);
        }
    }
    PyMem_Free(self->struct_types);
    self->struct_types = NULL;
    return -1;
}

static int
StructMeta_traverse(StructMetaObject *self, visitproc visit, void *arg)
{
    int out;
    Py_ssize_t i, nfields;
    Py_VISIT(self->struct_fields);
    Py_VISIT(self->struct_defaults);
    if (self->struct_types != NULL) {
        nfields = PyTuple_GET_SIZE(self->struct_fields);
        for (i = 0; i < nfields; i++) {
            out = TypeNode_traverse(self->struct_types[i], visit, arg);
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
    PyMem_Free(self->struct_offsets);
    if (self->struct_types != NULL) {
        for (i = 0; i < nfields; i++) {
            TypeNode_Free(self->struct_types[i]);
        }
    }
    return PyType_Type.tp_clear((PyObject *)self);
}

static void
StructMeta_dealloc(StructMetaObject *self)
{
    StructMeta_clear(self);
    PyType_Type.tp_dealloc((PyObject *)self);
}

static PyObject*
StructMeta_immutable(StructMetaObject *self, void *closure)
{
    if (self->immutable == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_asarray(StructMetaObject *self, void *closure)
{
    if (self->asarray == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructMeta_signature(StructMetaObject *self, void *closure)
{
    Py_ssize_t nfields, ndefaults, npos, i;
    MsgspecState *st;
    PyObject *res = NULL;
    PyObject *inspect = NULL;
    PyObject *parameter_cls = NULL;
    PyObject *parameter_empty = NULL;
    PyObject *parameter_kind = NULL;
    PyObject *signature_cls = NULL;
    PyObject *annotations = NULL;
    PyObject *parameters = NULL;
    PyObject *temp_args = NULL, *temp_kwargs = NULL;
    PyObject *field, *default_val, *parameter, *annotation;

    st = msgspec_get_global_state();

    nfields = PyTuple_GET_SIZE(self->struct_fields);
    ndefaults = PyTuple_GET_SIZE(self->struct_defaults);
    npos = nfields - ndefaults;

    inspect = PyImport_ImportModule("inspect");
    if (inspect == NULL)
        goto cleanup;
    parameter_cls = PyObject_GetAttrString(inspect, "Parameter");
    if (parameter_cls == NULL)
        goto cleanup;
    parameter_empty = PyObject_GetAttrString(parameter_cls, "empty");
    if (parameter_empty == NULL)
        goto cleanup;
    parameter_kind = PyObject_GetAttrString(parameter_cls, "POSITIONAL_OR_KEYWORD");
    if (parameter_kind == NULL)
        goto cleanup;
    signature_cls = PyObject_GetAttrString(inspect, "Signature");
    if (signature_cls == NULL)
        goto cleanup;

    annotations = CALL_ONE_ARG(st->get_type_hints, (PyObject *)self);
    if (annotations == NULL)
        goto cleanup;

    parameters = PyList_New(nfields);
    if (parameters == NULL)
        return NULL;

    temp_args = PyTuple_New(0);
    if (temp_args == NULL)
        goto cleanup;
    temp_kwargs = PyDict_New();
    if (temp_kwargs == NULL)
        goto cleanup;
    if (PyDict_SetItemString(temp_kwargs, "kind", parameter_kind) < 0)
        goto cleanup;

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(self->struct_fields, i);
        if (i < npos) {
            default_val = parameter_empty;
        } else {
            default_val = PyTuple_GET_ITEM(self->struct_defaults, i - npos);
        }
        annotation = PyDict_GetItem(annotations, field);
        if (annotation == NULL) {
            annotation = parameter_empty;
        }
        if (PyDict_SetItemString(temp_kwargs, "name", field) < 0)
            goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "default", default_val) < 0)
            goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "annotation", annotation) < 0)
            goto cleanup;
        parameter = PyObject_Call(parameter_cls, temp_args, temp_kwargs);
        if (parameter == NULL)
            goto cleanup;
        PyList_SET_ITEM(parameters, i, parameter);
    }
    res = CALL_ONE_ARG(signature_cls, parameters);
cleanup:
    Py_XDECREF(inspect);
    Py_XDECREF(parameter_cls);
    Py_XDECREF(parameter_empty);
    Py_XDECREF(parameter_kind);
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
    {"__match_args__", T_OBJECT_EX, offsetof(StructMetaObject, struct_fields), READONLY, "Positional match args"},
    {NULL},
};

static PyGetSetDef StructMeta_getset[] = {
    {"__signature__", (getter) StructMeta_signature, NULL, NULL, NULL},
    {"immutable", (getter) StructMeta_immutable, NULL, NULL, NULL},
    {"asarray", (getter) StructMeta_asarray, NULL, NULL, NULL},
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
maybe_deepcopy_default(PyObject *obj) {
    MsgspecState *st;
    PyObject *copy = NULL, *deepcopy = NULL, *res = NULL;
    PyTypeObject *type = Py_TYPE(obj);

    /* Known non-collection or recursively immutable types */
    if (obj == Py_None || obj == Py_False || obj == Py_True ||
        type == &PyLong_Type || type == &PyFloat_Type ||
        type == &PyBytes_Type || type == &PyUnicode_Type ||
        type == &PyByteArray_Type || type == &PyFrozenSet_Type
    ) {
        Py_INCREF(obj);
        return obj;
    }
    else if (type == &PyTuple_Type && (PyTuple_GET_SIZE(obj) == 0)) {
        Py_INCREF(obj);
        return obj;
    }
    else if (type == PyDateTimeAPI->DateTimeType ||
             type == PyDateTimeAPI->DeltaType ||
             type == PyDateTimeAPI->DateType ||
             type == PyDateTimeAPI->TimeType
    ) {
        Py_INCREF(obj);
        return obj;
    }

    st = msgspec_get_global_state();
    if (PyType_IsSubtype(type, st->EnumType)) {
        Py_INCREF(obj);
        return obj;
    }

    /* Fast paths for known empty collections */
    if (type == &PyDict_Type && PyDict_Size(obj) == 0) {
        return PyDict_New();
    }
    else if (type == &PyList_Type && PyList_GET_SIZE(obj) == 0) {
        return PyList_New(0);
    }
    else if (type == &PySet_Type && PySet_GET_SIZE(obj) == 0) {
        return PySet_New(NULL);
    }
    /* More complicated, invoke full deepcopy */
    copy = PyImport_ImportModule("copy");
    if (copy == NULL)
        goto cleanup;
    deepcopy = PyObject_GetAttrString(copy, "deepcopy");
    if (deepcopy == NULL)
        goto cleanup;
    res = CALL_ONE_ARG(deepcopy, obj);
cleanup:
    Py_XDECREF(copy);
    Py_XDECREF(deepcopy);
    return res;
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

static int
Struct_fill_in_defaults(StructMetaObject *st_type, PyObject *obj) {
    Py_ssize_t nfields, ndefaults, i;
    bool should_untrack;

    nfields = PyTuple_GET_SIZE(st_type->struct_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    should_untrack = PyObject_IS_GC(obj);

    for (i = 0; i < nfields; i++) {
        PyObject *val = Struct_get_index_noerror(obj, i);
        if (val == NULL) {
            if (i < (nfields - ndefaults)) {
                PyErr_Format(
                    msgspec_get_global_state()->DecodingError,
                    "Error decoding `%s`: missing required field `%S`",
                    ((PyTypeObject *)st_type)->tp_name,
                    PyTuple_GET_ITEM(st_type->struct_fields, i)
                );
                return -1;
            }
            else {
                /* Fill in default */
                val = maybe_deepcopy_default(
                    PyTuple_GET_ITEM(st_type->struct_defaults, i - (nfields - ndefaults))
                );
                if (val == NULL) return -1;
                Struct_set_index(obj, i, val);
            }
        }
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    if (should_untrack)
        PyObject_GC_UnTrack(obj);
    return 0;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyObject *self, *fields, *defaults, *field, *val;
    Py_ssize_t nargs, nkwargs, nfields, ndefaults, npos, i;
    int should_untrack;

    self = Struct_alloc(cls);
    if (self == NULL)
        return NULL;

    fields = StructMeta_GET_FIELDS(Py_TYPE(self));
    defaults = StructMeta_GET_DEFAULTS(Py_TYPE(self));

    nargs = PyVectorcall_NARGS(nargsf);
    nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    ndefaults = PyTuple_GET_SIZE(defaults);
    nfields = PyTuple_GET_SIZE(fields);
    npos = nfields - ndefaults;

    if (nargs > nfields) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra positional arguments provided"
        );
        goto error;
    }

    should_untrack = PyObject_IS_GC(self);

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(fields, i);
        val = (nkwargs == 0) ? NULL : find_keyword(kwnames, args + nargs, field);
        if (val != NULL) {
            if (i < nargs) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Argument '%U' given by name and position",
                    field
                );
                goto error;
            }
            Py_INCREF(val);
            nkwargs -= 1;
        }
        else if (i < nargs) {
            val = args[i];
            Py_INCREF(val);
        }
        else if (i < npos) {
            PyErr_Format(
                PyExc_TypeError,
                "Missing required argument '%U'",
                field
            );
            goto error;
        }
        else {
            val = maybe_deepcopy_default(PyTuple_GET_ITEM(defaults, i - npos));
            if (val == NULL)
                goto error;
        }
        Struct_set_index(self, i, val);
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    if (nkwargs > 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra keyword arguments provided"
        );
        goto error;
    }
    if (should_untrack)
        PyObject_GC_UnTrack(self);
    return self;

error:
    Py_DECREF(self);
    return NULL;
}

static int
Struct_setattro(PyObject *self, PyObject *key, PyObject *value) {
    if (((StructMetaObject *)Py_TYPE(self))->immutable == OPT_TRUE) {
        PyErr_Format(PyExc_TypeError, "immutable type: '%s'", Py_TYPE(self)->tp_name);
        return -1;
    }
    if (PyObject_GenericSetAttr(self, key, value) < 0)
        return -1;
    if (value != NULL && OBJ_IS_GC(value) && !IS_TRACKED(self))
        PyObject_GC_Track(self);
    return 0;
}

static PyObject *
Struct_repr(PyObject *self) {
    int recursive;
    Py_ssize_t nfields, i;
    PyObject *parts = NULL, *empty = NULL, *out = NULL;
    PyObject *part, *fields, *field, *val;

    recursive = Py_ReprEnter(self);
    if (recursive != 0) {
        out = (recursive < 0) ? NULL : PyUnicode_FromString("...");
        goto cleanup;
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

static Py_hash_t
Struct_hash(PyObject *self) {
    PyObject *val;
    Py_ssize_t i, nfields;
    Py_uhash_t acc = MS_HASH_XXPRIME_5;

    if (((StructMetaObject *)Py_TYPE(self))->immutable != OPT_TRUE) {
        PyErr_Format(PyExc_TypeError, "unhashable type: '%s'", Py_TYPE(self)->tp_name);
        return -1;
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
    int status;
    PyObject *left, *right;
    Py_ssize_t nfields, i;

    if (!(Py_TYPE(Py_TYPE(other)) == &StructMetaType)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    status = Py_TYPE(self) == Py_TYPE(other);
    if (status == 0)
        goto done;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));

    for (i = 0; i < nfields; i++) {
        left = Struct_get_index(self, i);
        if (left == NULL)
            return NULL;
        right = Struct_get_index(other, i);
        if (right == NULL)
            return NULL;
        Py_INCREF(left);
        Py_INCREF(right);
        status = PyObject_RichCompareBool(left, right, Py_EQ);
        Py_DECREF(left);
        Py_DECREF(right);
        if (status < 0)
            return NULL;
        if (status == 0)
            goto done;
    }
done:
    if (status == ((op == Py_EQ) ? 1 : 0)) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
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
    /* If self is untracked, then copy is untracked */
    if (PyObject_IS_GC(self) && !IS_TRACKED(self))
        PyObject_GC_UnTrack(res);
    return res;
error:
    Py_DECREF(res);
    return NULL;
}

static PyObject *
Struct_reduce(PyObject *self, PyObject *args)
{
    Py_ssize_t i, nfields;
    PyObject *values, *val;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));
    values = PyTuple_New(nfields);
    if (values == NULL)
        return NULL;
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto error;
        Py_INCREF(val);
        PyTuple_SET_ITEM(values, i, val);
    }
    return PyTuple_Pack(2, Py_TYPE(self), values);
error:
    Py_XDECREF(values);
    return NULL;
}

static PyObject *
StructMixin_fields(PyObject *self, void *closure) {
    PyObject *out;
    out = StructMeta_GET_FIELDS(Py_TYPE(self));
    Py_INCREF(out);
    return out;
}

static PyObject *
StructMixin_defaults(PyObject *self, void *closure) {
    PyObject *out;
    out = StructMeta_GET_DEFAULTS(Py_TYPE(self));
    Py_INCREF(out);
    return out;
}

static PyMethodDef Struct_methods[] = {
    {"__copy__", Struct_copy, METH_NOARGS, "copy a struct"},
    {"__reduce__", Struct_reduce, METH_NOARGS, "reduce a struct"},
    {NULL, NULL},
};

static PyGetSetDef StructMixin_getset[] = {
    {"__struct_fields__", (getter) StructMixin_fields, NULL, "Struct fields", NULL},
    {"__struct_defaults__", (getter) StructMixin_defaults, NULL, "Struct defaults", NULL},
    {NULL},
};

static PyTypeObject StructMixinType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._core._StructMixin",
    .tp_basicsize = 0,
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_setattro = Struct_setattro,
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
"Note that mutable default values are deepcopied in the constructor to\n"
"prevent accidental sharing.\n"
"\n"
"Additional class options can be enabled by passing keywords to the class\n"
"definition (see example below). The following options exist:\n"
"\n"
"- ``immutable``: whether instances of the class are immutable. If true,\n"
"  attribute assignment is disabled and a corresponding ``__hash__`` is defined.\n"
"- ``asarray``: whether instances of the class should be serialized as\n"
"  MessagePack arrays, rather than dicts (the default).\n"
"\n"
"Structs automatically define ``__init__``, ``__eq__``, ``__repr__``, and\n"
"``__copy__`` methods. Additional methods can be defined on the class as\n"
"needed. Note that ``__init__``/``__new__`` cannot be overridden, but other\n"
"methods can. A tuple of the field names is available on the class via the\n"
"``__struct_fields__`` attribute if needed.\n"
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
"we define a new `Struct` type for an immutable `Point` object.\n"
"\n"
">>> class Point(Struct, immutable=True):\n"
"...     x: float\n"
"...     y: float\n"
"...\n"
">>> {Point(1.5, 2.0): 1}  # immutable structs are hashable\n"
"{Point(1.5, 2.0): 1}"
);

/*************************************************************************
 * Ext                                                               *
 *************************************************************************/

typedef struct Ext {
    PyObject_HEAD
    char code;
    PyObject *data;
} Ext;

static PyObject *
Ext_New(char code, PyObject *data) {
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
    char code;
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
        long val = PyLong_AsLong(pycode);
        if ((val == -1 && PyErr_Occurred()) || val > 127 || val < -128) {
            PyErr_SetString(
                PyExc_ValueError,
                "code must be an int between -128 and 127"
            );
            return NULL;
        }
        else {
            code = val;
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
    {"code", T_BYTE, offsetof(Ext, code), READONLY, "The extension type code"},
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
    .tp_flags = Py_TPFLAGS_DEFAULT | _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_new = Ext_new,
    .tp_dealloc = (destructor) Ext_dealloc,
    .tp_call = PyVectorcall_Call,
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

    if (status == 0) {
        FAST_BYTEARRAY_SHRINK(state->output_buffer, state->output_len);
    }

    /* Reset buffer */
    state->output_buffer = old_buf;
    state->resize_buffer = &ms_resize_bytes;
    if (old_buf != NULL) {
        state->output_buffer_raw = PyBytes_AS_STRING(old_buf);
    }

    Py_RETURN_NONE;
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

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        MsgspecState *st = msgspec_get_global_state();
        if ((enc_hook = find_keyword(kwnames, args + nargs, st->str_enc_hook)) != NULL) nkwargs--;
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

enum mp_code {
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

static int
mpack_encode_long(EncoderState *self, PyObject *obj)
{
    int overflow;
    int64_t x = PyLong_AsLongLongAndOverflow(obj, &overflow);
    uint64_t ux = x;
    if (overflow != 0) {
        if (overflow > 0) {
            ux = PyLong_AsUnsignedLongLong(obj);
            x = (1ULL << 63) - 1ULL;
            if (ux == ((uint64_t)(-1)) && PyErr_Occurred()) {
                return -1;
            }
        } else {
            PyErr_SetString(PyExc_OverflowError, "can't serialize ints < -2**63");
            return -1;
        }
    }
    else if (x == -1 && PyErr_Occurred()) {
        return -1;
    }

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
    } else if(x < (1<<7)) {
        char buf[1] = {(x & 0xff)};
        return ms_write(self, buf, 1);
    } else {
        if(x < (1<<16)) {
            if(x < (1<<8)) {
                char buf[2] = {MP_UINT8, (x & 0xff)};
                return ms_write(self, buf, 2);
            } else {
                char buf[3];
                buf[0] = MP_UINT16;
                _msgspec_store16(&buf[1], (uint16_t)x);
                return ms_write(self, buf, 3);
            }
        } else {
            if(x < (1LL<<32)) {
                char buf[5];
                buf[0] = MP_UINT32;
                _msgspec_store32(&buf[1], (uint32_t)x);
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

static int
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

static int
mpack_encode_str(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len;
    const char* buf = unicode_str_and_size(obj, &len);
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
            msgspec_get_global_state()->EncodingError,
            "Can't encode strings longer than 2**32 - 1"
        );
        return -1;
    }
    return len > 0 ? ms_write(self, buf, len) : 0;
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
            msgspec_get_global_state()->EncodingError,
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
            msgspec_get_global_state()->EncodingError,
            "Can't encode %s longer than 2**32 - 1",
            typname
        );
        return -1;
    }
    return 0;
}

static int
mpack_encode_list(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = 0;

    len = PyList_GET_SIZE(obj);
    if (mpack_encode_array_header(self, len, "list") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
        if (mpack_encode(self, PyList_GET_ITEM(obj, i)) < 0) {
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
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    while (_PySet_NextEntry(obj, &ppos, &item, &hash)) {
        if (mpack_encode(self, item) < 0) {
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
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
        if (mpack_encode(self, PyTuple_GET_ITEM(obj, i)) < 0) {
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
            msgspec_get_global_state()->EncodingError,
            "Can't encode %s longer than 2**32 - 1",
            typname
        );
        return -1;
    }
    return 0;
}

static int
mpack_encode_dict(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val;
    Py_ssize_t len, pos = 0;
    int status = 0;

    len = PyDict_GET_SIZE(obj);
    if (mpack_encode_map_header(self, len, "dicts") < 0)
        return -1;
    if (len == 0)
        return 0;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    while (PyDict_Next(obj, &pos, &key, &val)) {
        if (mpack_encode(self, key) < 0 || mpack_encode(self, val) < 0) {
            status = -1;
            break;
        }
    }
    Py_LeaveRecursiveCall();
    return status;
}

static int
mpack_encode_struct(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val, *fields;
    Py_ssize_t i, len;
    int status = -1;
    bool asarray = ((StructMetaObject *)Py_TYPE(obj))->asarray == OPT_TRUE;

    fields = StructMeta_GET_FIELDS(Py_TYPE(obj));
    len = PyTuple_GET_SIZE(fields);

    if (asarray) {
        if (mpack_encode_array_header(self, len, "structs") < 0) return -1;
    }
    else {
        if (mpack_encode_map_header(self, len, "structs") < 0) return -1;
    }
    if (len == 0) return 0;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    if (asarray) {
        for (i = 0; i < len; i++) {
            val = Struct_get_index(obj, i);
            if (val == NULL || mpack_encode(self, val) < 0) {
                goto cleanup;
            }
        }
    }
    else {
        for (i = 0; i < len; i++) {
            key = PyTuple_GET_ITEM(fields, i);
            val = Struct_get_index(obj, i);
            if (val == NULL || mpack_encode_str(self, key) < 0 || mpack_encode(self, val) < 0) {
                goto cleanup;
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
            msgspec_get_global_state()->EncodingError,
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

    int status;
    PyObject *name = NULL;
    MsgspecState *st = msgspec_get_global_state();
    /* Try the private variable first for speed, fall back to the public
     * interface if not available */
    name = PyObject_GetAttr(obj, st->str__name_);
    if (name == NULL) {
        PyErr_Clear();
        name = PyObject_GetAttr(obj, st->str_name);
        if (name == NULL)
            return -1;
    }
    if (PyUnicode_CheckExact(name)) {
        status = mpack_encode_str(self, name);
    } else {
        PyErr_SetString(
            msgspec_get_global_state()->EncodingError,
            "Enum's with non-str names aren't supported"
        );
        status = -1;
    }
    Py_DECREF(name);
    return status;
}

static int
mpack_encode_datetime(EncoderState *self, PyObject *obj)
{
    int64_t seconds;
    int32_t nanoseconds;
    PyObject *timestamp;
    MsgspecState *st = msgspec_get_global_state();

    timestamp = CALL_ONE_ARG(st->timestamp, obj);
    if (timestamp == NULL) return -1;

    /* No need to check for overflow here, datetime.datetime can't represent
     * dates out of an int64_t timestamp range anyway */
    seconds = (int64_t)floor(PyFloat_AS_DOUBLE(timestamp));
    nanoseconds = PyDateTime_DATE_GET_MICROSECOND(obj) * 1000;

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

static int
mpack_encode(EncoderState *self, PyObject *obj)
{
    PyTypeObject *type;
    MsgspecState *st;

    type = Py_TYPE(obj);

    if (obj == Py_None) {
        return mpack_encode_none(self);
    }
    else if (obj == Py_False || obj == Py_True) {
        return mpack_encode_bool(self, obj);
    }
    else if (type == &PyLong_Type) {
        return mpack_encode_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return mpack_encode_float(self, obj);
    }
    else if (type == &PyUnicode_Type) {
        return mpack_encode_str(self, obj);
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
    else if (type == &PyList_Type) {
        return mpack_encode_list(self, obj);
    }
    else if (type == &PySet_Type) {
        return mpack_encode_set(self, obj);
    }
    else if (type == &PyTuple_Type) {
        return mpack_encode_tuple(self, obj);
    }
    else if (type == &PyDict_Type) {
        return mpack_encode_dict(self, obj);
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        return mpack_encode_struct(self, obj);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return mpack_encode_datetime(self, obj);
    }
    else if (type == &Ext_Type) {
        return mpack_encode_ext(self, obj);
    }
    st = msgspec_get_global_state();
    if (PyType_IsSubtype(type, st->EnumType)) {
        return mpack_encode_enum(self, obj);
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
    else {
        PyErr_Format(PyExc_TypeError,
                     "Encoding objects of type %.200s is unsupported",
                     type->tp_name);
        return -1;
    }
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

static int
json_encode_long(EncoderState *self, PyObject *obj) {
    int neg, overflow;
    char buf[20];
    char *p = &buf[20];
    int64_t xsigned = PyLong_AsLongLongAndOverflow(obj, &overflow);
    uint64_t x;
    if (overflow) {
        PyErr_SetString(PyExc_OverflowError, "can't serialize ints larger than 64 bits");
        return -1;
    }
    else if (xsigned == -1 && PyErr_Occurred()) {
        return -1;
    }
    neg = xsigned < 0;
    x = neg ? -xsigned : xsigned;
    while (x >= 100) {
        int64_t const old = x;
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
json_encode_float(EncoderState *self, PyObject *obj) {
    char buf[24];
    double x = PyFloat_AS_DOUBLE(obj);
    int n = format_double(x, buf);
    return ms_write(self, buf, n);
}

static inline int
json_encode_str_nocheck(EncoderState *self, PyObject *obj) {
    Py_ssize_t len;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) return -1;
    if (ms_ensure_space(self, len + 2) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p++ = '"';
    memcpy(p, buf, len);
    *(p + len) = '"';
    self->output_len += len + 2;
    return 0;
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

/* GCC generates better code if the uncommon path in the str encoding loop is
 * pulled out into a separate function. Clang generates the same code either
 * way. */
static int
json_write_str_fragment(
    EncoderState *self, const char *buf, Py_ssize_t start, Py_ssize_t i, char c, char escape
) {
    if (MS_LIKELY(start < i)) {
        if (MS_UNLIKELY(ms_write(self, buf + start, i - start) < 0)) return -1;
    }

    /* Write the escaped character */
    char escaped[6] = {'\\', escape, '0', '0'};
    if (MS_UNLIKELY(escape == 'u')) {
        static const char* const hex = "0123456789abcdef";
        escaped[4] = hex[c >> 4];
        escaped[5] = hex[c & 0xF];
        if (MS_UNLIKELY(ms_write(self, escaped, 6) < 0)) return -1;
    }
    else {
        if (MS_UNLIKELY(ms_write(self, escaped, 2) < 0)) return -1;
    }
    return i + 1;
}

static int
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

static const char base64_encode_table[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
json_encode_bin(EncoderState *self, const char* buf, Py_ssize_t len) {
    char *out;
    int nbits = 0, charbuf = 0;
    Py_ssize_t encoded_len;

    if (len >= (1LL << 32)) {
        PyErr_SetString(
            msgspec_get_global_state()->EncodingError,
            "Can't encode bytes-like objects longer than 2**32 - 1"
        );
        return -1;
    }

    /* Preallocate the buffer (ceil(4/3 * len) + 2) */
    encoded_len = 4 * ((len + 2) / 3) + 2;
    if (ms_ensure_space(self, encoded_len) < 0) return -1;

    /* Write to the buffer directly */
    out = self->output_buffer_raw + self->output_len;

    *out++ = '"';
    for (; len > 0; len--, buf++) {
        charbuf = (charbuf << 8) | *buf;
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
    *out++ = '"';

    self->output_len += encoded_len;
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
json_encode_enum(EncoderState *self, PyObject *obj)
{
    if (PyLong_Check(obj))
        return json_encode_long(self, obj);

    int status;
    PyObject *name = NULL;
    MsgspecState *st = msgspec_get_global_state();
    /* Try the private variable first for speed, fall back to the public
     * interface if not available */
    name = PyObject_GetAttr(obj, st->str__name_);
    if (name == NULL) {
        PyErr_Clear();
        name = PyObject_GetAttr(obj, st->str_name);
        if (name == NULL)
            return -1;
    }
    if (PyUnicode_CheckExact(name)) {
        status = json_encode_str(self, name);
    } else {
        PyErr_SetString(
            msgspec_get_global_state()->EncodingError,
            "Enum's with non-str names aren't supported"
        );
        status = -1;
    }
    Py_DECREF(name);
    return status;
}

static int
json_encode_list(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = -1;

    len = PyList_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
        if (json_encode(self, PyList_GET_ITEM(obj, i)) < 0) goto cleanup;
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
json_encode_set(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len, ppos = 0;
    Py_hash_t hash;
    PyObject *item;
    int status = -1;

    len = PySet_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    while (_PySet_NextEntry(obj, &ppos, &item, &hash)) {
        if (json_encode(self, item) < 0) goto cleanup;
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
json_encode_tuple(EncoderState *self, PyObject *obj)
{
    Py_ssize_t i, len;
    int status = -1;

    len = PyTuple_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
        if (json_encode(self, PyTuple_GET_ITEM(obj, i)) < 0) goto cleanup;
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
json_encode_dict(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val;
    Py_ssize_t len, pos = 0;
    int status = -1;

    len = PyDict_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "{}", 2);
    if (ms_write(self, "{", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    while (PyDict_Next(obj, &pos, &key, &val)) {
        if (!PyUnicode_CheckExact(key)) {
            PyErr_SetString(PyExc_TypeError, "dict keys must be strings");
            goto cleanup;
        }
        if (json_encode_str(self, key) < 0) goto cleanup;
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
json_encode_struct(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val, *fields;
    Py_ssize_t i, len;
    int status = -1;

    fields = StructMeta_GET_FIELDS(Py_TYPE(obj));
    len = PyTuple_GET_SIZE(fields);

    if (len == 0) return ms_write(self, "{}", 2);
    if (ms_write(self, "{", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object"))
        return -1;
    for (i = 0; i < len; i++) {
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
json_encode(EncoderState *self, PyObject *obj)
{
    MsgspecState *st;
    PyTypeObject *type = Py_TYPE(obj);

    if (obj == Py_None) {
        return json_encode_none(self);
    }
    else if (obj == Py_True) {
        return json_encode_true(self);
    }
    else if (obj == Py_False) {
        return json_encode_false(self);
    }
    else if (type == &PyLong_Type) {
        return json_encode_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return json_encode_float(self, obj);
    }
    else if (type == &PyUnicode_Type) {
        return json_encode_str(self, obj);
    }
    else if (type == &PyList_Type) {
        return json_encode_list(self, obj);
    }
    else if (type == &PyTuple_Type) {
        return json_encode_tuple(self, obj);
    }
    else if (type == &PySet_Type) {
        return json_encode_set(self, obj);
    }
    else if (type == &PyDict_Type) {
        return json_encode_dict(self, obj);
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        return json_encode_struct(self, obj);
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
    st = msgspec_get_global_state();
    if (PyType_IsSubtype(type, st->EnumType)) {
        return json_encode_enum(self, obj);
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
    else {
        PyErr_Format(PyExc_TypeError,
                     "Encoding objects of type %.200s is unsupported",
                     type->tp_name);
        return -1;
    }
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
    PyObject *tzinfo;

    /* Per-message attributes */
    PyObject *buffer_obj;
    char *input_buffer;
    Py_ssize_t input_len;
    Py_ssize_t next_read_idx;
} DecoderState;

typedef struct Decoder {
    PyObject_HEAD
    PyObject *orig_type;
    DecoderState state;
} Decoder;

PyDoc_STRVAR(Decoder__doc__,
"Decoder(type='Any', *, dec_hook=None, ext_hook=None, tzinfo=None)\n"
"--\n"
"\n"
"A MessagePack decoder.\n"
"\n"
"Parameters\n"
"----------\n"
"type : Type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default MessagePack types.\n"
"dec_hook : Callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic MessagePack types. This hook should transform ``obj`` into\n"
"    type ``type``, or raise a ``TypeError`` if unsupported.\n"
"ext_hook : Callable, optional\n"
"    An optional callback for decoding MessagePack extensions. Should have the\n"
"    signature ``ext_hook(code: int, data: memoryview) -> Any``. If provided,\n"
"    this will be called to deserialize all extension types found in the\n"
"    message. Note that ``data`` is a memoryview into the larger message\n"
"    buffer - any references created to the underlying buffer without copying\n"
"    the data out will cause the full message buffer to persist in memory.\n"
"    If not provided, extension types will decode as ``msgspec.Ext`` objects.\n"
"tzinfo : datetime.tzinfo, optional\n"
"    The timezone to use when decoding ``datetime.datetime`` objects. Defaults\n"
"    to ``None`` for \"naive\" datetimes."
);
static int
Decoder_init(Decoder *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"type", "dec_hook", "ext_hook", "tzinfo", NULL};
    MsgspecState *st = msgspec_get_global_state();
    PyObject *type = st->typing_any;
    PyObject *ext_hook = NULL;
    PyObject *dec_hook = NULL;
    PyObject *tzinfo = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|O$OOO", kwlist, &type, &dec_hook, &ext_hook, &tzinfo
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
    self->state.type = TypeNode_Convert(type);
    if (self->state.type == NULL) {
        return -1;
    }
    Py_INCREF(type);
    self->orig_type = type;

    /* Handle tzinfo */
    if (tzinfo == Py_None) {
        tzinfo = NULL;
    }
    if (tzinfo != NULL) {
        int ok = PyObject_IsInstance(tzinfo, (PyObject *)(PyDateTimeAPI->TZInfoType));
        if (ok == -1) return -1;
        if (ok == 0) {
            PyErr_SetString(PyExc_TypeError, "tzinfo must be an instance of tzinfo");
            return -1;
        }
        Py_INCREF(tzinfo);
    }
    self->state.tzinfo = tzinfo;
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
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");
    }
    typstr = TypeNode_Repr(self->state.type);
    if (typstr != NULL) {
        out = PyUnicode_FromFormat("Decoder(%S)", typstr);
    }
    Py_XDECREF(typstr);
    Py_ReprLeave((PyObject *)self);
    return out;
}

static MS_INLINE int
mp_read1(DecoderState *self, char *s)
{
    if (MS_LIKELY(1 <= self->input_len - self->next_read_idx)) {
        *s = *(self->input_buffer + self->next_read_idx);
        self->next_read_idx += 1;
        return 0;
    }
    return ms_err_truncated();
}

static MS_INLINE int
mp_read(DecoderState *self, char **s, Py_ssize_t n)
{
    if (MS_LIKELY(n <= self->input_len - self->next_read_idx)) {
        *s = self->input_buffer + self->next_read_idx;
        self->next_read_idx += n;
        return 0;
    }
    return ms_err_truncated();
}

static MS_INLINE Py_ssize_t
mp_decode_size1(DecoderState *self) {
    char s = 0;
    if (mp_read1(self, &s) < 0) return -1;
    return (Py_ssize_t)((unsigned char)s);
}

static MS_INLINE Py_ssize_t
mp_decode_size2(DecoderState *self) {
    char *s = NULL;
    if (mp_read(self, &s, 2) < 0) return -1;
    return (Py_ssize_t)(_msgspec_load16(uint16_t, s));
}

static MS_INLINE Py_ssize_t
mp_decode_size4(DecoderState *self) {
    char *s = NULL;
    if (mp_read(self, &s, 4) < 0) return -1;
    return (Py_ssize_t)(_msgspec_load32(uint32_t, s));
}

#define DATETIME_SET_MICROSECOND(o, v) \
    do { \
        ((PyDateTime_DateTime *)o)->data[7] = ((v) & 0xff0000) >> 16; \
        ((PyDateTime_DateTime *)o)->data[8] = ((v) & 0x00ff00) >> 8; \
        ((PyDateTime_DateTime *)o)->data[9] = ((v) & 0x0000ff); \
    } while (0);

static PyObject *
mp_decode_datetime(DecoderState *self, const char *data_buf, Py_ssize_t size) {
    uint64_t data64;
    uint32_t nanoseconds;
    int64_t seconds;
    PyObject *timestamp = NULL, *args = NULL, *res = NULL;

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
            PyErr_SetString(PyExc_ValueError, "Invalid MessagePack timestamp");
            return NULL;
    }

    if (nanoseconds > 999999999) {
        PyErr_SetString(
            PyExc_ValueError,
            "Invalid MessagePack timestamp: nanoseconds out of range"
        );
    }

    timestamp = PyLong_FromLongLong(seconds);
    if (timestamp == NULL) goto cleanup;
    if (self->tzinfo == NULL) {
        args = PyTuple_Pack(1, timestamp);
    } else {
        args = PyTuple_Pack(2, timestamp, self->tzinfo);
    }
    if (args == NULL) goto cleanup;
    res = PyDateTime_FromTimestamp(args);
    if (res == NULL) goto cleanup;

    /* Set the microseconds directly, rather than passing a float to
     * `PyDateTime_FromTimestamp`. This avoids resolution issues on larger
     * timestamps, ensuring we successfully roundtrip all values.
     */
    DATETIME_SET_MICROSECOND(res, nanoseconds / 1000);

cleanup:
    Py_XDECREF(timestamp);
    Py_XDECREF(args);
    return res;
}

static int mp_skip(DecoderState *self);

static int
mp_skip_array(DecoderState *self, Py_ssize_t size) {
    int status = -1;
    Py_ssize_t i;
    if (size < 0) return -1;
    if (size == 0) return 0;

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    for (i = 0; i < size; i++) {
        if (mp_skip(self) < 0) break;
    }
    status = 0;
    Py_LeaveRecursiveCall();
    return status;
}

static int
mp_skip_map(DecoderState *self, Py_ssize_t size) {
    return mp_skip_array(self, size * 2);
}

static int
mp_skip_ext(DecoderState *self, Py_ssize_t size) {
    char *s;
    if (size < 0) return -1;
    return mp_read(self, &s, size + 1);
}

static int
mp_skip(DecoderState *self) {
    char *s = NULL;
    char op = 0;
    Py_ssize_t size;

    if (mp_read1(self, &op) < 0) return -1;

    if (-32 <= op && op <= 127) {
        return 0;
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        return mp_read(self, &s, op & 0x1f);
    }
    else if ('\x90' <= op && op <= '\x9f') {
        return mp_skip_array(self, op & 0x0f);
    }
    else if ('\x80' <= op && op <= '\x8f') {
        return mp_skip_map(self, op & 0x0f);
    }
    switch ((enum mp_code)op) {
        case MP_NIL:
        case MP_TRUE:
        case MP_FALSE:
            return 0;
        case MP_UINT8:
        case MP_INT8:
            return mp_read1(self, &op);
        case MP_UINT16:
        case MP_INT16:
            return mp_read(self, &s, 2);
        case MP_UINT32:
        case MP_INT32:
        case MP_FLOAT32:
            return mp_read(self, &s, 4);
        case MP_UINT64:
        case MP_INT64:
        case MP_FLOAT64:
            return mp_read(self, &s, 8);
        case MP_STR8:
        case MP_BIN8:
            if ((size = mp_decode_size1(self)) < 0) return -1;
            return mp_read(self, &s, size);
        case MP_STR16:
        case MP_BIN16:
            if ((size = mp_decode_size2(self)) < 0) return -1;
            return mp_read(self, &s, size);
        case MP_STR32:
        case MP_BIN32:
            if ((size = mp_decode_size4(self)) < 0) return -1;
            return mp_read(self, &s, size);
        case MP_ARRAY16:
            return mp_skip_array(self, mp_decode_size2(self));
        case MP_ARRAY32:
            return mp_skip_array(self, mp_decode_size4(self));
        case MP_MAP16:
            return mp_skip_map(self, mp_decode_size2(self));
        case MP_MAP32:
            return mp_skip_map(self, mp_decode_size4(self));
        case MP_FIXEXT1:
            return mp_skip_ext(self, 1);
        case MP_FIXEXT2:
            return mp_skip_ext(self, 2);
        case MP_FIXEXT4:
            return mp_skip_ext(self, 4);
        case MP_FIXEXT8:
            return mp_skip_ext(self, 8);
        case MP_FIXEXT16:
            return mp_skip_ext(self, 16);
        case MP_EXT8:
            return mp_skip_ext(self, mp_decode_size1(self));
        case MP_EXT16:
            return mp_skip_ext(self, mp_decode_size2(self));
        case MP_EXT32:
            return mp_skip_ext(self, mp_decode_size4(self));
        default:
            PyErr_Format(msgspec_get_global_state()->DecodingError, "invalid opcode, '\\x%02x'.", op);
            return -1;
    }
}

static PyObject * mp_decode_type(
    DecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
);

static PyObject *
mp_format_validation_error(const char *expected, const char *got, TypeNode *ctx, Py_ssize_t ctx_ind) {
    MsgspecState *st = msgspec_get_global_state();
    if (ctx->types & MS_TYPE_STRUCT && ctx_ind != -1) {
        StructMetaObject *st_type = TypeNode_get_struct(ctx);
        PyObject *field = PyTuple_GET_ITEM(st_type->struct_fields, ctx_ind);
        PyObject *typstr = TypeNode_Repr(st_type->struct_types[ctx_ind]);
        if (typstr == NULL) return NULL;
        PyErr_Format(
            st->DecodingError,
            "Error decoding `%s` field `%S` (`%S`): expected `%s`, got `%s`",
            ((PyTypeObject *)st_type)->tp_name,
            field,
            typstr,
            expected,
            got
        );
        Py_DECREF(typstr);
    }
    else {
        PyObject *typstr = TypeNode_Repr(ctx);
        if (typstr == NULL) return NULL;
        PyErr_Format(
            st->DecodingError,
            "Error decoding `%S`: expected `%s`, got `%s`",
            typstr,
            expected,
            got
        );
        Py_DECREF(typstr);
    }
    return NULL;
}

static MS_NOINLINE PyObject *
mp_validation_error(char *got, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    PyObject *out = NULL;
    PyObject *repr = TypeNode_Repr(type);
    if (repr == NULL) return NULL;
    const char *expected = PyUnicode_AsUTF8(repr);
    if (expected == NULL) goto cleanup;
    out = mp_format_validation_error(expected, got, ctx, ctx_ind);
cleanup:
    Py_DECREF(repr);
    return out;
}

static PyObject *
mp_decode_type_intenum(DecoderState *self, PyObject *val, TypeNode *type) {
    if (val == NULL) return NULL;

    PyObject *out = NULL;
    MsgspecState *st = msgspec_get_global_state();
    PyObject *intenum = TypeNode_get_intenum(type);

    /* Fast path for common case. This accesses a non-public member of the
    * enum class to speedup lookups. If this fails, we clear errors and
    * use the slower-but-more-public method instead. */
    PyObject *member_table = PyObject_GetAttr(intenum, st->str__value2member_map_);
    if (MS_LIKELY(member_table != NULL)) {
        out = PyDict_GetItem(member_table, val);
        Py_DECREF(member_table);
        Py_XINCREF(out);
    }
    if (MS_UNLIKELY(out == NULL)) {
        PyErr_Clear();
        out = CALL_ONE_ARG(intenum, val);
    }
    Py_DECREF(val);
    if (MS_UNLIKELY(out == NULL)) {
        PyErr_Clear();
        PyErr_Format(
            st->DecodingError,
            "Error decoding enum `%s`: invalid value `%S`",
            ((PyTypeObject *)(intenum))->tp_name,
            val
        );
    }
    return out;
}

static MS_INLINE PyObject *
mp_decode_type_int(DecoderState *self, int64_t x, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    if (type->types & MS_TYPE_INTENUM) {
        return mp_decode_type_intenum(self, PyLong_FromLongLong(x), type);
    }
    else if (type->types & (MS_TYPE_ANY | MS_TYPE_INT)) {
        return PyLong_FromLongLong(x);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return PyFloat_FromDouble(x);
    }
    return mp_validation_error("int", type, ctx, ctx_ind);
}

static MS_INLINE PyObject *
mp_decode_type_uint(DecoderState *self, uint64_t x, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    if (type->types & MS_TYPE_INTENUM) {
        return mp_decode_type_intenum(self, PyLong_FromUnsignedLongLong(x), type);
    }
    else if (type->types & (MS_TYPE_ANY | MS_TYPE_INT)) {
        return PyLong_FromUnsignedLongLong(x);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return PyFloat_FromDouble(x);
    }
    return mp_validation_error("int", type, ctx, ctx_ind);
}

static MS_INLINE PyObject *
mp_decode_type_none(DecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_NONE)) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    return mp_validation_error("None", type, ctx, ctx_ind);
}

static MS_INLINE PyObject *
mp_decode_type_bool(DecoderState *self, PyObject *val, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(val);
        return val;
    }
    return mp_validation_error("bool", type, ctx, ctx_ind);
}

static PyObject *
mp_decode_type_float(DecoderState *self, double val, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)) {
        return PyFloat_FromDouble(val);
    }
    return mp_validation_error("float", type, ctx, ctx_ind);
}

static MS_NOINLINE PyObject *
mp_decode_type_enum(PyObject *val, TypeNode *type) {
    if (val == NULL) return NULL;
    PyObject *enum_obj = TypeNode_get_enum(type);
    PyObject *out = PyObject_GetAttr(enum_obj, val);
    Py_DECREF(val);
    if (MS_UNLIKELY(out == NULL)) {
        PyErr_Clear();
        PyErr_Format(
            msgspec_get_global_state()->DecodingError,
            "Error decoding enum `%s`: invalid name `%S`",
            ((PyTypeObject *)enum_obj)->tp_name,
            val
        );
    }
    return out;
}

static MS_INLINE PyObject *
mp_decode_type_str(DecoderState *self, Py_ssize_t size, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    if (MS_LIKELY(type->types & (MS_TYPE_ANY | MS_TYPE_STR | MS_TYPE_ENUM))) {
        char *s = NULL;
        PyObject *val;
        if (MS_UNLIKELY(mp_read(self, &s, size) < 0)) return NULL;
        val = PyUnicode_DecodeUTF8(s, size, NULL);
        if (MS_UNLIKELY(type->types & MS_TYPE_ENUM)) {
            return mp_decode_type_enum(val, type);
        }
        return val;
    }
    return mp_validation_error("str", type, ctx, ctx_ind);
}

static PyObject *
mp_decode_type_bin(
    DecoderState *self, Py_ssize_t size, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    if (MS_UNLIKELY(size < 0)) return NULL;

    char *s = NULL;
    if (MS_UNLIKELY(mp_read(self, &s, size) < 0)) return NULL;

    if (type->types & (MS_TYPE_ANY | MS_TYPE_BYTES)) {
        return PyBytes_FromStringAndSize(s, size);
    }
    else if (type->types & MS_TYPE_BYTEARRAY) {
        return PyByteArray_FromStringAndSize(s, size);
    }
    return mp_validation_error("bytes", type, ctx, ctx_ind);
}

static PyObject *
mp_decode_type_list(
    DecoderState *self, Py_ssize_t size, TypeNode *el_type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = PyList_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        item = mp_decode_type(self, el_type, ctx, ctx_ind, false);
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
mp_decode_type_set(
    DecoderState *self, Py_ssize_t size, TypeNode *el_type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = PySet_New(NULL);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        item = mp_decode_type(self, el_type, ctx, ctx_ind, true);
        if (MS_UNLIKELY(item == NULL || PySet_Add(res, item) < 0)) {
            Py_CLEAR(res);
            break;
        }
        Py_DECREF(item);
    }
    Py_LeaveRecursiveCall();
    return res;
}

static PyObject *
mp_decode_type_vartuple(
    DecoderState *self, Py_ssize_t size, TypeNode *el_type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    Py_ssize_t i;
    PyObject *res, *item;

    res = PyTuple_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        item = mp_decode_type(self, el_type, ctx, ctx_ind, is_key);
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
mp_decode_type_fixtuple(
    DecoderState *self, Py_ssize_t size, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    Py_ssize_t i, offset;
    PyObject *res, *item;
    TypeNodeExtra *tex = (TypeNodeExtra *)type;

    if (size != tex->fixtuple_size) {
        /* tuple is the incorrect size, raise and return */
        PyObject *typstr;
        MsgspecState *st = msgspec_get_global_state();
        if (ctx->types & MS_TYPE_STRUCT) {
            StructMetaObject *st_type = TypeNode_get_struct(ctx);
            PyObject *field = PyTuple_GET_ITEM(st_type->struct_fields, ctx_ind);
            if ((typstr = TypeNode_Repr(st_type->struct_types[ctx_ind])) == NULL) return NULL;
            PyErr_Format(
                st->DecodingError,
                "Error decoding `%s` field `%S` (`%S`): expected tuple of length %zd, got %zd",
                ((PyTypeObject *)st_type)->tp_name,
                field, typstr, tex->fixtuple_size, size
            );
        }
        else {
            if ((typstr = TypeNode_Repr(ctx)) == NULL) return NULL;
            PyErr_Format(
                st->DecodingError,
                "Error decoding `%S`: expected tuple of length %zd, got %zd",
                typstr, tex->fixtuple_size, size
            );
        }
        Py_DECREF(typstr);
        return NULL;
    }

    res = PyTuple_New(size);
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }

    offset = TypeNode_get_array_offset(type);
    for (i = 0; i < tex->fixtuple_size; i++) {
        item = mp_decode_type(self, tex->extra[offset + i], ctx, ctx_ind, is_key);
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
mp_decode_type_struct_array(
    DecoderState *self, Py_ssize_t size, StructMetaObject *st_type, TypeNode *type, bool is_key
) {
    Py_ssize_t i, nfields, ndefaults, npos;
    PyObject *res, *val = NULL;
    int should_untrack;

    res = Struct_alloc((PyTypeObject *)(st_type));
    if (res == NULL) return NULL;

    nfields = PyTuple_GET_SIZE(st_type->struct_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    npos = nfields - ndefaults;
    should_untrack = PyObject_IS_GC(res);

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < nfields; i++) {
        if (size > 0) {
            val = mp_decode_type(self, st_type->struct_types[i], type, i, is_key);
            if (MS_UNLIKELY(val == NULL)) goto error;
            size--;
        }
        else if (i < npos) {
            PyErr_Format(
                msgspec_get_global_state()->DecodingError,
                "Error decoding `%s`: missing required field `%S`",
                ((PyTypeObject *)st_type)->tp_name,
                PyTuple_GET_ITEM(st_type->struct_fields, i)
            );
            goto error;
        }
        else {
            val = maybe_deepcopy_default(
                PyTuple_GET_ITEM(st_type->struct_defaults, i - npos)
            );
            if (val == NULL)
                goto error;
        }
        Struct_set_index(res, i, val);
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    /* Ignore all trailing fields */
    while (size > 0) {
        if (mp_skip(self) < 0)
            goto error;
        size--;
    }
    Py_LeaveRecursiveCall();
    if (should_untrack)
        PyObject_GC_UnTrack(res);
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(res);
    return NULL;
}


static PyObject *
mp_decode_type_array(
    DecoderState *self, Py_ssize_t size, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    if (type->types & MS_TYPE_ANY) {
        if (is_key) {
            return mp_decode_type_vartuple(self, size, type, ctx, ctx_ind, is_key);
        }
        else {
            return mp_decode_type_list(self, size, type, ctx, ctx_ind);
        }
    }
    else if (type->types & MS_TYPE_LIST) {
        return mp_decode_type_list(self, size, TypeNode_get_array(type), ctx, ctx_ind);
    }
    else if (type->types & MS_TYPE_SET) {
        return mp_decode_type_set(self, size, TypeNode_get_array(type), ctx, ctx_ind);
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return mp_decode_type_vartuple(self, size, TypeNode_get_array(type), ctx, ctx_ind, is_key);
    }
    else if (type->types & MS_TYPE_FIXTUPLE) {
        return mp_decode_type_fixtuple(self, size, type, ctx, ctx_ind, is_key);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        if (struct_type->asarray == OPT_TRUE) {
            return mp_decode_type_struct_array(self, size, struct_type, type, is_key);
        }
    }
    return mp_validation_error("list", type, ctx, ctx_ind);
}

static PyObject *
mp_decode_type_dict(
    DecoderState *self, Py_ssize_t size, TypeNode *key_type, TypeNode *val_type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    Py_ssize_t i;
    PyObject *res, *key = NULL, *val = NULL;

    res = PyDict_New();
    if (res == NULL) return NULL;
    if (size == 0) return res;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        key = mp_decode_type(self, key_type, ctx, ctx_ind, true);
        if (MS_UNLIKELY(key == NULL))
            goto error;
        val = mp_decode_type(self, val_type, ctx, ctx_ind, false);
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
mp_error_expected(char op, char *expected, TypeNode *ctx, Py_ssize_t ctx_ind) {
    char *got;
    if (-32 <= op && op <= 127) {
        got = "int";
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        got = "str";
    }
    else if ('\x90' <= op && op <= '\x9f') {
        got = "list";
    }
    else if ('\x80' <= op && op <= '\x8f') {
        got = "dict";
    }
    else {
        switch ((enum mp_code)op) {
            case MP_NIL:
                got = "None";
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
                got = "list";
                break;
            case MP_MAP16:
            case MP_MAP32:
                got = "dict";
                break;
            case MP_FIXEXT1:
            case MP_FIXEXT2:
            case MP_FIXEXT4:
            case MP_FIXEXT8:
            case MP_FIXEXT16:
            case MP_EXT8:
            case MP_EXT16:
            case MP_EXT32:
                got = "Ext";
                break;
            default:
                got = "unknown";
                break;
        }
    }
    return mp_format_validation_error("str", got, ctx, ctx_ind);
}

static MS_INLINE Py_ssize_t
mp_decode_cstr(DecoderState *self, char ** out, TypeNode *ctx) {
    char op = 0;
    Py_ssize_t size;
    if (mp_read1(self, &op) < 0) return -1;

    if ('\xa0' <= op && op <= '\xbf') {
        size = op & 0x1f;
    }
    else if (op == MP_STR8) {
        size = mp_decode_size1(self);
    }
    else if (op == MP_STR16) {
        size = mp_decode_size2(self);
    }
    else if (op == MP_STR32) {
        size = mp_decode_size4(self);
    }
    else {
        mp_error_expected(op, "str", ctx, -1);
        return -1;
    }

    if (mp_read(self, out, size) < 0) return -1;
    return size;
}

static PyObject *
mp_decode_type_struct_map(
    DecoderState *self, Py_ssize_t size, StructMetaObject *st_type, TypeNode *type, bool is_key
) {
    Py_ssize_t i, key_size, field_index, pos = 0;
    char *key = NULL;
    PyObject *res, *val = NULL;

    res = Struct_alloc((PyTypeObject *)(st_type));
    if (res == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(res);
        return NULL;
    }
    for (i = 0; i < size; i++) {
        key_size = mp_decode_cstr(self, &key, type);
        if (MS_UNLIKELY(key_size < 0)) goto error;

        field_index = StructMeta_get_field_index(st_type, key, key_size, &pos);
        if (field_index < 0) {
            /* Skip unknown fields */
            if (mp_skip(self) < 0) goto error;
        }
        else {
            val = mp_decode_type(
                self, st_type->struct_types[field_index], type, field_index, is_key
            );
            if (val == NULL) goto error;
            Struct_set_index(res, field_index, val);
        }
    }

    if (Struct_fill_in_defaults(st_type, res) < 0) goto error;
    Py_LeaveRecursiveCall();
    return res;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(res);
    return NULL;
}

static PyObject *
mp_decode_type_map(
    DecoderState *self, Py_ssize_t size, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    if (type->types & MS_TYPE_ANY) {
        return mp_decode_type_dict(self, size, type, type, ctx, ctx_ind);
    }
    else if (type->types & MS_TYPE_DICT) {
        TypeNode *key, *val;
        TypeNode_get_dict(type, &key, &val);
        return mp_decode_type_dict(self, size, key, val, ctx, ctx_ind);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        if (struct_type->asarray != OPT_TRUE) {
            return mp_decode_type_struct_map(self, size, struct_type, type, is_key);
        }
    }
    return mp_validation_error("dict", type, ctx, ctx_ind);
}

static PyObject *
mp_decode_type_ext(DecoderState *self, Py_ssize_t size, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    Py_buffer *buffer;
    char code = 0, *data_buf = NULL;
    PyObject *data, *pycode = NULL, *view = NULL, *out = NULL;

    if (size < 0) return NULL;
    if (mp_read1(self, &code) < 0) return NULL;
    if (mp_read(self, &data_buf, size) < 0) return NULL;

    if (type->types & MS_TYPE_DATETIME && code == -1) {
        return mp_decode_datetime(self, data_buf, size);
    }
    else if (type->types & MS_TYPE_EXT) {
        data = PyBytes_FromStringAndSize(data_buf, size);
        if (data == NULL) return NULL;
        return Ext_New(code, data);
    }
    else if (!(type->types & MS_TYPE_ANY)) {
        return mp_validation_error("Ext", type, ctx, ctx_ind);
    }

    /* Decode Any.
     * - datetime if code == -1
     * - call ext_hook if available
     * - otherwise return Ext object
     * */
    if (code == -1) {
        return mp_decode_datetime(self, data_buf, size);
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

static PyObject *
mp_decode_type_custom(DecoderState *self, bool generic, TypeNode* type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    PyObject *obj, *custom_cls = NULL, *out = NULL;
    int status;
    PyObject *custom_obj = TypeNode_get_custom(type);
    TypeNode type_any = {MS_TYPE_ANY};

    if ((obj = mp_decode_type(self, &type_any, ctx, ctx_ind, false)) == NULL)
        return NULL;

    if (self->dec_hook != NULL) {
        out = PyObject_CallFunctionObjArgs(self->dec_hook, custom_obj, obj, NULL);
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
        mp_format_validation_error(
            ((PyTypeObject *)custom_cls)->tp_name,
            Py_TYPE(out)->tp_name,
            ctx,
            ctx_ind
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

static PyObject *
mp_decode_type(
    DecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    char op = 0;
    char *s = NULL;

    if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
        return mp_decode_type_custom(
            self, type->types & MS_TYPE_CUSTOM_GENERIC, type, ctx, ctx_ind
        );
    }

    if (mp_read1(self, &op) < 0) {
        return NULL;
    }

    if (-32 <= op && op <= 127) {
        return mp_decode_type_int(self, op, type, ctx, ctx_ind);
    }
    else if ('\xa0' <= op && op <= '\xbf') {
        return mp_decode_type_str(self, op & 0x1f, type, ctx, ctx_ind);
    }
    else if ('\x90' <= op && op <= '\x9f') {
        return mp_decode_type_array(self, op & 0x0f, type, ctx, ctx_ind, is_key);
    }
    else if ('\x80' <= op && op <= '\x8f') {
        return mp_decode_type_map(self, op & 0x0f, type, ctx, ctx_ind, is_key);
    }
    switch ((enum mp_code)op) {
        case MP_NIL:
            return mp_decode_type_none(self, type, ctx, ctx_ind);
        case MP_TRUE:
            return mp_decode_type_bool(self, Py_True, type, ctx, ctx_ind);
        case MP_FALSE:
            return mp_decode_type_bool(self, Py_False, type, ctx, ctx_ind);
        case MP_UINT8:
            if (MS_UNLIKELY(mp_read(self, &s, 1) < 0)) return NULL;
            return mp_decode_type_uint(self, *(uint8_t *)s, type, ctx, ctx_ind);
        case MP_UINT16:
            if (MS_UNLIKELY(mp_read(self, &s, 2) < 0)) return NULL;
            return mp_decode_type_uint(self, _msgspec_load16(uint16_t, s), type, ctx, ctx_ind);
        case MP_UINT32:
            if (MS_UNLIKELY(mp_read(self, &s, 4) < 0)) return NULL;
            return mp_decode_type_uint(self, _msgspec_load32(uint32_t, s), type, ctx, ctx_ind);
        case MP_UINT64:
            if (MS_UNLIKELY(mp_read(self, &s, 8) < 0)) return NULL;
            return mp_decode_type_uint(self, _msgspec_load64(uint64_t, s), type, ctx, ctx_ind);
        case MP_INT8:
            if (MS_UNLIKELY(mp_read(self, &s, 1) < 0)) return NULL;
            return mp_decode_type_int(self, *(int8_t *)s, type, ctx, ctx_ind);
        case MP_INT16:
            if (MS_UNLIKELY(mp_read(self, &s, 2) < 0)) return NULL;
            return mp_decode_type_int(self, _msgspec_load16(int16_t, s), type, ctx, ctx_ind);
        case MP_INT32:
            if (MS_UNLIKELY(mp_read(self, &s, 4) < 0)) return NULL;
            return mp_decode_type_int(self, _msgspec_load32(int32_t, s), type, ctx, ctx_ind);
        case MP_INT64:
            if (MS_UNLIKELY(mp_read(self, &s, 8) < 0)) return NULL;
            return mp_decode_type_int(self, _msgspec_load64(int64_t, s), type, ctx, ctx_ind);
        case MP_FLOAT32: {
            float f = 0;
            uint32_t uf;
            if (mp_read(self, &s, 4) < 0) return NULL;
            uf = _msgspec_load32(uint32_t, s);
            memcpy(&f, &uf, 4);
            return mp_decode_type_float(self, f, type, ctx, ctx_ind);
        }
        case MP_FLOAT64: {
            double f = 0;
            uint64_t uf;
            if (mp_read(self, &s, 8) < 0) return NULL;
            uf = _msgspec_load64(uint64_t, s);
            memcpy(&f, &uf, 8);
            return mp_decode_type_float(self, f, type, ctx, ctx_ind);
        }
        case MP_STR8: {
            Py_ssize_t size = mp_decode_size1(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mp_decode_type_str(self, size, type, ctx, ctx_ind);
        }
        case MP_STR16: {
            Py_ssize_t size = mp_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mp_decode_type_str(self, size, type, ctx, ctx_ind);
        }
        case MP_STR32: {
            Py_ssize_t size = mp_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mp_decode_type_str(self, size, type, ctx, ctx_ind);
        }
        case MP_BIN8:
            return mp_decode_type_bin(self, mp_decode_size1(self), type, ctx, ctx_ind);
        case MP_BIN16:
            return mp_decode_type_bin(self, mp_decode_size2(self), type, ctx, ctx_ind);
        case MP_BIN32:
            return mp_decode_type_bin(self, mp_decode_size4(self), type, ctx, ctx_ind);
        case MP_ARRAY16: {
            Py_ssize_t size = mp_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mp_decode_type_array(self, size, type, ctx, ctx_ind, is_key);
        }
        case MP_ARRAY32: {
            Py_ssize_t size = mp_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mp_decode_type_array(self, size, type, ctx, ctx_ind, is_key);
        }
        case MP_MAP16: {
            Py_ssize_t size = mp_decode_size2(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mp_decode_type_map(self, size, type, ctx, ctx_ind, is_key);
        }
        case MP_MAP32: {
            Py_ssize_t size = mp_decode_size4(self);
            if (MS_UNLIKELY(size < 0)) return NULL;
            return mp_decode_type_map(self, size, type, ctx, ctx_ind, is_key);
        }
        case MP_FIXEXT1:
            return mp_decode_type_ext(self, 1, type, ctx, ctx_ind);
        case MP_FIXEXT2:
            return mp_decode_type_ext(self, 2, type, ctx, ctx_ind);
        case MP_FIXEXT4:
            return mp_decode_type_ext(self, 4, type, ctx, ctx_ind);
        case MP_FIXEXT8:
            return mp_decode_type_ext(self, 8, type, ctx, ctx_ind);
        case MP_FIXEXT16:
            return mp_decode_type_ext(self, 16, type, ctx, ctx_ind);
        case MP_EXT8:
            return mp_decode_type_ext(self, mp_decode_size1(self), type, ctx, ctx_ind);
        case MP_EXT16:
            return mp_decode_type_ext(self, mp_decode_size2(self), type, ctx, ctx_ind);
        case MP_EXT32:
            return mp_decode_type_ext(self, mp_decode_size4(self), type, ctx, ctx_ind);
        default:
            PyErr_Format(msgspec_get_global_state()->DecodingError, "invalid opcode, '\\x%02x'.", op);
            return NULL;
    }
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
"    The deserialized object\n"
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
        self->state.input_buffer = buffer.buf;
        self->state.input_len = buffer.len;
        self->state.next_read_idx = 0;
        res = mp_decode_type(&(self->state), self->state.type, self->state.type, -1, false);
    }

    if (buffer.buf != NULL) {
        PyBuffer_Release(&buffer);
        self->state.buffer_obj = NULL;
        self->state.input_buffer = NULL;
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
    {"tzinfo", T_OBJECT, offsetof(Decoder, state.tzinfo), READONLY, "The Decoder tzinfo"},
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
"decode(buf, *, type='Any', dec_hook=None, ext_hook=None, tzinfo=None)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like\n"
"    The message to decode.\n"
"type : Type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default MessagePack types.\n"
"dec_hook : Callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic MessagePack types. This hook should transform ``obj`` into\n"
"    type ``type``, or raise a ``TypeError`` if unsupported.\n"
"ext_hook : Callable, optional\n"
"    An optional callback for decoding MessagePack extensions. Should have the\n"
"    signature ``ext_hook(code: int, data: memoryview) -> Any``. If provided,\n"
"    this will be called to deserialize all extension types found in the\n"
"    message. Note that ``data`` is a memoryview into the larger message\n"
"    buffer - any references created to the underlying buffer without copying\n"
"    the data out will cause the full message buffer to persist in memory.\n"
"    If not provided, extension types will decode as ``msgspec.Ext`` objects.\n"
"tzinfo : datetime.tzinfo, optional\n"
"    The timezone to use when decoding ``datetime.datetime`` objects. Defaults\n"
"    to ``None`` for \"naive\" datetimes.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object\n"
"\n"
"See Also\n"
"--------\n"
"Decoder.decode"
);
static PyObject*
msgspec_msgpack_decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *res = NULL, *buf = NULL, *type = NULL, *dec_hook = NULL, *ext_hook = NULL, *tzinfo = NULL;
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
        if ((tzinfo = find_keyword(kwnames, args + nargs, st->str_tzinfo)) != NULL) nkwargs--;
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

    /* Handle tzinfo */
    if (tzinfo == Py_None) {
        tzinfo = NULL;
    }
    if (tzinfo != NULL) {
        int ok = PyObject_IsInstance(tzinfo, (PyObject *)(PyDateTimeAPI->TZInfoType));
        if (ok == -1) return NULL;
        if (ok == 0) {
            PyErr_SetString(PyExc_TypeError, "tzinfo must be an instance of tzinfo");
            return NULL;
        }
        Py_INCREF(tzinfo);
    }
    state.tzinfo = tzinfo;

    /* Only build TypeNode if required */
    state.type = NULL;
    if (type != NULL && type != st->typing_any) {
        state.type = TypeNode_Convert(type);
        if (state.type == NULL) return NULL;
    }

    buffer.buf = NULL;
    if (PyObject_GetBuffer(buf, &buffer, PyBUF_CONTIG_RO) >= 0) {
        state.buffer_obj = buf;
        state.input_buffer = buffer.buf;
        state.input_len = buffer.len;
        state.next_read_idx = 0;
        if (state.type != NULL) {
            res = mp_decode_type(&state, state.type, state.type, -1, false);
        } else {
            TypeNode type_any = {MS_TYPE_ANY};
            res = mp_decode_type(&state, &type_any, &type_any, -1, false);
        }
    }

    if (state.type != NULL) {
        TypeNode_Free(state.type);
    }

    if (buffer.buf != NULL) {
        PyBuffer_Release(&buffer);
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
    PyObject *tzinfo;

    /* Temporary scratch space */
    unsigned char *scratch;
    ssize_t scratch_capacity;
    Py_ssize_t scratch_len;

    /* Per-message attributes */
    PyObject *buffer_obj;
    unsigned char *input_pos;
    unsigned char *input_end;
} JSONDecoderState;

typedef struct JSONDecoder {
    PyObject_HEAD
    PyObject *orig_type;
    JSONDecoderState state;
} JSONDecoder;

PyDoc_STRVAR(JSONDecoder__doc__,
"Decoder(type='Any', *, dec_hook=None, tzinfo=None)\n"
"--\n"
"\n"
"A JSON decoder.\n"
"\n"
"Parameters\n"
"----------\n"
"type : Type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default MessagePack types.\n"
"dec_hook : Callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic MessagePack types. This hook should transform ``obj`` into\n"
"    type ``type``, or raise a ``TypeError`` if unsupported.\n"
"tzinfo : datetime.tzinfo, optional\n"
"    The timezone to use when decoding ``datetime.datetime`` objects. Defaults\n"
"    to ``None`` for \"naive\" datetimes."
);
static int
JSONDecoder_init(JSONDecoder *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"type", "dec_hook", "tzinfo", NULL};
    MsgspecState *st = msgspec_get_global_state();
    PyObject *type = st->typing_any;
    PyObject *dec_hook = NULL;
    PyObject *tzinfo = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|O$OOO", kwlist, &type, &dec_hook, &tzinfo
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

    /* Handle type */
    self->state.type = TypeNode_Convert(type);
    if (self->state.type == NULL) {
        return -1;
    }
    Py_INCREF(type);
    self->orig_type = type;

    /* Handle tzinfo */
    if (tzinfo == Py_None) {
        tzinfo = NULL;
    }
    if (tzinfo != NULL) {
        int ok = PyObject_IsInstance(tzinfo, (PyObject *)(PyDateTimeAPI->TZInfoType));
        if (ok == -1) return -1;
        if (ok == 0) {
            PyErr_SetString(PyExc_TypeError, "tzinfo must be an instance of tzinfo");
            return -1;
        }
        Py_INCREF(tzinfo);
    }
    self->state.tzinfo = tzinfo;

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
    TypeNode_Free(self->state.type);
    Py_XDECREF(self->orig_type);
    Py_XDECREF(self->state.dec_hook);
    Py_TYPE(self)->tp_free((PyObject *)self);
    PyMem_Free(self->state.scratch);
}

static PyObject *
JSONDecoder_repr(JSONDecoder *self) {
    int recursive;
    PyObject *typstr, *out = NULL;

    recursive = Py_ReprEnter((PyObject *)self);
    if (recursive != 0) {
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");
    }
    typstr = TypeNode_Repr(self->state.type);
    if (typstr != NULL) {
        out = PyUnicode_FromFormat("Decoder(%S)", typstr);
    }
    Py_XDECREF(typstr);
    Py_ReprLeave((PyObject *)self);
    return out;
}

static MS_INLINE bool
js_read1(JSONDecoderState *self, unsigned char *c)
{
    if (MS_UNLIKELY(self->input_pos == self->input_end)) {
        ms_err_truncated();
        return false;
    }
    *c = *self->input_pos;
    self->input_pos += 1;
    return true;
}

static MS_INLINE bool
js_peek_skip_ws(JSONDecoderState *self, unsigned char *s)
{
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) {
            ms_err_truncated();
            return false;
        }
        unsigned char c = *self->input_pos;
        if (!(c == ' ' || c == '\n' || c == '\t' || c == '\r')) {
            *s = c;
            return true;
        }
        self->input_pos++;
    }
}

static MS_INLINE bool
js_remaining(JSONDecoderState *self, ptrdiff_t remaining)
{
    return self->input_end - self->input_pos >= remaining;
}

static PyObject *
js_err_invalid(const char *msg)
{
    PyErr_Format(
        msgspec_get_global_state()->DecodingError,
        "JSON is malformed: %s",
        msg
    );
    return NULL;
}

static int
js_skip(JSONDecoderState *self) {
    /* TODO! */
    return -1;
}

static PyObject * js_decode(
    JSONDecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
);

static PyObject *
js_decode_none(
    JSONDecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    if (MS_UNLIKELY(!js_remaining(self, 3))) {
        ms_err_truncated();
        return NULL;
    }
    self->input_pos++;  /* Already checked as 'n' */
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'u' || c2 != 'l' || c3 != 'l')) {
        return js_err_invalid("invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_NONE)) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    return mp_validation_error("None", type, ctx, ctx_ind);
}

static PyObject *
js_decode_true(
    JSONDecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    if (MS_UNLIKELY(!js_remaining(self, 3))) {
        ms_err_truncated();
        return NULL;
    }
    self->input_pos++;  /* Already checked as 't' */
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'r' || c2 != 'u' || c3 != 'e')) {
        return js_err_invalid("invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    return mp_validation_error("bool", type, ctx, ctx_ind);
}

static PyObject *
js_decode_false(
    JSONDecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    if (MS_UNLIKELY(!js_remaining(self, 4))) {
        ms_err_truncated();
        return NULL;
    }
    self->input_pos++;  /* Already checked as 'f' */
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    unsigned char c4 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'a' || c2 != 'l' || c3 != 's' || c4 != 'e')) {
        return js_err_invalid("invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL)) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    return mp_validation_error("bool", type, ctx, ctx_ind);
}

static PyObject *
js_decode_list(
    JSONDecoderState *self, TypeNode *el_type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    PyObject *out, *item = NULL;
    unsigned char c;
    bool first = true;

    self->input_pos++; /* Skip '[' */

    out = PyList_New(0);
    if (out == NULL) return NULL;
    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            js_err_invalid("expected ',' or '}'");
            goto error;
        }

        /* Parse item */
        item = js_decode(self, el_type, ctx, ctx_ind, is_key);
        if (item == NULL) goto error;

        /* Append item to list */
        if (PyList_Append(out, item) < 0) goto error;
        Py_CLEAR(item);
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
js_decode_set(
    JSONDecoderState *self, TypeNode *el_type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    PyObject *out, *item = NULL;
    unsigned char c;
    bool first = true;

    self->input_pos++; /* Skip '[' */

    out = PySet_New(NULL);
    if (out == NULL) return NULL;
    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            js_err_invalid("expected ',' or '}'");
            goto error;
        }

        /* Parse item */
        item = js_decode(self, el_type, ctx, ctx_ind, false);
        if (item == NULL) goto error;

        /* Append item to set */
        if (PySet_Add(out, item) < 0) goto error;
        Py_CLEAR(item);
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
js_decode_vartuple(
    JSONDecoderState *self, TypeNode *el_type,
    TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    PyObject *list, *item, *out = NULL;
    Py_ssize_t size, i;

    list = js_decode_list(self, el_type, ctx, ctx_ind, is_key);
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
js_decode_array(
    JSONDecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    if (type->types & MS_TYPE_ANY) {
        if (is_key) {
            return js_decode_vartuple(self, type, ctx, ctx_ind, is_key);
        }
        else {
            return js_decode_list(self, type, ctx, ctx_ind, false);
        }
    }
    else if (type->types & MS_TYPE_LIST) {
        return js_decode_list(self, TypeNode_get_array(type), ctx, ctx_ind, false);
    }
    else if (type->types & MS_TYPE_SET) {
        return js_decode_set(self, TypeNode_get_array(type), ctx, ctx_ind);
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return js_decode_vartuple(self, TypeNode_get_array(type), ctx, ctx_ind, is_key);
    }
    /* TODO: handle fixtuple and struct-array types */
    return mp_validation_error("list", type, ctx, ctx_ind);
}

#define JS_SCRATCH_MAX_SIZE 1024

static int
js_scratch_resize(JSONDecoderState *state, ssize_t size) {
    unsigned char *temp = PyMem_Realloc(state->scratch, size);
    if (MS_UNLIKELY(temp == NULL)) {
        PyErr_NoMemory();
        return -1;
    }
    state->scratch = temp;
    state->scratch_capacity = size;
    return 0;
}

static int
js_scratch_ensure_space(JSONDecoderState *state, Py_ssize_t size) {
    Py_ssize_t required = state->scratch_len + size;
    if (required >= state->scratch_capacity) {
        size_t new_size = Py_MAX(8, 1.5 * required);
        if (js_scratch_resize(state, new_size) < 0) return -1;
    }
    return 0;
}

static int
js_scratch_reset(JSONDecoderState *state) {
    state->scratch_len = 0;
    if (state->scratch_capacity > JS_SCRATCH_MAX_SIZE) {
        if (js_scratch_resize(state, JS_SCRATCH_MAX_SIZE) < 0) return -1;
    }
    return 0;
}

static int
js_scratch_extend(JSONDecoderState *state, const void *buf, Py_ssize_t size) {
    if (js_scratch_ensure_space(state, size) < 0) return -1;
    memcpy(state->scratch + state->scratch_len, buf, size);
    state->scratch_len += size;
    return 0;
}

static int
js_scratch_push(JSONDecoderState *state, unsigned char c) {
    return js_scratch_extend(state, &c, 1);
}

static int
js_read_codepoint(JSONDecoderState *self, unsigned int *out) {
    unsigned char c;
    unsigned int cp = 0;
    if (!js_remaining(self, 4)) return ms_err_truncated();
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
            js_err_invalid("invalid unicode escape");
            return -1;
        }
        cp = (cp << 4) + c;
    }
    *out = cp;
    return 0;
}

static int
js_parse_escape(JSONDecoderState *self) {
    unsigned char c;
    if (!js_read1(self, &c)) return -1;

    switch (c) {
        case '"': return js_scratch_push(self, '"');
        case '\\': return js_scratch_push(self, '\\');
        case '/': return js_scratch_push(self, '/');
        case 'b': return js_scratch_push(self, '\b');
        case 'f': return js_scratch_push(self, '\f');
        case 'n': return js_scratch_push(self, '\n');
        case 'r': return js_scratch_push(self, '\r');
        case 't': return js_scratch_push(self, '\t');
        case 'u': {
            unsigned int cp;
            if (js_read_codepoint(self, &cp) < 0) return -1;

            if (cp >= 0xD800 && cp <= 0xD8FF) {
                /* utf-16 pair, parse 2nd pair */
                unsigned int cp2;
                if (!js_remaining(self, 6)) return ms_err_truncated();
                if (self->input_pos[0] != '\\' || self->input_pos[1] != 'u') {
                    js_err_invalid("unexpected end of hex escape");
                    return -1;
                }
                self->input_pos += 2;
                if (js_read_codepoint(self, &cp2) < 0) return -1;
                if (cp2 < 0xDC00 || cp2 > 0xDFFF) {
                    js_err_invalid("invalid utf-16 surrogate pair");
                    return -1;
                }
                cp = 0x10000 + (((cp - 0xD800) << 10) | (cp2 - 0xDC00));
            }

            /* Encode the codepoint as utf-8 */
            if (js_scratch_ensure_space(self, 4) < 0) return -1;
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
        default:
            PyErr_SetString(msgspec_get_global_state()->DecodingError, "Invalid escape");
            return -1;
    }
    return 0;
}

static Py_ssize_t
js_decode_string_view(JSONDecoderState *self, char **out) {
    Py_ssize_t size;
    self->scratch_len = 0;
    self->input_pos++; /* Skip '"' */
    unsigned char *start = self->input_pos;
    while (true) {
        while (self->input_pos < self->input_end && !escape_table[*(self->input_pos)]) {
            self->input_pos += 1;
        }
        if (MS_UNLIKELY(self->input_pos == self->input_end)) {
            return ms_err_truncated();
        }
        switch (*(self->input_pos)) {
            case '"': {
                if (self->scratch_len == 0) {
                    *out = (char *)start;
                    size = self->input_pos - start;
                }
                else {
                    if (js_scratch_extend(self, start, self->input_pos - start) < 0) return -1;
                    *out = (char *)(self->scratch);
                    size = self->scratch_len;
                }
                self->input_pos += 1;
                return size;
            }
            case '\\': {
                if (js_scratch_extend(self, start, self->input_pos - start) < 0) return -1;
                self->input_pos += 1;
                if (js_parse_escape(self) < 0) return -1;
                start = self->input_pos;
                break;
            }
            default: {
                self->input_pos += 1;
                js_err_invalid("Invalid character");
                return -1;
            }
        }
    }
}

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
js_decode_binary(
    JSONDecoderState *self, const char *buffer, Py_ssize_t size,
    TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind
) {
    PyObject *out = NULL;
    char *bin_buffer;
    Py_ssize_t bin_size, i;

    if (size % 4 != 0) goto invalid;

    int npad = 0;
    if (size > 0 && buffer[size - 1] == '=') npad++;
    if (size > 1 && buffer[size - 2] == '=') npad++;

    bin_size = (size / 4) * 3 - npad;
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
    PyErr_SetString(
        msgspec_get_global_state()->DecodingError,
        "Invalid base64 encoded string"
    );
    return NULL;
}

static PyObject *
js_decode_string(JSONDecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind) {
    if (MS_LIKELY(type->types & (MS_TYPE_ANY | MS_TYPE_STR | MS_TYPE_ENUM | MS_TYPE_BYTES | MS_TYPE_BYTEARRAY))) {
        char *view = NULL;
        Py_ssize_t size = js_decode_string_view(self, &view);
        if (size < 0) return NULL;
        if (MS_UNLIKELY(type->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY))) {
            return js_decode_binary(self, view, size, type, ctx, ctx_ind);
        }
        PyObject *val = PyUnicode_DecodeUTF8(view, size, NULL);
        if (val == NULL) return NULL;
        if (MS_UNLIKELY(type->types & MS_TYPE_ENUM)) {
            return mp_decode_type_enum(val, type);
        }
        return val;
    }
    return mp_validation_error("str", type, ctx, ctx_ind);
}

static PyObject *
js_decode_dict(
    JSONDecoderState *self, TypeNode *key_type, TypeNode *val_type,
    TypeNode *ctx, Py_ssize_t ctx_ind
) {
    PyObject *out, *key = NULL, *val = NULL;
    unsigned char c;
    bool first = true;

    self->input_pos++; /* Skip '{' */

    out = PyDict_New();
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            js_err_invalid("expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            key = js_decode_string(self, key_type, ctx, ctx_ind);
            if (key == NULL) goto error;
        }
        else if (c == ',') {
            js_err_invalid("trailing comma in object");
            goto error;
        }
        else {
            js_err_invalid("key must be a string");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            js_err_invalid("expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        val = js_decode(self, val_type, ctx, ctx_ind, false);
        if (val == NULL) goto error;

        /* Add item to dict */
        if (MS_UNLIKELY(PyDict_SetItem(out, key, val) < 0))
            goto error;
        Py_CLEAR(key);
        Py_CLEAR(val);
    }
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
js_decode_struct_map(
    JSONDecoderState *self, StructMetaObject *st_type, TypeNode *type, bool is_key
) {
    PyObject *out, *val = NULL;
    Py_ssize_t key_size, field_index, pos = 0;
    unsigned char c;
    char *key = NULL;
    bool first = true;

    self->input_pos++; /* Skip '{' */

    out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL;
    }
    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            js_err_invalid("expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            key_size = js_decode_string_view(self, &key);
            if (key_size < 0) goto error;
        }
        else if (c == ',') {
            js_err_invalid("trailing comma in object");
            goto error;
        }
        else {
            js_err_invalid("key must be a string");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            js_err_invalid("expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        field_index = StructMeta_get_field_index(st_type, key, key_size, &pos);
        if (field_index < 0) {
            /* Skip unknown fields */
            if (js_skip(self) < 0) goto error;
        }
        else {
            val = js_decode(
                self, st_type->struct_types[field_index], type, field_index, is_key
            );
            if (val == NULL) goto error;
            Struct_set_index(out, field_index, val);
        }
    }
    if (Struct_fill_in_defaults(st_type, out) < 0) goto error;
    Py_LeaveRecursiveCall();
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
js_decode_object(
    JSONDecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    if (type->types & MS_TYPE_ANY) {
        return js_decode_dict(self, type, type, ctx, ctx_ind);
    }
    else if (type->types & MS_TYPE_DICT) {
        TypeNode *key, *val;
        TypeNode_get_dict(type, &key, &val);
        return js_decode_dict(self, key, val, ctx, ctx_ind);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        StructMetaObject *struct_type = TypeNode_get_struct(type);
        if (struct_type->asarray != OPT_TRUE) {
            return js_decode_struct_map(self, struct_type, type, is_key);
        }
    }
    return mp_validation_error("dict", type, ctx, ctx_ind);
}

static PyObject *
js_decode(
    JSONDecoderState *self, TypeNode *type, TypeNode *ctx, Py_ssize_t ctx_ind, bool is_key
) {
    unsigned char c;

    if (MS_UNLIKELY(!js_peek_skip_ws(self, &c))) return NULL;

    switch (c) {
        case 'n': return js_decode_none(self, type, ctx, ctx_ind);
        case 't': return js_decode_true(self, type, ctx, ctx_ind);
        case 'f': return js_decode_false(self, type, ctx, ctx_ind);
        case '[': return js_decode_array(self, type, ctx, ctx_ind, is_key);
        case '{': return js_decode_object(self, type, ctx, ctx_ind, is_key);
        case '"': return js_decode_string(self, type, ctx, ctx_ind);
        default: {
            return js_err_invalid("invalid character");
        }
    }
}

PyDoc_STRVAR(JSONDecoder_decode__doc__,
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
"    The deserialized object\n"
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

    if (PyObject_GetBuffer(args[0], &buffer, PyBUF_CONTIG_RO) >= 0) {
        self->state.buffer_obj = args[0];
        self->state.input_pos = buffer.buf;
        self->state.input_end = buffer.buf + buffer.len;
        res = js_decode(&(self->state), self->state.type, self->state.type, -1, false);
    }

    if (buffer.buf != NULL) {
        PyBuffer_Release(&buffer);
        self->state.buffer_obj = NULL;
        self->state.input_pos = NULL;
        self->state.input_end = NULL;
    }
    js_scratch_reset(&(self->state));

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
    {"tzinfo", T_OBJECT, offsetof(JSONDecoder, state.tzinfo), READONLY, "The Decoder tzinfo"},
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
"decode(buf, *, type='Any', dec_hook=None, tzinfo=None)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like\n"
"    The message to decode.\n"
"type : Type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default JSON types.\n"
"dec_hook : Callable, optional\n"
"    An optional callback for handling decoding custom types. Should have the\n"
"    signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type`` is the\n"
"    expected message type, and ``obj`` is the decoded representation composed\n"
"    of only basic JSON types. This hook should transform ``obj`` into type\n"
"    ``type``, or raise a ``TypeError`` if unsupported.\n"
"tzinfo : datetime.tzinfo, optional\n"
"    The timezone to use when decoding ``datetime.datetime`` objects. Defaults\n"
"    to ``None`` for \"naive\" datetimes.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object\n"
"\n"
"See Also\n"
"--------\n"
"Decoder.decode"
);
static PyObject*
msgspec_json_decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *res = NULL, *buf = NULL, *type = NULL, *dec_hook = NULL, *tzinfo = NULL;
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
        if ((tzinfo = find_keyword(kwnames, args + nargs, st->str_tzinfo)) != NULL) nkwargs--;
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

    /* Handle tzinfo */
    if (tzinfo == Py_None) {
        tzinfo = NULL;
    }
    if (tzinfo != NULL) {
        int ok = PyObject_IsInstance(tzinfo, (PyObject *)(PyDateTimeAPI->TZInfoType));
        if (ok == -1) return NULL;
        if (ok == 0) {
            PyErr_SetString(PyExc_TypeError, "tzinfo must be an instance of tzinfo");
            return NULL;
        }
        Py_INCREF(tzinfo);
    }
    state.tzinfo = tzinfo;

    state.scratch = NULL;
    state.scratch_capacity = 0;
    state.scratch_len = 0;

    /* Only build TypeNode if required */
    state.type = NULL;
    if (type != NULL && type != st->typing_any) {
        state.type = TypeNode_Convert(type);
        if (state.type == NULL) return NULL;
    }

    buffer.buf = NULL;
    if (PyObject_GetBuffer(buf, &buffer, PyBUF_CONTIG_RO) >= 0) {
        state.buffer_obj = buf;
        state.input_pos = buffer.buf;
        state.input_end = buffer.buf + buffer.len;

        if (state.type != NULL) {
            res = js_decode(&state, state.type, state.type, -1, false);
        } else {
            TypeNode type_any = {MS_TYPE_ANY};
            res = js_decode(&state, &type_any, &type_any, -1, false);
        }
    }

    PyMem_Free(state.scratch);

    if (state.type != NULL) {
        TypeNode_Free(state.type);
    }

    if (buffer.buf != NULL) {
        PyBuffer_Release(&buffer);
    }
    return res;
}


/*************************************************************************
 * Module Setup                                                          *
 *************************************************************************/

static struct PyMethodDef msgspec_methods[] = {
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
    {NULL, NULL} /* sentinel */
};

static int
msgspec_clear(PyObject *m)
{
    MsgspecState *st = msgspec_get_state(m);
    Py_CLEAR(st->MsgspecError);
    Py_CLEAR(st->EncodingError);
    Py_CLEAR(st->DecodingError);
    Py_CLEAR(st->StructType);
    Py_CLEAR(st->EnumType);
    Py_CLEAR(st->str__name_);
    Py_CLEAR(st->str__value2member_map_);
    Py_CLEAR(st->str_name);
    Py_CLEAR(st->str_type);
    Py_CLEAR(st->str_enc_hook);
    Py_CLEAR(st->str_dec_hook);
    Py_CLEAR(st->str_ext_hook);
    Py_CLEAR(st->str_tzinfo);
    Py_CLEAR(st->str___origin__);
    Py_CLEAR(st->str___args__);
    Py_CLEAR(st->typing_dict);
    Py_CLEAR(st->typing_list);
    Py_CLEAR(st->typing_set);
    Py_CLEAR(st->typing_tuple);
    Py_CLEAR(st->typing_union);
    Py_CLEAR(st->typing_any);
    Py_CLEAR(st->get_type_hints);
    Py_CLEAR(st->timestamp);
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

    MsgspecState *st = msgspec_get_state(m);
    Py_VISIT(st->MsgspecError);
    Py_VISIT(st->EncodingError);
    Py_VISIT(st->DecodingError);
    Py_VISIT(st->StructType);
    Py_VISIT(st->EnumType);
    Py_VISIT(st->typing_dict);
    Py_VISIT(st->typing_list);
    Py_VISIT(st->typing_set);
    Py_VISIT(st->typing_tuple);
    Py_VISIT(st->typing_union);
    Py_VISIT(st->typing_any);
    Py_VISIT(st->get_type_hints);
    Py_VISIT(st->timestamp);
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
    if (PyType_Ready(&JSONEncoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&JSONDecoder_Type) < 0)
        return NULL;

    /* Create the module */
    m = PyModule_Create(&msgspecmodule);
    if (m == NULL)
        return NULL;

    /* Add types */
    Py_INCREF(&Encoder_Type);
    if (PyModule_AddObject(m, "MsgpackEncoder", (PyObject *)&Encoder_Type) < 0)
        return NULL;
    Py_INCREF(&Decoder_Type);
    if (PyModule_AddObject(m, "MsgpackDecoder", (PyObject *)&Decoder_Type) < 0)
        return NULL;
    Py_INCREF(&Ext_Type);
    if (PyModule_AddObject(m, "Ext", (PyObject *)&Ext_Type) < 0)
        return NULL;
    Py_INCREF(&JSONEncoder_Type);
    if (PyModule_AddObject(m, "JSONEncoder", (PyObject *)&JSONEncoder_Type) < 0)
        return NULL;
    if (PyModule_AddObject(m, "JSONDecoder", (PyObject *)&JSONDecoder_Type) < 0)
        return NULL;

    st = msgspec_get_state(m);

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
    st->EncodingError = PyErr_NewExceptionWithDoc(
        "msgspec.EncodingError",
        "An error occurred while encoding an object",
        st->MsgspecError, NULL
    );
    if (st->EncodingError == NULL)
        return NULL;
    st->DecodingError = PyErr_NewExceptionWithDoc(
        "msgspec.DecodingError",
        "An error occurred while decoding an object",
        st->MsgspecError, NULL
    );
    if (st->DecodingError == NULL)
        return NULL;

    Py_INCREF(st->MsgspecError);
    if (PyModule_AddObject(m, "MsgspecError", st->MsgspecError) < 0)
        return NULL;
    Py_INCREF(st->EncodingError);
    if (PyModule_AddObject(m, "EncodingError", st->EncodingError) < 0)
        return NULL;
    Py_INCREF(st->DecodingError);
    if (PyModule_AddObject(m, "DecodingError", st->DecodingError) < 0)
        return NULL;

#define SET_REF(attr, name) \
    do { \
    st->attr = PyObject_GetAttrString(temp_module, name); \
    if (st->attr == NULL) return NULL; \
    } while (0)

    /* Get all imports from the typing module */
    temp_module = PyImport_ImportModule("typing");
    if (temp_module == NULL) return NULL;
    SET_REF(typing_list, "List");
    SET_REF(typing_set, "Set");
    SET_REF(typing_tuple, "Tuple");
    SET_REF(typing_dict, "Dict");
    SET_REF(typing_union, "Union");
    SET_REF(typing_any, "Any");
    SET_REF(get_type_hints, "get_type_hints");
    Py_DECREF(temp_module);

    /* Get the EnumType */
    temp_module = PyImport_ImportModule("enum");
    if (temp_module == NULL)
        return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "Enum");
    Py_DECREF(temp_module);
    if (temp_obj == NULL)
        return NULL;
    if (!PyType_Check(temp_obj)) {
        Py_DECREF(temp_obj);
        PyErr_SetString(PyExc_TypeError, "enum.Enum should be a type");
        return NULL;
    }
    st->EnumType = (PyTypeObject *)temp_obj;

    /* Get the datetime.datetime.timestamp method */
    temp_module = PyImport_ImportModule("datetime");
    if (temp_module == NULL)
        return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "datetime");
    Py_DECREF(temp_module);
    if (temp_obj == NULL)
        return NULL;
    st->timestamp = PyObject_GetAttrString(temp_obj, "timestamp");
    Py_DECREF(temp_obj);
    if (st->timestamp == NULL)
        return NULL;

    /* Initialize cached constant strings */
    st->str__name_ = PyUnicode_InternFromString("_name_");
    if (st->str__name_ == NULL)
        return NULL;
    st->str__value2member_map_ = PyUnicode_InternFromString("_value2member_map_");
    if (st->str__value2member_map_ == NULL)
        return NULL;
    st->str_name = PyUnicode_InternFromString("name");
    if (st->str_name == NULL)
        return NULL;
    st->str_type = PyUnicode_InternFromString("type");
    if (st->str_type == NULL)
        return NULL;
    st->str_enc_hook = PyUnicode_InternFromString("enc_hook");
    if (st->str_enc_hook == NULL)
        return NULL;
    st->str_dec_hook = PyUnicode_InternFromString("dec_hook");
    if (st->str_dec_hook == NULL)
        return NULL;
    st->str_ext_hook = PyUnicode_InternFromString("ext_hook");
    if (st->str_ext_hook == NULL)
        return NULL;
    st->str_tzinfo = PyUnicode_InternFromString("tzinfo");
    if (st->str_tzinfo == NULL)
        return NULL;
    st->str___origin__ = PyUnicode_InternFromString("__origin__");
    if (st->str___origin__ == NULL)
        return NULL;
    st->str___args__ = PyUnicode_InternFromString("__args__");
    if (st->str___args__ == NULL)
        return NULL;

    return m;
}
