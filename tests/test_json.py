from __future__ import annotations

import base64
import datetime
import enum
import gc
import itertools
import json
import math
import random
import string
import struct
import sys
from typing import (
    Any,
    Deque,
    Dict,
    FrozenSet,
    List,
    Literal,
    NamedTuple,
    Optional,
    Set,
    Tuple,
    TypedDict,
    Union,
)

import pytest

import msgspec

from utils import temp_module


UTC = datetime.timezone.utc


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


class PersonArray(msgspec.Struct, array_like=True):
    first: str
    last: str
    age: int
    prefect: bool = False


class Node(msgspec.Struct):
    left: Optional[Node] = None
    right: Optional[Node] = None


class Custom:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __eq__(self, other):
        return self.x == other.x and self.y == other.y


class TestInvalidJSONTypes:
    def test_invalid_type_union(self):
        literal = Literal["a", "b"]
        types = [FruitStr, literal, str, datetime.datetime, bytes, bytearray]
        for length in [2, 3, 4]:
            for types in itertools.combinations(types, length):
                if set(types) in ({bytes, bytearray}, {str, literal}):
                    continue
                with pytest.raises(TypeError, match="not supported"):
                    msgspec.json.Decoder(Union[types])

    def test_invalid_type_dict_non_str_key(self):
        with pytest.raises(TypeError, match="not supported"):
            msgspec.json.Decoder(Dict[int, int])

    def test_invalid_type_in_collection(self):
        with pytest.raises(TypeError, match="not supported"):
            msgspec.json.Decoder(List[Union[int, Dict[int, int]]])

        with pytest.raises(TypeError, match="not supported"):
            msgspec.json.Decoder(List[Dict[str, Union[int, str, bytes]]])

    @pytest.mark.parametrize("preinit", [False, True])
    def test_invalid_dict_type_in_struct(self, preinit):
        class Test(msgspec.Struct):
            a: int
            b: Dict[int, str]
            c: str

        if preinit:
            # Creating a msgpack decoder pre-parses the type definition
            msgspec.msgpack.Decoder(Test)

        with pytest.raises(TypeError, match="not supported"):
            msgspec.json.Decoder(Test)

        # Msgpack decoder still works
        dec = msgspec.msgpack.Decoder(Test)
        msg = Test(1, {2: "three"}, "four")
        assert dec.decode(msgspec.msgpack.encode(msg)) == msg

    @pytest.mark.parametrize("preinit", [False, True])
    def test_invalid_union_type_in_struct(self, preinit):
        class Test(msgspec.Struct):
            a: int
            b: Union[str, bytes]
            c: str

        if preinit:
            # Creating a msgpack decoder pre-parses the type definition
            msgspec.msgpack.Decoder(Test)

        with pytest.raises(TypeError, match="not supported"):
            msgspec.json.Decoder(Test)

        # Msgpack decoder still works
        dec = msgspec.msgpack.Decoder(Test)
        msg = Test(1, "two", "three")
        assert dec.decode(msgspec.msgpack.encode(msg)) == msg

    @pytest.mark.parametrize("preinit", [False, True])
    def test_invalid_type_nested_in_struct(self, preinit):
        source = """
        from __future__ import annotations

        import msgspec
        from typing import List, Dict

        class Inner(msgspec.Struct):
            a: List[Dict[int, int]]
            b: int

        class Outer(msgspec.Struct):
            a: List[Inner]
            b: bool
        """
        with temp_module(source) as mod:
            if preinit:
                msgspec.msgpack.Decoder(mod.Outer)

            with pytest.raises(TypeError, match="not supported"):
                msgspec.json.Decoder(mod.Outer)

            dec = msgspec.msgpack.Decoder(mod.Outer)
            msg = mod.Outer([mod.Inner([{2: 3}], 1)], False)
            assert dec.decode(msgspec.msgpack.encode(msg)) == msg

    @pytest.mark.parametrize("preinit", [False, True])
    @pytest.mark.parametrize("kind", ["A", "B", "C"])
    def test_invalid_type_recursive(self, preinit, kind):
        source = """
        from __future__ import annotations

        import msgspec
        from typing import Optional, Dict

        class A(msgspec.Struct):
            x: Optional[dict]
            y: Optional[B]

        class B(msgspec.Struct):
            x: Optional[dict]
            y: Optional[C]

        class C(msgspec.Struct):
            x: Optional[Dict[int, int]]
            y: Optional[A]
        """

        with temp_module(source) as mod:
            cls = getattr(mod, kind)
            if preinit:
                msgspec.msgpack.Decoder(cls)

            with pytest.raises(TypeError, match="not supported"):
                msgspec.json.Decoder(cls)

            dec = msgspec.msgpack.Decoder(cls)
            msg = {"x": None, "y": {"x": None, "y": {"x": None, "y": None}}}
            assert isinstance(dec.decode(msgspec.msgpack.encode(msg)), cls)


class TestEncodeFunction:
    def test_encode(self):
        assert msgspec.json.encode(1) == b"1"

    def test_encode_error(self):
        with pytest.raises(TypeError):
            msgspec.json.encode(object())

    def test_encode_large_object(self):
        """Check that buffer resize works"""
        data = "x" * 4097
        assert msgspec.json.encode(data) == f'"{data}"'.encode("utf-8")

    def test_encode_no_enc_hook(self):
        class Foo:
            pass

        with pytest.raises(
            TypeError, match="Encoding objects of type Foo is unsupported"
        ):
            msgspec.json.encode(Foo())

    def test_encode_enc_hook(self):
        unsupported = object()

        def enc_hook(x):
            assert x is unsupported
            return "hello"

        orig_refcount = sys.getrefcount(enc_hook)

        res = msgspec.json.encode(unsupported, enc_hook=enc_hook)
        assert msgspec.json.encode("hello") == res
        assert sys.getrefcount(enc_hook) == orig_refcount

    def test_encode_enc_hook_errors(self):
        def enc_hook(x):
            raise TypeError("bad")

        orig_refcount = sys.getrefcount(enc_hook)

        with pytest.raises(TypeError, match="bad"):
            msgspec.json.encode(object(), enc_hook=enc_hook)

        assert sys.getrefcount(enc_hook) == orig_refcount

    def test_encode_parse_arguments_errors(self):
        with pytest.raises(TypeError, match="Missing 1 required argument"):
            msgspec.json.encode()

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.json.encode(1, lambda x: None)

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.json.encode(1, 2, 3)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.json.encode(1, bad=1)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.json.encode(1, enc_hook=lambda x: None, extra="extra")


class TestEncoderMisc:
    @pytest.mark.parametrize("x", [-(2**63) - 1, 2**64])
    def test_encode_integer_limits(self, x):
        enc = msgspec.json.Encoder()
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
        enc = msgspec.json.Encoder()
        o = getattr(self, "rec_obj%d" % case)()
        with pytest.raises(RecursionError):
            enc.encode(o)

    def test_getsizeof(self):
        enc1 = msgspec.json.Encoder(write_buffer_size=64)
        enc2 = msgspec.json.Encoder(write_buffer_size=128)
        assert sys.getsizeof(enc1) == sys.getsizeof(enc2)  # no buffer allocated yet
        enc1.encode(None)
        enc2.encode(None)
        assert sys.getsizeof(enc1) < sys.getsizeof(enc2)

    def test_write_buffer_size_attribute(self):
        enc1 = msgspec.json.Encoder(write_buffer_size=64)
        enc2 = msgspec.json.Encoder(write_buffer_size=128)
        enc3 = msgspec.json.Encoder(write_buffer_size=1)
        assert enc1.write_buffer_size == 64
        assert enc2.write_buffer_size == 128
        assert enc3.write_buffer_size == 32

    def test_encode_no_enc_hook(self):
        class Foo:
            pass

        enc = msgspec.json.Encoder()
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

        enc = msgspec.json.Encoder(enc_hook=enc_hook)

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

        enc = msgspec.json.Encoder(enc_hook=enc_hook)

        with pytest.raises(TypeError, match="bad"):
            enc.encode(object())

    def test_encode_enc_hook_recurses(self):
        class Node:
            def __init__(self, a):
                self.a = a

        def enc_hook(x):
            return {"type": "Node", "a": x.a}

        enc = msgspec.json.Encoder(enc_hook=enc_hook)

        msg = enc.encode(Node(Node(1)))
        res = json.loads(msg)
        assert res == {"type": "Node", "a": {"type": "Node", "a": 1}}

    def test_encode_enc_hook_recursion_error(self):
        enc = msgspec.json.Encoder(enc_hook=lambda x: x)

        with pytest.raises(RecursionError):
            enc.encode(object())

    def test_encode_into_bad_arguments(self):
        enc = msgspec.json.Encoder()

        with pytest.raises(TypeError, match="bytearray"):
            enc.encode_into(1, b"test")

        with pytest.raises(TypeError):
            enc.encode_into(1, bytearray(), "bad")

        with pytest.raises(ValueError, match="offset"):
            enc.encode_into(1, bytearray(), -2)

    @pytest.mark.parametrize("buf_size", [0, 1, 16, 55, 60])
    def test_encode_into(self, buf_size):
        enc = msgspec.json.Encoder()

        msg = {"key": "x" * 48}
        encoded = msgspec.json.encode(msg)

        buf = bytearray(buf_size)
        out = enc.encode_into(msg, buf)
        assert out is None
        assert buf == encoded

    def test_encode_into_offset(self):
        enc = msgspec.json.Encoder()
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

    def test_encode_into_handles_errors_properly(self):
        enc = msgspec.json.Encoder()
        out1 = enc.encode([1, 2, 3])

        msg = [1, 2, object()]
        buf = bytearray()
        with pytest.raises(TypeError):
            enc.encode_into(msg, buf)

        assert buf  # buffer isn't reset upon error

        # Encoder still works
        out2 = enc.encode([1, 2, 3])
        assert out1 == out2


class TestDecodeFunction:
    def test_decode(self):
        assert msgspec.json.decode(b"[1, 2, 3]") == [1, 2, 3]

    def test_decode_type_keyword(self):
        assert msgspec.json.decode(b"[1, 2, 3]", type=Set[int]) == {1, 2, 3}

        with pytest.raises(msgspec.ValidationError):
            assert msgspec.json.decode(b"[1, 2, 3]", type=Set[str])

    def test_decode_type_any(self):
        assert msgspec.json.decode(b"[1, 2, 3]", type=Any) == [1, 2, 3]

    @pytest.mark.parametrize("array_like", [False, True])
    def test_decode_type_struct(self, array_like):
        class Point(msgspec.Struct, array_like=array_like):
            x: int
            y: int

        msg = msgspec.json.encode(Point(1, 2))

        for _ in range(2):
            assert msgspec.json.decode(msg, type=Point) == Point(1, 2)

    def test_decode_type_struct_not_json_compatible(self):
        class Test(msgspec.Struct):
            x: Dict[int, str]

        with pytest.raises(TypeError, match="not supported"):
            msgspec.json.decode(b'{"x": {1: "two"}}', type=Test)

    def test_decode_type_struct_invalid_type(self):
        class Test(msgspec.Struct):
            x: 1

        with pytest.raises(TypeError):
            msgspec.json.decode(b"{}", type=Test)

    def test_decode_invalid_type(self):
        with pytest.raises(TypeError, match="Type '1' is not supported"):
            msgspec.json.decode(b"[]", type=1)

    def test_decode_invalid_buf(self):
        with pytest.raises(TypeError):
            msgspec.json.decode(1)

    def test_decode_parse_arguments_errors(self):
        buf = b"[1, 2, 3]"

        with pytest.raises(TypeError, match="Missing 1 required argument"):
            msgspec.json.decode()

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.json.decode(buf, List[int])

        with pytest.raises(TypeError, match="Extra positional arguments"):
            msgspec.json.decode(buf, 2, 3)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.json.decode(buf, bad=1)

        with pytest.raises(TypeError, match="Extra keyword arguments"):
            msgspec.json.decode(buf, type=List[int], extra=1)

    def test_decode_dec_hook(self):
        def dec_hook(typ, obj):
            assert typ is Custom
            return typ(*obj)

        res = msgspec.json.decode(b"[1, 2]", type=Custom, dec_hook=dec_hook)
        assert res == Custom(1, 2)
        assert isinstance(res, Custom)

    def test_decode_with_trailing_characters_errors(self):
        with pytest.raises(msgspec.DecodeError):
            msgspec.json.decode(b'[1, 2, 3]"trailing"')


