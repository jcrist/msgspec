API Docs
========

Struct
------

.. currentmodule:: msgspec

.. autoclass:: Struct
    :members:

.. autofunction:: defstruct


Meta
----

.. autoclass:: Meta
    :members:

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

.. autofunction:: format


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


JSON Schema
-----------

.. currentmodule:: msgspec.json

.. autofunction:: schema

.. autofunction:: schema_components


Exceptions
----------

.. currentmodule:: msgspec

.. autoexception:: MsgspecError
    :show-inheritance:

.. autoexception:: EncodeError
    :show-inheritance:

.. autoexception:: DecodeError
    :show-inheritance:

.. autoexception:: ValidationError
    :show-inheritance:
