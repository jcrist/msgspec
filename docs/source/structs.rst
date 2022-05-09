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
  provided, it defaults to the empty set (note that mutable default values are
  deep-copied before use).

Struct types automatically generate a few methods based on the provided type
annotations:

- ``__init__``
- ``__repr__``
- ``__copy__``
- ``__eq__`` & ``__ne__``
- ``__match_args__`` (for Python 3.10+'s `pattern matching`_)

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
definition, but other methods can be overridden or added as needed.

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
    msgspec.DecodeError: Expected `float`, got `str` - at `$.y`

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


Frozen Instances
----------------

A struct type can optionally be marked as "frozen" by specifying
``frozen=True``. This disables modifying attributes after initialization,
and adds a ``__hash__`` method to the class definition.

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
    ... dec = msgspec.Decoder(Union[Get, Put])

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
  not provided or inherited. This can be overridden by passing a tag value
  explicitly (e.g. ``tag="get"``). or a callable from class name to ``str``
  (e.g.  ``tag=lambda name: name.lower()`` to lowercase the class name
  automatically). Note that the tag value must be unique for all struct types
  in a union.

If you like subclassing, both ``tag_field`` and ``tag`` are inheritable by
subclasses, allowing configuration to be set once on a base class and reused
for all struct types you wish to tag.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Union

    >>> # Create a base class for tagged structs, where:
    ... # - the tag field is "op"
    ... # - the tag is the class name lowercased
    ... class TaggedBase(tag_field="op", tag=lambda name: name.lowercase()):
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
    ... dec = msgspec.Decoder(Union[Get, Put])

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


Renaming Field Names
--------------------

Sometimes you want the field name used in the encoded message to differ from
the name used your Python code. Perhaps you want a ``camelCase`` naming
convention in your JSON messages, but to use ``snake_case`` field names in
Python.

To handle this, ``msgspec`` supports a ``rename`` configuration option in
`Struct` definitions. This can take a few different values:

- ``None``: the default, no field renaming (``example_field``)
- ``"lower"``: lowercase all fields (``example_field``)
- ``"upper"``: uppercase all fields (``EXAMPLE_FIELD``)
- ``"camel"``: camelCase all fields (``exampleField``)
- ``"pascal"``: PascalCase all fields (``ExampleField``)
- A callable (signature ``rename(name: str) -> str``) to use to rename all
  field names

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
    msgspec.DecodeError: Object missing required field `fieldTwo`


Note that if renaming to camelCase, you may run into issues if your field names
contain acronyms (e.g. ``FQDN`` in ``setHostnameAsFQDN``). Some JSON style
guides prefer to fully-uppercase these components (``FQDN``), but ``msgspec``
has no way to know if a component is an acroynm or not (and so will result in
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

    # Pass `.get` method to `rename` to explicitly rename all fields
    class V1PodSpec(msgspec.Struct, rename=v1podspec_names.get):
        ...
        service_account_name: str = ""
        set_hostname_as_fqdn: bool = False
        ...


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

    >>> Point = msgspec.defstruct("Point", [("x", float), ("y": float)])

    >>> p = Point(1.0, 2.0)

    >>> p
    Point(x=1.0, y=2.0)


.. _struct-nogc:

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
<struct-gc-benchmark>` from setting ``nogc=True`` on a struct definition. This
boost is tricky to measure in isolation, since it should only result in the
garbage collector not running as frequently - an integration benchmark is
recommended to determine if this is worthwhile for your workload. A workload is
likely to benefit from this optimization in the following situations:

- You're allocating a lot of struct objects at once (for example, decoding a
  large object). Setting ``nogc=True`` on these types will reduce the
  likelihood of a GC pass occurring while decoding, improving application
  latency.
- You have a large number of long-lived struct objects. Setting ``nogc=True``
  on these types will reduce the load on the GC during collection cycles of
  later generations.

Struct types with ``nogc=True`` will never be tracked, even if they reference
container types. It is your responsibility to ensure cycles with these objects
don't occur, as a cycle containing only ``nogc=True`` structs will *never* be
collected (leading to a memory leak).

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _pattern matching: https://docs.python.org/3/reference/compound_stmts.html#the-match-statement
.. _PEP 636: https://www.python.org/dev/peps/pep-0636/
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _attrs: https://www.attrs.org/en/stable/index.html
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright
.. _reference counting: https://en.wikipedia.org/wiki/Reference_counting
.. _cyclic garbage collector: https://devguide.python.org/garbage_collector/
.. _tagged unions: https://en.wikipedia.org/wiki/Tagged_union
