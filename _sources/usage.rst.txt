Usage
=====

``msgspec`` supports multiple serialization protocols, accessed through
separate submodules:

- ``msgspec.json`` (JSON_)
- ``msgspec.msgpack`` (MessagePack_)
- ``msgspec.yaml`` (YAML_)
- ``msgspec.toml`` (TOML_)

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


.. _typed-decoding:

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
  objects into a :doc:`Struct <structs>` instead of the default `dict`).

- The `type annotations`_ used to describe the expected types are compatible
  with tools like mypy_ or pyright_, providing excellent editor integration.

``msgspec`` uses Python `type annotations`_ to describe the expected types. A
:doc:`wide variety of builtin types are supported <supported-types>`.

Here we define a user schema as a :doc:`Struct <structs>` type. We then pass
the type to ``decode`` via the ``type`` keyword argument:

.. code-block:: python

    >>> import msgspec

    >>> class User(msgspec.Struct):
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

    >>> msgspec.json.decode(
    ...     b'{"name": "alice", "groups": ["admin", "engineering"]}',
    ...     type=User
    ... )
    User(name='alice', groups={'admin', 'engineering'}, email=None)

If a message doesn't match the expected type, an error is raised.

.. code-block:: python

    >>> msgspec.json.decode(
    ...     b'{"name": "bill", "groups": ["devops", 123]}',
    ...     type=User
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `str`, got `int` - at `$.groups[1]`

.. _strict-vs-lax:

"Strict" vs "Lax" Mode
~~~~~~~~~~~~~~~~~~~~~~

Unlike some other libraries (e.g. pydantic_), ``msgspec`` won't perform any
unsafe implicit conversion by default ("strict" mode). For example, if an
integer is specified and a string is provided instead, an error is raised
rather than attempting to cast the string to an int.

.. code-block:: python

    >>> msgspec.json.decode(b'[1, 2, "3"]', type=list[int])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$[2]`

For cases where you'd like a more lax set of conversion rules, you can pass
``strict=False`` to any ``decode`` function or ``Decoder`` class ("lax" mode).
See :doc:`supported-types` for information on how this affects individual
types.

.. code-block:: python

    >>> msgspec.json.decode(b'[1, 2, "3"]', type=list[int], strict=False)
    [1, 2, 3]


.. _JSON: https://json.org
.. _MessagePack: https://msgpack.org
.. _YAML: https://yaml.org
.. _TOML: https://toml.io
.. _type annotations: https://docs.python.org/3/library/typing.html
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _mypy: https://mypy.readthedocs.io
.. _pyright: https://github.com/microsoft/pyright
