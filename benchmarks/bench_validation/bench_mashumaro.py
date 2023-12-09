from __future__ import annotations

import enum
import dataclasses
import datetime
from typing import Literal

from mashumaro.mixins.orjson import DataClassORJSONMixin


class Permissions(enum.Enum):
    READ = "READ"
    WRITE = "WRITE"
    READ_WRITE = "READ_WRITE"


@dataclasses.dataclass(kw_only=True)
class File(DataClassORJSONMixin):
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_by: str | None = None
    updated_at: datetime.datetime | None = None
    nbytes: int
    permissions: Permissions
    type: Literal["file"] = "file"

    class Config:
        omit_default = True
        lazy_compilation = True


@dataclasses.dataclass(kw_only=True)
class Directory(DataClassORJSONMixin):
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_by: str | None = None
    updated_at: datetime.datetime | None = None
    contents: list[File | Directory]
    type: Literal["directory"] = "directory"

    class Config:
        omit_default = True
        lazy_compilation = True


label = "mashumaro"


def encode(x):
    return x.to_json()


def decode(msg):
    return Directory.from_json(msg)
