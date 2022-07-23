from __future__ import annotations

import enum
import gc
import sys
import random
import string
import weakref
from typing import (
    Literal,
    List,
    Union,
    Deque,
    NamedTuple,
    Dict,
    Tuple,
    Optional,
    TypedDict,
)

import pytest

import msgspec

from utils import temp_module


@pytest.fixture(params=["json", "msgpack"])
def proto(request):
    if request.param == "json":
        return msgspec.json
    elif request.param == "msgpack":
        return msgspec.msgpack


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


class PersonArray(msgspec.Struct, array_like=True):
    first: str
    last: str
    age: int


class PersonDict(TypedDict):
    first: str
    last: str
    age: int


class Point(NamedTuple):
    x: float
    y: float


class Rand:
    """Random source, pulled out into fixture with repr so the seed is
    displayed on failing tests"""

    def __init__(self, seed=0):
        self.seed = seed or random.randint(0, 2**32 - 1)
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
        count = sys.getrefcount(Test.__msgspec_cache__)
        dec2 = msgspec.msgpack.Decoder(Test)
        count2 = sys.getrefcount(Test.__msgspec_cache__)
        assert count2 == count + 1

        # Reference count decreases when decoder is dropped
        del dec2
        gc.collect()
        count3 = sys.getrefcount(Test.__msgspec_cache__)
        assert count == count3

    def test_int_lookup_gc(self):
        class Test(enum.IntEnum):
            A = 1
            B = 2

        dec = msgspec.msgpack.Decoder(Test)
        assert gc.is_tracked(Test.__msgspec_cache__)

        # Deleting all references and running GC cleans up cycle
        ref = weakref.ref(Test)
        del Test
        del dec
        gc.collect()
        assert ref() is None

    @pytest.mark.parametrize(
        "values",
        [
            [0, 1, 2, -(2**63) - 1],
            [0, 1, 2, 2**63],
        ],
    )
    def test_int_lookup_values_out_of_range(self, values):
        myenum = enum.IntEnum("myenum", [(f"x{i}", v) for i, v in enumerate(values)])

        with pytest.raises(NotImplementedError):
            msgspec.msgpack.Decoder(myenum)

    def test_msgspec_cache_overwritten(self):
        class Test(enum.IntEnum):
            A = 1

        Test.__msgspec_cache__ = 1

        with pytest.raises(RuntimeError, match="__msgspec_cache__"):
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
            [2**63 - 1, 2**63 - 2, 2**63 - 3],
            [-(2**63) + 1, -(2**63) + 2, -(2**63) + 3],
        ],
    )
    def test_compact(self, values):
        myenum = enum.IntEnum("myenum", [(f"x{i}", v) for i, v in enumerate(values)])
        dec = msgspec.msgpack.Decoder(myenum)

        assert hasattr(myenum, "__msgspec_cache__")

        for val in myenum:
            msg = msgspec.msgpack.encode(val)
            val2 = dec.decode(msg)
            assert val == val2

        for bad in [-1000, min(values) - 1, max(values) + 1, 1000]:
            with pytest.raises(msgspec.ValidationError):
                dec.decode(msgspec.msgpack.encode(bad))

    @pytest.mark.parametrize(
        "values",
        [
            [-(2**63), 2**63 - 1, 0],
            [2**63 - 2, 2**63 - 3, 2**63 - 1],
            [2**63 - 2, 2**63 - 3, 2**63 - 1, 0, 2, 3, 4, 5, 6],
        ],
    )
    def test_hashtable(self, values):
        myenum = enum.IntEnum("myenum", [(f"x{i}", v) for i, v in enumerate(values)])
        dec = msgspec.msgpack.Decoder(myenum)

        assert hasattr(myenum, "__msgspec_cache__")

        for val in myenum:
            msg = msgspec.msgpack.encode(val)
            val2 = dec.decode(msg)
            assert val == val2

        for bad in [-2000, -1, 1, 2000]:
            with pytest.raises(msgspec.ValidationError):
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

        for bad in [0, 7, 9, 56, -min(values), -max(values), 2**64 - 1, -(2**63)]:
            with pytest.raises(msgspec.ValidationError):
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
        count = sys.getrefcount(Test.__msgspec_cache__)
        dec2 = msgspec.msgpack.Decoder(Test)
        count2 = sys.getrefcount(Test.__msgspec_cache__)
        assert count2 == count + 1

        # Reference count decreases when decoder is dropped
        del dec2
        gc.collect()
        count3 = sys.getrefcount(Test.__msgspec_cache__)
        assert count == count3

    def test_str_lookup_gc(self):
        class Test(enum.Enum):
            A = 1
            B = 2

        dec = msgspec.msgpack.Decoder(Test)
        assert gc.is_tracked(Test.__msgspec_cache__)

        # Deleting all references and running GC cleans up cycle
        ref = weakref.ref(Test)
        del Test
        del dec
        gc.collect()
        assert ref() is None

    def test_msgspec_cache_overwritten(self):
        class Test(enum.Enum):
            A = 1

        Test.__msgspec_cache__ = 1

        with pytest.raises(RuntimeError, match="__msgspec_cache__"):
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
            with pytest.raises(msgspec.ValidationError):
                dec.decode(msgspec.msgpack.encode(key))

        # Try bad of different lengths
        for bad_length in [1, 7, 15, 30]:
            assert bad_length != length
            key = rand.str(bad_length)
            with pytest.raises(msgspec.ValidationError):
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
            with pytest.raises(msgspec.ValidationError):
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
            [0, 1, 2, 2**63],
            [0, 1, 2, -(2**63) - 1],
        ],
    )
    def test_int_literal_values_out_of_range(self, values):
        literal = Literal[tuple(values)]

        with pytest.raises(NotImplementedError):
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
        assert literal.__msgspec_cache__[0] is not None
        assert literal.__msgspec_cache__[1] is not None

        for val in [1, "two", None]:
            assert dec.decode(msgspec.msgpack.encode(val)) == val

    @pytest.mark.parametrize(
        "values", [(1, 2), ("one", "two"), (1, 2, "three", "four")]
    )
    def test_caching(self, values):
        literal = Literal[values]

        dec = msgspec.msgpack.Decoder(literal)  # noqa

        int_lookup, str_lookup = literal.__msgspec_cache__
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
    def test_msgspec_cache_overwritten(self, val):
        literal = Literal["a", "highly", "improbable", "set", "of", "strings"]

        literal.__msgspec_cache__ = val

        with pytest.raises(RuntimeError, match="__msgspec_cache__"):
            msgspec.msgpack.Decoder(literal)

    def test_multiple_literals(self):
        integers = Literal[-1, -2, -3]
        strings = Literal["apple", "banana"]
        both = Union[integers, strings]

        dec = msgspec.msgpack.Decoder(both)

        assert not hasattr(both, "__msgspec_cache__")

        for val in [-1, -2, -3, "apple", "banana"]:
            assert dec.decode(msgspec.msgpack.encode(val)) == val

        with pytest.raises(msgspec.ValidationError, match="Invalid enum value 4"):
            dec.decode(msgspec.msgpack.encode(4))

        with pytest.raises(
            msgspec.ValidationError, match="Invalid enum value 'carrot'"
        ):
            dec.decode(msgspec.msgpack.encode("carrot"))

    def test_nested_literals(self):
        """Python 3.9+ automatically denest literals, can drop this test when
        python 3.8 is dropped"""
        integers = Literal[-1, -2, -3]
        strings = Literal["apple", "banana"]
        both = Literal[integers, strings]

        dec = msgspec.msgpack.Decoder(both)

        assert hasattr(both, "__msgspec_cache__")

        for val in [-1, -2, -3, "apple", "banana"]:
            assert dec.decode(msgspec.msgpack.encode(val)) == val

        with pytest.raises(msgspec.ValidationError, match="Invalid enum value 4"):
            dec.decode(msgspec.msgpack.encode(4))

        with pytest.raises(
            msgspec.ValidationError, match="Invalid enum value 'carrot'"
        ):
            dec.decode(msgspec.msgpack.encode("carrot"))


