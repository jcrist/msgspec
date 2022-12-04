from __future__ import annotations

import datetime
import enum
import gc
import sys
import uuid
import weakref
from collections import namedtuple
from dataclasses import dataclass, field
from typing import (
    Deque,
    Dict,
    List,
    Literal,
    NamedTuple,
    Optional,
    Tuple,
    TypedDict,
    Union,
)

import pytest

import msgspec

from utils import temp_module

PY310 = sys.version_info[:2] >= (3, 10)
PY311 = sys.version_info[:2] >= (3, 11)

py310_plus = pytest.mark.skipif(not PY310, reason="3.10+ only")
py311_plus = pytest.mark.skipif(not PY311, reason="3.11+ only")


@pytest.fixture(params=["json", "msgpack"])
def proto(request):
    if request.param == "json":
        return msgspec.json
    elif request.param == "msgpack":
        return msgspec.msgpack


try:
    from enum import StrEnum
except ImportError:

    class StrEnum(str, enum.Enum):
        pass


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


@dataclass
class PersonDataclass:
    first: str
    last: str
    age: int


class PersonTuple(NamedTuple):
    first: str
    last: str
    age: int


class Custom:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __eq__(self, other):
        return self.x == other.x and self.y == other.y


class TestEncodeSubclasses:
    def test_encode_dict_subclass(self, proto):
        class subclass(dict):
            pass

        for msg in [{}, {"a": 1, "b": 2}]:
            assert proto.encode(subclass(msg)) == proto.encode(msg)

    def test_encode_list_subclass(self, proto):
        class subclass(list):
            pass

        for msg in [[], [1, 2]]:
            assert proto.encode(subclass(msg)) == proto.encode(msg)

    def test_encode_tuple_subclass(self, proto):
        class subclass(tuple):
            pass

        for msg in [(), (1, 2)]:
            assert proto.encode(subclass(msg)) == proto.encode(msg)


