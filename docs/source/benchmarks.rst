Benchmarks
==========

.. note::

    Benchmarks are *hard*.

    Repeatedly calling the same function in a tight loop will lead to the
    instruction cache staying hot and branches being highly predictable. That's
    not representative of real world access patterns. It's also hard to write a
    nonbiased benchmark. I wrote msgspec, naturally whatever benchmark I
    publish it's going to perform well in.

    Even so, people like to see benchmarks. I've tried to be as nonbiased as I
    can be, and the results hopefully indicate a few tradeoffs you make when
    you choose different serialization formats. I encourage you to write your
    own benchmarks before making these decisions.


.. _encoding-benchmark:


Benchmark - Encoding/Decoding
-----------------------------

Here we show a simple benchmark serializing some structured data. The data
we're serializing has the following schema (defined here using `msgspec.Struct`
types):

.. code-block:: python

    import msgspec
    from typing import List, Optional

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

The libraries we're benchmarking are the following:

- ``ujson`` - ujson_ with dict message types
- ``orjson`` - orjson_ with dict message types
- ``msgpack`` - msgpack_ with dict message types
- ``msgspec msgpack`` - msgspec_'s MessagePack encoding, with `msgspec.Struct`
  message types
- ``msgspec msgpack array-like`` - msgspec_'s MessagePack encoding, with
  `msgspec.Struct` message types configured with ``array_like=True``
- ``msgspec json`` - msgspec_'s JSON encoding, with `msgspec.Struct` message types
- ``msgspec json array-like`` - msgspec_'s JSON encoding, with `msgspec.Struct`
  message types configured with ``array_like=True``

Each benchmark creates one or more instances of a ``Person`` message, and
serializes it/deserializes it in a loop.

The full benchmark source can be found
`here <https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_encodings.py>`__.

1 Object
^^^^^^^^

Some workflows involve sending around very small messages. Here the overhead
per function call dominates (parsing of options, allocating temporary buffers,
etc...).

.. raw:: html

    <div id="bench-1"></div>

From the chart above, you can see that ``msgspec msgpack array-like`` is the
fastest method for both serialization and deserialization. This makes sense;
with ``array_like=True`` `msgspec.Struct` types don’t serialize the field names
in each message (things like ``"first"``, ``"last"``, …), only the values,
leading to smaller messages and higher performance.

The ``msgspec msgpack`` and ``msgspec json`` benchmarks also performed quite
well, encoding/decoding faster than all other options, even those implementing
the same serialization protocol. This is partly due to the use of `Struct`
types here - since all keys are statically known, the msgspec decoders can
apply a few optimizations not available to other Python libraries that rely on
`dict` types instead.

That said, all of these methods serialize/deserialize pretty quickly relative
to other python operations, so unless you're counting every microsecond your
choice here probably doesn't matter that much.

1000 Objects
^^^^^^^^^^^^

Here we serialize a list of 1000 ``Person`` objects. There's a lot more data
here, so the per-call overhead will no longer dominate, and we're now measuring
the efficiency of the encoding/decoding.

.. raw:: html

    <div id="bench-1k"></div>


Benchmark - Schema Validation
-----------------------------

The above benchmarks aren't 100% fair to ``msgspec``, as it also performs
schema validation on deserialization, checking that the message matches the
specified schema. None of the other options benchmarked support this natively.
Instead, many users perform validation post deserialization using additional
tools like pydantic_.

Here we benchmark the following validation libraries, measuring JSON encoding
and decoding time.

- msgspec_
- pydantic_
- cattrs_
- marshmallow_

The full benchmark source can be found
`here <https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_validation.py>`__.

.. raw:: html

    <div id="bench-1k-validate"></div>

This plot shows the performance benefit of performing type validation during
message decoding (as done by ``msgspec``) rather than as a secondary step with
a third-party library like pydantic_. In this benchmark ``msgspec`` is ~8x
faster than ``cattrs``, ~45x faster than ``pydantic``, and ~80x faster than
``marshmallow``.

Validating after decoding is slower for two reasons:

- It requires traversing over the entire output structure a second time (which
  can be slow due to pointer chasing)

- It may require converting some python objects to their desired output types
  (e.g. converting a decoded `dict` to a pydantic_ model), resulting in
  allocating many temporary python objects.

