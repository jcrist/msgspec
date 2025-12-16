from collections.abc import Callable
from typing import (
    Any,
    Generic,
    Literal,
    TypeVar,
    overload,
)

from typing_extensions import Buffer

T = TypeVar("T")

enc_hook_sig = Callable[[Any], Any] | None
ext_hook_sig = Callable[[int, memoryview], Any] | None
dec_hook_sig = Callable[[type, Any], Any] | None

class Ext:
    code: int
    data: bytes | bytearray | memoryview
    def __init__(self, code: int, data: bytes | bytearray | memoryview) -> None: ...

class Decoder(Generic[T]):
    type: type[T]
    strict: bool
    dec_hook: dec_hook_sig
    ext_hook: ext_hook_sig
    @overload
    def __init__(
        self: Decoder[Any],
        *,
        strict: bool = True,
        dec_hook: dec_hook_sig = None,
        ext_hook: ext_hook_sig = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Decoder[T],
        type: type[T] = ...,
        *,
        strict: bool = True,
        dec_hook: dec_hook_sig = None,
        ext_hook: ext_hook_sig = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Decoder[Any],
        type: Any = ...,
        *,
        strict: bool = True,
        dec_hook: dec_hook_sig = None,
        ext_hook: ext_hook_sig = None,
    ) -> None: ...
    def decode(self, buf: Buffer, /) -> T: ...

class Encoder:
    enc_hook: enc_hook_sig
    decimal_format: Literal["string", "number"]
    uuid_format: Literal["canonical", "hex", "bytes"]
    order: Literal[None, "deterministic", "sorted"]
    def __init__(
        self,
        *,
        enc_hook: enc_hook_sig = None,
        decimal_format: Literal["string", "number"] = "string",
        uuid_format: Literal["canonical", "hex", "bytes"] = "canonical",
        order: Literal[None, "deterministic", "sorted"] = None,
    ): ...
    def encode(self, obj: Any, /) -> bytes: ...
    def encode_into(
        self, obj: Any, buffer: bytearray, offset: int | None = 0, /
    ) -> None: ...

@overload
def decode(
    buf: Buffer,
    /,
    *,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
    ext_hook: ext_hook_sig = None,
) -> Any: ...
@overload
def decode(
    buf: Buffer,
    /,
    *,
    type: type[T] = ...,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
    ext_hook: ext_hook_sig = None,
) -> T: ...
@overload
def decode(
    buf: Buffer,
    /,
    *,
    type: Any = ...,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
    ext_hook: ext_hook_sig = None,
) -> Any: ...
def encode(
    obj: Any,
    /,
    *,
    enc_hook: enc_hook_sig = None,
    order: Literal[None, "deterministic", "sorted"] = None,
) -> bytes: ...
