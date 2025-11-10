import os

import pytest


@pytest.fixture(scope="session")
def typing_examples_file():
    return os.path.join(os.path.dirname(__file__), "basic_typing_examples.py")
