Changelog
=========

.. currentmodule:: msgspec

Version 0.10.1 (2022-12-08)
---------------------------

- Ignore attributes with leading underscores (``"_"``) when encoding
  `dataclasses` (:pr:`234`)

Version 0.10.0 (2022-12-07)
---------------------------

- Add ``forbid_unknown_fields`` configuration option to `Struct` types (:pr:`210`)
- **BREAKING**: Encode all `enum` types by value, rather than name (:pr:`211`)
- Fix a bug in the JSON encoder when base64 encoding binary objects (:pr:`217`)
- Add support for encoding/decoding `dataclasses` (:pr:`218`)
- Add support for encoding/decoding `datetime.date` objects (:pr:`221`)
- Add support for encoding/decoding `uuid.UUID` objects (:pr:`222`)
- **BREAKING**: support encoding/decoding `datetime.datetime` values without
  timezones by default (:pr:`224`).
- Add a ``tz`` :doc:`constraint <constraints>` to require aware or naive
  datetime/time objects when decoding (:pr:`224`).
- Add support for encoding/decoding `datetime.time` objects (:pr:`225`)
- Add a `msgspec.json.format` utility for efficiently pretty-printing already
  encoded JSON documents (:pr:`226`).
- Support decoding JSON from strings instead of just bytes-like objects
  (:pr:`229`)

Version 0.9.1 (2022-10-27)
--------------------------

- Enable Python 3.11 builds (:pr:`205`)
- Support greater than microsecond resolution when parsing JSON timestamps (:pr:`201`)
- Work around a limitation in mypy for typed decoders (:pr:`191`)

Version 0.9.0 (2022-09-13)
--------------------------

- Support for :doc:`constraints <constraints>` during validation. For example,
  this allows ensuring a field is an integer >= 0. (:pr:`176`)
- New utilities for generating :doc:`JSON Schemas <jsonschema>` from type
  definitions (:pr:`181`)
- Support for pretty printing using `rich
  <https://rich.readthedocs.io/en/stable/pretty.html>`_ (:pr:`183`)
- Improve integer encoding performance (:pr:`170`)
- Builtin support for renaming fields using kebab-case (:pr:`175`)
- Support for passing a mapping when renaming fields (:pr:`185`)

Version 0.8.0 (2022-08-01)
--------------------------

- Support integer tag values when using :ref:`tagged unions
  <struct-tagged-unions>` (:pr:`135`).
- Support decoding into `typing.TypedDict` types (:pr:`142`).
- Support encoding/decoding `typing.NamedTuple` types (:pr:`161`).
- Test against CPython 3.11 prelease builds (:pr:`146`).
- Add `ValidationError` (a subclass of `DecodeError`) to allow differentiating
  between errors due to a message not matching the schema from those due to the
  message being invalid JSON (:pr:`155`).
- Support encoding subclasses of `list`/`dict` (:pr:`160`).
- Fix a bug preventing decoding custom types wrapped in a `typing.Optional`
  (:pr:`162`).

Version 0.7.1 (2022-06-27)
--------------------------

- Further reduce the size of packaged wheels (:pr:`130`).
- Add `weakref` support for `Struct` types through a new ``weakref``
  configuration option (:pr:`131`).
- Fix a couple unlikely (but possible) bugs in the deallocation routine for
  Struct types (:pr:`131`).

Version 0.7.0 (2022-06-20)
--------------------------

- Dramatically speedup JSON string decoding, up to 2x speedup in some cases
  (:pr:`118`).
- Adds a cache for decoding short (< 32 character) ASCII dict keys. This
  results in up to a 40% speedup when decoding many dicts with common keys
  using an untyped decoder. It's still recommended to define `Struct` types
  when your messages have a common structure, but in cases where no type is
  provided decoding is now much more performant (:pr:`120`, :pr:`121`).
- Adds ``order`` and ``eq`` configuration options for `Struct` types, mirroring
  the ``dataclasses`` options of the same name. Order comparisons for Struct
  types are very performant, roughly `10x to 70x faster
  <https://jcristharif.com/msgspec/benchmarks.html#benchmark-structs>`__ than
  alternative libraries (:pr:`122`).
- Speedup `Struct` decoding for both JSON and MessagePack, on average 20%
  faster (:pr:`119`).
- Various additional performance improvements, mostly to the JSON
  implementation (:pr:`100`, :pr:`101`, :pr:`102`).
- Add `defstruct` method for dynamically defining new `Struct` types at
  runtime (:pr:`105`).
- Fix ARM support and publish ARM wheels for Linux and Mac (:pr:`104`).
- Reduce published wheel sizes by stripping debug symbols (:pr:`113`).
- Fix a memory leak in ``Struct.__reduce__`` (:pr:`117`).
- **BREAKING**: Rename ``nogc`` struct option to ``gc``. To disable GC on a
  Struct instance you now want to specify ``gc=False`` instead of ``nogc=True``
  (:pr:`124`).


Version 0.6.0 (2022-04-06)
--------------------------

- Add a new `msgspec.Raw <https://jcristharif.com/msgspec/usage.html#raw>`__
  type for delayed decoding of message fields / serializing already encoded
  fields (:pr:`92`).
