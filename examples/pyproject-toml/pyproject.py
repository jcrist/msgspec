from typing import Any

import msgspec


class Base(
    msgspec.Struct,
    omit_defaults=True,
    forbid_unknown_fields=True,
    rename="kebab",
):
    """A base class holding some common settings.

    - We set ``omit_defaults = True`` to omit any fields containing only their
      default value from the output when encoding.
    - We set ``forbid_unknown_fields = True`` to error nicely if an unknown
      field is present in the input TOML. This helps catch typo errors early,
      and is also required per PEP 621.
    - We set ``rename = "kebab"`` to rename all fields to use kebab case when
      encoding/decoding, as this is the convention used in pyproject.toml. For
      example, this will rename ``requires_python`` to ``requires-python``.
    """

    pass


class BuildSystem(Base):
    requires: list[str] = []
    build_backend: str | None = None
    backend_path: list[str] = []


class Readme(Base):
    file: str | None = None
    text: str | None = None
    content_type: str | None = None


class License(Base):
    file: str | None = None
    text: str | None = None


class Contributor(Base):
    name: str | None = None
    email: str | None = None


class Project(Base):
    name: str | None = None
    version: str | None = None
    description: str | None = None
    readme: str | Readme | None = None
    license: str | License | None = None
    authors: list[Contributor] = []
    maintainers: list[Contributor] = []
    keywords: list[str] = []
    classifiers: list[str] = []
    urls: dict[str, str] = {}
    requires_python: str | None = None
    dependencies: list[str] = []
    optional_dependencies: dict[str, list[str]] = {}
    scripts: dict[str, str] = {}
    gui_scripts: dict[str, str] = {}
    entry_points: dict[str, dict[str, str]] = {}
    dynamic: list[str] = []


class PyProject(Base):
    build_system: BuildSystem | None = None
    project: Project | None = None
    tool: dict[str, dict[str, Any]] = {}


def decode(data: bytes | str) -> PyProject:
    """Decode a ``pyproject.toml`` file from TOML"""
    return msgspec.toml.decode(data, type=PyProject)


def encode(msg: PyProject) -> bytes:
    """Encode a ``PyProject`` object to TOML"""
    return msgspec.toml.encode(msg)
