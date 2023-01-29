msgspec
=======

|github| |pypi| |conda| |codecov| |license|

``msgspec`` is a *fast* and *friendly* serialization library for Python. It
includes:

- 🚀 **High performance encoders/decoders** for common protocols (currently
  JSON, MessagePack, YAML and TOML). The JSON and MessagePack implementations
  regularly benchmark_ as the fastest options for Python.

- 📏 **Zero-cost schema validation** using familiar Python type annotations.
  In benchmarks_ ``msgspec`` decodes *and* validates JSON ~2x faster than
  orjson_ can decode it alone.

- ✨ **A speedy Struct type** for representing structured data. If you already
  use dataclasses_ or attrs_, structs_ should feel familiar. However, they're
  `10-100x <https://jcristharif.com/msgspec/benchmarks.html#benchmark-structs>`__
  faster for common operations.

All of this is included in a `lightweight library
<https://jcristharif.com/msgspec/benchmarks.html#benchmark-library-size>`__
with no required dependencies.

-----

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
encoding/decoding a message with ``msgspec`` can be `~2-80x faster than
alternative libraries <https://jcristharif.com/msgspec/benchmarks.html>`__.

.. image:: https://github.com/jcrist/msgspec/raw/main/docs/source/_static/bench-1.svg
    :target: https://jcristharif.com/msgspec/benchmarks.html

See `the documentation <https://jcristharif.com/msgspec/>`__ for more
information.

LICENSE
-------

New BSD. See the
`License File <https://github.com/jcrist/msgspec/blob/main/LICENSE>`_.

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _attrs: https://www.attrs.org
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _orjson: https://github.com/ijl/orjson
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _structs: https://jcristharif.com/msgspec/structs.html
.. _benchmark: https://jcristharif.com/msgspec/benchmarks.html
.. _benchmarks: https://jcristharif.com/msgspec/benchmarks.html

.. |github| image:: https://github.com/jcrist/msgspec/actions/workflows/ci.yml/badge.svg
   :target: https://github.com/jcrist/msgspec/actions/workflows/ci.yml
.. |pypi| image:: https://img.shields.io/pypi/v/msgspec.svg
   :target: https://pypi.org/project/msgspec/
.. |conda| image:: https://img.shields.io/conda/vn/conda-forge/msgspec.svg
   :target: https://anaconda.org/conda-forge/msgspec
.. |codecov| image:: https://codecov.io/gh/jcrist/msgspec/branch/main/graph/badge.svg
   :target: https://codecov.io/gh/jcrist/msgspec
.. |license| image:: https://img.shields.io/github/license/jcrist/msgspec.svg
   :target: https://github.com/jcrist/msgspec/blob/main/LICENSE
