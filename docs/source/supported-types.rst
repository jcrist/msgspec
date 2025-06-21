Supported Types
===============

``msgspec`` uses Python `type annotations`_ to describe the expected types.
Most combinations of the following types are supported (with a few restrictions):

**Builtin Types**

- `None`
- `bool`
- `int`
- `float`
- `str`
- `bytes`
- `bytearray`
- `tuple` / `typing.Tuple`
- `list` / `typing.List`
- `dict` / `typing.Dict`
- `set` / `typing.Set`
- `frozenset` / `typing.FrozenSet`

**Msgspec types**

- `msgspec.msgpack.Ext`
- `msgspec.Raw`
- `msgspec.UNSET`
- `msgspec.Struct` types

**Standard Library Types**

- `datetime.datetime`
- `datetime.date`
- `datetime.time`
- `datetime.timedelta`
- `uuid.UUID`
- `decimal.Decimal`
- `enum.Enum` types
- `enum.IntEnum` types
- `enum.StrEnum` types
- `enum.Flag` types
- `enum.IntFlag` types
- `dataclasses.dataclass` types

**Typing module types**

- `typing.Any`
- `typing.Optional`
- `typing.Union`
- `typing.Literal`
- `typing.NewType`
- `typing.Final`
- `typing.TypeAliasType`
- `typing.TypeAlias`
- `typing.NamedTuple` / `collections.namedtuple`
- `typing.TypedDict`
- `typing.Generic`
- `typing.TypeVar`

**Abstract types**

- `collections.abc.Collection` / `typing.Collection`
- `collections.abc.Sequence` / `typing.Sequence`
- `collections.abc.MutableSequence` / `typing.MutableSequence`
- `collections.abc.Set` / `typing.AbstractSet`
- `collections.abc.MutableSet` / `typing.MutableSet`
- `collections.abc.Mapping` / `typing.Mapping`
- `collections.abc.MutableMapping` / `typing.MutableMapping`

**Third-Party Libraries**

- attrs_ types

Additional types may be supported through :doc:`extensions <extending>`.

Note that except where explicitly stated, subclasses of these types are not
supported by default (see :doc:`extending` for how to add support yourself).

Here we document how msgspec maps Python objects to/from the various supported
protocols.

``None``
--------

`None` maps to the ``null`` value in all supported protocols. Note that TOML_
lacks a ``null`` value, attempted to encode a message containing ``None`` to
``TOML`` will result in an error.

.. code-block:: python

    >>> msgspec.json.encode(None)
    b'null'

    >>> msgspec.json.decode(b'null')
    None

If ``strict=False`` is specified, a string value of ``"null"`` (case
insensitive) may also be coerced to ``None``. See :ref:`strict-vs-lax` for more
information.

.. code-block:: python

   >>> msgspec.json.decode(b'"null"', type=None, strict=False)
   None

``bool``
--------

Booleans map to their corresponding ``true``/``false`` values in both all
supported protocols.

.. code-block:: python

    >>> msgspec.json.encode(True)
    b'true'

    >>> msgspec.json.decode(b'true')
    True

If ``strict=False`` is specified, values of ``"true"``/``"1"``/``1`` or
``"false"``/``"0"``/``0`` (case insensitive for strings) may also be coerced to
``True``/``False`` respectively. See :ref:`strict-vs-lax` for more information.

.. code-block:: python

   >>> msgspec.json.decode(b'"false"', type=bool, strict=False)
   False

   >>> msgspec.json.decode(b'"TRUE"', type=bool, strict=False)
   True

   >>> msgspec.json.decode(b'1', type=bool, strict=False)
   True

``int``
-------

Integers map to integers in all supported protocols.

Support for large integers varies by protocol:

- ``msgpack`` only supports encoding/decoding integers within
  ``[-2**63, 2**64 - 1]``, inclusive.
- ``json``, ``yaml``, and ``toml`` have no restrictions on encode or decode.

.. code-block:: python

    >>> msgspec.json.encode(123)
    b"123"

    >>> msgspec.json.decode(b"123", type=int)
    123

If ``strict=False`` is specified, string values may also be coerced to
integers, following the same restrictions as above. Likewise floats that have
an exact integer representation (i.e. no decimal component) may also be coerced
as integers. See :ref:`strict-vs-lax` for more information.

.. code-block:: python

   >>> msgspec.json.decode(b'"123"', type=int, strict=False)
   123

   >>> msgspec.json.decode(b'123.0', type=int, strict=False)
   123


``float``
---------

Floats map to floats in all supported protocols. Note that per RFC8259_, JSON
doesn't support nonfinite numbers (``nan``, ``infinity``, ``-infinity``);
``msgspec.json`` handles this by encoding these values as ``null``. The
``msgpack``, ``toml``, and ``yaml`` protocols lack this restriction, and can
accurately roundtrip any IEEE754 64 bit floating point value.

For all protocols, if a `float` type is specified and an `int` value is
provided, the `int` will be automatically converted.

.. code-block:: python

    >>> msgspec.json.encode(123.0)
    b"123.0"

    >>> # JSON doesn't support nonfinite values, these serialize as null
    ... msgspec.json.encode(float("nan"))
    b"null"

    >>> msgspec.json.decode(b"123.0", type=float)
    123.0

    >>> # Ints are automatically converted to floats
    ... msgspec.json.decode(b"123", type=float)
    123.0

If ``strict=False`` is specified, string values may also be coerced to floats.
Note that in this case the strings ``"nan"``, ``"inf"``/``"infinity"``,
``"-inf"``/``"-infinity"`` (case insensitive) will coerce to
``nan``/``inf``/``-inf``. See :ref:`strict-vs-lax` for more information.

.. code-block:: python

   >>> msgspec.json.decode(b'"123.45"', type=float, strict=False)
   123.45

   >>> msgspec.json.decode(b'"-inf"', type=float, strict=False)
   -inf

``str``
-------

Strings map to strings in all supported protocols.

Note that for JSON, only the characters required by RFC8259_ are escaped to
ascii; unicode characters (e.g. ``"𝄞"``) are *not* escaped and are serialized
directly as UTF-8 bytes.

.. code-block:: python

    >>> msgspec.json.encode("Hello, world!")
    b'"Hello, world!"'

    >>> msgspec.json.encode("𝄞 is not escaped")
    b'"\xf0\x9d\x84\x9e is not escaped"'

    >>> msgspec.json.decode(b'"Hello, world!"')
    "Hello, world!"