class TestDecoderMisc:
    def test_decoder_type_attribute(self):
        dec = msgspec.json.Decoder()
        assert dec.type is Any

        dec = msgspec.json.Decoder(int)
        assert dec.type is int

    def test_decoder_dec_hook_attribute(self):
        def dec_hook(typ, obj):
            pass

        dec = msgspec.json.Decoder()
        assert dec.dec_hook is None

        dec = msgspec.json.Decoder(dec_hook=None)
        assert dec.dec_hook is None

        dec = msgspec.json.Decoder(dec_hook=dec_hook)
        assert dec.dec_hook is dec_hook

    def test_decoder_dec_hook_not_callable(self):
        with pytest.raises(TypeError):
            msgspec.json.Decoder(dec_hook=1)

    def test_decoder_dec_hook(self):
        called = False

        def dec_hook(typ, obj):
            nonlocal called
            called = True
            assert typ is Custom
            return Custom(*obj)

        dec = msgspec.json.Decoder(type=List[Custom], dec_hook=dec_hook)
        msg = dec.decode(b"[[1,2],[3,4],[5,6]]")
        assert called
        assert msg == [Custom(1, 2), Custom(3, 4), Custom(5, 6)]
        assert isinstance(msg[0], Custom)

    def test_decoder_dec_hook_optional_custom_type(self):
        called = False

        def dec_hook(typ, obj):
            nonlocal called
            called = True

        dec = msgspec.json.Decoder(type=Optional[Custom], dec_hook=dec_hook)
        msg = dec.decode(msgspec.json.encode(None))
        assert not called
        assert msg is None

    def test_decode_dec_hook_errors(self):
        def dec_hook(typ, obj):
            assert obj == "some string"
            raise TypeError("Oh no!")

        dec = msgspec.json.Decoder(type=Custom, dec_hook=dec_hook)

        with pytest.raises(TypeError, match="Oh no!"):
            dec.decode(b'"some string"')

    def test_decode_dec_hook_wrong_type(self):
        dec = msgspec.json.Decoder(type=Custom, dec_hook=lambda t, o: o)

        with pytest.raises(
            msgspec.ValidationError,
            match="Expected `Custom`, got `list`",
        ):
            dec.decode(b"[1, 2]")

    def test_decode_dec_hook_wrong_type_in_struct(self):
        class Test(msgspec.Struct):
            point: Custom
            other: int

        dec = msgspec.json.Decoder(type=Test, dec_hook=lambda t, o: o)

        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(b'{"point": [1, 2], "other": 3}')

        assert "Expected `Custom`, got `list` - at `$.point`" == str(rec.value)

    def test_decode_dec_hook_wrong_type_generic(self):
        dec = msgspec.json.Decoder(type=Deque[int], dec_hook=lambda t, o: o)

        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(b"[1, 2, 3]")

        assert "Expected `collections.deque`, got `list`" == str(rec.value)

    def test_decode_dec_hook_isinstance_errors(self):
        class Metaclass(type):
            def __instancecheck__(self, obj):
                raise TypeError("Oh no!")

        class Custom(metaclass=Metaclass):
            pass

        dec = msgspec.json.Decoder(type=Custom)

        with pytest.raises(TypeError, match="Oh no!"):
            dec.decode(b"1")

    def test_decoder_repr(self):
        typ = List[Dict[str, float]]
        dec = msgspec.json.Decoder(typ)
        assert repr(dec) == f"msgspec.json.Decoder({typ!r})"

        dec = msgspec.json.Decoder()
        assert repr(dec) == f"msgspec.json.Decoder({Any!r})"

    def test_decode_with_trailing_characters_errors(self):
        dec = msgspec.json.Decoder()

        with pytest.raises(msgspec.DecodeError):
            dec.decode(b'[1, 2, 3]"trailing"')


class TestBoolAndNone:
    def test_encode_none(self):
        assert msgspec.json.encode(None) == b"null"

    def test_decode_none(self):
        assert msgspec.json.decode(b"null") is None
        assert msgspec.json.decode(b"   null   ") is None

    @pytest.mark.parametrize("s", [b"nul", b"nulll", b"nuul", b"nulp"])
    def test_decode_none_malformed(self, s):
        with pytest.raises(msgspec.DecodeError):
            msgspec.json.decode(s)

    def test_decode_none_typed(self):
        with pytest.raises(
            msgspec.ValidationError, match="Expected `int | null`, got `str`"
        ):
            msgspec.json.decode(b'"test"', type=Union[int, None])

    def test_encode_true(self):
        assert msgspec.json.encode(True) == b"true"

    def test_decode_true(self):
        assert msgspec.json.decode(b"true") is True
        assert msgspec.json.decode(b"   true   ") is True

    @pytest.mark.parametrize("s", [b"tru", b"truee", b"trru", b"trup"])
    def test_decode_true_malformed(self, s):
        with pytest.raises(msgspec.DecodeError):
            msgspec.json.decode(s)

    def test_encode_false(self):
        assert msgspec.json.encode(False) == b"false"

    def test_decode_false(self):
        assert msgspec.json.decode(b"false") is False
        assert msgspec.json.decode(b"   false   ") is False

    @pytest.mark.parametrize("s", [b"fals", b"falsee", b"faase", b"falsp"])
    def test_decode_false_malformed(self, s):
        with pytest.raises(msgspec.DecodeError):
            msgspec.json.decode(s)

    def test_decode_bool_typed(self):
        with pytest.raises(msgspec.ValidationError, match="Expected `bool`, got `str`"):
            msgspec.json.decode(b'"test"', type=bool)


class TestStrings:
    STRINGS = [
        ("", b'""'),
        ("a", b'"a"'),
        (" a b c d", b'" a b c d"'),
        ("123 á 456", b'"123 \xc3\xa1 456"'),
        ("á 456", b'"\xc3\xa1 456"'),
        ("123 á", b'"123 \xc3\xa1"'),
        ("123 𝄞 456", b'"123 \xf0\x9d\x84\x9e 456"'),
        ("𝄞 456", b'"\xf0\x9d\x84\x9e 456"'),
        ("123 𝄞", b'"123 \xf0\x9d\x84\x9e"'),
        ('123 \b\n\f\r\t"\\ 456', b'"123 \\b\\n\\f\\r\\t\\"\\\\ 456"'),
        ('\b\n\f\r\t"\\ 456', b'"\\b\\n\\f\\r\\t\\"\\\\ 456"'),
        ('123 \b\n\f\r\t"\\', b'"123 \\b\\n\\f\\r\\t\\"\\\\"'),
        ("123 \x01\x02\x03 456", b'"123 \\u0001\\u0002\\u0003 456"'),
        ("\x01\x02\x03 456", b'"\\u0001\\u0002\\u0003 456"'),
        ("123 \x01\x02\x03", b'"123 \\u0001\\u0002\\u0003"'),
    ]

    @pytest.mark.parametrize("decoded, encoded", STRINGS)
    def test_encode_str(self, decoded, encoded):
        assert msgspec.json.encode(decoded) == encoded

    @pytest.mark.parametrize("decoded, encoded", STRINGS)
    def test_decode_str(self, decoded, encoded):
        assert msgspec.json.decode(encoded) == decoded

    @pytest.mark.parametrize(
        "decoded, encoded",
        [
            ("123 Á 456", b'"123 \\u00C1 456"'),
            ("Á 456", b'"\\u00C1 456"'),
            ("123 Á", b'"123 \\u00C1"'),
            ("123 𝄞 456", b'"123 \\ud834\\udd1e 456"'),
            ("𝄞 456", b'"\\ud834\\udd1e 456"'),
            ("123 𝄞", b'"123 \\ud834\\udd1e"'),
            ("123 𝄞 456", b'"123 \\uD834\\uDD1E 456"'),
        ],
    )
    def test_decode_str_unicode_escapes(self, decoded, encoded):
        assert msgspec.json.decode(encoded) == decoded

    @pytest.mark.parametrize(
        "s, error",
        [
            (b'"\\u00cz 123"', "invalid character in unicode escape"),
            (b'"\\ud834\\uddz0 123"', "invalid character in unicode escape"),
            (b'"\\ud834"', "truncated"),
            (b'"\\ud834 1234567"', "unexpected end of escaped utf-16 surrogate pair"),
            (b'"\\udc00"', "invalid utf-16 surrogate pair"),
            (b'"\\udfff"', "invalid utf-16 surrogate pair"),
            (b'"\\ud834\\udb99"', "invalid utf-16 surrogate pair"),
            (b'"\\ud834\\ue000"', "invalid utf-16 surrogate pair"),
            (b'"\\v"', "invalid escape character in string"),
        ],
    )
    def test_decode_str_malformed_escapes(self, s, error):
        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s)

    def test_decode_str_invalid_byte(self):
        with pytest.raises(msgspec.DecodeError, match="invalid character"):
            msgspec.json.decode(b'"123 \x00 456"')

        with pytest.raises(msgspec.DecodeError, match="invalid character"):
            msgspec.json.decode(b'"123 \x01 456"')

    def test_decode_str_missing_closing_quote(self):
        with pytest.raises(msgspec.DecodeError, match="truncated"):
            msgspec.json.decode(b'"test')

    @pytest.mark.parametrize("length", range(10))
    @pytest.mark.parametrize("in_list", [False, True])
    @pytest.mark.parametrize("unicode", [False, True])
    @pytest.mark.parametrize("escape", [False, True])
    def test_decode_str_lengths(self, length, in_list, unicode, escape):
        """A test designed to get full coverage of the unrolled loops in the
        string parsing routine"""
        if unicode:
            prefix = "𝄞\nÁ\t\n𝄞Á" if escape else "𝄞Á"
        else:
            prefix = "a\nb\t\ncd" if escape else ""
        s = prefix + string.ascii_letters[:length]
        sol = [s, 1] if in_list else s
        buf = msgspec.json.encode(sol)
        res = msgspec.json.decode(buf)
        assert res == sol

        left, _, right = buf.rpartition(b'"')
        buf2 = left + b'\x01"' + right
        with pytest.raises(msgspec.DecodeError, match="invalid character"):
            msgspec.json.decode(buf2)

        # Test str skipping
        class Test(msgspec.Struct):
            x: int

        buf3 = msgspec.json.encode({"y": sol, "x": 1})
        msgspec.json.decode(buf3, type=Test)


