#include <stdarg.h>
#include <stdint.h>
#include "Python.h"
#include "datetime.h"
#include "structmember.h"

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
    PyObject *StructType;
    PyTypeObject *EnumType;
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
msgspec_get_global_state()
{
    return msgspec_get_state(PyState_FindModule(&msgspecmodule));
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
 * Struct Types                                                          *
 *************************************************************************/

typedef struct {
    PyHeapTypeObject base;
    PyObject *struct_fields;
    PyObject *struct_defaults;
    Py_ssize_t *struct_offsets;
} StructMetaObject;

static PyTypeObject StructMetaType;
static PyTypeObject StructMixinType;

#define StructMeta_GET_FIELDS(s) (((StructMetaObject *)(s))->struct_fields);
#define StructMeta_GET_NFIELDS(s) (PyTuple_GET_SIZE((((StructMetaObject *)(s))->struct_fields)));
#define StructMeta_GET_DEFAULTS(s) (((StructMetaObject *)(s))->struct_defaults);
#define StructMeta_GET_OFFSETS(s) (((StructMetaObject *)(s))->struct_offsets);

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

    /* Parse arguments: (name, bases, dict) */
    if (!PyArg_ParseTuple(args, "UO!O!:StructMeta.__new__", &name, &PyTuple_Type,
                          &bases, &PyDict_Type, &orig_dict))
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

    cls = (StructMetaObject *) PyType_Type.tp_new(type, new_args, kwargs);
    if (cls == NULL)
        goto error;
    ((PyTypeObject *)cls)->tp_vectorcall = (vectorcallfunc)Struct_vectorcall;
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
StructMeta_traverse(StructMetaObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->struct_fields);
    Py_VISIT(self->struct_defaults);
    return PyType_Type.tp_traverse((PyObject *)self, visit, arg);
}

static int
StructMeta_clear(StructMetaObject *self)
{
    Py_CLEAR(self->struct_fields);
    Py_CLEAR(self->struct_defaults);
    PyMem_Free(self->struct_offsets);
    return PyType_Type.tp_clear((PyObject *)self);
}

static void
StructMeta_dealloc(StructMetaObject *self)
{
    Py_XDECREF(self->struct_fields);
    Py_XDECREF(self->struct_defaults);
    PyType_Type.tp_dealloc((PyObject *)self);
}

static PyObject*
StructMeta_signature(StructMetaObject *self, void *closure)
{
    Py_ssize_t nfields, ndefaults, npos, i;
    PyObject *res = NULL;
    PyObject *inspect = NULL;
    PyObject *parameter_cls = NULL;
    PyObject *parameter_empty = NULL;
    PyObject *parameter_kind = NULL;
    PyObject *signature_cls = NULL;
    PyObject *typing = NULL;
    PyObject *get_type_hints = NULL;
    PyObject *annotations = NULL;
    PyObject *parameters = NULL;
    PyObject *temp_args = NULL, *temp_kwargs = NULL;
    PyObject *field, *default_val, *parameter, *annotation;

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
    typing = PyImport_ImportModule("typing");
    if (typing == NULL)
        goto cleanup;
    get_type_hints = PyObject_GetAttrString(typing, "get_type_hints");
    if (get_type_hints == NULL)
        goto cleanup;

    annotations = PyObject_CallFunctionObjArgs(get_type_hints, self, NULL);
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
    res = PyObject_CallFunctionObjArgs(signature_cls, parameters, NULL);
cleanup:
    Py_XDECREF(inspect);
    Py_XDECREF(parameter_cls);
    Py_XDECREF(parameter_empty);
    Py_XDECREF(parameter_kind);
    Py_XDECREF(signature_cls);
    Py_XDECREF(typing);
    Py_XDECREF(get_type_hints);
    Py_XDECREF(annotations);
    Py_XDECREF(parameters);
    Py_XDECREF(temp_args);
    Py_XDECREF(temp_kwargs);
    return res;
}

static PyMemberDef StructMeta_members[] = {
    {"__struct_fields__", T_OBJECT_EX, offsetof(StructMetaObject, struct_fields), READONLY, "Struct fields"},
    {"__struct_defaults__", T_OBJECT_EX, offsetof(StructMetaObject, struct_defaults), READONLY, "Struct defaults"},
    {NULL},
};

static PyGetSetDef StructMeta_getset[] = {
    {"__signature__", (getter) StructMeta_signature, NULL, NULL, NULL},
    {NULL},
};

