from typing import (
    Any,
    Callable,
    Generic,
    Optional,
    Type,
    TypeVar,
    Union,
    overload,
)

T = TypeVar("T")

enc_hook_sig = Optional[Callable[[Any], Any]]
ext_hook_sig = Optional[Callable[[int, memoryview], Any]]
dec_hook_sig = Optional[Callable[[Type, Any], Any]]

class Ext:
    code: int
    data: Union[bytes, bytearray, memoryview]
    def __init__(
        self, code: int, data: Union[bytes, bytearray, memoryview]
    ) -> None: ...

class Decoder(Generic[T]):
    type: Type[T]
    dec_hook: dec_hook_sig
    ext_hook: ext_hook_sig
    @overload
    def __init__(
        self: Decoder[Any],
        *,
        dec_hook: dec_hook_sig = None,
        ext_hook: ext_hook_sig = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Decoder[T],
        type: Type[T] = ...,
        *,
        dec_hook: dec_hook_sig = None,
        ext_hook: ext_hook_sig = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Decoder[Any],
        type: Any = ...,
        *,
        dec_hook: dec_hook_sig = None,
        ext_hook: ext_hook_sig = None,
    ) -> None: ...
    def decode(self, data: bytes) -> T: ...

class Encoder:
    enc_hook: enc_hook_sig
    write_buffer_size: int
    def __init__(
        self,
        *,
        enc_hook: enc_hook_sig = None,
        write_buffer_size: int = ...,
    ): ...
    def encode(self, obj: Any) -> bytes: ...
    def encode_into(
        self, obj: Any, buffer: bytearray, offset: Optional[int] = 0
    ) -> None: ...

@overload
def decode(
    buf: bytes,
    *,
    dec_hook: dec_hook_sig = None,
    ext_hook: ext_hook_sig = None,
) -> Any: ...
@overload
def decode(
    buf: bytes,
    *,
    type: Type[T] = ...,
    dec_hook: dec_hook_sig = None,
    ext_hook: ext_hook_sig = None,
) -> T: ...
@overload
def decode(
    buf: bytes,
    *,
    type: Any = ...,
    dec_hook: dec_hook_sig = None,
    ext_hook: ext_hook_sig = None,
) -> Any: ...
def encode(obj: Any, *, enc_hook: enc_hook_sig = None) -> bytes: ...
