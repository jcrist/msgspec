Benchmarks
==========

.. note::
   These benchmarks are for ``msgspec-x``, a community-driven fork of the original msgspec project.

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

    In all cases benchmarks were run on my local development machine (a ~2020
    x86 Linux laptop) using CPython 3.11.


JSON Serialization & Validation
-------------------------------

This benchmark covers the common case when working with ``msgspec`` or other
validation libraries. It measures two things:

- Decoding some JSON input, validating it against a schema, and converting it
  into user-friendly python objects.
- Encoding these same python objects back into JSON.

The data we're working with has the following schema (defined here using
`msgspec.Struct` types):

.. code-block:: python

    import enum
    import datetime
    import msgspec

    class Permissions(enum.Enum):
        READ = "READ"
        WRITE = "WRITE"
        READ_WRITE = "READ_WRITE"


    class File(msgspec.Struct, kw_only=True, tag="file"):
        name: str
        created_by: str
        created_at: datetime.datetime
        updated_by: str | None = None
        updated_at: datetime.datetime | None = None
        nbytes: int
        permissions: Permissions


    class Directory(msgspec.Struct, kw_only=True, tag="directory"):
        name: str
        created_by: str
        created_at: datetime.datetime
        updated_by: str | None = None
        updated_at: datetime.datetime | None = None
        contents: list[File | Directory]


The libraries we're comparing are the following:

- msgspec_ (0.18.5)
- mashumaro_ (3.11)
- pydantic_ (both 1.10.13 and 2.5.2)
- cattrs_ (23.2.3)

Each benchmark creates a message containing one or more ``File``/``Directory``
instances, then then serializes, deserializes, and validates it in a loop.

The full benchmark source can be found
`here <https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_validation>`__.

.. raw:: html

    <div id="bench-validate" style="width:75%"></div>

In this benchmark ``msgspec`` is ~6x faster than ``mashumaro``, ~10x faster
than ``cattrs``, and ~12x faster than ``pydantic`` V2, and ~85x faster than
``pydantic`` V1.

This plot shows the performance benefit of performing type validation during
message decoding (as done by ``msgspec``) rather than as a secondary step with
a third-party library like cattrs_ or pydantic_ V1. Validating after decoding
is slower for two reasons:

- It requires traversing over the entire output structure a second time (which
  can be slow due to pointer chasing)

- It may require converting some python objects to their desired output types
  (e.g. converting a decoded `dict` to a pydantic_ model), resulting in
  allocating many temporary python objects.

In contrast, libraries like ``msgspec`` that validate during decoding have none
of these issues. Only a single pass over the decoded data is taken, and the
specified output types are created correctly the first time, avoiding the need
for additional unnecessary allocations.

This benefit also shows up in the memory usage for the same benchmark:

.. raw:: html

    <div id="bench-validate-memory" style="width:75%"></div>

Here we compare the peak increase in memory usage (RSS) after loading the
schemas and data. ``msgspec``'s small library size, schema representation, and
in-memory state means it uses a fraction of the memory of other tools.

.. _json-benchmark:

JSON Serialization
------------------

``msgspec`` includes its own high performance JSON library, which may be used
by itself as a replacement for the standard library's `json.dumps`/`json.loads`
functions. Here we compare msgspec's JSON implementation against several other
popular Python JSON libraries.

- msgspec_ (0.18.5)
- orjson_ (3.9.10)
- ujson_ (5.9.0)
- rapidjson_ (1.13)
- simdjson_ (5.0.2)
- json_ (standard library)

The full benchmark source can be found
`here <https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_encodings.py>`__.

.. raw:: html

    <div id="bench-json" style="width:75%"></div>

In this case ``msgspec structs`` (which measures ``msgspec`` with
``msgspec.Struct`` schemas pre-defined) is the fastest. When used without
schemas, ``msgspec`` is on-par with ``orjson`` (the next fastest JSON library).

