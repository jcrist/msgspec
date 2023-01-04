import datetime
import decimal
import enum
import uuid
from copy import deepcopy
from collections import namedtuple
from dataclasses import dataclass, field
from typing import (
    Any,
    NewType,
    List,
    Tuple,
    Set,
    FrozenSet,
    Dict,
    Union,
    Literal,
    NamedTuple,
    TypedDict,
)

import pytest

import msgspec
import msgspec.inspect as mi
from msgspec import Meta

from utils import temp_module

try:
    from typing import Annotated
except ImportError:
    try:
        from typing_extensions import Annotated
    except ImportError:
        pytestmark = pytest.mark.skip("Annotated types not available")


def type_index(typ, args):
    try:
        return typ[args]
    except TypeError:
        pytest.skip("Not supported in Python 3.8")


@pytest.mark.parametrize(
    "a,b,sol",
    [
        (
            {"a": {"b": {"c": 1}}},
            {"a": {"b": {"d": 2}}},
            {"a": {"b": {"c": 1, "d": 2}}},
        ),
        ({"a": {"b": {"c": 1}}}, {"a": {"b": 2}}, {"a": {"b": 2}}),
        ({"a": [1, 2]}, {"a": [3, 4]}, {"a": [1, 2, 3, 4]}),
        ({"a": {"b": 1}}, {"a2": 3}, {"a": {"b": 1}, "a2": 3}),
        ({"a": 1}, {}, {"a": 1}),
    ],
)
def test_merge_json(a, b, sol):
    a_orig = deepcopy(a)
    b_orig = deepcopy(b)
    res = mi._merge_json(a, b)
    assert res == sol
    assert a == a_orig
    assert b == b_orig


def test_unset():
    assert repr(mi.UNSET) == "UNSET"
    assert mi.UNSET.__reduce__() == "UNSET"


def test_inspect_module_dir():
    assert mi.__dir__() == mi.__all__


def test_any():
    assert mi.type_info(Any) == mi.AnyType()


def test_none():
    assert mi.type_info(None) == mi.NoneType()


def test_bool():
    assert mi.type_info(bool) == mi.BoolType()


@pytest.mark.parametrize(
    "kw", [{}, dict(ge=2), dict(gt=2), dict(le=2), dict(lt=2), dict(multiple_of=2)]
)
@pytest.mark.parametrize("typ, info_type", [(int, mi.IntType), (float, mi.FloatType)])
def test_numeric(kw, typ, info_type):
    if kw:
        typ = Annotated[typ, Meta(**kw)]
    assert mi.type_info(typ) == info_type(**kw)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(pattern="[a-z]*"), dict(min_length=0), dict(max_length=3)],
)
def test_string(kw):
    typ = str
    if kw:
        typ = Annotated[typ, Meta(**kw)]
    assert mi.type_info(typ) == mi.StrType(**kw)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(min_length=0), dict(max_length=3)],
)
@pytest.mark.parametrize(
    "typ, info_type", [(bytes, mi.BytesType), (bytearray, mi.ByteArrayType)]
)
def test_binary(kw, typ, info_type):
    if kw:
        typ = Annotated[typ, Meta(**kw)]
    assert mi.type_info(typ) == info_type(**kw)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(tz=None), dict(tz=True), dict(tz=False)],
)
def test_datetime(kw):
    typ = datetime.datetime
    if kw:
        typ = Annotated[typ, Meta(**kw)]
    assert mi.type_info(typ) == mi.DateTimeType(**kw)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(tz=None), dict(tz=True), dict(tz=False)],
)
def test_time(kw):
    typ = datetime.time
    if kw:
        typ = Annotated[typ, Meta(**kw)]
    assert mi.type_info(typ) == mi.TimeType(**kw)


def test_date():
    assert mi.type_info(datetime.date) == mi.DateType()


def test_uuid():
    assert mi.type_info(uuid.UUID) == mi.UUIDType()


def test_raw():
    assert mi.type_info(msgspec.Raw) == mi.RawType()


def test_msgpack_ext():
    assert mi.type_info(msgspec.msgpack.Ext) == mi.ExtType()


def test_newtype():
    UserId = NewType("UserId", str)
    assert mi.type_info(UserId) == mi.StrType()
    assert mi.type_info(Annotated[UserId, Meta(max_length=10)]) == mi.StrType(
        max_length=10
    )


