Conda Repodata
==============

This example benchmarks using different JSON libraries to parse and query the
`current_repodata.json`_ file from conda-forge_. This is a medium-sized (~14
MiB) JSON file containing nested metadata about every package on conda-forge.

The following libraries are compared:

- json_
- ujson_
- orjson_
- simdjson_
- msgspec_

This benchmark measures how long it takes each library to decode the
``current_repodata.json`` file, extract the name and size of each package, and
determine the top 10 packages by file size.

**Results**

.. raw:: html

    <div id="bench-conda-repodata"></div>

.. code-block:: text

   $ python query_repodata.py
   json: 139.14 ms
   ujson: 124.91 ms
   orjson: 91.69 ms
   simdjson: 66.40 ms
   msgspec: 25.73 ms


**Commentary**

- All of these are fairly quick, library choice likely doesn't matter at all
  for simple scripts on small- to medium-sized data.

- While ``orjson`` is faster than ``json``, the difference between them is only
  ~30%. Creating python objects dominates the execution time of any well
  optimized decoding library. How fast the underlying JSON parser is matters,
  but JSON optimizations can only get you so far if you're still creating a new
  Python object for every node in the JSON object.

- ``simdjson`` is much more performant. This is partly due to the SIMD
  optimizations it uses, but mostly it's due to not creating so many Python
  objects. ``simdjson`` first parses a JSON blob into a proxy object. It then
  lazily creates Python objects as needed as different fields are accessed.
  This means you only pay the cost of creating Python objects for the fields
  you use; a query that only accesses a few fields runs much faster since not
  as many Python objects are created. The downside is every attribute access
  results in some indirection as new objects are created

- ``msgspec`` is the fastest option tested. It relies on defining a known
  schema beforehand. We don't define the schema for the entire structure, only
  for the fields we access. Only fields that are part of the schema are
  decoded, with a new Python object created for each. This allocates the same
  number of objects as ``simdjson``, but does it all at once, avoiding
  indirection costs later on during use. See :ref:`this performance tip
  <avoid-decoding-unused-fields>` for more information.

**Source**

The full example source can be found `here
<https://github.com/jcrist/msgspec/blob/main/examples/conda-repodata>`__.

.. literalinclude:: ../../../examples/conda-repodata/query_repodata.py
    :language: python

.. raw:: html

    <script src="https://cdn.jsdelivr.net/npm/vega@5.22.1"></script>
    <script src="https://cdn.jsdelivr.net/npm/vega-lite@5.5.0"></script>
    <script src="https://cdn.jsdelivr.net/npm/vega-embed@6.21.0"></script>
    <script type="text/javascript">
    var spec = {
        "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
        "title": {
            "text": "Benchmark - Processing current_repodata.json",
            "anchor": "start"
        },
        "config": {"view": {"discreteWidth": 400, "stroke": null}},
        "data": {
            "values": [
            {"library": "json", "time": 139.14},
            {"library": "ujson", "time": 124.91},
            {"library": "orjson", "time": 91.69},
            {"library": "simdjson", "time": 66.40},
            {"library": "msgspec", "time": 25.73}
            ]
        },
        "transform": [
            {
                "calculate": `join([format(datum.time, '.3'), 'ms'], ' ')`,
                "as": "tooltip",
            }
        ],
        "mark": "bar",
        "encoding": {
            "tooltip": {"field": "tooltip", "type": "nominal"},
            "x": {"field": "library", "type": "nominal", "axis": {"labelAngle": 0, "title": null}, "sort": {"field": "time", "order": "descending"}},
            "y": {"field": "time", "type": "quantitative", "axis": {"title": "time (ms)", "grid": false}}
        }
    };
    vegaEmbed("#bench-conda-repodata", spec);
    </script>


.. _conda-forge: https://conda-forge.org/
.. _current_repodata.json: https://conda.anaconda.org/conda-forge/noarch/current_repodata.json
.. _json: https://docs.python.org/3/library/json.html
.. _ujson: https://github.com/ultrajson/ultrajson
.. _msgspec: https://jcristharif.com/msgspec/
.. _orjson: https://github.com/ijl/orjson
.. _simdjson: https://github.com/TkTech/pysimdjson
