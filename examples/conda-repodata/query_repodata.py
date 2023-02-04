import json
import time

import orjson
import requests
import simdjson
import ujson

import msgspec


def query_msgspec(data: bytes) -> list[tuple[int, str]]:
    # Use Struct types to define the JSON schema. For efficiency we only define
    # the fields we actually need.
    class Package(msgspec.Struct):
        name: str
        size: int

    class RepoData(msgspec.Struct):
        packages: dict[str, Package]

    # Decode the data as a `RepoData` type
    repo_data = msgspec.json.decode(data, type=RepoData)

    # Sort packages by `size`, and return the top 10
    return sorted(
        ((p.size, p.name) for p in repo_data.packages.values()), reverse=True
    )[:10]


def query_orjson(data: bytes) -> list[tuple[int, str]]:
    repo_data = orjson.loads(data)
    return sorted(
        ((p["size"], p["name"]) for p in repo_data["packages"].values()), reverse=True
    )[:10]


def query_json(data: bytes) -> list[tuple[int, str]]:
    repo_data = json.loads(data)
    return sorted(
        ((p["size"], p["name"]) for p in repo_data["packages"].values()), reverse=True
    )[:10]


def query_ujson(data: bytes) -> list[tuple[int, str]]:
    repo_data = ujson.loads(data)
    return sorted(
        ((p["size"], p["name"]) for p in repo_data["packages"].values()), reverse=True
    )[:10]


def query_simdjson(data: bytes) -> list[tuple[int, str]]:
    repo_data = simdjson.Parser().parse(data)
    return sorted(
        ((p["size"], p["name"]) for p in repo_data["packages"].values()), reverse=True
    )[:10]


# Download the current_repodata.json file
resp = requests.get(
    "https://conda.anaconda.org/conda-forge/noarch/current_repodata.json"
)
resp.raise_for_status()
data = resp.content

libraries = [
    ("json", query_json),
    ("ujson", query_ujson),
    ("orjson", query_orjson),
    ("simdjson", query_simdjson),
    ("msgspec", query_msgspec),
]

# Run the query with each JSON library, timing the execution
for lib, func in libraries:
    start = time.perf_counter()
    func(data)
    stop = time.perf_counter()
    print(f"{lib}: {(stop - start) * 1000:.2f} ms")
