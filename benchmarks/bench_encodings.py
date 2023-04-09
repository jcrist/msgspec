from __future__ import annotations

import json
import timeit
from typing import List, Union

import msgpack
import orjson
import ujson
from generate_data import make_filesystem_data

import msgspec


class File(msgspec.Struct, tag="file"):
    name: str
    created_by: str
    created_at: str
    updated_at: str
    nbytes: int


class Directory(msgspec.Struct, tag="directory"):
    name: str
    created_by: str
    created_at: str
    updated_at: str
    contents: List[Union[File, Directory]]


def bench(dumps, loads, ndata, schema=None):
    data = make_filesystem_data(ndata)
    if schema:
        data = msgspec.from_builtins(data, schema)
    timer = timeit.Timer("func(data)", globals={"func": dumps, "data": data})
    n, t = timer.autorange()
    dumps_time = t / n

    data = dumps(data)

    timer = timeit.Timer("func(data)", globals={"func": loads, "data": data})
    n, t = timer.autorange()
    loads_time = t / n
    return dumps_time, loads_time


def bench_msgspec_msgpack(n):
    schema = File if n == 1 else Directory
    enc = msgspec.msgpack.Encoder()
    dec = msgspec.msgpack.Decoder(schema)
    return bench(enc.encode, dec.decode, n, schema)


def bench_msgspec_json(n):
    schema = File if n == 1 else Directory
    enc = msgspec.json.Encoder()
    dec = msgspec.json.Decoder(schema)
    return bench(enc.encode, dec.decode, n, schema)


def bench_msgpack(n):
    packer = msgpack.Packer()
    loads = msgpack.loads
    return bench(packer.pack, loads, n)


def bench_ujson(n):
    return bench(ujson.dumps, ujson.loads, n)


def bench_orjson(n):
    return bench(orjson.dumps, orjson.loads, n)


BENCHMARKS = [
    ("ujson", bench_ujson),
    ("orjson", bench_orjson),
    ("msgpack", bench_msgpack),
    ("msgspec msgpack", bench_msgspec_msgpack),
    ("msgspec json", bench_msgspec_json),
]


def run(n, quiet=False):
    if quiet:

        def log(x):
            pass

    else:
        log = print

    title = f"Benchmark - {n} object{'s' if n > 1 else ''}"
    log(title)

    results = []
    for name, func in BENCHMARKS:
        log(name)
        dumps_time, loads_time = func(n)
        log(f"  dumps: {dumps_time * 1e6:.2f} us")
        log(f"  loads: {loads_time * 1e6:.2f} us")
        log(f"  total: {(dumps_time + loads_time) * 1e6:.2f} us")
        results.append((name, dumps_time, loads_time))
    return results


def main():
    import argparse

    bench_names = ["1", "1k"]

    parser = argparse.ArgumentParser(
        description="Benchmark different python serializers"
    )
    parser.add_argument(
        "--benchmark",
        "-b",
        action="append",
        choices=["all", *bench_names],
        default=[],
        help="which benchmark(s) to run, defaults to 'all'",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="whether to output the results as json",
    )
    parser.add_argument(
        "--no-gc",
        action="store_true",
        help="whether to disable the gc during benchmarking",
    )
    args = parser.parse_args()

    if "all" in args.benchmark or not args.benchmark:
        to_run = bench_names
    else:
        to_run = sorted(set(args.benchmark))

    results = {}
    for bench in to_run:
        n = 1000 if bench.startswith("1k") else 1
        results[bench] = run(n, quiet=args.json)

    if args.json:
        print(json.dumps(results))


if __name__ == "__main__":
    main()
