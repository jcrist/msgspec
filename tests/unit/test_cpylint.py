"""This file contains some simple linters for catching some common but easy to
catch cpython capi bugs. These are naive string-munging checks, if you write
some code that _is_ correct but is failing, add `/* cpylint-ignore */` on the
failing source line and it will be ignored."""

import pytest


@pytest.fixture
def source(package_dir):
    with package_dir.joinpath("_core.c").open() as f:
        return f.read().splitlines()


def test_recursive_call_blocks(source):
    """Ensure all code that calls `Py_EnterRecursiveCall` doesn't return
    without calling `Py_LeaveRecursiveCall`"""

    in_block = False
    for lineno, line in enumerate(source, 1):
        if "cpylint-ignore" in line:
            continue

        if "Py_EnterRecursiveCall" in line:
            in_block = True
        elif "return " in line and in_block:
            raise ValueError(
                f"return without calling Py_LeaveRecursiveCall on line {lineno}"
            )
        elif "Py_LeaveRecursiveCall" in line:
            in_block = False


def test_recursive_repr_blocks(source):
    """Ensure all code that calls `Py_ReprEnter` doesn't return without
    calling `Py_ReprLeave`"""
    in_block = False
    for lineno, line in enumerate(source, 1):
        if "cpylint-ignore" in line:
            continue

        if "Py_ReprEnter" in line:
            in_block = True
        elif "return " in line and in_block:
            raise ValueError(f"return without calling Py_ReprLeave on line {lineno}")
        elif "Py_ReprLeave" in line:
            in_block = False
