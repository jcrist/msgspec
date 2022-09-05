import datetime
import enum
import re
from typing import Any, Union, Literal, Tuple, Dict

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
    get_type_hints = _get_type_hints
else:

    def get_type_hints(obj):
        return _get_type_hints(obj, include_extras=True)


def get_typeddict_hints(obj):
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

    Any schemas for (potentially) shared components are extracted and stored in a
    ``"$defs"`` field.

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
    (out,), components = schema_components(type)
    if components:
        out["$defs"] = components
    return out


def schema_components(
    *types: Any, ref_template: str = "#/$defs/{name}"
) -> Tuple[Tuple[Dict[str, Any], ...], Dict[str, Any]]:
    """Generate JSON Schemas for one or more types.

    Any schemas for (potentially) shared components are extracted and returned
    in a separate ``components`` dict.

    Parameters
    ----------
    types : Type
        One or more types to generate schemas for.
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
    """
    return SchemaBuilder(types, ref_template).run()


def normalize_default(d):
    from ._core import json_encode, json_decode

    return json_decode(json_encode(d))


def to_title(field, pat=re.compile("((?<=[a-z0-9])[A-Z]|(?!^)[A-Z](?=[a-z]))")):
    if "_" in field:
        out = field.replace("_", " ")
    elif "-" in field:
        out = field.replace("-", " ")
    else:
        out = pat.sub(r" \1", field)
    return out[0].upper() + out[1:]


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
    return type(t) is type and issubclass(t, dict) and hasattr(t, "__total__")


def is_namedtuple(t):
    return type(t) is type and issubclass(t, tuple) and hasattr(t, "_fields")


UNSET = type("Unset", (), {"__repr__": lambda s: "UNSET"})()


class SchemaBuilder:
    def __init__(self, types, ref_template):
        self.types = types
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
        if type(t) is _AnnotatedAlias:
            t = t.__origin__

        if is_enum(t):
            self.enums.add(t)
            return
        elif is_struct(t):
            if t in self.structs:
                return
            self.structs.add(t)
            self._collect_fields(t)
        elif is_typeddict(t):
            if t in self.typeddicts:
                return
            self.typeddicts.add(t)
            self._collect_fields(t)
        elif is_namedtuple(t):
            if t in self.namedtuples:
                return
            self.namedtuples.add(t)
            self._collect_fields(t)
        else:
            try:
                origin = t.__origin__
                args = t.__args__
            except AttributeError:
                return

            if origin in (dict, list, tuple, set, frozenset):
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

    def _type_to_schema(self, t, *, title=UNSET, default=UNSET, check_ref=True):
        from ._core import Raw, Ext, Meta

        if check_ref:
            if name := self.subtype_names.get(t):
                return {"$ref": self.ref_template.format(name=name)}

        if type(t) is _AnnotatedAlias:
            metadata = tuple(m for m in t.__metadata__ if type(m) is Meta)
            t = t.__origin__
        else:
            metadata = ()

        if type(t) is UnionType:
            t = Union
            args = t.__args__
        else:
            try:
                args = t.__args__
                t = t.__origin__
            except AttributeError:
                args = ()

        schema = {}
        extra_schema = {}

        if title is not UNSET:
            schema["title"] = title
        if default is not UNSET:
            schema["default"] = normalize_default(default)

        for meta in metadata:
            for attr in ["title", "description", "examples"]:
                if (value := getattr(meta, attr)) is not None:
                    schema[attr] = value
            if meta.extra_json_schema is not None:
                merge_json(extra_schema, meta.extra_json_schema)

            if t in (int, float):
                if meta.ge is not None:
                    schema["maximum"] = meta.ge
                if meta.gt is not None:
                    schema["exclusiveMaximum"] = meta.gt
                if meta.le is not None:
                    schema["minimum"] = meta.le
                if meta.lt is not None:
                    schema["exclusiveMinimum"] = meta.lt
                if meta.multiple_of is not None:
                    schema["multipleOf"] = meta.multiple_of
            elif t is str:
                if meta.pattern is not None:
                    schema["pattern"] = meta.pattern
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

        if t in (Any, Raw):
            pass
        elif t is None or t is type(None):
            schema["type"] = "null"
        elif t is bool:
            schema["type"] = "boolean"
        elif t is int:
            schema["type"] = "integer"
        elif t is float:
            schema["type"] = "float"
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
            if not args or len(args) == 2 and args[-1] is ...:
                schema["items"] = self._type_to_schema(args[0])
            else:
                schema["prefixItems"] = [self._type_to_schema(a) for a in args]
                schema["minItems"] = len(args)
                schema["maxItems"] = len(args)
                schema["items"] = False
        elif t is dict:
            schema["type"] = "object"
            if args:
                schema["additionalProperties"] = self._type_to_schema(args[1])
        elif t is Union:
            schema["anyOf"] = [self._type_to_schema(a) for a in args]
        elif t is Literal:
            schema["enum"] = sorted(args)
        elif is_enum(t):
            schema.setdefault("title", t.__name__)
            if t.__doc__:
                schema.setdefault("description", t.__doc__)
            if issubclass(t, enum.IntEnum):
                schema["enum"] = sorted(int(e) for e in t)
            else:
                schema["enum"] = sorted(e.name for e in t)
        elif is_struct(t):
            schema.setdefault("title", t.__name__)
            if t.__doc__:
                schema.setdefault("description", t.__doc__)
            hints = self._get_type_hints(t)
            n_required = len(t.__struct_fields__) - len(t.__struct_defaults__)
            fields = [
                self._type_to_schema(hints[f], default=d, title=to_title(ef))
                for f, ef, d in zip(
                    t.__struct_fields__,
                    t.__struct_encode_fields__,
                    (UNSET,) * n_required + t.__struct_defaults__,
                )
            ]
            if t.array_like:
                schema["type"] = "array"
                schema["prefixItems"] = fields
                schema["minItems"] = n_required
            else:
                schema["type"] = "object"
                schema["required"] = list(t.__struct_encode_fields__[:n_required])
                schema["properties"] = dict(zip(t.__struct_encode_fields__, fields))
        elif is_typeddict(t):
            schema.setdefault("title", t.__name__)
            if t.__doc__:
                schema.setdefault("description", t.__doc__)
            hints = self._get_type_hints(t)
            schema["type"] = "object"
            schema["properties"] = {
                field: self._type_to_schema(field_type, title=to_title(field))
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
            if t.__doc__:
                schema.setdefault("description", t.__doc__)
            hints = self._get_type_hints(t)
            fields = [
                self._type_to_schema(
                    hints.get(f, Any),
                    default=t._field_defaults.get(f, UNSET),
                    title=to_title(f),
                )
                for f in t._fields
            ]
            schema["type"] = "array"
            schema["prefixItems"] = fields
            schema["minItems"] = len(t._fields) - len(t._field_defaults)
            schema["maxItems"] = len(t._fields)
        elif t is Ext:
            raise TypeError("json-schema doesn't support msgpack Ext types")
        elif not extra_schema:
            # `t` is a custom type, an explicit schema is required
            raise TypeError(
                "Custom types currently require a schema be explicitly provided "
                "by annotating the type with `Meta(extra_json_schema=...)` - type "
                f"{t!r} is not supported"
            )

        if extra_schema:
            schema = merge_json(schema, extra_schema)

        return schema
