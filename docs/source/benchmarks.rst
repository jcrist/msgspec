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
- ``pyrobuf`` - pyrobuf_ with protobuf message types
- ``msgspec msgpack`` - msgspec_'s MessagePack encoding, with `msgspec.Struct`
  message types
- ``msgspec msgpack asarray`` - msgspec_'s MessagePack encoding, with
  `msgspec.Struct` message types configured with ``asarray=True``
- ``msgspec json`` - msgspec_'s JSON encoding, with `msgspec.Struct` message types
- ``msgspec json asarray`` - msgspec_'s JSON encoding, with `msgspec.Struct`
  message types configured with ``asarray=True``

Each benchmark creates one or more instances of a ``Person`` message, and
serializes it/deserializes it in a loop. The `full benchmark source can be
found here <https://github.com/jcrist/msgspec/tree/master/benchmarks>`__.

1 Object
^^^^^^^^

Some workflows involve sending around very small messages. Here the overhead
per function call dominates (parsing of options, allocating temporary buffers,
etc...).

.. raw:: html

    <div class="bk-root" id="bench-1"></div>

.. note::

    You can use the radio buttons on the bottom to sort by encode time, decode
    time, total roundtrip time, or serialized message size. Hovering over a bar
    will also display its corresponding value.

From the chart above, you can see that ``msgspec msgpack asarray`` is the
fastest method for both serialization and deserialization. It also results in
the smallest serialized message size, just edging out ``pyrobuf``. This makes
sense; with ``asarray=True`` `Struct` types don’t serialize the field names in
each message (things like ``"first"``, ``"last"``, …), only the values, leading
to smaller messages and higher performance.

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

    <div class="bk-root" id="bench-1k"></div>


Schema Validation
^^^^^^^^^^^^^^^^^

The above benchmarks aren't 100% fair to ``msgspec`` or ``pyrobuf``. Both
libraries also perform schema validation on deserialization, checking that the
message matches the specified schema. None of the other options benchmarked
support this natively. Instead, many users perform validation post
deserialization using additional tools like pydantic_. Here we add the cost of
schema validation during deserialization, using pydantic_ for all libraries
lacking builtin validation.

.. raw:: html

    <div class="bk-root" id="bench-1-validate"></div>


.. raw:: html

    <div class="bk-root" id="bench-1k-validate"></div>


These plots show the performance benefit of performing type validation during
message decoding (as done by ``msgspec`` and pyrobuf_) rather than as a
secondary step with a third-party library like pydantic_. Validating after
decoding is slower for two reasons:

- It requires traversing over the entire output structure a second time (which
  can be slow due to pointer chasing)

- It may require converting some python objects to their desired output types
  (e.g. converting a decoded `dict` to a pydantic_ model), resulting in
  allocating many temporary python objects.

In contrast, libraries like ``msgspec`` that validate during decoding have none
of these issues. Only a single pass over the decoded data is taken, and the
specified output types are created correctly the first time, avoiding the need
for additional unnecessary allocations.


Benchmark - Structs
-------------------

Here we benchmark common `msgspec.Struct` operations, comparing their
performance against other similar libraries. The cases compared are:

- Standard Python classes
- attrs_
- dataclasses_
- pydantic_
- ``msgspec``

For each library, the following operations are benchmarked:

- Time to define a new class. Many libraries that abstract away class
  boilerplate add overhead when defining classes, slowing import times for
  libraries that make use of these classes.
- Time to create an instance of that class.
- Time to compare two instances.

The `full benchmark source can be found here
<https://github.com/jcrist/msgspec/tree/master/benchmarks/bench_structs.py>`__.

**Results:**

+----------------------+-------------+-------------+--------------+
|                      | define (μs) | create (μs) | compare (μs) |
+======================+=============+=============+==============+
| **standard classes** | 4.21        | 0.47        | 0.12         |
+----------------------+-------------+-------------+--------------+
| **attrs**            | 690.21      | 0.84        | 0.30         |
+----------------------+-------------+-------------+--------------+
| **dataclasses**      | 300.48      | 0.63        | 0.28         |
+----------------------+-------------+-------------+--------------+
| **pydantic**         | 420.09      | 6.25        | 11.78        |
+----------------------+-------------+-------------+--------------+
| **msgspec**          | 13.61       | 0.09        | 0.02         |
+----------------------+-------------+-------------+--------------+

- Standard Python classes are the fastest to import (any library can only add
  overhead here). Still, ``msgspec`` isn't *that* much slower, especially
  compared to other options.
- Structs are optimized to be cheap to create, and that shows for the creation
  benchmark. They're roughly 5x faster than standard python classes here.
- Same goes for comparison, with structs measuring roughly 6x faster than
  standard python classes.

.. raw:: html

    <script type="text/javascript" src="https://cdn.bokeh.org/bokeh/release/bokeh-2.3.2.min.js" integrity="XypntL49z55iwGVUW4qsEu83zKL3XEcz0MjuGOQ9SlaaQ68X/g+k1FcioZi7oQAc" crossorigin="anonymous"></script>
    <script type="text/javascript" src="https://cdn.bokeh.org/bokeh/release/bokeh-widgets-2.3.2.min.js" integrity="TX0gSQTdXTTeScqxj6PVQxTiRW8DOoGVwinyi1D3kxv7wuxQ02XkOxv0xwiypcAH" crossorigin="anonymous"></script>
    <script>
    fetch('_static/bench-1.json')
        .then(function(response) { return response.json() })
        .then(function(item) { return Bokeh.embed.embed_item(item, 'bench-1') })
    fetch('_static/bench-1k.json')
        .then(function(response) { return response.json() })
        .then(function(item) { return Bokeh.embed.embed_item(item, 'bench-1k') })
    fetch('_static/bench-1-validate.json')
        .then(function(response) { return response.json() })
        .then(function(item) { return Bokeh.embed.embed_item(item, 'bench-1-validate') })
    fetch('_static/bench-1k-validate.json')
        .then(function(response) { return response.json() })
        .then(function(item) { return Bokeh.embed.embed_item(item, 'bench-1k-validate') })
    </script>


.. _msgspec: https://jcristharif.com/msgspec/
.. _msgpack: https://github.com/msgpack/msgpack-python
.. _orjson: https://github.com/ijl/orjson
.. _pyrobuf: https://github.com/appnexus/pyrobuf
.. _ujson: https://github.com/ultrajson/ultrajson
.. _attrs: https://www.attrs.org
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _pydantic: https://pydantic-docs.helpmanual.io/