class TestBinary:
    @pytest.mark.parametrize(
        "x", [b"", b"a", b"ab", b"abc", b"abcd", b"abcde", b"abcdef"]
    )
    @pytest.mark.parametrize("type", [bytes, bytearray, memoryview])
    def test_encode_binary(self, x, type):
        x = type(x)
        s = msgspec.json.encode(x)
        expected = b'"' + base64.b64encode(x) + b'"'
        assert s == expected

    @pytest.mark.parametrize(
        "x", [b"", b"a", b"ab", b"abc", b"abcd", b"abcde", b"abcdef"]
    )
    @pytest.mark.parametrize("type", [bytes, bytearray])
    def test_decode_binary(self, x, type):
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s, type=type)
        assert x == x2
        assert isinstance(x2, type)

    @pytest.mark.parametrize(
        "s", [b'"Y"', b'"YQ"', b'"YQ="', b'"YQI"', b'"YQI=="', b'"YQJj="', b'"AB*D"']
    )
    def test_malformed_base64_encoding(self, s):
        with pytest.raises(
            msgspec.ValidationError, match="Invalid base64 encoded string"
        ):
            msgspec.json.decode(s, type=bytes)


class TestDatetime:
    def test_encode_datetime(self):
        # All fields, zero padded
        x = datetime.datetime(1, 2, 3, 4, 5, 6, 7, UTC)
        s = msgspec.json.encode(x)
        assert s == b'"0001-02-03T04:05:06.000007Z"'

        # All fields, no zeros
        x = datetime.datetime(1234, 12, 31, 14, 56, 27, 123456, UTC)
        s = msgspec.json.encode(x)
        assert s == b'"1234-12-31T14:56:27.123456Z"'

    def test_encode_datetime_no_microsecond(self):
        x = datetime.datetime(1234, 12, 31, 14, 56, 27, 0, UTC)
        s = msgspec.json.encode(x)
        assert s == b'"1234-12-31T14:56:27Z"'

    @pytest.mark.parametrize(
        "offset",
        [
            datetime.timedelta(0),
            datetime.timedelta(days=1, microseconds=-1),
            datetime.timedelta(days=-1, microseconds=1),
            datetime.timedelta(days=1, seconds=-29),
            datetime.timedelta(days=-1, seconds=29),
            datetime.timedelta(days=0, seconds=30),
            datetime.timedelta(days=0, seconds=-30),
        ],
    )
    def test_encode_datetime_offset_is_appx_equal_to_utc(self, offset):
        tz = datetime.timezone(offset)
        x = datetime.datetime(1234, 12, 31, 14, 56, 27, 123456, tz)
        s = msgspec.json.encode(x)
        assert s == b'"1234-12-31T14:56:27.123456Z"'

    @pytest.mark.parametrize(
        "offset, expected",
        [
            (
                datetime.timedelta(days=1, seconds=-30),
                b'"1234-12-31T14:56:27.123456+23:59"',
            ),
            (
                datetime.timedelta(days=-1, seconds=30),
                b'"1234-12-31T14:56:27.123456-23:59"',
            ),
            (
                datetime.timedelta(minutes=19, seconds=32, microseconds=130000),
                b'"1234-12-31T14:56:27.123456+00:20"',
            ),
        ],
    )
    def test_encode_datetime_offset_rounds_to_nearest_minute(self, offset, expected):
        tz = datetime.timezone(offset)
        x = datetime.datetime(1234, 12, 31, 14, 56, 27, 123456, tz)
        s = msgspec.json.encode(x)
        assert s == expected

    def test_encode_datetime_no_tzinfo_errors_by_default(self):
        x = datetime.datetime.now()
        with pytest.raises(
            TypeError, match="Encoding timezone-naive datetime objects is unsupported"
        ):
            msgspec.json.encode(x)

    def test_encode_datetime_no_tzinfo_hits_enc_hook(self):
        x = datetime.datetime.now()
        res = msgspec.json.encode(x.replace(tzinfo=UTC))

        def enc_hook(obj):
            if isinstance(obj, datetime.datetime):
                return obj.replace(tzinfo=UTC)
            raise TypeError(str(type(obj)))

        sol = msgspec.json.encode(x, enc_hook=enc_hook)
        assert res == sol

    @pytest.mark.parametrize(
        "dt",
        [
            "0001-02-03T04:05:06.000007",
            "0001-02-03T04:05:06.007",
            "0001-02-03T04:05:06",
            "2021-12-11T21:19:22.123456",
        ],
    )
    @pytest.mark.parametrize("suffix", ["Z", "+00:00", "-00:00"])
    def test_decode_datetime_utc(self, dt, suffix):
        dt += suffix
        exp = datetime.datetime.fromisoformat(dt.replace("Z", "+00:00"))
        s = f'"{dt}"'.encode("utf-8")
        res = msgspec.json.decode(s, type=datetime.datetime)
        assert res == exp

    @pytest.mark.parametrize(
        "dt",
        [
            "2000-12-31T12:00:01",
            "2000-01-01T00:00:01",
            "2000-01-31T12:01:01",
            "2000-02-01T12:01:01",
            "2000-02-28T12:01:01",
            "2000-02-29T12:01:01",
            "2000-03-01T12:01:01",
        ],
    )
    @pytest.mark.parametrize("sign", ["-", "+"])
    @pytest.mark.parametrize("hour", [0, 8, 12, 16, 23])
    @pytest.mark.parametrize("minute", [0, 30])
    def test_decode_datetime_with_timezone(self, dt, sign, hour, minute):
        s = f"{dt}{sign}{hour:02}:{minute:02}"
        json_s = f'"{s}"'.encode("utf-8")
        exp = datetime.datetime.fromisoformat(s)
        res = msgspec.json.decode(json_s, type=datetime.datetime)
        assert res == exp

    @pytest.mark.parametrize("t", ["T", "t"])
    @pytest.mark.parametrize("z", ["Z", "z"])
    def test_decode_datetime_not_case_sensitive(self, t, z):
        """Both T & Z can be upper/lowercase"""
        s = f'"0001-02-03{t}04:05:06.000007{z}"'.encode("utf-8")
        exp = datetime.datetime(1, 2, 3, 4, 5, 6, 7, UTC)
        res = msgspec.json.decode(s, type=datetime.datetime)
        assert res == exp

    def test_decode_min_datetime(self):
        res = msgspec.json.decode(b'"0001-01-01T00:00:00Z"', type=datetime.datetime)
        exp = datetime.datetime.min.replace(tzinfo=UTC)
        assert res == exp

    def test_decode_max_datetime(self):
        res = msgspec.json.decode(
            b'"9999-12-31T23:59:59.999999Z"', type=datetime.datetime
        )
        exp = datetime.datetime.max.replace(tzinfo=UTC)
        assert res == exp

    @pytest.mark.parametrize(
        "msg, sol",
        [
            (
                b'"2022-01-02T03:04:05.1234564Z"',
                datetime.datetime(2022, 1, 2, 3, 4, 5, 123456, UTC),
            ),
            (
                b'"2022-01-02T03:04:05.1234565Z"',
                datetime.datetime(2022, 1, 2, 3, 4, 5, 123457, UTC),
            ),
            (
                b'"2022-01-02T03:04:05.12345650000000000001Z"',
                datetime.datetime(2022, 1, 2, 3, 4, 5, 123457, UTC),
            ),
            (
                b'"2022-01-02T03:04:05.9999995Z"',
                datetime.datetime(2022, 1, 2, 3, 4, 6, 0, UTC),
            ),
            (
                b'"2022-01-02T03:04:59.9999995Z"',
                datetime.datetime(2022, 1, 2, 3, 5, 0, 0, UTC),
            ),
            (
                b'"2022-01-02T03:59:59.9999995Z"',
                datetime.datetime(2022, 1, 2, 4, 0, 0, 0, UTC),
            ),
            (
                b'"2022-01-02T23:59:59.9999995Z"',
                datetime.datetime(2022, 1, 3, 0, 0, 0, 0, UTC),
            ),
            (
                b'"2022-02-28T23:59:59.9999995Z"',
                datetime.datetime(2022, 3, 1, 0, 0, 0, 0, UTC),
            ),
        ],
    )
    def test_decode_datetime_nanos(self, msg, sol):
        res = msgspec.json.decode(msg, type=datetime.datetime)
        assert res == sol

    @pytest.mark.parametrize(
        "s",
        [
            # Incorrect field lengths
            b'"001-02-03T04:05:06.000007Z"',
            b'"0001-2-03T04:05:06.000007Z"',
            b'"0001-02-3T04:05:06.000007Z"',
            b'"0001-02-03T4:05:06.000007Z"',
            b'"0001-02-03T04:5:06.000007Z"',
            b'"0001-02-03T04:05:6.000007Z"',
            b'"0001-02-03T04:05:06.000007+0:00"',
            b'"0001-02-03T04:05:06.000007+00:0"',
            # Trailing data
            b'"0001-02-03T04:05:06.000007+00:00:00"',
            # Truncated
            b'"0001-02-03T04:05:"',
            # Missing timezone
            b'"0001-02-03T04:05:06"',
            b'"0001-02-03T04:05:06.000001"',
            b'"0001-02-03T04:05:06.00000001"',
            # Missing +/-
            b'"0001-02-03T04:05:06.00000700:00"',
            # Missing digits after decimal
            b'"0001-02-03T04:05:06.Z"',
            # Invalid characters
            b'"000a-02-03T04:05:06.000007Z"',
            b'"0001-0a-03T04:05:06.000007Z"',
            b'"0001-02-0aT04:05:06.000007Z"',
            b'"0001-02-03T0a:05:06.000007Z"',
            b'"0001-02-03T04:0a:06.000007Z"',
            b'"0001-02-03T04:05:0a.000007Z"',
            b'"0001-02-03T04:05:06.00000aZ"',
            b'"0001-02-03T04:05:06.000007a"',
            b'"0001-02-03T04:05:06.000007+0a:00"',
            b'"0001-02-03T04:05:06.000007+00:0a"',
            # Year out of range
            b'"0000-02-03T04:05:06.000007Z"',
            # Month out of range
            b'"0001-00-03T04:05:06.000007Z"',
            b'"0001-13-03T04:05:06.000007Z"',
            # Day out of range for month
            b'"0001-02-00:05:06.000007Z"',
            b'"0001-02-29:05:06.000007Z"',
            b'"2000-02-30:05:06.000007Z"',
            # Hour out of range
            b'"0001-02-03T24:05:06.000007Z"',
            # Minute out of range
            b'"0001-02-03T04:60:06.000007Z"',
            # Second out of range
            b'"0001-02-03T04:05:60.000007Z"',
            # Timezone hour out of range
            b'"0001-02-03T04:05:06.000007+24:00"',
            b'"0001-02-03T04:05:06.000007-24:00"',
            # Timezone minute out of range
            b'"0001-02-03T04:05:06.000007+00:60"',
            b'"0001-02-03T04:05:06.000007-00:60"',
            # Year out of range with timezone applied
            b'"9999-12-31T23:59:59-00:01"',
        ],
    )
    def test_decode_datetime_malformed(self, s):
        with pytest.raises(msgspec.ValidationError, match="Invalid RFC3339"):
            msgspec.json.decode(s, type=datetime.datetime)


