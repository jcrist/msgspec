msgspec
=======

.. toctree::
   :maxdepth: 2
   :caption: Contents:


``msgspec`` is a fast and friendly implementation of the `MessagePack
<https://msgpack.org>`__ protocol for Python 3.8+. It supports message
validation through the use of schemas defined using Python's `type
annotations`_.

.. code-block:: python

    from typing import Optional, List
    import msgspec

    # Define a schema for a `User` type
    class User(msgspec.Struct):
        name: str
        groups: List[str] = []
        email: Optional[str] = None

    # Create a `User` object
    alice = User("alice", groups=["admin", "engineering"])

    # Serialize `alice` to `bytes` using the MessagePack protocol
    serialized_data = msgspec.encode(alice)

    # Deserialize and validate the message as a User type
    user = msgspec.Decoder(User).decode(serialized_data)

    assert user == alice


Highlights
----------

- ``msgspec`` is **fast**. `Benchmarks
  <https://github.com/jcrist/msgspec/tree/master/benchmarks>`__ show it's among
  the fastest serialization methods for Python.
- ``msgspec`` is **friendly**. Through use of Python's `type annotations`_,
  messages can be validated during deserialization in a declaritive way.
  ``msgspec`` also works well with other type-checking tooling like `mypy`,
  providing excellent editor integration.
- ``msgspec`` is **flexible**. Unlike ``msgpack`` or ``json``, ``msgspec`` natively
  supports a wider range of Python builtin types.
- ``msgspec`` supports :ref:`"schema evolution" <schema-evolution>`. Messages can
  be sent between clients with different schemas without error.

Installation
------------

``msgspec`` can be installed via ``pip`` or ``conda``. Note that Python >= 3.8 is
required.

**pip**

.. code-block:: shell

    pip install msgspec

**conda**

.. code-block:: shell

    conda install -c conda-forge msgspec


.. currentmodule:: msgspec


Usage
-----

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

Supported Types
~~~~~~~~~~~~~~~

Msgspec currently supports serializing/deserializing the following types:

- `None`
- `bool`
- `int`
- `float`
- `str`
- `bytes`
- `bytearray`
- `tuple`
- `list`
- `dict`
- `set`
- `enum.Enum`
- `msgspec.Struct`

Typed and Untyped Deserialization
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

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
- ``array``: `list`
- ``map``: `dict`

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
- `typing.Any`
- `typing.Optional`
- `enum.Enum` derived types
- `enum.IntEnum` derived types
- `msgspec.Struct` derived types

Structs
~~~~~~~

``msgspec`` can serialize many builtin types, but unlike protocols like
`pickle`_/`quickle`_, it can't serialize arbitrary user classes. Two
user-defined types are supported:

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

.. code-block:: python

    >>> harry = Person("Harry", "Potter", address="4 Privet Drive")
    >>> harry
    Person(first='Harry', last='Potter', address='4 Privet Drive', phone=None)
    >>> harry.first
    "Harry"
    >>> ron = Person("Ron", "Weasley", address="The Burrow")
    >>> ron == harry
    False

It is forbidden to override ``__init__``/``__new__`` in a struct definition,
but other methods can be overridden or added as needed. The struct fields are
available via the ``__struct_fields__`` attribute (a tuple of the fields in
argument order ) if you need them. Here we add a method for converting a struct
to a dict.

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

.. _schema-evolution:

Schema Evolution
~~~~~~~~~~~~~~~~

Msgspec includes support for "schema evolution", meaning that:

- Messages serialized with an older version of a schema will be deserializable
  using a newer version of the schema.
- Messages serialized with a newer version of the schema will be deserializable
  using an older version of the schema.

This can be useful if, for example, you have clients and servers with
mismatched versions.

For schema evolution to work smoothly, you need to follow a few guidelines:

1. Any new fields on a `Struct` must specify default values.
2. Don't change the type annotations for existing messages or fields

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
without error.

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
.. _quickle: https://jcristharif.com/quickle/
.. _pickle: https://docs.python.org/3/library/pickle.html


.. toctree::
    :hidden:
    :maxdepth: 2

    api.rst
