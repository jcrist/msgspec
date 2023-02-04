# type: ignore
import collections
import typing

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
    from typing_extensions import NotRequired, Required
except Exception:
    try:
        from typing import NotRequired, Required
    except Exception:
        Required = NotRequired = None


if Required is None and _AnnotatedAlias is None:
    # No extras available, so no `include_extras`
    get_type_hints = _get_type_hints
else:

    def get_type_hints(obj):
        return _get_type_hints(obj, include_extras=True)


# A mapping from a type annotation (or annotation __origin__) to the concrete
# python type that msgspec will use when decoding. Note that non-collection
# types don't strict need to be in this mapping. Common ones are added to avoid
# an unnecessary `getattr(t, "__origin__", None)` call on them.
# THIS IS PRIVATE FOR A REASON. DON'T MUCK WITH THIS.
_CONCRETE_TYPES = {
    t: t
    for t in [
        None,
        bool,
        int,
        float,
        str,
        bytes,
        bytearray,
        list,
        tuple,
        set,
        frozenset,
        dict,
    ]
}
_CONCRETE_TYPES.update(
    {
        typing.List: list,
        typing.Tuple: tuple,
        typing.Set: set,
        typing.FrozenSet: frozenset,
        typing.Dict: dict,
        typing.Collection: list,
        typing.MutableSequence: list,
        typing.Sequence: list,
        typing.MutableMapping: dict,
        typing.Mapping: dict,
        typing.MutableSet: set,
        typing.AbstractSet: set,
        collections.abc.Collection: list,
        collections.abc.MutableSequence: list,
        collections.abc.Sequence: list,
        collections.abc.MutableSet: set,
        collections.abc.Set: set,
        collections.abc.MutableMapping: dict,
        collections.abc.Mapping: dict,
    }
)


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


def get_dataclass_info(cls):
    from dataclasses import _FIELD, _FIELD_INITVAR, MISSING

    required = []
    optional = []
    defaults = []
    hints = None

    for field in cls.__dataclass_fields__.values():
        if field._field_type is not _FIELD:
            if field._field_type is _FIELD_INITVAR:
                raise TypeError("dataclasses with `InitVar` fields are not supported")
            continue
        name = field.name
        typ = field.type
        if type(typ) is str:
            if hints is None:
                hints = get_type_hints(cls)
            typ = hints[name]
        if field.default is not MISSING:
            defaults.append(field.default)
            optional.append((name, typ, False))
        elif field.default_factory is not MISSING:
            defaults.append(field.default_factory)
            optional.append((name, typ, True))
        else:
            required.append((name, typ, False))

    required.extend(optional)

    return tuple(required), tuple(defaults), hasattr(cls, "__post_init__")


def rebuild(cls, kwargs):
    """Used to unpickle Structs with keyword-only fields"""
    return cls(**kwargs)