In contrast, libraries like ``msgspec`` that validate during decoding have none
of these issues. Only a single pass over the decoded data is taken, and the
specified output types are created correctly the first time, avoiding the need
for additional unnecessary allocations.

.. _memory-benchmark:

Benchmark - Memory Usage
------------------------

Here we benchmark loading a `medium-sized JSON file
<https://conda.anaconda.org/conda-forge/noarch/repodata.json>`__ (~65 MiB)
containing information on all the ``noarch`` packages in conda-forge_. We
compare the following libraries:

- msgspec_ with ``msgspec.Struct`` schemas pre-defined
- msgspec_
- json_
- ujson_
- orjson_
- simdjson_

For each library, we measure both the peak increase in memory usage (RSS) and
the time to JSON decode the file.

The full benchmark source can be found `here
<https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_memory.py>`__.

**Results (smaller is better):**

+---------------------+--------------+------+-----------+------+
|                     | memory (MiB) | vs.  | time (ms) | vs.  |
+=====================+==============+======+===========+======+
| **msgspec structs** | 83.6         | 1.0x | 170.6     | 1.0x |
+---------------------+--------------+------+-----------+------+
| **msgspec**         | 145.3        | 1.7x | 383.1     | 2.2x |
+---------------------+--------------+------+-----------+------+
| **json**            | 213.5        | 2.6x | 526.4     | 3.1x |
+---------------------+--------------+------+-----------+------+
| **ujson**           | 230.6        | 2.8x | 666.8     | 3.9x |
+---------------------+--------------+------+-----------+------+
| **orjson**          | 263.9        | 3.2x | 410.0     | 2.4x |
+---------------------+--------------+------+-----------+------+
| **simdjson**        | 403.7        | 4.8x | 615.1     | 3.6x |
+---------------------+--------------+------+-----------+------+

- ``msgspec`` decoding into :doc:`Struct <structs>` types uses the least amount of
  memory, and is also the fastest to decode. This makes sense; ``Struct`` types
  are cheaper to allocate and more memory efficient than ``dict`` types, and for
  large messages these differences can really add up.

- ``msgspec`` decoding without a schema is the second best option for both
  memory usage and speed. When decoding without a schema, ``msgspec`` makes the
  assumption that the underlying message probably still has some structure;
  short dict keys are temporarily cached to be reused later on, rather than
  reallocated every time. This means that instead of allocating 10,000 copies
  of the string ``"name"``, only a single copy is allocated and reused. For
  large messages this can lead to significant memory savings. ``json`` and
  ``orjson`` also use similar optimizations, but not as effectively.

- ``orjson`` and ``simdjson`` use 3-5x more memory than ``msgspec`` in this
  benchmark. In addition to the reasons above, both of these decoders require
  copying the original message into a temporary buffer. In this case, the extra
  copy adds an extra 65 MiB of overhead!


.. _struct-benchmark:

Benchmark - Structs
-------------------

Here we benchmark common `msgspec.Struct` operations, comparing their
performance against other similar libraries. The cases compared are:

- ``msgspec``
- Standard Python classes
- dataclasses_
- attrs_
- pydantic_

For each library, the following operations are benchmarked:

- Time to define a new class. Many libraries that abstract away class
  boilerplate add overhead when defining classes, slowing import times for
  libraries that make use of these classes.
- Time to create an instance of that class.
- Time to compare two instances for equality (``==``/``!=``).
- Time to compare two instances for order (``<``/``>``/``<=``/``>=``)

The full benchmark source can be found `here
<https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_structs.py>`__.

**Results (smaller is better):**

+----------------------+-------------+-------------+---------------+------------+
|                      | import (μs) | create (μs) | equality (μs) | order (μs) |
+======================+=============+=============+===============+============+
| **msgspec**          | 9.92        | 0.09        | 0.02          | 0.03       |
+----------------------+-------------+-------------+---------------+------------+
| **standard classes** | 6.86        | 0.45        | 0.13          | 0.29       |
+----------------------+-------------+-------------+---------------+------------+
| **dataclasses**      | 489.07      | 0.47        | 0.27          | 0.30       |
+----------------------+-------------+-------------+---------------+------------+
| **attrs**            | 428.38      | 0.42        | 0.29          | 2.15       |
+----------------------+-------------+-------------+---------------+------------+
| **pydantic**         | 371.52      | 4.84        | 10.56         | N/A        |
+----------------------+-------------+-------------+---------------+------------+

