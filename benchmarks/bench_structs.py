"""This file benchmarks dataclass-like libraries. It measures the following
operations:

- Time to import a new class definition
- Time to create an instance of that class
- Time to compare an instance of that class with another instance.
"""

from time import perf_counter

classes_template = """
class C{n}:
    def __init__(self, a, b, c, d, e):
        self.a = a
        self.b = b
        self.c = c
        self.d = d
        self.e = e

    def __eq__(self, other):
        return (
            type(self) is type(other) and
            self.a == other.a and
            self.b == other.b and
            self.c == other.c and
            self.d == other.d and
            self.e == other.e
        )
"""

attrs_template = """
from attr import define

@define
class C{n}:
    a: int
    b: int
    c: int
    d: int
    e: int
"""

dataclasses_template = """
from dataclasses import dataclass

@dataclass
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

class C{n}(Struct):
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

    # Benchmark comparison
    N = 1000
    M = 1000
    val = M - 1
    needle = C(a=val, b=val, c=val, d=val, e=val)
    haystack = [C(a=i, b=i, c=i, d=i, e=i) for i in range(M)]
    start = perf_counter()
    for _ in range(N):
        haystack.index(needle)
    end = perf_counter()
    compare_time = ((end - start) / (N * M)) * 1e6
    print(f"- compare: {compare_time:.2f} μs")

    return (name, define_time, init_time, compare_time)


def format_table(results):
    columns = ("", "import (μs)", "create (μs)", "compare (μs)")

    rows = []
    for name, t1, t2, t3 in results:
        rows.append(
            (
                f"**{name}**",
                f"{t1:.2f}",
                f"{t2:.2f}",
                f"{t3:.2f}",
            )
        )

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
