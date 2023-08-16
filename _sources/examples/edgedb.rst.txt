Usage with EdgeDB
=================

.. image:: ../_static/edgedb.svg
   :width: 60%
   :align: center

`EdgeDB <https://www.edgedb.com/>`__ is an interesting new `graph-relational
database <https://www.edgedb.com/blog/the-graph-relational-database-defined>`__
system. It includes a powerful and ergonomic query language `"EdgeQL"
<https://www.edgedb.com/docs/edgeql/index>`__, along with client libraries that
integrate well with their respective language ecosystems.

In this example we demonstrate a few ways of integrating EdgeDB's
`Python client library <https://www.edgedb.com/docs/clients/python>`__
with ``msgspec``.

Setup
-----

This is not intended to be a complete EdgeDB tutorial; for that we recommend
going through the `official EdgeDB quickstart
<https://www.edgedb.com/docs/intro/quickstart>`__. This example assumes you
already have the EdgeDB CLI and Python library installed.

After cloning the ``msgspec`` repo, navigate to the ``edgedb`` example
directory `here
<https://github.com/jcrist/msgspec/blob/main/examples/edgedb>`__. Then
initialize a new ``edgedb`` project.

.. code-block:: bash

    $ edgedb project init --server-instance edgedb-msgspec-example --non-interactive


This will setup a new instance and apply the example schema:

.. literalinclude:: ../../../examples/edgedb/dbschema/default.esdl

We then need to insert some records. This is done with the following EdgeQL
query:

.. literalinclude:: ../../../examples/edgedb/insert_data.edgeql

To run this, execute the following:

.. code-block:: bash

    $ edgedb query -f insert_data.edgeql


JSON Encoding Query Results
---------------------------

The EdgeDB Python library returns objects as ``edgedb.Object`` instances
(`docs <https://www.edgedb.com/docs/clients/python/api/types#edgedb.Object>`__).
Here we query the movie "Dune" that we inserted above, requesting the movie
title and actors' names.

.. code-block:: python

    >>> import edgedb

    >>> import msgspec

    >>> client = edgedb.create_client()

    >>> dune = client.query_single(
    ...     """
    ...     SELECT Movie {
    ...         title,
    ...         actors: {
    ...             name
    ...         }
    ...     }
    ...     FILTER .title = 'Dune'
    ...     LIMIT 1
    ...     """
    ... )

    >>> dune
    Object{title := 'Dune', actors := [Object{name := 'Timothée Chalamet'}, Object{name := 'Zendaya'}]}

    >>> type(dune)
    edgedb.Object


These ``edgedb.Object`` instances are duck-type compatible with `dataclasses`,
which means ``msgspec`` already knows how to JSON encode them.

.. code-block:: python

    >>> json = msgspec.json.encode(dune)

    >>> print(msgspec.json.format(json.decode()))  # pretty-print the JSON
    {
      "id": "b21913c4-3b68-11ee-89b0-2f0b6819503d",
      "title": "Dune",
      "actors": [
        {
          "id": "b219195a-3b68-11ee-89b0-5b3794805cc7",
          "name": "Timothée Chalamet"
        },
        {
          "id": "b2192058-3b68-11ee-89b0-f7d83b95fb13",
          "name": "Zendaya"
        }
      ]
    }

Note that if you're immediately JSON encoding the results you may be better
served by using EdgeDB's ``query_json``/``query_single_json`` methods, which
return JSON strings directly (but strip the ``id`` fields).

.. code-block:: python

    >>> edgedb_json = client.query_single_json(
    ...     """
    ...     SELECT Movie {
    ...         title,
    ...         actors: {
    ...             name
    ...         }
    ...     }
    ...     FILTER .title = 'Dune'
    ...     LIMIT 1
    ...     """
    ... )

    >>> edgedb_json
    '{"title" : "Dune", "actors" : [{"name" : "Timothée Chalamet"},{"name" : "Zendaya"}]}'

If needed, this JSON string may be efficiently composed into a larger JSON
object using `msgspec.Raw`. Here we add some additional outer structure
wrapping the query result:

.. code-block:: python

    >>> import datetime

    >>> msg = {
    ...     "timestamp": datetime.datetime.now(datetime.timezone.utc),
    ...     "server_version": "3.2",
    ...     "query_result": msgspec.Raw(edgedb_json),
    ... }

    >>> json = msgspec.json.encode(msg)

    >>> print(msgspec.json.format(json.decode()))  # pretty-print the JSON
    {
      "timestamp": "2023-08-15T14:37:12.733731Z",
      "server_version": "3.2",
      "query_result": {
        "title": "Dune",
        "actors": [
          {
            "name": "Timothée Chalamet"
          },
          {
            "name": "Zendaya"
          }
        ]
      }
    }

Supporting Other EdgeDB Types
-----------------------------

Besides ``edgedb.Object``, ``msgspec`` also includes builtin support for JSON
encoding ``edgedb.NamedTuple`` types. There are a few remaining ``edgedb``
types that ``msgspec`` doesn't support out-of-the-box:

- ``edgedb.DateDuration`` (`docs <https://www.edgedb.com/docs/clients/python/api/types#edgedb.DateDuration>`__)
- ``edgedb.RelativeDuration`` (`docs <https://www.edgedb.com/docs/clients/python/api/types#edgedb.RelativeDuration>`__)

JSON encoding support for these may be added through the use of
:doc:`extensions <../extending>`.

.. code-block:: python

    >>> def enc_hook(obj):
    ...     if isinstance(obj, (edgedb.DateDuration, edgedb.RelativeDuration)):
    ...         # The str representation of these types are ISO8601 durations,
    ...         return str(obj)
    ...     # Raise a NotImplementedError for unsupported types
    ...     raise NotImplementedError

    >>> duration = client.query_single('SELECT <cal::date_duration>"1 year 2 days"')

    >>> duration
    <edgedb.DateDuration "P1Y2D">

    >>> msgspec.json.encode(duration, enc_hook=enc_hook)
    b'"P1Y2D"'

Converting Results to Structs
-----------------------------

If your application contains complex server-side logic, you may wish to convert
the query results into some other application-specific structured type.
``msgspec`` supports automatic conversion to other types `msgspec.convert`.

Here we'll define two `msgspec.Struct` types mirroring our Schema above:

.. code-block:: python

    >>> class Person(msgspec.Struct):
    ...     name: str

    >>> class Movie(msgspec.Struct):
    ...     title: str
    ...     actors: list[Person]

We can then convert the ``edgedb.Object`` results into our ``Struct`` types
using `msgspec.convert`. Note that the same conversion process would work if
``Person`` or ``Movie`` were defined as `dataclasses` or `attrs` types instead.

.. code-block:: python

    >>> msgspec.convert(dune, Movie, from_attributes=True)
    Movie(title='Dune', actors=[Person(name='Timothée Chalamet'), Person(name='Zendaya')])

These structs may then be used to implement application logic
(mutating/combining them as needed) before serializing the output to JSON.
