import gc
import json
import random
import string
import timeit
from typing import List, Optional

import orjson
import cattrs.preconf.orjson
import attrs
import msgspec
import pydantic
import marshmallow as mm


def orjson_dumps(x, **kwargs):
    return orjson.dumps(x, **kwargs).decode()


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
    return {"people": [make_person(rand) for _ in range(n)]}


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


#############################################################################
#  msgspec                                                                  #
#############################################################################


class AddressStruct(msgspec.Struct, gc=False):
    street: str
    state: str
    zip: int


class PersonStruct(msgspec.Struct, gc=False):
    first: str
    last: str
    age: int
    addresses: Optional[List[AddressStruct]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


class PeopleStruct(msgspec.Struct, gc=False):
    people: List[PersonStruct]


def bench_msgspec(n, no_gc):
    enc = msgspec.json.Encoder()
    dec = msgspec.json.Decoder(PeopleStruct)

    def convert(data):
        people = []
        for p in data["people"]:
            addrs = p.pop("addresses", None)
            people.append(
                PersonStruct(
                    addresses=[AddressStruct(**a) for a in addrs] if addrs else None,
                    **p,
                )
            )
        return PeopleStruct(people=people)

    return bench(enc.encode, dec.decode, n, convert, no_gc=no_gc)


#############################################################################
#  pydantic                                                                 #
#############################################################################


class BaseModel(pydantic.BaseModel):
    class Config:
        json_dumps = orjson_dumps
        json_loads = orjson.loads


class AddressModel(BaseModel):
    street: str
    state: str
    zip: int


class PersonModel(BaseModel):
    first: str
    last: str
    age: int
    addresses: Optional[List[AddressModel]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


class PeopleModel(BaseModel):
    people: List[PersonModel]


def bench_pydantic(n, no_gc):
    return bench(
        lambda p: p.json(),
        PeopleModel.parse_raw,
        n,
        lambda data: PeopleModel(**data),
        no_gc=no_gc,
    )


#############################################################################
#  cattrs                                                                   #
#############################################################################


@attrs.define
class AddressAttrs:
    street: str
    state: str
    zip: int


@attrs.define
class PersonAttrs:
    first: str
    last: str
    age: int
    addresses: Optional[List[AddressAttrs]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


@attrs.define
class PeopleAttrs:
    people: List[PersonAttrs]


def bench_cattrs(n, no_gc):
    converter = cattrs.preconf.orjson.make_converter()
    return bench(
        converter.dumps,
        lambda msg: converter.loads(msg, PeopleAttrs),
        n,
        lambda data: converter.structure(data, PeopleAttrs),
        no_gc=no_gc,
    )


#############################################################################
#  cattrs                                                                   #
#############################################################################


class AddressSchema(mm.Schema):
    street = mm.fields.String(required=True)
    state = mm.fields.String(required=True)
    zip = mm.fields.Integer(required=True)

    class Meta:
        json_module = orjson

    @mm.post_load
    def create(self, data, **kwargs):
        return AddressAttrs(**data)


class PersonSchema(mm.Schema):
    first = mm.fields.String(required=True)
    last = mm.fields.String(required=True)
    age = mm.fields.Integer(required=True)
    addresses = mm.fields.List(
        mm.fields.Nested(AddressSchema), allow_none=True, default=None
    )
    telephone = mm.fields.String(allow_none=True, default=None)
    email = mm.fields.String(allow_none=True, default=None)

    class Meta:
        json_module = orjson

    @mm.post_load
    def create(self, data, **kwargs):
        return PersonAttrs(**data)


class PeopleSchema(mm.Schema):
    people = mm.fields.List(mm.fields.Nested(PersonSchema))

    class Meta:
        json_module = orjson

    @mm.post_load
    def create(self, data, **kwargs):
        return PeopleAttrs(**data)


def bench_marshmallow(n, no_gc):
    def convert(data):
        people = []
        for p in data["people"]:
            addrs = p.pop("addresses", None)
            people.append(
                PersonAttrs(
                    addresses=[AddressAttrs(**a) for a in addrs] if addrs else None, **p
                )
            )
        return PeopleAttrs(people=people)

    schema = PeopleSchema()

    return bench(schema.dumps, schema.loads, n, convert, no_gc=no_gc)


BENCHMARKS = [
    ("msgspec", bench_msgspec),
    ("pydantic", bench_pydantic),
    ("cattrs", bench_cattrs),
    ("marshmallow", bench_marshmallow),
]


def run(n, no_gc=False, quiet=False):
    if quiet:

        def log(x):
            pass

    else:
        log = print

    title = f"Benchmark - {n} object{'s' if n > 1 else ''} with validation"
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

    parser = argparse.ArgumentParser(
        description="Benchmark different python validation libraries"
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

    results = run(1000, no_gc=args.no_gc, quiet=args.json)

    if args.json:
        print(json.dumps(results))


if __name__ == "__main__":
    main()
