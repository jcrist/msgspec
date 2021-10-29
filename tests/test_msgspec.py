from __future__ import annotations

from typing import Dict, Set, List, Tuple, Optional, Any, Union, NamedTuple, Deque
import datetime
import enum
import gc
import itertools
import math
import os
import pickle
import struct
import sys

import pytest

import msgspec


class FruitInt(enum.IntEnum):
    APPLE = 1
    BANANA = 2


class FruitStr(enum.Enum):
    APPLE = "apple"
    BANANA = "banana"


class VeggieInt(enum.IntEnum):
    CARROT = 1
    LETTUCE = 2


class VeggieStr(enum.Enum):
    CARROT = "carrot"
    LETTUCE = "banana"


class Person(msgspec.Struct):
    first: str
    last: str
    age: int
    prefect: bool = False


class PersonAA(msgspec.Struct, asarray=True):
    first: str
    last: str
    age: int
    prefect: bool = False


PERSON = Person("harry", "potter", 13)
PERSON_AA = PersonAA("harry", "potter", 13)


class Node(msgspec.Struct):
    left: Optional[Node] = None
    right: Optional[Node] = None


class Point(NamedTuple):
    x: float
    y: float


INTS = [
    -(2 ** 63),
    -(2 ** 31 + 1),
    -(2 ** 31),
    -(2 ** 15 + 1),
    -(2 ** 15),
    -(2 ** 7 + 1),
    -(2 ** 7),
    -(2 ** 5 + 1),
    -(2 ** 5),
    -1,
    0,
    1,
    2 ** 7 - 1,
    2 ** 7,
    2 ** 8 - 1,
    2 ** 8,
    2 ** 16 - 1,
    2 ** 16,
    2 ** 32 - 1,
    2 ** 32,
    2 ** 64 - 1,
]

FLOATS = [
    -1.5,
    0.0,
    1.5,
    -float("inf"),
    float("inf"),
    float("nan"),
    sys.float_info.max,
    sys.float_info.min,
    -sys.float_info.max,
    -sys.float_info.min,
]

SIZES = [0, 1, 31, 32, 2 ** 8 - 1, 2 ** 8, 2 ** 16 - 1, 2 ** 16]


def assert_eq(x, y):
    if isinstance(x, float) and math.isnan(x):
        assert math.isnan(y)
    else:
        assert x == y


class TestEncodeFunction:
    def test_encode(self):
        dec = msgspec.Decoder()
        assert dec.decode(msgspec.encode(1)) == 1

    def test_encode_error(self):
        with pytest.raises(TypeError):
            msgspec.encode(object())

    def test_encode_large_object(self):
        """Check that buffer resize works"""
        data = b"x" * 4097
        dec = msgspec.Decoder()
        assert dec.decode(msgspec.encode(data)) == data

    def test_encode_no_enc_hook(self):
        class Foo:
            pass

        with pytest.raises(
            TypeError, match="Encoding objects of type Foo is unsupported"
        ):
            msgspec.encode(Foo())

    def test_encode_enc_hook(self):
        unsupported = object()

        def enc_hook(x):
            assert x is unsupported
            return "hello"

        orig_refcount = sys.getrefcount(enc_hook)

        res = msgspec.encode(unsupported, enc_hook=enc_hook)
        assert msgspec.encode("hello") == res
        assert sys.getrefcount(enc_hook) == orig_refcount

    def test_encode_enc_hook_errors(self):
        def enc_hook(x):
            raise TypeError("bad")

        orig_refcount = sys.getrefcount(enc_hook)

        with pytest.raises(TypeError, match="bad"):
            msgspec.encode(object(), enc_hook=enc_hook)

        assert sys.getrefcount(enc_hook) == orig_refcount

    def test_encode_parse_arguments_errors(self):
        with pytest.raises(TypeError, match="Missing 1 required argument"):
            msgspec.encode()

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.encode(1, lambda x: None)

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.encode(1, 2, 3)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.encode(1, bad=1)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.encode(1, enc_hook=lambda x: None, extra="extra")


class TestDecodeFunction:
    def setup(self):
        self.buf = msgspec.encode([1, 2, 3])

    def test_decode(self):
        assert msgspec.decode(self.buf) == [1, 2, 3]

    def test_decode_type_keyword(self):
        assert msgspec.decode(self.buf, type=List[int]) == [1, 2, 3]

        with pytest.raises(msgspec.DecodingError):
            assert msgspec.decode(self.buf, type=List[str])

    def test_decode_type_any(self):
        assert msgspec.decode(self.buf, type=Any) == [1, 2, 3]

    def test_decode_invalid_type(self):
        with pytest.raises(TypeError, match="Type '1' is not supported"):
            msgspec.decode(self.buf, type=1)

    def test_decode_invalid_buf(self):
        with pytest.raises(TypeError):
            msgspec.decode(1)

    def test_decode_parse_arguments_errors(self):
        with pytest.raises(TypeError, match="Missing 1 required argument"):
            msgspec.decode()

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.decode(self.buf, List[int])

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.decode(self.buf, 2, 3)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.decode(self.buf, bad=1)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.decode(self.buf, type=List[int], extra=1)

    def test_decode_dec_hook(self):
        def dec_hook(typ, obj):
            assert typ is Point
            return typ(*obj)

        buf = msgspec.encode((1, 2))
        res = msgspec.decode(buf, type=Point, dec_hook=dec_hook)
        assert res == Point(1, 2)
        assert isinstance(res, Point)


