from ._core import (
    Struct,
    MsgspecError,
    EncodingError,
    DecodingError,
)
from . import msgpack

from ._version import get_versions

__version__ = get_versions()["version"]
del get_versions
