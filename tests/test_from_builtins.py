import datetime
import decimal
import enum
import gc
import inspect
import sys
import uuid
from base64 import b64encode
from contextlib import contextmanager
from dataclasses import dataclass, field
from typing import (
    Any,
    Dict,
    FrozenSet,
    Generic,
    List,
    Literal,
    NamedTuple,
    Set,
    Tuple,
    TypeVar,
    Union,
)

import pytest
from utils import temp_module

from msgspec import Meta, Struct, ValidationError, from_builtins, to_builtins

try:
    from typing import Annotated
except ImportError:
    try:
        from typing_extensions import Annotated
    except ImportError:
        Annotated = None

try:
    from typing import TypedDict
except ImportError:
    try:
        from typing_extensions import TypedDict
    except ImportError:
        TypedDict = None

try:
    import attrs
except ImportError:
    attrs = None

PY310 = sys.version_info[:2] >= (3, 10)
PY311 = sys.version_info[:2] >= (3, 11)

uses_annotated = pytest.mark.skipif(Annotated is None, reason="Annotated not available")

UTC = datetime.timezone.utc

T = TypeVar("T")


class AttrDict:
    def __init__(self, _data=None, **kwargs):
        self._data = _data or {}
        self._data.update(kwargs)

    def __getattr__(self, key):
        try:
            return self._data[key]
        except KeyError:
            raise AttributeError(key) from None


def KWList(**kwargs):
    return list(kwargs.values())


def assert_eq(x, y):
    assert x == y
    assert type(x) is type(y)


@contextmanager
def max_call_depth(n):
    cur_depth = len(inspect.stack(0))
    orig = sys.getrecursionlimit()
    try:
        # Our measure of the current stack depth can be off by a bit. Trying to
        # set a recursionlimit < the current depth will raise a RecursionError.
        # We just try again with a slightly higher limit, bailing after an
        # unreasonable amount of adjustments.
        #
        # Note that python 3.8 also has a minimum recursion limit of 64, so
        # there's some additional fiddliness there.
        for i in range(64):
            try:
                sys.setrecursionlimit(cur_depth + i + n)
                break
            except RecursionError:
                pass
        else:
            raise ValueError(
                "Failed to set low recursion limit, something is wrong here"
            )
        yield
    finally:
        sys.setrecursionlimit(orig)


def roundtrip(obj, typ):
    return from_builtins(to_builtins(obj), typ)


class TestFromBuiltins:
    def test_bad_calls(self):
        with pytest.raises(TypeError):
            from_builtins()

        with pytest.raises(TypeError):
            from_builtins(1)

        with pytest.raises(
            TypeError, match="builtin_types must be an iterable of types"
        ):
            from_builtins(1, int, builtin_types=1)

        with pytest.raises(TypeError) as rec:
            from_builtins(1, int, builtin_types=(int,))
        assert "Cannot treat" in str(rec.value)
        assert "int" in str(rec.value)

        with pytest.raises(TypeError, match="dec_hook must be callable"):
            from_builtins(1, int, dec_hook=1)

    def test_dec_hook_explicit_none(self):
        assert from_builtins(1, int, dec_hook=None) == 1

    def test_custom_input_type(self):
        class Custom:
            pass

        with pytest.raises(ValidationError, match="Expected `int`, got `Custom`"):
            from_builtins(Custom(), int)

    def test_custom_input_type_works_with_any(self):
        class Custom:
            pass

        x = Custom()
        res = from_builtins(x, Any)
        assert res is x
        assert sys.getrefcount(x) == 3  # x + res + 1

    def test_custom_input_type_works_with_custom(self):
        class Custom:
            pass

        x = Custom()
        res = from_builtins(x, Custom)
        assert res is x
        assert sys.getrefcount(x) == 3  # x + res + 1

    def test_custom_input_type_works_with_dec_hook(self):
        class Custom:
            pass

        class Custom2:
            pass

        def dec_hook(typ, x):
            if typ is Custom2:
                assert isinstance(x, Custom)
                return Custom2()
            raise TypeError

        x = Custom()
        res = from_builtins(x, Custom2, dec_hook=dec_hook)
        assert isinstance(res, Custom2)
        assert sys.getrefcount(res) == 2  # res + 1
        assert sys.getrefcount(x) == 2  # x + 1

    def test_unsupported_output_type(self):
        with pytest.raises(TypeError, match="more than one array-like"):
            from_builtins({}, Union[List[int], Tuple[str, ...]])

    @pytest.mark.parametrize(
        "val, got",
        [
            (None, "null"),
            (True, "bool"),
            (1, "int"),
            (1.5, "float"),
            ("a", "str"),
            (b"b", "bytes"),
            (bytearray(b"c"), "bytes"),
            (datetime.datetime(2022, 1, 2), "datetime"),
            (datetime.time(12, 34), "time"),
            (datetime.date(2022, 1, 2), "date"),
            (uuid.uuid4(), "uuid"),
            (decimal.Decimal("1.5"), "decimal"),
            ([1], "array"),
            ((1,), "array"),
            ({"a": 1}, "object"),
        ],
    )
    def test_wrong_type(self, val, got):
        # An arbitrary wrong type,
        if isinstance(val, int):
            typ = str
            expected = "str"
        else:
            typ = int
            expected = "int"
        msg = f"Expected `{expected}`, got `{got}`"
        with pytest.raises(ValidationError, match=msg):
            from_builtins(val, typ)


class TestNone:
    def test_none(self):
        assert from_builtins(None, Any) is None
        assert from_builtins(None, None) is None
        with pytest.raises(ValidationError, match="Expected `null`, got `int`"):
            from_builtins(1, None)


class TestBool:
    @pytest.mark.parametrize("val", [True, False])
    def test_bool(self, val):
        assert from_builtins(val, Any) is val
        assert from_builtins(val, bool) is val

    def test_bool_invalid(self):
        with pytest.raises(ValidationError, match="Expected `bool`, got `int`"):
            from_builtins(1, bool)

        with pytest.raises(ValidationError, match="Expected `bool`, got `str`"):
            from_builtins("true", bool)


class TestInt:
    def test_int(self):
        assert from_builtins(1, Any) == 1
        assert from_builtins(1, int) == 1
        with pytest.raises(ValidationError, match="Expected `int`, got `float`"):
            from_builtins(1.5, int)

    @pytest.mark.parametrize("val", [2**64, -(2**63) - 1])
    def test_int_out_of_range(self, val):
        with pytest.raises(ValidationError, match="Integer value out of range"):
            from_builtins(val, int)

    @pytest.mark.parametrize(
        "name, bound, good, bad",
        [
            ("ge", -1, [-1, 2**63], [-2]),
            ("gt", -1, [0, 2**63], [-1]),
            ("le", -1, [-1], [0, 2**63]),
            ("lt", -1, [-2], [-1, 2**63]),
        ],
    )
    @uses_annotated
    def test_int_constr_bounds(self, name, bound, good, bad):
        class Ex(Struct):
            x: Annotated[int, Meta(**{name: bound})]

        for x in good:
            assert from_builtins({"x": x}, Ex).x == x

        op = ">=" if name.startswith("g") else "<="
        offset = {"lt": -1, "gt": 1}.get(name, 0)
        err_msg = rf"Expected `int` {op} {bound + offset} - at `\$.x`"
        for x in bad:
            with pytest.raises(ValidationError, match=err_msg):
                from_builtins({"x": x}, Ex)

    @uses_annotated
    def test_int_constr_multiple_of(self):
        class Ex(Struct):
            x: Annotated[int, Meta(multiple_of=2)]

        for x in [-2, 0, 2, 40, 2**63 + 2]:
            assert from_builtins({"x": x}, Ex).x == x

        err_msg = r"Expected `int` that's a multiple of 2 - at `\$.x`"
        for x in [1, -1, 2**63 + 1]:
            with pytest.raises(ValidationError, match=err_msg):
                from_builtins({"x": x}, Ex)

    @pytest.mark.parametrize(
        "meta, good, bad",
        [
            (Meta(ge=0, le=10, multiple_of=2), [0, 2, 10], [-1, 1, 11]),
            (Meta(ge=0, multiple_of=2), [0, 2**63 + 2], [-2, 2**63 + 1]),
            (Meta(le=0, multiple_of=2), [0, -(2**63)], [-1, 2, 2**63]),
            (Meta(ge=0, le=10), [0, 10], [-1, 11]),
        ],
    )
    @uses_annotated
    def test_int_constrs(self, meta, good, bad):
        class Ex(Struct):
            x: Annotated[int, meta]

        for x in good:
            assert from_builtins({"x": x}, Ex).x == x

        for x in bad:
            with pytest.raises(ValidationError):
                from_builtins({"x": x}, Ex)

    def test_int_subclass(self):
        class MyInt(int):
            pass

        for val in [10, 0, -10]:
            sol = from_builtins(MyInt(val), int)
            assert type(sol) is int
            assert sol == val

        x = MyInt(100)
        sol = from_builtins(x, MyInt)
        assert sol is x
        assert sys.getrefcount(x) == 3  # x + sol + 1