class TestEncoderMisc:
    @pytest.mark.parametrize("x", [-(2 ** 63) - 1, 2 ** 64])
    def test_encode_integer_limits(self, x):
        enc = msgspec.Encoder()
        with pytest.raises(OverflowError):
            enc.encode(x)

    def rec_obj1(self):
        o = []
        o.append(o)
        return o

    def rec_obj2(self):
        o = ([],)
        o[0].append(o)
        return o

    def rec_obj3(self):
        o = {}
        o["a"] = o
        return o

    def rec_obj4(self):
        class Box(msgspec.Struct):
            a: "Box"

        o = Box(None)
        o.a = o
        return o

    @pytest.mark.parametrize("case", [1, 2, 3, 4])
    def test_encode_infinite_recursive_object_errors(self, case):
        enc = msgspec.Encoder()
        o = getattr(self, "rec_obj%d" % case)()
        with pytest.raises(RecursionError):
            enc.encode(o)

    def test_getsizeof(self):
        enc1 = msgspec.Encoder(write_buffer_size=64)
        enc2 = msgspec.Encoder(write_buffer_size=128)
        assert sys.getsizeof(enc1) == sys.getsizeof(enc2)  # no buffer allocated yet
        enc1.encode(None)
        enc2.encode(None)
        assert sys.getsizeof(enc1) < sys.getsizeof(enc2)

    def test_write_buffer_size_attribute(self):
        enc1 = msgspec.Encoder(write_buffer_size=64)
        enc2 = msgspec.Encoder(write_buffer_size=128)
        enc3 = msgspec.Encoder(write_buffer_size=1)
        assert enc1.write_buffer_size == 64
        assert enc2.write_buffer_size == 128
        assert enc3.write_buffer_size == 32

    def test_encode_no_enc_hook(self):
        class Foo:
            pass

        enc = msgspec.Encoder()
        assert enc.enc_hook is None

        with pytest.raises(
            TypeError, match="Encoding objects of type Foo is unsupported"
        ):
            enc.encode(Foo())

    def test_encode_enc_hook(self):
        unsupported = object()

        def enc_hook(x):
            assert x is unsupported
            return "hello"

        orig_refcount = sys.getrefcount(enc_hook)

        enc = msgspec.Encoder(enc_hook=enc_hook)

        assert enc.enc_hook is enc_hook
        assert sys.getrefcount(enc.enc_hook) == orig_refcount + 2
        assert sys.getrefcount(enc_hook) == orig_refcount + 1

        res = enc.encode(unsupported)
        assert enc.encode("hello") == res

        del enc
        assert sys.getrefcount(enc_hook) == orig_refcount

    def test_encode_enc_hook_errors(self):
        def enc_hook(x):
            raise TypeError("bad")

        enc = msgspec.Encoder(enc_hook=enc_hook)

        with pytest.raises(TypeError, match="bad"):
            enc.encode(object())

    def test_encode_enc_hook_recurses(self):
        class Node:
            def __init__(self, a):
                self.a = a

        def enc_hook(x):
            return {"type": "Node", "a": x.a}

        enc = msgspec.Encoder(enc_hook=enc_hook)

        msg = enc.encode(Node(Node(1)))
        res = msgspec.decode(msg)
        assert res == {"type": "Node", "a": {"type": "Node", "a": 1}}

    def test_encode_enc_hook_recursion_error(self):
        enc = msgspec.Encoder(enc_hook=lambda x: x)

        with pytest.raises(RecursionError):
            enc.encode(object())

    def test_encode_into_bad_arguments(self):
        enc = msgspec.Encoder()

        with pytest.raises(TypeError, match="bytearray"):
            enc.encode_into(1, b"test")

        with pytest.raises(TypeError):
            enc.encode_into(1, bytearray(), "bad")

        with pytest.raises(ValueError, match="offset"):
            enc.encode_into(1, bytearray(), -2)

    @pytest.mark.parametrize("buf_size", [0, 1, 16, 55, 60])
    def test_encode_into(self, buf_size):
        enc = msgspec.Encoder()

        msg = {"key": "x" * 48}
        encoded = msgspec.encode(msg)

        buf = bytearray(buf_size)
        out = enc.encode_into(msg, buf)
        assert out is None
        assert buf == encoded

    def test_encode_into_offset(self):
        enc = msgspec.Encoder()
        msg = {"key": "value"}
        encoded = enc.encode(msg)

        # Offset 0 is default
        buf = bytearray()
        enc.encode_into(msg, buf, 0)
        assert buf == encoded

        # Offset in bounds uses the provided offset
        buf = bytearray(b"01234")
        enc.encode_into(msg, buf, 2)
        assert buf == b"01" + encoded

        # Offset out of bounds appends to end
        buf = bytearray(b"01234")
        enc.encode_into(msg, buf, 1000)
        assert buf == b"01234" + encoded

        # Offset -1 means append at end
        buf = bytearray(b"01234")
        enc.encode_into(msg, buf, -1)
        assert buf == b"01234" + encoded

    def tesT_encode_into_handles_errors_properly(self):
        enc = msgspec.Encoder()
        out1 = enc.encode([1, 2, 3])

        msg = [1, 2, object()]
        buf = bytearray()
        with pytest.raises(TypeError):
            enc.encode_into(msg, buf)

        assert buf  # buffer isn't reset upon error

        # Encoder still works
        out2 = enc.encode([1, 2, 3])
        assert out1 == out2


