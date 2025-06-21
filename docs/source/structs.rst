Structs
=======

Structs are the preferred way of defining structured data types in ``msgspec``.
They're written in C and are quite speedy and lightweight (:doc:`measurably
faster <benchmarks>` to create/compare/encode/decode than similar options like
dataclasses_, attrs_, or pydantic_). They're great for representing structured
data both for serialization and for use in an application.

Structs are defined by subclassing from `msgspec.Struct` and annotating the
types of individual fields. Default values can also be provided for any
optional arguments. Here we define a struct representing a user, with one
required field and two optional fields.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Set, Optional

    >>> class User(msgspec.Struct):
    ...     """A struct describing a user"""
    ...     name : str
    ...     email : Optional[str] = None
    ...     groups : Set[str] = set()

- ``name`` is a *required* field expecting a `str`

- ``email`` is an *optional* field expecting a `str` or `None`, defaulting to
  `None` if no value is provided.

- ``groups`` is an *optional* field expecting a `set` of `str`. If no value is
  provided, it defaults to the empty set.

Struct types automatically generate a few methods based on the provided type
annotations:

- ``__init__``
- ``__repr__``
- ``__copy__``
- ``__replace__``
- ``__eq__`` & ``__ne__``
- ``__match_args__`` (for Python 3.10+'s `pattern matching`_)
- ``__rich_repr__`` (for pretty printing support with rich_)

.. code-block:: python

    >>> alice = User("alice", groups={"admin", "engineering"})

    >>> alice
    User(name='alice', email=None, groups={'admin', 'engineering'})

    >>> bob = User("bob", email="bob@company.com")

    >>> bob
    User(name='bob', email='bob@company.com', groups=set())

    >>> alice.name
    "alice"

    >>> bob.groups
    set()

    >>> alice == bob
    False

    >>> alice == User("alice", groups={"admin", "engineering"})
    True

Note that it is forbidden to override ``__init__``/``__new__`` in a struct
definition, but other methods can be overridden or added as needed. If you need
to customize the generated ``__init__``, see :ref:`struct-post-init`.

The struct fields are available via the ``__struct_fields__`` attribute (a
tuple of the fields in argument order ) if you need them. Here we add a method
for converting a struct to a dict.

.. code-block:: python

    >>> class Point(msgspec.Struct):
    ...     """A point in 2D space"""
    ...     x : float
    ...     y : float
    ...
    ...     def to_dict(self):
    ...         return {f: getattr(self, f) for f in self.__struct_fields__}
    ...

    >>> p = Point(1.0, 2.0)

    >>> p.to_dict()
    {"x": 1.0, "y": 2.0}


Default Values
--------------

Struct fields may be given default values, which are used if no value is
provided to ``__init__``, or when decoding a message. Default values are
configured as part of a Struct definition by assigning them after a field's
type annotation.

.. code-block:: python

    >>> from msgspec import Struct, field

    >>> import uuid

    >>> class Example(Struct):
    ...     a: int = 1
    ...     b: uuid.UUID = field(default_factory=uuid.uuid4)
    ...     c: list[int] = []

    >>> Example()
    Example(a=1, b=UUID('f63219d5-e9ca-4ae8-afd0-cba30e84222d'), c=[])

    >>> Example(a=2)
    Example(a=2, b=UUID('319a6c0f-2841-4439-8bc8-2c1daf7d77a2'), c=[])

    >>> Example().c is Example().c  # new list instance used each time
    False

Default values may be one of 3 kinds:

- A "static" default value. Here the same default value is used for all
  instances. These are specified by assigning the default value itself as part
  of the field definition (as in ``a`` above). Most default values will be of
  this variety.

- A "dynamic" default value. Here a new default value is used for every
  instance. These are specified by passing a 0-argument callable to the
  ``default_factory`` argument of `msgspec.field` (as in ``b`` above). This
  function will be called as needed to create a new default value per instance.
  These are mainly useful for occasions where you need dynamic defaults, or
  when a default value is a mutable object that you don't want to share between
  all instances of the struct (a `common gotcha
  <https://docs.python-guide.org/writing/gotchas/#mutable-default-arguments>`_
  in Python). Note that since the ``default_factory`` callables take no
  arguments, you might need to make use of a lambda_ or `functools.partial` to
  forward any additional parameters needed to the default factory.

- Builtin *empty* mutable collections (``[]``, ``{}``, ``set()``, and
  ``bytearray()``) may be used as default values (as in ``c`` above). Since
  defaults of these types are so common, these are "syntactic sugar" for
  specifying the corresponding ``default_factory`` (to avoid accidental sharing
  of mutable values). A default of ``[]`` is identical to a default of
  ``field(default_factory=list)``, with a new list instance used each time.
  Specifying a non-empty mutable collection (e.g. ``[1, 2, 3]``) as a default
  value will cause the struct definition to error (you should manually define a
  ``default_factory`` in this case).

