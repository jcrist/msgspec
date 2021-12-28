# fmt: off
import datetime
import pickle
from typing import List, Any, Type
import msgspec


def check_msgpack_Encoder_encode() -> None:
    enc = msgspec.msgpack.Encoder()
    b = enc.encode([1, 2, 3])

    reveal_type(b)  # assert "bytes" in typ


def check_msgpack_Encoder_encode_into() -> None:
    enc = msgspec.msgpack.Encoder()
    buf = bytearray(48)
    enc.encode_into([1, 2, 3], buf)
    enc.encode_into([1, 2, 3], buf, 2)


def check_msgpack_encode() -> None:
    b = msgspec.msgpack.encode([1, 2, 3])

    reveal_type(b)  # assert "bytes" in typ


def check_msgpack_Decoder_decode_any() -> None:
    dec = msgspec.msgpack.Decoder()
    b = msgspec.msgpack.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and "Any" in typ
    reveal_type(o)  # assert "Any" in typ


def check_msgpack_Decoder_decode_typed() -> None:
    dec = msgspec.msgpack.Decoder(List[int])
    b = msgspec.msgpack.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and ("List" in typ or "list" in typ) and "int" in typ
    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_msgpack_Decoder_decode_type_comment() -> None:
    dec = msgspec.msgpack.Decoder()  # type: msgspec.msgpack.Decoder[List[int]]
    b = msgspec.msgpack.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and ("List" in typ or "list" in typ) and "int" in typ
    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_msgpack_decode_any() -> None:
    b = msgspec.msgpack.encode([1, 2, 3])
    o = msgspec.msgpack.decode(b)

    reveal_type(o)  # assert "Any" in typ


def check_msgpack_decode_typed() -> None:
    b = msgspec.msgpack.encode([1, 2, 3])
    o = msgspec.msgpack.decode(b, type=List[int])

    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_msgpack_encode_enc_hook() -> None:
    msgspec.msgpack.encode(object(), enc_hook=lambda x: None)


def check_msgpack_Encoder_enc_hook() -> None:
    msgspec.msgpack.Encoder(enc_hook=lambda x: None)


def check_msgpack_decode_dec_hook() -> None:
    def dec_hook(typ: Type, obj: Any) -> Any:
        return typ(obj)

    msgspec.msgpack.decode(b"test", dec_hook=dec_hook)
    msgspec.msgpack.Decoder(dec_hook=dec_hook)


def check_msgpack_decode_ext_hook() -> None:
    def ext_hook(code: int, data: memoryview) -> Any:
        return pickle.loads(data)

    msgspec.msgpack.decode(b"test", ext_hook=ext_hook)
    msgspec.msgpack.Decoder(ext_hook=ext_hook)


def check_msgpack_Ext() -> None:
    ext = msgspec.msgpack.Ext(1, b"test")
    reveal_type(ext.code)  # assert "int" in typ
    reveal_type(ext.data)  # assert "bytes" in typ


def check_msgpack_tzinfo() -> None:
    dec = msgspec.msgpack.Decoder()
    reveal_type(dec.tzinfo)  # assert "tzinfo" in typ

    msgspec.msgpack.Decoder(tzinfo=None)
    msgspec.msgpack.Decoder(tzinfo=datetime.timezone.utc)
    msgspec.msgpack.decode(b"test", tzinfo=None)
    msgspec.msgpack.decode(b"test", tzinfo=datetime.timezone.utc)
