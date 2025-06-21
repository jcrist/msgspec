msgspec-x
=========

``msgspec-x`` is a community-driven fork of the original msgspec library. It provides two namespaces:
- ``msgspec``: 100% compatible with the original API for drop-in replacement.
- ``msgspec_x``: Extended features and community contributions.

Do not install both ``msgspec`` and ``msgspec-x`` in the same environment.

``msgspec-x`` is a *fast* serialization and validation library, with builtin
support for JSON_, MessagePack_, YAML_, and TOML_. It features:

- 🚀 **High performance encoders/decoders** for common protocols. The JSON and
  MessagePack implementations regularly :doc:`benchmark <benchmarks>` as the
  fastest options for Python.

- 🎉 **Support for a wide variety of Python types**. Additional types may
  be supported through :doc:`extensions <extending>`.

- 🔍 **Zero-cost schema validation** using familiar Python type annotations.
  In :doc:`benchmarks <benchmarks>` ``msgspec-x`` decodes *and* validates JSON
  faster than orjson_ can decode it alone.

- ✨ **A speedy Struct type** for representing structured data. If you already
  use dataclasses_ or attrs_, :doc:`structs` should feel familiar. However,
  they're :ref:`5-60x <struct-benchmark>` faster for common operations.

- 🆕 **StructMeta subclasses support** for advanced metaclass programming. 
  Create custom struct behaviors while maintaining full compatibility with
  all msgspec operations. See :ref:`struct-meta-subclasses` for details.

All of this is included in a :ref:`lightweight library
<benchmark-library-size>` with no required dependencies.

-----

``msgspec-x`` may be used for serialization alone, as a faster JSON or
MessagePack library. For the greatest benefit though, we recommend using
``msgspec-x`` to handle the full serialization & validation workflow:

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

``msgspec-x`` is designed to be as performant as possible, while retaining some
of the nicities of validation libraries like pydantic_. For supported types,
encoding/decoding a message with ``msgspec-x`` can be :doc:`~10-80x faster than
alternative libraries <benchmarks>`.

Highlights
----------

- ``msgspec-x`` is **fast**. It :doc:`benchmarks <benchmarks>` as the fastest
  serialization library for Python, outperforming all other JSON/MessagePack
  libraries compared.

- ``msgspec-x`` is **friendly**. Through use of Python's type annotations,
  messages are :ref:`validated <typed-decoding>` during deserialization in a
  declarative way. ``msgspec-x`` also works well with other type-checking tooling
  like mypy_ and pyright_, providing excellent editor integration.

- ``msgspec-x`` is **flexible**. It natively supports a :doc:`wide range of
  Python builtin types <supported-types>`. Support for additional types can
  also be added through :doc:`extensions <extending>`.

- ``msgspec-x`` is **lightweight**. It has no required dependencies, and the
  binary size is :ref:`a fraction of that of comparable libraries
  <benchmark-library-size>`.

- ``msgspec-x`` is **correct**. The encoders/decoders implemented are strictly
  compliant with their respective specifications, providing stronger guarantees
  of compatibility with other systems.

- ``msgspec-x`` is **extensible**. The new :ref:`StructMeta subclasses 
  <struct-meta-subclasses>` support enables advanced users to create custom
  struct behaviors through metaclass programming while maintaining full
  compatibility with all msgspec operations.

Used By
-------

``msgspec-x`` is used by many organizations and `open source projects
<https://github.com/nightsailer/msgspec-x/network/dependents>`__, here we highlight a
few:

.. grid:: 2 2 4 4

    .. grid-item-card:: NautilusTrader
        :link: https://nautilustrader.io/

        .. image:: _static/nautilus-trader.png

    .. grid-item-card:: Litestar
        :link: https://litestar.dev/

        .. image:: _static/litestar.png

    .. grid-item-card:: Sanic
        :link: https://sanic.dev/en/

        .. image:: _static/sanic.png

    .. grid-item-card:: Mosec
        :link: https://mosecorg.github.io/mosec/

        .. image:: _static/mosec.png

    .. grid-item-card:: Pioreactor
        :link: https://pioreactor.com/

        .. image:: _static/pioreactor.png

    .. grid-item-card:: Zero
        :link: https://github.com/Ananto30/zero

        .. image:: _static/zero.png

    .. grid-item-card:: anywidget
        :link: https://anywidget.dev/

        .. image:: _static/anywidget.png

    .. grid-item-card:: esmerald
        :link: https://esmerald.dev/

        .. image:: _static/esmerald.png


.. _type annotations: https://docs.python.org/3/library/typing.html
.. _JSON: https://json.org
.. _MessagePack: https://msgpack.org
.. _YAML: https://yaml.org
.. _TOML: https://toml.io
.. _attrs: https://www.attrs.org
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _orjson: https://github.com/ijl/orjson
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _mypy: https://mypy.readthedocs.io
.. _pyright: https://github.com/microsoft/pyright

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: Overview

    why.rst
    install.rst
    benchmarks.rst

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: User Guide

    usage.rst
    supported-types.rst
    structs.rst
    constraints.rst
    converters.rst
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

    api.rst
    examples/index.rst
    changelog.rst