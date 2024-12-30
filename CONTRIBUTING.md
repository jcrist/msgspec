# How to Contribute

Thank you for taking the time to contribute to `msgspec`!

Here we document some contribution guidelines to help you ensure that your
contribution is at its best.

## Setting up your Development Environment

Before getting started, you will need to already have installed:

- Python (3.8+ only), with development headers installed
- A C compiler (`gcc`, `clang`, and `msvc` are all tested)
- `git`

Once you have those installed, you're ready to:

- Clone the repository
- Install all development dependencies
- Build a development version of `msgspec`
- Install the `pre-commit` hooks

```bash
# Clone the repository
git clone https://github.com/jcrist/msgspec.git

# cd into the repo root directory
cd msgspec/

# Build and install msgspec & all dev dependencies
pip install -e ".[dev]"

# Install the pre-commit hooks
pre-commit install
```

## Editing and Rebuilding

You now have a "development" build of `msgspec` installed. This means that you
can make changes to the `.py` files and test them without requiring a rebuild
of msgspec's C extension. Edit away!

If you do make changes to a `.c` file, you'll need to recompile. You can do
this by running

```bash
pip install -e .
```

By default `msgspec` is built in release mode, with optimizations enabled. To
build a debug build instead (for use with e.g. `gdb` or `lldb`) define the
`MSGSPEC_DEBUG` environment variable before building.

```bash
MSGSPEC_DEBUG=1 pip install -e .
```

## Testing

Tests are located in the `tests/` directory. Any code changes should include
additional tests to ensure correctness. The tests are broken into various
`test_*.py` files specific to the functionality that they're testing.

The tests can be run using `pytest` as follows:

```bash
pytest
```

If you want to run a specific test file, you may specify that file explicitly:

```bash
pytest tests/test_json.py
```

## Linting

We use `pre-commit` to automatically run a few code linters before every
commit. If you followed the development setup above, you should already have
`pre-commit` and all the commit hooks installed.

These hooks will run whenever you try to commit changes.

```bash
git commit  # linters will run automatically here
```

If you wish to run the linters manually without committing, you can run:

```bash
pre-commit run
```

## Documentation

The source of the documentation can be found under `docs/source/`. They are
built using `Sphinx` and can be built locally by running the following steps:

```bash
cd docs/  # Make sure we are in the docs/ folder

make html  # Build the html

# Output can now be found under docs/build/html and can be viewed in the browser
```

## Continuous Integration (CI)

We use GitHub Actions to provide "continuous integration" testing for all Pull
Requests (PRs). When submitting a PR, please check to see that all tests pass,
and fix any issues that come up.

## Code of Conduct

``msgspec`` has a code of conduct that must be followed by all contributors to
the project. You may read the code of conduct
[here](https://github.com/jcrist/msgspec/blob/main/CODE_OF_CONDUCT.md).
