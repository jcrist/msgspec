import datetime
import enum
import re
import sys
from typing import Any, Union, Literal, List, Tuple, Dict, Set, FrozenSet

if sys.version_info >= (3, 9):
    from collections.abc import Iterable
else:
    from typing import Iterable

try:
    from types import UnionType
except Exception:
    UnionType = None

try:
    from typing_extensions import _AnnotatedAlias
except Exception:
    try:
        from typing import _AnnotatedAlias
    except Exception:
        _AnnotatedAlias = None

try:
    from typing_extensions import get_type_hints as _get_type_hints
except Exception:
    from typing import get_type_hints as _get_type_hints

try:
    from typing_extensions import Required, NotRequired
except Exception:
    try:
        from typing import Required, NotRequired
    except Exception:
        Required = NotRequired = None


if Required is None and _AnnotatedAlias is None:
    # No extras available, so no `include_extras`
    get_type_hints = _get_type_hints
else:

    def get_type_hints(obj):
        return _get_type_hints(obj, include_extras=True)


def get_typeddict_hints(obj):
    """Same as `get_type_hints`, but strips off Required/NotRequired"""
    hints = get_type_hints(obj)
    out = {}
    for k, v in hints.items():
        # Strip off Required/NotRequired
        if getattr(v, "__origin__", False) in (Required, NotRequired):
            v = v.__args__[0]
        out[k] = v
    return out


def schema(type: Any) -> Dict[str, Any]:
    """Generate a JSON Schema for a given type.

    Any schemas for (potentially) shared components are extracted and stored in
    a top-level ``"$defs"`` field.

    If you want to generate schemas for multiple types, or to have more control
    over the generated schema you may want to use ``schema_components`` instead.

    Parameters
    ----------
    type : Type
        The type to generate the schema for.

    Returns
    -------
    schema : dict
        The generated JSON Schema.

    See Also
    --------
    schema_components
    """
    (out,), components = schema_components((type,))
    if components:
        out["$defs"] = components
    return out


def schema_components(
    types: Iterable[Any], ref_template: str = "#/$defs/{name}"
) -> Tuple[Tuple[Dict[str, Any], ...], Dict[str, Any]]:
    """Generate JSON Schemas for one or more types.

    Any schemas for (potentially) shared components are extracted and returned
    in a separate ``components`` dict.

    Parameters
    ----------
    types : Iterable[Type]
        An iterable of one or more types to generate schemas for.
    ref_template : str, optional
        A template to use when generating ``"$ref"`` fields. This template is
        formatted with the type name as ``template.format(name=name)``. This
        can be useful if you intend to store the ``components`` mapping
        somewhere other than a top-level ``"$defs"`` field. For example, you
        might use ``ref_template="#/components/{name}"`` if generating an
        OpenAPI schema.

    Returns
    -------
    schemas : tuple[dict]
        A tuple of JSON Schemas, one for each type in ``types``.
    components : dict
        A mapping of name to schema for any shared components used by
        ``schemas``.

    See Also
    --------
    schema
    """
    return SchemaBuilder(types, ref_template).run()


def roundtrip_json(d):
    from ._core import json_encode, json_decode

    return json_decode(json_encode(d))


def merge_json(a, b):
    if b:
        a = a.copy()
        for key, b_val in b.items():
            if key in a:
                a_val = a[key]
                if isinstance(a_val, dict) and isinstance(b_val, dict):
                    a[key] = merge_json(a_val, b_val)
                elif isinstance(a_val, (list, tuple)) and isinstance(
                    b_val, (list, tuple)
                ):
                    a[key] = list(a_val) + list(b_val)
                else:
                    a[key] = b_val
            else:
                a[key] = b_val
    return a


def is_struct(t):
    from ._core import Struct

    return type(t) is type(Struct)


def is_enum(t):
    return type(t) is enum.EnumMeta


