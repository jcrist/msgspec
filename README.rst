msgspec
=======

|github|

``msgspec`` is a fast and friendly implementation of the `MessagePack
<https://msgpack.org>`__ protocol for Python 3.8+. It supports message
validation through the use of schemas defined using Python's `type annotations
<https://docs.python.org/3/library/typing.html>`__.

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
    serialized_data = msgspec.encode(alice)

    # Deserialize and validate the message as a User type
    user = msgspec.Decoder(User).decode(serialized_data)

    assert user == alice

See `the documentation <https://jcristharif.com/msgspec/>`__ for more
information.

LICENSE
-------

New BSD. See the
`License File <https://github.com/jcrist/msgspec/blob/master/LICENSE>`_.

.. |github| image:: https://github.com/jcrist/msgspec/actions/workflows/ci.yml/badge.svg
   :target: https://github.com/jcrist/msgspec/actions/workflows/ci.yml
