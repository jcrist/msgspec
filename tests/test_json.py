from __future__ import annotations

import enum
import base64
import datetime
import itertools
import gc
import json
import math
import random
import struct
import sys
import uuid
import types
import textwrap
from contextlib import contextmanager
from typing import Any, List, Set, Tuple, Union, Dict, Optional

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


class Node(msgspec.Struct):
    left: Optional[Node] = None
    right: Optional[Node] = None


@contextmanager
def temp_module(code):
    """Mutually recursive struct types defined inside functions don't work (and
    probably never will). To avoid populating a bunch of test structs in the
    top level of this module, we instead create a temporary module per test to
    exec whatever is needed for that test"""
    code = textwrap.dedent(code)
    name = f"temp_{uuid.uuid4().hex}"
    ns = {"__name__": name}
    exec(code, ns)
    mod = types.ModuleType(name)
    for k, v in ns.items():
        setattr(mod, k, v)

    try:
        sys.modules[name] = mod
        yield mod
    finally:
        sys.modules.pop(name, None)


class TestInvalidJSONTypes:
    def test_invalid_type_union(self):
        types = [str, datetime.datetime, bytes, bytearray]
        for length in [2, 3, 4]:
            for types in itertools.combinations(types, length):
                if set(types) == {bytes, bytearray}:
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
    @pytest.mark.parametrize("x", [-(2 ** 63) - 1, 2 ** 64])
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


class TestLiterals:
    def test_encode_none(self):
        assert msgspec.json.encode(None) == b"null"

    def test_decode_none(self):
        assert msgspec.json.decode(b"null") is None
        assert msgspec.json.decode(b"   null   ") is None

    @pytest.mark.parametrize("s", [b"nul", b"nulll", b"nuul", b"nulp"])
    def test_decode_none_malformed(self, s):
        with pytest.raises(msgspec.DecodingError):
            msgspec.json.decode(s)

    def test_decode_none_typed(self):
        with pytest.raises(
            msgspec.DecodingError, match="Expected `int | null`, got `str`"
        ):
            msgspec.json.decode(b'"test"', type=Union[int, None])

    def test_encode_true(self):
        assert msgspec.json.encode(True) == b"true"

    def test_decode_true(self):
        assert msgspec.json.decode(b"true") is True
        assert msgspec.json.decode(b"   true   ") is True

    @pytest.mark.parametrize("s", [b"tru", b"truee", b"trru", b"trup"])
    def test_decode_true_malformed(self, s):
        with pytest.raises(msgspec.DecodingError):
            msgspec.json.decode(s)

    def test_encode_false(self):
        assert msgspec.json.encode(False) == b"false"

    def test_decode_false(self):
        assert msgspec.json.decode(b"false") is False
        assert msgspec.json.decode(b"   false   ") is False

    @pytest.mark.parametrize("s", [b"fals", b"falsee", b"faase", b"falsp"])
    def test_decode_false_malformed(self, s):
        with pytest.raises(msgspec.DecodingError):
            msgspec.json.decode(s)

    def test_decode_bool_typed(self):
        with pytest.raises(msgspec.DecodingError, match="Expected `bool`, got `str`"):
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
        with pytest.raises(msgspec.DecodingError, match=error):
            msgspec.json.decode(s)

    def test_decode_str_invalid_byte(self):
        with pytest.raises(msgspec.DecodingError, match="invalid character"):
            msgspec.json.decode(b'"123 \x00 456"')

        with pytest.raises(msgspec.DecodingError, match="invalid character"):
            msgspec.json.decode(b'"123 \x01 456"')

    def test_decode_str_missing_closing_quote(self):
        with pytest.raises(msgspec.DecodingError, match="truncated"):
            msgspec.json.decode(b'"test')


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
            msgspec.DecodingError, match="Invalid base64 encoded string"
        ):
            msgspec.json.decode(s, type=bytes)