``bytes`` / ``bytearray`` / ``memoryview``
------------------------------------------

Bytes-like objects map to base64-encoded strings in JSON, YAML, and TOML. The
``bin`` type is used for MessagePack.

.. code-block:: python

    >>> msg = msgspec.json.encode(b"\xf0\x9d\x84\x9e")

    >>> msg
    b'"85+Eng=="'

    >>> msgspec.json.decode(msg, type=bytes)
    b'\xf0\x9d\x84\x9e'

    >>> msgspec.json.decode(msg, type=bytearray)
    bytearray(b'\xf0\x9d\x84\x9e')


.. note::

    For the ``msgpack`` protocol, `memoryview` objects will be decoded as
    direct views into the larger buffer containing the input message being
    decoded. This may be useful for implementing efficient zero-copy handling
    of large binary messages, but is also a potential footgun. As long as a
    decoded ``memoryview`` remains in memory, the input message buffer will
    also be persisted, potentially resulting in unnecessarily large memory
    usage. The usage of ``memoryview`` types in this manner is considered an
    advanced topic, and should only be used when you know their usage will
    result in a performance benefit.

    For all other protocols `memoryview` objects will still result in a copy,
    and will likely be slightly slower than decoding into a `bytes` object


``datetime``
------------

The encoding used for `datetime.datetime` objects depends on both the
protocol and whether these objects are timezone-aware_ or timezone-naive:

- **JSON**: Timezone-aware datetimes are encoded as RFC3339_ compatible
  strings. Timezone-naive datetimes are encoded the same, but lack the timezone
  component (making them not strictly RFC3339_ compatible, but still ISO8601_
  compatible).

- **MessagePack**: Timezone-aware datetimes are encoded using the `timestamp
  extension`. Timezone-naive datetimes are encoded the same, but lack the
  timezone component (making them not strictly RFC3339_ compatible, but still
  ISO8601_ compatible). During decoding, both string and timestamp-extension
  values are supported for flexibility.

- **YAML**: Datetimes are encoded using YAML's native datetime type. Both
  timezone-aware and timezone-naive datetimes are supported.

- **TOML**: Datetimes are encoded using TOML's native datetime type. Both
  timezone-aware and timezone-naive datetimes are supported.

Note that you can require a `datetime.datetime` object to be timezone-aware or
timezone-naive by specifying a ``tz`` constraint (see
:ref:`datetime-constraints` for more information).

.. code-block:: python

    >>> import datetime

    >>> tz = datetime.timezone(datetime.timedelta(hours=6))

    >>> tz_aware = datetime.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=tz)

    >>> msg = msgspec.json.encode(tz_aware)

    >>> msg
    b'"2021-04-02T18:18:10.000123+06:00"'

    >>> msgspec.json.decode(msg, type=datetime.datetime)
    datetime.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=datetime.timezone(datetime.timedelta(seconds=21600)))

    >>> tz_naive = datetime.datetime(2021, 4, 2, 18, 18, 10, 123)

    >>> msg = msgspec.json.encode(tz_naive)

    >>> msg
    b'"2021-04-02T18:18:10.000123"'

    >>> msgspec.json.decode(msg, type=datetime.datetime)
    datetime.datetime(2021, 4, 2, 18, 18, 10, 123)

    >>> msgspec.json.decode(b'"oops"', type=datetime.datetime)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid RFC3339 encoded datetime


Additionally, if ``strict=False`` is specified, all protocols will decode ints,
floats, or strings containing ints/floats as timezone-aware datetimes,
interpreting the value as seconds since the epoch in UTC (a `Unix Timestamp
<https://en.wikipedia.org/wiki/Unix_time>`__). See :ref:`strict-vs-lax` for
more information.

.. code-block:: python

    >>> msgspec.json.decode(b"1617405490.000123", type=datetime.datetime)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `datetime`, got `float`

    >>> msgspec.json.decode(b"1617405490.000123", type=datetime.datetime, strict=False)
    datetime.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=datetime.timezone.utc)

``date``
--------

`datetime.date` values map to:

- **JSON**: RFC3339_ encoded strings
- **MessagePack**: RFC3339_ encoded strings
- **YAML**: YAML's native date type
- **TOML** TOML's native date type

.. code-block:: python

    >>> import datetime

    >>> date = datetime.date(2021, 4, 2)

    >>> msg = msgspec.json.encode(date)

    >>> msg
    b'"2021-04-02"'

    >>> msgspec.json.decode(msg, type=datetime.date)
    datetime.date(2021, 4, 2)

    >>> msgspec.json.decode(b'"oops"', type=datetime.date)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid RFC3339 encoded date

``time``
--------

The encoding used for `datetime.time` objects is dependent on both the protocol
and whether these objects are timezone-aware_ or timezone-naive:

- **JSON**, **MessagePack**, and **YAML**: Timezone-aware times are encoded as
  RFC3339_ compatible strings. Timezone-naive times are encoded the same, but
  lack the timezone component (making them not strictly RFC3339_ compatible,
  but still ISO8601_ compatible).

- **TOML**: Timezone-naive times are encoded using TOML's native time type.
  Timezone-aware times aren't supported.

Note that you can require a `datetime.time` object to be timezone-aware or
timezone-naive by specifying a ``tz`` constraint (see
:ref:`datetime-constraints` for more information).

.. code-block:: python

    >>> import datetime

    >>> tz = datetime.timezone(datetime.timedelta(hours=6))

    >>> tz_aware = datetime.time(18, 18, 10, 123, tzinfo=tz)

    >>> msg = msgspec.json.encode(tz_aware)

    >>> msg
    b'"18:18:10.000123+06:00"'

    >>> msgspec.json.decode(msg, type=datetime.time)
    datetime.time(18, 18, 10, 123, tzinfo=datetime.timezone(datetime.timedelta(seconds=21600)))

    >>> tz_naive = datetime.time(18, 18, 10, 123)

    >>> msg = msgspec.json.encode(tz_naive)

    >>> msg
    b'"18:18:10.000123"'

    >>> msgspec.json.decode(msg, type=datetime.time)
    datetime.time(18, 18, 10, 123)

    >>> msgspec.json.decode(b'"oops"', type=datetime.time)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid RFC3339 encoded time

``timedelta``
-------------

