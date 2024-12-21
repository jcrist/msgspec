Schema Evolution
================

``msgspec`` includes support for "schema evolution", meaning that:

- Messages serialized with an older version of a schema will be deserializable
  using a newer version of the schema.
- Messages serialized with a newer version of the schema will be deserializable
  using an older version of the schema.

This can be useful if, for example, you have clients and servers with
mismatched versions.

For schema evolution to work smoothly, you need to follow a few guidelines:

1. Any new fields on a `msgspec.Struct` must specify default values.
2. Structs with ``array_like=True`` must not reorder fields, and any new fields
   must be appended to the end (and have defaults).
3. Don't change the type annotations for existing messages or fields.
4. Don't change the type codes or implementations for any defined
   :ref:`extensions <defining-extensions>` (MessagePack only).

For example, suppose we had a `msgspec.Struct` type representing a user:

.. code-block:: python

    >>> import msgspec

    >>> from typing import Set, Optional

    >>> class User(msgspec.Struct):
    ...     """A struct representing a user"""
    ...     name: str
    ...     groups: Set[str] = set()
    ...     email: Optional[str] = None

Then suppose we wanted to add a new ``phone`` field to this struct in a way
that wouldn't break clients/servers still using the prior definition. To
accomplish this, we add ``phone`` as an _optional_ field (defaulting to
``None``), at the end of the struct.

.. code-block:: python

    >>> class User2(msgspec.Struct):
    ...     """An updated version of the User struct, now with a phone number"""
    ...     name: str
    ...     groups: Set[str] = set()
    ...     email: Optional[str] = None
    ...     phone : Optional[str] = None

Messages serialized using both the old and new schemas can still be exchanged
without error. If an old message is deserialized using the new schema, the
missing fields all have default values that will be used. Likewise, if a new
message is deserialized with the old schema the unknown new fields will be
efficiently skipped without decoding.

.. code-block:: python

    >>> old_dec = msgspec.json.Decoder(User)

    >>> new_dec = msgspec.json.Decoder(User2)

    >>> new_msg = msgspec.json.encode(
    ...     User2("bob", groups={"finance"}, phone="512-867-5309")
    ... )

    >>> old_dec.decode(new_msg)  # deserializing a new msg with an older decoder
    User(name='bob', groups={'finance'}, email=None)

    >>> old_msg = msgspec.json.encode(
    ...     User("alice", groups={"admin", "engineering"})
    ... )

    >>> new_dec.decode(old_msg) # deserializing an old msg with a new decoder
    User2(name="alice", groups={"admin", "engineering"}, email=None, phone=None)