class TestEnum:
    def test_encode_enum(self):
        s = msgspec.json.encode(FruitStr.APPLE)
        assert s == b'"APPLE"'

    def test_decode_enum(self):
        x = msgspec.json.decode(b'"APPLE"', type=FruitStr)
        assert x == FruitStr.APPLE

    def test_decode_enum_invalid_value(self):
        with pytest.raises(msgspec.DecodingError, match="Invalid enum value 'MISSING'"):
            msgspec.json.decode(b'"MISSING"', type=FruitStr)

    def test_decode_enum_invalid_value_nested(self):
        class Test(msgspec.Struct):
            fruit: FruitStr

        with pytest.raises(
            msgspec.DecodingError, match=r"Invalid enum value 'MISSING' - at `\$.fruit`"
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

    @pytest.mark.parametrize("x", [-(2 ** 63), 2 ** 64 - 1])
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

    @pytest.mark.parametrize("x", [-(2 ** 63), 2 ** 64 - 1])
    def test_decode_int_boundaries(self, x):
        s = str(x).encode()
        x2 = msgspec.json.decode(s)
        assert isinstance(x2, int)
        assert x2 == x

    @pytest.mark.parametrize("x", [-(2 ** 63) - 1, 2 ** 64])
    def test_decode_int_out_of_range_convert_to_float(self, x):
        s = str(x).encode()
        x2 = msgspec.json.decode(s)
        assert isinstance(x2, float)
        assert x2 == float(x)

    @pytest.mark.parametrize("x", [-(2 ** 63) - 1, 2 ** 64])
    def test_decode_int_out_of_range_errors_if_int_requested(self, x):
        s = str(x).encode()
        with pytest.raises(msgspec.DecodingError, match="Expected `int`, got `float`"):
            msgspec.json.decode(s, type=int)

    @pytest.mark.parametrize("s", [b"   123   ", b"   -123   "])
    def test_decode_int_whitespace(self, s):
        assert msgspec.json.decode(s) == int(s)

    @pytest.mark.parametrize("s", [b"- 123", b"-n123", b"1 2", b"12n3", b"123n"])
    def test_decode_int_malformed(self, s):
        with pytest.raises(msgspec.DecodingError):
            msgspec.json.decode(s)

    def test_decode_int_converts_to_float_if_requested(self):
        x = msgspec.json.decode(b"123", type=float)
        assert isinstance(x, float)
        assert x == 123.0
        x = msgspec.json.decode(b"-123", type=float)
        assert isinstance(x, float)
        assert x == -123.0

    def test_decode_int_type_error(self):
        with pytest.raises(msgspec.DecodingError, match="Expected `str`, got `int`"):
            msgspec.json.decode(b"123", type=str)


class TestIntEnum:
    def test_encode_intenum(self):
        s = msgspec.json.encode(FruitInt.APPLE)
        assert s == b"1"

    def test_decode_intenum(self):
        x = msgspec.json.decode(b"1", type=FruitInt)
        assert x == FruitInt.APPLE

    def test_decode_intenum_invalid_value(self):
        with pytest.raises(msgspec.DecodingError, match="Invalid enum value `3`"):
            msgspec.json.decode(b"3", type=FruitInt)

    def test_decode_intenum_invalid_value_nested(self):
        class Test(msgspec.Struct):
            fruit: FruitInt

        with pytest.raises(
            msgspec.DecodingError, match=r"Invalid enum value `3` - at `\$.fruit`"
        ):
            msgspec.json.decode(b'{"fruit": 3}', type=Test)


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
            2.0 ** -24,
            2.0 ** -14,
            2.0 ** -149,
            2.0 ** -126,
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
        x = 10.0 ** n
        s = msgspec.json.encode(x)
        x2 = msgspec.json.decode(s)
        assert x == x2

    @pytest.mark.parametrize("n", range(-15, 14))
    def test_roundtrip_float_lots_of_middle_zeros(self, n):
        x = 1e15 + 10.0 ** n
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
        x = (2.0 ** n) * scale
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
            str(2 ** 64 - 1).encode(),  # 20 digits, no overflow
            str(2 ** 64).encode(),  # 20 digits, overflow
            str(2 ** 64 + 1).encode(),  # 20 digits, overflow
            str(2 ** 68).encode(),  # 21 digits
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
        with pytest.raises(msgspec.DecodingError, match="Expected `int`, got `float`"):
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
        with pytest.raises(msgspec.DecodingError, match="number out of range"):
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
        with pytest.raises(msgspec.DecodingError, match=error):
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
        with pytest.raises(msgspec.DecodingError, match=error):
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
        with pytest.raises(msgspec.DecodingError, match="out of range"):
            msgspec.json.decode(s)

    @pytest.mark.parametrize("s", [b"1.23e3", b"1.2", b"1e2"])
    def test_decode_float_err_expected_int(self, s):
        with pytest.raises(msgspec.DecodingError, match="Expected `int`, got `float`"):
            msgspec.json.decode(s, type=int)


class TestSequences:
    @pytest.mark.parametrize("x", [[], [1], [1, "two", False]])
    @pytest.mark.parametrize("type", [list, set, tuple])
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
    @pytest.mark.parametrize("type", [list, set, tuple])
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

    def test_decode_typed_vartuple(self):
        dec = msgspec.json.Decoder(Tuple[int, ...])
        assert dec.decode(b"[]") == ()
        assert dec.decode(b"[1]") == (1,)
        assert dec.decode(b"[1,2]") == (
            1,
            2,
        )

    @pytest.mark.parametrize("type", [List[int], Set[int], Tuple[int, ...]])
    def test_decode_typed_list_wrong_element_type(self, type):
        dec = msgspec.json.Decoder(type)
        with pytest.raises(
            msgspec.DecodingError, match=r"Expected `int`, got `str` - at `\$\[1\]`"
        ):
            dec.decode(b'[1, "oops"]')

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
        with pytest.raises(msgspec.DecodingError, match=error):
            msgspec.json.decode(s, type=type)

    def test_decode_fixtuple_any(self):
        dec = msgspec.json.Decoder(Tuple[Any, Any, Any])
        x = (1, "two", False)
        res = dec.decode(b'[1, "two", false]')
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="Expected `array`, got `int`"):
            dec.decode(b"1")
        with pytest.raises(msgspec.DecodingError, match="Expected `array` of length 3"):
            dec.decode(b'[1, "two"]')

    def test_decode_fixtuple_typed(self):
        dec = msgspec.json.Decoder(Tuple[int, str, bool])
        x = (1, "two", False)
        res = dec.decode(b'[1, "two", false]')
        assert res == x
        with pytest.raises(msgspec.DecodingError, match="Expected `bool`"):
            dec.decode(b'[1, "two", "three"]')
        with pytest.raises(msgspec.DecodingError, match="Expected `array` of length 3"):
            dec.decode(b'[1, "two"]')