class TestFloat:
    def test_float(self):
        assert from_builtins(1.5, Any) == 1.5
        assert from_builtins(1.5, float) == 1.5
        res = from_builtins(1, float)
        assert res == 1.0
        assert isinstance(res, float)
        with pytest.raises(ValidationError, match="Expected `float`, got `null`"):
            from_builtins(None, float)

    @pytest.mark.parametrize(
        "meta, good, bad",
        [
            (Meta(ge=0.0, le=10.0, multiple_of=2.0), [0, 2.0, 10], [-2, 11, 3]),
            (Meta(ge=0.0, multiple_of=2.0), [0, 2, 10.0], [-2, 3]),
            (Meta(le=10.0, multiple_of=2.0), [-2.0, 10.0], [11.0, 3.0]),
            (Meta(ge=0.0, le=10.0), [0.0, 2.0, 10.0], [-1.0, 11.5, 11]),
        ],
    )
    @uses_annotated
    def test_float_constrs(self, meta, good, bad):
        class Ex(Struct):
            x: Annotated[float, meta]

        for x in good:
            assert from_builtins({"x": x}, Ex).x == x

        for x in bad:
            with pytest.raises(ValidationError):
                from_builtins({"x": x}, Ex)


class TestStr:
    def test_str(self):
        assert from_builtins("test", Any) == "test"
        assert from_builtins("test", str) == "test"
        with pytest.raises(ValidationError, match="Expected `str`, got `bytes`"):
            from_builtins(b"test", str)

    @pytest.mark.parametrize(
        "meta, good, bad",
        [
            (
                Meta(min_length=2, max_length=3, pattern="x"),
                ["xy", "xyz"],
                ["x", "yy", "wxyz"],
            ),
            (Meta(min_length=2, max_length=4), ["xx", "xxxx"], ["x", "xxxxx"]),
            (Meta(min_length=2, pattern="x"), ["xy", "wxyz"], ["x", "bad"]),
            (Meta(max_length=3, pattern="x"), ["xy", "xyz"], ["y", "wxyz"]),
        ],
    )
    @uses_annotated
    def test_str_constrs(self, meta, good, bad):
        class Ex(Struct):
            x: Annotated[str, meta]

        for x in good:
            assert from_builtins({"x": x}, Ex).x == x

        for x in bad:
            with pytest.raises(ValidationError):
                from_builtins({"x": x}, Ex)


class TestBinary:
    @pytest.mark.parametrize("out_type", [bytes, bytearray])
    def test_binary_wrong_type(self, out_type):
        with pytest.raises(ValidationError, match="Expected `bytes`, got `int`"):
            from_builtins(1, out_type)

    @pytest.mark.parametrize("in_type", [bytes, bytearray])
    @pytest.mark.parametrize("out_type", [bytes, bytearray])
    def test_binary_builtin(self, in_type, out_type):
        res = from_builtins(in_type(b"test"), out_type)
        assert res == b"test"
        assert isinstance(res, out_type)

    @pytest.mark.parametrize("out_type", [bytes, bytearray])
    def test_binary_base64(self, out_type):
        res = from_builtins("AQI=", out_type)
        assert res == b"\x01\x02"
        assert isinstance(res, out_type)

    @pytest.mark.parametrize("out_type", [bytes, bytearray])
    def test_binary_base64_disabled(self, out_type):
        with pytest.raises(ValidationError, match="Expected `bytes`, got `str`"):
            from_builtins("AQI=", out_type, builtin_types=(bytes, bytearray))

    @pytest.mark.parametrize("in_type", [bytes, bytearray, str])
    @pytest.mark.parametrize("out_type", [bytes, bytearray])
    @uses_annotated
    def test_binary_constraints(self, in_type, out_type):
        class Ex(Struct):
            x: Annotated[out_type, Meta(min_length=2, max_length=4)]

        for x in [b"xx", b"xxx", b"xxxx"]:
            if in_type is str:
                msg = {"x": b64encode(x).decode("utf-8")}
            else:
                msg = {"x": in_type(x)}
            assert from_builtins(msg, Ex).x == x

        for x in [b"x", b"xxxxx"]:
            if in_type is str:
                msg = {"x": b64encode(x).decode("utf-8")}
            else:
                msg = {"x": in_type(x)}
            with pytest.raises(ValidationError):
                from_builtins(msg, Ex)

    def test_bytes_subclass(self):
        class MyBytes(bytes):
            pass

        msg = MyBytes(b"abc")

        for typ in [bytes, bytearray]:
            sol = from_builtins(msg, typ)
            assert type(sol) is typ
            assert sol == b"abc"

        assert sys.getrefcount(msg) == 2  # msg + 1
        sol = from_builtins(msg, MyBytes)
        assert sol is msg
        assert sys.getrefcount(msg) == 3  # msg + sol + 1


class TestDateTime:
    def test_datetime_wrong_type(self):
        with pytest.raises(ValidationError, match="Expected `datetime`, got `int`"):
            from_builtins(1, datetime.datetime)

    @pytest.mark.parametrize("tz", [False, True])
    def test_datetime_builtin(self, tz):
        dt = datetime.datetime.now(UTC if tz else None)
        assert from_builtins(dt, datetime.datetime) is dt

    @pytest.mark.parametrize("tz", [False, True])
    def test_datetime_str(self, tz):
        sol = datetime.datetime(1, 2, 3, 4, 5, 6, 7, UTC if tz else None)
        msg = "0001-02-03T04:05:06.000007" + ("Z" if tz else "")
        res = from_builtins(msg, datetime.datetime)
        assert res == sol

    def test_datetime_str_disabled(self):
        with pytest.raises(ValidationError, match="Expected `datetime`, got `str`"):
            from_builtins(
                "0001-02-03T04:05:06.000007Z",
                datetime.datetime,
                builtin_types=(datetime.datetime,),
            )

    @pytest.mark.parametrize("as_str", [False, True])
    @uses_annotated
    def test_datetime_constrs(self, as_str):
        class Ex(Struct):
            x: Annotated[datetime.datetime, Meta(tz=True)]

        builtin_types = None if as_str else (datetime.datetime,)

        aware = Ex(datetime.datetime(1, 2, 3, 4, 5, 6, 7, UTC))
        aware_msg = to_builtins(aware, builtin_types=builtin_types)
        naive = Ex(datetime.datetime(1, 2, 3, 4, 5, 6, 7))
        naive_msg = to_builtins(naive, builtin_types=builtin_types)

        assert from_builtins(aware_msg, Ex) == aware
        with pytest.raises(ValidationError):
            from_builtins(naive_msg, Ex)