class TestUnionTypeErrors:
    def test_decoder_unsupported_type(self, proto):
        with pytest.raises(TypeError):
            proto.Decoder(1)

    def test_decoder_validates_struct_definition_unsupported_types(self, proto):
        """Struct definitions aren't validated until first use"""

        class Test(msgspec.Struct):
            a: 1

        with pytest.raises(TypeError):
            proto.Decoder(Test)

    @pytest.mark.parametrize("typ", [Union[int, Deque], Union[Deque, int]])
    def test_err_union_with_custom_type(self, typ, proto):
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert "custom type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "typ",
        [
            Union[dict, Person],
            Union[Person, dict],
            Union[PersonDict, dict],
            Union[Person, PersonDict],
        ],
    )
    def test_err_union_with_multiple_dict_like_types(self, typ, proto):
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert "more than one dict-like type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "typ", [Union[PersonArray, list], Union[tuple, PersonArray]]
    )
    def test_err_union_with_struct_array_like_and_array(self, typ, proto):
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert "array_like=True" in str(rec.value)
        assert "Type unions containing" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("types", [(FruitInt, int), (FruitInt, Literal[1, 2])])
    def test_err_union_with_multiple_int_like_types(self, types, proto):
        typ = Union[types]
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert "int-like" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "types", [(FruitStr, str), (FruitStr, Literal["one", "two"])]
    )
    def test_err_union_with_multiple_str_like_types(self, types, proto):
        typ = Union[types]
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
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
    def test_err_union_conflicts(self, typ, kind, proto):
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert f"more than one {kind}" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.skipif(sys.version_info[:2] < (3, 10), reason="3.10 only")
    def test_310_union_types(self, proto):
        dec = proto.Decoder(int | str | None)
        for msg in [1, "abc", None]:
            assert dec.decode(proto.encode(msg)) == msg
        with pytest.raises(msgspec.ValidationError):
            assert dec.decode(proto.encode(1.5))


class TestStructUnion:
    def test_err_union_struct_mix_array_like(self, proto):
        class Test1(msgspec.Struct, tag=True, array_like=True):
            x: int

        class Test2(msgspec.Struct, tag=True, array_like=False):
            x: int

        typ = Union[Test1, Test2]

        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)

        assert "not supported" in str(rec.value)
        assert "array_like" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("array_like", [False, True])
    @pytest.mark.parametrize("tag1", [False, True])
    def test_err_union_struct_not_tagged(self, array_like, tag1, proto):
        class Test1(msgspec.Struct, tag=tag1, array_like=array_like):
            x: int

        class Test2(msgspec.Struct, array_like=array_like):
            x: int

        typ = Union[Test1, Test2]

        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)

        assert "not supported" in str(rec.value)
        assert "must be tagged" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("array_like", [False, True])
    def test_err_union_conflict_with_basic_type(self, array_like, proto):
        class Test1(msgspec.Struct, tag=True, array_like=array_like):
            x: int

        class Test2(msgspec.Struct, tag=True, array_like=array_like):
            x: int

        other = list if array_like else dict

        typ = Union[Test1, Test2, other]

        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)

        assert "not supported" in str(rec.value)
        if array_like:
            assert "other array-like types" in str(rec.value)
        else:
            assert "more than one dict-like type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("array_like", [False, True])
    def test_err_union_struct_different_fields(self, proto, array_like):
        class Test1(msgspec.Struct, tag_field="foo", array_like=array_like):
            x: int

        class Test2(msgspec.Struct, tag_field="bar", array_like=array_like):
            x: int

        typ = Union[Test1, Test2]

        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)

        assert "not supported" in str(rec.value)
        assert "the same `tag_field`" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("array_like", [False, True])
    def test_err_union_struct_mix_int_str_tags(self, proto, array_like):
        class Test1(msgspec.Struct, tag=1, array_like=array_like):
            x: int

        class Test2(msgspec.Struct, tag="two", array_like=array_like):
            x: int

        typ = Union[Test1, Test2]

        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)

        assert "not supported" in str(rec.value)
        assert "both `int` and `str` tags" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("array_like", [False, True])
    @pytest.mark.parametrize(
        "tags",
        [
            ("a", "b", "b"),
            ("a", "a", "b"),
            ("a", "b", "a"),
            (1, 2, 2),
            (1, 1, 2),
            (1, 2, 1),
        ],
    )
    def test_err_union_struct_non_unique_tag_values(self, proto, array_like, tags):
        class Test1(msgspec.Struct, tag=tags[0], array_like=array_like):
            x: int

        class Test2(msgspec.Struct, tag=tags[1], array_like=array_like):
            x: int

        class Test3(msgspec.Struct, tag=tags[2], array_like=array_like):
            x: int

        typ = Union[Test1, Test2, Test3]

        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)

        assert "not supported" in str(rec.value)
        assert "unique `tag`" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "tag1, tag2, unknown",
        [
            ("Test1", "Test2", "Test3"),
            (123, -123, 0),
        ],
    )
    def test_decode_struct_union(self, proto, tag1, tag2, unknown):
        class Test1(msgspec.Struct, tag=tag1):
            a: int
            b: int
            c: int = 0

        class Test2(msgspec.Struct, tag=tag2):
            x: int
            y: int

        dec = proto.Decoder(Union[Test1, Test2])
        enc = proto.Encoder()

        # Tag can be in any position
        assert dec.decode(enc.encode({"type": tag1, "a": 1, "b": 2})) == Test1(1, 2)
        assert dec.decode(enc.encode({"a": 1, "type": tag1, "b": 2})) == Test1(1, 2)
        assert dec.decode(enc.encode({"x": 1, "y": 2, "type": tag2})) == Test2(1, 2)

        # Optional fields still work
        assert dec.decode(enc.encode({"type": tag1, "a": 1, "b": 2, "c": 3})) == Test1(
            1, 2, 3
        )
        assert dec.decode(enc.encode({"a": 1, "b": 2, "c": 3, "type": tag1})) == Test1(
            1, 2, 3
        )

        # Extra fields still ignored
        assert dec.decode(enc.encode({"a": 1, "b": 2, "d": 4, "type": tag1})) == Test1(
            1, 2
        )

        # Tag missing
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode({"a": 1, "b": 2}))
        assert "missing required field `type`" in str(rec.value)

        # Tag wrong type
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode({"type": 123.456, "a": 1, "b": 2}))
        assert f"Expected `{type(tag1).__name__}`" in str(rec.value)
        assert "`$.type`" in str(rec.value)

        # Tag unknown
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode({"type": unknown, "a": 1, "b": 2}))
        assert f"Invalid value {unknown!r} - at `$.type`" == str(rec.value)

    @pytest.mark.parametrize(
        "tag1, tag2, tag3, unknown",
        [
            ("Test1", "Test2", "Test3", "Test4"),
            (123, -123, 0, -1),
        ],
    )
    def test_decode_struct_array_union(self, proto, tag1, tag2, tag3, unknown):
        class Test1(msgspec.Struct, tag=tag1, array_like=True):
            a: int
            b: int
            c: int = 0

        class Test2(msgspec.Struct, tag=tag2, array_like=True):
            x: int
            y: int

        class Test3(msgspec.Struct, tag=tag3, array_like=True):
            pass

        dec = proto.Decoder(Union[Test1, Test2, Test3])
        enc = proto.Encoder()

        # Decoding works
        assert dec.decode(enc.encode([tag1, 1, 2])) == Test1(1, 2)
        assert dec.decode(enc.encode([tag2, 3, 4])) == Test2(3, 4)
        assert dec.decode(enc.encode([tag3])) == Test3()

        # Optional & Extra fields still respected
        assert dec.decode(enc.encode([tag1, 1, 2, 3])) == Test1(1, 2, 3)
        assert dec.decode(enc.encode([tag1, 1, 2, 3, 4])) == Test1(1, 2, 3)

        # Missing required field
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode([tag1, 1]))
        assert "Expected `array` of at least length 3, got 2" in str(rec.value)

        # Type error has correct field index
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode([tag1, 1, "bad", 2]))
        assert "Expected `int`, got `str` - at `$[2]`" == str(rec.value)

        # Tag missing
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode([]))
        assert "Expected `array` of at least length 1, got 0" == str(rec.value)

        # Tag wrong type
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode([123.456, 2, 3, 4]))
        assert f"Expected `{type(tag1).__name__}`" in str(rec.value)
        assert "`$[0]`" in str(rec.value)

        # Tag unknown
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode([unknown, 1, 2, 3]))
        assert f"Invalid value {unknown!r} - at `$[0]`" == str(rec.value)

    @pytest.mark.parametrize("array_like", [False, True])
    def test_decode_struct_union_with_non_struct_types(self, array_like, proto):
        class Test1(msgspec.Struct, tag=True, array_like=array_like):
            a: int
            b: int

        class Test2(msgspec.Struct, tag=True, array_like=array_like):
            x: int
            y: int

        dec = proto.Decoder(Union[Test1, Test2, None, int, str])
        enc = proto.Encoder()

        for msg in [Test1(1, 2), Test2(3, 4), None, 5, 6]:
            assert dec.decode(enc.encode(msg)) == msg

        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(enc.encode(True))

        typ = "array" if array_like else "object"

        assert f"Expected `int | str | {typ} | null`, got `bool`" == str(rec.value)

    @pytest.mark.parametrize("array_like", [False, True])
    def test_struct_union_cached(self, array_like, proto):
        from msgspec._core import _struct_lookup_cache as cache

        cache.clear()

        class Test1(msgspec.Struct, tag=True, array_like=array_like):
            a: int
            b: int

        class Test2(msgspec.Struct, tag=True, array_like=array_like):
            x: int
            y: int

        typ1 = Union[Test2, Test1]
        typ2 = Union[Test1, Test2]
        typ3 = Union[Test1, Test2, int, None]

        for typ in [typ1, typ2, typ3]:
            for msg in [Test1(1, 2), Test2(3, 4)]:
                assert proto.decode(proto.encode(msg), type=typ) == msg

        assert len(cache) == 1
        assert frozenset((Test1, Test2)) in cache

    def test_struct_union_cache_evicted(self, proto):
        from msgspec._core import _struct_lookup_cache as cache

        MAX_CACHE_SIZE = 64  # XXX: update if hardcoded value in `_core.c` changes

        cache.clear()

        def call_with_new_types():
            class Test1(msgspec.Struct, tag=True):
                a: int

            class Test2(msgspec.Struct, tag=True):
                x: int

            typ = (Test1, Test2)

            proto.decode(proto.encode(Test1(1)), type=Union[typ])

            return frozenset(typ)

        first = call_with_new_types()
        assert first in cache

        # Fill up the cache
        for _ in range(MAX_CACHE_SIZE - 1):
            call_with_new_types()

        # Check that first item is still in cache and is first in order
        assert len(cache) == MAX_CACHE_SIZE
        assert first in cache
        assert first == list(cache.keys())[0]

        # Add a new item, causing an item to be popped from the cache
        new = call_with_new_types()

        assert len(cache) == MAX_CACHE_SIZE
        assert first not in cache
        assert frozenset(new) in cache


class TestStructOmitDefaults:
    def test_omit_defaults(self, proto):
        class Test(msgspec.Struct, omit_defaults=True):
            a: int = 0
            b: bool = False
            c: Optional[str] = None
            d: list = []
            e: Union[list, set] = set()
            f: dict = {}

        cases = [
            (Test(), {}),
            (Test(1), {"a": 1}),
            (Test(1, False), {"a": 1}),
            (Test(1, True), {"a": 1, "b": True}),
            (Test(1, c=None), {"a": 1}),
            (Test(1, c="test"), {"a": 1, "c": "test"}),
            (Test(1, d=[1]), {"a": 1, "d": [1]}),
            (Test(1, e={1}), {"a": 1, "e": [1]}),
            (Test(1, e=[]), {"a": 1, "e": []}),
            (Test(1, f={"a": 1}), {"a": 1, "f": {"a": 1}}),
        ]

        for obj, sol in cases:
            res = proto.decode(proto.encode(obj))
            assert res == sol

    def test_omit_defaults_positional(self, proto):
        class Test(msgspec.Struct, omit_defaults=True):
            a: int
            b: bool = False

        cases = [
            (Test(1), {"a": 1}),
            (Test(1, False), {"a": 1}),
            (Test(1, True), {"a": 1, "b": True}),
        ]

        for obj, sol in cases:
            res = proto.decode(proto.encode(obj))
            assert res == sol

    def test_omit_defaults_tagged(self, proto):
        class Test(msgspec.Struct, omit_defaults=True, tag=True):
            a: int
            b: bool = False

        cases = [
            (Test(1), {"type": "Test", "a": 1}),
            (Test(1, False), {"type": "Test", "a": 1}),
            (Test(1, True), {"type": "Test", "a": 1, "b": True}),
        ]

        for obj, sol in cases:
            res = proto.decode(proto.encode(obj))
            assert res == sol

    def test_omit_defaults_ignored_for_array_like(self, proto):
        class Test(msgspec.Struct, omit_defaults=True, array_like=True):
            a: int
            b: bool = False

        cases = [
            (Test(1), [1, False]),
            (Test(1, False), [1, False]),
            (Test(1, True), [1, True]),
        ]

        for obj, sol in cases:
            res = proto.decode(proto.encode(obj))
            assert res == sol


