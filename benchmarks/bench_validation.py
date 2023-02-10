import dataclasses
import datetime
import gc
import json
import random
import string
import timeit
from typing import List, Optional

import attrs
import cattrs.preconf.orjson
import orjson
import pydantic
from mashumaro.mixins.orjson import DataClassORJSONMixin

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
        "first": randstr(rand, 3, 15),
        "last": randstr(rand, 3, 15),
        "created": created.isoformat(),
        "updated": updated.isoformat(),
        "birthday": birthday.isoformat(),
        "addresses": addresses if addresses else None,
        "telephone": randstr(rand, 9) if has_phone else None,
        "email": randstr(rand, 15, 30) if has_email else None,
    }


def make_users(n, seed=42):
    rand = random.Random(seed)
    return {"users": [make_user(rand) for _ in range(n)]}


def bench(dumps, loads, ndata, convert=None, no_gc=False):
    setup = "" if no_gc else "gc.enable()"
    data = make_users(ndata)
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


class UserStruct(msgspec.Struct, gc=False):
    created: datetime.datetime
    updated: datetime.datetime
    first: str
    last: str
    birthday: datetime.date
    addresses: Optional[List[AddressStruct]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


class UsersStruct(msgspec.Struct, gc=False):
    users: List[UserStruct]


def bench_msgspec(n, no_gc):
    enc = msgspec.json.Encoder()
    dec = msgspec.json.Decoder(UsersStruct)

    def convert(data):
        return msgspec.from_builtins(data, UsersStruct)

    return bench(enc.encode, dec.decode, n, convert, no_gc=no_gc)


#############################################################################
#  pydantic                                                                 #
#############################################################################


def orjson_dumps(x, **kwargs):
    return orjson.dumps(x, **kwargs).decode()


class BaseModel(pydantic.BaseModel):
    class Config:
        json_dumps = orjson_dumps
        json_loads = orjson.loads


class AddressModel(BaseModel):
    street: str
    state: str
    zip: int


class UserModel(BaseModel):
    created: datetime.datetime
    updated: datetime.datetime
    first: str
    last: str
    birthday: datetime.date
    addresses: Optional[List[AddressModel]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


class UsersModel(BaseModel):
    users: List[UserModel]


def bench_pydantic(n, no_gc):
    return bench(
        lambda p: p.json(),
        UsersModel.parse_raw,
        n,
        lambda data: UsersModel(**data),
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
class UserAttrs:
    created: datetime.datetime
    updated: datetime.datetime
    first: str
    last: str
    birthday: datetime.date
    addresses: Optional[List[AddressAttrs]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


@attrs.define
class UsersAttrs:
    users: List[UserAttrs]


def bench_cattrs(n, no_gc):
    converter = cattrs.preconf.orjson.make_converter()
    converter.register_unstructure_hook(datetime.date, str)
    converter.register_structure_hook(
        datetime.date, lambda s, _: datetime.date.fromisoformat(s)
    )
    return bench(
        converter.dumps,
        lambda msg: converter.loads(msg, UsersAttrs),
        n,
        lambda data: converter.structure(data, UsersAttrs),
        no_gc=no_gc,
    )


#############################################################################
#  mashumaro                                                                #
#############################################################################


@dataclasses.dataclass
class AddressMashumaro(DataClassORJSONMixin):
    street: str
    state: str
    zip: int


@dataclasses.dataclass
class UserMashumaro(DataClassORJSONMixin):
    created: datetime.datetime
    updated: datetime.datetime
    first: str
    last: str
    birthday: datetime.date
    addresses: Optional[List[AddressMashumaro]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


@dataclasses.dataclass
class UsersMashumaro(DataClassORJSONMixin):
    users: List[UserMashumaro]


def bench_mashumaro(n, no_gc):
    return bench(
        lambda x: x.to_jsonb(),
        UsersMashumaro.from_json,
        n,
        UsersMashumaro.from_dict,
        no_gc=no_gc,
    )


BENCHMARKS = [
    ("msgspec", bench_msgspec),
    ("pydantic", bench_pydantic),
    ("cattrs", bench_cattrs),
    ("mashumaro", bench_mashumaro),
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
