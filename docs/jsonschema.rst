JSON Schema
===========

``msgspec`` provides a few utilities for generating `JSON Schema`_
specifications from msgspec-compatible :doc:`types <supported-types>` and
:doc:`constraints <constraints>`.

- `msgspec.json.schema`: generates a complete JSON Schema for a single type.
- `msgspec.json.schema_components`: generates JSON schemas for multiple types,
  along with a corresponding ``components`` mapping. This is mainly useful when
  generating multiple schemas to include in a larger specification like OpenAPI_.


The generated schemas are compatible with `JSON Schema`_ 2020-12 and OpenAPI_
3.1.


Example
-------


.. code-block:: python

    import msgspec
    from msgspec import Struct, Meta
    from typing import Annotated, Optional


    # A float constrained to values > 0
    PositiveFloat = Annotated[float, Meta(gt=0)]


    class Dimensions(Struct):
        """Dimensions for a product, all measurements in centimeters"""
        length: PositiveFloat
        width: PositiveFloat
        height: PositiveFloat


    class Product(Struct):
        """A product in a catalog"""
        id: int
        name: str
        price: PositiveFloat
        tags: set[str] = set()
        dimensions: Optional[Dimensions] = None


    # Generate a schema for a list of products
    schema = msgspec.json.schema(list[Product])

    # Print out that schema as JSON
    print(msgspec.json.encode(schema))


.. code-block:: json

    {
      "type": "array",
      "items": {"$ref": "#/$defs/Product"},
      "$defs": {
        "Dimensions": {
          "title": "Dimensions",
          "description": "Dimensions for a product, all measurements in centimeters",
          "type": "object",
          "properties": {
            "length": {"type": "number", "exclusiveMinimum": 0},
            "width": {"type": "number", "exclusiveMinimum": 0},
            "height": {"type": "number", "exclusiveMinimum": 0}
          },
          "required": ["length", "width", "height"]
        },
        "Product": {
          "title": "Product",
          "description": "A product in a catalog",
          "type": "object",
          "properties": {
            "id": {"type": "integer"},
            "name": {"type": "string"},
            "price": {"type": "number", "exclusiveMinimum": 0},
            "tags": {
              "type": "array",
              "items": {"type": "string"},
              "default": [],
            },
            "dimensions": {
              "anyOf": [{"type": "null"}, {"$ref": "#/$defs/Dimensions"}],
              "default": null,
            }
          },
          "required": ["id", "name", "price"]
        }
      }
    }


.. _JSON Schema: https://json-schema.org/
.. _OpenAPI: https://www.openapis.org/
