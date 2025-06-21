API Docs
========

Structs
-------

.. currentmodule:: msgspec

.. autoclass:: Struct

**StructMeta**

The metaclass for Struct types. This class can be subclassed to create custom struct behaviors. See :ref:`struct-meta-subclasses` for detailed information on using StructMeta subclasses.

.. autofunction:: field

.. autofunction:: defstruct

.. autofunction:: msgspec.structs.replace

.. autofunction:: msgspec.structs.asdict

.. autofunction:: msgspec.structs.astuple

.. autofunction:: msgspec.structs.force_setattr

.. autofunction:: msgspec.structs.fields

.. autoclass:: msgspec.structs.FieldInfo

.. autoclass:: msgspec.structs.StructConfig

.. autodata:: NODEFAULT
   :no-value:

Meta
----

.. autoclass:: Meta
    :members:


Raw
---

.. currentmodule:: msgspec

.. autoclass:: Raw
    :members:

Unset
-----

.. autodata:: UNSET
   :no-value:

.. autoclass:: UnsetType


JSON
----

.. currentmodule:: msgspec.json

.. autoclass:: Encoder
    :members: encode, encode_lines, encode_into

.. autoclass:: Decoder
    :members: decode, decode_lines

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

.. autofunction:: convert

.. autofunction:: to_builtins


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
.. autoclass:: MemoryViewType
.. autoclass:: DateTimeType
.. autoclass:: TimeType
.. autoclass:: DateType
.. autoclass:: TimeDeltaType
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
