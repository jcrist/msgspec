from ._core import (
    Struct,
    Raw,
    MsgspecError,
    EncodeError,
    DecodeError,
)
from . import msgpack
from . import json

from ._version import get_versions

__version__ = get_versions()["version"]
del get_versions
