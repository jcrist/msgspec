msgspec
=======

``msgspec`` is a *fast* and *friendly* serialization library for Python,
supporting both JSON_ and MessagePack_. It integrates well with Python's `type
annotations`_ , providing ergonomic (and performant!) schema validation.

**Define** your message schemas using standard Python type annotations.

.. code-block:: python

    >>> from typing import Optional, Set

    >>> import msgspec

    >>> class User(msgspec.Struct):
    ...     """A new type describing a User"""
    ...     name: str
    ...     groups: Set[str] = set()
    ...     email: Optional[str] = None

**Encode** messages as JSON_ or MessagePack_.

.. code-block:: python

    >>> alice = User("alice", groups={"admin", "engineering"})

    >>> alice
    User(name='alice', groups={"admin", "engineering"}, email=None)

    >>> msg = msgspec.json.encode(alice)

    >>> msg
    b'{"name":"alice","groups":["admin","engineering"],"email":null}'

**Decode** messages back into Python types (with optional schema validation).

.. code-block:: python

    >>> msgspec.json.decode(msg, type=User)
    User(name='alice', groups={"admin", "engineering"}, email=None)

    >>> msgspec.json.decode(b'{"name":"bob","groups":[123]}', type=User)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.DecodeError: Expected `str`, got `int` - at `$.groups[0]`

``msgspec`` is designed to be as performant as possible, while retaining some
of the nicities of validation libraries like pydantic_. For supported types,
encoding/decoding a message with ``msgspec`` can be *~2-40x faster* than
alternative libraries.

Highlights
----------

- ``msgspec`` is **fast**. :doc:`benchmarks` show it's among the fastest
  serialization methods for Python, outperforming all other JSON/MessagePack
  libraries compared.
- ``msgspec`` is **friendly**. Through use of Python's `type annotations`_,
  messages are :ref:`validated <typed-deserialization>` during deserialization
  in a declaritive way. ``msgspec`` also works well with other type-checking
  tooling like `mypy <https://mypy.readthedocs.io>`_, providing excellent
  editor integration.
- ``msgspec`` is **flexible**. Unlike other libraries like `json` or
  `msgpack-python`_, ``msgspec`` natively supports a wider range of Python builtin
  types. Support for additional types can also be added through :doc:`extensions
  <extending>`.
- ``msgspec`` is **correct**. The encoders/decoders implemented are strictly
  compliant with the JSON_ and MessagePack_ specifications, providing stronger
  guarantees of compatibility with other systems.
- ``msgspec`` supports :ref:`"schema evolution" <schema-evolution>`. Messages
  can be sent between clients with different schemas without error, allowing
  systems to evolve over time.


Installation
------------

``msgspec`` can be installed via ``pip`` or ``conda``. Note that Python >= 3.8
is required.

**pip**

.. code-block:: shell

    pip install msgspec

**conda**

.. code-block:: shell

    conda install msgspec -c conda-forge


.. _type annotations: https://docs.python.org/3/library/typing.html
.. _JSON: https://json.org
.. _MessagePack: https://msgpack.org
.. _msgpack-python: https://github.com/msgpack/msgpack-python
.. _pydantic: https://pydantic-docs.helpmanual.io/

.. toctree::
    :hidden:
    :maxdepth: 2

    usage.rst
    structs.rst
    extending.rst
    perf-tips.rst
    benchmarks.rst
    api.rst
