Usage
=====

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
create a ``Decoder`` once and use the ``Decoder.encode`` method instead.

.. code-block:: python

    >>> import msgspec

    >>> # Create a JSON decoder
    ... encoder = msgspec.json.Decoder()

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
While commonly used with `msgspec.Struct` types, most combinations of the
following types are supported (with a few restrictions, see
:ref:`supported-types` for more information):

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
- `datetime.datetime`
- `typing.Any`
- `typing.Optional`
- `typing.Union`
- `msgspec.msgpack.Ext`
- `enum.Enum` derived types
- `enum.IntEnum` derived types
- `msgspec.Struct` derived types
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
    msgspec.DecodeError: Expected `str`, got `int` - at `$[1].groups[1]`

Unlike some other libraries (e.g. pydantic_), ``msgspec`` won't perform any
unsafe implicit conversion. For example, if an integer is specified and a
string is decoded instead, an error is raised rather than attempting to cast
the string to an int.

.. code-block:: python

    >>> msgspec.json.decode(b'[1, 2, "3"]', type=List[int])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.DecodeError: Expected `int`, got `str` - at `$[2]`

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
supported. Values outside this range will raise a `msgspec.DecodeError`
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
ascii; unicode characters (e.g. ``"𝄞"``) are _not_ escaped and are serialized
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
    b'"85+Eng=="'

    >>> msgspec.json.decode(msg, type=bytearray)
    bytearray(b'"85+Eng=="')

``IntEnum``
~~~~~~~~~~~

`enum.IntEnum` types encode as their integer *values* in both JSON and
MessagePack. An error is raised during decoding if the value isn't an integer,
or doesn't match any valid `enum.IntEnum` member.

.. code-block:: python

    >>> import enum

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
    msgspec.DecodeError: Invalid enum value `4`

``Enum``
~~~~~~~~

`enum.Enum` types encode as strings of the member *names* (not their values) in
both JSON and MessagePack. An error is raised during decoding if the value
isn't a string or doesn't match any valid `enum.Enum` member.

.. code-block:: python

    >>> import enum

    >>> class Fruit(enum.Enum):
    ...     APPLE = "apple value"
    ...     BANANA = "banana value"

    >>> msgspec.json.encode(Fruit.APPLE)
    b'"APPLE"'

    >>> msgspec.json.decode(b'"APPLE"', type=Fruit)
    <Fruit.APPLE: 'apple value'>

    >>> msgspec.json.decode(b'"GRAPE"', type=Fruit)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.DecodeError: Invalid enum value 'GRAPE'

``datetime``
~~~~~~~~~~~~

`datetime.datetime` values are serialized as RFC3339_ encoded strings in JSON,
and the `timestamp extension`_ in MessagePack. Only `timezone aware
<https://docs.python.org/3/library/datetime.html#aware-and-naive-objects>`__
datetime objects are supported. During decoding, all timezones are normalized
to UTC.

.. code-block:: python

    >>> import datetime

    >>> tz = datetime.timezone(datetime.timedelta(hours=-6))

    >>> dt = datetime.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=tz)

    >>> msg = msgspec.json.encode(dt)

    >>> msg
    b'"2021-04-02T18:18:10.000123-06:00"'

    >>> msgspec.json.decode(msg, type=datetime.datetime)
    datetime.datetime(2021, 4, 3, 0, 18, 10, 123, tzinfo=datetime.timezone.utc)

    >>> msgspec.json.decode(b'"oops not a date"', type=datetime.datetime)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.DecodeError: Invalid RFC3339 encoded datetime

``list`` / ``tuple`` / ``set``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

`list`, `tuple`, and `set` objects map to arrays in both JSON and MessagePack.
An error is raised if the elements don't match the specified element type (if
provided).

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
    msgspec.DecodeError: Expected `int`, got `str` - at `$[2]`

``dict``
~~~~~~~~

Dicts encode/decode as JSON objects/MessagePack maps.

Note that JSON only supports string keys, while MessagePack supports any
hashable for the key type. An error is raised during decoding if the keys or
values don't match their respective types (if specified).

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
    msgspec.DecodeError: Expected `int`, got `str` - at `$[...]`

``Struct``
~~~~~~~~~~

Structs are the preferred way of defining structured data types in ``msgspec``.
You can think of them as similar to dataclasses_/attrs_/pydantic_, but much
faster to create/compare/encode/decode. For more information, see the
:doc:`structs` page.

By default `msgspec.Struct` types map to JSON objects/MessagePack maps. During
decoding, any extra fields are ignored, and any missing optional fields have
their default values applied. An error is raised during decoding if the type
doesn't match or if any required fields are missing.

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
    msgspec.DecodeError: Expected `str`, got `int` - at `$.groups[1]`

If you pass ``asarray=True`` when defining the struct type, they're instead
treated as ``array`` types during encoding/decoding (with fields serialized in
their definition order). This can further improve performance at the cost of
less human readable messaging. Like ``asarray=False`` structs, extra (trailing)
fields are ignored during decoding, and any missing optional fields have their
defaults applied. Type checking also still applies.

.. code-block:: python

    >>> from typing import Set, Optional

    >>> class User(msgspec.Struct, asarray=True):
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
    msgspec.DecodeError: Expected `str`, got `int` - at `$[1][1]`

``Union`` /  ``Optional``
~~~~~~~~~~~~~~~~~~~~~~~~~

Type unions are supported, with a few restrictions. These restrictions are in
place to remove any ambiguity during decoding - given an encoded value there
must always be a single type in a given `Union` that can decode that value.

Union restrictions are as follows:

- Unions may contain at most one of `int` / `IntEnum`

- Unions may contain at most one of `str` / `Enum`. `msgspec.json.Decoder`
  extends this requirement to also include `datetime` / `bytes` / `bytearray`.

- Unions may contain at most one `Struct` type

- Unions may contain at most one of `dict` / `Struct` (with ``asarray=False``)

- Unions may contain at most one of `list` / `tuple` / `set` / `Struct` (with
  ``asarray=True``).

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
    msgspec.DecodeError: Expected `int | str | array`, got `bool`

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
2. Structs with ``asarray=True`` must not reorder fields, and any new fields
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
.. _timestamp extension: https://github.com/msgpack/msgpack/blob/master/spec.md#timestamp-extension-type
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _attrs: https://www.attrs.org/en/stable/index.html
