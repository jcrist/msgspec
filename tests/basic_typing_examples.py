# fmt: off
import datetime
import pickle
from typing import List, Any, Type
import msgspec

def check___version__() -> None:
    reveal_type(msgspec.__version__)  # assert "str" in typ


def check_exceptions() -> None:
    reveal_type(msgspec.DecodeError)  # assert "Any" not in typ
    reveal_type(msgspec.EncodeError)  # assert "Any" not in typ
    reveal_type(msgspec.MsgspecError)  # assert "Any" not in typ

##########################################################
# Structs                                                #
##########################################################

def check_struct() -> None:
    class Test(msgspec.Struct):
        x: int
        y: str

    t = Test(1, "foo")
    reveal_type(t)  # assert "Test" in typ
    reveal_type(t.x)  # assert "int" in typ
    reveal_type(t.y)  # assert "str" in typ


def check_struct_omit_defaults() -> None:
    class Test(msgspec.Struct, omit_defaults=True):
        x: int
        y: str

    t = Test(1, "foo")
    reveal_type(t)  # assert "Test" in typ
    reveal_type(t.x)  # assert "int" in typ
    reveal_type(t.y)  # assert "str" in typ


def check_struct_rename() -> None:
    class TestLower(msgspec.Struct, rename="lower"):
        x: int

    class TestUpper(msgspec.Struct, rename="upper"):
        x: int

    class TestCamel(msgspec.Struct, rename="camel"):
        x: int

    class TestPascal(msgspec.Struct, rename="pascal"):
        x: int

    class TestCallable(msgspec.Struct, rename=lambda x: x.title()):
        x: int

    class TestNone(msgspec.Struct, rename=None):
        x: int

    o = sum(
        [
            TestLower(1).x,
            TestUpper(2).x,
            TestCamel(3).x,
            TestPascal(4).x,
            TestCallable(5).x,
            TestNone(6).x,
        ]
    )

    reveal_type(o)  # assert "int" in typ


def check_struct_array_like() -> None:
    class Test(msgspec.Struct, array_like=True):
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


def check_struct_nogc() -> None:
    class Test(msgspec.Struct, nogc=True):
        x: int
        y: str

    t = Test(1, "foo")
    reveal_type(t)  # assert "Test" in typ
    reveal_type(t.x)  # assert "int" in typ
    reveal_type(t.y)  # assert "str" in typ


def check_struct_tag_tag_field() -> None:
    class Test1(msgspec.Struct, tag=None):
        pass

    class Test2(msgspec.Struct, tag=True):
        pass

    class Test3(msgspec.Struct, tag=False):
        pass

    class Test4(msgspec.Struct, tag="mytag"):
        pass

    class Test5(msgspec.Struct, tag=lambda n: n.lower()):
        pass

    class Test6(msgspec.Struct, tag_field=None):
        pass

    class Test7(msgspec.Struct, tag_field="type"):
        pass


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


def check_struct_attributes() -> None:
    class Point(msgspec.Struct):
        x: int
        y: int

    for field in Point.__struct_fields__:
        reveal_type(field)  # assert "str" in typ

    p = Point(1, 2)

    for field in p.__struct_fields__:
        reveal_type(field)  # assert "str" in typ


##########################################################
# defstruct                                              #
##########################################################


def check_defstruct() -> None:
    Test = msgspec.defstruct("Test", ["x", "y"])
    for field in Test.__struct_fields__:
        reveal_type(field)  # assert "str" in typ
    Test(1, y=2)


def check_defstruct_field_types() -> None:
    Test = msgspec.defstruct(
        "Test",
        ("x", ("y", int), ("z", str, "default"))
    )


def check_defstruct_bases() -> None:
    class Base(msgspec.Struct):
        pass

    Test = msgspec.defstruct("Test", ["x", "y"], bases=(Base,))


def check_defstruct_namespace() -> None:
    msgspec.defstruct("Test", ["x", "y"], namespace={"classval": 1})


def check_defstruct_module() -> None:
    msgspec.defstruct("Test", ["x", "y"], module="mymod")


def check_defstruct_config_options() -> None:
    Test = msgspec.defstruct(
        "Test",
        ("x", "y"),
        omit_defaults=True,
        frozen=True,
        array_like=True,
        nogc=True,
        tag="mytag",
        tag_field="mytagfield",
        rename="lower"
    )

##########################################################
# Raw                                                    #
##########################################################

def check_raw_constructor() -> None:
    r = msgspec.Raw()
    r2 = msgspec.Raw(b"test")
    r3 = msgspec.Raw(bytearray(b"test"))
    r4 = msgspec.Raw(memoryview(b"test"))


def check_raw_copy() -> None:
    r = msgspec.Raw()
    r2 = r.copy()
    reveal_type(r2)  # assert "Raw" in typ


def check_raw_methods() -> None:
    r1 = msgspec.Raw(b"a")
    r2 = msgspec.Raw(b"b")
    if r1 == r2:
        print(r1)

    m = memoryview(r1)  # buffer protocol


def check_raw_pass_to_decode() -> None:
    r = msgspec.Raw()
    res = msgspec.json.decode(r)
    res2 = msgspec.msgpack.decode(r)


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