class TestTime:
    def test_time_wrong_type(self):
        with pytest.raises(ValidationError, match="Expected `time`, got `int`"):
            from_builtins(1, datetime.time)

    @pytest.mark.parametrize("tz", [False, True])
    def test_time_builtin(self, tz):
        t = datetime.time(12, 34, tzinfo=(UTC if tz else None))
        assert from_builtins(t, datetime.time) is t

    @pytest.mark.parametrize("tz", [False, True])
    def test_time_str(self, tz):
        sol = datetime.time(12, 34, tzinfo=(UTC if tz else None))
        msg = "12:34:00" + ("Z" if tz else "")
        res = from_builtins(msg, datetime.time)
        assert res == sol

    def test_time_str_disabled(self):
        with pytest.raises(ValidationError, match="Expected `time`, got `str`"):
            from_builtins("12:34:00Z", datetime.time, builtin_types=(datetime.time,))

    @pytest.mark.parametrize("as_str", [False, True])
    @uses_annotated
    def test_time_constrs(self, as_str):
        class Ex(Struct):
            x: Annotated[datetime.time, Meta(tz=True)]

        builtin_types = None if as_str else (datetime.time,)

        aware = Ex(datetime.time(12, 34, tzinfo=UTC))
        aware_msg = to_builtins(aware, builtin_types=builtin_types)
        naive = Ex(datetime.time(12, 34))
        naive_msg = to_builtins(naive, builtin_types=builtin_types)

        assert from_builtins(aware_msg, Ex) == aware
        with pytest.raises(ValidationError):
            from_builtins(naive_msg, Ex)


class TestDate:
    def test_date_wrong_type(self):
        with pytest.raises(ValidationError, match="Expected `date`, got `int`"):
            from_builtins(1, datetime.date)

    def test_date_builtin(self):
        dt = datetime.date.today()
        assert from_builtins(dt, datetime.date) is dt

    def test_date_str(self):
        sol = datetime.date.today()
        res = from_builtins(sol.isoformat(), datetime.date)
        assert res == sol

    def test_date_str_disabled(self):
        with pytest.raises(ValidationError, match="Expected `date`, got `str`"):
            from_builtins("2022-01-02", datetime.date, builtin_types=(datetime.date,))


class TestUUID:
    def test_uuid_wrong_type(self):
        with pytest.raises(ValidationError, match="Expected `uuid`, got `int`"):
            from_builtins(1, uuid.UUID)

    def test_uuid_builtin(self):
        x = uuid.uuid4()
        assert from_builtins(x, uuid.UUID) is x

    def test_uuid_str(self):
        sol = uuid.uuid4()
        res = from_builtins(str(sol), uuid.UUID)
        assert res == sol

    def test_uuid_str_disabled(self):
        msg = str(uuid.uuid4())
        with pytest.raises(ValidationError, match="Expected `uuid`, got `str`"):
            from_builtins(msg, uuid.UUID, builtin_types=(uuid.UUID,))


class TestDecimal:
    def test_decimal_wrong_type(self):
        with pytest.raises(ValidationError, match="Expected `decimal`, got `float`"):
            from_builtins(1.5, decimal.Decimal)

    def test_decimal_builtin(self):
        x = decimal.Decimal("1.5")
        assert from_builtins(x, decimal.Decimal) is x

    def test_decimal_str(self):
        sol = decimal.Decimal("1.5")
        res = from_builtins("1.5", decimal.Decimal)
        assert res == sol
        assert type(res) is decimal.Decimal

    def test_decimal_str_disabled(self):
        with pytest.raises(ValidationError, match="Expected `decimal`, got `str`"):
            from_builtins("1.5", decimal.Decimal, builtin_types=(decimal.Decimal,))


class TestEnum:
    def test_enum(self):
        class Ex(enum.Enum):
            x = "A"
            y = "B"

        assert from_builtins("A", Ex) is Ex.x
        assert from_builtins("B", Ex) is Ex.y
        with pytest.raises(ValidationError, match="Invalid enum value 'C'"):
            from_builtins("C", Ex)
        with pytest.raises(ValidationError, match="Expected `str`, got `int`"):
            from_builtins(1, Ex)

    def test_int_enum(self):
        class Ex(enum.IntEnum):
            x = 1
            y = 2

        assert from_builtins(1, Ex) is Ex.x
        assert from_builtins(2, Ex) is Ex.y
        with pytest.raises(ValidationError, match="Invalid enum value 3"):
            from_builtins(3, Ex)
        with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
            from_builtins("A", Ex)

    def test_int_enum_int_subclass(self):
        class MyInt(int):
            pass

        class Ex(enum.IntEnum):
            x = 1
            y = 2

        msg = MyInt(1)
        assert from_builtins(msg, Ex) is Ex.x
        assert sys.getrefcount(msg) == 2  # msg + 1
        assert from_builtins(MyInt(2), Ex) is Ex.y


class TestLiteral:
    def test_str_literal(self):
        typ = Literal["A", "B"]
        assert from_builtins("A", typ) == "A"
        assert from_builtins("B", typ) == "B"
        with pytest.raises(ValidationError, match="Invalid enum value 'C'"):
            from_builtins("C", typ)
        with pytest.raises(ValidationError, match="Expected `str`, got `int`"):
            from_builtins(1, typ)

    def test_int_literal(self):
        typ = Literal[1, -2]
        assert from_builtins(1, typ) == 1
        assert from_builtins(-2, typ) == -2
        with pytest.raises(ValidationError, match="Invalid enum value 3"):
            from_builtins(3, typ)
        with pytest.raises(ValidationError, match="Invalid enum value -3"):
            from_builtins(-3, typ)
        with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
            from_builtins("A", typ)


class TestSequences:
    def test_any_sequence(self):
        assert from_builtins((1, 2, 3), Any) == [1, 2, 3]

    @pytest.mark.parametrize("in_type", [list, tuple, set, frozenset])
    @pytest.mark.parametrize("out_type", [list, tuple, set, frozenset])
    def test_empty_sequence(self, in_type, out_type):
        assert from_builtins(in_type(), out_type) == out_type()

    @pytest.mark.parametrize("in_type", [list, tuple, set, frozenset])
    @pytest.mark.parametrize(
        "out_type_annot",
        [(list, List), (tuple, Tuple), (set, Set), (frozenset, FrozenSet)],
    )
    @pytest.mark.parametrize("item_annot", [None, int])
    def test_sequence(self, in_type, out_type_annot, item_annot):
        out_type, out_annot = out_type_annot
        if item_annot is not None:
            if out_annot is Tuple:
                out_annot = out_annot[item_annot, ...]
            else:
                out_annot = out_annot[item_annot]
        res = from_builtins(in_type([1, 2]), out_annot)
        sol = out_type([1, 2])
        assert res == sol
        assert isinstance(res, out_type)

    @pytest.mark.parametrize("in_type", [list, tuple, set, frozenset])
    @pytest.mark.parametrize(
        "out_annot", [List[int], Tuple[int, ...], Set[int], FrozenSet[int]]
    )
    def test_sequence_wrong_item_type(self, in_type, out_annot):
        with pytest.raises(
            ValidationError, match=r"Expected `int`, got `str` - at `\$\[0\]`"
        ):
            assert from_builtins(in_type(["bad"]), out_annot)

    @pytest.mark.parametrize("out_type", [list, tuple, set, frozenset])
    def test_sequence_wrong_type(self, out_type):
        with pytest.raises(ValidationError, match=r"Expected `array`, got `int`"):
            assert from_builtins(1, out_type)

    @pytest.mark.parametrize("kind", ["list", "tuple", "fixtuple", "set"])
    def test_sequence_cyclic_recursion(self, kind):
        depth = 50
        if kind == "list":
            typ = List[int]
            for _ in range(depth):
                typ = List[typ]
        elif kind == "tuple":
            typ = Tuple[int, ...]
            for _ in range(depth):
                typ = Tuple[typ, ...]
        elif kind == "fixtuple":
            typ = Tuple[int]
            for _ in range(depth):
                typ = Tuple[typ]
        elif kind == "set":
            typ = FrozenSet[int]
            for _ in range(depth):
                typ = FrozenSet[typ]

        msg = []
        msg.append(msg)
        with pytest.raises(RecursionError):
            with max_call_depth(5):
                assert from_builtins(msg, typ)

    @pytest.mark.parametrize("out_type", [list, tuple, set, frozenset])
    @uses_annotated
    def test_sequence_constrs(self, out_type):
        class Ex(Struct):
            x: Annotated[out_type, Meta(min_length=2, max_length=4)]

        for n in [2, 4]:
            x = out_type(range(n))
            assert from_builtins({"x": list(range(n))}, Ex).x == x

        for n in [1, 5]:
            x = out_type(range(n))
            with pytest.raises(ValidationError):
                from_builtins({"x": list(range(n))}, Ex)

    def test_fixtuple_any(self):
        typ = Tuple[Any, Any, Any]
        sol = (1, "two", False)
        res = from_builtins([1, "two", False], typ)
        assert res == sol

        with pytest.raises(ValidationError, match="Expected `array`, got `int`"):
            from_builtins(1, typ)

        with pytest.raises(ValidationError, match="Expected `array` of length 3"):
            from_builtins((1, "two"), typ)

    def test_fixtuple_typed(self):
        typ = Tuple[int, str, bool]
        sol = (1, "two", False)
        res = from_builtins([1, "two", False], typ)
        assert res == sol

        with pytest.raises(ValidationError, match="Expected `bool`"):
            from_builtins([1, "two", "three"], typ)

        with pytest.raises(ValidationError, match="Expected `array` of length 3"):
            from_builtins((1, "two"), typ)


class TestNamedTuple:
    def test_namedtuple_no_defaults(self):
        class Example(NamedTuple):
            a: int
            b: int
            c: int

        msg = Example(1, 2, 3)
        res = from_builtins([1, 2, 3], Example)
        assert res == msg

        with pytest.raises(ValidationError, match="length 3, got 1"):
            from_builtins([1], Example)

        with pytest.raises(ValidationError, match="length 3, got 6"):
            from_builtins([1, 2, 3, 4, 5, 6], Example)

    def test_namedtuple_with_defaults(self):
        class Example(NamedTuple):
            a: int
            b: int
            c: int = -3
            d: int = -4
            e: int = -5

        for args in [(1, 2), (1, 2, 3), (1, 2, 3, 4), (1, 2, 3, 4, 5)]:
            msg = Example(*args)
            res = from_builtins(args, Example)
            assert res == msg

        with pytest.raises(ValidationError, match="length 2 to 5, got 1"):
            from_builtins([1], Example)

        with pytest.raises(ValidationError, match="length 2 to 5, got 6"):
            from_builtins([1, 2, 3, 4, 5, 6], Example)

    def test_namedtuple_field_wrong_type(self):
        class Example(NamedTuple):
            a: int
            b: str

        with pytest.raises(
            ValidationError, match=r"Expected `int`, got `str` - at `\$\[0\]`"
        ):
            from_builtins(("bad", 1), Example)

    def test_namedtuple_not_array(self):
        class Example(NamedTuple):
            a: int
            b: str

        with pytest.raises(ValidationError, match="Expected `array`, got `object`"):
            from_builtins({"a": 1, "b": "two"}, Example)

    def test_namedtuple_cyclic_recursion(self):
        source = """
        from __future__ import annotations
        from typing import NamedTuple, Union, Dict

        class Ex(NamedTuple):
            a: int
            b: Union[Ex, None]
        """
        with temp_module(source) as mod:
            msg = [1]
            msg.append(msg)
            with pytest.raises(RecursionError):
                assert from_builtins(msg, mod.Ex)


class TestDict:
    def test_any_dict(self):
        assert from_builtins({"one": 1, 2: "two"}, Any) == {"one": 1, 2: "two"}

    def test_empty_dict(self):
        assert from_builtins({}, dict) == {}
        assert from_builtins({}, Dict[int, int]) == {}

    def test_typed_dict(self):
        res = from_builtins({"x": 1, "y": 2}, Dict[str, float])
        assert res == {"x": 1.0, "y": 2.0}
        assert all(type(v) is float for v in res.values())

        with pytest.raises(
            ValidationError, match=r"Expected `str`, got `int` - at `\$\[\.\.\.\]`"
        ):
            from_builtins({"x": 1}, Dict[str, str])

        with pytest.raises(
            ValidationError, match=r"Expected `int`, got `str` - at `key` in `\$`"
        ):
            from_builtins({"x": 1}, Dict[int, str])

    def test_dict_wrong_type(self):
        with pytest.raises(ValidationError, match=r"Expected `object`, got `int`"):
            assert from_builtins(1, dict)

    def test_str_formatted_keys(self):
        msg = {uuid.uuid4(): 1, uuid.uuid4(): 2}
        res = from_builtins(to_builtins(msg), Dict[uuid.UUID, int])
        assert res == msg

    @pytest.mark.parametrize("key_type", ["int", "enum", "literal"])
    def test_int_keys(self, key_type):
        msg = {1: "A", 2: "B"}
        if key_type == "enum":
            Key = enum.IntEnum("Key", ["one", "two"])
            sol = {Key.one: "A", Key.two: "B"}
        elif key_type == "literal":
            Key = Literal[1, 2]
            sol = msg
        else:
            Key = int
            sol = msg

        res = from_builtins(msg, Dict[Key, str])
        assert res == sol

        res = from_builtins(msg, Dict[Key, str], str_keys=True)
        assert res == sol

        str_msg = to_builtins(msg, str_keys=True)
        res = from_builtins(str_msg, Dict[Key, str], str_keys=True)
        assert res == sol

        with pytest.raises(
            ValidationError, match=r"Expected `int`, got `str` - at `key` in `\$`"
        ):
            from_builtins(str_msg, Dict[Key, str])

    def test_non_str_keys(self):
        from_builtins({1.5: 1}, Dict[float, int]) == {1.5: 1}

        with pytest.raises(ValidationError):
            from_builtins({"x": 1}, Dict[Tuple[int, int], int], str_keys=True)

    def test_dict_cyclic_recursion(self):
        depth = 50
        typ = Dict[str, int]
        for _ in range(depth):
            typ = Dict[str, typ]
        msg = {}
        msg["x"] = msg
        with pytest.raises(RecursionError):
            with max_call_depth(5):
                assert from_builtins(msg, typ)

    @uses_annotated
    def test_dict_constrs(self):
        class Ex(Struct):
            x: Annotated[dict, Meta(min_length=2, max_length=4)]

        for n in [2, 4]:
            x = {str(i): i for i in range(n)}
            assert from_builtins({"x": x}, Ex).x == x

        for n in [1, 5]:
            x = {str(i): i for i in range(n)}
            with pytest.raises(ValidationError):
                from_builtins({"x": x}, Ex)


@pytest.mark.skipif(TypedDict is None, reason="TypedDict not available")
class TestTypedDict:
    def test_typeddict_total_true(self):
        class Ex(TypedDict):
            a: int
            b: str

        x = {"a": 1, "b": "two"}
        assert from_builtins(x, Ex) == x

        x2 = {"a": 1, "b": "two", "c": "extra"}
        assert from_builtins(x2, Ex) == x

        with pytest.raises(ValidationError) as rec:
            from_builtins({"b": "two"}, Ex)
        assert "Object missing required field `a`" == str(rec.value)

        with pytest.raises(ValidationError) as rec:
            from_builtins({"a": 1, "b": 2}, Ex)
        assert "Expected `str`, got `int` - at `$.b`" == str(rec.value)

        with pytest.raises(ValidationError) as rec:
            from_builtins({"a": 1, 1: 2}, Ex)
        assert "Expected `str` - at `key` in `$`" == str(rec.value)

    def test_typeddict_total_false(self):
        class Ex(TypedDict, total=False):
            a: int
            b: str

        x = {"a": 1, "b": "two"}
        assert from_builtins(x, Ex) == x

        x2 = {"a": 1, "b": "two", "c": "extra"}
        assert from_builtins(x2, Ex) == x

        x3 = {"b": "two"}
        assert from_builtins(x3, Ex) == x3

        x4 = {}
        assert from_builtins(x4, Ex) == x4

    def test_typeddict_total_partially_optional(self):
        class Base(TypedDict):
            a: int
            b: str

        class Ex(Base, total=False):
            c: str

        if not hasattr(Ex, "__required_keys__"):
            # This should be Python 3.8, builtin typing only
            pytest.skip("partially optional TypedDict not supported")

        x = {"a": 1, "b": "two", "c": "extra"}
        assert from_builtins(x, Ex) == x

        x2 = {"a": 1, "b": "two"}
        assert from_builtins(x2, Ex) == x2

        with pytest.raises(ValidationError) as rec:
            from_builtins({"b": "two"}, Ex)
        assert "Object missing required field `a`" == str(rec.value)


