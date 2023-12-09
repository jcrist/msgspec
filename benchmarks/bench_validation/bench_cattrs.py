from __future__ import annotations

import enum
import datetime
from typing import Literal

import attrs
import cattrs.preconf.orjson


class Permissions(enum.Enum):
    READ = "READ"
    WRITE = "WRITE"
    READ_WRITE = "READ_WRITE"


@attrs.define(kw_only=True)
class File:
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_by: str | None = None
    updated_at: datetime.datetime | None = None
    nbytes: int
    permissions: Permissions
    type: Literal["file"] = "file"


@attrs.define(kw_only=True)
class Directory:
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_by: str | None = None
    updated_at: datetime.datetime | None = None
    contents: list[File | Directory]
    type: Literal["directory"] = "directory"


converter = cattrs.preconf.orjson.make_converter(omit_if_default=True)


def encode(obj):
    return converter.dumps(obj)


def decode(msg):
    return converter.loads(msg, Directory)


label = "cattrs"