This shows that ``msgspec`` is able to decode JSON faster when a schema is
provided. Due to a more efficient in memory representation, JSON decoding AND
schema validation with ``msgspec`` than just JSON decoding alone.

.. _msgpack-benchmark:

MessagePack Serialization
-------------------------

Likewise, ``msgspec`` includes its own high performance MessagePack_ library,
which may be used by itself without requiring usage of any of msgspec's
validation machinery. Here we compare msgspec's MessagePack implementation
against several other popular Python MessagePack libraries.

- msgspec_ (0.18.5)
- msgpack_ (1.0.7)
- ormsgpack_ (1.4.1)

.. raw:: html

    <div id="bench-msgpack" style="width:75%"></div>

As with the JSON benchmark above, ``msgspec`` with a schema provided (``msgspec
structs``) is faster than ``msgspec`` with no schema. In both cases though
``msgspec`` is measurably faster than other Python MessagePack libraries like
``msgpack`` or ``ormsgpack``.


JSON Serialization - Large Data
-------------------------------

Here we benchmark loading a `large JSON file
<https://conda.anaconda.org/conda-forge/noarch/repodata.json>`__ (~77 MiB)
containing information on all the ``noarch`` packages in conda-forge_. We
compare the following libraries:

- msgspec_ (0.18.5)
- orjson_ (3.9.10)
- ujson_ (5.9.0)
- rapidjson_ (1.13)
- simdjson_ (5.0.2)
- json_ (standard library)

For each library, we measure both the peak increase in memory usage (RSS) and
the time to JSON decode the file.

The full benchmark source can be found `here
<https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_large_json.py>`__.

**Results (smaller is better):**

+---------------------+--------------+------+-----------+------+
|                     | memory (MiB) | vs.  | time (ms) | vs.  |
+=====================+==============+======+===========+======+
| **msgspec structs** | 67.6         | 1.0x | 176.8     | 1.0x |
+---------------------+--------------+------+-----------+------+
| **msgspec**         | 218.3        | 3.2x | 630.5     | 3.6x |
+---------------------+--------------+------+-----------+------+
| **json**            | 295.0        | 4.4x | 868.6     | 4.9x |
+---------------------+--------------+------+-----------+------+
| **ujson**           | 349.1        | 5.2x | 1087.0    | 6.1x |
+---------------------+--------------+------+-----------+------+
| **rapidjson**       | 375.0        | 5.6x | 1004.0    | 5.7x |
+---------------------+--------------+------+-----------+------+
| **orjson**          | 406.3        | 6.0x | 691.7     | 3.9x |
+---------------------+--------------+------+-----------+------+
| **simdjson**        | 603.2        | 8.9x | 1053.0    | 6.0x |
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

- ``orjson`` and ``simdjson`` use 6-9x more memory than ``msgspec`` in this
  benchmark. In addition to the reasons above, both of these decoders require
  copying the original message into a temporary buffer. In this case, the extra
  copy adds an extra 77 MiB of overhead!

.. _struct-benchmark:

Structs
-------

Here we benchmark common `msgspec.Struct` operations, comparing their
performance against other similar libraries. The cases compared are:

- Standard Python classes
- dataclasses_
- msgspec_ (0.18.5)
- attrs_ (23.1.0)
- pydantic_ (2.5.2)

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
| **msgspec**          | 12.51       | 0.09        | 0.02          | 0.03       |
+----------------------+-------------+-------------+---------------+------------+
| **standard classes** | 7.88        | 0.35        | 0.08          | 0.16       |
+----------------------+-------------+-------------+---------------+------------+
| **attrs**            | 483.10      | 0.37        | 0.14          | 1.87       |
+----------------------+-------------+-------------+---------------+------------+
| **dataclasses**      | 506.09      | 0.36        | 0.14          | 0.16       |
+----------------------+-------------+-------------+---------------+------------+
| **pydantic**         | 673.47      | 1.54        | 0.60          | N/A        |
+----------------------+-------------+-------------+---------------+------------+