- Standard Python classes are the fastest to import (any library can only add
  overhead here). Still, ``msgspec`` isn't *that* much slower, especially
  compared to other options.
- Structs are optimized to be cheap to create, and that shows for the creation
  benchmark. They're roughly 5x faster than standard
  classes/``attrs``/``dataclasses``, and 50x faster than ``pydantic``.
- For equality comparison, msgspec Structs are roughly 6x to 500x faster than
  the alternatives.
- For order comparison, msgspec Structs are roughly 10x to 70x faster than the
  alternatives.

.. _struct-gc-benchmark:

Benchmark - Garbage Collection
------------------------------

`msgspec.Struct` instances implement several optimizations for reducing garbage
collection (GC) pressure and decreasing memory usage. Here we benchmark structs
(with and without :ref:`gc=False <struct-gc>`) against standard Python
classes (with and without `__slots__
<https://docs.python.org/3/reference/datamodel.html#slots>`__).

For each option we create a large dictionary containing many simple instances
of the benchmarked type, then measure:

- The amount of time it takes to do a full garbage collection (gc) pass
- The total amount of memory used by this data structure

The full benchmark source can be found `here
<https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_gc.py>`__.

**Results (smaller is better):**

+-----------------------------------+--------------+-------------------+
|                                   | GC time (ms) | Memory Used (MiB) |
+===================================+==============+===================+
| **standard class**                | 80.46        | 211.66            |
+-----------------------------------+--------------+-------------------+
| **standard class with __slots__** | 80.06        | 120.11            |
+-----------------------------------+--------------+-------------------+
| **msgspec struct**                | 13.96        | 120.11            |
+-----------------------------------+--------------+-------------------+
| **msgspec struct with gc=False**  | 1.07         | 104.85            |
+-----------------------------------+--------------+-------------------+

- Standard Python classes are the most memory hungry (since all data is stored
  in an instance dict). They also result in the largest GC pause, as the GC has
  to traverse the entire outer dict, each class instance, and each instance
  dict. All that pointer chasing has a cost.

- Standard classes with ``__slots__`` are less memory hungry, but still results
  in an equivalent GC pauses.

- `msgspec.Struct` instances have the same memory layout as a class with
  ``__slots__`` (and thus have the same memory usage), but due to deferred GC
  tracking a full GC pass completes in a fraction of the time.

- `msgspec.Struct` instances with ``gc=False`` have the lowest memory usage
  (lack of GC reduces memory by 16 bytes per instance). They also have the
  lowest GC pause (75x faster than standard classes!) since the entire
  composing dict can be skipped during GC traversal.


.. _benchmark-library-size:

Benchmark - Library Size
------------------------

Here we compare the on-disk size of a few Python libraries.

The full benchmark source can be found `here
<https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_library_size.py>`__.

**Results (smaller is better)**

+--------------+---------+------------+-------------+
|              | version | size (MiB) | vs. msgspec |
+==============+=========+============+=============+
| **msgspec**  | 0.12.0  | 0.34       | 1.00x       |
+--------------+---------+------------+-------------+
| **orjson**   | 3.8.5   | 0.56       | 1.64x       |
+--------------+---------+------------+-------------+
| **msgpack**  | 1.0.4   | 0.99       | 2.91x       |
+--------------+---------+------------+-------------+
| **pydantic** | 1.10.4  | 8.71       | 25.67x      |
+--------------+---------+------------+-------------+

The functionality available in ``msgspec`` is comparable to that of orjson_,
msgpack_, and pydantic_ combined. However, the total installed binary size of
``msgspec`` is a fraction of that of any of these libraries.

.. raw:: html

    <script src="https://cdn.jsdelivr.net/npm/vega@5.22.1"></script>
    <script src="https://cdn.jsdelivr.net/npm/vega-lite@5.5.0"></script>
    <script src="https://cdn.jsdelivr.net/npm/vega-embed@6.21.0"></script>

