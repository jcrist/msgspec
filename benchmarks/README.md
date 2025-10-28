# Benchmarks

Here we document how to run various performance benchmarks about 
serialization, validation, struct, gc and memory usage.

## Setup

Benchmark additional dependencies are included in the bench extra so you will have to run this:
```bash
pip install -e ".[dev, bench]"
```

If you want to run the benchmarks against pydantic v1, you'll have to explicitly
downgrade using this command:
```bash
pip install "pydantic<2"
```

## Running Benchmarks

```bash
# JSON Serialization & Validation
python -m benchmarks.bench_validation

# JSON/MessagePack serialization
python benchmarks/bench_encodings.py --protocol json
python benchmarks/bench_encodings.py --protocol msgpack

# JSON Serialization - Large Data
python benchmarks/bench_large_json.py

# Structs
python benchmarks/bench_structs.py

# Garbage Collection
python benchmarks/bench_gc.py

# Library size comparison
python benchmarks/bench_library_size.py
```

## Print versions of benchmarked libraries
```bash
python -m benchmarks.bench_validation --versions
python benchmarks/bench_encodings.py --protocol json --versions 
python benchmarks/bench_encodings.py --protocol msgpack --versions
python benchmarks/bench_large_json.py --versions
python benchmarks/bench_structs.py --versions
```