class TestDecoderMisc:
    def test_decoder_type_attribute(self):
        dec = msgspec.Decoder()
        assert dec.type is Any

        dec = msgspec.Decoder(int)
        assert dec.type is int

    def test_decoder_ext_hook_attribute(self):
        def ext_hook(code, buf):
            pass

        dec = msgspec.Decoder()
        assert dec.ext_hook is None

        dec = msgspec.Decoder(ext_hook=None)
        assert dec.ext_hook is None

        dec = msgspec.Decoder(ext_hook=ext_hook)
        assert dec.ext_hook is ext_hook

    def test_decoder_ext_hook_not_callable(self):
        with pytest.raises(TypeError):
            msgspec.Decoder(ext_hook=1)

    def test_decoder_dec_hook_attribute(self):
        def dec_hook(typ, obj):
            pass

        dec = msgspec.Decoder()
        assert dec.dec_hook is None

        dec = msgspec.Decoder(dec_hook=None)
        assert dec.dec_hook is None

        dec = msgspec.Decoder(dec_hook=dec_hook)
        assert dec.dec_hook is dec_hook

    def test_decoder_dec_hook_not_callable(self):
        with pytest.raises(TypeError):
            msgspec.Decoder(dec_hook=1)

    def test_decoder_dec_hook(self):
        called = False

        def dec_hook(typ, obj):
            nonlocal called
            called = True
            assert typ is Point
            return Point(*obj)

        dec = msgspec.Decoder(type=List[Point], dec_hook=dec_hook)
        buf = msgspec.encode([(1, 2), (3, 4), (5, 6)])
        msg = dec.decode(buf)
        assert called
        assert msg == [Point(1, 2), Point(3, 4), Point(5, 6)]
        assert isinstance(msg[0], Point)

    def test_decode_dec_hook_errors(self):
        def dec_hook(typ, obj):
            assert obj == "some string"
            raise TypeError("Oh no!")

        buf = msgspec.encode("some string")
        dec = msgspec.Decoder(type=Point, dec_hook=dec_hook)

        with pytest.raises(TypeError, match="Oh no!"):
            dec.decode(buf)

    def test_decode_dec_hook_wrong_type(self):
        dec = msgspec.Decoder(type=Point, dec_hook=lambda t, o: o)
        buf = msgspec.encode((1, 2))

        with pytest.raises(
            msgspec.DecodingError,
            match="Error decoding `Point`: expected `Point`, got `list`",
        ):
            dec.decode(buf)

    def test_decode_dec_hook_wrong_type_in_struct(self):
        class Test(msgspec.Struct):
            point: Point
            other: int

        dec = msgspec.Decoder(type=Test, dec_hook=lambda t, o: o)
        buf = msgspec.encode(Test((1, 2), 3))

        with pytest.raises(msgspec.DecodingError) as rec:
            dec.decode(buf)

        assert (
            "Error decoding `Test` field `point` (`Point`): expected `Point`, "
            "got `list`"
        ) == str(rec.value)

    def test_decode_dec_hook_wrong_type_generic(self):
        dec = msgspec.Decoder(type=Deque[int], dec_hook=lambda t, o: o)
        buf = msgspec.encode([1, 2, 3])

        with pytest.raises(msgspec.DecodingError) as rec:
            dec.decode(buf)

        assert (
            "Error decoding `typing.Deque[int]`: expected "
            "`collections.deque`, got `list`"
        ) == str(rec.value)

    def test_decode_dec_hook_isinstance_errors(self):
        class Metaclass(type):
            def __instancecheck__(self, obj):
                raise TypeError("Oh no!")

        class Custom(metaclass=Metaclass):
            pass

        dec = msgspec.Decoder(type=Custom)
        buf = msgspec.encode(1)

        with pytest.raises(TypeError, match="Oh no!"):
            dec.decode(buf)

    @pytest.mark.parametrize("typ, typstr", [(None, "None"), (Any, "Any")])
    def test_decoder_none_any_repr(self, typ, typstr):
        dec = msgspec.Decoder(typ)
        assert repr(dec) == f"Decoder({typstr})"
        # Optionality of None/Any doesn't change things
        dec = msgspec.Decoder(Optional[typ])
        assert repr(dec) == f"Decoder({typstr})"

    def test_decoder_union_with_any_is_just_any(self):
        dec = msgspec.Decoder(Union[int, float, str, Any, Node, None])
        assert repr(dec) == "Decoder(Any)"

    @pytest.mark.parametrize(
        "typ, typstr",
        [
            (bool, "bool"),
            (int, "int"),
            (float, "float"),
            (str, "str"),
            (bytes, "bytes"),
            (bytearray, "bytearray"),
            (datetime.datetime, "datetime"),
            (msgspec.Ext, "Ext"),
            (Dict, "Dict[Any, Any]"),
            (Dict[int, str], "Dict[int, str]"),
            (List, "List[Any]"),
            (List[Optional[int]], "List[Optional[int]]"),
            (Set, "Set[Any]"),
            (Set[Optional[int]], "Set[Optional[int]]"),
            (Tuple, "Tuple[Any, ...]"),
            (Tuple[Optional[int], ...], "Tuple[Optional[int], ...]"),
            (Tuple[int, str], "Tuple[int, str]"),
            (Person, "Person"),
            (FruitInt, "FruitInt"),
            (FruitStr, "FruitStr"),
            (Deque, "typing.Deque"),
            (Deque[int], str(Deque[int])),
            (List[Optional[Dict[str, Person]]], "List[Optional[Dict[str, Person]]]"),
            (Union[int, float], "Union[int, float]"),
            (Union[Person, str, int], "Union[int, str, Person]"),
        ],
    )
    def test_decoder_repr(self, typ, typstr):
        dec = msgspec.Decoder(typ)
        assert repr(dec) == f"Decoder({typstr})"

        if "Union" not in typstr:
            dec = msgspec.Decoder(Optional[typ])
            assert repr(dec) == f"Decoder(Optional[{typstr}])"

    def test_decoder_unsupported_type(self):
        with pytest.raises(TypeError):
            msgspec.Decoder(1)

    def test_decoder_validates_struct_definition_unsupported_types(self):
        """Struct definitions aren't validated until first use"""

        class Test(msgspec.Struct):
            a: 1

        with pytest.raises(TypeError):
            msgspec.Decoder(Test)

    @pytest.mark.parametrize("typ", [Union[int, Deque], Union[Deque, int]])
    def test_err_union_with_custom_type(self, typ):
        with pytest.raises(TypeError) as rec:
            msgspec.Decoder(typ)
        assert "custom type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("typ", [Union[dict, Person], Union[Person, dict]])
    def test_err_union_with_struct_and_dict(self, typ):
        with pytest.raises(TypeError) as rec:
            msgspec.Decoder(typ)
        assert "both a Struct type and a dict type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("typ", [Union[PersonAA, list], Union[tuple, PersonAA]])
    def test_err_union_with_struct_asarray_and_array(self, typ):
        with pytest.raises(TypeError) as rec:
            msgspec.Decoder(typ)
        assert "asarray=True" in str(rec.value)
        assert "Type unions containing" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("typ", [Union[FruitInt, int], Union[int, FruitInt]])
    def test_err_union_with_intenum_and_int(self, typ):
        with pytest.raises(TypeError) as rec:
            msgspec.Decoder(typ)
        assert "int and an IntEnum" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("typ", [Union[FruitStr, str], Union[str, FruitStr]])
    def test_err_union_with_enum_and_str(self, typ):
        with pytest.raises(TypeError) as rec:
            msgspec.Decoder(typ)
        assert "str and an Enum" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "typ,kind",
        [
            (Union[Person, PersonAA], "Struct"),
            (Union[Person, Node], "Struct"),
            (Union[FruitInt, VeggieInt], "IntEnum"),
            (Union[FruitStr, VeggieStr], "Enum"),
            (Union[Dict[int, float], dict], "dict"),
            (Union[List[int], List[float]], "array-like"),
            (Union[List[int], tuple], "array-like"),
            (Union[set, tuple], "array-like"),
            (Union[Tuple[int, ...], list], "array-like"),
            (Union[Tuple[int, float, str], set], "array-like"),
            (Union[Deque, int, Point], "custom"),
        ],
    )
    def test_err_union_conflicts(self, typ, kind):
        with pytest.raises(TypeError) as rec:
            msgspec.Decoder(typ)
        assert f"more than one {kind}" in str(rec.value)
        assert repr(typ) in str(rec.value)