class TestDataclass:
    @pytest.mark.parametrize("slots", [False, True])
    @pytest.mark.parametrize("attributes", [False, True])
    def test_dataclass(self, slots, attributes):
        Msg = AttrDict if attributes else dict

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

        sol = Example(1, 2, 3)
        msg = Msg(a=1, b=2, c=3)
        res = from_builtins(msg, Example, attributes=attributes)
        assert res == sol

        # Extra fields ignored
        res = from_builtins(
            Msg({"x": -1, "a": 1, "y": -2, "b": 2, "z": -3, "c": 3, "": -4}),
            Example,
            attributes=attributes,
        )
        assert res == sol

        # Missing fields error
        with pytest.raises(ValidationError, match="missing required field `b`"):
            from_builtins(Msg(a=1), Example, attributes=attributes)

        # Incorrect field types error
        with pytest.raises(
            ValidationError, match=r"Expected `int`, got `str` - at `\$.a`"
        ):
            from_builtins(Msg(a="bad"), Example, attributes=attributes)

    def test_dict_to_dataclass_errors(self):
        @dataclass
        class Example:
            a: int

        with pytest.raises(ValidationError, match=r"Expected `str` - at `key` in `\$`"):
            from_builtins({"a": 1, 1: 2}, Example)

    def test_attributes_option_disables_attribute_coercion(self):
        class Bad:
            def __init__(self):
                self.x = 1

        msg = Bad()

        @dataclass
        class Ex:
            x: int

        with pytest.raises(ValidationError, match="Expected `object`, got `Bad`"):
            from_builtins(msg, Ex)

        assert from_builtins(msg, Ex, attributes=True) == Ex(1)

    @pytest.mark.parametrize("slots", [False, True])
    @pytest.mark.parametrize("attributes", [False, True])
    def test_dataclass_defaults(self, slots, attributes):
        Msg = AttrDict if attributes else dict

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

        for args in [(1, 2), (1, 2, 3), (1, 2, 3, 4), (1, 2, 3, 4, 5)]:
            sol = Example(*args)
            msg = Msg(dict(zip("abcde", args)))
            res = from_builtins(msg, Example, attributes=attributes)
            assert res == sol

        # Missing fields error
        with pytest.raises(ValidationError, match="missing required field `a`"):
            from_builtins(Msg(c=1, d=2, e=3), Example, attributes=attributes)

    @pytest.mark.parametrize("attributes", [False, True])
    def test_dataclass_default_factory_errors(self, attributes):
        def bad():
            raise ValueError("Oh no!")

        @dataclass
        class Example:
            a: int = field(default_factory=bad)

        msg = AttrDict() if attributes else {}

        with pytest.raises(ValueError, match="Oh no!"):
            from_builtins(msg, Example, attributes=attributes)

    @pytest.mark.parametrize("attributes", [False, True])
    def test_dataclass_post_init(self, attributes):
        called = False

        @dataclass
        class Example:
            a: int

            def __post_init__(self):
                nonlocal called
                called = True

        msg = AttrDict(a=1) if attributes else {"a": 1}
        res = from_builtins(msg, Example, attributes=attributes)
        assert res.a == 1
        assert called

    @pytest.mark.parametrize("attributes", [False, True])
    def test_dataclass_post_init_errors(self, attributes):
        @dataclass
        class Example:
            a: int

            def __post_init__(self):
                raise ValueError("Oh no!")

        msg = AttrDict(a=1) if attributes else {"a": 1}

        with pytest.raises(ValueError, match="Oh no!"):
            from_builtins(msg, Example, attributes=attributes)

    @pytest.mark.parametrize("attributes", [False, True])
    def test_dataclass_not_object(self, attributes):
        @dataclass
        class Example:
            a: int
            b: int

        with pytest.raises(ValidationError, match="Expected `object`, got `array`"):
            from_builtins([], Example, attributes=attributes)


@pytest.mark.skipif(attrs is None, reason="attrs is not installed")
class TestAttrs:
    @pytest.mark.parametrize("slots", [False, True])
    @pytest.mark.parametrize("attributes", [False, True])
    def test_attrs(self, slots, attributes):
        Msg = AttrDict if attributes else dict

        @attrs.define(slots=slots)
        class Example:
            a: int
            b: int
            c: int

        sol = Example(1, 2, 3)
        msg = Msg(a=1, b=2, c=3)
        res = from_builtins(msg, Example, attributes=attributes)
        assert res == sol

        # Extra fields ignored
        res = from_builtins(
            Msg({"x": -1, "a": 1, "y": -2, "b": 2, "z": -3, "c": 3, "": -4}),
            Example,
            attributes=attributes,
        )
        assert res == sol

        # Missing fields error
        with pytest.raises(ValidationError, match="missing required field `b`"):
            from_builtins(Msg(a=1), Example, attributes=attributes)

        # Incorrect field types error
        with pytest.raises(
            ValidationError, match=r"Expected `int`, got `str` - at `\$.a`"
        ):
            from_builtins({"a": "bad"}, Example, attributes=attributes)

    def test_dict_to_attrs_errors(self):
        @attrs.define
        class Example:
            a: int

        with pytest.raises(ValidationError, match=r"Expected `str` - at `key` in `\$`"):
            from_builtins({"a": 1, 1: 2}, Example)

    def test_attributes_option_disables_attribute_coercion(self):
        class Bad:
            def __init__(self):
                self.x = 1

        msg = Bad()

        @attrs.define
        class Ex:
            x: int

        with pytest.raises(ValidationError, match="Expected `object`, got `Bad`"):
            from_builtins(msg, Ex)

        assert from_builtins(msg, Ex, attributes=True) == Ex(1)

    @pytest.mark.parametrize("slots", [False, True])
    @pytest.mark.parametrize("attributes", [False, True])
    def test_attrs_defaults(self, slots, attributes):
        Msg = AttrDict if attributes else dict

        @attrs.define(slots=slots)
        class Example:
            a: int
            b: int
            c: int = -3
            d: int = -4
            e: int = attrs.field(factory=lambda: -1000)

        for args in [(1, 2), (1, 2, 3), (1, 2, 3, 4), (1, 2, 3, 4, 5)]:
            sol = Example(*args)
            msg = Msg(dict(zip("abcde", args)))
            res = from_builtins(msg, Example, attributes=attributes)
            assert res == sol

        # Missing fields error
        with pytest.raises(ValidationError, match="missing required field `a`"):
            from_builtins(Msg(c=1, d=2, e=3), Example, attributes=attributes)

    @pytest.mark.parametrize("attributes", [False, True])
    def test_attrs_frozen(self, attributes):
        Msg = AttrDict if attributes else dict

        @attrs.define(frozen=True)
        class Example:
            x: int
            y: int

        sol = Example(1, 2)
        msg = Msg(x=1, y=2)
        res = from_builtins(msg, Example, attributes=attributes)
        assert res == sol

    @pytest.mark.parametrize("attributes", [False, True])
    def test_attrs_pre_init(self, attributes):
        Msg = AttrDict if attributes else dict

        called = False

        @attrs.define
        class Example:
            a: int

            def __attrs_pre_init__(self):
                nonlocal called
                called = True

        res = from_builtins(Msg(a=1), Example, attributes=attributes)
        assert res.a == 1
        assert called

    @pytest.mark.parametrize("attributes", [False, True])
    def test_attrs_pre_init_errors(self, attributes):
        Msg = AttrDict if attributes else dict

        @attrs.define
        class Example:
            a: int

            def __attrs_pre_init__(self):
                raise ValueError("Oh no!")

        with pytest.raises(ValueError, match="Oh no!"):
            from_builtins(Msg(a=1), Example, attributes=attributes)

    @pytest.mark.parametrize("attributes", [False, True])
    def test_attrs_post_init(self, attributes):
        Msg = AttrDict if attributes else dict

        called = False

        @attrs.define
        class Example:
            a: int

            def __attrs_post_init__(self):
                nonlocal called
                called = True

        res = from_builtins(Msg(a=1), Example, attributes=attributes)
        assert res.a == 1
        assert called

    @pytest.mark.parametrize("attributes", [False, True])
    def test_attrs_post_init_errors(self, attributes):
        Msg = AttrDict if attributes else dict

        @attrs.define
        class Example:
            a: int

            def __attrs_post_init__(self):
                raise ValueError("Oh no!")

        with pytest.raises(ValueError, match="Oh no!"):
            from_builtins(Msg(a=1), Example, attributes=attributes)


class TestStruct:
    class Account(Struct):
        first: str
        last: str
        age: int
        verified: bool = False

    @pytest.mark.parametrize("attributes", [False, True])
    def test_struct(self, attributes):
        Msg = AttrDict if attributes else dict

        msg = Msg(first="alice", last="munro", age=91, verified=True)
        sol = self.Account("alice", "munro", 91, True)
        res = from_builtins(msg, self.Account, attributes=attributes)
        assert res == sol

        with pytest.raises(ValidationError, match="Expected `object`, got `array`"):
            from_builtins([], self.Account, attributes=attributes)

        with pytest.raises(
            ValidationError, match=r"Expected `str`, got `int` - at `\$.last`"
        ):
            from_builtins(
                Msg(first="alice", last=1), self.Account, attributes=attributes
            )

        with pytest.raises(
            ValidationError, match="Object missing required field `age`"
        ):
            from_builtins(
                Msg(first="alice", last="munro"), self.Account, attributes=attributes
            )

    def test_dict_to_struct_errors(self):
        with pytest.raises(ValidationError, match=r"Expected `str` - at `key` in `\$`"):
            from_builtins({"age": 1, 1: 2}, self.Account)

    def test_attributes_option_disables_attribute_coercion(self):
        class Bad:
            def __init__(self):
                self.x = 1

        msg = Bad()

        class Ex(Struct):
            x: int

        with pytest.raises(ValidationError, match="Expected `object`, got `Bad`"):
            from_builtins(msg, Ex)

        assert from_builtins(msg, Ex, attributes=True) == Ex(1)

    @pytest.mark.parametrize("forbid_unknown_fields", [False, True])
    @pytest.mark.parametrize("attributes", [False, True])
    def test_struct_extra_fields(self, forbid_unknown_fields, attributes):
        Msg = AttrDict if attributes else dict

        class Ex(Struct, forbid_unknown_fields=forbid_unknown_fields):
            a: int
            b: int

        msg = Msg(x=1, a=2, y=3, b=4, z=5)
        if forbid_unknown_fields and not attributes:
            with pytest.raises(ValidationError, match="unknown field `x`"):
                from_builtins(msg, Ex, attributes=attributes)
        else:
            res = from_builtins(msg, Ex, attributes=attributes)
            assert res == Ex(2, 4)

    @pytest.mark.parametrize("attributes", [False, True])
    def test_struct_defaults_missing_fields(self, attributes):
        Msg = AttrDict if attributes else dict

        msg = Msg(first="alice", last="munro", age=91)
        res = from_builtins(msg, self.Account, attributes=attributes)
        assert res == self.Account("alice", "munro", 91)

    @pytest.mark.parametrize(
        "array_like, attributes", [(False, False), (True, False), (False, True)]
    )
    def test_struct_gc_maybe_untracked_on_decode(self, array_like, attributes):
        if attributes:
            Msg = AttrDict
        elif array_like:
            Msg = KWList
        else:
            Msg = dict

        class Test(Struct, array_like=array_like):
            x: Any
            y: Any
            z: Tuple = ()

        ts = [
            Msg(x=1, y=2),
            Msg(x=3, y="hello"),
            Msg(x=[], y=[]),
            Msg(x={}, y={}),
            Msg(x=None, y=None, z=()),
        ]
        a, b, c, d, e = from_builtins(ts, List[Test], attributes=attributes)
        assert not gc.is_tracked(a)
        assert not gc.is_tracked(b)
        assert gc.is_tracked(c)
        assert gc.is_tracked(d)
        assert not gc.is_tracked(e)

    @pytest.mark.parametrize(
        "array_like, attributes", [(False, False), (True, False), (False, True)]
    )
    def test_struct_gc_false_always_untracked_on_decode(self, array_like, attributes):
        if attributes:
            Msg = AttrDict
        elif array_like:
            Msg = KWList
        else:
            Msg = dict

        class Test(Struct, array_like=array_like, gc=False):
            x: Any
            y: Any

        ts = [
            Msg(x=1, y=2),
            Msg(x=[], y=[]),
            Msg(x={}, y={}),
        ]
        for obj in from_builtins(ts, List[Test], attributes=attributes):
            assert not gc.is_tracked(obj)

    @pytest.mark.parametrize("tag", ["Test", 123, -123])
    @pytest.mark.parametrize("attributes", [False, True])
    def test_tagged_struct(self, tag, attributes):
        Msg = AttrDict if attributes else dict

        class Test(Struct, tag=tag):
            a: int
            b: int

        # Test with and without tag
        for msg in [
            Msg(a=1, b=2),
            Msg(type=tag, a=1, b=2),
            Msg(a=1, type=tag, b=2),
        ]:
            res = from_builtins(msg, Test, attributes=attributes)
            assert res == Test(1, 2)

        # Tag incorrect type
        with pytest.raises(ValidationError) as rec:
            from_builtins(Msg(type=123.456), Test, attributes=attributes)
        assert f"Expected `{type(tag).__name__}`" in str(rec.value)
        assert "`$.type`" in str(rec.value)

        # Tag incorrect value
        bad = -3 if isinstance(tag, int) else "bad"
        with pytest.raises(ValidationError) as rec:
            from_builtins(Msg(type=bad), Test, attributes=attributes)
        assert f"Invalid value {bad!r}" in str(rec.value)
        assert "`$.type`" in str(rec.value)

    @pytest.mark.parametrize("tag_val", [2**64 - 1, 2**64, -(2**63) - 1])
    @pytest.mark.parametrize("attributes", [False, True])
    def test_tagged_struct_int_tag_not_int64_always_invalid(self, tag_val, attributes):
        """Tag values that don't fit in an int64 are currently unsupported, but
        we still want to raise a good error message."""

        Msg = AttrDict if attributes else dict

        class Test(Struct, tag=123):
            pass

        with pytest.raises(ValidationError) as rec:
            from_builtins(Msg(type=tag_val), Test, attributes=attributes)

        assert f"Invalid value {tag_val}" in str(rec.value)
        assert "`$.type`" in str(rec.value)

    @pytest.mark.parametrize("tag", ["Test", 123, -123])
    @pytest.mark.parametrize("attributes", [False, True])
    def test_tagged_empty_struct(self, tag, attributes):
        Msg = AttrDict if attributes else dict

        class Test(Struct, tag=tag):
            pass

        # Tag missing
        res = from_builtins(Msg(), Test, attributes=attributes)
        assert res == Test()

        # Tag present
        res = from_builtins(Msg(type=tag), Test, attributes=attributes)
        assert res == Test()


