import re
import subprocess

import pyright


def test_pyright(typing_examples_file):
    with open(typing_examples_file, "r") as fil:
        ex_lines = fil.readlines()

    result = pyright.run(typing_examples_file, stdout=subprocess.PIPE)
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
            assert False, (
                f"Failed check at {typing_examples_file}:{lineno}: "
                f"{check!r}, where 'typ' is {typ!r}"
            )