static PyTypeObject StructMetaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.StructMeta",
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
maybe_deepcopy_default(PyObject *obj, int *is_copy) {
    MsgspecState *st;
    PyObject *copy = NULL, *deepcopy = NULL, *res = NULL;
    PyTypeObject *type = Py_TYPE(obj);

    /* Known non-collection types */
    if (obj == Py_None || obj == Py_False || obj == Py_True ||
        type == &PyLong_Type || type == &PyFloat_Type ||
        type == &PyBytes_Type || type == &PyUnicode_Type ||
        type == &PyByteArray_Type
    ) {
        return obj;
    }
    else if (type == &PyTuple_Type && (PyTuple_GET_SIZE(obj) == 0)) {
        return obj;
    }
    else if (type == &PyFrozenSet_Type && PySet_GET_SIZE(obj) == 0) {
        return obj;
    }
    else if (type == PyDateTimeAPI->DeltaType ||
             type == PyDateTimeAPI->DateTimeType ||
             type == PyDateTimeAPI->DateType ||
             type == PyDateTimeAPI->TimeType
    ) {
        return obj;
    }

    st = msgspec_get_global_state();
    if (PyType_IsSubtype(type, st->EnumType)) {
        return obj;
    }

    if (is_copy != NULL)
        *is_copy = 1;

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
    res = PyObject_CallFunctionObjArgs(deepcopy, obj, NULL);
cleanup:
    Py_XDECREF(copy);
    Py_XDECREF(deepcopy);
    return res;
}

#if PY_VERSION_HEX < 0x03090000
#define IS_TRACKED _PyObject_GC_IS_TRACKED
#define CALL_ONE_ARG(fn, arg) PyObject_CallFunctionObjArgs((fn), (arg), NULL)
#else
#define IS_TRACKED  PyObject_GC_IsTracked
#define CALL_ONE_ARG(fn, arg) PyObject_CallOneArg((fn), (arg))
#endif
/* Is this object something that is/could be GC tracked? True if
 * - the value supports GC
 * - the value isn't a tuple or the object is tracked (skip tracked checks for non-tuples)
 */
#define OBJ_IS_GC(x) \
    (PyType_IS_GC(Py_TYPE(x)) && \
     (!PyTuple_CheckExact(x) || IS_TRACKED(x)))

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

/* Get field #index on obj. Returns a borrowed reference */
static inline PyObject*
Struct_get_index(PyObject *obj, Py_ssize_t index) {
    StructMetaObject *cls;
    char *addr;
    PyObject *val;

    cls = (StructMetaObject *)Py_TYPE(obj);
    addr = (char *)obj + cls->struct_offsets[index];
    val = *(PyObject **)addr;
    if (val == NULL)
        PyErr_Format(PyExc_AttributeError,
                     "Struct field %R is unset",
                     PyTuple_GET_ITEM(cls->struct_fields, index));
    return val;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyObject *self, *fields, *defaults, *field, *val;
    Py_ssize_t nargs, nkwargs, nfields, ndefaults, npos, i;
    int is_copy, should_untrack;

    self = cls->tp_alloc(cls, 0);
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
        return NULL;
    }

    should_untrack = PyObject_IS_GC(self);

    for (i = 0; i < nfields; i++) {
        is_copy = 0;
        field = PyTuple_GET_ITEM(fields, i);
        val = (nkwargs == 0) ? NULL : find_keyword(kwnames, args + nargs, field);
        if (val != NULL) {
            if (i < nargs) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Argument '%U' given by name and position",
                    field
                );
                return NULL;
            }
            nkwargs -= 1;
        }
        else if (i < nargs) {
            val = args[i];
        }
        else if (i < npos) {
            PyErr_Format(
                PyExc_TypeError,
                "Missing required argument '%U'",
                field
            );
            return NULL;
        }
        else {
            val = maybe_deepcopy_default(PyTuple_GET_ITEM(defaults, i - npos), &is_copy);
            if (val == NULL)
                return NULL;
        }
        Struct_set_index(self, i, val);
        if (!is_copy)
            Py_INCREF(val);
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    if (nkwargs > 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra keyword arguments provided"
        );
        return NULL;
    }
    if (should_untrack)
        PyObject_GC_UnTrack(self);
    return self;
}

static int
Struct_setattro(PyObject *self, PyObject *key, PyObject *value) {
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

    res = Py_TYPE(self)->tp_alloc(Py_TYPE(self), 0);
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
    .tp_name = "msgspec._StructMixin",
    .tp_basicsize = 0,
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_setattro = Struct_setattro,
    .tp_repr = Struct_repr,
    .tp_richcompare = Struct_richcompare,
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
);

/*************************************************************************
 * MessagePack                                                           *
 *************************************************************************/

