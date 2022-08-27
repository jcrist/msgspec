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
