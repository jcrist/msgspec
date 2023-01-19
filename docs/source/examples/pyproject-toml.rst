Parsing ``pyproject.toml``
==========================

`PEP 518`_ defined a new ``pyproject.toml`` configuration file Python projects
can use for configuring:

- Metadata (name, version, ...)
- Dependencies
- Build systems
- Additional development tools (black_, mypy_, pytest_, ... all support
  ``pyproject.toml`` files for configuration).

The format was defined in a series of Python Enhancement Proposals (PEPs),
which also serve as the main documentation for the file schema.

- `PEP 517`_: A build-system independent format for source trees
- `PEP 518`_: Specifying minimum build system requirements for Python projects
- `PEP 621`_: Storing project metadata in pyproject.toml

Here we define a msgspec schema for parsing and validating a ``pyproject.toml``
file. This includes full schema definitions for all fields in the
``build-system`` and ``project`` tables, as well as an untyped table under
``tool``.

The full example source can be found `here
<https://github.com/jcrist/msgspec/blob/main/examples/pyproject-toml>`__.

.. literalinclude:: ../../../examples/pyproject-toml/pyproject.py
    :language: python

Here we use it to load the `pyproject.toml for Starlette
<https://github.com/encode/starlette/blob/master/pyproject.toml>`__:

.. code-block:: ipython3

    In [1]: import pyproject

    In [2]: import urllib.request

    In [3]: url = "https://raw.githubusercontent.com/encode/starlette/master/pyproject.toml"

    In [4]: with urllib.request.urlopen(url) as f:
       ...:     data = f.read()

    In [5]: result = pyproject.decode(data)  # decode the pyproject.toml

    In [6]: result.build_system
    Out[6]: BuildSystem(requires=['hatchling'], build_backend='hatchling.build', backend_path=[])

    In [7]: result.project.name
    Out[7]: 'starlette'

Note that this only validates that fields are of the proper type. It doesn't
check:

- Whether strings like URLs or `dependency specifiers`_ are valid. Some of
  these could be handled using msgspec's existing :doc:`../constraints` system,
  but not all of them.
- Mutually exclusive field restrictions (for example, you can't set both
  ``project.license.file`` and ``project.license.text``). ``msgspec`` currently
  has no way of declaring these restrictions.

Even with these caveats, the schemas here are still useful:

- Since ``forbid_unknown_fields=True`` is configured, any extra fields will
  raise a nice error message. This is very useful for catching typos in
  configuration files, as the misspelled field names won't be silently ignored.
- Type errors for fields will also be caught, with a nice error raised.
- Any downstream consumers of ``decode`` have a nice high-level object to work
  with, complete with type annotations. This plays well with tab-completion and
  tools like mypy_ or pyright_, improving usability.

For example, here's an invalid ``pyproject.toml``.

.. code-block:: toml

    [build-system]
    requires = "hatchling"
    build-backend = "hatchling.build"

    [project]
    name = "myproject"
    version = "0.1.0"
    description = "a super great library"
    authors = [
        {name = "alice shmalice", email = "alice@company.com"}
    ]

Can you spot the error? Using the schemas defined above, ``msgpspec`` can
detect schema issues like this, and raise a nice error message. In this case
the issue is that ``build-system.requires`` should be an array of strings, not
a single string:

.. code-block:: ipython

    In [1]: import pyproject

    In [2]: with open("pyproject.toml", "rb") as f:
       ...:     invalid = f.read()

    In [3]: pyproject.decode(invalid)
    ---------------------------------------------------------------------------
    ValidationError                           Traceback (most recent call last)
    Cell In [3], line 1
    ----> 1 pyproject.decode(invalid)
    ValidationError: Expected `array`, got `str` - at `$.build-system.requires`


.. _PEP 517: https://peps.python.org/pep-0517/
.. _PEP 518: https://peps.python.org/pep-0518/
.. _PEP 621: https://peps.python.org/pep-0621/
.. _black: https://black.readthedocs.io
.. _mypy: https://mypy.readthedocs.io
.. _pyright: https://github.com/microsoft/pyright
.. _pytest: https://docs.pytest.org
.. _dependency specifiers: https://packaging.python.org/en/latest/specifications/dependency-specifiers/
