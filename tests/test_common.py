from __future__ import annotations

import enum
import gc
import sys
import random
import string
import weakref
from typing import Literal, List, Union, Deque, NamedTuple, Dict, Tuple

import pytest

import msgspec


@pytest.fixture(params=["json", "msgpack"])
def decoder_cls(request):
    if request.param == "json":
        return msgspec.json.Decoder
    elif request.param == "msgpack":
        return msgspec.msgpack.Decoder


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


class Point(NamedTuple):
    x: float
    y: float


class Rand:
    """Random source, pulled out into fixture with repr so the seed is
    displayed on failing tests"""

    def __init__(self, seed=0):
        self.seed = seed or random.randint(0, 2 ** 32 - 1)
        self.rand = random.Random(self.seed)

    def __repr__(self):
        return f"Rand({self.seed})"

    def str(self, n, m=0):
        """
        randstr(N) -> random string of length N.
        randstr(N,M) -> random string between lengths N & M
        """
        if m:
            n = self.rand.randint(n, m)
        return "".join(self.rand.choices(string.ascii_letters, k=n))


@pytest.fixture
def rand():
    yield Rand()


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


class TestEnum:
    def test_empty_errors(self):
        class Empty(enum.Enum):
            pass

        with pytest.raises(TypeError, match="Enum types must have at least one item"):
            msgspec.msgpack.Decoder(Empty)

    def test_str_lookup_reused(self):
        class Test(enum.Enum):
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

    def test_str_lookup_gc(self):
        class Test(enum.Enum):
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

    def test_msgspec_lookup_overwritten(self):
        class Test(enum.Enum):
            A = 1

        Test.__msgspec_lookup__ = 1

        with pytest.raises(RuntimeError, match="__msgspec_lookup__"):
            msgspec.msgpack.Decoder(Test)

    @pytest.mark.parametrize("length", [2, 8, 16])
    @pytest.mark.parametrize("nitems", [1, 3, 6, 12, 24, 48])
    def test_random_enum_same_lengths(self, rand, length, nitems):
        def strgen(length):
            """Yields unique random fixed-length strings"""
            seen = set()
            while True:
                x = rand.str(length)
                if x in seen:
                    continue
                seen.add(x)
                yield x

        unique_str = strgen(length).__next__

        myenum = enum.Enum("myenum", [unique_str() for _ in range(nitems)])
        dec = msgspec.msgpack.Decoder(myenum)

        for val in myenum:
            msg = msgspec.msgpack.encode(val.name)
            val2 = dec.decode(msg)
            assert val == val2

        for _ in range(10):
            key = unique_str()
            with pytest.raises(msgspec.DecodeError):
                dec.decode(msgspec.msgpack.encode(key))

        # Try bad of different lengths
        for bad_length in [1, 7, 15, 30]:
            assert bad_length != length
            key = rand.str(bad_length)
            with pytest.raises(msgspec.DecodeError):
                dec.decode(msgspec.msgpack.encode(key))

    @pytest.mark.parametrize("nitems", [1, 3, 6, 12, 24, 48])
    def test_random_enum_different_lengths(self, rand, nitems):
        def strgen():
            """Yields unique random strings"""
            seen = set()
            while True:
                x = rand.str(1, 32)
                if x in seen:
                    continue
                seen.add(x)
                yield x

        unique_str = strgen().__next__

        myenum = enum.Enum("myenum", [unique_str() for _ in range(nitems)])
        dec = msgspec.msgpack.Decoder(myenum)

        for val in myenum:
            msg = msgspec.msgpack.encode(val.name)
            val2 = dec.decode(msg)
            assert val == val2

        for _ in range(10):
            key = unique_str()
            with pytest.raises(msgspec.DecodeError):
                dec.decode(msgspec.msgpack.encode(key))


