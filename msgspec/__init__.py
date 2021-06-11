from .core import (
    Struct,
    Encoder,
    Decoder,
    Ext,
    MsgspecError,
    EncodingError,
    DecodingError,
    encode,
    decode,
)

from ._version import get_versions

__version__ = get_versions()["version"]
del get_versions
