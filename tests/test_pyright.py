import os
import re
import subprocess

import pytest

pytestmark = pytest.mark.pyright

pyright = pytest.importorskip("pyright")

PATH = os.path.join(os.path.dirname(__file__), "basic_typing_examples.py")


def test_pyright():
    with open(PATH, "r") as fil:
        ex_lines = fil.readlines()

    result = pyright.run(PATH, stdout=subprocess.PIPE)
    if result.returncode != 0:
        assert False, f"Unexpected pyright error:\n{result.stdout}"
    for line in result.stdout.decode().splitlines():
        try:
            _, lineno, _, msg = line.split(":", 3)
        except ValueError:
            continue
        lineno = int(lineno)
        pat = re.search("[\"'](.*)[\"']", msg)
        typ = pat.groups()[0]
        check = ex_lines[lineno - 1].split("#")[1].strip()
        try:
            exec(check, {"typ": typ})
        except Exception:
            assert (
                False
            ), f"Failed check at {PATH}:{lineno}: {check!r}, where 'typ' is {typ!r}"