class TestStructArray:
    class Account(Struct, array_like=True):
        first: str
        last: str
        age: int
        verified: bool = False

    def test_struct_array_like(self):
        msg = self.Account("alice", "munro", 91, True)
        res = roundtrip(msg, self.Account)
        assert res == msg

        with pytest.raises(ValidationError, match="Expected `array`, got `int`"):
            roundtrip(1, self.Account)

        # Wrong field type
        with pytest.raises(
            ValidationError, match=r"Expected `int`, got `str` - at `\$\[2\]`"
        ):
            roundtrip(("alice", "munro", "bad"), self.Account)

        # Missing fields
        with pytest.raises(
            ValidationError,
            match="Expected `array` of at least length 3, got 2",
        ):
            roundtrip(("alice", "munro"), self.Account)

        with pytest.raises(
            ValidationError,
            match="Expected `array` of at least length 3, got 0",
        ):
            roundtrip((), self.Account)

    @pytest.mark.parametrize("forbid_unknown_fields", [False, True])
    def test_struct_extra_fields(self, forbid_unknown_fields):
        class Ex(Struct, array_like=True, forbid_unknown_fields=forbid_unknown_fields):
            a: int
            b: int

        msg = (1, 2, 3, 4)
        if forbid_unknown_fields:
            with pytest.raises(
                ValidationError, match="Expected `array` of at most length 2, got 4"
            ):
                roundtrip(msg, Ex)
        else:
            res = roundtrip(msg, Ex)
            assert res == Ex(1, 2)

    def test_struct_defaults_missing_fields(self):
        res = roundtrip(("alice", "munro", 91), self.Account)
        assert res == self.Account("alice", "munro", 91)

    @pytest.mark.parametrize("tag", ["Test", -123, 123])
    def test_tagged_struct(self, tag):
        class Test(Struct, tag=tag, array_like=True):
            a: int
            b: int
            c: int = 0

        # Decode with tag
        res = roundtrip((tag, 1, 2), Test)
        assert res == Test(1, 2)
        res = roundtrip((tag, 1, 2, 3), Test)
        assert res == Test(1, 2, 3)

        # Trailing fields ignored
        res = roundtrip((tag, 1, 2, 3, 4), Test)
        assert res == Test(1, 2, 3)

        # Missing required field errors
        with pytest.raises(ValidationError) as rec:
            roundtrip((tag, 1), Test)
        assert "Expected `array` of at least length 3, got 2" in str(rec.value)

        # Tag missing
        with pytest.raises(ValidationError) as rec:
            roundtrip((), Test)
        assert "Expected `array` of at least length 3, got 0" in str(rec.value)

        # Tag incorrect type
        with pytest.raises(ValidationError) as rec:
            roundtrip((123.456, 2, 3), Test)
        assert f"Expected `{type(tag).__name__}`" in str(rec.value)
        assert "`$[0]`" in str(rec.value)

        # Tag incorrect value
        bad = -3 if isinstance(tag, int) else "bad"
        with pytest.raises(ValidationError) as rec:
            roundtrip((bad, 1, 2), Test)
        assert f"Invalid value {bad!r}" in str(rec.value)
        assert "`$[0]`" in str(rec.value)

        # Field incorrect type correct index
        with pytest.raises(ValidationError) as rec:
            roundtrip((tag, "a", 2), Test)
        assert "Expected `int`, got `str`" in str(rec.value)
        assert "`$[1]`" in str(rec.value)

    @pytest.mark.parametrize("tag", ["Test", 123, -123])
    def test_tagged_empty_struct(self, tag):
        class Test(Struct, tag=tag, array_like=True):
            pass

        # Decode with tag
        res = roundtrip((tag, 1, 2), Test)
        assert res == Test()

        # Tag missing
        with pytest.raises(ValidationError) as rec:
            roundtrip((), Test)
        assert "Expected `array` of at least length 1, got 0" in str(rec.value)


class TestStructUnion:
    @pytest.mark.parametrize(
        "tag1, tag2, unknown",
        [
            ("Test1", "Test2", "Test3"),
            (0, 1, 2),
            (123, -123, 0),
        ],
    )
    @pytest.mark.parametrize("attributes", [False, True])
    def test_struct_union(self, tag1, tag2, unknown, attributes):
        def decode(msg):
            if attributes:
                msg = AttrDict(msg)
            return from_builtins(msg, Union[Test1, Test2], attributes=attributes)

        class Test1(Struct, tag=tag1):
            a: int
            b: int
            c: int = 0

        class Test2(Struct, tag=tag2):
            x: int
            y: int

        # Tag can be in any position
        assert decode({"type": tag1, "a": 1, "b": 2}) == Test1(1, 2)
        assert decode({"a": 1, "type": tag1, "b": 2}) == Test1(1, 2)
        assert decode({"x": 1, "y": 2, "type": tag2}) == Test2(1, 2)

        # Optional fields still work
        assert decode({"type": tag1, "a": 1, "b": 2, "c": 3}) == Test1(1, 2, 3)
        assert decode({"a": 1, "b": 2, "c": 3, "type": tag1}) == Test1(1, 2, 3)

        # Extra fields still ignored
        assert decode({"a": 1, "b": 2, "d": 4, "type": tag1}) == Test1(1, 2)

        # Tag missing
        with pytest.raises(ValidationError) as rec:
            decode({"a": 1, "b": 2})
        assert "missing required field `type`" in str(rec.value)

        # Tag wrong type
        with pytest.raises(ValidationError) as rec:
            decode({"type": 123.456, "a": 1, "b": 2})
        assert f"Expected `{type(tag1).__name__}`" in str(rec.value)
        assert "`$.type`" in str(rec.value)

        # Tag unknown
        with pytest.raises(ValidationError) as rec:
            decode({"type": unknown, "a": 1, "b": 2})
        assert f"Invalid value {unknown!r} - at `$.type`" == str(rec.value)

    @pytest.mark.parametrize(
        "tag1, tag2, tag3, unknown",
        [
            ("Test1", "Test2", "Test3", "Test4"),
            (0, 1, 2, 3),
            (123, -123, 0, -1),
        ],
    )
    def test_struct_array_union(self, tag1, tag2, tag3, unknown):
        class Test1(Struct, tag=tag1, array_like=True):
            a: int
            b: int
            c: int = 0

        class Test2(Struct, tag=tag2, array_like=True):
            x: int
            y: int

        class Test3(Struct, tag=tag3, array_like=True):
            pass

        typ = Union[Test1, Test2, Test3]

        # Decoding works
        assert roundtrip([tag1, 1, 2], typ) == Test1(1, 2)
        assert roundtrip([tag2, 3, 4], typ) == Test2(3, 4)
        assert roundtrip([tag3], typ) == Test3()

        # Optional & Extra fields still respected
        assert roundtrip([tag1, 1, 2, 3], typ) == Test1(1, 2, 3)
        assert roundtrip([tag1, 1, 2, 3, 4], typ) == Test1(1, 2, 3)

        # Missing required field
        with pytest.raises(ValidationError) as rec:
            roundtrip([tag1, 1], typ)
        assert "Expected `array` of at least length 3, got 2" in str(rec.value)

        # Type error has correct field index
        with pytest.raises(ValidationError) as rec:
            roundtrip([tag1, 1, "bad", 2], typ)
        assert "Expected `int`, got `str` - at `$[2]`" == str(rec.value)

        # Tag missing
        with pytest.raises(ValidationError) as rec:
            roundtrip([], typ)
        assert "Expected `array` of at least length 1, got 0" == str(rec.value)

        # Tag wrong type
        with pytest.raises(ValidationError) as rec:
            roundtrip([123.456, 2, 3, 4], typ)
        assert f"Expected `{type(tag1).__name__}`" in str(rec.value)
        assert "`$[0]`" in str(rec.value)

        # Tag unknown
        with pytest.raises(ValidationError) as rec:
            roundtrip([unknown, 1, 2, 3], typ)
        assert f"Invalid value {unknown!r} - at `$[0]`" == str(rec.value)


