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

Encoding/Decoding as Arrays
---------------------------

By default Struct objects encode like dicts, with both the keys and values
present in the message. If you need higher performance (at the cost of more
inscrutable message encoding), you can set ``asarray=True`` on a struct
definition. Structs with this option enabled are encoded/decoded like array
types, removing the field names from the encoded message. This can provide on
average another ~2x speedup for decoding (and ~1.5x speedup for encoding).

.. code-block:: python

    >>> class AsArrayStruct(msgspec.Struct, asarray=True):
    ...     """This struct is serialized like an array (instead of like
    ...     a dict). This means no field names are sent as part of the
    ...     message, speeding up encoding/decoding."""
    ...     my_first_field: str
    ...     my_second_field: int

    >>> x = AsArrayStruct("some string", 2)

    >>> msg = msgspec.json.encode(x)

    >>> msg
    b'["some string",2]'

    >>> msgspec.json.decode(msg, type=AsArrayStruct)
    AsArrayStruct(my_first_field="some string", my_second_field=2)

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

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _pattern matching: https://docs.python.org/3/reference/compound_stmts.html#the-match-statement
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _attrs: https://www.attrs.org/en/stable/index.html
.. _pydantic: https://pydantic-docs.helpmanual.io/
.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright
