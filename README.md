<p align="center">
  <a href="https://jcristharif.com/msgspec/">
    <img src="https://raw.githubusercontent.com/jcrist/msgspec/main/docs/source/_static/msgspec-logo-light.svg" width="35%" alt="msgspec" />
  </a>
</p>

<p align="center">
  <a href="https://github.com/jcrist/msgspec/actions/workflows/ci.yml">
    <img src="https://github.com/jcrist/msgspec/actions/workflows/ci.yml/badge.svg">
  </a>
  <a href="https://jcristharif.com/msgspec/">
    <img src="https://img.shields.io/badge/docs-latest-blue.svg">
  </a>
  <a href="https://github.com/jcrist/msgspec/blob/main/LICENSE">
    <img src="https://img.shields.io/github/license/jcrist/msgspec.svg">
  </a>
  <a href="https://pypi.org/project/msgspec/">
    <img src="https://img.shields.io/pypi/v/msgspec.svg">
  </a>
  <a href="https://anaconda.org/conda-forge/msgspec">
    <img src="https://img.shields.io/conda/vn/conda-forge/msgspec.svg">
  </a>
  <a href="https://codecov.io/gh/jcrist/msgspec">
    <img src="https://codecov.io/gh/jcrist/msgspec/branch/main/graph/badge.svg">
  </a>
</p>


`msgspec` is a *fast* serialization and validation library, with builtin
support for [JSON](https://json.org), [MessagePack](https://msgpack.org),
[YAML](https://yaml.org), and [TOML](https://toml.io). It features:

- 🚀 **High performance encoders/decoders** for common protocols. The JSON and
  MessagePack implementations regularly
  [benchmark](https://jcristharif.com/msgspec/benchmarks.html) as the fastest
  options for Python.

- 🎉 **Support for a wide variety of Python types**. Additional types may be
  supported through
  [extensions](https://jcristharif.com/msgspec/extending.html).

- 🔍 **Zero-cost schema validation** using familiar Python type annotations. In
  [benchmarks](https://jcristharif.com/msgspec/benchmarks.html) `msgspec`
  decodes *and* validates JSON ~2x faster than
  [orjson](https://github.com/ijl/orjson) can decode it alone.

- ✨ **A speedy Struct type** for representing structured data. If you already
  use [dataclasses](https://docs.python.org/3/library/dataclasses.html) or
  [attrs](https://www.attrs.org),
  [structs](https://jcristharif.com/msgspec/structs.html) should feel familiar.
  However, they're
  [10-100x faster](https://jcristharif.com/msgspec/benchmarks.html#benchmark-structs>)
  for common operations.

All of this is included in a
[lightweight library](https://jcristharif.com/msgspec/benchmarks.html#benchmark-library-size)
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
[pydantic](https://pydantic-docs.helpmanual.io/). For supported types,
encoding/decoding a message with `msgspec` can be
[~2-80x faster than alternative libraries](https://jcristharif.com/msgspec/benchmarks.html).

<p align="center">
  <a href="https://jcristharif.com/msgspec/benchmarks.html">
    <img src="https://raw.githubusercontent.com/jcrist/msgspec/main/docs/source/_static/bench-1.svg">
  </a>
</p>

See [the documentation](https://jcristharif.com/msgspec/) for more information.


## LICENSE

New BSD. See the
[License File](https://github.com/jcrist/msgspec/blob/main/LICENSE).