class TestEnum:
    def test_encode_enum(self):
        s = msgspec.json.encode(FruitStr.APPLE)
        assert s == b'"APPLE"'

    def test_decode_enum(self):
        x = msgspec.json.decode(b'"APPLE"', type=FruitStr)
        assert x == FruitStr.APPLE

    def test_decode_enum_invalid_value(self):
        with pytest.raises(
            msgspec.ValidationError, match="Invalid enum value 'MISSING'"
        ):
            msgspec.json.decode(b'"MISSING"', type=FruitStr)

    def test_decode_enum_invalid_value_nested(self):
        class Test(msgspec.Struct):
            fruit: FruitStr

        with pytest.raises(
            msgspec.ValidationError,
            match=r"Invalid enum value 'MISSING' - at `\$.fruit`",
        ):
            msgspec.json.decode(b'{"fruit": "MISSING"}', type=Test)


class TestIntegers:
    @pytest.mark.parametrize("ndigits", range(21))
    def test_encode_int(self, ndigits):
        if ndigits == 0:
            s = b"0"
        else:
            s = "".join(
                itertools.islice(itertools.cycle("123456789"), ndigits)
            ).encode()

        x = int(s)
        assert msgspec.json.encode(x) == s
        if 0 < ndigits < 20:
            assert msgspec.json.encode(-x) == b"-" + s

    @pytest.mark.parametrize("x", [-(2**63), 2**64 - 1])
    def test_encode_int_boundaries(self, x):
        assert msgspec.json.encode(x) == str(x).encode()
        with pytest.raises(OverflowError):
            if x > 0:
                msgspec.json.encode(x + 1)
            else:
                msgspec.json.encode(x - 1)

    @pytest.mark.parametrize("ndigits", range(21))
    def test_decode_int(self, ndigits):
        if ndigits == 0:
            s = b"0"
        else:
            s = "".join(
                itertools.islice(itertools.cycle("123456789"), ndigits)
            ).encode()

        x = int(s)
        assert msgspec.json.decode(s) == x
        if 0 < ndigits < 20:
            assert msgspec.json.decode(b"-" + s) == -x

    @pytest.mark.parametrize("x", [2**63 - 1, 2**63, 2**63 + 1])
    def test_decode_int_19_digit_overflow_boundary(self, x):
        s = str(x).encode("utf-8")
        # Add extra trailing 0 to ensure no out-of-bounds reads
        buffer = memoryview(s + b"0")[:-1]
        assert msgspec.json.decode(buffer) == x

    @pytest.mark.parametrize("x", [-(2**63), 2**64 - 1])
    def test_decode_int_boundaries(self, x):
        s = str(x).encode()
        x2 = msgspec.json.decode(s)
        assert isinstance(x2, int)
        assert x2 == x

    @pytest.mark.parametrize("x", [-(2**63) - 1, 2**64])
    def test_decode_int_out_of_range_convert_to_float(self, x):
        s = str(x).encode()
        x2 = msgspec.json.decode(s)
        assert isinstance(x2, float)
        assert x2 == float(x)

    @pytest.mark.parametrize("x", [-(2**63) - 1, 2**64])
    def test_decode_int_out_of_range_errors_if_int_requested(self, x):
        s = str(x).encode()
        with pytest.raises(
            msgspec.ValidationError, match="Expected `int`, got `float`"
        ):
            msgspec.json.decode(s, type=int)

    @pytest.mark.parametrize("s", [b"   123   ", b"   -123   "])
    def test_decode_int_whitespace(self, s):
        assert msgspec.json.decode(s) == int(s)

    @pytest.mark.parametrize("s", [b"- 123", b"-n123", b"1 2", b"12n3", b"123n"])
    def test_decode_int_malformed(self, s):
        with pytest.raises(msgspec.DecodeError):
            msgspec.json.decode(s)

    def test_decode_int_converts_to_float_if_requested(self):
        x = msgspec.json.decode(b"123", type=float)
        assert isinstance(x, float)
        assert x == 123.0
        x = msgspec.json.decode(b"-123", type=float)
        assert isinstance(x, float)
        assert x == -123.0

    def test_decode_int_type_error(self):
        with pytest.raises(msgspec.ValidationError, match="Expected `str`, got `int`"):
            msgspec.json.decode(b"123", type=str)


class TestIntEnum:
    def test_encode_intenum(self):
        s = msgspec.json.encode(FruitInt.APPLE)
        assert s == b"1"

    def test_decode_intenum(self):
        x = msgspec.json.decode(b"1", type=FruitInt)
        assert x == FruitInt.APPLE

    def test_decode_intenum_invalid_value(self):
        with pytest.raises(msgspec.ValidationError, match="Invalid enum value 3"):
            msgspec.json.decode(b"3", type=FruitInt)

    def test_decode_intenum_invalid_value_nested(self):
        class Test(msgspec.Struct):
            fruit: FruitInt

        with pytest.raises(
            msgspec.ValidationError, match=r"Invalid enum value 3 - at `\$.fruit`"
        ):
            msgspec.json.decode(b'{"fruit": 3}', type=Test)


class TestLiteral:
    @pytest.mark.parametrize(
        "values",
        [
            (1, 2, 3),
            ("one", "two", "three"),
            (1, 2, "three", "four"),
            (1, None),
            ("one", None),
        ],
    )
    def test_literal(self, values):
        literal = Literal[values]
        dec = msgspec.json.Decoder(literal)
        for val in values:
            assert dec.decode(msgspec.json.encode(val)) == val

        for bad in ["bad", 1234]:
            with pytest.raises(msgspec.ValidationError):
                dec.decode(msgspec.json.encode(bad))

    def test_int_literal_errors(self):
        dec = msgspec.json.Decoder(Literal[1, 2, 3])

        with pytest.raises(msgspec.ValidationError, match="Invalid enum value 4"):
            dec.decode(b"4")

        with pytest.raises(msgspec.ValidationError, match="Expected `int`, got `str`"):
            dec.decode(b'"bad"')

    def test_str_literal_errors(self):
        dec = msgspec.json.Decoder(Literal["one", "two", "three"])

        with pytest.raises(msgspec.ValidationError, match="Expected `str`, got `int`"):
            dec.decode(b"4")

        with pytest.raises(msgspec.ValidationError, match="Invalid enum value 'bad'"):
            dec.decode(b'"bad"')


