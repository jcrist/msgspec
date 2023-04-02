from typing import Any, TypeVar, Union

from . import NODEFAULT, Struct

S = TypeVar("S", bound=Struct, covariant=True)

def replace(struct: S, /, **changes: Any) -> S: ...
def asdict(struct: Struct) -> dict[str, Any]: ...
def astuple(struct: Struct) -> tuple[Any, ...]: ...

class StructConfig:
    frozen: bool
    eq: bool
    order: bool
    array_like: bool
    gc: bool
    repr_omit_defaults: bool
    omit_defaults: bool
    forbid_unknown_fields: bool
    weakref: bool
    dict: bool
    tag: Union[str, int, None]
    tag_field: Union[str, None]

class FieldInfo(Struct):
    name: str
    encode_name: str
    type: Any
    default: Any = NODEFAULT
    default_factory: Any = NODEFAULT

    @property
    def required(self) -> bool: ...

def fields(type_or_instance: Struct | type[Struct]) -> tuple[FieldInfo]: ...
