import random
import string
import timeit
from typing import List, Optional

import gc
import msgpack
import msgspec
import orjson
import ormsgpack
import pydantic
import proto_bench


class Address(msgspec.Struct):
    street: str
    state: str
    zip: int


class Person(msgspec.Struct):
    first: str
    last: str
    age: int
    addresses: Optional[List[Address]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


class Address2(msgspec.Struct, asarray=True):
    street: str
    state: str
    zip: int


class Person2(msgspec.Struct, asarray=True):
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


def bench_msgspec(n, no_gc, validate=False):
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


def bench_msgspec_asarray(n, no_gc, validate=False):
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


def bench_ujson(n, no_gc, validate=False):
    import ujson

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


def bench_json(n, no_gc, validate=False):
    import json

    if validate:
        if n == 1:

            def loads(b):
                return PersonModel(**json.loads(b))

        else:

            def loads(b):
                return PeopleModel(items=json.loads(b)).items

    else:
        loads = json.loads

    return bench(json.dumps, loads, n, no_gc=no_gc)


def bench_ormsgpack(n, no_gc, validate=False):
    if validate:
        if n == 1:

            def loads(b):
                return PersonModel(**ormsgpack.unpackb(b))

        else:

            def loads(b):
                return PeopleModel(items=ormsgpack.unpackb(b)).items

    else:
        loads = ormsgpack.unpackb

    return bench(ormsgpack.packb, loads, n, no_gc=no_gc)


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
    ("json", bench_json),
    ("ujson", bench_ujson),
    ("orjson", bench_orjson),
    ("msgpack", bench_msgpack),
    ("ormsgpack", bench_ormsgpack),
    ("pyrobuf", bench_pyrobuf),
    ("msgspec.msgpack", bench_msgspec),
    ("msgspec.msgpack\nasarray", bench_msgspec_asarray),
    ("msgspec.json", bench_msgspec_json),
]


def format_time(n):
    if n >= 1:
        return "%.2f s" % n
    if n >= 1e-3:
        return "%.2f ms" % (n * 1e3)
    return "%.2f us" % (n * 1e6)


def format_bytes(n):
    if n >= 2 ** 30:
        return "%.1f GiB" % (n / (2 ** 30))
    elif n >= 2 ** 20:
        return "%.1f MiB" % (n / (2 ** 20))
    elif n >= 2 ** 10:
        return "%.1f KiB" % (n / (2 ** 10))
    return "%s B" % n


def preprocess_results(results):
    data = dict(zip(["benchmark", "encode", "decode"], map(list, zip(*results))))
    data["total"] = [d + l for d, l in zip(data["encode"], data["decode"])]

    max_time = max(data["total"])
    if max_time < 1e-6:
        time_unit = "ns"
        scale = 1e9
    elif max_time < 1e-3:
        time_unit = "us"
        scale = 1e6
    else:
        time_unit = "ms"
        scale = 1e3

    for k in ["encode", "decode", "total"]:
        data[f"{k}_labels"] = [format_time(t) for t in data[k]]
        data[k] = [scale * t for t in data[k]]

    return data, time_unit