class TestFloat:
    @pytest.mark.parametrize(
        "x",
        [
            0.1 + 0.2,
            -0.1 - 0.2,
            1.0,
            -1.0,
            sys.float_info.min,
            sys.float_info.max,
            1.0 / 3,
            2.0**-24,
            2.0**-14,
            2.0**-149,
            2.0**-126,
            sys.float_info.min / 2,
            sys.float_info.min / 10,
            sys.float_info.min / 1000,
            sys.float_info.min / 100000,
            5e-324,
            2.9802322387695312e-8,
            2.109808898695963e16,
            4.940656e-318,
            1.18575755e-316,
            2.989102097996e-312,
            9.0608011534336e15,
            4.708356024711512e18,
            9.409340012568248e18,
            1.2345678,
            5.764607523034235e39,
            1.152921504606847e40,
            2.305843009213694e40,
            4.294967294,
            4.294967295,
            4.294967296,
            4.294967297,
            4.294967298,
            1.7800590868057611e-307,
            2.8480945388892175e-306,
            2.446494580089078e-296,
            4.8929891601781557e-296,
            1.8014398509481984e16,
            3.6028797018963964e16,
            2.900835519859558e-216,
            5.801671039719115e-216,
            3.196104012172126e-27,
            9.007199254740991e15,
            9.007199254740992e15,
            3.1462737709539517e18,
        ],
    )
    def test_roundtrip_float_tricky_cases(self, x):
        """Tricky float values, many taken from
        https://github.com/ulfjack/ryu/blob/master/ryu/tests/d2s_test.cc"""
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s)
        assert x == x2

    @pytest.mark.parametrize("x", [-0.0, 0.0])
    def test_roundtrip_signed_zero(self, x):
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s)
        assert x == x2
        assert math.copysign(1.0, x) == math.copysign(1.0, x2)

    @pytest.mark.parametrize("n", range(-15, 15))
    def test_roundtrip_float_powers_10(self, n):
        x = 10.0**n
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s)
        assert x == x2

    @pytest.mark.parametrize("n", range(-15, 14))
    def test_roundtrip_float_lots_of_middle_zeros(self, n):
        x = 1e15 + 10.0**n
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s)
        assert x == x2

    @pytest.mark.parametrize("n", range(1, 17))
    def test_roundtrip_float_integers(self, n):
        x = float("".join(itertools.islice(itertools.cycle("123456789"), n)))
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s)
        assert x == x2

    @pytest.mark.parametrize("scale", [0.0001, 1, 1000])
    @pytest.mark.parametrize("n", range(54))
    def test_roundtrip_float_powers_of_2(self, n, scale):
        x = (2.0**n) * scale
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s)
        assert x == x2

    def test_roundtrip_float_random_checks(self):
        """Generate 1000 random floats to check"""
        rand = random.Random()

        def randfloats(N):
            while N:
                dbytes = rand.getrandbits(64).to_bytes(8, "big")
                x = struct.unpack("!d", dbytes)[0]
                if math.isfinite(x):
                    N -= 1
                    yield x

        for x in randfloats(1000):
            s = msgspec.json.encode(x)
            x2 = msgspec.json.decode(s)
            assert x == x2

    @pytest.mark.parametrize(
        "s",
        [
            str(2**64 - 1).encode(),  # 20 digits, no overflow
            str(2**64).encode(),  # 20 digits, overflow
            str(2**64 + 1).encode(),  # 20 digits, overflow
            str(2**68).encode(),  # 21 digits
        ],
    )
    @pytest.mark.parametrize("i", [-5, 0, 5])
    def test_decode_float_long_mantissa(self, s, i):
        if i > 0:
            s = s[:i] + b"." + s[i:]
        elif i == 0:
            s += b".0"
        else:
            s = b"0." + b"0" * (-i) + s
        x = msgspec.json.decode(s)
        x2 = msgspec.json.decode(s, type=float)
        assert float(s) == x == x2
        with pytest.raises(
            msgspec.ValidationError, match="Expected `int`, got `float`"
        ):
            msgspec.json.decode(s, type=int)

    @pytest.mark.parametrize("n", [5, 20, 300, 500])
    def test_decode_float_lots_of_leading_zeros(self, n):
        s = b"0." + b"0" * n + b"123"
        x = msgspec.json.decode(s)
        x2 = msgspec.json.decode(s, type=float)
        assert x == x2 == float(s)

    @pytest.mark.parametrize("n", [5, 20, 300, 500])
    def test_decode_float_lots_of_middle_leading_zeros(self, n):
        s = b"1." + b"0" * n + b"123"
        x = msgspec.json.decode(s)
        x2 = msgspec.json.decode(s, type=float)
        assert x == x2 == float(s)

    @pytest.mark.parametrize("prefix", [b"0", b"0.0", b"0.0001", b"123", b"123.000"])
    @pytest.mark.parametrize("e", [b"e", b"E"])
    @pytest.mark.parametrize("sign", [b"+", b"-", b""])
    @pytest.mark.parametrize("exp", [b"0", b"000", b"12", b"300"])
    def test_decode_float_with_exponent(self, prefix, e, sign, exp):
        s = prefix + e + sign + exp
        x = msgspec.json.decode(s)
        x2 = msgspec.json.decode(s, type=float)
        assert x == x2 == float(s)

    def test_decode_float_long_decimal_large_exponent(self):
        s = b"0." + b"0" * 500 + b"123e500"
        x = msgspec.json.decode(s)
        assert x == float(s)

    @pytest.mark.parametrize("s", [b"123e308", b"-123e308", b"123e50000", b"123e50000"])
    def test_decode_float_boundaries_errors(self, s):
        with pytest.raises(msgspec.ValidationError, match="Number out of range"):
            msgspec.json.decode(s)

    @pytest.mark.parametrize(
        "s",
        [
            "-2.2222222222223e-322",
            "9007199254740993.0",
            "860228122.6654514319E+90",
            "10000000000000000000",
            "10000000000000000000000000000001000000000000",
            "10000000000000000000000000000000000000000001",
            "1.1920928955078125e-07",
            (
                "9355950000000000000.0000000000000000000000000000000000184467440737095516160000"
                "018446744073709551616184467440737095516140737095516161844674407370955161600018"
                "446744073709551616600000184467440737095516161844674407370955161407370955161618"
                "446744073709551616000184467440737095516160184467440737095567445161618446744073"
                "709551614073709551616184467440737095516160001844674407370955161601844674407370"
                "955161161600018446744073709500184467440737095516160018446744073709551616001844"
                "674407370955116816446744073709551616000184407370955161601844674407370955161618"
                "446744073709551616000184467440753691075160161161600018446744073709500184467440"
                "737095516160018446744073709551616001844674407370955161618446744073709551616000"
                "1844955161618446744073709551616000184467440753691075160018446744073709"
            ),
            (
                "2.2250738585072021241887014792022203290724052827943903781430313383743510731924"
                "419468675440643256388185138218821850243806999994773301300564988410779192874134"
                "192929720097048195199306799329096904278406473168204156592672863293363047467012"
                "331685298342215274451726083585965456631928283524478778779989431077978383369915"
                "928859455521371418112845825114558431922307989750439508685941245723089173894616"
                "936837232119137365897797772328669884035639025104444303545739673370658398105542"
                "045669382465841374760715598117657387762674766591238719993190400631733470900301"
                "279018817520344719025002806127777791679839109057858400646471594381051148915428"
                "277504117468219413395246668250343130618158782937900420539237507208336669324158"
                "0002758391118854188641513168478436313080237596295773983001708984375e-308"
            ),
            "1.0000000000000006661338147750939242541790008544921875",
            "1090544144181609348835077142190",
            "2.2250738585072013e-308",
            "-92666518056446206563E3",
            "-92666518056446206563E3",
            "-42823146028335318693e-128",
            "90054602635948575728E72",
            (
                "1.0000000000000018855892087022346387017456602069175351539464355066307055836837"
                "3221972569761144603605635692374830246134201063722058e-309"
            ),
            "0e9999999999999999999999999999",
            "-2402844368454405395.2",
            "2402844368454405395.2",
            "7.0420557077594588669468784357561207962098443483187940792729600000e+59",
            "7.0420557077594588669468784357561207962098443483187940792729600000e+59",
            "-1.7339253062092163730578609458683877051596800000000000000000000000e+42",
            "-2.0972622234386619214559824785284023792871122537545728000000000000e+52",
            "-1.0001803374372191849407179462120053338028379051879898808320000000e+57",
            "-1.8607245283054342363818436991534856973992070520151142825984000000e+58",
            "-1.9189205311132686907264385602245237137907390376574976000000000000e+52",
            "-2.8184483231688951563253238886553506793085187889855201280000000000e+54",
            "-1.7664960224650106892054063261344555646357024359107788800000000000e+53",
            "-2.1470977154320536489471030463761883783915110400000000000000000000e+45",
            "-4.4900312744003159009338275160799498340862630046359789166919680000e+61",
            "1.797693134862315700000000000000001e308",
            "1.00000006e+09",
            "4.9406564584124653e-324",
            "4.9406564584124654e-324",
            "2.2250738585072009e-308",
            "2.2250738585072014e-308",
            "1.7976931348623157e308",
            "1.7976931348623158e308",
            "4503599627370496.5",
            "4503599627475352.5",
            "4503599627475353.5",
            "2251799813685248.25",
            "1125899906842624.125",
            "1125899906842901.875",
            "2251799813685803.75",
            "4503599627370497.5",
            "45035996.273704995",
            "45035996.273704985",
            (
                "0.0000000000000000000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000000000000000000445"
                "014771701440227211481959341826395186963909270329129604685221944964444404215389"
                "103305904781627017582829831782607924221374017287738918929105531441481564124348"
                "675997628212653465850710457376274429802596224490290377969811444461457051026631"
                "151003182879495279596682360399864792509657803421416370138126133331198987655154"
                "514403152612538132666529513060001849177663286607555958373922409899478075565940"
                "981010216121988146052587425791790000716759993441450860872056815779154359230189"
                "103349648694206140521828924314457976051636509036065141403772174422625615902446"
                "685257673724464300755133324500796506867194913776884780053099639677097589658441"
                "378944337966219939673169362804570848666132067970177289160800206986794085513437"
                "28867675409720757232455434770912461317493580281734466552734375"
            ),
            (
                "0.0000000000000000000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000000000000000000000"
                "000000000000000000000000000000000000000000000000000000000000000000000000000222"
                "507385850720088902458687608585988765042311224095946549352480256244000922823569"
                "517877588880375915526423097809504343120858773871583572918219930202943792242235"
                "598198275012420417889695713117910822610439719796040004548973919380791989360815"
                "256131133761498420432717510336273915497827315941438281362751138386040942494649"
                "422863166954291050802018159266421349966065178030950759130587198464239060686371"
                "020051087232827846788436319445158661350412234790147923695852083215976210663754"
                "016137365830441936037147783553066828345356340050740730401356029680463759185831"
                "631242245215992625464943008368518617194224176464551371354201322170313704965832"
                "101546540680353974179060225895030235019375197730309457631732108525072993050897"
                "61582519159720757232455434770912461317493580281734466552734375"
            ),
            (
                "143845666314139027352611820764223558118322784524633123116263665379036815209139"
                "419693036582863468763794815794077659918279138752713535303473835713411031060945"
                "569390082419354977279201654318268051974058035436546798544018359870131225762454"
                "556233139701832992861319612559027418772007391481806253083031653315809862498411"
                "888929828137181228878953731059903752911341543873895489475212472498306724110876"
                "448834645437669901867307840475112141480493722424080599312381693232622368309077"
                "056159757045779393298582616260425588452913412639628220212652625338938342180672"
                "795458852559611437980126909409632980505480308929973699687095125857301087740440"
                "745195384669860919821392688269207855703322826525930548119852605981316446918758"
                "669325733577952202040764549868426333992190522755661669812996741289128223168550"
                "466067127792719829000982468018631975097866573457668378425580226970891736171946"
                "604317520115884909788137047711185017157986905601606166617302905958843377601564"
                "443970505037755427769614392827809345379280384625271596601673322264644238289212"
                "394005244134682242972159388437821255870100435692424303005951748934664657772462"
                "249891975259738209522250031112418182351225107135618176937657765139002829779615"
                "620881537508915912839494571051586133448626710179749711112590927250519479287088"
                "961717975870344260801614334326215999814970060659779253557445756042922697427344"
                "363032381874773077131676339857211087495998192373246307688452867739265415001026"
                "982223940199342748237651323138921235358357356637691557265091686655361236618737"
                "895955498356671276709337290603018897622016905802535497362221166650454931695827"
                "188097569714354656446980679135870731887307570838334500409015197406832583817753"
                "126695417740666139222980134999469594150993565535565298572378215357008408956013"
                "9142231.7384750423625968754491545523922995489471381620816941686753406778438076"
                "131297804493233637590270129724669873709218168131626587547265451210905455072402"
                "670004565947865409496052607224619378706306348749917293982080264676981318986918"
                "30012167897399682179601734569071423681e-1500"
            ),
            "-2240084132271013504.131248280843119943687942846658579428",
        ],
        ids=itertools.count(),
    )
    def test_decode_float_cases_from_fastfloat(self, s):
        """Some tricky test cases from
        https://github.com/fastfloat/fast_float/blob/main/tests/basictest.cpp"""
        x = msgspec.json.decode(s.encode())
        assert x == float(s)

    @pytest.mark.parametrize("negative", [True, False])
    def test_decode_long_float_rounds_to_zero(self, negative):
        s = b"0." + b"0" * 400 + b"1"
        if negative:
            s = b"-" + s
        x = msgspec.json.decode(s)
        assert x == 0.0
        assert (math.copysign(1.0, x) < 0) == negative

    @pytest.mark.parametrize(
        "s",
        [
            "0." + "0" * 900 + "123451e875",
            "0." + "0" * 900 + "12345" * 40 + "1e875",
            "0." + "0" * 900 + "12345" * 400 + "1e875",
            "1" * 30000 + "e-30000",
        ],
        ids=itertools.count(),
    )
    def test_decode_long_float_truncated_but_exp_brings_back_in_bounds(self, s):
        """The digits part of these would put them over the limit to inf, but
        the exponent bit brings them back in range"""
        x = msgspec.json.decode(s.encode())
        assert x == float(s)

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"1.", "invalid number"),
            (b"1..", "invalid number"),
            (b"1.2.", "trailing characters"),
            (b".123", "invalid character"),
            (b"001.2", "invalid number"),
            (b"00.2", "invalid number"),
            (b"1a2", "trailing characters"),
            (b"1.e2", "invalid number"),
            (b"1.2e", "invalid number"),
            (b"1.2e+", "invalid number"),
            (b"1.2e-", "invalid number"),
            (b"1.2ea", "invalid number"),
            (b"1.2e1a", "trailing characters"),
            (b"1.2e1-2", "trailing characters"),
            (b"123 456", "trailing characters"),
            (b"123. 456", "invalid number"),
            (b"123.456 e2", "trailing characters"),
            (b"123.456e 2", "invalid number"),
        ],
    )
    def test_decode_float_malformed(self, s, error):
        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s)

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"1" * 25 + b".", "invalid number"),
            (b"1" * 25 + b".2.", "trailing characters"),
            (b"1" * 25 + b".e", "invalid number"),
            (b"1" * 25 + b".e2", "invalid number"),
            (b"1" * 25 + b".2e", "invalid number"),
            (b"1" * 25 + b".2e-", "invalid number"),
            (b"1" * 25 + b".2e+", "invalid number"),
            (b"1" * 25 + b".2ea", "invalid number"),
        ],
        ids=itertools.count(),
    )
    def test_decode_long_float_malformed(self, s, error):
        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s)

    @pytest.mark.parametrize(
        "s",
        [
            b"1e500",
            b"-1e500",
            b"123456789e301",
            b"-123456789e301",
            b"0.01e311",
            b"-0.01e311",
            b"1" * 3000 + b"e-2600",
            b"-" + b"1" * 3000 + b"e-2600",
        ],
        ids=itertools.count(),
    )
    def test_decode_float_out_of_bounds(self, s):
        with pytest.raises(msgspec.ValidationError, match="out of range"):
            msgspec.json.decode(s)

    @pytest.mark.parametrize("s", [b"1.23e3", b"1.2", b"1e2"])
    def test_decode_float_err_expected_int(self, s):
        with pytest.raises(
            msgspec.ValidationError, match="Expected `int`, got `float`"
        ):
            msgspec.json.decode(s, type=int)


