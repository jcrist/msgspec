from __future__ import annotations

import re
from collections.abc import Iterable
from typing import Any

from . import inspect as mi, to_builtins

__all__ = ("schema", "schema_components")


def schema(type: Any) -> dict[str, Any]:
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
) -> tuple[tuple[dict[str, Any], ...], dict[str, Any]]:
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
    type_infos = mi.multi_type_info(types, protocol="json")

    component_types = _collect_component_types(type_infos)

    name_map = _build_name_map(component_types)

    schemas = tuple(_to_schema(t, name_map, ref_template) for t in type_infos)

    components = {
        name_map[cls]: _to_schema(t, name_map, ref_template, check_ref=False)
        for cls, t in component_types.items()
    }
    return schemas, components


def _collect_component_types(type_infos: Iterable[mi.Type]) -> dict[Any, mi.Type]:
    """Find all types in the type tree that are "nameable" and worthy of being
    extracted out into a shared top-level components mapping.

    Currently this looks for Struct, Dataclass, NamedTuple, TypedDict, and Enum
    types.
    """
    components = {}

    def collect(t):
        if isinstance(
            t, (mi.StructType, mi.TypedDictType, mi.DataclassType, mi.NamedTupleType)
        ):
            if t.cls not in components:
                components[t.cls] = t
                for f in t.fields:
                    collect(f.type)
        elif isinstance(t, mi.EnumType):
            components[t.cls] = t
        elif isinstance(t, mi.Metadata):
            collect(t.type)
        elif isinstance(t, mi.CollectionType):
            collect(t.item_type)
        elif isinstance(t, mi.TupleType):
            for st in t.item_types:
                collect(st)
        elif isinstance(t, mi.DictType):
            collect(t.key_type)
            collect(t.value_type)
        elif isinstance(t, mi.UnionType):
            for st in t.types:
                collect(st)

    for t in type_infos:
        collect(t)

    return components


def _build_name_map(component_types: dict[Any, mi.Type]) -> dict[Any, str]:
    """A mapping from nameable subcomponents to a generated name.

    The generated name is usually a normalized version of the class name. In
    the case of conflicts, the name will be expanded to also include the full
    import path.
    """

    def normalize(name):
        return re.sub(r"[^a-zA-Z0-9.\-_]", "_", name)

    def fullname(cls):
        return normalize(f"{cls.__module__}.{cls.__qualname__}")

    conflicts = set()
    names: dict[str, Any] = {}

    for cls in component_types:
        name = normalize(cls.__name__)
        if name in names:
            old = names.pop(name)
            conflicts.add(name)
            names[fullname(old)] = old
        if name in conflicts:
            names[fullname(cls)] = cls
        else:
            names[name] = cls
    return {v: k for k, v in names.items()}


def _has_nondefault_docstring(t: mi.Type) -> bool:
    """Check if a type has a user-defined docstring.

    Some types like Enum or Dataclass generate a default docstring."""
    if not (doc := getattr(t.cls, "__doc__", None)):
        return False

    if isinstance(t, mi.EnumType):
        return doc != "An enumeration."
    elif isinstance(t, (mi.NamedTupleType, mi.DataclassType)):
        return not (doc.startswith(f"{t.cls.__name__}(") and doc.endswith(")"))
    return True


