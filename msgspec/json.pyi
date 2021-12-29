from typing import Any, Optional, Callable

enc_hook_sig = Optional[Callable[[Any], Any]]

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

def encode(obj: Any, *, enc_hook: enc_hook_sig = None) -> bytes: ...
