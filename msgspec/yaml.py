import datetime as _datetime
from typing import Any, Callable, Optional, Type, TypeVar, Union, overload

from . import (
    DecodeError as _DecodeError,
    from_builtins as _from_builtins,
    to_builtins as _to_builtins,
)

__all__ = ("encode", "decode")


def __dir__():
    return __all__


def _import_pyyaml(name):
    try:
        import yaml
    except ImportError:
        raise ImportError(
            f"`msgspec.yaml.{name}` requires PyYAML be installed.\n\n"
            "Please either `pip` or `conda` install it as follows:\n\n"
            "  $ python -m pip install pyyaml  # using pip\n"
            "  $ conda install pyyaml          # or using conda"
        ) from None
    else:
        return yaml


def encode(obj: Any, *, enc_hook: Optional[Callable[[Any], Any]] = None) -> bytes:
    """Serialize an object as YAML.

    Parameters
    ----------
    obj : Any
        The object to serialize.
    enc_hook : callable, optional
        A callable to call for objects that aren't supported msgspec types.
        Takes the unsupported object and should return a supported object, or
        raise a TypeError.

    Returns
    -------
    data : bytes
        The serialized object.

    Notes
    -----
    This function requires that the third-party `PyYAML library
    <https://pyyaml.org/>`_ is installed.

    See Also
    --------
    decode
    """
    yaml = _import_pyyaml("encode")
    # Use the C extension if available
    Dumper = getattr(yaml, "CSafeDumper", yaml.SafeDumper)

    return yaml.dump_all(
        [
            _to_builtins(
                obj,
                builtin_types=(_datetime.datetime, _datetime.date),
                enc_hook=enc_hook,
            )
        ],
        encoding="utf-8",
        Dumper=Dumper,
        allow_unicode=True,
        sort_keys=False,
    )


T = TypeVar("T")


@overload
def decode(
    buf: Union[bytes, str], *, dec_hook: Optional[Callable[[Type, Any], Any]] = None
) -> Any:
    pass


@overload
def decode(
    buf: Union[bytes, str],
    *,
    type: Type[T] = ...,
    dec_hook: Optional[Callable[[Type, Any], Any]] = None,
) -> T:
    pass


@overload
def decode(
    buf: Union[bytes, str],
    *,
    type: Any = ...,
    dec_hook: Optional[Callable[[Type, Any], Any]] = None,
) -> Any:
    pass


def decode(
    buf: Union[bytes, str],
    *,
    type: Type[T] = Any,
    dec_hook: Optional[Callable[[Type, Any], Any]] = None,
) -> T:
    """Deserialize an object from YAML.

    Parameters
    ----------
    buf : bytes-like or str
        The message to decode.
    type : type, optional
        A Python type (in type annotation form) to decode the object as. If
        provided, the message will be type checked and decoded as the specified
        type. Defaults to `Any`, in which case the message will be decoded
        using the default YAML types.
    dec_hook : callable, optional
        An optional callback for handling decoding custom types. Should have
        the signature ``dec_hook(type: Type, obj: Any) -> Any``, where ``type``
        is the expected message type, and ``obj`` is the decoded representation
        composed of only basic YAML types. This hook should transform ``obj``
        into type ``type``, or raise a ``TypeError`` if unsupported.

    Returns
    -------
    obj : Any
        The deserialized object.

    Notes
    -----
    This function requires that the third-party `PyYAML library
    <https://pyyaml.org/>`_ is installed.

    See Also
    --------
    encode
    """
    yaml = _import_pyyaml("decode")
    # Use the C extension if available
    Loader = getattr(yaml, "CSafeLoader", yaml.SafeLoader)
    if not isinstance(buf, (str, bytes)):
        # call `memoryview` first, since `bytes(1)` is actually valid
        buf = bytes(memoryview(buf))
    try:
        obj = yaml.load(buf, Loader)
    except yaml.YAMLError as exc:
        raise _DecodeError(str(exc)) from None

    if type is Any:
        return obj
    return _from_builtins(
        obj, type, builtin_types=(_datetime.datetime, _datetime.date), dec_hook=dec_hook
    )
