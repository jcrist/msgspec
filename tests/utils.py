import textwrap
import sys
import types
import uuid
from contextlib import contextmanager


@contextmanager
def temp_module(code):
    """Mutually recursive struct types defined inside functions don't work (and
    probably never will). To avoid populating a bunch of test structs in the
    top level of this module, we instead create a temporary module per test to
    exec whatever is needed for that test"""
    code = textwrap.dedent(code)
    name = f"temp_{uuid.uuid4().hex}"
    mod = types.ModuleType(name)
    sys.modules[name] = mod
    try:
        exec(code, mod.__dict__)
        yield mod
    finally:
        sys.modules.pop(name, None)
