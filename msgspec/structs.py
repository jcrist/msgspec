from typing import Any, Tuple, Type, Union

import msgspec
from msgspec import UNSET
from ._core import nodefault as _nodefault, Factory as _Factory
from ._utils import get_type_hints as _get_type_hints

__all__ = ("info", "StructField", "StructInfo", "UNSET")


def __dir__():
    return __all__


class StructField(msgspec.Struct):
    """A record describing a field in a struct type.

    Parameters
    ----------
    name: str
        The field name as seen by Python code (e.g. ``field_one``).
    encode_name: str
        The name used when encoding/decoding the field. This may differ if
        the field is renamed (e.g. ``fieldOne``).
    annotation: Any
        The full field type annotation.
    default: Any, optional
        A default value for the field. Will be `UNSET` if no default value is set.
    default_factory: Any, optional
        A callable that creates a default value for the field. Will be
        `UNSET` if no ``default_factory`` is set.
    """

    name: str
    encode_name: str
    annotation: Any
    default: Any = UNSET
    default_factory: Any = UNSET

    @property
    def required(self) -> bool:
        """A helper for checking whether a field is required"""
        return self.default is UNSET and self.default_factory is UNSET


class StructInfo(msgspec.Struct):
    """Information about a Struct type.

    Parameters
    ----------
    cls: type
        The corresponding Struct type.
    fields: Tuple[StructField, ...]
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

    cls: Type[msgspec.Struct]
    fields: Tuple[StructField, ...]
    tag_field: Union[str, None] = None
    tag: Union[str, int, None] = None
    array_like: bool = False
    forbid_unknown_fields: bool = False


def info(type: Type[msgspec.Struct]) -> StructInfo:
    """Get information about a Struct type.

    Parameters
    ----------
    type: type
        The Struct type to inspect.

    Returns
    -------
    info: StructInfo

    Notes
    -----
    This differs from `msgspec.inspect.type_info` in two ways:

    - It only works on `msgspec.Struct` types.
    - It doesn't recurse down into the field types.

    In many cases you probably want to use `msgspec.inspect.type_info` instead,
    as it handles all the types msgspec supports, and normalizes the various
    ways identical Python type annotations may be spelled into a type checked
    and consistent tree.

    See Also
    --------
    msgspec.inspect.type_info
    """
    hints = _get_type_hints(type)
    npos = len(type.__struct_fields__) - len(type.__struct_defaults__)
    fields = []
    for name, encode_name, default_obj in zip(
        type.__struct_fields__,
        type.__struct_encode_fields__,
        (_nodefault,) * npos + type.__struct_defaults__,
    ):
        default = default_factory = UNSET
        if isinstance(default_obj, _Factory):
            default_factory = default_obj.factory
        elif default_obj is not _nodefault:
            default = default_obj

        field = StructField(
            name=name,
            encode_name=encode_name,
            annotation=hints[name],
            default=default,
            default_factory=default_factory,
        )
        fields.append(field)

    return StructInfo(
        type,
        tuple(fields),
        tag_field=type.__struct_tag_field__,
        tag=type.__struct_tag__,
        array_like=type.array_like,
        forbid_unknown_fields=type.forbid_unknown_fields,
    )
