GeoJSON
=======

`GeoJSON <https://geojson.org>`__ is a popular format for encoding geographic
data. Its specification_ describes nine different types a message may take
(seven "geometry" types, plus two "feature" types). Here we provide one way of
implementing that specification using ``msgspec`` to handle the parsing and
validation.

The ``loads`` and ``dumps`` methods defined below work similar to the
standard library's ``json.loads``/``json.dumps``, but:

- Will result in high-level `msgspec.Struct` objects representing GeoJSON types
- Will error nicely if a field is missing or the wrong type
- Will fill in default values for optional fields
- Decodes and encodes *significantly faster* than the `json` module (as well as
  most other ``json`` implementations in Python).

This example makes use `msgspec.Struct` types to define the different GeoJSON
types, and :ref:`struct-tagged-unions` to differentiate between them. See the
relevant docs for more information.

The full example source can be found `here
<https://github.com/jcrist/msgspec/blob/main/examples/geojson>`__.

.. literalinclude:: ../../../examples/geojson/msgspec_geojson.py
    :language: python


Here we use the ``loads`` method defined above to read some `example GeoJSON`_.

.. code-block:: ipython3

    In [1]: import msgspec_geojson

    In [2]: with open("canada.json", "rb") as f:
       ...:     data = f.read()

    In [3]: canada = msgspec_geojson.loads(data)

    In [4]: type(canada)  # loaded as high-level, validated object
    Out[4]: msgspec_geojson.FeatureCollection

    In [5]: canada.features[0].properties
    Out[5]: {'name': 'Canada'}

Comparing performance to:

- orjson_
- `json`
- geojson_ (another validating Python implementation)

.. code-block:: ipython3

   In [6]: %timeit msgspec_geojson.loads(data)  # benchmark msgspec
   6.15 ms ± 13.8 µs per loop (mean ± std. dev. of 7 runs, 100 loops each)

   In [7]: %timeit orjson.loads(data)  # benchmark orjson
   8.67 ms ± 20.8 µs per loop (mean ± std. dev. of 7 runs, 100 loops each)

   In [8]: %timeit json.loads(data)  # benchmark json
   27.6 ms ± 102 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)

   In [9]: %timeit geojson.loads(data)  # benchmark geojson
   93.9 ms ± 88.1 µs per loop (mean ± std. dev. of 7 runs, 10 loops each)


This shows that the readable ``msgspec`` implementation above is 1.4x faster
than `orjson` (on this data), while also ensuring the loaded data is valid
GeoJSON.  Compared to geojson_ (another validating geojson library for python),
loading the data using ``msgspec`` was **15.3x faster**.

.. _specification: https://datatracker.ietf.org/doc/html/rfc7946
.. _example GeoJSON: https://github.com/jcrist/msgspec/blob/main/examples/geojson/canada.json
.. _orjson: https://github.com/ijl/orjson
.. _geojson: https://github.com/jazzband/geojson
