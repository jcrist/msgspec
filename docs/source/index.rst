msgspec
=======

``msgspec`` is *fast* and *friendly* serialization library for Python, with
builtin support for JSON_, MessagePack_, YAML_, and TOML_. It integrates well
with Python's `type annotations`_, providing ergonomic (and performant!) schema
validation.

**Define** your message schemas using standard Python type annotations.

.. code-block:: python

    >>> import msgspec

    >>> class User(msgspec.Struct):
    ...     """A new type describing a User"""
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

**Encode** messages as JSON, or one of the many other supported protocols.

.. code-block:: python

    >>> alice = User("alice", groups={"admin", "engineering"})

    >>> alice
    User(name='alice', groups={"admin", "engineering"}, email=None)

    >>> msg = msgspec.json.encode(alice)

    >>> msg
    b'{"name":"alice","groups":["admin","engineering"],"email":null}'

**Decode** messages back into Python objects, with optional schema validation.

.. code-block:: python

    >>> msgspec.json.decode(msg, type=User)
    User(name='alice', groups={"admin", "engineering"}, email=None)

    >>> msgspec.json.decode(b'{"name":"bob","groups":[123]}', type=User)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `str`, got `int` - at `$.groups[0]`

``msgspec`` is designed to be as performant as possible, while retaining some
of the nicities of validation libraries like pydantic_. For supported types,
encoding/decoding a message with ``msgspec`` can be :doc:`~2-40x faster than
alternative libraries <benchmarks>`.

Highlights
----------

- ``msgspec`` is **fast**. It :doc:`benchmarks <benchmarks>` as the fastest
  serialization library for Python, outperforming all other JSON/MessagePack
  libraries compared.
- ``msgspec`` is **friendly**. Through use of Python's type annotations,
  messages are :ref:`validated <typed-decoding>` during deserialization
  in a declaritive way. ``msgspec`` also works well with other type-checking
  tooling like mypy_ and pyright_, providing excellent editor integration.
- ``msgspec`` is **flexible**. It natively supports a wide range of Python
  builtin types. Support for additional types can also be added through
  :doc:`extensions <extending>`.
- ``msgspec`` is **lightweight**. It has no required dependencies, and the
  binary size is :ref:`a fraction of that of comparable libraries
  <benchmark-library-size>`.
- ``msgspec`` is **correct**. The encoders/decoders implemented are strictly
  compliant with their respective specifications, providing stronger guarantees
  of compatibility with other systems.

Used By
-------

``msgspec`` is used by many organizations and `open source projects
<https://github.com/jcrist/msgspec/network/dependents>`__, here we highlight a
few:

.. grid:: 2 2 4 4

    .. grid-item-card:: Pioreactor
        :link: https://pioreactor.com/

        .. image:: _static/pioreactor.png

    .. grid-item-card:: NautilusTrader
        :link: https://nautilustrader.io/

        .. image:: _static/nautilus-trader.png

    .. grid-item-card:: Starlite
        :link: https://starlite-api.github.io/starlite/latest/

        .. image:: _static/starlite.png


.. _type annotations: https://docs.python.org/3/library/typing.html
.. _JSON: https://json.org
.. _MessagePack: https://msgpack.org
.. _YAML: https://yaml.org
.. _TOML: https://toml.io
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _mypy: https://mypy.readthedocs.io
.. _pyright: https://github.com/microsoft/pyright

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: Overview

    why.rst
    install.rst

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: User Guide

    usage.rst
    supported-types.rst
    structs.rst
    constraints.rst
    jsonschema.rst
    schema-evolution.rst

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: Advanced

    extending.rst
    inspect.rst
    perf-tips.rst

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: Reference

    examples/index.rst
    api.rst
    benchmarks.rst
    changelog.rst