def test_custom():
    assert mi.type_info(decimal.Decimal) == mi.CustomType(decimal.Decimal)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(min_length=0), dict(max_length=3)],
)
@pytest.mark.parametrize(
    "typ, info_type",
    [
        (list, mi.ListType),
        (tuple, mi.VarTupleType),
        (set, mi.SetType),
        (frozenset, mi.FrozenSetType),
        (List, mi.ListType),
        (Tuple, mi.VarTupleType),
        (Set, mi.SetType),
        (FrozenSet, mi.FrozenSetType),
    ],
)
@pytest.mark.parametrize("has_item_type", [False, True])
def test_sequence(kw, typ, info_type, has_item_type):
    if has_item_type:
        item_type = mi.IntType()
        if info_type is mi.VarTupleType:
            typ = type_index(typ, (int, ...))
        else:
            typ = type_index(typ, int)
    else:
        item_type = mi.AnyType()

    if kw:
        typ = Annotated[typ, Meta(**kw)]

    sol = info_type(item_type=item_type, **kw)
    assert mi.type_info(typ) == sol


@pytest.mark.parametrize("typ", [Tuple, tuple])
def test_tuple(typ):
    assert mi.type_info(type_index(typ, ())) == mi.TupleType(())
    assert mi.type_info(type_index(typ, int)) == mi.TupleType((mi.IntType(),))
    assert mi.type_info(type_index(typ, (int, float))) == mi.TupleType(
        (mi.IntType(), mi.FloatType())
    )


@pytest.mark.parametrize("typ", [Dict, dict])
@pytest.mark.parametrize("kw", [{}, dict(min_length=0), dict(max_length=3)])
@pytest.mark.parametrize("has_args", [False, True])
def test_dict(typ, kw, has_args):
    if has_args:
        typ = type_index(typ, (int, float))
        key = mi.IntType()
        val = mi.FloatType()
    else:
        key = val = mi.AnyType()
    if kw:
        typ = Annotated[typ, Meta(**kw)]
    sol = mi.DictType(key_type=key, value_type=val, **kw)
    assert mi.type_info(typ) == sol


@pytest.mark.parametrize("use_union_operator", [False, True])
def test_union(use_union_operator):
    if use_union_operator:
        try:
            typ = int | str
        except TypeError:
            pytest.skip("Union operator not supported")
    else:
        typ = Union[int, str]

    sol = mi.UnionType((mi.IntType(), mi.StrType()))
    assert mi.type_info(typ) == sol

    assert not sol.includes_none
    assert mi.type_info(Union[int, None]).includes_none


def test_int_literal():
    assert mi.type_info(Literal[3, 1, 2]) == mi.LiteralType((1, 2, 3))


def test_str_literal():
    assert mi.type_info(Literal["c", "a", "b"]) == mi.LiteralType(("a", "b", "c"))


def test_int_enum():
    class Example(enum.IntEnum):
        B = 3
        A = 2

    assert mi.type_info(Example) == mi.EnumType(Example)


def test_enum():
    class Example(enum.Enum):
        B = "z"
        A = "y"

    assert mi.type_info(Example) == mi.EnumType(Example)


@pytest.mark.parametrize(
    "kw",
    [
        {},
        {"array_like": True},
        {"forbid_unknown_fields": True},
        {"tag": "Example", "tag_field": "type"},
    ],
)
def test_struct(kw):
    class Example(msgspec.Struct, **kw):
        x: int
        y: int = 0

    sol = mi.StructType(
        cls=Example,
        fields=(
            mi.Field(name="x", encode_name="x", type=mi.IntType()),
            mi.Field(
                name="y", encode_name="y", type=mi.IntType(), required=False, default=0
            ),
        ),
        **kw
    )
    assert mi.type_info(Example) == sol


def test_struct_no_fields():
    class Example(msgspec.Struct):
        pass

    sol = mi.StructType(Example, fields=())
    assert mi.type_info(Example) == sol


def test_struct_keyword_only():
    class Example(msgspec.Struct, kw_only=True):
        a: int
        b: int = 1
        c: int
        d: int = 2

    sol = mi.StructType(
        Example,
        fields=(
            mi.Field("a", "a", mi.IntType()),
            mi.Field("b", "b", mi.IntType(), required=False, default=1),
            mi.Field("c", "c", mi.IntType()),
            mi.Field("d", "d", mi.IntType(), required=False, default=2),
        ),
    )
    assert mi.type_info(Example) == sol


def test_struct_encode_name():
    class Example(msgspec.Struct, rename="camel"):
        field_one: int
        field_two: int

    sol = mi.StructType(
        Example,
        fields=(
            mi.Field("field_one", "fieldOne", mi.IntType()),
            mi.Field("field_two", "fieldTwo", mi.IntType()),
        ),
    )
    assert mi.type_info(Example) == sol


def test_typing_namedtuple():
    class Example(NamedTuple):
        a: str
        b: bool
        c: int = 0

    sol = mi.NamedTupleType(
        Example,
        fields=(
            mi.Field("a", "a", mi.StrType()),
            mi.Field("b", "b", mi.BoolType()),
            mi.Field("c", "c", mi.IntType(), required=False, default=0),
        ),
    )
    assert mi.type_info(Example) == sol


