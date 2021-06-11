# fmt: off
import pickle
from typing import List, Any
import msgspec


def check_Encoder_encode() -> None:
    enc = msgspec.Encoder()
    b = enc.encode([1, 2, 3])

    reveal_type(b)  # assert "bytes" in typ


def check_encode() -> None:
    b = msgspec.encode([1, 2, 3])

    reveal_type(b)  # assert "bytes" in typ


def check_Decoder_decode_any() -> None:
    dec = msgspec.Decoder()
    b = msgspec.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and "Any" in typ
    reveal_type(o)  # assert "Any" in typ


def check_Decoder_decode_typed() -> None:
    dec = msgspec.Decoder(List[int])
    b = msgspec.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and ("List" in typ or "list" in typ) and "int" in typ
    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_Decoder_decode_type_comment() -> None:
    dec = msgspec.Decoder()  # type: msgspec.Decoder[List[int]]
    b = msgspec.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and ("List" in typ or "list" in typ) and "int" in typ
    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_decode_any() -> None:
    b = msgspec.encode([1, 2, 3])
    o = msgspec.decode(b)

    reveal_type(o)  # assert "Any" in typ


def check_decode_typed() -> None:
    b = msgspec.encode([1, 2, 3])
    o = msgspec.decode(b, type=List[int])

    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_encode_default() -> None:
    msgspec.encode(object(), default=lambda x: None)


def check_Encoder_default() -> None:
    msgspec.Encoder(default=lambda x: None)


def check_decode_ext_hook() -> None:
    def ext_hook(code: int, data: memoryview) -> Any:
        return pickle.loads(data)

    msgspec.decode(b"test", ext_hook=ext_hook)
    msgspec.Decoder(ext_hook=ext_hook)


def check_Ext() -> None:
    ext = msgspec.Ext(1, b"test")
    reveal_type(ext.code)  # assert "int" in typ
    reveal_type(ext.data)  # assert "bytes" in typ
