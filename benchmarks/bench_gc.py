"""This file benchmarks GC collection time for a large number of tiny
dataclass-like instances.

For each type, the following is measured:

- Time for a single full GC pass over all the data.
- Amount of memory used to hold all the data
"""

import gc
import sys
import time

import msgspec


def sizeof(x, _seen=None):
    """Get the recursive sizeof for an object (memoized).

    Not generic, works on types used in this benchmark.
    """
    if _seen is None:
        _seen = set()

    _id = id(x)
    if _id in _seen:
        return 0

    _seen.add(_id)

    size = sys.getsizeof(x)

    if isinstance(x, dict):
        for k, v in x.items():
            size += sizeof(k, _seen)
            size += sizeof(v, _seen)
    if hasattr(x, "__dict__"):
        size += sizeof(x.__dict__, _seen)
    if hasattr(x, "__slots__"):
        for k in x.__slots__:
            size += sizeof(k, _seen)
            size += sizeof(getattr(x, k), _seen)
    return size


class Point(msgspec.Struct):
    x: int
    y: int
    z: int


class PointGCFalse(msgspec.Struct, gc=False):
    x: int
    y: int
    z: int


class PointClass:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z


class PointClassSlots:
    __slots__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z


def bench_gc(cls):
    # Allocate a dict of structs
    data = {i: cls(i, i, i) for i in range(1_000_000)}

    # Run a full collection
    start = time.perf_counter()
    gc.collect()
    stop = time.perf_counter()
    gc_time = (stop - start) * 1e3
    mibytes = sizeof(data) / (2**20)
    return gc_time, mibytes


def format_table(results):
    columns = ("", "GC time (ms)", "Memory Used (MiB)")

    rows = []
    for name, t, mem in results:
        rows.append((f"**{name}**", f"{t:.2f}", f"{mem:.2f}"))

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
    results = []
    for name, cls in [
        ("standard class", PointClass),
        ("standard class with __slots__", PointClassSlots),
        ("msgspec struct", Point),
        ("msgspec struct with gc=False", PointGCFalse),
    ]:
        print(f"Benchmarking {name}...")
        gc_time, mibytes = bench_gc(cls)
        results.append((name, gc_time, mibytes))

    print(format_table(results))


if __name__ == "__main__":
    main()