class TestIntEnum:
    def test_empty_errors(self, proto):
        class Empty(enum.IntEnum):
            pass

        with pytest.raises(TypeError, match="Enum types must have at least one item"):
            proto.Decoder(Empty)

    @pytest.mark.parametrize("base_cls", [enum.IntEnum, enum.Enum])
    def test_encode(self, proto, base_cls):
        class Test(base_cls):
            A = 1
            B = 2

        assert proto.encode(Test.A) == proto.encode(1)

    @pytest.mark.parametrize("base_cls", [enum.IntEnum, enum.Enum])
    def test_decode(self, proto, base_cls):
        class Test(base_cls):
            A = 1
            B = 2

        dec = proto.Decoder(Test)
        assert dec.decode(proto.encode(1)) is Test.A
        assert dec.decode(proto.encode(2)) is Test.B

        with pytest.raises(msgspec.ValidationError, match="Invalid enum value 3"):
            dec.decode(proto.encode(3))

    def test_decode_nested(self, proto):
        class Test(msgspec.Struct):
            fruit: FruitInt

        dec = proto.Decoder(Test)

        dec.decode(proto.encode({"fruit": 1})) == Test(FruitInt.APPLE)

        with pytest.raises(
            msgspec.ValidationError, match=r"Invalid enum value 3 - at `\$.fruit`"
        ):
            dec.decode(proto.encode({"fruit": 3}))

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
    def test_empty_errors(self, proto):
        class Empty(enum.Enum):
            pass

        with pytest.raises(TypeError, match="Enum types must have at least one item"):
            proto.Decoder(Empty)

    def test_unsupported_type_errors(self, proto):
        class Bad(enum.Enum):
            A = 1.5

        with pytest.raises(
            msgspec.EncodeError, match="Only enums with int or str values are supported"
        ):
            proto.encode(Bad.A)

        with pytest.raises(TypeError) as rec:
            proto.Decoder(Bad)

        assert "Enums must contain either all str or all int values" in str(rec.value)
        assert repr(Bad) in str(rec.value)

    @pytest.mark.parametrize(
        "values",
        [
            [("A", 1), ("B", 2), ("C", "c")],
            [("A", "a"), ("B", "b"), ("C", 3)],
        ],
    )
    def test_mixed_value_types_errors(self, values, proto):
        Bad = enum.Enum("Bad", values)

        with pytest.raises(TypeError) as rec:
            proto.Decoder(Bad)

        assert "Enums must contain either all str or all int values" in str(rec.value)
        assert repr(Bad) in str(rec.value)

    @pytest.mark.parametrize("base_cls", [StrEnum, enum.Enum])
    def test_encode(self, proto, base_cls):
        class Test(base_cls):
            A = "apple"
            B = "banana"

        assert proto.encode(Test.A) == proto.encode("apple")

    @pytest.mark.parametrize("base_cls", [StrEnum, enum.Enum])
    def test_decode(self, proto, base_cls):
        class Test(base_cls):
            A = "apple"
            B = "banana"

        dec = proto.Decoder(Test)
        assert dec.decode(proto.encode("apple")) is Test.A
        assert dec.decode(proto.encode("banana")) is Test.B

        with pytest.raises(
            msgspec.ValidationError, match="Invalid enum value 'cherry'"
        ):
            dec.decode(proto.encode("cherry"))

    def test_decode_nested(self, proto):
        class Test(msgspec.Struct):
            fruit: FruitStr

        dec = proto.Decoder(Test)

        dec.decode(proto.encode({"fruit": "apple"})) == Test(FruitStr.APPLE)

        with pytest.raises(
            msgspec.ValidationError,
            match=r"Invalid enum value 'cherry' - at `\$.fruit`",
        ):
            dec.decode(proto.encode({"fruit": "cherry"}))

    def test_str_lookup_reused(self):
        class Test(enum.Enum):
            A = "a"
            B = "b"

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
            A = "a"
            B = "b"

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

        myenum = enum.Enum(
            "myenum", [(unique_str(), unique_str()) for _ in range(nitems)]
        )
        dec = msgspec.msgpack.Decoder(myenum)

        for val in myenum:
            msg = msgspec.msgpack.encode(val.value)
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

        myenum = enum.Enum(
            "myenum", [(unique_str(), unique_str()) for _ in range(nitems)]
        )
        dec = msgspec.msgpack.Decoder(myenum)

        for val in myenum:
            msg = msgspec.msgpack.encode(val.value)
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

    def test_mix_int_and_int_literal(self):
        dec = msgspec.msgpack.Decoder(Union[Literal[-1, 1], int])
        for x in [-1, 1, 10]:
            assert dec.decode(msgspec.msgpack.encode(x)) == x

    def test_mix_str_and_str_literal(self):
        dec = msgspec.msgpack.Decoder(Union[Literal["a", "b"], str])
        for x in ["a", "b", "c"]:
            assert dec.decode(msgspec.msgpack.encode(x)) == x


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
            Union[PersonDataclass, dict],
            Union[Person, PersonDict],
        ],
    )
    def test_err_union_with_multiple_dict_like_types(self, typ, proto):
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert "more than one dict-like type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "typ",
        [
            Union[PersonArray, list],
            Union[tuple, PersonArray],
            Union[PersonArray, PersonTuple],
            Union[PersonTuple, frozenset],
        ],
    )
    def test_err_union_with_struct_array_like_and_array(self, typ, proto):
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert "more than one array-like type" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize("types", [(FruitInt, int), (FruitInt, Literal[1, 2])])
    def test_err_union_with_multiple_int_like_types(self, types, proto):
        typ = Union[types]
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert "int-like" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @pytest.mark.parametrize(
        "types",
        [(FruitStr, str), (FruitStr, Literal["one", "two"]), (FruitStr, datetime.date)],
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
            (Union[FruitInt, VeggieInt], "int enum"),
            (Union[FruitStr, VeggieStr], "str enum"),
            (Union[Dict[int, float], dict], "dict"),
            (Union[List[int], List[float]], "array-like"),
            (Union[List[int], tuple], "array-like"),
            (Union[set, tuple], "array-like"),
            (Union[Tuple[int, ...], list], "array-like"),
            (Union[Tuple[int, float, str], set], "array-like"),
            (Union[Deque, int, Custom], "custom"),
        ],
    )
    def test_err_union_conflicts(self, typ, kind, proto):
        with pytest.raises(TypeError) as rec:
            proto.Decoder(typ)
        assert f"more than one {kind}" in str(rec.value)
        assert repr(typ) in str(rec.value)

    @py310_plus
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
            assert "more than one array-like type" in str(rec.value)
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
            (0, 1, 2),
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
            (0, 1, 2, 3),
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