.. _struct-post-init:

Post-Init Processing
--------------------

If a struct type defines a ``__post_init__(self)`` method, this will be called
at the end of the generated ``__init__`` method. It has the same semantics as the
``dataclasses`` method `of the same name
<https://docs.python.org/3/library/dataclasses.html#post-init-processing>`__.
This method may be useful for adding additional logic to the init (such as
custom validation).

In addition to in ``__init__``, the ``__post_init__`` hook is also called when:

- Decoding into a struct type (e.g. ``msgspec.json.decode(..., type=MyStruct)``)
- Converting into a struct type (e.g. ``msgspec.convert(..., type=MyStruct)``)

In these cases any `TypeError` or `ValueError` exceptions raised by this method
will be considered "user facing" and converted into a `msgspec.ValidationError`
with additional context. All other exceptions will be raised directly.

.. code-block:: python

    >>> import msgspec

    >>> class Interval(msgspec.Struct):
    ...     low: float
    ...     high: float
    ...
    ...     def __post_init__(self):
    ...         if self.low > self.high:
    ...             raise ValueError("`low` may not be greater than `high`")

    >>> Interval(1, 2)  # valid interval
    Interval(low=1, high=2)

    >>> Interval(2, 1)  # invalid interval
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
      File "<stdin>", line 6, in __post_init__
    ValueError: `low` may not be greater than `high`

    >>> msgspec.json.decode(b'{"low": 2, "high": 1}', type=Interval)  # invalid interval from JSON
    Traceback (most recent call last):
      File "<stdin>", line 6, in __post_init__
    ValueError: `low` may not be greater than `high`

    The above exception was the direct cause of the following exception:

    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: `low` may not be greater than `high`

.. _struct-field-ordering:

Field Ordering
--------------

When defining a new struct type, fields are stored in the order they're defined
(keyword-only fields excluded, more on this later). This is nice for
readability since the generated ``__init__`` matches the field order.

.. code-block:: python

    class Example(msgspec.Struct):
        a: str
        b: int = 0

The generated ``__init__()`` for ``User`` looks like:

.. code-block:: python

    def __init__(self, a: str, b: int = 0):

One consequence of this is that you can't put fields without defaults after
fields with defaults, since the Python VM doesn't allow keyword arguments
before positional arguments. The following struct definition will error:

.. code-block:: python

   >>> class Invalid(msgspec.Struct):
   ...     a: str = ""
   ...     b: int  # oop, no default!
   Traceback (most recent call last):
     File "<stdin>", line 1, in <module>
   TypeError: Required field 'b' cannot follow optional fields. Either reorder
   the struct fields, or set `kw_only=True` in the struct definition.

Thankfully the error message includes some solutions:

- Reorder the struct fields, putting all required fields before all optional
  fields.

- Set ``kw_only=True`` in the struct definition. This option makes all fields
  defined on the struct `keyword-only parameters`_.

Keyword-only parameters have no such restriction; required and optional
parameters can be mixed in any order.

.. code-block:: python

   >>> class Example(msgspec.Struct, kw_only=True):
   ...     a: str = ""
   ...     b: int  # this is fine with kw_only=True

   >>> Example(a="example", b=123)
   Example(a='example', b=123)

Note that the ``kw_only`` setting only affects fields defined on that class,
*not* those defined on base or subclasses. This means you can define
keyword-only parameters on a base class then add positional parameters in a
subclass. All keyword-only parameters are reordered to go after all positional
fields.

.. code-block:: python

   >>> class Base(msgspec.Struct, kw_only=True):
   ...     a: str = ""
   ...     b: int

   >>> class Subclass(Base):
   ...     c: float
   ...     d: bytes = b""

The generated ``__init__()`` for ``Subclass`` looks like:

.. code-block:: python

    def __init__(self, c: float, d: bytes = b"", * a: str, b: int = 0):

The field ordering rules for ``Struct`` types are identical to those for
`dataclasses`, see the `dataclasses docs <dataclasses>`_ for more information.

Class Variables
---------------

Like `dataclasses`, `msgspec.Struct` types will exclude any attribute
annotations wrapped in `typing.ClassVar` from their fields.

.. code-block:: python

   >>> import msgspec

   >>> from typing import ClassVar

   >>> class Example(msgspec.Struct):
   ...     x: int
   ...     a_class_variable: ClassVar[int] = 2

   >>> Example.a_class_variable
   2

   >>> Example(1)  # only `x` is counted as a field
   Example(x=1)

Note that if using `PEP 563`_ "postponed evaluation of annotations" (e.g.
``from __future__ import annotations``) only the following spellings will work:

- ``ClassVar`` or ``ClassVar[<type>]``
- ``typing.ClassVar`` or ``typing.ClassVar[<type>]``