typedef struct MessagePack {
    PyObject_HEAD
    /* Configuration */
    Py_ssize_t write_buffer_size;

    PyObject *output_buffer;    /* Write into a local bytearray buffer before
                                   flushing to the stream */
    Py_ssize_t output_len;      /* Length of output_buffer */
    Py_ssize_t max_output_len;  /* Allocation size of output_buffer */
} MessagePack;

PyDoc_STRVAR(MessagePack__doc__,
"MessagePack(*, write_buffer_size=4096)\n"
"--\n"
"\n"
"A MessagePack encoder/decoder.\n"
"\n"
"Parameters\n"
"----------\n"
"write_buffer_size : int, optional\n"
"    The size of the internal static write buffer."
);
static int
MessagePack_init(MessagePack *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"write_buffer_size", NULL};
    Py_ssize_t write_buffer_size = 4096;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$n", kwlist,
                                     &write_buffer_size)) {
        return -1;
    }

    self->write_buffer_size = Py_MAX(write_buffer_size, 32);
    self->max_output_len = self->write_buffer_size;
    self->output_len = 0;
    self->output_buffer = PyBytes_FromStringAndSize(NULL, self->max_output_len);
    if (self->output_buffer == NULL)
        return -1;
    return 0;
}

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
    MP_INT64 = '\xd3'
};

static int
mp_write(MessagePack *self, const char *s, Py_ssize_t n)
{
    Py_ssize_t required;
    char *buffer;

    required = self->output_len + n;
    if (required > self->max_output_len) {
        /* Make space in buffer */
        if (self->output_len >= PY_SSIZE_T_MAX / 2 - n) {
            PyErr_NoMemory();
            return -1;
        }
        self->max_output_len = (self->output_len + n) / 2 * 3;
        if (_PyBytes_Resize(&self->output_buffer, self->max_output_len) < 0)
            return -1;
    }
    buffer = PyBytes_AS_STRING(self->output_buffer);
    memcpy(buffer + self->output_len, s, n);
    self->output_len += n;
    return 0;
}

static int
mp_encode_none(MessagePack *self)
{
    const char op = MP_NIL;
    return mp_write(self, &op, 1);
}

static int
mp_encode_bool(MessagePack *self, PyObject *obj)
{
    const char op = (obj == Py_True) ? MP_TRUE : MP_FALSE;
    return mp_write(self, &op, 1);
}

static int
mp_encode_long(MessagePack *self, PyObject *obj)
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
                return mp_write(self, buf, 9);
            } else {
                char buf[5];
                buf[0] = MP_INT32;
                _msgspec_store32(&buf[1], (int32_t)x);
                return mp_write(self, buf, 5);
            }
        } else {
            if(x < -(1<<7)) {
                char buf[3];
                buf[0] = MP_INT16;
                _msgspec_store16(&buf[1], (int16_t)x);
                return mp_write(self, buf, 3);
            } else {
                char buf[2] = {MP_INT8, (x & 0xff)};
                return mp_write(self, buf, 2);
            }
        }
    } else if(x < (1<<7)) {
        char buf[1] = {(x & 0xff)};
        return mp_write(self, buf, 1);
    } else {
        if(x < (1<<16)) {
            if(x < (1<<8)) {
                char buf[2] = {MP_UINT8, (x & 0xff)};
                return mp_write(self, buf, 2);
            } else {
                char buf[3];
                buf[0] = MP_UINT16;
                _msgspec_store16(&buf[1], (uint16_t)x);
                return mp_write(self, buf, 3);
            }
        } else {
            if(x < (1LL<<32)) {
                char buf[5];
                buf[0] = MP_UINT32;
                _msgspec_store32(&buf[1], (uint32_t)x);
                return mp_write(self, buf, 5);
            } else {
                char buf[9];
                buf[0] = MP_UINT64;
                _msgspec_store64(&buf[1], ux);
                return mp_write(self, buf, 9);
            }
        }
    }
}

static int
mp_encode_float(MessagePack *self, PyObject *obj)
{
    char buf[9];
    double x = PyFloat_AS_DOUBLE(obj);
    buf[0] = MP_FLOAT64;
    if (_PyFloat_Pack8(x, (unsigned char *)(&buf[1]), 0) < 0) {
        return -1;
    }
    return mp_write(self, buf, 9);
}

static int
mp_encode(MessagePack *self, PyObject *obj)
{
    PyTypeObject *type;

    type = Py_TYPE(obj);

    if (obj == Py_None) {
        return mp_encode_none(self);
    }
    else if (obj == Py_False || obj == Py_True) {
        return mp_encode_bool(self, obj);
    }
    else if (type == &PyLong_Type) {
        return mp_encode_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return mp_encode_float(self, obj);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "MessagePack.encode doesn't support objects of type %.200s",
                     type->tp_name);
        return -1;
    }
}

