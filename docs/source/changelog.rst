Changelog
=========

.. currentmodule:: msgspec

Version 0.20.0 (2025-01-03)
---------------------------

**🎉 MAJOR: Community Fork Release**

This is the first release of ``msgspec-x``, a community-driven fork of the original msgspec library by Jim Crist-Harif. This fork was created to accelerate community contributions and provide a platform for extended features while maintaining full backward compatibility.

**🚀 NEW MAJOR FEATURE: StructMeta Subclasses Support**

- **Add comprehensive support for StructMeta subclasses** - the primary feature that motivated this fork.
- Enable custom metaclasses that inherit from `StructMeta` to work seamlessly with all msgspec functions.
- **TECHNICAL**: Modified C code to use `PyType_IsSubtype()` instead of direct type comparison for StructMeta detection.
- Affected functions now support StructMeta subclasses:

  - `msgspec.structs.asdict` - Convert struct instances to dictionaries
  - `msgspec.structs.astuple` - Convert struct instances to tuples  
  - `msgspec.structs.replace` - Create modified copies of struct instances
  - `msgspec.structs.force_setattr` - Force attribute setting on frozen structs
  - JSON encoding/decoding operations
  - MessagePack encoding/decoding operations
  - `msgspec.convert` - Type conversion operations
  - `msgspec.to_builtins` - Convert to builtin types

- Comprehensive test coverage for StructMeta subclasses including:

  - Single-level StructMeta inheritance
  - Multi-level StructMeta inheritance chains
  - Integration with all struct utility functions
  - Encoder/decoder compatibility testing
  - Nested struct support with custom metaclasses

- **Use Cases**: This enables advanced users to create custom struct behaviors through metaclass programming while maintaining full compatibility with msgspec's serialization ecosystem.

**Project Rename and Fork**

- **BREAKING**: Project renamed from ``msgspec`` to ``msgspec-x``. Do not install both packages simultaneously.
- Fork created due to slow upstream maintenance and to enable faster community contribution cycles.
- All project metadata, URLs, and documentation updated to reflect the new ``msgspec-x`` identity.
- Repository moved to ``https://github.com/nightsailer/msgspec-x``.
- Maintainer changed to Night Sailer (nightsailer@gmail.com).

**Dual Namespace Architecture**

- Introduce dual namespace architecture to support both compatibility and extensions:

  - ``msgspec`` namespace: 100% API compatibility with the original library for drop-in replacement.
  - ``msgspec_x`` namespace: Extended features and community contributions (placeholder structure created).

- All existing code using ``import msgspec`` will continue to work without changes.
- New extended features will be available under ``msgspec_x`` namespace.

**Installation and Distribution**

- Package name changed to ``msgspec-x`` on PyPI.
- Updated installation commands: ``pip install msgspec-x`` and ``conda install msgspec-x -c conda-forge``.
- Added clear warnings about not installing both ``msgspec`` and ``msgspec-x`` in the same environment.
- Updated versioneer configuration to use ``msgspec-x-`` prefix for source distributions.

**Documentation Overhaul**

- Comprehensive documentation update to reflect the project fork and new architecture.
- Added explanation of the dual namespace system and community-driven development model.
- Updated all GitHub links, issue tracker URLs, and example source references.
- Enhanced installation documentation with compatibility warnings.
- Updated contributing guidelines and security policies for the new project structure.

**Community and Development**

- Established faster review and merge cycles for community contributions.
- Updated GitHub issue templates and workflows for the new repository.
- Created placeholder structure for experimental features in ``msgspec_x`` namespace.
- Enhanced project documentation to welcome community contributions.

**Technical Infrastructure**

- Updated build configuration (``setup.py``, ``setup.cfg``, ``MANIFEST.in``) for the new package structure.
- Enhanced CI/CD workflows for the dual namespace architecture.
- Updated type stub files and package metadata for both namespaces.
- Maintained all existing performance characteristics and API compatibility.

**Acknowledgments**

This release acknowledges and thanks Jim Crist-Harif for creating the original msgspec library. This fork exists to complement and extend his excellent work, not to replace it.

Version 0.19.0 (2024-12-27)
---------------------------

- Improve JSON encoding performance by up to 40% (:pr:`647`).
- Ensure `tuple` and `frozenset` default values are treated identically whether
  specified by value or ``default_factory`` (:pr:`653`).
