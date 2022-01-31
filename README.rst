msgspec
=======

|github| |pypi|

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

``msgspec`` is designed to be as performant as possible, while retaining some
of the nicities of validation libraries like `pydantic
<https://pydantic-docs.helpmanual.io/>`__. For supported types,
serializing/deserializing a message with ``msgspec`` can be *~2-20x faster*
than alternative libraries.

.. image:: https://github.com/jcrist/msgspec/raw/master/docs/source/_static/bench-1.png
    :target: https://jcristharif.com/msgspec/benchmarks.html

See `the documentation <https://jcristharif.com/msgspec/>`__ for more
information.

LICENSE
-------

New BSD. See the
`License File <https://github.com/jcrist/msgspec/blob/master/LICENSE>`_.

.. |github| image:: https://github.com/jcrist/msgspec/actions/workflows/ci.yml/badge.svg
   :target: https://github.com/jcrist/msgspec/actions/workflows/ci.yml
.. |pypi| image:: https://img.shields.io/pypi/v/msgspec.svg
   :target: https://pypi.org/project/msgspec/
