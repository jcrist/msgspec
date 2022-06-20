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
  tooling like mypy_ and pyright_, providing excellent editor integration.
- ``msgspec`` is **flexible**. Unlike other libraries like `json` or
  `msgpack-python`_, ``msgspec`` natively supports a wider range of Python builtin
  types. Support for additional types can also be added through :doc:`extensions
  <extending>`.
- ``msgspec`` is **correct**. The encoders/decoders implemented are strictly
  compliant with the JSON_ and MessagePack_ specifications, providing stronger
  guarantees of compatibility with other systems.


Why msgspec?
------------

If you're writing a networked application, you'll need some agreed upon
protocol that your clients and servers can use to communicate. JSON is a decent
choice here (though there are many other options). It's ubiquitous, and Python
has many libraries for parsing it into builtin types (``json``, ``ujson``,
``orjson``, ...).

*However, servers don't just parse JSON, they also need to do something with
it*.

``msgspec`` goes above and beyond other Python JSON libraries to help with the
following:

- **Validation**

  If a field is missing from a request or has the wrong type, you probably want
  to raise a nice error message rather than just throwing a 500 error.

  ``msgspec`` lets you describe your schema via `type annotations`_, and will
  efficiently :ref:`validate <typed-deserialization>` messages against this
  schema while decoding.

  It also integrates well with static analysis tools like mypy_ and pyright_,
  helping you avoid whole classes of runtime errors.

- **Application Logic**

  What your application actually does! While builtin types like dicts are
  fine for writing application logic, they aren't as ergonomic as custom
  classes (no attribute access, poor type checking, ...).

  ``msgspec`` supports a :ref:`wide variety of types <supported-types>`, letting
  you decouple the objects your application logic uses from those that JSON
  natively supports.

- **Future Flexibility**

  Application needs change; you'll want to make sure your clients/servers won't
  break if the JSON schema evolves over time.

  To handle this, ``msgspec`` supports :ref:`"schema evolution"
  <schema-evolution>`. Messages can be sent between clients with different
  schemas without error, allowing systems to evolve over time.

While there are other tools in this space, ``msgspec`` should (in general) be
an :doc:`order of magnitude faster <benchmarks>` than other options. We also
hope that it's quick to learn and friendly to use, letting you focus less on
serialization and more on your application code.



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
.. _mypy: https://mypy.readthedocs.io
.. _pyright: https://github.com/microsoft/pyright

.. toctree::
    :hidden:
    :maxdepth: 2

    usage.rst
    structs.rst
    extending.rst
    perf-tips.rst
    benchmarks.rst
    api.rst
    changelog.rst
