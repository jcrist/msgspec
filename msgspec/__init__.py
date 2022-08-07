from ._core import (
    Struct,
    defstruct,
    Raw,
    Meta,
    MsgspecError,
    EncodeError,
    DecodeError,
    ValidationError,
)
from . import msgpack
from . import json

from ._version import get_versions

__version__ = get_versions()["version"]
del get_versions
