<p align="center">
  <a href="https://github.com/Siyet/msgspec-arise">
    <img src="https://raw.githubusercontent.com/jcrist/msgspec/main/docs/_static/msgspec-logo-light.svg" width="35%" alt="msgspec-arise">
  </a>
</p>

<div align="center">

[![CI](https://github.com/Siyet/msgspec-arise/actions/workflows/ci.yml/badge.svg)](https://github.com/Siyet/msgspec-arise/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/Siyet/msgspec-arise.svg)](https://github.com/Siyet/msgspec-arise/blob/main/LICENSE)
[![PyPI Version](https://img.shields.io/pypi/v/msgspec-arise.svg)](https://pypi.org/project/msgspec-arise/)

</div>

> **Temporary community fork of [msgspec](https://github.com/jcrist/msgspec).**
> The original project is [no longer maintained](https://github.com/jcrist/msgspec/issues/990) — the sole owner is unreachable and the only other collaborator has stepped away.
> This fork exists to keep the project alive: merge pending fixes, review community PRs, and publish new releases.
> If `jcrist` returns and resumes maintenance, this fork will be archived.
> Drop-in replacement — `import msgspec` works as before.

`msgspec` is a *fast* serialization and validation library, with builtin
support for [JSON](https://json.org), [MessagePack](https://msgpack.org),
[YAML](https://yaml.org), and [TOML](https://toml.io/en/). It features:

- 🚀 **High performance encoders/decoders** for common protocols. The JSON and
  MessagePack implementations regularly
  [benchmark](https://jcristharif.com/msgspec/benchmarks.html) as the fastest
  options for Python.

- 🎉 **Support for a wide variety of Python types**. Additional types may be
  supported through
  [extensions](https://jcristharif.com/msgspec/extending.html).

- 🔍 **Zero-cost schema validation** using familiar Python type annotations. In
  [benchmarks](https://jcristharif.com/msgspec/benchmarks.html) `msgspec`
  decodes *and* validates JSON faster than
  [orjson](https://github.com/ijl/orjson) can decode it alone.

- ✨ **A speedy Struct type** for representing structured data. If you already
  use [dataclasses](https://docs.python.org/3/library/dataclasses.html) or
  [attrs](https://www.attrs.org/en/stable/),
  [structs](https://jcristharif.com/msgspec/structs.html) should feel familiar.
  However, they're
  [5-60x faster](https://jcristharif.com/msgspec/benchmarks.html#structs)
  for common operations.

All of this is included in a
[lightweight library](https://jcristharif.com/msgspec/benchmarks.html#library-size)
with no required dependencies.

---

`msgspec` may be used for serialization alone, as a faster JSON or
MessagePack library. For the greatest benefit though, we recommend using
`msgspec` to handle the full serialization & validation workflow:

**Define** your message schemas using standard Python type annotations.

```python
>>> import msgspec

>>> class User(msgspec.Struct):
...     """A new type describing a User"""
...     name: str
...     groups: set[str] = set()
...     email: str | None = None
```

**Encode** messages as JSON, or one of the many other supported protocols.

```python
>>> alice = User("alice", groups={"admin", "engineering"})

>>> alice
User(name='alice', groups={"admin", "engineering"}, email=None)

>>> msg = msgspec.json.encode(alice)

>>> msg
b'{"name":"alice","groups":["admin","engineering"],"email":null}'
```

**Decode** messages back into Python objects, with optional schema validation.

```python
>>> msgspec.json.decode(msg, type=User)
User(name='alice', groups={"admin", "engineering"}, email=None)

>>> msgspec.json.decode(b'{"name":"bob","groups":[123]}', type=User)
Traceback (most recent call last):
  File "<stdin>", line 1, in <module>
msgspec.ValidationError: Expected `str`, got `int` - at `$.groups[0]`
```

`msgspec` is designed to be as performant as possible, while retaining some of
the nicities of validation libraries like
[pydantic](https://docs.pydantic.dev/latest/). For supported types,
encoding/decoding a message with `msgspec` can be
[~10-80x faster than alternative libraries](https://jcristharif.com/msgspec/benchmarks.html).

<p align="center">
  <a href="https://jcristharif.com/msgspec/benchmarks.html">
    <img src="https://raw.githubusercontent.com/jcrist/msgspec/main/docs/_static/bench-validation.svg">
  </a>
</p>

See [the documentation](https://jcristharif.com/msgspec/) for more information.

## Installation

```bash
pip install msgspec-arise
```

## LICENSE

New BSD. See the
[License File](https://github.com/Siyet/msgspec-arise/blob/main/LICENSE).

## Acknowledgments

This is a community fork of [msgspec](https://github.com/jcrist/msgspec) by Jim Crist-Harif.
All credit for the original library goes to the original author and contributors.