def test_collections_namedtuple():
    Example = namedtuple("Example", ["a", "b", "c"], defaults=(0,))

    sol = mi.NamedTupleType(
        Example,
        fields=(
            mi.Field("a", "a", mi.AnyType()),
            mi.Field("b", "b", mi.AnyType()),
            mi.Field("c", "c", mi.AnyType(), required=False, default=0),
        ),
    )
    assert mi.type_info(Example) == sol


@pytest.mark.parametrize("use_typing_extensions", [False, True])
def test_typeddict(use_typing_extensions):
    if use_typing_extensions:
        tex = pytest.importorskip("typing_extensions")
        cls = tex.TypedDict
    else:
        cls = TypedDict

    class Example(cls):
        a: str
        b: bool
        c: int

    sol = mi.TypedDictType(
        Example,
        fields=(
            mi.Field("a", "a", mi.StrType()),
            mi.Field("b", "b", mi.BoolType()),
            mi.Field("c", "c", mi.IntType()),
        ),
    )
    assert mi.type_info(Example) == sol


@pytest.mark.parametrize("use_typing_extensions", [False, True])
def test_typeddict_optional(use_typing_extensions):
    if use_typing_extensions:
        tex = pytest.importorskip("typing_extensions")
        cls = tex.TypedDict
    else:
        cls = TypedDict

    class Base(cls):
        a: str
        b: bool

    class Example(Base, total=False):
        c: int

    if not hasattr(Example, "__required_keys__"):
        # This should be Python 3.8, builtin typing only
        pytest.skip("partially optional TypedDict not supported")

    sol = mi.TypedDictType(
        Example,
        fields=(
            mi.Field("a", "a", mi.StrType()),
            mi.Field("b", "b", mi.BoolType()),
            mi.Field("c", "c", mi.IntType(), required=False),
        ),
    )
    assert mi.type_info(Example) == sol


def test_dataclass():
    @dataclass
    class Example:
        x: int
        y: int = 0
        z: str = field(default_factory=str)

    sol = mi.DataclassType(
        Example,
        fields=(
            mi.Field("x", "x", mi.IntType()),
            mi.Field("y", "y", mi.IntType(), required=False, default=0),
            mi.Field("z", "z", mi.StrType(), required=False, default_factory=str),
        ),
    )
    assert mi.type_info(Example) == sol


@pytest.mark.parametrize("kind", ["struct", "namedtuple", "typeddict", "dataclass"])
def test_self_referential_objects(kind):
    if kind == "struct":
        code = """
        import msgspec

        class Node(msgspec.Struct):
            child: "Node"
        """
    elif kind == "namedtuple":
        code = """
        from typing import NamedTuple

        class Node(NamedTuple):
            child: "Node"
        """
    elif kind == "typeddict":
        code = """
        from typing import TypedDict

        class Node(TypedDict):
            child: "Node"
        """
    elif kind == "dataclass":
        code = """
        from dataclasses import dataclass

        @dataclass
        class Node:
            child: "Node"
        """

    with temp_module(code) as mod:
        res = mi.type_info(mod.Node)

    assert res.cls is mod.Node
    assert res.fields[0].name == "child"
    assert res.fields[0].type is res


def test_metadata():
    typ = Annotated[int, Meta(gt=1, title="a"), Meta(description="b")]

    assert mi.type_info(typ) == mi.Metadata(
        mi.IntType(gt=1), {"title": "a", "description": "b"}
    )

    typ = Annotated[
        int,
        Meta(extra_json_schema={"title": "a", "description": "b"}),
        Meta(extra_json_schema={"title": "c", "examples": [1, 2]}),
    ]

    assert mi.type_info(typ) == mi.Metadata(
        mi.IntType(), {"title": "c", "description": "b", "examples": [1, 2]}
    )


@pytest.mark.parametrize("protocol", [None, "msgpack", "json"])
def test_type_info_protocol(protocol):
    res = mi.type_info(Dict[str, int], protocol=protocol)
    assert res == mi.DictType(mi.StrType(), mi.IntType())

    if protocol == "json":
        with pytest.raises(TypeError):
            mi.type_info(Dict[float, int], protocol=protocol)
    else:
        res = mi.type_info(Dict[float, int], protocol=protocol)
        assert res == mi.DictType(mi.FloatType(), mi.IntType())


def test_type_info_protocol_invalid():
    with pytest.raises(ValueError, match="protocol must be one of"):
        mi.type_info(int, protocol="invalid")


def test_multi_type_info():
    class Example(msgspec.Struct):
        x: int
        y: int

    ex_type = mi.StructType(
        Example,
        fields=(
            mi.Field("x", "x", mi.IntType()),
            mi.Field("y", "y", mi.IntType()),
        ),
    )

    assert mi.multi_type_info([]) == ()

    res = mi.multi_type_info([Example, List[Example]])
    assert res == (ex_type, mi.ListType(ex_type))
    assert res[0] is res[1].item_type