def make_plot(results, title):
    import json
    import bokeh.plotting as bp
    from bokeh.transform import dodge
    from bokeh.layouts import column
    from bokeh.models import CustomJS, RadioGroup, FactorRange

    data, time_unit = preprocess_results(results)

    sort_options = ["total", "encode", "decode"]
    sort_orders = [
        list(zip(*sorted(zip(data[order], data["benchmark"]), reverse=True)))[1]
        for order in sort_options
    ]

    source = bp.ColumnDataSource(data=data)
    tooltips = [("time", "@$name")]

    x_range = FactorRange(*sort_orders[0])

    p = bp.figure(
        x_range=x_range,
        plot_height=250,
        plot_width=660,
        title=title,
        toolbar_location=None,
        tools="",
        tooltips=tooltips,
        sizing_mode="scale_width",
    )

    p.vbar(
        x=dodge("benchmark", -0.25, range=p.x_range),
        top="encode",
        width=0.2,
        source=source,
        color="#c9d9d3",
        legend_label="encode",
        name="encode_labels",
    )
    p.vbar(
        x=dodge("benchmark", 0.0, range=p.x_range),
        top="decode",
        width=0.2,
        source=source,
        color="#718dbf",
        legend_label="decode",
        name="decode_labels",
    )
    p.vbar(
        x=dodge("benchmark", 0.25, range=p.x_range),
        top="total",
        width=0.2,
        source=source,
        color="#e84d60",
        legend_label="total",
        name="total_labels",
    )

    p.x_range.range_padding = 0.1
    p.xgrid.grid_line_color = None
    p.ygrid.grid_line_color = None
    p.yaxis.axis_label = f"Time ({time_unit})"
    p.yaxis.minor_tick_line_color = None
    p.legend.location = "top_right"
    p.legend.orientation = "horizontal"

    # Setup widget
    select = RadioGroup(
        labels=sort_options, active=0, inline=True, css_classes=["centered-radio"]
    )
    callback = CustomJS(
        args=dict(x_range=x_range),
        code="""
        var lookup = {lookup_table};
        x_range.factors = lookup[this.active];
        x_range.change.emit();
        """.format(
            lookup_table=json.dumps(sort_orders)
        ),
    )
    select.js_on_click(callback)
    out = column(p, select, sizing_mode="scale_width")
    return out


def run(
    n,
    plot_title,
    plot_name,
    save_plot=False,
    save_json=False,
    no_gc=False,
    validate=False,
):
    results = []
    for name, func in BENCHMARKS:
        print(f"{name}")
        dumps_time, loads_time = func(n, no_gc, validate)
        total_time = dumps_time + loads_time
        print(f"  dumps: {dumps_time * 1e6:.2f} us")
        print(f"  loads: {loads_time * 1e6:.2f} us")
        print(f"  total: {total_time * 1e6:.2f} us")
        results.append((name, dumps_time, loads_time))
    if save_plot or save_json:
        import json
        from bokeh.resources import CDN
        from bokeh.embed import file_html, json_item

        plot = make_plot(results, plot_title)
        if save_plot:
            with open(f"{plot_name}.html", "w") as f:
                html = file_html(plot, CDN, "Benchmarks")
                f.write(html)
        if save_json:
            with open(f"{plot_name}.json", "w") as f:
                data = json.dumps(json_item(plot))
                f.write(data)


def run_1(save_plot=False, save_json=False, no_gc=False, validate=False):
    print(f"Benchmark - 1 object (validate={validate})")
    if validate:
        path = "bench-1-validate"
        title = "Benchmark - 1 object (with validation)"
    else:
        path = "bench-1"
        title = "Benchmark - 1 object"
    run(1, title, path, save_plot, save_json, no_gc, validate)


def run_1k(save_plot=False, save_json=False, no_gc=False, validate=False):
    print(f"Benchmark - 1k objects (validate={validate})")
    if validate:
        path = "bench-1k-validate"
        title = "Benchmark - 1000 objects (with validation)"
    else:
        path = "bench-1k"
        title = "Benchmark - 1000 objects"
    run(1000, title, path, save_plot, save_json, no_gc, validate)


def run_all(save_plot=False, save_json=False, no_gc=False, validate=False):
    for runner in [run_1, run_1k]:
        runner(save_plot, save_json, no_gc, validate)


benchmarks = {"all": run_all, "1": run_1, "1k": run_1k}


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Benchmark different python serializers"
    )
    parser.add_argument(
        "--benchmark",
        default="all",
        choices=list(benchmarks),
        help="which benchmark to run, defaults to 'all'",
    )
    parser.add_argument(
        "--plot",
        action="store_true",
        help="whether to plot the results",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="whether to output json representations of each plot",
    )
    parser.add_argument(
        "--no-gc",
        action="store_true",
        help="whether to disable the gc during benchmarking",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="whether to ensure all serializers include validation on deserialization",
    )
    args = parser.parse_args()
    benchmarks[args.benchmark](args.plot, args.json, args.no_gc, args.validate)


if __name__ == "__main__":
    main()
