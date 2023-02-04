from __future__ import annotations

import msgspec

Position = tuple[float, float]


# Define the 7 standard Geometry types.
# All types set `tag=True`, meaning that they'll make use of a `type` field to
# disambiguate between types when decoding.
class Point(msgspec.Struct, tag=True):
    coordinates: Position


class MultiPoint(msgspec.Struct, tag=True):
    coordinates: list[Position]


class LineString(msgspec.Struct, tag=True):
    coordinates: list[Position]


class MultiLineString(msgspec.Struct, tag=True):
    coordinates: list[list[Position]]


class Polygon(msgspec.Struct, tag=True):
    coordinates: list[list[Position]]


class MultiPolygon(msgspec.Struct, tag=True):
    coordinates: list[list[list[Position]]]


class GeometryCollection(msgspec.Struct, tag=True):
    geometries: list[Geometry]


Geometry = (
    Point
    | MultiPoint
    | LineString
    | MultiLineString
    | Polygon
    | MultiPolygon
    | GeometryCollection
)


# Define the two Feature types
class Feature(msgspec.Struct, tag=True):
    geometry: Geometry | None = None
    properties: dict | None = None
    id: str | int | None = None


class FeatureCollection(msgspec.Struct, tag=True):
    features: list[Feature]


# A union of all 9 GeoJSON types
GeoJSON = Geometry | Feature | FeatureCollection


# Create a decoder and an encoder to use for decoding & encoding GeoJSON types
loads = msgspec.json.Decoder(GeoJSON).decode
dumps = msgspec.json.Encoder().encode
