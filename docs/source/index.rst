msgspec
=======

``msgspec`` is a *fast* and *friendly* serialization library for Python,
supporting both `JSON <https://json.org>`__ and `MessagePack
<https://msgpack.org>`__. It integrates well with Python's `type annotations
<https://docs.python.org/3/library/typing.html>`__, providing ergonomic (and
performant!) schema validation.

.. code-block:: python

    from typing import Optional, Set
    import msgspec

    # Define a schema for a `User` type
    class User(msgspec.Struct):
        name: str
        groups: Set[str] = set()
        email: Optional[str] = None

    # Create a `User` object
    alice = User("alice", groups={"admin", "engineering"})

    # Serialize `alice` to `bytes` as JSON
    serialized_data = msgspec.json.encode(alice)
    # b'{"name":"alice","groups":["admin","engineering"],"email":null}'

    # Deserialize and validate the message as a User type
    user = msgspec.json.decode(serialized_data, type=User)

    assert user == alice

``msgspec`` is designed to be as performant as possible, while retaining some
of the nicities of validation libraries like `pydantic
<https://pydantic-docs.helpmanual.io/>`__. For supported types,
encoding/decoding a message with ``msgspec`` can be *~2-20x faster* than
alternative libraries.


Highlights
----------

- ``msgspec`` is **fast**. :doc:`benchmarks` show it's among the fastest
  serialization methods for Python.
- ``msgspec`` is **friendly**. Through use of Python's `type annotations`_,
  messages are :ref:`validated <typed-deserialization>` during deserialization
  in a declaritive way. ``msgspec`` also works well with other type-checking
  tooling like `mypy <https://mypy.readthedocs.io>`_, providing excellent
  editor integration.
- ``msgspec`` is **flexible**. Unlike other libraries like ``json`` or
  ``msgpack``, ``msgspec`` natively supports a wider range of Python builtin
  types. Support for additional types can also be added through :doc:`extensions
  <extending>`.
- ``msgspec`` supports :ref:`"schema evolution" <schema-evolution>`. Messages can
  be sent between clients with different schemas without error.

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


.. toctree::
    :hidden:
    :maxdepth: 2

    usage.rst
    structs.rst
    extending.rst
    benchmarks.rst
    api.rst
