# Msgspec-x


<p align="center">
  <a href="https://nightsailer.github.io/msgspec-x/">
    <img src="https://raw.githubusercontent.com/jcrist/msgspec/main/docs/source/_static/msgspec-logo-light.svg" width="35%" alt="msgspec" />
  </a>
</p>

<p align="center">
  <a href="https://github.com/nightsailer/msgspec-x/actions/workflows/ci.yml">
    <img src="https://github.com/nightsailer/msgspec-x/actions/workflows/ci.yml/badge.svg">
  </a>
  <a href="https://nightsailer.github.io/msgspec-x/">
    <img src="https://img.shields.io/badge/docs-latest-blue.svg">
  </a>
  <a href="https://github.com/nightsailer/msgspec-x/blob/main/LICENSE">
    <img src="https://img.shields.io/github/license/nightsailer/msgspec-x.svg">
  </a>
  <a href="https://pypi.org/project/msgspec-x/">
    <img src="https://img.shields.io/pypi/v/msgspec-x.svg">
  </a>
  <a href="https://anaconda.org/conda-forge/msgspec-x">
    <img src="https://img.shields.io/conda/vn/conda-forge/msgspec-x.svg">
  </a>
  <a href="https://codecov.io/gh/nightsailer/msgspec-x">
    <img src="https://codecov.io/gh/nightsailer/msgspec-x/branch/main/graph/badge.svg">
  </a>
</p>

## Overview

`msgspec-x` is a community-driven fork of the [original msgspec library](https://jcristharif.com/msgspec/) by Jim Crist-Harif. This project was created to address the challenge of slow upstream maintenance and to provide a platform for community contributions that couldn't be timely integrated into the original project.

### Why msgspec-x?

The original msgspec library is an excellent project, but the maintainer has limited time to review and merge community pull requests. This has resulted in valuable contributions and bug fixes being stuck in the review process. `msgspec-x` was created to:

- **Accelerate community contributions**: Provide a faster path for community PRs and enhancements
- **Enable rapid bug fixes**: Address issues without waiting for upstream review cycles  
- **Extend functionality**: Add new features that complement the original design
- **Maintain compatibility**: Keep full backward compatibility with the original msgspec API

### **⚠️ IMPORTANT: Installation Notice**

**Do not install both `msgspec` and `msgspec-x` simultaneously!** They are conflicting packages that cannot coexist in the same environment. If you have the original `msgspec` installed, uninstall it first:

```bash
pip uninstall msgspec
pip install msgspec-x
```

## Dual Namespace Architecture

`msgspec-x` provides two distinct namespaces to serve different needs:

### 1. `msgspec` Namespace - Full Compatibility
The `msgspec` namespace maintains 100% API compatibility with the original library. All your existing code will work without any changes:

```python
import msgspec  # Drop-in replacement for original msgspec

class User(msgspec.Struct):
    name: str
    email: str

# All existing msgspec code works exactly the same
user = User("alice", "alice@example.com")
data = msgspec.json.encode(user)
decoded = msgspec.json.decode(data, type=User)
```

### 2. `msgspec_x` Namespace - Extended Features
The `msgspec_x` namespace provides additional functionality and enhancements not available in the original library:

```python
import msgspec_x  # Extended features and community contributions

# Extended features will be documented as they are added
# This namespace allows for innovative features without breaking compatibility
```

## Core Features

`msgspec-x` inherits all the powerful features from the original msgspec library:

- 🚀 **High performance encoders/decoders** for JSON, MessagePack, YAML, and TOML
- 🎉 **Support for a wide variety of Python types** with extension capabilities
- 🔍 **Zero-cost schema validation** using Python type annotations
- ✨ **Fast Struct type** for structured data representation
- 📦 **Lightweight library** with no required dependencies

All protocols and performance characteristics are maintained from the original implementation.

## Quick Start

### Installation

```bash
pip install msgspec-x
```

### Basic Usage

Define your message schemas using standard Python type annotations:

```python
import msgspec

class User(msgspec.Struct):
    """A new type describing a User"""
    name: str
    groups: set[str] = set()
    email: str | None = None
```

Encode messages as JSON or other supported protocols:

```python
alice = User("alice", groups={"admin", "engineering"})
msg = msgspec.json.encode(alice)
# Output: b'{"name":"alice","groups":["admin","engineering"],"email":null}'
```

Decode messages back into Python objects with schema validation:

```python
# Successful decoding
user = msgspec.json.decode(msg, type=User)

# Validation error example
msgspec.json.decode(b'{"name":"bob","groups":[123]}', type=User)
# Raises: ValidationError: Expected `str`, got `int` - at `$.groups[0]`
```

## Performance

`msgspec-x` maintains the same exceptional performance characteristics as the original msgspec library. In benchmarks, it can be 10-80x faster than alternative libraries for encoding/decoding with validation.

<p align="center">
  <a href="https://jcristharif.com/msgspec/benchmarks.html">
    <img src="https://raw.githubusercontent.com/jcrist/msgspec/main/docs/source/_static/bench-validation.svg">
  </a>
</p>

## Community & Contributing

This project welcomes community contributions! Unlike the original project, we aim to provide faster review cycles and more responsive maintenance.

- 🐛 **Bug Reports**: Issues are addressed promptly
- 🚀 **Feature Requests**: Community-driven feature development
- 🔧 **Pull Requests**: Faster review and merge process
- 📚 **Documentation**: Community-maintained documentation improvements

## Documentation

For detailed documentation, examples, and API references, visit:
- **Project Documentation**: [https://nightsailer.github.io/msgspec-x/](https://nightsailer.github.io/msgspec-x/)
- **Original msgspec docs**: [https://jcristharif.com/msgspec/](https://jcristharif.com/msgspec/) (for reference)

## License

New BSD License. See the [License File](https://github.com/nightsailer/msgspec-x/blob/main/LICENSE).

## Acknowledgments

Special thanks to Jim Crist-Harif for creating the original msgspec library. This fork exists to complement and extend his excellent work, not to replace it.
