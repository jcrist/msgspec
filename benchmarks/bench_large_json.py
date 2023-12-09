import subprocess
import sys
import tempfile

import requests

TEMPLATE = """
import resource
import time

with open({path!r}, "rb") as f:
    data = f.read()

initial_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

{setup}

start = time.perf_counter()
for _ in range(5):
    decode(data)
stop = time.perf_counter()

max_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
# mem_mib = (max_rss * 1024 - len(data)) / (1024 * 1024)
mem_mib = (max_rss - initial_rss) / 1024
time_ms = ((stop - start) / 5) * 1000
print([mem_mib, time_ms])
"""

JSON = """
import json
decode = json.loads
"""

UJSON = """
import ujson
decode = ujson.loads
"""

ORJSON = """
import orjson
decode = orjson.loads
"""

RAPIDJSON = """
import rapidjson
decode = rapidjson.loads
"""

SIMDJSON = """
import simdjson
decode = simdjson.loads
"""

MSGSPEC = """
import msgspec
decode = msgspec.json.decode
"""

MSGSPEC_STRUCTS = """
import msgspec
from typing import Union

class Package(msgspec.Struct, gc=False):
    build: str
    build_number: int
    depends: tuple[str, ...]
    md5: str
    name: str
    sha256: str
    subdir: str
    version: str
    license: str = ""
    noarch: Union[str, bool, None] = None
    size: int = 0
    timestamp: int = 0

class RepoData(msgspec.Struct, gc=False):
    repodata_version: int
    info: dict
    packages: dict[str, Package]
    removed: tuple[str, ...]

decode = msgspec.json.Decoder(RepoData).decode
"""


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Benchmark decoding a large JSON message using various JSON libraries"
    )
    parser.add_argument(
        "--versions",
        action="store_true",
        help="Output library version info, and exit immediately",
    )
    args = parser.parse_args()

    benchmarks = [
        ("json", None, JSON),
        ("ujson", "ujson", UJSON),
        ("orjson", "orjson", ORJSON),
        ("rapidjson", "python-rapidjson", RAPIDJSON),
        ("simdjson", "pysimdjson", SIMDJSON),
        ("msgspec", "msgspec", MSGSPEC),
        ("msgspec structs", None, MSGSPEC_STRUCTS),
    ]

    if args.versions:
        import importlib.metadata

        for _, lib, _ in benchmarks:
            if lib is not None:
                version = importlib.metadata.version(lib)
                print(f"- {lib}: {version}")
        sys.exit(0)

    with tempfile.NamedTemporaryFile() as f:
        # Download the repodata.json
        resp = requests.get(
            "https://conda.anaconda.org/conda-forge/noarch/repodata.json"
        )
        resp.raise_for_status()
        f.write(resp.content)

        # Run the benchmark for each library
        results = {}
        import ast

        for lib, _, setup in benchmarks:
            script = TEMPLATE.format(path=f.name, setup=setup)
            # We execute each script in a subprocess to isolate their memory usage
            output = subprocess.check_output([sys.executable, "-c", script])
            results[lib] = ast.literal_eval(output.decode())

        # Compose the results table
        best_mem, best_time = results["msgspec structs"]
        columns = (
            "",
            "memory (MiB)",
            "vs.",
            "time (ms)",
            "vs.",
        )
        rows = [
            (
                f"**{lib}**",
                f"{mem:.1f}",
                f"{mem / best_mem:.1f}x",
                f"{time:.1f}",
                f"{time / best_time:.1f}x",
            )
            for lib, (mem, time) in results.items()
        ]
        rows.sort(key=lambda x: float(x[1]))
        widths = tuple(
            max(max(map(len, x)), len(c)) for x, c in zip(zip(*rows), columns)
        )
        row_template = ("|" + (" %%-%ds |" * len(columns))) % widths
        header = row_template % tuple(columns)
        bar_underline = "+%s+" % "+".join("=" * (w + 2) for w in widths)
        bar = "+%s+" % "+".join("-" * (w + 2) for w in widths)
        parts = [bar, header, bar_underline]
        for r in rows:
            parts.append(row_template % r)
            parts.append(bar)
        print("\n".join(parts))


if __name__ == "__main__":
    main()
