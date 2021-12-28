msgspec
=======

``msgspec`` is a fast and friendly implementation of the `MessagePack
<https://msgpack.org>`__ protocol for Python 3.8+. In addition to
serialization/deserialization, it supports message validation using schemas
defined via Python's `type annotations`_.

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
    serialized_data = msgspec.msgpack.encode(alice)

    # Deserialize and validate the message as a User type
    user = msgspec.msgpack.decode(serialized_data, type=User)

    assert user == alice


Highlights
----------

.. cssclass:: spaced

- ``msgspec`` is **fast**. :doc:`benchmarks` show it's among the fastest
  serialization methods for Python.
- ``msgspec`` is **friendly**. Through use of Python's `type annotations`_,
  messages can be :ref:`validated <typed-deserialization>` during
  deserialization in a declaritive way.  ``msgspec`` also works well with other
  type-checking tooling like `mypy <https://mypy.readthedocs.io>`_, providing
  excellent editor integration.
- ``msgspec`` is **flexible**. Unlike other libraries like ``msgpack`` or
  ``json``, ``msgspec`` natively supports a wider range of Python builtin
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
    extending.rst
    benchmarks.rst
    api.rst
