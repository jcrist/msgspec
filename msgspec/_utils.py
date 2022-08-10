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


def get_type_hints(obj):
    return _get_type_hints(obj, include_extras=True)


def get_typeddict_hints(obj):
    hints = _get_type_hints(obj, include_extras=True)
    out = {}
    for k, v in hints.items():
        # Strip off Required/NotRequired
        if getattr(v, "__origin__", None) in (Required, NotRequired):
            v = v.__args__[0]
        out[k] = v
    return out