class TestTypedDecoder:
    def check_unexpected_type(self, dec_type, val, msg):
        dec = msgspec.Decoder(dec_type)
        s = msgspec.Encoder().encode(val)
        with pytest.raises(msgspec.DecodingError, match=msg):
            dec.decode(s)

    def test_none(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(None)
        assert dec.decode(enc.encode(None)) is None
        with pytest.raises(msgspec.DecodingError, match="expected `None`"):
            assert dec.decode(enc.encode(1))

    @pytest.mark.parametrize("x", [False, True])
    def test_bool(self, x):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(bool)
        assert dec.decode(enc.encode(x)) is x

    def test_bool_unexpected_type(self):
        self.check_unexpected_type(bool, "a", "expected `bool`")

    @pytest.mark.parametrize("x", INTS)
    def test_int(self, x):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(int)
        assert dec.decode(enc.encode(x)) == x

    def test_int_unexpected_type(self):
        self.check_unexpected_type(int, "a", "expected `int`")

    @pytest.mark.parametrize("x", FLOATS + INTS)
    def test_float(self, x):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(float)
        res = dec.decode(enc.encode(x))
        sol = float(x)
        if math.isnan(sol):
            assert math.isnan(res)
        else:
            assert res == sol

    def test_float_unexpected_type(self):
        self.check_unexpected_type(float, "a", "expected `float`")

    def test_decode_float4(self):
        x = 1.2
        packed = struct.pack(">f", x)
        # Loss of resolution in float32 leads to some rounding error
        x4 = struct.unpack(">f", packed)[0]
        msg = b"\xca" + packed
        assert msgspec.decode(msg) == x4
        assert msgspec.decode(msg, type=float) == x4

    @pytest.mark.parametrize("size", SIZES)
    def test_str(self, size):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(str)
        x = "a" * size
        res = dec.decode(enc.encode(x))
        assert res == x

    def test_str_unexpected_type(self):
        self.check_unexpected_type(str, 1, "expected `str`")

    @pytest.mark.parametrize("size", SIZES)
    def test_bytes(self, size):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(bytes)
        x = b"a" * size
        res = dec.decode(enc.encode(x))
        assert isinstance(res, bytes)
        assert res == x

    def test_bytes_unexpected_type(self):
        self.check_unexpected_type(bytes, 1, "expected `bytes`")

    @pytest.mark.parametrize("size", SIZES)
    def test_bytearray(self, size):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(bytearray)
        x = bytearray(size)
        res = dec.decode(enc.encode(x))
        assert isinstance(res, bytearray)
        assert res == x

    def test_bytearray_unexpected_type(self):
        self.check_unexpected_type(bytearray, 1, "expected `bytearray`")

    def test_datetime(self):
        dec = msgspec.Decoder(datetime.datetime)
        x = datetime.datetime.now()
        res = dec.decode(msgspec.encode(x))
        assert x == res

    def test_datetime_unexpected_type(self):
        self.check_unexpected_type(datetime.datetime, 1, "expected `datetime`")
        self.check_unexpected_type(
            datetime.datetime, msgspec.Ext(1, b"test"), "expected `datetime`"
        )

    @pytest.mark.parametrize("size", SIZES)
    def test_list_lengths(self, size):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(list)
        x = list(range(size))
        res = dec.decode(enc.encode(x))
        assert res == x

    @pytest.mark.parametrize("typ", [list, List, List[Any]])
    def test_list_any(self, typ):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(typ)
        x = [1, "two", b"three"]
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match=r"expected `List\[Any\]`"):
            dec.decode(enc.encode(1))

    def test_list_typed(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(List[int])
        x = [1, 2, 3]
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="expected `int`"):
            dec.decode(enc.encode([1, 2, "three"]))

    @pytest.mark.parametrize("size", SIZES)
    def test_set_lengths(self, size):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(set)
        x = set(range(size))
        res = dec.decode(enc.encode(x))
        assert res == x

    @pytest.mark.parametrize("typ", [set, Set, Set[Any]])
    def test_set_any(self, typ):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(typ)
        x = {1, "two", b"three"}
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match=r"expected `Set\[Any\]`"):
            dec.decode(enc.encode(1))

    def test_set_typed(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Set[int])
        x = {1, 2, 3}
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="expected `int`"):
            dec.decode(enc.encode({1, 2, "three"}))

    @pytest.mark.parametrize("size", SIZES)
    def test_vartuple_lengths(self, size):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(tuple)
        x = tuple(range(size))
        res = dec.decode(enc.encode(x))
        assert res == x

    @pytest.mark.parametrize("typ", [tuple, Tuple, Tuple[Any, ...]])
    def test_vartuple_any(self, typ):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(typ)
        x = (1, "two", b"three")
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(
            msgspec.DecodingError, match=r"expected `Tuple\[Any, ...\]`"
        ):
            dec.decode(enc.encode(1))

    def test_vartuple_typed(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Tuple[int, ...])
        x = (1, 2, 3)
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="expected `int`"):
            dec.decode(enc.encode((1, 2, "three")))

    def test_fixtuple_any(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Tuple[Any, Any, Any])
        x = (1, "two", b"three")
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(
            msgspec.DecodingError, match=r"expected `Tuple\[Any, Any, Any\]`"
        ):
            dec.decode(enc.encode(1))
        with pytest.raises(
            msgspec.DecodingError,
            match=r"Error decoding `Tuple\[Any, Any, Any\]`: expected tuple of length 3, got 2",
        ):
            dec.decode(enc.encode((1, 2)))

    def test_fixtuple_typed(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Tuple[int, str, bytes])
        x = (1, "two", b"three")
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="expected `bytes`"):
            dec.decode(enc.encode((1, "two", "three")))
        with pytest.raises(
            msgspec.DecodingError,
            match=r"Error decoding `Tuple\[int, str, bytes\]`: expected tuple of length 3, got 2",
        ):
            dec.decode(enc.encode((1, 2)))

    @pytest.mark.parametrize("size", SIZES)
    def test_dict_lengths(self, size):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(dict)
        x = {i: i for i in range(size)}
        res = dec.decode(enc.encode(x))
        assert res == x

    @pytest.mark.parametrize("typ", [dict, Dict, Dict[Any, Any]])
    def test_dict_any_any(self, typ):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(typ)
        x = {1: "one", "two": 2, b"three": 3.0}
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match=r"expected `Dict\[Any, Any\]`"):
            dec.decode(enc.encode(1))

    def test_dict_any_val(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Dict[str, Any])
        x = {"a": 1, "b": "two", "c": b"three"}
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="expected `str`"):
            dec.decode(enc.encode({1: 2}))

    def test_dict_any_key(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Dict[Any, str])
        x = {1: "a", "two": "b", b"three": "c"}
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="expected `str`"):
            dec.decode(enc.encode({1: 2}))

    def test_dict_typed(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Dict[str, int])
        x = {"a": 1, "b": 2}
        res = dec.decode(enc.encode(x))
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="expected `str`"):
            dec.decode(enc.encode({1: 2}))
        with pytest.raises(msgspec.DecodingError, match="expected `int`"):
            dec.decode(enc.encode({"a": "two"}))

    def test_enum(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(FruitStr)

        a = enc.encode(FruitStr.APPLE)
        assert enc.encode("APPLE") == a
        assert dec.decode(a) == FruitStr.APPLE

        with pytest.raises(msgspec.DecodingError, match="truncated"):
            dec.decode(a[:-2])
        with pytest.raises(
            msgspec.DecodingError, match="Error decoding enum `FruitStr`"
        ):
            dec.decode(enc.encode("MISSING"))
        with pytest.raises(msgspec.DecodingError):
            dec.decode(enc.encode(1))

    def test_int_enum(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(FruitInt)

        a = enc.encode(FruitInt.APPLE)
        assert enc.encode(1) == a
        assert dec.decode(a) == FruitInt.APPLE

        with pytest.raises(msgspec.DecodingError, match="truncated"):
            dec.decode(a[:-2])
        with pytest.raises(
            msgspec.DecodingError, match="Error decoding enum `FruitInt`"
        ):
            dec.decode(enc.encode(1000))
        with pytest.raises(msgspec.DecodingError):
            dec.decode(enc.encode("INVALID"))

    def test_struct(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Person)

        x = Person(first="harry", last="potter", age=13)
        a = enc.encode(x)
        assert (
            enc.encode(
                {"first": "harry", "last": "potter", "age": 13, "prefect": False}
            )
            == a
        )
        assert dec.decode(a) == x

        with pytest.raises(msgspec.DecodingError, match="truncated"):
            dec.decode(a[:-2])

        with pytest.raises(
            msgspec.DecodingError,
            match="Error decoding `Person`: expected `Person`, got `int`",
        ):
            dec.decode(enc.encode(1))

        with pytest.raises(
            msgspec.DecodingError,
            match="Error decoding `Person`: expected `str`, got `int`",
        ):
            dec.decode(enc.encode({1: "harry"}))

    def test_struct_field_wrong_type(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Person)

        bad = enc.encode({"first": "harry", "last": "potter", "age": "thirteen"})
        with pytest.raises(msgspec.DecodingError, match="expected `int`"):
            dec.decode(bad)

    def test_struct_missing_fields(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Person)

        bad = enc.encode({"first": "harry", "last": "potter"})
        with pytest.raises(msgspec.DecodingError, match="missing required field `age`"):
            dec.decode(bad)

        bad = enc.encode({})
        with pytest.raises(
            msgspec.DecodingError, match="missing required field `first`"
        ):
            dec.decode(bad)

    @pytest.mark.parametrize(
        "extra",
        [
            None,
            False,
            True,
            1,
            2.0,
            "three",
            b"four",
            [1, 2],
            {3: 4},
            msgspec.Ext(1, b"12345"),
            msgspec.Ext(1, b""),
        ],
    )
    def test_struct_ignore_extra_fields(self, extra):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Person)

        a = enc.encode(
            {
                "extra1": extra,
                "first": "harry",
                "extra2": extra,
                "last": "potter",
                "age": 13,
                "extra3": extra,
            }
        )
        res = dec.decode(a)
        assert res == Person("harry", "potter", 13)

    def test_struct_defaults_missing_fields(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Person)

        a = enc.encode({"first": "harry", "last": "potter", "age": 13})
        res = dec.decode(a)
        assert res == Person("harry", "potter", 13)
        assert res.prefect is False

    def test_struct_asarray(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(PersonAA)

        x = PersonAA(first="harry", last="potter", age=13)
        a = enc.encode(x)
        assert enc.encode(("harry", "potter", 13, False)) == a
        assert dec.decode(a) == x

        with pytest.raises(msgspec.DecodingError, match="truncated"):
            dec.decode(a[:-2])

        with pytest.raises(
            msgspec.DecodingError,
            match="Error decoding `PersonAA`: expected `PersonAA`, got `int`",
        ):
            dec.decode(enc.encode(1))

        # Wrong field type
        bad = enc.encode(("harry", "potter", "thirteen"))
        with pytest.raises(msgspec.DecodingError, match="expected `int`"):
            dec.decode(bad)

        # Missing fields
        bad = enc.encode(("harry", "potter"))
        with pytest.raises(msgspec.DecodingError, match="missing required field `age`"):
            dec.decode(bad)

        bad = enc.encode(())
        with pytest.raises(
            msgspec.DecodingError, match="missing required field `first`"
        ):
            dec.decode(bad)

        # Extra fields ignored
        dec2 = msgspec.Decoder(List[PersonAA])
        msg = enc.encode(
            [
                ("harry", "potter", 13, False, 1, 2, 3, 4),
                ("ron", "weasley", 13, False, 5, 6),
            ]
        )
        res = dec2.decode(msg)
        assert res == [PersonAA("harry", "potter", 13), PersonAA("ron", "weasley", 13)]

        # Defaults applied
        res = dec.decode(enc.encode(("harry", "potter", 13)))
        assert res == PersonAA("harry", "potter", 13)
        assert res.prefect is False

    def test_struct_only_asarray_structs_can_decode_from_array(self):
        array_msg = msgspec.encode(("harry", "potter", 13))
        map_msg = msgspec.encode({"first": "harry", "last": "potter", "age": 13})
        sol = Person("harry", "potter", 13)
        array_sol = PersonAA("harry", "potter", 13)

        dec = msgspec.Decoder(Person)
        array_dec = msgspec.Decoder(PersonAA)

        assert array_dec.decode(array_msg) == array_sol
        assert dec.decode(map_msg) == sol
        with pytest.raises(
            msgspec.DecodingError,
            match="Error decoding `Person`: expected `Person`, got `list`",
        ):
            dec.decode(array_msg)
        with pytest.raises(
            msgspec.DecodingError,
            match="Error decoding `PersonAA`: expected `PersonAA`, got `dict`",
        ):
            array_dec.decode(map_msg)

    @pytest.mark.parametrize("asarray", [False, True])
    def test_struct_gc_maybe_untracked_on_decode(self, asarray):
        class Test(msgspec.Struct, asarray=asarray):
            x: Any
            y: Any
            z: Tuple = ()

        enc = msgspec.Encoder()
        dec = msgspec.Decoder(List[Test])

        ts = [
            Test(1, 2),
            Test(3, "hello"),
            Test([], []),
            Test({}, {}),
            Test(None, None, ()),
        ]
        a, b, c, d, e = dec.decode(enc.encode(ts))
        assert not gc.is_tracked(a)
        assert not gc.is_tracked(b)
        assert gc.is_tracked(c)
        assert gc.is_tracked(d)
        assert not gc.is_tracked(e)

    def test_struct_recursive_definition(self):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Node)

        x = Node(Node(Node(), Node(Node())))
        s = enc.encode(x)
        res = dec.decode(s)
        assert res == x

    @pytest.mark.parametrize(
        "typ, value",
        [
            (bool, False),
            (bool, True),
            (int, 1),
            (float, 2.5),
            (str, "a"),
            (bytes, b"a"),
            (bytearray, bytearray(b"a")),
            (FruitInt, FruitInt.APPLE),
            (FruitStr, FruitStr.APPLE),
            (Person, Person("harry", "potter", 13)),
            (list, [1]),
            (set, {1}),
            (tuple, (1, 2)),
            (Tuple[int, int], (1, 2)),
            (dict, {1: 2}),
            (datetime.datetime, datetime.datetime.now()),
        ],
    )
    def test_optional(self, typ, value):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(Optional[typ])

        s = enc.encode(value)
        s2 = enc.encode(None)
        assert dec.decode(s) == value
        assert dec.decode(s2) is None

        dec = msgspec.Decoder(typ)
        with pytest.raises(msgspec.DecodingError):
            dec.decode(s2)

    @pytest.mark.parametrize(
        "typ, value",
        [
            (List[Optional[int]], [1, None]),
            (Tuple[Optional[int], int], (None, 1)),
            (Set[Optional[int]], {1, None}),
            (Dict[str, Optional[int]], {"a": 1, "b": None}),
            (Dict[Optional[str], int], {"a": 1, None: 2}),
        ],
    )
    def test_optional_nested(self, typ, value):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder(typ)

        s = enc.encode(value)
        assert dec.decode(s) == value

    @pytest.mark.parametrize(
        "types, vals",
        [
            ([int, float], [1, 2.5]),
            (
                [datetime.datetime, msgspec.Ext, int, str],
                [datetime.datetime.now(), msgspec.Ext(1, b"two"), 1, "two"],
            ),
            ([str, bytearray], ["three", bytearray(b"four")]),
            ([bool, None, float, str], [True, None, 1.5, "test"]),
        ],
    )
    def test_union(self, types, vals):
        dec = msgspec.Decoder(List[Union[tuple(types)]])
        s = msgspec.encode(vals)
        res = dec.decode(s)
        assert res == vals
        for t, v in zip(types, res):
            if t is not None:
                t = getattr(t, "__origin__", t)
                assert type(v) == t

    @pytest.mark.parametrize(
        "types, vals",
        [
            (
                [PersonAA, FruitInt, FruitStr, Dict[int, str]],
                [PERSON_AA, FruitInt.APPLE, FruitStr.BANANA, {1: "two"}],
            ),
            (
                [Person, FruitInt, FruitStr, Tuple[int, ...]],
                [PERSON, FruitInt.APPLE, FruitStr.BANANA, (1, 2, 3)],
            ),
            (
                [Person, FruitInt, FruitStr, List[int]],
                [PERSON, FruitInt.APPLE, FruitStr.BANANA, [1, 2, 3]],
            ),
            (
                [Person, FruitInt, FruitStr, Set[int]],
                [PERSON, FruitInt.APPLE, FruitStr.BANANA, {1, 2, 3}],
            ),
            (
                [Person, FruitInt, FruitStr, Tuple[int, str, float]],
                [PERSON, FruitInt.APPLE, FruitStr.BANANA, (1, "two", 3.5)],
            ),
            (
                [Dict[int, str], FruitInt, FruitStr, Tuple[int, ...]],
                [{1: "two"}, FruitInt.APPLE, FruitStr.BANANA, (1, 2, 3)],
            ),
            (
                [Dict[int, str], FruitInt, FruitStr, List[int]],
                [{1: "two"}, FruitInt.APPLE, FruitStr.BANANA, [1, 2, 3]],
            ),
            (
                [Dict[int, str], FruitInt, FruitStr, Set[int]],
                [{1: "two"}, FruitInt.APPLE, FruitStr.BANANA, {1, 2, 3}],
            ),
            (
                [Dict[int, str], FruitInt, FruitStr, Tuple[int, str, float]],
                [{1: "two"}, FruitInt.APPLE, FruitStr.BANANA, (1, "two", 3.5)],
            ),
        ],
    )
    def test_compound_type_unions(self, types, vals):
        typ_vals = list(zip(types, vals))

        for N in range(2, len(typ_vals)):
            for typ_vals_subset in itertools.combinations(typ_vals, N):
                types, vals = zip(*typ_vals_subset)
                vals = list(vals)
                dec = msgspec.Decoder(List[Union[types]])
                s = msgspec.encode(vals)
                res = dec.decode(s)
                assert res == vals
                for t, v in zip(types, res):
                    t = getattr(t, "__origin__", t)
                    assert type(v) == t

    def test_union_error(self):
        msg = msgspec.encode(1)
        with pytest.raises(
            msgspec.DecodingError,
            match=r"Error decoding `Union\[bool, str\]`: expected `Union\[bool, str\]`, got `int`",
        ):
            msgspec.decode(msg, type=Union[bool, str])

    def test_decoding_error_no_struct_toplevel(self):
        b = msgspec.Encoder().encode([{"a": 1}])
        dec = msgspec.Decoder(List[Dict[str, str]])
        with pytest.raises(
            msgspec.DecodingError,
            match=r"Error decoding `List\[Dict\[str, str\]\]`: expected `str`, got `int`",
        ):
            dec.decode(b)


class TestExt:
    @pytest.mark.parametrize("data", [b"test", bytearray(b"test"), memoryview(b"test")])
    def test_init(self, data):
        x = msgspec.Ext(1, data)
        assert x.code == 1
        assert x.data == data

    def test_compare(self):
        x = msgspec.Ext(1, b"two")
        x2 = msgspec.Ext(1, b"two")
        x3 = msgspec.Ext(1, b"three")
        x4 = msgspec.Ext(2, b"two")
        assert x == x2
        assert not (x != x2)
        assert x != x3
        assert not (x == x3)
        assert x != x4
        assert not (x == x4)

    @pytest.mark.parametrize("code", [-129, 128, 2 ** 65])
    def test_code_out_of_range(self, code):
        with pytest.raises(ValueError):
            msgspec.Ext(code, b"bad")

    def test_data_wrong_type(self):
        with pytest.raises(TypeError):
            msgspec.Ext(1, 2)

    def test_code_wrong_type(self):
        with pytest.raises(TypeError):
            msgspec.Ext(b"bad", b"bad")

    def test_immutable(self):
        x = msgspec.Ext(1, b"two")
        with pytest.raises(AttributeError):
            x.code = 2

    def test_pickleable(self):
        x = msgspec.Ext(1, b"two")
        x2 = pickle.loads(pickle.dumps(x))
        assert x2.code == 1
        assert x2.data == b"two"

    @pytest.mark.parametrize("size", sorted({0, 1, 2, 4, 8, 16, *SIZES}))
    def test_serialize_compatibility(self, size):
        msgpack = pytest.importorskip("msgpack")
        data = b"x" * size
        code = 5

        msgspec_bytes = msgspec.encode(msgspec.Ext(code, data))
        msgpack_bytes = msgpack.dumps(msgpack.ExtType(code, data))
        assert msgspec_bytes == msgpack_bytes

    @pytest.mark.parametrize("typ", [bytearray, memoryview])
    def test_serialize_other_types(self, typ):
        buf = b"test"
        a = msgspec.encode(msgspec.Ext(1, buf))
        b = msgspec.encode(msgspec.Ext(1, typ(buf)))
        assert a == b

    @pytest.mark.parametrize("size", sorted({0, 1, 2, 4, 8, 16, *SIZES}))
    def test_roundtrip(self, size):
        data = b"x" * size
        code = 5

        buf = msgspec.encode(msgspec.Ext(code, data))
        out = msgspec.decode(buf)
        assert out.code == code
        assert out.data == data

    @pytest.mark.parametrize("size", sorted({0, 1, 2, 4, 8, 16, *SIZES}))
    def test_roundtrip_typed_decoder(self, size):
        dec = msgspec.Decoder(msgspec.Ext)

        ext = msgspec.Ext(5, b"x" * size)
        buf = msgspec.encode(ext)
        out = dec.decode(buf)
        assert out == ext

    def test_typed_decoder_skips_ext_hook(self):
        def ext_hook(code, data):
            assert False, "shouldn't ever get called"

        msg = [None, msgspec.Ext(1, b"test")]
        dec = msgspec.Decoder(List[Optional[msgspec.Ext]])
        buf = msgspec.encode(msg)
        out = dec.decode(buf)
        assert out == msg

    def test_ext_typed_decoder_error(self):
        dec = msgspec.Decoder(msgspec.Ext)
        with pytest.raises(msgspec.DecodingError, match="expected `Ext`"):
            assert dec.decode(msgspec.encode(1))

    @pytest.mark.parametrize("use_function", [True, False])
    def test_decoder_ext_hook(self, use_function):
        obj = {"x": range(10)}
        exp_buf = pickle.dumps(range(10))

        def enc_hook(x):
            return msgspec.Ext(5, pickle.dumps(x))

        def ext_hook(code, buf):
            assert isinstance(buf, memoryview)
            assert bytes(buf) == exp_buf
            assert code == 5
            return pickle.loads(buf)

        msg = msgspec.encode(obj, enc_hook=enc_hook)
        if use_function:
            out = msgspec.decode(msg, ext_hook=ext_hook)
        else:
            dec = msgspec.Decoder(ext_hook=ext_hook)
            out = dec.decode(msg)
        assert out == obj

    def test_decoder_ext_hook_bad_signature(self):
        msg = msgspec.encode(range(5), enc_hook=lambda x: msgspec.Ext(1, b"test"))
        with pytest.raises(TypeError):
            msgspec.decode(msg, ext_hook=lambda: None)

    def test_decoder_ext_hook_raises(self):
        class CustomError(Exception):
            pass

        def ext_hook(code, buf):
            raise CustomError

        msg = msgspec.encode(range(5), enc_hook=lambda x: msgspec.Ext(1, b"test"))
        with pytest.raises(CustomError):
            msgspec.decode(msg, ext_hook=ext_hook)


skip_windows = pytest.mark.skipif(
    os.name == "nt",
    reason="This test fails on windows due to bounds on the OS time handling routines",
)


class TestTimestampExt:
    def check(self, dt, msg):
        assert msgspec.encode(dt) == msg
        assert msgspec.decode(msg) == dt

    @skip_windows
    def test_timestamp32_lower(self):
        dt = datetime.datetime.fromtimestamp(0)
        msg = b"\xd6\xff\x00\x00\x00\x00"
        self.check(dt, msg)

    def test_timestamp32_upper(self):
        dt = datetime.datetime.fromtimestamp(2 ** 32 - 1)
        msg = b"\xd6\xff\xff\xff\xff\xff"
        self.check(dt, msg)

    @skip_windows
    def test_timestamp64_lower(self):
        dt = datetime.datetime.fromtimestamp(1e-6)
        msg = b"\xd7\xff\x00\x00\x0f\xa0\x00\x00\x00\x00"
        self.check(dt, msg)

    def test_timestamp64_upper(self):
        dt = datetime.datetime.fromtimestamp(2 ** 34) - datetime.timedelta(
            microseconds=1
        )
        msg = b"\xd7\xff\xeek\x18c\xff\xff\xff\xff"
        self.check(dt, msg)

    @skip_windows
    def test_timestamp96_lower(self):
        dt = datetime.datetime.fromtimestamp(-1e-6)
        msg = b"\xc7\x0c\xff;\x9a\xc6\x18\xff\xff\xff\xff\xff\xff\xff\xff"
        self.check(dt, msg)

    def test_timestamp96_upper(self):
        dt = datetime.datetime.fromtimestamp(2 ** 34)
        msg = b"\xc7\x0c\xff\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00"
        self.check(dt, msg)

    def test_tzinfo_kwarg(self):
        dec = msgspec.Decoder()
        assert dec.tzinfo is None

        dec = msgspec.Decoder(tzinfo=None)
        assert dec.tzinfo is None

        dec = msgspec.Decoder(tzinfo=datetime.timezone.utc)
        assert dec.tzinfo is datetime.timezone.utc

        with pytest.raises(TypeError, match="tzinfo"):
            msgspec.Decoder(tzinfo=1)

        msg = msgspec.encode(1)
        with pytest.raises(TypeError, match="tzinfo"):
            msgspec.decode(msg, tzinfo=1)

    @pytest.mark.parametrize("timestamp", [0, 0.001, 2 ** 32 - 1])
    def test_decode_with_tzinfo(self, timestamp):
        dt = datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc)

        msg = msgspec.encode(dt)

        dec = msgspec.Decoder(tzinfo=datetime.timezone.utc)

        assert dec.decode(msg) == dt
        assert msgspec.decode(msg, tzinfo=datetime.timezone.utc) == dt


