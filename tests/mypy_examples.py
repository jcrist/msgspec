# fmt: off
import datetime
import pickle
from typing import List, Any, Type
import msgspec

##########################################################
# Structs                                                #
##########################################################

def check___version__() -> None:
    reveal_type(msgspec.__version__)  # assert "str" in typ


def check_exceptions() -> None:
    reveal_type(msgspec.DecodeError)  # assert "Any" not in typ
    reveal_type(msgspec.EncodeError)  # assert "Any" not in typ
    reveal_type(msgspec.MsgspecError)  # assert "Any" not in typ


def check_struct() -> None:
    class Test(msgspec.Struct):
        x: int
        y: str

    t = Test(1, "foo")
    reveal_type(t)  # assert "Test" in typ
    reveal_type(t.x)  # assert "int" in typ
    reveal_type(t.y)  # assert "str" in typ


def check_struct_asarray() -> None:
    class Test(msgspec.Struct, asarray=True):
        x: int
        y: str

    t = Test(1, "foo")
    reveal_type(t)  # assert "Test" in typ
    reveal_type(t.x)  # assert "int" in typ
    reveal_type(t.y)  # assert "str" in typ


def check_struct_frozen() -> None:
    class Test(msgspec.Struct, frozen=True):
        x: int
        y: str

    t = Test(1, "foo")
    reveal_type(t)  # assert "Test" in typ
    reveal_type(t.x)  # assert "int" in typ
    reveal_type(t.y)  # assert "str" in typ


def check_struct_methods() -> None:
    class Point(msgspec.Struct):
        x: int
        y: int


    a = Point(1, 2)
    b = Point(3, 4)
    if a == b:
        print("equal")
    a.x = a.x + b.y
    repr(a)


##########################################################
# MessagePack                                            #
##########################################################

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


##########################################################
# JSON                                                   #
##########################################################

def check_json_Encoder_encode() -> None:
    enc = msgspec.json.Encoder()
    b = enc.encode([1, 2, 3])

    reveal_type(b)  # assert "bytes" in typ


def check_json_Encoder_encode_into() -> None:
    enc = msgspec.json.Encoder()
    buf = bytearray(48)
    enc.encode_into([1, 2, 3], buf)
    enc.encode_into([1, 2, 3], buf, 2)


def check_json_encode() -> None:
    b = msgspec.json.encode([1, 2, 3])

    reveal_type(b)  # assert "bytes" in typ


def check_json_Decoder_decode_any() -> None:
    dec = msgspec.json.Decoder()
    b = msgspec.json.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and "Any" in typ
    reveal_type(o)  # assert "Any" in typ


def check_json_Decoder_decode_typed() -> None:
    dec = msgspec.json.Decoder(List[int])
    b = msgspec.json.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and ("List" in typ or "list" in typ) and "int" in typ
    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_json_Decoder_decode_type_comment() -> None:
    dec = msgspec.json.Decoder()  # type: msgspec.json.Decoder[List[int]]
    b = msgspec.json.encode([1, 2, 3])
    o = dec.decode(b)

    reveal_type(dec)  # assert "Decoder" in typ and ("List" in typ or "list" in typ) and "int" in typ
    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_json_decode_any() -> None:
    b = msgspec.json.encode([1, 2, 3])
    o = msgspec.json.decode(b)

    reveal_type(o)  # assert "Any" in typ


def check_json_decode_typed() -> None:
    b = msgspec.json.encode([1, 2, 3])
    o = msgspec.json.decode(b, type=List[int])

    reveal_type(o)  # assert ("List" in typ or "list" in typ) and "int" in typ


def check_json_encode_enc_hook() -> None:
    msgspec.json.encode(object(), enc_hook=lambda x: None)


def check_json_Encoder_enc_hook() -> None:
    msgspec.json.Encoder(enc_hook=lambda x: None)


def check_json_decode_dec_hook() -> None:
    def dec_hook(typ: Type, obj: Any) -> Any:
        return typ(obj)

    msgspec.json.decode(b"test", dec_hook=dec_hook)
    msgspec.json.Decoder(dec_hook=dec_hook)
