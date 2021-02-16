msgspec
=======

``msgspec`` is a fast and friendly implementation of the `MessagePack
<https://msgpack.org>`__ protocol for Python 3.8+. It supports message
validation through the use of schemas defined using Python's `type annotations
<https://docs.python.org/3/library/typing.html>`__.

.. code-block:: python

    from typing import Optional, List
    from msgspec import Struct, Encoder, Decoder

    # Define a schema for a `User` type
    class User(Struct):
        name: str
        groups: List[str] = set()
        email: Optional[str] = None

    # Create a `User` object
    alice = User("alice", groups=["admin", "engineering"])

    # Serialize `alice` to `bytes` using the MessagePack protocol
    serialized_data = Encoder().encode(alice)

    # Deserialize and validate the message as a User type
    user = Decoder(User).decode(serialized_data)

    assert user == alice


LICENSE
-------

New BSD. See the
`License File <https://github.com/jcrist/msgspec/blob/master/LICENSE>`_.