Importing ``ClassVar`` or ``typing`` under an aliased name (e.g. ``import
typing as typ`` or ``from typing import ClassVar as CV``) will not be properly
detected.

Type Validation
---------------

Unlike some other libraries (e.g. pydantic_), the type annotations on a
`msgspec.Struct` class are not checked at runtime during normal use. Types are
only checked when *decoding* a serialized message when using a `typed decoder
<typed-deserialization>`.

.. code-block:: python

    >>> import msgspec

    >>> class Point(msgspec.Struct):
    ...     x: float
    ...     y: float

    >>> # Improper types in *your* code aren't checked at runtime
    ... Point(x=1, y="oops")
    Point(x=1, y='oops')

    >>> # Improper types when decoding *are* checked at runtime
    ... msgspec.json.decode(b'{"x": 1.0, "y": "oops"}', type=Point)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `float`, got `str` - at `$.y`

This is intentional. Static type checkers like mypy_/pyright_ work well with
``msgspec``, and can be used to catch bugs without ever running your code. When
possible, static tools or unit tests should be preferred over adding expensive
runtime checks which slow down every ``__init__`` call.

The input(s) to your programs however cannot be checked statically, as they
aren't known until runtime. As such, ``msgspec`` does perform type validation
when decoding messages (provided an expected decode type is provided). This
validation is fast enough that it is *negligible in cost* - there is no added
performance benefit when not using it. In fact, in most cases it's faster to
decode a message into a type validated `msgspec.Struct` than into an untyped
`dict`.


Pattern Matching
----------------

If using Python 3.10+, `msgspec.Struct` types can be used in `pattern matching`_
blocks. Replicating an example from `PEP 636`_:

.. code-block:: python

    # NOTE: this example requires Python 3.10+
    >>> import msgspec

    >>> class Point(msgspec.Struct):
    ...     x: float
    ...     y: float

    >>> def where_is(point):
    ...     match point:
    ...         case Point(0, 0):
    ...             print("Origin")
    ...         case Point(0, y):
    ...             print(f"Y={y}")
    ...         case Point(x, 0):
    ...             print(f"X={x}")
    ...         case Point():
    ...             print("Somewhere else")
    ...         case _:
    ...             print("Not a point")

    >>> where_is(Point(0, 6))
    "Y=6"


Equality and Order
------------------

By default struct types define an ``__eq__`` method based on the type
definition. This enables support for equality comparisons. Additionally, you
may configure ``order=True`` to make a struct type *orderable* through
generation of ``__lt__``, ``__le__``, ``__gt__``, and ``__ge__`` methods. These
methods compare and order instances of a struct type the same as if they were
tuples of their field values (in definition order).

.. code-block:: python

    >>> class Point(msgspec.Struct, order=True):
    ...     x: float
    ...     y: float

    >>> Point(1, 2) == Point(1, 2)
    True

    >>> Point(1, 2) < Point(3, 4)
    True


In *rare* instances you may opt to disable generation of the ``__eq__`` method
by configuring ``eq=False``.  Equality checks will then fall back to *identity
comparisons*, where the only value a struct instance of that type will compare
equal to is itself.

.. code-block:: python

    >>> class Point(msgspec.Struct, eq=False):
    ...     x: float
    ...     y: float


    >>> p = Point(1, 2)

    >>> p == Point(1, 2)
    False

    >>> p == p  # identity comparison only
    True


Frozen Instances
----------------

A struct type can optionally be marked as "frozen" by specifying
``frozen=True``. This disables modifying attributes after initialization, and
adds a ``__hash__`` method to the class definition. Note that for the
``__hash__`` to work, all fields on the struct must also be hashable.

.. code-block:: python

    >>> class Point(msgspec.Struct, frozen=True):
    ...     """This struct is immutable & hashable"""
    ...     x: float
    ...     y: float
    ...

    >>> p = Point(1.0, 2.0)

    >>> {p: 1}  # frozen structs are hashable, and can be keys in dicts
    {Point(1.0, 2.0): 1}

    >>> p.x = 2.0  # frozen structs cannot be modified after creation
    Traceback (most recent call last):
        ...
    AttributeError: immutable type: 'Point'


.. _struct-tagged-unions:

Tagged Unions
-------------

By default a serialized struct only contains information on the *values*
present in the struct instance - no information is serialized noting which
struct type corresponds to the message. Instead, the user is expected to
know the type the message corresponds to, and pass that information
appropriately to the decoder.

.. code-block:: python

    >>> import msgspec

    >>> class Get(msgspec.Struct):
    ...     key: str

    >>> msg = msgspec.json.encode(Get("my key"))

    >>> msg  # No type information present in the message
    b'{"key":"my key"}'

    >>> msgspec.json.decode(msg, type=Get)
    Get(key='my key')

