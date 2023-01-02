from __future__ import annotations

from collections.abc import Iterable
from typing import Any, Union, Tuple

from ._core import Struct


__all__ = (
    "type_info",
    "multi_type_info",
    "Type",
    "Metadata",
    "AnyType",
    "NoneType",
    "BoolType",
    "IntType",
    "FloatType",
    "StrType",
    "BytesType",
    "ByteArrayType",
    "DateTimeType",
    "TimeType",
    "DateType",
    "UUIDType",
    "ExtType",
    "EnumType",
    "LiteralType",
    "CustomType",
    "UnionType",
    "ListType",
    "SetType",
    "FrozenSetType",
    "VarTupleType",
    "TupleType",
    "DictType",
    "Field",
    "TypedDictType",
    "NamedTupleType",
    "DataclassType",
    "StructType",
    "UNSET",
)


class _UnsetSingleton:
    """A singleton indicating a value is unset"""

    def __repr__(self):
        return "UNSET"

    def __reduce__(self):
        return "UNSET"


UNSET = _UnsetSingleton()


class Type(Struct):
    """The base Type."""


class Metadata(Type):
    """A type wrapping a subtype with additional metadata.

    Parameters
    ----------
    type: Type
        The subtype.
    extra_json_schema: dict, optional
        A dict of extra fields to set for the subtype when generating a
        json-schema.
    """

    type: Type
    extra_json_schema: Union[dict, None] = None


class AnyType(Type):
    """A type corresponding to `typing.Any`."""


class NoneType(Type):
    """A type corresponding to `None`."""


class BoolType(Type):
    """A type corresponding to `bool`."""


class IntType(Type):
    """A type corresponding to `int`.

    Parameters
    ----------
    gt: int, optional
        If set, an instance of this type must be greater than ``gt``.
    ge: int, optional
        If set, an instance of this type must be greater than or equal to ``ge``.
    lt: int, optional
        If set, an instance of this type must be less than to ``lt``.
    le: int, optional
        If set, an instance of this type must be less than or equal to ``le``.
    multiple_of: int, optional
        If set, an instance of this type must be a multiple of ``multiple_of``.
    """

    gt: Union[int, None] = None
    ge: Union[int, None] = None
    lt: Union[int, None] = None
    le: Union[int, None] = None
    multiple_of: Union[int, None] = None


class FloatType(Type):
    """A type corresponding to `float`.

    Parameters
    ----------
    gt: float, optional
        If set, an instance of this type must be greater than ``gt``.
    ge: float, optional
        If set, an instance of this type must be greater than or equal to ``ge``.
    lt: float, optional
        If set, an instance of this type must be less than to ``lt``.
    le: float, optional
        If set, an instance of this type must be less than or equal to ``le``.
    multiple_of: float, optional
        If set, an instance of this type must be a multiple of ``multiple_of``.
    """

    gt: Union[float, None] = None
    ge: Union[float, None] = None
    lt: Union[float, None] = None
    le: Union[float, None] = None
    multiple_of: Union[float, None] = None


class StrType(Type):
    """A type corresponding to `str`.

    Parameters
    ----------
    min_length: int, optional
        If set, an instance of this type must have length greater than or equal
        to ``min_length``.
    max_length: int, optional
        If set, an instance of this type must have length less than or equal
        to ``max_length``.
    pattern: str, optional
        If set, an instance of this type must match against this regex pattern.
        Note that the pattern is treated as **unanchored**.
    """

    min_length: Union[int, None] = None
    max_length: Union[int, None] = None
    pattern: Union[str, None] = None


class BytesType(Type):
    """A type corresponding to `bytes`.

    Parameters
    ----------
    min_length: int, optional
        If set, an instance of this type must have length greater than or equal
        to ``min_length``.
    max_length: int, optional
        If set, an instance of this type must have length less than or equal
        to ``max_length``.
    """

    min_length: Union[int, None] = None
    max_length: Union[int, None] = None


