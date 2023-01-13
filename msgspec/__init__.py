from ._core import (
    Struct,
    replace,
    defstruct,
    Raw,
    Meta,
    to_builtins,
    MsgspecError,
    EncodeError,
    DecodeError,
    ValidationError,
)
from . import msgpack
from . import json
from . import inspect

from ._version import get_versions

__version__ = get_versions()["version"]
del get_versions
