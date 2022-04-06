from typing import Type
from typing import Callable, Tuple, Any, Union, TypeVar, ClassVar, Literal, overload

# Use `__dataclass_transform__` to catch more errors under pyright. Since we don't expose
# the underlying metaclass, hide it under an underscore name. See
# https://github.com/microsoft/pyright/blob/main/specs/dataclass_transforms.md
# for more information.

_T = TypeVar("_T")

def __dataclass_transform__(
    *,
    eq_default: bool = True,
    order_default: bool = False,
    kw_only_default: bool = False,
    field_descriptors: Tuple[Union[type, Callable[..., Any]], ...] = (()),
) -> Callable[[_T], _T]: ...
@__dataclass_transform__()
class __StructMeta(type):
    def __new__(
        cls: Type[type], name: str, bases: tuple, classdict: dict
    ) -> "__StructMeta": ...

class Struct(metaclass=__StructMeta):
    __struct_fields__: ClassVar[Tuple[str, ...]]
    def __init__(self, *args: Any, **kwargs: Any) -> None: ...
    def __init_subclass__(
        cls,
        tag: Union[None, bool, str, Callable[[str], str]] = None,
        tag_field: Union[None, str] = None,
        omit_defaults: bool = False,
        rename: Union[
            None, Literal["lower", "upper", "camel", "pascal"], Callable[[str], str]
        ] = None,
        frozen: bool = False,
        array_like: bool = False,
        nogc: bool = False,
    ) -> None: ...

# Lie and say `Raw` is a subclass of `bytes`, so mypy will accept it in most
# places where an object that implements the buffer protocol is valid
class Raw(bytes):
    @overload
    def __init__(self) -> None: ...
    @overload
    def __init__(self, msg: bytes) -> None: ...
    def copy(self) -> "Raw": ...

class MsgspecError(Exception): ...
class DecodeError(MsgspecError): ...
class EncodeError(MsgspecError): ...

from . import msgpack
from . import json

__version__: str
