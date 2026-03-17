import gc
import weakref

import pytest

import msgspec


@pytest.mark.parametrize("proto", [msgspec.json, msgspec.msgpack])
def test_encode_set_doesnt_leak_item_refs(proto):
    class Ex:
        pass

    items = {Ex(), Ex(), Ex()}
    refs = [weakref.ref(item) for item in items]

    proto.Encoder(enc_hook=lambda x: None).encode(items)

    del items
    gc.collect()

    assert all(ref() is None for ref in refs)
