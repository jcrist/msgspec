import argparse
import json
import tempfile
from ..generate_data import make_filesystem_data
import sys
import subprocess


LIBRARIES = ["msgspec", "mashumaro", "cattrs", "pydantic"]


def parse_list(value):
    libs = [lib.strip() for lib in value.split(",")]
    for lib in libs:
        if lib not in LIBRARIES:
            print(f"{lib!r} is not a supported library, choose from {LIBRARIES}")
            sys.exit(1)
    return libs


parser = argparse.ArgumentParser(
    description="Benchmark different python validation libraries"
)
parser.add_argument(
    "--json",
    action="store_true",
    help="Whether to output the results as json",
)
parser.add_argument(
    "-n",
    type=int,
    help="The number of objects in the generated data, defaults to 1000",
    default=1000,
)
parser.add_argument(
    "--libs",
    type=parse_list,
    help="A comma-separated list of libraries to benchmark. Defaults to all.",
    default=LIBRARIES,
)
parser.add_argument(
    "--versions",
    action="store_true",
    help="Output library version info, and exit immediately",
)
args = parser.parse_args()

if args.versions:
    import importlib.metadata

    for lib in args.libs:
        version = importlib.metadata.version(lib)
        print(f"- {lib}: {version}")
    sys.exit(0)


data = json.dumps(make_filesystem_data(args.n)).encode("utf-8")

results = []
with tempfile.NamedTemporaryFile() as f:
    f.write(data)
    f.flush()

    for lib in args.libs:
        res = subprocess.check_output(
            [sys.executable, "-m", "benchmarks.bench_validation.runner", lib, f.name]
        )
        results.append(json.loads(res))

if args.json:
    for line in results:
        print(json.dumps(line))
else:
    # Compose the results table
    results.sort(key=lambda row: row["encode"] + row["decode"])
    best_et = results[0]["encode"]
    best_dt = results[0]["decode"]
    best_tt = best_et + best_dt
    best_mem = results[0]["memory"]

    columns = (
        "",
        "encode (μs)",
        "vs.",
        "decode (μs)",
        "vs.",
        "total (μs)",
        "vs.",
        "memory (MiB)",
        "vs.",
    )
    rows = [
        (
            r["label"],
            f"{1_000_000 * r['encode']:.1f}",
            f"{r['encode'] / best_et:.1f}",
            f"{1_000_000 * r['decode']:.1f}",
            f"{r['decode'] / best_dt:.1f}",
            f"{1_000_000 * (r['encode'] + r['decode']):.1f}",
            f"{(r['encode'] + r['decode']) / best_tt:.1f}",
            f"{r['memory']:.1f}",
            f"{r['memory'] / best_mem:.1f}",
        )
        for r in results
    ]
    widths = tuple(max(max(map(len, x)), len(c)) for x, c in zip(zip(*rows), columns))
    row_template = ("|" + (" %%-%ds |" * len(columns))) % widths
    header = row_template % tuple(columns)
    bar_underline = "+%s+" % "+".join("=" * (w + 2) for w in widths)
    bar = "+%s+" % "+".join("-" * (w + 2) for w in widths)
    parts = [bar, header, bar_underline]
    for r in rows:
        parts.append(row_template % r)
        parts.append(bar)
    print("\n".join(parts))
