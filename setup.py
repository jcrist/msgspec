import sys
import os

from setuptools import setup
from setuptools.extension import Extension

# Check for 32-bit windows builds, which currently aren't supported. We can't
# rely on `platform.architecture` here since users can still run 32-bit python
# builds on 64 bit architectures.
if sys.platform == "win32" and sys.maxsize == (2**31 - 1):
    import textwrap

    error = """
    ====================================================================
    `msgspec` currently doesn't support 32-bit Python windows builds. If
    this is important for your use case, please comment on this issue:

    https://github.com/jcrist/msgspec/issues/845
    ====================================================================
    """
    print(textwrap.dedent(error))
    exit(1)


SANITIZE = os.environ.get("MSGSPEC_SANITIZE", False)
COVERAGE = os.environ.get("MSGSPEC_COVERAGE", False)
DEBUG = os.environ.get("MSGSPEC_DEBUG", SANITIZE or COVERAGE)

extra_compile_args = []
extra_link_args = []
if SANITIZE:
    extra_compile_args.extend(["-fsanitize=address", "-fsanitize=undefined"])
    extra_link_args.extend(["-lasan", "-lubsan"])
if COVERAGE:
    extra_compile_args.append("--coverage")
    extra_link_args.append("-lgcov")
if DEBUG:
    extra_compile_args.extend(["-O0", "-g", "-UNDEBUG"])

# from https://py-free-threading.github.io/faq/#im-trying-to-build-a-library-on-windows-but-msvc-says-c-atomic-support-is-not-enabled
if sys.platform == "win32":
    extra_compile_args.extend(
        [
            "/std:c11",
            "/experimental:c11atomics",
        ]
    )

ext_modules = [
    Extension(
        "msgspec._core",
        [os.path.join("msgspec", "_core.c")],
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
    )
]

setup(
    name="msgspec",
    license="BSD",
    packages=["msgspec"],
    package_data={"msgspec": ["py.typed", "*.pyi"]},
    ext_modules=ext_modules,
)