`datetime.timedelta` values map to extended `ISO 8601 duration strings`_ in all
protocols.

The format as described in the ISO specification is fairly lax and a bit
underspecified, leading most real-world implementations to implement a stricter
subset.

The duration format used here is as follows:

.. code-block:: text

   [+/-]P[#D][T[#H][#M][#S]]

- The format starts with an optional sign (``-`` or ``+``). If negative, the
  whole duration is negated.

- The letter ``P`` follows (case insensitive)

- There are then four segments, each consisting of a number and unit. The units
  are ``D``, ``H``, ``M``, ``S`` (case insensitive) for days, hours, minutes,
  and seconds respectively. These segments must occur in this order.

  - If a segment would have a 0 value it may be omitted, with the caveat that at
    least one segment must be present.

  - If a time (hour, minute, or second) segment is present then the letter ``T``
    (case insensitive) must precede the first time segment. Likewise if a ``T``
    is present, there must be at least 1 segment after the ``T``.

  - Each segment is composed of 1 or more digits, followed by the unit. Leading
    0s are accepted. The *final* segment may include a decimal component if
    needed.

A few examples:

.. code-block:: python

   "P0D"                # 0 days
   "P1D"                # 1 Day
   "PT1H30S"            # 1 Hour and 30 minutes
   "PT1.5H"             # 1 Hour and 30 minutes
   "-PT1M30S"           # -90 seconds
   "PT1H30M25.5S"       # 1 Hour, 30 minutes, and 25.5 seconds

While msgspec will decode duration strings making use of the ``H`` (hour) or
``M`` (minute) units, durations encoded by msgspec will only consist of ``D``
(day) and ``S`` (second) segments.

The implementation in ``msgspec`` is compatible with the ones in:

- Java's ``time.Duration.parse`` (`docs <https://docs.oracle.com/en/java/javase/17/docs/api/java.base/java/time/Duration.html#parse(java.lang.CharSequence)>`__)
- Javascript's proposed ``Temporal.Duration`` standard API (`docs <https://tc39.es/proposal-temporal/docs/duration.html>`__)
- Python libraries like pendulum_ or pydantic_.

Duration strings produced by msgspec should be interchangeable with these
libraries, as well as similar ones in other language ecosystems.

.. code-block:: python

    >>> from datetime import timedelta

    >>> msgspec.json.encode(timedelta(seconds=123))
    b'"PT123S"'

    >>> msgspec.json.encode(timedelta(days=1, seconds=30, microseconds=123))
    b'"P1DT30.000123S"'

    >>> msgspec.json.decode(b'"PT123S"', type=timedelta)
    datetime.timedelta(seconds=123)

    >>> msgspec.json.decode(b'"PT1.5M"', type=timedelta)
    datetime.timedelta(seconds=90)

    >>> msgspec.json.decode(b'"oops"', type=datetime.timedelta)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid ISO8601 duration

Additionally, if ``strict=False`` is specified, all protocols will decode ints,
floats, or strings containing ints/floats as timedeltas, interpreting the value
as total seconds. See :ref:`strict-vs-lax` for more information.

.. code-block:: python

    >>> msgspec.json.decode(b"123.4", type=datetime.timedelta)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `duration`, got `float`

    >>> msgspec.json.decode(b"123.4", type=datetime.timedelta, strict=False)
    datetime.timedelta(seconds=123, microseconds=400000)

``uuid``
--------

`uuid.UUID` values are serialized as RFC4122_ encoded canonical strings in all
protocols by default. Subclasses of `uuid.UUID` are also supported for encoding
only.

.. code-block:: python

    >>> import uuid

    >>> u = uuid.UUID("c4524ac0-e81e-4aa8-a595-0aec605a659a")

    >>> msgspec.json.encode(u)
    b'"c4524ac0-e81e-4aa8-a595-0aec605a659a"'

    >>> msgspec.json.decode(b'"c4524ac0-e81e-4aa8-a595-0aec605a659a"', type=uuid.UUID)
    UUID('c4524ac0-e81e-4aa8-a595-0aec605a659a')

    >>> msgspec.json.decode(b'"oops"', type=uuid.UUID)
    Traceback (most recent call last):
        File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid UUID

Alternative formats are also supported by the JSON and MessagePack encoders.
The format may be selected by passing it to ``uuid_format`` when creating an
``Encoder``. The following options are supported:

- ``canonical``: UUIDs are encoded as RFC4122_ canonical strings (same as
  ``str(uuid)``). This is the default.
- ``hex``: UUIDs are encoded as RFC4122_ hex strings (same as ``uuid.hex``).
- ``bytes``: UUIDs are encoded as binary values of the uuid's big-endian
  128-bit integer representation (same as ``uuid.bytes``). This is only supported
  by the MessagePack encoder.

When decoding, any of the above formats are accepted.

.. code-block:: python

    >>> enc = msgspec.json.Encoder(uuid_format="hex")

    >>> uuid_hex = enc.encode(u)

    >>> uuid_hex
    b'"c4524ac0e81e4aa8a5950aec605a659a"'

    >>> msgspec.json.decode(uuid_hex, type=uuid.UUID)
    UUID('c4524ac0-e81e-4aa8-a595-0aec605a659a')

    >>> enc = msgspec.msgpack.Encoder(uuid_format="bytes")

    >>> uuid_bytes = enc.encode(u)

    >>> msgspec.msgpack.decode(uuid_bytes, type=uuid.UUID)
    UUID('c4524ac0-e81e-4aa8-a595-0aec605a659a')


``decimal``
-----------

`decimal.Decimal` values are encoded as their string representation in all
protocols by default. This ensures no precision loss during serialization, as
would happen with a float representation.

.. code-block:: python

    >>> import decimal

    >>> x = decimal.Decimal("1.2345")

    >>> msg = msgspec.json.encode(x)

    >>> msg
    b'"1.2345"'

    >>> msgspec.json.decode(msg, type=decimal.Decimal)
    Decimal('1.2345')

    >>> msgspec.json.decode(b'"oops"', type=decimal.Decimal)
    Traceback (most recent call last):
        File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid decimal string

For JSON and MessagePack you may instead encode decimal values the same as
numbers by creating a ``Encoder`` and specifying ``decimal_format='number'``.

.. code-block:: python

    >>> encoder = msgspec.json.Encoder(decimal_format="number")

    >>> encoder.encode(x)
    b'1.2345'

This setting is not yet supported for YAML or TOML - if this option is
important for you please `open an issue`_.

All protocols will also decode `decimal.Decimal` values from ``int`` or
``float`` inputs. For JSON the value is parsed directly from the serialized
bytes, avoiding any precision loss:

.. code-block:: python

   >>> msgspec.json.decode(b"1.3", type=decimal.Decimal)
   Decimal('1.3')

   >>> msgspec.json.decode(b"1.300", type=decimal.Decimal)
   Decimal('1.300')

   >>> msgspec.json.decode(b"0.1234567891234567811", type=decimal.Decimal)
   Decimal('0.1234567891234567811')

Other protocols will coerce float inputs to the shortest decimal value that
roundtrips back to the corresponding IEEE754 float representation (this is
effectively equivalent to ``decimal.Decimal(str(float_val))``). This may result
in precision loss for some inputs! In general we recommend avoiding parsing
`decimal.Decimal` values from anything but strings.

.. code-block:: python

   >>> msgspec.yaml.decode(b"1.3", type=decimal.Decimal)
   Decimal('1.3')

   >>> msgspec.yaml.decode(b"1.300", type=decimal.Decimal)  # trailing 0s truncated!
   Decimal('1.3')

   >>> msgspec.yaml.decode(b"0.1234567891234567811", type=decimal.Decimal)  # precision loss!
   Decimal('0.12345678912345678')


``list`` / ``tuple`` / ``set`` / ``frozenset``
----------------------------------------------

`list`, `tuple`, `set`, and `frozenset` objects map to arrays in all protocols.
An error is raised if the elements don't match the specified element type (if
provided).