- Fix memory leak of ``match_args`` in ``StructConfig`` object (:pr:`684`).
- Fix memory leak in ``Raw.copy()`` (:pr:`709`).
- Update ``decode`` signatures for PEP 688 (:pr:`740`).
- Generate ``__replace__`` method on ``Struct`` types, for use with
  `copy.replace` (:pr:`747`).
- Fix incorrect decoding of certain > 64 bit integers (:pr:`751`).
- Call ``__post_init__`` when converting from an object to a Struct
  (:pr:`752`).
- **BREAKING**: Expand buffer when ``encode_into`` is passed a buffer smaller
  than ``offset`` (:pr:`753`).
- Support `Raw` objects as inputs to `convert` (:pr:`754`).
- Error nicely when a dataclass *type* (instead of an *instance*) is passed to
  ``encode`` (:pr:`755`).
- Drop support for Python 3.8 (:pr:`756`, :pr:`763`).
- Add support for Python 3.13 (:pr:`711`).
- Remove deprecated ``from_builtins`` (:pr:`761`).
- Support encoding any ``Enum`` type whose ``.value`` is a supported type
  (:pr:`757`).
- Don't fail eagerly when processing generic types with unsupported
  ``__parameters__`` (:pr:`772`).
- Use ``eval_type_backport`` to backport type annotations to Python 3.9
  (:pr:`773`).

Version 0.18.6 (2024-01-21)
---------------------------

- Support coercing integral floats to ints when ``strict=False`` (:pr:`619`).
- Preserve leading ``_`` when renaming fields to camel or pascal case (:pr:`620`).
- Support zero-copy decoding binary fields to a ``memoryview`` (:pr:`624`).
- Fix a bug when inheriting from the same ``Generic`` base class multiple times
  (:pr:`626`).
- Add an ``order`` option to all encoders for enforcing deterministic/sorted
  ordering when encoding. This can help provide a more consistent or human
  readable output (:pr:`627`).
- Support inheriting from any slots-class when defining a new ``Struct`` type
  with ``gc=False`` (:pr:`635`).
- Automatically infer the input field naming convention when converting
  non-dict mappings or arbitrary objects to ``Struct`` types in
  ``msgspec.convert`` (:pr:`636`).

Version 0.18.5 (2023-12-12)
---------------------------

- Support unhashable ``Annotated`` metadata in `msgspec.inspect.type_info`
  (:pr:`566`).
- Fix bug preventing decoding dataclasses/attrs types with default values and
  ``slots=True, frozen=True`` (:pr:`569`).
- Support passing parametrized generic struct types to `msgspec.structs.fields`
  (:pr:`571`).
- Validate ``str`` constraints on dict keys when decoding msgpack (:pr:`577`).
- Support ``UUID`` subclasses as inputs to `msgspec.convert` (:pr:`592`).
- Call ``__eq__`` from generated ``__ne__`` if user defines manual ``__eq__``
  method on a ``Struct`` type (:pr:`593`).
- Include the ``Struct`` type in the generated hash (:pr:`595`).
- Add a ``cache_hash`` struct option (:pr:`596`).
- Fix a bug around caching of dataclass type info when dealing with subclasses
  of dataclasses (:pr:`599`).
- Add `msgspec.structs.force_setattr` (:pr:`600`).
- Support custom dict key types in JSON encoder and decoder (:pr:`602`).
- Include ``dict`` key constraints in generated JSON schema via the
  ``propertyNames`` field (:pr:`604`).
- Add a ``schema_hook`` for generating JSON schemas for custom types (:pr:`605`).
- Add support for Python 3.12's ``type`` aliases (:pr:`606`).

Version 0.18.4 (2023-10-04)
---------------------------

- Resolve an issue leading to periodic segfaults when importing ``msgspec`` on
  CPython 3.12 (:pr:`561`)

Version 0.18.3 (2023-10-03)
---------------------------

- Improve type annotation for ``Struct.__rich_repr__`` (:pr:`557`)
- Add pre-built wheels for Python 3.12 (:pr:`558`)

Version 0.18.2 (2023-08-26)
---------------------------

- Support ``Enum._missing_`` hooks for handling unknown enum values (:pr:`532`).
- Fix JSON encoding of `datetime.datetime` objects with `zoneinfo.ZoneInfo`
  timezone components (:pr:`534`).
- Add support for ``attrs`` `validators
  <https://www.attrs.org/en/stable/examples.html#validators>`__ (:pr:`538`).