class TestGenericStruct:
    @pytest.mark.parametrize(
        "array_like, attributes", [(False, False), (True, False), (False, True)]
    )
    def test_generic_struct(self, array_like, attributes):
        if attributes:
            Msg = AttrDict
        elif array_like:
            Msg = KWList
        else:
            Msg = dict

        class Ex(Struct, Generic[T], array_like=array_like):
            x: T
            y: List[T]

        sol = Ex(1, [1, 2])
        msg = Msg(x=1, y=[1, 2])

        res = from_builtins(msg, Ex, attributes=attributes)
        assert res == sol

        res = from_builtins(msg, Ex[int], attributes=attributes)
        assert res == sol

        res = from_builtins(msg, Ex[Union[int, str]], attributes=attributes)
        assert res == sol

        res = from_builtins(msg, Ex[float], attributes=attributes)
        assert type(res.x) is float

        with pytest.raises(ValidationError, match="Expected `str`, got `int`"):
            from_builtins(msg, Ex[str], attributes=attributes)

    @pytest.mark.parametrize(
        "array_like, attributes", [(False, False), (True, False), (False, True)]
    )
    def test_generic_struct_union(self, array_like, attributes):
        if attributes:
            Msg = AttrDict
        elif array_like:
            Msg = KWList
        else:
            Msg = dict

        class Test1(Struct, Generic[T], tag=True, array_like=array_like):
            a: Union[T, None]
            b: int

        class Test2(Struct, Generic[T], tag=True, array_like=array_like):
            x: T
            y: int

        typ = Union[Test1[T], Test2[T]]

        msg1 = Test1(1, 2)
        s1 = Msg(type="Test1", a=1, b=2)
        msg2 = Test2("three", 4)
        s2 = Msg(type="Test2", x="three", y=4)
        msg3 = Test1(None, 4)
        s3 = Msg(type="Test1", a=None, b=4)

        assert from_builtins(s1, typ, attributes=attributes) == msg1
        assert from_builtins(s2, typ, attributes=attributes) == msg2
        assert from_builtins(s3, typ, attributes=attributes) == msg3

        assert from_builtins(s1, typ[int], attributes=attributes) == msg1
        assert from_builtins(s3, typ[int], attributes=attributes) == msg3
        assert from_builtins(s2, typ[str], attributes=attributes) == msg2
        assert from_builtins(s3, typ[str], attributes=attributes) == msg3

        with pytest.raises(ValidationError) as rec:
            from_builtins(s1, typ[str], attributes=attributes)
        assert "Expected `str | null`, got `int`" in str(rec.value)
        loc = "$[1]" if array_like else "$.a"
        assert loc in str(rec.value)

        with pytest.raises(ValidationError) as rec:
            from_builtins(s2, typ[int], attributes=attributes)
        assert "Expected `int`, got `str`" in str(rec.value)
        loc = "$[1]" if array_like else "$.x"
        assert loc in str(rec.value)


class TestStrValues:
    def test_str_values_none(self):
        for x in ["null", "Null", "nUll", "nuLl", "nulL"]:
            assert from_builtins(x, None, str_values=True) is None

        for x in ["xull", "nxll", "nuxl", "nulx"]:
            with pytest.raises(ValidationError, match="Expected `null`, got `str`"):
                from_builtins(x, None, str_values=True)

    def test_str_values_bool_true(self):
        for x in ["1", "true", "True", "tRue", "trUe", "truE"]:
            assert from_builtins(x, bool, str_values=True) is True

    def test_str_values_bool_false(self):
        for x in ["0", "false", "False", "fAlse", "faLse", "falSe", "falsE"]:
            assert from_builtins(x, bool, str_values=True) is False

    def test_str_values_bool_true_invalid(self):
        for x in ["x", "xx", "xrue", "txue", "trxe", "trux"]:
            with pytest.raises(ValidationError, match="Expected `bool`, got `str`"):
                assert from_builtins(x, bool, str_values=True)

    def test_str_values_bool_false_invalid(self):
        for x in ["x", "xx", "xalse", "fxlse", "faxse", "falxe", "falsx"]:
            with pytest.raises(ValidationError, match="Expected `bool`, got `str`"):
                assert from_builtins(x, bool, str_values=True)

    def test_str_values_int(self):
        for x in ["1", "-1", "123456"]:
            assert from_builtins(x, int, str_values=True) == int(x)

        for x in ["a", "1a", "1.0", "1.."]:
            with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
                from_builtins(x, int, str_values=True)

    @uses_annotated
    def test_str_values_int_constr(self):
        typ = Annotated[int, Meta(ge=0)]
        assert from_builtins("1", typ, str_values=True) == 1

        with pytest.raises(ValidationError):
            from_builtins("-1", typ, str_values=True)

    def test_str_values_int_enum(self):
        class Ex(enum.IntEnum):
            x = 1
            y = -2

        assert from_builtins("1", Ex, str_values=True) is Ex.x
        assert from_builtins("-2", Ex, str_values=True) is Ex.y
        with pytest.raises(ValidationError, match="Invalid enum value 3"):
            from_builtins("3", Ex, str_values=True)
        with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
            from_builtins("A", Ex, str_values=True)

    def test_str_values_int_literal(self):
        typ = Literal[1, -2]
        assert from_builtins("1", typ, str_values=True) == 1
        assert from_builtins("-2", typ, str_values=True) == -2
        with pytest.raises(ValidationError, match="Invalid enum value 3"):
            from_builtins("3", typ, str_values=True)
        with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
            from_builtins("A", typ, str_values=True)

    def test_str_values_float(self):
        for x in ["1", "-1", "123456", "1.5", "-1.5", "inf"]:
            assert from_builtins(x, float, str_values=True) == float(x)

        for x in ["a", "1a", "1.0.0", "1.."]:
            with pytest.raises(ValidationError, match="Expected `float`, got `str`"):
                from_builtins(x, float, str_values=True)

    @uses_annotated
    def test_str_values_float_constr(self):
        assert (
            from_builtins("1.5", Annotated[float, Meta(ge=0)], str_values=True) == 1.5
        )

        with pytest.raises(ValidationError):
            from_builtins("-1.0", Annotated[float, Meta(ge=0)], str_values=True)

    def test_str_values_str(self):
        for x in ["1", "1.5", "false", "null"]:
            assert from_builtins(x, str, str_values=True) == x

    @uses_annotated
    def test_str_values_str_constr(self):
        typ = Annotated[str, Meta(max_length=10)]
        assert from_builtins("xxx", typ, str_values=True) == "xxx"

        with pytest.raises(ValidationError):
            from_builtins("x" * 20, typ, str_values=True)

    @pytest.mark.parametrize(
        "msg, sol",
        [
            ("1", 1),
            ("0", 0),
            ("-1", -1),
            ("12.5", 12.5),
            ("inf", float("inf")),
            ("true", True),
            ("false", False),
            ("null", None),
            ("1a", "1a"),
            ("falsx", "falsx"),
            ("nulx", "nulx"),
        ],
    )
    def test_str_values_union_valid(self, msg, sol):
        typ = Union[int, float, bool, None, str]
        assert_eq(from_builtins(msg, typ, str_values=True), sol)

    @pytest.mark.parametrize("msg", ["1a", "1.5a", "falsx", "trux", "nulx"])
    def test_str_values_union_invalid(self, msg):
        typ = Union[int, float, bool, None]
        with pytest.raises(
            ValidationError, match="Expected `int | float | bool | null`"
        ):
            from_builtins(msg, typ, str_values=True)

    @pytest.mark.parametrize(
        "msg, err",
        [
            ("-1", "`int` >= 0"),
            ("184467440737095516100", "out of range"),
            ("18446744073709551617", "out of range"),
            ("-9223372036854775809", "out of range"),
            ("100.5", "`float` <= 100.0"),
            ("x" * 11, "length <= 10"),
        ],
    )
    @uses_annotated
    def test_str_values_union_invalid_constr(self, msg, err):
        """Ensure that values that parse properly but don't meet the specified
        constraints error with a specific constraint error"""
        typ = Union[
            Annotated[int, Meta(ge=0)],
            Annotated[float, Meta(le=100)],
            Annotated[str, Meta(max_length=10)],
        ]
        with pytest.raises(ValidationError, match=err):
            from_builtins(msg, typ, str_values=True)

    def test_str_values_union_extended(self):
        typ = Union[int, float, bool, None, datetime.datetime]
        dt = datetime.datetime.now()
        assert_eq(from_builtins("1", typ, str_values=True), 1)
        assert_eq(from_builtins("1.5", typ, str_values=True), 1.5)
        assert_eq(from_builtins("false", typ, str_values=True), False)
        assert_eq(from_builtins("null", typ, str_values=True), None)
        assert_eq(from_builtins(dt.isoformat(), typ, str_values=True), dt)


class TestCustom:
    def test_custom(self):
        def dec_hook(typ, x):
            assert typ is complex
            return complex(*x)

        msg = {"x": (1, 2)}
        sol = {"x": complex(1, 2)}
        res = from_builtins(msg, Dict[str, complex], dec_hook=dec_hook)
        assert res == sol

    def test_custom_no_dec_hook(self):
        with pytest.raises(ValidationError, match="Expected `complex`, got `str`"):
            from_builtins({"x": "oh no"}, Dict[str, complex])

    def test_custom_dec_hook_errors(self):
        def dec_hook(typ, x):
            raise TypeError("Oops!")

        with pytest.raises(TypeError, match="Oops!"):
            from_builtins({"x": (1, 2)}, Dict[str, complex], dec_hook=dec_hook)
