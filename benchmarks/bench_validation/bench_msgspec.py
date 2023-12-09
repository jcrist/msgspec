from __future__ import annotations

import enum
import datetime

import msgspec


class Permissions(enum.Enum):
    READ = "READ"
    WRITE = "WRITE"
    READ_WRITE = "READ_WRITE"


class File(msgspec.Struct, kw_only=True, omit_defaults=True, tag="file"):
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_by: str | None = None
    updated_at: datetime.datetime | None = None
    nbytes: int
    permissions: Permissions


class Directory(msgspec.Struct, kw_only=True, omit_defaults=True, tag="directory"):
    name: str
    created_by: str
    created_at: datetime.datetime
    updated_by: str | None = None
    updated_at: datetime.datetime | None = None
    contents: list[File | Directory]


enc = msgspec.json.Encoder()
dec = msgspec.json.Decoder(Directory)

label = "msgspec"
encode = enc.encode
decode = dec.decode