In most cases this works well - schemas are often simple and each value may
only correspond to at most one Struct type. However, sometimes you may have a
message (or a field in a message) that may contain one of a number of different
structured types. In this case we need some way to determine the type of the
message from the message itself!

``msgspec`` handles this through the use of `Tagged Unions`_. A new field (the
"tag field") is added to the serialized representation of all struct types in
the union. Each struct type associates a different value (the "tag") with this
field. When the decoder encounters a tagged union it decodes the tag first and
uses it to determine the type to use when decoding the rest of the object. This
process is efficient and makes determining the type of a serialized message
unambiguous.

The quickest way to enable tagged unions is to set ``tag=True`` when defining
every struct type in the union. In this case ``tag_field`` defaults to
``"type"``, and ``tag`` defaults to the struct class name (e.g. ``"Get"``).

.. code-block:: python

    >>> import msgspec

    >>> from typing import Union

    >>> # Pass in ``tag=True`` to tag the structs using the default configuration
    ... class Get(msgspec.Struct, tag=True):
    ...     key: str

    >>> class Put(msgspec.Struct, tag=True):
    ...     key: str
    ...     val: str

    >>> msg = msgspec.json.encode(Get("my key"))

    >>> msg  # "type" is the tag field, "Get" is the tag
    b'{"type":"Get","key":"my key"}'

    >>> # Create a decoder for decoding either Get or Put
    ... dec = msgspec.json.Decoder(Union[Get, Put])

    >>> # The tag value is used to determine the message type
    ... dec.decode(b'{"type": "Put", "key": "my key", "val": "my val"}')
    Put(key='my key', val='my val')

    >>> dec.decode(b'{"type": "Get", "key": "my key"}')
    Get(key='my key')

    >>> # A tagged union can also contain non-struct types.
    ... msgspec.json.decode(
    ...     b'123',
    ...     type=Union[Get, Put, int]
    ... )
    123

If you want to change this behavior to use a different tag field and/or value,
you can further configure things through the ``tag_field`` and ``tag`` kwargs.
A struct's tagging configuration is determined as follows.

- If ``tag`` and ``tag_field`` are ``None`` (the default), or ``tag=False``,
  then the struct is considered "untagged". The struct is serialized with only
  its standard fields, and cannot participate in ``Union`` types with other
  structs.

- If either ``tag`` or ``tag_field`` are non-None, then the struct is
  considered "tagged". The struct is serialized with an additional field (the
  ``tag_field``) mapping to its corresponding ``tag`` value. It can participate
  in ``Union`` types with other structs, provided they all share the same
  ``tag_field`` and have unique ``tag`` values.

- If a struct is tagged, ``tag_field`` defaults to ``"type"`` if not provided
  or inherited. This can be overridden by passing a tag field explicitly (e.g.
  ``tag_field="kind"``). Note that ``tag_field`` must not conflict with any
  other field names in the struct, and must be the same for all struct types in
  a union.

- If a struct is tagged, ``tag`` defaults to the class name (e.g. ``"Get"``) if
  not provided or inherited. This can be overridden by passing a string (or
  less commonly an integer) value explicitly (e.g. ``tag="get"``).  ``tag`` can
  also be passed a callable that takes the class qualname and returns a valid tag
  value (e.g. ``tag=str.lower``). Note that tag values must be unique for all
  struct types in a union, and ``str`` and ``int`` tag types cannot both be
  used within the same union.

If you like subclassing, both ``tag_field`` and ``tag`` are inheritable by
subclasses, allowing configuration to be set once on a base class and reused
for all struct types you wish to tag.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Union

    >>> # Create a base class for tagged structs, where:
    ... # - the tag field is "op"
    ... # - the tag is the class name lowercased
    ... class TaggedBase(msgspec.Struct, tag_field="op", tag=str.lower):
    ...     pass

    >>> # Use the base class to pass on the configuration
    ... class Get(TaggedBase):
    ...     key: str

    >>> class Put(TaggedBase):
    ...     key: str
    ...     val: str

    >>> msg = msgspec.json.encode(Get("my key"))

    >>> msg  # "op" is the tag field, "get" is the tag
    b'{"op":"get","key":"my key"}'

    >>> # Create a decoder for decoding either Get or Put
    ... dec = msgspec.json.Decoder(Union[Get, Put])

    >>> # The tag value is used to determine the message type
    ... dec.decode(b'{"op": "put", "key": "my key", "val": "my val"}')
    Put(key='my key', val='my val')

    >>> dec.decode(b'{"op": "get", "key": "my key"}')
    Get(key='my key')


.. _omit_defaults:

Omitting Default Values
-----------------------

By default, ``msgspec`` encodes all fields in a Struct type, including optional
fields (those configured with a default value).