def is_typeddict(t):
    try:
        return issubclass(t, dict) and hasattr(t, "__total__")
    except TypeError:
        return False


def is_namedtuple(t):
    try:
        return issubclass(t, tuple) and hasattr(t, "_fields")
    except TypeError:
        return False


def has_nondefault_docstring(t):
    if not (doc := getattr(t, "__doc__", None)):
        return False

    if is_enum(t):
        # TODO: The startswith check was added for compatibility with the
        # Python 3.11 release candidate, but the cpython code requiring this
        # change has since been removed. This check can be removed once Python
        # 3.11 is released.
        return not (
            doc == "An enumeration."
            or doc.startswith("A collection of name/value pairs.")
        )
    elif is_namedtuple(t):
        return not (doc.startswith(f"{t.__name__}(") and doc.endswith(")"))
    return True


UNSET = type("Unset", (), {"__repr__": lambda s: "UNSET"})()


def origin_args_metadata(t):
    from ._core import Meta

    if type(t) is _AnnotatedAlias:
        metadata = tuple(m for m in t.__metadata__ if type(m) is Meta)
        t = t.__origin__
    else:
        metadata = ()

    if type(t) is UnionType:
        args = t.__args__
        t = Union
    elif t in (List, Tuple, Set, FrozenSet, Dict):
        t = t.__origin__
        args = None
    elif hasattr(t, "__origin__"):
        args = getattr(t, "__args__", None)
        t = t.__origin__
    else:
        args = None

    return t, args, metadata