class TestDict:
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
            msgspec.DecodingError, match=r"Expected `int`, got `str` - at `\$\[...\]`"
        ):
            dec.decode(b'{"a": "bad"}')

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
        with pytest.raises(msgspec.DecodingError, match=error):
            msgspec.json.decode(s, type=type)


class TestStruct:
    def test_encode_empty_struct(self):
        class Test(msgspec.Struct):
            pass

        s = msgspec.json.encode(Test())
        assert s == b"{}"

    def test_encode_one_field_struct(self):
        class Test(msgspec.Struct):
            a: int

        s = msgspec.json.encode(Test(a=1))
        assert s == b'{"a":1}'

    def test_encode_two_field_struct(self):
        class Test(msgspec.Struct):
            a: int
            b: str

        s = msgspec.json.encode(Test(a=1, b="two"))
        assert s == b'{"a":1,"b":"two"}'

    def test_decode_struct(self):
        dec = msgspec.json.Decoder(Person)
        msg = b'{"first": "harry", "last": "potter", "age": 13, "prefect": false}'
        x = dec.decode(msg)
        assert x == Person("harry", "potter", 13, False)

        with pytest.raises(msgspec.DecodingError, match="Expected `object`, got `int`"):
            dec.decode(b"1")

    def test_decode_struct_field_wrong_type(self):
        dec = msgspec.json.Decoder(Person)

        msg = b'{"first": "harry", "last": "potter", "age": "bad"}'
        with pytest.raises(
            msgspec.DecodingError, match=r"Expected `int`, got `str` - at `\$.age`"
        ):
            dec.decode(msg)

    def test_decode_struct_missing_fields(self):
        bad = b'{"first": "harry", "last": "potter"}'
        with pytest.raises(
            msgspec.DecodingError, match="Object missing required field `age`"
        ):
            msgspec.json.decode(bad, type=Person)

        bad = b"{}"
        with pytest.raises(
            msgspec.DecodingError, match="Object missing required field `first`"
        ):
            msgspec.json.decode(bad, type=Person)

        bad = b'[{"first": "harry", "last": "potter"}]'
        with pytest.raises(
            msgspec.DecodingError,
            match=r"Object missing required field `age` - at `\$\[0\]`",
        ):
            msgspec.json.decode(bad, type=List[Person])

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
            (b'{"age": 13, "first" 2}', "expected ':'"),
            (b'{"age": 13, "first": 2  "c"}', r"expected ',' or '}'"),
        ],
    )
    @pytest.mark.parametrize("type", [dict, Any])
    def test_decode_struct_malformed(self, s, error, type):
        with pytest.raises(msgspec.DecodingError, match=error):
            msgspec.json.decode(s, type=type)