def _to_schema(
    t: mi.Type, name_map: dict[Any, str], ref_template: str, check_ref: bool = True
) -> dict[str, Any]:
    """Converts a Type to a json-schema."""
    schema: dict[str, Any] = {}

    while isinstance(t, mi.Metadata):
        schema = mi._merge_json(schema, t.extra_json_schema)
        t = t.type

    if check_ref and hasattr(t, "cls"):
        if name := name_map.get(t.cls):
            schema["$ref"] = ref_template.format(name=name)
            return schema

    if isinstance(t, (mi.AnyType, mi.RawType)):
        pass
    elif isinstance(t, mi.NoneType):
        schema["type"] = "null"
    elif isinstance(t, mi.BoolType):
        schema["type"] = "boolean"
    elif isinstance(t, (mi.IntType, mi.FloatType)):
        schema["type"] = "integer" if isinstance(t, mi.IntType) else "number"
        if t.ge is not None:
            schema["minimum"] = t.ge
        if t.gt is not None:
            schema["exclusiveMinimum"] = t.gt
        if t.le is not None:
            schema["maximum"] = t.le
        if t.lt is not None:
            schema["exclusiveMaximum"] = t.lt
        if t.multiple_of is not None:
            schema["multipleOf"] = t.multiple_of
    elif isinstance(t, mi.StrType):
        schema["type"] = "string"
        if t.max_length is not None:
            schema["maxLength"] = t.max_length
        if t.min_length is not None:
            schema["minLength"] = t.min_length
        if t.pattern is not None:
            schema["pattern"] = t.pattern
    elif isinstance(t, (mi.BytesType, mi.ByteArrayType)):
        schema["type"] = "string"
        schema["contentEncoding"] = "base64"
        if t.max_length is not None:
            schema["maxLength"] = 4 * ((t.max_length + 2) // 3)
        if t.min_length is not None:
            schema["minLength"] = 4 * ((t.min_length + 2) // 3)
    elif isinstance(t, mi.DateTimeType):
        schema["type"] = "string"
        if t.tz is True:
            schema["format"] = "date-time"
    elif isinstance(t, mi.TimeType):
        schema["type"] = "string"
        if t.tz is True:
            schema["format"] = "time"
        elif t.tz is False:
            schema["format"] = "partial-time"
    elif isinstance(t, mi.DateType):
        schema["type"] = "string"
        schema["format"] = "date"
    elif isinstance(t, mi.UUIDType):
        schema["type"] = "string"
        schema["format"] = "uuid"
    elif isinstance(t, mi.DecimalType):
        schema["type"] = "string"
        schema["format"] = "decimal"
    elif isinstance(t, mi.CollectionType):
        schema["type"] = "array"
        if not isinstance(t.item_type, mi.AnyType):
            schema["items"] = _to_schema(t.item_type, name_map, ref_template)
        if t.max_length is not None:
            schema["maxItems"] = t.max_length
        if t.min_length is not None:
            schema["minItems"] = t.min_length
    elif isinstance(t, mi.TupleType):
        schema["type"] = "array"
        schema["minItems"] = schema["maxItems"] = len(t.item_types)
        if t.item_types:
            schema["prefixItems"] = [
                _to_schema(i, name_map, ref_template) for i in t.item_types
            ]
            schema["items"] = False
    elif isinstance(t, mi.DictType):
        schema["type"] = "object"
        if not isinstance(t.value_type, mi.AnyType):
            schema["additionalProperties"] = _to_schema(
                t.value_type, name_map, ref_template
            )
        if t.max_length is not None:
            schema["maxProperties"] = t.max_length
        if t.min_length is not None:
            schema["minProperties"] = t.min_length
    elif isinstance(t, mi.UnionType):
        structs = {}
        other = []
        tag_field = None
        for subtype in t.types:
            real_type = subtype
            while isinstance(real_type, mi.Metadata):
                real_type = real_type.type
            if isinstance(real_type, mi.StructType) and not real_type.array_like:
                tag_field = real_type.tag_field
                structs[subtype.tag] = real_type
            else:
                other.append(subtype)

        options = [_to_schema(a, name_map, ref_template) for a in other]

        if len(structs) >= 2:
            mapping = {
                k: ref_template.format(name=name_map[v.cls]) for k, v in structs.items()
            }
            struct_schema = {
                "anyOf": [
                    _to_schema(v, name_map, ref_template) for v in structs.values()
                ],
                "discriminator": {"propertyName": tag_field, "mapping": mapping},
            }
            if options:
                options.append(struct_schema)
                schema["anyOf"] = options
            else:
                schema.update(struct_schema)
        elif len(structs) == 1:
            _, subtype = structs.popitem()
            options.append(_to_schema(subtype, name_map, ref_template))
            schema["anyOf"] = options
        else:
            schema["anyOf"] = options
    elif isinstance(t, mi.LiteralType):
        schema["enum"] = sorted(t.values)
    elif isinstance(t, mi.EnumType):
        schema.setdefault("title", t.cls.__name__)
        if _has_nondefault_docstring(t):
            schema.setdefault("description", t.cls.__doc__)
        schema["enum"] = sorted(e.value for e in t.cls)
    elif isinstance(t, mi.StructType):
        schema.setdefault("title", t.cls.__name__)
        if _has_nondefault_docstring(t):
            schema.setdefault("description", t.cls.__doc__)
        required = []
        names = []
        fields = []

        if t.tag_field is not None:
            required.append(t.tag_field)
            names.append(t.tag_field)
            fields.append({"enum": [t.tag]})

        for field in t.fields:
            field_schema = _to_schema(field.type, name_map, ref_template)
            if field.required:
                required.append(field.encode_name)
            elif field.default is not mi.UNSET:
                field_schema["default"] = to_builtins(field.default, str_keys=True)
            elif field.default_factory in (list, dict, set, bytearray):
                field_schema["default"] = field.default_factory()
            names.append(field.encode_name)
            fields.append(field_schema)

        if t.array_like:
            n_trailing_defaults = 0
            for n_trailing_defaults, f in enumerate(reversed(t.fields)):
                if f.required:
                    break
            schema["type"] = "array"
            schema["prefixItems"] = fields
            schema["minItems"] = len(fields) - n_trailing_defaults
            if t.forbid_unknown_fields:
                schema["maxItems"] = len(fields)
        else:
            schema["type"] = "object"
            schema["properties"] = dict(zip(names, fields))
            schema["required"] = required
            if t.forbid_unknown_fields:
                schema["additionalProperties"] = False
    elif isinstance(t, (mi.TypedDictType, mi.DataclassType, mi.NamedTupleType)):
        schema.setdefault("title", t.cls.__name__)
        if _has_nondefault_docstring(t):
            schema.setdefault("description", t.cls.__doc__)
        names = []
        fields = []
        required = []
        for field in t.fields:
            field_schema = _to_schema(field.type, name_map, ref_template)
            if field.required:
                required.append(field.encode_name)
            elif field.default is not mi.UNSET:
                field_schema["default"] = to_builtins(field.default, str_keys=True)
            names.append(field.encode_name)
            fields.append(field_schema)
        if isinstance(t, mi.NamedTupleType):
            schema["type"] = "array"
            schema["prefixItems"] = fields
            schema["minItems"] = len(required)
            schema["maxItems"] = len(fields)
        else:
            schema["type"] = "object"
            schema["properties"] = dict(zip(names, fields))
            schema["required"] = required
    elif isinstance(t, mi.ExtType):
        raise TypeError("json-schema doesn't support msgpack Ext types")
    elif isinstance(t, mi.CustomType):
        if not schema:
            raise TypeError(
                "Custom types currently require a schema be explicitly provided "
                "by annotating the type with `Meta(extra_json_schema=...)` - type "
                f"{t.cls!r} is not supported"
            )
    else:
        # This should be unreachable
        raise TypeError(f"json-schema doesn't support type {t!r}")

    return schema