class PointUpper(msgspec.Struct, rename="upper"):
    x: int
    y: int


class TestStructRename:
    def test_rename_encode_struct(self, proto):
        res = proto.encode(PointUpper(1, 2))
        exp = proto.encode({"X": 1, "Y": 2})
        assert res == exp

    def test_rename_decode_struct(self, proto):
        msg = proto.encode({"X": 1, "Y": 2})
        res = proto.decode(msg, type=PointUpper)
        assert res == PointUpper(1, 2)

    def test_rename_decode_struct_wrong_type(self, proto):
        msg = proto.encode({"X": 1, "Y": "bad"})
        with pytest.raises(msgspec.ValidationError) as rec:
            proto.decode(msg, type=PointUpper)
        assert "Expected `int`, got `str` - at `$.Y`" == str(rec.value)

    def test_rename_decode_struct_missing_field(self, proto):
        msg = proto.encode({"X": 1})
        with pytest.raises(msgspec.ValidationError) as rec:
            proto.decode(msg, type=PointUpper)
        assert "Object missing required field `Y`" == str(rec.value)


class TestTypedDict:
    def test_type_cached(self, proto):
        class Ex(TypedDict):
            a: int
            b: str

        msg = {"a": 1, "b": "two"}

        dec = proto.Decoder(Ex)
        info = Ex.__msgspec_cache__
        assert info is not None
        dec2 = proto.Decoder(Ex)
        assert Ex.__msgspec_cache__ is info

        assert dec.decode(proto.encode(msg)) == msg
        assert dec2.decode(proto.encode(msg)) == msg

    def test_subtype_error(self, proto):
        class Ex(TypedDict):
            a: int
            b: Union[list, tuple]

        with pytest.raises(TypeError, match="may not contain more than one array-like"):
            proto.Decoder(Ex)
        assert not hasattr(Ex, "__msgspec_cache__")

    @pytest.mark.parametrize("msgpack_first", [False, True])
    def test_type_errors_not_json(self, msgpack_first):
        class Ex(TypedDict):
            a: int
            b: Dict[int, int]

        if msgpack_first:
            dec = msgspec.msgpack.Decoder(Ex)
            info = Ex.__msgspec_cache__
            assert info is not None
            msg = {"a": 1, "b": {1: 2}}
            assert dec.decode(msgspec.msgpack.encode(msg)) == msg

        with pytest.raises(TypeError, match="JSON doesn't support"):
            msgspec.json.Decoder(Ex)

        if msgpack_first:
            assert Ex.__msgspec_cache__ is info
        else:
            assert not hasattr(Ex, "__msgspec_cache__")

    def test_recursive_type(self, proto):
        source = """
        from __future__ import annotations
        from typing import TypedDict, Union

        class Ex(TypedDict):
            a: int
            b: Union[Ex, None]
        """

        with temp_module(source) as mod:
            msg = {"a": 1, "b": {"a": 2, "b": None}}
            dec = proto.Decoder(mod.Ex)
            assert dec.decode(proto.encode(msg)) == msg

            with pytest.raises(msgspec.ValidationError) as rec:
                dec.decode(proto.encode({"a": 1, "b": {"a": "bad"}}))
            assert "`$.b.a`" in str(rec.value)
            assert "Expected `int`, got `str`" in str(rec.value)

    @pytest.mark.parametrize("msgpack_first", [True])
    def test_recursive_type_errors_not_json(self, msgpack_first):
        source = """
        from __future__ import annotations
        from typing import TypedDict, Union, Dict

        class Ex(TypedDict):
            a: int
            b: Union[Ex, None]
            c: Dict[int, int]
        """
        with temp_module(source) as mod:
            if msgpack_first:
                dec = msgspec.msgpack.Decoder(mod.Ex)
                info = mod.Ex.__msgspec_cache__
                assert info is not None
                msg = {"a": 1, "b": None, "c": {1: 2}}
                assert dec.decode(msgspec.msgpack.encode(msg)) == msg

            with pytest.raises(TypeError, match="JSON doesn't support"):
                msgspec.json.Decoder(mod.Ex)

            if msgpack_first:
                assert mod.Ex.__msgspec_cache__ is info
            else:
                assert not hasattr(mod.Ex, "__msgspec_cache__")

    def test_total_true(self, proto):
        class Ex(TypedDict):
            a: int
            b: str

        dec = proto.Decoder(Ex)

        x = {"a": 1, "b": "two"}
        assert dec.decode(proto.encode(x)) == x

        x2 = {"a": 1, "b": "two", "c": "extra"}
        assert dec.decode(proto.encode(x2)) == x

        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(proto.encode({"b": "two"}))
        assert "Object missing required field `a`" == str(rec.value)

        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(proto.encode({"a": 1, "b": 2}))
        assert "Expected `str`, got `int` - at `$.b`" == str(rec.value)

    def test_duplicate_keys(self, proto):
        """Validating if all required keys are present is done with a count. We
        need to ensure that duplicate required keys don't increment the count,
        masking a missing field."""

        class Ex(TypedDict):
            a: int
            b: str

        dec = proto.Decoder(Ex)

        temp = proto.encode({"a": 1, "b": "two", "x": 2})

        msg = temp.replace(b"x", b"a")
        assert dec.decode(msg) == {"a": 2, "b": "two"}

        msg = temp.replace(b"x", b"a").replace(b"b", b"c")
        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(msg)
        assert "Object missing required field `b`" == str(rec.value)

    def test_total_false(self, proto):
        class Ex(TypedDict, total=False):
            a: int
            b: str

        dec = proto.Decoder(Ex)

        x = {"a": 1, "b": "two"}
        assert dec.decode(proto.encode(x)) == x

        x2 = {"a": 1, "b": "two", "c": "extra"}
        assert dec.decode(proto.encode(x2)) == x

        x3 = {"b": "two"}
        assert dec.decode(proto.encode(x3)) == x3

        x4 = {}
        assert dec.decode(proto.encode(x4)) == x4

    @pytest.mark.parametrize("use_typing_extensions", [False, True])
    def test_total_partially_optional(self, proto, use_typing_extensions):
        if use_typing_extensions:
            tex = pytest.importorskip("typing_extensions")
            cls = tex.TypedDict
        else:
            cls = TypedDict

        class Base(cls):
            a: int
            b: str

        class Ex(Base, total=False):
            c: str

        if not hasattr(Ex, "__required_keys__"):
            # This should be Python 3.8, builtin typing only
            pytest.skip(reason="partially optional TypedDict not supported")

        dec = proto.Decoder(Ex)

        x = {"a": 1, "b": "two", "c": "extra"}
        assert dec.decode(proto.encode(x)) == x

        x2 = {"a": 1, "b": "two"}
        assert dec.decode(proto.encode(x2)) == x2

        with pytest.raises(msgspec.ValidationError) as rec:
            dec.decode(proto.encode({"b": "two"}))
        assert "Object missing required field `a`" == str(rec.value)

    def test_keys_are_their_interned_values(self, proto):
        """Ensure that we're not allocating new keys here, but reusing the
        existing keys on the TypedDict schema"""

        class Ex(TypedDict):
            key_name_1: int
            key_name_2: int

        dec = proto.Decoder(Ex)
        msg = dec.decode(proto.encode({"key_name_1": 1, "key_name_2": 2}))
        for k1, k2 in zip(sorted(Ex.__annotations__), sorted(msg)):
            assert k1 is k2