class TestStructForbidUnknownFields:
    def test_forbid_unknown_fields(self, proto):
        class Test(msgspec.Struct, forbid_unknown_fields=True):
            x: int
            y: int

        good = Test(1, 2)
        assert proto.decode(proto.encode(good), type=Test) == good

        bad = proto.encode({"x": 1, "y": 2, "z": 3})
        with pytest.raises(
            msgspec.ValidationError, match="Object contains unknown field `z`"
        ):
            proto.decode(bad, type=Test)

    def test_forbid_unknown_fields_array_like(self, proto):
        class Test(msgspec.Struct, forbid_unknown_fields=True, array_like=True):
            x: int
            y: int

        good = Test(1, 2)
        assert proto.decode(proto.encode(good), type=Test) == good

        bad = proto.encode([1, 2, 3])
        with pytest.raises(
            msgspec.ValidationError, match="Expected `array` of at most length 2"
        ):
            proto.decode(bad, type=Test)


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

    def test_multiple_typeddict_errors(self, proto):
        class Ex1(TypedDict):
            a: int

        class Ex2(TypedDict):
            b: int

        with pytest.raises(TypeError, match="may not contain more than one TypedDict"):
            proto.Decoder(Union[Ex1, Ex2])

    def test_subtype_error(self, proto):
        class Ex(TypedDict):
            a: int
            b: Union[list, tuple]

        with pytest.raises(TypeError, match="may not contain more than one array-like"):
            proto.Decoder(Ex)
        assert not hasattr(Ex, "__msgspec_cache__")

    @pytest.mark.parametrize("msgpack_first", [False, True])
    @pytest.mark.parametrize("wrap", [False, True])
    def test_type_errors_not_json(self, msgpack_first, wrap):
        class Ex(TypedDict):
            a: int
            b: Dict[int, int]

        if wrap:
            typ = TypedDict("Test", {"x": Ex})
        else:
            typ = Ex

        if msgpack_first:
            dec = msgspec.msgpack.Decoder(typ)
            info = Ex.__msgspec_cache__
            assert info is not None
            msg = {"a": 1, "b": {1: 2}}
            if wrap:
                msg = {"x": msg}
            assert dec.decode(msgspec.msgpack.encode(msg)) == msg

        with pytest.raises(TypeError, match="JSON doesn't support"):
            msgspec.json.Decoder(typ)

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
            pytest.skip("partially optional TypedDict not supported")

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


