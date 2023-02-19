API Docs
========

Structs
-------

.. currentmodule:: msgspec

.. autoclass:: Struct

.. autofunction:: field

.. autofunction:: defstruct

.. autofunction:: msgspec.structs.replace

.. autofunction:: msgspec.structs.asdict

.. autofunction:: msgspec.structs.astuple

.. autoclass:: msgspec.structs.StructConfig

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


YAML
----

.. currentmodule:: msgspec.yaml

.. autofunction:: encode

.. autofunction:: decode


TOML
----

.. currentmodule:: msgspec.toml

.. autofunction:: encode

.. autofunction:: decode


JSON Schema
-----------

.. currentmodule:: msgspec.json

.. autofunction:: schema

.. autofunction:: schema_components


.. _inspect-api:


Converters
----------

.. currentmodule:: msgspec

.. autofunction:: to_builtins

.. autofunction:: from_builtins


Inspect
-------

.. currentmodule:: msgspec.inspect

.. autofunction:: type_info
.. autofunction:: multi_type_info
.. autoclass:: Type
.. autoclass:: Metadata
.. autoclass:: AnyType
.. autoclass:: NoneType
.. autoclass:: BoolType
.. autoclass:: IntType
.. autoclass:: FloatType
.. autoclass:: StrType
.. autoclass:: BytesType
.. autoclass:: ByteArrayType
.. autoclass:: DateTimeType
.. autoclass:: TimeType
.. autoclass:: DateType
.. autoclass:: UUIDType
.. autoclass:: DecimalType
.. autoclass:: ExtType
.. autoclass:: RawType
.. autoclass:: EnumType
.. autoclass:: LiteralType
.. autoclass:: CustomType
.. autoclass:: UnionType
    :members:
.. autoclass:: CollectionType
.. autoclass:: ListType
.. autoclass:: SetType
.. autoclass:: FrozenSetType
.. autoclass:: VarTupleType
.. autoclass:: TupleType
.. autoclass:: DictType
.. autoclass:: Field
.. autoclass:: TypedDictType
.. autoclass:: NamedTupleType
.. autoclass:: DataclassType
.. autoclass:: StructType
.. autodata:: UNSET


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
