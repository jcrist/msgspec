msgspec
=======

|github| |pypi| |conda| |codecov|

``msgspec`` is a *fast* and *friendly* serialization library for Python,
supporting both JSON_ and MessagePack_. It integrates well with Python's `type
annotations`_, providing ergonomic (and performant!) schema validation.

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
    msgspec.ValidationError: Expected `str`, got `int` - at `$.groups[0]`

``msgspec`` is designed to be as performant as possible, while retaining some
of the nicities of validation libraries like pydantic_. For supported types,
encoding/decoding a message with ``msgspec`` can be *~2-40x faster* than
alternative libraries.

.. image:: https://github.com/jcrist/msgspec/raw/main/docs/source/_static/bench-1.svg
    :target: https://jcristharif.com/msgspec/benchmarks.html

See `the documentation <https://jcristharif.com/msgspec/>`__ for more
information.

LICENSE
-------

New BSD. See the
`License File <https://github.com/jcrist/msgspec/blob/main/LICENSE>`_.

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _JSON: https://json.org
.. _MessagePack: https://msgpack.org
.. _pydantic: https://pydantic-docs.helpmanual.io/

.. |github| image:: https://github.com/jcrist/msgspec/actions/workflows/ci.yml/badge.svg
   :target: https://github.com/jcrist/msgspec/actions/workflows/ci.yml
.. |pypi| image:: https://img.shields.io/pypi/v/msgspec.svg
   :target: https://pypi.org/project/msgspec/
.. |conda| image:: https://img.shields.io/conda/vn/conda-forge/msgspec.svg
   :target: https://anaconda.org/conda-forge/msgspec
.. |codecov| image:: https://codecov.io/gh/jcrist/msgspec/branch/main/graph/badge.svg
   :target: https://codecov.io/gh/jcrist/msgspec
