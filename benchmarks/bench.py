import gc
import json
import random
import string
import timeit
from typing import List, Optional

import msgpack
import msgspec
import orjson
import proto_bench
import pydantic
import ujson


class Address(msgspec.Struct, gc=False):
    street: str
    state: str
    zip: int


class Person(msgspec.Struct, gc=False):
    first: str
    last: str
    age: int
    addresses: Optional[List[Address]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


class Address2(msgspec.Struct, array_like=True, gc=False):
    street: str
    state: str
    zip: int


class Person2(msgspec.Struct, array_like=True, gc=False):
    first: str
    last: str
    age: int
    addresses: Optional[List[Address2]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


class AddressModel(pydantic.BaseModel):
    street: str
    state: str
    zip: int


class PersonModel(pydantic.BaseModel):
    first: str
    last: str
    age: int
    addresses: Optional[List[AddressModel]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


class PeopleModel(pydantic.BaseModel):
    items: List[PersonModel]


states = [
    "AL",
    "AK",
    "AZ",
    "AR",
    "CA",
    "CO",
    "CT",
    "DE",
    "FL",
    "GA",
    "HI",
    "ID",
    "IL",
    "IN",
    "IA",
    "KS",
    "KY",
    "LA",
    "ME",
    "MD",
    "MA",
    "MI",
    "MN",
    "MS",
    "MO",
    "MT",
    "NE",
    "NV",
    "NH",
    "NJ",
    "NM",
    "NY",
    "NC",
    "ND",
    "OH",
    "OK",
    "OR",
    "PA",
    "RI",
    "SC",
    "SD",
    "TN",
    "TX",
    "UT",
    "VT",
    "VA",
    "WA",
    "WV",
    "WI",
    "WY",
]


def randstr(random, min=None, max=None):
    if max is not None:
        min = random.randint(min, max)
    return "".join(random.choices(string.ascii_letters, k=min))


def make_person(rand=random):
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

    return {
        "first": randstr(rand, 3, 15),
        "last": randstr(rand, 3, 15),
        "age": rand.randint(0, 99),
        "addresses": addresses if addresses else None,
        "telephone": randstr(rand, 9) if has_phone else None,
        "email": randstr(rand, 15, 30) if has_email else None,
    }


def make_people(n, seed=42):
    rand = random.Random(seed)
    if n > 1:
        return [make_person(rand) for _ in range(n)]
    else:
        person = make_person(rand)
        person["addresses"] = None
        return person


def bench(dumps, loads, ndata, convert=None, no_gc=False):
    setup = "" if no_gc else "gc.enable()"
    data = make_people(ndata)
    if convert:
        data = convert(data)
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


def bench_msgspec_msgpack(n, no_gc, validate=False):
    enc = msgspec.msgpack.Encoder()
    dec = msgspec.msgpack.Decoder(Person if n == 1 else List[Person])

    def convert_one(addresses=None, **kwargs):
        addrs = [Address(**a) for a in addresses] if addresses else None
        return Person(addresses=addrs, **kwargs)

    def convert(data):
        return (
            [convert_one(**d) for d in data]
            if isinstance(data, list)
            else convert_one(**data)
        )

    return bench(enc.encode, dec.decode, n, convert, no_gc=no_gc)


def bench_msgspec_msgpack_array_like(n, no_gc, validate=False):
    enc = msgspec.msgpack.Encoder()
    dec = msgspec.msgpack.Decoder(Person2 if n == 1 else List[Person2])

    def convert_one(addresses=None, **kwargs):
        addrs = [Address2(**a) for a in addresses] if addresses else None
        return Person2(addresses=addrs, **kwargs)

    def convert(data):
        return (
            [convert_one(**d) for d in data]
            if isinstance(data, list)
            else convert_one(**data)
        )

    return bench(enc.encode, dec.decode, n, convert, no_gc=no_gc)


def bench_msgspec_json(n, no_gc, validate=False):
    enc = msgspec.json.Encoder()
    dec = msgspec.json.Decoder(Person if n == 1 else List[Person])

    def convert_one(addresses=None, **kwargs):
        addrs = [Address(**a) for a in addresses] if addresses else None
        return Person(addresses=addrs, **kwargs)

    def convert(data):
        return (
            [convert_one(**d) for d in data]
            if isinstance(data, list)
            else convert_one(**data)
        )

    return bench(enc.encode, dec.decode, n, convert, no_gc=no_gc)


def bench_msgspec_json_array_like(n, no_gc, validate=False):
    enc = msgspec.json.Encoder()
    dec = msgspec.json.Decoder(Person2 if n == 1 else List[Person2])

    def convert_one(addresses=None, **kwargs):
        addrs = [Address2(**a) for a in addresses] if addresses else None
        return Person2(addresses=addrs, **kwargs)

    def convert(data):
        return (
            [convert_one(**d) for d in data]
            if isinstance(data, list)
            else convert_one(**data)
        )

    return bench(enc.encode, dec.decode, n, convert, no_gc=no_gc)


def bench_msgpack(n, no_gc, validate=False):
    packer = msgpack.Packer()
    if validate:
        if n == 1:

            def loads(b):
                return PersonModel(**msgpack.loads(b))

        else:

            def loads(b):
                return PeopleModel(items=msgpack.loads(b)).items

    else:
        loads = msgpack.loads

    return bench(packer.pack, loads, n, no_gc=no_gc)


def bench_ujson(n, no_gc, validate=False):
    if validate:
        if n == 1:

            def loads(b):
                return PersonModel(**ujson.loads(b))

        else:

            def loads(b):
                return PeopleModel(items=ujson.loads(b)).items

    else:
        loads = ujson.loads

    return bench(ujson.dumps, loads, n, no_gc=no_gc)


def bench_orjson(n, no_gc, validate=False):
    if validate:
        if n == 1:

            def loads(b):
                return PersonModel(**orjson.loads(b))

        else:

            def loads(b):
                return PeopleModel(items=orjson.loads(b)).items

    else:
        loads = orjson.loads

    return bench(orjson.dumps, loads, n, no_gc=no_gc)


def bench_pyrobuf(n, no_gc, validate=False):
    def convert_one(addresses=None, email=None, telephone=None, **kwargs):
        p = proto_bench.Person()
        p.ParseFromDict(kwargs)
        if addresses:
            for a in addresses:
                p.addresses.append(proto_bench.Address(**a))
        if telephone:
            p.telephone = telephone
        if email:
            p.email = email
        return p

    def convert(data):
        if isinstance(data, list):
            data = proto_bench.People(people=[convert_one(**d) for d in data])
        else:
            data = convert_one(**data)
        return data

    if n > 1:
        loads = proto_bench.People.FromString
    else:
        loads = proto_bench.Person.FromString

    def dumps(p):
        return p.SerializeToString()

    return bench(dumps, loads, n, convert, no_gc=no_gc)


BENCHMARKS = [
    ("ujson", bench_ujson),
    ("orjson", bench_orjson),
    ("msgpack", bench_msgpack),
    ("pyrobuf", bench_pyrobuf),
    ("msgspec msgpack", bench_msgspec_msgpack),
    ("msgspec msgpack array-like", bench_msgspec_msgpack_array_like),
    ("msgspec json", bench_msgspec_json),
    ("msgspec json array-like", bench_msgspec_json_array_like),
]


def run(n, validate=False, no_gc=False, quiet=False):
    if quiet:

        def log(x):
            pass

    else:
        log = print

    title = f"Benchmark - {n} object{'s' if n > 1 else ''}"
    if validate:
        title += " (with validation)"
    log(title)

    results = []
    for name, func in BENCHMARKS:
        log(name)
        dumps_time, loads_time = func(n, no_gc, validate)
        log(f"  dumps: {dumps_time * 1e6:.2f} us")
        log(f"  loads: {loads_time * 1e6:.2f} us")
        log(f"  total: {(dumps_time + loads_time) * 1e6:.2f} us")
        results.append((name, dumps_time, loads_time))
    return results


def main():
    import argparse

    bench_names = ["1", "1k", "1-valid", "1k-valid"]

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
        validate = bench.endswith("valid")
        results[bench] = run(n, validate=validate, no_gc=args.no_gc, quiet=args.json)

    if args.json:
        print(json.dumps(results))


if __name__ == "__main__":
    main()
