from typing import Any, Type, TypeVar, Generic, Optional, Callable, Union, overload

T = TypeVar("T")

class Ext:
    code: int
    data: Union[bytes, bytearray, memoryview]
    def __init__(
        self, code: int, data: Union[bytes, bytearray, memoryview]
    ) -> None: ...

class Decoder(Generic[T]):
    @overload
    def __init__(
        self: Decoder[Any], ext_hook: Optional[Callable[[int, memoryview], Any]] = None
    ) -> None: ...
    @overload
    def __init__(
        self: Decoder[T],
        type: Type[T] = ...,
        ext_hook: Optional[Callable[[int, memoryview], Any]] = None,
    ) -> None: ...
    def decode(self, data: bytes) -> T: ...

class Encoder:
    def __init__(
        self, *, default: Callable[[Any], Any] = ..., write_buffer_size: int = ...
    ): ...
    def encode(self, obj: Any) -> bytes: ...

@overload
def decode(
    buf: bytes, ext_hook: Optional[Callable[[int, memoryview], Any]] = None
) -> Any: ...
@overload
def decode(
    buf: bytes,
    *,
    type: Type[T] = ...,
    ext_hook: Optional[Callable[[int, memoryview], Any]] = None
) -> T: ...
def encode(obj: Any, *, default: Callable[[Any], Any] = ...) -> bytes: ...