class ByteArrayType(Type):
    """A type corresponding to `bytearray`.

    Parameters
    ----------
    min_length: int, optional
        If set, an instance of this type must have length greater than or equal
        to ``min_length``.
    max_length: int, optional
        If set, an instance of this type must have length less than or equal
        to ``max_length``.
    """

    min_length: Union[int, None] = None
    max_length: Union[int, None] = None


class DateTimeType(Type):
    """A type corresponding to `datetime.datetime`.

    Parameters
    ----------
    tz: bool
        The timezone-requirements for an instance of this type. ``True``
        indicates a timezone-aware value is required, ``False`` indicates a
        timezone-aware value is required. The default is ``None``, which
        accepts either timezone-aware or timezone-naive values.
    """

    tz: Union[bool, None] = None


class TimeType(Type):
    """A type corresponding to `datetime.time`.

    Parameters
    ----------
    tz: bool
        The timezone-requirements for an instance of this type. ``True``
        indicates a timezone-aware value is required, ``False`` indicates a
        timezone-aware value is required. The default is ``None``, which
        accepts either timezone-aware or timezone-naive values.
    """

    tz: Union[bool, None] = None


class DateType(Type):
    """A type corresponding to `datetime.date`."""


class UUIDType(Type):
    """A type corresponding to `uuid.UUID`."""


class ExtType(Type):
    """A type corresponding to `msgspec.msgpack.Ext`."""


class EnumType(Type):
    """A type corresponding to an `enum.Enum` type.

    Parameters
    ----------
    cls: type
        The corresponding `enum.Enum` type.
    """

    cls: type


class LiteralType(Type):
    """A type corresponding to a `typing.Literal` type.

    Parameters
    ----------
    values: tuple
        A tuple of possible values for this literal instance. Only `str` or
        `int` literals are supported.
    """

    values: Union[Tuple[str, ...], Tuple[int, ...]]


class CustomType(Type):
    """A custom type.

    Parameters
    ----------
    cls: type
        The corresponding custom type.
    """

    cls: type


class UnionType(Type):
    """A union type.

    Parameters
    ----------
    types: Tuple[Type, ...]
        A tuple of possible types for this union.
    """

    types: Tuple[Type, ...]


class _ArrayType(Type):
    item_type: Type
    min_length: Union[int, None] = None
    max_length: Union[int, None] = None


class ListType(_ArrayType):
    """A type corresponding to a `list`.

    Parameters
    ----------
    item_type: Type
        The item type.
    min_length: int, optional
        If set, an instance of this type must have length greater than or equal
        to ``min_length``.
    max_length: int, optional
        If set, an instance of this type must have length less than or equal
        to ``max_length``.
    """


class VarTupleType(_ArrayType):
    """A type corresponding to a variadic `tuple`.

    Parameters
    ----------
    item_type: Type
        The item type.
    min_length: int, optional
        If set, an instance of this type must have length greater than or equal
        to ``min_length``.
    max_length: int, optional
        If set, an instance of this type must have length less than or equal
        to ``max_length``.
    """


class SetType(_ArrayType):
    """A type corresponding to a `set`.

    Parameters
    ----------
    item_type: Type
        The item type.
    min_length: int, optional
        If set, an instance of this type must have length greater than or equal
        to ``min_length``.
    max_length: int, optional
        If set, an instance of this type must have length less than or equal
        to ``max_length``.
    """


class FrozenSetType(_ArrayType):
    """A type corresponding to a `frozenset`.

    Parameters
    ----------
    item_type: Type
        The item type.
    min_length: int, optional
        If set, an instance of this type must have length greater than or equal
        to ``min_length``.
    max_length: int, optional
        If set, an instance of this type must have length less than or equal
        to ``max_length``.
    """