class TestNamedTuple:
    def test_type_cached(self, proto):
        class Ex(NamedTuple):
            a: int
            b: str

        msg = (1, "two")

        dec = proto.Decoder(Ex)
        info = Ex.__msgspec_cache__
        assert info is not None
        dec2 = proto.Decoder(Ex)
        assert Ex.__msgspec_cache__ is info

        assert dec.decode(proto.encode(msg)) == msg
        assert dec2.decode(proto.encode(msg)) == msg

    def test_multiple_namedtuple_errors(self, proto):
        class Ex1(NamedTuple):
            a: int

        class Ex2(NamedTuple):
            b: int

        with pytest.raises(TypeError, match="may not contain more than one NamedTuple"):
            proto.Decoder(Union[Ex1, Ex2])

    def test_subtype_error(self, proto):
        class Ex(NamedTuple):
            a: int
            b: Union[list, tuple]

        with pytest.raises(TypeError, match="may not contain more than one array-like"):
            proto.Decoder(Ex)
        assert not hasattr(Ex, "__msgspec_cache__")

    @pytest.mark.parametrize("msgpack_first", [False, True])
    @pytest.mark.parametrize("wrap", [False, True])
    def test_type_errors_not_json(self, msgpack_first, wrap):
        class Ex(NamedTuple):
            a: int
            b: Dict[int, int]

        if wrap:
            typ = TypedDict("Test", {"x": Ex})
        else:
            typ = Ex

        if msgpack_first:
            dec = msgspec.msgpack.Decoder(typ)
            info = Ex.__msgspec_cache__
            assert info is not None
            msg = Ex(1, {1: 2})
            if wrap:
                msg = {"x": msg}
            assert dec.decode(msgspec.msgpack.encode(msg)) == msg

        with pytest.raises(TypeError, match="JSON doesn't support"):
            msgspec.json.Decoder(typ)

        if msgpack_first:
            assert Ex.__msgspec_cache__ is info
        else:
            assert not hasattr(Ex, "__msgspec_cache__")

    def test_recursive_type(self, proto):
        source = """
        from __future__ import annotations
        from typing import NamedTuple, Union

        class Ex(NamedTuple):
            a: int
            b: Union[Ex, None]
        """

        with temp_module(source) as mod:
            msg = mod.Ex(1, mod.Ex(2, None))
            dec = proto.Decoder(mod.Ex)
            assert dec.decode(proto.encode(msg)) == msg

            with pytest.raises(msgspec.ValidationError) as rec:
                dec.decode(proto.encode(mod.Ex(1, ("bad", "two"))))
            assert "`$[1][0]`" in str(rec.value)
            assert "Expected `int`, got `str`" in str(rec.value)

    @pytest.mark.parametrize("msgpack_first", [True])
    def test_recursive_type_errors_not_json(self, msgpack_first):
        source = """
        from __future__ import annotations
        from typing import NamedTuple, Union, Dict

        class Ex(NamedTuple):
            a: int
            b: Union[Ex, None]
            c: Dict[int, int]
        """
        with temp_module(source) as mod:
            if msgpack_first:
                dec = msgspec.msgpack.Decoder(mod.Ex)
                info = mod.Ex.__msgspec_cache__
                assert info is not None
                msg = mod.Ex(1, None, {1: 2})
                assert dec.decode(msgspec.msgpack.encode(msg)) == msg

            with pytest.raises(TypeError, match="JSON doesn't support"):
                msgspec.json.Decoder(mod.Ex)

            if msgpack_first:
                assert mod.Ex.__msgspec_cache__ is info
            else:
                assert not hasattr(mod.Ex, "__msgspec_cache__")

    @pytest.mark.parametrize("use_typing", [True, False])
    def test_decode_namedtuple_no_defaults(self, proto, use_typing):
        if use_typing:

            class Example(NamedTuple):
                a: int
                b: int
                c: int

        else:
            Example = namedtuple("Example", "a b c")

        dec = proto.Decoder(Example)
        msg = Example(1, 2, 3)
        res = dec.decode(proto.encode(msg))
        assert res == msg

        suffix = ", got 1" if proto is msgspec.msgpack else ""
        with pytest.raises(msgspec.ValidationError, match=f"length 3{suffix}"):
            dec.decode(proto.encode((1,)))

        suffix = ", got 6" if proto is msgspec.msgpack else ""
        with pytest.raises(msgspec.ValidationError, match=f"length 3{suffix}"):
            dec.decode(proto.encode((1, 2, 3, 4, 5, 6)))

    @pytest.mark.parametrize("use_typing", [True, False])
    def test_decode_namedtuple_with_defaults(self, proto, use_typing):
        if use_typing:

            class Example(NamedTuple):
                a: int
                b: int
                c: int = -3
                d: int = -4
                e: int = -5

        else:
            Example = namedtuple("Example", "a b c d e", defaults=(-3, -4, -5))

        dec = proto.Decoder(Example)
        for args in [(1, 2), (1, 2, 3), (1, 2, 3, 4), (1, 2, 3, 4, 5)]:
            msg = Example(*args)
        res = dec.decode(proto.encode(msg))
        assert res == msg

        suffix = ", got 1" if proto is msgspec.msgpack else ""
        with pytest.raises(msgspec.ValidationError, match=f"length 2 to 5{suffix}"):
            dec.decode(proto.encode((1,)))

        suffix = ", got 6" if proto is msgspec.msgpack else ""
        with pytest.raises(msgspec.ValidationError, match=f"length 2 to 5{suffix}"):
            dec.decode(proto.encode((1, 2, 3, 4, 5, 6)))

    def test_decode_namedtuple_field_wrong_type(self, proto):
        dec = proto.Decoder(PersonTuple)
        msg = proto.encode((1, "bad", 2))
        with pytest.raises(
            msgspec.ValidationError, match=r"Expected `str`, got `int` - at `\$\[0\]`"
        ):
            dec.decode(msg)

    def test_decode_namedtuple_not_array(self, proto):
        dec = proto.Decoder(PersonTuple)
        msg = proto.encode({})
        with pytest.raises(
            msgspec.ValidationError, match="Expected `array`, got `object`"
        ):
            dec.decode(msg)


