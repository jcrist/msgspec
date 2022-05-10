API Docs
========

Struct
------

.. currentmodule:: msgspec

.. autoclass:: Struct
    :members:

.. autofunction:: defstruct


Raw
---

.. currentmodule:: msgspec

.. autoclass:: Raw
    :members:


JSON
----

.. currentmodule:: msgspec.json

.. autoclass:: Encoder
    :members: encode, encode_into

.. autoclass:: Decoder
    :members: decode

.. autofunction:: encode

.. autofunction:: decode


MessagePack
-----------

.. currentmodule:: msgspec.msgpack

.. autoclass:: Encoder
    :members: encode, encode_into

.. autoclass:: Decoder
    :members: decode

.. autoclass:: Ext
    :members:

.. autofunction:: encode

.. autofunction:: decode


Exceptions
----------

.. currentmodule:: msgspec

.. autoexception:: MsgspecError
    :show-inheritance:

.. autoexception:: EncodeError
    :show-inheritance:

.. autoexception:: DecodeError
    :show-inheritance:
