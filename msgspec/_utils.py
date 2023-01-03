# type: ignore
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


def get_dataclass_info(cls):
    from dataclasses import MISSING, _FIELD_INITVAR, _FIELD

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
