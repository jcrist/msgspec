import datetime
import enum
import uuid
from base64 import b64encode
from typing import Any, Literal, List, Tuple, Set, FrozenSet

import pytest

from msgspec import to_builtins, from_builtins, ValidationError, Meta, Struct

try:
    from typing import Annotated
except ImportError:
    try:
        from typing_extensions import Annotated
    except ImportError:
        Annotated = None


uses_annotated = pytest.mark.skipif(Annotated is None, reason="Annotated not available")

UTC = datetime.timezone.utc


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
        with pytest.raises(ValidationError, match="Expected `bool`, got `int`"):
            from_builtins(1, bool)


class TestInt:
    def test_int(self):
        assert from_builtins(1, Any) == 1
        assert from_builtins(1, int) == 1
        with pytest.raises(ValidationError, match="Expected `int`, got `float`"):
            from_builtins(1.5, int)

    @pytest.mark.parametrize("val", [2**64, -(2**63) - 1])
    def test_int_out_of_range(self, val):
        with pytest.raises(ValidationError, match="Integer is out of range"):
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

    @pytest.mark.parametrize("in_type", [list, tuple])
    @pytest.mark.parametrize("out_type", [list, tuple, set, frozenset])
    def test_empty_sequence(self, in_type, out_type):
        assert from_builtins(in_type(), out_type) == out_type()

    @pytest.mark.parametrize("in_type", [list, tuple])
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

    @pytest.mark.parametrize("in_type", [list, tuple])
    @pytest.mark.parametrize(
        "out_annot", [List[int], Tuple[int, ...], Set[int], FrozenSet[int]]
    )
    def test_sequence_wrong_item_type(self, in_type, out_annot):
        with pytest.raises(
            ValidationError, match=r"Expected `int`, got `str` - at `\$\[1\]`"
        ):
            assert from_builtins(in_type([1, "bad"]), out_annot)

    @pytest.mark.parametrize("out_type", [list, tuple, set, frozenset])
    def test_sequence_wrong_type(self, out_type):
        with pytest.raises(ValidationError, match=r"Expected `array`, got `int`"):
            assert from_builtins(1, out_type)

    @pytest.mark.parametrize("out_type", [list, tuple, set, frozenset])
    def test_sequence_cyclic_recursion(self, out_type):
        msg = [1, 2]
        msg.append(msg)
        with pytest.raises(RecursionError):
            assert from_builtins(msg, out_type)

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