class TestLiterals:
    def test_empty_errors(self):
        with pytest.raises(
            TypeError, match="Literal types must have at least one item"
        ):
            msgspec.msgpack.Decoder(Literal[()])

    @pytest.mark.parametrize(
        "values",
        [
            [0, 1, 2, 2 ** 64],
            [0, 1, 2, -(2 ** 63) - 1],
            [0, 1, 2, 2 ** 63 + 1, -(2 ** 64)],
        ],
    )
    def test_int_literal_values_out_of_range(self, values):
        literal = Literal[tuple(values)]

        with pytest.raises(OverflowError):
            msgspec.msgpack.Decoder(literal)

    @pytest.mark.parametrize(
        "typ",
        [
            Literal[1, False],
            Literal["ok", b"bad"],
            Literal[1, object()],
            Union[Literal[1, 2], Literal[3, False]],
            Union[Literal["one", "two"], Literal[3, False]],
            Literal[Literal[1, 2], Literal[3, False]],
            Literal[Literal["one", "two"], Literal[3, False]],
            Literal[1, 2, List[int]],
            Literal[1, 2, List],
        ],
    )
    def test_invalid_values(self, typ):
        with pytest.raises(TypeError, match="not supported"):
            msgspec.msgpack.Decoder(typ)

    def test_literal_valid_values(self):
        literal = Literal[1, "two", None]

        dec = msgspec.msgpack.Decoder(literal)
        assert literal.__msgspec_lookup__[0] is not None
        assert literal.__msgspec_lookup__[1] is not None

        for val in [1, "two", None]:
            assert dec.decode(msgspec.msgpack.encode(val)) == val

    @pytest.mark.parametrize(
        "values", [(1, 2), ("one", "two"), (1, 2, "three", "four")]
    )
    def test_caching(self, values):
        literal = Literal[values]

        dec = msgspec.msgpack.Decoder(literal)  # noqa

        int_lookup, str_lookup = literal.__msgspec_lookup__
        assert (int_lookup is not None) == any(isinstance(i, int) for i in values)
        assert (str_lookup is not None) == any(isinstance(i, str) for i in values)

        intcount = sys.getrefcount(int_lookup)
        strcount = sys.getrefcount(str_lookup)

        dec2 = msgspec.msgpack.Decoder(literal)

        if int_lookup is not None:
            assert sys.getrefcount(int_lookup) == intcount + 1
        if str_lookup is not None:
            assert sys.getrefcount(str_lookup) == strcount + 1

        # Reference count decreases when decoder is dropped
        del dec2
        gc.collect()

        if int_lookup is not None:
            assert sys.getrefcount(int_lookup) == intcount
        if str_lookup is not None:
            assert sys.getrefcount(str_lookup) == strcount

    @pytest.mark.parametrize("val", [None, (), (1,), (1, 2), (1, 2, 3)])
    def test_msgspec_lookup_overwritten(self, val):
        literal = Literal["a", "highly", "improbable", "set", "of", "strings"]

        literal.__msgspec_lookup__ = val

        with pytest.raises(RuntimeError, match="__msgspec_lookup__"):
            msgspec.msgpack.Decoder(literal)

    def test_multiple_literals(self):
        integers = Literal[-1, -2, -3]
        strings = Literal["apple", "banana"]
        both = Union[integers, strings]

        dec = msgspec.msgpack.Decoder(both)

        assert not hasattr(both, "__msgspec_lookup__")

        for val in [-1, -2, -3, "apple", "banana"]:
            assert dec.decode(msgspec.msgpack.encode(val)) == val

        with pytest.raises(msgspec.DecodeError, match="Invalid enum value `4`"):
            dec.decode(msgspec.msgpack.encode(4))

        with pytest.raises(msgspec.DecodeError, match="Invalid enum value 'carrot'"):
            dec.decode(msgspec.msgpack.encode("carrot"))

    def test_nested_literals(self):
        """Python 3.9+ automatically denest literals, can drop this test when
        python 3.8 is dropped"""
        integers = Literal[-1, -2, -3]
        strings = Literal["apple", "banana"]
        both = Literal[integers, strings]

        dec = msgspec.msgpack.Decoder(both)

        assert hasattr(both, "__msgspec_lookup__")

        for val in [-1, -2, -3, "apple", "banana"]:
            assert dec.decode(msgspec.msgpack.encode(val)) == val

        with pytest.raises(msgspec.DecodeError, match="Invalid enum value `4`"):
            dec.decode(msgspec.msgpack.encode(4))

        with pytest.raises(msgspec.DecodeError, match="Invalid enum value 'carrot'"):
            dec.decode(msgspec.msgpack.encode("carrot"))


