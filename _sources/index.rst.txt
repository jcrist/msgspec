msgspec
=======

``msgspec`` is a fast and friendly implementation of the `MessagePack
<https://msgpack.org>`__ protocol for Python 3.8+. In addition to
serialization/deserialization, it supports runtime message validation using
schemas defined via Python's `type annotations`_.

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

- ``msgspec`` is **fast**. :doc:`benchmarks` show it's among the fastest
  serialization methods for Python.
- ``msgspec`` is **friendly**. Through use of Python's `type annotations`_,
  messages can be :ref:`validated <typed-deserialization>` during
  deserialization in a declaritive way.  ``msgspec`` also works well with other
  type-checking tooling like `mypy <https://mypy.readthedocs.io>`_, providing
  excellent editor integration.
- ``msgspec`` is **flexible**. Unlike other libraries like ``msgpack`` or
  ``json``, ``msgspec`` natively supports a wider range of Python builtin
  types. Support for additional types can also be added through :ref:`extensions`.
- ``msgspec`` supports :ref:`"schema evolution" <schema-evolution>`. Messages can
  be sent between clients with different schemas without error.

Installation
------------

``msgspec`` can be installed via ``pip`` (and soon via ``conda``). Note that
Python >= 3.8 is required.

**pip**

.. code-block:: shell

    pip install msgspec


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
- `msgspec.Ext`

Support for serializing additional types can be added through the use of the
``default`` callback on the `Encoder`, or by defining custom :ref:`extensions`.

.. _typed-deserialization:

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
- ``array``: `list` or `tuple` [#tuple]_
- ``map``: `dict`
- ``ext``: `msgspec.Ext`

.. [#tuple] Tuples are only used when the array type must be hashable (e.g.
   keys in a ``dict`` or ``set``). All other array types are deserialized as
   lists by default.

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
- `msgspec.Ext`
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

.. _extensions:

Extensions
~~~~~~~~~~

The MessagePack specification provides support for defining custom
`Extensions <https://github.com/msgpack/msgpack/blob/master/spec.md#extension-types>`__.
Extensions consist of:

- An integer code (between 0 and 127, inclusive) representing the "type" of the
  extension.
- An arbitrary byte buffer of data (up to ``(2^32) - 1`` in length).

By default extensions are serialized to/from `Ext` objects.

.. code-block:: python

    >>> ext = msgspec.Ext(1, b"some data")  # an extension object, with type code 1
    >>> msg = msgspec.encode(ext)
    >>> ext2 = msgspec.decode(msg)
    >>> ext == ext2  # deserializes as an Ext object
    True

While manually creating `Ext` objects from buffers can be useful, usually the
user wants to map extension types to/from their own custom objects. This can be
accomplished by defining two callback functions:

- ``default`` in `Encoder`, for transforming custom types into values
  that ``msgspec`` already knows how to serialize.
- ``ext_hook`` in `Decoder`, for converting extensions back into those
  custom types.

These should have the following signatures:

.. code-block:: python

    def default(obj: Any) -> Any:
        """Given an object that msgspec doesn't know how to serialize by
        default, convert it into an object that it does know how to
        serialize"""
        pass

    def ext_hook(code: int, data: memoryview) -> Any:
        """Given an extension type code and data buffer, deserialize whatever
        custom object the extension type represents"""
        pass


For example, perhaps you wanted to serialize `complex` number objects as an
extension type.  These objects can be represented as tuples of two floats (one
"real" and one "imaginary"). If we represent each float as 8 bytes (a
"double"), then any complex number can be fully represented by a 16 byte
buffer.

.. code-block::

    +---------+---------+
    |  real   |  imag   |
    +---------+---------+
      8 bytes   8 bytes 
    

Here we define ``default`` and ``ext_hook`` callbacks to convert `complex`
objects to/from this binary representation as a MessagePack extension.

.. code-block:: python

    import msgspec
    import struct
    from typing import Any

    # All extension types need a unique integer designator so the decoder knows
    # which type they're decoding. Here we arbitrarily choose 1, but any integer
    # between 0 and 127 (inclusive) would work.
    COMPLEX_TYPE_CODE = 1

    def default(obj: Any) -> Any:
        if isinstance(obj, complex):
            # encode the complex number into a 16 byte buffer
            data = struct.pack('dd', obj.real, obj.imag)

            # Return an `Ext` object so msgspec serializes it as an extension type.
            return msgspec.Ext(COMPLEX_TYPE_CODE, data)
        else:
            # Raise a TypeError for other types
            raise TypeError(f"Objects of type {type(obj)} are not supported")


    def ext_hook(code: int, data: memoryview) -> Any:
        if code == COMPLEX_TYPE_CODE:
            # This extension type represents a complex number, decode the data
            # buffer accordingly.
            real, imag = struct.unpack('dd', data)
            return complex(real, imag)
        else:
            # Raise a TypeError for other extension type codes
            raise TypeError(f"Extension type code {code} is not supported")


    # Create an encoder and a decoder using the custom callbacks
    enc = msgspec.Encoder(default=default)
    dec = msgspec.Decoder(ext_hook=ext_hook)

    # Define a message that contains complex numbers
    msg = {"roots": [0, 0.75, 1 + 0.5j, 1 - 0.5j]}

    # Encode and decode the message to show that things work
    buf = enc.encode(msg)
    msg2 = dec.decode(buf)
    assert msg == msg2  # True


.. note::

    Note that the ``data`` argument to ``ext_hook`` is a `memoryview`. This
    view is attached to the larger buffer containing the complete message being
    decoded. As such, you'll want to ensure that you don't keep a reference to
    the underlying buffer, otherwise you may accidentally persist the larger
    message buffer around for longer than necessary, resulting in increased
    memory usage.



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
3. Don't change the type codes or implementations for any defined
   :ref:`Extensions`

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
.. _quickle: https://jcristharif.com/quickle/
.. _pickle: https://docs.python.org/3/library/pickle.html


.. toctree::
    :hidden:
    :maxdepth: 2

    benchmarks.rst
    api.rst