Subclasses of these types are also supported for encoding only. To decode into
a ``list`` subclass you'll need to implement a ``dec_hook`` (see
:doc:`extending`).

.. code-block:: python

    >>> msgspec.json.encode([1, 2, 3])
    b'[1,2,3]'

    >>> msgspec.json.encode({1, 2, 3})
    b'[1,2,3]'

    >>> msgspec.json.decode(b'[1,2,3]', type=set)
    {1, 2, 3}

    >>> from typing import Set

    >>> # Decode as a set of ints
    ... msgspec.json.decode(b'[1, 2, 3]', type=Set[int])
    {1, 2, 3}

    >>> # Oops, all elements should be ints
    ... msgspec.json.decode(b'[1, 2, "oops"]', type=Set[int])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$[2]`

``NamedTuple``
--------------

`typing.NamedTuple` types map to arrays in all protocols.  An error is raised
during decoding if the type doesn't match or if any required fields are
missing.

Note that ``msgspec`` supports both `typing.NamedTuple` and
`collections.namedtuple`, although the latter lacks a way to specify field
types.

When possible we recommend using `msgspec.Struct` (possibly with
``array_like=True`` and ``frozen=True``) instead of ``NamedTuple`` for
specifying schemas - :doc:`structs` are faster, more ergonomic, and support
additional features.  Still, you may want to use a ``NamedTuple`` if you're
already using them elsewhere, or if you have downstream code that requires a
``tuple`` instead of an object.

.. code-block:: python

    >>> from typing import NamedTuple

    >>> class Person(NamedTuple):
    ...     name: str
    ...     age: int

    >>> ben = Person("ben", 25)

    >>> msg = msgspec.json.encode(ben)

    >>> msgspec.json.decode(msg, type=Person)
    Person(name='ben', age=25)

    >>> wrong_type = b'["chad", "twenty"]'

    >>> msgspec.json.decode(wrong_type, type=Person)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$[1]`

Other types that duck-type as ``NamedTuple`` (for example
`edgedb NamedTuples <https://www.edgedb.com/docs/clients/python/api/types#named-tuples>`__)
are also supported.

.. code-block:: python

    >>> import edgedb

    >>> client = edgedb.create_client()

    >>> alice = client.query_single(
    ...     "SELECT (name := 'Alice', dob := <cal::local_date>'1984-03-01')"
    ... )

    >>> alice
    (name := 'Alice', dob := datetime.date(1984, 3, 1))

    >>> msgspec.json.encode(alice)
    b'["Alice","1984-03-01"]'

``dict``
--------

Dicts encode/decode as objects/maps in all protocols.

Dict subclasses (`collections.OrderedDict`, for example) are also supported for
encoding only. To decode into a ``dict`` subclass you'll need to implement a
``dec_hook`` (see :doc:`extending`).

JSON and TOML only support key types that encode as strings or numbers (for
example `str`, `int`, `float`, `enum.Enum`, `datetime.datetime`, `uuid.UUID`,
...). MessagePack and YAML support any hashable for the key type.

An error is raised during decoding if the keys or values don't match their
respective types (if specified).

.. code-block:: python

    >>> msgspec.json.encode({"x": 1, "y": 2})
    b'{"x":1,"y":2}'

    >>> from typing import Dict

    >>> # Decode as a Dict of str -> int
    ... msgspec.json.decode(b'{"x":1,"y":2}', type=Dict[str, int])
    {"x": 1, "y": 2}

    >>> # Oops, there's a mistyped value
    ... msgspec.json.decode(b'{"x":1,"y":"oops"}', type=Dict[str, int])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$[...]`

``TypedDict``
-------------

`typing.TypedDict` provides a way to specify different types for different
values in a ``dict``, rather than a single value type (the ``int`` in
``Dict[str, int]``, for example).  At runtime these are just standard
``dict`` types, the ``TypedDict`` type is only there to provide the schema
information during decoding. Note that ``msgspec`` supports both
`typing.TypedDict` and ``typing_extensions.TypedDict`` (a backport).

`typing.TypedDict` types map to objects/maps in all protocols. During decoding,
any extra fields are ignored. An error is raised during decoding if the type
doesn't match or if any required fields are missing.

When possible we recommend using `msgspec.Struct` instead of ``TypedDict`` for
specifying schemas - :doc:`structs` are faster, more ergonomic, and support
additional features. Still, you may want to use a ``TypedDict`` if you're
already using them elsewhere, or if you have downstream code that requires a
``dict`` instead of an object.

.. code-block:: python

    >>> from typing import TypedDict

    >>> class Person(TypedDict):
    ...     name: str
    ...     age: int

    >>> ben = {"name": "ben", "age": 25}

    >>> msg = msgspec.json.encode(ben)

    >>> msgspec.json.decode(msg, type=Person)
    {'name': 'ben', 'age': 25}

    >>> wrong_type = b'{"name": "chad", "age": "twenty"}'

    >>> msgspec.json.decode(wrong_type, type=Person)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$.age`

