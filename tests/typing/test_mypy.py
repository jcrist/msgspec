import re

from mypy import api


def get_lineno_type(line):
    assert "revealed type" in line.lower()
    _, lineno, msg = line.split(":", 2)
    lineno = int(lineno)
    pat = re.search("[\"'](.*)[\"']", msg)
    typ = pat.groups()[0]
    return lineno, typ


def test_mypy(typing_examples_file):
    with open(typing_examples_file, "r") as fil:
        ex_lines = fil.readlines()

    stdout, stderr, code = api.run([typing_examples_file])
    lines = stdout.splitlines()
    for line in lines:
        if "revealed type" in line.lower():
            lineno, typ = get_lineno_type(line)
            check = ex_lines[lineno - 1].split("#")[1].strip()
            try:
                exec(check, {"typ": typ})
            except Exception:
                assert False, (
                    f"Failed check at {typing_examples_file}:{lineno}: "
                    f"{check!r}, where 'typ' is {typ!r}"
                )
        elif "success" not in line.lower():
            assert False, line
