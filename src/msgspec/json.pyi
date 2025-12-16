from collections.abc import Callable, Iterable
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
dec_hook_sig = Callable[[type, Any], Any] | None
float_hook_sig = Callable[[str], Any] | None
schema_hook_sig = Callable[[type], dict[str, Any]] | None

class Encoder:
    enc_hook: enc_hook_sig
    decimal_format: Literal["string", "number"]
    uuid_format: Literal["canonical", "hex"]
    order: Literal[None, "deterministic", "sorted"]

    def __init__(
        self,
        *,
        enc_hook: enc_hook_sig = None,
        decimal_format: Literal["string", "number"] = "string",
        uuid_format: Literal["canonical", "hex"] = "canonical",
        order: Literal[None, "deterministic", "sorted"] = None,
    ): ...
    def encode(self, obj: Any, /) -> bytes: ...
    def encode_lines(self, items: Iterable, /) -> bytes: ...
    def encode_into(
        self, obj: Any, buffer: bytearray, offset: int | None = 0, /
    ) -> None: ...

class Decoder(Generic[T]):
    type: type[T]
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
        type: type[T] = ...,
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
    def decode(self, buf: Buffer | str, /) -> T: ...
    def decode_lines(self, buf: Buffer | str, /) -> list[T]: ...

@overload
def decode(
    buf: Buffer | str,
    /,
    *,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
) -> Any: ...
@overload
def decode(
    buf: Buffer | str,
    /,
    *,
    type: type[T] = ...,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
) -> T: ...
@overload
def decode(
    buf: Buffer | str,
    /,
    *,
    type: Any = ...,
    strict: bool = True,
    dec_hook: dec_hook_sig = None,
) -> Any: ...
def encode(
    obj: Any,
    /,
    *,
    enc_hook: enc_hook_sig = None,
    order: Literal[None, "deterministic", "sorted"] = None,
) -> bytes: ...
def schema(type: Any, *, schema_hook: schema_hook_sig = None) -> dict[str, Any]: ...
def schema_components(
    types: Iterable[Any],
    *,
    schema_hook: schema_hook_sig = None,
    ref_template: str = "#/$defs/{name}",
) -> tuple[tuple[dict[str, Any], ...], dict[str, Any]]: ...
@overload
def format(buf: str, /, *, indent: int = 2) -> str: ...
@overload
def format(buf: Buffer, /, *, indent: int = 2) -> bytes: ...
