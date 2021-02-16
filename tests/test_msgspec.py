import enum
import math
import sys

import pytest

import msgspec

class FruitInt(enum.IntEnum):
    APPLE = 1
    BANANA = 2


class FruitStr(enum.Enum):
    APPLE = "apple"
    BANANA = "banana"


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


class TestEncoderErrors:
    @pytest.mark.parametrize("x", [-2 ** 63 - 1, 2 ** 64])
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


def test_enum():
    enc = msgspec.Encoder()
    dec = msgspec.Decoder(FruitStr)

    a = enc.encode(FruitStr.APPLE)
    assert enc.encode("APPLE") == a
    assert dec.decode(a) == FruitStr.APPLE

    with pytest.raises(ValueError, match="truncated"):
        dec.decode(a[:-2])
    with pytest.raises(TypeError, match="Error decoding enum `FruitStr`"):
        dec.decode(enc.encode("MISSING"))
    with pytest.raises(TypeError):
        dec.decode(enc.encode(1))


def test_int_enum():
    enc = msgspec.Encoder()
    dec = msgspec.Decoder(FruitInt)

    a = enc.encode(FruitInt.APPLE)
    assert enc.encode(1) == a
    assert dec.decode(a) == FruitInt.APPLE

    with pytest.raises(ValueError, match="truncated"):
        dec.decode(a[:-2])
    with pytest.raises(TypeError, match="Error decoding enum `FruitInt`"):
        dec.decode(enc.encode(1000))
    with pytest.raises(TypeError):
        dec.decode(enc.encode("INVALID"))


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
    def test_bytearray(self, size):
        self.check(bytearray(size))

    @pytest.mark.parametrize("size", SIZES)
    def test_dict(self, size):
        self.check({str(i): i for i in range(size)})

    @pytest.mark.parametrize("size", SIZES)
    def test_list(self, size):
        self.check(list(range(size)))


class TestUntypedRoundtripCommon(CommonTypeTestBase):
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
