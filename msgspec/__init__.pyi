from typing import (
    Any,
    Callable,
    ClassVar,
    Dict,
    Final,
    Iterable,
    Literal,
    Mapping,
    Optional,
    Tuple,
    Type,
    TypeVar,
    Union,
    overload,
)

T = TypeVar("T")

class _Unset:
    pass

UNSET = _Unset()

@overload
def field(*, default: T) -> T: ...
@overload
def field(*, default_factory: Callable[[], T]) -> T: ...
@overload
def field() -> Any: ...

# Use `__dataclass_transform__` to catch more errors under pyright. Since we don't expose
# the underlying metaclass, hide it under an underscore name. See
# https://github.com/microsoft/pyright/blob/main/specs/dataclass_transforms.md
# for more information.

def __dataclass_transform__(
    *,
    eq_default: bool = True,
    order_default: bool = False,
    kw_only_default: bool = False,
    field_specifiers: Tuple[Union[type, Callable[..., Any]], ...] = (),
) -> Callable[[T], T]: ...
@__dataclass_transform__(field_specifiers=(field,))
class __StructMeta(type):
    def __new__(
        cls: Type[type], name: str, bases: tuple, classdict: dict
    ) -> "__StructMeta": ...

class Struct(metaclass=__StructMeta):
    __struct_fields__: ClassVar[Tuple[str, ...]]
    __match_args__: ClassVar[Tuple[str, ...]]
    def __init__(self, *args: Any, **kwargs: Any) -> None: ...
    def __init_subclass__(
        cls,
        tag: Union[None, bool, str, int, Callable[[str], Union[str, int]]] = None,
        tag_field: Union[None, str] = None,
        rename: Union[
            None,
            Literal["lower", "upper", "camel", "pascal", "kebab"],
            Callable[[str], Optional[str]],
            Mapping[str, str],
        ] = None,
        omit_defaults: bool = False,
        forbid_unknown_fields: bool = False,
        frozen: bool = False,
        eq: bool = True,
        order: bool = False,
        kw_only: bool = False,
        array_like: bool = False,
        gc: bool = True,
        weakref: bool = False,
    ) -> None: ...
    def __rich_repr__(self) -> Iterable[Tuple[str, Any]]: ...

def defstruct(
    name: str,
    fields: Iterable[Union[str, Tuple[str, Type], Tuple[str, Type, Any]]],
    *,
    bases: Tuple[Type[Struct], ...] = (),
    module: Optional[str] = None,
    namespace: Optional[Dict[str, Any]] = None,
    tag: Union[None, bool, str, int, Callable[[str], Union[str, int]]] = None,
    tag_field: Union[None, str] = None,
    rename: Union[
        None,
        Literal["lower", "upper", "camel", "pascal", "kebab"],
        Callable[[str], Optional[str]],
        Mapping[str, str],
    ] = None,
    omit_defaults: bool = False,
    forbid_unknown_fields: bool = False,
    frozen: bool = False,
    eq: bool = True,
    order: bool = False,
    kw_only: bool = False,
    array_like: bool = False,
    gc: bool = True,
    weakref: bool = False,
) -> Type[Struct]: ...

# Lie and say `Raw` is a subclass of `bytes`, so mypy will accept it in most
# places where an object that implements the buffer protocol is valid
class Raw(bytes):
    @overload
    def __new__(cls) -> "Raw": ...
    @overload
    def __new__(cls, msg: Union[bytes, str]) -> "Raw": ...
    def copy(self) -> "Raw": ...

class Meta:
    def __init__(
        self,
        *,
        gt: Union[int, float, None] = None,
        ge: Union[int, float, None] = None,
        lt: Union[int, float, None] = None,
        le: Union[int, float, None] = None,
        multiple_of: Union[int, float, None] = None,
        pattern: Union[str, None] = None,
        min_length: Union[int, None] = None,
        max_length: Union[int, None] = None,
        tz: Union[bool, None] = None,
        title: Union[str, None] = None,
        description: Union[str, None] = None,
        examples: Union[list, None] = None,
        extra_json_schema: Union[dict, None] = None,
        extra: Union[dict, None] = None,
    ): ...
    gt: Final[Union[int, float, None]]
    ge: Final[Union[int, float, None]]
    lt: Final[Union[int, float, None]]
    le: Final[Union[int, float, None]]
    multiple_of: Final[Union[int, float, None]]
    pattern: Final[Union[str, None]]
    min_length: Final[Union[int, None]]
    max_length: Final[Union[int, None]]
    tz: Final[Union[int, None]]
    title: Final[Union[str, None]]
    description: Final[Union[str, None]]
    examples: Final[Union[list, None]]
    extra_json_schema: Final[Union[dict, None]]
    extra: Final[Union[dict, None]]
    def __rich_repr__(self) -> Iterable[Tuple[str, Any]]: ...

def to_builtins(
    obj: Any,
    *,
    str_keys: bool = False,
    builtin_types: Union[Iterable[Type], None] = None,
    enc_hook: Optional[Callable[[Any], Any]] = None,
) -> Any: ...
@overload
def from_builtins(
    obj: Any,
    type: Type[T],
    *,
    str_keys: bool = False,
    str_values: bool = False,
    builtin_types: Union[Iterable[Type], None] = None,
    dec_hook: Optional[Callable[[Type, Any], Any]] = None,
) -> T: ...
@overload
def from_builtins(
    obj: Any,
    type: Any,
    *,
    str_keys: bool = False,
    builtin_types: Union[Iterable[Type], None] = None,
    dec_hook: Optional[Callable[[Type, Any], Any]] = None,
) -> Any: ...

class MsgspecError(Exception): ...
class EncodeError(MsgspecError): ...
class DecodeError(MsgspecError): ...
class ValidationError(DecodeError): ...

from . import inspect, json, msgpack, structs, toml, yaml

__version__: str
