#define PY_SSIZE_T_CLEAN
#include <Python.h>
#define MSGSPEC_USE_STRUCTURES
#define MSGSPEC_USE_CAPSULE_API
#include "msgspec.h"

#include <stddef.h>

/* Modeled after multidict's C-API tests written by the same author Vizonex */

/* TODO: (Vizonex) 
    I think I could make a template github repo 
    for users who want a quick and dirty template to work off of for 
    something like this... 
*/

typedef struct _mod_state {
    Msgspec_CAPI* capi;
} mod_state;

static PyModuleDef _testcapi_module;

static inline mod_state *
get_mod_state(PyObject *mod)
{
    mod_state *state = (mod_state *)PyModule_GetState(mod);
    assert(state != NULL);
    return state;
}

static mod_state *
testcapi_get_global_state(void)
{
    PyObject *module = PyState_FindModule(&_testcapi_module);
    return module == NULL ? NULL : get_mod_state(module);
}

static inline Msgspec_CAPI*
get_capi(PyObject *mod)
{
    return get_mod_state(mod)->capi;
}

static int
check_nargs(const char *name, const Py_ssize_t nargs, const Py_ssize_t required)
{
    if (nargs != required) {
        PyErr_Format(PyExc_TypeError,
                     "%s should be called with %d arguments, got %d",
                     name,
                     required,
                     nargs);
        return -1;
    }
    return 0;
}

/* Factory Objects */
static PyObject * factory_type(PyObject* mod, PyObject* Py_UNUSED(unused)){
    return Py_NewRef((PyObject*)get_capi(mod)->Factory_Type);
}

//  PyObject *const *args, Py_ssize_t nargs

