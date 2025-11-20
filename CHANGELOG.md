# Changelog

---

## Unreleased

## [0.19.0] - 2024-12-27

- Improve JSON encoding performance by up to 40% (#647).
- Ensure `tuple` and `frozenset` default values are treated identically whether specified by value or `default_factory` (#653).
- Fix memory leak of `match_args` in `StructConfig` object (#684).
- Fix memory leak in `Raw.copy()` (#709).
- Update `decode` signatures for PEP 688 (#740).
- Generate `__replace__` method on `Struct` types, for use with `copy.replace` (#747).
- Fix incorrect decoding of certain > 64 bit integers (#751).
- Call `__post_init__` when converting from an object to a Struct (#752).
- **BREAKING**: Expand buffer when `encode_into` is passed a buffer smaller than `offset` (#753).
- Support `Raw` objects as inputs to `convert` (#754).
- Error nicely when a dataclass *type* (instead of an *instance*) is passed to `encode` (#755).
- Drop support for Python 3.8 (#756, #763).
- Add support for Python 3.13 (#711).
- Remove deprecated `from_builtins` (#761).
- Support encoding any `Enum` type whose `.value` is a supported type (#757).
- Don't fail eagerly when processing generic types with unsupported `__parameters__` (#772).
- Use `eval_type_backport` to backport type annotations to Python 3.9 (#773).

## [0.18.6] - 2024-01-22

- Support coercing integral floats to ints when `strict=False` (#619).
- Preserve leading `_` when renaming fields to camel or pascal case (#620).
- Support zero-copy decoding binary fields to a `memoryview` (#624).
- Fix a bug when inheriting from the same `Generic` base class multiple times (#626).
- Add an `order` option to all encoders for enforcing deterministic/sorted ordering when encoding. This can help provide a more consistent or human readable output (#627).
- Support inheriting from any slots-class when defining a new `Struct` type with `gc=False` (#635).
- Automatically infer the input field naming convention when converting non-dict mappings or arbitrary objects to `Struct` types in `msgspec.convert` (#636).

## [0.18.5] - 2023-12-13

- Support unhashable `Annotated` metadata in `msgspec.inspect.type_info` (#566).
- Fix bug preventing decoding dataclasses/attrs types with default values and `slots=True, frozen=True` (#569).
- Support passing parametrized generic struct types to `msgspec.structs.fields` (#571).
- Validate `str` constraints on dict keys when decoding msgpack (#577).
- Support `UUID` subclasses as inputs to `msgspec.convert` (#592).
- Call `__eq__` from generated `__ne__` if user defines manual `__eq__` method on a `Struct` type (#593).
- Include the `Struct` type in the generated hash (#595).
- Add a `cache_hash` struct option (#596).
- Fix a bug around caching of dataclass type info when dealing with subclasses of dataclasses (#599).
- Add `msgspec.structs.force_setattr` (#600).
- Support custom dict key types in JSON encoder and decoder (#602).
- Include `dict` key constraints in generated JSON schema via the `propertyNames` field (#604).
- Add a `schema_hook` for generating JSON schemas for custom types (#605).
- Add support for Python 3.12's `type` aliases (#606).

## [0.18.4] - 2023-10-05

- Resolve an issue leading to periodic segfaults when importing ``msgspec`` on CPython 3.12 (#561)

## [0.18.3] - 2023-10-03

- Improve type annotation for `Struct.__rich_repr__` (#557)
- Add pre-built wheels for Python 3.12 (#558)

## [0.18.2] - 2023-08-26

- Support `Enum._missing_` [hooks](https://docs.python.org/3/library/enum.html#enum.Enum._missing_) for handling unknown enum values (#532).
- Fix JSON encoding of `datetime.datetime` objects with `zoneinfo.ZoneInfo` timezone components (#534).
- Add support for `attrs` [validators](https://www.attrs.org/en/stable/examples.html#validators>) (#538).
- Relax datetime/time parsing format to allow some RFC3339 extensions from ISO8601 (#539).

## [0.18.1] - 2023-08-16

- Support custom `builtin_types` in `msgspec.to_builtins` (#517).
- Try `getattr` before `getitem` when converting with `from_attributes=True` (#519).
- More efficient module state access in top-level functions (#521).

## [0.18.0] - 2023-08-10

- Add a new `msgspec.json.Decoder.decode_lines` method for decoding newline-delimited JSON into a list of values (#485).
- Support for decoding UUIDs from binary values (#499).
- Support for encoding UUIDs in alternate formats (#499).
- Overhaul how dataclasses are encoded to support more dataclass-like objects (#501).
- Encode all declared fields on a dataclass (#501).
- Support encoding `edgedb.Object` instances as dataclass-like objects (#501).
- Improve performance when json decoding `float` values (#510).
- Support for JSON encoding dicts with `float` keys (#510).
- Support for JSON decoding dicts with `float` keys (#510).
- Add `float_hook` to `msgspec.json.Decoder` to support changing the default for how JSON floats are decoded (#511).

## [0.17.0] - 2023-07-12

- Ensure `None` may be explicitly passed to `defstruct` for `module`/`namespace`/`bases` (#445).
- Support decoding `datetime.datetime` values from `int`/`float` values (interpreted as seconds since the Unix epoch) when `strict=False` (#452).
- Support subclasses of collection types (`list`, `dict`, ...) as inputs to `convert` (#453).
- Support `str` subclasses as keys in `to_builtins` and all protocol `encode` methods (#454).
- Improved performance when JSON encoding `decimal.Decimal` values (#455).
- Improved performance when JSON encoding `int`/`float` values (#458).
- Improved performance when JSON encoding `str` values (#459).
- Wrap errors in `dec_hook` with a `ValidationError` (#460).
- Support decoding `decimal.Decimal` values from numeric values (#463)
- Support encoding `decimal.Decimal` values as numeric values (#465).
- Support converting `decimal.Decimal` values to `float` in `convert` (#466).
- Preliminary support for CPython 3.12 beta releases (#467).
- Support decoding integers that don't fit into an `int64`/`uint64` (#469).
- Add a new optional `__post_init__` method for `Struct` types (#470).
- Support decoding `0`/`1` into `bool` types when `strict=False` (#471).
- Wrap errors raised in `__post_init__`/`__attrs_post_init__` in a `ValidationError` when decoding (#472).
- Add native support for encoding/decoding `datetime.timedelta` types (#475).
- Add a new `msgspec.json.Encoder.encode_lines` method for encoding an iterable of values as newline-delimited JSON (#479).

## [0.16.0] - 2023-06-12

- Deprecate `msgspec.from_builtins` in favor of [`msgspec.convert`](https://jcristharif.com/msgspec/api.html#msgspec.convert). The new `convert` function provides a superset of the functionality available in the old `from_builtins` function. See the [converters docs](https://jcristharif.com/msgspec/converters.html) for more information (#431).
- Add a `from_attributes` argument to `msgspec.convert` for allowing conversion between object types with matching attribute names. One use case for this is converting ORM objects to `Struct` or `dataclasses` types (#419).
- Support passing generic `Mapping` objects as inputs to `msgspec.convert`. These may be coerced to `dict`/`Struct`/`dataclasses`/`attrs` types (#427).
- Add a new `strict` keyword argument to all `decode` functions, `Decoder` classes, as well as `msgspec.convert`. This defaults to `True`, setting it to false enables a wider set of coercion rules (e.g. coercing a `str` input to an `int`). See ["Strict" vs "Lax" Mode](https://jcristharif.com/msgspec/usage.html#strict-vs-lax-mode) for more information (#434).
- Allow any of msgspec's [supported types](https://jcristharif.com/msgspec/supported-types.html) as inputs to `msgspec.convert` (#431, #418).
- Passthrough input unchanged when coercing to `typing.Any` type in `msgspec.convert` (#435).
- Support parametrizing `Decoder` types at runtime (#415).
- Support encoding subclasses of `UUID` (#429).

## [0.15.1] - 2023-05-19

- Fix a reference counting bug introduced in 0.15.0 when decoding naive (no timezone) `datetime`/`time` objects in both the `msgpack` and `json` decoders (#409).
- Work around an upstream bug in CPython to properly support `Required`/`NotRequired` in `TypedDict` when `__future__.annotations` is enabled (#410).

## [0.15.0] - 2023-05-10

- Add support for Generic `Struct` types (#386, #393).
- Add support for Generic `dataclasses` and `attrs` types (#396).
- Add support for Generic `typing.TypedDict` and `typing.NamedTuple` types (#398).
- **BREAKING**: No longer normalize timezones to UTC when decoding `datetime` objects from JSON (#391).
- Support decoding unhyphenated UUIDs (#392).
- A few type annotation fixups (#383, #387).
- Dedent docstrings for descriptions when generating JSON schemas (#397).
- Use a variant of `__qualname__` when auto-generating Struct tags rather than `__name__` (#399).
- Fix bug when handling `typing.Literal` types containing a literal `None` (#400).
- Make all `Encoder`/`Decoder` methods threadsafe (#402).
- **BREAKING**: Drop the `write_buffer_size` kwarg to `Encoder` (#402).

## [0.14.2] - 2023-04-20

- Remove `__del__` trashcan usage for structs with `gc=False` (#369).
- Support overriding `__setattr__` in Struct types (#376).
- Support encoding large integers in JSON (#378).
- Fix a memory leak when msgpack decoding variable length tuples with more than 16 elements (#380).
- Remove JSON compatibility checks when constructing a `msgspec.json.Decoder`. Trying to decode messages into types that JSON doesn't support will now error at decode time, rather than when the decoder is constructed (#381).

## [0.14.1] - 2023-04-07

- Further optimize decoding of JSON arrays into lists #363
- Fix a bug preventing using structs configured with ``dict=True`` on Python 3.11 #365
- Avoid preallocating large lists/tuples in the msgpack decoder #367

## [0.14.0] - 2023-04-02

- Support encoding and decoding [attrs](https://attrs.org) types (#323).
- Add ``repr_omit_defaults`` configuration option for omitting struct default values in the ``repr`` (#322).
- Expose a struct's configuration through a ``__struct_config__`` attribute (#328).
- Add `msgspec.structs.fields` utility function for inspecting the fields configured on a Struct (#330).
- Add a ``dict`` configuration option for adding a ``__dict__`` attribute to a Struct (#331).
- Allow non-struct mixins to be used with struct types (#332).
- Fix a bug when defining both ``lt`` and ``gt`` constraints on an integer type (#335).
- Fix a bug supporting fields defined with `msgspec.field()` with no arguments (#343).
- Allow arbitrary input types to `msgspec.from_builtins` (#346).
- Support decoding into subclasses of ``int`` & ``bytes`` in `msgspec.from_builtins` (#346).
- Add `msgspec.UNSET` and `msgspec.UnsetType` for tracking unset fields. See the [docs](https://jcristharif.com/msgspec/supported-types.html#unset) for more information (#350).
- **BREAKING**: In the unlikely event you were using the previous ``msgspec.UNSET`` singleton to explicitly indicate no default value on struct types, you should now make use of `msgspec.NODEFAULT` instead (#350).
- Improve struct type annotations now that ``mypy`` supports `typing.dataclass_transform` (#352).
- Support `typing.Final` annotations for indicating that an object field should be treated as immutable (#354).
- Add a ``name`` keyword option to `msgspec.field` for renaming a single field (#356).
- **BREAKING**: The rules around class inheritance and a struct's ``rename`` option have changed. See #356 for more information.

## [0.13.1] - 2023-02-10

- Fix a memory leak in the JSON decoder introduced in 0.13.0, caused by a reference counting bug when decoding into ``Struct`` types (#312).

## [0.13.0] - 2023-02-09

- Add `to_builtins` function for converting messages composed of any supported type to ones composed of only simple builtin types commonly supported by Python serialization libraries (#258).
- Add `from_builtins` function for converting and validating messages composed of simple builtin types to ones composed of any type msgspec supports (#266, #302).
- Add `msgspec.yaml` module for encoding/decoding YAML (#267).
- Add `msgspec.toml` module for encoding/decoding TOML (#268).
- Add `msgspec.structs.replace` function for creating a copy of an existing `Struct` with some changes applied (#262).
- Add `msgspec.structs.asdict` and `msgspec.structs.astuple` functions for converting a struct instance to a `dict` or `tuple` respectively (#300).
- Support arbitrarily nested `typing.NewType`/`typing.Annotated` types (#272).
- Improve error message for invalid keyword arguments passed to `Struct.__init__` (#273).
- Support `default_factory` configuration for `Struct` fields (#274).
- **BREAKING**: With the exception of empty builtin collections (`[]`, `{}`, `set()`, `bytearray()`), mutable default values in Struct types are no longer deepcopied when used. If a different mutable default value is needed, please configure a `default_factory` instead (#274).
- Improve performance of creating Structs with default parameters (#274).
- Support `typing.ClassVar` annotations of `Struct` types (#281).
- Support encoding/decoding `decimal.Decimal` types (#288).
- Support "abstract" type annotations like `collections.abc.MutableMapping`/`typing.MutableMapping` in decoders (#290).
- Support any string-like or int-like type as a `dict` key when encoding or decoding JSON (#292).
- Improved performance encoding large collections in JSON and MessagePack encoders (#294, #298).

## [0.12.0] - 2023-01-05

- Support encoding `set` and `frozenset` subclasses (#249).
- Support encoding/decoding `typing.NewType` types (#251).
- Allow creating a `msgspec.Raw` object from a `str` (#252).
- Add new experimental `msgspec.inspect` module for inspecting type annotations. This is intended to be used for building downstream tooling based on msgspec-compatible types. See [the docs](https://jcristharif.com/msgspec/inspect.html) for more information (#253).
- Add new `extra` field to `msgspec.Meta`, for storing arbitrary user-defined metadata (#255).
- Improved performance for JSON encoding strings (#256).

## [0.11.0] - 2022-12-19

- Improve performance of constructors for `Struct` types when using keyword arguments (#237).
- Support [constraints](https://jcristharif.com/msgspec/constraints.html) on dict keys for JSON (#239).
- Add support for keyword-only arguments in `Struct` types, matching the behavior of `kw_only` for `dataclasses` (#242).
- **BREAKING**: Change the parameter ordering rules used by `Struct` types to match the behavior of `dataclasses`. For most users this change shouldn't break anything. However, if your struct definitions have required fields after optional fields, you'll now get an error on import. This error can be fixed by either:

  - Reordering your fields so all required fields are before all optional fields
  - Using keyword-only parameters (by passing the ``kw_only=True`` option).

  See [Field Ordering](https://jcristharif.com/msgspec/structs.html#field-ordering) for more information (#242).

- Support encoding/decoding dictionaries with integer keys for JSON (#243).

## [0.10.1] - 2022-12-08

- Ignore attributes with leading underscores (`"_"`) when encoding `dataclasses` (#234)

## [0.10.0] - 2022-12-08

- Add ``forbid_unknown_fields`` configuration option to `Struct` types (#210)
- **BREAKING**: Encode all `enum` types by value, rather than name (#211)
- Fix a bug in the JSON encoder when base64 encoding binary objects (#217)
- Add support for encoding/decoding `dataclasses` (#218)
- Add support for encoding/decoding `datetime.date` objects (#221)
- Add support for encoding/decoding `uuid.UUID` objects (#222)
- **BREAKING**: support encoding/decoding `datetime.datetime` values without timezones by default (#224).
- Add a ``tz`` [constraint](https://jcristharif.com/msgspec/constraints.html#datetime-constraints) to require aware or naive datetime/time objects when decoding (#224).
- Add support for encoding/decoding `datetime.time` objects (#225)
- Add a `msgspec.json.format` utility for efficiently pretty-printing already encoded JSON documents (#226).
- Support decoding JSON from strings instead of just bytes-like objects (#229)

## [0.9.1] - 2022-10-28

- Enable Python 3.11 builds (#205)
- Support greater than microsecond resolution when parsing JSON timestamps (#201)
- Work around a limitation in mypy for typed decoders (#191)

## [0.9.0] - 2022-09-14

- Support for [constraints](https://jcristharif.com/msgspec/constraints.html) during validation. For example, this allows ensuring a field is an integer >= 0. (#176)
- New utilities for generating [JSON Schema](https://jcristharif.com/msgspec/jsonschema.html) from type definitions (#181)
- Support for pretty printing using [rich](https://rich.readthedocs.io/en/stable/pretty.html) (#183)
- Improve integer encoding performance (#170)
- Builtin support for renaming fields using kebab-case (#175)
- Support for passing a mapping when renaming fields (#185)

## [0.8.0] - 2022-08-02

- Support integer tag values when using [tagged unions](https://jcristharif.com/msgspec/structs.html#tagged-unions) (#135).
- Support decoding into `typing.TypedDict` types (#142).
- Support encoding/decoding `typing.NamedTuple` types (#161).
- Test against CPython 3.11 prelease builds (#146).
- Add `ValidationError` (a subclass of `DecodeError`) to allow differentiating between errors due to a message not matching the schema from those due to the message being invalid JSON (#155).
- Support encoding subclasses of `list`/`dict` (#160).
- Fix a bug preventing decoding custom types wrapped in a `typing.Optional` (#162).

## [0.7.1] - 2022-06-28

- Further reduce the size of packaged wheels (#130).
- Add `weakref` support for `Struct` types through a new ``weakref`` configuration option (#131).
- Fix a couple unlikely (but possible) bugs in the deallocation routine for Struct types (#131).

## [0.7.0] - 2022-06-20

- Dramatically speedup JSON string decoding, up to 2x speedup in some cases (#118).
- Adds a cache for decoding short (< 32 character) ASCII dict keys. This results in up to a 40% speedup when decoding many dicts with common keys using an untyped decoder. It's still recommended to define `Struct` types when your messages have a common structure, but in cases where no type is provided decoding is now much more performant (#120, #121).
- Adds ``order`` and ``eq`` configuration options for `Struct` types, mirroring the ``dataclasses`` options of the same name. Order comparisons for Struct types are very performant, roughly [10x to 70x faster](https://jcristharif.com/msgspec/benchmarks.html#benchmark-structs) than alternative libraries (#122).
- Speedup `Struct` decoding for both JSON and MessagePack, on average 20% faster (#119).
- Various additional performance improvements, mostly to the JSON implementation (#100, #101, #102).
- Add `defstruct` method for dynamically defining new `Struct` types at runtime (#105).
- Fix ARM support and publish ARM wheels for Linux and Mac (#104).
- Reduce published wheel sizes by stripping debug symbols (#113).
- Fix a memory leak in ``Struct.__reduce__`` (#117).
- Rename ``nogc`` struct option to ``gc`` (a breaking change). To disable GC on a Struct instance you now want to specify ``gc=False`` instead of ``nogc=True`` (#124).

## [0.6.0] - 2022-04-06

- Add a new [`msgspec.Raw`](https://jcristharif.com/msgspec/usage.html#raw) type for delayed decoding of message fields / serializing already encoded fields.
- Add `omit_defaults` option to `Struct` types ([docs](https://jcristharif.com/msgspec/structs.html#omitting-default-values)). If enabled, fields containing their respective default value will be omitted from serialized message. This improves both encode and decode performance.
- Add `rename` option to `Struct` types ([docs](https://jcristharif.com/msgspec/structs.html#renaming-field-names)) for altering the field names used for encoding. A major use of this is supporting `camelCase` JSON field names, while letting Python code use the more standard `snake_case` field names.
- Improve performance of [`nogc=True` structs](https://jcristharif.com/msgspec/structs.html#disabling-garbage-collection-advanced). GC is now avoided in more cases, and `nogc=True` structs use 16 fewer bytes per instance. Also added a [benchmark](https://jcristharif.com/msgspec/benchmarks.html#benchmark-garbage-collection) for how `msgspec` can interact with application GC usage.
- Cache creation of [tagged union](https://jcristharif.com/msgspec/structs.html#tagged-unions) lookup tables, reducing memory usage for applications making heavy use of tagged unions.
- Support encoding and decoding `frozenset` instances
- A smattering of other performance improvements.

## [0.5.0] - 2022-03-09

- Support [tagged unions](https://jcristharif.com/msgspec/structs.html#tagged-unions) for encoding/decoding a `Union` of `msgspec.Struct` types.
- Further improve encoding performance of `enum.Enum` by 20-30%
- Reduce overhead of calling `msgspec.json.decode`/`msgspec.msgpack.decode` with `type=SomeStructType`. It's still faster to create a `Decoder` once and call `decoder.decode` multiple times, but for struct types the overhead of calling the top-level function is decreased significantly.
- Rename the Struct option `asarray` to `array_like` (a breaking change)

## [0.4.2] - 2022-02-28

- Support `Literal` string types as dict keys in JSON
- Support Python 3.10 style unions (e.g. `int | float | None`)
- Publish Python 3.10 wheels

## [0.4.1] - 2022-02-23

- Optimize decoding of `Enum` types, ~10x faster
- Optimize decoding of `IntEnum` types, ~12 faster
- Support decoding `typing.Literal` types
- Add `nogc` option for `Struct` types, disabling the cyclic garbage collector for their instances

## [0.4.0] - 2022-02-08

This is a major release with several large changes:

- Moved MessagePack support to the `msgspec.msgpack` submodule
- New JSON support available in `msgspec.json`
- Improved error message generation to provide full path to the mistyped values
- Renamed the `immutable` kwarg in `msgspec.Struct` to `frozen` to better match python conventions
- Renamed `EncodingError` to `EncodeError`/`DecodingError` to `DecodeError` to better match python conventions
- Improved `pyright` support, allowing more errors to be statically caught by their type checker
- Adds support for Python 3.10 pattern matching on `msgspec.Struct` types
- Adds support for decoding into `typing.Union` types (with a few restrictions)
- General performance improvements across all encoders/decoders

## [0.3.2] - 2021-07-23

- Faster float encoding and decoding
- General micro-optimizations for write/read hot path. Most visible for large messages.

## [0.3.1] - 2021-07-13

- Use a freelist for small structs to improve struct allocation time.
- Small perf improvement for struct serialization

## [0.3.0] - 2021-07-07

- Add `Encoder.encode_into` api, for encoding into an existing buffer without copying
- Add support for encoding/decoding MessagePack extensions
- Add support for encoding/decoding `datetime` objects
- Add support for encoding/decoding custom objects without relying on MessagePack extensions
- Add support for marking `Struct` types as hashable
- Add support for serializing `Struct` types as MessagePack `array` objects rather than `map` objects.
- Several performance improvements. On average 50% faster encoding and 30% faster decoding.