class TestDataclass:
    def test_encode_dataclass_no_slots(self, proto):
        @dataclass
        class Test:
            x: int
            y: int

        x = Test(1, 2)
        res = proto.encode(x)
        sol = proto.encode({"x": 1, "y": 2})
        assert res == sol

    @py310_plus
    def test_encode_dataclass_slots(self, proto):
        @dataclass(slots=True)
        class Test:
            x: int
            y: int

        x = Test(1, 2)
        res = proto.encode(x)
        sol = proto.encode({"x": 1, "y": 2})
        assert res == sol

    @py310_plus
    @pytest.mark.parametrize("slots", [True, False])
    def test_encode_dataclass_missing_fields(self, proto, slots):
        @dataclass(slots=slots)
        class Test:
            x: int
            y: int
            z: int

        x = Test(1, 2, 3)
        sol = {"x": 1, "y": 2, "z": 3}
        for key in "xyz":
            delattr(x, key)
            del sol[key]
            res = proto.decode(proto.encode(x))
            assert res == sol

    @py310_plus
    @pytest.mark.parametrize("slots_base", [True, False])
    @pytest.mark.parametrize("slots", [True, False])
    def test_encode_dataclass_subclasses(self, proto, slots_base, slots):
        @dataclass(slots=slots_base)
        class Base:
            x: int
            y: int

        @dataclass(slots=slots)
        class Test(Base):
            y: int
            z: int

        x = Test(1, 2, 3)
        res = proto.decode(proto.encode(x))
        assert res == {"x": 1, "y": 2, "z": 3}

        # Missing attribute ignored
        del x.y
        res = proto.decode(proto.encode(x))
        assert res == {"x": 1, "z": 3}

    @py311_plus
    def test_encode_dataclass_weakref_slot(self, proto):
        @dataclass(slots=True, weakref_slot=True)
        class Test:
            x: int
            y: int

        x = Test(1, 2)
        ref = weakref.ref(x)  # noqa
        res = proto.decode(proto.encode(x))
        assert res == {"x": 1, "y": 2}

    def test_type_cached(self, proto):
        @dataclass
        class Ex:
            a: int
            b: str

        msg = Ex(a=1, b="two")

        dec = proto.Decoder(Ex)
        info = Ex.__msgspec_cache__
        assert info is not None
        dec2 = proto.Decoder(Ex)
        assert Ex.__msgspec_cache__ is info

        assert dec.decode(proto.encode(msg)) == msg
        assert dec2.decode(proto.encode(msg)) == msg

    def test_multiple_dataclasses_errors(self, proto):
        @dataclass
        class Ex1:
            a: int

        @dataclass
        class Ex2:
            b: int

        with pytest.raises(TypeError, match="may not contain more than one dataclass"):
            proto.Decoder(Union[Ex1, Ex2])

    def test_subtype_error(self, proto):
        @dataclass
        class Ex:
            a: int
            b: Union[list, tuple]

        with pytest.raises(TypeError, match="may not contain more than one array-like"):
            proto.Decoder(Ex)
        assert not hasattr(Ex, "__msgspec_cache__")

    @pytest.mark.parametrize("msgpack_first", [False, True])
    @pytest.mark.parametrize("wrap", [False, True])
    def test_type_errors_not_json(self, msgpack_first, wrap):
        @dataclass
        class Ex:
            a: int
            b: Dict[int, int]

        if wrap:
            typ = TypedDict("Test", {"x": Ex})
        else:
            typ = Ex

        if msgpack_first:
            dec = msgspec.msgpack.Decoder(typ)
            info = Ex.__msgspec_cache__
            assert info is not None
            msg = Ex(a=1, b={1: 2})
            if wrap:
                msg = {"x": msg}
            assert dec.decode(msgspec.msgpack.encode(msg)) == msg

        with pytest.raises(TypeError, match="JSON doesn't support"):
            msgspec.json.Decoder(typ)

        if msgpack_first:
            assert Ex.__msgspec_cache__ is info
        else:
            assert not hasattr(Ex, "__msgspec_cache__")

    def test_recursive_type(self, proto):
        source = """
        from __future__ import annotations
        from typing import Union
        from dataclasses import dataclass

        @dataclass
        class Ex:
            a: int
            b: Union[Ex, None]
        """

        with temp_module(source) as mod:
            msg = mod.Ex(a=1, b=mod.Ex(a=2, b=None))
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
        from typing import Union, Dict
        from dataclasses import dataclass

        @dataclass
        class Ex:
            a: int
            b: Union[Ex, None]
            c: Dict[int, int]
        """
        with temp_module(source) as mod:
            if msgpack_first:
                dec = msgspec.msgpack.Decoder(mod.Ex)
                info = mod.Ex.__msgspec_cache__
                assert info is not None
                msg = mod.Ex(a=1, b=None, c={1: 2})
                assert dec.decode(msgspec.msgpack.encode(msg)) == msg

            with pytest.raises(TypeError, match="JSON doesn't support"):
                msgspec.json.Decoder(mod.Ex)

            if msgpack_first:
                assert mod.Ex.__msgspec_cache__ is info
            else:
                assert not hasattr(mod.Ex, "__msgspec_cache__")

    def test_classvars_ignored(self, proto):
        source = """
        from __future__ import annotations

        from typing import ClassVar
        from dataclasses import dataclass

        @dataclass
        class Ex:
            a: int
            other: ClassVar[int]
        """
        with temp_module(source) as mod:
            msg = mod.Ex(a=1)
            dec = proto.Decoder(mod.Ex)
            res = dec.decode(proto.encode({"a": 1, "other": 2}))
            assert res == msg
            assert not hasattr(res, "other")

    def test_initvars_forbidden(self, proto):
        source = """
        from dataclasses import dataclass, InitVar

        @dataclass
        class Ex:
            a: int
            other: InitVar[int]
        """
        with temp_module(source) as mod:
            with pytest.raises(TypeError, match="`InitVar` fields are not supported"):
                proto.Decoder(mod.Ex)

    @pytest.mark.parametrize("slots", [False, True])
    def test_decode_dataclass(self, proto, slots):
        if slots:
            if not PY310:
                pytest.skip(reason="Python 3.10+ required")
            kws = {"slots": True}
        else:
            kws = {}

        @dataclass(**kws)
        class Example:
            a: int
            b: int
            c: int

        dec = proto.Decoder(Example)
        msg = Example(1, 2, 3)
        res = dec.decode(proto.encode(msg))
        assert res == msg

        # Extra fields ignored
        res = dec.decode(
            proto.encode({"x": -1, "a": 1, "y": -2, "b": 2, "z": -3, "c": 3, "": -4})
        )
        assert res == msg

        # Missing fields error
        with pytest.raises(msgspec.ValidationError, match="missing required field `b`"):
            dec.decode(proto.encode({"a": 1}))

        # Incorrect field types error
        with pytest.raises(
            msgspec.ValidationError, match=r"Expected `int`, got `str` - at `\$.a`"
        ):
            dec.decode(proto.encode({"a": "bad"}))

    @pytest.mark.parametrize("slots", [False, True])
    def test_decode_dataclass_defaults(self, proto, slots):
        if slots:
            if not PY310:
                pytest.skip(reason="Python 3.10+ required")
            kws = {"slots": True}
        else:
            kws = {}

        @dataclass(**kws)
        class Example:
            a: int
            b: int
            c: int = -3
            d: int = -4
            e: int = field(default_factory=lambda: -1000)

        dec = proto.Decoder(Example)
        for args in [(1, 2), (1, 2, 3), (1, 2, 3, 4), (1, 2, 3, 4, 5)]:
            msg = Example(*args)
        res = dec.decode(proto.encode(msg))
        assert res == msg

        # Missing fields error
        with pytest.raises(msgspec.ValidationError, match="missing required field `a`"):
            dec.decode(proto.encode({"c": 1, "d": 2, "e": 3}))

    def test_decode_dataclass_default_factory_errors(self, proto):
        def bad():
            raise ValueError("Oh no!")

        @dataclass
        class Example:
            a: int = field(default_factory=bad)

        with pytest.raises(ValueError, match="Oh no!"):
            proto.decode(proto.encode({}), type=Example)

    def test_decode_dataclass_post_init(self, proto):
        called = False

        @dataclass
        class Example:
            a: int

            def __post_init__(self):
                nonlocal called
                called = True

        res = proto.decode(proto.encode({"a": 1}), type=Example)
        assert res.a == 1
        assert called

    def test_decode_dataclass_post_init_errors(self, proto):
        @dataclass
        class Example:
            a: int

            def __post_init__(self):
                raise ValueError("Oh no!")

        with pytest.raises(ValueError, match="Oh no!"):
            proto.decode(proto.encode({"a": 1}), type=Example)

    def test_decode_dataclass_not_object(self, proto):
        @dataclass
        class Example:
            a: int
            b: int

        dec = proto.Decoder(Example)
        msg = proto.encode([])
        with pytest.raises(
            msgspec.ValidationError, match="Expected `object`, got `array`"
        ):
            dec.decode(msg)


class TestDate:
    def test_encode_date(self, proto):
        # All fields, zero padded
        x = datetime.date(1, 2, 3)
        s = proto.decode(proto.encode(x))
        assert s == "0001-02-03"

        # All fields, no zeros
        x = datetime.date(1234, 12, 31)
        s = proto.decode(proto.encode(x))
        assert s == "1234-12-31"

    @pytest.mark.parametrize(
        "s",
        [
            "0001-01-01",
            "9999-12-31",
            "0001-02-03",
            "2020-02-29",
        ],
    )
    def test_decode_date(self, proto, s):
        sol = datetime.date.fromisoformat(s)
        res = proto.decode(proto.encode(s), type=datetime.date)
        assert type(res) is datetime.date
        assert res == sol

    def test_decode_date_wrong_type(self, proto):
        msg = proto.encode([])
        with pytest.raises(
            msgspec.ValidationError, match="Expected `date`, got `array`"
        ):
            proto.decode(msg, type=datetime.date)

    @pytest.mark.parametrize(
        "s",
        [
            # Incorrect field lengths
            "001-02-03",
            "0001-2-03",
            "0001-02-3",
            # Trailing data
            "0001-02-0300",
            # Truncated
            "0001-02-",
            # Invalid characters
            "000a-02-03",
            "0001-0a-03",
            "0001-02-0a",
            # Year out of range
            "0000-02-03",
            # Month out of range
            "0001-00-03",
            "0001-13-03",
            # Day out of range for month
            "0001-02-00",
            "0001-02-29",
            "2000-02-30",
        ],
    )
    def test_decode_date_malformed(self, proto, s):
        msg = proto.encode(s)
        with pytest.raises(msgspec.ValidationError, match="Invalid RFC3339"):
            proto.decode(msg, type=datetime.date)


class TestUUID:
    def test_encode_uuid(self, proto):
        u = uuid.uuid4()
        res = proto.encode(u)
        sol = proto.encode(str(u))
        assert res == sol

    def test_encode_uuid_malformed_internals(self, proto):
        """Ensure that if some other code mutates the uuid object, we error
        nicely rather than segfaulting"""
        u = uuid.uuid4()
        object.__delattr__(u, "int")

        with pytest.raises(AttributeError):
            proto.encode(u)

        u = uuid.uuid4()
        object.__setattr__(u, "int", "oops")

        with pytest.raises(TypeError):
            proto.encode(u)

    @pytest.mark.parametrize("upper", [False, True])
    def test_decode_uuid(self, proto, upper):
        u = uuid.uuid4()
        s = str(u).upper() if upper else str(u)
        msg = proto.encode(s)
        res = proto.decode(msg, type=uuid.UUID)
        assert res == u
        assert res.is_safe == u.is_safe

    @pytest.mark.parametrize(
        "uuid_str",
        [
            # Truncated segments
            "1234567-1234-1234-1234-1234567890abc",
            "12345678-123-1234-1234-1234567890abc",
            "12345678-1234-123-1234-1234567890abc",
            "12345678-1234-1234-123-1234567890abc",
            "12345678-1234-1234-1234-1234567890a-",
            # Invalid character
            "1234567x-1234-1234-1234-1234567890ab",
            "12345678-123x-1234-1234-1234567890ab",
            "12345678-1234-123x-1234-1234567890ab",
            "12345678-1234-1234-123x-1234567890ab",
            "12345678-1234-1234-1234-1234567890ax",
            # Invalid dash
            "12345678.1234-1234-1234-1234567890ab",
            "12345678-1234.1234-1234-1234567890ab",
            "12345678-1234-1234.1234-1234567890ab",
            "12345678-1234-1234-1234.1234567890ab",
            # Trailing data
            "12345678-1234-1234-1234-1234567890ab-",
            "12345678-1234-1234-1234-1234567890abc",
        ],
    )
    def test_decode_uuid_malformed(self, proto, uuid_str):
        msg = proto.encode(uuid_str)
        with pytest.raises(msgspec.ValidationError, match="Invalid uuid"):
            proto.decode(msg, type=uuid.UUID)
