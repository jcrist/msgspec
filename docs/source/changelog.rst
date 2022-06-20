Changelog
=========

Version 0.6.0 (2022-04-06)
--------------------------

- Add a new `msgspec.Raw <https://jcristharif.com/msgspec/usage.html#raw>`__
  type for delayed decoding of message fields / serializing already encoded
  fields.
- Add ``omit_defaults`` option to ``Struct`` types (`docs
  <https://jcristharif.com/msgspec/structs.html#omitting-default-values>`__).
  If enabled, fields containing their respective default value will be omitted
  from serialized message. This improves both encode and decode performance.
- Add ``rename`` option to ``Struct`` types (`docs
  <https://jcristharif.com/msgspec/structs.html#renaming-field-names>`__) for
  altering the field names used for encoding. A major use of this is supporting
  ``camelCase`` JSON field names, while letting Python code use the more
  standard ``snake_case`` field names.
- Improve performance of ``nogc=True`` structs (`docs
  <https://jcristharif.com/msgspec/structs.html#disabling-garbage-collection-advanced>`__).
  GC is now avoided in more cases, and ``nogc=True`` structs use 16 fewer bytes
  per instance. Also added a `benchmark
  <https://jcristharif.com/msgspec/benchmarks.html#benchmark-garbage-collection>`__
  for how ``msgspec`` can interact with application GC usage.
- Cache creation of `tagged union
  <https://jcristharif.com/msgspec/structs.html#tagged-unions>`__ lookup
  tables, reducing memory usage for applications making heavy use of tagged
  unions.
- Support encoding and decoding ``frozenset`` instances.
- A smattering of other performance improvements.


Version 0.5.0 (2022-03-09)
--------------------------

- Support `tagged unions
  <https://jcristharif.com/msgspec/structs.html#tagged-unions>`__ for
  encoding/decoding a ``Union`` of ``msgspec.Struct`` types.
- Further improve encoding performance of ``enum.Enum`` instances by 20-30%.
- Reduce overhead of calling ``msgspec.json.decode``/``msgspec.msgpack.decode``
  with ``type=SomeStructType``. It's still faster to create a ``Decoder`` once
  and call ``decoder.decode`` multiple times, but for struct types the overhead
  of calling the top-level function is decreased significantly.
- Rename the Struct option ``asarray`` to ``array_like`` (a breaking change).


Version 0.4.2 (2022-02-28)
--------------------------

- Support ``typing.Literal`` string types as dict keys in JSON.
- Support Python 3.10 style unions (for example, ``int | float | None``).
- Publish Python 3.10 wheels.


Version 0.4.1 (2022-02-23)
--------------------------

- Optimize decoding of ``Enum`` types, on average ~10x faster.
- Optimize decoding of ``IntEnum`` types, on average ~12x faster.
- Support decoding ``typing.Literal`` types.
- Add ``nogc`` option for ``Struct`` types, disabling the cyclic garbage
  collector for their instances.


Version 0.4.0 (2022-02-08)
--------------------------

- Moved MessagePack support to the ``msgspec.msgpack`` submodule.
- New JSON support available in ``msgspec.json``.
- Improved error message generation to provide full path to the mistyped values.
- Renamed the ``immutable`` kwarg in ``msgspec.Struct`` to ``frozen`` to better
  match python conventions.
- Renamed ``EncodingError`` to ``EncodeError``/``DecodingError`` to
  ``DecodeError`` to better match python conventions.
- Improved ``pyright`` support, allowing more errors to be statically caught by
  their type checker.
- Adds support for Python 3.10 pattern matching on ``msgspec.Struct`` types.
- Adds support for decoding into ``typing.Union`` types (with a few
  restrictions).
- General performance improvements across all encoders/decoders.


Version 0.3.2 (2021-07-23)
--------------------------

- Faster float encoding and decoding.
- General micro-optimizations for MessagePack encode/decode. This is most
  visible for large messages.


Version 0.3.1 (2021-07-12)
--------------------------

- Use a freelist for small structs to improve struct allocation time.
- Small performance improvements for struct serialization.


Version 0.3.0 (2021-07-07)
--------------------------

- Add ``Encoder.encode_into`` api, for encoding into an existing buffer without
  copying.
- Add support for encoding/decoding MessagePack extensions.
- Add support for encoding/decoding ``datetime`` objects.
- Add support for encoding/decoding custom objects without relying on
  MessagePack extensions.
- Add support for marking ``Struct`` types as hashable.
- Add support for serializing ``Struct`` types as MessagePack ``array`` objects
  rather than ``map`` objects.
- Several performance improvements. On average 50% faster encoding and 30%
  faster decoding.


Version 0.2.0 (2021-02-25)
--------------------------

- Add ``default`` callback to ``encode``/``Encoder``
- Fix bug in ``Encoder`` dealloc


Version 0.1.0 (2021-02-23)
--------------------------

- Initial Release
