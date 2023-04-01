from ._core import (
    DecodeError,
    EncodeError,
    Field as _Field,
    Meta,
    MsgspecError,
    Raw,
    Struct,
    UnsetType,
    UNSET,
    NODEFAULT,
    ValidationError,
    defstruct,
    from_builtins,
    to_builtins,
)


def field(*, default=NODEFAULT, default_factory=NODEFAULT, name=None):
    """
    Configuration for a Struct field.

    Parameters
    ----------
    default : Any, optional
        A default value to use for this field.
    default_factory : callable, optional
        A zero-argument function called to generate a new default value
        per-instance, rather than using a constant value as in ``default``.
    name : str, optional
        The name to use when encoding/decoding this field. If present, this
        will override any struct-level configuration using the ``rename``
        option for this field.
    """
    return _Field(default=default, default_factory=default_factory, name=name)


from . import msgpack
from . import json
from . import yaml
from . import toml
from . import inspect
from . import structs
from ._version import get_versions

__version__ = get_versions()["version"]
del get_versions
