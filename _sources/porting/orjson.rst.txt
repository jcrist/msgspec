Porting from orjson
===================

If you're currently using orjson_ and considering migrating to ``msgspec``,
this guide covers the key API differences and shows how to translate common
patterns.

Why migrate?
------------

Both ``orjson`` and ``msgspec`` are high-performance JSON libraries written in
compiled languages.

There are a few reasons you might prefer ``msgspec``:

- **Typed decoding and validation**. ``orjson.loads`` always returns plain
  Python builtins (``dict``, ``list``, ...). With ``msgspec`` you can decode
  directly into typed objects (``Struct``, ``dataclass``, ``TypedDict``, etc.)
  while validating the schema in a single pass. See :doc:`../usage` for details.

- **Schema evolution**. ``msgspec`` has first-class support for evolving
  schemas over time without breaking existing clients. See
  :doc:`../schema-evolution`.

- **Multiple protocols**. ``msgspec`` also supports MessagePack, YAML, and
  TOML with a consistent API, making it easy to switch between formats.

- **JSON Schema generation**. ``msgspec`` can generate JSON Schema definitions
  from your types via :func:`msgspec.json.schema`. Useful for OpenAPI
  documentation and API tooling.

Basic usage
-----------

For most users, migration is straightforward -- swap function names and you're
done:

.. code-block:: python

    # Before (orjson)
    import orjson

    data = orjson.dumps({"hello": "world"})
    obj = orjson.loads(data)

    # After (msgspec)
    import msgspec

    data = msgspec.json.encode({"hello": "world"})
    obj = msgspec.json.decode(data)

Both return ``bytes`` from encoding and accept ``bytes`` or ``str`` for
decoding.

Typed decoding
--------------

The main advantage of ``msgspec`` over ``orjson`` is typed decoding. Instead of
getting a plain ``dict`` back, you can decode directly into structured types:

.. code-block:: python

    import msgspec

    class User(msgspec.Struct):
        name: str
        age: int
        email: str | None = None

    data = b'{"name": "Alice", "age": 30}'
    user = msgspec.json.decode(data, type=User)

    print(user.name)  # "Alice"
    print(user.age)   # 30
    print(user.email)  # None

This validates the data while decoding -- if a field is missing or has the
wrong type, a ``msgspec.ValidationError`` is raised with a clear error message.

Encoder and Decoder classes
---------------------------

If you're making multiple calls, using the ``Encoder`` and ``Decoder`` classes
is more efficient:

.. code-block:: python

    # orjson has no stateful encoder/decoder

    # msgspec
    encoder = msgspec.json.Encoder()
    decoder = msgspec.json.Decoder(User)

    data = encoder.encode(user)
    user = decoder.decode(data)

Option mapping
--------------

``orjson`` uses bitwise ``OPT_*`` flags. ``msgspec`` uses keyword arguments on
``Encoder``/``Decoder`` or the ``encode``/``decode`` functions.

Sorting keys
^^^^^^^^^^^^

.. code-block:: python

    # orjson
    orjson.dumps(obj, option=orjson.OPT_SORT_KEYS)

    # msgspec
    msgspec.json.encode(obj, order="sorted")

    # Or with an Encoder
    encoder = msgspec.json.Encoder(order="sorted")

``msgspec`` also supports ``order="deterministic"`` which provides a
consistent ordering without the cost of full alphabetical sorting.

Pretty-printing
^^^^^^^^^^^^^^^

.. code-block:: python

    # orjson
    orjson.dumps(obj, option=orjson.OPT_INDENT_2)

    # msgspec -- use format() on already-encoded JSON
    raw = msgspec.json.encode(obj)
    pretty = msgspec.json.format(raw, indent=2)

Unlike ``orjson``'s single-pass approach, ``msgspec`` uses a two-step process:
encode first, then format. ``msgspec.json.format`` works on any JSON bytes/str,
not just ``msgspec``-encoded data, and supports arbitrary indent levels.

Custom type serialization
^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: python

    # orjson
    def default(obj):
        if isinstance(obj, complex):
            return {"real": obj.real, "imag": obj.imag}
        raise TypeError

    orjson.dumps(obj, default=default)

    # msgspec
    def enc_hook(obj):
        if isinstance(obj, complex):
            return {"real": obj.real, "imag": obj.imag}
        raise NotImplementedError

    msgspec.json.encode(obj, enc_hook=enc_hook)

The key difference: ``orjson``'s ``default`` should raise ``TypeError`` for
unhandled types, while ``msgspec``'s ``enc_hook`` should raise
``NotImplementedError``.

Appending newlines
^^^^^^^^^^^^^^^^^^

.. code-block:: python

    # orjson
    orjson.dumps(obj, option=orjson.OPT_APPEND_NEWLINE)

    # msgspec -- no built-in flag, just append manually
    msgspec.json.encode(obj) + b"\n"

Naive datetimes
^^^^^^^^^^^^^^^

By default, both libraries encode naive datetimes identically (without
timezone info):

.. code-block:: python

    import datetime

    dt = datetime.datetime(2024, 1, 1, 12, 0, 0)

    orjson.dumps(dt)           # b'"2024-01-01T12:00:00"'
    msgspec.json.encode(dt)    # b'"2024-01-01T12:00:00"'

``orjson`` has ``OPT_NAIVE_UTC`` to force treating naive datetimes as UTC:

.. code-block:: python

    orjson.dumps(dt, option=orjson.OPT_NAIVE_UTC)
    # b'"2024-01-01T12:00:00+00:00"'

``msgspec`` has no equivalent flag. Convert before encoding instead:

.. code-block:: python

    def ensure_utc(dt):
        if dt.tzinfo is None:
            return dt.replace(tzinfo=datetime.timezone.utc)
        return dt

    msgspec.json.encode(ensure_utc(dt))

.. note::

    ``enc_hook`` is only called for types that ``msgspec`` doesn't natively
    support. Since ``datetime`` is natively supported, you need to convert
    before passing to ``encode``.

Numpy arrays
^^^^^^^^^^^^

.. code-block:: python

    import numpy as np

    # orjson -- native support
    orjson.dumps(arr, option=orjson.OPT_SERIALIZE_NUMPY)

    # msgspec -- use enc_hook
    def enc_hook(obj):
        if isinstance(obj, np.ndarray):
            return obj.tolist()
        raise NotImplementedError

    msgspec.json.encode(arr, enc_hook=enc_hook)

Pre-serialized JSON fragments
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: python

    # orjson
    orjson.dumps({"key": orjson.Fragment(b'{"already":"json"}')})

    # msgspec
    msgspec.json.encode({"key": msgspec.Raw(b'{"already":"json"}')})

Decimal and UUID handling
-------------------------

``msgspec`` has built-in support for both ``Decimal`` and ``UUID``, with
configurable output formats:

.. code-block:: python

    from decimal import Decimal
    from uuid import uuid4

    enc = msgspec.json.Encoder(
        decimal_format="number",     # or "string" (default)
        uuid_format="hex",           # or "canonical" (default)
    )

``orjson`` requires a ``default`` callback for ``Decimal``.

Line-delimited JSON
-------------------

``msgspec`` has built-in support for encoding and decoding newline-delimited
JSON (NDJSON):

.. code-block:: python

    encoder = msgspec.json.Encoder()
    decoder = msgspec.json.Decoder(User)

    # Encode multiple objects as NDJSON
    data = encoder.encode_lines([user1, user2, user3])

    # Decode NDJSON back
    users = decoder.decode_lines(data)

``orjson`` has no equivalent -- you'd need to encode each object separately and
join with newlines.

Error handling
--------------

.. code-block:: python

    # orjson
    try:
        orjson.loads(bad_data)
    except orjson.JSONDecodeError:
        ...

    # msgspec
    try:
        msgspec.json.decode(bad_data, type=User)
    except msgspec.ValidationError:
        # Schema validation error (wrong type, missing field, etc.)
        ...
    except msgspec.DecodeError:
        # Malformed JSON
        ...

``msgspec`` distinguishes between malformed JSON (``DecodeError``) and valid
JSON that doesn't match the schema (``ValidationError``, a subclass of
``DecodeError``). Note that ``ValidationError`` must be caught first since it
inherits from ``DecodeError``.

Features with no direct equivalent
-----------------------------------

Some ``orjson`` options have no direct ``msgspec`` flag, but can be achieved
through other means:

.. list-table::
   :header-rows: 1

   * - orjson
     - msgspec workaround
   * - ``OPT_NON_STR_KEYS``
     - Use ``enc_hook``/``dec_hook`` or ``msgspec.to_builtins(obj, str_keys=True)``
   * - ``OPT_OMIT_MICROSECONDS``
     - Convert before encoding: ``dt.replace(microsecond=0)``. ``enc_hook`` cannot
       be used here because ``datetime`` is a natively supported type.
   * - ``OPT_STRICT_INTEGER`` (53-bit)
     - Use ``Meta(ge=-(2**53 - 1), le=2**53 - 1)`` constraint
   * - ``OPT_PASSTHROUGH_DATACLASS``
     - Not directly supported. ``dataclass`` is natively supported, so ``enc_hook``
       is not called for it. Convert to a ``dict`` before encoding if you need
       custom serialization.
   * - ``OPT_PASSTHROUGH_SUBCLASS``
     - Not applicable. ``msgspec`` always serializes subclasses of builtins as
       their parent type, with no option to redirect them to ``enc_hook``.

.. _orjson: https://github.com/ijl/orjson
