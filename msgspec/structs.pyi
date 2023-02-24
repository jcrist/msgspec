from typing import Any, TypeVar, Union

from . import UNSET, Struct

S = TypeVar("S", bound=Struct, covariant=True)

def replace(struct: S, /, **changes: Any) -> S: ...
def asdict(struct: Struct) -> dict: ...
def astuple(struct: Struct) -> tuple: ...

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
    tag: Union[str, int, None]
    tag_field: Union[str, None]

class FieldInfo(Struct):
    name: str
    encode_name: str
    type: Any
    default: Any = UNSET
    default_factory: Any = UNSET

    @property
    def required(self) -> bool: ...

def fields(type_or_instance: Struct | type[Struct]) -> tuple[FieldInfo]: ...