.. code-block:: python

    >>> import msgspec

    >>> class User(msgspec.Struct):
    ...     name : str
    ...     email : Optional[str] = None
    ...     groups : Set[str] = set()

    >>> alice = User("alice")

    >>> alice  # email & groups are using the default values
    User(name='alice', email=None, groups=set())

    >>> msgspec.json.encode(alice)  # default values are present in encoded message
    b'{"name":"alice","email":null,"groups":[]}'

If the default values are known on the decoding end (making serializing them
redundant), it may be beneficial and desired to omit default values from the
encoded message. This can be done by configuring ``omit_defaults=True`` as part
of the Struct definition:

.. code-block:: python

    >>> import msgspec

    >>> class User(msgspec.Struct, omit_defaults=True):
    ...     name : str
    ...     email : Optional[str] = None
    ...     groups : Set[str] = set()

    >>> alice = User("alice")

    >>> msgspec.json.encode(alice)  # default values are omitted
    b'{"name":"alice"}'

    >>> bob = User("bob", email="bob@company.com")

    >>> msgspec.json.encode(bob)
    b'{"name":"bob","email":"bob@company.com"}'

Omitting defaults reduces the size of the encoded message, and often also
improves encoding and decoding performance (since there's less work to do).

Note that detection of default values is optimized for performance; in certain
situations a default value may still be encoded. For the curious, the current
detection logic is as follows:

.. code-block:: python

    >>> def matches_default(value: Any, default: Any) -> bool:
    ...     """Whether a value matches the default for a field"""
    ...     if value is default:
    ...         return True
    ...     if type(value) != type(default):
    ...         return False
    ...     if type(value) in (list, set, dict) and (len(value) == len(default) == 0):
    ...         return True
    ...     return False


.. _forbid-unknown-fields:

Forbidding Unknown Fields
-------------------------

By default ``msgspec`` will skip unknown fields encountered when decoding into
``Struct`` types. This is normally desired, as it allows for
:doc:`schema-evolution` and more flexible decoding.

One downside is that typos may go unnoticed when decoding ``Struct`` types with
optional fields. For example:

.. code-block:: python

    >>> class Example(msgspec.Struct):
    ...     field_one: int
    ...     field_two: bool = False

    >>> msgspec.json.decode(
    ...     b'{"field_one": 1, "field_twoo": true}',  # oops, a typo
    ...     type=Example
    ... )
    Example(field_one=1, field_two=False)

In this example, the misspelled ``"field_twoo"`` is ignored since no field with
that name exists. Since ``field_two`` has a default value, the default is used
and no error is raised for a missing field.

To prevent typos like this, you can configure ``forbid_unknown_fields=True`` as
part of the struct definition. If this option is enabled, any unknown fields
encountered will result in an error.

.. code-block:: python

    >>> class Example(msgspec.Struct, forbid_unknown_fields=True):
    ...     field_one: int
    ...     field_two: bool = False

    >>> msgspec.json.decode(
    ...     b'{"field_one": 1, "field_twoo": true}',  # oops, a typo
    ...     type=Example
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Object contains unknown field `field_twoo`


Renaming Fields
---------------

Sometimes you want the field name used in the encoded message to differ from
the name used by your Python code. Perhaps you want a ``camelCase`` naming
convention in your JSON messages, but to use ``snake_case`` field names in
Python.

``msgspec`` supports two places for configuring a field's name used for
encoding/decoding:

**On the field definition**

If you're only renaming a few fields, you might find configuring the new names
as part of the field definition to be the simplest option. To do this you can
use the ``name`` argument in `msgspec.field`. Any fields declared with this
option will use the new name for encoding/decoding.

.. code-block:: python

    >>> import msgspec

    >>> class Example(msgspec.Struct):
    ...     x: int
    ...     y: int
    ...     z: int = msgspec.field(name="field_z")  # renamed to "field_z"

    >>> # Python code uses the original field names
    ... ex = Example(x=1, y=2, z=3)

    >>> # Encoded messages use the renamed field names
    ... msgspec.json.encode(ex)
    b'{"x":1,"y":2,"field_z":3}'

    >>> # Decoding also uses the renamed field names
    ... msgspec.json.decode(b'{"x": 1, "y": 2, "field_z": 3}', type=Example)
    Example(x=1, y=2, z=3)

**On the struct definition**

If you're renaming lots of fields (especially if you're renaming them with a
naming convention like ``camelCase``), you may wish to make use of the
``rename`` configuration option in the `Struct` definition instead. This can
take a few different values:

- ``None``: the default, no field renaming (``example_field``)
- ``"lower"``: lowercase all fields (``example_field``)
- ``"upper"``: uppercase all fields (``EXAMPLE_FIELD``)
- ``"camel"``: camelCase all fields (``exampleField``)
- ``"pascal"``: PascalCase all fields (``ExampleField``)
- A mapping from field names to the renamed names. Field names missing from the
  mapping will not be renamed.
- A callable (signature ``rename(name: str) -> Optional[str]``) to use to
  rename all field names. Note that ``None`` for a return value indicates the
  original field name should be used.

The renamed field names are used for encoding and decoding only, any python
code will still refer to them using their original names.

.. code-block:: python

    >>> import msgspec

    >>> class Example(msgspec.Struct, rename="camel"):
    ...     """A struct with fields renamed using camelCase"""
    ...     field_one: int
    ...     field_two: str

    >>> # Python code uses the original field names
    ... ex = Example(1, field_two="two")

    >>> # Encoded messages use the renamed field names
    ... msgspec.json.encode(ex)
    b'{"fieldOne":1,"fieldTwo":"two"}'

    >>> # Decoding uses the renamed field names
    ... msgspec.json.decode(b'{"fieldOne": 3, "fieldTwo": "four"}', type=Example)
    Example(field_one=3, field_two='four')

    >>> # Decoding errors also use the renamed field names
    ... msgspec.json.decode(b'{"fieldOne": 5}', type=Example)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Object missing required field `fieldTwo`

If renaming to camelCase, you may run into issues if your field names contain
acronyms (e.g. ``FQDN`` in ``setHostnameAsFQDN``). Some JSON style guides
prefer to fully-uppercase these components (``FQDN``), but ``msgspec`` has no
way to know if a component is an acroynm or not (and so will result in
``Fqdn``). As such, we recommend using an explicit dict mapping for renaming if
generating `Struct` types to match an existing API.

.. code-block:: python

    # https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.19/#podspec-v1-core
    # An explicit mapping from python name -> JSON field name
    v1podspec_names = {
        ...
        "service_account_name": "serviceAccountName",
        "set_hostname_as_fqdn": "setHostnameAsFQDN",
        ...
    }

    # Pass the mapping to `rename` to explicitly rename all fields
    class V1PodSpec(msgspec.Struct, rename=v1podspec_names):
        ...
        service_account_name: str = ""
        set_hostname_as_fqdn: bool = False
        ...


Note that if both the ``rename`` configuration option and the ``name`` arg to
`msgspec.field` are used, names set explicitly via `msgspec.field` take
precedence.

.. code-block:: python

    >>> import msgspec

    >>> class Example(msgspec.Struct, rename="camel"):
    ...     field_x: int
    ...     field_y: int = msgspec.field(name="y")  # set explicitly

    >>> msgspec.json.encode(Example(1, 2))
    b'{"fieldX":1,"y":2}'


Encoding/Decoding as Arrays
---------------------------

By default Struct objects encode the same dicts, with both the keys and values
present in the message.

.. code-block:: python

    >>> import msgspec

    >>> class Point(msgspec.Struct):
    ...     x: int
    ...     y: int

    >>> msgspec.json.encode(Point(1, 2))
    b'{"x":1,"y":2}'

If you need higher performance (at the cost of more inscrutable message
encoding), you can set ``array_like=True`` on a struct definition. Structs with
this option enabled are encoded/decoded as array-like types, removing the field
names from the encoded message. This can provide on average another ~2x speedup
for decoding (and ~1.5x speedup for encoding).

.. code-block:: python

    >>> class Point2(msgspec.Struct, array_like=True):
    ...     x: int
    ...     y: int

    >>> msgspec.json.encode(Point2(1, 2))
    b'[1,2]'

    >>> msgspec.json.decode(b'[3,4]', type=Point2)
    Point2(x=3, y=4)

Note that :ref:`struct-tagged-unions` also work with structs with
``array_like=True``. In this case the tag is encoded as the first item in the
array, and is used to determine which type in the union to use when decoding.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Union

    >>> class Get(msgspec.Struct, tag=True, array_like=True):
    ...     key: str

    >>> class Put(msgspec.Struct, tag=True, array_like=True):
    ...     key: str
    ...     val: str

    >>> msgspec.json.encode(Get("my key"))
    b'["Get","my key"]'

    >>> msgspec.json.decode(
    ...     b'["Put", "my key", "my val"]',
    ...     type=Union[Get, Put]
    ... )
    Put(key='my key', val='my val')


Runtime Definition
------------------

In some cases it can be useful to dynamically generate `msgspec.Struct` classes
at runtime. This can be handled through the use of `msgspec.defstruct`, which
has a signature similar to `dataclasses.make_dataclass`. See
`msgspec.defstruct` for more information.

.. code-block:: python

    >>> import msgspec

    >>> Point = msgspec.defstruct("Point", [("x", float), ("y", float)])

    >>> p = Point(1.0, 2.0)

    >>> p
    Point(x=1.0, y=2.0)


.. _struct-gc:

Disabling Garbage Collection (Advanced)
---------------------------------------

.. warning::

    This is an advanced optimization, and only recommended for users who fully
    understand the implications of disabling the GC.

Python uses `reference counting`_ to detect when memory can be freed, with a
periodic `cyclic garbage collector`_ pass to detect and free cyclic references.
Garbage collection (GC) is triggered by the number of uncollected GC-enabled
(objects that contain other objects) objects passing a certain threshold. This
design means that garbage collection passes often run during code that creates
a lot of objects (for example, deserializing a large message).

By default, `msgspec.Struct` types will only be tracked if they contain a
reference to a tracked object themselves. This means that structs referencing
only scalar values (ints, strings, bools, ...) won't contribute to GC load, but
structs referencing containers (lists, dicts, structs, ...) will.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Any

    >>> import gc

    >>> class Example(msgspec.Struct):
    ...     x: Any
    ...     y: Any

    >>> ex1 = Example(1, "two")

    >>> # ex1 is untracked, since it only references untracked objects
    ... gc.is_tracked(ex1)
    False

    >>> ex2 = Example([1, 2, 3], (4, 5, 6))

    >>> # ex2 is tracked, since it references tracked objects
    ... gc.is_tracked(ex2)
    True

If you *are certain* that your struct types can *never* participate in a
reference cycle, you *may* find a :ref:`performance boost
<struct-gc-benchmark>` from setting ``gc=False`` on a struct definition. This
boost is tricky to measure in isolation, since it should only result in the
garbage collector not running as frequently - an integration benchmark is
recommended to determine if this is worthwhile for your workload. A workload is
likely to benefit from this optimization in the following situations:

- You're allocating a lot of struct objects at once (for example, decoding a
  large object). Setting ``gc=False`` on these types will reduce the
  likelihood of a GC pass occurring while decoding, improving application
  latency.