class TupleType(Type):
    """A type corresponding to `tuple`.

    Parameters
    ----------
    item_types: Tuple[Type, ...]
        A tuple of types for each element in the tuple.
    """

    item_types: Tuple[Type, ...]


class DictType(Type):
    """A type corresponding to `dict`.

    Parameters
    ----------
    key_type: Type
        The key type.
    value_type: Type
        The value type.
    min_length: int, optional
        If set, an instance of this type must have length greater than or equal
        to ``min_length``.
    max_length: int, optional
        If set, an instance of this type must have length less than or equal
        to ``max_length``.
    """

    key_type: Type
    value_type: Type
    min_length: Union[int, None] = None
    max_length: Union[int, None] = None


class Field(Struct):
    """A record describing a field in an object-like type.

    Parameters
    ----------
    name: str
        The field name as seen by Python code (e.g. ``field_one``).
    encode_name: str
        The name used when encoding/decoding the field. This may differ if
        the field is renamed (e.g. ``fieldOne``).
    type: Type
        The field type.
    required: bool, optional
        Whether the field is required. Note that if `required` is False doesn't
        necessarily mean that `default` or `default_factory` will be set -
        optional fields may exist with no default value.
    default: Any, optional
        A default value for the field. Will be `UNSET` if no default value is set.
    default_factory: Any, optional
        A callable that creates a default value for the field. Will be
        `UNSET` if no ``default_factory`` is set.
    """

    name: str
    encode_name: str
    type: Type
    required: bool = True
    default: Any = UNSET
    default_factory: Any = UNSET


class TypedDictType(Type):
    """A type corresponding to a `typing.TypedDict` type.

    Parameters
    ----------
    fields: Tuple[Field, ...]
        A tuple of fields in the TypedDict.
    """

    fields: Tuple[Field, ...]


class NamedTupleType(Type):
    """A type corresponding to a `typing.NamedTuple` type.

    Parameters
    ----------
    cls: type
        The corresponding NamedTuple type.
    fields: Tuple[Field, ...]
        A tuple of fields in the NamedTuple.
    """

    cls: type
    fields: Tuple[Field, ...]


class DataclassType(Type):
    """A type corresponding to a `dataclasses.dataclass` type.

    Parameters
    ----------
    cls: type
        The corresponding dataclass type.
    fields: Tuple[Field, ...]
        A tuple of fields in the dataclass.
    """

    cls: type
    fields: Tuple[Field, ...]


class StructType(Type):
    """A type corresponding to a `msgspec.Struct` type.

    Parameters
    ----------
    cls: type
        The corresponding Struct type.
    fields: Tuple[Field, ...]
        A tuple of fields in the Struct.
    tag_field: str or None, optional
        If set, the field name used for the tag in a tagged union.
    tag: str, int, or None, optional
        If set, the value used for hte tag in a tagged union.
    array_like: bool, optional
        Whether the struct is encoded as an array rather than an object.
    forbid_unknown_fields: bool, optional
        If ``False`` (the default) unknown fields are ignored when decoding. If
        ``True`` any unknown fields will result in an error.
    """

    cls: type
    fields: Tuple[Field, ...]
    tag_field: Union[str, None] = None
    tag: Union[str, int, None] = None
    array_like: bool = False
    forbid_unknown_fields: bool = False


def multi_type_info(types: Iterable[Any]) -> Tuple[Type, ...]:
    """Get information about multiple msgspec-compatible types.

    Parameters
    ----------
    types: an iterable of types
        The types to get info about.

    Returns
    -------
    Tuple[Type, ...]
    """
    # TODO
    return tuple(AnyType() for t in types)


def type_info(type: Any) -> Type:
    """Get information about a msgspec-compatible type.

    Note that if you need to inspect multiple types it's more efficient to call
    `multi_type_info` once with a sequence of types than calling `type_info`
    multiple times.

    Parameters
    ----------
    type: type
        The type to get info about.

    Returns
    -------
    Type
    """
    return multi_type_info([type])[0]