- Relax datetime/time parsing format to allow some RFC3339 extensions from
  ISO8601 (:pr:`539`).

Version 0.18.1 (2023-08-16)
---------------------------

- Support custom ``builtin_types`` in `msgspec.to_builtins` (:pr:`517`).
- Try ``getattr`` before ``getitem`` when converting with
  ``from_attributes=True`` (:pr:`519`).
- More efficient module state access in top-level functions (:pr:`521`).

Version 0.18.0 (2023-08-10)
---------------------------

- Add a new `msgspec.json.Decoder.decode_lines` method for decoding
  newline-delimited JSON into a list of values (:pr:`485`).
- Support for decoding UUIDs from binary values (:pr:`499`).
- Support for encoding UUIDs in alternate formats (:pr:`499`).
- Overhaul how dataclasses are encoded to support more dataclass-like objects
  (:pr:`501`).
- Encode all declared fields on a dataclass (:pr:`501`).
- Support encoding ``edgedb.Object`` instances as dataclass-like objects
  (:pr:`501`).
- Improve performance when json decoding ``float`` values (:pr:`510`).
- Support for JSON encoding dicts with ``float`` keys (:pr:`510`).
- Support for JSON decoding dicts with ``float`` keys (:pr:`510`).
- Add ``float_hook`` to `msgspec.json.Decoder` to support changing the default
  for how JSON floats are decoded (:pr:`511`).

Version 0.17.0 (2023-07-11)
---------------------------

- Ensure ``None`` may be explicitly passed to `defstruct` for
  ``module``/``namespace``/``bases`` (:pr:`445`).
- Support decoding `datetime.datetime` values from ``int``/``float`` values
  (interpreted as seconds since the Unix epoch) when ``strict=False``
  (:pr:`452`).
- Support subclasses of collection types (``list``, ``dict``, ...) as inputs to
  `convert` (:pr:`453`).
- Support ``str`` subclasses as keys in `to_builtins` and all protocol
  ``encode`` methods (:pr:`454`).
- Improved performance when JSON encoding `decimal.Decimal` values (:pr:`455`).
- Improved performance when JSON encoding ``int``/``float`` values (:pr:`458`).
- Improved performance when JSON encoding ``str`` values (:pr:`459`).
- Wrap errors in ``dec_hook`` with a `ValidationError` (:pr:`460`).
- Support decoding `decimal.Decimal` values from numeric values (:pr:`463`)
- Support encoding `decimal.Decimal` values as numeric values (:pr:`465`).
- Support converting `decimal.Decimal` values to ``float`` in `convert`
  (:pr:`466`).
- Preliminary support for CPython 3.12 beta releases (:pr:`467`).
- Support decoding integers that don't fit into an ``int64``/``uint64``
  (:pr:`469`).
- Add a new optional ``__post_init__`` method for `Struct` types (:pr:`470`).
- Support decoding ``0``/``1`` into ``bool`` types when ``strict=False``
  (:pr:`471`).
- Wrap errors raised in ``__post_init__``/``__attrs_post_init__`` in a
  `ValidationError` when decoding (:pr:`472`).
- Add native support for encoding/decoding `datetime.timedelta` types
  (:pr:`475`).
- Add a new `msgspec.json.Encoder.encode_lines` method for encoding an iterable
  of values as newline-delimited JSON (:pr:`479`).

Version 0.16.0 (2023-06-12)
---------------------------

- Deprecate ``msgspec.from_builtins`` in favor of `msgspec.convert`. The new
  ``convert`` function provides a superset of the functionality available in
  the old ``from_builtins`` function (:pr:`431`).
- Add a ``from_attributes`` argument to `msgspec.convert` for allowing
  conversion between object types with matching attribute names. One use case
  for this is converting ORM objects to `Struct` or `dataclasses` types
  (:pr:`419`).
- Support passing generic ``Mapping`` objects as inputs to `msgspec.convert`.
  These may be coerced to `dict`/`Struct`/`dataclasses`/`attrs` types
  (:pr:`427`).
- Add a new ``strict`` keyword argument to all ``decode`` functions,
  ``Decoder`` classes, as well as `msgspec.convert`. This defaults to ``True``,
  setting it to false enables a wider set of coercion rules (e.g. coercing a
  `str` input to an `int`). See :ref:`strict-vs-lax` for more information
  (:pr:`434`).
