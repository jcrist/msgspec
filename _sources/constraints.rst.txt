Constraints
===========

When using :ref:`typed-decoding` ``msgspec`` will ensure decoded
messages match the specified types. For example, to decode a list of integers
from JSON:

.. code-block:: python

    >>> import msgspec

    >>> msgspec.json.decode(b"[1, 2, 3]", type=list[int])
    [1, 2, 3]

    >>> msgspec.json.decode(b'[1, 2, "oops"]', type=list[int])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int`, got `str` - at `$[2]`

Often this is sufficient, but sometimes you also need to impose constraints on
the *values* (rather than the *types*) found in the message.

Constraints in ``msgspec`` are specified by wrapping a type with
`typing.Annotated`, and adding a `msgspec.Meta` annotation.

For example, to constrain the list to positive integers (``> 0``), you'd make
use of the ``gt`` (greater-than) constraint:

.. code-block:: python

    >>> from typing import Annotated

    >>> PositiveInt = Annotated[int, msgspec.Meta(gt=0)]

    >>> msgspec.json.decode(b'[1, 2, 3]', type=list[PositiveInt])
    [1, 2, 3]

    >>> msgspec.json.decode(b'[1, 2, -1]', type=list[PositiveInt])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int` >= 1 - at `$[2]`

Constraints can be combined to enforce complex requirements. Here's a more
complete example enforcing the following constraints on a ``User`` struct:

- ``name`` is a ``str`` with ``1 <= length <= 32`` matching the regular
  expression ``"^[a-z_][a-z0-9_-]*$"``.
- ``groups`` is a ``set`` of at most 16 strings, each with the same constraints
  as ``name`` above, defaulting to the empty ``set``.
- ``cpu_limit`` is a ``float`` with a value ``>= 0.1`` and ``<= 8``, defaulting
  to 1.
- ``mem_limit`` is an ``int`` with a value ``>= 256`` and ``<= 8192``,
  defaulting to 1024.

.. code-block:: python

    from typing import Annotated

    from msgspec import Struct, Meta

    UnixName = Annotated[
        str, Meta(min_length=1, max_length=32, pattern="^[a-z_][a-z0-9_-]*$")
    ]

    class User(Struct):
        name: UnixName
        groups: Annotated[set[UnixName], Meta(max_length=16)] = set()
        cpu_limit: Annotated[float, Meta(ge=0.1, le=8)] = 1
        mem_limit: Annotated[int, Meta(ge=256, le=8192)] = 1024

As shown above, ``Annotated`` types can applied inline, or used to create type
aliases and then reused elsewhere (as done with ``UnixName``).

The following constraints are supported:

Numeric Constraints
-------------------

These constraints are valid on `int` or `float` types:

- ``ge``: The value must be greater than or equal to ``ge``.
- ``gt``: The value must be greater than ``gt``.
- ``le``: The value must be less than or equal to ``le``.
- ``lt``: The value must be less than ``lt``.
- ``multiple_of``: The value must be a multiple of ``multiple_of``.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Annotated

    >>> msgspec.json.decode(b'-1', type=Annotated[int, msgspec.Meta(ge=0)])
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `int` >= 0

.. warning::

    While ``multiple_of`` works on ``float`` types, we don't recommend
    specifying *non-integral* ``multiple_of`` constraints, as they may be
    erroneously marked as invalid due to floating point precision issues. For
    example, annotating a ``float`` type with ``multiple_of=10`` is fine, but
    ``multiple_of=0.1`` may lead to issues. See `this GitHub issue
    <https://github.com/json-schema-org/json-schema-spec/issues/312>`_ for more
    details.

String Constraints
------------------

These constraints are valid on `str` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.
- ``pattern``: A regular expression pattern that the value must match. Note
  that patterns are treated as *unanchored*. This means that the pattern "es"
  matches not just "es" but also "expression". If required, you must explicitly
  anchor the pattern by adding a "^" prefix and "$" suffix. For example, the
  pattern "^es$" only matches the string "es"

.. code-block:: python

    >>> import msgspec

    >>> from typing import Annotated

    >>> msgspec.json.decode(
    ...     b'"invalid username"',
    ...     type=Annotated[str, msgspec.Meta(pattern="^[a-z0-9_]*$")]
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `str` matching regex '^[a-z0-9_]*$'

.. _datetime-constraints:

Datetime Constraints
--------------------

These constraints are valid on `datetime.datetime` and `datetime.time` types:

- ``tz``: Whether the annotated type is required to be timezone-aware_. Set to
  ``True`` to require timezone-aware values, or ``False`` to require
  timezone-naive values. The default is ``None``, which accepts either
  timezone-aware or timezone-naive values.

.. code-block:: python

    >>> import msgspec

    >>> from datetime import datetime

    >>> from typing import Annotated

    >>> msgspec.json.decode(
    ...     b'"2022-04-02T18:18:10"',
    ...     type=Annotated[datetime, msgspec.Meta(tz=True)]  # require timezone aware
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `datetime` with a timezone component

    >>> msgspec.json.decode(
    ...     b'"2022-04-02T18:18:10-06:00"',
    ...     type=Annotated[datetime, msgspec.Meta(tz=False)]  # require timezone naive
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `datetime` with no timezone component

Bytes Constraints
-----------------

These constraints are valid on `bytes` and `bytearray` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Annotated

    >>> msgspec.json.decode(
    ...     b'"ZXhhbXBsZQ=="',
    ...     type=Annotated[bytes, msgspec.Meta(min_length=10)]
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `bytes` of length >= 10

Sequence Constraints
--------------------

These constraints are valid on `list`, `tuple`, `set`, and `frozenset` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Annotated

    >>> msgspec.json.decode(
    ...     b'[1, 2, 3, 4]',
    ...     type=Annotated[list[int], msgspec.Meta(max_length=3)]
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `array` of length <= 3

Mapping Constraints
-------------------

These constraints are valid on `dict` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> import msgspec

    >>> from typing import Annotated

    >>> msgspec.json.decode(
    ...     b'{"a": 1, "b": 2, "c": 3, "d": 4}',
    ...     type=Annotated[dict[str, int], msgspec.Meta(max_length=3)]
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    msgspec.ValidationError: Expected `object` of length <= 3

.. _timezone-aware: https://docs.python.org/3/library/datetime.html#aware-and-naive-objects