class TestUnionTypeErrors:
    def test_decoder_unsupported_type(self, decoder_cls):
        with pytest.raises(TypeError):
            decoder_cls(1)

    def test_decoder_validates_struct_definition_unsupported_types(self, decoder_cls):
        """Struct definitions aren't validated until first use"""

        class Test(msgspec.Struct):
            a: 1

        with pytest.raises(TypeError):
            decoder_cls(Test)

    @pytest.mark.parametrize("typ", [Union[int, Deque], Union[Deque, int]])
    def test_err_union_with_custom_type(self, typ, decoder_cls):
        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)
        assert "custom type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("typ", [Union[dict, Person], Union[Person, dict]])
    def test_err_union_with_struct_and_dict(self, typ, decoder_cls):
        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)
        assert "both a Struct type and a dict type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("typ", [Union[PersonAA, list], Union[tuple, PersonAA]])
    def test_err_union_with_struct_asarray_and_array(self, typ, decoder_cls):
        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)
        assert "asarray=True" in str(rec.value)
        assert "Type unions containing" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("types", [(FruitInt, int), (FruitInt, Literal[1, 2])])
    def test_err_union_with_multiple_int_like_types(self, types, decoder_cls):
        typ = Union[types]
        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)
        assert "int-like" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "types", [(FruitStr, str), (FruitStr, Literal["one", "two"])]
    )
    def test_err_union_with_multiple_str_like_types(self, types, decoder_cls):
        typ = Union[types]
        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)
        assert "str-like" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "typ,kind",
        [
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
    def test_err_union_conflicts(self, typ, kind, decoder_cls):
        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)
        assert f"more than one {kind}" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.skipif(sys.version_info[:2] < (3, 10), reason="3.10 only")
    def test_310_union_types(self, decoder_cls):
        dec = decoder_cls(int | str | None)
        assert dec.decode(b"1") == 1
        assert dec.decode(b'"abc"') == "abc"
        assert dec.decode(b"null") is None
        with pytest.raises(msgspec.DecodeError):
            dec.decode(b"1.5")


class TestStructUnion:
    def test_err_union_struct_mix_asarray(self, decoder_cls):
        class Test1(msgspec.Struct, tag=True, asarray=True):
            x: int

        class Test2(msgspec.Struct, tag=True, asarray=False):
            x: int

        typ = Union[Test1, Test2]

        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)

        assert "not supported" in str(rec.value)
        assert "asarray" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("asarray", [False, True])
    @pytest.mark.parametrize("tag1", [False, True])
    def test_err_union_struct_not_tagged(self, asarray, tag1, decoder_cls):
        class Test1(msgspec.Struct, tag=tag1, asarray=asarray):
            x: int

        class Test2(msgspec.Struct, asarray=asarray):
            x: int

        typ = Union[Test1, Test2]

        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)

        assert "not supported" in str(rec.value)
        assert "must be tagged" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("asarray", [False, True])
    def test_err_union_struct_different_fields(self, decoder_cls, asarray):
        class Test1(msgspec.Struct, tag_field="foo", asarray=asarray):
            x: int

        class Test2(msgspec.Struct, tag_field="bar", asarray=asarray):
            x: int

        typ = Union[Test1, Test2]

        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)

        assert "not supported" in str(rec.value)
        assert "the same `tag_field`" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("asarray", [False, True])
    @pytest.mark.parametrize(
        "tags", [("a", "b", "b"), ("a", "a", "b"), ("a", "b", "a")]
    )
    def test_err_union_struct_non_unique_tag_values(self, decoder_cls, asarray, tags):
        class Test1(msgspec.Struct, tag=tags[0], asarray=asarray):
            x: int

        class Test2(msgspec.Struct, tag=tags[1], asarray=asarray):
            x: int

        class Test3(msgspec.Struct, tag=tags[2], asarray=asarray):
            x: int

        typ = Union[Test1, Test2, Test3]

        with pytest.raises(TypeError) as rec:
            decoder_cls(typ)

        assert "not supported" in str(rec.value)
        assert "unique `tag`" in str(rec.value)
        assert repr(typ) in str(rec.value)