class TestSequences:
    @pytest.mark.parametrize("x", [[], [1], [1, "two", False]])
    @pytest.mark.parametrize("type", [list, set, frozenset, tuple])
    def test_roundtrip_sequence(self, x, type):
        x = type(x)
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s, type=type)
        assert x == x2
        assert isinstance(x2, type)

    @pytest.mark.parametrize(
        "s, x",
        [
            (b"[\t\n\r ]", []),
            (b"[\t\n 1\r ]", [1]),
            (b"[ 1\n ,\t  2\r  ]", [1, 2]),
            (b" \t [\n 1 ,  2 \t ]\r  ", [1, 2]),
        ],
    )
    @pytest.mark.parametrize("type", [list, set, frozenset, tuple])
    def test_decode_sequence_ignores_whitespace(self, s, x, type):
        x2 = msgspec.json.decode(s, type=type)
        assert isinstance(x2, type)
        assert type(x) == type(x2)

    def test_decode_typed_list(self):
        dec = msgspec.json.Decoder(List[int])
        assert dec.decode(b"[]") == []
        assert dec.decode(b"[1]") == [1]
        assert dec.decode(b"[1,2]") == [1, 2]

    def test_decode_typed_set(self):
        dec = msgspec.json.Decoder(Set[int])
        assert dec.decode(b"[]") == set()
        assert dec.decode(b"[1]") == {1}
        assert dec.decode(b"[1,2]") == {1, 2}

    def test_decode_typed_frozenset(self):
        dec = msgspec.json.Decoder(FrozenSet[int])
        assert dec.decode(b"[]") == frozenset()
        assert dec.decode(b"[1]") == frozenset({1})
        assert dec.decode(b"[1,2]") == frozenset({1, 2})

    def test_decode_typed_vartuple(self):
        dec = msgspec.json.Decoder(Tuple[int, ...])
        assert dec.decode(b"[]") == ()
        assert dec.decode(b"[1]") == (1,)
        assert dec.decode(b"[1,2]") == (
            1,
            2,
        )

    @pytest.mark.parametrize("type", [List[int], Set[int], Tuple[int, ...]])
    @pytest.mark.parametrize("bad_index", [0, 9, 10, 91, 1234])
    def test_decode_typed_list_wrong_element_type(self, type, bad_index):
        dec = msgspec.json.Decoder(type)
        data = [1] * (bad_index + 1)
        data[bad_index] = "oops"
        msg = msgspec.json.encode(data)
        err_msg = rf"Expected `int`, got `str` - at `\$\[{bad_index}\]`"
        with pytest.raises(msgspec.ValidationError, match=err_msg):
            dec.decode(msg)

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"[", "truncated"),
            (b"[1", "truncated"),
            (b"[,]", "invalid character"),
            (b"[, 1]", "invalid character"),
            (b"[1, ]", "trailing comma in array"),
            (b"[1, 2 3]", r"expected ',' or ']'"),
        ],
    )
    @pytest.mark.parametrize("type", [list, set, tuple, Tuple[int, int, int]])
    def test_decode_sequence_malformed(self, s, error, type):
        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=type)

    def test_decode_fixtuple_any(self):
        dec = msgspec.json.Decoder(Tuple[Any, Any, Any])
        x = (1, "two", False)
        res = dec.decode(b'[1, "two", false]')
        assert res == x
        with pytest.raises(
            msgspec.ValidationError, match="Expected `array`, got `int`"
        ):
            dec.decode(b"1")
        with pytest.raises(
            msgspec.ValidationError, match="Expected `array` of length 3"
        ):
            dec.decode(b'[1, "two"]')

    def test_decode_fixtuple_typed(self):
        dec = msgspec.json.Decoder(Tuple[int, str, bool])
        x = (1, "two", False)
        res = dec.decode(b'[1, "two", false]')
        assert res == x
        with pytest.raises(msgspec.ValidationError, match="Expected `bool`"):
            dec.decode(b'[1, "two", "three"]')
        with pytest.raises(
            msgspec.ValidationError, match="Expected `array` of length 3"
        ):
            dec.decode(b'[1, "two"]')


class TestNamedTuple:
    """Most tests are in `test_common`, this just tests some JSON peculiarities"""

    @pytest.mark.parametrize(
        "s, x",
        [(b"[\t\n\r ]", ()), (b"   [  1  ,  2  ]   ", (1, 2))],
    )
    def test_decode_namedtuple_ignores_whitespace(self, s, x):
        class Test(NamedTuple):
            a: int = 0
            b: int = 1

        x2 = msgspec.json.decode(s, type=Test)
        assert x2 == Test(*x)

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"[", "truncated"),
            (b"[1", "truncated"),
            (b"[,]", "invalid character"),
            (b"[, 1]", "invalid character"),
            (b"[1, ]", "trailing comma in array"),
            (b"[1, 2 3]", r"expected ',' or ']'"),
        ],
    )
    def test_decode_namedtuple_malformed(self, s, error):
        class Test(NamedTuple):
            a: int
            b: int

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Test)


class TestDict:
    def test_encode_dict_raises_non_string_keys(self):
        with pytest.raises(TypeError, match="dict keys must be strings"):
            msgspec.json.encode({"a": 1, 2: "bad"})

    @pytest.mark.parametrize("x", [{}, {"a": 1}, {"a": 1, "b": 2}])
    def test_roundtrip_dict(self, x):
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s)
        assert x == x2
        assert json.loads(s) == x

    def test_decode_any_dict(self):
        x = msgspec.json.decode(b'{"a": 1, "b": "two", "c": false}')
        assert x == {"a": 1, "b": "two", "c": False}

    @pytest.mark.parametrize(
        "s, x",
        [
            (b"{\t\n\r }", {}),
            (b'{\t\n\r "a"    :     1}', {"a": 1}),
            (b'{ "a"\t : 1 \n, "b": \r 2  }', {"a": 1, "b": 2}),
            (b'   { "a"\t : 1 \n, "b": \r 2  }   ', {"a": 1, "b": 2}),
        ],
    )
    def test_decode_dict_ignores_whitespace(self, s, x):
        x2 = msgspec.json.decode(s)
        assert x == x2

    def test_decode_typed_dict_wrong_element_type(self):
        dec = msgspec.json.Decoder(Dict[str, int])
        with pytest.raises(
            msgspec.ValidationError, match=r"Expected `int`, got `str` - at `\$\[...\]`"
        ):
            dec.decode(b'{"a": "bad"}')

    def test_decode_typed_dict_literal_key(self):
        dec = msgspec.json.Decoder(Dict[Literal["a", "b"], int])
        assert dec.decode(b'{"a": 1, "b": 2}') == {"a": 1, "b": 2}

        with pytest.raises(msgspec.ValidationError, match="Invalid enum value 'c'"):
            dec.decode(b'{"a": 1, "c": 2}')

    @pytest.mark.parametrize("length", [3, 32, 33])
    def test_decode_dict_string_cache(self, length):
        key = "x" * length
        msg = [{key: 1}, {key: 2}, {key: 3}]
        res = msgspec.json.decode(msgspec.json.encode(msg))
        assert msg == res
        ids = {id(k) for d in res for k in d.keys()}
        if length > 32:
            assert len(ids) == 3
        else:
            assert len(ids) == 1

    def test_decode_dict_string_cache_ascii_only(self):
        """Short non-ascii strings aren't cached"""
        s = "123 á 456"
        msg = [{s: 1}, {s: 2}, {s: 3}]
        res = msgspec.json.decode(msgspec.json.encode(msg))
        ids = {id(k) for d in res for k in d.keys()}
        assert len(ids) == 3

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"{", "truncated"),
            (b'{"a"', "truncated"),
            (b"{,}", "object keys must be strings"),
            (b"{:}", "object keys must be strings"),
            (b"{1: 2}", "object keys must be strings"),
            (b'{"a": 1, }', "trailing comma in object"),
            (b'{"a": 1, "b" 2}', "expected ':'"),
            (b'{"a": 1, "b": 2  "c"}', r"expected ',' or '}'"),
        ],
    )
    @pytest.mark.parametrize("type", [dict, Any])
    def test_decode_dict_malformed(self, s, error, type):
        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=type)


class TestTypedDict:
    """Most tests are in `test_common`, this just tests some JSON peculiarities"""

    @pytest.mark.parametrize(
        "s, x",
        [
            (b"{\t\n\r }", {}),
            (b'{\t\n\r "a"    :     1}', {"a": 1}),
            (b'{ "a"\t : 1 \n, "b": \r 2  }', {"a": 1, "b": 2}),
            (b'   { "a"\t : 1 \n, "b": \r 2  }   ', {"a": 1, "b": 2}),
        ],
    )
    def test_decode_typeddict_ignores_whitespace(self, s, x):
        class Test(TypedDict, total=False):
            a: int
            b: int

        x2 = msgspec.json.decode(s, type=Test)
        assert x == x2

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"{", "truncated"),
            (b'{"a"', "truncated"),
            (b"{,}", "object keys must be strings"),
            (b"{:}", "object keys must be strings"),
            (b"{1: 2}", "object keys must be strings"),
            (b'{"a": 1, }', "trailing comma in object"),
            (b'{"a": 1, "b" 2}', "expected ':'"),
            (b'{"a": 1, "b": 2  "c"}', r"expected ',' or '}'"),
        ],
    )
    def test_decode_typeddict_malformed(self, s, error):
        class Test(TypedDict, total=False):
            a: int
            b: int

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Test)


