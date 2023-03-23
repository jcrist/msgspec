import base64
import datetime
import decimal
import enum
import os
import sys
import uuid
import weakref
from dataclasses import dataclass
from typing import Any, NamedTuple, Union

import pytest

from msgspec import UNSET, EncodeError, Struct, UnsetType, to_builtins

PY310 = sys.version_info[:2] >= (3, 10)
PY311 = sys.version_info[:2] >= (3, 11)

py310_plus = pytest.mark.skipif(not PY310, reason="3.10+ only")
py311_plus = pytest.mark.skipif(not PY311, reason="3.11+ only")

slots_params = [False, pytest.param(True, marks=[py310_plus])]


class FruitInt(enum.IntEnum):
    APPLE = -1
    BANANA = 2

    def __eq__(self, other):
        assert type(other) is type(self)
        return super().__eq__(other)

    def __hash__(self):
        return super().__hash__()


class FruitStr(enum.Enum):
    APPLE = "apple"
    BANANA = "banana"

    def __eq__(self, other):
        assert type(other) is type(self)
        return super().__eq__(other)

    def __hash__(self):
        return super().__hash__()


class Bad:
    """A type that msgspec doesn't natively handle"""


class TestToBuiltins:
    def test_to_builtins_bad_calls(self):
        with pytest.raises(TypeError):
            to_builtins()

        with pytest.raises(
            TypeError, match="builtin_types must be an iterable of types"
        ):
            to_builtins([1], builtin_types=1)

        with pytest.raises(TypeError) as rec:
            to_builtins([1], builtin_types=(int,))
        assert "Cannot treat" in str(rec.value)
        assert "int" in str(rec.value)

        with pytest.raises(TypeError, match="enc_hook must be callable"):
            to_builtins([1], enc_hook=1)

    def test_to_builtins_enc_hook_explicit_none(self):
        assert to_builtins(1, enc_hook=None) == 1

    @pytest.mark.parametrize("case", [1, 2, 3, 4, 5])
    def test_to_builtins_recursive(self, case):
        if case == 1:
            o = []
            o.append(o)
        elif case == 2:
            o = ([],)
            o[0].append(o)
        elif case == 3:
            o = {}
            o["a"] = o
        elif case == 4:

            class Box(Struct):
                a: "Box"

            o = Box(None)
            o.a = o
        elif case == 5:

            @dataclass
            class Box:
                a: "Box"

            o = Box(None)
            o.a = o

        with pytest.raises(RecursionError):
            to_builtins(o)

    def test_none(self):
        assert to_builtins(None) is None

    def test_bool(self):
        assert to_builtins(False) is False
        assert to_builtins(True) is True

    def test_int(self):
        assert to_builtins(1) == 1

    def test_float(self):
        assert to_builtins(1.5) == 1.5

    def test_str(self):
        assert to_builtins("abc") == "abc"

    @pytest.mark.parametrize("typ", [bytes, bytearray, memoryview])
    @pytest.mark.parametrize("size", range(5))
    def test_binary(self, typ, size):
        msg = typ(os.urandom(size))
        res = to_builtins(msg)
        sol = base64.b64encode(msg).decode("utf-8")
        assert res == sol

    @pytest.mark.parametrize("typ", [bytes, bytearray, memoryview])
    def test_binary_builtin_types(self, typ):
        msg = typ(b"\x01\x02\x03")
        res = to_builtins(msg, builtin_types=(typ,))
        assert res is msg

    @pytest.mark.parametrize("tzinfo", [None, datetime.timezone.utc])
    @pytest.mark.parametrize("microsecond", [123456, 123, 0])
    def test_datetime(self, tzinfo, microsecond):
        msg = datetime.datetime.now(tzinfo).replace(microsecond=microsecond)
        res = to_builtins(msg)
        sol = msg.isoformat().replace("+00:00", "Z")
        assert res == sol

    def test_datetime_builtin_types(self):
        msg = datetime.datetime.now()
        res = to_builtins(msg, builtin_types=(datetime.datetime,))
        assert res is msg

    def test_date(self):
        msg = datetime.date.today()
        res = to_builtins(msg)
        sol = msg.isoformat()
        assert res == sol

    def test_date_builtin_types(self):
        msg = datetime.date.today()
        res = to_builtins(msg, builtin_types=(datetime.date,))
        assert res is msg

    @pytest.mark.parametrize("tzinfo", [None, datetime.timezone.utc])
    @pytest.mark.parametrize("microsecond", [123456, 123, 0])
    def test_time(self, tzinfo, microsecond):
        msg = datetime.datetime.now(tzinfo).replace(microsecond=microsecond).timetz()
        res = to_builtins(msg)
        sol = msg.isoformat().replace("+00:00", "Z")
        assert res == sol

    def test_time_builtin_types(self):
        msg = datetime.datetime.now().time()
        res = to_builtins(msg, builtin_types=(datetime.time,))
        assert res is msg

    def test_uuid(self):
        msg = uuid.uuid4()
        assert to_builtins(msg) == str(msg)

    def test_uuid_builtin_types(self):
        msg = uuid.uuid4()
        res = to_builtins(msg, builtin_types=(uuid.UUID,))
        assert res is msg

    def test_decimal(self):
        msg = decimal.Decimal("1.5")
        assert to_builtins(msg) == str(msg)

    def test_decimal_builtin_types(self):
        msg = decimal.Decimal("1.5")
        res = to_builtins(msg, builtin_types=(decimal.Decimal,))
        assert res is msg

    def test_intenum(self):
        res = to_builtins(FruitInt.APPLE)
        assert res == -1
        assert type(res) is int

    def test_enum(self):
        res = to_builtins(FruitStr.APPLE)
        assert res == "apple"
        assert type(res) is str

    def test_enum_invalid(self):
        class Bad(enum.Enum):
            x = (1, 2)

        with pytest.raises(EncodeError, match="Only enums with int or str"):
            to_builtins(Bad.x)

    @pytest.mark.parametrize(
        "in_type, out_type",
        [(list, list), (tuple, tuple), (set, list), (frozenset, list)],
    )
    @pytest.mark.parametrize("subclass", [False, True])
    def test_sequence(self, in_type, out_type, subclass):
        if subclass:

            class in_type(in_type):
                pass

        msg = in_type([1, FruitInt.APPLE])
        res = to_builtins(msg)
        assert res == out_type([1, -1])
        assert res is not msg

        res = to_builtins(in_type())
        assert res == out_type()

    @pytest.mark.parametrize("in_type", [list, tuple, set, frozenset])
    def test_sequence_unsupported_item(self, in_type):
        msg = in_type([1, Bad(), 3])
        with pytest.raises(TypeError, match="Encoding objects of type Bad"):
            to_builtins(msg)

    def test_namedtuple(self):
        class Point(NamedTuple):
            x: int
            y: FruitInt

        assert to_builtins(Point(1, FruitInt.APPLE)) == (1, -1)

    @pytest.mark.parametrize("subclass", [False, True])
    def test_dict(self, subclass):
        if subclass:

            class in_type(dict):
                pass

        else:
            in_type = dict

        msg = in_type({FruitStr.BANANA: 1, "b": [FruitInt.APPLE], 3: "three"})
        sol = {"banana": 1, "b": [-1], 3: "three"}

        res = to_builtins(msg)
        assert res == sol
        assert res is not msg

        res = to_builtins(in_type())
        assert res == {}

    def test_dict_unsupported_key(self):
        msg = {Bad(): 1}
        with pytest.raises(TypeError, match="Encoding objects of type Bad"):
            to_builtins(msg)

    def test_dict_unsupported_value(self):
        msg = {"x": Bad()}
        with pytest.raises(TypeError, match="Encoding objects of type Bad"):
            to_builtins(msg)

    def test_dict_str_keys(self):
        assert to_builtins({FruitStr.BANANA: 1}, str_keys=True) == {"banana": 1}
        assert to_builtins({"banana": 1}, str_keys=True) == {"banana": 1}
        assert to_builtins({FruitInt.BANANA: 1}, str_keys=True) == {"2": 1}
        assert to_builtins({2: 1}, str_keys=True) == {"2": 1}

        with pytest.raises(
            TypeError, match="Only dicts with `str` or `int` keys are supported"
        ):
            to_builtins({(1, 2): 3}, str_keys=True)

    def test_dict_sequence_keys(self):
        msg = {frozenset([1, 2]): 1}
        assert to_builtins(msg) == {(1, 2): 1}

        with pytest.raises(
            TypeError, match="Only dicts with `str` or `int` keys are supported"
        ):
            to_builtins(msg, str_keys=True)

    @pytest.mark.parametrize("tagged", [False, True])
    def test_struct_object(self, tagged):
        class Ex(Struct, tag=tagged):
            x: int
            y: FruitInt

        sol = {"type": "Ex", "x": 1, "y": -1} if tagged else {"x": 1, "y": -1}
        assert to_builtins(Ex(1, FruitInt.APPLE)) == sol

    def test_struct_object_omit_defaults(self):
        class Ex(Struct, omit_defaults=True):
            x: int
            a: list = []
            b: FruitStr = FruitStr.BANANA
            c: FruitInt = FruitInt.APPLE

        assert to_builtins(Ex(1)) == {"x": 1}
        assert to_builtins(Ex(1, a=[2])) == {"x": 1, "a": [2]}
        assert to_builtins(Ex(1, b=FruitStr.APPLE)) == {"x": 1, "b": "apple"}

    @pytest.mark.parametrize("tagged", [False, True])
    def test_struct_array(self, tagged):
        class Ex(Struct, array_like=True, tag=tagged):
            x: int
            y: FruitInt

        sol = ["Ex", 1, -1] if tagged else [1, -1]
        assert to_builtins(Ex(1, FruitInt.APPLE)) == sol

    @pytest.mark.parametrize("tagged", [False, True])
    def test_struct_array_keys(self, tagged):
        class Ex(Struct, array_like=True, tag=tagged, frozen=True):
            x: int
            y: FruitInt

        msg = {Ex(1, FruitInt.APPLE): "abc"}
        sol = {("Ex", 1, -1) if tagged else (1, -1): "abc"}
        assert to_builtins(msg) == sol

    @pytest.mark.parametrize("array_like", [False, True])
    def test_struct_unsupported_value(self, array_like):
        class Ex(Struct):
            a: Any
            b: Any

        msg = Ex(1, Bad())
        with pytest.raises(TypeError, match="Encoding objects of type Bad"):
            to_builtins(msg)

    @pytest.mark.parametrize("slots", slots_params)
    def test_dataclass(self, slots):
        @dataclass(**({"slots": True} if slots else {}))
        class Ex:
            x: int
            y: FruitInt

        msg = Ex(1, FruitInt.APPLE)
        assert to_builtins(msg) == {"x": 1, "y": -1}

    @pytest.mark.parametrize("slots", slots_params)
    def test_dataclass_missing_fields(self, slots):
        @dataclass(**({"slots": True} if slots else {}))
        class Ex:
            x: int
            y: int
            z: int

        x = Ex(1, 2, 3)
        sol = {"x": 1, "y": 2, "z": 3}
        for key in "xyz":
            delattr(x, key)
            del sol[key]
            assert to_builtins(x) == sol

    @pytest.mark.parametrize("slots_base", slots_params)
    @pytest.mark.parametrize("slots", slots_params)
    def test_dataclass_subclasses(self, slots_base, slots):
        @dataclass(**({"slots": True} if slots_base else {}))
        class Base:
            x: int
            y: int

        @dataclass(**({"slots": True} if slots else {}))
        class Ex(Base):
            y: int
            z: int

        x = Ex(1, 2, 3)
        res = to_builtins(x)
        assert res == {"x": 1, "y": 2, "z": 3}

        # Missing attribute ignored
        del x.y
        res = to_builtins(x)
        assert res == {"x": 1, "z": 3}

    @py311_plus
    def test_dataclass_weakref_slot(self):
        @dataclass(slots=True, weakref_slot=True)
        class Ex:
            x: int
            y: int

        x = Ex(1, 2)
        ref = weakref.ref(x)  # noqa
        res = to_builtins(x)
        assert res == {"x": 1, "y": 2}

    @pytest.mark.parametrize("slots", slots_params)
    def test_dataclass_skip_leading_underscore(self, slots):
        @dataclass(**({"slots": True} if slots else {}))
        class Ex:
            x: int
            y: int
            _z: int

        x = Ex(1, 2, 3)
        res = to_builtins(x)
        assert res == {"x": 1, "y": 2}

    @pytest.mark.parametrize("slots", slots_params)
    def test_dataclass_unsupported_value(self, slots):
        @dataclass(**({"slots": True} if slots else {}))
        class Ex:
            x: Any
            y: Any

        msg = Ex(1, Bad())
        with pytest.raises(TypeError, match="Encoding objects of type Bad"):
            to_builtins(msg)

    @pytest.mark.parametrize("slots", [True, False])
    def test_attrs(self, slots):
        attrs = pytest.importorskip("attrs")

        @attrs.define(slots=slots)
        class Ex:
            x: int
            y: FruitInt

        msg = Ex(1, FruitInt.APPLE)
        assert to_builtins(msg) == {"x": 1, "y": -1}

    @pytest.mark.parametrize("kind", ["struct", "dataclass", "attrs"])
    def test_unset_fields(self, kind):
        if kind == "struct":

            class Ex(Struct):
                x: Union[int, UnsetType]
                y: Union[int, UnsetType]

        elif kind == "dataclass":

            @dataclass
            class Ex:
                x: Union[int, UnsetType]
                y: Union[int, UnsetType]

        elif kind == "attrs":
            attrs = pytest.importorskip("attrs")

            @attrs.define
            class Ex:
                x: Union[int, UnsetType]
                y: Union[int, UnsetType]

        res = to_builtins(Ex(1, UNSET))
        assert res == {"x": 1}

        res = to_builtins(Ex(UNSET, 2))
        assert res == {"y": 2}

        res = to_builtins(Ex(UNSET, UNSET))
        assert res == {}

    def test_unset_errors_in_other_contexts(self):
        with pytest.raises(TypeError):
            to_builtins(UNSET)

    def test_custom(self):
        with pytest.raises(TypeError, match="Encoding objects of type Bad"):
            to_builtins(Bad())

        assert to_builtins(Bad(), enc_hook=lambda x: "bad") == "bad"
