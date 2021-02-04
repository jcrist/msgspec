import weakref
from . import core


_NODE_CACHE = weakref.WeakValueDictionary()


def type_node(type, *args):
    key = (type, *map(id, args))
    try:
        return _NODE_CACHE[key]
    except KeyError:
        out = _NODE_CACHE[key] = core.TypeNode(type, *args)
        return out
