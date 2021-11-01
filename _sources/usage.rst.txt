Usage
=====

.. currentmodule:: msgspec

For ease of use, ``msgspec`` exposes two functions `encode` and `decode`, for
serializing and deserializing objects respectively.

.. code-block:: python

    >>> import msgspec
    >>> data = msgspec.encode({"hello": "world"})
    >>> msgspec.decode(data)
    {'hello': 'world'}

Note that if you're making multiple calls to `encode` or `decode`, it's more
efficient to create an `Encoder` and `Decoder` once, and then use
`Encoder.encode` and `Decoder.decode`.

.. code-block:: python

    >>> enc = msgspec.Encoder()
    >>> dec = msgspec.Decoder()
    >>> data = enc.encode({"hello": "world"})
    >>> dec.decode(data)
    {'hello': 'world'}

.. _supported-types:

Supported Types
---------------

Msgspec currently supports serializing/deserializing the following concrete
types:

- `None`
- `bool`
- `int`
- `float`
- `str`
- `bytes`
- `bytearray`
- `memoryview`
- `tuple`
- `list`
- `dict`
- `set`
- `datetime.datetime`
- `enum.Enum`
- `msgspec.Struct`
- `msgspec.Ext`

Support for serializing additional types can be added through extensions, for
more information see :doc:`extending`

.. _typed-deserialization:

Typed and Untyped Deserialization
---------------------------------

By default, both `decode` and `Decoder.decode` will deserialize messages
without any validation, performing *untyped deserialization*. `MessagePack
types <https://github.com/msgpack/msgpack/blob/master/spec.md#formats>`_ are
mapped to Python types as follows:

- ``nil``: `None`
- ``bool``: `bool`
- ``int``: `int`
- ``float``: `float`
- ``str``: `str`
- ``bin``: `bytes`
- ``array``: `list` or `tuple` [#tuple]_
- ``map``: `dict`
- ``ext``: `msgspec.Ext` [#extensions]_ or `datetime.datetime` [#datetime]_

.. [#tuple] Tuples are only used when the array type must be hashable (e.g.
   keys in a ``dict`` or ``set``). All other array types are deserialized as
   lists by default.

.. [#extensions] extension types can be mapped to/from custom types *even when
   using untyped deserialization* through msgspec's extension mechanism. For
   more information see :doc:`extending`.

.. [#datetime] datetime objects are encoded/decoded using the `timestamp
   extension type
   <https://github.com/msgpack/msgpack/blob/master/spec.md#timestamp-extension-type>`__.
   Note that on Windows this will only work for datetimes _after_ the `Unix
   epoch <https://en.wikipedia.org/wiki/Unix_time>`__ (this is fixable with
   some effort, if you need to support earlier timestamps on windows please
   file an issue).

Messages composed of any combination of these will deserialize successfully
without any further validation:

.. code-block:: python

    >>> b = msgspec.encode([1, "two", b"three"])  # encode a list with mixed types
    >>> msgspec.decode(b)  # decodes using default types and no validation
    [1, "two", b"three"]
    >>> msgspec.Decoder().decode(b)  # likewise for Decoder.decode
    [1, "two", b"three"]

If you want to deserialize a MessagePack type to a different Python type, or
perform any type checking you'll need to specify a *deserialization type*. This
can be passed to either `Decoder` (recommended) or `decode`.

For example, say we wanted to deserialize the above as a ``set`` instead of a
``list`` (the default). We'd do this by specifying the expected message type as
a `set`:

.. code-block:: python

    >>> msgspec.Decoder(set).decode(b)  # deserialize as a set
    {1, "two", b"three"}
    >>> msgspec.decode(b, type=set)  # can also pass to msgspec.decode
    {1, "two", b"three"}

Nested type specifications are fully supported, and can be used to validate at
deserialization time. If a message doesn't match the specified type, an
informative error message will be raised. For example, say we expect the above
message to be a set of ints:

.. code-block:: python

    >>> from typing import Set
    >>> dec = msgspec.Decoder(Set[int])  # define a decoder for a set of ints
    >>> dec
    Decoder(Set[int])
    >>> dec.decode(b)  # Invalid messages raise a nice error
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.DecodingError: Error decoding `Set[int]`: expected `int`, got `str`
    >>> b2 = msgspec.encode({1, 2, 3})
    >>> dec.decode(b2)  # Valid messages deserialize properly
    {1, 2, 3}

Typed deserializers are most commonly used along with `Struct` messages to
provide a message schema, but any combination of the following types is
acceptible:

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
- `msgspec.Ext`
- `enum.Enum` derived types
- `enum.IntEnum` derived types
- `msgspec.Struct` derived types

Custom types are also supported, provided a valid ``ext_hook`` or ``dec_hook``
callback is defined on the Decoder. For more information, see :doc:`extending`.


Structs
-------

``msgspec`` can serialize many builtin types, but unlike protocols like
`pickle`_, it can't serialize arbitrary user classes by default. Two
user-defined types are supported though:

- `Struct`
- `enum.Enum`

Structs are useful for defining structured messages. Fields are defined using
python type annotations. Default values can also be specified for any optional
arguments.

Here we define a struct representing a person, with two required fields and two
optional fields.

.. code-block:: python

    >>> class Person(msgspec.Struct):
    ...     """A struct describing a person"""
    ...     first : str
    ...     last : str
    ...     address : str = ""
    ...     phone : str = None

Struct types automatically generate a few methods based on the provided type
annotations:

- ``__init__``
- ``__repr__``
- ``__copy__``
- ``__eq__`` & ``__ne__``
- ``__match_args__`` (for Python 3.10+'s `pattern matching`_)

.. code-block:: python

    >>> harry = Person("Harry", "Potter", address="4 Privet Drive")
    >>> harry
    Person(first='Harry', last='Potter', address='4 Privet Drive', phone=None)
    >>> harry.first
    "Harry"
    >>> ron = Person("Ron", "Weasley", address="The Burrow")
    >>> ron == harry
    False

If using Python 3.10+, struct types can also be used in `pattern matching`_
blocks. Replicating an example from `PEP 636
<https://www.python.org/dev/peps/pep-0636/>`__:

.. code-block:: python

    # NOTE: this example requires Python 3.10+
    >>> class Point(msgspec.Struct):
    ...     x: float
    ...     y: float
    ...
    >>> def where_is(point):
    ...     match point:
    ...         case Point(0, 0):
    ...             print("Origin")
    ...         case Point(0, y):
    ...             print(f"Y={y}")
    ...         case Point(x, 0):
    ...             print(f"X={x}")
    ...         case Point():
    ...             print("Somewhere else")
    ...         case _:
    ...             print("Not a point")
    ...
    >>> where_is(Point(0, 6))
    "Y=6"

If needed, a ``__hash__`` method can also be generated by specifying
``immutable=True`` when defining the struct. Note that this disables modifying
field values after initialization.

.. code-block:: python

    >>> class Point(msgspec.Struct, immutable=True):
    ...     """This struct is immutable & hashable"""
    ...     x: float
    ...     y: float
    ...
    >>> p = Point(1.0, 2.0)
    >>> {p: 1}  # immutable structs are hashable, and can be keys in dicts
    {Point(1.0, 2.0): 1}
    >>> p.x = 2.0  # immutable structs cannot be modified after creation
    Traceback (most recent call last):
        ...
    TypeError: immutable type: 'Point'

Note that it is forbidden to override ``__init__``/``__new__`` in a struct
definition, but other methods can be overridden or added as needed.

The struct fields are available via the ``__struct_fields__`` attribute (a
tuple of the fields in argument order ) if you need them. Here we add a method
for converting a struct to a dict.

.. code-block:: python

    >>> class Point(msgspec.Struct):
    ...     """A point in 2D space"""
    ...     x : float
    ...     y : float
    ...     
    ...     def to_dict(self):
    ...         return {f: getattr(self, f) for f in self.__struct_fields__}
    ...
    >>> p = Point(1.0, 2.0)
    >>> p.to_dict()
    {"x": 1.0, "y": 2.0}

Struct types are written in C and are quite speedy and lightweight. They're
great for defining structured messages both for serialization and for use in an
application.

Like with builtin types, to deserialize a message as a struct you need to
provide the expected deserialization type to `Decoder`.

.. code-block:: python

    >>> dec = msgspec.Decoder(Person)  # Create a decoder that expects a Person
    >>> dec
    Decoder(Person)
    >>> data = msgspec.encode(harry)
    >>> dec.decode(data)
    Person(first='Harry', last='Potter', address='4 Privet Drive', phone=None)

Using structs for message schemas not only adds validation during
deserialization, it also can improve performance. Depending on the schema,
deserializing a message into a `Struct` can be *roughly twice as fast* as
deserializing it into a `dict`.

If you need higher performance (at the cost of more inscrutable message
encoding), you can set ``asarray=True`` on a struct definition. Structs with
this option enabled are encoded/decoded as MessagePack ``array`` types (rather
than ``map`` types), removing the field names from the serialized message. This
can provide another ~2x speedup for decoding (and ~1.5x speedup for encoding).

.. code-block:: python

    >>> class ArrayBasedStruct(msgspec.Struct, asarray=True):
    ...     """This struct is serialized as a MessagePack array type
    ...     (instead of a map type). This means no field names are sent
    ...     as part of the message, speeding up encoding/decoding."""
    ...     my_first_field: str
    ...     my_second_field: int
    ...
    >>> x = ArrayBasedStruct("some string", 2)
    >>> msgspec.encode(x)
    b'\x92\xabsome string\x02'

.. _schema-evolution:

Schema Evolution
----------------

Msgspec includes support for "schema evolution", meaning that:

- Messages serialized with an older version of a schema will be deserializable
  using a newer version of the schema.
- Messages serialized with a newer version of the schema will be deserializable
  using an older version of the schema.

This can be useful if, for example, you have clients and servers with
mismatched versions.

For schema evolution to work smoothly, you need to follow a few guidelines:

1. Any new fields on a `Struct` must specify default values.
2. Structs with ``asarray=True`` must not reorder fields, and any new fields
   must be appended to the end (and have defaults).
3. Don't change the type annotations for existing messages or fields.
4. Don't change the type codes or implementations for any defined
   :ref:`Extensions`.

For example, suppose we wanted to add a new ``email`` field to our ``Person``
struct. To do so, we add it at the end of the definition, with a default value.

.. code-block:: python

    >>> class Person2(msgspec.Struct):
    ...     """A struct describing a person"""
    ...     first : str
    ...     last : str
    ...     address : str = ""
    ...     phone : str = None
    ...     email : str = None  # added at the end, with a default
    ...
    >>> vernon = Person2("Vernon", "Dursley", address="4 Privet Drive", email="vernon@grunnings.com")

Messages serialized using the new and old schemas can still be exchanged
without error. If an old message is deserialized using the new schema, the
missing fields all have default values that will be used. Likewise, if a new
message is deserialized with the old schema the unknown new fields will be
efficiently skipped without decoding.

.. code-block:: python

    >>> old_dec = msgspec.Decoder(Person)
    >>> new_dec = msgspec.Decoder(Person2)

    >>> new_msg = msgspec.encode(vernon)
    >>> old_dec.decode(new_msg)  # deserializing a new msg with an older decoder
    Person(first="Vernon", last="Dursley", address="4 Privet Drive", phone=None)

    >>> old_msg = msgspec.encode(harry)
    >>> new_dec.decode(old_msg) # deserializing an old msg with a new decoder
    Person2(first='Harry', last='Potter', address='4 Privet Drive', phone=None, email=None)


.. _type annotations: https://docs.python.org/3/library/typing.html
.. _pickle: https://docs.python.org/3/library/pickle.html
.. _pattern matching: https://docs.python.org/3/reference/compound_stmts.html#the-match-statement
