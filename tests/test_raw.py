import operator
import sys
import weakref

import pytest

import msgspec


def test_raw_noargs():
    r = msgspec.Raw()
    assert bytes(r) == b""
    assert len(r) == 0
    assert not r


@pytest.mark.parametrize("type", [bytes, bytearray, memoryview])
def test_raw_constructor(type):
    r = msgspec.Raw(type(b"test"))
    assert bytes(r) == b"test"
    assert len(r) == 4
    assert r


def test_raw_constructor_errors():
    with pytest.raises(TypeError):
        msgspec.Raw("test")

    with pytest.raises(TypeError):
        msgspec.Raw(msg=b"test")

    with pytest.raises(TypeError):
        msgspec.Raw(b"test", b"extra")


def test_raw_from_view():
    r = msgspec.Raw(memoryview(b"123456")[:3])
    assert bytes(r) == b"123"
    assert len(r) == 3
    assert r


def test_raw_copy():
    r = msgspec.Raw(b"test")
    c1 = sys.getrefcount(r)
    r2 = r.copy()
    c2 = sys.getrefcount(r)
    assert c1 + 1 == c2
    assert r2 is r

    r = msgspec.Raw()
    assert r.copy() is r

    m = memoryview(b"test")
    ref = weakref.ref(m)
    r = msgspec.Raw(m)
    del m
    # Raw holds a ref
    assert ref() is not None
    r2 = r.copy()
    # Actually copied
    assert r2 is not r
    assert bytes(r2) == b"test"
    # Copy doesn't accidentally release buffer
    assert ref() is not None
    del r
    # Copy doesn't hold a reference to original view
    assert ref() is None


def test_raw_pickle():
    orig_buffer = b"test"
    r = msgspec.Raw(orig_buffer)
    o = r.__reduce__()
    assert o == (msgspec.Raw, (b"test",))
    assert o[1][0] is orig_buffer

    r = msgspec.Raw(memoryview(b"test")[:3])
    o = r.__reduce__()
    assert o == (msgspec.Raw, (b"tes",))


def test_raw_comparison():
    r = msgspec.Raw()
    assert r == r
    assert not r != r
    assert msgspec.Raw() == msgspec.Raw()
    assert msgspec.Raw(b"") == msgspec.Raw()
    assert not msgspec.Raw(b"") == msgspec.Raw(b"other")
    assert msgspec.Raw(b"test") == msgspec.Raw(memoryview(b"testy")[:4])
    assert msgspec.Raw(b"test") != msgspec.Raw(b"tesp")
    assert msgspec.Raw(b"test") != msgspec.Raw(b"")
    assert msgspec.Raw(b"") != msgspec.Raw(b"test")
    assert msgspec.Raw() != 1
    assert 1 != msgspec.Raw()

    for op in [operator.lt, operator.gt, operator.le, operator.ge]:
        with pytest.raises(TypeError):
            op(msgspec.Raw(), msgspec.Raw())
