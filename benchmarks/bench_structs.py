"""This file benchmarks dataclass-like libraries. It measures the following
operations:

- Time to import a new class definition
- Time to create an instance of that class
- Time to compare an instance of that class with another instance.
"""

from time import perf_counter


order_template = """
    def __{method}__(self, other):
        if type(self) is not type(other):
            return NotImplemented
        return (
            (self.a, self.b, self.c, self.d, self.e) {op}
            (other.a, other.b, other.c, other.d, other.e)
        )
"""


classes_template = """
import reprlib

class C{n}:
    def __init__(self, a, b, c, d, e):
        self.a = a
        self.b = b
        self.c = c
        self.d = d
        self.e = e

    @reprlib.recursive_repr()
    def __repr__(self):
        return (
            f"{{type(self).__name__}}(a={{self.a!r}}, b={{self.b!r}}, "
            f"c={{self.c!r}}, d={{self.d!r}}, e={{self.e!r}})"
        )

    def __eq__(self, other):
        if type(self) is not type(other):
            return NotImplemented
        return (
            self.a == other.a and
            self.b == other.b and
            self.c == other.c and
            self.d == other.d and
            self.e == other.e
        )
""" + "".join(
    [
        order_template.format(method="lt", op="<"),
        order_template.format(method="le", op="<="),
        order_template.format(method="gt", op=">"),
        order_template.format(method="ge", op=">="),
    ]
)

attrs_template = """
from attr import define

@define(order=True)
class C{n}:
    a: int
    b: int
    c: int
    d: int
    e: int
"""

dataclasses_template = """
from dataclasses import dataclass

@dataclass(order=True)
class C{n}:
    a: int
    b: int
    c: int
    d: int
    e: int
"""

pydantic_template = """
from pydantic import BaseModel

class C{n}(BaseModel):
    a: int
    b: int
    c: int
    d: int
    e: int
"""

msgspec_template = """
from msgspec import Struct

class C{n}(Struct, order=True):
    a: int
    b: int
    c: int
    d: int
    e: int
"""

sources = {
    "standard classes": classes_template,
    "attrs": attrs_template,
    "dataclasses": dataclasses_template,
    "pydantic": pydantic_template,
    "msgspec": msgspec_template,
}


def bench(name, template):
    print(f"Benchmarking {name}:")

    N_classes = 100

    source = "\n".join(template.format(n=i) for i in range(N_classes))
    code_obj = compile(source, "__main__", "exec")

    # Benchmark definining new types
    N = 200
    start = perf_counter()
    for _ in range(N):
        ns = {}
        exec(code_obj, ns)
    end = perf_counter()
    define_time = ((end - start) / (N * N_classes)) * 1e6
    print(f"- define: {define_time:.2f} μs")

    C = ns["C0"]

    # Benchmark creating new instances
    N = 1000
    M = 1000
    start = perf_counter()
    for _ in range(N):
        [C(a=i, b=i, c=i, d=i, e=i) for i in range(M)]
    end = perf_counter()
    init_time = ((end - start) / (N * M)) * 1e6
    print(f"- init: {init_time:.2f} μs")

    # Benchmark equality
    N = 1000
    M = 1000
    val = M - 1
    needle = C(a=val, b=val, c=val, d=val, e=val)
    haystack = [C(a=i, b=i, c=i, d=i, e=i) for i in range(M)]
    start = perf_counter()
    for _ in range(N):
        haystack.index(needle)
    end = perf_counter()
    equality_time = ((end - start) / (N * M)) * 1e6
    print(f"- equality: {equality_time:.2f} μs")

    # Benchmark order
    try:
        needle < needle
    except TypeError:
        order_time = None
        print("- order: N/A")
    else:
        start = perf_counter()
        for _ in range(N):
            for obj in haystack:
                if obj >= needle:
                    break
        end = perf_counter()
        order_time = ((end - start) / (N * M)) * 1e6
        print(f"- order: {order_time:.2f} μs")

    return (name, define_time, init_time, equality_time, order_time)


def format_table(results):
    columns = (
        "",
        "import (μs)",
        "create (μs)",
        "equality (μs)",
        "order (μs)",
    )

    def f(n):
        return "N/A" if n is None else f"{n:.2f}"

    rows = []
    for name, *times in results:
        rows.append((f"**{name}**", *(f(t) for t in times)))

    widths = tuple(max(max(map(len, x)), len(c)) for x, c in zip(zip(*rows), columns))
    row_template = ("|" + (" %%-%ds |" * len(columns))) % widths
    header = row_template % tuple(columns)
    bar_underline = "+%s+" % "+".join("=" * (w + 2) for w in widths)
    bar = "+%s+" % "+".join("-" * (w + 2) for w in widths)
    parts = [bar, header, bar_underline]
    for r in rows:
        parts.append(row_template % r)
        parts.append(bar)
    return "\n".join(parts)


def main():
    import argparse

    parser = argparse.ArgumentParser(description="Benchmark msgspec Struct operations")
    parser.add_argument(
        "--output-table",
        action="store_true",
        help="whether to output a ReST table at the end",
    )
    args = parser.parse_args()

    results = []
    for name, source in sources.items():
        results.append(bench(name, source))

    if args.output_table:
        print(format_table(results))


if __name__ == "__main__":
    main()