- You have a large number of long-lived struct objects. Setting ``gc=False``
  on these types will reduce the load on the GC during collection cycles of
  later generations.

Struct types with ``gc=False`` will never be tracked, even if they reference
container types. It is your responsibility to ensure cycles with these objects
don't occur, as a cycle containing only ``gc=False`` structs will *never* be
collected (leading to a memory leak).


.. _struct-meta-subclasses:

StructMeta Subclasses (Advanced)
---------------------------------

.. versionadded:: 0.20.0

``msgspec-x`` provides comprehensive support for custom metaclasses that inherit from `msgspec.StructMeta`. This advanced feature enables users to create custom struct behaviors through metaclass programming while maintaining full compatibility with msgspec's serialization ecosystem.

**What are StructMeta Subclasses?**

StructMeta subclasses allow you to extend or customize the behavior of struct creation and management by defining your own metaclass that inherits from `msgspec.StructMeta`. This enables advanced patterns like:

- Adding custom validation logic during struct class creation
- Implementing custom field processing or transformation
- Integrating with external frameworks or ORMs
- Creating domain-specific struct variants with specialized behaviors

**Basic Usage**

Here's a simple example of creating and using a custom StructMeta subclass:

.. code-block:: python

    >>> import msgspec
    >>> from msgspec import StructMeta

    >>> class CustomMeta(StructMeta):
    ...     """A custom metaclass that extends StructMeta"""
    ...     def __new__(cls, name, bases, namespace):
    ...         # Custom logic during class creation
    ...         print(f"Creating struct class: {name}")
    ...         return super().__new__(cls, name, bases, namespace)

    >>> class CustomStruct(metaclass=CustomMeta):
    ...     x: int
    ...     y: str
    ...     z: float = 3.14
    Creating struct class: CustomStruct

    >>> # Instances work exactly like regular structs
    ... obj = CustomStruct(x=42, y="hello")
    >>> obj
    CustomStruct(x=42, y='hello', z=3.14)

