from __future__ import annotations

import asyncio
from typing import Any

import msgspec


# Some utilities for writing and reading length-prefix framed messages. Using
# length-prefixed framing makes it easier for the reader to determine the
# boundaries of each message before passing it to msgspec to be decoded.
async def prefixed_send(stream: asyncio.StreamWriter, buffer: bytes) -> None:
    """Write a length-prefixed buffer to the stream"""
    # Encode the message length as a 4 byte big-endian integer.
    prefix = len(buffer).to_bytes(4, "big")

    # Write the prefix and buffer to the stream.
    stream.write(prefix)
    stream.write(buffer)
    await stream.drain()


async def prefixed_recv(stream: asyncio.StreamReader) -> bytes:
    """Read a length-prefixed buffer from the stream"""
    # Read the next 4 byte prefix
    prefix = await stream.readexactly(4)

    # Convert the prefix back into an integer for the next message length
    n = int.from_bytes(prefix, "big")

    # Read in the full message buffer
    return await stream.readexactly(n)


# Define some request types. We set `tag=True` on each type so they can be used
# in a "tagged-union" defining the request types.
class Get(msgspec.Struct, tag=True):
    key: str


class Put(msgspec.Struct, tag=True):
    key: str
    val: str


class Del(msgspec.Struct, tag=True):
    key: str


class ListKeys(msgspec.Struct, tag=True):
    pass


# A union of all valid request types
Request = Get | Put | Del | ListKeys


class Server:
    """An example TCP key-value server using asyncio and msgspec"""

    def __init__(self, host: str = "127.0.0.1", port: int = 8888):
        self.host = host
        self.port = port
        self.kv: dict[str, str] = {}
        # A msgpack encoder for encoding responses
        self.encoder = msgspec.msgpack.Encoder()
        # A *typed* msgpack decoder for decoding requests. If a request doesn't
        # match the specified types, a nice error will be raised.
        self.decoder = msgspec.msgpack.Decoder(Request)

    async def handle_connection(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ):
        """Handle the full lifetime of a single connection"""
        print("Connection opened")
        while True:
            try:
                # Receive and decode a request
                buffer = await prefixed_recv(reader)
                req = self.decoder.decode(buffer)

                # Process the request
                resp = await self.handle_request(req)

                # Encode and write the response
                buffer = self.encoder.encode(resp)
                await prefixed_send(writer, buffer)
            except EOFError:
                print("Connection closed")
                return

    async def handle_request(self, req: Request) -> Any:
        """Handle a single request and return the result (if any)"""
        # We use pattern matching here to branch on the different message types.
        # You could just as well use an if-else statement, but pattern matching
        # works pretty well here.
        match req:
            case Get(key):
                # Return the value for a key, or None if missing
                return self.kv.get(key)
            case Put(key, val):
                # Add a new key-value pair
                self.kv[key] = val
                return None
            case Del(key):
                # Remove a key-value pair if it exists
                self.kv.pop(key, None)
                return None
            case ListKeys():
                # Return a list of all keys in the store
                return sorted(self.kv)

    async def serve_forever(self) -> None:
        server = await asyncio.start_server(
            self.handle_connection, self.host, self.port
        )
        print(f"Serving on tcp://{self.host}:{self.port}...")
        async with server:
            await server.serve_forever()

    def run(self) -> None:
        """Run the server until ctrl-C"""
        asyncio.run(self.serve_forever())


class Client:
    """An example TCP key-value client using asyncio and msgspec."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.reader = reader
        self.writer = writer

    @classmethod
    async def create(cls, host: str = "127.0.0.1", port: int = 8888):
        """Create a new client"""
        reader, writer = await asyncio.open_connection(host, port)
        return cls(reader, writer)

    async def close(self) -> None:
        """Close the client."""
        self.writer.close()
        await self.writer.wait_closed()

    async def request(self, req):
        """Send a request and await the response"""
        # Encode and send the request
        buffer = msgspec.msgpack.encode(req)
        await prefixed_send(self.writer, buffer)

        # Receive and decode the response
        buffer = await prefixed_recv(self.reader)
        return msgspec.msgpack.decode(buffer)

    async def get(self, key: str) -> str | None:
        """Get a key from the KV store, returning None if not present"""
        return await self.request(Get(key))

    async def put(self, key: str, val: str) -> None:
        """Put a key-val pair in the KV store"""
        return await self.request(Put(key, val))

    async def delete(self, key: str) -> None:
        """Delete a key-val pair from the KV store"""
        return await self.request(Del(key))

    async def list_keys(self) -> list[str]:
        """List all keys in the KV store"""
        return await self.request(ListKeys())


if __name__ == "__main__":
    Server().run()