class TestStructArray:
    def test_struct_asarray(self):
        dec = msgspec.json.Decoder(PersonAA)

        x = PersonAA(first="harry", last="potter", age=13)
        a = msgspec.json.encode(x)
        assert msgspec.json.encode(("harry", "potter", 13, False)) == a
        assert dec.decode(a) == x

        with pytest.raises(msgspec.DecodingError, match="Expected `array`, got `int`"):
            dec.decode(b"1")

        # Wrong field type
        bad = msgspec.json.encode(("harry", "potter", "thirteen"))
        with pytest.raises(
            msgspec.DecodingError, match=r"Expected `int`, got `str` - at `\$\[2\]`"
        ):
            dec.decode(bad)

        # Missing fields
        bad = msgspec.json.encode(("harry", "potter"))
        with pytest.raises(msgspec.DecodingError, match="missing required field `age`"):
            dec.decode(bad)

        bad = msgspec.json.encode(())
        with pytest.raises(
            msgspec.DecodingError, match="missing required field `first`"
        ):
            dec.decode(bad)

        # Extra fields ignored
        dec2 = msgspec.json.Decoder(List[PersonAA])
        msg = msgspec.json.encode(
            [
                ("harry", "potter", 13, False, 1, 2, 3, 4),
                ("ron", "weasley", 13, False, 5, 6),
            ]
        )
        res = dec2.decode(msg)
        assert res == [PersonAA("harry", "potter", 13), PersonAA("ron", "weasley", 13)]

        # Defaults applied
        res = dec.decode(msgspec.json.encode(("harry", "potter", 13)))
        assert res == PersonAA("harry", "potter", 13)
        assert res.prefect is False

    def test_struct_map_and_asarray_messages_cant_mix(self):
        array_msg = msgspec.json.encode(("harry", "potter", 13))
        map_msg = msgspec.json.encode({"first": "harry", "last": "potter", "age": 13})
        sol = Person("harry", "potter", 13)
        array_sol = PersonAA("harry", "potter", 13)

        dec = msgspec.json.Decoder(Person)
        array_dec = msgspec.json.Decoder(PersonAA)

        assert array_dec.decode(array_msg) == array_sol
        assert dec.decode(map_msg) == sol
        with pytest.raises(
            msgspec.DecodingError, match="Expected `object`, got `array`"
        ):
            dec.decode(array_msg)
        with pytest.raises(
            msgspec.DecodingError, match="Expected `array`, got `object`"
        ):
            array_dec.decode(map_msg)

    @pytest.mark.parametrize("asarray", [False, True])
    def test_struct_gc_maybe_untracked_on_decode(self, asarray):
        class Test(msgspec.Struct, asarray=asarray):
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

    def test_struct_recursive_definition(self):
        enc = msgspec.json.Encoder()
        dec = msgspec.json.Decoder(Node)

        x = Node(Node(Node(), Node(Node())))
        s = enc.encode(x)
        res = dec.decode(s)
        assert res == x
