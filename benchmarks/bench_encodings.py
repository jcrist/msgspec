from __future__ import annotations

import sys
import dataclasses
import json
import timeit
import importlib.metadata
from typing import Any, Literal, Callable

from .generate_data import make_filesystem_data

import msgspec


class File(msgspec.Struct, kw_only=True, omit_defaults=True, tag="file"):
    name: str
    created_by: str
    created_at: str
    updated_by: str | None = None
    updated_at: str | None = None
    nbytes: int
    permissions: Literal["READ", "WRITE", "READ_WRITE"]


class Directory(msgspec.Struct, kw_only=True, omit_defaults=True, tag="directory"):
    name: str
    created_by: str
    created_at: str
    updated_by: str | None = None
    updated_at: str | None = None
    contents: list[File | Directory]


@dataclasses.dataclass
class Benchmark:
    label: str
    version: str
    encode: Callable
    decode: Callable
    schema: Any = None

    def run(self, data: bytes) -> dict:
        if self.schema is not None:
            data = msgspec.convert(data, self.schema)
        timer = timeit.Timer("func(data)", globals={"func": self.encode, "data": data})
        n, t = timer.autorange()
        encode_time = t / n

        data = self.encode(data)

        timer = timeit.Timer("func(data)", globals={"func": self.decode, "data": data})
        n, t = timer.autorange()
        decode_time = t / n

        return {
            "label": self.label,
            "encode": encode_time,
            "decode": decode_time,
        }


def json_benchmarks():
    import orjson
    import ujson
    import rapidjson
    import simdjson

    simdjson_ver = importlib.metadata.version("pysimdjson")

    rj_dumps = rapidjson.Encoder()
    rj_loads = rapidjson.Decoder()

    def uj_dumps(obj):
        return ujson.dumps(obj)

    enc = msgspec.json.Encoder()
    dec = msgspec.json.Decoder(Directory)
    dec2 = msgspec.json.Decoder()

    return [
        Benchmark("msgspec structs", None, enc.encode, dec.decode, Directory),
        Benchmark("msgspec", msgspec.__version__, enc.encode, dec2.decode),
        Benchmark("json", None, json.dumps, json.loads),
        Benchmark("orjson", orjson.__version__, orjson.dumps, orjson.loads),
        Benchmark("ujson", ujson.__version__, uj_dumps, ujson.loads),
        Benchmark("rapidjson", rapidjson.__version__, rj_dumps, rj_loads),
        Benchmark("simdjson", simdjson_ver, simdjson.dumps, simdjson.loads),
    ]


def msgpack_benchmarks():
    import msgpack
    import ormsgpack

    enc = msgspec.msgpack.Encoder()
    dec = msgspec.msgpack.Decoder(Directory)
    dec2 = msgspec.msgpack.Decoder()

    return [
        Benchmark("msgspec structs", None, enc.encode, dec.decode, Directory),
        Benchmark("msgspec", msgspec.__version__, enc.encode, dec2.decode),
        Benchmark("msgpack", msgpack.__version__, msgpack.dumps, msgpack.loads),
        Benchmark(
            "ormsgpack", ormsgpack.__version__, ormsgpack.packb, ormsgpack.unpackb
        ),
    ]


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Benchmark different python serialization libraries"
    )
    parser.add_argument(
        "--versions",
        action="store_true",
        help="Output library version info, and exit immediately",
    )
    parser.add_argument(
        "-n",
        type=int,
        help="The number of objects in the generated data, defaults to 1000",
        default=1000,
    )
    parser.add_argument(
        "-p",
        "--protocol",
        choices=["json", "msgpack"],
        default="json",
        help="The protocol to benchmark, defaults to JSON",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="whether to output the results as json",
    )
    args = parser.parse_args()

    benchmarks = json_benchmarks() if args.protocol == "json" else msgpack_benchmarks()

    if args.versions:
        for bench in benchmarks:
            if bench.version is not None:
                print(f"- {bench.label}: {bench.version}")
        sys.exit(0)

    data = make_filesystem_data(args.n)

    results = [benchmark.run(data) for benchmark in benchmarks]

    if args.json:
        for line in results:
            print(json.dumps(line))
    else:
        # Compose the results table
        results.sort(key=lambda row: row["encode"] + row["decode"])
        best_et = results[0]["encode"]
        best_dt = results[0]["decode"]
        best_tt = best_et + best_dt

        columns = (
            "",
            "encode (μs)",
            "vs.",
            "decode (μs)",
            "vs.",
            "total (μs)",
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
            )
            for r in results
        ]
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