class CommonTypeTestBase:
    """Test msgspec untyped encode/decode"""

    def test_none(self):
        self.check(None)

    @pytest.mark.parametrize("x", [False, True])
    def test_bool(self, x):
        self.check(x)

    @pytest.mark.parametrize("x", INTS)
    def test_int(self, x):
        self.check(x)

    @pytest.mark.parametrize("x", FLOATS)
    def test_float(self, x):
        self.check(x)

    @pytest.mark.parametrize("size", SIZES)
    def test_str(self, size):
        self.check(" " * size)

    @pytest.mark.parametrize("size", SIZES)
    def test_bytes(self, size):
        self.check(b" " * size)

    @pytest.mark.parametrize("size", SIZES)
    def test_dict(self, size):
        self.check({str(i): i for i in range(size)})

    @pytest.mark.parametrize("size", SIZES)
    def test_list(self, size):
        self.check(list(range(size)))


class TestDecodeArrayTypeUsesTupleIfHashableRequired:
    def test_decode_tuple_dict_keys_as_tuples(self):
        orig = {(1, 2): [1, 2, [3, 4]], (1, (2, 3)): [4, 5, 6]}
        data = msgspec.encode(orig)
        out = msgspec.decode(data)
        assert orig == out

    @pytest.mark.parametrize(
        "typ",
        [
            Dict[Tuple[int, Tuple[int, int]], List[int]],
            Dict[Tuple[int, Tuple[int, ...]], Any],
            Dict[Tuple, List[int]],
            Dict[Tuple[Any, ...], Any],
            Dict[Tuple[Any, Any], Any],
        ],
    )
    def test_decode_dict_key_status_forwarded_through_typed_tuples(self, typ):
        orig = {(1, (2, 3)): [1, 2, 3]}
        data = msgspec.encode(orig)
        out = msgspec.Decoder(typ).decode(data)
        assert orig == out

    def test_decode_tuple_set_keys_as_tuples(self):
        orig = {(1, 2), (3, (4, 5)), 6}
        data = msgspec.encode(orig)
        out = msgspec.decode(data, type=set)
        assert orig == out

    def test_decode_hashable_struct_in_key(self):
        class Test(msgspec.Struct):
            data: List[int]

            def __hash__(self):
                return hash(tuple(self.data))

        orig = {(1, Test([1, 2])): [1, 2]}
        data = msgspec.encode(orig)
        out = msgspec.Decoder(Dict[Tuple[int, Test], List[int]]).decode(data)
        assert orig == out


class TestUntypedDecoder(CommonTypeTestBase):
    """Check the untyped deserializer works for common types"""

    def check(self, x):
        enc = msgspec.Encoder()
        dec = msgspec.Decoder()
        assert_eq(dec.decode(enc.encode(x)), x)


class TestCompatibility(CommonTypeTestBase):
    """Test compatibility with the existing python msgpack library"""

    def check(self, x):
        msgpack = pytest.importorskip("msgpack")

        enc = msgspec.Encoder()
        dec = msgspec.Decoder()

        assert_eq(dec.decode(msgpack.dumps(x)), x)
        assert_eq(msgpack.loads(enc.encode(x)), x)
