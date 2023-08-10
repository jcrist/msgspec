from collections.abc import Iterable
from typing import (
    Any,
    Callable,
    Dict,
    Generic,
    Iterable,
    Literal,
    Optional,
    Tuple,
    Type,
    TypeVar,
    Union,
    overload,
)

T = TypeVar("T")

enc_hook_sig = Optional[Callable[[Any], Any]]
dec_hook_sig = Optional[Callable[[type, Any], Any]]
float_hook_sig = Optional[Callable[[str], Any]]

class Encoder:
    enc_hook: enc_hook_sig
    decimal_format: Literal["string", "number"]
    uuid_format: Literal["canonical", "hex"]

    def __init__(
        self,
        *,
        enc_hook: enc_hook_sig = None,
        decimal_format: Literal["string", "number"] = "string",
        uuid_format: Literal["canonical", "hex"] = "canonical",
    ): ...
    def encode(self, obj: Any) -> bytes: ...
    def encode_lines(self, items: Iterable) -> bytes: ...
    def encode_into(
        self, obj: Any, buffer: bytearray, offset: Optional[int] = 0
    ) -> None: ...

class Decoder(Generic[T]):
    type: Type[T]
    strict: bool
    dec_hook: dec_hook_sig
    float_hook: float_hook_sig

    @overload
    def __init__(
        self: Decoder[Any],
        *,
        strict: bool = True,
        dec_hook: dec_hook_sig = None,
        float_hook: float_hook_sig = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Decoder[T],
        type: Type[T] = ...,
        *,
        strict: bool = True,
        dec_hook: dec_hook_sig = None,
        float_hook: float_hook_sig = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Decoder[Any],
        type: Any = ...,
        *,
        strict: bool = True,
        dec_hook: dec_hook_sig = None,
        float_hook: float_hook_sig = None,
    ) -> None: ...
    def decode(self, data: Union[bytes, str]) -> T: ...
    def decode_lines(self, data: Union[bytes, str]) -> list[T]: ...

@overload
def decode(
    buf: Union[bytes, str],
    *,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
) -> Any: ...
@overload
def decode(
    buf: Union[bytes, str],
    *,
    type: Type[T] = ...,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
) -> T: ...
@overload
def decode(
    buf: Union[bytes, str],
    *,
    type: Any = ...,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
) -> Any: ...
def encode(obj: Any, *, enc_hook: enc_hook_sig = None) -> bytes: ...
def schema(type: Any) -> Dict[str, Any]: ...
def schema_components(
    types: Iterable[Any], ref_template: str = "#/$defs/{name}"
) -> Tuple[Tuple[Dict[str, Any], ...], Dict[str, Any]]: ...
@overload
def format(buf: str, *, indent: int = 2) -> str: ...
@overload
def format(buf: bytes, *, indent: int = 2) -> bytes: ...
