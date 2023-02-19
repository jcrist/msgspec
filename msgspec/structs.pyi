from typing import Any, TypeVar, Union

from . import Struct

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
