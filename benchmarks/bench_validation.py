from __future__ import annotations

import dataclasses
import datetime
import gc
import timeit
from typing import Annotated, List, Literal, Union

import attrs
import cattrs.preconf.orjson
import pydantic
from generate_data import make_filesystem_data
from mashumaro.mixins.orjson import DataClassORJSONMixin

import msgspec

#############################################################################
#  msgspec                                                                  #
#############################################################################


class File(msgspec.Struct, tag="file"):
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_at: datetime.datetime
    nbytes: int


class Directory(msgspec.Struct, tag="directory"):
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_at: datetime.datetime
    contents: List[Union[File, Directory]]


def bench_msgspec(n):
    enc = msgspec.json.Encoder()
    dec = msgspec.json.Decoder(Directory)

    def convert(data):
        return msgspec.convert(data, Directory)

    return bench(enc.encode, dec.decode, n, convert)


#############################################################################
#  pydantic                                                                 #
#############################################################################


class FileModel(pydantic.BaseModel):
    type: Literal["file"] = "file"
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_at: datetime.datetime
    nbytes: int


class DirectoryModel(pydantic.BaseModel):
    type: Literal["directory"] = "directory"
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_at: datetime.datetime
    contents: List[
        Annotated[
            Union[FileModel, DirectoryModel], pydantic.Field(discriminator="type")
        ]
    ]


def bench_pydantic(n):
    return bench(
        lambda p: p.model_dump_json(),
        DirectoryModel.model_validate_json,
        n,
        lambda data: DirectoryModel(**data),
    )


#############################################################################
#  cattrs                                                                   #
#############################################################################


@attrs.define
class FileAttrs:
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_at: datetime.datetime
    nbytes: int
    type: Literal["file"] = "file"


@attrs.define
class DirectoryAttrs:
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_at: datetime.datetime
    contents: List[Union[FileAttrs, DirectoryAttrs]]
    type: Literal["directory"] = "directory"


def bench_cattrs(n):
    converter = cattrs.preconf.orjson.make_converter()
    return bench(
        converter.dumps,
        lambda msg: converter.loads(msg, DirectoryAttrs),
        n,
        lambda data: converter.structure(data, DirectoryAttrs),
    )


#############################################################################
#  mashumaro                                                                #
#############################################################################


@dataclasses.dataclass
class FileMashumaro(DataClassORJSONMixin):
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_at: datetime.datetime
    nbytes: int
    type: Literal["file"] = "file"

    class Config:
        lazy_compilation = True


@dataclasses.dataclass
class DirectoryMashumaro(DataClassORJSONMixin):
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_at: datetime.datetime
    contents: List[Union[FileMashumaro, DirectoryMashumaro]]
    type: Literal["directory"] = "directory"

    class Config:
        lazy_compilation = True


def bench_mashumaro(n):
    return bench(
        lambda x: x.to_json(),
        lambda x: DirectoryMashumaro.from_json(x),
        n,
        lambda x: DirectoryMashumaro.from_dict(x),
    )


#############################################################################
#  common                                                                   #
#############################################################################

BENCHMARKS = [
    ("msgspec", bench_msgspec),
    ("pydantic", bench_pydantic),
    ("cattrs", bench_cattrs),
    ("mashumaro", bench_mashumaro),
]


def bench(dumps, loads, ndata, convert):
    raw_data = make_filesystem_data(ndata)
    msg = convert(raw_data)
    json_data = dumps(msg)
    msg2 = loads(json_data)
    assert msg == msg2
    del msg2

    gc.collect()
    timer = timeit.Timer("func(data)", setup="", globals={"func": dumps, "data": msg})
    n, t = timer.autorange()
    dumps_time = t / n

    gc.collect()
    timer = timeit.Timer(
        "func(data)", setup="", globals={"func": loads, "data": json_data}
    )
    n, t = timer.autorange()
    loads_time = t / n
    return dumps_time, loads_time


def run(n, quiet=False):
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
        dumps_time, loads_time = func(n)
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
    args = parser.parse_args()

    results = run(1000, quiet=args.json)

    if args.json:
        import json

        print(json.dumps(results))


if __name__ == "__main__":
    main()