**Full Compatibility with msgspec Functions**

Structures created with StructMeta subclasses work seamlessly with all msgspec operations:

.. code-block:: python

    >>> from msgspec.structs import asdict, astuple, replace, force_setattr

    >>> # All struct utility functions work
    >>> asdict(obj)
    {'x': 42, 'y': 'hello', 'z': 3.14}

    >>> astuple(obj)
    (42, 'hello', 3.14)

    >>> replace(obj, x=100)
    CustomStruct(x=100, y='hello', z=3.14)

    >>> # JSON encoding/decoding works
    >>> import msgspec.json
    >>> data = msgspec.json.encode(obj)
    >>> msgspec.json.decode(data, type=CustomStruct)
    CustomStruct(x=42, y='hello', z=3.14)

    >>> # MessagePack encoding/decoding works
    >>> import msgspec.msgpack
    >>> data = msgspec.msgpack.encode(obj)
    >>> msgspec.msgpack.decode(data, type=CustomStruct)
    CustomStruct(x=42, y='hello', z=3.14)

    >>> # Type conversion works
    >>> msgspec.convert({'x': 1, 'y': 'test', 'z': 2.5}, type=CustomStruct)
    CustomStruct(x=1, y='test', z=2.5)

**Multi-Level Inheritance**

StructMeta subclasses support inheritance chains, allowing for sophisticated metaclass hierarchies:

.. code-block:: python

    >>> class BaseMeta(StructMeta):
    ...     """Base custom metaclass"""
    ...     pass

    >>> class DerivedMeta(BaseMeta):
    ...     """Derived custom metaclass"""
    ...     def __new__(cls, name, bases, namespace):
    ...         # Add custom behavior
    ...         result = super().__new__(cls, name, bases, namespace)
    ...         result._custom_attribute = f"Enhanced {name}"
    ...         return result

    >>> class EnhancedStruct(metaclass=DerivedMeta):
    ...     value: int
    ...     name: str

    >>> obj = EnhancedStruct(value=123, name="test")
    >>> obj._custom_attribute
    'Enhanced EnhancedStruct'

    >>> # All msgspec functions still work
    >>> asdict(obj)
    {'value': 123, 'name': 'test'}

**Nested Structures**

StructMeta subclasses work correctly with nested structures and complex serialization scenarios:

.. code-block:: python

    >>> class ContainerMeta(StructMeta):
    ...     """Metaclass for container structures"""
    ...     pass

    >>> class Item(metaclass=ContainerMeta):
    ...     id: int
    ...     name: str

    >>> class Container(metaclass=ContainerMeta):
    ...     items: list[Item]
    ...     count: int

    >>> container = Container(
    ...     items=[Item(id=1, name="first"), Item(id=2, name="second")],
    ...     count=2
    ... )

    >>> # Complex nested encoding/decoding works
    >>> data = msgspec.json.encode(container)
    >>> decoded = msgspec.json.decode(data, type=Container)
    >>> decoded.items[0].name
    'first'

**Integration with Struct Options**

StructMeta subclasses work with all struct configuration options:

.. code-block:: python

    >>> class FrozenMeta(StructMeta):
    ...     """Metaclass for immutable structures"""
    ...     pass

    >>> class ImmutablePoint(metaclass=FrozenMeta, frozen=True, order=True):
    ...     x: float
    ...     y: float

    >>> p1 = ImmutablePoint(1.0, 2.0)
    >>> p2 = ImmutablePoint(3.0, 4.0)

    >>> # Frozen behavior works
    >>> try:
    ...     p1.x = 5.0
    ... except AttributeError as e:
    ...     print(f"Expected error: {e}")
    Expected error: immutable type: 'ImmutablePoint'

    >>> # Ordering works
    >>> p1 < p2
    True

    >>> # All msgspec functions work
    >>> replace(p1, x=10.0)
    ImmutablePoint(x=10.0, y=2.0)

**Technical Implementation**

``msgspec-x`` achieves StructMeta subclass support by modifying the core C implementation to use `PyType_IsSubtype()` checks instead of direct type comparisons. This change affects all core msgspec operations including:

- Type validation during encoding/decoding
- Struct utility functions (asdict, astuple, replace, etc.)
- Type conversion operations
- Tagged union resolution
- Performance optimizations

The implementation maintains full backward compatibility - existing code continues to work unchanged, while new code can take advantage of the enhanced metaclass support.

**Use Cases**

StructMeta subclasses enable advanced patterns such as:

- **Framework Integration**: Creating structs that automatically integrate with web frameworks, ORMs, or validation libraries
- **Domain-Specific Languages**: Building specialized struct types for specific problem domains
- **Automatic Documentation**: Metaclasses that generate documentation or schema information
- **Validation Enhancement**: Adding complex validation logic at the class level
- **Serialization Customization**: Implementing custom serialization behaviors for specific use cases

This feature is particularly valuable for library authors who want to build higher-level abstractions on top of msgspec's fast serialization capabilities.

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _pattern matching: https://docs.python.org/3/reference/compound_stmts.html#the-match-statement
.. _PEP 636: https://peps.python.org/pep-0636/
.. _PEP 563: https://peps.python.org/pep-0563/
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _attrs: https://www.attrs.org/en/stable/index.html
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright
.. _reference counting: https://en.wikipedia.org/wiki/Reference_counting
.. _cyclic garbage collector: https://devguide.python.org/garbage_collector/
.. _tagged unions: https://en.wikipedia.org/wiki/Tagged_union
.. _rich: https://rich.readthedocs.io/en/stable/pretty.html
.. _keyword-only parameters: https://docs.python.org/3/glossary.html#term-parameter
.. _lambda: https://docs.python.org/3/tutorial/controlflow.html#lambda-expressions
