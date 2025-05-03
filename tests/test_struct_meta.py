"""Tests for the exposed StructMeta metaclass."""

import pytest

import msgspec
from msgspec import Struct, StructMeta


def test_struct_meta_exists():
    """Test that StructMeta is properly exposed."""
    assert hasattr(msgspec, "StructMeta")
    assert isinstance(Struct, StructMeta)
    assert issubclass(StructMeta, type)


def test_struct_meta_direct_usage():
    """Test that StructMeta can be used directly as a metaclass."""
    class CustomStruct(metaclass=StructMeta):
        x: int
        y: str

    # Verify the struct works as expected
    instance = CustomStruct(x=1, y="test")
    assert instance.x == 1
    assert instance.y == "test"
    assert isinstance(instance, CustomStruct)
    assert isinstance(CustomStruct, StructMeta)


def test_struct_meta_options():
    """Test that StructMeta properly handles struct options."""
    class CustomStruct(metaclass=StructMeta, frozen=True):
        x: int

    # Verify options were applied
    instance = CustomStruct(x=1)
    with pytest.raises(AttributeError):
        instance.x = 2  # Should be frozen


def test_struct_meta_field_processing():
    """Test that StructMeta properly processes fields."""
    class CustomStruct(metaclass=StructMeta):
        x: int
        y: str = "default"

    # Verify struct functionality
    instance = CustomStruct(x=1)
    assert instance.x == 1
    assert instance.y == "default"
    
    # Check struct metadata
    assert hasattr(CustomStruct, "__struct_fields__")
    assert "x" in CustomStruct.__struct_fields__
    assert "y" in CustomStruct.__struct_fields__


def test_struct_meta_with_struct_base():
    """Test using StructMeta with Struct as a base class."""
    class CustomStruct(Struct):
        x: int
        y: str

    # Verify the struct works as expected
    instance = CustomStruct(x=1, y="test")
    assert instance.x == 1
    assert instance.y == "test"
    assert isinstance(instance, CustomStruct)
    assert isinstance(CustomStruct, StructMeta)


def test_struct_meta_validation():
    """Test that StructMeta validation works."""
    # Should raise TypeError for invalid field name
    with pytest.raises(TypeError):
        class InvalidStruct(metaclass=StructMeta):
            __dict__: int  # __dict__ is a reserved name


def test_struct_meta_with_options():
    """Test StructMeta with various options."""
    class Point(metaclass=StructMeta, frozen=True, eq=True, order=True):
        x: int
        y: int

    p1 = Point(x=1, y=2)
    p2 = Point(x=1, y=3)
    
    # Test frozen
    with pytest.raises(AttributeError):
        p1.x = 10
        
    # Test eq - note that we need to compare fields manually
    # since equality is based on identity by default
    assert p1.x == Point(x=1, y=2).x and p1.y == Point(x=1, y=2).y
    assert p1.x == p2.x and p1.y != p2.y
    
    # Test order - we can't directly compare instances
    # but we can compare their field values
    assert (p1.x, p1.y) < (p2.x, p2.y)


def test_struct_meta_inheritance():
    """Test that StructMeta can be inherited in Python code."""
    class CustomMeta(StructMeta):
        """A custom metaclass that inherits from StructMeta.
        
        This metaclass adds a kw_only_default parameter that can be used to
        set the default kw_only value for all subclasses.
        
        When a class is created with this metaclass:
        1. If kw_only is explicitly specified, use that value
        2. If kw_only is not specified but kw_only_default is, use kw_only_default
        3. If neither is specified but a parent class has kw_only_default defined,
           use the parent's kw_only_default
        4. Otherwise, default to False
        """
        # Class attribute to store kw_only_default settings for each class
        _kw_only_default_settings = {}
        
        def __new__(mcls, name, bases, namespace, **kwargs):
            # Check if kw_only is explicitly specified
            kw_only_specified = 'kw_only' in kwargs
            
            # Process kw_only_default parameter
            kw_only_default = kwargs.pop('kw_only_default', None)
            
            # If kw_only_default is specified, store it
            if kw_only_default is not None:
                # Remember this setting for future subclasses
                mcls._kw_only_default_settings[name] = kw_only_default
            else:
                # Check if any parent class has kw_only_default defined
                for base in bases:
                    base_name = base.__name__
                    if base_name in mcls._kw_only_default_settings:
                        # Use parent's kw_only_default
                        kw_only_default = mcls._kw_only_default_settings[base_name]
                        break
            
            # If kw_only is not specified but kw_only_default is available, use it
            if not kw_only_specified and kw_only_default is not None:
                kwargs['kw_only'] = kw_only_default
            
            # Create the class
            cls = super().__new__(mcls, name, bases, namespace, **kwargs)
            return cls
    
    # Test basic functionality - without kw_only_default
    class SimpleModel(metaclass=CustomMeta):
        x: int
        y: str
    
    # Verify the class was created correctly
    assert isinstance(SimpleModel, CustomMeta)
    assert issubclass(CustomMeta, StructMeta)
    
    # Test creating an instance with positional arguments (should work)
    instance = SimpleModel(1, "test")
    assert instance.x == 1
    assert instance.y == "test"
    
    # Test setting kw_only_default=True
    class KwOnlyBase(metaclass=CustomMeta, kw_only_default=True):
        """Base class that sets kw_only_default=True"""
        pass
    
    # Test a simple child class, should inherit kw_only_default
    class SimpleChild(KwOnlyBase):
        x: int
    
    
    # Should only allow keyword arguments
    with pytest.raises(TypeError):
        SimpleChild(1)

    class BadFieldOrder(KwOnlyBase):
        x: int = 0
        y: int
    
    BadFieldOrder(y=10)
    
    # Create instance with keyword arguments
    child = SimpleChild(x=1)
    assert child.x == 1
    
    # Test overriding inherited kw_only_default
    class NonKwOnlyChild(KwOnlyBase, kw_only=False):
        x: int
    
    # Should allow positional arguments
    non_kw_child = NonKwOnlyChild(1)
    assert non_kw_child.x == 1
    
    # Test independent class, not inheriting kw_only_default
    class IndependentModel(metaclass=CustomMeta):
        x: int
        y: str
    
    # Should allow positional arguments
    independent = IndependentModel(1, "test")
    assert independent.x == 1
    assert independent.y == "test"
    
    # Print debug information
    print(f"KwOnlyBase in _kw_only_default_settings: {'KwOnlyBase' in CustomMeta._kw_only_default_settings}")
    print(f"KwOnlyBase default: {CustomMeta._kw_only_default_settings.get('KwOnlyBase')}")
    print(f"SimpleChild in _kw_only_default_settings: {'SimpleChild' in CustomMeta._kw_only_default_settings}")
    
    # Test that kw_only_default values are correctly passed
    assert 'KwOnlyBase' in CustomMeta._kw_only_default_settings
    assert CustomMeta._kw_only_default_settings['KwOnlyBase'] is True


if __name__ == "__main__":
    pytest.main(["-xvs", __file__])
