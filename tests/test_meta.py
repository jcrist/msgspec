import re

from msgspec import Meta

import pytest

FIELDS = {
    "gt": 0,
    "ge": 0,
    "lt": 10,
    "le": 10,
    "multiple_of": 1,
    "pattern": "^foo$",
    "min_length": 0,
    "max_length": 10,
    "title": "example title",
    "description": "example description",
    "examples": ["example 1", "example 2"],
    "extra_json_schema": {"foo": "bar"},
}


def assert_eq(a, b):
    assert a == b
    assert not a != b


def assert_ne(a, b):
    assert a != b
    assert not a == b


class TestMetaObject:
    def test_init_nokwargs(self):
        c = Meta()
        for f in FIELDS:
            assert getattr(c, f) is None

    @pytest.mark.parametrize("field", FIELDS)
    def test_init_explicit_none(self, field):
        c = Meta(**{field: None})
        for f in FIELDS:
            assert getattr(c, f) is None

    @pytest.mark.parametrize("field", FIELDS)
    def test_init(self, field):
        c = Meta(**{field: FIELDS[field]})
        for f in FIELDS:
            sol = FIELDS[field] if f == field else None
            assert getattr(c, f) == sol

    def test_repr_empty(self):
        assert repr(Meta()) == "msgspec.Meta()"
        for field in FIELDS:
            c = Meta(**{field: None})
            assert repr(c) == "msgspec.Meta()"

    @pytest.mark.parametrize("field", FIELDS)
    def test_repr_one_field(self, field):
        c = Meta(**{field: FIELDS[field]})
        assert repr(c) == f"msgspec.Meta({field}={FIELDS[field]!r})"

    def test_repr_multiple_fields(self):
        c = Meta(gt=0, lt=1)
        assert repr(c) == "msgspec.Meta(gt=0, lt=1)"

    def test_equality(self):
        assert_eq(Meta(), Meta())
        assert_ne(Meta(), None)

        with pytest.raises(TypeError):
            Meta() > Meta()
        with pytest.raises(TypeError):
            Meta() > None

    @pytest.mark.parametrize("field", FIELDS)
    def test_field_equality(self, field):
        val = FIELDS[field]
        if isinstance(val, dict):
            val2 = {}
        elif isinstance(val, list):
            val2 = []
        elif isinstance(val, int):
            val2 = val + 25
        else:
            val2 = "foobar"

        c = Meta(**{field: val})
        c2 = Meta(**{field: val})
        c3 = Meta(**{field: val2})
        c4 = Meta()
        assert_eq(c, c)
        assert_eq(c, c2)
        assert_ne(c, c3)
        assert_ne(c, c4)
        assert_ne(c4, c)

    @pytest.mark.parametrize("field", ["gt", "ge", "lt", "le", "multiple_of"])
    def test_numeric_fields(self, field):
        Meta(**{field: 1})
        Meta(**{field: 2.5})
        with pytest.raises(
            TypeError, match=f"`{field}` must be an int or float, got str"
        ):
            Meta(**{field: "bad"})

        with pytest.raises(ValueError, match=f"`{field}` must be finite"):
            Meta(**{field: float("inf")})

    @pytest.mark.parametrize("val", [0, 0.0, 2**53, float(2**53)])
    def test_multiple_of_bounds(self, val):
        with pytest.raises(
            ValueError, match=r"`multiple_of` must be > 0 and < 2\*\*53"
        ):
            Meta(multiple_of=val)

    @pytest.mark.parametrize("field", ["min_length", "max_length"])
    def test_nonnegative_integer_fields(self, field):
        Meta(**{field: 0})
        Meta(**{field: 10})
        with pytest.raises(TypeError, match=f"`{field}` must be an int, got float"):
            Meta(**{field: 1.5})
        with pytest.raises(ValueError, match=f"{field}` must be >= 0, got -10"):
            Meta(**{field: -10})

    @pytest.mark.parametrize("field", ["pattern", "title", "description"])
    def test_string_fields(self, field):
        Meta(**{field: "good"})
        with pytest.raises(TypeError, match=f"`{field}` must be a str, got bytes"):
            Meta(**{field: b"bad"})

    @pytest.mark.parametrize("field", ["examples"])
    def test_list_fields(self, field):
        Meta(**{field: ["good", "stuff"]})
        with pytest.raises(TypeError, match=f"`{field}` must be a list, got str"):
            Meta(**{field: "bad"})

    @pytest.mark.parametrize("field", ["extra_json_schema"])
    def test_dict_fields(self, field):
        Meta(**{field: {"good": "stuff"}})
        with pytest.raises(TypeError, match=f"`{field}` must be a dict, got str"):
            Meta(**{field: "bad"})

    def test_invalid_pattern_errors(self):
        with pytest.raises(re.error):
            Meta(pattern="[abc")

    def test_conflicting_bounds_errors(self):
        with pytest.raises(ValueError, match="both `gt` and `ge`"):
            Meta(gt=0, ge=1)

        with pytest.raises(ValueError, match="both `lt` and `le`"):
            Meta(lt=0, le=1)

    def test_mixing_numeric_and_nonnumeric_constraints_errors(self):
        with pytest.raises(ValueError, match="Cannot mix numeric constraints"):
            Meta(gt=0, pattern="foo")
