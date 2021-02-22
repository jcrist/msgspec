import re
import os

import pytest

api = pytest.importorskip("mypy.api")

PATH = os.path.join(os.path.dirname(__file__), "mypy_examples.py")


def get_lineno_type(line):
    assert "revealed type" in line.lower()
    _, lineno, msg = line.split(":", 2)
    lineno = int(lineno)
    pat = re.search("'(.*)'", msg)
    typ = pat.groups()[0]
    return lineno, typ


def test_mypy():
    with open(PATH, "r") as fil:
        ex_lines = fil.readlines()

    stdout, stderr, code = api.run([PATH])
    for line in stdout.splitlines():
        lineno, typ = get_lineno_type(line)
        check = ex_lines[lineno - 1].split("#")[1].strip()
        try:
            exec(check, {"typ": typ})
        except Exception:
            assert (
                False
            ), f"Failed check at {PATH}:{lineno}: {check!r}, where 'typ' is {typ!r}"