static PyObject* factory_check(PyObject* mod, PyObject* arg){
    if (Factory_Check(get_capi(mod), arg)){
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

// TBD
// static PyObject* factory_check_exact(PyObject* mod, PyObject* arg){
//     if (Factory_CheckExact(get_capi(mod), arg)){
//         Py_RETURN_TRUE;
//     }
//     Py_RETURN_FALSE;
// }

static PyObject* factory_new(PyObject* mod, PyObject* arg){
    return get_capi(mod)->Factory_New(arg);
}

static PyObject* factory_create(PyObject* mod, PyObject* arg){
    return get_capi(mod)->Factory_Create(arg);
}


/* Field_Type Objects... */

static PyObject* field_check(PyObject* mod, PyObject* arg){
    if (Field_Check(get_capi(mod), arg)){
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject * field_type(PyObject* mod, PyObject* Py_UNUSED(unused)){
    return Py_NewRef((PyObject*)get_capi(mod)->Field_Type);
}

static PyObject* field_new(PyObject* mod, PyObject *const *args, Py_ssize_t nargs){
    if (check_nargs("field_new", nargs, 3) < 0){
        return NULL;
    }
    // Will simulate None as NULL since it's kind of difficult to simulate a Null Pointer...
    PyObject* _default = Py_IsNone(args[1]) ? NULL: Py_NewRef(args[1]);
    PyObject* _factory = Py_IsNone(args[2]) ? NULL: Py_NewRef(args[2]);
    PyObject* ret = get_capi(mod)->Field_New(args[0], _default, _factory);
    /* Py_CLEAR has null checks of it's own before it clears out a value... */
    Py_CLEAR(_default);
    Py_CLEAR(_factory);
    return ret;
}

typedef int (*capi_getter_func)(PyObject* self, PyObject** value);

static PyObject* handle_getter_capi_func(capi_getter_func func, PyObject* self){
    PyObject* value = NULL;
    if (func(self, &value) < 0){
        return NULL;
    }
    return value;
}

static PyObject* handle_getter_capi_func_can_be_null(capi_getter_func func, PyObject* self){
    PyObject* value = NULL;
    if (func(self, &value) < 0){
        return NULL;
    }
    if (value == NULL){
        Py_RETURN_NONE;
    }
    return value;
}


static PyObject* field_get_name(PyObject* mod, PyObject* arg){
    return handle_getter_capi_func(get_capi(mod)->Field_GetName, arg);
}
static PyObject* field_get_default(PyObject* mod, PyObject* arg){
    return handle_getter_capi_func_can_be_null(get_capi(mod)->Field_GetDefault, arg);
}
static PyObject* field_get_factory(PyObject* mod, PyObject* arg){
    return handle_getter_capi_func_can_be_null(get_capi(mod)->Field_GetFactory, arg);
}


/* This test simulates something very close to that of SQLModel 
and it simulates an ORM styled library where __table_name__ & __abstract_table__ attributes
are added in without disrupting Struct or anything that comes after it... */

typedef struct {
    StructMetaObject base;
    /* These items should not be called with other fellow struct 
    memebers these are primarly used as class attributes */
    const char* table_name;
    int abstract_table;
} TableMetaObject;

static PyTypeObject TableMetaType;

/* tp_base is a lifesaver and is the key to making a working subclassable type */

static PyObject* TableMeta_New(PyTypeObject* type, PyObject* args, PyObject* kwargs){
    /* Will access the newly made object using PyType_GenericNew to save us a hassle */
    int abstract = 1;
    int grab_later = 0;
    const char* table_name = NULL;

    mod_state* state = testcapi_get_global_state();
    PyObject* table = NULL;

    /* kwargs can show up as NULL whenever it feels like it so we need an extra check here. */
    if (kwargs != NULL)
        table = PyDict_GetItemString(kwargs, "table");
    if (table != NULL){
        /* table is confirmed now so we 
        initalize that value to prevent it 
        from getting deleted */
        Py_INCREF(table);

        if (PyUnicode_Check(table)){
            table_name = PyUnicode_AsUTF8(table);
            abstract = 0;
        } else if (PyBool_Check(table)) {
            if (Py_IsTrue(table)){
                abstract = 0;
                /* We grab the name from the class object being derrived instead*/
                table_name = PyUnicode_AsUTF8(PyTuple_GET_ITEM(args, 0));
            }
            /* if flase we shall remain abstract */
        }
        /* 
            disallow msgspec from complaining about our 
            newly added attribute by deleting it...
        */
        PyDict_DelItemString(kwargs, "table");
    }
    TableMetaObject *obj = (TableMetaObject*)type->tp_base->tp_new(type, args, kwargs);
    if (obj == NULL)
        return NULL;
    obj->abstract_table = abstract;
    obj->table_name = table_name;
    return (PyObject*)obj;
}


static int table_meta_traverse(PyObject * self, visitproc visit, void * arg){
    return Py_TYPE(self)->tp_base->tp_traverse((PyObject*)self, visit, arg);
}

static int table_meta_clear(PyObject * self){
    return Py_TYPE(self)->tp_base->tp_clear((PyObject*)self);
}

static void table_meta_dealloc(PyObject* self){
    Py_TYPE(self)->tp_base->tp_dealloc((PyObject*)self);
}

static PyObject* TableMixin_table_name(PyObject* self, void* closure){
    TableMetaObject* meta = ((TableMetaObject*)Py_TYPE(self));
    if (meta->abstract_table)
        Py_RETURN_NONE;
    return PyUnicode_FromString(meta->table_name);
};

static PyObject* TableMixin_abstract_table(PyObject* self, void* closure){
    return PyBool_FromLong(((TableMetaObject*)Py_TYPE(self))->abstract_table);
}

static PyGetSetDef TableMixin_getset[] = {
    {"__table_name__", (getter)TableMixin_table_name, NULL, "Table name", NULL},
    {"__abstract_table__", (getter)TableMixin_abstract_table, NULL, "flag for checking if given ORM Table is abstract", NULL},
    {NULL},
};

static PyTypeObject TableMetaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._testcapi.TableMeta",
    .tp_basicsize = sizeof(TableMetaObject),
    .tp_itemsize = 0,
    .tp_vectorcall_offset = offsetof(PyTypeObject, tp_vectorcall),
    .tp_call = PyVectorcall_Call,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_TYPE_SUBCLASS | Py_TPFLAGS_HAVE_GC | _Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_BASETYPE,
    .tp_new = TableMeta_New,
    .tp_traverse = (traverseproc)table_meta_traverse,
    .tp_clear = (inquiry)table_meta_clear,
    .tp_dealloc = (destructor)table_meta_dealloc,
};

/* This MixinType will allow ourselves gain access to our class attributes
without screwing around with Msgspec's internal types */
static PyTypeObject TableMixinType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._testcapi.TableMixin",
    .tp_basicsize = 0,
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_getset = TableMixin_getset,
};



/* module slots */

static int
module_traverse(PyObject *mod, visitproc visit, void *arg)
{
    return 0;
}

static int
module_clear(PyObject *mod)
{
    return 0;
}

static void
module_free(void *mod)
{
    (void)module_clear((PyObject *)mod);
}

/* These can get annoying to configure so I made simple macros - Vizonex */
#define MM_O(name) \
    {#name, (PyCFunction)(name), METH_O}

#define MM_NOARGS(name) \
    {#name, (PyCFunction)(name), METH_NOARGS}

#define MM_FASTCALL(name) \
    {#name, (PyCFunction)(name), METH_FASTCALL}


static PyMethodDef module_methods[] = {
    MM_O(factory_check),
    MM_O(factory_new),
    MM_O(factory_create),
    MM_NOARGS(factory_type),
    MM_O(field_check),
    MM_O(field_get_default),
    MM_O(field_get_factory),
    MM_O(field_get_name),
    MM_FASTCALL(field_new),
    MM_NOARGS(field_type),
    {NULL, NULL}
};

PyDoc_STRVAR(Table__doc__, 
"A Subclass of StructMeta meant to give an example of how msgspec could be used as an ORM Library."
);

static int
module_exec(PyObject *mod)
{
    mod_state *state = get_mod_state(mod);
    state->capi = Msgspec_Import();
    if (state->capi == NULL) {
        return -1;
    }

    TableMetaType.tp_base = state->capi->StructMeta_Type;
    TableMixinType.tp_base = state->capi->StructMixin_Type;

    if (PyType_Ready(&TableMetaType) < 0)
        return -1;

    if (PyModule_AddObjectRef(mod, "TableMeta", (PyObject*)(&TableMetaType)) < 0)
        return -1;

    if (PyType_Ready(&TableMixinType) < 0)
        return -1;

    if (PyModule_AddObjectRef(mod, "TableMixin", (PyObject*)(&TableMixinType)) < 0)
        return -1;

    PyObject* TableType = PyObject_CallFunction(
        (PyObject *)&TableMetaType, "s(O){ssss}", "Table", &TableMixinType,
        "__module__", "msgspec._testcapi", "__doc__", Table__doc__
    );
    if (TableType == NULL) return -1;
    Py_INCREF(TableType);
    if (PyModule_AddObject(mod, "Table", TableType) < 0)
        return -1;
    return 0;
}

static struct PyModuleDef_Slot module_slots[] = {
    {Py_mod_exec, module_exec},
#if PY_VERSION_HEX >= 0x030c00f0
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
#if PY_VERSION_HEX >= 0x030d00f0
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL},
};

static PyModuleDef _testcapi_module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "msgspec._testcapi",
    .m_size = sizeof(mod_state),
    .m_methods = module_methods,
    .m_slots = module_slots,
    .m_traverse = module_traverse,
    .m_clear = module_clear,
    .m_free = (freefunc)module_free,
};

PyMODINIT_FUNC
PyInit__testcapi(void)
{
    return PyModuleDef_Init(&_testcapi_module);
}