PyDoc_STRVAR(MessagePack_encode__doc__,
"encode(obj)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object\n"
);
static PyObject*
MessagePack_encode(MessagePack *self, PyObject *const *args, Py_ssize_t nargs)
{
    int status;
    PyObject *res = NULL, *obj = NULL;

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }
    obj = args[0];

    /* reset buffer */
    self->output_len = 0;
    if (self->output_buffer == NULL) {
        self->max_output_len = self->write_buffer_size;
        self->output_buffer = PyBytes_FromStringAndSize(NULL, self->max_output_len);
        if (self->output_buffer == NULL)
            return NULL;
    }

    status = mp_encode(self, obj);

    if (status == 0) {
        if (self->max_output_len > self->write_buffer_size) {
            /* Buffer was resized, trim to length */
            res = self->output_buffer;
            self->output_buffer = NULL;
            _PyBytes_Resize(&res, self->output_len);
        }
        else {
            /* Only constant buffer used, copy to output */
            res = PyBytes_FromStringAndSize(
                PyBytes_AS_STRING(self->output_buffer),
                self->output_len
            );
        }
    } else {
        /* Error in encode, drop buffer if necessary */
        if (self->max_output_len > self->write_buffer_size) {
            Py_DECREF(self->output_buffer);
            self->output_buffer = NULL;
        }
    }
    return res;
}

static PyObject*
MessagePack_sizeof(MessagePack *self)
{
    Py_ssize_t res;

    res = sizeof(MessagePack);
    if (self->output_buffer != NULL) {
        res += self->max_output_len;
    }
    return PyLong_FromSsize_t(res);
}

static struct PyMethodDef MessagePack_methods[] = {
    {
        "encode", (PyCFunction) MessagePack_encode, METH_FASTCALL,
        MessagePack_encode__doc__,
    },
    {
        "__sizeof__", (PyCFunction) MessagePack_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static void
MessagePack_dealloc(MessagePack *self)
{
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject MessagePack_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec.core.MessagePack",
    .tp_doc = MessagePack__doc__,
    .tp_basicsize = sizeof(MessagePack),
    .tp_dealloc = (destructor)MessagePack_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)MessagePack_init,
    .tp_methods = MessagePack_methods,
};

/*************************************************************************
 * Module Setup                                                          *
 *************************************************************************/

static int
msgspec_clear(PyObject *m)
{
    MsgspecState *st = msgspec_get_state(m);
    Py_CLEAR(st->StructType);
    Py_CLEAR(st->EnumType);
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
    Py_VISIT(st->StructType);
    Py_VISIT(st->EnumType);
    return 0;
}

static struct PyModuleDef msgspecmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "msgspec.core",
    .m_size = sizeof(MsgspecState),
    .m_traverse = msgspec_traverse,
    .m_clear = msgspec_clear,
    .m_free =(freefunc)msgspec_free
};

PyMODINIT_FUNC
PyInit_core(void)
{
    PyObject *m, *temp_module, *temp_type;
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
    if (PyType_Ready(&MessagePack_Type) < 0)
        return NULL;

    /* Create the module */
    m = PyModule_Create(&msgspecmodule);
    if (m == NULL)
        return NULL;

    /* Add types */
    Py_INCREF(&MessagePack_Type);
    if (PyModule_AddObject(m, "MessagePack", (PyObject *)&MessagePack_Type) < 0)
        return NULL;

    st = msgspec_get_state(m);

    /* Initialize the Struct Type */
    st->StructType = PyObject_CallFunction(
        (PyObject *)&StructMetaType, "s(O){ssss}", "Struct", &StructMixinType,
        "__module__", "msgspec.core", "__doc__", Struct__doc__
    );
    if (st->StructType == NULL)
        return NULL;
    Py_INCREF(st->StructType);
    if (PyModule_AddObject(m, "Struct", st->StructType) < 0)
        return NULL;

    /* Get the EnumType */
    temp_module = PyImport_ImportModule("enum");
    if (temp_module == NULL)
        return NULL;
    temp_type = PyObject_GetAttrString(temp_module, "Enum");
    Py_DECREF(temp_module);
    if (temp_type == NULL)
        return NULL;
    if (!PyType_Check(temp_type)) {
        Py_DECREF(temp_type);
        PyErr_SetString(PyExc_TypeError, "enum.Enum should be a type");
        return NULL;
    }
    st->EnumType = (PyTypeObject *)temp_type;

    return m;
}
