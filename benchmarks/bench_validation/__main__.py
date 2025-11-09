import argparse
import json
import tempfile
from ..generate_data import make_filesystem_data
import sys
import subprocess
import shutil


LIBRARIES = ["msgspec", "mashumaro", "cattrs", "pydantic"]


parser = argparse.ArgumentParser(
    description="Benchmark different python validation libraries"
)
parser.add_argument(
    "--json",
    action="store_true",
    help="Whether to output the results as json",
)
parser.add_argument(
    "--size",
    "-s",
    type=int,
    help="The number of objects in the generated data, defaults to 1000",
    default=1000,
)
parser.add_argument(
    "--iterations",
    "-n",
    type=int,
    help="The number of iterations to perform for each library, defaults to auto-detection",
)
parser.add_argument(
    "--rounds",
    "-r",
    type=int,
    help="The number of times to repeat the benchmark for each library if --iterations is selected, defaults to 5",
    default=5,
)
parser.add_argument(
    "--lib",
    dest="libs",
    nargs="*",
    choices=LIBRARIES,
    default=LIBRARIES,
    help="A list of libraries to benchmark. Defaults to all.",
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


data = json.dumps(make_filesystem_data(int(args.size))).encode("utf-8")

iterations = str(args.iterations or 0)
rounds = str(args.rounds or 0)
header = "-" * shutil.get_terminal_size().columns
results = []
with tempfile.NamedTemporaryFile() as f:
    f.write(data)
    f.flush()

    for lib in args.libs:
        try:
            res = subprocess.check_output(
                [
                    sys.executable,
                    "-m",
                    "benchmarks.bench_validation.runner",
                    lib,
                    f.name,
                    iterations,
                    rounds,
                ],
                stderr=subprocess.STDOUT,
            )
            results.append(json.loads(res))
        except subprocess.CalledProcessError as e:
            if not args.json:
                print(header, file=sys.stderr)
                print(f"Warning: {lib} failed to run, skipping...", file=sys.stderr)
                print(e.output.decode("utf-8", errors="replace"), file=sys.stderr)
                print(header, file=sys.stderr)

if not results:
    print("Error: All libraries failed to run. No results to display.", file=sys.stderr)
    sys.exit(1)

if args.json:
    for line in results:
        print(json.dumps(line))
else:
    # Compose the results table
    results.sort(key=lambda row: row["encode"] + row["decode"])
    best_et = results[0]["encode"]
    best_dt = results[0]["decode"]
    best_tt = best_et + best_dt
    # Avoid division by zero if memory is 0
    best_mem = results[0]["memory"] or 1.0

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
