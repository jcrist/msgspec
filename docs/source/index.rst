msgspec
=======

``msgspec`` is a *fast* and *friendly* serialization library for Python 3.8+,
currently supporting both `JSON <https://json.org>`__ and `MessagePack
<https://msgpack.org>`__ (``msgpack``). It integrates well with Python's `type
annotations <https://docs.python.org/3/library/typing.html>`__, supporting
ergonomic (and performant!) schema validation.

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

    # Serialize `alice` to `bytes` as JSON
    serialized_data = msgspec.json.encode(alice)

    # Deserialize and validate the message as a User type
    user = msgspec.json.decode(serialized_data, type=User)

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
    extending.rst
    benchmarks.rst
    api.rst
