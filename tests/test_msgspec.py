from __future__ import annotations

from typing import Dict, Set, List, Tuple, Optional, Any
import enum
import gc
import math
import pickle
import sys

import pytest

import msgspec


class FruitInt(enum.IntEnum):
    APPLE = 1
    BANANA = 2


class FruitStr(enum.Enum):
    APPLE = "apple"
    BANANA = "banana"


class Person(msgspec.Struct):
    first: str
    last: str
    age: int
    prefect: bool = False


class Node(msgspec.Struct):
    left: Optional[Node] = None
    right: Optional[Node] = None


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

    def test_encode_no_default(self):
        class Foo:
            pass

        with pytest.raises(
            TypeError, match="Encoding objects of type Foo is unsupported"
        ):
            msgspec.encode(Foo())

    def test_encode_default(self):
        unsupported = object()

        def default(x):
            assert x is unsupported
            return "hello"

        orig_refcount = sys.getrefcount(default)

        res = msgspec.encode(unsupported, default=default)
        assert msgspec.encode("hello") == res
        assert sys.getrefcount(default) == orig_refcount

    def test_encode_default_errors(self):
        def default(x):
            raise TypeError("bad")

        orig_refcount = sys.getrefcount(default)

        with pytest.raises(TypeError, match="bad"):
            msgspec.encode(object(), default=default)

        assert sys.getrefcount(default) == orig_refcount

    def test_encode_parse_arguments_errors(self):
        with pytest.raises(TypeError, match="Missing 1 required argument"):
            msgspec.encode()

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.encode(1, lambda x: None)

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.encode(1, 2, 3)

        with pytest.raises(TypeError, match="Invalid keyword argument 'bad'"):
            msgspec.encode(1, bad=1)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.encode(1, default=lambda x: None, extra="extra")


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

        with pytest.raises(TypeError, match="Invalid keyword argument 'bad'"):
            msgspec.decode(self.buf, bad=1)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.decode(self.buf, type=List[int], extra=1)


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
        a = sys.getsizeof(msgspec.Encoder(write_buffer_size=64))
        b = sys.getsizeof(msgspec.Encoder(write_buffer_size=128))
        assert b > a

    def test_encode_no_default(self):
        class Foo:
            pass

        enc = msgspec.Encoder()
        assert enc.default is None

        with pytest.raises(
            TypeError, match="Encoding objects of type Foo is unsupported"
        ):
            enc.encode(Foo())

    def test_encode_default(self):
        unsupported = object()

        def default(x):
            assert x is unsupported
            return "hello"

        orig_refcount = sys.getrefcount(default)

        enc = msgspec.Encoder(default=default)

        assert enc.default is default
        assert sys.getrefcount(enc.default) == orig_refcount + 2
        assert sys.getrefcount(default) == orig_refcount + 1

        res = enc.encode(unsupported)
        assert enc.encode("hello") == res

        del enc
        assert sys.getrefcount(default) == orig_refcount

    def test_encode_default_errors(self):
        def default(x):
            raise TypeError("bad")

        enc = msgspec.Encoder(default=default)

        with pytest.raises(TypeError, match="bad"):
            enc.encode(object())

    def test_encode_default_recurses(self):
        class Node:
            def __init__(self, a):
                self.a = a

        def default(x):
            return {"type": "Node", "a": x.a}

        enc = msgspec.Encoder(default=default)

        msg = enc.encode(Node(Node(1)))
        res = msgspec.decode(msg)
        assert res == {"type": "Node", "a": {"type": "Node", "a": 1}}

    def test_encode_default_recursion_error(self):
        enc = msgspec.Encoder(default=lambda x: x)

        with pytest.raises(RecursionError):
            enc.encode(object())


class TestDecoderMisc:
    def test_decoder_type_attribute(self):
        dec = msgspec.Decoder()
        assert dec.type is Any

        dec = msgspec.Decoder(int)
        assert dec.type is int

    @pytest.mark.parametrize("typ, typstr", [(None, "None"), (Any, "Any")])
    def test_decoder_none_any_repr(self, typ, typstr):
        dec = msgspec.Decoder(typ)
        assert repr(dec) == f"Decoder({typstr})"
        # Optionality of None/Any doesn't change things
        dec = msgspec.Decoder(Optional[typ])
        assert repr(dec) == f"Decoder({typstr})"

    @pytest.mark.parametrize(
        "typ, typstr",
        [
            (bool, "bool"),
            (int, "int"),
            (float, "float"),
            (str, "str"),
            (bytes, "bytes"),
            (bytearray, "bytearray"),
            (msgspec.ExtType, "ExtType"),
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
            (List[Optional[Dict[str, Person]]], "List[Optional[Dict[str, Person]]]"),
        ],
    )
    def test_decoder_repr(self, typ, typstr):
        dec = msgspec.Decoder(typ)
        assert repr(dec) == f"Decoder({typstr})"

        dec = msgspec.Decoder(Optional[typ])
        assert repr(dec) == f"Decoder(Optional[{typstr}])"

    def test_decoder_unsupported_type(self):
        with pytest.raises(TypeError):
            msgspec.Decoder(1)

        with pytest.raises(TypeError):
            msgspec.Decoder(slice)

    def test_decoder_validates_struct_definition_unsupported_types(self):
        """Struct definitions aren't validated until first use"""

        class Test(msgspec.Struct):
            a: slice

        with pytest.raises(TypeError):
            msgspec.Decoder(Test)


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
        with pytest.raises(msgspec.DecodingError, match="expected `list`"):
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
        with pytest.raises(msgspec.DecodingError, match="expected `set`"):
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
        with pytest.raises(msgspec.DecodingError, match="expected `tuple`"):
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
        with pytest.raises(msgspec.DecodingError, match="expected `tuple`"):
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
        with pytest.raises(msgspec.DecodingError, match="expected `dict`"):
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

        with pytest.raises(msgspec.DecodingError, match="expected `struct`"):
            dec.decode(enc.encode(1))

        with pytest.raises(
            msgspec.DecodingError,
            match=r"Error decoding `Person` field `first` \(`str`\): expected `str`, got `int`",
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
        "extra", [None, False, True, 1, 2.0, "three", b"four", [1, 2], {3: 4}]
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

    def test_struct_gc_maybe_untracked_on_decode(self):
        class Test(msgspec.Struct):
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

    def test_decoding_error_no_struct_toplevel(self):
        b = msgspec.Encoder().encode([{"a": 1}])
        dec = msgspec.Decoder(List[Dict[str, str]])
        with pytest.raises(
            msgspec.DecodingError,
            match=r"Error decoding `List\[Dict\[str, str\]\]`: expected `str`, got `int`",
        ):
            dec.decode(b)


class TestExtType:
    @pytest.mark.parametrize("data", [b"test", bytearray(b"test")])
    def test_init(self, data):
        x = msgspec.ExtType(1, data)
        assert x.code == 1
        assert x.data == data

    @pytest.mark.parametrize("code", [-129, 128, 2 ** 65])
    def test_code_out_of_range(self, code):
        with pytest.raises(ValueError):
            msgspec.ExtType(code, b"bad")

    def test_data_wrong_type(self):
        with pytest.raises(TypeError):
            msgspec.ExtType(1, 2)

    def test_code_wrong_type(self):
        with pytest.raises(TypeError):
            msgspec.ExtType(b"bad", b"bad")

    def test_immutable(self):
        x = msgspec.ExtType(1, b"two")
        with pytest.raises(AttributeError):
            x.code = 2

    def test_pickleable(self):
        x = msgspec.ExtType(1, b"two")
        x2 = pickle.loads(pickle.dumps(x))
        assert x2.code == 1
        assert x2.data == b"two"

    @pytest.mark.parametrize("size", sorted({0, 1, 2, 4, 8, 16, *SIZES}))
    def test_serialize_compatibility(self, size):
        msgpack = pytest.importorskip("msgpack")
        data = b"x" * size
        code = 5

        msgspec_bytes = msgspec.encode(msgspec.ExtType(code, data))
        msgpack_bytes = msgpack.dumps(msgpack.ExtType(code, data))
        assert msgspec_bytes == msgpack_bytes

    def test_serialize_bytearray(self):
        a = msgspec.encode(msgspec.ExtType(1, b"test"))
        b = msgspec.encode(msgspec.ExtType(1, bytearray(b"test")))
        assert a == b


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
