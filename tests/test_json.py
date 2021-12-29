from __future__ import annotations

import base64
import json
import math
import sys

import pytest

import msgspec


# TODO: these values don't fully cover the possible branches/lookups in the
# int/float encoding routines. These lists should be expanded at some point to
# provide better coverage.
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

SIZES = [0, 1, 10]


def assert_eq(x, y):
    if isinstance(y, float) and not math.isfinite(y):
        # all non-finite numbers are serialized as null
        assert x is None
    else:
        assert x == y


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

    def tesT_encode_into_handles_errors_properly(self):
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

    @pytest.mark.parametrize("size", SIZES)
    def test_encode_memoryview(self, size):
        """We don't support memoryview as a decode type, just check we can
        encode them fine"""
        buf = bytearray(size)
        msg = msgspec.json.encode(memoryview(buf))
        res = base64.b64decode(json.loads(msg))
        assert buf == res


class TestCompatibility:
    """Test compatibility with the existing python orjson library"""

    def check(self, x):
        orjson = pytest.importorskip("orjson")
        res = orjson.loads(msgspec.json.encode(x))
        if isinstance(x, bytes):
            res = base64.b64decode(res)
        assert_eq(res, x)

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
