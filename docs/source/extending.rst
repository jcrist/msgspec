Extending
=========

To allow encoding/decoding types other than those :doc:`natively supported
<supported-types>`, ``msgspec`` provides a few callbacks to
``Encoder``/``Decoder``.

- ``enc_hook``, for transforming custom types into values
  that ``msgspec``:doc:`natively supports <supported-types>`.
- ``dec_hook``, for converting natively supported types back into
  a custom type when using :ref:`typed decoding <typed-decoding>`.
- ``ext_hook`` (MessagePack only), for converting `MessagePack extensions`_
  back into custom types.

These should have the following signatures:

.. code-block:: python

    def enc_hook(obj: Any) -> Any:
        """Given an object that msgspec doesn't know how to serialize by
        default, convert it into an object that it does know how to
        serialize"""
        pass

    def dec_hook(type: Type, obj: Any) -> Any:
        """Given a type in a schema, convert ``obj`` (composed of natively
        supported objects) into an object of type ``type``.

        Any `TypeError` or `ValueError` exceptions raised by this method will
        be considered "user facing" and converted into a `ValidationError` with
        additional context. All other exceptions will be raised directly.
        """
        pass

    def ext_hook(code: int, data: memoryview) -> Any:
        """MessagePack only. Given an extension type code and data buffer,
        deserialize whatever custom object the extension type represents"""
        pass

These can be composed together to form complex behaviors as needed.
However, most use cases follow one of these patterns:

- Mapping a custom type to/from natively supported types via ``enc_hook`` and
  ``dec_hook`` callbacks.
- Defining a custom `MessagePack extension`_ to represent your type, then
  mapping to/from that extension via ``enc_hook`` and ``ext_hook`` callbacks.

Both methods are illustrated below.

Mapping to/from native types
----------------------------

This method uses messages composed only of natively supported types. During
encoding, custom types are mapped to natively supported types, which are then
serialized. This process is then reversed during decoding.

.. code-block::

    custom type -> native types -> encoded message -> native types -> custom type

This means that :ref:`typed decoding <typed-decoding>` is required to roundtrip
a message, since no custom type info is sent as part of the message.

This method works best for types that are similar to a natively supported type
(e.g. a `collections.deque` is similar to a `list`).  This can be accomplished
by defining two callback functions:

- ``enc_hook`` in ``Encoder``, for transforming custom types into values
  that ``msgspec`` already knows how to serialize.
- ``dec_hook`` in ``Decoder``, for converting natively supported types back
  into a custom type when using :ref:`typed decoding <typed-decoding>`.

Here we define ``enc_hook`` and ``dec_hook`` callbacks to convert `complex`
objects to/from objects, which are then natively handled by ``msgspec``.

.. code-block:: python

    import msgspec
    from typing import Any, Type

    def enc_hook(obj: Any) -> Any:
        if isinstance(obj, complex):
            # convert the complex to a tuple of real, imag
            return (obj.real, obj.imag)
        else:
            # Raise a NotImplementedError for other types
            raise NotImplementedError(f"Objects of type {type(obj)} are not supported")


    def dec_hook(type: Type, obj: Any) -> Any:
        # `type` here is the value of the custom type annotation being decoded.
        if type is complex:
            # Convert ``obj`` (which should be a ``tuple``) to a complex
            real, imag = obj
            return complex(real, imag)
        else:
            # Raise a NotImplementedError for other types
            raise NotImplementedError(f"Objects of type {type} are not supported")


    # Define a message that contains a complex type
    class MyMessage(msgspec.Struct):
        field_1: str
        field_2: complex

    # Create an encoder and a decoder using the custom callbacks.
    # Note that typed deserialization is required for successful
    # roundtripping here, so we pass `MyMessage` to `Decoder`.
    enc = msgspec.json.Encoder(enc_hook=enc_hook)
    dec = msgspec.json.Decoder(MyMessage, dec_hook=dec_hook)

    # An example message
    msg = MyMessage("some string", complex(1, 2))

    # Encode and decode the message to show that things work
    buf = enc.encode(msg)
    msg2 = dec.decode(buf)
    assert msg == msg2  # True

.. _defining-extensions:

Defining a custom extension (MessagePack only)
----------------------------------------------

The MessagePack specification provides support for defining custom Extensions_.
Extensions consist of:

- An integer code (between 0 and 127, inclusive) representing the "type" of the
  extension.
- An arbitrary byte buffer of data (up to ``(2^32) - 1`` in length).

By default extensions are serialized to/from `msgspec.msgpack.Ext` objects.

.. code-block:: python

    >>> ext = msgspec.msgpack.Ext(1, b"some data")  # an extension object, with type code 1
    >>> msg = msgspec.msgpack.encode(ext)
    >>> ext2 = msgspec.msgpack.decode(msg)
    >>> ext == ext2  # deserializes as an Ext object
    True

While manually creating `Ext` objects from buffers can be useful, usually the
user wants to map extension types to/from their own custom objects. This can be
accomplished by defining two callback functions:

- ``enc_hook`` in `msgspec.msgpack.Encoder`, for transforming custom types into
  values that ``msgspec`` already knows how to serialize.

- ``ext_hook`` in `msgspec.msgpack.Decoder`, for converting extensions back
  into those custom types.

This method defines a new extension type, and sends this type information
along as part of the message. This means that when properly configured, custom
types can be deserialized even when using untyped deserialization. However, if
you're communicating with MessagePack libraries other than ``msgspec``, you'd
have to ensure your extension type was supported by those libraries as well.

For example, perhaps you wanted to serialize `complex` number objects as an
extension type.  These objects can be represented as tuples of two floats (one
"real" and one "imaginary"). If we represent each float as 8 bytes (a
"double"), then any complex number can be fully represented by a 16 byte
buffer.

.. code-block::

    +---------+---------+
    |  real   |  imag   |
    +---------+---------+
      8 bytes   8 bytes


Here we define ``enc_hook`` and ``ext_hook`` callbacks to convert `complex`
objects to/from this binary representation as a MessagePack extension.

.. code-block:: python

    import msgspec
    import struct
    from typing import Any

    # All extension types need a unique integer designator so the decoder knows
    # which type they're decoding. Here we arbitrarily choose 1, but any integer
    # between 0 and 127 (inclusive) would work.
    COMPLEX_TYPE_CODE = 1

    def enc_hook(obj: Any) -> Any:
        if isinstance(obj, complex):
            # encode the complex number into a 16 byte buffer
            data = struct.pack('dd', obj.real, obj.imag)

            # Return an `Ext` object so msgspec serializes it as an extension type.
            return msgspec.msgpack.Ext(COMPLEX_TYPE_CODE, data)
        else:
            # Raise a NotImplementedError for other types
            raise NotImplementedError(f"Objects of type {type(obj)} are not supported")


    def ext_hook(code: int, data: memoryview) -> Any:
        if code == COMPLEX_TYPE_CODE:
            # This extension type represents a complex number, decode the data
            # buffer accordingly.
            real, imag = struct.unpack('dd', data)
            return complex(real, imag)
        else:
            # Raise a NotImplementedError for other extension type codes
            raise NotImplementedError(f"Extension type code {code} is not supported")


    # Create an encoder and a decoder using the custom callbacks
    enc = msgspec.msgpack.Encoder(enc_hook=enc_hook)
    dec = msgspec.msgpack.Decoder(ext_hook=ext_hook)

    # Define a message that contains complex numbers
    msg = {"roots": [0, 0.75, 1 + 0.5j, 1 - 0.5j]}

    # Encode and decode the message to show that things work
    buf = enc.encode(msg)
    msg2 = dec.decode(buf)
    assert msg == msg2  # True

.. note::

    Note that the ``data`` argument to ``ext_hook`` is a `memoryview`. This
    view is attached to the larger buffer containing the complete message being
    decoded. As such, you'll want to ensure that you don't keep a reference to
    the underlying buffer, otherwise you may accidentally persist the larger
    message buffer around for longer than necessary, resulting in increased
    memory usage.

.. _extensions:
.. _MessagePack extensions:
.. _MessagePack extension: https://github.com/msgpack/msgpack/blob/master/spec.md#extension-types
