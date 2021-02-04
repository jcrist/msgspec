import enum
from typing import Any, List, Set, Tuple, Dict, Union
import weakref
from . import core


_NODE_CACHE = weakref.WeakValueDictionary()

NoneType = type(None)


def type_node(type, *args):
    key = (type, *map(id, args))
    try:
        return _NODE_CACHE[key]
    except KeyError:
        out = _NODE_CACHE[key] = core.TypeNode(type, *args)
        return out


type_codes = {
    dict: core.DICT,
    Dict: core.DICT,
    list: core.LIST,
    List: core.LIST,
    set: core.SET,
    Set: core.SET,
    tuple: core.TUPLE,
    Tuple: core.TUPLE,
    Union: core.OPTIONAL,
}


def convert(t):
    if t in {None, NoneType, bool, int, float, str, bytes}:
        return t

    try:
        if issubclass(t, (core.Struct, enum.Enum)):
            return t
    except Exception:
        pass

    try:
        return type_codes[t]
    except Exception:
        pass

    try:
        origin = t.__origin__
        code = type_codes[origin]
    except Exception:
        raise TypeError(f"Can't convert type {t}") from None

    args = t.__args__

    if origin is Union:
        if len(args) == 2:
            if args[0] is NoneType:
                return type_node(code, convert(args[1]))
            elif args[1] is NoneType:
                return type_node(code, convert(args[0]))
        raise TypeError(f"Can't convert type {t}")

    if t.__parameters__:
        return type_node(code)
    return type_node(code, *(convert(a) for a in args))