- Support all :doc:`supported-types` as inputs to `msgspec.convert` (:pr:`431`,
  :pr:`418`).
- Passthrough input unchanged when coercing to `typing.Any` type in
  `msgspec.convert` (:pr:`435`).
- Support parametrizing ``Decoder`` types at runtime (:pr:`415`).
- Support encoding subclasses of ``UUID`` (:pr:`429`).

Version 0.15.1 (2023-05-19)
---------------------------

- Fix a reference counting bug introduced in 0.15.0 when decoding naive (no
  timezone) ``datetime``/``time`` objects in both the ``msgpack`` and ``json``
  decoders (:pr:`409`).
- Work around an upstream bug in CPython to properly support
  `typing.Required`/`typing.NotRequired` in `typing.TypedDict` when
  ``__future__.annotations`` is enabled (:pr:`410`).

Version 0.15.0 (2023-05-10)
---------------------------

- Add support for Generic `Struct` types (:pr:`386`, :pr:`393`).
- Add support for Generic `dataclasses` and `attrs <https://attrs.org>`__ types
  (:pr:`396`).
- Add support for Generic `typing.TypedDict` and `typing.NamedTuple` types
  (:pr:`398`).
- **BREAKING**: No longer normalize timezones to UTC when decoding `datetime`
  objects from JSON (:pr:`391`).
- Support decoding unhyphenated UUIDs (:pr:`392`).
- A few type annotation fixups (:pr:`383`, :pr:`387`).
- Dedent docstrings for descriptions when generating JSON schemas (:pr:`397`).
- Use a variant of ``__qualname__`` when auto-generating Struct tags rather
  than ``__name__`` (:pr:`399`).
- Fix bug when handling `typing.Literal` types containing a literal ``None``
  (:pr:`400`).
- Make all ``Encoder``/``Decoder`` methods threadsafe (:pr:`402`).
- **BREAKING**: Drop the ``write_buffer_size`` kwarg to ``Encoder`` (:pr:`402`).

Version 0.14.2 (2023-04-19)
---------------------------

- Remove ``__del__`` trashcan usage for structs with ``gc=False`` (:pr:`369`).
- Support overriding ``__setattr__`` in Struct types (:pr:`376`).
- Support encoding large integers in JSON (:pr:`378`).
- Fix a memory leak when msgpack decoding variable length tuples with more than
  16 elements (:pr:`380`).
- Remove JSON compatibility checks when constructing a
  ``msgspec.json.Decoder``. Trying to decode messages into types that JSON
  doesn't support will now error at decode time, rather than when the decoder
  is constructed (:pr:`381`).

Version 0.14.1 (2023-04-07)
---------------------------

- Further optimize decoding of JSON arrays into lists (:pr:`363`).
- Fix a bug preventing using structs configured with ``dict=True`` on Python
  3.11 (:pr:`365`).
- Avoid preallocating large lists/tuples in the msgpack decoder (:pr:`367`).

Version 0.14.0 (2023-04-02)
---------------------------

- Support encoding and decoding `attrs <https://attrs.org>`__ types (:pr:`323`).
- Add ``repr_omit_defaults`` configuration option for omitting struct default
  values in the ``repr`` (:pr:`322`).
- Expose a struct's configuration through a ``__struct_config__`` attribute
  (:pr:`328`).
- Add `msgspec.structs.fields` utility function for inspecting the fields
  configured on a Struct (:pr:`330`).
- Add a ``dict`` configuration option for adding a ``__dict__`` attribute to a
  Struct (:pr:`331`).
- Allow non-struct mixins to be used with struct types (:pr:`332`).
- Fix a bug when defining both ``lt`` and ``gt`` constraints on an integer
  type (:pr:`335`).
- Fix a bug supporting fields defined with `msgspec.field()` with no arguments
  (:pr:`343`).
- Allow arbitrary input types to `msgspec.from_builtins` (:pr:`346`).
- Support decoding into subclasses of ``int`` & ``bytes`` in
  `msgspec.from_builtins` (:pr:`346`).
- Add `msgspec.UNSET` and `msgspec.UnsetType` for tracking unset fields. See the
  :ref:`docs <unset-type>` for more information (:pr:`350`).
- **BREAKING**: In the unlikely event you were using the previous
  ``msgspec.UNSET`` singleton to explicitly indicate no default value on struct
  types, you should now make use of `msgspec.NODEFAULT` instead (:pr:`350`).
