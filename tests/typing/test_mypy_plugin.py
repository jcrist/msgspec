from __future__ import annotations

import tempfile
import textwrap
from collections.abc import Sequence
from pathlib import Path

from mypy import api as mypy_api

_MYPY_INI = """\
[mypy]
strict = True
plugins = msgspec.mypy
"""


def _run_mypy(code: str) -> tuple[Sequence[str], int]:
    """Run mypy on a code snippet with the msgspec mypy plugin enabled.

    Returns (output_lines, exit_code).
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        config_path = Path(tmpdir) / "mypy.ini"
        config_path.write_text(_MYPY_INI)
        source_path = Path(tmpdir) / "test_source.py"
        source_path.write_text(textwrap.dedent(code))

        result = mypy_api.run(
            [
                "--no-incremental",
                "--show-error-codes",
                "--show-traceback",
                "--config-file",
                str(config_path),
                str(source_path),
            ]
        )
    stdout, stderr, exit_code = result
    lines = [line for line in stdout.strip().splitlines() if line.strip()]
    if stderr.strip():
        lines.append("STDERR: " + stderr.strip())
    return lines, exit_code


def _assert_mypy_ok(code: str) -> None:
    lines, exit_code = _run_mypy(code)
    assert exit_code == 0, "Expected no errors, got:\n" + "\n".join(lines)


def _assert_mypy_errors(code: str, expected_fragments: Sequence[str]) -> None:
    lines, exit_code = _run_mypy(code)
    assert exit_code != 0, "Expected errors but mypy passed"
    output = "\n".join(lines)
    for fragment in expected_fragments:
        assert fragment in output, (
            f"Expected fragment {fragment!r} not found in mypy output:\n{output}"
        )


class TestReplace:
    def test_replace_valid_field(self) -> None:
        _assert_mypy_ok(
            """
            import msgspec
            from msgspec.structs import replace

            class C(msgspec.Struct):
                name: str
                value: int

            c = C(name="foo", value=1)
            c2 = replace(c, name="bar")
            c3 = replace(c, value=2)
            c4 = replace(c, name="bar", value=2)
            c5 = replace(c)
        """
        )

    def test_replace_invalid_field(self) -> None:
        _assert_mypy_errors(
            """
            import msgspec
            from msgspec.structs import replace

            class C(msgspec.Struct):
                name: str

            c = C(name="foo")
            replace(c, foobar=42)
            """,
            ['Unexpected keyword argument "foobar"'],
        )

    def test_replace_wrong_type(self) -> None:
        _assert_mypy_errors(
            """
            import msgspec
            from msgspec.structs import replace

            class C(msgspec.Struct):
                name: str

            c = C(name="foo")
            replace(c, name=42)
            """,
            ['Argument "name" to "replace" of "C" has incompatible type "int"; expected "str"'],
        )

    def test_replace_too_many_positional(self) -> None:
        _assert_mypy_errors(
            """
            import msgspec
            from msgspec.structs import replace

            class C(msgspec.Struct):
                name: str

            c = C(name="foo")
            replace(c, "foo")
            """,
            ["Too many positional arguments"],
        )

    def test_replace_preserves_type(self) -> None:
        _assert_mypy_ok(
            """
            import msgspec
            from msgspec.structs import replace

            class C(msgspec.Struct):
                name: str

            c = C(name="foo")
            c2: C = replace(c, name="bar")
        """
        )


class TestReplaceFromNonStruct:
    def test_replace_int(self) -> None:
        _assert_mypy_errors(
            """
            from msgspec.structs import replace
            replace(42, name="foo")
            """,
            ['Argument 1 to "replace" has incompatible type "int"'],
        )

    def test_replace_none(self) -> None:
        _assert_mypy_errors(
            """
            from msgspec.structs import replace
            replace(None, name="foo")
            """,
            ['Argument 1 to "replace"'],
        )


class TestReplaceFromAny:
    def test_replace_any(self) -> None:
        _assert_mypy_ok(
            """
            from typing import Any
            from msgspec.structs import replace

            any_val: Any = 42
            ret = replace(any_val)
        """
        )


class TestReplaceGeneric:
    def test_replace_generic_valid(self) -> None:
        _assert_mypy_ok(
            """
            from typing import Generic, TypeVar
            import msgspec
            from msgspec.structs import replace

            T = TypeVar("T")

            class A(msgspec.Struct, Generic[T]):
                x: T

            a = A(x=42)
            a2 = replace(a, x=42)
        """
        )

    def test_replace_generic_wrong_type(self) -> None:
        _assert_mypy_errors(
            """
            from typing import Generic, TypeVar
            import msgspec
            from msgspec.structs import replace

            T = TypeVar("T")

            class A(msgspec.Struct, Generic[T]):
                x: T

            a = A(x=42)
            replace(a, x="42")
            """,
            [
                'Argument "x" to "replace" of "A[int]" has incompatible type "str"; expected "int"'
            ],
        )


class TestReplaceUnion:
    def test_replace_union_valid(self) -> None:
        _assert_mypy_ok(
            """
            from typing import Generic, TypeVar, Union
            import msgspec
            from msgspec.structs import replace

            T = TypeVar("T")

            class A(msgspec.Struct, Generic[T]):
                x: T
                y: bool

            class B(msgspec.Struct):
                x: int
                y: int

            a_or_b: Union[A[int], B]
            _ = replace(a_or_b, x=42, y=True)
        """
        )

    def test_replace_union_incompatible_field(self) -> None:
        _assert_mypy_errors(
            """
            from typing import Union
            import msgspec
            from msgspec.structs import replace

            class A(msgspec.Struct):
                x: int
                z: str

            class B(msgspec.Struct):
                x: int
                z: bytes

            a_or_b: Union[A, B]
            replace(a_or_b, z="42")
            """,
            [
                'Argument "z" to "replace" of "A | B" has incompatible type "str"; expected "Never"'
            ],
        )


class TestReplaceTypeVarBound:
    def test_replace_typevar_bound_valid(self) -> None:
        _assert_mypy_ok(
            """
            from typing import TypeVar
            import msgspec
            from msgspec.structs import replace

            class A(msgspec.Struct):
                x: int

            class B(A):
                pass

            TA = TypeVar("TA", bound=A)

            def f(t: TA) -> TA:
                return replace(t, x=42)

            f(A(x=42))
            f(B(x=42))
        """
        )

    def test_replace_typevar_bound_wrong_type(self) -> None:
        _assert_mypy_errors(
            """
            from typing import TypeVar
            import msgspec
            from msgspec.structs import replace

            class A(msgspec.Struct):
                x: int

            TA = TypeVar("TA", bound=A)

            def f(t: TA) -> TA:
                return replace(t, x="42")
            """,
            [
                'Argument "x" to "replace" of "TA" has incompatible type "str"; expected "int"'
            ],
        )

    def test_replace_typevar_bound_non_struct(self) -> None:
        _assert_mypy_errors(
            """
            from typing import TypeVar
            from msgspec.structs import replace

            TInt = TypeVar("TInt", bound=int)

            def f(t: TInt) -> None:
                replace(t, x=42)
            """,
            [
                'Argument 1 to "replace" has a variable type "TInt" not bound to a msgspec Struct'
            ],
        )


class TestReplaceFrozen:
    def test_replace_frozen_struct(self) -> None:
        _assert_mypy_ok(
            """
            import msgspec
            from msgspec.structs import replace

            class C(msgspec.Struct, frozen=True):
                name: str
                value: int

            c = C(name="foo", value=1)
            c2 = replace(c, name="bar")
        """
        )

    def test_replace_frozen_struct_invalid_field(self) -> None:
        _assert_mypy_errors(
            """
            import msgspec
            from msgspec.structs import replace

            class C(msgspec.Struct, frozen=True):
                name: str

            c = C(name="foo")
            replace(c, invalid="bar")
            """,
            ['Unexpected keyword argument "invalid"'],
        )


class TestReplaceFromFunction:
    def test_replace_from_function_return(self) -> None:
        _assert_mypy_ok(
            """
            import msgspec
            from msgspec.structs import replace

            class C(msgspec.Struct):
                name: str

            def make_c() -> C:
                return C(name="foo")

            c = replace(make_c(), name="bar")
        """
        )
