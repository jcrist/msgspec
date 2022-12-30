Usage
=====

.. currentmodule:: msgspec

``msgspec`` supports multiple serialization protocols, accessed through
separate submodules:

- ``msgspec.json`` (JSON_)
- ``msgspec.msgpack`` (MessagePack_)

Each supports a consistent interface, making it simple to switch between
protocols as needed.

Encoding
--------

Each submodule has an ``encode`` method for encoding Python objects using the
respective protocol.

.. code-block:: python

    >>> import msgspec

    >>> # Encode as JSON
    ... msgspec.json.encode({"hello": "world"})
    b'{"hello":"world"}'

    >>> # Encode as msgpack
    ... msgspec.msgpack.encode({"hello": "world"})
    b'\x81\xa5hello\xa5world'

Note that if you're making multiple calls to ``encode``, it's more efficient to
create an ``Encoder`` once and use the ``Encoder.encode`` method instead.

.. code-block:: python

    >>> import msgspec

    >>> # Create a JSON encoder
    ... encoder = msgspec.json.Encoder()

    >>> # Encode as JSON using the encoder
    ... encoder.encode({"hello": "world"})
    b'{"hello":"world"}'

Decoding
--------

Each submodule has ``decode`` method for decoding messages using the respective
protocol.

.. code-block:: python

    >>> import msgspec

    >>> # Decode JSON
    ... msgspec.json.decode(b'{"hello":"world"}')
    {'hello': 'world'}

    >>> # Decode msgpack
    ... msgspec.msgpack.decode(b'\x81\xa5hello\xa5world')
    {'hello': 'world'}

Note that if you're making multiple calls to ``decode``, it's more efficient to
create a ``Decoder`` once and use the ``Decoder.decode`` method instead.

.. code-block:: python

    >>> import msgspec

    >>> # Create a JSON decoder
    ... decoder = msgspec.json.Decoder()

    >>> # Decode JSON using the decoder
    ... decoder.decode(b'{"hello":"world"}')
    {'hello': 'world'}


.. _typed-deserialization:

Typed Decoding
--------------

``msgspec`` optionally supports specifying the expected output types during
decoding. This serves a few purposes:

- Often serialized data has a fixed schema (e.g. a request handler in a REST
  api expects a certain JSON structure). Specifying the expected types allows
  ``msgspec`` to perform validation during decoding, with *no* added runtime
  cost.

- Python has a much richer type system than serialization protocols like JSON_
  or MessagePack_. Specifying the output types lets ``msgspec`` decode messages
  into types other than the defaults described above (e.g. decoding JSON
  objects into a `msgspec.Struct` (see :doc:`structs`) instead of the default
  `dict`).

``msgspec`` uses Python `type annotations`_ to describe the expected types.
Most combinations of the following types are supported (see
:ref:`supported-types` for a few restrictions):

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
- `datetime.datetime`
- `datetime.date`
- `datetime.time`
- `uuid.UUID`
- `typing.Any`
- `typing.Optional`
- `typing.Union`
- `typing.Literal`
- `typing.NamedTuple` / `collections.namedtuple`
- `typing.TypedDict`
- `dataclasses.dataclass` types
- `msgspec.msgpack.Ext`
- `msgspec.Raw`
- `enum.Enum` subclasses
- `enum.IntEnum` subclasses
- `msgspec.Struct` subclasses
- `dict` subclasses (encoding only)
- `list` subclasses (encoding only)
- `tuple` subclasses (encoding only)
- `set` subclasses (encoding only)
- `frozenset` subclasses (encoding only)
- Custom types (see :doc:`extending`)

To specify the expected type, you can pass it to ``decode``, or when creating a
``Decoder`` (more efficient than calling ``decode`` multiple times).

.. code-block:: python

    >>> import msgspec

    >>> from typing import List, Optional

    >>> # Define a type for describing a user
    ... class User(msgspec.Struct):
    ...     name: str
    ...     groups: List[str] = []
    ...     email: Optional[str] = None

    >>> # Decode a User from JSON
    ... msgspec.json.decode(
    ...     b'{"name": "bob", "email": "bob@company.com"}',
    ...     type=User
    ... )
    User(name='bob', groups=[], email="bob@company.com")

    >>> # Create a decoder that expects a list of users
    ... decoder = msgspec.json.Decoder(List[User])

    >>> # Decode a list of users from JSON
    .... decoder.decode(
    ...     b"""[
    ...       {"name": "bob", "email": "bob@company.com"},
    ...       {"name": "carol", "groups": ["admin"]}
    ...     ]"""
    ... )
    [User(name='bob', groups=[], email='bob@company.com'),
     User(name='carol', groups=['admin'], email=None)]

If a message doesn't match the expected type, an error is raised.

