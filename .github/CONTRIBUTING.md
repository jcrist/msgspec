# How to Contribute

Thank you for taking the time to contribute to `msgspec`!

Here we document some contribution guidelines to help you ensure that your
contribution is at its best.

## Setting up your Development Environment

> [!TIP]
> If you're using VS Code or Cursor, you can develop inside a container using the provided `.devcontainer` configuration. This provides a pre-configured environment with all dependencies installed. Simply open the project in your editor and select "Reopen in Container" when prompted.

Before getting started, you will need to already have installed:

- [Git](https://git-scm.com/book/en/v2/Getting-Started-Installing-Git)
- [uv](https://docs.astral.sh/uv/)
- A C compiler (`gcc`, `clang`, and `msvc` are all tested)

Once you have those installed, you're ready to clone the repository:

```
git clone https://github.com/jcrist/msgspec.git
cd msgspec/
```

Next, you'll need to install the command runner [`just`](https://just.systems/man/en/):

```
uv tool install rust-just
```

This will output extra information if the installation directory is not already on your `PATH`. If you do not want the `just` binary on your `PATH`, you can use uv to invoke it via a virtual environment:

```
uv run --only-dev just --help
```

At this point you're all set up to start contributing to `msgspec`!

If you'd like to install the Git hooks, you can do so by running:

```
just pre-commit install
```

Not installing the Git hooks will mean that the linters will not run automatically when you commit changes and instead must be run [manually](#linting).

## Editing and Rebuilding

You now have a "development" build of `msgspec` installed. This means that you
can make changes to the `.py` files and test them without requiring a rebuild
of msgspec's C extension. Edit away!

If you do make changes to a `.c` file, you'll need to recompile. You can do
this by running commands with the `rebuild` variable set to `true` or `1`:

```
just rebuild=1 test
```

By default `msgspec` is built in release mode, with optimizations enabled. To
build a debug build instead (for use with e.g. `gdb` or `lldb`) set the
`debug` variable to `true` or `1` when running the command:

```
just rebuild=1 debug=1 test
```

## Testing

Tests are located in the `tests/` directory. Any code changes should include
additional tests to ensure correctness. The tests are broken into various
`test_*.py` files specific to the functionality that they're testing.

The recipes prefixed with `test-` use `pytest` to run the tests. For example:

```
just test-all
```

All such recipes accept additional arguments that are passed to `pytest`. For
example, if you want to run a specific test file:

```
just test tests/test_json.py
```

To invoke `pytest` directly, you can use the `env` recipe with the `test` environment:

```
just env test pytest --help
```

## Linting

We use [`prek`](https://github.com/j178/prek) to automatically run a few code
linters before every commit. If you followed the development setup above, you
should already have `prek` and all the Git hooks installed.

These hooks will run whenever you try to commit changes.

```
git commit  # linters will run automatically here
```

If you wish to run the linters manually without committing, you can run:

```
just hooks
```

You may also run the same hooks without applying fixes:

```
just check
```

## Documentation

The source of the documentation can be found under `docs/source/`. They are
built using `Sphinx` and can be built locally by running:

```
just doc-build
```

The built HTML documentation can be served locally by running:

```
just doc-serve
```

## Benchmarking

You can run all [benchmarks](https://jcristharif.com/msgspec/benchmarks.html) with the `bench` recipe, for example:

```
just bench encodings
```

Omit the benchmark name to list all available benchmarks.

## Python Version Management

You can use the `python` variable to specify the version of Python to use for each environment. For example, to run tests with Python 3.12:

```
just python=3.12 test
```

Existing environments will be re-created if they are using a different version of Python. Environments will continue using the same version of Python until an invocation specifies a different version.

You can update the Python version for all environments with the `env-sync` recipe:

```
just python=3.12 env-sync all
```

## Dev Container

You can manually use dev containers as long as the official [`devcontainer`](https://github.com/devcontainers/cli) CLI is on PATH.

> [!NOTE]
> If you need to install the `devcontainer` CLI and don't yet have Node.js installed, the easiest way is to use [`mise`](https://github.com/jdx/mise) (mise-en-place).
>
> 1. [Install](https://mise.jdx.dev/installing-mise.html) `mise`
> 2. Add the [shim directory](https://mise.jdx.dev/dev-tools/shims.html#mise-activate-shims) to your PATH in one of the following ways:
>     - Manually, by finding the path in the output of `mise doctor`
>     - Automatically, by configuring your shell with the output of the following command (see the help text for the supported shells):
>        ```
>        mise activate [SHELL] --shims
>        ```
> 3. Install the `devcontainer` CLI by running:
>     ```
>     mise use -g node@lts
>     mise use -g npm:@devcontainers/cli
>     ```

To start the dev container, you can use the `dev-start` recipe:

```
just dev-start
```

To open a shell in the dev container, you can use the `dev-shell` recipe:

```
just shell
```

To run a command in the dev container as if you were in a shell, you can use the `dev` recipe:

```
just run just test
```

To stop the dev container, you can use the `dev-stop` recipe:

```
just dev-stop
```

To remove the dev container, you can use the `dev-remove` recipe:

```
just dev-remove
```

## Continuous Integration (CI)

We use GitHub Actions to provide "continuous integration" testing for all Pull
Requests (PRs). When submitting a PR, please check to see that all tests pass,
and fix any issues that come up.

## Code of Conduct

`msgspec` has a code of conduct that must be followed by all contributors to
the project. You may read the code of conduct
[here](https://github.com/jcrist/msgspec/blob/main/.github/CODE_OF_CONDUCT.md).
