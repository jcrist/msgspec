import pytest

from benchmarks.generate_data import make_filesystem_data


@pytest.fixture
def bench(benchmark, bench_config):
    if not bench_config["calibrate"]:
        return lambda target, *args, **kwargs: benchmark(target, *args, **kwargs)

    return lambda target, *args, **kwargs: benchmark.pedantic(
        target,
        args=args,
        kwargs=kwargs,
        rounds=bench_config["rounds"],
        iterations=bench_config["iterations"],
        warmup_rounds=bench_config["warmup_rounds"],
    )


@pytest.fixture
def filesystem_data(bench_config):
    return lambda size=bench_config["size"]: make_filesystem_data(size)


@pytest.fixture(scope="class")
def shared_data():
    """
    This is used to share data between serially-executed tests within a class.
    """
    return {}


@pytest.fixture
def bench_config(request):
    return {
        "calibrate": request.config.getoption("--calibrate"),
        "rounds": request.config.getoption("--rounds"),
        "warmup_rounds": request.config.getoption("--warmup-rounds"),
        "iterations": request.config.getoption("--iterations"),
        "size": request.config.getoption("--size"),
    }