.. code-block:: python

    >>> decoder.decode(
    ...     b"""[
    ...       {"name": "darla", "email": "darla@company.com"},
    ...       {"name": "eric", "groups": ["admin", 123]}
    ...     ]"""
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `str`, got `int` - at `$[1].groups[1]`

Unlike some other libraries (e.g. pydantic_), ``msgspec`` won't perform any
unsafe implicit conversion. For example, if an integer is specified and a
string is decoded instead, an error is raised rather than attempting to cast
the string to an int.

.. code-block:: python

    >>> msgspec.json.decode(b'[1, 2, "3"]', type=List[int])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$[2]`

The *one exception* to this rule is float handling - if a `float` is specified
and an integer is decoded, the integer will be converted to a float. This is
done to play nicer with JSON, which makes no distinction between integer and
floating point numbers.

.. code-block:: python

    >>> msgspec.json.decode(b'[1.5, 2.5, 3]', type=List[float])
    [1.5, 2.5, 3.0]

.. _supported-types:

Supported Types
---------------

Here we document how msgspec maps Python objects to/from the JSON_/MessagePack_
protocols.

Note that except where explicitly stated, subclasses of these types are not
supported by default (see :doc:`extending` for how to add support yourself).

``None``
~~~~~~~~

`None` maps to ``null`` in JSON/``nil`` in MessagePack.

.. code-block:: python

    >>> msgspec.json.encode(None)
    b'null'

    >>> msgspec.json.decode(b'null')
    None

``bool``
~~~~~~~~

Booleans map to their corresponding ``true``/``false`` values in both JSON and
MessagePack.

.. code-block:: python

    >>> msgspec.json.encode(True)
    b'true'

    >>> msgspec.json.decode(b'true')
    True

``int``
~~~~~~~

Integers map to JSON numbers/MessagePack integers. Only values that fit in an
``int64`` or ``uint64`` (within ``[-2**63, 2**64 - 1]``, inclusive) are
supported. Values outside this range will raise a `msgspec.ValidationError`
during decoding.

.. code-block:: python

    >>> msgspec.json.encode(123)
    b"123"

    >>> msgspec.json.decode(b"123", type=int)
    123


``float``
~~~~~~~~~

Floats map to JSON numbers/MessagePack floats. Note that per RFC8259_, JSON
doesn't support nonfinite numbers (``nan``, ``infinity``, ``-infinity``);
``msgspec.json`` handles this by encoding these values as ``null``.
``msgspec.msgpack`` lacks this restriction, and can accurately roundtrip any
IEEE754 64 bit floating point value.

For all decoders, if a `float` type is specified and an `int` value is
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

``str``
~~~~~~~

Strings map to JSON or MessagePack strings.

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
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Bytes-like objects map to base64-encoded strings in JSON, and the ``bin``
type in MessagePack.

.. code-block:: python

    >>> msg = msgspec.json.encode(b"\xf0\x9d\x84\x9e")

    >>> msg
    b'"85+Eng=="'

    >>> msgspec.json.decode(msg, type=bytes)
    b'\xf0\x9d\x84\x9e'

    >>> msgspec.json.decode(msg, type=bytearray)
    bytearray(b'\xf0\x9d\x84\x9e')

``datetime``
~~~~~~~~~~~~

The encoding used for `datetime.datetime` objects is dependent on whether these
objects are timezone-aware_ or timezone-naive:

- Timezone-aware datetimes are encoded as RFC3339_ compatible strings in JSON,
  and the `timestamp extension` in MessagePack. Both protocols support decoding
  RFC3339_ strings into ``datetime`` objects. During decoding all timezones are
  normalized to UTC.

- Timezone-naive datetimes are encoded as ISO8601_ compatible strings (*almost*
  RFC3339_ compatible) in both protocols. The lack of a timezone component
  means they're not strictly RFC3339_ compatible, but are still ISO8601_
  compatible.

Note that you can require a `datetime.datetime` object to be timezone-aware or
timezone-naive by specifying a ``tz`` constraint (see
:ref:`datetime-constraints` for more information).

.. code-block:: python

    >>> import datetime

    >>> tz = datetime.timezone(datetime.timedelta(hours=-6))

    >>> tz_aware = datetime.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=tz)

    >>> msg = msgspec.json.encode(tz_aware)

    >>> msg
    b'"2021-04-02T18:18:10.000123-06:00"'

    >>> msgspec.json.decode(msg, type=datetime.datetime)
    datetime.datetime(2021, 4, 3, 0, 18, 10, 123, tzinfo=datetime.timezone.utc)

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

``date``
~~~~~~~~~~~~

`datetime.date` values are serialized as RFC3339_ encoded strings.

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
~~~~~~~~~~~~

The encoding used for `datetime.time` objects is dependent on whether these
objects are timezone-aware_ or timezone-naive:

- Timezone-aware times are encoded as RFC3339_ compatible strings. Note that
  during decoding all timezones are normalized to UTC.

- Timezone-naive times are encoded as ISO8601_ compatible strings (*almost*
  RFC3339_ compatible) in both protocols. The lack of a timezone component
  means they're not strictly RFC3339_ compatible, but are still ISO8601_
  compatible.

Note that you can require a `datetime.time` object to be timezone-aware or
timezone-naive by specifying a ``tz`` constraint (see
:ref:`datetime-constraints` for more information).

.. code-block:: python

    >>> import datetime

    >>> tz = datetime.timezone(datetime.timedelta(hours=-6))

    >>> tz_aware = datetime.time(18, 18, 10, 123, tzinfo=tz)

    >>> msg = msgspec.json.encode(tz_aware)

    >>> msg
    b'"18:18:10.000123-06:00"'

    >>> msgspec.json.decode(msg, type=datetime.time)
    datetime.time(0, 18, 10, 123, tzinfo=datetime.timezone.utc)

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

``uuid``
~~~~~~~~

`uuid.UUID` values are serialized as RFC4122_ encoded strings.

.. code-block:: python

    >>> import uuid

    >>> u = uuid.UUID("c4524ac0-e81e-4aa8-a595-0aec605a659a")

    >>> msg = msgspec.json.encode(u)

    >>> msg
    b'"c4524ac0-e81e-4aa8-a595-0aec605a659a"'

    >>> msgspec.json.decode(msg, type=uuid.UUID)
    UUID('c4524ac0-e81e-4aa8-a595-0aec605a659a')

    >>> msgspec.json.decode(b'"oops"', type=uuid.UUID)
    Traceback (most recent call last):
        File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Invalid UUID


``list`` / ``tuple`` / ``set`` / ``frozenset``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

`list`, `tuple`, `set`, and `frozenset` objects map to arrays in both JSON and
MessagePack.  An error is raised if the elements don't match the specified
element type (if provided).

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
~~~~~~~~~~~~~~

`typing.NamedTuple` types map to arrays in both JSON and MessagePack.  An error
is raised during decoding if the type doesn't match or if any required fields
are missing.

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

``dict``
~~~~~~~~

Dicts encode/decode as JSON objects/MessagePack maps.

Dict subclasses (`collections.OrderedDict`, for example) are also supported for
encoding only. To decode into a ``dict`` subclass you'll need to implement a
``dec_hook`` (see :doc:`extending`).

JSON only supports `str`, `Literal`, `Enum`, `int`, and `IntEnum` key types
(integers are encoded as strings). MessagePack supports any hashable for the
key type.

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
~~~~~~~~~~~~~~~~~~~~

`typing.TypedDict` provides a way to specify different types for different
values in a ``dict``, rather than a single value type (the ``int`` in
``Dict[str, int]``, for example).  At runtime these are just standard
``dict`` types, the ``TypedDict`` type is only there to provide the schema
information during decoding. Note that ``msgspec`` supports both
`typing.TypedDict` and ``typing_extensions.TypedDict`` (a backport).

`typing.TypedDict` types map to JSON objects/MessagePack maps. During decoding,
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
~~~~~~~~~~~~~~~

`dataclasses` map to JSON objects/MessagePack maps.

During encoding, all attributes without a leading underscore (``"_"``) are
encoded.

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

``Struct``
~~~~~~~~~~

Structs are the preferred way of defining structured data types in ``msgspec``.
You can think of them as similar to dataclasses_/attrs_/pydantic_, but much
faster to create/compare/encode/decode. For more information, see the
:doc:`structs` page.

By default `msgspec.Struct` types map to JSON objects/MessagePack maps. During
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
treated as ``array`` types during encoding/decoding.  In this case fields are
serialized in their normalized order (definition order, with optional fields
shifted to the end). This can further improve performance at the cost of less
human readable messaging. Like ``array_like=False`` (the default) structs,
extra (trailing) fields are ignored during decoding, and any missing optional
fields have their defaults applied. Type checking also still applies.

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

``Enum`` / ``IntEnum``
~~~~~~~~~~~~~~~~~~~~~~

`enum.Enum` and `enum.IntEnum` types encode as their member *values* in both
JSON and MessagePack. Only enums composed of all string or all integer values
are supported. An error is raised during decoding if the value isn't the proper
type, or doesn't match any valid member.

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

``Literal``
~~~~~~~~~~~

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

``Union`` /  ``Optional``
~~~~~~~~~~~~~~~~~~~~~~~~~

Type unions are supported, with a few restrictions. These restrictions are in
place to remove any ambiguity during decoding - given an encoded value there
must always be a single type in a given `typing.Union` that can decode that
value.

Union restrictions are as follows:

- Unions may contain at most one of `int` / `enum.IntEnum`

- Unions may contain at most one of `str` / `enum.Enum`. `msgspec.json.Decoder`
  extends this requirement to also include `datetime` / `bytes` / `bytearray`.

- Unions may contain at most one *untagged* `Struct` type. Unions containing
  multiple struct types are only supported through :ref:`struct-tagged-unions`.

- Unions may contain at most one of `dict` / `typing.TypedDict` /
  `dataclasses.dataclass` / `Struct` (with ``array_like=False``)

- Unions may contain at most one of `list` / `tuple` / `set` / `frozenset` /
  `typing.NamedTuple` / `Struct` (with ``array_like=True``).

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
~~~~~~~

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
~~~~~~~

When decoding a message with `Any` type (or no type specified), encoded types
map to Python types in a protocol specific manner.

**JSON**

JSON types are decoded to Python types as follows:

- ``null``: `None`
- ``bool``: `bool`
- ``string``: `str`
- ``number``: `int` or `float` [#number_json]_
- ``array``: `list`
- ``object``: `dict`

.. [#number_json] Numbers are decoded as integers if they contain no decimal or
   exponent components (e.g. ``1`` but not ``1.0`` or ``1e10``), and fit in either
   an ``int64`` or ``uint64`` (within ``[-2**63, 2**64 - 1]``, inclusive). All
   other numbers decode as floats.

**MessagePack**

MessagePack types are decoded to Python types as follows:

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

.. _schema-evolution:

Schema Evolution
----------------

``msgspec`` includes support for "schema evolution", meaning that:

- Messages serialized with an older version of a schema will be deserializable
  using a newer version of the schema.
- Messages serialized with a newer version of the schema will be deserializable
  using an older version of the schema.

This can be useful if, for example, you have clients and servers with
mismatched versions.

For schema evolution to work smoothly, you need to follow a few guidelines:

1. Any new fields on a `msgspec.Struct` must specify default values.
2. Structs with ``array_like=True`` must not reorder fields, and any new fields
   must be appended to the end (and have defaults).
3. Don't change the type annotations for existing messages or fields.
4. Don't change the type codes or implementations for any defined
   :ref:`extensions <defining-extensions>` (MessagePack only).

For example, suppose we had a `msgspec.Struct` type representing a user:

.. code-block:: python

    >>> import msgpsec

    >>> from typing import Set, Optional

    >>> class User(msgspec.Struct):
    ...     """A struct representing a user"""
    ...     name: str
    ...     groups: Set[str] = set()
    ...     email: Optional[str] = None

Then suppose we wanted to add a new ``phone`` field to this struct in a way
that wouldn't break clients/servers still using the prior definition. To
accomplish this, we add ``phone`` as an _optional_ field (defaulting to
``None``), at the end of the struct.

.. code-block:: python

    >>> class User2(msgspec.Struct):
    ...     """An updated version of the User struct, now with a phone number"""
    ...     name: str
    ...     groups: Set[str] = set()
    ...     email: Optional[str] = None
    ...     phone : Optional[str] = None

Messages serialized using the new and old schemas can still be exchanged
without error. If an old message is deserialized using the new schema, the
missing fields all have default values that will be used. Likewise, if a new
message is deserialized with the old schema the unknown new fields will be
efficiently skipped without decoding.

.. code-block:: python

    >>> old_dec = msgspec.json.Decoder(User)

    >>> new_dec = msgspec.json.Decoder(User2)

    >>> new_msg = msgspec.json.encode(
    ...     User2("bob", groups={"finance"}, phone="512-867-5309")
    ... )

    >>> old_dec.decode(new_msg)  # deserializing a new msg with an older decoder
    User(name='bob', groups={'finance'}, email=None)

    >>> old_msg = msgspec.json.encode(
    ...     User("alice", groups={"admin", "engineering"})
    ... )

    >>> new_dec.decode(old_msg) # deserializing an old msg with a new decoder
    User2(name="alice", groups={"admin", "engineering"}, email=None, phone=None)

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _pickle: https://docs.python.org/3/library/pickle.html
.. _pattern matching: https://docs.python.org/3/reference/compound_stmts.html#the-match-statement
.. _JSON: https://json.org
.. _MessagePack: https://msgpack.org
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _RFC8259: https://datatracker.ietf.org/doc/html/rfc8259
.. _RFC3339: https://datatracker.ietf.org/doc/html/rfc3339
.. _RFC4122: https://datatracker.ietf.org/doc/html/rfc4122
.. _ISO8601: https://en.wikipedia.org/wiki/ISO_8601
.. _timestamp extension: https://github.com/msgpack/msgpack/blob/master/spec.md#timestamp-extension-type
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _attrs: https://www.attrs.org/en/stable/index.html
.. _timezone-aware: https://docs.python.org/3/library/datetime.html#aware-and-naive-objects