- Improve struct type annotations now that ``mypy`` supports
  `typing.dataclass_transform` (:pr:`352`).
- Support `typing.Final` annotations for indicating that an object field should
  be treated as immutable (:pr:`354`).
- Add a ``name`` keyword option to `msgspec.field` for renaming a single field
  (:pr:`356`).
- **BREAKING**: The rules around class inheritance and a struct's ``rename``
  option have changed. See :pr:`356` for more information.

Version 0.13.1 (2023-02-09)
---------------------------

- Fix a memory leak in the JSON decoder introduced in 0.13.0, caused by a
  reference counting bug when decoding into ``Struct`` types (:pr:`312`).

Version 0.13.0 (2023-02-08)
---------------------------

- Add `to_builtins` function for converting messages composed of any supported
  type to ones composed of only simple builtin types commonly supported by
  Python serialization libraries (:pr:`258`).
- Add `from_builtins` function for converting and validating messages composed
  of simple builtin types to ones composed of any type msgspec supports
  (:pr:`266`, :pr:`302`).
- Add `msgspec.yaml` module for encoding/decoding YAML (:pr:`267`).
- Add `msgspec.toml` module for encoding/decoding TOML (:pr:`268`).
- Add `msgspec.structs.replace` function for creating a copy of an existing
  `Struct` with some changes applied (:pr:`262`).
- Add `msgspec.structs.asdict` and `msgspec.structs.astuple` functions for
  converting a struct instance to a `dict` or `tuple` respectively (:pr:`300`).
- Support arbitrarily nested `typing.NewType`/`typing.Annotated` types
  (:pr:`272`).
- Improve error message for invalid keyword arguments passed to
  ``Struct.__init__`` (:pr:`273`).
- Support ``default_factory`` configuration for `Struct` fields (:pr:`274`).
- **BREAKING**: With the exception of empty builtin collections (``[]``,
  ``{}``, ``set()``, ``bytearray()``), mutable default values in Struct types
  are no longer deepcopied when used. If a different mutable default value is
  needed, please configure a ``default_factory`` instead (:pr:`274`).
- Improve performance of creating Structs with default parameters (:pr:`274`).
- Support `typing.ClassVar` annotations of `Struct` types (:pr:`281`).
- Support encoding/decoding `decimal.Decimal` types (:pr:`288`).
- Support "abstract" type annotations like
  `collections.abc.MutableMapping`/`typing.MutableMapping` in decoders
  (:pr:`290`).
- Support any string-like or int-like type as a ``dict`` key when encoding or
  decoding JSON (:pr:`292`).
- Improved performance encoding large collections in JSON and MessagePack
  encoders (:pr:`294`, :pr:`298`).


Version 0.12.0 (2023-01-05)
---------------------------

- Support encoding ``set`` and ``frozenset`` subclasses (:pr:`249`).
- Support encoding/decoding `typing.NewType` types (:pr:`251`).
- Allow creating a `msgspec.Raw` object from a ``str`` (:pr:`252`).
- Add new experimental ``msgspec.inspect`` module for inspecting type
  annotations. This is intended to be used for building downstream tooling
  based on msgspec-compatible types. See :doc:`the docs <inspect>` for more
  information (:pr:`253`).
- Add new ``extra`` field to `msgspec.Meta`, for storing arbitrary user-defined
  metadata (:pr:`255`).
- Improved performance for JSON encoding strings (:pr:`256`).

Version 0.11.0 (2022-12-19)
---------------------------

- Improve performance of constructors for `Struct` types when using keyword
  arguments (:pr:`237`).
- Support :doc:`constraints` on dict keys for JSON (:pr:`239`).
- Add support for keyword-only arguments in `Struct` types, matching the
  behavior of ``kw_only`` for `dataclasses` (:pr:`242`).
- **BREAKING**: Change the parameter ordering rules used by `Struct` types to
  match the behavior of `dataclasses`. For most users this change shouldn't
  break anything. However, if your struct definitions have required fields
  after optional fields, you'll now get an error on import. This error can be
  fixed by either:

  - Reordering your fields so all required fields are before all optional
    fields
  - Using keyword-only parameters (by passing the ``kw_only=True`` option).

  See :ref:`struct-field-ordering` for more information (:pr:`242`).
- Support encoding/decoding dictionaries with integer keys for JSON (:pr:`243`).

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
