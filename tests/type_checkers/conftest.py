from pathlib import Path


def pytest_collection_modifyitems(items):
    here = Path(__file__).parent
    for item in items:
        if here in item.path.parents:
            item.add_marker("types")
