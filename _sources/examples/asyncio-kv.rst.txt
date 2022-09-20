Asyncio TCP Key-Value Server
============================

This example demonstrates writing a small TCP server and client using `asyncio`
and ``msgspec``.

The server defines a few operations:

- ``get(key: str) -> str | None``: get the value for a single key from the
  store if it exists.
- ``put(key: str, val: str) -> None``: add a new key-value pair to the store.
- ``delete(key: str) -> None``: delete a key-value pair from the store if it exists.
- ``list_keys() -> list[str]``: list all the keys currently set in the store.

Each operation has a corresponding request type defined as a :doc:`Struct <../structs>`
type. Note that these structs are :ref:`tagged <struct-tagged-unions>` so they
can be part of a ``Union`` of all request types the server handles.

`msgspec.msgpack` is used to handle the encoding/decoding of the various
messages. The length of each message is prefixed to each message
(`Length-prefix framing
<https://eli.thegreenplace.net/2011/08/02/length-prefix-framing-for-protocol-buffers>`__)
to make it easier to efficiently determine message boundaries.

The full example source can be found `here
<https://github.com/jcrist/msgspec/blob/main/examples/asyncio-kv>`__.

.. literalinclude:: ../../../examples/asyncio-kv/kv.py
    :language: python


An example usage session:

**Server**

.. code-block:: shell

   $ python kv.py
   Serving on tcp://127.0.0.1:8888...
   Connection opened
   Connection closed


**Client**

.. code-block:: ipython3

    In [1]: from kv import Client

    In [2]: client = await Client.create()

    In [3]: await client.put("foo", "bar")

    In [4]: await client.put("fizz", "buzz")

    In [5]: await client.get("foo")
    Out[5]: 'bar'

    In [6]: await client.list_keys()
    Out[6]: ['fizz', 'foo']

    In [7]: await client.delete("fizz")

    In [8]: await client.list_keys()
    Out[8]: ['foo']
