from ._core import (
    Field as _Field,
    Struct,
    defstruct,
    UNSET,
    Raw,
    Meta,
    to_builtins,
    from_builtins,
    MsgspecError,
    EncodeError,
    DecodeError,
    ValidationError,
)
from . import msgpack
from . import json
from . import yaml
from . import toml
from . import inspect
from . import structs


def field(*, default=UNSET, default_factory=UNSET):
    """
    Configuration for a Struct field.

    Parameters
    ----------
    default : Any, optional
        A default value to use for this field.
    default_factory : callable, optional
        A zero-argument function called to generate a new default value
        per-instance, rather than using a constant value as in ``default``.
    """
    return _Field(default=default, default_factory=default_factory)


from ._version import get_versions

__version__ = get_versions()["version"]
del get_versions