- Standard Python classes are the fastest to import (any library can only add
  overhead here). Still, ``msgspec`` isn't *that* much slower, especially
  compared to other options.
- Structs are optimized to be cheap to create, and that shows for the creation
  benchmark. They're roughly 4x faster than standard
  classes/``attrs``/``dataclasses``, and 17x faster than ``pydantic``.
- For equality comparison, msgspec Structs are roughly 4x to 30x faster than
  the alternatives.
- For order comparison, msgspec Structs are roughly 5x to 60x faster than the
  alternatives.

.. _struct-gc-benchmark:

Garbage Collection
------------------

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

Library Size
------------

Here we compare the on-disk size of ``msgspec`` and ``pydantic``, its closest
equivalent.

The full benchmark source can be found `here
<https://github.com/jcrist/msgspec/tree/main/benchmarks/bench_library_size.py>`__.

**Results (smaller is better)**

+--------------+---------+------------+-------------+
|              | version | size (MiB) | vs. msgspec |
+==============+=========+============+=============+
| **msgspec**  | 0.18.4  | 0.46       | 1.00x       |
+--------------+---------+------------+-------------+
| **pydantic** | 2.5.2   | 6.71       | 14.66x      |
+--------------+---------+------------+-------------+

For applications where dependency size matters, ``msgspec`` is roughly 15x
smaller on disk.

.. raw:: html

    <script src="https://cdn.jsdelivr.net/npm/vega@5.22.1"></script>
    <script src="https://cdn.jsdelivr.net/npm/vega-lite@5.5.0"></script>
    <script src="https://cdn.jsdelivr.net/npm/vega-embed@6.21.0"></script>

