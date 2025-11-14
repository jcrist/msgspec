Performance Tips
================

Here we present a few tips and tricks for squeezing maximum performance out of
``msgspec``. They're presented in order from "sane, definitely a good idea" to
"fast, but you may not want to do this".

Reuse Encoders/Decoders
-----------------------

Every call to a top-level ``encode`` function like `msgspec.json.encode`
allocates some temporary internal state used for encoding. While fine for
normal use, for maximum performance you'll want to create an ``Encoder`` (e.g.
`msgspec.json.Encoder`) once and reuse it for all encoding calls, avoiding
paying that setup cost for every call.

.. code-block:: python

    >>> import msgspec

    >>> encoder = msgspec.json.Encoder()  # Create once

    >>> for msg in msgs:
    ...     data = encoder.encode(msg)  # reuse multiple times

The same goes for decoding. If you're making multiple ``decode`` calls in a
performance-sensitive code path, you'll want to create a ``Decoder`` (e.g.
`msgspec.json.Decoder`) once and reuse it for each call. Since decoders are
typed, you may need to create multiple decoders, one for each type.

.. code-block:: python

    >>> import msgspec

    >>> decoder = msgspec.json.Decoder(list[int])  # Create once

    >>> for data in input_buffers:
    ...     msg = decoder.decode(data)  # reuse multiple times


Use Structs
-----------

:doc:`structs` are msgspec's native way of expressing user-defined types.
They're :ref:`fast to encode/decode <json-benchmark>` and :ref:`fast to use
<struct-benchmark>`. If you have data with a known schema, we recommend
defining a `msgspec.Struct` type (or types) for your schema and preferring that
over other types like `dict`/`dataclasses`/...


Avoid Encoding Default Values
-----------------------------

By default, ``msgspec`` encodes all fields in a Struct type, including optional
fields (those configured with a default value). If the default values are known
on the decoding end (making serializing them redundant), it may be beneficial
to omit default values from the encoded message. This can be done by
configuring ``omit_defaults=True`` as part of the Struct definition Omitting
defaults reduces the size of the encoded message, and often also improves
encoding and decoding performance (since there's less work to do).

For more information, see :ref:`omit_defaults`.


.. _avoid-decoding-unused-fields:

Avoid Decoding Unused Fields
----------------------------

When decoding large inputs, sometimes you're only interested in a few specific
fields. Since decoding large objects is inherently allocation heavy, it may be
beneficial to define a smaller `msgspec.Struct` type that only has the fields
you require.

For example, say you're interested in decoding some JSON from the `Twitter API
<https://developer.twitter.com/en/docs/twitter-api/v1/data-dictionary/object-model/tweet>`__.
A ``Tweet`` object has many nested fields on it - perhaps you only care about
the tweet text, the user name, and the number of favorites. By defining struct
types with only those fields, ``msgspec`` can avoid doing unnecessary work
decoding fields that are never used.

.. code-block:: python

    >>> import msgspec

    >>> class User(msgspec.Struct):
    ...     name: str

    >>> class Tweet(msgspec.Struct):
    ...     user: User
    ...     full_text: str
    ...     favorite_count: int


We can then use these types to decode the `example tweet json
<https://developer.twitter.com/en/docs/twitter-api/v1/data-dictionary/object-model/example-payloads>`__:

.. code-block:: python

    >>> tweet = msgspec.json.decode(example_json, type=Tweet)

    >>> tweet.user.name
    'Twitter Dev'

    >>> tweet.user.favorite_count
    70

Of course there are downsides to defining smaller "view" types, but if decoding
performance is a bottleneck in your workflow, you may benefit from this
technique.

For a more in-depth example of this technique, see the
:doc:`examples/conda-repodata` example.


Reduce Allocations
------------------

Every call to ``encode``/``Encoder.encode`` allocates a new `bytes` object for
the output. ``msgspec`` exposes an alternative ``Encoder.encode_into`` (e.g.
`msgspec.json.Encoder.encode_into`) that writes into a pre-allocated
`bytearray` instead (possibly reallocating to increase capacity).

This has a few uses:

Reusing an output buffer
^^^^^^^^^^^^^^^^^^^^^^^^

If you're encoding and writing messages to a socket/file in a hot loop, you
*may* benefit from allocating a single `bytearray` buffer once and reusing it
for every message.

For example:

.. code-block:: python

    encoder = msgspec.msgpack.Encoder()

    # Allocate a single shared buffer
    buffer = bytearray()

    for msg in msgs:
        # Encode a message into the buffer at the start of the buffer.
        # Note that this overwrites any previous contents.
        encoder.encode_into(msg, buffer)

        # Write the buffer to the socket
        socket.sendall(buffer)

A few caveats:

- ``Encoder.encode_into`` will expand the capacity of ``buffer`` as needed to
  fit the message size. This means that if a large message is encountered the
  buffer will be expanded to be equally large, but won't be reduced back to
  normal afterwards (possibly bloating memory usage). You can use
  `sys.getsizeof` (or call `bytearray.__sizeof__`) directly to determine the
  actual capacity of the buffer, since ``len(buffer)`` will only reflect the
  part of the buffer that is written to.

- Small messages (for some definition of "small") likely won't see a
  performance improvement from using this method, and may instead see a
  slowdown. We recommend using a realistic benchmark to determine if this
  method can benefit your workload.

Line-Delimited JSON
^^^^^^^^^^^^^^^^^^^

Some protocols require appending a suffix to an encoded message. One place
where this comes up is when encoding `line-delimited JSON`_, where every
payload contains a JSON message followed by ``b"\n"``.

This *could* be handled in python as:

.. code-block:: python

    import msgspec

    json_msg = msgspec.json.encode(["my", "message"])

    full_payload = json_msg + b'\n'

However, this results in an unnecessary copy of ``json_msg``, which can be
avoided by using `msgspec.json.Encoder.encode_into`.

.. code-block:: python

    import msgspec

    encoder = msgspec.json.Encoder()

    # Allocate a buffer. We recommend using a small non-empty buffer to
    # avoid reallocating for small messages. Choose something larger than
    # your common message size, but not excessively large.
    buffer = bytearray(64)

    # Encode into the existing buffer.
    encoder.encode_into(["my", "message"], buffer)

    # Append a newline character without copying
    buffer.extend(b"\n")

    # Write the full buffer to a socket/file/etc...
    socket.sendall(buffer)

Length-Prefix Framing
^^^^^^^^^^^^^^^^^^^^^

Some protocols require prepending a prefix to an encoded message. This comes up
in `Length-prefix framing
<https://eli.thegreenplace.net/2011/08/02/length-prefix-framing-for-protocol-buffers>`__
, where every message is prefixed by its length stored as a fixed-width integer
(e.g. a big-endian uint32). Like line-delimited JSON above, this is more
efficient to do using ``Encoder.encode_into`` to avoid excessive copying.

.. code-block:: python

    import msgspec

    encoder = msgspec.msgpack.Encoder()

    # Allocate a buffer. We recommend using a small non-empty buffer to
    # avoid reallocating for small messages. Choose something larger than
    # your common message size, but not excessively large.
    buffer = bytearray(64)

    # Encode into the existing buffer, offset by 4 bytes at the front to
    # store the length prefix.
    encoder.encode_into(msg, buffer, 4)

    # Encode the message length as a 4 byte big-endian integer, and
    # prefix the message with it (without copying).
    n = len(msg) - 4
    buffer[:4] = n.to_bytes(4, "big")

    # Write the buffer to a socket/file/etc...
    socket.sendall(buffer)

Use MessagePack
---------------

``msgspec`` supports both JSON_ and MessagePack_ protocols. The latter is less
commonly used, but :ref:`can be more performant <msgpack-benchmark>`. If
performance is an issue (and MessagePack is an acceptable solution), you may
benefit from using it instead of JSON. And since ``msgspec`` supports both
protocols with a consistent interface, switching from ``msgspec.json`` to
``msgspec.msgpack`` should be fairly painless.

Use ``gc=False``
-----------------

Python processes with a large number of long-lived objects, or operations that
allocate a large number of objects at once may suffer reduced performance due
to Python's garbage collector (GC). By default, `msgspec.Struct` types
implement a few optimizations to reduce the load on the GC (and thus reduce the
frequency and duration of a GC pause). If you find that GC is still a problem,
and **are certain** that your Struct types may never participate in a reference
cycle, then you **may** benefit from setting ``gc=False`` on your Struct
types.  Depending on workload, this can result in a measurable decrease in
pause time and frequency due to GC passes. See :ref:`struct-gc` for more
details.

Use ``array_like=True``
-----------------------

One touted benefit of JSON_ and MessagePack_ is that they're "self-describing"
protocols. JSON objects serialize their field names along with their values. If
both ends of a connection already know the field names though, serializing them
may be an unnecessary cost. If you need higher performance (at the cost of more
inscrutable message encoding), you can set ``array_like=True`` on a struct
definition. Structs with this option enabled are encoded/decoded like array
types, removing the field names from the encoded message. This can provide on
average another ~2x speedup for decoding (and ~1.5x speedup for encoding).

.. code-block:: python

    >>> class Example(msgspec.Struct, array_like=True):
    ...     my_first_field: str
    ...     my_second_field: int

    >>> x = Example("some string", 2)

    >>> msg = msgspec.json.encode(x)

    >>> msg
    b'["some string",2]'

    >>> msgspec.json.decode(msg, type=Example)
    Example(my_first_field="some string", my_second_field=2)


.. _JSON: https://json.org
.. _MessagePack: https://msgpack.org
.. _line-delimited JSON: https://en.wikipedia.org/wiki/JSON_streaming#Line-delimited_JSON
