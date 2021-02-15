import math
import sys

import pytest

import msgspec

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


class TestCompatibility:
    """Test compatibility with the existing python msgpack library"""
    def check(self, x):
        msgpack = pytest.importorskip("msgpack")

        enc = msgspec.Encoder()
        dec = msgspec.Decoder()

        assert_eq(dec.decode(msgpack.dumps(x)), x)
        assert_eq(msgpack.loads(enc.encode(x)), x)

    def test_none(self):
        self.check(None)

    @pytest.mark.parametrize("x", [None, False, True])
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
