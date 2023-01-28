import pytest

import msgspec
import msgspec.structs as ms


class TestStructInfo:
    def test_struct_field(self):
        def factory():
            return 1

        class Example(msgspec.Struct):
            x: int
            y: int = 0
            z: int = msgspec.field(default_factory=factory)

        info = ms.info(Example)
        x_field, y_field, z_field = info.fields

        assert x_field.required
        assert x_field.default is ms.UNSET
        assert x_field.default_factory is ms.UNSET

        assert not y_field.required
        assert y_field.default == 0
        assert y_field.default_factory is ms.UNSET

        assert not z_field.required
        assert z_field.default is ms.UNSET
        assert z_field.default_factory is factory

    @pytest.mark.parametrize(
        "kw",
        [
            {},
            {"array_like": True},
            {"forbid_unknown_fields": True},
            {"tag": "Example", "tag_field": "type"},
        ],
    )
    def test_struct_info(self, kw):
        def factory():
            return "foo"

        class Example(msgspec.Struct, **kw):
            x: int
            y: int = 0
            z: int = msgspec.field(default_factory=factory)

        sol = ms.StructInfo(
            cls=Example,
            fields=(
                ms.StructField("x", "x", int),
                ms.StructField("y", "y", int, default=0),
                ms.StructField("z", "z", int, default_factory=factory),
            ),
            **kw
        )
        assert ms.info(Example) == sol

    def test_struct_info_no_fields(self):
        class Example(msgspec.Struct):
            pass

        sol = ms.StructInfo(Example, fields=())
        assert ms.info(Example) == sol

    def test_struct_info_keyword_only(self):
        class Example(msgspec.Struct, kw_only=True):
            a: int
            b: int = 1
            c: int
            d: int = 2

        sol = ms.StructInfo(
            Example,
            fields=(
                ms.StructField("a", "a", int),
                ms.StructField("b", "b", int, default=1),
                ms.StructField("c", "c", int),
                ms.StructField("d", "d", int, default=2),
            ),
        )
        assert ms.info(Example) == sol

    def test_struct_info_encode_name(self):
        class Example(msgspec.Struct, rename="camel"):
            field_one: int
            field_two: int

        sol = ms.StructInfo(
            Example,
            fields=(
                ms.StructField("field_one", "fieldOne", int),
                ms.StructField("field_two", "fieldTwo", int),
            ),
        )
        assert ms.info(Example) == sol
