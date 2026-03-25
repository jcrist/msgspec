"""Mypy plugin for msgspec.

Provides type-checking support for ``msgspec.structs.replace``, validating
that keyword arguments match the fields of the target Struct.

Enable by adding ``msgspec.mypy`` to your mypy plugins::

    # mypy.ini or pyproject.toml
    [mypy]
    plugins = [msgspec.mypy]
"""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Callable, Mapping
from functools import reduce

import mypy.plugin
from mypy.meet import meet_types
from mypy.nodes import ARG_NAMED_OPT, ARG_POS, FuncDef
from mypy.plugin import FunctionSigContext, Plugin
from mypy.typeops import expand_type_by_instance
from mypy.types import (
    AnyType,
    CallableType,
    Instance,
    ProperType,
    Type,
    TypeVarType,
    UninhabitedType,
    UnionType,
    get_proper_type,
)

try:
    from mypy.typeops import format_type_bare
except ImportError:
    from mypy.checker import format_type_bare  # type: ignore[no-redef]

MSGSPEC_STRUCT_FULLNAME = "msgspec.Struct"


def _is_msgspec_struct(typ: Instance) -> bool:
    return any(base.fullname == MSGSPEC_STRUCT_FULLNAME for base in typ.type.mro)


def _get_struct_init_type(typ: Instance) -> CallableType | None:
    if not _is_msgspec_struct(typ):
        return None
    init_method = typ.type.get_method("__init__")
    if not isinstance(init_method, FuncDef) or not isinstance(
        init_method.type, CallableType
    ):
        return None
    return init_method.type


def _fail_not_struct(
    ctx: FunctionSigContext, t: ProperType, parent_t: ProperType
) -> None:
    t_name = format_type_bare(t, ctx.api.options)
    if parent_t is t:
        msg = (
            f'Argument 1 to "replace" has a variable type "{t_name}"'
            f" not bound to a msgspec Struct"
            if isinstance(t, TypeVarType)
            else f'Argument 1 to "replace" has incompatible type "{t_name}";'
            f" expected a msgspec Struct"
        )
    else:
        pt_name = format_type_bare(parent_t, ctx.api.options)
        msg = (
            f'Argument 1 to "replace" has type "{pt_name}" whose item "{t_name}"'
            f" is not bound to a msgspec Struct"
            if isinstance(t, TypeVarType)
            else f'Argument 1 to "replace" has incompatible type "{pt_name}" whose'
            f' item "{t_name}" is not a msgspec Struct'
        )
    ctx.api.fail(msg, ctx.context)


def _get_expanded_struct_types(
    ctx: FunctionSigContext,
    typ: ProperType,
    display_typ: ProperType,
    parent_typ: ProperType,
) -> list[Mapping[str, Type]] | None:
    if isinstance(typ, AnyType):
        return None
    if isinstance(typ, UnionType):
        ret: list[Mapping[str, Type]] | None = []
        for item in typ.relevant_items():
            item = get_proper_type(item)
            item_types = _get_expanded_struct_types(ctx, item, item, parent_typ)
            if ret is not None and item_types is not None:
                ret += item_types
            else:
                ret = None  # but keep iterating to emit all errors
        return ret
    if isinstance(typ, TypeVarType):
        return _get_expanded_struct_types(
            ctx, get_proper_type(typ.upper_bound), display_typ, parent_typ
        )
    if isinstance(typ, Instance):
        init_func = _get_struct_init_type(typ)
        if init_func is None:
            _fail_not_struct(ctx, display_typ, parent_typ)
            return None
        init_func = expand_type_by_instance(init_func, typ)
        # [1:] to skip the self argument of Struct.__init__
        field_names = [n for n in init_func.arg_names[1:] if n is not None]
        field_types = init_func.arg_types[1:]
        return [dict(zip(field_names, field_types))]
    _fail_not_struct(ctx, display_typ, parent_typ)
    return None


def _meet_fields(types: list[Mapping[str, Type]]) -> Mapping[str, Type]:
    field_to_types: defaultdict[str, list[Type]] = defaultdict(list)
    for fields in types:
        for name, typ in fields.items():
            field_to_types[name].append(typ)
    return {
        name: (
            get_proper_type(reduce(meet_types, f_types))
            if len(f_types) == len(types)
            else UninhabitedType()
        )
        for name, f_types in field_to_types.items()
    }


def _replace_sig_callback(ctx: mypy.plugin.FunctionSigContext) -> CallableType:
    if len(ctx.args) != 2:
        ctx.api.fail(
            f'"{ctx.default_signature.name}" has unexpected type annotation',
            ctx.context,
        )
        return ctx.default_signature

    if len(ctx.args[0]) != 1:
        return ctx.default_signature  # leave it to the type checker to complain

    inst_arg = ctx.args[0][0]
    inst_type = get_proper_type(ctx.api.get_expression_type(inst_arg))
    inst_type_str = format_type_bare(inst_type, ctx.api.options)

    struct_types = _get_expanded_struct_types(ctx, inst_type, inst_type, inst_type)
    if struct_types is None:
        return ctx.default_signature
    fields = _meet_fields(struct_types)

    return CallableType(
        arg_names=["struct", *fields.keys()],
        arg_kinds=[ARG_POS] + [ARG_NAMED_OPT] * len(fields),
        arg_types=[inst_type, *fields.values()],
        ret_type=inst_type,
        fallback=ctx.default_signature.fallback,
        name=f"replace of {inst_type_str}",
    )


class MsgspecPlugin(Plugin):
    def get_function_signature_hook(
        self, fullname: str
    ) -> Callable[[FunctionSigContext], CallableType] | None:
        if fullname == "msgspec.structs.replace":
            return _replace_sig_callback
        return None


def plugin(version: str) -> type[MsgspecPlugin]:
    return MsgspecPlugin