class SchemaBuilder:
    def __init__(self, types, ref_template):
        self.types = tuple(types)
        self.ref_template = ref_template
        # Cache of type hints per type
        self.type_hints = {}
        # Collections of component types to extract
        self.structs = set()
        self.enums = set()
        self.typeddicts = set()
        self.namedtuples = set()

    def _get_type_hints(self, t: Any) -> dict:
        """A cached version of `get_type_hints`"""
        try:
            return self.type_hints[t]
        except KeyError:
            out = self.type_hints[t] = get_type_hints(t)
            return out

    @property
    def subtypes(self):
        return (*self.structs, *self.enums, *self.typeddicts, *self.namedtuples)

    def run(self):
        # First construct a decoder to validate the types are valid
        from ._core import JSONDecoder

        JSONDecoder(Tuple[self.types])

        for t in self.types:
            self._collect_type(t)

        self._init_name_map()

        schemas = [self._type_to_schema(t) for t in self.types]
        components = {
            self.subtype_names[t]: self._type_to_schema(t, check_ref=False)
            for t in self.subtypes
        }
        return schemas, components

    def _collect_type(self, t):
        t, args, metadata = origin_args_metadata(t)

        if is_enum(t):
            self.enums.add(t)
        elif is_struct(t):
            if t not in self.structs:
                self.structs.add(t)
                self._collect_fields(t)
        elif is_typeddict(t):
            if t not in self.typeddicts:
                self.typeddicts.add(t)
                self._collect_fields(t)
        elif is_namedtuple(t):
            if t not in self.namedtuples:
                self.namedtuples.add(t)
                self._collect_fields(t)
        elif t in (dict, list, tuple, set, frozenset, Union) and args:
            for a in args:
                if a is not ...:
                    self._collect_type(a)

    def _collect_fields(self, t):
        fields = self._get_type_hints(t)
        for v in fields.values():
            self._collect_type(v)

    def _init_name_map(self):
        def normalize(name):
            return re.sub(r"[^a-zA-Z0-9.\-_]", "_", name)

        def fullname(t):
            return normalize(f"{t.__module__}.{t.__qualname__}")

        conflicts = set()
        names = {}

        for t in self.subtypes:
            name = normalize(t.__name__)
            if name in names:
                old_t = names.pop(name)
                conflicts.add(name)
                names[fullname(old_t)] = old_t
            if name in conflicts:
                names[fullname(t)] = t
            else:
                names[name] = t
        self.subtype_names = {v: k for k, v in names.items()}

    def _process_metadata(self, t, metadata):
        schema = {}
        extra = {}
        for meta in metadata:
            if meta.title:
                schema["title"] = meta.title
            if meta.description:
                schema["description"] = meta.description
            if meta.examples:
                schema["examples"] = roundtrip_json(meta.examples)
            if meta.extra_json_schema is not None:
                extra = merge_json(extra, roundtrip_json(meta.extra_json_schema))
            if meta.ge is not None:
                schema["minimum"] = meta.ge
            if meta.gt is not None:
                schema["exclusiveMinimum"] = meta.gt
            if meta.le is not None:
                schema["maximum"] = meta.le
            if meta.lt is not None:
                schema["exclusiveMaximum"] = meta.lt
            if meta.multiple_of is not None:
                schema["multipleOf"] = meta.multiple_of
            if meta.pattern is not None:
                schema["pattern"] = meta.pattern
            if t is str:
                if meta.max_length is not None:
                    schema["maxLength"] = meta.max_length
                if meta.min_length is not None:
                    schema["minLength"] = meta.min_length
            elif t in (bytes, bytearray):
                if meta.max_length is not None:
                    schema["maxLength"] = 4 * ((meta.max_length + 2) // 3)
                if meta.min_length is not None:
                    schema["minLength"] = 4 * ((meta.min_length + 2) // 3)
            elif t in (list, tuple, set, frozenset):
                if meta.max_length is not None:
                    schema["maxItems"] = meta.max_length
                if meta.min_length is not None:
                    schema["minItems"] = meta.min_length
            elif t is dict:
                if meta.max_length is not None:
                    schema["maxProperties"] = meta.max_length
                if meta.min_length is not None:
                    schema["minProperties"] = meta.min_length
        return schema, extra

    def _type_to_schema(self, typ, *, default=UNSET, check_ref=True):
        from ._core import Raw, Ext

        t, args, metadata = origin_args_metadata(typ)
        schema, extra = self._process_metadata(t, metadata)

        if check_ref and (name := self.subtype_names.get(t)):
            schema["$ref"] = self.ref_template.format(name=name)
            return merge_json(schema, extra)

        if default is not UNSET:
            schema["default"] = roundtrip_json(default)

        if t in (Any, Raw):
            pass
        elif t is None or t is type(None):
            schema["type"] = "null"
        elif t is bool:
            schema["type"] = "boolean"
        elif t is int:
            schema["type"] = "integer"
        elif t is float:
            schema["type"] = "number"
        elif t is str:
            schema["type"] = "string"
        elif t in (bytes, bytearray):
            schema["type"] = "string"
            schema["contentEncoding"] = "base64"
        elif t is datetime.datetime:
            schema["type"] = "string"
            schema["format"] = "date-time"
        elif t in (list, set, frozenset):
            schema["type"] = "array"
            if args:
                schema["items"] = self._type_to_schema(args[0])
        elif t is tuple:
            schema["type"] = "array"
            if args is not None:
                # Handle an annoying compatibility issue:
                # - Tuple[()] has args == ((),)
                # - tuple[()] has args == ()
                if args == ((),):
                    args = ()
                if len(args) == 2 and args[-1] is ...:
                    schema["items"] = self._type_to_schema(args[0])
                elif args:
                    schema["prefixItems"] = [self._type_to_schema(a) for a in args]
                    schema["minItems"] = len(args)
                    schema["maxItems"] = len(args)
                    schema["items"] = False
                else:
                    schema["minItems"] = 0
                    schema["maxItems"] = 0
        elif t is dict:
            schema["type"] = "object"
            if args:
                schema["additionalProperties"] = self._type_to_schema(args[1])
        elif t is Union:
            structs = {}
            other = []
            tag_field = None
            for a in args:
                a_t, a_args, a_metadata = origin_args_metadata(a)
                if is_struct(a_t) and not a_t.array_like:
                    tag_field = a_t.__struct_tag_field__
                    structs[a_t.__struct_tag__] = a
                else:
                    other.append(a)

            options = [self._type_to_schema(a) for a in other]

            if len(structs) >= 2:
                mapping = {
                    k: self.ref_template.format(name=self.subtype_names[v])
                    for k, v in structs.items()
                }
                struct_schema = {
                    "anyOf": [self._type_to_schema(s) for s in structs.values()],
                    "discriminator": {"propertyName": tag_field, "mapping": mapping},
                }
                if options:
                    options.append(struct_schema)
                    schema["anyOf"] = options
                else:
                    schema.update(struct_schema)
            else:
                if len(structs) == 1:
                    options.append(self._type_to_schema(*structs.values()))
                schema["anyOf"] = options
        elif t is Literal:
            schema["enum"] = sorted(args)
        elif is_enum(t):
            schema.setdefault("title", t.__name__)
            if has_nondefault_docstring(t):
                schema.setdefault("description", t.__doc__)
            if issubclass(t, enum.IntEnum):
                schema["enum"] = sorted(int(e) for e in t)
            else:
                schema["enum"] = sorted(e.name for e in t)
        elif is_struct(t):
            schema.setdefault("title", t.__name__)
            if has_nondefault_docstring(t):
                schema.setdefault("description", t.__doc__)
            hints = self._get_type_hints(t)
            required = []
            names = []
            fields = []

            if t.__struct_tag_field__:
                required.append(t.__struct_tag_field__)
                names.append(t.__struct_tag_field__)
                fields.append({"enum": [t.__struct_tag__]})

            n_required = len(t.__struct_fields__) - len(t.__struct_defaults__)
            required.extend(t.__struct_encode_fields__[:n_required])
            names.extend(t.__struct_encode_fields__)
            fields.extend(
                self._type_to_schema(hints[f], default=d)
                for f, ef, d in zip(
                    t.__struct_fields__,
                    t.__struct_encode_fields__,
                    (UNSET,) * n_required + t.__struct_defaults__,
                )
            )
            if t.array_like:
                schema["type"] = "array"
                schema["prefixItems"] = fields
                schema["minItems"] = len(required)
            else:
                schema["type"] = "object"
                schema["properties"] = dict(zip(names, fields))
                schema["required"] = required
        elif is_typeddict(t):
            schema.setdefault("title", t.__name__)
            if has_nondefault_docstring(t):
                schema.setdefault("description", t.__doc__)
            hints = self._get_type_hints(t)
            schema["type"] = "object"
            schema["properties"] = {
                field: self._type_to_schema(field_type)
                for field, field_type in hints.items()
            }
            if hasattr(t, "__required_keys__"):
                required = sorted(t.__required_keys__)
            elif t.__total__:
                required = sorted(hints)
            else:
                required = []
            schema["required"] = required
        elif is_namedtuple(t):
            schema.setdefault("title", t.__name__)
            if has_nondefault_docstring(t):
                schema.setdefault("description", t.__doc__)
            hints = self._get_type_hints(t)
            fields = [
                self._type_to_schema(
                    hints.get(f, Any), default=t._field_defaults.get(f, UNSET)
                )
                for f in t._fields
            ]
            schema["type"] = "array"
            schema["prefixItems"] = fields
            schema["minItems"] = len(t._fields) - len(t._field_defaults)
            schema["maxItems"] = len(t._fields)
        elif t is Ext:
            raise TypeError("json-schema doesn't support msgpack Ext types")
        elif not extra:
            # `t` is a custom type, an explicit schema is required
            raise TypeError(
                "Custom types currently require a schema be explicitly provided "
                "by annotating the type with `Meta(extra_json_schema=...)` - type "
                f"{t!r} is not supported"
            )

        if extra:
            schema = merge_json(schema, extra)

        return schema