- Add ``omit_defaults`` option to ``Struct`` types (`docs
  <https://jcristharif.com/msgspec/structs.html#omitting-default-values>`__).
  If enabled, fields containing their respective default value will be omitted
  from serialized message. This improves both encode and decode performance
  (:pr:`94`).
- Add ``rename`` option to ``Struct`` types (`docs
  <https://jcristharif.com/msgspec/structs.html#renaming-field-names>`__) for
  altering the field names used for encoding. A major use of this is supporting
  ``camelCase`` JSON field names, while letting Python code use the more
  standard ``snake_case`` field names (:pr:`98`).
- Improve performance of ``nogc=True`` structs (`docs
  <https://jcristharif.com/msgspec/structs.html#disabling-garbage-collection-advanced>`__).
  GC is now avoided in more cases, and ``nogc=True`` structs use 16 fewer bytes
  per instance. Also added a `benchmark
  <https://jcristharif.com/msgspec/benchmarks.html#benchmark-garbage-collection>`__
  for how ``msgspec`` can interact with application GC usage (:pr:`93`).
- Cache creation of `tagged union
  <https://jcristharif.com/msgspec/structs.html#tagged-unions>`__ lookup
  tables, reducing memory usage for applications making heavy use of tagged
  unions (:pr:`91`).
- Support encoding and decoding ``frozenset`` instances (:pr:`95`).
- A smattering of other performance improvements.


Version 0.5.0 (2022-03-09)
--------------------------

- Support `tagged unions
  <https://jcristharif.com/msgspec/structs.html#tagged-unions>`__ for
  encoding/decoding a ``Union`` of ``msgspec.Struct`` types (:pr:`83`).
- Further improve encoding performance of ``enum.Enum`` instances by 20-30%
  (:pr:`84`).
- Reduce overhead of calling ``msgspec.json.decode``/``msgspec.msgpack.decode``
  with ``type=SomeStructType``. It's still faster to create a ``Decoder`` once
  and call ``decoder.decode`` multiple times, but for struct types the overhead
  of calling the top-level function is decreased significantly (:pr:`77`,
  :pr:`88`).
- **BREAKING**: Rename the Struct option ``asarray`` to ``array_like`` (:pr:`85`).


Version 0.4.2 (2022-02-28)
--------------------------

- Support ``typing.Literal`` string types as dict keys in JSON (:pr:`78`).
- Support Python 3.10 style unions (for example, ``int | float | None``)
  (:pr:`75`).
- Publish Python 3.10 wheels (:pr:`80`).


Version 0.4.1 (2022-02-23)
--------------------------

- Optimize decoding of ``Enum`` types, on average ~10x faster (:pr:`69`).
- Optimize decoding of ``IntEnum`` types, on average ~12x faster (:pr:`68`).
- Support decoding ``typing.Literal`` types (:pr:`71`).
- Add ``nogc`` option for ``Struct`` types, disabling the cyclic garbage
  collector for their instances (:pr:`72`).


Version 0.4.0 (2022-02-08)
--------------------------

- Moved MessagePack support to the ``msgspec.msgpack`` submodule (:pr:`56`).
- New JSON support available in ``msgspec.json`` (:pr:`56`).
- Improved error message generation to provide full path to the mistyped values
  (:pr:`56`).
- Renamed the ``immutable`` kwarg in ``msgspec.Struct`` to ``frozen`` to better
  match python conventions (:pr:`60`).
- Renamed ``EncodingError`` to ``EncodeError``/``DecodingError`` to
  ``DecodeError`` to better match python conventions (:pr:`61`).
- Improved ``pyright`` support, allowing more errors to be statically caught by
  their type checker (:pr:`60`).
- Adds support for Python 3.10 pattern matching on ``msgspec.Struct`` types
  (:pr:`53`).
- Adds support for decoding into ``typing.Union`` types (with a few
  restrictions) (:pr:`54`).
- General performance improvements across all encoders/decoders.


Version 0.3.2 (2021-07-23)
--------------------------

- Faster float encoding and decoding (:pr:`47`).
- General micro-optimizations for MessagePack encode/decode. This is most
  visible for large messages (:pr:`48`, :pr:`50`).


Version 0.3.1 (2021-07-12)
--------------------------

- Use a freelist for small structs to improve struct allocation time
  (:pr:`44`).
- Small performance improvements for struct serialization (:pr:`45`).


Version 0.3.0 (2021-07-07)
--------------------------

- Add ``Encoder.encode_into`` api, for encoding into an existing buffer without
  copying (:pr:`34`).
- Add support for encoding/decoding MessagePack extensions (:pr:`31`).
- Add support for encoding/decoding ``datetime`` objects (:pr:`36`).
- Add support for encoding/decoding custom objects without relying on
  MessagePack extensions (:pr:`32`, :pr:`33`).
- Add support for marking ``Struct`` types as hashable (:pr:`39`).
- Add support for serializing ``Struct`` types as MessagePack ``array`` objects
  rather than ``map`` objects (:pr:`39`).
- Several performance improvements. On average 50% faster encoding and 30%
  faster decoding.


Version 0.2.0 (2021-02-25)
--------------------------

- Add ``default`` callback to ``encode``/``Encoder`` (:pr:`21`).
- Fix bug in ``Encoder`` dealloc (:pr:`21`).


Version 0.1.0 (2021-02-23)
--------------------------

- Initial Release
