from typing import TypeVar, Any

from . import Struct

S = TypeVar("S", bound=Struct, covariant=True)

def replace(struct: S, /, **changes: Any) -> S: ...