class TestStruct:
    @pytest.mark.parametrize("tag", [False, "Test", 123])
    def test_encode_empty_struct(self, tag):
        class Test(msgspec.Struct, tag=tag):
            pass

        s = msgspec.json.encode(Test())
        if tag:
            expected = msgspec.json.encode({"type": tag})
            assert s == expected
        else:
            assert s == b"{}"

    @pytest.mark.parametrize("tag", [False, "Test", 123])
    def test_encode_one_field_struct(self, tag):
        class Test(msgspec.Struct, tag=tag):
            a: int

        s = msgspec.json.encode(Test(a=1))
        if tag:
            expected = msgspec.json.encode({"type": tag, "a": 1})
            assert s == expected
        else:
            assert s == b'{"a":1}'

    @pytest.mark.parametrize("tag", [False, "Test", 123])
    def test_encode_two_field_struct(self, tag):
        class Test(msgspec.Struct, tag=tag):
            a: int
            b: str

        s = msgspec.json.encode(Test(a=1, b="two"))
        if tag:
            expected = msgspec.json.encode({"type": tag, "a": 1, "b": "two"})
            assert s == expected
        else:
            assert s == b'{"a":1,"b":"two"}'

    def test_decode_struct(self):
        dec = msgspec.json.Decoder(Person)
        msg = b'{"first": "harry", "last": "potter", "age": 13, "prefect": false}'
        x = dec.decode(msg)
        assert x == Person("harry", "potter", 13, False)

        with pytest.raises(
            msgspec.ValidationError, match="Expected `object`, got `int`"
        ):
            dec.decode(b"1")

    def test_decode_struct_field_wrong_type(self):
        dec = msgspec.json.Decoder(Person)

        msg = b'{"first": "harry", "last": "potter", "age": "bad"}'
        with pytest.raises(
            msgspec.ValidationError, match=r"Expected `int`, got `str` - at `\$.age`"
        ):
            dec.decode(msg)

    def test_decode_struct_missing_fields(self):
        bad = b'{"first": "harry", "last": "potter"}'
        with pytest.raises(
            msgspec.ValidationError, match="Object missing required field `age`"
        ):
            msgspec.json.decode(bad, type=Person)

        bad = b"{}"
        with pytest.raises(
            msgspec.ValidationError, match="Object missing required field `first`"
        ):
            msgspec.json.decode(bad, type=Person)

        bad = b'[{"first": "harry", "last": "potter"}]'
        with pytest.raises(
            msgspec.ValidationError,
            match=r"Object missing required field `age` - at `\$\[0\]`",
        ):
            msgspec.json.decode(bad, type=List[Person])

    def test_decode_struct_fields_mixed_order(self):
        class Test(msgspec.Struct):
            a: int
            b: int
            c: int
            d: int

        sol = Test(0, 1, 2, 3)

        dec = msgspec.json.Decoder(Test)

        pairs = list(zip("abcdef", range(6)))

        for data in itertools.permutations(pairs):
            msg = msgspec.json.encode(dict(data))
            res = dec.decode(msg)
            assert res == sol

    @pytest.mark.parametrize(
        "extra",
        [
            None,
            False,
            True,
            1,
            2.0,
            "three",
            [1, 2],
            {"a": 1},
        ],
    )
    def test_decode_struct_ignore_extra_fields(self, extra):
        dec = msgspec.json.Decoder(Person)

        a = msgspec.json.encode(
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

    def test_decode_struct_defaults_missing_fields(self):
        dec = msgspec.json.Decoder(Person)

        a = b'{"first": "harry", "last": "potter", "age": 13}'
        res = dec.decode(a)
        assert res == Person("harry", "potter", 13)
        assert res.prefect is False

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"{", "truncated"),
            (b'{"first"', "truncated"),
            (b"{,}", "object keys must be strings"),
            (b"{:}", "object keys must be strings"),
            (b"{1: 2}", "object keys must be strings"),
            (b'{"age": 13, }', "trailing comma in object"),
            (b'{"age": 13, "first" "harry"}', "expected ':'"),
            (b'{"age": 13, "first": "harry"  "c"}', r"expected ',' or '}'"),
        ],
    )
    def test_decode_struct_malformed(self, s, error):
        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Person)

    @pytest.mark.parametrize("array_like", [False, True])
    def test_struct_gc_maybe_untracked_on_decode(self, array_like):
        class Test(msgspec.Struct, array_like=array_like):
            x: Any
            y: Any
            z: Tuple = ()

        enc = msgspec.json.Encoder()
        dec = msgspec.json.Decoder(List[Test])

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

    @pytest.mark.parametrize("array_like", [False, True])
    def test_struct_gc_false_always_untracked_on_decode(self, array_like):
        class Test(msgspec.Struct, array_like=array_like, gc=False):
            x: Any
            y: Any

        dec = msgspec.json.Decoder(List[Test])

        ts = [
            Test(1, 2),
            Test([], []),
            Test({}, {}),
        ]
        for obj in dec.decode(msgspec.json.encode(ts)):
            assert not gc.is_tracked(obj)

    def test_struct_recursive_definition(self):
        enc = msgspec.json.Encoder()
        dec = msgspec.json.Decoder(Node)

        x = Node(Node(Node(), Node(Node())))
        s = enc.encode(x)
        res = dec.decode(s)
        assert res == x

    @pytest.mark.parametrize("tag", ["Test", 0, 2**63 - 1, -(2**63)])
    def test_decode_tagged_struct(self, tag):
        class Test(msgspec.Struct, tag=tag):
            a: int
            b: int

        dec = msgspec.json.Decoder(Test)

        # Test decode with and without tag
        for msg in [
            {"a": 1, "b": 2},
            {"type": tag, "a": 1, "b": 2},
            {"a": 1, "type": tag, "b": 2},
        ]:
            res = dec.decode(msgspec.json.encode(msg))
            assert res == Test(1, 2)

        # Tag incorrect type
        for bad in [False, 123.456]:
            with pytest.raises(msgspec.ValidationError) as rec:
                dec.decode(msgspec.json.encode({"type": bad}))
            assert f"Expected `{type(tag).__name__}`" in str(rec.value)
            assert "`$.type`" in str(rec.value)

        # Tag incorrect value
        bad = -3 if isinstance(tag, int) else "bad"
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(msgspec.json.encode({"type": bad}))
        assert f"Invalid value {bad!r}" in str(rec.value)
        assert "`$.type`" in str(rec.value)

    @pytest.mark.parametrize("tag", ["Test", 123, -123])
    def test_decode_tagged_empty_struct(self, tag):
        class Test(msgspec.Struct, tag=tag):
            pass

        dec = msgspec.json.Decoder(Test)

        # Tag missing
        res = dec.decode(msgspec.json.encode({}))
        assert res == Test()

        # Tag present
        res = dec.decode(msgspec.json.encode({"type": tag}))
        assert res == Test()

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"{", "truncated"),
            (b'{"type"', "truncated"),
            (b"{,}", "object keys must be strings"),
            (b"{:}", "object keys must be strings"),
            (b"{1: 2}", "object keys must be strings"),
            (b'{"type": "Test1", }', "trailing comma in object"),
            (b'{"type": "Test1", "a" 1}', "expected ':'"),
            (b'{"type": "Test1", "a": 1 "b"}', r"expected ',' or '}'"),
            (b'{"type": nulp}', r"invalid character"),
            (b'{"type": "nulp}', r"truncated"),
            (b'{"a": 1, }', "trailing comma in object"),
            (b'{"a": 1, "b" 1}', "expected ':'"),
            (b'{"a": 1 "b"}', r"expected ',' or '}'"),
        ],
    )
    def test_decode_struct_tag_malformed(self, s, error):
        class Test1(msgspec.Struct, tag=True):
            a: int
            b: int

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Test1)

    @pytest.mark.parametrize("ndigits", range(19))
    @pytest.mark.parametrize("negative", [False, True])
    def test_decode_tagged_struct_int_tag(self, ndigits, negative):
        if ndigits == 0:
            s = b"0"
        else:
            s = "".join(
                itertools.islice(itertools.cycle("123456789"), ndigits)
            ).encode()

        tag = int(s)
        if negative:
            tag = -tag

        class Test(msgspec.Struct, tag=tag):
            x: int

        t = Test(1)
        msg = msgspec.json.encode(t)
        assert msgspec.json.decode(msg, type=Test) == t

    def test_decode_tagged_struct_int_tag_uint64_always_invalid(self):
        """Uint64 values aren't currently valid tag values, but we still want
        to raise a good error message."""

        class Test(msgspec.Struct, tag=123):
            pass

        with pytest.raises(msgspec.ValidationError) as rec:
            msgspec.json.decode(msgspec.json.encode({"type": 2**64 - 1}), type=Test)
        assert f"Invalid value {2**64 - 1}" in str(rec.value)
        assert "`$.type`" in str(rec.value)

    @pytest.mark.parametrize(
        "s, error",
        [
            (b'{"type": 00}', "invalid number"),
            (b'{"type": -n123}', "invalid character"),
            (b'{"type": 123n}', "expected ',' or '}'"),
            (b'{"type": 123.}', "invalid number"),
            (b'{"type": 123.n}', "invalid number"),
            (b'{"type": 123e}', "invalid number"),
            (b'{"type": 123en}', "invalid number"),
            (b'{"type": 123, }', "trailing comma in object"),
            (b'{"type": 123, "a" 1}', "expected ':'"),
            (b'{"type": 123, "a": 1 "b"}', "expected ',' or '}'"),
            (b'{"type": nulp}', "invalid character"),
            (b'{"type": "bad}', "truncated"),
            (b'{"type": bad}', "invalid character"),
        ],
    )
    def test_decode_struct_int_tag_malformed(self, s, error):
        class Test1(msgspec.Struct, tag=123):
            a: int
            b: int

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Test1)


class TestStructUnion:
    """Most functionality is tested in `test_common.py:TestStructUnion`, this only
    checks for malformed inputs and whitespace handling"""

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"{", "truncated"),
            (b'{"type"', "truncated"),
            (b"{,}", "object keys must be strings"),
            (b"{:}", "object keys must be strings"),
            (b"{1: 2}", "object keys must be strings"),
            (b'{"type": "Test1", }', "trailing comma in object"),
            (b'{"type": "Test1", "a" 1}', "expected ':'"),
            (b'{"type": "Test1", "a": 1 "b"}', r"expected ',' or '}'"),
            (b'{"type": nulp}', r"invalid character"),
            (b'{"a": 1, }', "trailing comma in object"),
            (b'{"a": 1, "b" 1}', "expected ':'"),
            (b'{"a": 1 "b"}', r"expected ',' or '}'"),
        ],
    )
    def test_decode_struct_union_malformed(self, s, error):
        class Test1(msgspec.Struct, tag=True):
            a: int
            b: int

        class Test2(msgspec.Struct, tag=True):
            pass

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Union[Test1, Test2])

    @pytest.mark.parametrize(
        "s, error",
        [
            (b'{"type": 00}', "invalid number"),
            (b'{"type": -n123}', "invalid character"),
            (b'{"type": 123n}', "expected ',' or '}'"),
            (b'{"type": 123.}', "invalid number"),
            (b'{"type": 123.n}', "invalid number"),
            (b'{"type": 123e}', "invalid number"),
            (b'{"type": 123en}', "invalid number"),
            (b'{"type": 123, }', "trailing comma in object"),
            (b'{"type": 123, "a" 1}', "expected ':'"),
            (b'{"type": 123, "a": 1 "b"}', "expected ',' or '}'"),
            (b'{"type": nulp}', "invalid character"),
            (b'{"type": "bad}', "truncated"),
            (b'{"type": bad}', "invalid character"),
        ],
    )
    def test_decode_struct_union_int_tag_malformed(self, s, error):
        class Test1(msgspec.Struct, tag=-123):
            a: int
            b: int

        class Test2(msgspec.Struct, tag=123):
            pass

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Union[Test1, Test2])

    @pytest.mark.parametrize(
        "s",
        [
            b'  {  "type"  :  "Test1"  ,  "a"  :  1  ,  "b"  :  2  }  ',
            b'  {  "a"  :  1  ,  "type"  :  "Test1"  ,  "b"  :  2  }  ',
            b'  {  "a"  :  1  ,  "b"  :  2  ,  "type"  :  "Test1"  }  ',
        ],
    )
    def test_decode_struct_union_ignores_whitespace(self, s):
        class Test1(msgspec.Struct, tag=True):
            a: int
            b: int

        class Test2(msgspec.Struct, tag=True):
            pass

        res = msgspec.json.decode(s, type=Union[Test1, Test2])
        assert res == Test1(1, 2)

    @pytest.mark.parametrize(
        "s",
        [
            b'  {  "type"  :  -123  ,  "a"  :  1  ,  "b"  :  2  }  ',
            b'  {  "a"  :  1  ,  "type"  :  -123  ,  "b"  :  2  }  ',
            b'  {  "a"  :  1  ,  "b"  :  2  ,  "type"  :  -123  }  ',
        ],
    )
    def test_decode_struct_union_int_tag_ignores_whitespace(self, s):
        class Test1(msgspec.Struct, tag=-123):
            a: int
            b: int

        class Test2(msgspec.Struct, tag=123):
            pass

        res = msgspec.json.decode(s, type=Union[Test1, Test2])
        assert res == Test1(1, 2)

    @pytest.mark.parametrize("preinit", [False, True])
    @pytest.mark.parametrize("tags", [("A", "B"), (0, 2), (0, 1000)])
    @pytest.mark.parametrize("wrap", [False, True])
    def test_struct_union_not_json_compatible(self, preinit, tags, wrap):
        class Test1(msgspec.Struct, tag=tags[0]):
            a: int

        class Test2(msgspec.Struct, tag=tags[1]):
            b: Dict[int, int]

        typ = Union[Test1, Test2]
        if wrap:
            typ = TypedDict("Test3", {"c": typ})

        if preinit:
            msgspec.msgpack.Decoder(typ)

        with pytest.raises(TypeError, match="JSON doesn't support dicts with"):
            msgspec.json.Decoder(typ)


class TestStructArray:
    @pytest.mark.parametrize("tag", [False, True])
    def test_encode_empty_struct(self, tag):
        class Test(msgspec.Struct, array_like=True, tag=tag):
            pass

        s = msgspec.json.encode(Test())
        if tag:
            assert s == b'["Test"]'
        else:
            assert s == b"[]"

    @pytest.mark.parametrize("tag", [False, True])
    def test_encode_one_field_struct(self, tag):
        class Test(msgspec.Struct, array_like=True, tag=tag):
            a: int

        s = msgspec.json.encode(Test(a=1))
        if tag:
            assert s == b'["Test",1]'
        else:
            assert s == b"[1]"

    @pytest.mark.parametrize("tag", [False, True])
    def test_encode_two_field_struct(self, tag):
        class Test(msgspec.Struct, array_like=True, tag=tag):
            a: int
            b: str

        s = msgspec.json.encode(Test(a=1, b="two"))
        if tag:
            assert s == b'["Test",1,"two"]'
        else:
            assert s == b'[1,"two"]'

    def test_struct_array_like(self):
        dec = msgspec.json.Decoder(PersonArray)

        x = PersonArray(first="harry", last="potter", age=13)
        a = msgspec.json.encode(x)
        assert msgspec.json.encode(("harry", "potter", 13, False)) == a
        assert dec.decode(a) == x

        with pytest.raises(
            msgspec.ValidationError, match="Expected `array`, got `int`"
        ):
            dec.decode(b"1")

        # Wrong field type
        bad = msgspec.json.encode(("harry", "potter", "thirteen"))
        with pytest.raises(
            msgspec.ValidationError, match=r"Expected `int`, got `str` - at `\$\[2\]`"
        ):
            dec.decode(bad)

        # Missing fields
        bad = msgspec.json.encode(("harry", "potter"))
        with pytest.raises(
            msgspec.ValidationError,
            match="Expected `array` of at least length 3, got 2",
        ):
            dec.decode(bad)

        bad = msgspec.json.encode(())
        with pytest.raises(
            msgspec.ValidationError,
            match="Expected `array` of at least length 3, got 0",
        ):
            dec.decode(bad)

        # Extra fields ignored
        dec2 = msgspec.json.Decoder(List[PersonArray])
        msg = msgspec.json.encode(
            [
                ("harry", "potter", 13, False, 1, 2, 3, 4),
                ("ron", "weasley", 13, False, 5, 6),
            ]
        )
        res = dec2.decode(msg)
        assert res == [
            PersonArray("harry", "potter", 13),
            PersonArray("ron", "weasley", 13),
        ]

        # Defaults applied
        res = dec.decode(msgspec.json.encode(("harry", "potter", 13)))
        assert res == PersonArray("harry", "potter", 13)
        assert res.prefect is False

    def test_struct_map_and_array_like_messages_cant_mix(self):
        array_msg = msgspec.json.encode(("harry", "potter", 13))
        map_msg = msgspec.json.encode({"first": "harry", "last": "potter", "age": 13})
        sol = Person("harry", "potter", 13)
        array_sol = PersonArray("harry", "potter", 13)

        dec = msgspec.json.Decoder(Person)
        array_dec = msgspec.json.Decoder(PersonArray)

        assert array_dec.decode(array_msg) == array_sol
        assert dec.decode(map_msg) == sol
        with pytest.raises(
            msgspec.ValidationError, match="Expected `object`, got `array`"
        ):
            dec.decode(array_msg)
        with pytest.raises(
            msgspec.ValidationError, match="Expected `array`, got `object`"
        ):
            array_dec.decode(map_msg)

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"[", "truncated"),
            (b"[1", "truncated"),
            (b"[,]", "invalid character"),
            (b"[, 1]", "invalid character"),
            (b"[1, ]", "trailing comma in array"),
            (b"[1, 2 3]", r"expected ',' or ']'"),
        ],
    )
    def test_decode_struct_array_like_malformed(self, s, error):
        class Point(msgspec.Struct, array_like=True):
            x: int
            y: int
            z: int

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Point)

    @pytest.mark.parametrize("tag", ["Test", 123])
    def test_decode_tagged_struct(self, tag):
        class Test(msgspec.Struct, tag=tag, array_like=True):
            a: int
            b: int
            c: int = 0

        dec = msgspec.json.Decoder(Test)

        # Decode with tag
        res = dec.decode(msgspec.json.encode([tag, 1, 2]))
        assert res == Test(1, 2)
        res = dec.decode(msgspec.json.encode([tag, 1, 2, 3]))
        assert res == Test(1, 2, 3)

        # Trailing fields ignored
        res = dec.decode(msgspec.json.encode([tag, 1, 2, 3, 4]))
        assert res == Test(1, 2, 3)

        # Missing required field errors
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(msgspec.json.encode([tag, 1]))
        assert "Expected `array` of at least length 3, got 2" in str(rec.value)

        # Tag missing
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(msgspec.json.encode([]))
        assert "Expected `array` of at least length 3, got 0" in str(rec.value)

        # Tag incorrect type
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(msgspec.json.encode([123.456, 2, 3]))
        assert f"Expected `{type(tag).__name__}`" in str(rec.value)
        assert "`$[0]`" in str(rec.value)

        # Tag incorrect value
        bad = 0 if isinstance(tag, int) else "bad"
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(msgspec.json.encode([bad, 1, 2]))
        assert f"Invalid value {bad!r}" in str(rec.value)
        assert "`$[0]`" in str(rec.value)

        # Field incorrect type correct index
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(msgspec.json.encode([tag, "a", 2]))
        assert "Expected `int`, got `str`" in str(rec.value)
        assert "`$[1]`" in str(rec.value)

    @pytest.mark.parametrize("tag", ["Test", 123])
    def test_decode_tagged_empty_struct(self, tag):
        class Test(msgspec.Struct, tag=tag, array_like=True):
            pass

        dec = msgspec.json.Decoder(Test)

        # Decode with tag
        res = dec.decode(msgspec.json.encode([tag, 1, 2]))
        assert res == Test()

        # Tag missing
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(msgspec.json.encode([]))
        assert "Expected `array` of at least length 1, got 0" in str(rec.value)


class TestStructArrayUnion:
    """Most functionality is tested in `test_common.py:TestStructUnion`, this
    only checks for malformed inputs and whitespace handling"""

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"[,]", "invalid character"),
            (b"[, 1]", "invalid character"),
            (b"[nulp]", "invalid character"),
            (b'["Test1", nulp]', "invalid character"),
            (b"[", "truncated"),
            (b'["Test1', "truncated"),
            (b'["Test1"', "truncated"),
            (b'["Test1",', "truncated"),
            (b'["Test1]', "truncated"),
            (b'["Test1", ]', "trailing comma in array"),
            (b'["Test1" g', r"expected ',' or ']'"),
            (b'["Test1", 1 g', r"expected ',' or ']'"),
            (b'["Test1", 2 3]', r"expected ',' or ']'"),
        ],
    )
    def test_decode_struct_array_like_union_malformed(self, s, error):
        class Test1(msgspec.Struct, tag=True, array_like=True):
            x: int
            y: int
            z: int

        class Test2(msgspec.Struct, tag=True, array_like=True):
            pass

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Union[Test1, Test2])

    @pytest.mark.parametrize(
        "s, error",
        [
            (b"[,]", "invalid character"),
            (b"[, 1]", "invalid character"),
            (b"[nulp]", "invalid character"),
            (b"[123, nulp]", "invalid character"),
            (b"[", "truncated"),
            (b"[123.n,", "invalid number"),
            (b"[123en,", "invalid number"),
            (b"[123", "truncated"),
            (b"[123,", "truncated"),
            (b"[123, ]", "trailing comma in array"),
            (b"[123 g", r"expected ',' or ']'"),
            (b"[123, 1 g", r"expected ',' or ']'"),
        ],
    )
    def test_decode_struct_array_like_union_int_tag_malformed(self, s, error):
        class Test1(msgspec.Struct, tag=123, array_like=True):
            x: int
            y: int
            z: int

        class Test2(msgspec.Struct, tag=-123, array_like=True):
            pass

        with pytest.raises(msgspec.DecodeError, match=error):
            msgspec.json.decode(s, type=Union[Test1, Test2])

    def test_decode_struct_array_union_ignores_whitespace(self):
        s = b'  [  "Test1"  ,  1  ,  2  ]  '

        class Test1(msgspec.Struct, tag=True, array_like=True):
            a: int
            b: int

        class Test2(msgspec.Struct, tag=True, array_like=True):
            pass

        res = msgspec.json.decode(s, type=Union[Test1, Test2])
        assert res == Test1(1, 2)

    def test_decode_struct_array_union_int_tag_ignores_whitespace(self):
        s = b"  [  123  ,  1  ,  2  ]  "

        class Test1(msgspec.Struct, tag=123, array_like=True):
            a: int
            b: int

        class Test2(msgspec.Struct, tag=-123, array_like=True):
            pass

        res = msgspec.json.decode(s, type=Union[Test1, Test2])
        assert res == Test1(1, 2)


class TestRaw:
    def test_encode_raw(self):
        b = b'{"x":1}'
        r = msgspec.Raw(b)
        assert msgspec.json.encode(r) == b
        assert msgspec.json.encode({"y": r}) == b'{"y":{"x":1}}'

    def test_decode_raw_field(self):
        class Test(msgspec.Struct):
            x: int
            y: msgspec.Raw

        s = b'{"x": 1, "y": [1, 2, 3]  }'
        res = msgspec.json.decode(s, type=Test)
        assert res.x == 1
        assert bytes(res.y) == b"[1, 2, 3]"

    def test_decode_raw_optional_field(self):
        default = msgspec.Raw()

        class Test(msgspec.Struct):
            x: int
            y: msgspec.Raw = default

        s = b'{"x": 1, "y": [1, 2, 3]  }'
        res = msgspec.json.decode(s, type=Test)
        assert res.x == 1
        assert bytes(res.y) == b"[1, 2, 3]"

        s = b'{"x": 1}'
        res = msgspec.json.decode(s, type=Test)
        assert res.x == 1
        assert res.y is default

    def test_decode_raw_malformed_data(self):
        class Test(msgspec.Struct):
            x: int
            y: msgspec.Raw

        s = b'{"x": 1, "y": [1, 2,]}'
        with pytest.raises(msgspec.DecodeError, match="malformed"):
            msgspec.json.decode(s, type=Test)

    def test_decode_raw_is_view(self):
        s = b'   {"x": 1}   '
        r = msgspec.json.decode(s, type=msgspec.Raw)
        assert bytes(r) == b'{"x": 1}'
        assert r.copy() is not r  # actual copy indicates a view

    def test_raw_in_union_works_but_doesnt_change_anything(self):
        class Test(msgspec.Struct):
            x: Union[int, str, msgspec.Raw]

        r = msgspec.json.decode(b'{"x": 1}', type=Test)
        assert r == Test(1)

    def test_raw_can_be_mixed_with_custom_type(self):
        class Test(msgspec.Struct):
            x: Union[Custom, msgspec.Raw]

        def dec_hook(typ, obj):
            assert typ is Custom
            return typ(*obj)

        res = msgspec.json.decode(b'{"x": [1, 2]}', type=Test, dec_hook=dec_hook)
        assert res == Test(Custom(1, 2))