.. raw:: html

    <script type="text/javascript">

    function buildPlot(div, rows, title) {
        var i, time_unit, scale, max_time = 0;
        for (i = 0; i < rows.length; i++) {
            var total = rows[i].encode + rows[i].decode;
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
            var label = rows[i].label;
            var et = rows[i].encode * scale;
            var dt = rows[i].decode * scale;
            var tt = et + dt;
            data.push({library: label, method: "encode", time: et});
            data.push({library: label, method: "decode", time: dt});
            data.push({library: label, method: "total", time: tt});
        }

        var spec = {
            "$schema": "https://vega.github.io/schema/vega-lite/v5.2.0.json",
            "title": title,
            "config": {
                "view": {"stroke": null},
                "legend": {"title": null, "labelFontSize": 12},
                "title": {"fontSize": 14, "offset": 10},
                "axis": {"titleFontSize": 12, "titlePadding": 10}
            },
            "width": "container",
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
                "row": {
                    "field": "library",
                    "header": {
                        "orient": "left",
                        "labelAngle": 0,
                        "labelAlign": "left",
                        "labelFontSize": 12
                    },
                    "sort": {"field": "time", "op": "sum", "order": "ascending"},
                    "title": null,
                    "type": "nominal",
                },
                "tooltip": {"field": "tooltip", "type": "nominal"},
                "x": {
                    "axis": {"grid": false, "title": `Time (${time_unit})`},
                    "field": "time",
                    "type": "quantitative",
                },
                "y": {
                    "axis": {"labels": false, "ticks": false, "title": null},
                    "field": "method",
                    "type": "nominal",
                    "sort": columns,
                },
            },
        };
        vegaEmbed(div, spec);
    }

    function buildMemPlot(div, rows, title) {
        var data = [];
        for (i = 0; i < rows.length; i++) {
            data.push({library: rows[i].label, memory: rows[i].memory});
        }

        var spec = {
            "$schema": "https://vega.github.io/schema/vega-lite/v5.2.0.json",
            "title": title,
            "config": {
                "view": {"stroke": null},
                "legend": {"title": null, "labelFontSize": 12},
                "title": {"fontSize": 14, "offset": 10},
                "axis": {"titleFontSize": 12, "titlePadding": 10}
            },
            "width": "container",
            "data": {"values": data},
            "transform": [
                {
                    "calculate": "join([format(datum.memory, '.3'), ' MiB'], '')",
                    "as": "tooltip",
                }
            ],
            "mark": "bar",
            "encoding": {
                "row": {
                    "field": "library",
                    "header": {
                        "orient": "left",
                        "labelAngle": 0,
                        "labelAlign": "left",
                        "labelFontSize": 12
                    },
                    "sort": {"field": "memory", "order": "ascending"},
                    "title": null,
                    "type": "nominal",
                },
                "tooltip": {"field": "tooltip", "type": "nominal"},
                "x": {
                    "axis": {"grid": false, "title": "Memory (MiB)"},
                    "field": "memory",
                    "type": "quantitative",
                },
            },
        };
        vegaEmbed(div, spec);
    }

    var results_valid = [
        {"label": "msgspec", "encode": 0.00016727479400015, "decode": 0.0004222057979986857, "memory": 0.640625},
        {"label": "mashumaro", "encode": 0.000797896412001137, "decode": 0.0026786830099990765, "memory": 7.1171875},
        {"label": "cattrs", "encode": 0.002065396289999626, "decode": 0.0033923348699954657, "memory": 3.25390625},
        {"label": "pydantic v2", "encode": 0.0034702956599994648, "decode": 0.0038069566000012854, "memory": 16.26171875},
        {"label": "pydantic v1", "encode": 0.01961492505001843, "decode": 0.02528851079996457, "memory": 10.03125},
    ];
    var results_json = [
        {"label": "msgspec structs", "encode": 0.00014051752349996606, "decode": 0.00036725287499939443},
        {"label": "msgspec", "encode": 0.00018274705249996258, "decode": 0.00048175174399875685},
        {"label": "json", "encode": 0.0012280583099982323, "decode": 0.0009195450700008223},
        {"label": "orjson", "encode": 0.00017935967999983403, "decode": 0.0004634268540012272},
        {"label": "ujson", "encode": 0.0006279176680000091, "decode": 0.0008554406740004197},
        {"label": "rapidjson", "encode": 0.000513588076000815, "decode": 0.0011320363100003306},
        {"label": "simdjson", "encode": 0.00123421613499886, "decode": 0.0007710835699999734},
    ];
    var results_msgpack = [
        {"label": "msgspec structs", "encode": 0.00011157811949942698, "decode": 0.000347989668000082},
        {"label": "msgspec", "encode": 0.00012483930500002316, "decode": 0.000487175850001222},
        {"label": "msgpack", "encode": 0.00040346372400017574, "decode": 0.0007988804240012541},
        {"label": "ormsgpack", "encode": 0.00016052370499983226, "decode": 0.0007458347079991654}
    ];
    buildPlot('#bench-validate', results_valid, "Benchmark - JSON Serialization & Validation");
    buildMemPlot('#bench-validate-memory', results_valid, "Benchmark - Serialization & Validation");
    buildPlot('#bench-json', results_json, "Benchmark - JSON Serialization");
    buildPlot('#bench-msgpack', results_msgpack, "Benchmark - MessagePack Serialization");
    </script>


.. _msgspec: https://jcristharif.com/msgspec/
.. _msgpack: https://github.com/msgpack/msgpack-python
.. _ormsgpack: https://github.com/aviramha/ormsgpack
.. _MessagePack: https://msgpack.org
.. _orjson: https://github.com/ijl/orjson
.. _json: https://docs.python.org/3/library/json.html
.. _simdjson: https://github.com/TkTech/pysimdjson
.. _ujson: https://github.com/ultrajson/ultrajson
.. _rapidjson: https://github.com/python-rapidjson/python-rapidjson
.. _attrs: https://www.attrs.org
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _cattrs: https://catt.rs/en/latest/
.. _mashumaro: https://github.com/Fatal1ty/mashumaro
.. _conda-forge: https://conda-forge.org/
