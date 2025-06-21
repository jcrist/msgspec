Inspecting Types
----------------

.. currentmodule:: msgspec.inspect

.. warning::

   This module is experimental. While we don't expect any breaking changes, we
   also don't promise not to break things between releases while this interface
   stabilizes.

``msgspec`` provides type-introspection support, which can be used to build
tooling on top of msgspec-compatible types. Possible use cases include:

- Generating OpenAPI_ specifications from msgspec-compatible types (note that
  the builtin :doc:`jsonschema` support may be a better starting point for
  this).
- Generating example instances of types for testing or documentation purposes
- Integration with hypothesis_ for testing

The main function here is `msgspec.inspect.type_info` for converting a type
annotation into a corresponding `msgspec.inspect.Type` object. There's also
`msgspec.inspect.multi_type_info` which converts an iterable of annotations;
this function is more efficient than calling `type_info` in a loop.

.. code-block:: python

    >>> import msgspec

    >>> msgspec.inspect.type_info(bool)
    BoolType()

    >>> msgspec.inspect.type_info(int)
    IntType(gt=None, ge=None, lt=None, le=None, multiple_of=None)

    >>> msgspec.inspect.type_info(list[int])  # nested types are traversed
    ListType(
        item_type=IntType(gt=None, ge=None, lt=None, le=None, multiple_of=None),
        min_length=None,
        max_length=None
    )

    >>> msgspec.inspect.multi_type_info([bool, int])  # inspect multiple types
    (BoolType(), IntType(gt=None, ge=None, lt=None, le=None, multiple_of=None))


Types with :doc:`constraints` will include the constraint information as well:

.. code-block:: python

    >>> from typing import Annotated

    >>> from msgspec import Meta

    >>> PositiveInt = Annotated[int, Meta(gt=0)]

    >>> msgspec.inspect.type_info(PositiveInt)
    IntType(gt=0, ge=None, lt=None, le=None, multiple_of=None)

Compound types like :doc:`structs` are also supported:

.. code-block:: python

    >>> class User(msgspec.Struct):
    ...     name: str
    ...     groups: list[str] = []
    ...     email: str | None = None

    >>> msgspec.inspect.type_info(User)
    StructType(
        cls=User,
        fields=(
            Field(
                name='name',
                encode_name='name',
                type=StrType(min_length=None, max_length=None, pattern=None),
                required=True,
                default=UNSET,
                default_factory=UNSET
            ),
            Field(
                name='groups',
                encode_name='groups',
                type=ListType(
                    item_type=StrType(min_length=None, max_length=None, pattern=None),
                    min_length=None,
                    max_length=None
                ),
                required=False,
                default=[],
                default_factory=UNSET
            ),
            Field(
                name='email',
                encode_name='email',
                type=UnionType(
                    types=(
                        StrType(min_length=None, max_length=None, pattern=None),
                        NoneType()
                    )
                ),
                required=False,
                default=None,
                default_factory=UNSET
            )
        ),
        tag_field=None,
        tag=None,
        array_like=False,
        forbid_unknown_fields=False
    )

Types with additional metadata like ``extra_json_schema`` or ``title`` will be
wrapped in a `msgspec.inspect.Metadata` object. Note that all JSON schema
specific fields are merged into a single ``extra_json_schema`` dict.

.. code-block:: python

    >>> UnixName = Annotated[
    ...     str,
    ...     Meta(
    ...         min_length=1,
    ...         max_length=32,
    ...         pattern="^[a-z_][a-z0-9_-]*$",
    ...         description="A valid UNIX username"
    ...     )
    ... ]

    >>> msgspec.inspect.type_info(UnixName)
    Metadata(
        type=StrType(
            min_length=1,
            max_length=32,
            pattern='^[a-z_][a-z0-9_-]*$'
        ),
        extra_json_schema={'description': 'A valid UNIX username'}
    )

Every type supported by ``msgspec`` has a corresponding `msgspec.inspect.Type`
subclass. See the :ref:`API docs <inspect-api>` for a complete list of types.

For an example of using these functions, you might find our builtin
:doc:`jsonschema` generator implementation useful - the code for this can be
found `here
<https://github.com/nightsailer/msgspec-x/blob/main/msgspec/_json_schema.py>`__. In
particular, take a look at the large if-else statement in ``_to_schema``.


.. _OpenAPI: https://www.openapis.org/
.. _hypothesis: https://hypothesis.readthedocs.io/en/latest/