``dataclasses``
---------------

`dataclasses` map to objects/maps in all protocols.

During decoding, any extra fields are ignored. An error is raised if a field's
type doesn't match or if any required fields are missing.

If a ``__post_init__`` method is defined on the dataclass, it is called after
the object is decoded. Note that `"Init-only parameters"
<https://docs.python.org/3/library/dataclasses.html#init-only-variables>`__
(i.e. ``InitVar`` fields) are _not_ supported.

When possible we recommend using `msgspec.Struct` instead of dataclasses for
specifying schemas - :doc:`structs` are faster, more ergonomic, and support
additional features.

.. code-block:: python

    >>> from dataclasses import dataclass

    >>> @dataclass
    ... class Person:
    ...     name: str
    ...     age: int

    >>> carol = Person(name="carol", age=32)

    >>> msg = msgspec.json.encode(carol)

    >>> msgspec.json.decode(msg, type=Person)
    Person(name='carol', age=32)

    >>> wrong_type = b'{"name": "doug", "age": "thirty"}'

    >>> msgspec.json.decode(wrong_type, type=Person)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$.age`

Other types that duck-type as ``dataclasses`` (for example
`edgedb Objects <https://www.edgedb.com/docs/clients/python/api/types#objects>`__ or
`pydantic dataclasses <https://docs.pydantic.dev/latest/usage/dataclasses/>`__)
are also supported.

.. code-block:: python

    >>> import edgedb

    >>> client = edgedb.create_client()

    >>> alice = client.query_single(
    ...     "SELECT User {name, dob} FILTER .name = <str>$name LIMIT 1",
    ...     name="Alice"
    ... )

    >>> alice
    Object{name := 'Alice', dob := datetime.date(1984, 3, 1)}

    >>> msgspec.json.encode(alice)
    b'{"id":"a6b951cc-2d00-11ee-91aa-b3f17e9898ce","name":"Alice","dob":"1984-03-01"}'

For a more complete example using EdgeDB, see :doc:`examples/edgedb`.

``attrs``
---------

attrs_ types map to objects/maps in all protocols.

During encoding, all attributes without a leading underscore (``"_"``) are
encoded.

During decoding, any extra fields are ignored. An error is raised if a field's
type doesn't match or if any required fields are missing.

If the ``__attrs_pre_init__`` or ``__attrs_post_init__`` methods are defined on
the class, they are called as part of the decoding process. Likewise, if a
class makes use of attrs' `validators
<https://www.attrs.org/en/stable/examples.html#validators>`__, the validators
will be called, and a `msgspec.ValidationError` raised on error. Note that
attrs' `converters
<https://www.attrs.org/en/stable/examples.html#conversion>`__ are not currently
supported.

When possible we recommend using `msgspec.Struct` instead of attrs_ types for
specifying schemas - :doc:`structs` are faster, more ergonomic, and support
additional features.

.. code-block:: python

    >>> from attrs import define

    >>> @define
    ... class Person:
    ...     name: str
    ...     age: int

    >>> carol = Person(name="carol", age=32)

    >>> msg = msgspec.json.encode(carol)

    >>> msgspec.json.decode(msg, type=Person)
    Person(name='carol', age=32)

    >>> wrong_type = b'{"name": "doug", "age": "thirty"}'

    >>> msgspec.json.decode(wrong_type, type=Person)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$.age`

``Struct``
----------

Structs are the preferred way of defining structured data types in ``msgspec``.
You can think of them as similar to dataclasses_/attrs_/pydantic_, but much
faster to create/compare/encode/decode. For more information, see the
:doc:`structs` page.

By default `msgspec.Struct` types map to objects/maps in all protocols. During
decoding, any unknown fields are ignored (this can be disabled, see
:ref:`forbid-unknown-fields`), and any missing optional fields have their
default values applied. An error is raised during decoding if the type doesn't
match or if any required fields are missing.

.. code-block:: python

    >>> from typing import Set, Optional

    >>> class User(msgspec.Struct):
    ...     name: str
    ...     groups: Set[str] = set()
    ...     email: Optional[str] = None

    >>> alice = User("alice", groups={"admin", "engineering"})

    >>> msgspec.json.encode(alice)
    b'{"name":"alice","groups":["admin","engineering"],"email":null}'

    >>> msg = b"""
    ... {
    ...     "name": "bob",
    ...     "email": "bob@company.com",
    ...     "unknown_field": [1, 2, 3]
    ... }
    ... """

    >>> msgspec.json.decode(msg, type=User)
    User(name='bob', groups=[], email="bob@company.com")

    >>> wrong_type = b"""
    ... {
    ...     "name": "bob",
    ...     "groups": ["engineering", 123]
    ... }
    ... """

    >>> msgspec.json.decode(wrong_type, type=User)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `str`, got `int` - at `$.groups[1]`

If you pass ``array_like=True`` when defining the struct type, they're instead
treated as array types during encoding/decoding. In this case fields are
serialized in their :ref:`field order <struct-field-ordering>`. This can
further improve performance at the cost of less human readable messaging. Like
``array_like=False`` (the default) structs, extra (trailing) fields are ignored
during decoding, and any missing optional fields have their defaults applied.
Type checking also still applies.

.. code-block:: python

    >>> from typing import Set, Optional

    >>> class User(msgspec.Struct, array_like=True):
    ...     name: str
    ...     groups: Set[str] = set()
    ...     email: Optional[str] = None

    >>> alice = User("alice", groups={"admin", "engineering"})

    >>> msgspec.json.encode(alice)
    b'["alice",["admin","engineering"],null]'

    >>> msgspec.json.decode(b'["bob"]', type=User)
    User(name="bob", groups=[], email=None)

    >>> msgspec.json.decode(b'["carol", ["admin"], null, ["extra", "field"]]', type=User)
    User(name="carol", groups=["admin"], email=None)

    >>> msgspec.json.decode(b'["david", ["finance", 123]]')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `str`, got `int` - at `$[1][1]`

.. _unset-type:

``UNSET``
---------

`msgspec.UNSET` is a singleton object used to indicate that a field has no set
value. This is useful for cases where you need to differentiate between a
message where a field is missing and a message where the field is explicitly
``None``.

.. code-block:: python

    >>> from msgspec import Struct, UnsetType, UNSET, json

    >>> class Example(Struct):
    ...     x: int
    ...     y: int | None | UnsetType = UNSET  # a field, defaulting to UNSET

During encoding, any field containing ``UNSET`` is omitted from the message.

.. code-block:: python

    >>> json.encode(Example(1))  # y is UNSET
    b'{"x":1}'

    >>> json.encode(Example(1, UNSET))  # y is UNSET
    b'{"x":1}'

    >>> json.encode(Example(1, None))  # y is None
    b'{"x":1,"y":null}'

    >>> json.encode(Example(1, 2))  # y is 2
    b'{"x":1,"y":2}'

During decoding, if a field isn't explicitly set in the message, the default
value of ``UNSET`` will be set instead. This lets downstream consumers
determine whether a field was left unset, or explicitly set to ``None``

.. code-block:: python

    >>> json.decode(b'{"x": 1}', type=Example)  # y defaults to UNSET
    Example(x=1, y=UNSET)

    >>> json.decode(b'{"x": 1, "y": null}', type=Example)  # y is None
    Example(x=1, y=None)

    >>> json.decode(b'{"x": 1, "y": 2}', type=Example)  # y is 2
    Example(x=1, y=2)

``UNSET`` fields are supported for `msgspec.Struct`, `dataclasses`, and attrs_
types. It is an error to use `msgspec.UNSET` or `msgspec.UnsetType` anywhere
other than a field for one of these types.

``Enum`` / ``IntEnum`` / ``StrEnum``
------------------------------------

Enum types (`enum.Enum`, `enum.IntEnum`, `enum.StrEnum`, ...) encode as their
member *values* in all protocols.

Any enum whose *value* is a supported type may be encoded, but only enums
composed of all string or all integer values may be decoded.

An error is raised during decoding if the value isn't the proper type, or
doesn't match any valid member.

.. code-block:: python

    >>> import enum

    >>> class Fruit(enum.Enum):
    ...     APPLE = "apple"
    ...     BANANA = "banana"

    >>> msgspec.json.encode(Fruit.APPLE)
    b'"apple"'

    >>> msgspec.json.decode(b'"apple"', type=Fruit)
    <Fruit.APPLE: 'apple'>

    >>> msgspec.json.decode(b'"grape"', type=Fruit)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid enum value 'grape'

    >>> class JobState(enum.IntEnum):
    ...     CREATED = 0
    ...     RUNNING = 1
    ...     SUCCEEDED = 2
    ...     FAILED = 3

    >>> msgspec.json.encode(JobState.RUNNING)
    b'1'

    >>> msgspec.json.decode(b'2', type=JobState)
    <JobState.SUCCEEDED: 2>

    >>> msgspec.json.decode(b'4', type=JobState)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid enum value 4

If the enum type includes a ``_missing_`` method (`docs
<https://docs.python.org/3/library/enum.html#enum.Enum._missing_>`__), this
method will be called to handle any missing values. It should return a valid
enum member, or ``None`` if the value is invalid. One potential use case of
this is supporting case-insensitive enums:

.. code-block:: python

    >>> import enum

    >>> class Fruit(enum.Enum):
    ...     APPLE = "apple"
    ...     BANANA = "banana"
    ...
    ...     @classmethod
    ...     def _missing_(cls, name):
    ...         """Called to handle missing enum values"""
    ...         # Normalize value to lowercase
    ...         value = name.lower()
    ...         # Return valid enum value, or None if invalid
    ...         return cls._value2member_map_.get(value)

    >>> msgspec.json.decode(b'"apple"', type=Fruit)
    <Fruit.APPLE: "apple">

    >>> msgspec.json.decode(b'"ApPlE"', type=Fruit)
    <Fruit.APPLE: "apple">

    >>> msgspec.json.decode(b'"grape"', type=Fruit)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid enum value 'grape'

``Literal``
-----------

`typing.Literal` types can be used to ensure that a decoded object is within a
set of valid values. An `enum.Enum` or `enum.IntEnum` can be used for the same
purpose, but with a `typing.Literal` the decoded values are literal `int` or
`str` instances rather than `enum` objects.

A literal can be composed of any of the following objects:

- `None`
- `int` values
- `str` values
- Nested `typing.Literal` types

An error is raised during decoding if the value isn't in the set of valid
values, or doesn't match any of their component types.

.. code-block:: python

    >>> from typing import Literal

    >>> msgspec.json.decode(b'1', type=Literal[1, 2, 3])
    1

    >>> msgspec.json.decode(b'"one"', type=Literal["one", "two", "three"])
    'one'

    >>> msgspec.json.decode(b'4', type=Literal[1, 2, 3])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid enum value 4

    >>> msgspec.json.decode(b'"bad"', type=Literal[1, 2, 3])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str`

``NewType``
-----------

`typing.NewType` types are treated identically to their base type. Their
support here is purely to aid static analysis tools like mypy_ or pyright_.

.. code-block:: python

    >>> from typing import NewType

    >>> UserId = NewType("UserId", int)

    >>> msgspec.json.encode(UserId(1234))
    b'1234'

    >>> msgspec.json.decode(b'1234', type=UserId)
    1234

    >>> msgspec.json.decode(b'"oops"', type=UserId)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str`

Type Aliases
------------

For complex types, sometimes it can be nice to write the type once so you can
reuse it later.

.. code-block:: python

    Point = tuple[float, float]

Here ``Point`` is a "type alias" for ``tuple[float, float]`` - ``msgspec``
will substitute in ``tuple[float, float]`` whenever the ``Point`` type
is used in an annotation.

``msgspec`` supports the following equivalent forms:

.. code-block:: python

    # Using variable assignment
    Point = tuple[float, float]

    # Using variable assignment, annotated as a `TypeAlias`
    Point: TypeAlias = tuple[float, float]

    # Using Python 3.12's new `type` statement. This only works on Python 3.12+
    type Point = tuple[float, float]

To learn more about Type Aliases, see Python's `Type Alias docs here
<https://docs.python.org/3/library/typing.html#type-aliases>`__.

Generic Types
-------------

``msgspec`` supports generic types, including `user-defined generic types`_
based on any of the following types:

- `msgspec.Struct`
- `dataclasses`
- `attrs`
- `typing.TypedDict`
- `typing.NamedTuple`

Generic types may be useful for reusing common message structures.

To define a generic type:

- Define one or more type variables (`typing.TypeVar`) to parametrize your type with.
- Add `typing.Generic` as a base class when defining your type, parametrizing
  it by the relevant type variables.
- When annotating the field types, use the relevant type variables instead of
  "concrete" types anywhere you want to be generic.

For example, here we define a generic ``Paginated`` struct type for storing
extra pagination information in an API response.

.. code-block:: python

    import msgspec
    from typing import Generic, TypeVar

    # A type variable for the item type
    T = TypeVar("T")

    class Paginated(msgspec.Struct, Generic[T]):
        """A generic paginated API wrapper, parametrized by the item type."""
        page: int        # The current page number
        per_page: int    # Number of items per page
        total: int       # The total number of items found
        items: list[T]   # Items returned, up to `per_page` in length

This type is generic over the type of item contained in ``Paginated.items``.
This ``Paginated`` wrapper may then be used to decode a message containing a
specific item type by parametrizing it with that type. When processing a
generic type, the parametrized types are substituted for the type variables.

Here we define a ``User`` type, then use it to decode a paginated API response
containing a list of users:

.. code-block:: python

    class User(msgspec.Struct):
        """A user model"""
        name: str
        groups: list[str] = []

    json_str = """
    {
        "page": 1,
        "per_page": 5,
        "total": 252,
        "items": [
            {"name": "alice", "groups": ["admin"]},
            {"name": "ben"},
            {"name": "carol", "groups": ["engineering"]},
            {"name": "dan", "groups": ["hr"]},
            {"name": "ellen", "groups": ["engineering"]}
        ]
    }
    """

    # Decode a paginated response containing a list of users
    msg = msgspec.json.decode(json_str, type=Paginated[User])
    print(msg)
    #> Paginated(
    #>     page=1, per_page=5, total=252,
    #>     items=[
    #>         User(name='alice', groups=['admin']),
    #>         User(name='ben', groups=[]),
    #>         User(name='carol', groups=['engineering']),
    #>         User(name='dan', groups=['hr']),
    #>         User(name='ellen', groups=['engineering'])
    #>     ]
    #> )

If instead we wanted to decode a paginated response of another type (say
``Team``), we could do this by parametrizing ``Paginated`` with a different
type.

.. code-block:: python

    # Decode a paginated response containing a list of teams
    msgspec.json.decode(some_other_message, type=Paginated[Team])

Any unparametrized type variables will be treated as `typing.Any` when decoding.

.. code-block:: python

    # These are equivalent.
    # The unparametrized version substitutes in `Any` for `T`
    msgspec.json.decode(some_other_message, type=Paginated)
    msgspec.json.decode(some_other_message, type=Paginated[Any])

However, if an unparametrized type variable has a ``bound`` (`docs
<https://peps.python.org/pep-0484/#type-variables-with-an-upper-bound>`__),
then the bound type will be used instead.

.. code-block:: python

    from collections.abc import Sequence
    S = TypeVar("S", bound=Sequence)  # Can be any sequence type

    class Example(msgspec.Struct, Generic[S]):
        value: S

    msg = b'{"value": [1, 2, 3]}'

    # These are equivalent.
    # The unparametrized version substitutes in `Sequence` for `S`
    msgspec.json.decode(some_other_message, type=Example)
    msgspec.json.decode(some_other_message, type=Example[Sequence])

See the official Python docs on `generic types`_ and the `corresponding PEP
<https://peps.python.org/pep-0484/#generics>`__ for more information.

Abstract Types
--------------

``msgspec`` supports several "abstract" types, decoding them as
instances of their most common concrete type.

**Decoded as lists**

- `collections.abc.Collection` / `typing.Collection`
- `collections.abc.Sequence` / `typing.Sequence`
- `collections.abc.MutableSequence` / `typing.MutableSequence`

**Decoded as sets**

- `collections.abc.Set` / `typing.AbstractSet`
- `collections.abc.MutableSet` / `typing.MutableSet`

**Decoded as dicts**

- `collections.abc.Mapping` / `typing.Mapping`
- `collections.abc.MutableMapping` / `typing.MutableMapping`

.. code-block:: python

    >>> from typing import MutableMapping

    >>> msgspec.json.decode(b'{"x": 1}', type=MutableMapping[str, int])
    {"x": 1}

    >>> msgspec.json.decode(b'{"x": "oops"}', type=MutableMapping[str, int])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$[...]`

``Union`` /  ``Optional``
-------------------------

Type unions are supported, with a few restrictions. These restrictions are in
place to remove any ambiguity during decoding - given an encoded value there
must always be a single type in a given `typing.Union` that can decode that
value.

Union restrictions are as follows:

- Unions may contain at most one type that encodes to an integer (`int`,
  `enum.IntEnum`)

- Unions may contain at most one type that encodes to a string (`str`,
  `enum.Enum`, `bytes`, `bytearray`, `datetime.datetime`, `datetime.date`,
  `datetime.time`, `uuid.UUID`, `decimal.Decimal`). Note that this restriction
  is fixable with some work, if this is a feature you need please `open an issue`_.

- Unions may contain at most one type that encodes to an object (`dict`,
  `typing.TypedDict`, dataclasses_, attrs_, `Struct` with ``array_like=False``)

- Unions may contain at most one type that encodes to an array (`list`,
  `tuple`, `set`, `frozenset`, `typing.NamedTuple`, `Struct` with
  ``array_like=True``).

- Unions may contain at most one *untagged* `Struct` type. Unions containing
  multiple struct types are only supported through :ref:`struct-tagged-unions`.

- Unions with custom types are unsupported beyond optionality (i.e.
  ``Optional[CustomType]``)

.. code-block:: python

    >>> from typing import Union, List

    >>> # A decoder expecting either an int, a str, or a list of strings
    ... decoder = msgspec.json.Decoder(Union[int, str, List[str]])

    >>> decoder.decode(b'1')
    1

    >>> decoder.decode(b'"two"')
    "two"

    >>> decoder.decode(b'["three", "four"]')
    ["three", "four"]

    >>> decoder.decode(b'false')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int | str | array`, got `bool`

``Raw``
-------

`msgspec.Raw` is a buffer-like type containing an already encoded messages.
They have two common uses:

**1. Avoiding unnecessary encoding cost**

Wrapping an already encoded buffer in `msgspec.Raw` lets the encoder avoid
re-encoding the message, instead it will simply be copied to the output buffer.
This can be useful when part of a message already exists in an encoded format
(e.g. reading JSON bytes from a database and returning them as part of a larger
message).

.. code-block:: python

    >>> import msgspec

    >>> # Create a new `Raw` object wrapping a pre-encoded message
    ... fragment = msgspec.Raw(b'{"x": 1, "y": 2}')

    >>> # Compose a larger message containing the pre-encoded fragment
    ... msg = {"a": 1, "b": fragment}

    >>> # During encoding, the raw message is efficiently copied into
    ... # the output buffer, avoiding any extra encoding cost
    ... msgspec.json.encode(msg)
    b'{"a":1,"b":{"x": 1, "y": 2}}'


**2. Delaying decoding of part of a message**

Sometimes the type of a serialized value depends on the value of other fields
in a message. ``msgspec`` provides an optimized version of one common pattern
(:ref:`struct-tagged-unions`), but if you need to do something more complicated
you may find using `msgspec.Raw` useful here.

For example, here we demonstrate how to decode a message where the type of one
field (``point``) depends on the value of another (``dimensions``).

.. code-block:: python

    >>> import msgspec

    >>> from typing import Union

    >>> class Point1D(msgspec.Struct):
    ...     x: int

    >>> class Point2D(msgspec.Struct):
    ...     x: int
    ...     y: int

    >>> class Point3D(msgspec.Struct):
    ...     x: int
    ...     y: int
    ...     z: int

    >>> class Model(msgspec.Struct):
    ...     dimensions: int
    ...     point: msgspec.Raw  # use msgspec.Raw to delay decoding the point field

    >>> def decode_point(msg: bytes) -> Union[Point1D, Point2D, Point3D]:
    ...     """A function for efficiently decoding the `point` field"""
    ...     # First decode the outer `Model` struct. Decoding of the `point`
    ...     # field is delayed, with the composite bytes stored as a `Raw` object
    ...     # on `point`.
    ...     model = msgspec.json.decode(msg, type=Model)
    ...
    ...     # Based on the value of `dimensions`, determine which type to use
    ...     # when decoding the `point` field
    ...     if model.dimensions == 1:
    ...         point_type = Point1D
    ...     elif model.dimensions == 2:
    ...         point_type = Point2D
    ...     elif model.dimensions == 3:
    ...         point_type = Point3D
    ...     else:
    ...         raise ValueError("Too many dimensions!")
    ...
    ...     # Now that we know the type of `point`, we can finish decoding it.
    ...     # Note that `Raw` objects are buffer-like, and can be passed
    ...     # directly to the `decode` method.
    ...     return msgspec.json.decode(model.point, type=point_type)

    >>> decode_point(b'{"dimensions": 2, "point": {"x": 1, "y": 2}}')
    Point2D(x=1, y=2)

    >>> decode_point(b'{"dimensions": 3, "point": {"x": 1, "y": 2, "z": 3}}')
    Point3D(x=1, y=2, z=3)


``Any``
-------

When decoding a message with `Any` type (or no type specified), encoded types
map to Python types in a protocol specific manner.

**JSON**

JSON_ types are decoded to Python types as follows:

- ``null``: `None`
- ``bool``: `bool`
- ``string``: `str`
- ``number``: `int` or `float` [#number_json]_
- ``array``: `list`
- ``object``: `dict`

.. [#number_json] Numbers are decoded as integers if they contain no decimal or
   exponent components (e.g. ``1`` but not ``1.0`` or ``1e10``). All other
   numbers decode as floats.

**MessagePack**

MessagePack_ types are decoded to Python types as follows:

- ``nil``: `None`
- ``bool``: `bool`
- ``int``: `int`
- ``float``: `float`
- ``str``: `str`
- ``bin``: `bytes`
- ``array``: `list` or `tuple` [#tuple]_
- ``map``: `dict`
- ``ext``: `msgspec.msgpack.Ext`, `datetime.datetime`, or a custom type

.. [#tuple] Tuples are only used when the array type must be hashable (e.g.
   keys in a ``dict`` or ``set``). All other array types are deserialized as lists
   by default.

**YAML**

YAML_ types are decoded to Python types as follows:

- ``null``: `None`
- ``bool``: `bool`
- ``string``: `str`
- ``int``: `int`
- ``float``: `float`
- ``array``: `list`
- ``object``: `dict`
- ``timestamp``: `datetime.datetime`
- ``date``: `datetime.date`

**TOML**

TOML_ types are decoded to Python types as follows:

- ``bool``: `bool`
- ``string``: `str`
- ``int``: `int`
- ``float``: `float`
- ``array``: `list`
- ``table``: `dict`
- ``datetime``: `datetime.datetime`
- ``date``: `datetime.date`
- ``time``: `datetime.time`


.. _type annotations: https://docs.python.org/3/library/typing.html
.. _JSON: https://json.org
.. _MessagePack: https://msgpack.org
.. _YAML: https://yaml.org
.. _TOML: https://toml.io
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _pendulum: https://pendulum.eustace.io/
.. _RFC8259: https://datatracker.ietf.org/doc/html/rfc8259
.. _RFC3339: https://datatracker.ietf.org/doc/html/rfc3339
.. _RFC4122: https://datatracker.ietf.org/doc/html/rfc4122
.. _ISO8601: https://en.wikipedia.org/wiki/ISO_8601
.. _timestamp extension: https://github.com/msgpack/msgpack/blob/master/spec.md#timestamp-extension-type
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _attrs: https://www.attrs.org/en/stable/index.html
.. _timezone-aware: https://docs.python.org/3/library/datetime.html#aware-and-naive-objects
.. _mypy: https://mypy.readthedocs.io
.. _pyright: https://github.com/microsoft/pyright
.. _generic types:
.. _user-defined generic types: https://docs.python.org/3/library/typing.html#user-defined-generic-types
.. _open an issue: https://github.com/nightsailer/msgspec-x/issues>
.. _ISO 8601 duration strings: https://en.wikipedia.org/wiki/ISO_8601#Durations

- ``msgspec`` is used by many organizations and `open source projects
  <https://github.com/nightsailer/msgspec-x/network/dependents>`__, here we highlight a
  few:
