import enum
import copy
import datetime
import gc
import inspect
import pickle
import sys

import pytest

from msgspec import Struct


class Fruit(enum.IntEnum):
    APPLE = 1
    BANANA = 2


def as_tuple(x):
    return tuple(getattr(x, f) for f in x.__struct_fields__)


def test_struct_class_attributes():
    assert Struct.__struct_fields__ == ()
    assert Struct.__struct_defaults__ == ()
    assert Struct.__match_args__ == ()
    assert Struct.__slots__ == ()
    assert Struct.__module__ == "msgspec"


def test_struct_instance_attributes():
    class Test(Struct):
        c: int
        b: float
        a: str = "hello"

    x = Test(1, 2.0, a="goodbye")

    assert x.__struct_fields__ == ("c", "b", "a")
    assert x.__struct_defaults__ == ("hello",)
    assert x.__slots__ == ("a", "b", "c")

    assert x.c == 1
    assert x.b == 2.0
    assert x.a == "goodbye"


def test_struct_subclass_forbids_init_new_slots():
    with pytest.raises(TypeError, match="__init__"):

        class Test1(Struct):
            a: int

            def __init__(self, a):
                pass

    with pytest.raises(TypeError, match="__new__"):

        class Test2(Struct):
            a: int

            def __new__(self, a):
                pass

    with pytest.raises(TypeError, match="__slots__"):

        class Test3(Struct):
            __slots__ = ("a",)
            a: int


def test_struct_subclass_forbids_non_struct_bases():
    class Mixin(object):
        def method(self):
            pass

    with pytest.raises(TypeError, match="All base classes must be"):

        class Test(Struct, Mixin):
            a: int


def test_struct_subclass_forbids_mixed_layouts():
    class A(Struct):
        a: int
        b: int

    class B(Struct):
        c: int
        d: int

    # This error is raised by cpython
    with pytest.raises(TypeError, match="lay-out conflict"):

        class C(A, B):
            pass


def test_structmeta_no_args():
    class Test(Struct):
        pass

    assert Test.__struct_fields__ == ()
    assert Test.__struct_defaults__ == ()
    assert Test.__match_args__ == ()
    assert Test.__slots__ == ()

    sig = inspect.Signature(parameters=[])
    assert Test.__signature__ == sig


def test_structmeta_positional_only():
    class Test(Struct):
        y: float
        x: int

    assert Test.__struct_fields__ == ("y", "x")
    assert Test.__struct_defaults__ == ()
    assert Test.__match_args__ == ("y", "x")
    assert Test.__slots__ == ("x", "y")

    sig = inspect.Signature(
        parameters=[
            inspect.Parameter(
                "y", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=float
            ),
            inspect.Parameter(
                "x", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int
            ),
        ]
    )
    assert Test.__signature__ == sig


def test_structmeta_positional_and_keyword():
    class Test(Struct):
        c: int
        d: int = 1
        b: float
        a: float = 2.0

    assert Test.__struct_fields__ == ("c", "b", "d", "a")
    assert Test.__struct_defaults__ == (1, 2.0)
    assert Test.__match_args__ == ("c", "b", "d", "a")
    assert Test.__slots__ == ("a", "b", "c", "d")

    sig = inspect.Signature(
        parameters=[
            inspect.Parameter(
                "c", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int
            ),
            inspect.Parameter(
                "b", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=float
            ),
            inspect.Parameter(
                "d", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int, default=1
            ),
            inspect.Parameter(
                "a",
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
                annotation=float,
                default=2.0,
            ),
        ]
    )
    assert Test.__signature__ == sig


def test_structmeta_keyword_only():
    class Test(Struct):
        y: int = 1
        x: float = 2.0

    assert Test.__struct_fields__ == ("y", "x")
    assert Test.__struct_defaults__ == (1, 2.0)
    assert Test.__match_args__ == ("y", "x")
    assert Test.__slots__ == ("x", "y")

    sig = inspect.Signature(
        parameters=[
            inspect.Parameter(
                "y", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int, default=1
            ),
            inspect.Parameter(
                "x",
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
                annotation=float,
                default=2.0,
            ),
        ]
    )
    assert Test.__signature__ == sig


def test_structmeta_subclass_no_change():
    class Test(Struct):
        y: float
        x: int

    class Test2(Test):
        pass

    assert Test2.__struct_fields__ == ("y", "x")
    assert Test2.__struct_defaults__ == ()
    assert Test2.__match_args__ == ("y", "x")
    assert Test2.__slots__ == ()

    sig = inspect.Signature(
        parameters=[
            inspect.Parameter(
                "y", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=float
            ),
            inspect.Parameter(
                "x", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int
            ),
        ]
    )
    assert Test2.__signature__ == sig

    assert as_tuple(Test2(1, 2)) == (1, 2)
    assert as_tuple(Test2(y=1, x=2)) == (1, 2)


def test_structmeta_subclass_extends():
    class Test(Struct):
        c: int
        d: int = 1
        b: float
        a: float = 2.0

    class Test2(Test):
        e: str
        f: float = 3.0

    assert Test2.__struct_fields__ == ("c", "b", "e", "d", "a", "f")
    assert Test2.__struct_defaults__ == (1, 2.0, 3.0)
    assert Test2.__match_args__ == ("c", "b", "e", "d", "a", "f")
    assert Test2.__slots__ == ("e", "f")

    sig = inspect.Signature(
        parameters=[
            inspect.Parameter(
                "c", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int
            ),
            inspect.Parameter(
                "b", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=float
            ),
            inspect.Parameter(
                "e", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=str
            ),
            inspect.Parameter(
                "d", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int, default=1
            ),
            inspect.Parameter(
                "a",
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
                annotation=float,
                default=2.0,
            ),
            inspect.Parameter(
                "f",
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
                annotation=float,
                default=3.0,
            ),
        ]
    )
    assert Test2.__signature__ == sig

    assert as_tuple(Test2(1, 2, 3, 4, 5, 6)) == (1, 2, 3, 4, 5, 6)
    assert as_tuple(Test2(4, 5, 6)) == (4, 5, 6, 1, 2.0, 3.0)


def test_structmeta_subclass_overrides():
    class Test(Struct):
        c: int
        d: int = 1
        b: float
        a: float = 2.0

    class Test2(Test):
        d: int = 2  # change default
        c: int = 3  # switch to keyword
        a: float  # switch to positional

    assert Test2.__struct_fields__ == ("b", "a", "d", "c")
    assert Test2.__struct_defaults__ == (2, 3)
    assert Test2.__match_args__ == ("b", "a", "d", "c")
    assert Test2.__slots__ == ()

    sig = inspect.Signature(
        parameters=[
            inspect.Parameter(
                "b", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=float
            ),
            inspect.Parameter(
                "a", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=float
            ),
            inspect.Parameter(
                "d", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int, default=2
            ),
            inspect.Parameter(
                "c", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int, default=3
            ),
        ]
    )
    assert Test2.__signature__ == sig

    assert as_tuple(Test2(1, 2, 3, 4)) == (1, 2, 3, 4)
    assert as_tuple(Test2(4, 5)) == (4, 5, 2, 3)


def test_structmeta_subclass_mixin_struct_base():
    class A(Struct):
        b: int
        a: float = 1.0

    class Mixin(Struct):
        def as_dict(self):
            return {f: getattr(self, f) for f in self.__struct_fields__}

    class B(A, Mixin):
        a: float = 2.0

    assert B.__struct_fields__ == ("b", "a")
    assert B.__struct_defaults__ == (2.0,)
    assert B.__match_args__ == ("b", "a")
    assert B.__slots__ == ()

    sig = inspect.Signature(
        parameters=[
            inspect.Parameter(
                "b", inspect.Parameter.POSITIONAL_OR_KEYWORD, annotation=int
            ),
            inspect.Parameter(
                "a",
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
                annotation=float,
                default=2.0,
            ),
        ]
    )
    assert B.__signature__ == sig

    b = B(1)
    assert b.as_dict() == {"b": 1, "a": 2.0}


def test_struct_init():
    class Test(Struct):
        a: int
        b: float
        c: int = 3
        d: float = 4.0

    assert as_tuple(Test(1, 2.0)) == (1, 2.0, 3, 4.0)
    assert as_tuple(Test(1, b=2.0)) == (1, 2.0, 3, 4.0)
    assert as_tuple(Test(a=1, b=2.0)) == (1, 2.0, 3, 4.0)
    assert as_tuple(Test(1, b=2.0, c=5)) == (1, 2.0, 5, 4.0)
    assert as_tuple(Test(1, b=2.0, d=5.0)) == (1, 2.0, 3, 5.0)
    assert as_tuple(Test(1, 2.0, 5)) == (1, 2.0, 5, 4.0)
    assert as_tuple(Test(1, 2.0, 5, 6.0)) == (1, 2.0, 5, 6.0)

    with pytest.raises(TypeError, match="Missing required argument 'a'"):
        Test()

    with pytest.raises(TypeError, match="Missing required argument 'b'"):
        Test(1)

    with pytest.raises(TypeError, match="Extra positional arguments provided"):
        Test(1, 2, 3, 4, 5)

    with pytest.raises(TypeError, match="Argument 'a' given by name and position"):
        Test(1, 2, a=3)

    with pytest.raises(TypeError, match="Extra keyword arguments provided"):
        Test(1, 2, e=5)


def test_struct_repr():
    assert repr(Struct()) == "Struct()"

    class Test(Struct):
        pass

    assert repr(Test()) == "Test()"

    class Test(Struct):
        a: int
        b: str

    assert repr(Test(1, "hello")) == "Test(a=1, b='hello')"


def test_struct_repr_errors():
    msg = "Oh no!"

    class Bad:
        def __repr__(self):
            raise ValueError(msg)

    class Test(Struct):
        a: object
        b: object

    t = Test(1, Bad())

    with pytest.raises(ValueError, match=msg):
        repr(t)


def test_struct_copy():
    x = copy.copy(Struct())
    assert type(x) is Struct

    class Test(Struct):
        b: int
        a: int

    x = copy.copy(Test(1, 2))
    assert type(x) is Test
    assert x.b == 1
    assert x.a == 2


def test_struct_compare():
    def assert_eq(a, b):
        assert a == b
        assert not a != b

    def assert_neq(a, b):
        assert a != b
        assert not a == b

    class Test(Struct):
        a: int
        b: int

    class Test2(Test):
        pass

    x = Struct()

    assert_eq(x, Struct())
    assert_neq(x, None)

    x = Test(1, 2)
    assert_eq(x, Test(1, 2))
    assert_neq(x, None)
    assert_neq(x, Test(1, 3))
    assert_neq(x, Test(2, 2))
    assert_neq(x, Test2(1, 2))


def test_struct_compare_errors():
    msg = "Oh no!"

    class Bad:
        def __eq__(self, other):
            raise ValueError(msg)

        __ne__ = __eq__

    class Test(Struct):
        a: object
        b: object

    t = Test(1, Bad())
    t2 = Test(1, 2)

    with pytest.raises(ValueError, match=msg):
        t == t2
    with pytest.raises(ValueError, match=msg):
        t != t2
    with pytest.raises(ValueError, match=msg):
        t2 == t
    with pytest.raises(ValueError, match=msg):
        t2 != t


@pytest.mark.parametrize(
    "default",
    [
        None,
        False,
        True,
        1,
        2.0,
        1.5 + 2.32j,
        b"test",
        "test",
        bytearray(b"test"),
        (),
        frozenset(),
        frozenset((1, (2, 3, 4), 5)),
        Fruit.APPLE,
        datetime.time(1),
        datetime.date.today(),
        datetime.timedelta(seconds=2),
        datetime.datetime.now(),
    ],
)
def test_struct_immutable_defaults_use_instance(default):
    class Test(Struct):
        value: object = default

    t = Test()
    assert t.value is default


@pytest.mark.parametrize("default", [[], {}, set()])
def test_struct_empty_mutable_defaults_fast_copy(default):
    class Test(Struct):
        value: object = default

    t = Test()
    assert t.value == default
    assert t.value is not default


class Point(Struct):
    x: int
    y: int


@pytest.mark.parametrize(
    "default",
    [(Point(1, 2),), [Point(1, 2)], {"testing": Point(1, 2)}],
)
def test_struct_mutable_defaults_deep_copy(default):
    class Test(Struct):
        value: object = default

    t = Test()
    assert t.value == default
    assert t.value is not default
    if isinstance(default, dict):
        lr_iter = zip(t.value.values(), default.values())
    else:
        lr_iter = zip(t.value, default)
    for x, y in lr_iter:
        assert x == y
        assert x is not y


def test_struct_reference_counting():
    """Test that struct operations that access fields properly decref"""

    class Test(Struct):
        value: list

    data = [1, 2, 3]

    t = Test(data)
    assert sys.getrefcount(data) == 3

    repr(t)
    assert sys.getrefcount(data) == 3

    t2 = t.__copy__()
    assert sys.getrefcount(data) == 4

    assert t == t2
    assert sys.getrefcount(data) == 4


def test_struct_gc_not_added_if_not_needed():
    """Structs aren't tracked by GC until/unless they reference a container type"""

    class Test(Struct):
        x: object
        y: object

    assert not gc.is_tracked(Test(1, 2))
    assert not gc.is_tracked(Test("hello", "world"))
    assert gc.is_tracked(Test([1, 2, 3], 1))
    assert gc.is_tracked(Test(1, [1, 2, 3]))
    # Tuples are all tracked on creation, but through GC passes eventually
    # become untracked if they don't contain tracked types
    untracked_tuple = (1, 2, 3)
    for i in range(5):
        gc.collect()
        if not gc.is_tracked(untracked_tuple):
            break
    else:
        assert False, "something has changed with Python's GC, investigate"
    assert not gc.is_tracked(Test(1, untracked_tuple))
    tracked_tuple = ([],)
    assert gc.is_tracked(Test(1, tracked_tuple))

    # On mutation, if a tracked objected is stored on a struct, an untracked
    # struct will become tracked
    t = Test(1, 2)
    assert not gc.is_tracked(t)
    t.x = 3
    assert not gc.is_tracked(t)
    t.x = untracked_tuple
    assert not gc.is_tracked(t)
    t.x = []
    assert gc.is_tracked(t)

    # An error in setattr doesn't change tracked status
    t = Test(1, 2)
    assert not gc.is_tracked(t)
    with pytest.raises(AttributeError):
        t.z = []
    assert not gc.is_tracked(t)


def test_struct_nogc():
    """nogc structs are never tracked, even if they reference a container type"""

    class Test(Struct, nogc=True):
        x: object
        y: object

    assert not gc.is_tracked(Test(1, 2))
    assert not gc.is_tracked(Test([1, 2, 3], 1))
    assert not gc.is_tracked(Test(1, [1, 2, 3]))

    # Tracked status doesn't change on mutation
    t = Test(1, 2)
    assert not gc.is_tracked(t)
    t.x = []
    assert not gc.is_tracked(t)


def test_struct_gc_set_on_copy():
    """Copying doesn't go through the struct constructor"""

    class Test(Struct):
        x: object
        y: object

    assert not gc.is_tracked(copy.copy(Test(1, 2)))
    assert not gc.is_tracked(copy.copy(Test(1, ())))
    assert gc.is_tracked(copy.copy(Test(1, [])))


class MyStruct(Struct):
    x: int
    y: int
    z: str = "default"


def test_structs_are_pickleable():
    t = MyStruct(1, 2, "hello")
    t2 = MyStruct(3, 4)

    assert pickle.loads(pickle.dumps(t)) == t
    assert pickle.loads(pickle.dumps(t2)) == t2


def test_struct_handles_missing_attributes():
    """If an attribute is unset, raise an AttributeError appropriately"""
    t = MyStruct(1, 2)
    del t.y
    t2 = MyStruct(1, 2)

    match = "Struct field 'y' is unset"

    with pytest.raises(AttributeError, match=match):
        repr(t)

    with pytest.raises(AttributeError, match=match):
        copy.copy(t)

    with pytest.raises(AttributeError, match=match):
        t == t2

    with pytest.raises(AttributeError, match=match):
        t2 == t

    with pytest.raises(AttributeError, match=match):
        pickle.dumps(t)


@pytest.mark.parametrize("option", ["frozen", "asarray", "nogc"])
def test_struct_option_precedence(option):
    def get(cls):
        return getattr(cls, option)

    class Default(Struct):
        pass

    assert not get(Default)

    class Enabled(Struct, **{option: True}):
        pass

    assert get(Enabled)

    class Disabled(Struct, **{option: False}):
        pass

    assert not get(Disabled)

    class T(Enabled):
        pass

    assert get(T)

    class T(Enabled, **{option: False}):
        pass

    assert not get(T)

    class T(Enabled, Default):
        pass

    assert get(T)

    class T(Default, Enabled):
        pass

    assert get(T)

    class T(Default, Disabled, Enabled):
        pass

    assert not get(T)


def test_invalid_option_raises():
    with pytest.raises(TypeError):

        class Foo(Struct, invalid=True):
            pass


class FrozenPoint(Struct, frozen=True):
    x: int
    y: int


class TestFrozen:
    def test_frozen_objects_no_setattr(self):
        p = FrozenPoint(1, 2)
        with pytest.raises(AttributeError, match="immutable type: 'FrozenPoint'"):
            p.x = 3

    def test_frozen_objects_hashable(self):
        p1 = FrozenPoint(1, 2)
        p2 = FrozenPoint(1, 2)
        p3 = FrozenPoint(1, 3)
        assert hash(p1) == hash(p2)
        assert hash(p1) != hash(p3)
        assert p1 == p2
        assert p1 != p3

    def test_frozen_objects_hash_errors_if_field_unhashable(self):
        p = FrozenPoint(1, [2])
        with pytest.raises(TypeError):
            hash(p)

    def test_mutable_objects_hash_errors(self):
        p = Point(1, 2)
        with pytest.raises(TypeError, match="unhashable type"):
            hash(p)


class TestTagAndTagField:
    @pytest.mark.parametrize(
        "opts, tag_field, tag",
        [
            # Default & explicit NULL
            ({}, None, None),
            ({"tag": None, "tag_field": None}, None, None),
            # tag=True
            ({"tag": True}, "type", "Test"),
            ({"tag": True, "tag_field": "test"}, "test", "Test"),
            # tag=False
            ({"tag": False}, None, None),
            ({"tag": False, "tag_field": "kind"}, None, None),
            # tag str
            ({"tag": "test"}, "type", "test"),
            (dict(tag="test", tag_field="kind"), "kind", "test"),
            # tag callable
            (dict(tag=lambda n: n.lower()), "type", "test"),
            (dict(tag=lambda n: n.lower(), tag_field="kind"), "kind", "test"),
            # tag_field alone
            (dict(tag_field="kind"), "kind", "Test"),
        ],
    )
    def test_config(self, opts, tag_field, tag):
        class Test(Struct, **opts):
            x: int
            y: int

        assert Test.__struct_tag_field__ == tag_field
        assert Test.__struct_tag__ == tag

    @pytest.mark.parametrize(
        "opts1, opts2, tag_field, tag",
        [
            # tag=True
            ({"tag": True}, {}, "type", "S2"),
            ({"tag": True}, {"tag": None}, "type", "S2"),
            ({"tag": True}, {"tag": False}, None, None),
            ({"tag": True}, {"tag_field": "foo"}, "foo", "S2"),
            # tag str
            ({"tag": "test"}, {}, "type", "test"),
            ({"tag": "test"}, {"tag": "test2"}, "type", "test2"),
            ({"tag": "test"}, {"tag": None}, "type", "test"),
            ({"tag": "test"}, {"tag_field": "foo"}, "foo", "test"),
            # tag callable
            ({"tag": lambda n: n.lower()}, {}, "type", "s2"),
            ({"tag": lambda n: n.lower()}, {"tag": False}, None, None),
            ({"tag": lambda n: n.lower()}, {"tag": None}, "type", "s2"),
            ({"tag": lambda n: n.lower()}, {"tag_field": "foo"}, "foo", "s2"),
        ],
    )
    def test_inheritance(self, opts1, opts2, tag_field, tag):
        class S1(Struct, **opts1):
            pass

        class S2(S1, **opts2):
            pass

        assert S2.__struct_tag_field__ == tag_field
        assert S2.__struct_tag__ == tag

    @pytest.mark.parametrize("tag", [b"bad", lambda n: b"bad"])
    def test_tag_wrong_type(self, tag):
        with pytest.raises(TypeError, match="`tag` must be a `str`"):

            class Test(Struct, tag=tag):
                pass

    def test_tag_field_wrong_type(self):
        with pytest.raises(TypeError, match="`tag_field` must be a `str`"):

            class Test(Struct, tag_field=b"bad"):
                pass

    def test_tag_field_collision(self):
        with pytest.raises(ValueError, match="tag_field='y'"):

            class Test(Struct, tag_field="y"):
                x: int
                y: int

    def test_tag_field_inheritance_collision(self):
        # Inherit the tag field
        class Base(Struct, tag_field="y"):
            pass

        with pytest.raises(ValueError, match="tag_field='y'"):

            class Test(Base):
                x: int
                y: int

        # Inherit the field
        class Base(Struct):
            x: int
            y: int

        with pytest.raises(ValueError, match="tag_field='y'"):

            class Test(Base, tag_field="y"):  # noqa
                pass