.. raw:: html

    <script type="text/javascript">

    function buildPlot(div, rows, title) {
        var i, time_unit, scale, max_time = 0;
        for (i = 0; i < rows.length; i++) {
            var total = rows[i][1] + rows[i][2];
            if (total > max_time) {
                max_time = total;
            }
        }
        if (max_time < 1e-6) {
            time_unit = "ns";
            scale = 1e9;
        }
        else if (max_time < 1e-3) {
            time_unit = "μs";
            scale = 1e6;
        }
        else {
            time_unit = "ms";
            scale = 1e3;
        }

        var columns = ["encode", "decode", "total"];
        var data = [];
        for (i = 0; i < rows.length; i++) {
            var lib = rows[i][0];
            var et = rows[i][1] * scale;
            var dt = rows[i][2] * scale;
            var tt = et + dt;
            data.push({library: lib, method: "encode", time: et});
            data.push({library: lib, method: "decode", time: dt});
            data.push({library: lib, method: "total", time: tt});
        }

        var spec = {
            "$schema": "https://vega.github.io/schema/vega-lite/v5.2.0.json",
            "title": title,
            "config": {
                "view": {"continuousHeight": 250, "stroke": null},
                "legend": {"title": null},
            },
            "data": {"values": data},
            "transform": [
                {
                    "calculate": `join([format(datum.time, '.3'), ' ${time_unit}'], '')`,
                    "as": "tooltip",
                }
            ],
            "mark": "bar",
            "encoding": {
                "color": {
                    "field": "method",
                    "type": "nominal",
                    "scale": {"scheme": "tableau20"},
                    "sort": columns,
                },
                "column": {
                    "field": "library",
                    "header": {"labelExpr": "split(datum.label, ' ')", "orient": "bottom"},
                    "sort": {"field": "time", "op": "sum", "order": "descending"},
                    "title": null,
                    "type": "nominal",
                },
                "tooltip": {"field": "tooltip", "type": "nominal"},
                "x": {
                    "axis": {"labels": false, "ticks": false, "title": null},
                    "field": "method",
                    "type": "nominal",
                    "sort": columns,
                },
                "y": {
                    "axis": {"grid": false, "title": `Time (${time_unit})`},
                    "field": "time",
                    "type": "quantitative",
                },
            },
        };
        vegaEmbed(div, spec);
    }

    var results = {"1": [["ujson", 6.717021639924496e-07, 7.829359059687704e-07], ["orjson", 2.631088870111853e-07, 4.62388165993616e-07], ["msgpack", 3.223358949762769e-07, 6.897511919960379e-07], ["msgspec msgpack", 1.1219781800173223e-07, 2.1338467899477108e-07], ["msgspec msgpack array-like", 8.444309020414948e-08, 1.779988644993864e-07], ["msgspec json", 1.4419139400706626e-07, 2.535316209832672e-07], ["msgspec json array-like", 1.1690347650437616e-07, 1.9026524299988523e-07]], "1k": [["ujson", 0.001032715715118684, 0.0015374938599416056], ["orjson", 0.00036241704699932595, 0.000918797859980259], ["msgpack", 0.0006078476320253685, 0.0012546482899051625], ["msgspec msgpack", 0.00017605937899497804, 0.0005109944079886191], ["msgspec msgpack array-like", 0.00013544270300189964, 0.0004263007240369916], ["msgspec json", 0.0002518398549873382, 0.0005371352119836957], ["msgspec json array-like", 0.00022411236297921277, 0.00042209100601030514]]}
    var results_valid = [["msgspec", 0.00024952584999846295, 0.0005602700359886513], ["pydantic", 0.018896686400694307, 0.016377520850801373], ["cattrs", 0.002034737430221867, 0.004708789379801601], ["marshmallow", 0.01409857090038713, 0.052351984195411205]]
    buildPlot('#bench-1', results["1"], "Benchmark - 1 Object");
    buildPlot('#bench-1k', results["1k"], "Benchmark - 1000 Objects");
    buildPlot('#bench-1k-validate', results_valid, "Benchmark - 1000 Objects, With Validation");
    </script>


.. _msgspec: https://jcristharif.com/msgspec/
.. _msgpack: https://github.com/msgpack/msgpack-python
.. _orjson: https://github.com/ijl/orjson
.. _json: https://docs.python.org/3/library/json.html
.. _simdjson: https://github.com/TkTech/pysimdjson
.. _ujson: https://github.com/ultrajson/ultrajson
.. _attrs: https://www.attrs.org
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _marshmallow: https://marshmallow.readthedocs.io/en/stable/index.html
.. _cattrs: https://catt.rs/en/latest/
.. _conda-forge: https://conda-forge.org/
