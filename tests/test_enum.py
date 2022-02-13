import enum
import gc
import sys
import weakref

import pytest

import msgspec


class TestIntEnum:
    def test_empty_errors(self):
        class Empty(enum.IntEnum):
            pass

        with pytest.raises(TypeError, match="Enum types must have at least one item"):
            msgspec.msgpack.Decoder(Empty)

    def test_int_lookup_reused(self):
        class Test(enum.IntEnum):
            A = 1
            B = 2

        dec = msgspec.msgpack.Decoder(Test)  # noqa
        count = sys.getrefcount(Test.__msgspec_lookup__)
        dec2 = msgspec.msgpack.Decoder(Test)
        count2 = sys.getrefcount(Test.__msgspec_lookup__)
        assert count2 == count + 1

        # Reference count decreases when decoder is dropped
        del dec2
        gc.collect()
        count3 = sys.getrefcount(Test.__msgspec_lookup__)
        assert count == count3

    def test_int_lookup_gc(self):
        class Test(enum.IntEnum):
            A = 1
            B = 2

        dec = msgspec.msgpack.Decoder(Test)
        assert gc.is_tracked(Test.__msgspec_lookup__)

        # Deleting all references and running GC cleans up cycle
        ref = weakref.ref(Test)
        del Test
        del dec
        gc.collect()
        assert ref() is None

    @pytest.mark.parametrize(
        "values",
        [
            [0, 1, 2, 2 ** 64],
            [0, 1, 2, -(2 ** 63) - 1],
            [0, 1, 2, 2 ** 63 + 1, -(2 ** 64)],
        ],
    )
    def test_int_lookup_values_out_of_range(self, values):
        myenum = enum.IntEnum("myenum", [(f"x{i}", v) for i, v in enumerate(values)])

        with pytest.raises(OverflowError):
            msgspec.msgpack.Decoder(myenum)

    def test_msgspec_lookup_overwritten(self):
        class Test(enum.IntEnum):
            A = 1

        Test.__msgspec_lookup__ = 1

        with pytest.raises(RuntimeError, match="__msgspec_lookup__"):
            msgspec.msgpack.Decoder(Test)

    @pytest.mark.parametrize(
        "values",
        [
            [0],
            [1],
            [-1],
            [3, 4, 5, 2, 1],
            [4, 3, 1, 2, 7],
            [-4, -3, -2, -1, 0, 1, 2, 3, 4],
            [-4, -3, -1, -2, -7],
            [-4, -3, 1, 0, -2, -1],
            [2 ** 63 - 1, 2 ** 63 - 2, 2 ** 63 - 3],
            [-(2 ** 63) + 1, -(2 ** 63) + 2, -(2 ** 63) + 3],
        ],
    )
    def test_compact(self, values):
        myenum = enum.IntEnum("myenum", [(f"x{i}", v) for i, v in enumerate(values)])
        dec = msgspec.msgpack.Decoder(myenum)

        assert hasattr(myenum, "__msgspec_lookup__")

        for val in myenum:
            msg = msgspec.msgpack.encode(val)
            val2 = dec.decode(msg)
            assert val == val2

        for bad in [-1000, min(values) - 1, max(values) + 1, 1000]:
            with pytest.raises(msgspec.DecodeError):
                dec.decode(msgspec.msgpack.encode(bad))

    @pytest.mark.parametrize(
        "values",
        [
            [-(2 ** 63), 2 ** 63 - 1, 0],
            [-(2 ** 63), 2 ** 64 - 1, 0],
            [2 ** 64 - 2, 2 ** 64 - 3, 2 ** 64 - 1],
            [2 ** 64 - 2, 2 ** 64 - 3, 2 ** 64 - 1, 0, 2, 3, 4, 5, 6],
        ],
    )
    def test_hashtable(self, values):
        myenum = enum.IntEnum("myenum", [(f"x{i}", v) for i, v in enumerate(values)])
        dec = msgspec.msgpack.Decoder(myenum)

        assert hasattr(myenum, "__msgspec_lookup__")

        for val in myenum:
            msg = msgspec.msgpack.encode(val)
            val2 = dec.decode(msg)
            assert val == val2

        for bad in [-2000, -1, 1, 2000]:
            with pytest.raises(msgspec.DecodeError):
                dec.decode(msgspec.msgpack.encode(bad))

    @pytest.mark.parametrize(
        "values",
        [
            [8, 16, 24, 32, 40, 48],
            [-8, -16, -24, -32, -40, -48],
        ],
    )
    def test_hashtable_collisions(self, values):
        myenum = enum.IntEnum("myenum", [(f"x{i}", v) for i, v in enumerate(values)])
        dec = msgspec.msgpack.Decoder(myenum)

        for val in myenum:
            msg = msgspec.msgpack.encode(val)
            val2 = dec.decode(msg)
            assert val == val2

        for bad in [0, 7, 9, 56, -min(values), -max(values), 2 ** 64 - 1, -(2 ** 63)]:
            with pytest.raises(msgspec.DecodeError):
                dec.decode(msgspec.msgpack.encode(bad))
