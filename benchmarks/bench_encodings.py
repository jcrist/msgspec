import datetime
import gc
import json
import random
import string
import timeit
from typing import List, Optional

import msgpack
import orjson
import ujson

import msgspec

# fmt: off
states = [
    "AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "HI", "ID",
    "IL", "IN", "IA", "KS", "KY", "LA", "ME", "MD", "MA", "MI", "MN", "MS",
    "MO", "MT", "NE", "NV", "NH", "NJ", "NM", "NY", "NC", "ND", "OH", "OK",
    "OR", "PA", "RI", "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV",
    "WI", "WY",
]
# fmt: on

UTC = datetime.timezone.utc
date_1950 = datetime.datetime(1950, 1, 1, tzinfo=UTC)
date_2018 = datetime.datetime(2018, 1, 1, tzinfo=UTC)
date_2023 = datetime.datetime(2023, 1, 1, tzinfo=UTC)


def randdt(random, min, max):
    ts = random.randint(min.timestamp(), max.timestamp())
    return datetime.datetime.fromtimestamp(ts).replace(tzinfo=UTC)


def randstr(random, min=None, max=None):
    if max is not None:
        min = random.randint(min, max)
    return "".join(random.choices(string.ascii_letters, k=min))


def make_user(rand=random):
    n_addresses = rand.choice([0, 1, 1, 2, 2, 3])
    has_phone = rand.choice([True, True, False])
    has_email = rand.choice([True, True, False])

    addresses = [
        {
            "street": randstr(rand, 10, 40),
            "state": rand.choice(states),
            "zip": rand.randint(10000, 99999),
        }
        for _ in range(n_addresses)
    ]

    created = randdt(rand, date_2018, date_2023)
    updated = randdt(rand, created, date_2023)
    birthday = randdt(rand, date_1950, date_2023).date()

    return {
        "created": created.isoformat(),
        "updated": updated.isoformat(),
        "first": randstr(rand, 3, 15),
        "last": randstr(rand, 3, 15),
        "birthday": birthday.isoformat(),
        "addresses": addresses if addresses else None,
        "telephone": randstr(rand, 9) if has_phone else None,
        "email": randstr(rand, 15, 30) if has_email else None,
    }


def make_users(n, seed=42):
    rand = random.Random(seed)
    if n == 1:
        user = make_user(rand)
        user["addresses"] = None
        return user
    else:
        return [make_user(rand) for _ in range(n)]


class Address(msgspec.Struct, gc=False):
    street: str
    state: str
    zip: int


class User(msgspec.Struct, gc=False):
    created: str
    updated: str
    first: str
    last: str
    birthday: str
    addresses: Optional[List[Address]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


def bench(dumps, loads, ndata, schema=None, no_gc=False):
    setup = "" if no_gc else "gc.enable()"
    data = make_users(ndata)
    if schema:
        data = msgspec.from_builtins(data, schema)
    gc.collect()
    timer = timeit.Timer(
        "func(data)", setup=setup, globals={"func": dumps, "data": data, "gc": gc}
    )
    n, t = timer.autorange()
    dumps_time = t / n

    data = dumps(data)

    gc.collect()
    timer = timeit.Timer(
        "func(data)", setup=setup, globals={"func": loads, "data": data, "gc": gc}
    )
    n, t = timer.autorange()
    loads_time = t / n
    return dumps_time, loads_time


def bench_msgspec_msgpack(n, no_gc):
    schema = User if n == 1 else List[User]
    enc = msgspec.msgpack.Encoder()
    dec = msgspec.msgpack.Decoder(schema)
    return bench(enc.encode, dec.decode, n, schema, no_gc=no_gc)


def bench_msgspec_json(n, no_gc):
    schema = User if n == 1 else List[User]
    enc = msgspec.json.Encoder()
    dec = msgspec.json.Decoder(schema)
    return bench(enc.encode, dec.decode, n, schema, no_gc=no_gc)


def bench_msgpack(n, no_gc):
    packer = msgpack.Packer()
    loads = msgpack.loads
    return bench(packer.pack, loads, n, no_gc=no_gc)


def bench_ujson(n, no_gc):
    return bench(ujson.dumps, ujson.loads, n, no_gc=no_gc)


def bench_orjson(n, no_gc):
    return bench(orjson.dumps, orjson.loads, n, no_gc=no_gc)


BENCHMARKS = [
    ("ujson", bench_ujson),
    ("orjson", bench_orjson),
    ("msgpack", bench_msgpack),
    ("msgspec msgpack", bench_msgspec_msgpack),
    ("msgspec json", bench_msgspec_json),
]


def run(n, no_gc=False, quiet=False):
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
        dumps_time, loads_time = func(n, no_gc)
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
        results[bench] = run(n, no_gc=args.no_gc, quiet=args.json)

    if args.json:
        print(json.dumps(results))


if __name__ == "__main__":
    main()
