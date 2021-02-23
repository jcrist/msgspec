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

- ``msgpack`` - msgpack_ with dict message types
- ``orjson`` - orjson_ with dict message types
- ``pyrobuf`` - pyrobuf_ with protobuf message types
- ``msgspec`` - msgspec_ with `msgspec.Struct` message types

Each benchmark creates one or more instances of a ``Person`` message, and
serializes it/deserializes it in a loop. The `full benchmark source can be
found here <https://github.com/jcrist/msgspec/tree/master/benchmarks>`__.

Benchmark - 1 Object
--------------------

Some workflows involve sending around very small messages. Here the overhead
per function call dominates (parsing of options, allocating temporary buffers,
etc...).

.. raw:: html

    <div class="bk-root" id="bench-1"></div>

.. note::

    You can use the radio buttons on the bottom to sort by encode time, decode
    time, or total roundtrip time.

From the chart above, you can see that ``msgspec`` is the fastest method for
both serialization and deserialization, followed by ``msgpack`` and ``orjson``.
I'm actually surprised at how much overhead ``pyrobuf`` has (the actual
protobuf encoding should be pretty efficient), I suspect there's some
optimizations that could still be done there.

That said, all of these methods serialize/deserialize pretty quickly relative
to other python operations, so unless you're counting every microsecond your
choice here probably doesn't matter that much.

Benchmark - 1000 Objects
------------------------

Here we serialize a list of 1000 ``Person`` objects. There's a lot more data
here, so the per-call overhead will no longer dominate, and we're now measuring
the efficiency of the encoding/decoding.

.. raw:: html

    <div class="bk-root" id="bench-1k"></div>


Benchmark - with Validation
---------------------------

The above benchmarks aren't 100% fair to ``msgspec`` or ``pyrobuf``. Both
libraries also perform schema validation on deserialization, checking that the
message matches the specified schema. Neither ``msgpack`` nor ``orjson``
support this builtin. Instead, many users perform validation post
deserialization using additional tools like pydantic_. Here we add the cost of
Pydantic validation to ``msgpack`` and ``orjson`` to ensure all benchmarks are
doing both deserialization and validation.

.. raw:: html

    <div class="bk-root" id="bench-1-validate"></div>


.. raw:: html

    <div class="bk-root" id="bench-1k-validate"></div>


.. raw:: html

    <script type="text/javascript" src="https://cdn.bokeh.org/bokeh/release/bokeh-2.1.1.min.js" integrity="sha384-kLr4fYcqcSpbuI95brIH3vnnYCquzzSxHPU6XGQCIkQRGJwhg0StNbj1eegrHs12" crossorigin="anonymous"></script>
    <script type="text/javascript" src="https://cdn.bokeh.org/bokeh/release/bokeh-widgets-2.1.1.min.js" integrity="sha384-xIGPmVtaOm+z0BqfSOMn4lOR6ciex448GIKG4eE61LsAvmGj48XcMQZtKcE/UXZe" crossorigin="anonymous"></script>
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
.. _pydantic: https://pydantic-docs.helpmanual.io/
